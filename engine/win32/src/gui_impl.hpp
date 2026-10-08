#pragma once

// Shared private implementation of GuiService across the batch-1 TU
// (gui.cpp: dialogs, tooltip, tray) and the batch-2 TU (gui_window.cpp: the
// Gui/GuiControl object family). The nested GuiService::Impl definition lives
// here because both TUs mutate one object; everything else is small helpers
// they must agree on. Not installed - engine/win32/src is not on any public
// include path.

#include "rime/win32/gui.hpp"
#include "rime/win32/ui_thread.hpp"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rime::win32 {

// Stable English failure texts (tests and docs match on these).
inline constexpr const char* kNoUiThread = "gui service has no UI thread";
inline constexpr const char* kCancelledBeforeStart = "gui call was cancelled before it started";
inline constexpr const char* kDeadlinePassed = "gui call deadline passed before it could start";

// The queued-phase cap when the caller passes no deadline (0): the same
// 30-second queue bound StorageService::select_file uses.
inline constexpr std::chrono::seconds kQueueDefault{30};

inline std::int64_t system_unix_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Budget for UiThread::call's queued phase: remaining time until the
// caller's absolute deadline, kQueueDefault when there is none, and a
// sentinel "already expired" the caller turns into a Timeout before even
// queueing. Once the pump claims the task it runs unbounded (documented
// split in UiThread::call).
inline std::chrono::milliseconds queue_budget(std::int64_t deadline_unix_ms, bool& expired) {
  expired = false;
  if (deadline_unix_ms <= 0) return kQueueDefault;
  const std::int64_t remaining = deadline_unix_ms - system_unix_ms();
  if (remaining <= 0) {
    expired = true;
    return std::chrono::milliseconds(0);
  }
  return std::chrono::milliseconds(remaining);
}

inline rime::core::Error no_ui_error() {
  return {rime::core::Error::Code::InvalidState, kNoUiThread};
}
inline rime::core::Error expired_error() {
  return {rime::core::Error::Code::Timeout, kDeadlinePassed};
}

// One child control as the pump knows it. `hmenu_id` is the 1-based id we
// pass as the child's HMENU (WM_COMMAND LOWORD); MAX_CONTROLS_PER_GUI in AHK
// is 11000, so 16 bits never wrap inside one window.
struct GuiChildRecord {
  HWND hwnd{nullptr};
  GuiService::GuiControlKind kind{GuiService::GuiControlKind::Text};
  std::string vname;
  std::uint16_t hmenu_id{0};
  bool hidden{false};
  bool disabled{false};
  // Per-control font (ctrl.SetFont); starts as a copy of the GUI font.
  HFONT font{nullptr};
  bool owns_font{false};
  LOGFONTW font_log{};
  // Picture content ownership (freed at destroy/sweep; never exposed).
  HBITMAP picture_bitmap{nullptr};
  HICON picture_icon{nullptr};
};

// One script-created Gui window. Lives in GuiService::Impl::guis, addressed
// by stable id; the window procedure reaches it through GWLP_USERDATA. Only
// ever touched on the pump.
struct GuiRecord {
  HWND hwnd{nullptr};
  // Stable id (mirrors the map key; the payload builder and diagnostics
  // read it without going through the map).
  std::uint64_t id_hint{0};
  // Set when the window is gone (Destroy, default-close, WM_DESTROY). Keeps
  // the id valid so later calls fail with InvalidState instead of recreating.
  bool dead{false};
  std::uint64_t channel{0};
  GuiService::GuiEventInterest interest;

  std::map<std::uint64_t, GuiChildRecord> children;  // ctrl id -> child
  std::map<std::uintptr_t, std::uint64_t> by_hwnd;   // child hwnd -> ctrl id
  std::map<std::uint16_t, std::uint64_t> by_hmenu;   // hmenu id -> ctrl id
  std::uint16_t next_hmenu{1};

  // Auto-positioning state (script_gui.cpp:3230-3231, 4921-4924): the next
  // control with no explicit x/y lands below the previous one.
  bool have_prev{false};
  int prev_x{0};
  int prev_y{0};
  int prev_w{0};
  int prev_h{0};
  // True when the previous control was Text (AHK adds a deadspace after a
  // text run so later input controls line up, script_gui.cpp:3238-3243).
  bool prev_was_text{false};
  // True while consecutive radios form one AHK radio group: the first gets
  // WS_GROUP, and the next non-radio closes the group with WS_GROUP
  // (script_gui.cpp:2911-2919).
  bool in_radio_group{false};
  // -1 = not yet computed from the current font (script_gui.cpp:11171-11182).
  int margin_x{-1};
  int margin_y{-1};
  // First Show has not happened yet (default-center + default-size rules,
  // script_gui.cpp:7470-7477).
  bool shown_once{false};

  // Current GUI font; drives control sizing defaults and WM_SETFONT. The
  // stock DEFAULT_GUI_FONT is not owned (never DeleteObject'd).
  HFONT font{nullptr};
  bool owns_font{false};
  LOGFONTW font_log{};

  // Optional background (Gui.BackColor): brush owned by the record, freed on
  // destroy/sweep; WndProc serves it from WM_CTLCOLOR* / WM_ERASEBKGND.
  bool has_back_color{false};
  std::uint32_t back_color{0};
  HBRUSH back_brush{nullptr};

  // Event sink copy taken at record creation (see GuiService::set_event_sink;
  // empty = no host queue attached yet, pushes are dropped until then).
  std::function<void(std::uint64_t, std::string)> event_sink;
};

// The pump-owned id -> record table. Free helpers in gui_window.cpp take
// this (never GuiService::Impl, whose type is private to GuiService members).
using GuiMap = std::map<std::uint64_t, std::unique_ptr<GuiRecord>>;

struct GuiService::Impl {
  UiThread* ui{nullptr};
  bool tray_added{false};
  bool tray_frozen{false};
  bool comctl_ready{false};
  HICON custom_icon{nullptr};
  HWND tooltips[20]{};

  // ---- batch 2 ----
  // Current event sink (JS-side queue push). Assignments are idempotent and
  // happen on the JS thread before any Gui exists.
  std::function<void(std::uint64_t, std::string)> event_sink;
  // All script-created Gui records; destroyed only on the pump.
  GuiMap guis;
};

}  // namespace rime::win32
