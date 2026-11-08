---
id: stage-b-spin
title: Deck #4: stage B spins up to GOW3_STAGE_B_SPIN_US before it blocks
area: stage-b
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: 4158db0eff
files: gpu/shim/gow3_stage_b_wait.h, gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h, gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h, gpu/shadps4/video_core/renderer_vulkan/vk_semaphore.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp, gpu/shim/gow3_toggles.h
symbols: Gow3StageBWait, Spin, SleepTimer, Enabled, SpinNs, OnStageB, PacketRing, recorder_running, queued_chunks, StageBSpin
switches: GOW3_STAGE_B_SPIN, GOW3_STAGE_B_SPIN_US, toggle2:StageBSpin
tests:
docs: impl-stage-b-bundle-2.md#4-stage-b-never-blocks-for-a-short-wait-opt-in, plan-linux-deck.md#spin-compact-texture-memo-and-the-task-split-2026-11-04, plan-linux-deck.md#cpu-clocks-and-the-power-cap-2026-11-0405-log-datadeck-epp-perf-1log-datadeck-dedicated-1log
depends_on: lineage-draw-pipe
conflicts_with:
supersedes:
related: deck-epp-governor, signal-sleep, stage-b-core, thread-nice, deck-preset, stage-b-counters, C1-semaphore-poll
checked: 2026-11-07
---
## What it does

Item 4 of study-deck-apu-deep-dive.md: on the Deck (amd-pstate, default EPP) a block over ~0.2 ms was thought to put
a busy core back on a sticky ~2.4 GHz plateau. With `GOW3_STAGE_B_SPIN=1`, stage B only (`OnStageB()`:
`Gow3Sections::recording_thread`) spins with pauses up to `GOW3_STAGE_B_SPIN_US` (default 200, max 2000) before it
blocks in: the packet ring (`DrawPipe::Run`: the existing spin, 50 us below 12 threads and 200 at 12+ or
`GOW3_PIPE_SPIN_US`, lasts the longer of the two),
`Scheduler::SyncRecording` (polls `queued_chunks` and the new hint `recorder_running`), `Semaphore::Wait` (a query
every 20 us) and `Presenter::GetRenderFrame` (a free frame every 2 us, its fence every 20 us). It also added the
"Stage B sleeps per frame" counter (`SleepTimer`, spin on or off).

## Invariants

- Each spin is only a delay before the unchanged blocking wait: `SyncRecording` and both `GetRenderFrame` spins
  ignore `Spin`'s result and re-check their own condition under their lock / fence wait. Only `Semaphore::Wait`
  returns on a true `Spin`, so there `done()` (`Refresh(); IsFree(tick)`) must stay exactly the wait's condition; a
  looser one would end a GPU wait early.
- `Semaphore::Wait`'s spin refreshes the known GPU tick every 20 us, which also restarts C1-semaphore-poll's
  interval (`refreshed_ns`) while it spins.
- `recorder_running` is a hint written next to `recorder_busy` under `recorder_mutex`; the check under the mutex in
  `SyncRecording` still decides.
- Off stage B, or with the spin off, `Spin` returns false at once and calls nothing (no extra system calls).
- `SleepTimer` only reads the clock with `Gow3Stats::enabled` on stage B.

## Interactions

- Pointless under EPP / governor `performance` (deck-epp-governor): the plateau it fights is gone; measured neutral
  there too.
- Opposite approach to signal-sleep / drain-hybrid (sleep sooner to save CPU power). On a 15 W APU a spinning core
  takes power from the GPU (the impl doc's named cost); the preset's "Left out" list gives no reason of its own.
- Not covered: `WaitDeferredSignals` / `WaitHostCopies` / `Gow3Copy::WaitAsync` (they already yield-spin; signal-sleep
  changes those).
- The counter feeds stage-b-counters / `deck_summary.py` whether the spin is on or not.

## Evidence

- `deck-spin-1.log` (`deck.sh ab "0 1024" "0 0" 20 4`): 77.1 vs 77.1 FPS; sleeps 4.2 -> 1.7 a frame, 4.8 waits a
  frame ended by the spin, GPU waits (0.7 a frame) unchanged: neutral. The user kept `GOW3_STAGE_B_SPIN=1` in
  extra-env.txt for a while.
- Under EPP performance (`deck-epp-perf-1.log`): spin off / on 75.6 / 75.9 FPS, neutral; "spin left off".
- `deck.sh clocks`: stage B ran at ~2.98 GHz mean without it (baseline 1), not the ~2.4 GHz plateau the study
  assumed, so the premise did not hold for stage B.

## History and pitfalls

- Left out of DECK_SWITCHES ("Left out: ... `GOW3_STAGE_B_SPIN`"). Retry only if stage B is shown to clock down
  after its sleeps (a `deck.sh clocks` trace with long waits), and only at a frame cap with power to spare.
