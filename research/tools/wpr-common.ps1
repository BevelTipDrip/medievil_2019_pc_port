# Shared by wpr-capture.ps1 and wpr-hotkey.ps1 (dot-sourced): where traces go, and WPR itself.
#
# WPR's -stop rejects a results path that passes through a junction: "Invalid results path passed for
# starting the trace", 0xc5600600, and nothing is saved (2026-11-15). C:\Users\Ty\MediEvil\research-data
# became a junction to D:\MediEvil-archive\research-data after the last capture that saved (stall-1,
# 2026-10-30), so the traces folder is resolved to its real path first.

# The path with every junction or symbolic link along it replaced by its target.
function Get-RealPath([string]$Path) {
    $parts = $Path.TrimEnd('\').Split('\')
    $real = $parts[0] + '\'
    foreach ($part in $parts[1..($parts.Length - 1)]) {
        $real = Join-Path $real $part
        $item = Get-Item -LiteralPath $real -Force -ErrorAction SilentlyContinue
        if ($item -and ($item.LinkType -eq 'Junction' -or $item.LinkType -eq 'SymbolicLink')) {
            $real = [string]($item.Target | Select-Object -First 1)
        }
    }
    $real
}

$TraceDir = 'C:\Users\Ty\MediEvil\research-data\wpr'
New-Item -ItemType Directory -Force $TraceDir | Out-Null
$TraceDir = Get-RealPath $TraceDir

# A trace name no earlier capture has: <tag>-<yyyyMMdd-HHmmss>, with -2, -3 ... if that exists too. Captures
# are never overwritten (2026-11-25: a reused tag restarted the numbering at 1 and replaced an earlier session's
# trace). Returns the base name; <base>.etl and <base>.times.txt go into $TraceDir.
function Get-UniqueTraceName([string]$Tag) {
    $base = "$Tag-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
    $name = $base
    $n = 2
    while ((Test-Path (Join-Path $TraceDir "$name.etl")) -or (Test-Path (Join-Path $TraceDir "$name.times.txt"))) {
        $name = "$base-$n"
        $n++
    }
    $name
}

# Free space where the traces go (GB).
function Get-TraceFreeGB { (Get-PSDrive $TraceDir.Substring(0, 1)).Free / 1GB }

# The ADK's WPR (Windows Performance Toolkit, installed 2026-10-18) if present, else Windows' own.
$adk = 'C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\wpr.exe'
$wpr = if (Test-Path $adk) { $adk } else { 'wpr' }
