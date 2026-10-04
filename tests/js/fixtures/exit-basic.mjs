import { runtime } from "rime:runtime";
runtime.exit(7);
throw new Error("unreachable");
