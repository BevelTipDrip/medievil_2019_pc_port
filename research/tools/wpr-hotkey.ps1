# Saves a WPR trace from a key press while the game keeps focus: tabbing out changes how the
# desktop window manager presents the game, which hides frame pacing problems.
# Run once from an administrator PowerShell (WPR needs it), before or after launching the game:
#   powershell -ExecutionPolicy Bypass -File <this file> -Key F8 -Tag heavy-episode
# Not F9 or F10 with MediEvil: the game uses them (UE4 upscaler, motion vector view).
# Frame-time jitter: -WprProfile <tools>\wpr-jitter.wprp!Jitter. The default CPU profile walks the stack
# on every context switch (~10% of the game's CPU at 237 FPS) and makes the jitter it should measure.
# Default (ring): WPR records all the time into its in-memory ring buffer; a press saves what is
# in it, i.e. the seconds BEFORE the press (how many depends on the profile's buffers and the load;
# the trace shows it), then recording restarts. Nothing of an event is lost to start-up time.
# -Forward: a press starts a -Seconds recording instead (wpr-capture.ps1, at once).
# Traces: research-data\wpr\<tag>-<yyyyMMdd-HHmmss>.etl, one per press, never overwriting an earlier one
# (Get-UniqueTraceName, wpr-common.ps1; through its real path); <name>.times.txt holds the save time. One beep: saving / recording; two beeps: saved; one low
# beep: failed (the message says why; recording restarts). Presses while saving are ignored.
# Ctrl+C here ends it.
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
. (Join-Path $PSScriptRoot 'wpr-common.ps1')
$dir = $TraceDir
Write-Host "Traces go to $dir"
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
                & $capture -Seconds $Seconds -Tag $Tag -WprProfile $WprProfile -Delay 0 -MinFreeGB $MinFreeGB
            } else {
                $name = Get-UniqueTraceName $Tag
                $etl = Join-Path $dir "$name.etl"
                "saved $(Get-Date -Format 'HH:mm:ss.fff') (ring buffer: the trace ends here)" |
                    Out-File -Encoding utf8 (Join-Path $dir "$name.times.txt")
                & $wpr -stop $etl "MediEvil $name" -skipPdbGen
                if ($LASTEXITCODE -ne 0) {
                    # A failed stop leaves the profile running: cancel it, or every later start fails
                    # with "The profiles are already running" (0xc5583001).
                    $code = $LASTEXITCODE
                    "stop failed ($code)" | Out-File -Encoding utf8 -Append (Join-Path $dir "$name.times.txt")
                    & $wpr -cancel 2>$null | Out-Null
                    Start-Ring
                    throw "wpr -stop failed ($code): nothing saved; recording restarted."
                }
                Write-Host "Saved $etl"
                $free = Get-TraceFreeGB
                if ($free -lt $MinFreeGB) { throw ("Only {0:N1} GB free on {1} not restarting." -f $free, $dir.Substring(0, 2)) }
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
