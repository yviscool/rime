#Requires AutoHotkey v2.0
; Cold-start signal: the first thing the auto-execute section does is stamp
; QPC into the signal file, then exit. The launcher times spawn -> file.
; Usage (from the launcher, not by hand):
;   AutoHotkey64.exe ahk/coldstart-target.ahk <signal-file>

if A_Args.Length < 1 {
    FileAppend("usage: coldstart-target.ahk <signal-file>`n", "*")
    ExitApp(2)
}
DllCall("QueryPerformanceCounter", "Int64*", &t := 0)
try {
    FileAppend(t, A_Args[1])
} catch {
    ExitApp(1)
}
ExitApp(0)
