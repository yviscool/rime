// Realism: L6 - executed by quickjs_reload_exit_first (rime_js_bundle, the
// production host wiring).
import { runtime } from "rime:runtime";
import { input } from "rime:input";

// Exit first: the teardown handler runs while an exit is already pending and
// tries to revive the script with runtime.reload(). The request must be
// refused (exit outranks reload), so no second pass can start - pass 1 would
// be caught by the count guard and exit 9 - and the entry point keeps
// returning the originally requested 4.
if (runtime.reloadState().count > 0) {
  runtime.exit(9);
}
input.onExit(() => {
  try {
    runtime.reload();
  } catch {
    // Refusal error, as designed: the exit is already pending.
  }
});
runtime.exit(4);
throw new Error("unreachable");
