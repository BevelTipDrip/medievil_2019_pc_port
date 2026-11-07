---
id: R4a
title: r.NumBufferedOcclusionQueries=2 (rejected)
area: ue4
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: ours
class: M
commits:
files: scripts/ue4_override.py
symbols: apply
switches:
tests: R4a
docs: plan-frame-pacing.md#r4a-result-2026-10-18-1405-1415-launched-by-claude-with-the-users-go-rejected, fix-inventory.md#10-tried-and-dropped
depends_on:
related: hzb-remap, occlusion-queries-off, frames-queued-2, ue4-cvars-tool
verified: 2026-11-07
---
## What it does

A config-only test (no runtime change), through the then-new `GOW3_UE4_CVARS` pak option:
`r.NumBufferedOcclusionQueries=2` (UE 4.21 default 1) so the render thread reads occlusion queries two frames old,
long decoded by stage A, instead of spinning on `Sleep(0)` in `RHIGetRenderQueryResult` (0x689c10) until the frame
that issued them was decoded. Query artifacts barely apply here: the port answers every query "visible".

## Invariants

- Nothing in the code: `ue4_override.apply` writes `r.NumBufferedOcclusionQueries` only as `3`, and only together
  with the HZB remap (`hzb`). A value of 2 with queries on and no remap is this rejected test.

## Interactions

- hzb-remap later uses `r.NumBufferedOcclusionQueries=3` (T-HZ4e: the remaining ~15-18 queries a frame no longer
  spin), in a different pipeline (HZB, no map wait, after many stage A/B changes).
- occlusion-queries-off removed the queries entirely (T-O1), the route taken instead.
- frames-queued-2: named in the write-up as a condition under which a retry could make sense.

## Evidence

- R4a (2026-10-18 14:05-14:15, Graveyard, + 4K boot, R1, `GOW3_WAIT_SITES=1`, log `data/r4a-occlusion2.log`): the
  spin at guest+0x689c89 fell from ~1.3 M to ~100 k calls/s, but at 2,900-3,300 draws a frame (T-R23's 161 windows vs
  R4a's 23) FPS median / min 94.3 / 80.7 -> 75.3 / 74.6, p99 14.6 -> 16.2 ms, blocked on GPU ticks 1.3% -> 21.9%.
- The user: GPU at ~50% in the busy area, the frame rate "rapidly jumping between 75 FPS and" higher, "performance
  may be somewhat worse ... I don't believe we're actually waiting on the GPU."

## History and pitfalls

- "GPU ticks" is the CPU waiting for submitted work (`Scheduler::Wait`), a sync stall, not GPU throughput. Inferred:
  the occlusion wait had throttled the render thread; without it the thread ran into a harder sync point (likely
  frames queued 1, or query / command memory reuse). Not confirmed.
- The write-up's rule: "Do not retry R4a without a new reason" (e.g. together with frames queued 2, or after the sync
  point is found with `GOW3_PERF_DIAG=1`). With HZB the buffered value 3 is already in use; with queries off it is
  moot.
