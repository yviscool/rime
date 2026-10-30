import { existsSync, mkdirSync, statSync } from "node:fs";
import { basename, resolve } from "node:path";

const root = resolve(import.meta.dir, "..");
const rawArgs = process.argv.slice(2);

const trace = rawArgs.includes("--trace");
const scriptArgs = rawArgs.filter((arg) => arg !== "--trace");

if (scriptArgs.length === 0) {
  console.error("Usage: bun tools/run.ts <script.ts> [--trace]");
  process.exit(1);
}

const inputPath = scriptArgs[0];
const scriptPath = resolve(process.cwd(), inputPath);

if (!existsSync(scriptPath)) {
  console.error(`Error: Script file not found: ${inputPath}`);
  process.exit(1);
}

try {
  if (!statSync(scriptPath).isFile()) {
    console.error(`Error: Script path is not a file: ${inputPath}`);
    process.exit(1);
  }
} catch (err) {
  console.error(`Error checking script path ${inputPath}:`, err);
  process.exit(1);
}

const runsDir = resolve(root, "build/ts/runs");
mkdirSync(runsDir, { recursive: true });

const scriptBaseName = basename(scriptPath).replace(/\.[^.]+$/, "");
const bundlePath = resolve(runsDir, `${scriptBaseName}.js`);

const externals = [
  "rime:runtime",
  "rime:window",
  "rime:input",
  "rime:process",
  "rime:clipboard",
  "rime:screen",
  "rime:sound",
  "rime:automation",
  "rime:control",
  "rime:storage",
  "rime:registry",
  "rime:ui",
];

const buildResult = await Bun.build({
  entrypoints: [scriptPath],
  outdir: runsDir,
  naming: `${scriptBaseName}.js`,
  target: "browser",
  format: "esm",
  external: externals,
  minify: false,
  sourcemap: "none",
  banner:
    'if (typeof console === "undefined") { globalThis.console = { log: (...a) => { try { globalThis.__rime_debug?.(a.join(" ")); } catch {} }, error: (...a) => {}, warn: (...a) => {}, info: (...a) => {} }; }',
});

if (!buildResult.success) {
  console.error(`Error: Failed to compile TypeScript script: ${inputPath}`);
  for (const log of buildResult.logs) {
    console.error(log);
  }
  process.exit(1);
}

const hostCandidates = [
  resolve(root, "build/quickjs/hosts/desktop/rime_host.exe"),
  resolve(root, "build/msvc/hosts/desktop/rime_host.exe"),
  resolve(root, "build/dev/hosts/desktop/rime_host.exe"),
  resolve(root, "build/quickjs/tests/js/rime_js_bundle.exe"),
];

function findHostExecutable(): string | null {
  for (const candidate of hostCandidates) {
    if (existsSync(candidate)) {
      try {
        if (statSync(candidate).isFile()) return candidate;
      } catch {}
    }
    const noExt = candidate.replace(/\.exe$/, "");
    if (existsSync(noExt)) {
      try {
        if (statSync(noExt).isFile()) return noExt;
      } catch {}
    }
  }
  return null;
}

const hostExecutable = findHostExecutable();

if (!hostExecutable) {
  console.error("Error: Could not locate Rime host executable.");
  console.error("Searched locations:");
  for (const candidate of hostCandidates) {
    console.error(`  - ${candidate}`);
  }
  console.error("\nPlease build the host before running scripts:");
  console.error("  bun run quickjs:test   (Builds QuickJS preset)");
  console.error("  cmake --build --preset quickjs");
  console.error("  cmake --build --preset msvc");
  process.exit(1);
}

const isJsBundleRunner = basename(hostExecutable).toLowerCase().includes("rime_js_bundle");
const hostArgs = isJsBundleRunner
  ? [hostExecutable, "--production", bundlePath]
  : [hostExecutable, bundlePath];

const env = { ...process.env };
if (trace) {
  env.RIME_TRACE = "1";
}

const proc = Bun.spawn(hostArgs, {
  cwd: root,
  env,
  stdout: "inherit",
  stderr: "inherit",
});

const exitCode = await proc.exited;
if (exitCode === 0) {
  console.log(`[Rime Runner] Script "${inputPath}" completed successfully (exit code 0).`);
} else {
  console.error(`[Rime Runner] Script "${inputPath}" exited with code ${exitCode}.`);
}
process.exit(exitCode);
