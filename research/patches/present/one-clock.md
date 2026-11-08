---
id: one-clock
title: One-clock limiter: the tick derived from the frame limit, frames released on ticks only (v1, v2 fixed grid)
area: present
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: db7fa8fc33, e6f88f4f84
files: gpu/shadps4/core/libraries/videoout/driver.cpp
symbols: PresentThread, one_clock, one_clock_ticks, set_one_clock, tick_period, grid_start, grid_tick, last_release_tick, redraw_idle
switches: GOW3_ONE_CLOCK
tests: T-F13, T-F14, T-F15
docs: fix-inventory.md#10-tried-and-dropped, plan-stage-b-split.md#t-f2-which-barriers-break-the-passes-log-t-f2-barrier-breaks-trace-33
depends_on: precise-tick
conflicts_with: vblank-sync, newest-frame
related: precise-limiter, reflex, inline-present, present-side-limiter, upstream-vsync-limit-rule, frame-limit-parse
checked: 2026-11-07
---
## What it does

`GOW3_ONE_CLOCK=1` (with `GOW3_PRECISE_TICK=1` and a frame limit) makes the vblank tick the only clock:
`set_one_clock` sets N = max(1, round(vblank Hz / limit)) ticks a frame and a tick of 1 s / (limit x N)
(960 / 120 -> 8 ticks of 1041.667 us; a 119 limit gives a 952 Hz tick). Frames are released on ticks only, on the
Nth tick after the last release, or on the first tick a late frame arrives. v1 (`db7fa8f`) counted ticks since the
last flip; v2 (`e6f88f4`) puts tick k at `grid_start + k * tick_period`, never re-anchored, and counts slots in grid
ticks (`grid_tick >= last_release_tick + one_clock_ticks`). Off by default; the code stays behind the switch.

## Invariants

- Engages only if `one_clock && precise_tick && limit` in `set_one_clock`; any other state leaves
  `one_clock_ticks = 0` and the normal tick / slot paths. `set_one_clock` runs again on every limit change
  (in-game menu, toggle-file cap) and resets `grid_start`, `grid_tick`, `last_release_tick`.
- `grid_tick` advances only in the `precise_tick && one_clock_ticks && frame_limit` wait branch. With
  `GOW3_VBLANK_SYNC=1` the `display_vblank->Wait()` branch is taken first, `grid_tick` stays 0 and the release
  test never passes: no frame is ever flipped (code reading, never run). Do not combine the two.
- While it is on, the immediate-flip loop after the vblank events is skipped (`one_clock_ticks && frame_limit` in
  the `continue` condition): frames must never leave between ticks, or there are two clocks again.
- The guest's vblank counter and events advance once per loop pass, so with one clock they run at limit x N Hz
  (952 Hz at 119), not at the configured tick rate.
- A missed tick skips to the next grid point with the same phase (v2); only `set_one_clock` moves the grid.
- The release branch does not test `vblank_status.count % (main_port.flip_rate + 1)`: a guest flip rate is ignored
  while one clock is on (MediEvil sets none that we know of; another title would need it).

## Interactions

- precise-tick: required (`precise_tick`); its own `next_tick = max(next + period, now - period)` re-anchor is what
  moved v1's grid (T-F14). The Deck preset has `present_precise_tick=0`, so one clock cannot engage there.
- reflex: inert under it (`current_limit` returns 0 with Reflex, so `frame_limit` is 0). Without
  `VK_NV_low_latency2` (AMD, Intel) `reflex` is false and the Reflex preset is the Precise limiter, so it can engage. Reflex is how the
  "one clock" idea of T-F13 was finally met: the driver paces and our limiter steps aside.
- upstream-vsync-limit-rule: with VSync on and a limit >= refresh - 1 the limit is 0, so one clock is off.
- precise-limiter: the one-clock branch comes before the `flip_slot || precise_capped` branch, so the precise
  slots and `GOW3_LIMIT_HOLD` never run while it is on.
- inline-present: with the present on the tick thread a release tick holds the 0.3-0.5 ms present plus the vblank
  events inside a 1.04 ms tick (the T-F14 overruns).
- vblank-sync: the combination never releases a frame (Invariants).
- newest-frame (conflict, code reading): with `GOW3_NEWEST_FRAME=1` the pacer thread takes requests
  (`pacer_limit = frame_limit`) while the one-clock branch also calls `receive_request`: two releasers, two clocks.

## Evidence

- T-F13 (motivation, RTSS limiter at 119, ours at 120 never waits): present returns 8.402 / 8.404 ms (1% / 99%), 1% / 0.1% lows
  118.8-119.0 / 117.0-118.9, no flicker; RTSS at 120 flickered (two limiters at one rate fight).
- T-F14 (v1, RTSS off, trace 45): rejected. The user: the worst of the night, 1% lows 90-107, 0.1% ~110, many
  stutters. Release gaps of 8 ticks 3,523, 7 ticks 41, 9 ticks 36; releases 7.82 / 8.85 ms.
- T-F15 (v2, RTSS off, trace 46): rejected. The user: lows better than v1, not RTSS's locked pace, real frame-time
  dips became full stalls. Still 7- and 9-tick gaps in equal numbers (11-26 each per 10 s; 7.56-7.83 / 8.84-9.11 ms).
- Not tested on the Deck or with Reflex.

## History and pitfalls

- v1's gaps came from the grid re-anchoring on overrun ticks; v2 fixed that, and the remaining equal 7/9 gaps mean
  the tick thread itself ran about a tick late: `SetCurrentThreadRealtime` is a no-op on Windows (normal priority).
- That led to `GOW3_PRESENT_PRIORITY` (`BoostPresentingThread`, T-F16) and then to Reflex (T-F17/T-F18). The
  Reflex and Precise presets now set `GOW3_PRESENT_PRIORITY=1` (Standard does not; Windows only), so the T-F15 cause may be gone; one clock was not re-run.
- Worth retrying only with: the present off the tick thread (`present_inline=0`) or N = 2 at 240 Hz, present
  priority on, the Precise or Standard limiter (not Reflex), and a flip trace to count release gaps in ticks.
