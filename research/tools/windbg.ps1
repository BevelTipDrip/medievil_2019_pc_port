# Attaches WinDbg to the running medievil-runtime.exe with research/tools/windbg-medievil.txt
# (symbols, first-chance AV ignored, break on the runtime's fatal_exception), then lets it run.
# It never starts or stops the game. Symbols for the port's own code need the PDB lab build
# (research/tools/build-pdb.sh --install) to be the exe that is running.
#   -Cdb   console debugger (cdb.exe) in this window instead of the WinDbg GUI
param([switch]$Cdb)
$Debuggers = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64'
$Script = Join-Path $PSScriptRoot 'windbg-medievil.txt'
$game = Get-Process medievil-runtime -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $game) { throw 'medievil-runtime.exe is not running.' }
$pdb = [IO.Path]::ChangeExtension($game.Path, '.pdb')
if (-not (Test-Path $pdb)) {
    Write-Warning "No $pdb beside the running exe: port symbols unavailable (build-pdb.sh --install)."
} elseif ((Get-Item $pdb).LastWriteTime -lt (Get-Item $game.Path).LastWriteTime.AddMinutes(-1)) {
    Write-Warning "$pdb is older than the running exe: it may not match."
}
New-Item -ItemType Directory -Force 'C:\Users\Ty\MediEvil\research-data\symbols' | Out-Null
$exe = if ($Cdb) { 'cdb.exe' } else { 'windbg.exe' }
Write-Host "Attaching $exe to medievil-runtime.exe (pid $($game.Id))"
& (Join-Path $Debuggers $exe) -p $game.Id -c "`$`$>< $Script; g"
