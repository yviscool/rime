# L6 scenario: send-latency

Same task, two runtimes: inject one keystroke, time until the sender's
own hook callback fires.

## Run

```powershell
# Rime (quickjs preset build):
build/quickjs/tests/native/rime_bench.exe --input-latency 200

# AHK v2:
AutoHotkey64.exe benchmarks/send-latency/ahk/send-latency.ahk 200
```

## Reading the numbers

- Both rows are send-call -> own-hook-callback, paired by order, negatives
  counted as reordered (never averaged in).
- Probe keys differ by necessity (Rime: F24 batch; AHK: letter into its own
  off-screen Edit) - see `scenario.json` caveats. The measured path
  (SendInput -> OS -> low-level hook -> subscriber) is the same shape.
- Rime's path additionally tags markers, serializes batches and hops
  threads; that overhead is the documented price of its self/foreign
  distinction, not noise.
