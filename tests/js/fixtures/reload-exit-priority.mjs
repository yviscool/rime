// Realism: L6 - executed by quickjs_reload_exit_priority (rime_js_bundle,
// the production host wiring).
import { runtime } from "rime:runtime";
import { input } from "rime:input";

// Pass 0 asks for the reload; the teardown handler must observe reason
// "reload" and its exit must outrank the pending reload, so the process comes
// back with 4 and never reaches a second pass - pass 1 would hit the count
// guard below and exit 9 instead. Any other reason (for example "stop")
// exits 6, so the asserted reason is the exact payload the handler saw.
if (runtime.reloadState().count > 0) {
  runtime.exit(9);
}
input.onExit((event) => {
  runtime.exit(event.reason === "reload" ? 4 : 6);
});
runtime.reload();
throw new Error("unreachable");
