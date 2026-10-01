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
  Rect rect{};
  bool visible{false};
  bool minimized{false};
  std::uint32_t process_id{0};
  // "normal" | "minimized" | "maximized" | "hidden"
  std::string state{"normal"};
};

// Deterministic JSON shape shared by the executor, the JS module and tests:
// {id,title,className,processName,rect:{left,top,right,bottom},visible,
//  minimized,state,processId}
rime::core::json::Value window_info_json(const WindowInfo& info);

// WinTitle-style selector resolved entirely on the UI lane. JS never sees an
// HWND: `id` is the stable window id, `active` selects the foreground window,
// `exact_title` switches contains-matching to equality.
struct WindowQuery {
  std::string title;   // UTF-8
  bool exact_title{false};
  std::string class_name;
  std::string process_name;  // ahk_exe: image basename match
  std::uint64_t id{0};       // ahk_id: stable id; 0 = unset
  bool include_hidden{false};
  bool active{false};
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
  rime::core::Error info(std::uint64_t id, WindowInfo& out,
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

  UiThread& ui();
  [[nodiscard]] UiThreadState state() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rime::win32
