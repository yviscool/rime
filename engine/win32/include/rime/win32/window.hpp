#pragma once

#include "rime/core/json.hpp"
#include "rime/core/types.hpp"
#include "rime/win32/ui_thread.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rime::win32 {

struct Rect {
  long left{0};
  long top{0};
  long right{0};
  long bottom{0};

  [[nodiscard]] long width() const { return right - left; }
  [[nodiscard]] long height() const { return bottom - top; }
  friend bool operator==(const Rect&, const Rect&) = default;
};

struct WindowInfo {
  std::uint64_t id{0};
  std::string title;         // UTF-8
  std::string class_name;    // UTF-8 Win32 class
  std::string process_name;  // UTF-8 image basename, e.g. "notepad.exe"
  std::string process_path;  // UTF-8 full image path (WinGetProcessPath), "" when unreadable
  Rect rect{};
  bool visible{false};
  bool minimized{false};
  std::uint32_t process_id{0};
  // "normal" | "minimized" | "maximized" | "hidden"
  std::string state{"normal"};
  // Extended detail (WinGet* family): client area in screen coordinates,
  // GWL_STYLE/GWL_EXSTYLE bits, IsWindowEnabled, WS_EX_TOPMOST, and the
  // WinGetMinMax triple (-1 minimized | 0 normal | 1 maximized).
  Rect client_rect{};
  std::int64_t style{0};
  std::int64_t ex_style{0};
  bool enabled{true};
  bool always_on_top{false};
  int min_max{0};
  // Layered window alpha (WinGetTransparent): 0..255, or -1 when the window
  // is not layered or has no LWA_ALPHA. Color key (WinGetTransColor):
  // "0xRRGGBB" (RGB order, like AHK) or "" when absent.
  int transparent{-1};
  std::string trans_color;
};

// Deterministic JSON shape shared by the executor, the JS module and tests:
// {id,title,className,processName,processPath,rect,clientRect,visible,
//  minimized,state,processId,style,exStyle,enabled,alwaysOnTop,minMax,
//  transparent,transColor}
rime::core::json::Value window_info_json(const WindowInfo& info);

// AHK TitleMatchMode (SetTitleMatchMode / A_TitleMatchMode): 1 = leading
// part, 2 = anywhere, 3 = exact, RegEx = pattern search. The wire `matchMode`
// field uses the Rime spellings ("startswith"/"contains"/"exact"/"regex");
// the settings surface mirrors AHK ("1"/"2"/"3"/"RegEx").
enum class TitleMatchMode { StartsWith = 1, Contains = 2, Exact = 3, Regex = 4 };

// AHK vocabulary helpers: "1"/"2"/"3" map by value, "RegEx" matches
// case-insensitively like AHK's ConvertTitleMatchMode; anything else is
// nullopt. The inverse always emits the canonical AHK spelling.
[[nodiscard]] std::optional<TitleMatchMode> parse_title_match_mode(std::string_view text);
[[nodiscard]] std::string title_match_mode_text(TitleMatchMode mode);

// Global window options (SetTitleMatchMode/DetectHiddenWindows/
// DetectHiddenText). Stored as atomics on the service so JS reads/writes stay
// synchronous: a setter on the JS thread is ordered before any later query
// posted through the UI-lane queue (the queue hop provides the edge).
struct WindowSettings {
  TitleMatchMode title_match_mode{TitleMatchMode::Contains};
  // SetTitleMatchMode Fast/Slow; only WinText matching will observe it.
  bool title_match_mode_slow{false};
  bool detect_hidden_windows{false};
  bool detect_hidden_text{false};
  friend bool operator==(const WindowSettings&, const WindowSettings&) = default;
};

// nullopt fields are left unchanged by set_settings.
struct WindowSettingsPatch {
  std::optional<TitleMatchMode> title_match_mode;
  std::optional<bool> title_match_mode_slow;
  std::optional<bool> detect_hidden_windows;
  std::optional<bool> detect_hidden_text;
};

// WinTitle-style selector resolved entirely on the UI lane. JS never sees an
// HWND: `id` is the stable window id, `active` selects the foreground window,
// `title_match_mode` overrides the global SetTitleMatchMode for this query,
// `include_hidden` overrides DetectHiddenWindows; unset fields fall back to
// the service settings at resolve time.
struct WindowQuery {
  std::string title;   // UTF-8
  std::optional<TitleMatchMode> title_match_mode;
  std::string class_name;
  std::string process_name;         // ahk_exe: image basename match
  std::uint64_t id{0};              // ahk_id: stable id; 0 = unset
  std::optional<bool> include_hidden;
  bool active{false};
  // Exact equality powers window-group dedup (AHK GroupAdd skips a spec the
  // group already carries).
  friend bool operator==(const WindowQuery&, const WindowQuery&) = default;
};

// Validates one AHK-style RegEx pattern (optional i)/m)/s) option prefix) for
// the RegEx match mode: unsupported option letters and uncompilable patterns
// fail with InvalidContract/Unsupported. The JS module calls this during
// parse so bad patterns surface as a synchronous TypeError; the service
// compiles the same patterns again when it resolves a query.
rime::core::Error validate_window_regex(const std::string& utf8_pattern);

// A child control surfaced by WinGetControls / WinGetControlsHwnd. `id` is a
// stable id in the same space as WindowInfo::id (raw HWNDs never leave the
// UI lane); `class_nn` is AHK's "Class" + per-class instance number.
struct ControlInfo {
  std::uint64_t id{0};
  std::string class_name;  // UTF-8 Win32 class, e.g. "Edit"
  std::string class_nn;    // UTF-8, e.g. "Edit1"
};

// WinWait family conditions (AHK WinWait/WinWaitActive/WinWaitClose/
// WinWaitNotActive): wait until a matching window exists / becomes the
// foreground / no matching window remains / the match stops being foreground.
// notActive is the logical negation of Active, so a query that matches
// nothing is "not active" immediately (no window to be active).
enum class WaitCondition { Exists, Active, Closed, NotActive };

// Result of one wait evaluation: `met` reports whether `until` is satisfied;
// `target` carries the snapshot the wait resolves with for Exists/Active
// (nullopt for Closed/NotActive, and for Active while no snapshot was built).
struct WaitEvaluation {
  bool met{false};
  std::optional<WindowInfo> target;
};

// Top-level window operations. Every call is routed to the UI thread; raw
// HWND values never leave the UI lane. Window ids are stable per service
// and fail with InvalidState once the underlying window is gone. `timeout`
// bounds the queued UI phase; an expired deadline fails with Timeout.
class WindowService final {
 public:
  WindowService();
  ~WindowService();
  WindowService(const WindowService&) = delete;
  WindowService& operator=(const WindowService&) = delete;

  rime::core::Error start();
  rime::core::Error stop();

  rime::core::Error list(std::vector<WindowInfo>& out,
                         std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // Resolves a WindowQuery on the UI lane; an empty query behaves like list().
  rime::core::Error query(const WindowQuery& query, std::vector<WindowInfo>& out,
                          std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // nullopt (with a successful result) when there is no foreground window.
  rime::core::Error active(std::optional<WindowInfo>& out,
                           std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // WinExist: true when at least one window matches the query. Stops at the
  // first match, so it never builds full snapshots it does not need.
  rime::core::Error exists(const WindowQuery& query, bool& out,
                           std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // WinActive: true when the foreground window matches the query. Evaluated
  // entirely on the UI lane; `query.active` is ignored (the target is always
  // the foreground window).
  rime::core::Error matches_active(const WindowQuery& query, bool& out,
                                   std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // One WinWait-family evaluation (exists/active/closed/notActive) with the
  // snapshot the wait resolves with. The JS wait loop polls this from the
  // worker lane; `timeout` bounds each UI round-trip, not the whole wait.
  rime::core::Error evaluate_wait(const WindowQuery& query, WaitCondition until,
                                  WaitEvaluation& out,
                                  std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // Window groups (AHK GroupAdd/GroupActivate/GroupClose/GroupDeactivate):
  // named lists of query specs evaluated against live windows with the
  // current settings. The registry and the visited-window cycle state are
  // UI-thread only, so every method runs through the UI lane without locks.
  // group_add appends a spec unless the group already carries it and
  // resolves with the resulting spec count; a missing group is created.
  rime::core::Error group_add(const std::string& name, const WindowQuery& spec,
                              std::size_t& spec_count,
                              std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // Cycles focus through the group's members (AHK GroupActivate): resolves
  // with the activated snapshot, or nullopt when there is nothing to
  // activate. A missing group is created empty (AHK's create-if-missing).
  rime::core::Error group_activate(const std::string& name, bool reverse,
                                   std::optional<WindowInfo>& out,
                                   std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // Activates an eligible non-member (AHK GroupDeactivate); InvalidContract
  // when the group was never created (AHK's argument error).
  rime::core::Error group_deactivate(const std::string& name, bool reverse,
                                     std::optional<WindowInfo>& out,
                                     std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // mode: "" closes the foreground member (if any) then activates the next;
  // "reverse" does so starting from the most recent member; "all" closes
  // every member and activates nothing. InvalidContract for an unknown mode
  // or a group that was never created.
  rime::core::Error group_close(const std::string& name, std::string_view mode,
                                std::uint64_t& closed, std::optional<WindowInfo>& activated,
                                std::chrono::milliseconds timeout = std::chrono::seconds(5));
  rime::core::Error info(std::uint64_t id, WindowInfo& out,
                         std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // WinGetControls/WinGetControlsHwnd: child controls in EnumChildWindows
  // order (z-order, hidden controls included, matching AHK numbering).
  rime::core::Error controls(std::uint64_t id, std::vector<ControlInfo>& out,
                             std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // WinGetText: concatenated WM_GETTEXT of the child controls, "\r\n" after
  // each non-empty text (trailing separator included, like AHK); hidden
  // controls are skipped unless DetectHiddenText is on.
  rime::core::Error text(std::uint64_t id, std::string& out,
                         std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // Moves the window to a named placement: left, right, top, bottom, full.
  rime::core::Error move(std::uint64_t id, std::string_view placement,
                         std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // Internal/test-only geometry helpers (kept public for CLI/tests; not a
  // general placement API): move_rect positions by rect, placement_rect
  // resolves a named placement against the primary monitor work area.
  rime::core::Error move_rect(std::uint64_t id, const Rect& rect,
                              std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // Restores when minimized and requests foreground activation.
  rime::core::Error focus(std::uint64_t id,
                          std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // z-order changes (AHK WinMoveTop/WinMoveBottom): SetWindowPos with
  // SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE onto HWND_TOP/HWND_BOTTOM; no
  // move, no resize, no activation. `bottom` picks HWND_BOTTOM.
  rime::core::Error zorder(std::uint64_t id, bool bottom,
                           std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // Force close (AHK WinKill / Util_WinKill): WM_CLOSE via
  // SendMessageTimeout (capped at 500ms inside the action deadline); if the
  // target is hung or refused, fall back to TerminateProcess. Refuses to
  // terminate the runtime's own process.
  rime::core::Error kill(std::uint64_t id,
                         std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // Invalidate the window (AHK WinRedraw): InvalidateRect(NULL, TRUE) only -
  // WM_PAINT lands on the owner's pump, we never force UpdateWindow.
  rime::core::Error redraw(std::uint64_t id,
                           std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // Window state mutations (WinClose/WinHide/WinShow/WinMinimize/WinMaximize/
  // WinRestore equivalents). `close` delivers WM_CLOSE and waits for the
  // target thread to process it within the timeout.
  rime::core::Error close(std::uint64_t id,
                          std::chrono::milliseconds timeout = std::chrono::seconds(5));
  rime::core::Error hide(std::uint64_t id,
                         std::chrono::milliseconds timeout = std::chrono::seconds(5));
  rime::core::Error show(std::uint64_t id,
                         std::chrono::milliseconds timeout = std::chrono::seconds(5));
  rime::core::Error minimize(std::uint64_t id,
                             std::chrono::milliseconds timeout = std::chrono::seconds(5));
  rime::core::Error maximize(std::uint64_t id,
                             std::chrono::milliseconds timeout = std::chrono::seconds(5));
  rime::core::Error restore(std::uint64_t id,
                            std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // Resolves a placement against the primary monitor work area.
  rime::core::Error placement_rect(std::string_view placement, Rect& out,
                                   std::chrono::milliseconds timeout = std::chrono::seconds(5));

  // Global window settings (SetTitleMatchMode/DetectHiddenWindows/
  // DetectHiddenText). Both run on the caller thread over relaxed atomics;
  // a later query posted to the UI lane observes every prior write. The
  // setters' single-reader contract is the JS thread; `set_settings` returns
  // the settings in effect before the patch (AHK Set* return-previous).
  [[nodiscard]] WindowSettings settings() const;
  WindowSettings set_settings(const WindowSettingsPatch& patch);

  UiThread& ui();
  [[nodiscard]] UiThreadState state() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rime::win32
