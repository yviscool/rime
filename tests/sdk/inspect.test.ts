import { expect, test } from "bun:test";
import { parseInspection } from "../../sdk/src/inspect";

const full = {
  modules: { native: ["rime:runtime", "rime:window"], files: ["main.mjs"], fileRoot: "/" },
  functions: ["ping"],
  subscriptions: ["js:1", "cancellation:2"],
  tasks: { async: 0, queued: 0, timers: 0, callbacks: 1 },
  errors: [{ where: "rime:input.chord", message: "queue pump failed" }],
};

test("parses the kind-all inspection shape", () => {
  const parsed = parseInspection(JSON.stringify(full));
  expect(parsed.modules.native).toEqual(["rime:runtime", "rime:window"]);
  expect(parsed.modules.fileRoot).toBe("/");
  expect(parsed.functions).toEqual(["ping"]);
  expect(parsed.subscriptions).toEqual(["js:1", "cancellation:2"]);
  expect(parsed.tasks).toEqual({ async: 0, queued: 0, timers: 0, callbacks: 1 });
  expect(parsed.errors).toEqual([{ where: "rime:input.chord", message: "queue pump failed" }]);
});

test("tolerates unknown fields for forward compatibility", () => {
  const parsed = parseInspection(JSON.stringify({ ...full, futureSection: { x: 1 } }));
  expect(parsed.tasks.callbacks).toBe(1);
});

test("rejects non-JSON and non-object output", () => {
  expect(() => parseInspection("not json")).toThrow(TypeError);
  expect(() => parseInspection("[]")).toThrow("must be an object");
});

test("rejects missing or ill-typed sections with field-named TypeErrors", () => {
  expect(() => parseInspection("{}")).toThrow("inspection.modules");
  expect(() =>
    parseInspection(JSON.stringify({ ...full, tasks: "0" })),
  ).toThrow("inspection.tasks");
  expect(() =>
    parseInspection(JSON.stringify({ ...full, tasks: { ...full.tasks, async: "0" } })),
  ).toThrow("inspection.tasks.async");
  expect(() =>
    parseInspection(JSON.stringify({ ...full, subscriptions: [1] })),
  ).toThrow("inspection.subscriptions");
  expect(() =>
    parseInspection(JSON.stringify({ ...full, modules: { ...full.modules, files: "main.mjs" } })),
  ).toThrow("inspection.modules.files");
  expect(() =>
    parseInspection(JSON.stringify({ ...full, errors: [{ where: "x" }] })),
  ).toThrow("inspection.errors[0]");
});
