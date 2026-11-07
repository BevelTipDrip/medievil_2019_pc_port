---
id: stage-b-core
title: Stage B on a dedicated core (SMT sibling kept free), Windows and Linux
area: stage-b
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: e46dede0e5, 8bb92e4560
files: gpu/shim/gow3_threads.h, gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h, launcher/medievil_launcher.py
symbols: DedicateCore, Run, stage_b_core, prep_workers, PREP_WORKERS
switches: GOW3_STAGE_B_CORE
tests: T-HZ4h
docs: plan-stage-b-split.md#t-hz4f--t-hz4g--t-hz4h-where-the-remaining-spikes-come-from-2026-10-20-2157-2225-launched-by-claude-with-the-users-go, fix-inventory.md#10-tried-and-dropped, study-deck-rescore-rejected.md#a-switches-that-already-exist-no-build, plan-linux-deck.md#cpu-clocks-and-the-power-cap-2026-11-0405-log-datadeck-epp-perf-1log-datadeck-dedicated-1log
depends_on: lineage-draw-pipe
conflicts_with:
supersedes:
related: core-sched, thread-nice, prep-workers-2, prep-priority, deck-cpu-clock-split, stage-b-spin, recorder-lean
verified: 2026-11-07
---
## What it does

`GOW3_STAGE_B_CORE=1`: at the start of `DrawPipe::Run` stage B calls `Gow3Threads::DedicateCore("Stage B")`.
Windows (`e46dede`): one logical processor of the last physical core goes to stage B; both logical processors of
that core leave the process's default CPU sets, priority above normal. Linux (`8bb92e4`): stage B is pinned with
`sched_setaffinity` to one CPU of the highest `core_id`, its sibling stays free, and every other thread listed in
`/proc/self/task` is moved to the remaining cores (fails with fewer than 4 CPUs). `8bb92e4` also added launcher
settings: "Dedicated core for the GPU thread" (`stage_b_core`) and "Draw preparation helpers" (`prep_workers` ->
`GOW3_PREP_WORKERS`, `PREP_WORKERS` choices).

## Invariants

- On Linux, threads created later inherit their creator's affinity: a thread stage B itself creates would be pinned
  to stage B's CPU. Anything spawned from stage B after `DedicateCore` must reset its own affinity. Lazily created
  pools are the risk: `Gow3Copy`'s pool is a function-static made on first use (`GetPool`); which thread uses it
  first was not checked.
- On Linux `Gow3Threads::Available()` reads the calling thread's affinity (`sched_getaffinity(0)`): on stage B after
  the pin it returns 1. Sizes computed lazily there take the small-machine value, e.g. the packet-ring spin
  (`Available() >= 12 ? 200 : 50`, a static first read inside `DrawPipe::Run`) is 50 us on a 16-thread Linux PC with
  the switch on. Windows reads the process mask and is unaffected.
- Threads that change their own affinity after the move (none known) undo the isolation silently; the log line
  "Stage B: dedicated core N (CPU x; sibling CPU y kept free), N other threads moved" is the check.
- An early failure (fewer than 4 CPUs or CPU sets, no `core_id`) leaves everything as before ("Stage B: no core
  dedicated (CPU sets unavailable)"). A late one does not: on Linux the other threads are already moved when stage
  B's own `sched_setaffinity` fails; on Windows the process default CPU sets are already set when
  `SetThreadSelectedCpuSets` fails.

## Interactions

- core-sched (Deck #7, core scheduling cookie) and thread-nice (Deck #8) are the later Linux attempts at the same
  goal; prep-workers-2 / prep-priority change who competes with stage B's sibling.
- On a 4-core Deck the game keeps 6 of 8 CPUs (sibling idle); with the 15 W cap an idle sibling does not give stage
  B more clock (deck-cpu-clock-split).

## Evidence

- T-HZ4h (PC, build `e46dede`): per-draw cost of normal frames -4%, slow frames still +22% (textures +34%): "not core
  sharing or preemption". Gameplay p99 7.95 vs 7.28 ms, frames over 1.5x median 70 vs 15 a minute; the user: "a lot
  of large stutters", 1% and 0.1% lows worse. Rejected, off. (The first launch hit the S1 silent boot exit, its
  third occurrence, a known boot issue.)
- Deck, confounded (2 helpers + dedicated core together, `deck-helpers-run1.log`): 63.2 vs 64.4 FPS, stage B busy
  79 -> 70%.
- Deck, dedicated core alone (`deck-dedicated-1.log`): 74.1 FPS at 1,155 draws ("same draws/s" in the doc, against
  an unnamed reference; baseline 1's 74.2 at 1,241 is ~7% more draws/s), stage B busy 96 -> 89%, GPU 98% busy at
  1,024 MHz.

## History and pitfalls

- Rescore A3 (study-deck-rescore-rejected.md) predicted stage B -0.7 to -1.7 ms and net FPS -2% to +8% on the Deck;
  the clean Deck run showed no FPS gain, so the prediction was not confirmed.
- Why dropped: on 16+ threads there was no contention to remove (PC); on the Deck the APU is power-capped and GPU
  bound in the heavy view, so freeing stage B's sibling bought nothing. Worth retrying only if stage B is shown to
  lose time to its sibling (per-thread samples) with GPU and power headroom left.
- The launcher's checkbox stays (Experimental) and only sets the variable.
