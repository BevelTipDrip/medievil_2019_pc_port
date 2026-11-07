---
id: newest-frame
title: Newest-frame mode: show the newest finished frame at each cap slot from a dedicated pacer thread
area: present
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: 7cca8943ae, 03a84b53bd, fbce36f354, f4ffc01d79
files: gpu/shadps4/core/libraries/videoout/driver.cpp, gpu/shadps4/core/libraries/videoout/driver.h, gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_presenter.h
symbols: PacerThread, DropOlderFinished, DropFlip, newest_active, pacer_limit, pacer_thread, newest_frame, completed, WaitFrameReady, DiscardFrame
switches: GOW3_NEWEST_FRAME, GOW3_PACER_SPIN_US
tests: T-L5
docs: plan-stage-b-split.md#t-l5-newest-frame-mode-gow3_newest_frame1-the-users-proposal, fix-inventory.md#10-tried-and-dropped
depends_on: precise-limiter
related: inline-present, frames-queued-2, present-side-limiter, reflex
verified: 2026-11-07
---
## What it does

The user's proposal. With `GOW3_NEWEST_FRAME=1` and a frame limit, the game renders uncapped. At each slot the newest
frame the GPU has finished is shown, and every older one completes its flip unshown (`DropFlip`: flip status, events
and labels as for a shown flip, the frame back to the presenter's pool). `SubmitFlipInternal` completes the guest's flip
as soon as `PrepareFrame` has copied the frame out (`Request::completed`, while `newest_active`). `DropOlderFinished`
drops finished frames older than the newest; frames finish in queue order (`Presenter::WaitFrameReady`). A dedicated
`gow3:Pacer` thread (`PacerThread`, `fbce36f`) releases at each slot with `PreciseWait`. Since `f4ffc01` it spins
the last 0.7 ms (`GOW3_PACER_SPIN_US`). The mode implies `precise_limit`. Off by default, removed from the launcher
in T-F27, and the code stays behind the switch.

## Invariants

- The pacer thread exists only if `GOW3_NEWEST_FRAME=1` at construction. `newest_active` and `pacer_limit` are set by
  the vblank loop and are true / non-zero only while a frame limit applies. Without a limit, frames take the normal
  path.
- In this mode the guest's flip must complete when the frame is copied out, not at the present. Otherwise the guest
  waits for its display buffer's label and never renders a newer frame (T-L5 run 1: 0 frames dropped).
- `DropFlip` completes only flips not already `completed`, so each flip completes exactly once.
- While the pacer runs, the vblank loop releases nothing (`if (newest_active) ... else while`): one releaser only.
- With Reflex, `frame_limit` is 0, so the mode is off.

## Interactions

- precise-limiter: implied (`precise_limit = newest_frame || ...`). The 0.7 ms spin this mode introduced became the
  precise limiter's slot spin (`5ecf53b`) and the present-side waits' spin.
- inline-present: the pacer calls `Flip`, so with `GOW3_PRESENT_THREAD=0` (the default since T-F27) it presents on
  the pacer thread while the vblank thread can still present idle redraws (menu open, nothing queued): two threads in
  `Presenter::Present` without the swap queue. All T-L5 runs used the swap thread; a retry needs a lock or the swap
  thread [code reading].
- frames-queued-2: also lets the game run ahead of the present; it cost lows.
- reflex: replaced the goal (even presents at a cap) by another route.

## Evidence

T-L5 here is plan-stage-b-split.md's newest-frame runs, not the drain test of the same name in plan-baseline-issues.md.
- Run 1 (`7cca894`): not working. Even the intro videos had capped-like lows. 0 frames dropped, each frame ~7.9 ms in
  the queue.
- Run 2 (`03a84b5`): the mode works (~325 FPS rendered, ~2 of 3 dropped), "once again, it's not working" (the user).
  Shown frames median 8.29, p1 7.54, p99 9.24, p99.9 11.3 ms apart, the same spread as T-L4. Motion error median 0.99,
  p99 4.18 ms (T-L4: 0.32 / 1.26).
- Run 3 (`fbce36f`, pacer thread, 480 tick): main menu lows ~110 (uncapped 180+). Releases median 8.26, p1 7.91,
  p99 8.76, p99.9 9.11, in a sawtooth: the timer wakes on a ~0.5 ms grid, which led to the 0.7 ms spin (`f4ffc01`).

## History and pitfalls

- Dropped because the present timing kept the wobble and the content timing got worse (Fast Sync micro-judder). Its
  spin finding outlived it.
- Worth retrying only if the presents themselves are exact first (Reflex or the hold), with a flip trace that
  measures the content step against the display step (`newest_analyze.py`), and with the user's judgement of the
  motion. The cost stays: no GPU headroom is freed, since the game renders every frame.
