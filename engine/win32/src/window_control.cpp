#include "rime/win32/window.hpp"

#include "rime/core/lane.hpp"

#include "utf.hpp"
#include "window_match.hpp"

#include <windows.h>

#include <string>

// Control verbs (@rime/control Phase 1): Win32-direct implementations behind
// WindowService::control_* (see window.hpp). Every method resolves the stable
// id to an HWND on the UI lane, then applies the AHK-documented message
// sequence: PostMessage for clicks (never Send), SendMessageTimeout with
// SMTO_ABORTIFHUNG for text round-trips, attach + SetFocus/SetActiveWindow
// for focus paths.

namespace rime::win32 {

namespace lane = rime::core;

namespace {

using Error = rime::core::Error;
using Code = rime::core::Error::Code;

constexpr const char* kControlGoneMessage = "control no longer exists";

bool past_deadline(const std::chrono::milliseconds timeout) {
  return timeout <= std::chrono::milliseconds::zero();
}

// Button VK -> down/up message pair. X buttons ride WM_XBUTTONDOWN/UP with
// the XBUTTON id in the high word (AHK Line::ConvertMouseButton set).
bool click_messages(const int vk, UINT& down, UINT& up, WPARAM& wparam) {
  wparam = 0;
  switch (vk) {
    case VK_LBUTTON:
      down = WM_LBUTTONDOWN;
      up = WM_LBUTTONUP;
      return true;
    case VK_RBUTTON:
      down = WM_RBUTTONDOWN;
      up = WM_RBUTTONUP;
      return true;
    case VK_MBUTTON:
      down = WM_MBUTTONDOWN;
      up = WM_MBUTTONUP;
      return true;
    case VK_XBUTTON1:
    case VK_XBUTTON2:
      down = WM_XBUTTONDOWN;
      up = WM_XBUTTONUP;
      wparam = MAKEWPARAM(0, vk == VK_XBUTTON1 ? XBUTTON1 : XBUTTON2);
      return true;
    default:
      return false;
  }
}

}  // namespace

rime::core::Error WindowService::control_click(const std::uint64_t id, const ControlClick& click,
                                               const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) return {Code::InvalidContract, "control.click timed out before dispatch"};
  if (click.count < 0) return {Code::InvalidContract, "control.click count must be >= 0"};
  UINT down = 0;
  UINT up = 0;
  WPARAM wparam = 0;
  if (!click_messages(click.vk, down, up, wparam)) {
    return {Code::InvalidContract, "control.click button must be a mouse button"};
  }
  if (click.count == 0) return Error::none();  // AHK: count 0 is a silent no-op.
  Error result = Error::none();
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND control = registry().hwnd_for(id);
        if (!control) {
          result = {Code::TargetGone, kControlGoneMessage};
          return;
        }
        POINT point{click.x, click.y};
        if (click.x == 0 && click.y == 0) {
          // Caller passed no position: click the center (AHK/AutoIt3 rule).
          RECT rect{};
          if (!GetWindowRect(control, &rect)) {
            result = {Code::ExecutionFailed, "cannot read the control rect"};
            return;
          }
          point.x = (rect.right - rect.left) / 2;
          point.y = (rect.bottom - rect.top) / 2;
        }
        const LPARAM lparam = MAKELPARAM(point.x, point.y);
        if (click.activate) {
          // AHK non-NA path: attach to the target thread and activate the
          // root window first. A hung target is skipped but the messages
          // below are still posted (queue behavior, AHK rule).
          const HWND root = GetAncestor(control, GA_ROOT);
          DWORD target_thread = 0;
          if (root) target_thread = GetWindowThreadProcessId(root, nullptr);
          const DWORD self_thread = GetCurrentThreadId();
          bool attached = false;
          if (target_thread != 0 && target_thread != self_thread &&
              !IsHungAppWindow(root)) {
            attached = AttachThreadInput(self_thread, target_thread, TRUE) != FALSE;
            if (attached && root) SetActiveWindow(root);
          }
          for (int i = 0; i < click.count && result.ok(); ++i) {
            if (click.phase != ControlClick::Phase::Up &&
                !PostMessageW(control, down, wparam, lparam)) {
              result = {Code::ExecutionFailed, "cannot post the click down message"};
            }
            if (result.ok() && click.phase != ControlClick::Phase::Down &&
                !PostMessageW(control, up, wparam, lparam)) {
              result = {Code::ExecutionFailed, "cannot post the click up message"};
            }
          }
          if (attached && target_thread != 0) AttachThreadInput(self_thread, target_thread, FALSE);
        } else {
          for (int i = 0; i < click.count && result.ok(); ++i) {
            if (click.phase != ControlClick::Phase::Up &&
                !PostMessageW(control, down, wparam, lparam)) {
              result = {Code::ExecutionFailed, "cannot post the click down message"};
            }
            if (result.ok() && click.phase != ControlClick::Phase::Down &&
                !PostMessageW(control, up, wparam, lparam)) {
              result = {Code::ExecutionFailed, "cannot post the click up message"};
            }
          }
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_focus(const std::uint64_t id,
                                               const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) return {Code::InvalidContract, "control.focus timed out before dispatch"};
  Error result = Error::none();
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND control = registry().hwnd_for(id);
        if (!control) {
          result = {Code::TargetGone, kControlGoneMessage};
          return;
        }
        // AHK ControlFocus rule: attach, SetFocus, never report failure -
        // focus can change on the next line, so checking would be a lie.
        const DWORD target_thread = GetWindowThreadProcessId(control, nullptr);
        const DWORD self_thread = GetCurrentThreadId();
        const bool attached = target_thread != 0 && target_thread != self_thread &&
                              !IsHungAppWindow(control) &&
                              AttachThreadInput(self_thread, target_thread, TRUE) != FALSE;
        SetFocus(control);
        if (attached) AttachThreadInput(self_thread, target_thread, FALSE);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_set_text(const std::uint64_t id,
                                                  const std::wstring& text,
                                                  const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control.setText timed out before dispatch"};
  }
  Error result = Error::none();
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND control = registry().hwnd_for(id);
        if (!control) {
          result = {Code::TargetGone, kControlGoneMessage};
          return;
        }
        // AHK rule: Send (not Post) - most apps ignore a posted WM_SETTEXT.
        DWORD_PTR ignored = 0;
        if (!SendMessageTimeoutW(control, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()),
                                 SMTO_ABORTIFHUNG, 5000, &ignored)) {
          result = {Code::ExecutionFailed, "control did not accept the text"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_get_text(const std::uint64_t id, std::string& out,
                                                  const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control.getText timed out before dispatch"};
  }
  out.clear();
  Error result = Error::none();
  std::wstring wide;
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND control = registry().hwnd_for(id);
        if (!control) {
          result = {Code::TargetGone, kControlGoneMessage};
          return;
        }
        // AHK GetWindowTextTimeout rule: length first (0 means empty, not an
        // error), then the text, clamped against lying reporters.
        DWORD_PTR length = 0;
        if (!SendMessageTimeoutW(control, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG, 5000,
                                 &length) ||
            length == 0) {
          return;
        }
        std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
        DWORD_PTR copied = 0;
        if (!SendMessageTimeoutW(control, WM_GETTEXT, static_cast<WPARAM>(buffer.size()),
                                 reinterpret_cast<LPARAM>(buffer.data()), SMTO_ABORTIFHUNG, 5000,
                                 &copied) ||
            copied == 0) {
          return;
        }
        if (copied > buffer.size() - 1) copied = buffer.size() - 1;
        while (copied > 0 && buffer[copied - 1] == L'\0') --copied;
        wide.assign(buffer.data(), copied);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = to_utf8(wide);
  return Error::none();
}

rime::core::Error WindowService::control_send_text(const std::uint64_t id,
                                                   const std::wstring& text,
                                                   const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control.sendText timed out before dispatch"};
  }
  Error result = Error::none();
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND control = registry().hwnd_for(id);
        if (!control) {
          result = {Code::TargetGone, kControlGoneMessage};
          return;
        }
        // AHK ControlSendText (RAW_TEXT) rule: \r and \n become VK_RETURN,
        // \b becomes VK_BACK, \t becomes VK_TAB (posted as key messages);
        // every other char goes by WM_CHAR. No modifier parsing, no global
        // side effects - that is what makes sendText the safe default.
        for (const wchar_t ch : text) {
          int vk = 0;
          if (ch == L'\r' || ch == L'\n') {
            vk = VK_RETURN;
          } else if (ch == L'\b') {
            vk = VK_BACK;
          } else if (ch == L'\t') {
            vk = VK_TAB;
          }
          if (vk != 0) {
            const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
            const LPARAM down_param = static_cast<LPARAM>((scan << 16) | 1);
            const LPARAM up_param = static_cast<LPARAM>((scan << 16) | 0xC0000001);
            if (!PostMessageW(control, WM_KEYDOWN, static_cast<WPARAM>(vk), down_param) ||
                !PostMessageW(control, WM_KEYUP, static_cast<WPARAM>(vk), up_param)) {
              result = {Code::ExecutionFailed, "cannot post the control key message"};
              return;
            }
          } else if (!PostMessageW(control, WM_CHAR, static_cast<WPARAM>(ch), 0)) {
            result = {Code::ExecutionFailed, "cannot post the control char message"};
            return;
          }
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_is_visible(const std::uint64_t id, bool& out,
                                                    const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control visibility check timed out before dispatch"};
  }
  Error result = Error::none();
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND control = registry().hwnd_for(id);
        if (!control) {
          result = {Code::TargetGone, kControlGoneMessage};
          return;
        }
        out = IsWindowVisible(control) != FALSE;
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_is_enabled(const std::uint64_t id, bool& out,
                                                    const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control enabled check timed out before dispatch"};
  }
  Error result = Error::none();
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND control = registry().hwnd_for(id);
        if (!control) {
          result = {Code::TargetGone, kControlGoneMessage};
          return;
        }
        out = IsWindowEnabled(control) != FALSE;
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_rect(const std::uint64_t id, Rect& out,
                                              const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control rect read timed out before dispatch"};
  }
  Error result = Error::none();
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND control = registry().hwnd_for(id);
        if (!control) {
          result = {Code::TargetGone, kControlGoneMessage};
          return;
        }
        RECT rect{};
        if (!GetWindowRect(control, &rect)) {
          result = {Code::ExecutionFailed, "cannot read the control rect"};
          return;
        }
        out = Rect{rect.left, rect.top, rect.right, rect.bottom};
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_alive(const std::uint64_t id, bool& out,
                                              const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control liveness check timed out before dispatch"};
  }
  Error result = Error::none();
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        // Ids are never recycled inside one service (generation-guarded), so
        // liveness is just resolvability. Nothing is freed: dispose() is a
        // probe, not a destructor.
        out = registry().hwnd_for(id) != nullptr;
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_id_for_hwnd(const std::uint64_t window_id, void* hwnd,
                                                      std::uint64_t& out,
                                                      const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control hwnd resolve timed out before dispatch"};
  }
  Error result = Error::none();
  const auto call_error = ui().call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = registry().hwnd_for(window_id);
        if (!window) {
          result = {Code::TargetGone, kControlGoneMessage};
          return;
        }
        const HWND target = static_cast<HWND>(hwnd);
        if (!IsWindow(target)) {
          result = {Code::InvalidContract, "control hwnd is not a live window"};
          return;
        }
        struct Search {
          detail::WindowRegistry* registry;
          HWND target;
          std::uint64_t found{0};
        } state{&registry(), target, 0};
        EnumChildWindows(
            window,
            [](HWND control, LPARAM parameter) -> BOOL {
              auto* search = reinterpret_cast<Search*>(parameter);
              if (control == search->target) {
                search->found = search->registry->id_for(control);
                return FALSE;
              }
              return TRUE;
            },
            reinterpret_cast<LPARAM>(&state));
        if (state.found == 0) {
          result = {Code::TargetGone, "control hwnd is not a descendant of the window"};
          return;
        }
        out = state.found;
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

}  // namespace rime::win32
