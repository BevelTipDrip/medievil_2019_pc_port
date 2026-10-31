# Work queue: what is next, in order

Status: started 2026-10-30 (the user's list after N-26). Parent: [START-HERE.md](START-HERE.md). The single to-do list:
update an item's state here when it moves; details live in the linked plan or study. States: `open` / `in progress` /
`done` (with commit) / `dropped` (with why).

## The user's order (2026-10-30)

| # | Item | State | Where |
|---|---|---|---|
| 1 | **Release on GitHub + upstream check.** Build a proper release of `lab`; re-check what upstream (bc160201983) has added since the 1.3.5 merge (their focus has been other areas; ours is performance). Publishing needs the user's go. **2026-10-30:** upstream at 1.3.8 (19 commits not in `lab`); merged on branch `merge-1.3.8` (`e308c4c`, builds; 17 conflicts resolved, see its commit message). **No regression from the merge (2026-10-30):** the pre-merge `lab` build with the same settings and logging
(`t-lab-1-premerge-check`, effects 100, `frames_queued=1`) is no better than the merge (`t-m138-1-check`,
`t-m138-3-upstream-off`): heavy normal frames 4.55 ms (lab) vs 4.45 / 4.48 ms, lowest gameplay 5-s window 180 FPS vs
186 / 184, heavy-scene frames ≥ 30 ms 4 (30-37 ms) vs 0 / 1. The better alley lows remembered from before were
from runs without logging. **Clean run of the merge with the flip trace** (`t-m138-flip-1`, `-NoStats`,
`GOW3_FLIP_TRACE`, trace in `research-data\flip`; tools `research/tools/flip_lows.py` / `flip_stages.py`; N-27): (a) the fight dip the
user sees (~100 FPS) is 86 frames over 0.74 s, ramping smoothly 7.6 → 9.9 ms, with queued → GPU-ready equal to the
frame time: **GPU-bound** (the effects burst at 100% effects, N-18/N-19); (b) **isolated single-frame stalls of ~7 ms**
in a flat 237 FPS scene (81, 96, 116 s, ...) that land on a different stage each time (present thread, game submit →
stage A, recording): process-wide, not draw cost. Whether (b) is new with the merge is open: next, the same trace
with upstream's host switches off (`GOW3_TIMER_RES=0`, `GOW3_POWER_THROTTLE=1`, `GOW3_PAGEABLE_MEMORY=0`,
`GOW3_FUTEX_COND=0`), or the `lab` build with the same trace. **Host switches off (`t-m138-flip-2-host-off`):**
the stalls stay and come more often (5.3/min at the cap vs 2.8/min; 10-200 ms frames 148 in 208 s vs 150 in 467 s):
upstream's host switches are not the cause and stay on. **Pre-merge `lab` with the same trace (`t-lab-flip-1`):
both are old.** Fight dips to a 1% low of 99-105 FPS (10-11.8 ms frames), as on the merge (103); stalls 2.0/min at the cap
(7, up to 9.9 ms) vs 2.8/min on the merge (19, 7 of them over 10 ms): perhaps somewhat larger on the merge, too few to
tell. Fix them on the merge: the stalls first (a WPR capture, the user runs it: CPU sampling + context switches,
stall times from the flip trace), the GPU effects burst after (which passes grow; effects at 75% is the default).
Then A/B of upstream's switches (`GOW3_FUTEX_COND`, `GOW3_TIMER_RES`, `GOW3_PAGEABLE_MEMORY`, `GOW3_SPARE_IMAGE_BLOCK`, `GOW3_TRACKER_MERGE_GAP`), then **move the user's game to 01.02** (the user is getting the update package) and port our 01.00-only hooks (HZB remap, level hook, live cvars, boot display size / 4K metrics patches) by finding them per eboot, as upstream's `ue4_override.setres_slot` does for r.SetRes. Then the release. | in progress | [plan-upstream-merge.md](plan-upstream-merge.md), [study-upstream-commits.md](study-upstream-commits.md), [repo-boundaries.md](repo-boundaries.md) |
| 2 | **Pending fixes from the 2026-10-29 research:** go through the list below, aim also at lower-end hardware. | open | [study-frame-cost-2026-10-29.md](study-frame-cost-2026-10-29.md) §2 |
| 3 | **Stage B per draw** (pending fixes 2, 3, 4, 6), then the **ring reuse retest** (with the stamp fix; the user saw better 0.1% lows with reuse during effect-heavy stretches, N-15). | open | below; [plan-frame-pacing.md](plan-frame-pacing.md) N-15, N-16 |
| 4 | **Graphics settings:** live switches for the quality options, and more settings the launcher lacks, so a player can min-max (highest frame rate, keep what they care about). | open | [plan-live-switches.md](plan-live-switches.md) |
| 5 | **DLSS / FSR** for slower GPUs and CPUs (on the 4090 / 7800X3D it may not matter any more). | open | [plan-dlss.md](plan-dlss.md), [plan-fsr-frame-generation.md](plan-fsr-frame-generation.md), [plan-ue4-motion-vectors.md](plan-ue4-motion-vectors.md) |
| 6 | **Scaling on lower-end hardware:** measure raw costs on this system, then emulate fewer cores / threads and a slower GPU, and look for non-linear drops (the CPU/GPU pipeline is asynchronous; a bottleneck on one side may behave badly). | open | to write: `study-scaling.md` |

## Pending fixes (research 2026-10-29, ranked there; state now)

**Stage B review (2026-10-30, on `lab` = upstream 1.3.8 merged).** Still valid after the merge: upstream's
`GOW3_RECORD_DATA_FIX` adds a `SyncWithDrawRecorder()` to every `RecordData()` (1 + N per descriptor push), but on
stage B it returns at once (`DrawPipe::OnStageB()`), so per-draw cost is unchanged and item 2's design stands.
Step 0's bind counter landed (`32586bc`, "Binds per frame"); the rest of the Step-0 counters (joint S1 predicate,
writes per push, why a texture set is not memoized, FindImage generation-only misses, PrepareRenderState sub-timers
for the 0.5-2 ms render-state spikes in ~8% of heavy frames) was never built. Order: (1) the Step-0 counter build
(stats-gated, no behaviour change; gates items 3, 4, 8, 10, and the sub-timers bear on the user's question about
single slow frames under steady load, N-27); (2) item 6 LTO (no code, A-B-A); (3) item 2 behind a Gow3Toggle bit,
A/B inside one launch; (4) item 3's sampler-handle cache (key also on `ta_bc_base` and `sampler_lod_bias`), S1 only
if its gate passes; (5) item 4 if gated in; item 8 last (risky). Then the ring reuse retest (stamp fix in `32586bc`).

| # | Fix | Expected | State |
|---|---|---|---|
| 1 | Preshadows cut | stage B -0.4 to -0.9 ms in busy views | **done as a live switch** (N-25, `fdd12e8`), default on. Open: variant (a) `r.Shadow.PreShadowFadeResolution=32;r.Shadow.MinPreShadowResolution=16` (PS4-1080p-equivalent culling at 4K, keeps them) if the user wants them on but cheaper |
| 2 | Bulk descriptor copy per push (Bloodborne 0.3), one ChunkGuard | stage B -0.1 to -0.2 ms (2.5-6%) | **done** (`e0c7566`, N-30: descriptors -0.12 us a draw, ~-0.18 ms of stage B a heavy frame) |
| 3 | Sampler-handle cache in the texture-set memo, then S1 delta-draw | -0.1 ms, then -0.25 to -0.35 ms | sampler cache **go** (N-28: samplers 0.12 ms); S1 needs its predicate counter (not built) |
| 4 | Memoizable sets that contain the draw's own target | -0.08 to -0.17 ms | **gate passed** (N-28: the dropped sets are own-target ones, colour more than depth; ~78 a heavy frame, ~2.3 us each) |
| 5 | Mid-frame 4K stencil-plane copy | GPU <= 0.1 ms | open, low |
| 6 | LTO build (ThinLTO, clang/MinGW) | 0-2% stage B | **done** (N-29: ThinLTO on by default; equal or slightly better, within noise) |
| 7 | Volumetric fog grid at PS4 density for 4K (GridPixelSize 16 -> 32) | GPU 0.05-0.2 ms; more on weak GPUs | open; a candidate quality option (item 4) |
| 8 | Skip identity GPU-particle sim draws | -0.15 to -0.28 ms | open, risky (frozen particles) |
| 9 | Shadow-map transition batching | GPU 0.02-0.12 ms | open, re-evaluate after 1 |
| 10 | FindImage generation-only misses | lows +1-3 FPS | **dropped** (N-28: 3-9 generation-only misses a frame, gate 50); render-state spikes now 0.8% of heavy frames, in target creation |
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
| S1 boot crash (~1 in 8 launches): `pthread_mutex_lock` on a garbage mutex, found in the Windows event log ([plan-baseline-issues.md](plan-baseline-issues.md) S1) | open: lock sites in `runtime_mutex.c`, then a crash dump |
| `frames_queued=2` now smoother than 1; default stays 1 so testing does not mask real issues (N-25) | decided |
