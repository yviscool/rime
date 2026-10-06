#Requires AutoHotkey v2.0
; L6 counterpart of `rime_bench --l4-activate`: resolve a self-created
; fixture window by exact title, then foreground-activate it, per iteration.
; No popups, no MsgBox: results go to stdout. The fixture is off-screen and
; toolwindow-styled, mirroring the Rime fixture (bench.cpp l4_fixture).
; Usage: AutoHotkey64.exe ahk/activate.ahk [iters]

iters := 200
if A_Args.Length >= 1 {
    iters := Integer(A_Args[1])
    if iters <= 0 {
        FileAppend("iters must be positive`n", "*")
        ExitApp(1)
    }
}

token := DllCall("GetCurrentProcessId", "UInt") "." A_TickCount
title := "AHKBenchTarget " token
g := Gui("+ToolWindow", title)
g.Show("x3000 y100 w400 h300")
if !WinWait(title, , 5) {
    FileAppend("fixture window did not appear`n", "*")
    ExitApp(1)
}

DllCall("QueryPerformanceFrequency", "Int64*", &freq := 0)
times := []
succeeded := 0
loop iters {
    DllCall("QueryPerformanceCounter", "Int64*", &t0 := 0)
    try {
        WinActivate(title)
        ; 1ms poll instead of WinWaitActive: WinWaitActive's internal quantum
        ; would dominate the measurement (observed ~220ms flat). Bounded 2s.
        deadline := A_TickCount + 2000
        while !WinActive(title) {
            if A_TickCount >= deadline
                break
            Sleep(1)
        }
    }
    DllCall("QueryPerformanceCounter", "Int64*", &t1 := 0)
    if WinActive(title) {
        succeeded++
        times.Push((t1 - t0) * 1000000.0 / freq)
    }
}
g.Destroy()

if times.Length = 0 {
    FileAppend("no successful iterations`n", "*")
    ExitApp(1)
}

; Insertion sort (iters is small; no dependency on Sort numeric modes).
sorted := times.Clone()
loop sorted.Length {
    i := A_Index
    while i > 1 && sorted[i - 1] > sorted[i] {
        tmp := sorted[i - 1]
        sorted[i - 1] := sorted[i]
        sorted[i] := tmp
        i--
    }
}

At(sorted, p) {
    idx := Integer(p * (sorted.Length - 1)) + 1
    return sorted[idx]
}

FileAppend(
    "ahk.activate iters=" times.Length
    " p50=" Round(At(sorted, 0.50), 2) "us"
    " p90=" Round(At(sorted, 0.90), 2) "us"
    " p95=" Round(At(sorted, 0.95), 2) "us"
    " p99=" Round(At(sorted, 0.99), 2) "us"
    " p99.9=" Round(At(sorted, 0.999), 2) "us"
    " max=" Round(sorted[sorted.Length], 2) "us"
    " success=" succeeded "/" iters "`n", "*")
