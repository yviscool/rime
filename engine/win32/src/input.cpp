#include "rime/win32/input.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace rime::win32 {
namespace {

constexpr UINT kQuitMessage = WM_APP + 77;
constexpr UINT kWakeMessage = WM_APP + 78;
constexpr std::size_t kPendingCapacity = 2048;
constexpr auto kInstallTimeout = std::chrono::seconds(2);

bool async_key_down(const int virtual_key) {
  return (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
}

// Extended-key list per SendInput docs: without KEYEVENTF_EXTENDEDKEY these
// arrive with the wrong scan semantics (arrows collapse to the numeric pad,
// right-hand modifiers to their left twins).
bool is_extended_vk(const std::uint32_t vk) {
  switch (vk) {
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_UP:
    case VK_RIGHT:
    case VK_DOWN:
    case VK_NUMLOCK:
    case VK_SCROLL:
    case VK_RCONTROL:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
    case VK_APPS:
      return true;
    default:
      return false;
  }
}

// AHK MOUSE_COORD_TO_ABS (keyboard_mouse.cpp, MouseMove conversion comment):
//   ((65536 * coord) / extent) + (coord < 0 ? -1 : 1)
// Applied against SM_CXSCREEN/SM_CYSCREEN like AHK's primary-only SendInput
// path; the +-1 avoids snapping pixel 0 to a rounded-down boundary. The
// 64-bit intermediate keeps extreme coordinates free of signed overflow
// (the in-range result matches the 32-bit AHK expression).
int mouse_coord_to_abs(const int coord, const int extent) {
  if (extent <= 0) return 0;
  const auto scaled = (65536ll * coord) / extent;
  return static_cast<int>(scaled) + (coord < 0 ? -1 : 1);
}

}  // namespace

struct InputService::Impl {
  struct Entry {
    std::uint64_t id{0};
    Callback callback;
    bool closed{false};
    int in_flight{0};
    std::condition_variable idle;
  };

  // Shared state; the mutex also orders subscribe/unsubscribe against
  // delivery in-flight counters.
  std::mutex mutex;
  std::condition_variable condition;
  InputServiceState state{InputServiceState::Created};
  bool stopping{false};
  bool ready{false};
  bool install_ok{false};
  DWORD thread_id{0};
  std::thread thread;

  std::vector<std::shared_ptr<Entry>> entries;
  std::vector<InputEvent> pending;
  std::uint64_t next_sequence{1};
  std::uint64_t next_id{1};
  std::uint64_t dropped{0};

  // Serializes send() batches against each other; stop() joins an in-flight
  // batch before unhooking so a refused-later send can never inject into a
  // desktop the hooks no longer observe.
  std::mutex send_mutex;

  // Hook-thread only.
  HHOOK keyboard_hook{nullptr};
  HHOOK mouse_hook{nullptr};

  // The single running service owns the low-level hook callbacks (they
  // receive no per-hook user pointer).
  static std::atomic<Impl*> owner;

  void enqueue(InputEvent event) {
    DWORD hook_thread = 0;
    {
      std::lock_guard lock(mutex);
      event.sequence = next_sequence++;
      if (pending.size() >= kPendingCapacity) {
        pending.erase(pending.begin());
        ++dropped;
      }
      pending.push_back(std::move(event));
      // Snapshot thread_id under the same lock: it is written by thread_main
      // and cleared on shutdown, so an unlocked read races.
      hook_thread = thread_id;
    }
    // The delivery loop drains pending only when GetMessage returns; wake it.
    if (hook_thread != 0) PostThreadMessageW(hook_thread, kWakeMessage, 0, 0);
  }

  void deliver_events() {
    std::vector<InputEvent> batch;
    std::vector<std::shared_ptr<Entry>> targets;
    {
      std::lock_guard lock(mutex);
      batch.swap(pending);
      targets.assign(entries.begin(), entries.end());
    }
    for (const auto& event : batch) {
      for (const auto& entry : targets) {
        bool run = false;
        {
          std::lock_guard lock(mutex);
          if (!entry->closed) {
            entry->in_flight++;
            run = true;
          }
        }
        if (!run) continue;
        try {
          entry->callback(event);
        } catch (...) {
          // Subscribers must not throw across the hook boundary.
        }
        {
          std::lock_guard lock(mutex);
          entry->in_flight--;
          entry->idle.notify_all();
        }
      }
    }
  }

  void cleanup_hooks() {
    if (mouse_hook) {
      UnhookWindowsHookEx(mouse_hook);
      mouse_hook = nullptr;
    }
    if (keyboard_hook) {
      UnhookWindowsHookEx(keyboard_hook);
      keyboard_hook = nullptr;
    }
  }

  void thread_main();
  static LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam);
  static LRESULT CALLBACK mouse_proc(int code, WPARAM wparam, LPARAM lparam);
};

std::atomic<InputService::Impl*> InputService::Impl::owner{nullptr};

void InputService::Impl::thread_main() {
  {
    std::lock_guard lock(mutex);
    thread_id = GetCurrentThreadId();
  }
  keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_proc, GetModuleHandleW(nullptr), 0);
  mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, GetModuleHandleW(nullptr), 0);
  const bool installed = keyboard_hook != nullptr && mouse_hook != nullptr;
  {
    std::lock_guard lock(mutex);
    install_ok = installed;
    ready = true;
  }
  condition.notify_all();
  if (!installed) {
    cleanup_hooks();
    Impl* expected = this;
    owner.compare_exchange_strong(expected, nullptr);
    return;
  }

  MSG message;
  for (;;) {
    const BOOL received = GetMessageW(&message, nullptr, 0, 0);
    // GetMessageW returns -1 on error and 0 on WM_QUIT; both exit the pump
    // the same way. The error detail (GetLastError) is deliberately not acted
    // on here to keep the pump shape unchanged; hook cleanup below still runs
    // on either path.
    if (received <= 0 || message.message == kQuitMessage) break;
    {
      std::lock_guard lock(mutex);
      if (stopping) break;
    }
    deliver_events();
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  cleanup_hooks();
  {
    std::lock_guard lock(mutex);
    for (auto& entry : entries) entry->closed = true;
    entries.clear();
    pending.clear();
  }
  Impl* expected = this;
  owner.compare_exchange_strong(expected, nullptr);
}

LRESULT CALLBACK InputService::Impl::keyboard_proc(const int code, const WPARAM wparam,
                                                   const LPARAM lparam) {
  if (code == HC_ACTION) {
    Impl* self = owner.load(std::memory_order_acquire);
    if (self) {
      const auto* data = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
      const bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
      const bool up = wparam == WM_KEYUP || wparam == WM_SYSKEYUP;
      if (down || up) {
        InputEvent event;
        event.kind = InputEventKind::Key;
        event.timestamp_ms = data->time;
        event.injected = (data->flags & LLKHF_INJECTED) != 0;
        event.self_injected =
            event.injected &&
            data->dwExtraInfo == static_cast<ULONG_PTR>(k_self_injected_marker);
        event.key_down = down;
        event.vk = data->vkCode;
        event.scan = data->scanCode;
        event.alt = (data->flags & LLKHF_ALTDOWN) != 0;
        event.control = async_key_down(VK_CONTROL);
        event.shift = async_key_down(VK_SHIFT);
        event.super = async_key_down(VK_LWIN) || async_key_down(VK_RWIN);
        self->enqueue(event);
      }
    }
  }
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK InputService::Impl::mouse_proc(const int code, const WPARAM wparam,
                                                const LPARAM lparam) {
  if (code == HC_ACTION) {
    Impl* self = owner.load(std::memory_order_acquire);
    if (self) {
      const auto* data = reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
      InputEvent event;
      event.kind = InputEventKind::Mouse;
      event.timestamp_ms = data->time;
      event.injected = (data->flags & LLMHF_INJECTED) != 0;
      // WH_MOUSE_LL reports dwExtraInfo zero-extended from 32 bits on this
      // Windows (probed: 0xDEADBEEF00000042 arrives as 0x42) while the
      // keyboard hook preserves all 64 bits, so the marker is matched on its
      // low half here and in full on the keyboard path.
      event.self_injected =
          event.injected &&
          static_cast<std::uint32_t>(data->dwExtraInfo) ==
              static_cast<std::uint32_t>(k_self_injected_marker);
      event.x = data->pt.x;
      event.y = data->pt.y;
      bool valid = false;
      switch (wparam) {
        case WM_MOUSEMOVE:
          event.mouse_action = MouseAction::Move;
          valid = true;
          break;
        case WM_LBUTTONDOWN:
          event.mouse_action = MouseAction::Down;
          event.button = 1;
          valid = true;
          break;
        case WM_LBUTTONUP:
          event.mouse_action = MouseAction::Up;
          event.button = 1;
          valid = true;
          break;
        case WM_RBUTTONDOWN:
          event.mouse_action = MouseAction::Down;
          event.button = 2;
          valid = true;
          break;
        case WM_RBUTTONUP:
          event.mouse_action = MouseAction::Up;
          event.button = 2;
          valid = true;
          break;
        case WM_MBUTTONDOWN:
          event.mouse_action = MouseAction::Down;
          event.button = 3;
          valid = true;
          break;
        case WM_MBUTTONUP:
          event.mouse_action = MouseAction::Up;
          event.button = 3;
          valid = true;
          break;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
          event.mouse_action = MouseAction::Wheel;
          event.wheel_delta = static_cast<std::int32_t>(GET_WHEEL_DELTA_WPARAM(wparam));
          valid = true;
          break;
        default:
          break;
      }
      if (valid) self->enqueue(event);
    }
  }
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

InputService::InputService() : impl_(std::make_unique<Impl>()) {}
InputService::~InputService() { (void)stop(); }

rime::core::Error InputService::start() {
  std::unique_lock lock(impl_->mutex);
  if (impl_->state != InputServiceState::Created) {
    return {rime::core::Error::Code::InvalidState, "input service can only start once"};
  }
  Impl* expected = nullptr;
  if (!Impl::owner.compare_exchange_strong(expected, impl_.get())) {
    return {rime::core::Error::Code::ExecutionFailed,
            "another input service is already running"};
  }
  impl_->state = InputServiceState::Running;
  impl_->stopping = false;
  impl_->ready = false;
  impl_->install_ok = false;
  try {
    impl_->thread = std::thread(&Impl::thread_main, impl_.get());
  } catch (...) {
    Impl::owner.store(nullptr);
    impl_->state = InputServiceState::Stopped;
    return {rime::core::Error::Code::ExecutionFailed, "cannot start the input hook thread"};
  }
  // Hooks install inside the thread; wait for its verdict.
  if (!impl_->condition.wait_for(lock, kInstallTimeout,
                                 [this] { return impl_->ready; })) {
    impl_->stopping = true;
    const DWORD thread_id = impl_->thread_id;
    lock.unlock();
    if (thread_id != 0) PostThreadMessageW(thread_id, kQuitMessage, 0, 0);
    if (impl_->thread.joinable()) impl_->thread.join();
    lock.lock();
    impl_->state = InputServiceState::Stopped;
    return {rime::core::Error::Code::ExecutionFailed, "input hook installation timed out"};
  }
  if (!impl_->install_ok) {
    lock.unlock();
    if (impl_->thread.joinable()) impl_->thread.join();
    lock.lock();
    impl_->state = InputServiceState::Stopped;
    return {rime::core::Error::Code::ExecutionFailed,
            "cannot install low-level keyboard/mouse hooks"};
  }
  return rime::core::Error::none();
}

rime::core::Error InputService::stop() {
  std::unique_lock lock(impl_->mutex);
  if (impl_->state == InputServiceState::Stopped) return rime::core::Error::none();
  if (impl_->state == InputServiceState::Created) {
    impl_->state = InputServiceState::Stopped;
    return rime::core::Error::none();
  }
  if (!impl_->ready) {
    // stop() raced the install phase: wait it out on the same lock.
    impl_->condition.wait(lock, [this] { return impl_->ready; });
  }
  if (impl_->state == InputServiceState::Stopped) return rime::core::Error::none();
  impl_->stopping = true;
  impl_->state = InputServiceState::Stopping;
  std::thread worker = std::move(impl_->thread);
  const DWORD thread_id = impl_->thread_id;
  lock.unlock();
  // Wait out an in-flight send()/send_mouse() batch: it observed Running
  // before the state flip, so let it inject while the hooks are still
  // installed; every later batch observes Stopping and refuses.
  {
    std::lock_guard send_lock(impl_->send_mutex);
  }
  if (thread_id != 0) PostThreadMessageW(thread_id, kQuitMessage, 0, 0);
  if (worker.joinable()) worker.join();
  lock.lock();
  impl_->state = InputServiceState::Stopped;
  return rime::core::Error::none();
}

InputServiceState InputService::state() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->state;
}

std::uint64_t InputService::subscribe(Callback callback) {
  if (!callback) return 0;
  std::lock_guard lock(impl_->mutex);
  if (impl_->state != InputServiceState::Running || impl_->stopping) return 0;
  auto entry = std::make_shared<Impl::Entry>();
  entry->id = impl_->next_id++;
  entry->callback = std::move(callback);
  impl_->entries.push_back(entry);
  return entry->id;
}

bool InputService::unsubscribe(const std::uint64_t id) {
  std::unique_lock lock(impl_->mutex);
  auto found = impl_->entries.end();
  for (auto it = impl_->entries.begin(); it != impl_->entries.end(); ++it) {
    if ((*it)->id == id) {
      found = it;
      break;
    }
  }
  if (found == impl_->entries.end()) return false;
  const auto entry = *found;
  entry->closed = true;
  // impl_->thread_id is read under impl_->mutex here (the lock is held), and
  // enqueue() snapshots it under the same lock, so neither races thread_main.
  const bool called_on_hook_thread =
      impl_->thread_id != 0 && impl_->thread_id == GetCurrentThreadId();
  if (entry->in_flight > 0 && !called_on_hook_thread) {
    // Bounded wait: a wedged subscriber must not hang unsubscribe forever.
    // On timeout the entry is dropped and failure is reported so the caller
    // can diagnose the stuck callback.
    constexpr auto kDrainTimeout = std::chrono::seconds(5);
    const bool drained = entry->idle.wait_for(
        lock, kDrainTimeout, [&entry] { return entry->in_flight == 0; });
    if (!drained) {
      impl_->entries.erase(found);
      return false;
    }
  }
  impl_->entries.erase(found);
  return true;
}

std::size_t InputService::subscription_count() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->entries.size();
}

std::uint64_t InputService::dropped_events() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->dropped;
}

rime::core::Error InputService::send(const std::vector<SendKeyEvent>& keys) {
  if (keys.empty()) {
    return {rime::core::Error::Code::InvalidContract, "send requires at least one key step"};
  }
  for (const auto& step : keys) {
    if (step.unicode) {
      if (step.vk > 0xFFFF) {
        return {rime::core::Error::Code::InvalidContract,
                "unicode key steps require a UTF-16 code unit in 0..65535"};
      }
    } else if (step.vk == 0 || step.vk > 0xFE) {
      return {rime::core::Error::Code::InvalidContract,
              "send key steps require a virtual key in 1..254"};
    }
  }
  std::lock_guard send_lock(impl_->send_mutex);
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state != InputServiceState::Running || impl_->stopping) {
      return {rime::core::Error::Code::InvalidState, "input service is not running"};
    }
  }
  std::vector<INPUT> inputs;
  inputs.reserve(keys.size());
  for (const auto& step : keys) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    if (step.unicode) {
      // KEYEVENTF_UNICODE packet (AHK SendUnicodeChar): the code unit rides
      // in wScan, no VK, no scan mapping, no extended-key bit. The hook then
      // reports vk 231 (0xE7, Windows' unicode-injection placeholder) with
      // scan = the code unit - probed on this build for ASCII, Latin-1, CJK
      // and surrogate units - which no chord can match (chords resolve real
      // virtual keys only).
      input.ki.wVk = 0;
      input.ki.wScan = static_cast<WORD>(step.vk);
      input.ki.dwFlags = static_cast<DWORD>(KEYEVENTF_UNICODE) |
                         (step.down ? 0u : static_cast<DWORD>(KEYEVENTF_KEYUP));
    } else {
      input.ki.wVk = static_cast<WORD>(step.vk);
      input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(step.vk, MAPVK_VK_TO_VSC));
      input.ki.dwFlags = (step.down ? 0u : static_cast<DWORD>(KEYEVENTF_KEYUP)) |
                         (is_extended_vk(step.vk) ? static_cast<DWORD>(KEYEVENTF_EXTENDEDKEY) : 0u);
    }
    input.ki.dwExtraInfo = static_cast<ULONG_PTR>(k_self_injected_marker);
    inputs.push_back(input);
  }
  const UINT sent =
      SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
  if (sent != inputs.size()) {
    return {rime::core::Error::Code::ExecutionFailed,
            "SendInput injected " + std::to_string(sent) + " of " +
                std::to_string(inputs.size()) + " key steps"};
  }
  return rime::core::Error::none();
}

rime::core::Error InputService::send_mouse(const std::vector<SendMouseStep>& steps) {
  if (steps.empty()) {
    return {rime::core::Error::Code::InvalidContract, "send_mouse requires at least one step"};
  }
  for (const auto& step : steps) {
    if (step.action != SendMouseAction::Move && step.action != SendMouseAction::RelMove &&
        step.action != SendMouseAction::Down && step.action != SendMouseAction::Up) {
      return {rime::core::Error::Code::InvalidContract, "send_mouse step action is unknown"};
    }
    if ((step.action == SendMouseAction::Down || step.action == SendMouseAction::Up) &&
        (step.button < 1 || step.button > 3)) {
      return {rime::core::Error::Code::InvalidContract,
              "send_mouse buttons must be 1 (left), 2 (right) or 3 (middle)"};
    }
  }
  std::lock_guard send_lock(impl_->send_mutex);
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state != InputServiceState::Running || impl_->stopping) {
      return {rime::core::Error::Code::InvalidState, "input service is not running"};
    }
  }
  const int width = GetSystemMetrics(SM_CXSCREEN);
  const int height = GetSystemMetrics(SM_CYSCREEN);
  // AHK v2 translates logical L/R into physical through SM_SWAPBUTTON
  // (keyboard_mouse.cpp:2190) before choosing the event flags; middle is
  // never swapped. We always inject in SendInput mode, so the swap applies
  // to every button batch (SendPlay would be exempt).
  const bool swap_buttons = GetSystemMetrics(SM_SWAPBUTTON) != 0;
  std::vector<INPUT> inputs;
  inputs.reserve(steps.size());
  for (const auto& step : steps) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    switch (step.action) {
      case SendMouseAction::Move:
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        input.mi.dx = mouse_coord_to_abs(step.x, width);
        input.mi.dy = mouse_coord_to_abs(step.y, height);
        break;
      case SendMouseAction::RelMove:
        // Raw deltas: SendInput applies them in batch order, so mixed
        // absolute/relative sequences need no cursor prediction here.
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        input.mi.dx = step.x;
        input.mi.dy = step.y;
        break;
      case SendMouseAction::Down:
      case SendMouseAction::Up: {
        std::uint32_t button = step.button;
        if (swap_buttons && (button == 1 || button == 2)) button = 3 - button;
        const bool up = step.action == SendMouseAction::Up;
        if (button == 1) {
          input.mi.dwFlags = up ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN;
        } else if (button == 2) {
          input.mi.dwFlags = up ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN;
        } else {
          input.mi.dwFlags = up ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_MIDDLEDOWN;
        }
        break;
      }
      default:
        return {rime::core::Error::Code::InvalidContract, "send_mouse step action is unknown"};
    }
    input.mi.dwExtraInfo = static_cast<ULONG_PTR>(k_self_injected_marker);
    inputs.push_back(input);
  }
  const UINT sent =
      SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
  if (sent != inputs.size()) {
    return {rime::core::Error::Code::ExecutionFailed,
            "SendInput injected " + std::to_string(sent) + " of " +
                std::to_string(inputs.size()) + " mouse steps"};
  }
  return rime::core::Error::none();
}

ModifierState read_modifier_state() {
  ModifierState state;
  state.lcontrol = async_key_down(VK_LCONTROL);
  state.rcontrol = async_key_down(VK_RCONTROL);
  state.lshift = async_key_down(VK_LSHIFT);
  state.rshift = async_key_down(VK_RSHIFT);
  state.lalt = async_key_down(VK_LMENU);
  state.ralt = async_key_down(VK_RMENU);
  state.lwin = async_key_down(VK_LWIN);
  state.rwin = async_key_down(VK_RWIN);
  // Toggle bit of GetKeyState: the CapsLock LED state (0/1), not the
  // thread-queued down bit.
  state.caps_lock = (GetKeyState(VK_CAPITAL) & 1) != 0;
  return state;
}

}  // namespace rime::win32
