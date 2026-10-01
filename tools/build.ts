const result = Bun.spawnSync([process.execPath, "tools/native-test.ts"], { stdout: "inherit", stderr: "inherit" });
process.exit(result.exitCode ?? 1);
