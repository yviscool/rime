// Bun all-in-one: compatibility matrix generation from docs/api/coverage.json.
// `bun tools/coverage-matrix.ts` verifies docs/api/compatibility-matrix.md is
// in sync; `--write` regenerates it.
import { readFile, stat, writeFile } from "node:fs/promises";
import { resolve } from "node:path";

interface CoverageEntry {
  ahkName: string;
  domain: string;
  sourceFiles: string[];
  tsModule: string;
  lane: string;
  async: string;
  capability: string;
  cancellation: string;
  status: string;
  contractTest: string;
}

interface CoverageFile {
  entries: CoverageEntry[];
}

const root = resolve(import.meta.dir, "..");
const coverage_path = resolve(root, "docs/api/coverage.json");
const matrix_path = resolve(root, "docs/api/compatibility-matrix.md");
const core_builtins_path = resolve(root, "docs/api/core-builtins.json");
const directives_doc_path = resolve(root, "docs/api/directives-and-syntax.md");
const ahk_script_path = resolve(root, "rime-research/AutoHotkey-alpha/source/script.cpp");
const ahk_functions_h_path = resolve(root, "rime-research/AutoHotkey-alpha/source/lib/functions.h");

// Denominator drift: the tracked sets must equal the AHK source truth.
// After M0 the denominator is frozen; only status transitions are allowed.
async function checkDrift(): Promise<void> {
  const coverage = JSON.parse(await readFile(coverage_path, "utf8")) as CoverageFile;
  const core = JSON.parse(await readFile(core_builtins_path, "utf8")) as {
    entries: Array<{ ahkName: string }>;
  };
  const doc = await readFile(directives_doc_path, "utf8");
  const script = await readFile(ahk_script_path, "utf8");
  const functions_h = await readFile(ahk_functions_h_path, "utf8");

  const fail = (message: string): never => {
    throw new Error(`denominator drift: ${message}`);
  };

  const md_funcs = new Set(
    [...functions_h.matchAll(/md_func[a-z_]*\(\s*([A-Za-z_][A-Za-z0-9_]*)/g)].map((m) => m[1]),
  );
  const tracked_funcs = new Set(coverage.entries.map((e) => e.ahkName));
  for (const name of md_funcs) {
    if (!tracked_funcs.has(name)) fail(`coverage.json misses md_func ${name}`);
  }
  for (const name of tracked_funcs) {
    if (!md_funcs.has(name)) fail(`coverage.json has ${name} not in functions.h md_func`);
  }

  const g_bif_section = script.slice(
    script.indexOf("FuncEntry g_BIF"),
    script.indexOf("};", script.indexOf("FuncEntry g_BIF")),
  );
  const g_bifs = new Set(
    [...g_bif_section.matchAll(/BIF[1ni]\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)/g)].map((m) => m[1]),
  );
  const tracked_bifs = new Set(core.entries.map((e) => e.ahkName));
  for (const name of g_bifs) {
    if (!tracked_bifs.has(name)) fail(`core-builtins.json misses g_BIF ${name}`);
  }
  for (const name of tracked_bifs) {
    if (!g_bifs.has(name)) fail(`core-builtins.json has ${name} not in script.cpp g_BIF`);
  }

  const directives = new Set(
    [...script.matchAll(/IS_DIRECTIVE_MATCH\(_T\("#([A-Za-z]+)"\)\)/g)].map((m) => m[1]),
  );
  const tracked_directives = new Set(
    [...doc.matchAll(/^\| `#([A-Za-z]+)` \|/gm)].map((m) => m[1]),
  );
  for (const name of directives) {
    if (!tracked_directives.has(name)) fail(`directives-and-syntax.md misses #${name}`);
  }
  for (const name of tracked_directives) {
    if (!directives.has(name)) fail(`directives-and-syntax.md has #${name} not in script.cpp`);
  }

  const builtins = JSON.parse(await readFile(resolve(root, "docs/api/builtins.json"), "utf8")) as {
    domains: Record<string, Array<{ name: string }>>;
  };
  const tracked_vars = new Set(
    Object.values(builtins.domains).flatMap((list) => list.map((e) => e.name)),
  );
  const g_biv_section = script.slice(
    script.indexOf("g_BIV_A[]"),
    script.indexOf("};", script.indexOf("g_BIV_A[]")),
  );
  const g_bivs = new Set(
    [...g_biv_section.matchAll(/A_[_wx]*\(([A-Za-z_][A-Za-z0-9_]*)/g)].map((m) => `A_${m[1]}`),
  );
  for (const name of g_bivs) {
    if (!tracked_vars.has(name)) fail(`builtins.json misses g_BIV_A ${name}`);
  }
  // Documented v1/alias names kept for readers; each records its reason in `source`.
  const aliases = new Set([
    "A_Args",
    "A_TempDir",
    "A_Computer",
    "A_CaretX",
    "A_CaretY",
    "A_Gui",
    "A_GuiControl",
    "A_LoopFileLongPath",
    "A_LoopFileTime",
    "A_Paused",
    "A_CoordMode",
  ]);
  for (const name of tracked_vars) {
    if (!g_bivs.has(name) && !aliases.has(name)) {
      fail(`builtins.json has ${name} not in script.cpp g_BIV_A and not a documented alias`);
    }
  }

  console.log(
    `Denominator frozen: ${md_funcs.size} md_func + ${g_bifs.size} g_BIF + ${directives.size} directives + ${g_bivs.size} builtin vars`,
  );

  await checkActionRegistry(root, fail);
}

// Action registry drift: the type strings the executors accept must equal
// contracts/registry/actions.json, and every referenced file must exist.
async function checkActionRegistry(
  root: string,
  fail: (message: string) => never,
): Promise<void> {
  const registry = JSON.parse(
    await readFile(resolve(root, "contracts/registry/actions.json"), "utf8"),
  ) as {
    actions: Array<{
      type: string;
      executor: string;
      module: string;
      sdk: string;
      payloadSchema: string;
      validation?: string;
    }>;
    capabilities: Array<{ name: string; actions: string[] }>;
  };

  const registry_types = new Set(registry.actions.map((a) => a.type));
  const code_types = new Set<string>();
  // Include paths and filenames also look like "word.word"; skip file extensions.
  const non_type_verbs = new Set([
    "h", "hpp", "hxx", "c", "cc", "cpp", "cxx", "js", "mjs", "ts", "tsx", "json", "md",
    "txt", "log", "dll", "exe", "lib", "ini", "yaml", "yml", "py", "png", "ico",
  ]);
  const { readdir } = await import("node:fs/promises");
  const engine_files = (await readdir(resolve(root, "engine"), { recursive: true })) as string[];
  const executors = engine_files
    .map((f) => f.replace(/\\/g, "/"))
    .filter((f) => f.endsWith(".cpp") && f.includes("executor"));
  for (const rel of executors) {
    const text = await readFile(resolve(root, "engine", rel), "utf8");
    // Two or more dotted segments ("window.move", "window.group.add");
    // the last segment doubles as the file-extension filter below.
    for (const m of text.matchAll(/"([a-z][a-z0-9]*(?:\.[a-z][a-z0-9]*)+)"/g)) {
      const segments = m[1].split(".");
      if (non_type_verbs.has(segments[segments.length - 1])) continue;
      code_types.add(m[1]);
    }
  }
  for (const name of code_types) {
    if (!registry_types.has(name)) fail(`actions.json misses executor type ${name}`);
  }
  for (const name of registry_types) {
    if (!code_types.has(name)) fail(`actions.json has ${name} no executor accepts`);
  }

  const capability_actions = new Set(registry.capabilities.flatMap((c) => c.actions));
  for (const name of capability_actions) {
    if (!registry_types.has(name)) fail(`actions.json capability references unknown type ${name}`);
  }

  for (const action of registry.actions) {
    const refs = [action.executor, action.module, action.sdk, action.validation ?? ""];
    if (action.payloadSchema !== "inline") refs.push(action.payloadSchema);
    for (const ref of refs) {
      const file = ref.split(/[:#]/).slice(0, -1).join(":").replace(/:\d+$/, "");
      if (!file) continue;
      try {
        await stat(resolve(root, file));
      } catch {
        fail(`actions.json ${action.type} references missing file: ${file}`);
      }
    }
  }
  console.log(
    `Action registry checked: ${registry_types.size} types across ${executors.length} executor files`,
  );
  await checkProductionRegistration(root, registry, fail);
}

// Production wiring drift: Bootstrap::register_executors must cover every
// implemented action type, or the desktop bundle answers `unsupported` for a
// type the executors dispatch (17 window types once shipped exactly that
// way). The window family is covered by iterating the executor's exported
// window_action_types(); every other implemented type must appear as a
// literal in the register_executors body, and that body must not register a
// type the registry no longer declares.
async function checkProductionRegistration(
  root: string,
  registry: {
    actions: Array<{ type: string; status?: string }>;
  },
  fail: (message: string) => never,
): Promise<void> {
  const bootstrap = await readFile(resolve(root, "engine/win32/js/src/bootstrap.cpp"), "utf8");
  const start = bootstrap.indexOf("Bootstrap::register_executors");
  if (start < 0) fail("bootstrap.cpp no longer defines Bootstrap::register_executors");
  const next = bootstrap.indexOf("Bootstrap::", start + 1);
  const body = bootstrap.slice(start, next > 0 ? next : undefined);

  const exec_source = await readFile(resolve(root, "engine/win32/src/window_executor.cpp"), "utf8");
  const accept_start = exec_source.indexOf("window_action_types() {");
  const accept_end = exec_source.indexOf("return types;", accept_start);
  if (accept_start < 0 || accept_end < 0) {
    fail("window_executor.cpp no longer defines window_action_types()");
  }
  const accept_body = exec_source.slice(accept_start, accept_end);
  const window_accept = new Set(
    [...accept_body.matchAll(/"([a-z][a-z0-9]*(?:\.[a-z][a-z0-9]*)+)"/g)].map((m) => m[1]),
  );

  const implemented = registry.actions
    .filter((action) => (action.status ?? "implemented") === "implemented")
    .map((action) => action.type);
  const registry_window = new Set(implemented.filter((type) => type.startsWith("window.")));
  for (const type of registry_window) {
    if (!window_accept.has(type)) fail(`window_action_types() misses registry type ${type}`);
  }
  for (const type of window_accept) {
    if (!registry_window.has(type)) fail(`window_action_types() has unregistered type ${type}`);
  }

  if (!/for \(const std::string& type : window_action_types\(\)\)/.test(body)) {
    fail("register_executors must register the window family via window_action_types()");
  }
  const body_literals = new Set(
    [...body.matchAll(/"([a-z][a-z0-9]*(?:\.[a-z][a-z0-9]*)+)"/g)].map((m) => m[1]),
  );
  for (const type of implemented) {
    if (type.startsWith("window.")) continue;
    if (!body_literals.has(type)) fail(`register_executors misses implemented type ${type}`);
  }
  for (const type of body_literals) {
    if (!registry_window.has(type) && !implemented.includes(type)) {
      fail(`register_executors registers type ${type} the registry does not declare`);
    }
  }
  console.log(
    `Production bootstrap registers all ${implemented.length} implemented action types`,
  );
}

const columns = [
  "AHK Function",
  "Domain",
  "Source Files",
  "TS Module",
  "Lane",
  "Async",
  "Capability",
  "Cancellation",
  "Status",
  "Contract Test",
] as const;

const required: Array<keyof CoverageEntry> = [
  "ahkName",
  "domain",
  "sourceFiles",
  "tsModule",
  "lane",
  "async",
  "capability",
  "cancellation",
  "status",
  "contractTest",
];

function escape_cell(value: string): string {
  // Input assumption: plain cell text without pre-escaped pipes; only `|` is escaped.
  return value.replace(/\|/g, "\\|");
}

function normalize_newlines(value: string): string {
  return value.replace(/\r\n/g, "\n");
}

async function render(): Promise<string> {
  const coverage = JSON.parse(await readFile(coverage_path, "utf8")) as CoverageFile;
  if (!Array.isArray(coverage.entries) || coverage.entries.length === 0) {
    throw new Error("coverage.json has no entries");
  }
  for (const [index, entry] of coverage.entries.entries()) {
    for (const field of required) {
      const value = entry[field];
      const missing =
        value === undefined ||
        value === null ||
        (typeof value === "string" && value.length === 0) ||
        (Array.isArray(value) && value.length === 0);
      if (missing) {
        throw new Error(`coverage entry ${index} (${entry.ahkName ?? "?"}) misses ${field}`);
      }
    }
    // contractTest names real test files (or "missing"): a stale path would
    // silently advertise coverage that no longer exists on disk.
    if (entry.contractTest !== "missing") {
      for (const file of entry.contractTest.split(",")) {
        const path = file.trim();
        if (!path) continue;
        try {
          await stat(resolve(root, path));
        } catch {
          throw new Error(
            `coverage entry ${index} (${entry.ahkName ?? "?"}) contractTest file not found: ${path}`,
          );
        }
      }
    }
  }
  const rows = [...coverage.entries].sort((a, b) =>
    a.ahkName < b.ahkName ? -1 : a.ahkName > b.ahkName ? 1 : 0,
  );
  const lines: string[] = [];
  lines.push("# AHK Function Compatibility Matrix");
  lines.push("");
  lines.push(
    `Source: docs/api/coverage.json, derived from the ${rows.length} md_func entries in functions.h. contract-only means the target contract is registered; it does not mean a JavaScript binding or Native executor exists. js-native means ECMAScript already covers the item (equivalence mapping in docs/api/runtime-language.md, zero dedicated code); terminal statuses per docs/api/stdlib.md are implemented | js-native | unsupported-by-policy. sourceFiles is a function-name hit index and must be verified before implementation.`,
  );
  lines.push("");
  lines.push("");
  lines.push(`| ${columns.join(" | ")} |`);
  lines.push(`|${columns.map(() => "---").join("|")}|`);
  for (const entry of rows) {
    lines.push(
      `| \`${escape_cell(entry.ahkName)}\` | ${escape_cell(entry.domain)} | ${escape_cell(
        entry.sourceFiles.join("<br>"),
      )} | \`${escape_cell(entry.tsModule)}\` | ${escape_cell(entry.lane)} | ${escape_cell(
        entry.async,
      )} | \`${escape_cell(entry.capability)}\` | ${escape_cell(entry.cancellation)} | ${escape_cell(
        entry.status,
      )} | ${escape_cell(entry.contractTest)} |`,
    );
  }
  lines.push("");
  return normalize_newlines(lines.join("\n"));
}

await checkDrift();
const expected = await render();
const write = process.argv.includes("--write");
if (write) {
  await writeFile(matrix_path, expected, "utf8");
  console.log(`Wrote ${matrix_path}`);
} else {
  let actual = "";
  try {
    actual = normalize_newlines(await readFile(matrix_path, "utf8"));
  } catch {
    actual = "";
  }
  if (normalize_newlines(actual) !== normalize_newlines(expected)) {
    console.error("docs/api/compatibility-matrix.md is stale; run: bun tools/coverage-matrix.ts --write");
    process.exit(1);
  }
  console.log("Compatibility matrix is up to date");
}
