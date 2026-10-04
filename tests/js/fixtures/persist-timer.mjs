import { runtime } from "rime:runtime";

// Residency from a plain delay timer: settle(5000) cannot pass while this
// delay is armed (Host::idle watches delay timers), so bootstrap only gets
// here if it accepts "long pending delay" as a settle explanation and falls
// into the residency pump - the 5600 vs 5000 margin is the same timeout
// the old code reported as "QuickJS script did not settle". runtime.delay
// itself cannot be the guard: it blocks settle before the main check is
// ever reached.
globalThis.__rim_failure = "settle did not tolerate the long timer";
runtime.delay(5600, null).then(() => {
  globalThis.__rim_failure = undefined;
  runtime.exit(0);
});
