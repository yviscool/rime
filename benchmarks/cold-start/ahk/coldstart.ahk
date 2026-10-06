#Requires AutoHotkey v2.0
; Cold-start PK launcher: spawn N AutoHotkey64 processes running
; coldstart-target.ahk, time spawn -> first-script-line for each.
; Reports percentiles plus the AHK-internal stamp (spawn overhead excluded).
; No popups: stdout only.
; Usage: AutoHotkey64.exe ahk/coldstart.ahk [count]

count := 20
if A_Args.Length >= 1 {
    count := Integer(A_Args[1])
    if count <= 0 {
        FileAppend("count must be positive`n", "*")
        ExitApp(1)
    }
}

exe := "C:\Program Files\AutoHotkey\v2\AutoHotkey64.exe"
target := A_ScriptDir "\coldstart-target.ahk"
DllCall("QueryPerformanceFrequency", "Int64*", &freq := 0)
toUs := (t0, t1) => (t1 - t0) * 1000000.0 / freq

totals := []
internals := []
loop count {
    signal := A_Temp "\ahkcold-" A_TickCount "-" A_Index ".txt"
    try FileDelete(signal)
    DllCall("QueryPerformanceCounter", "Int64*", &t0 := 0)
    try {
        Run('"' exe '" "' target '" "' signal '"', , "Hide")
    } catch {
        FileAppend("spawn failed`n", "*")
        ExitApp(1)
    }
    deadline := A_TickCount + 10000
    while !FileExist(signal) {
        if A_TickCount >= deadline {
            FileAppend("signal timeout`n", "*")
            ExitApp(1)
        }
        Sleep(1)
    }
    DllCall("QueryPerformanceCounter", "Int64*", &t1 := 0)
    totals.Push(toUs(t0, t1))
    try {
        inner := Integer(FileRead(signal))
        internals.Push(toUs(t0, inner))
    }
    try FileDelete(signal)
}

SortNumeric(arr) {
    sorted := arr.Clone()
    loop sorted.Length {
        i := A_Index
        while i > 1 && sorted[i - 1] > sorted[i] {
            tmp := sorted[i - 1]
            sorted[i - 1] := sorted[i]
            sorted[i] := tmp
            i--
        }
    }
    return sorted
}

At(sorted, p) {
    idx := Integer(p * (sorted.Length - 1)) + 1
    return sorted[idx]
}

s := SortNumeric(totals)
FileAppend(
    "ahk.cold-spawn iters=" s.Length
    " p50=" Round(At(s, 0.50), 0) "us"
    " p90=" Round(At(s, 0.90), 0) "us"
    " p99=" Round(At(s, 0.99), 0) "us"
    " max=" Round(s[s.Length], 0) "us`n", "*")
q := SortNumeric(internals)
FileAppend(
    "ahk.cold-spawn-to-first-line iters=" q.Length
    " p50=" Round(At(q, 0.50), 0) "us"
    " p99=" Round(At(q, 0.99), 0) "us"
    " (includes CreateProcess; 1ms poll quantum inside)`n", "*")
