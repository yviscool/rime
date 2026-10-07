#pragma once

#include "rime/core/types.hpp"

#include <string>

namespace rime::win32 {

// Test-only fault injection seam for WindowService (same rules as
// input_seam/screen_seam: a stub may replace the environment - the OS
// outcomes these methods observe - never the unit under test). The healthy
// desktop rarely denies SetForegroundWindow on demand, expires a deadline
// mid-call, or destroys a window between test setup and the call, so a test
// arms those outcomes at the service entry while the whole real
// start()/query()/focus()/move()/close() path still runs when no fault is
// armed.
//
// Mutex-protected and armed per call: arm() installs one fault, consume()
// fires it for the matching method and counts down `times` (-1 = until
// clear()). ctest runs serially, so no concurrent consumer exists. Every
// armed fault must be cleared (clear() or exhaustion); a test that leaks an
// armed fault fails its neighbours. See window.cpp for the call sites.
namespace window_seam {

enum class FaultKind {
  // Foreground refused: ExecutionFailed, the same code and shape as the real
  // "denied by the foreground lock" refusal.
  Denied,
  // Deadline expired inside the call: Timeout, the same code as the real
  // expired-deadline path.
  Timeout,
  // Target resolved to nothing: TargetGone, the same code as a destroyed or
  // previous-generation window id.
  TargetGone,
};

struct Fault {
  FaultKind kind = FaultKind::Denied;
  // Method name the fault applies to ("query", "info", "focus", "move",
  // "move_rect", "close"); empty matches every hooked method.
  std::string method;
  // How many matching calls fail before the fault disarms itself;
  // -1 fails every matching call until clear().
  int times = 1;
};

// Installs a fault; false when one is already armed (clear it first).
bool arm(Fault fault);
// Disarms any fault. Idempotent.
void clear();
// Fires the armed fault for `method` when one matches: fills `out` and
// returns true (the caller returns `out` without touching the desktop).
// False when no fault is armed or the method filter does not match.
bool consume(const std::string& method, rime::core::Error& out);

}  // namespace window_seam

}  // namespace rime::win32
