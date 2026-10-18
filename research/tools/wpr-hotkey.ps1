# Starts a WPR capture (wpr-capture.ps1) from a key press while the game keeps focus: tabbing out
# changes how the desktop window manager presents the game, which hides frame pacing problems.
# Run once from an administrator PowerShell (WPR needs it), before or after launching the game:
#   powershell -ExecutionPolicy Bypass -File <this file> -Key F9 -Seconds 30 -Tag heavy-episode
# Each press of the key records -Seconds at once, as <tag>-1, <tag>-2, ... One beep: recording;
# two beeps: saved. Presses during a capture are ignored. Ctrl+C in this window ends it.
param([string]$Key = 'F9', [int]$Seconds = 30, [string]$Tag = 'capture', [string]$WprProfile = 'CPU')
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
$WM_HOTKEY = 0x0312
$MOD_NOREPEAT = 0x4000
Add-Type -AssemblyName System.Windows.Forms
$vk = [uint32][System.Windows.Forms.Keys]$Key
if (-not [Gow3HotKey]::RegisterHotKey([IntPtr]::Zero, 1, $MOD_NOREPEAT, $vk)) {
    throw "Could not register $Key as a hotkey (another program has it): pick another with -Key."
}
$capture = Join-Path $PSScriptRoot 'wpr-capture.ps1'
Write-Host "Press $Key in the game to record $Seconds s ($WprProfile profile). Ctrl+C here to stop."
try {
    $n = 0
    $msg = New-Object Gow3HotKey+MSG
    while ([Gow3HotKey]::GetMessage([ref]$msg, [IntPtr]::Zero, 0, 0) -gt 0) {
        if ($msg.message -ne $WM_HOTKEY) { continue }
        $n++
        [console]::Beep(880, 200)
        try {
            & $capture -Seconds $Seconds -Tag "$Tag-$n" -WprProfile $WprProfile -Delay 0
            [console]::Beep(660, 150); [console]::Beep(660, 150)
        } catch {
            Write-Host "Capture failed: $_"
            [console]::Beep(300, 600)
        }
        # Drop presses made while recording.
        while ([Gow3HotKey]::PeekMessage([ref]$msg, [IntPtr]::Zero, $WM_HOTKEY, $WM_HOTKEY, 1)) { }
        Write-Host "Ready: press $Key for the next capture."
    }
} finally {
    [Gow3HotKey]::UnregisterHotKey([IntPtr]::Zero, 1) | Out-Null
}
