// Realism: L6 - the production rime_js_bundle (--production) wiring must
// grant ui.create: the native rime:ui module answers a real call (an empty
// ToolTip is AHK's no-op clear, so nothing is drawn) instead of rejecting
// with capability_denied. A bundle under the demo grant would deny it, so
// this fixture only passes where production_capabilities() really carries
// the capability the registry declares.

import { ui } from "rime:ui";

const outcome = await ui.toolTip("").then(
  (value) => ({ resolved: value }),
  (error) => ({ code: error.code, message: error.message }),
);

if (outcome.code !== undefined) {
  throw new Error(
    `ui.create must be granted in production: ${outcome.code}: ${outcome.message}`,
  );
}
if (outcome.resolved !== null) {
  throw new Error(`toolTip("") must resolve null: ${JSON.stringify(outcome.resolved)}`);
}
