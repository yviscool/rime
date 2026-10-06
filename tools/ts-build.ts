import { mkdirSync } from "node:fs";
import { resolve } from "node:path";

const root = resolve(import.meta.dir, "..");
const outputDir = resolve(root, "build/ts");
mkdirSync(outputDir, { recursive: true });

const result = await Bun.build({
  entrypoints: [resolve(root, "apps/rim/src/main.ts")],
  outdir: outputDir,
  naming: "rim.js",
  target: "browser",
  format: "esm",
  external: ["rime:runtime", "rime:window", "rime:input", "rime:process", "rime:clipboard",
           "rime:automation", "rime:storage"],
  minify: false,
  sourcemap: "none",
});

if (!result.success) {
  for (const message of result.logs) console.error(message);
  process.exit(1);
}

console.log(`Built TypeScript bundle: ${resolve(outputDir, "rim.js")}`);
