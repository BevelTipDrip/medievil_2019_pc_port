---
id: vblank-sync
title: Vblank tick locked to the display's vblank (GOW3_VBLANK_SYNC)
area: present
status: rejected
platforms: windows
default_windows: off
default_deck: n/a
origin: ours
class: G
commits: c32a18e770
files: gpu/shadps4/core/libraries/videoout/driver.cpp
symbols: DisplayVblank, display_vblank, WaitForVBlank, Gow3DisplayRefreshHz
switches: GOW3_VBLANK_SYNC
tests: T-V120, T-V120s
docs: plan-stage-b-split.md#t-v120-120-hz-tick-vsync-fifo-no-limiter-50-ue4_upscaleperformance-2026-10-23-launched-by-claude-with-the-users-go-f4ffc01-gow3_vblank_hz120-trace-flip-trace-8csv, fix-inventory.md#10-tried-and-dropped
depends_on:
related: precise-tick, one-clock, vrr-auto-limits
verified: 2026-11-07
---
## What it does

`GOW3_VBLANK_SYNC=1`: `DisplayVblank::Create` finds the DXGI output of the primary monitor (the one whose refresh SDL
reports). The vblank loop in `PresentThread` then waits on `IDXGIOutput::WaitForVBlank` for every Nth display vblank
(N = round(refresh / tick Hz)) instead of on the PC clock. The aim was the T-V120 waves (12.5 / 4.17 ms frame pairs),
then read as a beat between a PC-clock tick and a fixed refresh. Windows only: on Linux, and without a matching output,
`Create` returns null and the PC clock paces. Off by default; the code stays behind the switch, out of the launcher
since T-F27.

## Invariants

- The `display_vblank` branch is checked first in the vblank loop, so it overrides the precise tick and one-clock.
  With one-clock on, `grid_tick` never advances and no frame is released (see one-clock).
- N assumes a fixed refresh. Under VRR, `WaitForVBlank` has no steady vblank to wait for: the display refreshes when
  a frame arrives, so the tick ties itself to its own presents.
- `display_vblank` is a function-local static: created once, never freed.
- Only the non-immediate path was run (T-V120s: no limiter). With a cap or Reflex each pass first waits on the PC
  clock until `tick_deadline` (now + tick period), then `Wait()` waits N more vblanks, so a tick would span one to
  two tick periods [code reading].

## Interactions

- precise-tick: the replacement for the T-V120 problem (a fixed-grid PC clock).
- one-clock: never combine with it (no releases).
- vrr-auto-limits: the VRR display behaviour that defeats this switch is also behind the half-refresh rule.

## Evidence

- T-V120s (`c32a18e`, G-Sync on, confirmed by the indicator): log "tick on every 2 of the display's vblanks (240 Hz)".
  Presented median 19 ms (~52 FPS), cycling ~14.5 / 27.7 / 19 ms. The user: the videos stutter badly, main menu 1% low
  1.7 FPS. Rejected.
- It also showed the T-V120 explanation was wrong: with G-Sync on there is no fixed refresh to beat against. Why the
  T-V120 presents snapped to a 4.167 ms grid stayed open; the baseline recheck could not reproduce it.

## History and pitfalls

- Only on a fixed-refresh display (G-Sync / FreeSync off) could a display-locked tick make sense, and no such run was
  made. To retry it, the run needs VRR off, confirmed, plus a flip trace and a measured display-side time
  (PresentMon), because the present calls do not show the display.
