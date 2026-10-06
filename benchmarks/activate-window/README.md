# L6 scenario: activate-window

Same task, two runtimes. Rime side is the existing harness; AHK side is
a self-contained v2 script. Both create their own off-screen toolwindow
fixture (unique title per run), resolve it by exact title, foreground it
per iteration, and print percentile rows in the same shape.

## Run

```powershell
# Rime (from the repo root, quickjs preset build):
build/quickjs/tests/native/rime_bench.exe --l4-activate 200

# AHK v2 (needs AutoHotkey64.exe):
AutoHotkey64.exe benchmarks/activate-window/ahk/activate.ahk 200
```

## Reading the numbers

- Compare `l4.focus.confirmed` (Rime: request + polled switch) with
  `ahk.activate` (WinActivate + 1ms-polled switch). The request-only
  rows (`l4.focus` vs a bare WinActivate loop) are a different metric;
  do not mix them.
- Methodology note (measured 2026-10-06, loaded dev box, Debug vs
  interpreted): AHK's number includes its own foreground-retry policy;
  Rime's includes its ladder + 1ms poll quantum. Same box, same fixture
  shape, same iteration count - that is the parity this scenario claims,
  nothing more.
- Machine disclosure per `docs/performance/SPEC.md` section 1 is
  mandatory for any quoted comparison.
