#pragma once

#include "rime/core/cancellation.hpp"
#include "rime/core/types.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace rime::win32 {

class UiThread;

// Win32 GUI surface of M6 batch 1: AHK MsgBox / InputBox (modal dialogs),
// ToolTip, TraySetIcon and TrayTip (script-owned tray state). Every UI-thread
// object - dialog, tooltip window, NOTIFYICONDATA - is owned and touched on
// set_ui_thread()'s pump only (AGENTS: HWND work never leaves the UI Thread);
// public methods marshal through UiThread::call, so they are callable from a
// worker body. The dialogs run as a Win32 modal dialog on that pump - the
// same bounded shape StorageService::select_file uses: the queued phase is
// bounded by the deadline, and once the pump claims the task the modal runs
// to completion (an already-open dialog cannot be interrupted; the caller's
// own timeout closes it, see msg_box()). Nothing here builds an Action.
//
// Semantics mirror AutoHotkey v2 (source citations inline): button names,
// the "Timeout" word, the X/ESC rules and the tray rules are AHK's, not
// inventions; deviations are called out where they exist.
class GuiService {
 public:
  GuiService();
  ~GuiService();
  GuiService(const GuiService&) = delete;
  GuiService& operator=(const GuiService&) = delete;

  // ---- MsgBox (script2.cpp:1022-1058, window.cpp:932-1091, docs MsgBox) ----

  struct MsgBoxSpec {
    std::string text;
    std::string title;
    // AHK Group #1 button sets: 0 OK, 1 OKCancel, 2 AbortRetryIgnore,
    // 3 YesNoCancel, 4 YesNo, 5 RetryCancel, 6 CancelTryAgainContinue.
    int buttons{0};
    // MB_ICON* value or 0 (no icon): 0x10 error, 0x20 question, 0x30 warning,
    // 0x40 information (script2.cpp:968-978 Iconx/Icon?/Icon!/Iconi).
    unsigned int icon{0};
    // 1-based default button (script2.cpp:979-986 Default1..Default4).
    int default_index{1};
    // AHK T option in seconds; 0 = no timeout. Clamped the way window.cpp
    // clamps it (>2147483s saturates; a negative value becomes 0.1s).
    double timeout_seconds{0};
  };

  // Runs the dialog on the UI pump and returns the pressed button's AHK name
  // ("OK", "Yes", "No", "Cancel", "Abort", "Retry", "Ignore", "TryAgain",
  // "Continue") or "Timeout". The X/ESC rules are the OS's own (docs
  // MsgBox): OK-only -> X is OK; a Cancel button present -> X/ESC is Cancel;
  // otherwise X is disabled and ESC does nothing. InvalidContract when the
  // spec's button set is out of 0..6. InvalidState when no pump is attached.
  rime::core::Error msg_box(const MsgBoxSpec& spec, std::string& result_out,
                            std::int64_t deadline_unix_ms, rime::core::CancellationToken cancel);

  // ---- InputBox (lib/InputBox.cpp:26-208, docs InputBox) ----

  struct InputBoxSpec {
    std::string prompt;
    std::string title;
    std::string default_value;
    // Password masking (InputBox.cpp:37-48): '*' when set.
    bool password{false};
    // 0 = default dialog size; x/y unset = centered on the cursor's monitor.
    int width{0};
    int height{0};
    std::optional<int> x;
    std::optional<int> y;
    // AHK T option; 0 = none (same clamp as msg_box).
    double timeout_seconds{0};
  };

  struct InputBoxResult {
    std::string value;  // the edit box contents at close, even on Cancel/Timeout
    std::string result; // "OK", "Cancel" or "Timeout" (InputBox.cpp:166-173)
  };

  rime::core::Error input_box(const InputBoxSpec& spec, InputBoxResult& out,
                              std::int64_t deadline_unix_ms, rime::core::CancellationToken cancel);

  // ---- ToolTip (script2.cpp:1084-1260, docs ToolTip) ----

  // Shows/updates tooltip `which` (1..20; InvalidContract outside), or
  // destroys it when `text` is blank (script2.cpp:1096-1105). Omitted x/y
  // (nullopt) default to cursor + 16,16 (script2.cpp:1115-1121); given
  // coordinates are screen pixels, clamped inside the monitor work area the
  // way AHK clamps them. Returns no value: AHK's HWND return is a raw handle
  // and this runtime never exposes one (gui-menu.md §0.4).
  rime::core::Error tool_tip(const std::string& text, std::optional<int> x, std::optional<int> y,
                             int which, std::int64_t deadline_unix_ms,
                             rime::core::CancellationToken cancel);

  // ---- TraySetIcon (script.cpp:872-960, docs TraySetIcon) ----

  // Sets the process tray icon, adding the icon first if none exists. An
  // empty file or "*" restores the standard icon (script.cpp:890-903);
  // icon_number 0 means 1 (script.cpp:956-957). freeze records AHK's frozen
  // flag; batch 1 has no automatic icon updates, so it changes nothing else.
  rime::core::Error tray_set_icon(const std::string& file, int icon_number, bool freeze,
                                  bool freeze_given, std::int64_t deadline_unix_ms,
                                  rime::core::CancellationToken cancel);

  // ---- TrayTip (script2.cpp:85-135, docs TrayTip) ----

  // Shows a balloon on the tray icon (NIF_INFO). info_flags is AHK's
  // dwInfoFlags: 0x10 error, 0x20 warning, 0x40 info; mute adds
  // NIIF_NOSOUND. Empty text with a title shows a title-only balloon
  // (script2.cpp:124-126); both empty removes the notification. Like AHK
  // this never fails once the tray icon exists - only a pump/cancel problem
  // can reject.
  rime::core::Error tray_tip(const std::string& text, const std::string& title,
                             unsigned int info_flags, bool mute, std::int64_t deadline_unix_ms,
                             rime::core::CancellationToken cancel);

  // Borrows the runtime's single UI pump (never a second one). Attach after
  // the pump is running; detach on shutdown after stop().
  void set_ui_thread(UiThread* ui);

  // Removes the tray icon and destroys the tooltip windows on the pump.
  // Repeatable; a detached or dead pump makes it a no-op (the pump destroys
  // its windows when the UI thread exits).
  rime::core::Error stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rime::win32
