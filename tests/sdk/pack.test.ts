// Realism: L5
// Tests the standalone packager (tools/pack.ts) and PE-embedded bundle execution:
// 1. Bun.build bundles TypeScript into an ESM bundle with rime:* externals.
// 2. tools/pack.ts appends bundle bytes, 8-byte uint64 length, and 24-byte magic trailer.
// 3. The generated executable loads the embedded payload from memory and executes it.
// 4. Command line arguments are forwarded to the script via globalThis.A_Args and process.argv.
// 5. tools/run.ts dispatches 'pack' commands to tools/pack.ts.

import { afterAll, beforeAll, describe, expect, it } from "bun:test";
import { existsSync, mkdirSync, rmSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";

const root = resolve(import.meta.dir, "../..");
const tempDir = resolve(root, "build/ts/runs/pack-test");

describe("standalone packager & PE embedding", () => {
  beforeAll(() => {
    mkdirSync(tempDir, { recursive: true });
  });

  afterAll(() => {
    try {
      rmSync(tempDir, { recursive: true, force: true });
    } catch {}
  });

  it("packs a script into a standalone .exe and executes cleanly", () => {
    const scriptPath = resolve(tempDir, "exit-zero.ts");
    const exePath = resolve(tempDir, "exit-zero.exe");

    writeFileSync(
      scriptPath,
      `import { runtime } from "rime:runtime";
runtime.exit(0);
`
    );

    const packProc = Bun.spawnSync([
      process.execPath,
      resolve(root, "tools/pack.ts"),
      scriptPath,
      "-o",
      exePath,
    ]);

    expect(packProc.exitCode).toBe(0);
    expect(existsSync(exePath)).toBe(true);

    const runProc = Bun.spawnSync([exePath]);
    expect(runProc.exitCode).toBe(0);
  });

  it("forwards command-line arguments to embedded script via A_Args and process.argv", () => {
    const scriptPath = resolve(tempDir, "args-test.ts");
    const exePath = resolve(tempDir, "args-test.exe");

    writeFileSync(
      scriptPath,
      `import { runtime } from "rime:runtime";
const aArgs = (globalThis as any).A_Args;
const argv = (globalThis as any).process?.argv;

if (!Array.isArray(aArgs) || aArgs.length !== 2) {
  runtime.exit(10);
}
if (aArgs[0] !== "alpha" || aArgs[1] !== "beta") {
  runtime.exit(11);
}
if (!Array.isArray(argv) || argv.length < 3) {
  runtime.exit(12);
}
if (argv[1] !== "alpha" || argv[2] !== "beta") {
  runtime.exit(13);
}

runtime.exit(0);
`
    );

    const packProc = Bun.spawnSync([
      process.execPath,
      resolve(root, "tools/pack.ts"),
      scriptPath,
      "-o",
      exePath,
    ]);

    expect(packProc.exitCode).toBe(0);

    // Run with expected arguments -> exit 0
    const passProc = Bun.spawnSync([exePath, "alpha", "beta"]);
    expect(passProc.exitCode).toBe(0);

    // Run with mismatched arguments -> non-zero exit code
    const failProc = Bun.spawnSync([exePath, "wrong"]);
    expect(failProc.exitCode).toBe(10);
  });

  it("supports tools/run.ts pack forwarding", () => {
    const scriptPath = resolve(tempDir, "dispatch-test.ts");
    const exePath = resolve(tempDir, "dispatch-test.exe");

    writeFileSync(
      scriptPath,
      `import { runtime } from "rime:runtime";
runtime.exit(0);
`
    );

    const dispatchProc = Bun.spawnSync([
      process.execPath,
      resolve(root, "tools/run.ts"),
      "pack",
      scriptPath,
      "-o",
      exePath,
    ]);

    expect(dispatchProc.exitCode).toBe(0);
    expect(existsSync(exePath)).toBe(true);

    const runProc = Bun.spawnSync([exePath]);
    expect(runProc.exitCode).toBe(0);
  });
});
