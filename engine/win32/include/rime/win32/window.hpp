#pragma once

#include "rime/core/json.hpp"
#include "rime/core/types.hpp"
#include "rime/win32/ui_thread.hpp"

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
  std::string title;  // UTF-8
  Rect rect{};
  bool visible{false};
  bool minimized{false};
  std::uint32_t process_id{0};
};

// Deterministic JSON shape shared by the executor, the JS module and tests:
// {id,title,rect:{left,top,right,bottom},visible,minimized,processId}
rime::core::json::Value window_info_json(const WindowInfo& info);

// Top-level window operations. Every call is routed to the UI thread; raw
// HWND values never leave the UI lane. Window ids are stable per service
// and fail with InvalidState once the underlying window is gone.
class WindowService final {
 public:
  WindowService();
  ~WindowService();
  WindowService(const WindowService&) = delete;
  WindowService& operator=(const WindowService&) = delete;

  rime::core::Error start();
  rime::core::Error stop();

  rime::core::Error list(std::vector<WindowInfo>& out);
  // nullopt (with a successful result) when there is no foreground window.
  rime::core::Error active(std::optional<WindowInfo>& out);
  rime::core::Error info(std::uint64_t id, WindowInfo& out);
  // Moves the window to a named placement: left, right, top, bottom, full.
  rime::core::Error move(std::uint64_t id, std::string_view placement);
  rime::core::Error move_rect(std::uint64_t id, const Rect& rect);
  // Restores when minimized and requests foreground activation.
  rime::core::Error focus(std::uint64_t id);
  // Resolves a placement against the primary monitor work area.
  rime::core::Error placement_rect(std::string_view placement, Rect& out);

  UiThread& ui();
  [[nodiscard]] UiThreadState state() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rime::win32
