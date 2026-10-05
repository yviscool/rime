// Realism: L6 - executed by quickjs_reload_slice (production host loop).
import { runtime } from "rime:runtime";
// Unconditional reload: the embedder's reload ceiling must stop this with a
// clear diagnostic instead of looping the host forever on the same file.
runtime.reload();
