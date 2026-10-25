# Records MediEvil's presents with PresentMon 2.5.1 (C:\mgspf_tools\pw\presentmon, used read-only) to
# C:\Users\Ty\MediEvil\research-data\pm-<tag>.csv. Never starts or stops the game.
# Tracing needs administrator rights: PresentMon relaunches itself elevated in its own console window,
# which takes focus; the delay leaves time to click back into the game before recording starts.
#   powershell -ExecutionPolicy Bypass -File presentmon-capture.ps1 -Tag reflex [-Seconds 30] [-Delay 10]
param([string]$Tag = 'run', [int]$Seconds = 30, [int]$Delay = 10)
$ErrorActionPreference = 'Stop'
$exe = 'C:\mgspf_tools\pw\presentmon\PresentMon-2.5.1-x64.exe'
$csv = "C:\Users\Ty\MediEvil\research-data\pm-$Tag.csv"
if (-not (Get-Process medievil-runtime -ErrorAction SilentlyContinue)) { Write-Host 'The game is not running; nothing recorded.'; exit 1 }
$arguments = @('--process_name', 'medievil-runtime.exe', '--output_file', "`"$csv`"", '--v1_metrics',
               '--stop_existing_session', '--restart_as_admin', '--delay', $Delay, '--timed', $Seconds,
               '--terminate_after_timed')
Start-Process -FilePath $exe -ArgumentList $arguments | Out-Null
Write-Host "PresentMon started: recording $Seconds s after a $Delay s delay; CSV: $csv"
