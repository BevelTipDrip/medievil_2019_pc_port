---
id: occlusion-queries-off
title: Occlusion queries off where the HZB remap cannot run (the port answers queries "visible")
area: ue4
status: active
platforms: all
default_windows: mixed
default_deck: mixed
origin: ours
class: M
commits: c8615eaab3
files: scripts/ue4_override.py, run.py, gpu/shadps4/video_core/amdgpu/liverpool.cpp
symbols: apply, OcclusionCounterValidMask, OcclusionCounterStep
switches: GOW3_HZB_REMAP, perf_hzb_occlusion
tests: T-O1, T-U1
docs: plan-frame-pacing.md#t-o1-occlusion-queries-off-2026-10-19-1920-1926-launched-by-claude-with-the-users-go-build-42a4943-log-datat-o1-no-occlusionlog, plan-frame-pacing.md#t-u1-uploads-intro-trace-bit-62-ab-2026-10-19-171x-1726-launched-by-claude-with-the-users-go-build-fde3624-log-datat-u1-uploadslog, fix-inventory.md#7-medievil-specific-ue4-config-eboot-patches-hooks, fix-inventory.md#caveats-in-the-evidence
depends_on:
related: hzb-remap, O2-O3, R4a
checked: 2026-11-07
---
## What it does

Finding (G): shadPS4's `ZPASS_DONE` / `PixelPipeStatDump` handler (liverpool.cpp, from the shared root commit
`6849d33`) writes every render backend's counter with the valid bit and an ever-growing count at decode, so every
occlusion query reads "visible": the queries cull nothing and their bounding-box draws cost ~150 draws a frame.
`ue4_override.apply` writes `r.AllowOcclusionQueries=0` whenever `hzb` is false, i.e. wherever the HZB remap does
not run: `perf_hzb_occlusion=0` (`GOW3_HZB_REMAP=0`), or an eboot whose sha256 / signatures the remap does not know.

## Invariants

- Exactly one of the two culling routes is active per launch, decided by the env `GOW3_HZB_REMAP == '1'` that run.py
  passes to the pak writer: queries off, or queries on + HZB + 3 buffered queries. Writing both apart re-creates
  either useless query draws or T-HZ1's wrong culling.
- Correct only while the port's answer stays "visible": with real results (`GOW3_OCCLUSION=1`, O2-O3,
  `rasterizer->OcclusionEvent`) the queries would cull and this default would lose that culling.
- "Visible" can never hide what should be drawn and never leaves the game waiting on GPU data; queries off loses
  nothing the port delivered.
- Defaults: Windows and Deck (`perf_hzb_occlusion=1` in the preset) have the remap on for 01.00 / 01.02 eboots
  whose sha256 the profile knows, so this card is the default only for other eboots or with
  `perf_hzb_occlusion=0` (hence `mixed`). run.py only `setdefault`s `GOW3_HZB_REMAP=1`: a 0 from the ini, the
  environment or extra-env.txt wins.
- liverpool.cpp is listed for the finding (shadPS4 code from the root commit, not changed by this patch): changing
  that synthetic answer changes what this default trades away.

## Interactions

- hzb-remap replaced it as the default (`57dc5c4`, 2026-10-20); this is its fallback.
- O2-O3: real query results through Vulkan occlusion queries, correct but cost more than they saved (T-O3c).
- R4a: tried first to remove the render thread's query wait (2 buffered queries), slower.

## Evidence

- T-O1 (2026-10-19, build `42a4943`): 0 occlusion queries (was ~130-160 a frame); heavy view ~2,635 draws at 132-135
  FPS, p99 8.4-9.2 ms vs T-D2d (queries on) ~2,785 draws, 127-130 FPS, p99 8.6-9.5: ~150 fewer draws, ~+5% at the
  same draw rate. The user: "I don't see any issues, nothing stands out to me ... so are my 1% lows and average FPS."
- T-U1 (2026-10-19): occlusion culling effectively off, every primitive in the frustum drawn.

## History and pitfalls

- First a run-game.ps1 default (T-O1), then the pak default in `c8615ea`; the 2026-10-20 inventory still listed it as
  the default after `57dc5c4` (fix-inventory Caveats).
- The synthetic answer is not MediEvil- or God of War-specific: shadPS4 main has the same block; any shadPS4-lineage
  port culls nothing by occlusion queries.
