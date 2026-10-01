import { existsSync } from "node:fs";
import { resolve } from "node:path";

const root = resolve(import.meta.dir, "..");
const build = Bun.spawnSync([process.execPath, "tools/ts-build.ts"], {
  cwd: root,
  stdout: "inherit",
  stderr: "inherit",
});
if (build.exitCode !== 0) process.exit(build.exitCode ?? 1);

const native = Bun.spawnSync([process.execPath, "tools/native-test.ts", "--preset=quickjs"], {
  cwd: root,
  stdout: "inherit",
  stderr: "inherit",
});
if (native.exitCode !== 0) process.exit(native.exitCode ?? 1);

const runner = resolve(root, "build/quickjs/tests/js/rime_js_bundle.exe");
const bundle = resolve(root, "build/ts/rim.js");
if (!existsSync(runner)) throw new Error(`QuickJS bundle runner not found: ${runner}`);
const result = Bun.spawnSync([runner, bundle], { cwd: root, stdout: "inherit", stderr: "inherit" });
if (result.exitCode !== 0) process.exit(result.exitCode ?? 1);
console.log("QuickJS executed the Rim TypeScript bundle successfully");
