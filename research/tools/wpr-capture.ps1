# Records a whole-system CPU sampling trace (every thread, with stacks) with Windows Performance
# Recorder while the game runs: the port's own sampler only covers its GPU threads, not the game's.
# Needs an elevated (administrator) PowerShell: the user runs it. It never starts or stops the game.
#   powershell -ExecutionPolicy Bypass -File <this file> -Seconds 20 -Tag graveyard
# Start it once the game is at the scene to measure; it waits 5 s (time to switch back), records,
# and writes C:\Users\Ty\MediEvil\research-data\wpr\<tag>.etl.
# The full CPU profile (stacks) writes ~1 GB per 15-45 s: light profile, 20 s and a free-space check
# by default; -WprProfile CPU for stacks.
# -Delay 0 starts at once (wpr-hotkey.ps1 calls it so, from a key press inside the game).
param([int]$Seconds = 20, [string]$Tag = 'capture', [string]$WprProfile = 'CPU.light', [int]$MinFreeGB = 10,
      [int]$Delay = 5)
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this from an administrator PowerShell (WPR needs it).'
}
if (-not (Get-Process medievil-runtime -ErrorAction SilentlyContinue)) { throw 'The game is not running.' }
$dir = 'C:\Users\Ty\MediEvil\research-data\wpr'
New-Item -ItemType Directory -Force $dir | Out-Null
$free = (Get-PSDrive C).Free / 1GB
if ($free -lt $MinFreeGB) { throw ("Only {0:N1} GB free on C: (need {1}): the trace and its merge need room." -f $free, $MinFreeGB) }
$etl = Join-Path $dir "$Tag.etl"
# The ADK's WPR (Windows Performance Toolkit, installed 2026-10-18) if present, else Windows' own.
$adk = 'C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\wpr.exe'
$wpr = if (Test-Path $adk) { $adk } else { 'wpr' }
& $wpr -cancel 2>$null | Out-Null
if ($Delay -gt 0) {
    Write-Host "Recording starts in $Delay s: switch back to the game."
    Start-Sleep -Seconds $Delay
}
& $wpr -start $WprProfile -filemode
# Wall-clock bounds, to line the trace up with the game log's .times file (tee_run.py).
"start $(Get-Date -Format 'HH:mm:ss.fff')" | Out-File -Encoding utf8 (Join-Path $dir "$Tag.times.txt")
Write-Host "Recording $Seconds s..."
Start-Sleep -Seconds $Seconds
# -skipPdbGen: no NGEN / embedded PDB generation at the merge (not needed for this game).
# Reading: full CPU-profile traces lose events, and xperf then reports "corrupt" (0x80070570)
# unless given -tle (tolerate lost events) and -tti: xperf -tle -tti -i <etl> ...
"stop $(Get-Date -Format 'HH:mm:ss.fff')" | Out-File -Encoding utf8 -Append (Join-Path $dir "$Tag.times.txt")
& $wpr -stop $etl "MediEvil $Tag" -skipPdbGen
Write-Host "Saved $etl"
