// Bun all-in-one: compatibility matrix generation from docs/api/coverage.json.
// `bun tools/coverage-matrix.ts` verifies docs/api/compatibility-matrix.md is
// in sync; `--write` regenerates it; `--summary <file>` appends the accounting
// report (terminal counts, percentage, evidence gaps) as markdown for CI.
import { appendFile, readFile, stat, writeFile } from "node:fs/promises";
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
    throw new Error(`matrix check failed: ${message}`);
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

// ---------------------------------------------------------------------------
// Terminal accounting.
//
// One rule, four ledgers, machine checked - the plan document carries a
// generated block so its numbers can never disagree with the JSONs they
// summarise. Historical note: the §1.2 table used to count objects as 0 while
// the M4 record counted File's 31 members, i.e. the same milestone had two
// different arithmetic. A generated block removes the human from the loop.
// ---------------------------------------------------------------------------

const TERMINAL = new Set(["implemented", "js-native", "unsupported-by-policy"]);

interface LedgerRow {
  ledger: string;
  denominator: number;
  terminal: number;
  remaining: number;
  breakdown: Record<string, number>;
  rule: string;
}

interface EvidenceGap {
  ledger: string;
  count: number;
  names: string[];
}

interface Accounting {
  schemaVersion: number;
  rule: string;
  ledgers: LedgerRow[];
  total: { denominator: number; terminal: number; remaining: number; percent: number };
  evidence_gaps: EvidenceGap[];
}

// Terminal means the status reached a documented end state. Evidence means a
// contract/compatibility test path that exists on disk. The two are tracked
// separately on purpose: a status flip without evidence is exactly the kind
// of self-reported progress this project refuses to count as done.
async function computeAccounting(): Promise<Accounting> {
  const coverage = JSON.parse(await readFile(coverage_path, "utf8")) as CoverageFile;
  const core = JSON.parse(await readFile(core_builtins_path, "utf8")) as {
    entries: Array<{ ahkName: string; status: string; contractTest?: string }>;
  };
  const builtins = JSON.parse(await readFile(resolve(root, "docs/api/builtins.json"), "utf8")) as {
    domains: Record<string, Array<{ name: string; status?: string; compatibilityTest?: string }>>;
  };
  const objects = JSON.parse(await readFile(resolve(root, "docs/api/objects.json"), "utf8")) as {
    objects: Array<{ name: string; members: Array<{ name: string; compatibilityTest?: string }> }>;
  };

  const count_status = (
    list: Array<{ status?: string }>,
  ): { terminal: number; breakdown: Record<string, number> } => {
    const breakdown: Record<string, number> = {};
    for (const entry of list) {
      const key = entry.status || "unset";
      breakdown[key] = (breakdown[key] ?? 0) + 1;
    }
    return {
      terminal: list.filter((entry) => TERMINAL.has(entry.status ?? "")).length,
      breakdown,
    };
  };

  const cov = count_status(coverage.entries);
  const cor = count_status(core.entries);

  const builtin_all = Object.values(builtins.domains).flat();
  const builtin_excluded = builtin_all.filter((entry) => entry.status === "excluded").length;
  const builtin_scored = builtin_all.filter((entry) => entry.status !== "excluded");
  const bui = count_status(builtin_scored);

  // objects.json members carry no `status` field yet (M8 owns the per-member
  // restoration pass), so their terminal count is the evidence count: a
  // compatibilityTest that names a real file. Until members gain a status
  // column this is the only non-self-reported reading available.
  let object_terminal = 0;
  const object_members: Array<{ id: string; compatibilityTest?: string }> = [];
  for (const object of objects.objects) {
    for (const member of object.members) {
      object_members.push({
        id: `${object.name}.${member.name}`,
        compatibilityTest: member.compatibilityTest,
      });
    }
  }
  object_terminal = object_members.filter(
    (member) => typeof member.compatibilityTest === "string" && member.compatibilityTest !== "missing",
  ).length;

  const rows: LedgerRow[] = [
    {
      ledger: "coverage",
      denominator: coverage.entries.length,
      terminal: cov.terminal,
      remaining: coverage.entries.length - cov.terminal,
      breakdown: cov.breakdown,
      rule: "status in implemented | js-native | unsupported-by-policy",
    },
    {
      ledger: "core-builtins",
      denominator: core.entries.length,
      terminal: cor.terminal,
      remaining: core.entries.length - cor.terminal,
      breakdown: cor.breakdown,
      rule: "status in implemented | js-native | unsupported-by-policy",
    },
    {
      ledger: "builtins",
      denominator: builtin_scored.length,
      terminal: bui.terminal,
      remaining: builtin_scored.length - bui.terminal,
      breakdown: { ...bui.breakdown, excluded: builtin_excluded },
      rule: "status in implemented | js-native | unsupported-by-policy (excluded leaves the denominator)",
    },
    {
      ledger: "objects",
      denominator: object_members.length,
      terminal: object_terminal,
      remaining: object_members.length - object_terminal,
      breakdown: { "compatibility-test": object_terminal, uncovered: object_members.length - object_terminal },
      rule: "member has a compatibilityTest naming a real file (members carry no status until M8)",
    },
  ];

  const denominator = rows.reduce((sum, row) => sum + row.denominator, 0);
  const terminal = rows.reduce((sum, row) => sum + row.terminal, 0);

  // Evidence gaps: a terminal status with no test path on disk. Printed, not
  // failed - `js-native` legitimately has none (no dedicated code exists) and
  // `unsupported-by-policy` gates are M8's refusal tests. What must stay
  // visible is `implemented` claiming a contract that no test file backs.
  const gap_for = (
    ledger: string,
    list: Array<{ ahkName?: string; name?: string; status: string; contractTest?: string; compatibilityTest?: string }>,
    evidence_key: "contractTest" | "compatibilityTest",
  ): EvidenceGap | null => {
    const names = list
      .filter(
        (entry) =>
          entry.status === "implemented" &&
          (!entry[evidence_key] || entry[evidence_key] === "missing"),
      )
      .map((entry) => entry.ahkName ?? entry.name ?? "?");
    if (names.length === 0) return null;
    return { ledger, count: names.length, names };
  };

  const gaps: EvidenceGap[] = [
    gap_for("coverage", coverage.entries, "contractTest"),
    gap_for("core-builtins", core.entries, "contractTest"),
    gap_for(
      "builtins",
      builtin_scored.map((entry) => ({ ...entry, status: entry.status ?? "unset" })),
      "compatibilityTest",
    ),
  ].filter((gap): gap is EvidenceGap => gap !== null);

  return {
    schemaVersion: 1,
    rule: "terminal = implemented | js-native | unsupported-by-policy; objects members count by evidence until they gain a status column",
    ledgers: rows,
    total: { denominator, terminal, remaining: denominator - terminal, percent: Number(((terminal / denominator) * 100).toFixed(2)) },
    evidence_gaps: gaps,
  };
}

function render_accounting_doc(accounting: Accounting): string {
  // Markdown cells cannot contain a raw pipe; the rule strings use `|` as a
  // status alternation, so they are escaped for the table only.
  const cell = (value: string): string => value.replace(/\|/g, "\\|");
  const lines: string[] = [];
  lines.push("| 清单 | 分母 | 终态 | 剩余 | 终态口径 |");
  lines.push("|---|---:|---:|---:|---|");
  for (const row of accounting.ledgers) {
    const parts = Object.entries(row.breakdown)
      .sort((a, b) => b[1] - a[1])
      .map(([key, value]) => `${key} ${value}`)
      .join("、");
    lines.push(
      `| ${row.ledger} | ${row.denominator} | ${row.terminal} | ${row.remaining} | ${cell(row.rule)}；实测 ${parts} |`,
    );
  }
  lines.push(
    `| **合计** | **${accounting.total.denominator}** | **${accounting.total.terminal}** | **${accounting.total.remaining}** | **${accounting.total.percent}%** |`,
  );
  const gaps = accounting.evidence_gaps;
  lines.push("");
  lines.push(
    gaps.length === 0
      ? "证据缺口：无（每个 `implemented` 条目都有指向真实文件的 contract/compatibility 测试路径）。"
      : `证据缺口（` +
          gaps.map((gap) => `\`${gap.ledger}\` ${gap.count} 项：${gap.names.slice(0, 8).join("、")}${gap.names.length > 8 ? " …" : ""}`).join("；") +
          "）——状态已翻 `implemented` 但没有测试文件背书，按 AGENTS 反作弊第 9 条不得作为契约证据。",
  );
  lines.push("");
  lines.push(
    "本表由 `bun tools/coverage-matrix.ts --write` 生成，`bun run matrix:check` 校验；真值源为 `docs/api/accounting.json`。手工改写会被检查拒绝。",
  );
  return lines.join("\n");
}

// Job-summary report (`--summary <path>`): the same numbers matrix:check
// enforces, emitted as GitHub-flavored markdown so a run shows the accounting
// without opening a log. 99% is M8's target, not a gate this file enforces -
// a build that failed on a percentage nobody can reach yet would only teach
// people to ignore the gate.
function renderSummary(accounting: Accounting): string {
  const share = (terminal: number, denominator: number): string =>
    denominator === 0 ? "n/a" : `${((terminal / denominator) * 100).toFixed(2)}%`;
  const lines = [
    "## AHK accounting",
    "",
    `**${accounting.total.terminal}/${accounting.total.denominator} = ${accounting.total.percent}%** terminal, ` +
      `${accounting.total.remaining} remaining · M8 target **99%** (target only — not enforced as a gate yet).`,
    "",
    "| ledger | terminal | denominator | remaining | share |",
    "| --- | --- | --- | --- | --- |",
    ...accounting.ledgers.map(
      (row) =>
        `| ${row.ledger} | ${row.terminal} | ${row.denominator} | ${row.remaining} | ${share(row.terminal, row.denominator)} |`,
    ),
    "",
    accounting.evidence_gaps.length === 0
      ? "**Evidence gaps:** none — every `implemented` row names a test file that exists on disk."
      : `**Evidence gaps:** ${accounting.evidence_gaps
          .map((gap) => `\`${gap.ledger}\` ${gap.count} (${gap.names.join(", ")})`)
          .join("; ")} — status says implemented with no test behind it (AGENTS anti-cheat rule 9).`,
    "",
    `Rule: ${accounting.rule}`,
    "",
  ];
  return `${lines.join("\n")}\n`;
}

const ACCOUNTING_START = "<!-- accounting:start generated by tools/coverage-matrix.ts -->";
const ACCOUNTING_END = "<!-- accounting:end -->";

async function checkAccounting(write: boolean): Promise<void> {
  const accounting = await computeAccounting();
  const json_path = resolve(root, "docs/api/accounting.json");
  const doc_path = resolve(root, "docs/AHK99-IMPLEMENTATION-PLAN.md");
  const expected_json = `${JSON.stringify(accounting, null, 2)}\n`;
  const expected_doc = render_accounting_doc(accounting);

  const doc = await readFile(doc_path, "utf8");
  const start = doc.indexOf(ACCOUNTING_START);
  const end = doc.indexOf(ACCOUNTING_END);
  if (start < 0 || end < 0 || end < start) {
    throw new Error(`accounting block markers missing in ${doc_path}`);
  }

  const actual_doc = doc.slice(start + ACCOUNTING_START.length, end).replace(/^\n/, "").trimEnd();
  let actual_json = "";
  try {
    actual_json = await readFile(json_path, "utf8");
  } catch {
    actual_json = "";
  }

  if (write) {
    await writeFile(json_path, expected_json, "utf8");
    const next =
      doc.slice(0, start + ACCOUNTING_START.length) +
      `\n${expected_doc}\n` +
      doc.slice(end);
    await writeFile(doc_path, next, "utf8");
    console.log(`Wrote ${json_path}`);
    console.log(`Rewrote the accounting block in ${doc_path}`);
  } else {
    if (actual_json !== expected_json) {
      console.error("docs/api/accounting.json is stale; run: bun tools/coverage-matrix.ts --write");
      process.exit(1);
    }
    if (actual_doc !== expected_doc.trimEnd()) {
      console.error(
        "docs/AHK99-IMPLEMENTATION-PLAN.md accounting block is stale; run: bun tools/coverage-matrix.ts --write",
      );
      process.exit(1);
    }
  }

  const gap_note =
    accounting.evidence_gaps.length === 0
      ? ""
      : ` | evidence gaps: ${accounting.evidence_gaps
          .map((gap) => `${gap.ledger} ${gap.count}`)
          .join(", ")}`;
  console.log(
    `Terminal ${accounting.total.terminal}/${accounting.total.denominator} = ${accounting.total.percent}%` +
      ` (${accounting.ledgers.map((row) => `${row.ledger} ${row.terminal}/${row.denominator}`).join(", ")})${gap_note}`,
  );
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
    capabilities: Array<{
      name: string;
      status?: string;
      declared?: string;
      checked?: string[];
      actions: string[];
    }>;
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
  await checkCapabilityGrants(root, registry, fail);
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

// Capability grant drift. `production_capabilities()` is the allow-list the
// desktop host really runs with, and it is hand-written next to two hundred
// other lines of bootstrap; the registry is the declaration of record. The two
// are compared here because neither direction is safe on its own: a capability
// the registry gains but the bootstrap forgets is denied only at run time
// (the script sees `capability ... denied` on its first call), and one the
// registry downgrades to `planned` or `test-only` would keep working for every
// user of the shipped host. `demo_capabilities()` is the smaller allow-list of
// the demo host and must stay a subset of the production one.
async function checkCapabilityGrants(
  root: string,
  registry: {
    capabilities: Array<{
      name: string;
      status?: string;
      declared?: string;
      checked?: string[];
    }>;
  },
  fail: (message: string) => never,
): Promise<void> {
  const bootstrap = await readFile(resolve(root, "engine/win32/js/src/bootstrap.cpp"), "utf8");
  const body_of = (name: string): string => {
    const match = bootstrap.match(new RegExp(`${name}\\(\\)\\s*\\{([\\s\\S]*?)\\n\\}`));
    if (!match) fail(`bootstrap.cpp no longer defines ${name}()`);
    return match[1];
  };
  const literals = (text: string): Set<string> =>
    new Set([...text.matchAll(/"([^"]+)"/g)].map((m) => m[1]));

  const granted = literals(body_of("production_capabilities"));
  const demo = literals(body_of("demo_capabilities"));
  const implemented = new Set(
    registry.capabilities
      .filter((c) => (c.status ?? "implemented") === "implemented")
      .map((c) => c.name),
  );

  for (const name of implemented) {
    if (!granted.has(name)) fail(`production_capabilities() misses implemented capability ${name}`);
  }
  for (const name of granted) {
    if (!implemented.has(name)) {
      fail(`production_capabilities() grants ${name}, which actions.json does not mark implemented`);
    }
  }
  for (const name of demo) {
    if (!granted.has(name)) fail(`demo_capabilities() grants ${name} production does not grant`);
  }

  // The `declared` and `checked` pointers are `file:line` references into
  // module sources. Their content is not compared (the gate lines name
  // capability constants rather than the capability string, and some entries
  // deliberately point at the code that surrounds a gate), but a renamed or
  // deleted file, or a line past the end of the file, must fail the check
  // rather than sit in the ledger as a pointer nobody can follow.
  for (const cap of registry.capabilities) {
    for (const ref of [...(cap.declared ? [cap.declared] : []), ...(cap.checked ?? [])]) {
      const cut = ref.lastIndexOf(":");
      const file = cut > 0 ? ref.slice(0, cut) : "";
      const line = Number(ref.slice(cut + 1));
      let source: string;
      try {
        source = await readFile(resolve(root, file), "utf8");
      } catch {
        fail(`capability ${cap.name} references missing file: ${file}`);
      }
      const lines = source.split(/\r?\n/).length;
      if (!Number.isInteger(line) || line < 1 || line > lines) {
        fail(`capability ${cap.name} points at ${ref}, past the end of ${file} (${lines} lines)`);
      }
    }
  }
  console.log(
    `Production grants all ${implemented.size} implemented capabilities and ${registry.capabilities.length} capability refs resolve`,
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

const write = process.argv.includes("--write");
await checkDrift();
await checkAccounting(write);
const expected = await render();
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

const summary_index = process.argv.indexOf("--summary");
if (summary_index >= 0) {
  const summary_path = process.argv[summary_index + 1];
  if (!summary_path || summary_path.startsWith("--")) {
    console.error("--summary requires the file to append the report to");
    process.exit(2);
  }
  await appendFile(summary_path, renderSummary(await computeAccounting()), "utf8");
  console.log(`Appended the accounting report to ${summary_path}`);
}
