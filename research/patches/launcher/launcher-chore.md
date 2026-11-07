---
id: launcher-chore
title: Launcher text fixes with no behaviour of their own
area: launcher
status: chore
platforms: all
default_windows: n/a
default_deck: n/a
origin: ours
class: G
commits: de69b9f1bd
files: launcher/medievil_launcher.py
symbols: PERF_PATCHES
switches:
tests:
docs:
depends_on:
conflicts_with:
supersedes:
related: perf-patches-ini, hzb-remap, game-update-0102
---
## What it does

- `de69b9f`: the "HZB occlusion culling" description in `PERF_PATCHES` used to say MediEvil 01.00 only. It now
  says "MediEvil 01.00 and 01.02", because the HZB remap hooks were mapped for 01.02 too (game-update-0102; run.py
  turns on `GOW3_HZB_REMAP` for every eboot `src/hzb_hook.h` knows). Text only: the key, the variables and the
  default did not change.

## Invariants

- The launcher texts in `PERF_PATCHES` and the in-game `hints` in `PerfPatchesSection` (gow3_overlay.cpp) are
  written separately. A text fix in one should be checked against the other; no test compares them.
