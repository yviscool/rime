// Realism: L2 — pure table check. Oracle: the hardcoded 22 directive names from
// script.cpp IS_DIRECTIVE_MATCH (frozen denominator); the document under test
// is docs/api/directives-and-syntax.md. No logic shared with tools/coverage-matrix.ts.
import { expect, test } from "bun:test";
import { readFileSync } from "node:fs";
import { resolve } from "node:path";

const EXPECTED_22 = [
  "HotIf",
  "Hotstring",
  "Include",
  "SuspendExempt",
  "MaxThreads",
  "MaxThreadsBuffer",
  "MaxThreadsPerHotkey",
  "InputLevel",
  "HotIfTimeout",
  "SingleInstance",
  "Requires",
  "ErrorStdOut",
  "NoTrayIcon",
  "Import",
  "Module",
  "UseHook",
  "WinActivateForce",
  "ClipboardTimeout",
  "DllLoad",
  "StructPack",
  "Warn",
  "IncludeAgain",
];

const TS_API_4 = ["HotIf", "Hotstring", "Include", "SuspendExempt"];
const EXCLUDED_5 = ["ClipboardTimeout", "DllLoad", "StructPack", "Warn", "IncludeAgain"];

const doc = readFileSync(
  resolve(import.meta.dir, "../../docs/api/directives-and-syntax.md"),
  "utf8",
);

test("all 22 directives appear as table rows", () => {
  expect(EXPECTED_22).toHaveLength(22);
  expect(new Set(EXPECTED_22).size).toBe(22);
  for (const name of EXPECTED_22) {
    expect(doc).toContain(`| \`#${name}\` |`);
  }
});

test("classification counts match the documented 4 / 13 / 5 split", () => {
  expect(TS_API_4).toHaveLength(4);
  expect(EXCLUDED_5).toHaveLength(5);
  const configMapped = EXPECTED_22.filter(
    (n) => !TS_API_4.includes(n) && !EXCLUDED_5.includes(n),
  );
  expect(configMapped).toHaveLength(13);
  expect(doc).toContain("TS 等价 API 4、配置映射 13、排除 5");
});

test("each TS-equivalent directive names its runtime destination", () => {
  expect(doc).toContain("hotkeys.register({ criterion })");
  expect(doc).toContain("hotstrings.register()");
  expect(doc).toContain("ES module import + 构建期 bundler");
  expect(doc).toContain("hotkeys.register({ suspendExempt })");
});

test("excluded directives record their replacement, not silence", () => {
  expect(doc).toContain("`#ClipboardTimeout`");
  expect(doc).toContain("Action `deadline` + `AbortSignal`");
  expect(doc).toContain("unsupported-by-policy");
});
