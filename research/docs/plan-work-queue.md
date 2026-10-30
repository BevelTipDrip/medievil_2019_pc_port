# Work queue: what is next, in order

Status: started 2026-10-30 (the user's list after N-26). Parent: [START-HERE.md](START-HERE.md). The single to-do list:
update an item's state here when it moves; details live in the linked plan or study. States: `open` / `in progress` /
`done` (with commit) / `dropped` (with why).

## The user's order (2026-10-30)

| # | Item | State | Where |
|---|---|---|---|
| 1 | **Release on GitHub + upstream check.** Build a proper release of `lab`; re-check what upstream (bc160201983) has added since the 1.3.5 merge (their focus has been other areas; ours is performance). Publishing needs the user's go. **2026-10-30:** upstream at 1.3.8 (19 commits not in `lab`); merged on branch `merge-1.3.8` (`e308c4c`, builds; 17 conflicts resolved, see its commit message). Next: regression runs on 01.00 (t-m138-*), A/B of upstream's switches (`GOW3_FUTEX_COND`, `GOW3_TIMER_RES`, `GOW3_PAGEABLE_MEMORY`, `GOW3_SPARE_IMAGE_BLOCK`, `GOW3_TRACKER_MERGE_GAP`), then **move the user's game to 01.02** (the user is getting the update package) and port our 01.00-only hooks (HZB remap, level hook, live cvars, boot display size / 4K metrics patches) by finding them per eboot, as upstream's `ue4_override.setres_slot` does for r.SetRes. Then the release. | in progress | [plan-upstream-merge.md](plan-upstream-merge.md), [study-upstream-commits.md](study-upstream-commits.md), [repo-boundaries.md](repo-boundaries.md) |
| 2 | **Pending fixes from the 2026-10-29 research:** go through the list below, aim also at lower-end hardware. | open | [study-frame-cost-2026-10-29.md](study-frame-cost-2026-10-29.md) §2 |
| 3 | **Stage B per draw** (pending fixes 2, 3, 4, 6), then the **ring reuse retest** (with the stamp fix; the user saw better 0.1% lows with reuse during effect-heavy stretches, N-15). | open | below; [plan-frame-pacing.md](plan-frame-pacing.md) N-15, N-16 |
| 4 | **Graphics settings:** live switches for the quality options, and more settings the launcher lacks, so a player can min-max (highest frame rate, keep what they care about). | open | [plan-live-switches.md](plan-live-switches.md) |
| 5 | **DLSS / FSR** for slower GPUs and CPUs (on the 4090 / 7800X3D it may not matter any more). | open | [plan-dlss.md](plan-dlss.md), [plan-fsr-frame-generation.md](plan-fsr-frame-generation.md), [plan-ue4-motion-vectors.md](plan-ue4-motion-vectors.md) |
| 6 | **Scaling on lower-end hardware:** measure raw costs on this system, then emulate fewer cores / threads and a slower GPU, and look for non-linear drops (the CPU/GPU pipeline is asynchronous; a bottleneck on one side may behave badly). | open | to write: `study-scaling.md` |

## Pending fixes (research 2026-10-29, ranked there; state now)

| # | Fix | Expected | State |
|---|---|---|---|
| 1 | Preshadows cut | stage B -0.4 to -0.9 ms in busy views | **done as a live switch** (N-25, `fdd12e8`), default on. Open: variant (a) `r.Shadow.PreShadowFadeResolution=32;r.Shadow.MinPreShadowResolution=16` (PS4-1080p-equivalent culling at 4K, keeps them) if the user wants them on but cheaper |
| 2 | Bulk descriptor copy per push (Bloodborne 0.3), one ChunkGuard | stage B -0.1 to -0.2 ms (2.5-6%) | open |
| 3 | Sampler-handle cache in the texture-set memo, then S1 delta-draw | -0.1 ms, then -0.25 to -0.35 ms | open |
| 4 | Memoizable sets that contain the draw's own target | -0.08 to -0.17 ms | open, gated on the Step-0 counter |
| 5 | Mid-frame 4K stencil-plane copy | GPU <= 0.1 ms | open, low |
| 6 | LTO build (ThinLTO, clang/MinGW) | 0-2% stage B | open, quick (no code) |
| 7 | Volumetric fog grid at PS4 density for 4K (GridPixelSize 16 -> 32) | GPU 0.05-0.2 ms; more on weak GPUs | open; a candidate quality option (item 4) |
| 8 | Skip identity GPU-particle sim draws | -0.15 to -0.28 ms | open, risky (frozen particles) |
| 9 | Shadow-map transition batching | GPU 0.02-0.12 ms | open, re-evaluate after 1 |
| 10 | FindImage generation-only misses | lows +1-3 FPS | open; parking (N-24) removed one source of bumps |
| 11 | `r.Shadow.MaxResolution=1024` | 0-5% | open, after 1; risk to the 0.1% lows |
| 12 | Real results for the ~20 shadow occlusion queries | -1% to +3% | open, low |

## Found on the way (2026-10-30)

| Item | State |
|---|---|
| The in-game menu doubles ~60 frames a second while open (~177 FPS, 296 frames at ~8.7 ms per 5 s; N-25) | open |
| Parking: some 4K pairs made new while one sat parked (a texture lookup first, other geometry fields?) (N-24) | open, small |
| `ue4_override.py` High/Ultra: `r.Shadow.MaxResolution`, `r.Shadow.DistanceScale`, `r.Shadow.CSM.MaxCascades` overwritten later by the local / sun shadow tables (study-live-switches.md) | open: the presets don't do what they say |
| Pak values likely inert: `r.GarlicHeapSizeMB` / `r.OnionHeapSizeMB` (not in the 01.00 eboot), `r.Streaming.PoolSize=1600` (PS4 device profile sets 2000 at a higher priority) (study-live-switches.md) | open: verify, then drop or fix |
| Toggle-file bit 31 switches two things (narrow image stages, pool views kept across a decommit) (study-live-switches.md) | open, small |
| CLAUDE.md says `origin` is the public upstream; it is the BevelTipDrip fork (`upstream` is bc160201983, push disabled) | the user to edit (Claude does not edit CLAUDE.md unasked) |
| `frames_queued=2` now smoother than 1; default stays 1 so testing does not mask real issues (N-25) | decided |
