---
id: precise-limiter
title: Precise frame limiter: high-resolution timer + 0.7 ms spin, frames released only at their slot; hold; present priority
area: present
status: active
platforms: all
default_windows: mixed
default_deck: off
origin: ours
class: G
commits: 52319839bf, c552021880, c67b7e1585, 5ecf53bec9, 6353aaa180, 6e957349cf, 0c357bc1cb
files: gpu/shadps4/core/libraries/videoout/driver.cpp, scripts/graphics_options.py
symbols: PreciseWait, BoostPresentingThread, precise_limit, precise_capped, limit_hold, slot_spin, slot_slack, next_flip, wait_slot, PRESENT_LIMITERS
switches: GOW3_PRECISE_LIMIT, GOW3_LIMIT_HOLD, GOW3_PRESENT_PRIORITY, GOW3_PACER_SPIN_US, present_limiter
tests: T-L1b, T-L2, T-L3, T-L4, T-F7, T-F8, T-F9, T-F15, T-F16
docs: fix-inventory.md#6-frame-pacing-and-present, plan-stage-b-split.md#t-l1b-precise-frame-limiter-at-233-2026-10-21-0140-launched-by-claude-with-the-users-go-build-a297c6b-log-datat-l1b-precise-233log, plan-stage-b-split.md#t-l3--t-l4-precise-limiter-2026-10-22-launched-by-claude-with-the-users-go--nostats-gow3_precise_limit1-traces-flip-trace-2csv-flip-trace-3csv, plan-stage-b-split.md#t-f2-which-barriers-break-the-passes-log-t-f2-barrier-breaks-trace-33, study-deck-patch-audit.md#16-presentation-and-pacing
depends_on:
related: reflex, precise-tick, inline-present, newest-frame, present-side-limiter, one-clock, lean-barriers, thread-nice, deck-preset, menu-idle-redraw
checked: 2026-11-07
---
## What it does

`GOW3_PRECISE_LIMIT=1` with a frame limit. The vblank tick no longer releases capped frames (`flip_slot` is false;
`precise_capped` only checks whether a frame is pending). The loop after the vblank events waits for the slot with
`PreciseWait(next_flip, slot_spin)`: a high-resolution waitable timer up to the slot minus `GOW3_PACER_SPIN_US`
(700 us), then a `_mm_pause` spin. On Linux that is `sleep_until`, then the spin. The slack is 0.1 ms instead of half
a vblank. `GOW3_LIMIT_HOLD=1`: a frame whose slot lies past the current tick window holds the vblank thread until the
slot, with no tick in between, as RTSS does. `GOW3_PRESENT_PRIORITY=1` (`BoostPresentingThread`, Windows only): the
vblank, pacer and swap threads run `THREAD_PRIORITY_TIME_CRITICAL` in MMCSS "Games" at high priority.
`present_limiter=precise` sets all three. `reflex` sets them too, as its fallback.

## Invariants

- Capped frames leave only at their slot: `flip_slot = !frame_limit || (!precise_limit && ...)`. Releasing at a
  tick again brings back the T-L3 symptom: frames released a tick late.
- With nothing queued, the tick still redraws (`pending` is false, so it takes the idle-redraw branch). Without that
  redraw the save list froze (T-L4, fixed in `c67b7e1`).
- `next_flip = max(next_flip + frame_period, now - frame_period)`: a grid that does not drift and never falls more
  than one period behind.
- The slot spin has to be longer than the timer's wake grid, ~0.5 ms on the user's Windows PC (T-L5 run 3).
  `PreciseWait`'s default spin of 100 us suits only `wait_slot(tick_deadline)`. A 0.1 ms slot spin spread the
  releases over 7.6-9.1 ms (T-F7).
- Does nothing while Reflex paces (`current_limit` returns 0), but `BoostPresentingThread` still applies.
- The slot loop runs only on the immediate path (`immediate_flips`, `main_port.flip_rate == 0`, one-clock off).
- The hold blocks the vblank thread for up to one frame period, with no guest vblank events in that time (on purpose).
- Cost when capped: a 700 us spin per frame (~4% of a CPU at 60 FPS), at TIME_CRITICAL on Windows. On Linux
  `sleep_until` is accurate to ~50-100 us [inferred, patch audit], so the spin there is ~7x longer than needed.

## Interactions

- reflex: this is its fallback (same variables). `default_windows: mixed` means the whole limiter runs on a GPU
  without `VK_NV_low_latency2`; on NVIDIA only the priority part does.
- precise-tick: with a 960 Hz precise tick, the slots and the tick are two clocks (T-F13 inference). The rule from
  T-P960: a high tick is fine with an exact limiter.
- inline-present: with the hold, the thread held is the one that presents.
- newest-frame: implies `precise_limit`. Its pacer introduced the 0.7 ms spin (`f4ffc01`).
- present-side-limiter: `GOW3_LIMIT_IN_PRESENT` builds on this limiter (`flip_deadline`).
- one-clock: its branch runs first, so these slots never run while it is on.
- lean-barriers: `5ecf53b` changed both (it is also that card's commit).
- thread-nice: the Linux counterpart of the priority (nice -10). `BoostPresentingThread` is a no-op on Linux.
- deck-preset: the Standard limiter (half-vblank slack, `sleep_until`), so this limiter is off there.
- menu-idle-redraw: a tick with a capped frame pending skips the idle redraw; a redraw never moves `next_flip`.

## Evidence

- T-L1b (first version, spinning up to 1 ms on the present thread, cap 233): heavy views worst 9.4-10.2 ms (T-L0:
  7.4-8.9). Kept off.
- T-L2 (old limiter, 120 cap): releases 6.3-10.4 ms apart (p99 9.86, p99.9 10.43). T-L4 (slot-only release):
  presented p99 9.05, p99.9 9.39, 3 of 9,411 frames over 10 ms. The user: "technically better", heavy G-Sync flicker.
- T-F8 (0.7 ms slot spin): releases 8.03 / 8.63 ms apart (from 7.6 / 9.1). T-F9 (hold): releases 8.333 / 8.334, but
  the screen showed 8.05-8.11 / 8.56-8.61 (the work after the release).
- T-F16 (hold + priority): releases 8.333 / 8.334, 1% lows 112-117, 0.1% 85-116. The user: significantly better
  than the other runs, still behind RTSS. Then Reflex (T-F17 on).
- Never measured as the fallback on AMD / Intel. Patch audit 3.3 proposes an A/B on a 5800X3D + 6900XT; not run.

## History and pitfalls

- T-L1b's spin on the realtime present thread took a core from stage A / B. The timer rework (`c552021`, 0.1 ms
  spin) changed nothing in T-L3 because the tick still released capped frames a tick late (fixed by `c67b7e1`). The
  ~0.5 ms timer grid (T-L5 run 3, T-F7) then led to the 0.7 ms slot spin (`5ecf53b`).
- `44994b9` put `present_precise_limit` in the launcher; `0c357bc` replaced it with `present_limiter` (precise /
  reflex set this limiter's three variables).
- `SetCurrentThreadRealtime` is a no-op on Windows. Before `6e95734`, every presenting thread ran at normal priority
  (T-F15: releases a tick late).
- Patch audit item 5 lists it as a cost on 4-6 core CPUs. To retry on Linux, use a shorter spin.
