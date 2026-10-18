# Records a whole-system CPU sampling trace (every thread, with stacks) with Windows Performance
# Recorder while the game runs: the port's own sampler only covers its GPU threads, not the game's.
# Needs an elevated (administrator) PowerShell: the user runs it. It never starts or stops the game.
#   powershell -ExecutionPolicy Bypass -File <this file> -Seconds 20 -Tag graveyard
# Start it once the game is at the scene to measure; it waits 5 s (time to switch back), records,
# and writes C:\Users\Ty\MediEvil\research-data\wpr\<tag>.etl.
# The full CPU profile (stacks) writes ~1 GB per 15-45 s and the Windows 8.1 xperf here cannot read it
# (reports 0x80070570, corrupt): light profile, 20 s and a free-space check by default.
param([int]$Seconds = 20, [string]$Tag = 'capture', [string]$WprProfile = 'CPU.light', [int]$MinFreeGB = 10)
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
Write-Host 'Recording starts in 5 s: switch back to the game.'
Start-Sleep -Seconds 5
& $wpr -start $WprProfile -filemode
Write-Host "Recording $Seconds s..."
Start-Sleep -Seconds $Seconds
# -skipPdbGen: no NGEN / embedded PDB generation at the merge. Both full CPU-profile traces came
# out unreadable (0x80070570, also to the current xperf) while light ones read fine; that step
# is the suspect.
& $wpr -stop $etl "MediEvil $Tag" -skipPdbGen
Write-Host "Saved $etl"
