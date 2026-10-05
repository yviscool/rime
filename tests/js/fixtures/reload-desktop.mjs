// Realism: L6 - executed by quickjs_desktop_reload (rime_host, production
// capabilities).
import { runtime } from "rime:runtime";

// Production host (rime_host) must reload exactly once: pass 0 asks for it,
// pass 1 sees count 1 and exits 0. Every other count exits 9, and a reload
// request that never reaches the embedder unwinds without being recorded, so
// the host would return 1 instead of 0 - neither reads as success.
const state = runtime.reloadState();
if (state.count === 0) {
  runtime.reload();
}
if (state.count === 1) {
  runtime.exit(0);
}
runtime.exit(9);
