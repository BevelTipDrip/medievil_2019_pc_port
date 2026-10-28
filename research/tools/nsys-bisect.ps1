# Finds which Nsight Systems option hangs 'nsys stop' (the report never saves): each configuration profiles a
# dummy Python program (no game) for 3 s and reports how long the save took, or "HANG" after -TimeoutSec.
# Run from an administrator PowerShell (the options under test need it):
#   powershell -ExecutionPolicy Bypass -File <this file>
# Reports go to research-data\nsys\bisect\ (deleted at the end unless -Keep).
# -NoAdmin: run without elevation (the admin-only options are disabled by nsys; tests the script itself).
# -Only <name>: just that configuration.
param([int]$TimeoutSec = 90, [switch]$Keep, [switch]$NoAdmin, [string]$Only = '')
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $NoAdmin -and -not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this from an administrator PowerShell.'
}
$nsys = 'C:\Program Files\NVIDIA Corporation\Nsight Systems 2026.5.1\target-windows-x64\nsys.exe'
if (Get-Process nsys -ErrorAction SilentlyContinue) { throw 'nsys.exe is still running: end it first (Ctrl+C in its window).' }
$dir = 'C:\Users\Ty\MediEvil\research-data\nsys\bisect'
New-Item -ItemType Directory -Force $dir | Out-Null
$base = @('--sample=none', '--stop-on-exit=false', '--force-overwrite=true')
$configs = [ordered]@{
    'baseline'           = @{ launch = @('--trace=none'); start = @() }
    'gpu-metrics'        = @{ launch = @('--trace=none'); start = @('--gpu-metrics-devices=all', '--gpu-metrics-set=ad10x', '--gpu-metrics-frequency=20000') }
    'gpuctxsw'           = @{ launch = @('--trace=none'); start = @('--gpuctxsw=true') }
    'cpuctxsw'           = @{ launch = @('--trace=none'); start = @('--cpuctxsw=process-tree') }
    'reflex-events'      = @{ launch = @('--trace=none'); start = @('--reflex-events=true') }
    'wddm'               = @{ launch = @('--trace=wddm'); start = @() }
    'wddm-additional'    = @{ launch = @('--trace=wddm', '--wddm-additional-events=true', '--wddm-memory-trace=false'); start = @() }
    # The run-game.ps1 -Nsys set that hung (without the Vulkan trace: the dummy program has no Vulkan).
    'everything'         = @{ launch = @('--trace=wddm', '--wddm-additional-events=true', '--wddm-memory-trace=false')
                              start = @('--gpu-metrics-devices=all', '--gpu-metrics-set=ad10x', '--gpu-metrics-frequency=20000',
                                        '--gpuctxsw=true', '--cpuctxsw=process-tree', '--reflex-events=true') }
    # run-game.ps1 -Nsys after dropping WDDM (the Vulkan trace has no effect on the dummy program).
    'nsys-run-set'       = @{ launch = @('--trace=vulkan', '--vulkan-gpu-workload=individual')
                              start = @('--gpu-metrics-devices=all', '--gpu-metrics-set=ad10x', '--gpu-metrics-frequency=20000',
                                        '--gpuctxsw=true', '--cpuctxsw=process-tree', '--reflex-events=true') }
}
if ($Only) { $configs = [ordered]@{ $Only = $configs[$Only] } }
$results = @()
foreach ($name in $configs.Keys) {
    $cfg = $configs[$name]
    $session = "bisect-$name"
    Write-Host "== $name"
    $launch = @('launch', "--session-new=$session") + $cfg.launch + @('py', '-3.14', '-c', '"import time; time.sleep(600)"')
    Start-Process -FilePath $nsys -ArgumentList $launch -NoNewWindow | Out-Null
    Start-Sleep 4
    # CPU context switches off unless this configuration tests them.
    $cpu = if ($cfg.start -match '^--cpuctxsw') { @() } else { @('--cpuctxsw=none') }
    & $nsys start "--session=$session" @base @cpu @($cfg.start) --output (Join-Path $dir $name) 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) {
        $results += [pscustomobject]@{ Config = $name; Result = "start failed ($LASTEXITCODE)" }
    } else {
        Start-Sleep 3
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $stop = Start-Process -FilePath $nsys -ArgumentList @('stop', "--session=$session") -NoNewWindow -PassThru
        if ($stop.WaitForExit($TimeoutSec * 1000)) {
            # The report can land a moment after 'nsys stop' returns.
            $saved = $false
            for ($i = 0; $i -lt 30 -and -not $saved; $i++) {
                $saved = Test-Path (Join-Path $dir "$name.nsys-rep")
                if (-not $saved) { Start-Sleep 1 }
            }
            $results += [pscustomobject]@{ Config = $name; Result = ("saved in {0:N1} s, report {1}" -f $sw.Elapsed.TotalSeconds, $(if ($saved) { 'present' } else { 'MISSING' })) }
        } else {
            $results += [pscustomobject]@{ Config = $name; Result = "HANG (no save in $TimeoutSec s)" }
        }
    }
    # Clean up this configuration: the dummy program, the session, any nsys left.
    Get-CimInstance Win32_Process -Filter "Name='python.exe'" | Where-Object { $_.CommandLine -match 'time.sleep\(600\)' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    & $nsys shutdown "--session=$session" 2>&1 | Out-Null
    Start-Sleep 2
    Get-Process nsys -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep 1
}
Write-Host ''
$results | Format-Table -AutoSize | Out-Host
$results | Format-Table -AutoSize | Out-String | Out-File -Encoding utf8 (Join-Path $dir 'results.txt')
if (-not $Keep) { Get-ChildItem $dir -Filter *.nsys-rep | Remove-Item -Force }
Write-Host "Results saved: $dir\results.txt"
