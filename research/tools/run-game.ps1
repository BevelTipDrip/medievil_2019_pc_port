# Starts the game through run.py with research switches, for test runs the user launches (one change
# per launch). Backs up medievil.ini next to it first and prints exactly what this launch changes.
# run.py only prints (the launcher is what writes user\last_run.log): the output is shown here and
# written to research\docs\data\<tag>.log by tee_run.py. Launcher-only settings (Two-stage GPU
# pipeline, GPU readbacks, patch selection) are not applied: their defaults are, or pass them by -Env.
# The user's PowerShell blocks scripts: run it as
#   powershell -ExecutionPolicy Bypass -Command "& '<this file>' -Env @{NAME='value'} -Tag name"
#   -Validation   Khronos validation layer (GOW3_VK_VALIDATION=1): names bad handles; much slower
#   -Samples      GPU-thread sampling profiler (GOW3_SAMPLE_FILE) into research-data\samples\<tag>
#   -TextureHelper  parallel texture binding (GOW3_TEXTURE_HELPER=1)
#   -Ps4Memory    the PS4 memory sizes instead of the 4K defaults below (A/B runs)
#   -Env @{NAME='value'}  any other environment variable for this launch
#   -Tag name     name for the saved log / samples (default: run-<time>)
param([switch]$Validation, [switch]$Samples, [switch]$TextureHelper, [switch]$Ps4Memory, [hashtable]$Env = @{},
      [string]$Tag = '')
$Port = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$Game = 'C:\Users\Ty\MediEvil\Game Files\CUSA11227'
if (-not $Tag) { $Tag = 'run-' + (Get-Date -Format 'yyyy-MM-dd-HHmm') }
if (Get-Process medievil-runtime -ErrorAction SilentlyContinue) { throw 'medievil-runtime.exe is already running.' }

$ini = Join-Path $Port 'medievil.ini'
if (Test-Path $ini) { Copy-Item $ini "$ini.bak-$Tag" }

# GOW3_FRAME_STATS: the per-second Perf / Frame stats / Draw pipe / Stall lines the launcher's runs log.
$set = [ordered]@{ GOW3_GAME_DIR = $Game; GOW3_FRAME_STATS = '1' }
# Live toggles (gpu/shim/gow3_toggles.h bits): the runtime watches this file only when it is set. Reset to 0 at
# every launch, so a launch never starts with a toggle left from the one before.
$toggles = 'C:\Users\Ty\MediEvil\research-data\toggles.txt'
Set-Content -Path $toggles -Value '0' -Encoding ascii
$set.GOW3_TOGGLE_FILE = $toggles
# I1 (menu navigation at 4K): boot at 4K like a PS4 Pro, as research\tools\launcher.cmd does. Without it the runs
# from T-B1 to T-D2b (2026-10-19) lacked the '4K display metrics at boot' patch and the menus broke again.
$set.GOW3_UE4_BOOT_4K = '1'
if ($Validation) { $set.GOW3_VK_VALIDATION = '1'; $set.VK_ADD_LAYER_PATH = 'C:\msys64\clang64\bin' }
if ($Samples) {
    $dir = "C:\Users\Ty\MediEvil\research-data\samples\$Tag"
    New-Item -ItemType Directory -Force $dir | Out-Null
    $set.GOW3_SAMPLE_FILE = "$dir\samples"
    # The samples are offsets into this exact exe: keep it (and its PDB) with them.
    Copy-Item (Join-Path $Port 'out\medievil-runtime.exe') $dir
    if (Test-Path (Join-Path $Port 'out\medievil-runtime.pdb')) { Copy-Item (Join-Path $Port 'out\medievil-runtime.pdb') $dir }
}
if ($TextureHelper) { $set.GOW3_TEXTURE_HELPER = '1' }
# 4K defaults for test runs (2026-10-19, the user): the PS4's 400-800 MB texture streaming pool thrashes at 4K
# (S2: 2 GiB more guest memory, a 4600 MB GPU heap and a 1600 MB pool stop small pans re-streaming). -Env
# GOW3_UE4_CVARS are appended after these, so they add to them and win on the same name.
$cvars = if ($Ps4Memory) { '' } else { $set.GOW3_DMEM_MB = '7104'; 'r.GarlicHeapSizeMB=4600;r.Streaming.PoolSize=1600' }
foreach ($k in $Env.Keys) { $set[$k] = [string]$Env[$k] }
if ($Env.ContainsKey('GOW3_UE4_CVARS')) { $cvars = (@($cvars, [string]$Env.GOW3_UE4_CVARS) | Where-Object { $_ }) -join ';' }
if ($cvars) { $set.GOW3_UE4_CVARS = $cvars }

Write-Host "Launch '$Tag' (build $(git -C $Port rev-parse --short HEAD)); settings from medievil.ini:"
Get-Content $ini | Where-Object { $_ -match '^(render_resolution|ue4_quality|engine_fps|vsync|async_shaders)=' } |
    ForEach-Object { Write-Host "  $_" }
foreach ($k in $set.Keys) { Write-Host "  env $k=$($set[$k])"; Set-Item "env:$k" $set[$k] }

$log = Join-Path $Port "research\docs\data\$Tag.log"
Push-Location $Port
try { py -3.14 (Join-Path $PSScriptRoot 'tee_run.py') $log py -3.14 run.py } finally { Pop-Location }
Write-Host "Log saved: $log"
