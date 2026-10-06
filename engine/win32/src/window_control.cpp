#include "rime/win32/window.hpp"

#include "rime/core/lane.hpp"

#include "utf.hpp"
#include "window_match.hpp"

#include <windows.h>

#include <commctrl.h>

#include <optional>
#include <string>
#include <vector>

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
using detail::ClassNNCounter;
using detail::ClassNNInstance;
using detail::next_class_nn;

constexpr const char* kControlGoneMessage = "control no longer exists";

bool past_deadline(const std::chrono::milliseconds timeout) {
  return timeout <= std::chrono::milliseconds::zero();
}

// SendMessageTimeout wrappers: 2s for state/list traffic, 5s for text
// (AHK's two tiers). SMTO_ABORTIFHUNG throughout - hung targets fail fast
// instead of hanging the lane.
bool send2(HWND control, UINT message, WPARAM wparam, LPARAM lparam, DWORD_PTR& result) {
  return SendMessageTimeoutW(control, message, wparam, lparam, SMTO_ABORTIFHUNG, 2000,
                             &result) != FALSE;
}

bool send5(HWND control, UINT message, WPARAM wparam, LPARAM lparam, DWORD_PTR& result) {
  return SendMessageTimeoutW(control, message, wparam, lparam, SMTO_ABORTIFHUNG, 5000,
                             &result) != FALSE;
}

bool ascii_case_insensitive_contains(const std::wstring& haystack, const wchar_t* needle) {
  if (!needle || !*needle) return false;
  std::wstring hay = haystack;
  for (auto& ch : hay) {
    if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
  }
  std::wstring ndl(needle);
  for (auto& ch : ndl) {
    if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
  }
  return hay.find(ndl) != std::wstring::npos;
}

// AHK GetIndexControlType rule: class-name substring decides the message
// table. Anything else is ERR_GUI_NOT_FOR_THIS_TYPE.
std::optional<WindowService::ControlListKind> list_kind_of(HWND control) {
  wchar_t name[256] = {0};
  if (GetClassNameW(control, name, 256) == 0) return std::nullopt;
  if (ascii_case_insensitive_contains(name, L"combo")) return WindowService::ControlListKind::Combo;
  if (ascii_case_insensitive_contains(name, L"list")) return WindowService::ControlListKind::List;
  return std::nullopt;
}

bool is_multi_select_list(HWND control) {
  const LONG_PTR style = GetWindowLongPtrW(control, GWL_STYLE);
  return (style & (LBS_EXTENDEDSEL | LBS_MULTIPLESEL)) != 0;
}

// AHK ControlNotifyParent: the parent learns through WM_COMMAND, because a
// bare SETCURSEL never updates most apps' UI.
void notify_parent(HWND control, UINT code) {
  const HWND parent = GetParent(control);
  if (!parent) return;
  const UINT_PTR id = GetDlgCtrlID(control);
  DWORD_PTR ignored = 0;
  (void)SendMessageTimeoutW(parent, WM_COMMAND, MAKEWPARAM(id, code),
                            reinterpret_cast<LPARAM>(control), SMTO_ABORTIFHUNG, 2000, &ignored);
}

// Shared WM_GETTEXT two-step (AHK GetWindowTextTimeout rule); used by
// control_get_text and list choice reads. Unreadable text reads as empty
// (AHK rule), never as an error.
void read_control_text(HWND control, std::wstring& out) {
  DWORD_PTR length = 0;
  if (!send5(control, WM_GETTEXTLENGTH, 0, 0, length) || length == 0) {
    out.clear();
    return;
  }
  std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
  DWORD_PTR copied = 0;
  if (!send5(control, WM_GETTEXT, static_cast<WPARAM>(buffer.size()),
             reinterpret_cast<LPARAM>(buffer.data()), copied) ||
      copied == 0) {
    out.clear();
    return;
  }
  if (copied > buffer.size() - 1) copied = buffer.size() - 1;
  while (copied > 0 && buffer[copied - 1] == L'\0') --copied;
  out.assign(buffer.data(), copied);
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
        read_control_text(control, wide);
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
                                                      const std::chrono::milliseconds timeout) {  if (past_deadline(timeout)) {
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

// ---- List family (@rime/control Phase 2) ---------------------------------
// Message tables straight from script_autoit.cpp: ControlChooseIndex
// (CB/LB_SETCURSEL, multi LB_SETSEL cumulative), ControlChooseString
// (CB/LB_SELECTSTRING, multi LB_FINDSTRING + LB_SETSEL), ControlNotifyParent
// (two WM_COMMAND round-trips - a bare SETCURSEL never updates most apps).

namespace {

struct ListMsgs {
  UINT add;
  UINT del;
  UINT select_text;
  UINT find;
  UINT get_current;
  UINT get_count;
  UINT get_text_len;
  UINT get_text;
  UINT notify1;
  UINT notify2;
};

ListMsgs combo_msgs() {
  return {CB_ADDSTRING, CB_DELETESTRING, CB_SELECTSTRING, CB_FINDSTRINGEXACT, CB_GETCURSEL,
          CB_GETCOUNT, CB_GETLBTEXTLEN, CB_GETLBTEXT, CBN_SELCHANGE, CBN_SELENDOK};
}

ListMsgs list_msgs() {
  return {LB_ADDSTRING, LB_DELETESTRING, LB_SELECTSTRING, LB_FINDSTRINGEXACT, LB_GETCURSEL,
          LB_GETCOUNT, LB_GETTEXTLEN, LB_GETTEXT, LBN_SELCHANGE, LBN_DBLCLK};
}

}  // namespace

rime::core::Error WindowService::control_list_add(const std::uint64_t id, const std::wstring& text,
                                                  int& index1,
                                                  const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control list add timed out before dispatch"};
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
        const auto kind = list_kind_of(control);
        if (!kind.has_value()) {
          result = {Code::InvalidContract, "control is not a ComboBox or ListBox"};
          return;
        }
        const ListMsgs msgs = *kind == ControlListKind::Combo ? combo_msgs() : list_msgs();
        DWORD_PTR added = 0;
        if (!send2(control, msgs.add, 0, reinterpret_cast<LPARAM>(text.c_str()), added) ||
            added == static_cast<DWORD_PTR>(CB_ERR)) {
          result = {Code::ExecutionFailed, "control did not accept the item"};
          return;
        }
        index1 = static_cast<int>(added) + 1;  // 1-based externally (AHK rule)
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_list_delete(const std::uint64_t id, const int index1,
                                                     const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control list delete timed out before dispatch"};
  }
  if (index1 < 1) return {Code::InvalidContract, "control list index starts at 1"};
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
        const auto kind = list_kind_of(control);
        if (!kind.has_value()) {
          result = {Code::InvalidContract, "control is not a ComboBox or ListBox"};
          return;
        }
        const ListMsgs msgs = *kind == ControlListKind::Combo ? combo_msgs() : list_msgs();
        DWORD_PTR deleted = 0;
        if (!send2(control, msgs.del, static_cast<WPARAM>(index1 - 1), 0, deleted) ||
            deleted == static_cast<DWORD_PTR>(CB_ERR)) {
          result = {Code::ExecutionFailed, "control did not delete the item"};
          return;
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_list_choose_index(
    const std::uint64_t id, const int index1, const bool notify,
    const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control list choose timed out before dispatch"};
  }
  if (index1 < 0) return {Code::InvalidContract, "control list index starts at 1 (-1 clears)"};
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
        const auto kind = list_kind_of(control);
        if (!kind.has_value()) {
          result = {Code::InvalidContract, "control is not a ComboBox or ListBox"};
          return;
        }
        DWORD_PTR applied = 0;
        UINT notify1 = 0;
        UINT notify2 = 0;
        if (*kind == ControlListKind::Combo) {
          if (!send2(control, CB_SETCURSEL, static_cast<WPARAM>(index1 - 1), 0, applied) ||
              (applied == static_cast<DWORD_PTR>(CB_ERR) && index1 != 0)) {
            result = {Code::ExecutionFailed, "control did not select the index"};
            return;
          }
          notify1 = CBN_SELCHANGE;
          notify2 = CBN_SELENDOK;
        } else if (is_multi_select_list(control)) {
          // Multi-select uses the cumulative method (AHK rule): TRUE adds to
          // the selection; index -1 in the wire form clears it.
          if (!send2(control, LB_SETSEL, index1 == 0 ? FALSE : TRUE,
                     static_cast<LPARAM>(index1 - 1), applied) ||
              (applied == static_cast<DWORD_PTR>(LB_ERR) && index1 != 0)) {
            result = {Code::ExecutionFailed, "control did not select the index"};
            return;
          }
          notify1 = LBN_SELCHANGE;
          notify2 = LBN_DBLCLK;
        } else {
          if (!send2(control, LB_SETCURSEL, static_cast<WPARAM>(index1 - 1), 0, applied) ||
              (applied == static_cast<DWORD_PTR>(LB_ERR) && index1 != 0)) {
            result = {Code::ExecutionFailed, "control did not select the index"};
            return;
          }
          notify1 = LBN_SELCHANGE;
          notify2 = LBN_DBLCLK;
        }
        if (notify) {
          notify_parent(control, notify1);
          notify_parent(control, notify2);
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_list_choose_text(
    const std::uint64_t id, const std::wstring& text, const bool notify,
    const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control list choose timed out before dispatch"};
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
        const auto kind = list_kind_of(control);
        if (!kind.has_value()) {
          result = {Code::InvalidContract, "control is not a ComboBox or ListBox"};
          return;
        }
        DWORD_PTR found = 0;
        UINT notify1 = 0;
        UINT notify2 = 0;
        if (*kind == ControlListKind::Combo) {
          if (!send2(control, CB_SELECTSTRING, static_cast<WPARAM>(-1),
                     reinterpret_cast<LPARAM>(text.c_str()), found) ||
              found == static_cast<DWORD_PTR>(CB_ERR)) {
            result = {Code::ExecutionFailed, "control has no such item"};
            return;
          }
          notify1 = CBN_SELCHANGE;
          notify2 = CBN_SELENDOK;
        } else if (is_multi_select_list(control)) {
          // LB_SELECTSTRING is unsupported by multi-select lists (AHK rule):
          // find first, then accumulate with LB_SETSEL.
          if (!send2(control, LB_FINDSTRING, static_cast<WPARAM>(-1),
                     reinterpret_cast<LPARAM>(text.c_str()), found) ||
              found == static_cast<DWORD_PTR>(LB_ERR)) {
            result = {Code::ExecutionFailed, "control has no such item"};
            return;
          }
          DWORD_PTR applied = 0;
          if (!send2(control, LB_SETSEL, TRUE, static_cast<LPARAM>(found), applied) ||
              applied == static_cast<DWORD_PTR>(LB_ERR)) {
            result = {Code::ExecutionFailed, "control did not select the item"};
            return;
          }
          notify1 = LBN_SELCHANGE;
          notify2 = LBN_DBLCLK;
        } else {
          if (!send2(control, LB_SELECTSTRING, static_cast<WPARAM>(-1),
                     reinterpret_cast<LPARAM>(text.c_str()), found) ||
              found == static_cast<DWORD_PTR>(LB_ERR)) {
            result = {Code::ExecutionFailed, "control has no such item"};
            return;
          }
          notify1 = LBN_SELCHANGE;
          notify2 = LBN_DBLCLK;
        }
        if (notify) {
          notify_parent(control, notify1);
          notify_parent(control, notify2);
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_list_find(const std::uint64_t id,
                                                   const std::wstring& text, int& index1,
                                                   const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control list find timed out before dispatch"};
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
        const auto kind = list_kind_of(control);
        if (!kind.has_value()) {
          result = {Code::InvalidContract, "control is not a ComboBox or ListBox"};
          return;
        }
        const ListMsgs msgs = *kind == ControlListKind::Combo ? combo_msgs() : list_msgs();
        DWORD_PTR found = 0;
        if (!send2(control, msgs.find, static_cast<WPARAM>(-1),
                   reinterpret_cast<LPARAM>(text.c_str()), found)) {
          result = {Code::ExecutionFailed, "control search failed"};
          return;
        }
        // 0 means not found (AHK rule); CB_ERR and LB_ERR share the value.
        index1 = (found == static_cast<DWORD_PTR>(CB_ERR)) ? 0 : static_cast<int>(found) + 1;
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_list_index(const std::uint64_t id, int& index1,
                                                    const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control list index read timed out before dispatch"};
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
        const auto kind = list_kind_of(control);
        if (!kind.has_value()) {
          result = {Code::InvalidContract, "control is not a ComboBox or ListBox"};
          return;
        }
        const UINT message =
            *kind == ControlListKind::Combo ? CB_GETCURSEL : LB_GETCURSEL;
        DWORD_PTR current = 0;
        if (!send2(control, message, 0, 0, current)) {
          result = {Code::ExecutionFailed, "control selection read failed"};
          return;
        }
        index1 = (current == static_cast<DWORD_PTR>(CB_ERR)) ? 0 : static_cast<int>(current) + 1;
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_list_choice(const std::uint64_t id, const int index1,
                                                     std::string& out,
                                                     const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control list choice read timed out before dispatch"};
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
        const auto kind = list_kind_of(control);
        if (!kind.has_value()) {
          result = {Code::InvalidContract, "control is not a ComboBox or ListBox"};
          return;
        }
        const ListMsgs msgs = *kind == ControlListKind::Combo ? combo_msgs() : list_msgs();
        int zero_based = index1 - 1;
        if (index1 == 0) {
          // Omitted index reads the current one (AHK rule).
          DWORD_PTR current = 0;
          const UINT message =
              *kind == ControlListKind::Combo ? CB_GETCURSEL : LB_GETCURSEL;
          if (!send2(control, message, 0, 0, current) ||
              current == static_cast<DWORD_PTR>(CB_ERR)) {
            return;
          }
          zero_based = static_cast<int>(current);
        } else if (index1 < 0) {
          result = {Code::InvalidContract, "control list index starts at 1 (0 reads current)"};
          return;
        }
        DWORD_PTR length = 0;
        if (!send2(control, msgs.get_text_len, static_cast<WPARAM>(zero_based), 0, length)) {
          result = {Code::ExecutionFailed, "control item read failed"};
          return;
        }
        std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
        DWORD_PTR copied = 0;
        if (!send2(control, msgs.get_text, static_cast<WPARAM>(zero_based),
                   reinterpret_cast<LPARAM>(buffer.data()), copied) ||
            copied == 0) {
          return;
        }
        if (copied > buffer.size() - 1) copied = buffer.size() - 1;
        wide.assign(buffer.data(), copied);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = to_utf8(wide);
  return Error::none();
}

rime::core::Error WindowService::control_list_items(const std::uint64_t id,
                                                    const std::size_t limit,
                                                    std::vector<std::string>& out,
                                                    const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control list items read timed out before dispatch"};
  }
  if (limit == 0 || limit > 10'000) {
    return {Code::InvalidContract, "control list items limit must be in 1..10000"};
  }
  out.clear();
  Error result = Error::none();
  std::vector<std::wstring> wides;
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
        const auto kind = list_kind_of(control);
        if (!kind.has_value()) {
          result = {Code::InvalidContract, "control is not a ComboBox or ListBox"};
          return;
        }
        const ListMsgs msgs = *kind == ControlListKind::Combo ? combo_msgs() : list_msgs();
        DWORD_PTR count = 0;
        if (!send2(control, msgs.get_count, 0, 0, count) ||
            count == static_cast<DWORD_PTR>(CB_ERR)) {
          return;
        }
        const std::size_t total =
            (std::min)(static_cast<std::size_t>(count), limit);
        for (std::size_t i = 0; i < total; ++i) {
          DWORD_PTR length = 0;
          if (!send2(control, msgs.get_text_len, static_cast<WPARAM>(i), 0, length)) {
            result = {Code::ExecutionFailed, "control item read failed"};
            return;
          }
          std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
          DWORD_PTR copied = 0;
          if (!send2(control, msgs.get_text, static_cast<WPARAM>(i),
                     reinterpret_cast<LPARAM>(buffer.data()), copied) ||
              copied == 0) {
            wides.emplace_back();
            continue;
          }
          if (copied > buffer.size() - 1) copied = buffer.size() - 1;
          wides.emplace_back(buffer.data(), copied);
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  for (const auto& wide : wides) out.push_back(to_utf8(wide));
  return Error::none();
}

rime::core::Error WindowService::control_tab_select(const std::uint64_t id, const int index1,
                                                    const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control tab select timed out before dispatch"};
  }
  if (index1 < 1) return {Code::InvalidContract, "control tab index starts at 1"};
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
        wchar_t tab_class[256] = {0};
        if (GetClassNameW(control, tab_class, 256) == 0 ||
            !ascii_case_insensitive_contains(tab_class, L"tab")) {
          result = {Code::InvalidContract, "control is not a tab control"};
          return;
        }
        DWORD_PTR applied = 0;
        // SETCURFOCUS for every style (AHK ControlSetTab rule). Synthetic
        // clicks and the space key were both verified to have no observable
        // effect on TCS_BUTTONS tabs in background operation - those need
        // genuine activation, which automation by definition cannot provide.
        if (!send2(control, TCM_SETCURFOCUS, static_cast<WPARAM>(index1 - 1), 0, applied)) {
          result = {Code::ExecutionFailed, "control did not select the tab"};
          return;
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_edit_count(const std::uint64_t id, int& lines,
                                                    const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control edit count timed out before dispatch"};
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
        DWORD_PTR count = 0;
        if (!send2(control, EM_GETLINECOUNT, 0, 0, count)) {
          result = {Code::ExecutionFailed, "control line count failed"};
          return;
        }
        lines = static_cast<int>(count);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_edit_caret(const std::uint64_t id, int& line1, int& col1,
                                                    const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control edit caret timed out before dispatch"};
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
        // AHK's fast caret math (not Au3's decrement loop): selection start
        // -> line of start -> line start index -> column. All 1-based out.
        DWORD_PTR sel_start = 0;
        DWORD_PTR sel_end = 0;
        DWORD_PTR sel_status = 0;
        if (!send2(control, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_start),
                   reinterpret_cast<LPARAM>(&sel_end), sel_status)) {
          result = {Code::ExecutionFailed, "control selection read failed"};
          return;
        }
        DWORD_PTR line = 0;
        if (!send2(control, EM_LINEFROMCHAR, sel_start, 0, line)) {
          result = {Code::ExecutionFailed, "control line read failed"};
          return;
        }
        DWORD_PTR line_start = 0;
        if (!send2(control, EM_LINEINDEX, line, 0, line_start) ||
            line_start == static_cast<DWORD_PTR>(-1)) {
          result = {Code::ExecutionFailed, "control line start read failed"};
          return;
        }
        line1 = static_cast<int>(line) + 1;
        col1 = static_cast<int>(sel_start - line_start) + 1;
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_edit_line(const std::uint64_t id, const int line1,
                                                   std::string& out,
                                                   const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control edit line timed out before dispatch"};
  }
  if (line1 < 1) return {Code::InvalidContract, "control edit line starts at 1"};
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
        // EM_GETLINE reads into a caller-sized buffer (first word = size);
        // 0 means empty line OR out of range - EM_GETLINECOUNT tells apart.
        std::wstring buffer(32767 + 1, L'\0');
        *reinterpret_cast<WORD*>(buffer.data()) = 32767;
        DWORD_PTR copied = 0;
        if (!send2(control, EM_GETLINE, static_cast<WPARAM>(line1 - 1),
                   reinterpret_cast<LPARAM>(buffer.data()), copied)) {
          result = {Code::ExecutionFailed, "control line read failed"};
          return;
        }
        if (copied == 0) {
          DWORD_PTR count = 0;
          if (!send2(control, EM_GETLINECOUNT, 0, 0, count)) {
            result = {Code::ExecutionFailed, "control line count failed"};
            return;
          }
          if (static_cast<int>(count) < line1) {
            result = {Code::InvalidContract, "control edit line is past the last line"};
            return;
          }
          return;
        }
        wide.assign(buffer.data(), copied);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = to_utf8(wide);
  return Error::none();
}

rime::core::Error WindowService::control_edit_selected(const std::uint64_t id, std::string& out,
                                                       const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control edit selection timed out before dispatch"};
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
        DWORD_PTR sel_start = 0;
        DWORD_PTR sel_end = 0;
        DWORD_PTR sel_status = 0;
        if (!send2(control, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_start),
                   reinterpret_cast<LPARAM>(&sel_end), sel_status)) {
          result = {Code::ExecutionFailed, "control selection read failed"};
          return;
        }
        if (sel_end <= sel_start) return;
        read_control_text(control, wide);
        // Slice [start, end) in characters (AHK rule; RichEdit-correct,
        // unlike byte loops).
        if (sel_start >= wide.size()) return;
        wide = wide.substr(static_cast<std::size_t>(sel_start),
                           static_cast<std::size_t>(sel_end - sel_start));
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = to_utf8(wide);
  return Error::none();
}

rime::core::Error WindowService::control_edit_paste(const std::uint64_t id,
                                                    const std::wstring& text,
                                                    const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control edit paste timed out before dispatch"};
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
        DWORD_PTR ignored = 0;
        if (!send2(control, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(text.c_str()),
                   ignored)) {
          result = {Code::ExecutionFailed, "control did not accept the paste"};
          return;
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_set_checked(const std::uint64_t id, const int checked,
                                                    const bool ensure_active,
                                                    const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control check set timed out before dispatch"};
  }
  if (checked < -1 || checked > 1) {
    return {Code::InvalidContract, "control check must be -1, 0 or 1"};
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
        const bool toggle = checked == -1;
        const bool want = toggle ? false : (checked == 1);
        if (!toggle) {
          // Pre-check (AHK rule): already there means done.
          DWORD_PTR state = 0;
          if (!send2(control, BM_GETCHECK, 0, 0, state)) {
            result = {Code::ExecutionFailed, "control check read failed"};
            return;
          }
          if ((state == BST_CHECKED) == want) return;
        }
        if (ensure_active) {
          // BM_CLICK wants its dialog active (MSDN rule, AHK comment).
          const HWND root = GetAncestor(control, GA_ROOT);
          const DWORD target_thread =
              root ? GetWindowThreadProcessId(root, nullptr) : 0;
          const DWORD self_thread = GetCurrentThreadId();
          if (target_thread != 0 && target_thread != self_thread &&
              !IsHungAppWindow(root)) {
            if (AttachThreadInput(self_thread, target_thread, TRUE) != FALSE) {
              SetActiveWindow(root);
              AttachThreadInput(self_thread, target_thread, FALSE);
            }
          }
        }
        RECT rect{};
        if (!GetWindowRect(control, &rect)) {
          result = {Code::ExecutionFailed, "cannot read the control rect"};
          return;
        }
        // Synthetic center click, not BM_SETCHECK/BM_CLICK: WM_NOTIFY cannot
        // cross processes, and the click path is what AHK proved compatible.
        const LPARAM center =
            MAKELPARAM((rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2);
        if (!PostMessageW(control, WM_LBUTTONDOWN, MK_LBUTTON, center) ||
            !PostMessageW(control, WM_LBUTTONUP, 0, center)) {
          result = {Code::ExecutionFailed, "cannot post the check click"};
          return;
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_is_checked(const std::uint64_t id, bool& out,
                                                    const std::chrono::milliseconds timeout) {  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control check read timed out before dispatch"};
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
        DWORD_PTR state = 0;
        if (!send2(control, BM_GETCHECK, 0, 0, state)) {
          result = {Code::ExecutionFailed, "control check read failed"};
          return;
        }
        out = state == BST_CHECKED;
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_set_visible(const std::uint64_t id, const bool visible,
                                                      const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control visibility set timed out before dispatch"};
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
        // AHK rule: showing never activates (SW_SHOWNOACTIVATE); hiding is
        // plain SW_HIDE. No ControlDelay equivalent here - settle lives in
        // the executor.
        ShowWindow(control, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_move(const std::uint64_t id, const ControlMoveRect& rect,
                                              const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control move timed out before dispatch"};
  }
  if (!rect.x && !rect.y && !rect.w && !rect.h) {
    return {Code::InvalidContract, "control move needs at least one of x, y, w, h"};
  }
  if ((rect.w && *rect.w < 1) || (rect.h && *rect.h < 1)) {
    return {Code::InvalidContract, "control move w/h must be >= 1"};
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
        // AHK coordinate rule: externally relative to the top-level
        // window's client area, but MoveWindow wants immediate-parent
        // coordinates. So: screen rect -> top-client frame -> apply the
        // caller's fields -> screen -> parent frame -> MoveWindow.
        RECT current{};
        if (!GetWindowRect(control, &current)) {
          result = {Code::ExecutionFailed, "cannot read the control rect"};
          return;
        }
        HWND root = control;
        for (HWND walk = GetParent(control); walk != nullptr; walk = GetParent(walk)) {
          root = walk;
        }
        POINT origin{0, 0};
        ClientToScreen(root, &origin);
        const int rel_left = current.left - origin.x;
        const int rel_top = current.top - origin.y;
        const int rel_right = current.right - origin.x;
        const int rel_bottom = current.bottom - origin.y;
        const int new_left = rect.x ? static_cast<int>(*rect.x) : rel_left;
        const int new_top = rect.y ? static_cast<int>(*rect.y) : rel_top;
        // A move without w/h keeps the size (AHK WinMove rule): shift the
        // far edges by the same displacement instead of pinning them, or a
        // pure position move would collapse or invert the rect.
        const int old_w = rel_right - rel_left;
        const int old_h = rel_bottom - rel_top;
        const int new_right = rect.w ? new_left + static_cast<int>(*rect.w) : new_left + old_w;
        const int new_bottom = rect.h ? new_top + static_cast<int>(*rect.h) : new_top + old_h;
        POINT move_top_left{origin.x + new_left, origin.y + new_top};
        POINT move_bottom_right{origin.x + new_right, origin.y + new_bottom};
        const HWND parent = GetParent(control);
        if (parent) {
          ScreenToClient(parent, &move_top_left);
          ScreenToClient(parent, &move_bottom_right);
        }
        const POINT top_left = move_top_left;
        const POINT bottom_right = move_bottom_right;
        if (!MoveWindow(control, top_left.x, top_left.y, bottom_right.x - top_left.x,
                        bottom_right.y - top_left.y, TRUE)) {
          result = {Code::ExecutionFailed, "control did not move"};
          return;
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_set_enabled(const std::uint64_t id, const bool enabled,
                                                     const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control enable set timed out before dispatch"};
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
        EnableWindow(control, enabled ? TRUE : FALSE);
        // AHK rule: verify, report failure instead of assuming it worked.
        if ((IsWindowEnabled(control) != FALSE) != enabled) {
          result = {Code::ExecutionFailed, "control did not change enabled state"};
          return;
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_tab_index(const std::uint64_t id, int& index1,
                                                   const std::chrono::milliseconds timeout) {  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control tab index read timed out before dispatch"};
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
        DWORD_PTR current = 0;
        // Button-style tabs track focus, not selection (TCM_GETCURSEL stays
        // -1): read back what select() writes (TCM_SETCURFOCUS).
        const LONG_PTR style = GetWindowLongPtrW(control, GWL_STYLE);
        const UINT message =
            ((style & TCS_BUTTONS) != 0) ? TCM_GETCURFOCUS : TCM_GETCURSEL;
        if (!send2(control, message, 0, 0, current)) {
          result = {Code::ExecutionFailed, "control tab index read failed"};
          return;
        }
        index1 = static_cast<int>(current) + 1;
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_class_nn(const std::uint64_t id, std::string& out,
                                                  const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control ClassNN read timed out before dispatch"};
  }
  out.clear();
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
        // Number the control among its parent's children with the shared
        // counter, so the answer agrees with controls() by construction.
        const HWND parent = GetParent(control);
        if (!parent) {
          result = {Code::InvalidContract, "control has no parent to number against"};
          return;
        }
        // The shared counter is append-only per enumeration; recount with a
        // fresh counter and pick the target's number.
        ClassNNCounter counts;
        bool found = false;
        std::string name;
        struct PickState {
          ClassNNCounter* counts;
          HWND target;
          std::string* name;
          bool* found;
        } pick{&counts, control, &name, &found};
        EnumChildWindows(
            parent,
            [](HWND child, LPARAM parameter) -> BOOL {
              auto* pick_state = reinterpret_cast<PickState*>(parameter);
              const ClassNNInstance instance = next_class_nn(*pick_state->counts, child);
              if (child == pick_state->target && instance.number != 0) {
                *pick_state->name =
                    to_utf8(instance.class_name) + std::to_string(instance.number);
                *pick_state->found = true;
                return FALSE;
              }
              return TRUE;
            },
            reinterpret_cast<LPARAM>(&pick));
        if (!found) {
          result = {Code::ExecutionFailed, "control has no ClassNN (nameless or capped)"};
          return;
        }
        out = name;
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_get_style(const std::uint64_t id, std::uint32_t& out,
                                                   const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control style read timed out before dispatch"};
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
        out = static_cast<std::uint32_t>(GetWindowLongPtrW(control, GWL_STYLE));
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_get_ex_style(const std::uint64_t id, std::uint32_t& out,
                                                      const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control ex-style read timed out before dispatch"};
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
        out = static_cast<std::uint32_t>(GetWindowLongPtrW(control, GWL_EXSTYLE));
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_focused_child(const std::uint64_t window_id,
                                                       std::uint64_t& out,
                                                       const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control focus read timed out before dispatch"};
  }
  out = 0;
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
        // AHK ControlGetFocus rule: the focused window of the window's own
        // thread, verified to be a child - 0 when nothing inside qualifies
        // (console windows have no input queue and read 0 the same way).
        DWORD thread = GetWindowThreadProcessId(window, nullptr);
        if (thread == 0) return;
        GUITHREADINFO info{};
        info.cbSize = sizeof(info);
        if (!GetGUIThreadInfo(thread, &info) || !info.hwndFocus) return;
        if (!IsChild(window, info.hwndFocus)) return;
        out = registry().id_for(info.hwndFocus);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_set_dropped(const std::uint64_t id, const bool dropped,
                                                     const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control drop-down set timed out before dispatch"};
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
        DWORD_PTR ignored = 0;
        if (!send2(control, CB_SHOWDROPDOWN, dropped ? TRUE : FALSE, 0, ignored)) {
          result = {Code::ExecutionFailed, "control did not change drop-down state"};
          return;
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_set_style(const std::uint64_t id, const bool extended,
                                                   const char op, const std::uint32_t bits,
                                                   const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control style set timed out before dispatch"};
  }
  if (op != '+' && op != '-' && op != '^' && op != '=') {
    return {Code::InvalidContract, "control style op must be +, -, ^ or ="};
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
        const int index = extended ? GWL_EXSTYLE : GWL_STYLE;
        const LONG_PTR current = GetWindowLongPtrW(control, index);
        LONG_PTR next = current;
        if (op == '+') {
          next = current | bits;
        } else if (op == '-') {
          next = current & ~static_cast<LONG_PTR>(bits);
        } else if (op == '^') {
          next = current ^ bits;
        } else {
          next = bits;
        }
        SetWindowLongPtrW(control, index, next);
        // AHK rule: re-read and report partial success as failure.
        if (GetWindowLongPtrW(control, index) != next) {
          result = {Code::ExecutionFailed, "control style change did not stick"};
          return;
        }
        InvalidateRect(control, nullptr, TRUE);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_send_keys(
    const std::uint64_t id, const std::vector<ControlKeyStep>& steps,
    const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control send timed out before dispatch"};
  }
  if (steps.empty() || steps.size() > 10'000) {
    return {Code::InvalidContract, "control send needs 1..10000 steps"};
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
        for (const auto& step : steps) {
          bool posted = false;
          if (step.is_char) {
            posted = PostMessageW(control, WM_CHAR, static_cast<WPARAM>(step.ch), 0) != FALSE;
          } else {
            const UINT scan = MapVirtualKeyW(step.vk, MAPVK_VK_TO_VSC);
            const LPARAM param = step.down
                                     ? static_cast<LPARAM>((scan << 16) | 1)
                                     : static_cast<LPARAM>((scan << 16) | 0xC0000001);
            posted = PostMessageW(control, step.down ? WM_KEYDOWN : WM_KEYUP,
                                  static_cast<WPARAM>(step.vk), param) != FALSE;
          }
          if (!posted) {
            result = {Code::ExecutionFailed, "control did not accept the keystroke"};
            return;
          }
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

// ---- ListView / StatusBar (cross-process reads) ---------------------------
// Text-bearing messages carry a caller buffer pointer, which is meaningless
// across processes (AHK allocates remote memory for exactly this reason).
// Same-process targets take the stack-buffer fast path; foreign targets go
// through a VirtualAllocEx round-trip. Either way the caller sees text.

namespace {

struct RemoteBuffer {
  HANDLE process{nullptr};
  LPVOID remote{nullptr};

  RemoteBuffer() = default;
  RemoteBuffer(const RemoteBuffer&) = delete;
  RemoteBuffer& operator=(const RemoteBuffer&) = delete;
  ~RemoteBuffer() {
    if (remote != nullptr && process != nullptr) {
      (void)VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    }
    if (process != nullptr) CloseHandle(process);
  }

  bool open(DWORD pid, SIZE_T bytes) {
    process = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE,
                          pid);
    if (process == nullptr) return false;
    remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return remote != nullptr;
  }
};

bool process_of(HWND window, DWORD& pid) {
  pid = 0;
  return GetWindowThreadProcessId(window, &pid) != 0 && pid != 0;
}

}  // namespace

rime::core::Error WindowService::control_listview_count(const std::uint64_t id, int& rows,
                                                        const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control listview count timed out before dispatch"};
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
        DWORD_PTR count = 0;
        if (!send2(control, LVM_GETITEMCOUNT, 0, 0, count)) {
          result = {Code::ExecutionFailed, "control item count failed"};
          return;
        }
        rows = static_cast<int>(count);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_listview_columns(const std::uint64_t id, int& cols,
                                                          const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control listview columns timed out before dispatch"};
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
        DWORD_PTR header_raw = 0;
        if (!send2(control, LVM_GETHEADER, 0, 0, header_raw)) {
          result = {Code::ExecutionFailed, "control header read failed"};
          return;
        }
        const HWND header = reinterpret_cast<HWND>(header_raw);
        if (!header) {
          cols = 1;  // No header: single implicit column (AHK rule treats it so).
          return;
        }
        DWORD_PTR items = 0;
        if (!send2(header, HDM_GETITEMCOUNT, 0, 0, items)) {
          result = {Code::ExecutionFailed, "control column count failed"};
          return;
        }
        cols = static_cast<int>(items);
        if (cols < 1) cols = 1;
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::control_listview_text(const std::uint64_t id, const int row1,
                                                       const int col1, std::string& out,
                                                       const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control listview text timed out before dispatch"};
  }
  if (row1 < 1 || col1 < 1) {
    return {Code::InvalidContract, "control listview row/col start at 1"};
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
        constexpr SIZE_T kChars = 512;
        wchar_t local[512] = {0};
        DWORD pid = 0;
        if (!process_of(control, pid)) {
          result = {Code::ExecutionFailed, "control process is unreadable"};
          return;
        }
        const bool lineal = pid == GetCurrentProcessId();
        LVITEMW item{};
        item.iSubItem = col1 - 1;
        item.cchTextMax = static_cast<int>(kChars);
        wchar_t* text_ptr = local;
        RemoteBuffer remote;
        LPVOID remote_item = nullptr;
        if (!lineal) {
          if (!remote.open(pid, sizeof(LVITEMW) + kChars * sizeof(wchar_t))) {
            result = {Code::ExecutionFailed, "control process memory is unreachable"};
            return;
          }
          remote_item = remote.remote;
          text_ptr = reinterpret_cast<wchar_t*>(static_cast<char*>(remote.remote) +
                                                       sizeof(LVITEMW));
          item.pszText = text_ptr;
          SIZE_T written = 0;
          LVITEMW setup = item;
          if (!WriteProcessMemory(remote.process, remote_item, &setup, sizeof(setup), &written) ||
              written != sizeof(setup)) {
            result = {Code::ExecutionFailed, "control process write failed"};
            return;
          }
        } else {
          item.pszText = local;
        }
        DWORD_PTR answered = 0;
        const LPARAM target = lineal ? reinterpret_cast<LPARAM>(&item)
                                     : reinterpret_cast<LPARAM>(remote_item);
        if (!send5(control, LVM_GETITEMTEXTW, static_cast<WPARAM>(row1 - 1), target, answered)) {
          result = {Code::ExecutionFailed, "control item text failed"};
          return;
        }
        if (!lineal) {
          SIZE_T read = 0;
          if (!ReadProcessMemory(remote.process, text_ptr, local, kChars * sizeof(wchar_t),
                                 &read) ||
              read == 0) {
            result = {Code::ExecutionFailed, "control process read failed"};
            return;
          }
        }
        wide = local;
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = to_utf8(wide);
  return Error::none();
}

rime::core::Error WindowService::control_statusbar_text(const std::uint64_t id, const int part1,
                                                        std::string& out,
                                                        const std::chrono::milliseconds timeout) {
  if (past_deadline(timeout)) {
    return {Code::InvalidContract, "control statusbar text timed out before dispatch"};
  }
  if (part1 < 1) {
    return {Code::InvalidContract, "control statusbar part starts at 1"};
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
        DWORD_PTR parts = 0;
        if (!send2(control, SB_GETPARTS, 0, 0, parts) || parts == 0) {
          result = {Code::ExecutionFailed, "control has no statusbar parts"};
          return;
        }
        if (part1 > static_cast<int>(parts)) {
          result = {Code::InvalidContract, "control statusbar part is past the last part"};
          return;
        }
        constexpr SIZE_T kChars = 512;
        wchar_t local[512] = {0};
        DWORD pid = 0;
        if (!process_of(control, pid)) {
          result = {Code::ExecutionFailed, "control process is unreadable"};
          return;
        }
        const bool lineal = pid == GetCurrentProcessId();
        wchar_t* text_ptr = local;
        RemoteBuffer remote;
        if (!lineal) {
          if (!remote.open(pid, kChars * sizeof(wchar_t))) {
            result = {Code::ExecutionFailed, "control process memory is unreachable"};
            return;
          }
          text_ptr = static_cast<wchar_t*>(remote.remote);
        }
        DWORD_PTR got = 0;
        if (!send5(control, SB_GETTEXTW, static_cast<WPARAM>(part1 - 1),
                   reinterpret_cast<LPARAM>(text_ptr), got)) {
          result = {Code::ExecutionFailed, "control statusbar text failed"};
          return;
        }
        if (!lineal) {
          SIZE_T read = 0;
          if (!ReadProcessMemory(remote.process, text_ptr, local, kChars * sizeof(wchar_t),
                                 &read) ||
              read == 0) {
            result = {Code::ExecutionFailed, "control process read failed"};
            return;
          }
        }
        wide = local;
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = to_utf8(wide);
  return Error::none();
}

}  // namespace rime::win32
