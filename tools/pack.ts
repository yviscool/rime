import { chmodSync, existsSync, mkdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { basename, dirname, resolve } from "node:path";

const root = resolve(import.meta.dir, "..");
const rawArgs = process.argv.slice(2);

let inputPath: string | null = null;
let outputPath: string | null = null;

for (let i = 0; i < rawArgs.length; ++i) {
  const arg = rawArgs[i];
  if (arg === "-o" || arg === "--output") {
    if (i + 1 < rawArgs.length) {
      outputPath = rawArgs[++i];
    } else {
      console.error("Error: -o/--output requires a filename argument");
      process.exit(1);
    }
  } else if (arg.startsWith("-o=")) {
    outputPath = arg.slice(3);
  } else if (arg.startsWith("--output=")) {
    outputPath = arg.slice(9);
  } else if (!arg.startsWith("-")) {
    if (!inputPath) {
      inputPath = arg;
    } else {
      console.error(`Error: Unexpected positional argument '${arg}'`);
      process.exit(1);
    }
  } else {
    console.error(`Error: Unknown flag '${arg}'`);
    process.exit(1);
  }
}

if (!inputPath) {
  console.error("Usage: bun tools/pack.ts <script.ts> [-o <out.exe>]");
  process.exit(1);
}

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

if (!outputPath) {
  const scriptBaseName = basename(scriptPath).replace(/\.[^.]+$/, "");
  outputPath = resolve(process.cwd(), `${scriptBaseName}.exe`);
}
const outResolved = resolve(process.cwd(), outputPath);

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

const banner =
  'if (typeof console === "undefined") { globalThis.console = { log: (...a) => { try { globalThis.__rime_debug?.(a.join(" ")); } catch {} }, error: (...a) => {}, warn: (...a) => {}, info: (...a) => {} }; }';

const buildResult = await Bun.build({
  entrypoints: [scriptPath],
  target: "browser",
  format: "esm",
  external: externals,
  minify: false,
  sourcemap: "none",
  banner,
});

if (!buildResult.success || buildResult.outputs.length === 0) {
  console.error(`Error: Failed to compile TypeScript script: ${inputPath}`);
  for (const log of buildResult.logs) {
    console.error(log);
  }
  process.exit(1);
}

const bundleText = await buildResult.outputs[0].text();
const bundleBytes = Buffer.from(bundleText, "utf-8");

const hostCandidates = [
  resolve(root, "build/quickjs/hosts/desktop/rime_host.exe"),
  resolve(root, "build/dev/hosts/desktop/rime_host.exe"),
  resolve(root, "build/msvc/hosts/desktop/rime_host.exe"),
  resolve(root, "build/hosts/desktop/rime_host.exe"),
];

function findTemplateExecutable(): string | null {
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

const templatePath = findTemplateExecutable();
if (!templatePath) {
  console.error("Error: Could not find template binary rime_host.exe");
  console.error("Please run: cmake --build build/quickjs (or cmake --build build/dev)");
  process.exit(1);
}

const templateBytes = readFileSync(templatePath);

// Trailer format:
// [UTF-8 JS bundle bytes] + [uint64_t length (8 bytes, little-endian)] + [Magic string: "RIME_BUNDLE_PAYLOAD_V1\0\0" (24 bytes)]
const lengthBuf = Buffer.alloc(8);
lengthBuf.writeBigUInt64LE(BigInt(bundleBytes.length), 0);

const magicBuf = Buffer.alloc(24);
magicBuf.write("RIME_BUNDLE_PAYLOAD_V1", 0, "utf-8");

mkdirSync(dirname(outResolved), { recursive: true });

const finalBinary = Buffer.concat([templateBytes, bundleBytes, lengthBuf, magicBuf]);
writeFileSync(outResolved, finalBinary);

try {
  chmodSync(outResolved, 0o755);
} catch {}

console.log(`Successfully packed '${inputPath}' -> '${outResolved}' (${finalBinary.length} bytes)`);
