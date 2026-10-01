const checks = ["contract:smoke", "contract:check", "matrix:check", "typecheck", "sdk:test", "native:test", "ts:quickjs"] as const;
for (const script of checks) {
  const result = Bun.spawnSync([process.execPath, "run", script], { stdout: "inherit", stderr: "inherit" });
  if (result.exitCode !== 0) throw new Error(`${script} checks failed`);
}
