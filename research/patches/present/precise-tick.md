---
id: precise-tick
title: Precise vblank tick on a fixed grid (timer + spin), tick rate as a launcher setting (tick = cap rule)
area: present
status: active
platforms: all
default_windows: on
default_deck: off
origin: ours
class: C
commits: 181c4c169c, 44994b9bff, 0c357bc1cb
files: gpu/shadps4/core/libraries/videoout/driver.cpp, gpu/shim/core/emulator_settings.h, scripts/graphics_options.py, launcher/medievil_launcher.py
symbols: precise_tick, tick_spin, next_tick, PreciseWait, GetVblankFrequency, present_environment, PRESENT_DEFAULTS, PRESENT_FLAGS
switches: GOW3_PRECISE_TICK, GOW3_TICK_SPIN_US, present_precise_tick, present_tick_hz, GOW3_VBLANK_HZ
tests: T-V120, T-V120p, T-N120, T-P120, T-P960
docs: fix-inventory.md#6-frame-pacing-and-present, plan-stage-b-split.md#t-v120-120-hz-tick-vsync-fifo-no-limiter-50-ue4_upscaleperformance-2026-10-23-launched-by-claude-with-the-users-go-f4ffc01-gow3_vblank_hz120-trace-flip-trace-8csv, plan-stage-b-split.md#t-p120-the-configuration-that-holds-the-cap-2026-10-23-0300-the-user-launcher-settings, plan-stage-b-split.md#ablation-of-the-t-p120-settings-2026-10-23-evening-traces-research-dataflip-trace-18csv-to--24csv, plan-stage-b-split.md#t-p960-high-tick--limiter-on-top-of-the-t-p120-settings-2026-10-23-trace-flip-trace-25csv, plan-linux-deck.md#proton-runs-3-4-2026-11-03-settings-changed-by-the-user-while-exploring, study-deck-patch-audit.md#short-answer
depends_on:
related: inline-present, precise-limiter, reflex, one-clock, vblank-sync, presentation-settings, deck-preset, thread-nice
checked: 2026-11-07
---
## What it does

`GOW3_PRECISE_TICK=1`: the vblank loop in `PresentThread` waits with `PreciseWait(next_tick, tick_spin)`, a
high-resolution timer and then a spin of `GOW3_TICK_SPIN_US` (700 us), on a fixed grid
(`next_tick = max(next_tick + tick_period, now - tick_period)`). It replaces `AccurateTimer`, whose ticks were
±0.7 ms off (T-V120). `44994b9` made the tick rate a launcher setting (`present_tick_hz` -> `GOW3_VBLANK_HZ`, read
once by `GetVblankFrequency`). `0c357bc` made 960 Hz with the precise tick the default (`PRESENT_DEFAULTS`). UE4 waits
for each vblank, so the tick paces the game's frame starts as well as the presents.

## Invariants

- Assumption (class C): the game starts its frames on the guest's vblank events (UE4 on PS4). A game that does not
  gets the steadier presents but not steadier frame starts.
- A frame limit applies only below the tick: in `current_limit`, `limit >= vblank_hz` gives 0. The rule: tick = cap,
  or a high tick plus an exact limiter, never a coarse 480 Hz tick plus a limiter.
- A tick of 120 Hz or more makes the guest see a 119.88 Hz display (upstream code in the `VideoOutDriver`
  constructor). That is what lets UE4 run up to 120.
- `next_tick = max(next_tick + period, now - period)`: after an overrun of more than one period (an inline present,
  a hold) the grid re-anchors to `now - period` (one or two immediate ticks, then a new phase). Harmless for the tick
  alone; it is what moved one-clock v1's "one clock" (T-F14), so one-clock v2 counts ticks on its own grid.
- The precise wait only paces the tick on the non-immediate path: no limit left after `current_limit`'s rules and no
  Reflex (T-P120: `fps_limit` 0, tick 120). With a cap or Reflex (the Windows defaults) each pass ends in
  `request_cv.wait_until(tick_deadline)`, and `tick_deadline` (now + period, taken after `next_tick` moved) is never
  before `next_tick`, so the next `PreciseWait(next_tick)` finds its slot passed: no sleep, no spin. The tick is then
  timed by the condition-variable timeout [code reading, not measured].
- `GetVblankFrequency` is static, so a new tick rate needs a restart.
- With the precise tick `timer.Start()` never runs, but `timer.End()` does on the non-immediate path, so
  `AccurateTimer::total_wait` goes far negative (`start_time` never set) and the "Dont draw too fast" test always
  passes: a `DrawBlankFrame` on every tick before the port opens (`MustKeepDrawing` is a stub, false). On the
  immediate path `total_wait` stays 0: no blank frames [code reading].
- Cost, where the wait is live (non-immediate path): the spin takes ~0.7 of every 1.04 ms at 960 Hz (two thirds of a
  core), at TIME_CRITICAL + MMCSS on Windows with `GOW3_PRESENT_PRIORITY=1` (set by the Reflex and Precise presets).
  Uncapped on AMD / Intel (no Reflex) pays it; capped or NVIDIA-Reflex play should not (previous point).

## Interactions

- inline-present: the user's pair of essential settings (T-P120, ablation).
- precise-limiter: T-P960 is a high precise tick plus the exact limiter; being capped, the tick's own wait was inert
  there by the code, and the limiter's slot spin did the timing. one-clock derives the tick from the limit
  (rejected). vblank-sync is the other tick source (rejected).
- reflex: with Reflex the loop is on the immediate path: the tick still runs at about 960 Hz for the guest's vblank
  events, timed by the condition-variable wait, and the precise wait does nothing [code reading].
- deck-preset: 900 Hz, not precise (`present_precise_tick=0`). The plain tick still wakes 900 times a second.
- thread-nice: on Linux the vblank thread gets nice -10 from that switch, not from this one.

## Evidence

- Code reading (2026-11-07, LANDMINES item 5): with a cap or Reflex (the Windows defaults) the precise tick's wait never
  sleeps or spins; each pass ends waiting for `tick_deadline` (plain `sleep_until`, or a 100 us spin with the precise
  limiter). Its 700 us spin runs only uncapped without Reflex. The ~0.65-core cost (T-P960, audit #75) was computed.
- T-V120 (`AccurateTimer`, 120 Hz tick, VSync): the user, "1% and .1% lows were a solid 118-119 ... We weren't able
  to get a frametime this steady before", then waves of 12.5 / 4.17 ms frames.
- T-V120p: ticks p1 / p99 8.32 / 8.34 ms. Present calls 7.8-8.9 ms apart. The user: 1% lows in the 80s on the NVIDIA
  overlay (with the swap thread and FIFO).
- T-P120 (tick 120, precise, inline, Immediate): the user: a flat frame-time graph, 1% lows 118-119, at 4K native
  with G-Sync on or off. A 165 Hz tick kept the 0.1% lows above 150. Ablation 0d (165): 165.0 avg, 1% 158.1, 0.1% 151.8,
  0 frames over 9 ms in 75 s.
- T-P960 (960 tick + 120 limit): fight 1% 107.6, 0.1% 100.5. The user: dips to ~95 FPS instead of 60.
- Deck, Proton run 4: with the precise tick off (tick 120), the main menu graph completely flat at 60 FPS (the
  user). The user's native Deck ini has it off.

## History and pitfalls

- Upstream `55e1a76` chose a 480 Hz tick plus a limiter, because a 120 Hz vblank turned 9 ms frames into 17 ms ones.
  What failed there was the coarse timing around the high tick, not the high tick itself (T-P960).
- Patch audit item 1 and Deck plan L1: still the default for a fresh install, a handheld or a 4-6 core PC. Proposed
  fixes: off at <= 8 threads, a shorter spin on Linux, or tick = cap = panel refresh. None is built. The audit's
  ~0.63-0.67 CPU and T-P960's "two thirds of a core" assume the spin runs every tick; by the code it does only
  uncapped without Reflex. Measure the vblank thread's CPU capped vs uncapped before acting on it.
