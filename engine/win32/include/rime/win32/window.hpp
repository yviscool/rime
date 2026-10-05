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

// A partial move/resize (AHK WinMove X/Y/Width/Height): omitted fields keep
// the current value. x/y are screen coordinates of the top-left corner
// (negative is valid on a multi-monitor desktop); w/h are the outer frame
// size in pixels and must be >= 1.
struct RectMove {
  std::optional<std::int64_t> x;
  std::optional<std::int64_t> y;
  std::optional<std::int64_t> w;
  std::optional<std::int64_t> h;
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
  // Region bounding box (extension beyond AHK, which has no getter) as
  // "left,top,right,bottom" in window coordinates, or "" when the window
  // has no region. Non-rectangular regions report their bounding box.
  std::string region;
};

// Deterministic JSON shape shared by the executor, the JS module and tests:
// {id,title,className,processName,processPath,rect,clientRect,visible,
//  minimized,state,processId,style,exStyle,enabled,alwaysOnTop,minMax,
//  transparent,transColor,region}
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

// AHK WinSetStyle/WinSetExStyle change string: an optional '+', '-' or '^'
// prefix adds / removes / toggles the mask, a bare number replaces the
// whole style; the number is decimal or 0x-hex and fits 32 bits. An empty
// or malformed string fails (AHK treats an explicit blank as an argument
// error too). Shared by the module (sync TypeError), the executor and the
// service (InvalidContract) so every layer accepts exactly one grammar.
enum class StyleChangeOp { Add, Remove, Toggle, Replace };
struct StyleChange {
  StyleChangeOp op{StyleChangeOp::Replace};
  std::uint32_t mask{0};
};
[[nodiscard]] bool parse_style_change(std::string_view text, StyleChange& out);

// AHK WinSetTransColor value: '' or 'off' (case-insensitive) clears the
// color key, 'RRGGBB' / '0xRRGGBB' sets it in RGB order (hex only - HTML
// color names are a documented non-goal for now), and an optional
// ' <0-255>' tail adds LWA_ALPHA alongside the key, mirroring AHK's
// space-separated alpha suffix.
struct TransColorChange {
  bool off{false};       // clear WS_EX_LAYERED entirely
  bool color_key{false}; // LWA_COLORKEY requested
  std::uint32_t rgb{0};  // 0xRRGGBB when color_key
  bool with_alpha{false};
  int alpha{0};
};
[[nodiscard]] bool parse_trans_color_change(std::string_view text, TransColorChange& out);

// AHK WinSetRegion options string: coordinate pairs '<x>-<y>' (first pair
// anchors the shape; extra pairs are polygon vertices) plus the letter
// options 'E' (ellipse), 'R'/'R<rrw>-<rrh>' (rounded rectangle, default
// 30x30), 'W<width>'/'Wind' (width or winding fill) and 'H<height>'.
// Numbers are signed decimal like AHK's ATOI. A blank string restores the
// window region (SetWindowRgn NULL). Kind selection follows AHK: ellipse
// beats rounded beats rectangle (width and height both present) beats
// polygon; ellipse/rounded shapes without both dimensions and polygons
// with fewer than three points are refused as contract errors (AHK lets
// them fail at the Win32 layer).
enum class RegionKind { Restore, Ellipse, RoundRect, Rect, Polygon };
struct RegionSpec {
  RegionKind kind{RegionKind::Restore};
  std::vector<std::int32_t> coords;  // interleaved x0,y0,x1,y1,...
  std::int32_t width{0};
  std::int32_t height{0};
  std::int32_t round_width{30};
  std::int32_t round_height{30};
  bool winding{false};
};
[[nodiscard]] bool parse_region_options(std::string_view text, RegionSpec& out);

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

// Result of one MouseGetPos point query: the non-child parent window under
// the point (WindowFromPoint + the first WS_CHILD-free ancestor, AHK
// GetNonChildParent) plus the child control under it. Both are nullopt when
// nothing answers -- the desktop, or a window with no control per se.
struct WindowAtInfo {
  std::optional<WindowInfo> window;
  std::optional<ControlInfo> control;
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

namespace detail {
// Implementation types behind WindowService's PIMPL. They stay incomplete
// here (the complete definitions live in the src-internal window_match.hpp,
// which owns HWND) so this public header keeps no Win32 dependency.
class WindowRegistry;
struct WindowGroups;
}  // namespace detail

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
  // MouseGetPos point query: the window (and control) under the screen point
  // (x, y), resolved entirely on the UI lane. The control search replicates
  // AHK EnumChildFindPoint: visible children whose rect contains the point,
  // rect-enclosure beats center distance, ClassNN numbered like controls().
  // No window at the point (desktop) succeeds with nullopt window and
  // control -- it is a read of what exists, not an error.
  rime::core::Error window_at(std::int32_t x, std::int32_t y, WindowAtInfo& out,
                              std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // Moves the window to a named placement: left, right, top, bottom, full.
  rime::core::Error move(std::uint64_t id, std::string_view placement,
                         std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // Moves/resizes by rect (AHK WinMove X/Y/Width/Height): omitted RectMove
  // fields keep the current value; w/h must be >= 1; x/y are screen
  // coordinates. This is the coordinate path behind window.move's rect
  // payload; `placement_rect` (below) resolves a named placement against the
  // primary monitor work area.
  rime::core::Error move_rect(std::uint64_t id, const RectMove& move,
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
  // Minimize every window on the desktop / undo it (AHK WinMinimizeAll /
  // WinMinimizeAllUndo): posts WM_COMMAND 419 / 416 to the Shell_TrayWnd
  // taskbar window. Fire-and-forget - the shell applies the change
  // asynchronously and callers observe the effect through info(); blocking
  // on it would deadlock against messages it sends back to this process's
  // UI thread while windows change state.
  rime::core::Error minimize_all(bool undo,
                                 std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // WinSetTitle: SetWindowTextW; the empty string clears the title.
  rime::core::Error set_title(std::uint64_t id, const std::string& title,
                              std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // WinSetEnabled: 1 enables, 0 disables, -1 toggles; verifies the flag
  // actually changed (EnableWindow's return value is not reliable).
  rime::core::Error set_enabled(std::uint64_t id, int value,
                                std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // WinSetAlwaysOnTop: 1 topmost, 0 notopmost, -1 toggles the current
  // WS_EX_TOPMOST state; SetWindowPos with SWP_NOACTIVATE (SetWindowLong
  // does not take on some windows).
  rime::core::Error set_always_on_top(std::uint64_t id, int value,
                                      std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // WinSetStyle / WinSetExStyle: AHK's '+N'/'-N'/'^N'/bare-number change
  // string against GWL_STYLE or GWL_EXSTYLE, then the frame refresh AHK
  // pairs with it (SWP_FRAMECHANGED + InvalidateRect).
  rime::core::Error set_style(std::uint64_t id, std::string_view value,
                              std::chrono::milliseconds timeout = std::chrono::seconds(5));
  rime::core::Error set_ex_style(std::uint64_t id, std::string_view value,
                                 std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // WinSetTransparent: -1 drops WS_EX_LAYERED (the OS forgets the alpha
  // with it), 0..255 sets WS_EX_LAYERED plus LWA_ALPHA.
  rime::core::Error set_transparent(std::uint64_t id, int value,
                                    std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // WinSetTransColor: color key (optional alpha) or ''/'off' to clear;
  // clears WS_EX_LAYERED exactly like AHK's WinSetTrans with no flags.
  rime::core::Error set_trans_color(std::uint64_t id, std::string_view value,
                                    std::chrono::milliseconds timeout = std::chrono::seconds(5));
  // WinSetRegion: builds the region from the AHK options string and hands
  // it to SetWindowRgn (the OS owns the HRGN after a successful call); a
  // blank string clears the region back to normal.
  rime::core::Error set_region(std::uint64_t id, std::string_view value,
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
  // Impl is defined in window.cpp only, so the translation units that own a
  // slice of the service (window_groups.cpp, window_set.cpp, window_at.cpp)
  // reach the shared state through these instead of the PIMPL. Both return
  // references into Impl; they are UI-lane resources like impl_ itself.
  detail::WindowRegistry& registry();
  detail::WindowGroups& group_state();
  // Shared body of set_style / set_ex_style (identical but for the index).
  rime::core::Error set_style_bits(std::uint64_t id, std::string_view value, int index,
                                   std::chrono::milliseconds timeout);
};

}  // namespace rime::win32
