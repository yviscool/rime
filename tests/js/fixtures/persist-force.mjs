import { runtime } from "rime:runtime";
import { input } from "rime:input";

// Residency guard: without the bootstrap pump the main check runs while
// this flag is still set and exits 1, so an early normal return cannot pass
// for success. The clearing action is a setTimer tick - Host::idle does not
// watch events timers, so settle returns while the tick is still armed,
// which is exactly the case the pump has to catch; a runtime.delay would
// instead block settle itself.
globalThis.__rim_failure = "force residency did not pump";
runtime.persistent(true);
input.setTimer(() => {
  // The once-tick removes its own registration right after this callback
  // returns, so clearing the force flag here drops residency to zero and
  // the pump has to notice and stop - that is the assertion, not this line.
  runtime.persistent(false);
  globalThis.__rim_failure = undefined;
}, -60);
// Bounded residency: if the force flag or the probe stays set after the
// clear, this once-timer fires with the registration closed and exits 1
// instead of leaving the pump resident forever.
const watchdog = input.setTimer(() => {
  watchdog.close();
  if (runtime.persistent()) {
    globalThis.__rim_failure = "residency did not drop after clear";
    runtime.exit(1);
  }
}, -400);
