---
id: frames-queued-2
title: Frames queued 2: stage B's flip stops waiting for the GPU to finish the previous frame
area: present
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: upstream 1.0.0
class: G
commits:
files: gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp, gpu/shim/gow3_graphics.h, gpu/shim/gow3_settings.cpp, gpu/shim/gow3_overlay.cpp, launcher/medievil_launcher.py
symbols: PrepareFrame, recent_frame_ticks, frames_ahead, frames_queued, ParseFramesQueued, FramesQueued, FRAMES_QUEUED
switches: GOW3_FRAMES_AHEAD
tests: T-W1, T-Q2, N-15, N-25
docs: plan-frame-pacing.md#gpu-wait-sites-2026-10-18-1420-gow3_perf_diag1-current-build-without-r4a-run-perfdiag-heavy, plan-stage-b-split.md#t-w1-frames-queued-2-2026-10-19-2112-2116-launched-by-claude-with-the-users-go-build-3b30fe9-log-datat-w1-frames-queuedlog, plan-stage-b-split.md#t-s5--t-q2-the-remaining-spikes-frames-queued-2-2026-10-21-2210-2222-launched-by-claude-with-the-users-go-50-slow-frames--sections-logs-datat-s5-spikeslog-datat-q2-frames-queued-2log, plan-frame-pacing.md#n-25-preshadows-as-a-live-switch-2026-10-30-launched-by-claude-with-the-users-go-t-park3-clean-preshadows-offlog-t-pre2-live-switchlog, study-deck-rescore-rejected.md#a-switches-that-already-exist-no-build, plan-linux-deck.md#clock-push-pull-at-gpu-auto-2026-11-06-the-users-model-tooling-5ead842-not-run, study-eop-wait-site.md#4-consequences-for-the-hybrid-u5, fix-inventory.md#10-tried-and-dropped
related: early-frame-eop, decode-pacing, U5, U5-2, lineage-draw-pipe, reflex, deck-preset
checked: 2026-11-07
---
## What it does

The inherited "Frames queued" setting (`frames_queued` in medievil.ini, in-game menu > Performance, live;
`GOW3_FRAMES_AHEAD` at start) bounds how far stage B runs ahead of the GPU. At the end of `PrepareFrame` (on
stage B, at the flip) the frame's `ready_tick` goes into `recent_frame_ticks`; with a bound of n, stage B waits
(`draw_scheduler.Wait`) for the frame n flips back. The default 1 waits for the previous frame; this card is the
value 2 (one more frame of slack and of latency). It was measured and not made the default.

## Invariants

- `ParseFramesQueued` accepts only 1, 2 and 0 (`FramesQueued = {1, 2, 0}`); any other number, `GOW3_FRAMES_AHEAD=3`
  included, and an empty value become 1. The value goes through `atoi` first, so a non-numeric one (`off`, `auto`)
  is 0, unbounded. The launcher (`FRAMES_QUEUED`) and the overlay offer the same three.
- The bound is read on every `PrepareFrame`, so switching is live; going down (2 -> 1) pops the deque and waits only
  on the entry that leaves exactly `frames_ahead` behind, so no frame is waited for twice.
- 0 clears `recent_frame_ticks` and removes the bound: the code comment records 12.5 / 25 ms alternation at 80 FPS
  without it. Never use 0.
- The wait is part of stage B's "busy" time in the section stats (T-P4's profile, quoted with T-S5: ~27% of stage B in this wait at 1); it is
  slack when stage A waits on stage B, but real stage B time when stage B is saturated (the Deck question).

## Interactions

- early-frame-eop / U5 / U5-2: study-eop-wait-site.md (inferred, not run): today's early frame EOP with
  `frames_queued=1` lets the GPU lag 3 frames behind the RHI frame, which is enough for the copy model; under
  in-place reads (U5) it "breaks outright with `frames_queued` 0 or >= 2", and fix (B) needs it fixed at 1.
- decode-pacing: its frame lead cap is a second bound on the same pipe; how the two combine at 2 is unmeasured.
- reflex: the default limiter is chosen for latency; a second queued frame adds up to a frame of latency on top
  (no run measured the two together).
- deck-preset: writes `frames_queued=1`. Its `DECK_SWITCHES` turn early frame EOP on, so a Deck retry at 2 runs
  in the "GPU done >= G-5" row of the EOP study: fine under today's copy model, unsafe for any in-place read work.

## Evidence

- GPU wait sites (2026-10-18, PC): the frames-queued wait ~869 ms per 5 s (~17%), max 69.7 ms. F1 (try 2) declined
  by the user: "buffering extra frames isn't going to get us what we need here, the issues are too systemic".
- T-W1 (live A/B): at 1 the wait was 0.02-0.03 ms per 5 s while panning; at 2 panning 184-195 FPS, p99 9.7-10.4 ms
  (1: 190-201 FPS, 8.7-11.3 ms). The user: "No real difference, it seemed to hurt if anything."
- T-Q2: 213 vs 210 FPS, p99 6.49 vs 6.38 ms. The user: "buffering additional frames seems to hurt stability every
  time, especially with 1% lows". N-15 (ring reuse on): did not help.
- N-25 (2026-10-30, after the HZB fix, parking, preshadows off): the user found 2 gives "even cleaner frametime
  graphs", at ~4.2 ms more latency at 237. Default stays 1 "so testing does not mask real issues".
- Deck: not run. study-deck-rescore-rejected A1 scores it at 0 to -0.8 ms of stage B a frame (estimate).

## History and pitfalls

- The bound came with upstream 0.2 (`bf95396`, `BB_FRAMES_AHEAD` from the Bloodborne port, with the 12.5 / 25 ms
  comment); `013c4a8` ("fila de quadros", before 1.0.0) made it the live `frames_queued` setting. "Never 0" rests on
  that inherited comment: T-N120 ran `GOW3_FRAMES_AHEAD=0` (a present test);
  its write-up reports no 12.5 / 25 ms alternation, but did not look for one.
- "Slack" held on the PC because stage A waited on stage B; on the Deck stage B is saturated, so the verdict may be
  wrong there. Retry: one Deck launch, menu Frames queued 1 <-> 2 every ~30 s with `GOW3_FRAME_STATS=1
  GOW3_SECTIONS=1 GOW3_SLOW_FRAMES=1.4` (flip_present us/draw, stage B busy, FPS, p99), as rescore A1 plans.
- Upstream #30's Linux report (PC, RADV) lists "`FRAMES_AHEAD` 2/3" among things tried without change; 3 parses
  to 1, so only 2 was really tried there.
- The in-game menu's save once made a test value permanent (T-W1, fixed in `e9d8699`): check medievil.ini after
  a frames-queued A/B.
