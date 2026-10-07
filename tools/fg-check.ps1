# Rime foreground probe (P2-6 fixture library).
#
# Usage (FLAKY.md protocol step 6): before an isolated rerun of an
# interactive slice, run this from a tracked location and confirm your own
# window holds the foreground with no contender present:
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/fg-check.ps1
#
# Output line 1 is the foreground window (hwnd/pid/process/title/visible);
# the remaining lines list every process that currently owns a titled
# top-level window (the contender list). A rerun is only valid when line 1
# names your own terminal/fixture host. Promoted from the untracked
# build/fg-check.ps1 so the probe itself is versioned with the protocol
# that cites it; historical FLAKY rows keep citing the build/ path they
# actually ran.
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class Win32Fg {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr s, StringBuilder t, int c);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr s);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr s, out uint pid);
}
'@
$fg = [Win32Fg]::GetForegroundWindow()
$sb = New-Object System.Text.StringBuilder 256
[void][Win32Fg]::GetWindowText($fg, $sb, 256)
$pid2 = 0
[void][Win32Fg]::GetWindowThreadProcessId($fg, [ref]$pid2)
$p = Get-Process -Id $pid2 -ErrorAction SilentlyContinue
Write-Output ("fg={0} pid={1} proc={2} title='{3}' visible={4}" -f $fg, $pid2, $p.ProcessName, $sb.ToString(), [Win32Fg]::IsWindowVisible($fg))
Get-Process | Where-Object { $_.MainWindowTitle -ne "" } | ForEach-Object { Write-Output ("win: {0} [{1}] '{2}'" -f $_.Id, $_.ProcessName, $_.MainWindowTitle) }
