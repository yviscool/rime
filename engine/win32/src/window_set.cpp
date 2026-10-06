#include "rime/win32/window.hpp"

#include "rime/core/lane.hpp"

#include "utf.hpp"
#include "window_foreground.hpp"
#include "window_match.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// WinSet* family: every string-valued setter (title, enabled, always-on-top,
// style/ex-style, transparency, color key, region). Parsing lives in
// window_parse.cpp; this TU only applies an already-parsed change on the UI
// lane.

namespace rime::win32 {

using detail::expired_deadline;
using detail::kWindowGoneMessage;

namespace {

namespace lane = rime::core;

}  // namespace

rime::core::Error WindowService::set_title(const std::uint64_t id, const std::string& title,
                                           const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = registry().hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, kWindowGoneMessage};
          return;
        }
        // WinSetTitle: SetWindowText fails for windows that refuse the
        // change; that is a hard error, matching AHK's FR_E_WIN32.
        if (!SetWindowTextW(window, from_utf8(title).c_str())) {
          const DWORD failure = GetLastError();
          result = {rime::core::Error::Code::ExecutionFailed,
                    "SetWindowText failed (win32 error " + std::to_string(failure) + ")"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::set_enabled(const std::uint64_t id, const int value,
                                             const std::chrono::milliseconds timeout) {
  if (value < -1 || value > 1) {
    return {rime::core::Error::Code::InvalidContract, "enabled value must be -1, 0 or 1"};
  }
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = registry().hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, kWindowGoneMessage};
          return;
        }
        // WinSetEnabled: -1 toggles the current state; EnableWindow's
        // return value is unreliable, so verify through IsWindowEnabled.
        const BOOL want = value == -1 ? (IsWindowEnabled(window) ? 0 : 1) : value;
        EnableWindow(window, want);
        if ((IsWindowEnabled(window) ? 1 : 0) != (want ? 1 : 0)) {
          result = {rime::core::Error::Code::ExecutionFailed,
                    "EnableWindow did not take effect"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::set_always_on_top(const std::uint64_t id, const int value,
                                                   const std::chrono::milliseconds timeout) {
  if (value < -1 || value > 1) {
    return {rime::core::Error::Code::InvalidContract, "always-on-top value must be -1, 0 or 1"};
  }
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = registry().hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, kWindowGoneMessage};
          return;
        }
        // WinSetAlwaysOnTop: SetWindowPos with the topmost handle; -1
        // resolves against the current WS_EX_TOPMOST bit (SetWindowLong
        // does not take on some windows, so the z-order call is required).
        // Windows silently ignores the z-order change unless the calling
        // process holds SetForegroundWindow permission (MSDN SetWindowPos),
        // so the result is read back and, when it did not take, the
        // foreground is acquired through the shared ladder (bare Alt tap
        // last, like AHK's WinActivate) and restored before one final
        // attempt.
        const bool topmost =
            value == -1 ? (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0
                        : value != 0;
        const auto apply = [&]() {
          if (!SetWindowPos(window, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
            return false;
          }
          return ((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) ==
                 topmost;
        };
        bool took = apply();
        if (!took) {
          const HWND previous_foreground = GetForegroundWindow();
          static_cast<void>(acquire_foreground(window));
          took = apply();
          if (previous_foreground && previous_foreground != window) {
            static_cast<void>(SetForegroundWindow(previous_foreground));
          }
        }
        if (!took) {
          result = {rime::core::Error::Code::ExecutionFailed,
                    "topmost state did not take (SetForegroundWindow permission denied)"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::set_style_bits(const std::uint64_t id,
                                                const std::string_view value, const int index,
                                                const std::chrono::milliseconds timeout) {
  StyleChange change;
  if (!parse_style_change(value, change)) {
    return {rime::core::Error::Code::InvalidContract,
            "style value must be '+N', '-N', '^N' or a plain decimal/0x-hex number"};
  }
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = registry().hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, kWindowGoneMessage};
          return;
        }
        // AHK WinSetStyle: work in unsigned 32-bit (sign-extension of
        // WS_POPUP-style bits would make same-value comparisons lie), treat
        // "no change needed" as success, then SetWindowLong with the MSDN
        // precise error check plus a read-back (AHK: even a partial change
        // counts as a success).
        const auto original = static_cast<std::uint32_t>(GetWindowLongPtrW(window, index));
        std::uint32_t updated = original;
        switch (change.op) {
          case StyleChangeOp::Add:
            updated = original | change.mask;
            break;
          case StyleChangeOp::Remove:
            updated = original & ~change.mask;
            break;
          case StyleChangeOp::Toggle:
            updated = original ^ change.mask;
            break;
          case StyleChangeOp::Replace:
            updated = change.mask;
            break;
        }
        if (updated == original) return;
        SetLastError(ERROR_SUCCESS);
        SetWindowLongPtrW(window, index, static_cast<LONG_PTR>(updated));
        if (GetLastError() != ERROR_SUCCESS ||
            static_cast<std::uint32_t>(GetWindowLongPtrW(window, index)) == original) {
          result = {rime::core::Error::Code::ExecutionFailed,
                    "SetWindowLong did not take effect"};
          return;
        }
        // AHK pairs the style change with a frame refresh; without
        // SWP_FRAMECHANGED only parts of the border repaint.
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(window, nullptr, TRUE);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::set_style(const std::uint64_t id, const std::string_view value,
                                            const std::chrono::milliseconds timeout) {
  return set_style_bits(id, value, GWL_STYLE, timeout);
}

rime::core::Error WindowService::set_ex_style(const std::uint64_t id,
                                              const std::string_view value,
                                              const std::chrono::milliseconds timeout) {
  return set_style_bits(id, value, GWL_EXSTYLE, timeout);
}

rime::core::Error WindowService::set_transparent(const std::uint64_t id, const int value,
                                                  const std::chrono::milliseconds timeout) {
  if (value < -1 || value > 255) {
    return {rime::core::Error::Code::InvalidContract, "transparent value must be -1 or 0..255"};
  }
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = registry().hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, kWindowGoneMessage};
          return;
        }
        const LONG_PTR ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if (value == -1) {
          // AHK WinSetTrans with no flags: drop WS_EX_LAYERED; the OS
          // forgets the alpha and the color key along with it.
          SetLastError(ERROR_SUCCESS);
          SetWindowLongPtrW(window, GWL_EXSTYLE, ex_style & ~WS_EX_LAYERED);
          if (GetLastError() != ERROR_SUCCESS) {
            result = {rime::core::Error::Code::ExecutionFailed,
                      "SetWindowLong failed to clear WS_EX_LAYERED"};
          }
          return;
        }
        SetWindowLongPtrW(window, GWL_EXSTYLE, ex_style | WS_EX_LAYERED);
        if (!SetLayeredWindowAttributes(window, 0, static_cast<BYTE>(value), LWA_ALPHA)) {
          const DWORD failure = GetLastError();
          result = {rime::core::Error::Code::ExecutionFailed,
                    "SetLayeredWindowAttributes failed (win32 error " +
                        std::to_string(failure) + ")"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::set_trans_color(const std::uint64_t id,
                                                  const std::string_view value,
                                                  const std::chrono::milliseconds timeout) {
  TransColorChange change;
  if (!parse_trans_color_change(value, change)) {
    return {rime::core::Error::Code::InvalidContract,
            "trans-color value must be 'off', '', 'RRGGBB'/'0xRRGGBB' "
            "and an optional 0..255 alpha suffix"};
  }
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = registry().hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, kWindowGoneMessage};
          return;
        }
        const LONG_PTR ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if (change.off) {
          // Same clear path as WinSetTransparent("Off"): no flags left, so
          // AHK drops WS_EX_LAYERED instead of leaving it set with no key.
          SetLastError(ERROR_SUCCESS);
          SetWindowLongPtrW(window, GWL_EXSTYLE, ex_style & ~WS_EX_LAYERED);
          if (GetLastError() != ERROR_SUCCESS) {
            result = {rime::core::Error::Code::ExecutionFailed,
                      "SetWindowLong failed to clear WS_EX_LAYERED"};
          }
          return;
        }
        SetWindowLongPtrW(window, GWL_EXSTYLE, ex_style | WS_EX_LAYERED);
        // 0xRRGGBB (our wire order) to Win32's 0x00BBGGRR color key.
        const COLORREF color =
            static_cast<COLORREF>(((change.rgb >> 16) & 0xFF) | (change.rgb & 0xFF00) |
                                  ((change.rgb & 0xFF) << 16));
        const DWORD flags =
            (change.color_key ? LWA_COLORKEY : 0) | (change.with_alpha ? LWA_ALPHA : 0);
        if (!SetLayeredWindowAttributes(window, color, static_cast<BYTE>(change.alpha), flags)) {
          const DWORD failure = GetLastError();
          result = {rime::core::Error::Code::ExecutionFailed,
                    "SetLayeredWindowAttributes failed (win32 error " +
                        std::to_string(failure) + ")"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::set_region(const std::uint64_t id, const std::string_view value,
                                             const std::chrono::milliseconds timeout) {
  RegionSpec spec;
  if (!parse_region_options(value, spec)) {
    return {rime::core::Error::Code::InvalidContract,
            "region value must be '<x>-<y>' coordinate pairs with optional E, "
            "R[<rrw>-<rrh>], W[<width>]/Wind and H[<height>] options"};
  }
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = registry().hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, kWindowGoneMessage};
          return;
        }
        if (spec.kind == RegionKind::Restore) {
          // AHK: setting the region to NULL restores the window's proper
          // region (GetWindowRect-based hacks leave maximized windows
          // clipped, per the AHK v1.0.31.07 note).
          if (!SetWindowRgn(window, nullptr, TRUE)) {
            const DWORD failure = GetLastError();
            result = {rime::core::Error::Code::ExecutionFailed,
                      "SetWindowRgn failed to clear the region (win32 error " +
                          std::to_string(failure) + ")"};
          }
          return;
        }
        // Width and height are relative sizes; AHK converts them to the
        // right and bottom edges by adding the anchor point.
        const std::int64_t right =
            static_cast<std::int64_t>(spec.coords[0]) + spec.width;
        const std::int64_t bottom =
            static_cast<std::int64_t>(spec.coords[1]) + spec.height;
        HRGN region = nullptr;
        switch (spec.kind) {
          case RegionKind::Ellipse:
            region = CreateEllipticRgn(spec.coords[0], spec.coords[1],
                                       static_cast<int>(right), static_cast<int>(bottom));
            break;
          case RegionKind::RoundRect:
            region = CreateRoundRectRgn(spec.coords[0], spec.coords[1],
                                        static_cast<int>(right), static_cast<int>(bottom),
                                        spec.round_width, spec.round_height);
            break;
          case RegionKind::Rect:
            region = CreateRectRgn(spec.coords[0], spec.coords[1], static_cast<int>(right),
                                   static_cast<int>(bottom));
            break;
          case RegionKind::Polygon: {
            std::vector<POINT> points(spec.coords.size() / 2);
            for (std::size_t index = 0; index < points.size(); ++index) {
              points[index].x = spec.coords[index * 2];
              points[index].y = spec.coords[index * 2 + 1];
            }
            region = CreatePolygonRgn(points.data(), static_cast<int>(points.size()),
                                      spec.winding ? WINDING : ALTERNATE);
            break;
          }
          case RegionKind::Restore:
            return;  // handled above; keeps the switch exhaustive
        }
        if (!region) {
          const DWORD failure = GetLastError();
          result = {rime::core::Error::Code::ExecutionFailed,
                    "region creation failed (win32 error " + std::to_string(failure) + ")"};
          return;
        }
        // On success the OS owns the HRGN and frees the previous region;
        // on failure we still own it and must release it (AHK same).
        if (!SetWindowRgn(window, region, TRUE)) {
          DeleteObject(region);
          const DWORD failure = GetLastError();
          result = {rime::core::Error::Code::ExecutionFailed,
                    "SetWindowRgn failed (win32 error " + std::to_string(failure) + ")"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

}  // namespace rime::win32
