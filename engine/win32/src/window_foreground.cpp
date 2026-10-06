#include "window_foreground.hpp"

namespace rime::win32 {
namespace {

// AHK's WinActivate taps bare Alt when SetForegroundWindow is refused: the
// key event makes this process the owner of the last input event, which is
// one of the conditions Windows grants the foreground on. It is injected
// input, so it is the last rung, not the first.
void tap_bare_alt() {
  INPUT tap[2] = {};
  tap[0].type = INPUT_KEYBOARD;
  tap[0].ki.wVk = VK_MENU;
  tap[1].type = INPUT_KEYBOARD;
  tap[1].ki.wVk = VK_MENU;
  tap[1].ki.dwFlags = KEYEVENTF_KEYUP;
  static_cast<void>(SendInput(2, tap, sizeof(INPUT)));
}

}  // namespace

ForegroundRun acquire_foreground(const HWND window) {
  if (window == nullptr) return ForegroundRun::Denied;
  if (GetForegroundWindow() == window) return ForegroundRun::Already;

  if (SetForegroundWindow(window) != FALSE && GetForegroundWindow() == window) {
    return ForegroundRun::Direct;
  }

  const DWORD self = GetCurrentThreadId();
  const HWND foreground = GetForegroundWindow();
  const DWORD owner = foreground != nullptr ? GetWindowThreadProcessId(foreground, nullptr) : 0;
  const bool attached =
      owner != 0 && owner != self && AttachThreadInput(self, owner, TRUE) != FALSE;
  static_cast<void>(SetForegroundWindow(window));
  const bool won_while_attached = GetForegroundWindow() == window;
  if (attached) static_cast<void>(AttachThreadInput(self, owner, FALSE));
  if (won_while_attached) return ForegroundRun::AttachedInput;

  tap_bare_alt();
  static_cast<void>(SetForegroundWindow(window));
  if (GetForegroundWindow() == window) return ForegroundRun::AltTap;
  return ForegroundRun::Denied;
}

}  // namespace rime::win32
