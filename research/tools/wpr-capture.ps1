# Records a whole-system CPU sampling trace (every thread, with stacks) with Windows Performance
# Recorder while the game runs: the port's own sampler only covers its GPU threads, not the game's.
# Needs an elevated (administrator) PowerShell: the user runs it. It never starts or stops the game.
#   powershell -ExecutionPolicy Bypass -File <this file> -Seconds 45 -Tag graveyard
# Start it once the game is at the scene to measure; it waits 5 s (time to switch back), records,
# and writes C:\Users\Ty\MediEvil\research-data\wpr\<tag>.etl.
param([int]$Seconds = 45, [string]$Tag = 'capture')
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this from an administrator PowerShell (WPR needs it).'
}
if (-not (Get-Process medievil-runtime -ErrorAction SilentlyContinue)) { throw 'The game is not running.' }
$dir = 'C:\Users\Ty\MediEvil\research-data\wpr'
New-Item -ItemType Directory -Force $dir | Out-Null
$etl = Join-Path $dir "$Tag.etl"
wpr -cancel 2>$null | Out-Null
Write-Host 'Recording starts in 5 s: switch back to the game.'
Start-Sleep -Seconds 5
wpr -start CPU -filemode
Write-Host "Recording $Seconds s..."
Start-Sleep -Seconds $Seconds
wpr -stop $etl "MediEvil $Tag"
Write-Host "Saved $etl"
