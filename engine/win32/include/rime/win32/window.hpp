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
