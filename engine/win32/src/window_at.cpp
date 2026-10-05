#include "rime/win32/window.hpp"

#include "rime/core/lane.hpp"

#include "utf.hpp"
#include "window_match.hpp"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// MouseGetPos point query: the window under the point, the topmost control
// under it and that control's ClassNN. Everything is resolved on the UI lane;
// raw HWNDs never leave it.

namespace rime::win32 {

using detail::ClassNNCounter;
using detail::ClassNNInstance;
using detail::build_info;
using detail::expired_deadline;
using detail::next_class_nn;

namespace {

namespace lane = rime::core;

}  // namespace

rime::core::Error WindowService::window_at(const std::int32_t x, const std::int32_t y,
                                           WindowAtInfo& out,
                                           const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  WindowAtInfo info;
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const POINT point{x, y};
        const HWND child_under = WindowFromPoint(point);
        // No window under the point (desktop, or WindowFromPoint refused):
        // a successful read of "nothing is here", like AHK's blank outputs.
        if (!child_under) return;
        // AHK GetNonChildParent (source/window.cpp:1136): first ancestor
        // without WS_CHILD; the window itself when it is not a child.
        HWND parent = child_under;
        for (;;) {
          if ((static_cast<DWORD>(GetWindowLongPtrW(parent, GWL_STYLE)) & WS_CHILD) == 0) break;
          const HWND next = GetParent(parent);
          if (!next) break;
          parent = next;
        }
        WindowInfo window_info;
        if (!build_info(registry(), parent, window_info).ok()) {
          return;  // parent vanished between the point read and the snapshot
        }
        info.window = std::move(window_info);

        // AHK EnumChildFindPoint (source/script2.cpp:1380): the topmost
        // visible descendant whose rect contains the point; a rect entirely
        // enclosed by the incumbent wins outright, otherwise the closer
        // center wins unless the candidate entirely encloses the incumbent.
        // Enumeration continues through every descendant so equal candidates
        // keep z-order precedence.
        HWND control_hwnd = child_under;
        if (child_under != parent) {
          struct PointState {
            POINT point;
            HWND found{nullptr};
            RECT found_rect{};
            double distance{0.0};
          } find{point, nullptr, {}, 0.0};
          EnumChildWindows(
              parent,
              [](HWND control, LPARAM parameter) -> BOOL {
                auto* state = reinterpret_cast<PointState*>(parameter);
                if (!IsWindowVisible(control)) return TRUE;  // Window Spy rule
                RECT rect{};
                if (!GetWindowRect(control, &rect)) return TRUE;
                // right/bottom are exclusive (MSDN): use < on both edges.
                if (!(state->point.x >= rect.left && state->point.x < rect.right &&
                      state->point.y >= rect.top && state->point.y < rect.bottom)) {
                  return TRUE;
                }
                const double center_x = rect.left + (rect.right - rect.left) / 2.0;
                const double center_y = rect.top + (rect.bottom - rect.top) / 2.0;
                const double distance =
                    std::hypot(static_cast<double>(state->point.x) - center_x,
                               static_cast<double>(state->point.y) - center_y);
                bool update = state->found == nullptr;
                if (!update) {
                  const RECT& old = state->found_rect;
                  if (rect.left >= old.left && rect.right <= old.right &&
                      rect.top >= old.top && rect.bottom <= old.bottom) {
                    update = true;  // new is entirely enclosed by old
                  } else if (distance < state->distance &&
                             (old.left < rect.left || old.right > rect.right ||
                              old.top < rect.top || old.bottom > rect.bottom)) {
                    update = true;  // closer center, and new does not enclose old
                  }
                }
                if (update) {
                  state->found = control;
                  state->found_rect = rect;
                  state->distance = distance;
                }
                return TRUE;
              },
              reinterpret_cast<LPARAM>(&find));
          if (find.found) control_hwnd = find.found;
        }
        // The parent itself (no control per se): window only, blank control.
        if (control_hwnd == parent) return;
        // ClassNN with the exact numbering controls() emits: the shared
        // counter walks the same descendants with the same case-insensitive
        // per-class count, stopping at the target (AHK ControlGetClassNN
        // counts the same way but is case-sensitive; we stay consistent with
        // our own WinGetControls).
        struct ClassNNState {
          ClassNNCounter counts;
          HWND target{nullptr};
          std::wstring class_name;
          int instance{0};
        } state{{}, control_hwnd, {}, 0};
        EnumChildWindows(
            parent,
            [](HWND control, LPARAM parameter) -> BOOL {
              auto* cn = reinterpret_cast<ClassNNState*>(parameter);
              const ClassNNInstance instance = next_class_nn(cn->counts, control);
              if (instance.number == 0) return TRUE;  // unnameable or past the cap
              if (control == cn->target) {
                cn->instance = instance.number;
                cn->class_name = instance.class_name;
                return FALSE;  // target numbered; stop the walk
              }
              return TRUE;
            },
            reinterpret_cast<LPARAM>(&state));
        if (state.instance == 0) return;  // vanished or unnameable: window only
        ControlInfo control_info;
        control_info.id = registry().id_for(control_hwnd);
        control_info.class_name = to_utf8(state.class_name);
        control_info.class_nn = control_info.class_name + std::to_string(state.instance);
        info.control = std::move(control_info);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = std::move(info);
  return rime::core::Error::none();
}

}  // namespace rime::win32
