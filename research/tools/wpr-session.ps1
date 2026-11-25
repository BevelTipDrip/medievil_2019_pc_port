# One-click WPR session for MediEvil frame-time captures (the frame-jitter-capture skill): the light Jitter profile
# into the ring buffer, F8 saves the last ~75-90 s, every capture under a new name. Right-click > Run with PowerShell
# (or run it from any PowerShell): it asks for administrator rights itself (WPR needs them).
#   powershell -ExecutionPolicy Bypass -File C:\Users\Ty\MediEvil\medievil_2019_pc_port\research\tools\wpr-session.ps1
#   ... -Tag graveyard-fight      (the name's start; the date and time follow it: graveyard-fight-20261125-041729.etl)
#   ... -Gpu                      (the JitterGpu profile: adds the graphics kernel's GPU queue events, for GPU idle time)
# Traces go to D:\MediEvil-archive\research-data\wpr (research-data\wpr's real path). Ctrl+C ends the session.
# Before pressing F8 the first time: a stretch of ~60 s with no WPR running is the reference (the skill's rules).
param([string]$Tag = 'session', [string]$Key = 'F8', [switch]$Gpu)
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = "-NoExit -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Tag `"$Tag`" -Key $Key" + $(if ($Gpu) { ' -Gpu' } else { '' })
    Start-Process powershell -Verb RunAs -ArgumentList $argList
    return
}
$hotkey = Join-Path $PSScriptRoot 'wpr-hotkey.ps1'
$wprProfile = (Join-Path $PSScriptRoot 'wpr-jitter.wprp') + $(if ($Gpu) { '!JitterGpu' } else { '!Jitter' })
& $hotkey -Key $Key -Tag $Tag -WprProfile $wprProfile
