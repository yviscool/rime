#pragma once

#include <windows.h>

namespace rime::win32 {

// Test-only OS substitution seam (AGENTS testing rules: a stub may replace
// the environment - these two OS calls - never the unit under test). The
// healthy desktop rarely fails SetWindowsHookExW or partially consumes a
// SendInput batch, so a test injects those outcomes at the OS boundary while
// the whole real start()/send()/send_mouse() path still runs.
//
// Process-global and unsynchronized beyond the atomic store: set it before
// the operation and restore it (nullptr) right after. ctest runs serially,
// so no concurrent consumer exists. See input.cpp for the call sites.
namespace input_seam {

using HookInstaller = HHOOK(WINAPI*)(int, HOOKPROC, HINSTANCE, DWORD);
using SendInputFn = UINT(WINAPI*)(UINT, LPINPUT, int);

// nullptr restores the real API.
void set_hook_installer(HookInstaller installer);
void set_send_input(SendInputFn sender);

}  // namespace input_seam

}  // namespace rime::win32
