#pragma once

#include <windows.h>

namespace rime::win32 {

// Which rung of the activation ladder actually moved the window to the
// foreground. The verdict is returned rather than folded into a bool so a
// caller that owns a trace (AHK-TS-WINDOWS-API-DESIGN.md requires one entry
// per attempt) can report *how* the foreground was won instead of only that
// it was, and so a failure can say which rungs were exhausted.
enum class ForegroundRun {
  // It was already the foreground window.
  Already,
  // The first SetForegroundWindow took.
  Direct,
  // Needed AttachThreadInput to the current foreground thread's input queue.
  AttachedInput,
  // Needed the bare Alt tap that makes this process the last input owner.
  AltTap,
  // Every rung was refused: the foreground lock still holds.
  Denied,
};

// Moves `window` to the foreground following the ladder
// docs/AHK-TS-WINDOWS-API-DESIGN.md prescribes: SetForegroundWindow, then
// AttachThreadInput to the foreground thread, then the bare Alt tap AHK's
// WinActivate uses. Restoring an iconic window is the caller's job, because
// it is also the caller's z-order decision.
//
// The verdict is read back from GetForegroundWindow(): SetForegroundWindow
// can report failure while the window did in fact come forward, and trusting
// its return value is what made the retry loops in the tests spin for their
// whole deadline.
//
// Everything here runs on the UI thread. The input queues are attached only
// for the duration of the two calls below and detached on every path, because
// a queue left attached keeps two threads sharing input state.
[[nodiscard]] ForegroundRun acquire_foreground(HWND window);

}  // namespace rime::win32
