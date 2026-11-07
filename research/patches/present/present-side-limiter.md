---
id: present-side-limiter
title: Limiter wait right before / inside / after vkQueuePresentKHR (RTSS-style), spin holding submit_mutex
area: present
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: 2defb00d22, 8f6718072c, 6658f4f733
files: gpu/shadps4/core/libraries/videoout/driver.cpp, gpu/shadps4/core/libraries/videoout/driver.h, gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_presenter.h
symbols: limit_in_present, flip_deadline, SetPresentDeadline, present_deadline, after_present, wait_until, submit_mutex
switches: GOW3_LIMIT_IN_PRESENT, GOW3_LIMIT_AFTER_PRESENT
tests: T-F9, T-F10, T-F11, T-F12
docs: plan-stage-b-split.md#t-f2-which-barriers-break-the-passes-log-t-f2-barrier-breaks-trace-33, fix-inventory.md#10-tried-and-dropped
depends_on: precise-limiter
related: reflex, inline-present, one-clock, flip-trace, lean-barriers
verified: 2026-11-07
---
## What it does

With the precise limiter and a cap, `GOW3_LIMIT_IN_PRESENT=1` (`2defb00`) releases frames as they arrive. The
vblank loop sets `flip_deadline = next_flip`, `Flip` passes it on with `SetPresentDeadline`, and `Presenter::Present`
waits for the slot after the frame's acquire, blit and submit, right before `vkQueuePresentKHR` (the RTSS limiter's
place). `8f67180` spins the last 0.5 ms inside `Scheduler::submit_mutex` and adds the `present_start` flip-trace
stage. `GOW3_LIMIT_AFTER_PRESENT=1` (`6658f4f`, on top of `GOW3_LIMIT_IN_PRESENT=1`) presents at once and then holds the
thread until the slot (RTSS's back edge). Both are off by default; the code stays behind the switches, out of the launcher since T-F27.

## Invariants

- Live only with `precise_limit && limit_in_present` in `PresentThread`. Under Reflex, `frame_limit` is 0 and no
  deadline is ever set.
- `present_deadline` is taken and cleared on every `Present`, so a deadline applies to one present only. `Flip`
  sets it in the same presenter work, right before `Present`, so no idle redraw can take it.
- Holding `submit_mutex` while spinning (up to 0.5 ms a frame) blocks every queue submission on other threads,
  stage B's included. Tolerable only because it is short. Never put a long wait inside that lock (the Reflex sleep
  sits outside it for this reason).
- `wait_until` uses a high-resolution timer only on Windows. On Linux it is a pure `_mm_pause` spin for the whole wait
  (up to a frame per frame). Do not enable it on the Deck as it is.
- With `GOW3_LIMIT_AFTER_PRESENT=1`, `limited` is false, so no wait happens before the present. The hold comes after
  `swapchain.Present()`, outside `submit_mutex`. Alone (without `GOW3_LIMIT_IN_PRESENT=1`) it does nothing: only
  the in-present path sets a deadline.

## Interactions

- precise-limiter: required. Its slot grid (`next_flip`) supplies the deadline.
- reflex: the successor. The driver sleeps where this waited, and it measured better.
- inline-present: with presents on the vblank thread the wait blocks it, as RTSS's hook does. All runs were inline.
- flip-trace: the `present_start` stage from `8f67180` stays in use (T-F24 measured Reflex with it).
- lean-barriers: T-F10's intermittent flicker was first suspected to come from the barrier changes (default since
  T-F8). T-F11 / T-F12 kept the flicker with the present-side wait, and Reflex with VSync and 119 later removed it.

## Evidence

- T-F9 (the motivation, hold only): releases exact (8.333 / 8.334 ms), but release -> presented took 0.35 ± 0.25 ms,
  so the screen showed 8.05-8.11 / 8.56-8.61.
- T-F10 (in present): the present call took 8.06 ms, screen 8.11 / 8.33 / 8.56 ms, and 1,087 of 3,589 frames off by
  more than 0.1 ms. The user: 0.1% low 115, 1% 117.
- T-F11 (+ the `submit_mutex` spin): `vkQueuePresentKHR` starts exactly 8.333 ms apart (0 frames off by more than
  0.1 ms), but the call takes 0.22-0.48 ms, so returns come 8.14 / 8.53 ms apart. The user: 0.1% lows 115-116 and a
  persistent flicker.
- T-F12 (back edge): every stage matched the RTSS run (returns 8.333 / 8.334). The user: larger intermittent spikes,
  still flicker. Both modes rejected.

## History and pitfalls

- Exact present starts did not fix the VRR flicker. The flicker came from the cap at exactly half the refresh
  (T-F26 / T-F27), not from present timing. So a retry is only worth it with a 119 cap and VSync on, compared against
  Reflex (T-F27) on a non-NVIDIA GPU, where it could replace the Precise fallback. It also needs the Linux wait fixed
  first (a timer before the spin).
