# L4/L5 Harness Design (BenchmarkTarget + pressure rig)

Status: design only. No code until this doc is reviewed.

## 1. Goal

Close the two gaps `rime_bench` deliberately leaves open:

- L4: end-to-end task latency (physical input -> Win32 side effect).
- L5: sustained input pressure (queue depth, drop rate, CPU, GC).

Non-goal: AHK dual implementation (L6). That needs scenario parity
first; it gets its own doc after L4 exists.

## 2. BenchmarkTarget.exe

One fixture process, no production code inside:

- A top-level HWND (`RimeBenchTarget`), one Edit, one Button, one List.
- A minimal UIA provider for the Button (find + invoke pattern only).
- Clipboard cooperation: answers a known text payload on demand.
- Started/stopped by the harness; asserts its own teardown (no orphan
  windows, clipboard restored) so every run is self-cleaning (L5 rule).

## 3. L4 task set (each: N iterations, percentile report)

1. `hotkey -> activate Notepad-class window` (window search + activate).
2. `clipboard read -> transform -> paste` round trip.
3. `hotstring expand` (::email -> full text).
4. `UIA find button -> invoke` on BenchmarkTarget.
5. `launcher file search -> open` (storage + process).

Each task records stage stamps (input-received, dispatched, action-built,
executor-enter, win32-return) into the Action Trace - the same Trace the
runtime already emits, so no parallel instrumentation.

## 4. L5 pressure rig

- Synthetic `MouseMove` storm at 10k and 100k events/s into the real
  InputService; observe: CPU, RSS, max queue depth, coalesced/dropped
  counters, dispatch p99.9, QuickJS GC pauses.
- Pass criteria are about stability, not speed: zero growth in RSS,
  zero unbounded queue growth, p99.9 dispatch within budget.
- Never runs inside ctest (timing-flaky by design, same rule as
  `rime_bench`); manual + nightly CI only.

## 5. Anti-cheating for harnesses

- Fixture windows are created, asserted and destroyed inside the run.
- Clipboard saved before, restored after (see reload_slice precedent).
- Foreign-process races stay out of scope and must be declared, not
  hidden (see clipboard wire-mutex postmortem).
- Numbers without machine disclosure (SPEC section 1) are invalid.
