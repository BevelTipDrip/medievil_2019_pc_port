---
id: early-z-pass-off
title: r.EarlyZPass=0 alone (PS4 depth prepass off)
area: ue4
status: superseded
superseded_by: no-decal-prepass
platforms: all
default_windows: off
default_deck: off
origin: ours
class: M
commits: fca0d8534a
files: scripts/ue4_override.py
symbols: apply
switches:
tests: T-Z1
docs: plan-stage-b-split.md#t-z1-depth-prepass-off-2026-10-21-0058-0122-launched-by-claude-with-the-users-go-build-8acd280-logs-datat-z1-earlyz-offlog-datat-z1-baselinelog, plan-stage-b-split.md#t-z2-t-z4b-dbuffer-decals-and-the-forced-prepass-2026-10-21-0200-0314-launched-by-claude-with-the-users-go, fix-inventory.md#10-tried-and-dropped
depends_on:
related: no-decal-prepass, ue4-cvars-tool
verified: 2026-11-07
---
## What it does

`fca0d85` made the override pak write `r.EarlyZPass=0` (the PS4 build sets 1 in PS4Engine.ini; DefaultEngine.ini
says 0) to drop the depth prepass of opaque occluders, on the premise that it cost ~120 draws a frame for a GPU that
was not the limit. Replaced the same night by `cd72da1` (no-decal-prepass), which writes `r.EarlyZPass=1` again
together with `r.DBuffer=0`. No code of it remains.

## Invariants

- None live: `ue4_override.apply` no longer writes `r.EarlyZPass=0`. Anyone re-adding it must know that with DBuffer
  decals on UE 4.21 runs a full prepass regardless (`r.EarlyZPass` does not control it), and with `r.DBuffer=0` it
  removes the remaining partial prepass the GPU uses for early depth.

## Interactions

- superseded by no-decal-prepass: the real cost was DBuffer's forced full prepass (628-739 draws), not the
  partial one this switch controls.
- `GOW3_UE4_CVARS='r.EarlyZPass=0'` still reproduces it for an A/B (ue4-cvars-tool).

## Evidence

- T-Z1 (2026-10-21, build `8acd280`), same heavy spot: baseline ~2,280-2,310 draws, 157-162 FPS, p99 7.4-7.6 ms;
  `r.EarlyZPass=0` ~2,180 draws at 176-178 FPS, p99 6.8-6.9 ms (standing view ~2,015 draws, 189-194 FPS). The user
  saw no visual artifacts. A first T-Z1 with the monitor off ran at a 60 Hz FIFO swapchain and is not comparable.
- T-Z2 heavy-view captures: the prepass was still a full one (628 / 739 draws) whatever `r.EarlyZPass` said, so
  T-Z1's "~120 fewer draws" is not what the captures show.

## History and pitfalls

- Default for about two hours (commits 2026-10-21 01:22 to 03:26); fix-inventory section 10 lists it as tried and
  dropped: "T-Z1 gain was real but the full prepass came from DBuffer decals".
- Worth retrying only if a scene is found where the partial prepass costs more GPU than early depth saves, with
  `r.DBuffer=0` kept (T-Z2 measured that combination: best standing lows, larger panning spikes).
