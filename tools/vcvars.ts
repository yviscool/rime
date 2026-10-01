import { existsSync } from "node:fs";

// Machine-local fallbacks for installs that are not registered with vswhere.
const hardcoded = [
  "C:/tools/vs2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files/Microsoft Visual Studio/2026/Enterprise/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files/Microsoft Visual Studio/2026/Community/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files/Microsoft Visual Studio/2026/BuildTools/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat",
];

function viaVswhere(): string | undefined {
  const vswhere =
    [
      "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe",
      "C:/Program Files/Microsoft Visual Studio/Installer/vswhere.exe",
    ].find((value) => existsSync(value));
  if (!vswhere) return undefined;
  const result = Bun.spawnSync(
    [
      vswhere,
      "-latest",
      "-products",
      "*",
      "-requires",
      "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
      "-property",
      "installationPath",
    ],
    { stdout: "pipe", stderr: "pipe" },
  );
  if (result.exitCode !== 0) return undefined;
  const install = result.stdout.toString().split(/\r?\n/).find((line) => line.trim());
  if (!install) return undefined;
  const bat = `${install.trim()}/VC/Auxiliary/Build/vcvars64.bat`;
  return existsSync(bat) ? bat : undefined;
}

/** Locate vcvars64.bat: RIME_VCVARS, then vswhere, then known install paths. */
export function findVcvars(): string | undefined {
  const candidates = [process.env.RIME_VCVARS, viaVswhere(), ...hardcoded];
  return candidates.find((value) => value && existsSync(value));
}
