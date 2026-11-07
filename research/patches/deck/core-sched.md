---
id: core-sched
title: Deck #7: core scheduling cookie on stage B (GOW3_STAGE_B_CORE_SCHED)
area: deck
status: rejected
platforms: linux
default_windows: n/a
default_deck: off
origin: ours
class: G
commits: 24e461b
files: gpu/shim/gow3_linux_sched.cpp, gpu/shim/gow3_linux_sched.h, gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h
symbols: StageBThread, CoreSchedWanted, PR_SCHED_CORE_CREATE, PR_SCHED_CORE_SCOPE_THREAD
switches: GOW3_STAGE_B_CORE_SCHED
tests:
docs: impl-deck-build-sched-pool.md#7-core-scheduling-cookie-on-stage-b-gow3_stage_b_core_sched1, plan-linux-deck.md#round-1-results-gpu-clock-auto-2026-11-05-ini-logging-67-uncapped-launched-by-the-user, study-deck-apu-deep-dive.md#13-smt-and-stage-b
depends_on:
related: thread-nice, stage-b-core, stage-b-spin, drain-hybrid, prep-workers-2, deck-preset
---
## What it does

With `GOW3_STAGE_B_CORE_SCHED=1` at start (env starting with `1`, read once), `Gow3LinuxSched::StageBThread()`
(called from `DrawPipe::Run` after the thread is named and `GOW3_STAGE_B_CORE` ran) gives stage B (`gow3:DrawRec`)
its own core-scheduling cookie: `prctl(PR_SCHED_CORE, PR_SCHED_CORE_CREATE, 0, PR_SCHED_CORE_SCOPE_THREAD, 0)`.
While stage B runs, its SMT sibling may run only tasks with that cookie (none) and idles, so stage B gets the
whole Zen 2 core (no halved ROB / queues). When stage B sleeps the sibling runs anything. No affinity changes.
One log line: the cookie, or the error (EINVAL: no `CONFIG_SCHED_CORE` / before 5.14; ENODEV: no SMT). Windows: no-op.

## Invariants

- Off unless the env is set; a failed `prctl` only logs, nothing else changes.
- The cookie is per thread and inherited by threads stage B creates later (none known).
- Stage B's spin before it blocks (`stage-b-spin`, 50 us default) counts as running, so the sibling idles through
  the spin too.
- Not live: no toggle bit, one launch per state.

## Interactions

- `stage-b-core` (dedicated core, sibling kept free by affinity) is the other route to the same goal; both cost one
  of 8 CPUs while stage B runs.
- `drain-hybrid` / stage A: stage A is the thread that lost the most (its drain waits tripled), i.e. it could not
  run beside stage B any more.
- `prep-workers-2`: the APU study proposed the cookie as the sibling fix instead of fewer workers; it removes the
  workers from stage B's core but not from the CPU budget.
- `thread-nice` shares commit `24e461b` and file; it stayed.

## Evidence

- Round 1, GPU clock auto (plan, launch 4, `data/deck-coresched-1.log`): the cookie was applied ("core scheduling
  cookie 0x83ff0c57"), and the result was **66.4 FPS** (other round 1 launches 77.1-80.15), worst frame median
  **35.4 ms** (others ~17-18), p99 22.79 ms. The sibling idles; stage A drain waits 2.1 -> 6.9 ms a frame.
- The study's prediction: stage B -5-10% per draw, FPS -5% to +5% [inferred]; the measured FPS fell ~16%.

## History and pitfalls

- From study-deck-apu-deep-dive.md 1.3 / ranked action 7. Rejected after one launch, never in the Deck preset
  (plan, "Deck Optimized preset": "core scheduling (rejected)").
- Worth retrying only if the game's CPU demand drops well below 7 busy threads (stage A, the game threads and the
  recorder no longer need stage B's sibling), or with a cookie shared by stage B and stage A so they pair on one
  core (not built). Whether `scx_lavd` honours cookies was not checked.
