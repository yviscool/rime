#Requires -Version 5.1
<#
.SYNOPSIS
  Machine disclosure for benchmark runs (SPEC.md section 1).
  Prints CPU / RAM / OS / AHK version / repo commit / build flags as
  JSON so a RESULTS.md entry can paste it verbatim.
#>
$ErrorActionPreference = 'Stop'
$cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
$ram = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
$os = (Get-CimInstance Win32_OperatingSystem).Caption + ' ' + (Get-CimInstance Win32_OperatingSystem).BuildNumber
$ahk = ''
$ahkExe = 'C:\Program Files\AutoHotkey\v2\AutoHotkey64.exe'
if (Test-Path $ahkExe) {
  $ahk = (Get-Item $ahkExe).VersionInfo.FileVersion
}
$commit = ''
try { $commit = (git -C $PSScriptRoot\.. rev-parse --short HEAD 2>$null) } catch {}
$load = (Get-CimInstance Win32_Processor | Measure-Object -Property LoadPercentage -Average).Average
[ordered]@{
  cpu = $cpu
  ram_gb = $ram
  os = $os
  ahk_version = $ahk
  rime_commit = $commit
  desktop_load_percent_at_run = $load
} | ConvertTo-Json -Compress
