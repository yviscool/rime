// Realism: L6 - executed by quickjs_reload_slice (production host loop plus
// a real on-disk replacement).
import { clipboard } from "rime:clipboard";
import { runtime } from "rime:runtime";

// First pass of the file-change test: this content (V1) must be the one the
// host read. The count guard fails the test (exit 11) if the host ever runs
// V1 again after the reload, which would mean the file on disk had not been
// replaced yet. The clipboard is the only cross-thread channel a script has:
// "rime-reload-ready" tells the harness V1 is running, "rime-reload-go"
// tells it the replacement is in place - both conditions, polled with a
// bounded wait, never a sleep.
if (runtime.reloadState().count > 0) {
  runtime.exit(11);
}
let sawGo = false;
let handshakeFailure = "";
try {
  await clipboard.write("rime-reload-ready");
  for (let attempt = 0; attempt < 500; attempt += 1) {
    const { text } = await clipboard.read();
    if (text === "rime-reload-go") {
      sawGo = true;
      break;
    }
    await runtime.delay(20, null);
  }
} catch (error) {
  handshakeFailure = String(error);
}
if (handshakeFailure) {
  globalThis.__rim_failure = handshakeFailure;
  runtime.exit(14);
}
if (!sawGo) {
  runtime.exit(13);
}
runtime.reload();
throw new Error("unreachable");
