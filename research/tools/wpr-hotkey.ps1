# Saves a WPR trace from a key press while the game keeps focus: tabbing out changes how the
# desktop window manager presents the game, which hides frame pacing problems.
# Run once from an administrator PowerShell (WPR needs it), before or after launching the game:
#   powershell -ExecutionPolicy Bypass -File <this file> -Key F9 -Tag heavy-episode
# Default (ring): WPR records all the time into its in-memory ring buffer; a press saves what is
# in it, i.e. the seconds BEFORE the press (how many depends on the profile's buffers and the load;
# the trace shows it), then recording restarts. Nothing of an event is lost to start-up time.
# -Forward: a press starts a -Seconds recording instead (wpr-capture.ps1, at once).
# Traces: research-data\wpr\<tag>-1.etl, -2, ...; <tag>-N.times.txt holds the save time. One beep:
# saving / recording; two beeps: saved. Presses while saving are ignored. Ctrl+C here ends it.
param([string]$Key = 'F9', [string]$Tag = 'capture', [string]$WprProfile = 'CPU', [switch]$Forward,
      [int]$Seconds = 30, [int]$MinFreeGB = 10)
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this from an administrator PowerShell (WPR needs it).'
}
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Gow3HotKey {
    [StructLayout(LayoutKind.Sequential)]
    public struct MSG { public IntPtr hwnd; public uint message; public UIntPtr wParam; public IntPtr lParam;
                        public uint time; public int x; public int y; }
    [DllImport("user32.dll")] public static extern bool RegisterHotKey(IntPtr hwnd, int id, uint mods, uint vk);
    [DllImport("user32.dll")] public static extern bool UnregisterHotKey(IntPtr hwnd, int id);
    [DllImport("user32.dll")] public static extern int GetMessage(out MSG msg, IntPtr hwnd, uint min, uint max);
    [DllImport("user32.dll")] public static extern bool PeekMessage(out MSG msg, IntPtr hwnd, uint min, uint max,
                                                                    uint remove);
}
'@
Add-Type -AssemblyName System.Windows.Forms
$WM_HOTKEY = 0x0312
$MOD_NOREPEAT = 0x4000
$vk = [uint32][System.Windows.Forms.Keys]$Key
$dir = 'C:\Users\Ty\MediEvil\research-data\wpr'
New-Item -ItemType Directory -Force $dir | Out-Null
# The ADK's WPR (Windows Performance Toolkit, installed 2026-10-18) if present, else Windows' own.
$adk = 'C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\wpr.exe'
$wpr = if (Test-Path $adk) { $adk } else { 'wpr' }
$capture = Join-Path $PSScriptRoot 'wpr-capture.ps1'
function Start-Ring {
    & $wpr -start $WprProfile
    if ($LASTEXITCODE -ne 0) { throw "wpr -start failed ($LASTEXITCODE)" }
}
if (-not [Gow3HotKey]::RegisterHotKey([IntPtr]::Zero, 1, $MOD_NOREPEAT, $vk)) {
    throw "Could not register $Key as a hotkey (another program has it): pick another with -Key."
}
try {
    if ($Forward) {
        Write-Host "Press $Key in the game to record $Seconds s from then ($WprProfile profile). Ctrl+C here to stop."
    } else {
        & $wpr -cancel 2>$null | Out-Null
        Start-Ring
        Write-Host "Recording into the ring buffer ($WprProfile profile). Press $Key in the game to save the last seconds. Ctrl+C here to stop."
    }
    $n = 0
    $msg = New-Object Gow3HotKey+MSG
    while ([Gow3HotKey]::GetMessage([ref]$msg, [IntPtr]::Zero, 0, 0) -gt 0) {
        if ($msg.message -ne $WM_HOTKEY) { continue }
        $n++
        [console]::Beep(880, 200)
        try {
            if ($Forward) {
                & $capture -Seconds $Seconds -Tag "$Tag-$n" -WprProfile $WprProfile -Delay 0 -MinFreeGB $MinFreeGB
            } else {
                $etl = Join-Path $dir "$Tag-$n.etl"
                "saved $(Get-Date -Format 'HH:mm:ss.fff') (ring buffer: the trace ends here)" |
                    Out-File -Encoding utf8 (Join-Path $dir "$Tag-$n.times.txt")
                & $wpr -stop $etl "MediEvil $Tag-$n" -skipPdbGen
                Write-Host "Saved $etl"
                $free = (Get-PSDrive C).Free / 1GB
                if ($free -lt $MinFreeGB) { throw ("Only {0:N1} GB free on C: not restarting." -f $free) }
                Start-Ring
            }
            [console]::Beep(660, 150); [console]::Beep(660, 150)
        } catch {
            Write-Host "Capture failed: $_"
            [console]::Beep(300, 600)
        }
        # Drop presses made while saving.
        while ([Gow3HotKey]::PeekMessage([ref]$msg, [IntPtr]::Zero, $WM_HOTKEY, $WM_HOTKEY, 1)) { }
        Write-Host "Ready: press $Key for the next capture."
    }
} finally {
    [Gow3HotKey]::UnregisterHotKey([IntPtr]::Zero, 1) | Out-Null
    if (-not $Forward) { & $wpr -cancel 2>$null | Out-Null }
}
