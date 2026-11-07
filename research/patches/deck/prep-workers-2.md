---
id: prep-workers-2
title: Two draw-preparation helpers instead of four on the Deck (GOW3_PREP_WORKERS=2)
area: deck
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: upstream 1.0.0
class: G
commits:
files: gpu/shadps4/video_core/renderer_vulkan/vk_draw_prep.cpp, launcher/medievil_launcher.py
symbols: DefaultWorkerCount, prep_workers
switches: GOW3_PREP_WORKERS
tests: Launch 14
docs: plan-linux-deck.md#two-draw-preparation-helpers-2026-11-06-build-d2f145f-lows-log-datadeck-helpers2-1log, plan-linux-deck.md#launch-14-results-2026-11-06-1945-2022-build-5ead842-trace-2-helpers-logging-ini-67-uncapped-log-datadeck-pushpull-1log-traces-research-datadeckdeck-pushpull-auto1fixed1100capsweep-csv, study-deck-rescore-rejected.md#a-switches-that-already-exist-no-build
depends_on:
related: lineage-draw-preparation, T2, lineage-texture-set-memo, stage-b-core, core-sched, deck-preset
---
## What it does

An existing switch, no new code: `GOW3_PREP_WORKERS=N` (`DefaultWorkerCount`, clamped 0-16) sets the number of
draw-preparation workers; unset, it is half the hardware threads clamped 1-8 (Deck 4, 16-thread PC 8). The launcher's
"Draw preparation helpers" (`prep_workers`, Advanced) writes the env. Tried: 2 on the Deck (fewer busy threads at the
15 W cap, less contention on stage B's SMT sibling).

## Invariants

- Unset stays the default on every platform; the Deck preset does not set it.
- Class assumption of the idea: workers cost power and sibling time without being needed. Against it: an unprepared
  draw cannot use the texture-set memo (`BindTexturesFromSet` returns at `!prepared`) and binds on the slow path,
  ~15 us a call on the Deck (patch audit #3), so fewer workers can cost stage B more than they save.

## Interactions

- `lineage-draw-preparation`: the workers themselves (SCHED_IDLE on Linux, idle priority on Windows).
- `T2` / `lineage-texture-set-memo`: only prepared draws reach the memo; watch "prepared" share and slow binds.
- `core-sched`: the APU study proposed the cookie as the sibling fix instead of fewer workers (also rejected).
- `stage-b-core`: the only earlier 2-worker run was confounded with the dedicated core (rescore A4).

## Evidence

- 1,400 MHz (`data/deck-helpers2-1.log`, last 17 windows in the heavy spot): 76.3 vs 76.5 FPS, low 1% / 0.1%
  57.8 / 52.6 vs 56.6 / 51.0, sd 1.66 vs 1.88 ms, p99 17.20 vs 17.64, stage B 68.8 vs 75.1%. The user: "1400mhz was
  a little ambitious, the CPU gets starved. However, in 'auto' mode, 2 helpers is worth a couple frames."
- Launch 14, auto clock: 80.8 FPS mean (median 80.95, low 1% 63.7, 0.1% 56.4) against launch 10's 81.7 with 4
  workers: inside the between-launch noise; "the user's '+2 FPS' is not confirmed in this view".
- Launch 15 also ran with 2 workers; no 2-vs-4 A/B in one launch exists.

## History and pitfalls

- Rescore A4 predicted -0.5 to +0.5 ms; an earlier confounded run showed 90% of draws prepared with 2 workers
  (vs 97.8%), up to ~1.5 ms more slow binds [inferred].
- Not a code rejection: the switch stays. Worth a retry as a same-launch comparison at a 60 cap judged by CPU power
  and per-thread CPU (the method since launch 14), or with `GOW3_PREP_WORKERS=3`, while watching the unprepared share.
