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
#   -Nsys         NVIDIA Nsight Systems captures into research-data\nsys\<tag>-N.nsys-rep: Vulkan GPU workloads,
#                 GPU metrics (SM active / issue / occupancy, bandwidth), GPU and CPU context switches, Reflex
#                 events. F9 (system-wide) starts a capture, F9 again stops and saves it; the game keeps running.
#                 Needs an administrator PowerShell (GPU counters and context switches).
#   -Ngfx         NVIDIA Nsight Graphics GPU Trace (per-pass GPU timing, unit throughputs, SM idle / warp stall
#                 reasons) into research-data\ngfx\<tag>: F11 in the game traces 5 frames; F11 again for another.
#                 Needs an administrator PowerShell.
#   -Env @{NAME='value'}  any other environment variable for this launch
#   -Tag name     name for the saved log / samples (default: run-<time>)
param([switch]$Validation, [switch]$Samples, [switch]$TextureHelper, [switch]$Ps4Memory, [switch]$NoStats,
      [switch]$Nsys, [switch]$Ngfx,
      [hashtable]$Env = @{},
      [string]$Tag = '')
$Port = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$Game = 'C:\Users\Ty\MediEvil\Game Files\CUSA11227'
if (-not $Tag) { $Tag = 'run-' + (Get-Date -Format 'yyyy-MM-dd-HHmm') }
if (Get-Process medievil-runtime -ErrorAction SilentlyContinue) { throw 'medievil-runtime.exe is already running.' }

$ini = Join-Path $Port 'medievil.ini'
if (Test-Path $ini) { Copy-Item $ini "$ini.bak-$Tag" }

# GOW3_FRAME_STATS: the per-second Perf / Frame stats / Draw pipe / Stall lines the launcher's runs log.
# -NoStats: none of them (as a launcher run): stage B prints some of these reports itself (T-S2).
$set = [ordered]@{ GOW3_GAME_DIR = $Game }
if (-not $NoStats) { $set.GOW3_FRAME_STATS = '1' }
# Live toggles (gpu/shim/gow3_toggles.h bits): the runtime watches this file only when it is set. Reset to 0 at
# every launch, so a launch never starts with a toggle left from the one before.
$toggles = 'C:\Users\Ty\MediEvil\research-data\toggles.txt'
Set-Content -Path $toggles -Value '0' -Encoding ascii
$set.GOW3_TOGGLE_FILE = $toggles
# The 4K boot (I1), the 4K memory sizes (S2) and occlusion queries off (T-O1) are run.py / ue4_override defaults
# since 2026-10-20 (before, this script set them; the runs T-B1 to T-D2b lacked the 4K boot).
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
# -Ps4Memory: the PS4's memory sizes instead of the 4K defaults (GOW3_PS4_MEMORY=1, A/B runs). -Env GOW3_UE4_CVARS
# win over the defaults on the same name (e.g. 'r.AllowOcclusionQueries=1' for a culling test).
if ($Ps4Memory) { $set.GOW3_PS4_MEMORY = '1' }
# -Ngfx: Nsight Graphics GPU Trace (run.py GOW3_NGFX): F11 in the game traces 5 frames into research-data\ngfx\<tag>;
# Reflex stays on. Needs an administrator PowerShell (GPU counters).
if ($Ngfx) {
    $principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '-Ngfx needs an administrator PowerShell (GPU counters).'
    }
    $set.GOW3_NGFX = "C:\Users\Ty\MediEvil\research-data\ngfx\$Tag"
    New-Item -ItemType Directory -Force $set.GOW3_NGFX | Out-Null
    $set.GOW3_SKIP_TOOL_INFO = '1'
    # The game's console output does not come through ngfx: the game writes it to a file itself.
    $set.GOW3_STDOUT_FILE = "C:\Users\Ty\MediEvil\research-data\ngfx\$Tag\game.log"
}
# -Nsys: Nsight's Vulkan layer (2026.5.1) crashes the boot at vkGetPhysicalDeviceToolProperties (skipped
# by GOW3_SKIP_TOOL_INFO). Reflex off too (the precise limiter stays), so no other untested call goes through it.
if ($Nsys) { $set.GOW3_SKIP_TOOL_INFO = '1'; $set.GOW3_REFLEX = '0' }
$cvars = ''
foreach ($k in $Env.Keys) { $set[$k] = [string]$Env[$k] }
if ($Env.ContainsKey('GOW3_UE4_CVARS')) { $cvars = (@($cvars, [string]$Env.GOW3_UE4_CVARS) | Where-Object { $_ }) -join ';' }
if ($cvars) { $set.GOW3_UE4_CVARS = $cvars }

Write-Host "Launch '$Tag' (build $(git -C $Port rev-parse --short HEAD)); settings from medievil.ini:"
Get-Content $ini | Where-Object { $_ -match '^(output_resolution|render_scale|display_mode|fps_limit|ue4_quality|engine_fps|vsync|present_limiter|async_shaders)=' } |
    ForEach-Object { Write-Host "  $_" }
foreach ($k in $set.Keys) { Write-Host "  env $k=$($set[$k])"; Set-Item "env:$k" $set[$k] }

$log = Join-Path $Port "research\docs\data\$Tag.log"
Push-Location $Port
$tee = @((Join-Path $PSScriptRoot 'tee_run.py'), $log, 'py', '-3.14', 'run.py')
if (-not $Nsys) {
    try { py -3.14 @tee } finally { Pop-Location }
    Write-Host "Log saved: $log"
    return
}

# -Nsys: 'nsys launch' starts the game inside a session that records nothing yet (Vulkan trace follows
# py -> tee_run.py -> run.py -> medievil-runtime.exe); F9, a system-wide hotkey here (Nsight's own hotkey
# never reached the game through the launcher chain), runs 'nsys start' and the next F9 'nsys stop'. Each pair
# saves research-data\nsys\<tag>-N.nsys-rep. tee_run.py runs inside the session: nsys does not pass the
# game's console output through, so the log is written by tee_run.py itself.
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Pop-Location
    throw '-Nsys needs an administrator PowerShell (GPU counters, context switches).'
}
$nsysExe = 'C:\Program Files\NVIDIA Corporation\Nsight Systems 2026.5.1\target-windows-x64\nsys.exe'
$dir = 'C:\Users\Ty\MediEvil\research-data\nsys'
New-Item -ItemType Directory -Force $dir | Out-Null
$session = "medievil-$Tag"
# No WDDM trace: with it, 'nsys stop' never finishes saving (2026.5.1, elevated; nsys-bisect.ps1, 2026-10-27).
$launchArgs = @('launch', "--session-new=$session", '--trace=vulkan', '--vulkan-gpu-workload=individual',
                'py', '-3.14') + $tee
# CPU sampling off (WPR covers it): less overhead on the frames being measured.
$startArgs = @('start', "--session=$session", '--gpu-metrics-devices=all', '--gpu-metrics-set=ad10x',
               '--gpu-metrics-frequency=20000', '--gpuctxsw=true', '--cpuctxsw=process-tree', '--sample=none',
               '--reflex-events=true', '--stop-on-exit=false', '--force-overwrite=true')
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Gow3NsysKey {
    [StructLayout(LayoutKind.Sequential)]
    public struct MSG { public IntPtr hwnd; public uint message; public UIntPtr wParam; public IntPtr lParam;
                        public uint time; public int x; public int y; }
    [DllImport("user32.dll")] public static extern bool RegisterHotKey(IntPtr hwnd, int id, uint mods, uint vk);
    [DllImport("user32.dll")] public static extern bool UnregisterHotKey(IntPtr hwnd, int id);
    [DllImport("user32.dll")] public static extern bool PeekMessage(out MSG msg, IntPtr hwnd, uint min, uint max,
                                                                    uint remove);
}
'@
if (-not [Gow3NsysKey]::RegisterHotKey([IntPtr]::Zero, 1, 0x4000, 0x78)) {   # MOD_NOREPEAT, VK_F9
    Pop-Location
    throw 'Could not register F9 as a hotkey (another program has it, e.g. wpr-hotkey.ps1).'
}
# Cues through the audio device ([console]::Beep needs a PC speaker, silent here): start, saved, failed.
function Play-Cue([string]$Name) {
    try { (New-Object System.Media.SoundPlayer "C:\Windows\Media\$Name.wav").Play() } catch { }
}
try {
    $game = Start-Process -FilePath $nsysExe -ArgumentList ($launchArgs | ForEach-Object {
        if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -NoNewWindow -PassThru
    Write-Host "  Nsight Systems session '$session': F9 starts a capture, F9 again stops and saves it ($dir\$Tag-N.nsys-rep)."
    $n = 0
    $recording = $false
    $msg = New-Object Gow3NsysKey+MSG
    while (-not $game.HasExited) {
        if (-not [Gow3NsysKey]::PeekMessage([ref]$msg, [IntPtr]::Zero, 0x0312, 0x0312, 1)) {
            Start-Sleep -Milliseconds 50
            continue
        }
        if (-not $recording) {
            $n++
            & $nsysExe @startArgs --output (Join-Path $dir "$Tag-$n")
            if ($LASTEXITCODE -eq 0) {
                $recording = $true
                "start $(Get-Date -Format 'HH:mm:ss.fff')" | Out-File -Encoding utf8 (Join-Path $dir "$Tag-$n.times.txt")
                Write-Host "Capture $n started $(Get-Date -Format 'HH:mm:ss.fff')"
                Play-Cue 'Speech On'
            } else {
                Write-Host "nsys start failed ($LASTEXITCODE)"
                Play-Cue 'Windows Critical Stop'
            }
        } else {
            "stop $(Get-Date -Format 'HH:mm:ss.fff')" | Out-File -Append -Encoding utf8 (Join-Path $dir "$Tag-$n.times.txt")
            Write-Host "Capture $n stopping $(Get-Date -Format 'HH:mm:ss.fff'), saving..."
            & $nsysExe stop "--session=$session"
            $recording = $false
            Play-Cue 'Speech Off'
        }
        # Drop presses made while starting or saving.
        while ([Gow3NsysKey]::PeekMessage([ref]$msg, [IntPtr]::Zero, 0x0312, 0x0312, 1)) { }
    }
    if ($recording) { & $nsysExe stop "--session=$session" }
} finally {
    [Gow3NsysKey]::UnregisterHotKey([IntPtr]::Zero, 1) | Out-Null
    Pop-Location
}
Write-Host "Log saved: $log"
