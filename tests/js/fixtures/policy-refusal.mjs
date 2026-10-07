// Refusal contract for the `unsupported-by-policy` rows (M8's rejection
// tests, AGENTS plan item 3). Three ledgers name the same policy: 25 rows in
// docs/api/core-builtins.json (stdlib.md §5 blacklist), 11 in
// docs/api/coverage.json, and 11 language-core members in
// docs/api/objects.json (Array.Capacity, Buffer.Ptr, ComObject.__Item/
// __Value/Ptr, Func arity/reflection, Object.__Ref - stdlib.md
// §5.1/§5.4/§5.7 and native-interop.md). No name is claimed by both ledgers;
// the coverage
// rows Critical/Pause/PostMessage/Thread are the ones §2.4's table lists as
// COV, while Callback*, Obj*DataPtr* and SendMessage are coverage-only and
// are documented in runtime-language.md §2.4 and window.md respectively.
// The objects ledger's member names are not globals or module methods, but
// the raw-handle property names they carry (Ptr, and the Handle/Hwnd family
// design-review.md:64 promises never surface) are asserted absent at the
// entry-point level too, so a leak cannot hide behind casing.
// The claim each row makes is that Rime exposes no entry point for it - so
// the fixture walks every registered module plus the global object and fails
// if any of those names resolves, matched case-insensitively on the API name
// so an alias like `dllCall` cannot pass by changing casing.
//
// The second half pins the alternative the row documents instead of the
// function: §2.4 names `input.suspend` for Pause, the structured `windows.*`
// write family for PostMessage/SendMessage, `sound.get*`/`set*` for
// SoundGetInterface, `automation.find` for ComObjGet/ComObjActive,
// `input.subscribe` for ComObjConnect (event sinks), `control.resolve` for
// ControlGetHwnd (stable ControlId, never a raw HWND), and Uint8Array for
// the address-shaped string and number buffers (this runtime has no
// TextDecoder/TextEncoder - see the probe note below). Rows whose
// alternative is deliberately
// script-invisible (naked COM/DLL interop -> isolated plugin, reference
// counting -> GC, `Critical`/`Thread` -> kernel SchedulerPolicy, QI/variant
// unpacking -> the same isolated COM face) assert
// absence only: there is no JS surface to require.
//
// Run through `rime_js_bundle --production`, so the module set is the
// production wiring and a capability grant can never be what the assertions
// observe.

import { runtime } from "rime:runtime";
import { windows, groups, settings } from "rime:window";
import { input } from "rime:input";
import { process } from "rime:process";
import { clipboard } from "rime:clipboard";
import { screen } from "rime:screen";
import { automation } from "rime:automation";
import { control } from "rime:control";
import { sound } from "rime:sound";
import { storage } from "rime:storage";
import { registry } from "rime:registry";
import { ui } from "rime:ui";

const refused = [
  // core-builtins + coverage (stdlib.md §5)
  "CallbackCreate",
  "CallbackFree",
  "ComCall",
  "ComObjActive",
  "ComObjConnect",
  "ComObjFlags",
  "ComObjFromPtr",
  "ComObjGet",
  "ComObjQuery",
  "ComObjType",
  "ComObjValue",
  "ControlGetHwnd",
  "Critical",
  "DllCall",
  "NumGet",
  "NumPut",
  "ObjAddRef",
  "ObjFromPtr",
  "ObjFromPtrAddRef",
  "ObjGetDataPtr",
  "ObjGetDataSize",
  "ObjGetCapacity",
  "ObjPtr",
  "ObjPtrAddRef",
  "ObjRelease",
  "ObjSetCapacity",
  "ObjSetDataPtr",
  "Pause",
  "PostMessage",
  "SendMessage",
  "SoundGetInterface",
  "StrGet",
  "StrPtr",
  "StrPut",
  "Thread",
  "VarSetStrCapacity",
  // objects ledger + design-review.md:64: raw-handle property names (the
  // Buffer.Ptr / ComObject.Ptr member rows are unsupported-by-policy; a
  // Handle or Hwnd own key on any reachable object would break the branded
  // id promise), asserted at the entry-point level with the same
  // case-insensitive match.
  "Handle",
  "Hwnd",
  "Ptr",
];

const surfaces = {
  runtime,
  windows,
  groups,
  settings,
  input,
  process,
  clipboard,
  screen,
  automation,
  control,
  sound,
  storage,
  registry,
  ui,
};

// Every name a script can reach: globals, module bindings, and the methods
// those bindings carry. The API name is the last path segment, which is the
// segment an AHK migration would look for.
const exposed = [];
const collect = (label, value) => {
  exposed.push(label);
  if (value !== null && (typeof value === "object" || typeof value === "function")) {
    for (const key of Object.keys(value)) exposed.push(`${label}.${key}`);
  }
};
for (const key of Object.keys(globalThis)) collect(`globalThis.${key}`, globalThis[key]);
for (const [module, binding] of Object.entries(surfaces)) collect(module, binding);

const byApiName = new Map();
for (const path of exposed) {
  const api = path.split(".").pop().toLowerCase();
  if (!byApiName.has(api)) byApiName.set(api, path);
}

const leaks = refused.filter((name) => byApiName.has(name.toLowerCase()));
if (leaks.length !== 0) {
  throw new Error(
    `unsupported-by-policy entry points are reachable: ${leaks
      .map((name) => `${name} <- ${byApiName.get(name.toLowerCase())}`)
      .join("; ")}`,
  );
}

// The documented replacement must be the thing that exists instead.
// TypedArray is the binary alternative the rows name: this QuickJS build
// ships neither TextDecoder nor TextEncoder (probed: both `typeof` are
// `undefined` while Uint8Array/ArrayBuffer are functions), so docs that
// promised those two promised a global the runtime does not have.
const alternatives = [
  ["Pause", "input.suspend", () => typeof input.suspend === "function"],
  ["PostMessage", "windows.focus", () => typeof windows.focus === "function"],
  ["SendMessage", "windows.close", () => typeof windows.close === "function"],
  ["SoundGetInterface", "sound.getVolume", () => typeof sound.getVolume === "function"],
  ["SoundGetInterface", "sound.setVolume", () => typeof sound.setVolume === "function"],
  ["NumGet", "Uint8Array", () => typeof Uint8Array === "function"],
  ["NumPut", "Uint8Array", () => typeof Uint8Array === "function"],
  ["StrGet", "Uint8Array", () => typeof Uint8Array === "function"],
  ["StrPut", "Uint8Array", () => typeof Uint8Array === "function"],
  // M8 COM 定档: script-visible side of the boundary. The COM face is the
  // isolated plugin, but the two things a script reaches for instead -
  // "find an object somewhere" and "subscribe to its events" - are already
  // capabilities of the public API, so their absence is not a dead end.
  ["ComObjGet", "automation.find", () => typeof automation.find === "function"],
  ["ComObjActive", "automation.find", () => typeof automation.find === "function"],
  ["ComObjConnect", "input.subscribe", () => typeof input.subscribe === "function"],
  // M8 Control 定档: the id, not the handle.
  ["ControlGetHwnd", "control.resolve", () => typeof control.resolve === "function"],
];
for (const [name, alternative, present] of alternatives) {
  if (!present()) {
    throw new Error(`${name} was refused but its documented alternative ${alternative} is missing`);
  }
}

// The policy itself is inspectable: SchedulerPolicy is a runtime surface,
// not a script thread.
if (typeof input.policy !== "function" || typeof input.setTimer !== "function") {
  throw new Error("Critical/Thread alternatives (input.policy, input.setTimer) are missing");
}
