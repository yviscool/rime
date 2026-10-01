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
    `Source: docs/api/coverage.json, derived from the ${rows.length} md_func entries in functions.h. contract-only means the target contract is registered; it does not mean a JavaScript binding or Native executor exists. sourceFiles is a function-name hit index and must be verified before implementation.`,
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
