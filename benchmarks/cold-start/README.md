# L6 scenario: cold-start

Same task, two runtimes: spawn a process, time until its first script
line runs.

## Run

```powershell
# AHK v2 (20 spawns):
AutoHotkey64.exe benchmarks/cold-start/ahk/coldstart.ahk 20
```

## Reading the numbers

- `ahk.cold-spawn`: spawn decision -> signal file seen (user-felt total,
  includes CreateProcess and ~1ms file-poll quantum).
- `ahk.cold-spawn-to-first-line`: spawn decision -> QPC stamp written by
  the target's first line (excludes the poll quantum).
- Rime side is **pending**: host-process spawn timing needs bootstrap
  wiring (hosts/desktop + trivial bundle + readiness signal). The
  in-process `js.abi.load+execute` number is a lower bound and must NOT
  be quoted against AHK process numbers.
