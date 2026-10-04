import { runtime } from "rime:runtime";
// Pending work the exit abandons: it keeps the runtime non-idle for 60s, so
// settle can only return by noticing the exit below, never by quiescence.
void runtime.delay(60000, null).then(() => { throw new Error("abandoned"); });
// Exit from the async turn: the module evaluates cleanly, so bootstrap has
// to settle first and drop the request it observes while waiting.
void runtime.delay(50, null).then(() => { runtime.exit(3); });
