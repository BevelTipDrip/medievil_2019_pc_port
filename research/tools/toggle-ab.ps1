# A/B of one live toggle inside one launch: alternates the toggle file between "on" (bit clear) and
# "off" (bit set) every -Seconds while the game runs, and stops (file back to 0) when it exits. The
# runtime logs every change ("Runtime: disabled optimizations ..."), so the run log's stats windows
# can be split by state. Start it after run-game.ps1 (which resets the file to 0 at launch).
#   powershell -ExecutionPolicy Bypass -File <this file> -Word 2 -Bit 1 -Seconds 20
# -Word 1: the first number of the file (gow3_toggles.h Gow3Toggle), 2: the second (Gow3Toggle2).
param([ValidateSet(1, 2)][int]$Word = 2, [uint64]$Bit = 1, [int]$Seconds = 20)
$toggles = 'C:\Users\Ty\MediEvil\research-data\toggles.txt'
$started = $false
for ($i = 0; $i -lt 900; ++$i) {   # wait up to 15 minutes for the game
    if (Get-Process medievil-runtime -ErrorAction SilentlyContinue) { $started = $true; break }
    Start-Sleep -Seconds 1
}
if (-not $started) { throw 'medievil-runtime did not start' }
$off = $false
while (Get-Process medievil-runtime -ErrorAction SilentlyContinue) {
    $value = if ($off) { $Bit } else { 0 }
    $text = if ($Word -eq 1) { "$value 0" } else { "0 $value" }
    Set-Content -Path $toggles -Value $text -Encoding ascii
    Write-Host "$(Get-Date -Format 'HH:mm:ss.fff') toggles '$text' ($(if ($off) { 'off' } else { 'on' }))"
    Start-Sleep -Seconds $Seconds
    $off = -not $off
}
Set-Content -Path $toggles -Value '0' -Encoding ascii
Write-Host 'Game exited; toggles back to 0'
