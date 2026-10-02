#pragma once

// Shared test-process setup: every native test entry point must fail
// non-interactively. A failed assert() ends in abort(), which by default
// raises the Visual C++ "abort() has been called" dialog (or the WER
// Retry/Ignore box) and blocks ctest until a human clicks it - useless in
// CI and hostile on a dev box. With the handlers below, every failure mode
// lands on stderr with a nonzero exit code and nothing pops up.

#ifdef _MSC_VER

#include <crtdbg.h>
#include <stdlib.h>

#include <cstdlib>
#include <windows.h>

namespace rime::test {

inline void keep_failures_non_interactive() {
  // abort(): keep the stderr message, drop the WER/Retry dialog.
  _set_abort_behavior(_WRITE_ABORT_MSG, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
  // CRT assert/heap diagnostics: stderr instead of a message box.
  _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
  // Runtime error popups (pure virtual call, invalid parameter, crash
  // errors): terminate via quiet abort() instead of showing UI.
  _set_purecall_handler([]() { std::abort(); });
  _set_invalid_parameter_handler(
      [](const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
        std::abort();
      });
  SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
               SEM_NOOPENFILEERRORBOX);
}

}  // namespace rime::test

#else  // !_MSC_VER

namespace rime::test {
inline void keep_failures_non_interactive() {}
}  // namespace rime::test

#endif
