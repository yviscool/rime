// Realism: L5 - the real GuiService on a real UiThread pump: modal dialogs
// created by DialogBoxIndirectParamW with a real WM_TIMER timeout, real
// TOOLTIPS_CLASS windows and the real tray icon (Shell_NotifyIconW). Every
// dialog assertion is driven through the operating system, never through the
// service: a watcher thread finds the dialog by class + title (FindWindowW),
// posts the click the way the dialog manager would (WM_COMMAND / WM_CLOSE)
// and the service's return value is compared against the AHK words the docs
// promise ("Yes", "OK", "Timeout", ...). The tray icon's existence is
// observed by an independent NIM_MODIFY probe on this process's own message
// window, so a service that returned ok without talking to the shell cannot
// pass. Interactive dialogs all carry a safety T timeout, so a lost watcher
// fails the assertion with a wrong word instead of hanging the suite; the
// timeout tests additionally prove the dialog really appeared (a watcher
// flags the window's existence before the timer fires - a facade that
// returns "Timeout" without ever showing a dialog cannot pass). The
// headless, invalid-spec and pre-cancelled paths run without ever showing a
// window, and stop() is proven repeatable.

#include "rime/core/cancellation.hpp"
#include "rime/core/types.hpp"
#include "rime/win32/gui.hpp"
#include "rime/win32/ui_thread.hpp"

#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

using rime::core::Error;
using ErrorCode = rime::core::Error::Code;
using GuiService = rime::win32::GuiService;
using UiThread = rime::win32::UiThread;

void require(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "require failed: %s\n", what);
    std::fflush(stderr);
    std::abort();
  }
}

void require_error(const Error& error, ErrorCode code, const char* what) {
  if (error.code != code) {
    std::fprintf(stderr, "require failed: %s (got code %s: %s)\n", what,
                 rime::core::error_code_name(error.code), error.message.c_str());
    std::fflush(stderr);
    std::abort();
  }
}

void require_word(const std::string& actual, const char* expected, const char* what) {
  if (actual != expected) {
    std::fprintf(stderr, "require failed: %s (got \"%s\", want \"%s\")\n", what, actual.c_str(),
                 expected);
    std::fflush(stderr);
    std::abort();
  }
}

std::int64_t deadline_ms(int extra_ms = 20000) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
             .count() +
         extra_ms;
}

// Condition polling with a timeout: the dialog's appearance is the condition,
// never a bare sleep. 10 ms is the poll pace; the deadline is the failure.
HWND wait_for_dialog(const wchar_t* title, std::chrono::milliseconds timeout) {
  const auto until = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    HWND window = FindWindowW(L"#32770", title);
    if (window) return window;
    if (std::chrono::steady_clock::now() >= until) return nullptr;
    Sleep(10);
  }
}

// Cross-thread click: PostMessage lands in the dialog's own modal pump, the
// same dispatch path a real button press takes (BN_CLICKED in WM_COMMAND).
void post_click(HWND dialog, int id) {
  PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(id, 1), 0);
}

int count_own_visible_tooltips() {
  int count = 0;
  EnumWindows(
      [](HWND window, LPARAM parameter) -> BOOL {
        auto* out = reinterpret_cast<int*>(parameter);
        wchar_t class_name[64]{};
        if (GetClassNameW(window, class_name, 64) == 0) return TRUE;
        if (wcscmp(class_name, L"tooltips_class32") != 0) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid != GetCurrentProcessId()) return TRUE;
        if (!IsWindowVisible(window)) return TRUE;
        ++*out;
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&count));
  return count;
}

// Independent shell observation: NIM_MODIFY with NIF_TIP answers whether the
// icon with this uID currently exists on the window (it rewrites the tip
// with the same text, so the probe changes nothing observable).
bool tray_icon_present(HWND owner) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = owner;
  nid.uID = 1;
  nid.uFlags = NIF_TIP;
  lstrcpyW(nid.szTip, L"rime");
  return Shell_NotifyIconW(NIM_MODIFY, &nid) == TRUE;
}

}  // namespace

int main() {
  UiThread ui;
  require(ui.start().ok(), "UiThread starts");
  GuiService service;
  service.set_ui_thread(&ui);
  const HWND owner = reinterpret_cast<HWND>(ui.message_window());
  require(owner != nullptr, "pump exposes its message window");

  // ---- headless guard: no pump attached, nothing may be shown -------------
  {
    GuiService headless;
    GuiService::MsgBoxSpec spec;
    spec.text = "no pump";
    std::string result;
    require_error(headless.msg_box(spec, result, deadline_ms(), {}), ErrorCode::InvalidState,
                  "msg_box without a pump reports InvalidState");
    require(headless.stop().ok(), "headless stop is a no-op");
  }

  // ---- spec validation (no window involved) -------------------------------
  {
    GuiService::MsgBoxSpec spec;
    std::string result;
    spec.buttons = 7;
    require_error(service.msg_box(spec, result, deadline_ms(), {}), ErrorCode::InvalidContract,
                  "button set 7 is rejected");
    spec.buttons = 4;
    spec.default_index = 9;
    require_error(service.msg_box(spec, result, deadline_ms(), {}), ErrorCode::InvalidContract,
                  "default index past the set is rejected");
    spec.default_index = 1;
    spec.icon = 0x50;
    require_error(service.msg_box(spec, result, deadline_ms(), {}), ErrorCode::InvalidContract,
                  "unknown icon value is rejected");
    spec.icon = 0;
    require_error(service.msg_box(spec, result, deadline_ms() - 30000, {}),
                  ErrorCode::Timeout, "an already-passed deadline never queues");
    rime::core::CancellationSource cancelled;
    cancelled.cancel();
    require_error(service.msg_box(spec, result, deadline_ms(), cancelled.token()),
                  ErrorCode::Cancelled, "a pre-cancelled call never shows a dialog");
    require_error(service.tool_tip("x", std::nullopt, std::nullopt, 0, deadline_ms(), {}),
                  ErrorCode::InvalidContract, "tooltip index 0 is rejected");
    require_error(service.tool_tip("x", std::nullopt, std::nullopt, 21, deadline_ms(), {}),
                  ErrorCode::InvalidContract, "tooltip index 21 is rejected");
  }

  // ---- MsgBox timeout: real dialog, real timer, real window sighting ------
  {
    const wchar_t* title = L"rime-gui-timeout";
    GuiService::MsgBoxSpec spec;
    spec.text = "this box closes itself";
    spec.title = "rime-gui-timeout";
    spec.buttons = 4;  // YesNo: no Cancel, X disabled
    spec.timeout_seconds = 0.5;
    std::string result;
    bool seen = false;
    std::thread watcher([&] {
      HWND dialog = wait_for_dialog(title, std::chrono::milliseconds(400));
      seen = dialog != nullptr;
      if (dialog) PostMessageW(dialog, WM_CLOSE, 0, 0);  // X on YesNo must NOT close
    });
    const Error error = service.msg_box(spec, result, deadline_ms(), {});
    watcher.join();
    require(error.ok(), "timed msg_box succeeds");
    require(seen, "the dialog window really existed before the timer fired");
    require_word(result, "Timeout", "timer closes a YesNo box as Timeout");
  }

  // ---- MsgBox Yes: watcher-driven click, docs return word -----------------
  {
    GuiService::MsgBoxSpec spec;
    spec.text = "press yes";
    spec.title = "rime-gui-yesno";
    spec.buttons = 4;
    spec.timeout_seconds = 4;  // safety net: a lost watcher fails, never hangs
    std::string result;
    std::thread watcher([&] {
      HWND dialog = wait_for_dialog(L"rime-gui-yesno", std::chrono::milliseconds(3000));
      if (dialog) post_click(dialog, IDYES);
    });
    const Error error = service.msg_box(spec, result, deadline_ms(), {});
    watcher.join();
    require(error.ok(), "yes/no msg_box succeeds");
    require_word(result, "Yes", "clicking Yes returns the Yes word");
  }

  // ---- MsgBox X on OK-only acts as OK (docs: The Close button) ------------
  {
    GuiService::MsgBoxSpec spec;
    spec.text = "ok only";
    spec.title = "rime-gui-x-ok";
    spec.buttons = 0;
    spec.timeout_seconds = 4;
    std::string result;
    std::thread watcher([&] {
      HWND dialog = wait_for_dialog(L"rime-gui-x-ok", std::chrono::milliseconds(3000));
      if (dialog) PostMessageW(dialog, WM_CLOSE, 0, 0);
    });
    const Error error = service.msg_box(spec, result, deadline_ms(), {});
    watcher.join();
    require(error.ok(), "ok-only msg_box succeeds");
    require_word(result, "OK", "X on an OK-only box acts as OK");
  }

  // ---- ESC on YesNo is swallowed; the box still answers No ----------------
  {
    GuiService::MsgBoxSpec spec;
    spec.text = "esc must not close me";
    spec.title = "rime-gui-esc-yn";
    spec.buttons = 4;
    spec.timeout_seconds = 4;
    std::string result;
    std::thread watcher([&] {
      HWND dialog = wait_for_dialog(L"rime-gui-esc-yn", std::chrono::milliseconds(3000));
      if (!dialog) return;
      post_click(dialog, IDCANCEL);   // no Cancel in YesNo: must be ignored
      PostMessageW(dialog, WM_CLOSE, 0, 0);  // X likewise disabled
      Sleep(50);  // only to order the two dismissals; the result word is the assertion
      post_click(dialog, IDNO);
    });
    const Error error = service.msg_box(spec, result, deadline_ms(), {});
    watcher.join();
    require(error.ok(), "yes/no box survives ESC and X");
    require_word(result, "No", "cancel-like dismissal is refused, No still works");
  }

  // ---- InputBox: real edit control round-trip -----------------------------
  {
    GuiService::InputBoxSpec spec;
    spec.prompt = "type something";
    spec.title = "rime-gui-input";
    spec.default_value = "seed";
    spec.timeout_seconds = 5;
    GuiService::InputBoxResult out;
    std::thread watcher([&] {
      HWND dialog = wait_for_dialog(L"rime-gui-input", std::chrono::milliseconds(3000));
      if (!dialog) return;
      // The children are created inside WM_INITDIALOG; the window itself can
      // be found a beat earlier, so poll for the edit before typing.
      const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
      HWND edit = nullptr;
      for (;;) {
        edit = FindWindowExW(dialog, nullptr, L"EDIT", nullptr);
        if (edit || std::chrono::steady_clock::now() >= until) break;
        Sleep(5);
      }
      if (!edit) return;
      SetWindowTextW(edit, L"hello-rime");
      post_click(dialog, IDOK);
    });
    const Error error = service.input_box(spec, out, deadline_ms(), {});
    watcher.join();
    require(error.ok(), "input_box succeeds");
    require_word(out.result, "OK", "Enter/OK closes with OK");
    require_word(out.value, "hello-rime", "the edit contents come back");
  }

  // ---- InputBox timeout keeps the typed/default value ---------------------
  {
    GuiService::InputBoxSpec spec;
    spec.prompt = "will time out";
    spec.title = "rime-gui-input-timeout";
    spec.default_value = "seed";
    spec.timeout_seconds = 0.15;
    GuiService::InputBoxResult out;
    const Error error = service.input_box(spec, out, deadline_ms(), {});
    require(error.ok(), "timed input_box succeeds");
    require_word(out.result, "Timeout", "timer closes the input box as Timeout");
    require_word(out.value, "seed", "timeout returns the untouched default value");
  }

  // ---- ToolTip: real tooltip windows, show/hide observed by enumeration ---
  {
    require(service.tool_tip("", std::nullopt, std::nullopt, 1, deadline_ms(), {}).ok(),
            "clearing tooltip 1 starts clean");
    require(count_own_visible_tooltips() == 0, "no tooltip window is visible at the start");
    require(
        service.tool_tip("rime tooltip marker", std::nullopt, std::nullopt, 1, deadline_ms(), {})
            .ok(),
        "showing a tooltip succeeds");
    require(count_own_visible_tooltips() == 1, "the tooltip window exists and is visible");
    require(service.tool_tip("", std::nullopt, std::nullopt, 1, deadline_ms(), {}).ok(),
            "hiding the tooltip succeeds");
    require(count_own_visible_tooltips() == 0, "the tooltip window is gone after hide");
    // A second index is independent (docs ToolTip's Which).
    require(
        service.tool_tip("second tip", std::optional<int>(10), std::optional<int>(10), 2,
                         deadline_ms(), {})
            .ok(),
        "showing tooltip 2 succeeds");
    require(count_own_visible_tooltips() == 1, "tooltip 2 shows");
    require(service.tool_tip("", std::nullopt, std::nullopt, 2, deadline_ms(), {}).ok(),
            "hiding tooltip 2 succeeds");
    require(count_own_visible_tooltips() == 0, "no tooltip lingers");
  }

  // ---- Tray: independent shell probes before / after ----------------------
  {
    require(!tray_icon_present(owner), "a fresh process has no tray icon yet");
    require(service.tray_set_icon("", 1, false, false, deadline_ms(), {}).ok(),
            "restoring the standard icon creates the tray icon");
    require(tray_icon_present(owner), "the shell now reports the tray icon");

    const Error bad = service.tray_set_icon("C:\\Windows\\__rime_no_such_icon__.ico", 1, false,
                                            false, deadline_ms(), {});
    require_error(bad, ErrorCode::ExecutionFailed, "a missing icon file fails");
    require(bad.message.find("load icon failed") != std::string::npos,
            "the failure names the load");
    require(tray_icon_present(owner), "a failed load leaves the tray icon intact");

    require(service.tray_tip("body", "title", 0x40, false, deadline_ms(), {}).ok(),
            "tray_tip succeeds while the icon exists");
    require(service.tray_set_icon("", 1, true, true, deadline_ms(), {}).ok(),
            "freeze flag records without error");
  }

  // ---- stop() refuses while a modal dialog is open -------------------------
  // A dialog the user never closes must not turn shutdown into an opaque
  // queue Timeout: stop names the blocking dialog and leaves no state.
  {
    std::string outcome;
    std::thread dialog([&] {
      GuiService::MsgBoxSpec spec;
      spec.text = "close me";
      spec.title = "rime-gui-stop-refusal";
      spec.buttons = 0;  // OK-only: X acts as OK, the test can dismiss it
      spec.timeout_seconds = 10;  // safety net only, never the assertion path
      const Error error = service.msg_box(spec, outcome, deadline_ms(), {});
      require(error.ok(), "the dialog settles once dismissed");
    });
    HWND seen = wait_for_dialog(L"rime-gui-stop-refusal", std::chrono::seconds(5));
    require(seen != nullptr, "the refusal dialog really appeared");
    const Error refused = service.stop();
    require(!refused.ok(), "stop refuses with an open modal");
    require(refused.code == ErrorCode::ExecutionFailed, "refusal is ExecutionFailed");
    require(refused.message.find("rime-gui-stop-refusal") != std::string::npos,
            "the refusal names the blocking dialog");
    PostMessageW(seen, WM_CLOSE, 0, 0);
    dialog.join();
    require(service.stop().ok(), "stop succeeds once the modal is gone");
  }

  // ---- stop(): removes the tray icon, repeatable --------------------------
  {
    require(service.stop().ok(), "stop succeeds");
    require(!tray_icon_present(owner), "stop removed the tray icon from the shell");
    require(service.stop().ok(), "stop is repeatable");
    require(service.tool_tip("", std::nullopt, std::nullopt, 1, deadline_ms(), {}).ok(),
            "clearing a tooltip after stop reports cleanly");
  }

  require(ui.stop().ok(), "UiThread stops");
  std::printf("gui service tests passed\n");
  return 0;
}
