#Requires AutoHotkey v2.0
; L6 counterpart of `rime_bench --input-latency`: time Send -> own hook
; callback via InputHook.OnKeyDown. Probing findings (do not "simplify"):
; OnKeyDown fires only for keys registered with KeyOpt "N"; F13-F24/Shift
; never arrive, only character keys do. Off-screen windows do NOT receive
; chars in any Send mode, so the probe key "q" is observed in transit by
; the hook (exact pairing, reordered counted), and run integrity is proven
; by received == sent plus foreground still held at the end. No popups.
; Usage: AutoHotkey64.exe ahk/send-latency.ahk [count]

count := 200
if A_Args.Length >= 1 {
    count := Integer(A_Args[1])
    if count <= 0 {
        FileAppend("count must be positive`n", "*")
        ExitApp(1)
    }
}

g := Gui("+ToolWindow", "AHKSendTarget")
g.Show("x3000 y100 w400 h300")
WinActivate(g.Hwnd)
if !WinWaitActive("AHKSendTarget", , 2) {
    g.Destroy()
    FileAppend("fixture did not activate; aborting instead of typing elsewhere`n", "*")
    ExitApp(1)
}

DllCall("QueryPerformanceFrequency", "Int64*", &freq := 0)
send_ns := []
arrive_ns := []
received := 0

ih := InputHook("", "{Enter}")
ih.MinSendLevel := 0
ih.KeyOpt("q", "N")
ih.OnKeyDown := OnKeyHook
ih.Start()

OnKeyHook(thisHook, vk, sc) {
    global arrive_ns, received
    DllCall("QueryPerformanceCounter", "Int64*", &t := 0)
    arrive_ns.Push(t)
    received++
}

sent := 0
loop count {
    DllCall("QueryPerformanceCounter", "Int64*", &t0 := 0)
    send_ns.Push(t0)
    Send("q")
    sent++
    Sleep(5)
}

deadline := A_TickCount + 10000
while received < sent && A_TickCount < deadline
    Sleep(5)
ih.Stop()

if received = 0 {
    g.Destroy()
    FileAppend("no callbacks received`n", "*")
    ExitApp(1)
}

; Delivery proof: hook receipt plus foreground still held (stray keys
; cannot have landed elsewhere while our window stayed foreground).
clean := (received = sent) && WinActive("AHKSendTarget")
g.Destroy()

pairs := []
reordered := 0
n := Min(arrive_ns.Length, send_ns.Length)
loop n {
    i := A_Index
    if arrive_ns[i] >= send_ns[i]
        pairs.Push((arrive_ns[i] - send_ns[i]) * 1000000.0 / freq)
    else
        reordered++
}

if pairs.Length = 0 {
    FileAppend("no valid pairs`n", "*")
    ExitApp(1)
}

sorted := pairs.Clone()
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
    "ahk.send->hook iters=" pairs.Length
    " p50=" Round(At(sorted, 0.50), 2) "us"
    " p90=" Round(At(sorted, 0.90), 2) "us"
    " p95=" Round(At(sorted, 0.95), 2) "us"
    " p99=" Round(At(sorted, 0.99), 2) "us"
    " p99.9=" Round(At(sorted, 0.999), 2) "us"
    " max=" Round(sorted[sorted.Length], 2) "us"
    " received=" received "/" sent
    " reordered=" reordered
    " clean=" (clean ? "yes" : "no") "`n", "*")
