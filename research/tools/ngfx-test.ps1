# Checks Nsight Graphics' command line before a game launch: that a program it launches (--exe) inherits this
# shell's environment (run.py passes the game's settings that way), and how --args survives (ngfx splits it on
# spaces; the game's arguments include "Game Files"). No game: the launched program is a small Python script.
#   powershell -ExecutionPolicy Bypass -File <this file>
# Notes: no --platform (Qt takes it as its own option and ngfx fails to start).
$ngfx = 'C:\Program Files\NVIDIA Corporation\Nsight Graphics 2026.3.1\host\windows-desktop-nomad-x64\ngfx.exe'
$out = 'C:\Users\Ty\MediEvil\research-data\ngfx'
New-Item -ItemType Directory -Force $out | Out-Null
# The script records its arguments and one variable; its path has no spaces.
$script = Join-Path $out 'argtest.py'
@'
import os, sys
tag = sys.argv[1]
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), f'result-{tag}.txt'), 'w') as f:
    f.write(f'argv={sys.argv[2:]!r} env={os.environ.get("GOW3_NGFX_TEST")!r}\n')
'@ | Set-Content -Encoding ascii $script
Get-ChildItem $out -Filter 'result-*.txt' | Remove-Item
$env:GOW3_NGFX_TEST = 'inherited; with a semicolon'
$py = (& py -3.14 -c 'import sys; print(sys.executable)')
$styles = [ordered]@{
    plain   = "$script plain C:\Game Files\x"
    quoted  = "$script quoted `"C:\Game Files\x`""
    escaped = "$script escaped \`"C:\Game Files\x\`""
}
foreach ($tag in $styles.Keys) {
    Write-Host "== $tag : --args $($styles[$tag])"
    & $ngfx --activity 'GPU Trace Profiler' --exe $py --args $styles[$tag] --launch-detached | Out-Null
    Write-Host "   exit code $LASTEXITCODE"
}
Start-Sleep 5
foreach ($tag in $styles.Keys) {
    $r = Join-Path $out "result-$tag.txt"
    if (Test-Path $r) { Write-Host "$tag -> $(Get-Content $r)" } else { Write-Host "$tag -> did not run" }
}
