# Live bisect of a visual bug inside one launch: switches groups of optimisations off one group at a
# time (the toggle file, gpu/shim/gow3_toggles.h), each for -Seconds, with all on for -Gap seconds
# between. Beeps N times when group N goes off and once (low) when all are back on, so the user can
# watch the bug instead of a clock. Every change is printed with its time and logged by the runtime
# ("Runtime: disabled optimizations ..."). Start it after run-game.ps1, with the user at the spot.
#   powershell -ExecutionPolicy Bypass -File <this file> -Seconds 20 -Gap 10
param([int]$Seconds = 20, [int]$Gap = 10, [int]$Rounds = 1)
$toggles = 'C:\Users\Ty\MediEvil\research-data\toggles.txt'
# Not here: the in-game menu's live performance switches (ring reuse, cheaper binding, ...): the user
# switches those directly (for the frozen torch of plan-0102.md: none changed it).
$groups = @(
    @{ Name = 'Texture memos (set, binding, view)'; Word1 = (1 -shl 45) -bor 1024 -bor (1 -shl 33); Word2 = 0 },
    @{ Name = 'Render state memo'; Word1 = (1 -shl 44); Word2 = 0 },
    @{ Name = 'Draw preparation (prepared draws and resources)'; Word1 = 4096 -bor 8192; Word2 = 0 },
    @{ Name = 'Constant ring (constants copied as before it)'; Word1 = (1 -shl 36); Word2 = 0 },
    @{ Name = 'Image lookups (find-image cache, image descriptors, access memo)'; Word1 = 128 -bor 32 -bor 512; Word2 = 0 }
)
if (-not (Get-Process medievil-runtime -ErrorAction SilentlyContinue)) { throw 'medievil-runtime is not running' }
function Set-Toggles([uint64]$w1, [uint64]$w2) { Set-Content -Path $toggles -Value "$w1 $w2" -Encoding ascii }
try {
    for ($r = 1; $r -le $Rounds; ++$r) {
        for ($g = 0; $g -lt $groups.Count; ++$g) {
            if (-not (Get-Process medievil-runtime -ErrorAction SilentlyContinue)) { return }
            $group = $groups[$g]
            Set-Toggles ([uint64]$group.Word1) ([uint64]$group.Word2)
            Write-Host "$(Get-Date -Format 'HH:mm:ss') group $($g + 1) OFF: $($group.Name)"
            for ($b = 0; $b -le $g; ++$b) { [console]::Beep(1200, 150); Start-Sleep -Milliseconds 150 }
            Start-Sleep -Seconds $Seconds
            Set-Toggles 0 0
            Write-Host "$(Get-Date -Format 'HH:mm:ss') all on"
            [console]::Beep(500, 400)
            Start-Sleep -Seconds $Gap
        }
    }
} finally {
    Set-Toggles 0 0
    Write-Host 'Toggles back to 0'
}
