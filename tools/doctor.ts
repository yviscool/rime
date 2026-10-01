import { delimiter } from "node:path";
import { existsSync, statSync } from "node:fs";
import { findVcvars } from "./vcvars";

const problems: string[] = [];

function findInPath(name: string): string | undefined {
  return (process.env.PATH ?? "")
    .split(delimiter)
    .flatMap((path) => [`${path}/${name}`, `${path}/${name}.exe`])
    .find((value) => {
      try { return existsSync(value) && statSync(value).isFile(); } catch { return false; }
    });
}

const cmake = process.env.RIME_CMAKE ?? findInPath("cmake");
const ninja = findInPath("ninja");
const vcvars = findVcvars();
const quickjs_cache = existsSync("build/quickjs/_deps/quickjs_ng-src/quickjs.h");
const contracts = [
  "contracts/schema/action-v1.schema.json",
  "contracts/schema/result-v1.schema.json",
];

if (!cmake) problems.push("CMake 3.24+ not found (set RIME_CMAKE)");
if (!ninja) problems.push("ninja not found on PATH");
if (!vcvars) problems.push("vcvars64.bat not found (set RIME_VCVARS); MSVC presets will fail");
if (!quickjs_cache) problems.push("QuickJS-ng source not fetched yet (run bun run quickjs:test once)");
for (const contract of contracts) {
  try { await Bun.file(contract).json(); } catch { problems.push(`contract unreadable: ${contract}`); }
}

console.log(`Rime doctor: Bun ${Bun.version}, cmake ${cmake ? "ok" : "missing"}, ninja ${ninja ? "ok" : "missing"}`);
console.log(`MSVC environment: ${vcvars ?? "missing"}`);
console.log(`QuickJS-ng cache: ${quickjs_cache ? "ok" : "not fetched (network required on first run)"}`);
if (problems.length > 0) {
  for (const problem of problems) console.error(`- ${problem}`);
  process.exit(1);
}
console.log("Rime doctor: healthy");
