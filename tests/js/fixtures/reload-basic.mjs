// Realism: L6 - executed by quickjs_reload_basic (rime_js_bundle, the
// production host wiring).
import { runtime } from "rime:runtime";
// Pass 0 reloads and never returns (runtime.reload always throws); pass 1
// sees count 1 and exits 5, so the expected exit code proves the script file
// was re-read and evaluated a second time in a fresh runtime.
const state = runtime.reloadState();
if (state.count === 0) {
  runtime.reload();
}
runtime.exit(5);
throw new Error("unreachable");
