import { runtime } from "rime:runtime";
import { input } from "rime:input";

// Residency comes from the live hotkey registration (events probe > 0)
// until the tick below closes it; without the pump the main check sees the
// flag and exits 1 before the tick ever runs. setTimer tick instead of the
// runtime.delay in the sketch: Host::idle watches delay timers, so a delay
// would block settle itself rather than fail past it.
globalThis.__rim_failure = "hotkey residency did not pump";
const hotkey = input.hotkey("f24", () => {});
input.setTimer(() => {
  hotkey.close();
  globalThis.__rim_failure = undefined;
  runtime.exit(0);
}, -80);
