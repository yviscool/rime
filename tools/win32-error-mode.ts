import { dlopen, FFIType } from "bun:ffi";

// Loader and CRT error dialogs are modal: a broken test (missing DLL, failed
// assertion) would otherwise pop a message box and block ctest until someone
// clicks it. Error mode is inherited by every child process, so setting it
// once in the runner makes ctest and the test binaries fail fast on stderr.
const SEM_FAILCRITICALERRORS = 0x0001;
const SEM_NOGPFAULTERRORBOX = 0x0002;
const SEM_NOOPENFILEERRORBOX = 0x8000;

/** Suppress Windows error dialogs for this process tree. Best effort. */
export function suppressWindowsErrorDialogs(): void {
  if (process.platform !== "win32") return;
  try {
    const kernel32 = dlopen("kernel32.dll", {
      SetErrorMode: { args: [FFIType.u32], returns: FFIType.u32 },
    });
    kernel32.symbols.SetErrorMode(
      SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX,
    );
  } catch {
    // Non-fatal: without FFI the tests still run, they just keep dialogs.
  }
}
