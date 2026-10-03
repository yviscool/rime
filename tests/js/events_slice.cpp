// Needs an interactive desktop, exclusive run: installs global hooks, types
// real keys into a window it creates, posts probe messages and writes the
// clipboard.
#include "rime/action/dispatcher.hpp"
#include "rime/action/kernel.hpp"
#include "rime/core/json.hpp"
#include "rime/core/trace.hpp"
#include "rime/js/abi.hpp"
#include "rime/js/host.hpp"
#include "rime/js/runtime.hpp"
#include "rime/win32/clipboard.hpp"
#include "rime/win32/input.hpp"
#include "rime/win32/input_executor.hpp"
#include "rime/win32/js_input.hpp"
#include "rime/win32/window.hpp"

#include <windows.h>
#include <imm.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace {

using namespace std::chrono_literals;
using rime::win32::InputService;

void send_vk(const WORD virtual_key) {
  INPUT inputs[2]{};
  inputs[0].type = INPUT_KEYBOARD;
  inputs[0].ki.wVk = virtual_key;
  inputs[1].type = INPUT_KEYBOARD;
  inputs[1].ki.wVk = virtual_key;
  inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
  // NOTE: hoisted out of assert() so the SendInput side effect still runs
  // under NDEBUG where assert() is compiled out.
  const UINT sent = SendInput(2, inputs, sizeof(INPUT));
  assert(sent == 2);
}

// Dispatches this thread's window messages. The slice's edit control belongs
// to the test thread, so keystrokes (typed or injected by the hotstring
// replacement) only reach it while this thread pumps its own queue.
void pump_window_messages() {
  MSG message;
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

template <typename Predicate>
bool wait_for(Predicate predicate, const std::chrono::milliseconds timeout = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    pump_window_messages();
    std::this_thread::sleep_for(10ms);
  }
  pump_window_messages();
  return predicate();
}

// Runs the JS lane in short slices while this thread keeps pumping, so a wait
// never starves either the runtime or the edit control.
void settle_pumping(rime::js::Runtime& runtime, std::chrono::milliseconds duration) {
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto error = runtime.settle(20ms);
    pump_window_messages();
    if (error.ok()) {
      std::this_thread::sleep_for(5ms);
      pump_window_messages();
    }
  }
}

// The last script handed to run(); every silent failure reports it so a bare
// exit code still names the section that stopped.
std::string g_stage;

int fail(const char* reason) {
  std::fprintf(stderr, "events slice failed in %s: %s\n", g_stage.c_str(), reason);
  return 1;
}

void run(rime::js::Runtime& runtime, const std::string& source, const std::string& filename) {
  g_stage = filename;
  const auto error = runtime.evaluate_module(source, filename).get();
  if (!error.ok()) {
    std::fprintf(stderr, "js step failed (%s): %s\n", filename.c_str(), error.message.c_str());
    std::abort();
  }
}

// A rejected promise is delivered to OnError and also counted as an error
// during the evaluation that produced it, so evaluate_module reports the
// rejection as a failure. This variant accepts exactly that rejection and
// still fails the slice on any other error.
void run_expect_error(rime::js::Runtime& runtime, const std::string& source,
                      const std::string& filename, const std::string& expect) {
  g_stage = filename;
  const auto error = runtime.evaluate_module(source, filename).get();
  if (error.ok()) {
    std::fprintf(stderr, "expected error '%s' in (%s)\n", expect.c_str(), filename.c_str());
    std::abort();
  }
  if (error.message.find(expect) == std::string::npos) {
    std::fprintf(stderr, "js step failed (%s): %s\n", filename.c_str(), error.message.c_str());
    std::abort();
  }
}

// Keeps the JS lane draining for `duration` so queued hook events, timer
// ticks and window messages are delivered before the next assertion.
void pump(rime::js::Runtime& runtime, const std::chrono::milliseconds duration,
          const std::string& label) {
  const auto deadline = std::chrono::steady_clock::now() + duration;
  int step = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    run(runtime,
        "globalThis.pumpFlag = 'pending';\n"
        "waitFor(() => false, 40).then(v => { globalThis.pumpFlag = v; });",
        label + "-" + std::to_string(step++) + ".mjs");
    settle_pumping(runtime, 400ms);
    pump_window_messages();
  }
}

// Arms a waitFor on `condition` and reports the condition text when it never
// became true, so a failed wait names what was being waited for.
void wait_js(rime::js::Runtime& runtime, const std::string& condition,
             const std::string& label) {
  run(runtime,
      "globalThis.waitedFor = 'pending';\n"
      "waitFor(() => " +
          condition +
          ", 4000).then(v => { globalThis.waitedFor = v; });",
      label + ".mjs");
  settle_pumping(runtime, 7000ms);
  run(runtime,
      "if (globalThis.waitedFor !== true)\n"
      "  throw new Error('condition never became true: " +
          condition + "');",
      label + "-check.mjs");
}

// Records the Action IR the event layer built, then hands the batch to the
// real executor: the slice asserts the exact payload and still gets the
// injected keys on the desktop.
class RecordingExecutor final : public rime::action::Executor {
 public:
  explicit RecordingExecutor(std::shared_ptr<rime::action::Executor> inner)
      : inner_(std::move(inner)) {}

  rime::action::Result execute(const rime::action::Action& action,
                               rime::core::CancellationToken cancellation) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      type_ = action.type;
      source_kind_ = action.source.kind;
      payload_ = action.payload;
      ++executed_;
    }
    return inner_->execute(action, cancellation);
  }

  [[nodiscard]] int executed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return executed_;
  }
  [[nodiscard]] std::string source_kind() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return source_kind_;
  }
  [[nodiscard]] std::string payload() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return payload_;
  }

 private:
  std::shared_ptr<rime::action::Executor> inner_;
  mutable std::mutex mutex_;
  int executed_{0};
  std::string type_;
  std::string source_kind_;
  std::string payload_;
};

// Records the chord-built action it executed so the slice can assert the
// exact IR that crossed the hook -> event queue -> dispatcher -> kernel
// boundary.
class ProbeExecutor final : public rime::action::Executor {
 public:
  rime::action::Result execute(const rime::action::Action& action,
                               rime::core::CancellationToken) override {
    std::lock_guard<std::mutex> lock(mutex_);
    action_id_ = action.id;
    type_ = action.type;
    source_kind_ = action.source.kind;
    target_kind_ = action.target.kind;
    target_id_ = action.target.id;
    payload_ = action.payload;
    ++executed_;
    rime::action::Result result;
    result.id = action.id;
    result.succeeded = true;
    result.value = rime::core::json::Value::object();
    result.value.set("fired", rime::core::json::Value::boolean(true));
    return result;
  }

  [[nodiscard]] int executed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return executed_;
  }
  [[nodiscard]] std::uint64_t action_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return action_id_;
  }
  [[nodiscard]] std::string type() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return type_;
  }
  [[nodiscard]] std::string source_kind() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return source_kind_;
  }
  [[nodiscard]] std::string target_kind() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return target_kind_;
  }
  [[nodiscard]] std::string target_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return target_id_;
  }
  [[nodiscard]] std::string payload() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return payload_;
  }

 private:
  mutable std::mutex mutex_;
  int executed_{0};
  std::uint64_t action_id_{0};
  std::string type_;
  std::string source_kind_;
  std::string target_kind_;
  std::string target_id_;
  std::string payload_;
};

// onExit runs after the JS context is on its way out, so the handler proves
// itself by calling into a native module instead of writing a JS global.
std::mutex g_marks_mutex;
std::vector<std::string> g_marks;

bool mark_seen(const std::string& text) {
  std::lock_guard<std::mutex> lock(g_marks_mutex);
  for (const std::string& entry : g_marks) {
    if (entry == text) return true;
  }
  return false;
}

JSValue testprobe_mark(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(context, "mark(text)");
  const char* text = JS_ToCString(context, argv[0]);
  if (!text) return JS_EXCEPTION;
  {
    std::lock_guard<std::mutex> lock(g_marks_mutex);
    g_marks.emplace_back(text);
  }
  JS_FreeCString(context, text);
  return JS_UNDEFINED;
}

int testprobe_init(JSContext* context, JSModuleDef* module) {
  JSValue function = JS_NewCFunction(context, testprobe_mark, "mark", 1);
  if (JS_IsException(function)) return -1;
  return JS_SetModuleExport(context, module, "mark", function);
}

JSModuleDef* create_testprobe_module(JSContext* context) {
  JSModuleDef* module = JS_NewCModule(context, "rime:testprobe", testprobe_init);
  if (!module) return nullptr;
  if (JS_AddModuleExport(context, module, "mark") < 0) return nullptr;
  return module;
}

constexpr wchar_t kEventsWindowClass[] = L"RimeEventsSliceWindow";

// Everything the edit control actually received, logged by subclassing it.
// The final text alone cannot separate "keys went to another window" from
// "the IME rewrote them", this can.
std::vector<wchar_t> g_edit_chars;
WNDPROC g_edit_original = nullptr;

LRESULT CALLBACK log_edit_proc(const HWND window, const UINT message, const WPARAM wparam,
                               const LPARAM lparam) {
  if (message == WM_CHAR) g_edit_chars.push_back(static_cast<wchar_t>(wparam));
  if (g_edit_original) return CallWindowProcW(g_edit_original, window, message, wparam, lparam);
  return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK events_wnd_proc(const HWND window, const UINT message, const WPARAM wparam,
                                 const LPARAM lparam) {
  if (message == WM_DESTROY) return 0;
  return DefWindowProcW(window, message, wparam, lparam);
}

// Focused edit control: hotstring matching needs real keystrokes (they have
// to reach the low-level hook), and those land in whatever window owns the
// focus, so the slice owns the focus while it types.
struct EditTarget {
  HWND window{nullptr};
  HWND edit{nullptr};
};

bool bring_to_front(const EditTarget& target);

bool focus_edit(EditTarget& target) {
  // The desktop runs a Chinese IME; on that layout the injected keystrokes
  // start a pinyin composition instead of landing as ASCII (the hotstring
  // hook still sees the virtual keys, only the edit text goes wrong). The
  // slice types ASCII, so its thread uses the US English layout throughout.
  const HKL us_layout = LoadKeyboardLayoutW(L"00000409", KLF_ACTIVATE);
  if (us_layout != nullptr) (void)ActivateKeyboardLayout(us_layout, 0);
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = events_wnd_proc;
  window_class.hInstance = instance;
  window_class.lpszClassName = kEventsWindowClass;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  if (RegisterClassW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }
  target.window = CreateWindowExW(0, kEventsWindowClass, L"Rime events slice",
                                  WS_OVERLAPPEDWINDOW, 80, 80, 520, 200, nullptr, nullptr,
                                  instance, nullptr);
  if (!target.window) return false;
  target.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 8, 8,
                                500, 28, target.window, nullptr, instance, nullptr);
  if (!target.edit) return false;
  // Detach the IME from the control: the desktop's Chinese IME turns the
  // typed trigger into a pinyin composition and commits a CJK candidate over
  // the ASCII text this slice asserts on. Without a context the keystrokes
  // translate straight to characters.
  (void)ImmAssociateContext(target.edit, nullptr);
  g_edit_original = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(target.edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(log_edit_proc)));
  if (!g_edit_original) return false;
  ShowWindow(target.window, SW_SHOW);
  UpdateWindow(target.window);
  pump_window_messages();
  return bring_to_front(target);
}

// SetForegroundWindow is refused for a background process; attaching to the
// current foreground thread's input queue is the documented way around it.
// Keys only land in our edit while this window is the foreground one, so the
// attempt repeats until both the foreground window and the focus agree -
// re-checked before every batch, because the desktop steals focus.
bool bring_to_front(const EditTarget& target) {
  const DWORD self_thread = GetCurrentThreadId();
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (GetForegroundWindow() == target.window && GetFocus() == target.edit) return true;
    const HWND foreground = GetForegroundWindow();
    const DWORD foreground_thread =
        foreground != nullptr ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const bool attached =
        foreground_thread != 0 && foreground_thread != self_thread &&
        AttachThreadInput(self_thread, foreground_thread, TRUE) != FALSE;
    (void)SetForegroundWindow(target.window);
    SetFocus(target.edit);
    if (attached) (void)AttachThreadInput(self_thread, foreground_thread, FALSE);
    pump_window_messages();
    if (GetForegroundWindow() == target.window && GetFocus() == target.edit) return true;
    std::this_thread::sleep_for(20ms);
  }
  return false;
}

std::wstring edit_text(const HWND edit) {
  const int length = GetWindowTextLengthW(edit);
  if (length <= 0) return std::wstring{};
  std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
  const int copied = GetWindowTextW(edit, text.data(), length + 1);
  if (copied <= 0) return std::wstring{};
  text.resize(static_cast<std::size_t>(copied));
  return text;
}

std::string narrow(const std::wstring& text) {
  if (text.empty()) return std::string{};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 0) return std::string{};
  std::string out(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                      nullptr, nullptr);
  return out;
}

bool contains_ci(const std::wstring& haystack, const std::wstring& needle) {
  if (needle.empty()) return true;
  std::wstring lowered = haystack;
  std::wstring lowered_needle = needle;
  CharLowerBuffW(lowered.data(), static_cast<DWORD>(lowered.size()));
  CharLowerBuffW(lowered_needle.data(), static_cast<DWORD>(lowered_needle.size()));
  return lowered.find(lowered_needle) != std::wstring::npos;
}

// What the edit control holds versus what it was actually sent. A failed
// wait reports both so the cause separates cleanly: keystrokes that never
// landed (focus/layout/IME) look nothing like keystrokes the hook saw but
// the event layer never fired.
void report_edit(const char* what, const HWND edit) {
  const HWND focused = GetFocus();
  const HWND foreground = GetForegroundWindow();
  wchar_t foreground_title[128]{};
  (void)GetWindowTextW(foreground, foreground_title, 128);
  const HKL layout = GetKeyboardLayout(0);
  std::fprintf(stderr, "%s: edit [%s] received [%s] layout %04X focus %s foreground [%s]\n", what,
               narrow(edit_text(edit)).c_str(),
               narrow(std::wstring(g_edit_chars.begin(), g_edit_chars.end())).c_str(),
               static_cast<unsigned>(LOWORD(reinterpret_cast<ULONG_PTR>(layout))),
               focused == edit ? "on" : "NOT on", narrow(foreground_title).c_str());
}

// Types ASCII text as plain keystrokes: the matcher lowercases letters
// itself, so only the virtual-key sequence matters here. Focus is re-asserted
// first - the desktop steals the foreground mid-test, and keys typed into
// another window still reach the hook but never land in the edit.
bool type_ascii(const std::string& text, const EditTarget& target) {
  if (!bring_to_front(target)) return false;
  for (const char character : text) {
    WORD virtual_key = 0;
    if (character >= 'a' && character <= 'z') {
      virtual_key = static_cast<WORD>('A' + (character - 'a'));
    } else if (character >= '0' && character <= '9') {
      virtual_key = static_cast<WORD>(character);
    } else if (character == ' ') {
      virtual_key = VK_SPACE;
    } else {
      continue;
    }
    send_vk(virtual_key);
    pump_window_messages();
    std::this_thread::sleep_for(5ms);
  }
  pump_window_messages();
  return true;
}

// Foreign clipboard write: the raw Win32 path carries no self-change flag,
// so OnClipboardChange must report type 0 for it (AHK's foreign change).
bool write_foreign_text(const std::wstring& text) {
  if (OpenClipboard(nullptr) == FALSE) return false;
  bool ok = EmptyClipboard() != FALSE;
  if (ok) {
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (handle == nullptr) {
      ok = false;
    } else {
      void* buffer = GlobalLock(handle);
      if (buffer == nullptr) {
        GlobalFree(handle);
        ok = false;
      } else {
        std::memcpy(buffer, text.c_str(), bytes);
        GlobalUnlock(handle);
        ok = SetClipboardData(CF_UNICODETEXT, handle) != nullptr;
        if (!ok) GlobalFree(handle);
      }
    }
  }
  (void)CloseClipboard();
  return ok;
}

}  // namespace

int main() {
  InputService service;
  assert(service.start().ok());
  rime::win32::WindowService window_service;
  assert(window_service.start().ok());
  rime::win32::ClipboardService clipboard_service;

  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  rime::action::Kernel kernel(
      std::make_shared<rime::action::StaticCapabilityPolicy>(
          std::unordered_set<std::string>{"windows.hook.global", "probe.chord",
                                          "windows.input.inject", "windows.input.read",
                                          "windows.clipboard.read"}),
      trace);
  auto probe = std::make_shared<ProbeExecutor>();
  assert(kernel.register_executor("probe.chord", probe).ok());
  auto input_executor = std::make_shared<rime::win32::InputExecutor>(service);
  auto recording = std::make_shared<RecordingExecutor>(input_executor);
  assert(kernel.register_executor("input.send", recording).ok());
  assert(kernel.register_executor("input.mouse", input_executor).ok());
  rime::action::Dispatcher dispatcher(kernel, rime::action::default_dispatch_policy());
  std::atomic<std::uint64_t> next_action_id{0};
  rime::win32::InputModuleBinding binding;
  binding.service = &service;
  binding.window_service = &window_service;
  binding.clipboard_service = &clipboard_service;
  binding.kernel = &kernel;
  binding.dispatcher = &dispatcher;
  binding.next_action_id = &next_action_id;
  rime::js::Runtime runtime;
  assert(rime::win32::register_input_module(runtime, &binding).ok());
  assert(runtime
             .add_native_module("rime:testprobe",
                                [](JSContext* context) { return create_testprobe_module(context); })
             .ok());
  assert(runtime.start().ok());

  // Wiring is part of the contract: a binding without the dispatcher the
  // event actions queue through is rejected at register time.
  {
    rime::win32::InputModuleBinding partial;
    partial.service = &service;
    partial.kernel = &kernel;
    rime::js::Runtime partial_runtime;
    assert(rime::win32::register_input_module(partial_runtime, &partial).code ==
           rime::core::Error::Code::InvalidContract);
  }

  // Shared helpers, the export surface, and the onExit handler that marks
  // through the native module once the runtime stops.
  run(runtime,
      "import { input } from 'rime:input';\n"
      "globalThis.input = input;\n",
      "events-boot-import-input.mjs");
  run(runtime,
      "import { runtime } from 'rime:runtime';\n"
      "globalThis.runtime = runtime;\n",
      "events-boot-import-runtime.mjs");
  run(runtime,
      "import { mark } from 'rime:testprobe';\n"
      "globalThis.mark = mark;\n",
      "events-boot-import-testprobe.mjs");
  run(runtime,
      "globalThis.waitFor = async (predicate, timeoutMs) => {\n"
      "  const deadline = Date.now() + timeoutMs;\n"
      "  while (Date.now() < deadline) {\n"
      "    if (predicate()) return true;\n"
      "    await runtime.delay(20, null);\n"
      "  }\n"
      "  return predicate();\n"
      "};\n"
      "globalThis.expectT = (bucket, name, fn) => {\n"
      "  try { fn(); } catch (e) { bucket[name] = (e instanceof TypeError); }\n"
      "};\n"
      "globalThis.checkT = (bucket, name) => {\n"
      "  if (!bucket[name]) throw new Error('expected a TypeError for ' + name);\n"
      "};\n",
      "events-boot-helpers.mjs");
  run(runtime,
      "globalThis.expected = ['hotkey','hotstring','hotIf','hotIfWinActive','hotIfWinExist',\n"
      "                       'hotIfWinNotActive','hotIfWinNotExist','installKeybdHook',\n"
      "                       'installMouseHook','setTimer','onMessage','onClipboardChange',\n"
      "                       'onError','onExit'];\n"
      "globalThis.missing = globalThis.expected.filter(n => typeof input[n] !== 'function');\n"
      "input.onExit(ev => { mark('exit:' + ev.reason); });\n",
      "events-boot.mjs");
  run(runtime,
      "if (globalThis.missing.length !== 0)\n"
      "  throw new Error('missing event exports: ' + globalThis.missing.join(','));\n"
      "if (typeof globalThis.waitFor !== 'function') throw new Error('waitFor helper missing');\n",
      "events-boot-check.mjs");

  // Install*Hook round trip: both hooks are up at start, an empty install
  // list uninstalls them (they report the effective state, a deviation from
  // AHK's void return), and the keyboard hook comes back for the slice.
  run(runtime,
      "globalThis.hook = {};\n"
      "globalThis.hook.keyboardOn = input.installKeybdHook();\n"
      "globalThis.hook.keyboardOff = input.installKeybdHook(false);\n"
      "globalThis.hook.mouseOn = input.installMouseHook();\n"
      "globalThis.hook.mouseOff = input.installMouseHook(false);\n"
      "globalThis.hook.keyboardAgain = input.installKeybdHook(true);\n"
      "globalThis.hookErrors = {};\n"
      "expectT(globalThis.hookErrors, 'installNotBool', () => input.installKeybdHook(1));\n"
      "expectT(globalThis.hookErrors, 'forceNotBool', () => input.installKeybdHook(true, 'x'));\n",
      "events-hook.mjs");
  run(runtime,
      "const hook = globalThis.hook;\n"
      "if (hook.keyboardOn !== true) throw new Error('keyboard hook must start installed');\n"
      "if (hook.keyboardOff !== false) throw new Error('empty install list must uninstall');\n"
      "if (hook.mouseOn !== true) throw new Error('mouse hook must start installed');\n"
      "if (hook.mouseOff !== false) throw new Error('empty install list must uninstall');\n"
      "if (hook.keyboardAgain !== true) throw new Error('keyboard hook must reinstall');\n"
      "checkT(globalThis.hookErrors, 'installNotBool');\n"
      "checkT(globalThis.hookErrors, 'forceNotBool');\n",
      "events-hook-check.mjs");

  // Hotkey: registration shape, dedupe on (chord, criterion) and the argument
  // contract, all before anything is pressed.
  run(runtime,
      "globalThis.hkLog = [];\n"
      "const onF24 = ev => { globalThis.hkLog.push(ev.name); };\n"
      "globalThis.onF24 = onF24;\n"
      "globalThis.hkSub = input.hotkey('f24', onF24);\n"
      "globalThis.hkDup = input.hotkey('f24', onF24);\n"
      "globalThis.hkErrors = {};\n"
      "expectT(globalThis.hkErrors, 'arity', () => input.hotkey('f24'));\n"
      "expectT(globalThis.hkErrors, 'emptyName', () => input.hotkey('', onF24));\n"
      "expectT(globalThis.hkErrors, 'badName', () => input.hotkey('f99', onF24));\n"
      "expectT(globalThis.hkErrors, 'badAction', () => input.hotkey('f24', 42));\n"
      "expectT(globalThis.hkErrors, 'badOptions',\n"
      "        () => input.hotkey('f24', onF24, 'Maybe'));\n"
      "expectT(globalThis.hkErrors, 'leftRight', () => input.hotkey('<^a', onF24));\n"
      "expectT(globalThis.hkErrors, 'controlMissing', () => input.hotkey('f18', 'on'));\n"
      "globalThis.ahkAccepted = true;\n"
      "try { globalThis.ahkSub = input.hotkey('^!f24', () => {}); }\n"
      "catch (e) { globalThis.ahkAccepted = false; }\n",
      "events-hotkey.mjs");
  run(runtime,
      "const hk = globalThis.hkSub;\n"
      "if (typeof hk.id !== 'number' || hk.id <= 0) throw new Error('hotkey id must be positive');\n"
      "if (hk.kind !== 'hotkey') throw new Error('hotkey kind must be hotkey');\n"
      "if (typeof hk.close !== 'function') throw new Error('hotkey must expose close()');\n"
      "if (globalThis.hkDup.id !== hk.id) throw new Error('same chord must reuse one id');\n"
      "if (!globalThis.ahkAccepted) throw new Error('AHK symbol prefixes must be accepted');\n"
      "const errors = globalThis.hkErrors;\n"
      "for (const name of ['arity','emptyName','badName','badAction','badOptions','leftRight',\n"
      "                    'controlMissing']) {\n"
      "  checkT(errors, name);\n"
      "}\n",
      "events-hotkey-check.mjs");

  send_vk(VK_F24);
  wait_js(runtime, "globalThis.hkLog.length >= 1", "events-hotkey-wait");
  run(runtime,
      "if (globalThis.hkLog[0] !== 'f24')\n"
      "  throw new Error('hotkey payload must carry the chord name: ' + globalThis.hkLog[0]);\n",
      "events-hotkey-fire-check.mjs");

  // Control form: off/toggle/on and the On/Off registration option all flip
  // the same registration; a disabled hotkey must stay silent.
  run(runtime,
      "input.hotkey('f24', 'off');",
      "events-hotkey-off.mjs");
  send_vk(VK_F24);
  pump(runtime, 500ms, "events-hotkey-off-pump");
  run(runtime,
      "if (globalThis.hkLog.length !== 1)\n"
      "  throw new Error('disabled hotkey must not fire: ' + globalThis.hkLog.length);\n"
      "input.hotkey('f24', 'toggle');",
      "events-hotkey-toggle-off-check.mjs");
  send_vk(VK_F24);
  wait_js(runtime, "globalThis.hkLog.length >= 2", "events-hotkey-toggle-on");
  run(runtime,
      "input.hotkey('f24', 'toggle');",
      "events-hotkey-toggle-off.mjs");
  send_vk(VK_F24);
  pump(runtime, 500ms, "events-hotkey-toggle-pump");
  run(runtime,
      "if (globalThis.hkLog.length !== 2)\n"
      "  throw new Error('second toggle must disable again: ' + globalThis.hkLog.length);\n"
      "input.hotkey('f24', 'on');",
      "events-hotkey-on-check.mjs");
  send_vk(VK_F24);
  wait_js(runtime, "globalThis.hkLog.length >= 3", "events-hotkey-on-wait");
  run(runtime,
      "input.hotkey('f24', globalThis.onF24, 'Off');",
      "events-hotkey-option-off.mjs");
  send_vk(VK_F24);
  pump(runtime, 500ms, "events-hotkey-option-pump");
  run(runtime,
      "if (globalThis.hkLog.length !== 3)\n"
      "  throw new Error('Off option must disable the hotkey: ' + globalThis.hkLog.length);\n"
      "input.hotkey('f24', 'on');",
      "events-hotkey-option-on-check.mjs");
  send_vk(VK_F24);
  wait_js(runtime, "globalThis.hkLog.length >= 4", "events-hotkey-option-on");

  // Action form: the trigger queues an Action through the dispatcher instead
  // of invoking script, so the exact IR can be asserted on the C++ side.
  run(runtime,
      "globalThis.actionSub = input.hotkey('f23', {\n"
      "  type: 'probe.chord', capability: 'probe.chord',\n"
      "  target: { kind: 'chord', id: 'hotkey' },\n"
      "  payload: { note: 'hotkey-ir' }\n"
      "});",
      "events-hotkey-action.mjs");
  const int actions_before = probe->executed();
  send_vk(VK_F23);
  assert(wait_for([&] { return probe->executed() > actions_before; }));
  assert(probe->type() == "probe.chord");
  assert(probe->source_kind() == "hotkey");
  assert(probe->target_kind() == "chord");
  assert(probe->target_id() == "hotkey");
  assert(probe->payload().find("hotkey-ir") != std::string::npos);
  assert(probe->action_id() > 0);

  // HotIf: a failing criterion falls through to the next registration, a
  // function criterion receives {active, seq}, and window criteria decide
  // from the watcher snapshot once the capture thread has produced one.
  run(runtime,
      "globalThis.hifLog = [];\n"
      "globalThis.hifPayloads = [];\n"
      "globalThis.prevNone = input.hotIf(null);\n"
      "input.hotIf(() => false);\n"
      "globalThis.neverSub = input.hotkey('f22', () => globalThis.hifLog.push('never'));\n"
      "globalThis.prevFn = input.hotIf(() => true);\n"
      "globalThis.alwaysSub = input.hotkey('f22', () => globalThis.hifLog.push('always'));\n"
      "input.hotIf(null);\n"
      "globalThis.plainSub = input.hotkey('f21', () => globalThis.hifLog.push('plain'));\n"
      "globalThis.guardFn = payload => {\n"
      "  globalThis.hifPayloads.push(payload);\n"
      "  return false;\n"
      "};\n"
      "input.hotIf(globalThis.guardFn);\n"
      "globalThis.guardSub = input.hotkey('f20', () => globalThis.hifLog.push('guarded'));\n"
      "globalThis.hifErrors = {};\n"
      "expectT(globalThis.hifErrors, 'hotIfNotFunction', () => input.hotIf(42));\n"
      "expectT(globalThis.hifErrors, 'winNotStrings',\n"
      "        () => input.hotIfWinActive(42));\n",
      "events-hotif.mjs");
  run(runtime,
      "if (globalThis.prevNone.kind !== 'none' || globalThis.prevNone.id !== 0)\n"
      "  throw new Error('first hotIf must report no previous criterion');\n"
      "if (globalThis.prevFn.kind !== 'function' || !(globalThis.prevFn.id > 0))\n"
      "  throw new Error('hotIf must return the previous function criterion');\n"
      "checkT(globalThis.hifErrors, 'hotIfNotFunction');\n"
      "checkT(globalThis.hifErrors, 'winNotStrings');\n",
      "events-hotif-check.mjs");
  send_vk(VK_F22);
  wait_js(runtime, "globalThis.hifLog.length >= 1", "events-hotif-never");
  send_vk(VK_F21);
  wait_js(runtime, "globalThis.hifLog.length >= 2", "events-hotif-plain");
  run(runtime,
      "if (globalThis.hifLog[0] !== 'always')\n"
      "  throw new Error('false criterion must fall through: ' + globalThis.hifLog[0]);\n"
      "if (globalThis.hifLog[1] !== 'plain')\n"
      "  throw new Error('unconditional hotkey must fire: ' + globalThis.hifLog[1]);\n",
      "events-hotif-order-check.mjs");
  send_vk(VK_F20);
  pump(runtime, 500ms, "events-hotif-guard-pump");
  run(runtime,
      "if (globalThis.hifLog.length !== 2)\n"
      "  throw new Error('guarded hotkey must stay silent: ' + globalThis.hifLog.length);\n"
      "if (globalThis.hifPayloads.length < 1) throw new Error('hotIf fn never ran');\n"
      "const payload = globalThis.hifPayloads[0];\n"
      "if (typeof payload.seq !== 'number') throw new Error('hotIf payload must carry seq');\n"
      "if (payload.active !== null && typeof payload.active !== 'object')\n"
      "  throw new Error('hotIf payload active must be an object or null');\n"
      "globalThis.prevWin = input.hotIfWinNotExist('RimeEventsSliceNoSuchClass');\n"
      "globalThis.winSub = input.hotkey('f19', () => globalThis.hifLog.push('winNotExist'));\n"
      "globalThis.prevWinActive = input.hotIfWinActive('RimeEventsSliceNoSuchClass');\n"
      "globalThis.noFireSub = input.hotkey('f18', () => globalThis.hifLog.push('winActive'));\n"
      "input.hotIf(null);",
      "events-hotif-window.mjs");
  run(runtime,
      "if (globalThis.prevWin.kind !== 'function')\n"
      "  throw new Error('window hotIf must return the previous criterion');\n"
      "if (globalThis.prevWinActive.kind !== 'winNotExist' ||\n"
      "    !(globalThis.prevWinActive.id > 0))\n"
      "  throw new Error('descriptor must name the window criterion');\n",
      "events-hotif-window-check.mjs");
  // The watcher snapshots on its own schedule; wait past its first capture
  // before pressing, otherwise window criteria fail closed by design.
  std::this_thread::sleep_for(600ms);
  send_vk(VK_F19);
  wait_js(runtime, "globalThis.hifLog.length >= 3", "events-hotif-window-wait");
  send_vk(VK_F18);
  pump(runtime, 500ms, "events-hotif-window-pump");
  run(runtime,
      "if (globalThis.hifLog.length !== 3 || globalThis.hifLog[2] !== 'winNotExist')\n"
      "  throw new Error('winNotExist must fire and winActive must not: ' +\n"
      "                  JSON.stringify(globalThis.hifLog));\n",
      "events-hotif-window-result.mjs");

  // The hook stays while registrations need it, and a forced uninstall is
  // visible both as a return value and as silence until it is reinstalled.
  run(runtime,
      "globalThis.hook.withHotkey = input.installKeybdHook(false);\n"
      "globalThis.hook.forcedOff = input.installKeybdHook(false, true);\n",
      "events-hook-force.mjs");
  run(runtime,
      "if (globalThis.hook.withHotkey !== true)\n"
      "  throw new Error('a live registration must keep the hook installed');\n"
      "if (globalThis.hook.forcedOff !== false)\n"
      "  throw new Error('forced uninstall must report the hook as down');\n",
      "events-hook-force-check.mjs");
  send_vk(VK_F24);
  pump(runtime, 400ms, "events-hook-force-pump");
  run(runtime,
      "if (globalThis.hkLog.length !== 4)\n"
      "  throw new Error('no events may reach a forced-off hook: ' + globalThis.hkLog.length);\n"
      "globalThis.hook.forcedOn = input.installKeybdHook(true);",
      "events-hook-restore.mjs");
  send_vk(VK_F24);
  wait_js(runtime, "globalThis.hkLog.length >= 5", "events-hook-restore-wait");

  // Close: the first close releases the registration, the second is false,
  // and a later press dispatches nothing.
  run(runtime,
      "globalThis.hkClosed = globalThis.hkSub.close();\n"
      "globalThis.hkClosedAgain = globalThis.hkSub.close();\n"
      "globalThis.ahkClosed = globalThis.ahkSub.close();\n"
      "globalThis.unknownClosed = globalThis.actionSub.close();\n"
      "globalThis.unknownClosedAgain = globalThis.actionSub.close();\n"
      "globalThis.neverSub.close();\n"
      "globalThis.alwaysSub.close();\n"
      "globalThis.plainSub.close();\n"
      "globalThis.guardSub.close();\n"
      "globalThis.winSub.close();\n"
      "globalThis.noFireSub.close();",
      "events-hotkey-close.mjs");
  run(runtime,
      "if (!globalThis.hkClosed) throw new Error('first close must be true');\n"
      "if (globalThis.hkClosedAgain) throw new Error('second close must be false');\n"
      "if (!globalThis.unknownClosed) throw new Error('action close must be true');\n"
      "if (globalThis.unknownClosedAgain) throw new Error('action second close must be false');\n",
      "events-hotkey-close-check.mjs");
  const int actions_after_close = probe->executed();
  send_vk(VK_F23);
  send_vk(VK_F24);
  pump(runtime, 500ms, "events-hotkey-close-pump");
  run(runtime,
      "if (globalThis.hkLog.length !== 5)\n"
      "  throw new Error('closed hotkey must stay silent: ' + globalThis.hkLog.length);\n",
      "events-hotkey-closed-frozen.mjs");
  assert(probe->executed() == actions_after_close);

  // setTimer: argument contract, periodic and run-once periods, the period-0
  // delete, and priority ordering inside one tick.
  run(runtime,
      "globalThis.timerErrors = {};\n"
      "expectT(globalThis.timerErrors, 'arity', () => input.setTimer());\n"
      "expectT(globalThis.timerErrors, 'notFunction', () => input.setTimer(42));\n"
      "expectT(globalThis.timerErrors, 'fraction', () => input.setTimer(() => {}, 1.5));\n"
      "expectT(globalThis.timerErrors, 'periodNotNumber',\n"
      "        () => input.setTimer(() => {}, 'x'));\n"
      "expectT(globalThis.timerErrors, 'priorityNotNumber',\n"
      "        () => input.setTimer(() => {}, 100, 'x'));\n"
      "globalThis.ticks = 0;\n"
      "globalThis.tickFn = () => { globalThis.ticks += 1; };\n"
      "globalThis.tickSub = input.setTimer(globalThis.tickFn, 60);\n"
      "globalThis.onceTicks = 0;\n"
      "globalThis.onceSub = input.setTimer(() => { globalThis.onceTicks += 1; }, -80);\n"
      "globalThis.zeroFresh = input.setTimer(() => {}, 0);\n"
      "globalThis.zeroExisting = input.setTimer(globalThis.tickFn, 0);\n"
      "globalThis.tickSub2 = input.setTimer(globalThis.tickFn, 60);\n",
      "events-timer.mjs");
  run(runtime,
      "const errors = globalThis.timerErrors;\n"
      "for (const name of ['arity','notFunction','fraction','periodNotNumber',\n"
      "                    'priorityNotNumber']) {\n"
      "  checkT(errors, name);\n"
      "}\n"
      "if (globalThis.tickSub.kind !== 'timer') throw new Error('timer kind must be timer');\n"
      "if (globalThis.zeroFresh !== null || globalThis.zeroExisting !== null)\n"
      "  throw new Error('period 0 must delete and return null');\n"
      "if (globalThis.tickSub2.id === globalThis.tickSub.id)\n"
      "  throw new Error('a re-created timer must get a new id');\n",
      "events-timer-check.mjs");
  wait_js(runtime, "globalThis.ticks >= 2 && globalThis.onceTicks >= 1",
          "events-timer-fire");
  run(runtime,
      "globalThis.onceSeen = globalThis.onceTicks;",
      "events-timer-once-snapshot.mjs");
  pump(runtime, 350ms, "events-timer-once-pump");
  run(runtime,
      "if (globalThis.onceTicks !== globalThis.onceSeen)\n"
      "  throw new Error('a run-once timer must fire exactly once');\n"
      "globalThis.order = [];\n"
      "globalThis.lowSub = input.setTimer(() => { globalThis.order.push('low'); }, 120, -1);\n"
      "globalThis.highSub = input.setTimer(() => { globalThis.order.push('high'); }, 120, 10);\n",
      "events-timer-priority.mjs");
  // Both timers come due while the JS lane is busy, so the first tick after
  // it yields collects them together and priority decides the order.
  run(runtime,
      "const until = Date.now() + 400;\n"
      "while (Date.now() < until) {}",
      "events-timer-busy.mjs");
  wait_js(runtime, "globalThis.order.length >= 2", "events-timer-priority-wait");
  run(runtime,
      "if (globalThis.order[0] !== 'high' || globalThis.order[1] !== 'low')\n"
      "  throw new Error('priority must order a shared tick: ' +\n"
      "                  JSON.stringify(globalThis.order));\n"
      "globalThis.lowSub.close();\n"
      "globalThis.highSub.close();\n"
      "globalThis.tickSub2.close();\n"
      "globalThis.onceSub.close();\n",
      "events-timer-priority-check.mjs");

  // Coalescing: a 1ms period driving a 100ms handler may only run at handler
  // speed, and the backlog after close stays bounded (one queued tick), so
  // quiescence returns in well under the second the naive queue would take.
  run(runtime,
      "globalThis.coarse = 0;\n"
      "globalThis.spinFn = () => {\n"
      "  globalThis.coarse += 1;\n"
      "  const until = Date.now() + 100;\n"
      "  while (Date.now() < until) {}\n"
      "};\n"
      "globalThis.spinSub = input.setTimer(globalThis.spinFn, 1);",
      "events-timer-spin.mjs");
  pump(runtime, 400ms, "events-timer-spin-pump");
  run(runtime,
      "globalThis.coarseSeen = globalThis.coarse;\n"
      "globalThis.spinSub.close();",
      "events-timer-spin-close.mjs");
  run(runtime,
      "if (globalThis.coarseSeen < 2 || globalThis.coarseSeen > 10)\n"
      "  throw new Error('a 1ms timer with a 100ms handler must run at handler speed: ' +\n"
      "                  globalThis.coarseSeen);\n",
      "events-timer-spin-check.mjs");
  {
    const auto settle_started = std::chrono::steady_clock::now();
    const auto settle_error = runtime.settle(3000ms);
    const auto settle_elapsed = std::chrono::steady_clock::now() - settle_started;
    assert(settle_error.ok());
    assert(settle_elapsed < 1500ms);
  }

  // OnMessage: a posted window message reaches the monitor with its full
  // payload, the (msg, fn) pair dedupes onto one subscription, and
  // maxInstances 0 is AHK's delete signal.
  run(runtime,
      "globalThis.msgs = [];\n"
      "globalThis.msgFn = ev => { globalThis.msgs.push(ev); };\n"
      "globalThis.msgSub = input.onMessage(0x6001, globalThis.msgFn, 4);\n"
      "globalThis.msgDup = input.onMessage(0x6001, globalThis.msgFn, 9);\n"
      "globalThis.goneLog = 0;\n"
      "globalThis.goneFn = () => { globalThis.goneLog += 1; };\n"
      "input.onMessage(0x6002, globalThis.goneFn, 2);\n"
      "globalThis.goneDelete = input.onMessage(0x6002, globalThis.goneFn, 0);\n"
      "globalThis.goneDeleteAgain = input.onMessage(0x6002, globalThis.goneFn, 0);\n"
      "globalThis.msgErrors = {};\n"
      "expectT(globalThis.msgErrors, 'arity', () => input.onMessage(0x6001));\n"
      "expectT(globalThis.msgErrors, 'notFunction', () => input.onMessage(0x6001, 42));\n"
      "expectT(globalThis.msgErrors, 'negative', () => input.onMessage(-1, () => {}));\n"
      "expectT(globalThis.msgErrors, 'maxNegative',\n"
      "        () => input.onMessage(0x6003, () => {}, -1));\n"
      "expectT(globalThis.msgErrors, 'notNumber', () => input.onMessage('x', () => {}));\n",
      "events-message.mjs");
  run(runtime,
      "if (globalThis.msgSub.kind !== 'message') throw new Error('message kind mismatch');\n"
      "if (globalThis.msgDup.id !== globalThis.msgSub.id)\n"
      "  throw new Error('same (msg, fn) must reuse one id');\n"
      "if (globalThis.goneDelete !== null || globalThis.goneDeleteAgain !== null)\n"
      "  throw new Error('maxInstances 0 must delete and return null');\n"
      "const errors = globalThis.msgErrors;\n"
      "for (const name of ['arity','notFunction','negative','maxNegative','notNumber']) {\n"
      "  checkT(errors, name);\n"
      "}\n",
      "events-message-check.mjs");
  {
    const std::uintptr_t pump_window = window_service.ui().message_window();
    assert(pump_window != 0);
    const BOOL posted_one =
        PostMessageW(reinterpret_cast<HWND>(pump_window), 0x6001, 123, 456);
    if (posted_one == FALSE) return fail("PostMessage 0x6001 failed");
    const BOOL posted_two =
        PostMessageW(reinterpret_cast<HWND>(pump_window), 0x6002, 7, 8);
    if (posted_two == FALSE) return fail("PostMessage 0x6002 failed");
  }
  wait_js(runtime, "globalThis.msgs.length >= 1", "events-message-wait");
  pump(runtime, 400ms, "events-message-pump");
  run(runtime,
      "const message = globalThis.msgs[0];\n"
      "if (message.msg !== 0x6001) throw new Error('msg mismatch: ' + message.msg);\n"
      "if (message.wParam !== 123) throw new Error('wParam mismatch: ' + message.wParam);\n"
      "if (message.lParam !== 456) throw new Error('lParam mismatch: ' + message.lParam);\n"
      "if (typeof message.hwnd !== 'number' || !(message.hwnd > 0))\n"
      "  throw new Error('hwnd must be a positive number');\n"
      "if (globalThis.goneLog !== 0)\n"
      "  throw new Error('a deleted monitor must not fire: ' + globalThis.goneLog);\n"
      "globalThis.msgClosed = globalThis.msgSub.close();\n"
      "globalThis.msgClosedAgain = globalThis.msgSub.close();",
      "events-message-check2.mjs");
  {
    const std::uintptr_t pump_window = window_service.ui().message_window();
    const BOOL posted = PostMessageW(reinterpret_cast<HWND>(pump_window), 0x6001, 1, 2);
    if (posted == FALSE) return fail("PostMessage for delete case failed");
  }
  pump(runtime, 400ms, "events-message-closed-pump");
  run(runtime,
      "if (globalThis.msgs.length !== 1)\n"
      "  throw new Error('closed monitor must not fire: ' + globalThis.msgs.length);\n"
      "if (!globalThis.msgClosed || globalThis.msgClosedAgain)\n"
      "  throw new Error('message close must be true then false');\n",
      "events-message-closed-check.mjs");

  // Hotstring settings and the spec grammar are plain state (no hook
  // capability required); registration still needs it.
  run(runtime,
      "globalThis.hsErrors = {};\n"
      "expectT(globalThis.hsErrors, 'noColon', () => input.hotstring('btw'));\n"
      "expectT(globalThis.hsErrors, 'badOptions', () => input.hotstring(':K:btw::x'));\n"
      "expectT(globalThis.hsErrors, 'missingOptionsClose',\n"
      "        () => input.hotstring(':btw'));\n"
      "expectT(globalThis.hsErrors, 'missingDoubleColon',\n"
      "        () => input.hotstring('::btw'));\n"
      "expectT(globalThis.hsErrors, 'emptyTrigger', () => input.hotstring('::::'));\n"
      "expectT(globalThis.hsErrors, 'arity', () => input.hotstring());\n"
      "expectT(globalThis.hsErrors, 'endCharsValue',\n"
      "        () => input.hotstring('EndChars', 42));\n"
      "expectT(globalThis.hsErrors, 'mouseResetValue',\n"
      "        () => input.hotstring('MouseReset', 42));\n"
      "expectT(globalThis.hsErrors, 'actionBad', () => input.hotstring('::btw::x', 42));\n"
      "globalThis.endCharsDefault = input.hotstring('EndChars');\n"
      "globalThis.endCharsSet = input.hotstring('EndChars', '!');\n"
      "globalThis.endCharsRead = input.hotstring('EndChars');\n"
      "input.hotstring('EndChars', globalThis.endCharsDefault);\n"
      "globalThis.mouseDefault = input.hotstring('MouseReset');\n"
      "globalThis.mouseOff = input.hotstring('MouseReset', false);\n"
      "globalThis.mouseOn = input.hotstring('MouseReset', true);\n"
      "globalThis.resetResult = input.hotstring('Reset');\n",
      "events-hotstring-settings.mjs");
  run(runtime,
      "const errors = globalThis.hsErrors;\n"
      "for (const name of ['noColon','badOptions','missingOptionsClose',\n"
      "                    'missingDoubleColon','emptyTrigger','arity','endCharsValue',\n"
      "                    'mouseResetValue','actionBad']) {\n"
      "  checkT(errors, name);\n"
      "}\n"
      "if (globalThis.endCharsSet !== '!' || globalThis.endCharsRead !== '!')\n"
      "  throw new Error('EndChars must round trip');\n"
      "if (typeof globalThis.endCharsDefault !== 'string')\n"
      "  throw new Error('EndChars must read back as a string');\n"
      "if (globalThis.mouseDefault !== true || globalThis.mouseOff !== false ||\n"
      "    globalThis.mouseOn !== true)\n"
      "  throw new Error('MouseReset must round trip');\n"
      "if (globalThis.resetResult !== null) throw new Error('Reset must return null');\n",
      "events-hotstring-settings-check.mjs");

  EditTarget edit{};
  if (!focus_edit(edit)) {
    std::fprintf(stderr, "events slice could not focus its edit control\n");
    return 1;
  }
  // The observer-form trigger stays in the buffer (it never injects), so the
  // replacement below is appended to it; case is not asserted because the
  // desktop's modifier state decides how the typed trigger renders.
  const std::wstring first_replacement = L"btw2 by the way ";
  const std::wstring after_reenable = L"btw2 by the way btw by the way ";
  // End to end: an observer form fires on the trigger plus end char, the
  // replacement form queues an input.send action (asserted as IR) and lands
  // as text in the focused edit control, and the control form silences it.
  // One pass of that exchange; the caller retries it from a clean buffer.
  const auto hotstring_pass = [&]() -> bool {
    run(runtime,
        "globalThis.hsLog = 0;\n"
        "globalThis.observerSub = input.hotstring('::btw2::replaced',\n"
        "                                        () => { globalThis.hsLog += 1; });\n"
        "globalThis.replSub = input.hotstring('::btw::by the way');",
        "events-hotstring-register.mjs");
    run(runtime,
        "if (globalThis.observerSub.kind !== 'hotstring')\n"
        "  throw new Error('hotstring kind mismatch');\n"
        "if (globalThis.replSub.kind !== 'hotstring')\n"
        "  throw new Error('replacement hotstring kind mismatch');\n",
        "events-hotstring-register-check.mjs");
    const int replacements_before = recording->executed();

    if (!type_ascii("btw2 ", edit)) {
      report_edit("could not focus the edit to type the observer trigger", edit.edit);
      return false;
    }
    if (!wait_for([&] { return contains_ci(edit_text(edit.edit), L"btw"); })) {
      report_edit("typed trigger never reached the edit", edit.edit);
      return false;
    }
    run(runtime,
        "globalThis.waitedFor = 'pending';\n"
        "waitFor(() => globalThis.hsLog >= 1, 4000).then(v => { globalThis.waitedFor = v; });",
        "events-hotstring-observer.mjs");
    settle_pumping(runtime, 7000ms);
    const auto observer_error =
        runtime
            .evaluate_module("if (globalThis.waitedFor !== true) {\n"
                             "  throw new Error('observer never fired: hsLog=' + globalThis.hsLog +\n"
                             "                  ' waitedFor=' + globalThis.waitedFor);\n"
                             "}\n",
                             "events-hotstring-observer-check.mjs")
            .get();
    if (!observer_error.ok()) {
      std::fprintf(stderr, "  js: %s\n", observer_error.message.c_str());
      // What the hook actually saw: a stray navigation key or a mouse move
      // resets the hotstring stream, which is invisible in the edit text.
      const auto history_error =
          runtime
              .evaluate_module(
                  "const history = input.keyHistory({ maxEvents: 64 });\n"
                  "const parts = history.events.map(entry =>\n"
                  "    (entry.down ? 'D' : 'U') + entry.vk +\n"
                  "    (entry.injected ? 'i' : '') + (entry.selfInjected ? 'S' : ''));\n"
                  "throw new Error('key history: ' + parts.join(' '));\n",
                  "events-hotstring-history.mjs")
              .get();
      std::fprintf(stderr, "  %s\n", history_error.message.c_str());
      report_edit("observer hotstring never fired", edit.edit);
      return false;
    }
    run(runtime,
        "if (globalThis.hsLog !== 1)\n"
        "  throw new Error('observer hotstring must fire once: ' + globalThis.hsLog);\n",
        "events-hotstring-observer-check.mjs");

  if (!type_ascii("btw ", edit)) {
    report_edit("could not focus the edit to type the replacement trigger", edit.edit);
    return false;
  }
    const bool recorded_replacement = wait_for([&] { return recording->executed() > replacements_before; });
  if (!recorded_replacement) {
    report_edit("hotstring replacement never executed an action", edit.edit);
    return false;
  }
    const bool replacement_landed = wait_for(
        [&] { return contains_ci(edit_text(edit.edit), first_replacement); });
    if (!replacement_landed) {
      report_edit("replacement never landed", edit.edit);
      return false;
    }
    assert(recording->source_kind() == "hotstring");
    assert(recording->payload().find("\"vk\":8") != std::string::npos);
    assert(recording->payload().find("\"vk\":98") != std::string::npos);

    run(runtime,
        "globalThis.hsOff = input.hotstring('::btw::by the way', 'off');",
        "events-hotstring-off.mjs");
  if (!type_ascii("btw ", edit)) {
    report_edit("could not focus the edit to type the disabled trigger", edit.edit);
    return false;
  }
    pump(runtime, 500ms, "events-hotstring-off-pump");
    {
      const std::wstring typed_while_off = edit_text(edit.edit);
      if (!contains_ci(typed_while_off, L"btw ")) {
        std::fprintf(stderr, "disabled hotstring must leave the typed trigger in place\n");
        return false;
      }
    }
    run(runtime,
        "if (!globalThis.hsOff || globalThis.hsOff.id !== globalThis.replSub.id)\n"
        "  throw new Error('control form must reuse the registration id');\n"
        "globalThis.hsOn = input.hotstring('::btw::by the way', 'on');\n"
        "globalThis.hsClosed = globalThis.observerSub.close();\n"
        "globalThis.hsClosedAgain = globalThis.observerSub.close();",
        "events-hotstring-on.mjs");
    // The trigger typed while the hotstring was off stays in the buffer, so the
    // re-enabled run erases only the trigger it just saw.
  if (!type_ascii("btw ", edit)) {
    report_edit("could not focus the edit to type the re-enabled trigger", edit.edit);
    return false;
  }
    const bool reenabled_landed =
        wait_for([&] { return contains_ci(edit_text(edit.edit), after_reenable); });
    if (!reenabled_landed) {
      report_edit("re-enabled replacement never landed", edit.edit);
      return false;
    }
    run(runtime,
        "if (!globalThis.hsClosed || globalThis.hsClosedAgain)\n"
        "  throw new Error('hotstring close must be true then false');\n",
        "events-hotstring-close-check.mjs");
  if (!type_ascii("btw2 ", edit)) {
    report_edit("could not focus the edit to type the closed trigger", edit.edit);
    return false;
  }
    pump(runtime, 500ms, "events-hotstring-closed-pump");
    run(runtime,
        "if (globalThis.hsLog !== 1)\n"
        "  throw new Error('closed hotstring must stay silent: ' + globalThis.hsLog);\n",
        "events-hotstring-closed-check.mjs");

    // Wildcard: fires on the trigger alone (no end char) and still erases the
    // typed trigger before injecting its replacement.
    run(runtime,
        "globalThis.wildSub = input.hotstring(':*:wldx::wild');\n"
        "globalThis.optionsOnly = input.hotstring(':*:');\n"
        "globalThis.optionsReset = input.hotstring(':*0:');",
        "events-hotstring-wild.mjs");
  if (!type_ascii("wldx", edit)) {
    report_edit("could not focus the edit to type the wildcard trigger", edit.edit);
    return false;
  }
    const bool wild_landed = wait_for([&] {
      return contains_ci(edit_text(edit.edit), L"wild");
    });
    if (!wild_landed) {
      std::fprintf(stderr, "  recorded actions: %d (baseline %d)\n", recording->executed(),
                   replacements_before);
      report_edit("wildcard replacement never landed", edit.edit);
      return false;
    }
    run(runtime,
        "if (globalThis.optionsOnly !== null || globalThis.optionsReset !== null)\n"
        "  throw new Error('options-only form must return null');\n"
        "globalThis.wildSub.close();\n"
        "globalThis.replSub.close();",
        "events-hotstring-wild-check.mjs");
    if (recording->executed() <= replacements_before + 1) {
      report_edit("expected three recorded hotstring actions", edit.edit);
      return false;
    }
    return true;
  };

  // The desktop is shared with whatever else is running: a stray key resets
  // the hotstring stream and a stolen foreground swallows the injected text.
  // Clear both the buffer and the edit and run the exchange again instead of
  // failing on input this slice does not own.
  bool hotstring_passed = false;
  for (int attempt = 0; attempt < 3 && !hotstring_passed; ++attempt) {
    if (attempt > 0) {
      (void)SetWindowTextW(edit.edit, L"");
      (void)SendMessageW(edit.edit, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
      pump_window_messages();
      run(runtime, "input.hotstring('Reset');", "events-hotstring-reset.mjs");
    }
    hotstring_passed = hotstring_pass();
  }
  if (!hotstring_passed) {
    report_edit("hotstring exchange never converged", edit.edit);
    return fail("hotstring end to end exchange");
  }
  (void)DestroyWindow(edit.window);

  // OnClipboardChange: the service marks the first notification after our own
  // write (AHK type 1), a foreign write reports type 0.
  run(runtime,
      "globalThis.clipTypes = [];\n"
      "globalThis.clipSub = input.onClipboardChange(ev => {\n"
      "  globalThis.clipTypes.push(ev.type);\n"
      "});\n"
      "globalThis.clipErrors = {};\n"
      "expectT(globalThis.clipErrors, 'arity', () => input.onClipboardChange());\n"
      "expectT(globalThis.clipErrors, 'notFunction',\n"
      "        () => input.onClipboardChange(42));\n",
      "events-clipboard.mjs");
  run(runtime,
      "if (globalThis.clipSub.kind !== 'clipboard')\n"
      "  throw new Error('clipboard kind mismatch');\n"
      "checkT(globalThis.clipErrors, 'arity');\n"
      "checkT(globalThis.clipErrors, 'notFunction');\n",
      "events-clipboard-check.mjs");
  std::string clipboard_before;
  const bool had_clipboard_text = clipboard_service.read_text(clipboard_before).ok();
  assert(clipboard_service.write_text("rime-events-slice-self").ok());
  wait_js(runtime, "globalThis.clipTypes.length >= 1", "events-clipboard-self");
  run(runtime,
      "if (globalThis.clipTypes[0] !== 1)\n"
      "  throw new Error('own change must report type 1: ' + globalThis.clipTypes[0]);\n",
      "events-clipboard-self-check.mjs");
  if (!write_foreign_text(L"rime-events-slice-foreign"))
    return fail("raw clipboard write failed");
  wait_js(runtime, "globalThis.clipTypes.length >= 2", "events-clipboard-foreign");
  run(runtime,
      "if (globalThis.clipTypes[1] !== 0)\n"
      "  throw new Error('foreign change must report type 0: ' + globalThis.clipTypes[1]);\n"
      "globalThis.clipClosed = globalThis.clipSub.close();\n"
      "globalThis.clipClosedAgain = globalThis.clipSub.close();\n",
      "events-clipboard-foreign-check.mjs");
  if (had_clipboard_text) {
    assert(clipboard_service.write_text(clipboard_before).ok());
  }

  // OnError: an unhandled rejection reaches the observer as {where, message},
  // and an observer that always throws stays bounded (loop prevention).
  run_expect_error(runtime,
      "globalThis.errLogs = [];\n"
      "globalThis.throwCount = 0;\n"
      "globalThis.errSub = input.onError(ev => { globalThis.errLogs.push(ev); });\n"
      "globalThis.throwSub = input.onError(ev => {\n"
      "  globalThis.throwCount += 1;\n"
      "  throw new Error('observer boom');\n"
      "});\n"
      "globalThis.errErrors = {};\n"
      "expectT(globalThis.errErrors, 'arity', () => input.onError());\n"
      "expectT(globalThis.errErrors, 'notFunction', () => input.onError(42));\n"
      "Promise.reject(new Error('events-slice-boom'));",
      "events-error.mjs", "events-slice-boom");
  run(runtime,
      "if (globalThis.errSub.kind !== 'error') throw new Error('error kind mismatch');\n"
      "checkT(globalThis.errErrors, 'arity');\n"
      "checkT(globalThis.errErrors, 'notFunction');\n",
      "events-error-check.mjs");
  wait_js(runtime, "globalThis.errLogs.length >= 1", "events-error-wait");
  pump(runtime, 400ms, "events-error-pump");
  run(runtime,
      "const boom = globalThis.errLogs.find(e => String(e.message).includes('events-slice-boom'));\n"
      "if (!boom) throw new Error('rejection must reach onError: ' +\n"
      "                          JSON.stringify(globalThis.errLogs));\n"
      "if (boom.where !== 'promise') throw new Error('rejection where must be promise: ' +\n"
      "                                             boom.where);\n"
      "if (globalThis.throwCount < 1 || globalThis.throwCount > 3)\n"
      "  throw new Error('a throwing observer must stay bounded: ' +\n"
      "                  globalThis.throwCount);\n",
      "events-error-check2.mjs");

  // Lifetime: stop runs the onExit handlers on the JS thread, then the host
  // teardown closes every registration the slice left open.
  assert(runtime.stop().ok());
  assert(binding.events != nullptr);
  assert(binding.events->closed);
  assert(binding.events->open_count() == 0);
  assert(binding.events->dispatch_subscription == 0);
  assert(!binding.events->dispatch_live());
  assert(mark_seen("exit:stop"));

  // Capability gate: a fresh runtime with an empty policy sees the hook and
  // clipboard gates denied by Error naming the capability, while everything
  // without a capability keeps working.
  {
    rime::action::Kernel denied_kernel(std::make_shared<rime::action::StaticCapabilityPolicy>(
        std::unordered_set<std::string>{}));
    rime::action::Dispatcher denied_dispatcher(denied_kernel,
                                               rime::action::default_dispatch_policy());
    std::atomic<std::uint64_t> denied_next_action_id{0};
    rime::win32::InputModuleBinding denied_binding;
    denied_binding.service = &service;
    denied_binding.window_service = &window_service;
    denied_binding.clipboard_service = &clipboard_service;
    denied_binding.kernel = &denied_kernel;
    denied_binding.dispatcher = &denied_dispatcher;
    denied_binding.next_action_id = &denied_next_action_id;
    rime::js::Runtime denied_runtime;
    assert(rime::win32::register_input_module(denied_runtime, &denied_binding).ok());
    assert(denied_runtime.start().ok());
    run(denied_runtime,
        "import { input } from 'rime:input';\n"
        "globalThis.denied = {};\n"
        "globalThis.allowed = {};\n"
        "const deny = (name, fn, capability) => {\n"
        "  try { fn(); } catch (e) {\n"
        "    globalThis.denied[name] =\n"
        "        (e instanceof Error && String(e.message).includes(capability));\n"
        "  }\n"
        "};\n"
        "deny('hotkey', () => input.hotkey('f24', () => {}), 'windows.hook.global');\n"
        "deny('hotstring', () => input.hotstring('::btw::x'), 'windows.hook.global');\n"
        "deny('installHook', () => input.installKeybdHook(), 'windows.hook.global');\n"
        "deny('clipboard', () => input.onClipboardChange(() => {}), 'windows.clipboard.read');\n"
        "globalThis.allowed.settings = (input.hotstring('EndChars') !== undefined);\n"
        "globalThis.allowed.timer =\n"
        "    (typeof input.setTimer(() => {}, 60000).close === 'function');\n"
        "globalThis.allowed.message =\n"
        "    (typeof input.onMessage(0x6001, () => {}).close === 'function');\n"
        "globalThis.allowed.error =\n"
        "    (typeof input.onError(() => {}).close === 'function');\n"
        "globalThis.allowed.exit =\n"
        "    (typeof input.onExit(() => {}).close === 'function');\n"
        "input.hotIf(() => true);\n"
        "globalThis.allowed.hotIf = (input.hotIf(null).kind === 'function');\n"
        "globalThis.allowed.window =\n"
        "    (input.hotIfWinNotExist('RimeEventsSliceNoSuchClass').kind === 'none');\n",
        "events-deny.mjs");
    assert(denied_runtime.settle(5000ms).ok());
    run(denied_runtime,
        "for (const name of ['hotkey','hotstring','installHook','clipboard']) {\n"
        "  if (!globalThis.denied[name])\n"
        "    throw new Error('expected a capability denial for ' + name);\n"
        "}\n"
        "for (const name of Object.keys(globalThis.allowed)) {\n"
        "  if (!globalThis.allowed[name])\n"
        "    throw new Error('expected ' + name + ' to stay allowed');\n"
        "}\n",
        "events-deny-check.mjs");
    assert(denied_runtime.stop().ok());
    assert(denied_binding.events != nullptr);
    assert(denied_binding.events->closed);
    assert(denied_binding.events->open_count() == 0);
  }

  // Embedder unload: a live hotkey subscription refuses the unload with its
  // own reason, closing it lets teardown run and unload succeeds.
  {
    rime::win32::InputModuleBinding abi_binding;
    abi_binding.service = &service;
    abi_binding.window_service = &window_service;
    abi_binding.clipboard_service = &clipboard_service;
    abi_binding.kernel = &kernel;
    abi_binding.dispatcher = &dispatcher;
    abi_binding.next_action_id = &next_action_id;
    rime::js::HostAbi abi;
    assert(rime::win32::register_input_module(abi.host(), &abi_binding).ok());
    assert(abi.load("import { input } from 'rime:input';\n"
                    "globalThis.abiSub = input.hotkey('f22', () => {});",
                    "abi-hotkey.mjs")
               .ok());
    assert(abi.execute().ok());
    const auto busy = abi.unload();
    assert(!busy.ok());
    assert(busy.code == rime::core::Error::Code::InvalidState);
    assert(busy.message.find("subscription") != std::string::npos);
    assert(abi.state() == rime::js::HostAbiState::Executed);
    assert(abi.execute("import { input } from 'rime:input';\n"
                       "globalThis.abiClosed = globalThis.abiSub.close();",
                       "abi-close.mjs")
               .ok());
    assert(abi.unload().ok());
    assert(abi.state() == rime::js::HostAbiState::Unloaded);
    assert(abi_binding.events != nullptr);
    assert(abi_binding.events->closed);
    assert(abi_binding.events->open_count() == 0);
  }

  assert(window_service.stop().ok());
  assert(service.stop().ok());
  assert(service.stop().ok());
  return 0;
}
