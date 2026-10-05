// Realism: L6 - executed by quickjs_reload_slice (the reloaded pass after a
// real on-disk replacement).
import { runtime } from "rime:runtime";

// Second pass of the file-change test: this content (V2) only exists because
// the harness replaced the file while V1 was running, and it must only ever
// be evaluated after a reload (exit 12 if the replacement landed too early)
// - so exit 5 here proves the reload re-read the file from disk.
const state = runtime.reloadState();
if (state.count === 0) {
  runtime.exit(12);
}
runtime.exit(5);
