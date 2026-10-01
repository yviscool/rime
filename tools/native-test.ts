import { delimiter } from "node:path";
import { existsSync, statSync, unlinkSync, writeFileSync, mkdirSync } from "node:fs";

const preset_argument = process.argv.find((argument) => argument.startsWith("--preset="));
const preset_index = process.argv.indexOf("--preset");
const preset = preset_argument?.slice("--preset=".length) ??
  (preset_index >= 0 ? process.argv[preset_index + 1] : undefined) ??
  process.env.RIME_PRESET ?? "msvc";
const lock_path = `build/${preset}.lock`;
const cmakeCandidates = [
  process.env.RIME_CMAKE,
  ...(process.env.PATH ?? "").split(delimiter).flatMap((path) => [
    `${path}/cmake`,
    `${path}/cmake.exe`,
    `${path}/cmake/bin/cmake.exe`,
  ]),
  ...(process.platform === "win32" ? ["C:/tools/cmake/bin/cmake.exe"] : []),
].filter((value): value is string => Boolean(value));

const cmake = cmakeCandidates
  .map((value) => (value.toLowerCase().endsWith("cmake") && process.platform === "win32" ? `${value}.exe` : value))
  .find((value) => existsSync(value) && statSync(value).isFile());
if (!cmake) throw new Error("CMake was not found. Set RIME_CMAKE or install CMake 3.24+.");
const ctest_name = process.platform === "win32" ? "ctest.exe" : "ctest";
const ctest = `${cmake.substring(0, cmake.lastIndexOf("cmake"))}${ctest_name}`;

const vcvarsCandidates = [
  process.env.RIME_VCVARS,
  "C:/tools/vs2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat",
].filter((value): value is string => Boolean(value));

function captureVcvarsEnv(bat: string): Record<string, string> {
  const script_path = `build\\.vcvars-env-${process.pid}.bat`;
  const script = [
    "@echo off",
    `call "${bat}" >nul 2>&1`,
    "if errorlevel 1 exit /b 1",
    "set",
    "",
  ].join("\r\n");
  writeFileSync(script_path, script);
  try {
    const result = Bun.spawnSync(["cmd", "/d", "/s", "/c", script_path], {
      stdout: "pipe",
      stderr: "pipe",
    });
    if (result.exitCode !== 0) {
      throw new Error(`failed to load MSVC environment from ${bat}`);
    }
    const env: Record<string, string> = { ...process.env } as Record<string, string>;
    for (const line of result.stdout.toString().split(/\r?\n/)) {
      const separator = line.indexOf("=");
      if (separator > 0) env[line.slice(0, separator)] = line.slice(separator + 1);
    }
    return env;
  } finally {
    try { unlinkSync(script_path); } catch {}
  }
}

let command_env: Record<string, string> | undefined;
if (preset.startsWith("msvc")) {
  const bat = vcvarsCandidates.find((value) => existsSync(value));
  if (!bat) {
    throw new Error(`MSVC preset '${preset}' requires vcvars64.bat; set RIME_VCVARS`);
  }
  mkdirSync("build", { recursive: true });
  command_env = captureVcvarsEnv(bat);
}
const command_env_options = command_env ? { env: command_env } : {};

if (existsSync(lock_path)) {
  throw new Error(`native test preset '${preset}' is already running; use a different RIME_PRESET`);
}
await Bun.write(lock_path, `${process.pid}\n`);

const commands = [
  ["-S", ".", "--preset", preset],
  ["--build", "--preset", preset],
] as const;

try {
  for (const args of commands) {
    const result = Bun.spawnSync([cmake, ...args], {
      stdout: "inherit",
      stderr: "inherit",
      ...command_env_options,
    });
    if (result.exitCode !== 0) process.exitCode = result.exitCode ?? 1;
    if (process.exitCode) break;
  }
  if (!process.exitCode) {
    const test_result = Bun.spawnSync([ctest, "--test-dir", `build/${preset}`, "--output-on-failure"], {
      stdout: "inherit",
      stderr: "inherit",
      ...command_env_options,
    });
    process.exitCode = test_result.exitCode ?? 1;
  }
} finally {
  try { unlinkSync(lock_path); } catch {}
}
