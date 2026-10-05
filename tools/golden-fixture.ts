// Bun all-in-one: QuickJS consumer generator for the golden `failure` layer,
// run as `bun tools/golden-fixture.ts` (write) or `bun tools/golden-fixture.ts
// --check` (drift gate, also invoked by `bun run contract:smoke`).
//
// Every failure entry has to be classified as either expressible from the
// rime:* modules (executed: the generated script drives that call and asserts
// golden.code plus golden.messageContains) or skipped with a reason naming
// the layer that blocks it. The classification is static because the modules
// validate their arguments before an Action exists, so QuickJS cannot observe
// a payload it is never allowed to build: probing at runtime would either
// assert the wrong layer or touch the desktop.
//
// The table below and contracts/golden/*.json therefore must stay in step,
// and `--check` proves they are: it renders in memory and compares byte for
// byte with the checked-in fixture, so a golden edit, a renamed failure entry
// or a stale classification fails contract:smoke instead of drifting.
//
// Output: tests/js/fixtures/golden-violations.generated.mjs, executed by the
// ctest `quickjs_golden_failures` through `rime_js_bundle --production`, i.e.
// under production_capabilities(), so the capability policy is never what
// stops an entry from reaching its executor.
import { readFile, readdir, writeFile } from "node:fs/promises";
import { resolve } from "node:path";

type SkipReason = "module-guard" | "module-shape" | "module-target-fixed";

type Classification =
  | { kind: "execute"; via: string; call: string }
  | { kind: "skip"; via: string; reason: SkipReason; note: string };

interface GoldenFailure {
  name: string;
  payload: unknown;
  code: string;
  messageContains?: string;
  target?: { kind: string; id: string };
}

interface GoldenFile {
  type: string;
  failure: GoldenFailure[];
}

interface Row {
  type: string;
  name: string;
  code: string;
  messageContains?: string;
  classification: Classification;
}

interface Rendered {
  text: string;
  executed: number;
  skipped: number;
}

const root = resolve(import.meta.dir, "..");
const golden_dir = resolve(root, "contracts/golden");
const fixture_rel = "tests/js/fixtures/golden-violations.generated.mjs";
const fixture_path = resolve(root, fixture_rel);

// `via` names the exported binding the entry would have to go through. The
// renderer imports exactly those bindings, so a classification pointing at a
// module the fixture does not import (or at no module at all) fails here.
const runtime_module = { module: "rime:runtime", bindings: ["runtime"] };
const modules: ReadonlyArray<{ module: string; bindings: readonly string[] }> = [
  { module: "rime:window", bindings: ["windows", "groups"] },
  { module: "rime:input", bindings: ["input"] },
  { module: "rime:process", bindings: ["process"] },
  { module: "rime:clipboard", bindings: ["clipboard"] },
  { module: "rime:automation", bindings: ["automation"] },
];

// Classification of contracts/golden/<type>.json failure[<name>], keyed by
// "<type>/<name>". Every key and every golden failure entry must appear
// exactly once - render() checks both directions, so adding a golden entry
// without deciding what QuickJS can do with it fails the generator.
const classification: Record<string, Classification> = {
  "automation.invoke/target-kind-is-window": {
    kind: "skip",
    via: "automation",
    reason: "module-target-fixed",
    note:
      "automation.invoke(elementId) always builds target {kind: element}, so a " +
      "window target cannot be forged (engine/win32/js/src/automation_module.cpp " +
      "automation_invoke)",
  },
  "clipboard.write/text-not-a-string": {
    kind: "skip",
    via: "clipboard",
    reason: "module-guard",
    note:
      "clipboard.write(text) rejects a non-string first argument before any Action " +
      "exists (engine/win32/js/src/clipboard_module.cpp clipboard_write)",
  },
  "clipboard.write/target-id-is-not-default": {
    kind: "skip",
    via: "clipboard",
    reason: "module-target-fixed",
    note:
      "clipboard.write always targets {kind: clipboard, id: default} " +
      "(engine/win32/js/src/clipboard_module.cpp clipboard_write)",
  },
  "input.mouse/steps-not-an-array": {
    kind: "skip",
    via: "input",
    reason: "module-guard",
    note:
      "input.mouse({steps}) requires steps to be an array " +
      "(engine/win32/js/src/input_module.cpp input_mouse)",
  },
  "input.mouse/button-outside-1-2-3": {
    kind: "skip",
    via: "input",
    reason: "module-guard",
    note:
      "input.mouse validates every step button to 1, 2 or 3 before it builds an " +
      "Action (engine/win32/js/src/input_module.cpp parse_mouse_step)",
  },
  "input.send/payload-is-an-object": {
    kind: "skip",
    via: "input",
    reason: "module-guard",
    note:
      "input.send(steps) takes a step array, so the golden object wrapper is " +
      "rejected before it builds an Action (engine/win32/js/src/input_module.cpp " +
      "input_send)",
  },
  "input.send/target-kind-is-window": {
    kind: "skip",
    via: "input",
    reason: "module-target-fixed",
    note:
      "input.send always targets {kind: input, id: keyboard} " +
      "(engine/win32/js/src/input_module.cpp input_send)",
  },
  "process.launch/command-not-a-string": {
    kind: "skip",
    via: "process",
    reason: "module-guard",
    note:
      "process.launch(options) requires a non-empty string command " +
      "(engine/win32/js/src/process_module.cpp process_launch)",
  },
  "process.launch/args-not-a-string": {
    kind: "skip",
    via: "process",
    reason: "module-guard",
    note:
      "process.launch(options) requires args to be a string " +
      "(engine/win32/js/src/process_module.cpp process_launch)",
  },
  "process.terminate/target-id-is-not-a-pid": {
    kind: "skip",
    via: "process",
    reason: "module-target-fixed",
    note:
      "process.terminate(pid) takes a positive integer and builds target id from " +
      "it, so the id not-a-pid cannot be expressed " +
      "(engine/win32/js/src/process_module.cpp process_terminate)",
  },
  "window.close/target-kind-is-group": {
    kind: "skip",
    via: "windows",
    reason: "module-target-fixed",
    note:
      "windows.close(id) always targets {kind: window} " +
      "(engine/win32/js/src/window_module.cpp run_window_mutation)",
  },
  "window.focus/target-kind-is-group": {
    kind: "skip",
    via: "windows",
    reason: "module-target-fixed",
    note:
      "windows.focus(id) always targets {kind: window} " +
      "(engine/win32/js/src/window_module.cpp run_window_mutation)",
  },
  "window.group.add/empty-query-spec": {
    kind: "execute",
    via: "groups",
    // The empty spec passes parse_window_query (all fields optional) and
    // group_query_payload({}) stringifies to the golden payload {}, so the
    // executor's own query check is what must reject.
    call: 'groups.add("golden-empty-query", {})',
  },
  "window.kill/target-kind-is-group": {
    kind: "skip",
    via: "windows",
    reason: "module-target-fixed",
    note:
      "windows.kill(id) always targets {kind: window} " +
      "(engine/win32/js/src/window_module.cpp run_window_mutation)",
  },
  "window.move/position-and-rect-conflict": {
    kind: "skip",
    via: "windows",
    reason: "module-shape",
    note:
      "windows.move(target, positionOrRect) builds either position or rect, never " +
      "both, so the conflicting payload has no JS spelling " +
      "(engine/win32/js/src/window_module.cpp run_window_mutation)",
  },
  "window.move/rect-without-fields": {
    kind: "skip",
    via: "windows",
    reason: "module-guard",
    note:
      "windows.move requires the rect object to set at least one of x, y, w, h " +
      "(engine/win32/js/src/window_module.cpp run_window_mutation)",
  },
  "window.set.region/unknown-region-option": {
    kind: "skip",
    via: "windows",
    reason: "module-guard",
    note:
      "windows.setRegion parses the region string with parse_region_options before " +
      "it builds an Action (engine/win32/js/src/window_module.cpp run_window_set)",
  },
  "window.set.region/single-pair-is-not-a-polygon": {
    kind: "skip",
    via: "windows",
    reason: "module-guard",
    note:
      "windows.setRegion parses the region string with parse_region_options before " +
      "it builds an Action (engine/win32/js/src/window_module.cpp run_window_set)",
  },
  "window.set.title/title-not-a-string": {
    kind: "skip",
    via: "windows",
    reason: "module-guard",
    note:
      "windows.setTitle requires a string value " +
      "(engine/win32/js/src/window_module.cpp run_window_set)",
  },
  "window.set.transparent/value-out-of-range": {
    kind: "skip",
    via: "windows",
    reason: "module-guard",
    note:
      "windows.setTransparent requires value -1 or 0..255 " +
      "(engine/win32/js/src/window_module.cpp run_window_set)",
  },
  "window.zorder/placement-outside-enum": {
    kind: "skip",
    via: "windows",
    reason: "module-guard",
    note:
      "windows.zorder requires placement top or bottom " +
      "(engine/win32/js/src/window_module.cpp run_window_mutation)",
  },
};

const header = [
  "// GENERATED by tools/golden-fixture.ts - DO NOT EDIT.",
  "// Source: the `failure` array of every contracts/golden/*.json file.",
  "// Regenerate: bun tools/golden-fixture.ts",
  "// Verify:     bun tools/golden-fixture.ts --check (also runs in contract:smoke)",
  "//",
  "// QuickJS consumer of the golden failure layer. `executed` entries are driven",
  "// through the rime:* module that owns their action type and must reject with",
  "// the golden code (and messageContains when the golden file declares one);",
  "// `skipped` entries carry the reason no JS call can produce that rejection.",
  "// Each skipped entry still names the module binding that stands between JS",
  "// and the executor, so the rime:* imports below double as a registration check.",
  "// Run through `rime_js_bundle --production` (production_capabilities()), so a",
  "// capability denial can never be what the assertions observe.",
  "",
];

const is_known_binding = (binding: string): boolean =>
  runtime_module.bindings.includes(binding) || modules.some((entry) => entry.bindings.includes(binding));

async function collect_rows(): Promise<Row[]> {
  const files = (await readdir(golden_dir))
    .filter((name) => name.endsWith(".json"))
    .sort();
  const rows: Row[] = [];
  for (const file of files) {
    const golden = JSON.parse(await readFile(resolve(golden_dir, file), "utf8")) as GoldenFile;
    for (const entry of golden.failure ?? []) {
      const key = `${golden.type}/${entry.name}`;
      const decided = classification[key];
      if (!decided) {
        throw new Error(
          `golden failure entry ${key} has no classification in tools/golden-fixture.ts`,
        );
      }
      if (!is_known_binding(decided.via)) {
        throw new Error(`classification for ${key} names an unknown module binding ${decided.via}`);
      }
      rows.push({
        type: golden.type,
        name: entry.name,
        code: entry.code,
        messageContains: entry.messageContains,
        classification: decided,
      });
    }
  }
  const seen = new Set<string>();
  for (const key of Object.keys(classification)) {
    if (seen.has(key)) throw new Error(`duplicate classification key ${key}`);
    seen.add(key);
  }
  for (const row of rows) {
    const key = `${row.type}/${row.name}`;
    if (seen.delete(key) === false) {
      throw new Error(`classification ${key} does not match any golden failure entry`);
    }
  }
  if (seen.size > 0) {
    throw new Error(`stale classifications without a golden failure entry: ${[...seen].join(", ")}`);
  }
  return rows;
}

function render_rows(rows: Row[]): Rendered {
  const vias = new Set<string>(rows.map((row) => row.classification.via));
  vias.add("runtime");
  const lines = [...header];

  const emit_import = (entry: { module: string; readonly bindings: readonly string[] }): void => {
    const used = entry.bindings.filter((binding) => vias.has(binding));
    if (used.length > 0) lines.push(`import { ${used.join(", ")} } from "${entry.module}";`);
  };
  emit_import(runtime_module);
  for (const entry of modules) emit_import(entry);
  lines.push("");

  const executed = rows.filter((row) => row.classification.kind === "execute");
  const skipped = rows.filter((row) => row.classification.kind === "skip");

  lines.push("// Driven end to end: the call must reject with the golden code and message.");
  lines.push(executed.length > 0 ? "const executed = [" : "const executed = []; // no entry is expressible today");
  for (const row of executed) {
    const decided = row.classification as Extract<Classification, { kind: "execute" }>;
    lines.push("  {");
    lines.push(`    type: ${JSON.stringify(row.type)},`);
    lines.push(`    name: ${JSON.stringify(row.name)},`);
    lines.push(`    code: ${JSON.stringify(row.code)},`);
    if (row.messageContains !== undefined) {
      lines.push(`    messageContains: ${JSON.stringify(row.messageContains)},`);
    }
    lines.push(`    via: ${decided.via},`);
    lines.push(`    call: () => ${decided.call},`);
    lines.push("  },");
  }
  if (executed.length > 0) lines.push("];");
  lines.push("");

  lines.push("// Expressibility audit: why no QuickJS call can produce this rejection.");
  lines.push("const skipped = [");
  for (const row of skipped) {
    const decided = row.classification as Extract<Classification, { kind: "skip" }>;
    lines.push("  {");
    lines.push(`    type: ${JSON.stringify(row.type)},`);
    lines.push(`    name: ${JSON.stringify(row.name)},`);
    lines.push(`    reason: ${JSON.stringify(decided.reason)},`);
    lines.push(`    via: ${decided.via},`);
    lines.push(`    note: ${JSON.stringify(decided.note)},`);
    lines.push("  },");
  }
  lines.push("];");
  lines.push("");

  lines.push("function label(entry) {");
  lines.push("  return entry.type + \"/\" + entry.name;");
  lines.push("}");
  lines.push("");
  lines.push("async function run(entry) {");
  lines.push("  let error = null;");
  lines.push("  try {");
  lines.push("    // One microtask before the call, so the rejection is born after this");
  lines.push("    // await attached its handler. QuickJS reports a rejection as unhandled");
  lines.push("    // at the moment it fires (host.cpp rejection_thunk) and eval_module");
  lines.push("    // turns such a record during evaluation into a script failure, which");
  lines.push("    // would let the rejection tracker decide what the assertions accept.");
  lines.push("    await Promise.resolve().then(() => entry.call());");
  lines.push("  } catch (thrown) {");
  lines.push("    error = thrown;");
  lines.push("  }");
  lines.push("  if (error === null) {");
  lines.push('    throw new Error(label(entry) + ": expected a rejection, the call resolved");');
  lines.push("  }");
  lines.push("  if (error.code !== entry.code) {");
  lines.push(
    '    throw new Error(label(entry) + ": expected code " + entry.code + ", got " + ' +
      'String(error.code) + " (" + String(error.message) + ")");',
  );
  lines.push("  }");
  lines.push("  if (entry.messageContains && !String(error.message).includes(entry.messageContains)) {");
  lines.push(
    '    throw new Error(label(entry) + ": message must contain " + ' +
      'JSON.stringify(entry.messageContains) + ", got " + String(error.message));',
  );
  lines.push("  }");
  lines.push("}");
  lines.push("");
  lines.push("async function main() {");
  lines.push("  for (const entry of executed) {");
  lines.push("    await run(entry);");
  lines.push("  }");
  lines.push("}");
  lines.push("");
  lines.push("// A rejection that reaches this harness must be published, not swallowed: the");
  lines.push("// host only checks globalThis.__rim_failure after settle (bootstrap.cpp).");
  lines.push("// QuickJS puts the frames - not the message - in error.stack, so the message");
  lines.push("// is carried first and the frames only decorate it.");
  lines.push("try {");
  lines.push("  await main();");
  lines.push("} catch (error) {");
  lines.push(
    '  globalThis.__rim_failure = String(error) + (error && error.stack ? "\\n" + String(error.stack) : "");',
  );
  lines.push("}");
  lines.push("");
  lines.push(
    'runtime.debug("golden quickjs failures: executed " + executed.length + ", skipped " + skipped.length);',
  );
  lines.push("");

  return { text: lines.join("\n"), executed: executed.length, skipped: skipped.length };
}

export async function render(): Promise<Rendered> {
  return render_rows(await collect_rows());
}

export async function check_drift(): Promise<Rendered> {
  const rendered = await render();
  let actual: string | null = null;
  try {
    actual = await readFile(fixture_path, "utf8");
  } catch {
    actual = null;
  }
  if (actual !== rendered.text) {
    throw new Error(
      `golden fixture drift: ${fixture_rel} does not match contracts/golden/*.json\n` +
        `  run: bun tools/golden-fixture.ts`,
    );
  }
  return rendered;
}

async function main(): Promise<void> {
  const check = process.argv.slice(2).includes("--check");
  if (check) {
    const rendered = await check_drift();
    console.log(
      `Golden fixture drift check passed (${rendered.executed} executed, ${rendered.skipped} skipped)`,
    );
    return;
  }
  const rendered = await render();
  await writeFile(fixture_path, rendered.text, "utf8");
  console.log(
    `Wrote ${fixture_rel} (${rendered.executed} executed, ${rendered.skipped} skipped)`,
  );
}

if (import.meta.main) await main();
