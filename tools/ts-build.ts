import { mkdirSync, readdirSync, statSync } from "node:fs";
import { resolve } from "node:path";

const root = resolve(import.meta.dir, "..");
const outputDir = resolve(root, "build/ts");
mkdirSync(outputDir, { recursive: true });

const externals = ["rime:runtime", "rime:window", "rime:input", "rime:process", "rime:clipboard",
         "rime:screen", "rime:sound", "rime:automation", "rime:control", "rime:storage",
         "rime:registry", "rime:ui"];

// Every apps/<name>/src/main.ts builds to build/ts/<name>.js (rim, spy, …).
// Adding an app needs no tooling change: drop the directory in and rebuild.
const appsDir = resolve(root, "apps");
const entrypoints = readdirSync(appsDir)
  .map((name) => ({ name, main: resolve(appsDir, name, "src", "main.ts") }))
  .filter(({ main }) => {
    try {
      return statSync(main).isFile();
    } catch {
      return false;
    }
  });

if (entrypoints.length === 0) process.exit(1);

for (const { name, main } of entrypoints) {
  const result = await Bun.build({
    entrypoints: [main],
    outdir: outputDir,
    naming: `${name}.js`,
    target: "browser",
    format: "esm",
    external: externals,
    minify: false,
    sourcemap: "none",
  });

  if (!result.success) {
    for (const message of result.logs) console.error(message);
    process.exit(1);
  }
  console.log(`Built TypeScript bundle: ${resolve(outputDir, `${name}.js`)}`);
}
