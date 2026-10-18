# Starts the game through run.py with research switches, for test runs the user launches (one change
# per launch). Backs up medievil.ini next to it first and prints exactly what this launch changes.
# The log is user\last_run.log as usual; -Tag copies it to research\docs\data\<tag>.log afterwards.
#   -Validation   Khronos validation layer (GOW3_VK_VALIDATION=1): names bad handles; much slower
#   -Samples      GPU-thread sampling profiler (GOW3_SAMPLE_FILE) into research-data\samples\<tag>
#   -TextureHelper  parallel texture binding (GOW3_TEXTURE_HELPER=1)
#   -Env @{NAME='value'}  any other environment variable for this launch
#   -Tag name     name for the saved log / samples (default: run-<time>)
param([switch]$Validation, [switch]$Samples, [switch]$TextureHelper, [hashtable]$Env = @{}, [string]$Tag = '')
$Port = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$Game = 'C:\Users\Ty\MediEvil\Game Files\CUSA11227'
if (-not $Tag) { $Tag = 'run-' + (Get-Date -Format 'yyyy-MM-dd-HHmm') }
if (Get-Process medievil-runtime -ErrorAction SilentlyContinue) { throw 'medievil-runtime.exe is already running.' }

$ini = Join-Path $Port 'medievil.ini'
if (Test-Path $ini) { Copy-Item $ini "$ini.bak-$Tag" }

$set = [ordered]@{ GOW3_GAME_DIR = $Game }
if ($Validation) { $set.GOW3_VK_VALIDATION = '1'; $set.VK_ADD_LAYER_PATH = 'C:\msys64\clang64\bin' }
if ($Samples) {
    $dir = "C:\Users\Ty\MediEvil\research-data\samples\$Tag"
    New-Item -ItemType Directory -Force $dir | Out-Null
    $set.GOW3_SAMPLE_FILE = "$dir\samples"
}
if ($TextureHelper) { $set.GOW3_TEXTURE_HELPER = '1' }
foreach ($k in $Env.Keys) { $set[$k] = [string]$Env[$k] }

Write-Host "Launch '$Tag' (build $(git -C $Port rev-parse --short HEAD)); settings from medievil.ini:"
Get-Content $ini | Where-Object { $_ -match '^(render_resolution|ue4_quality|engine_fps|vsync|async_shaders)=' } |
    ForEach-Object { Write-Host "  $_" }
foreach ($k in $set.Keys) { Write-Host "  env $k=$($set[$k])"; Set-Item "env:$k" $set[$k] }

Push-Location $Port
try { py -3.14 run.py } finally { Pop-Location }

$log = Join-Path $Port 'user\last_run.log'
if (Test-Path $log) {
    $saved = Join-Path $Port "research\docs\data\$Tag.log"
    Copy-Item $log $saved
    Write-Host "Log saved: $saved"
}
