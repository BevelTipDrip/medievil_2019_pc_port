---
id: reflex
title: NVIDIA Reflex frame limiter through VK_NV_low_latency2, sleep right before vkQueuePresentKHR, falls back to Precise
area: present
status: active
platforms: all
default_windows: mixed
default_deck: off
origin: ours
class: G
commits: 966d23d173, 8858c4f208, 047cb7b6f2, 0c357bc1cb
files: gpu/shadps4/core/libraries/videoout/driver.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_presenter.h, scripts/graphics_options.py
symbols: ReflexEnabled, ReflexSleepBeforePresent, LatencySleep, SetLatencyInterval, ApplyLatencyMode, LatencyMarker, IsLowLatency2Enabled, reflex_interval, immediate_flips, PRESENT_LIMITERS
switches: GOW3_REFLEX, GOW3_REFLEX_SLEEP_BEFORE_PRESENT, present_limiter
tests: T-F16, T-F17, T-F18, T-F19, T-F23, T-F24, T-F25, T-F26, T-F27
docs: fix-inventory.md#6-frame-pacing-and-present, plan-stage-b-split.md#t-f2-which-barriers-break-the-passes-log-t-f2-barrier-breaks-trace-33, plan-stage-b-split.md#t-f27-the-capped-mode-that-works-and-the-launcher-consolidation-2026-10-25, study-deck-patch-audit.md#16-presentation-and-pacing
depends_on:
related: precise-limiter, precise-tick, inline-present, vrr-auto-limits, frame-limit-parse, upstream-vsync-limit-rule, present-side-limiter, one-clock, menu-idle-redraw, display-trace, vsync-control, deck-preset
checked: 2026-11-07
---
## What it does

`GOW3_REFLEX=1` enables `VK_KHR_present_id` + `VK_NV_low_latency2` (`vk_instance.cpp`), creates the swapchain with
`SwapchainLatencyCreateInfoNV` and hands the frame limit to the driver as `minimumIntervalUs` (`SetLatencyInterval`
-> `ApplyLatencyMode`, `lowLatencyBoost` false). In `PresentThread`, `current_limit` then returns 0, so the port's own
limiter steps aside, and `immediate_flips` sends each frame to the presenter as soon as it is queued. With
`GOW3_REFLEX_SLEEP_BEFORE_PRESENT=1`, `vkLatencySleepNV` (`LatencySleep`, a timeline-semaphore wait) runs in
`Presenter::Present` right before `vkQueuePresentKHR`, where RTSS's hook sleeps; without it, after the present in
`Flip`. Default: `present_limiter=reflex` (`PRESENT_LIMITERS`), which also sets the Precise limiter's variables.

## Invariants

- `reflex` in `PresentThread` needs `GOW3_REFLEX=1` and `presenter->ReflexEnabled()` (`low_latency2`: the
  `presentId` feature and both extensions). Without the second check a GPU without the extension would get
  `current_limit` 0 with nothing pacing it (uncapped).
- While Reflex paces, `current_limit` returns 0 every pass. Any other limiter added later must step aside the same
  way: two limiters at one rate fight (T-F13).
- `immediate_flips = frame_limit != 0 || reflex`. Frames that wait for the next tick reach the driver up to 1.13 ms
  late (T-F17). A guest `flip_rate != 0` still skips the immediate loop.
- Interval changes go through `RunPresenter` (the presenting thread). `ApplyLatencyMode` runs again at the end of
  `Swapchain::Create`, or a recreate (resize, VSync switch) would lose the cap.
- `LatencySleep` in `Present` runs outside `Scheduler::submit_mutex`: a sleep of up to a frame under it would block
  every queue submission, stage B's included. A game frame sleeps once: in `Flip` only when
  `!ReflexSleepBeforePresent()`, otherwise in `Present` (`is_game_frame`).
- The semaphore wait has a 100 ms timeout: a driver that never signals costs 100 ms a frame, not a hang.
- "limit >= tick -> 0" and the VSync rule (`limit + 1 >= refresh` -> 0) apply first; Reflex then gets interval 0.

## Interactions

- `default_windows: mixed`: the preset sets `GOW3_REFLEX=1` on every Windows GPU, but only NVIDIA has
  `VK_NV_low_latency2`; AMD / Intel get precise-limiter (same variables). On NVIDIA `GOW3_PRESENT_PRIORITY` still
  makes the vblank thread time-critical + MMCSS, while `GOW3_LIMIT_HOLD` does nothing.
- precise-tick: Reflex puts the vblank loop on the immediate path, where the precise tick's wait is a no-op (see
  that card) [code reading].
- inline-present (default): the sleep blocks the vblank thread itself, so no ticks or guest vblank events arrive
  during it, as with RTSS or the hold. Every Reflex measurement was inline.
- vrr-auto-limits, frame-limit-parse: the 119 cap of T-F27 (T-F26's parse bug gave interval 0).
  upstream-vsync-limit-rule zeroes a limit at refresh - 1 or above before Reflex sees it.
- menu-idle-redraw: `DrawLastFrame` calls `Present(frame, true)` with `is_game_frame` defaulting to true, so with
  the sleep before the present the menu's idle redraws also take a Reflex sleep [code reading, not measured].
- display-trace: `GOW3_REFLEX=1` adds `VK_KHR_present_id` before it checks for `VK_NV_low_latency2`, so present ids
  are on in every default Windows run whose driver has `presentId`, AMD / Intel included. T-V120b suspected that
  present ids change the present spread; never re-measured.
- deck-preset: Standard limiter. RADV has no `VK_NV_low_latency2`. NVIDIA on Linux was never tested.

## Evidence

- T-F17 (sleep after the present, frames released on the next 960 Hz tick): lows ~110 / ~108, presents reached the
  driver 7.91 / 8.91 ms apart. `8858c4f` (immediate release) came from this run.
- T-F18: the driver's sleep ended 8.327 / 8.340 ms apart (1% / 99%). The user: 1% low 117, 0.1% 114, plus a drop to
  ~60 every couple of seconds not in the trace. PresentMon: display changes 8.18 / 8.48 ms apart.
- T-F19: on screen equal to RTSS's Reflex limiter at 119.98 (1% / 0.1%: ours 111.4 / 104.2, RTSS 109.6 / 102.7).
- T-F24 (sleep before the present, Immediate): present calls start 8.329 / 8.333 / 8.337 ms apart. The user: the best
  1% / 0.1% lows so far (up to 118), flicker still bad. T-F25 (VSync on): still a lot of VRR flicker.
- T-F27 (+ VSync on, 119 cap, 240 Hz OLED): the user, "looks great, huge improvement", flicker gone; confirmed in a
  second run with RTSS's limiter off.

## History and pitfalls

- `966d23d` opt-in, sleep after the present; `8858c4f` immediate release; `047cb7b` sleep before the present (after
  T-F20-T-F23: blips and flicker with the sleep after it); `0c357bc` default + fallback (`ReflexEnabled()` check).
- Reflex alone did not fix the VRR flicker; the 119 cap with VSync on did.
- The latency markers: `eSimulationStart` (id `present_id + 1`) goes out right after the sleep and the frame's
  present follows at once, so the driver sees a near-zero "simulation". Only `minimumIntervalUs` paces. Good enough
  for pacing; the markers do not describe the game's frame [code reading].
