# Study: where heavy frames lose time (2026-10-29)

Status: offline research only (a workflow of 35 read-only agents: a ledger of prior work, six lenses, one skeptic per
candidate, a synthesis and a completeness critic). Nothing launched. Parent: [plan-frame-pacing.md](plan-frame-pacing.md)
N-16. Agent scratch outputs (scripts, exports, the 114-entry ledger): session scratchpad `research\` (not kept).
Section A is the synthesis (skeptic corrections applied); section B is the critic's review of it, which corrects A
in places (B wins where they differ: B.2 bursts, B.3 the narrow-2 counters, B.1 the ring).

# A. Synthesis


Marks: [V] means seen in data or code by a lens or the skeptic. [?] means inferred. Where the skeptic corrected a lens, the corrected value is used.

**Caveat on the data.** Almost every heavy-area per-frame log was recorded with **ring reuse ON** (t-pan5-growth, t-flip1-split; the "constant ring … 10.41 MB more reused" line) [V]. Ring reuse is now OFF. The RenderDoc captures 41595/42818 come from before DBuffer was turned off, and they are capture-stall frames [V]. t-p960-gpu-profile-5 ran at a 120 cap, before lean barriers. Its windows count each real frame twice, so once-per-frame passes print 0.5/frame [V]. nsys-fight-1-1 is an older build with tracing overhead (151 FPS) [V]. Every number below needs re-baselining on current defaults (Step 0).

---

## 1. What the frame time goes to

### GPU work (goal 1)
- **Metric.** Use GPU active time, not frame length. In ngfx fight-1 the five frames last 6.5–10.0 ms, but GPU active time is flat at 6.19–6.28 ms. All of the spread is idle time (0.30–3.72 ms). The "10 ms" fight-1 frame is 6.3 ms of work plus 3.7 ms of waiting for the next submit [V, GPUTRACE_FRAME.xls, decomp.py].
- **N-5 correction.** In ngfx-ab-1, narrow image stages changed GPU active time from 4.876 to 4.836 ms (**−0.8%**). The reported 9% came from idle variance (mean idle 0.84 → 0.39 ms) [V]. The ledger and the "CURRENT STATE ~9%" line should be corrected.
- **The real ~10 ms GPU frames are ngfx-narrow-2's third trace** (`medievil-runtime_2026_10_28_02_12_32.ngfx-gputrace`, a fight with a screen-filling green effect at low health): **9.46–9.76 ms active**, GR 97%, idle 0.23–0.29 ms [V]. Compared with fight-1:

  | Counter | narrow-2 vs fight-1 |
  |---|---|
  | SM instructions | 2.1× (5.2e9 vs 2.45e9) |
  | Pixel warps | 2.56× |
  | Depth-passed pixels (prop2zrop) | 2.15× (1.25 → 2.7e8) |
  | Colour-ROP pixels | ~flat (1.78e8 vs 1.76–1.81e8) |

  So the extra work is in **passes with no colour output**: depth-only, masked or discarding passes. It is not translucency [V counters; which passes is ?]. The preshadow receiver-stencil redraws at 4K (depth-tested, no colour) fit this signature, and so do masked-material depth or shadow passes [?].
- **Ordinary heavy frame cost.** 4.8–6.3 ms active at the locked 2.19 GHz, roughly 4.0–5.1 ms at play clocks of 2.65–2.7 GHz [? clock scaling]. In ab-1, pixel-warp launch is register-allocation-limited 21–29% of cycles, long-scoreboard L1TEX stalls dominate, and the top 10 shaders hold ~60% of warp time (0xf0ce740f 10.5%, 0xcd6edcde 7.8%, 0xd88fc300 7.1%) [V].
- **Known per-class costs** (profile-5, ×2 corrected, segment-timed so these are ceilings) [V]:

  | Pass class | ms per frame |
  |---|---|
  | Fog scattering (27e12fb9 / 3ab54396, 60×34×16) | 0.15–0.28 |
  | Light-grid injection (0ca8545f) | 0.055 calm, 0.16–0.2 fight; tracks light count, not cells |
  | Shadow 512 D16 + 4K stencil + projection | 0.18–0.52 per real frame |

- **Shadow/preshadow work is the largest repeated block.**
  - Capture A: 26 separate 512² D16 maps per frame, each written once and read 1–4 times; capture B: 34 [V].
  - 9 (A) / 21 (B) of those maps hold environment preshadows [V]. A 1024² preshadow cache (R182075) serves exactly 15 projections in both captures, so the cache is **full**. The overflow, 23 (A) / 45 (B) preshadows, is re-rendered every frame [V counts; ? that this happens every frame].
  - Preshadows altogether: ~260 (A) to ~445 (B) draws (11–16%) and ~94–160 passes (25–35%) [V].

### GPU waiting (goal 2)
- **Between frames** (nsys fight): GPU idle 7.4–7.5% of the span. 607 of 646 ms is idle because stage B had not submitted yet; only 38 ms is idle with work already queued [V].
- **Frames run lockstep with frames_queued=1.** In nsys, 776 frames have the GPU waiting on stage B, 483 have stage B waiting on the GPU, and the limiter switches 335 times. GPU time and stage-B time correlate at 0.60 [V].
- **Inside the frame, drains are cheap individually.**
  - ab-1 has 204–214 go-idles per frame against ~155 barriers plus ~48 graphics↔compute switches [V counts, different runs, ?].
  - fight-1 frames with 312 vs 487 go-idles have the same active time. 175 extra drains add ~0.13 ms of WFI stall, ~0.7 µs each [V].
  - The "WFI 44–82%" share counts front-end wait cycles that overlap real work. Do not use it as lost time.
  - The ceiling for drain/bubble loss is N-1's GR-active minus SM-active gap, ~1.3–1.7 ms per frame [V], and only part of that is removable.
- **Copy windows.** 0.435 ms per frame has sync-CE ≥5% [V]. Of that, **0.21 ms sits in one window 20–30% into the frame**, with SM active 18% and PCIe RX only 22%. That is a VRAM-side copy, most likely the 4K stencil-plane copy (CopyBufferToStencil, vk_runtime.cpp:765) [?]. The total SM deficit across all copy windows, measured against the 71% frame mean, is ~0.165 ms [V]. The copy engine itself is busy only 0.062 ms per frame [V].
- **Unattributed.** 0.1–0.2 ms per frame of compute at SM issue 2.4% with PCIe RX 43% (the GPU reading system memory) [V]. Its source is unknown and it is not tied to staging [?].

### CPU feed (goal 3)
- **Stage B is the critical path in ordinary heavy frames.** t-pan5: 4.55 ms frame, 1,673 draws, packet 4.08 ms, ticks wait 0.10 ms. Stage B shows "busy" 88–91%, but that figure includes flip and EOP waits (plan-stage-b-split.md:25) [V].
- **Stage B draw section 3.49 ms per heavy frame** (t-pan5-growth.log:2300-2420, sections on) [V]:

  | Part | ms |
  |---|---|
  | Textures | 0.77 |
  | Buffers | 0.23 |
  | Vertex | 0.52 |
  | Index | 0.22 |
  | Descriptors | 0.46 |
  | Begin render | 0.26 |
  | Render state | 0.16 |
  | Dynamic | 0.16 |
  | Emit vertex | 0.12 |
  | Tasks | 0.41 |
  | Unattributed | ~0.64 |

- **Stage A** spends 26% of its time in drains and is idle 0.3%. The guest is never late [V].
- **CPU-slow frames** run 6.9–8.3 ms, exactly the 130–145 FPS low band. They carry +19–28% draws, +62–72 slow image lookups, ~140–165 unprepared draws and texslow 0.54 ms. Draw count alone explains ~0.7–1.0 ms of their +1.6–2.0 ms draw-section excess [V].
- **Render-state spikes.** 0.5–2 ms spikes appear in ~8% of heavy frames, with no relation to lookup counts (R² 0.04–0.07) [V]. Cause unknown.

---

## 2. Ranked plan

### Step 0: re-baseline and fix the yardsticks (do first, no behaviour change)

1. **Metric policy.**
   - Goal 1: ngfx GPU active time per frame, together with SM throughput.
   - Goal 3: stage-B µs per packet and the draw section.
   - Frame time: p50/p99 and 1%/0.1% lows at the same spot, with stats off.
   - Never usage %, stage-B busy %, drain count or the WFI share alone.
   - Windows "GPU thread us/draw" always prints 0.00. It is `#ifndef _WIN32` at liverpool.cpp:1468 and measures stage A anyway [V]. Do not use it.
2. **Extend the uncommitted t-binds1 build with counters only** (stats-gated, one build):
   - **Joint S1 repeat predicate**, AND-ed, per draw: same pipeline, same texset slot hit for every stage, begin-render memo hit with equal signature, no dirty non-user-data register blocks, byte-equal vertex input. Also accumulate pool-section time for draws that match it.
   - **Descriptors:** writes per push, RecordData calls per packet, and an rdtsc sub-section over the RecordData loop (vk_pipeline_common.cpp:86-114).
   - **Texture sets:** reasons a set was not memoized at vk_rasterizer.cpp:2692 (is_target colour/depth, force_general, storage, needs_rebind, generation change, size); early returns at :2793-2800; TexSlow split into "memo not applicable" and "memo miss".
   - **FindImage:** generation-only misses (key matches, result equals cached image_id); images_registered delta per frame; a WaitTimer on the texture-cache mutex; sub-timers in PrepareRenderState (FindTargetMemoized→FindImage, FindRenderTarget, new-image creation).
   - **GPU particle sim:** draws keyed by guest PS hash, with counts for dt==0, dt==0 two frames running with the same tile hash, frames containing an inject draw, and stage-B time of the sim draws.
   - **TransferMark labels** on CopyBuffer, FillBuffer and CopyBufferToStencil (vk_runtime.cpp:117/765), for GOW3_GPU_PROFILE.
3. **GOW3_GPU_PROFILE fixes** (vk_gpu_profiler.cpp):
   - Count frames by flips. The 2× comes from two display passes per frame in today's code, not from merge hazard H2 [V].
   - Print class totals (shadow 512 D16, 4K depth-only, 4K projection, GBuffer, translucency, post, each compute hash) before the top 30.
   - Report per-frame p50/p99/max.
   - Add timestamps on present_scheduler and flip_scheduler; their GPU work currently lands in the "idle" label [V].
   - Validation already passes on existing data: (4.12 − 1.37…1.61) × 2 = 5.0–5.5 ms, against ngfx's ~5.1 ms at play clock [V].

### Ranked items

Ordered by heavy-frame gain per unit of risk and effort. **QW** marks a quick win.

#### 1. Preshadow cut / cull (QW, no build) · goals 3, 1, 2
- **Mechanism.** The 16-entry preshadow cache is full. 23–45 preshadows per frame overflow and are re-rendered. Each one costs a 512 depth map share, a 4K receiver-stencil redraw and a 4K projection pass [V].
- **Corrected gain** (for a view like the captures): −260 to −445 draws and −94 to −160 passes per frame. Stage B −0.4 to −0.9 ms at 1.5–2.0 µs per draw (these draws are cheaper than average). GPU ≤0.3–0.7 ms of timestamp span, mostly bubble, so likely less. Today's view may hold fewer preshadows: t-pan5 has 263–270 passes in total [V], against 459 in capture B.
- **Possible link to goal 1's 10 ms frames.** The receiver-stencil redraws at 4K add depth-passed pixels with no colour output, which matches narrow-2's signature [?]. Check against Z1 (section 5).
- **Confidence:** medium. **Risk:** quality only. Characters lose static shadows under stationary light with Preshadows=0. Single-sample shadowing depends on valid volume lighting samples [?].
- **Measure first.** One launch with `GOW3_UE4_CVARS='r.Shadow.Preshadows=0'`, the upper bound; this cvar is read per frame. Same spot: still, pan, fight.
  - Draws must fall ≥200 and passes ≥80, and the stage-B packet must fall ~1.5–2 µs per removed draw.
  - Then compare frame p50/p99 and the lows.
  - Screenshots: Dan under a mausoleum or tree in moonlight.
  - If draws don't drop, stop.
- **Then pick a variant, one per launch:**
  - (a) `r.Shadow.PreShadowFadeResolution=32;r.Shadow.MinPreShadowResolution=16`: PS4-1080p-equivalent culling at 4K, the only variant with no quality loss against PS4 [? 4K screen-radius inflation];
  - (b) `r.Shadow.ForceSingleSampleShadowingFromStationary=1`, checking its draw drop matches the upper bound (it may apply only when proxies are created) [?];
  - (c) cache-capacity probe `r.Shadow.PreShadowResolutionFactor=0.25`.
- **Dropped sub-tests:** CachePreshadow=1 (the cache already works [V]) and PreshadowsForceLowestDetailLevel (≈0).
- **Implementation:** a `perf_preshadows` switch in scripts/ue4_override.py apply() next to the DBuffer/FixDeltaSeconds block (:330-341), wired through graphics_options.py PERF_SWITCHES and gow3_settings.h:142.

#### 2. Bulk descriptor copy per push (Bloodborne 0.3 port), plus a single guard · goal 3
- **Mechanism.** BindResources makes 1 + N RecordData calls per push. Each call takes the ChunkGuard spin lock, a reserve check, Allocate+PrefetchAhead and a memcpy (vk_pipeline_common.cpp:88-114; vk_scheduler.h:805-818, 1002-1011) [V]. RenderDoc shows **~14 writes per push** (median 10–12, p90 27) and 0% exact push repeats [V]. Every rasterizer write already points into `image_infos` / `buffer_infos`, and none uses texel views [V]. So BB's bulk path (bloodborne_pc vk_pipeline_common.cpp:51-90: within-check, reserve sum + 256, 3 RecordData calls, rebase pointers) drops in.
- **Corrected gain:** ~0.05–0.12 µs per packet, ≈0.1–0.2 ms per frame, 2.5–6% of stage B. FPS changes only where stage B limits (+1–4%). No GPU effect and no effect on the lows.
- **Confidence:** medium. **Risk:** low. The old loop stays as the fallback.
- **Measure:** a Gow3Toggle bit flipped at runtime through GOW3_TOGGLE_FILE every ~15 s, at one spot with sections on. Expect "descriptors" 0.217 → 0.10–0.17 µs per packet. Drop it if the saving is under 0.03.
- **Extension:** take ChunkGuard once for the whole block, and give EmitVertexBuffers one block (7 guarded ops → 2, ~0.03–0.05 ms).
- **Later and separate:** skip identical setVertexInputEXT / pushConstants. That is ~0.02 ms of stage B, and the recorder isn't the limit. It is medium risk: about 12 host pipeline sites need invalidation (fsr_pass.cpp:238/313/358, pp_pass.cpp:234/253, vk_camera_motion.cpp:213/353, fault_manager.cpp:150) [V]. Build it only if (a) moves FPS.

#### 3. Sampler-handle cache in the TextureSet memo entry (QW), then S1 delta-draw · goal 3
- **Mechanism.** On every memo hit, BindSamplers re-hashes each S# with XXH3 and calls GetSampler under `samplers_mutex` (vk_rasterizer.cpp:2718-2737; texture_cache.cpp:1504-1521) [V]. Caching the handles in the memo entry removes ~0.12 ms per frame (sections on) and is low risk.
- **S1 proper is smaller than claimed.** BeginRendering is already memoized. The texset emit loop, Transit and the PrepareRenderState upscaler/camera-motion hooks (:266-296) must stay. Skipping UpdateDynamicState needs a clean-register-block check (:601-664) [V].
- **Corrected gain:** ~0.25–0.35 ms per frame with stats off (6–9% of stage-B draw time). In stage-B-bound slow frames (busy ≥87%), −0.3 to −0.5 ms. Zero in GPU-bound frames. Overlaps item 8.
- **Confidence:** medium. **Risk:** medium (cache invalidation: registry and scene generation, needs_rebind, layouts).
- **Gate:** the Step-0 joint predicate ≥55–60%, and the predicate-gated pool ≥0.25 ms per frame. If the sampler cache alone (~0.1 ms) doesn't move the lows, S1's ~0.3 ms won't move them much either.

#### 4. Make sets that contain the current target memoizable · goal 3
- **Finding.** This corrects T-S1c and study-stage-b-native.md:48-49. The steady ~104 "other key" misses per frame are not rotating images. When a set contains the draw's own target (`is_target`), or a force_general or storage image, the slow path writes `set_slot->key = 0` (vk_rasterizer.cpp:2679, 2692-2693), so the same set misses again every frame [V]. These are most likely translucency, soft-particle, fog and projection draws sampling depth in place [?].
- **Corrected gain:** 0.08–0.17 ms per frame of stage B (2–4%). The floor is constant across fast and slow frames, so there is no effect on the lows.
- **Risk:** medium (layout code). It needs a per-entry target/feedback flag, the depth-in-place or feedback transition redone on a hit, and the check at :2854 relaxed.
- **Gate:** Step-0 counter (a) puts ≥90 of the ~104 in is_target, and those binds cost ≥1 µs each.
- **Not the proposed fix:** a hash change or a per-image cache would gain about 0 (ImageDescHash is stable, vk_draw_prep.h:51-61).

#### 5. Identify, then avoid, the mid-frame 4K stencil-plane copy · goals 2, 1
- **Mechanism.** The 0.21 ms per frame window at SM 18% is probably CopyBufferToStencil. Its source is compute-written VRAM (buffer_cache.cpp:1102-1104), it runs once per frame, and its barrier comes from early-z+late-z+all (t-dw1:822) [V for the copy and barrier; ? that this is the window].
- **Corrected gain:** ≤~0.1 ms of GPU per frame.
- **Measure:** the Step-0 TransferMark label "stencil plane copy". The weak no-build cross-check is matching decile-2 bursts in fight.sqlite.
- **Fix options:** copy only when the plane really changed, or keep the stencil in a separate S8 image.
- **Blocked:** VK_EXT_shader_stencil_export is unavailable [V, all recent logs].
- **Dropped part:** the upload-prologue / download-epilogue batching is worth ~0.5–1% at high complexity. Do not build it unless "buffer upload" labels exceed ~0.05 ms.

#### 6. LTO build experiment (QW, no code) · goal 3
- **Facts.** build.sh:23 and build-pdb.sh:17 pass `-DGOW3_LTO=OFF`, while the CMake default is ON, with no reason recorded (0af6cd4) [V]. Bloodborne measured −1.8% with LTO and −4.8% with LTO+PGO, on GCC, Linux, 3 runs [V].
- **Corrected gain:** 0–2% of stage B. Clang ThinLTO on MinGW is unmeasured.
- **Steps:**
  1. Build with `GOW3_LTO=ON` into a separate out dir.
  2. Confirm `-flto=thin` and lld in build.ninja, that it links, and that windbg/symbolize still resolve.
  3. A-B-A at a fixed heavy spot with sections on. Keep it if packet µs falls ≥2% with A-B-A spread smaller than the difference.
- **PGO** needs clang code work: replace `__gcov_dump` in gow3gpu.cpp:203-226 with `__llvm_profile_write_file`, plus llvm-profdata and the use flags. Only after LTO pays.

#### 7. Volumetric fog grid at PS4 density for 4K (GridPixelSize 16 → 32) · goal 1
- **Facts.** The 4K grid is 240×135×64. The PS4 profile sets 16 px ("saves ~6msec/frame"); PS4_Neo inherits it [V].
- **Corrected gain:** 0.05–0.2 ms of GPU (1–4%), fight-dependent. No drain or oscillation effect. **Drop the light-grid half**: it is 0 to negative, because a single partial wave tracks light count, not cells [V reasoning, data 0.055 vs 0.2].
- **Open conflict:** a device-profile cvar may outrank `[SystemSettings]`, so `GOW3_UE4_CVARS` may be ignored [?]. Verify it took effect: fog volume 120×68×64 in GOW3_TEXCACHE_STATS, or a (30,17,16) dispatch.
- **Zero launch first:** read the three fog dispatch durations in the fight-1 and narrow-2 traces (Z2). Park it if under 0.15 ms.
- **Implementation:** add OVERBITE/Config/PS4/PS4DeviceProfiles.ini to the override pak files in ue4_override.py apply(), the same mechanism as GAME_CONFIG.

#### 8. Skip identity GPU-particle sim draws (dt=0 for ≥2 frames) · goal 3
- **Facts.** In the captures, 155/127 of 194 sim draws per run are dt=0 copies with output equal to input [V]. The light captures also have 194, a level-wide constant [V].
- **Corrected gain:** 0.15–0.28 ms of stage B if they qualify, and it could be ~0 if a global inject reset is needed. Overlaps S1.
- **Gate:** the Step-0 counter (c) × measured µs per draw ≥0.2 ms per frame.
- **Risk:** medium-high: frozen or flickering particles. A game hook needs Ghidra work on SimulateGPUParticles.

#### 9. Shadow-map transition batching · goal 2
- **Facts.** ~26–34 D16 transitions each way per frame [V]. Batching removes ~55–65 drains, but ~0 pass breaks, because each barrier already sits at a target change [V, old build].
- **Corrected gain:** GPU 0.02–0.12 ms. Projection pass merging via VK_EXT_dynamic_rendering_unused_attachments adds ≤0.05–0.1 ms GPU and 0.05–0.15 ms stage B, at medium-high risk because many depth-only pipeline variants are needed.
- **Cheapest form:** append Attachment→ReadOnly for every written shadow map to the barrier that already exists at the end of the shadow phase (eid ~9775). That adds zero drains.
- **Gate:**
  - Zero launch: in the ab-1 trace 02_57_34, measure the summed idle bubble across the shadow and projection ranges. Drop it if under ~0.05 ms per frame.
  - Re-evaluate after item 1, which removes many of these maps.
- **"Drop the global memory barrier" is a no-op:** srcStage ORs into the next flush (vk_runtime.cpp:934) [V].

#### 10. FindImage generation-only misses and render-state spikes · goal 3, lows
- **Corrected gain:** 0.05–0.15 ms on the 6.9–8 ms frames, so lows +1–3 FPS, not +10–15. The direct texslow slope is 1.4–1.5 µs per lookup [V].
- **The target-memo half is worthless:** exact render targets survive generation bumps through still_exact(), texture_cache.cpp:1054-1065 [V].
- **Build gates:**
  - Region stamps only if generation-only misses are ≥50 per slow frame.
  - Pursue lock holders only if mutex wait is ≥0.2 ms in spike frames.
- **The render-state spikes need the Step-0 sub-timers;** this mechanism does not explain them.

#### 11. r.Shadow.MaxResolution=1024, Min kept at 512 · goals 2, 3
- **Corrected gain:** 0–5% in heavy play. It removes ~17–28 atlases and their transitions; draws fall only in still views.
- **Risks:** (a) per-object shadows fall from 504 to 256 for most objects [?]; (b) point/spot maps resize with screen size again, the c2ad62e flicker and T-C2 hitch mechanism, which **could worsen the 0.1% lows**.
- Do it only after item 1. Kill it on any re-creation storm or a draw drop under ~60.

#### 12. Real results for the ~20 remaining shadow occlusion queries · goal 3
- **Corrected net:** −1% to +3%.
- **Costs:** O2 opens a query segment on all ~270 passes, adds ~1 extra submit per frame, and adds a blocking tail when results are later than 3 frames (T-O2d max 13–16 ms, T-O3c 59 ms) [V]. That tail risks the 0.1% lows.
- **Step 0 offline:** the RenderDoc depth-test overlay for the 19 P144886 boxes in `research-data\renderdoc\heavy_frame41595.rdc`. Stop if ≤2 are hidden.

**Stacking caution.** Items 1, 3, 8, 9 and 11 overlap: preshadow draws are the repeated-pipeline draws S1 targets, and the batching targets the same shadow maps. Re-measure after each one.

**Rough realistic total if items 1–6 pay.** Stage B −0.8 to −1.5 ms per heavy frame (item 1 is most of it). GPU −0.2 to −0.6 ms. Either one only shows in frames where it is the limiter [?].

---

## 3. The oscillation

**Shape (t-pan5 standing, reuse ON)** [V]:
- A floor at the Reflex interval (4,219 µs), with 30–45% of frames there.
- A body of 4.3–6 ms frames (p90 4.98, p99 5.84).
- Isolated 7–9.8 ms frames.
- Arithmetic: one ~9 ms frame per second at 225 FPS gives a 1% low of ~138 and a 0.1% low of ~112.

**Best explanations, in order of current likelihood:**

1. **Lockstep max(C, G) with correlated content spikes.** With frames_queued=1, each frame's period is max(stage-B time of N+1, GPU time of N). Both sit at ~4.1–4.6 ms in heavy views, so the limiter changes frame to frame (776 / 483 / 335 in nsys [V]). Both rise together with draw count (corr 0.60 [V]). A replay of the real trace gives about +1% from splitting submits or frames_queued=2, with lows unchanged [V]. frames_queued=2 was tried three times and hurt the lows (T-W1, T-Q2, N-15) [V]. **So this coupling cannot be tuned away; only the per-frame work on both sides can come down.**
2. **CPU-slow content frames.** These are the 6.9–8.3 ms frames (130–145 FPS band) with +19–28% draws, more slow lookups, texslow ×2, and render-state spikes [V]. Fix path: item 1 (fewer draws), then items 2–4 and 10.
3. **GPU-heavy effect frames (narrow-2 class).** 9.5–9.8 ms of real GPU work, from depth-only or masked pixel work [V counters, ? source]. These are event-driven, not steady. Fix path: Z1 attribution, then items 1 and 7.
4. **N-15 ring-reuse GPU-completion spikes.** The 1 Hz / 4 s-locked ~9 ms frames in t-pan5 (R = 0.97) are these spikes [V], so the "external poller" explanation is refuted. Reuse is now OFF, and the user reports a 0.1% low of ~160 while panning with it off. They should be gone or rarer [?]. Their trigger is an unknown periodic event inside the game when the view is still [?].
5. **Minor.** GPUBusy alternates ±0.075 ms on every other frame (~3.5%, cause unknown) [V]. Too small to set the lows.

**How to tell them apart (one launch, current defaults, Step-0 build).** Standing 60 s at the heavy spot, then pan, then fight, with GOW3_FRAME_STATS=1, GOW3_SLOW_FRAMES=1, GOW3_SECTIONS=1, and optionally PresentMon. Classify every slow frame:

| Signature | Points to |
|---|---|
| ticks or flip_present ≥1 ms inside tasks | GPU-limited or GPU-wait (3, or 4 if still periodic) |
| draw section ≈ frame time with busy ≥85% | CPU-slow (2) |
| render state ≥0.5 ms | spike class (2) |

Then:
- Run a Rayleigh test on slow-frame timestamps (scratch oscillation\period_scan.py) to confirm or rule out 1 Hz locking without reuse.
- Report the share of 1%-low frames in each class.
- Follow with one sections-off run at the same spot for the true lows.

**Fix path.** Whichever class owns the lows decides the order: CPU-slow → items 1, 2, 4, 3; GPU-heavy → Z1, then items 1 and 7; periodic → instrument the game-side trigger (N-15 follow-up). Item 1 is the only one that shrinks both sides of the lockstep.

---

## 4. Dropped ideas (do not re-propose)

**Submission, pacing and present**
- **Mid-frame split submits (2–4 parts):** the trace replay gives +1.2% mean and leaves the lows unchanged. The effect equals frames_queued=2, which hurt three times.
- **Idle flush, BB style (+SubmitAsync):** the model had an off-by-one. Corrected: ≤+1% free, about 0 at 100 µs per flush, net negative in stage-B-bound stretches. BB's rule wouldn't fire under a backlog.
- **frames_queued=2:** T-W1, T-Q2 and N-15 all show it hurts the lows.
- **Swapchain acquire off the graphics queue:** Reflex at 4,219 µs keeps presents from outrunning the display. Acquire returns in ≤1.9 µs. The narrow-2 10 ms frames are 97% FE-active.
- **Reflex vs Precise test:** stage B waits only ~0.1 ms per frame. N-1 (Precise) still had GPU-bound 10 ms frames.
- **External 1 Hz poller (FanControl / overlay):** those are N-15 reuse spikes. The FanControl phase bin in nsys holds the *best* frames.

**CPU pipeline**
- **Recorder 200 µs spin cut:** gain below noise. Blocked-on-recorder is 0.0–0.1%, and spin 0 likely costs 0.1–0.9 ms. Its metric is dead on Windows.
- **Stage-B drain bubbles (0.45 ms):** moving the work doesn't remove it. Refill latency is µs. Stage A still has 29–42% slack with reuse off.
- **ObtainBuffer memo (S4):** the paths it would skip are already lock-free. No CPU-write generation exists for invalidation. Its slow-frame claim was wrong (+10%, not +23–38%).

**GPU work and copies**
- **Copy prologue/epilogue batching:** ~0.5–1%. The big copy window is not hoistable.
- **Light grid 128 px:** a single partial wave whose time follows light count. 0 or negative.
- **r.SeparateTranslucencyScreenPercentage=50:** colour-ROP pixels are flat. The narrow-2 extra work has no colour output.
- **Ring reuse:** off. Mechanism unknown (N-15). Don't re-enable without a cause.

**Shadows and barriers**
- **r.Shadow.CachePreshadow=1 test:** the cache already works and is full (R182075, 15 reads).
- **r.Shadow.PreshadowsForceLowestDetailLevel:** same draws and passes. ≈0.
- **Shadow-map backing rename (N-6):** the premise is wrong. 26–34 separate maps, each written once.
- **"Drop the global memory barrier" from shadow flushes:** it ORs into the next flush. No-op.
- **Narrower barrier masks as a GPU win (N-4/N-5):** −0.8% active, not 9%.

**Metrics**
- **Ranking by usage %, stage-B busy %, drain count, WFI share or GR-active alone:** all have misled before.

---

## 5. Open questions and the runs and captures needed

### Zero-launch (GUI or offline; do first)

- **Z1, highest value for goal 1.** Open `research-data\ngfx\ngfx-narrow-2\medievil-runtime_2026_10_28_02_12_32.ngfx-gputrace` and compare range timings against `ngfx-fight-1\medievil-runtime_2026_10_27_02_25_26.ngfx-gputrace`. Which passes carry the doubled depth-passed pixels and SM instructions: prepass, masked shadow depths, preshadow receiver-stencil redraws, or effect particles in an opaque path? This decides whether item 1 also attacks the 10 ms GPU frames.
- **Z2.** In the fight-1 and ab-1 (`…02_57_34`) traces, read:
  - (a) the three fog dispatch durations (item 7 gate);
  - (b) idle bubbles inside the 512 shadow and projection ranges (item 9 gate);
  - (c) what runs in the ~0.21 ms low-SM window ~25% into the frame (item 5).
- **Z3.** RenderDoc depth-test overlay on the 19 occlusion boxes in `heavy_frame41595.rdc` (item 12 gate).
- **Z4.** Fold the profile-5 top-30 labels into classes (×2) for a first per-class GPU budget.
- **Z5.** Ledger and doc corrections:
  - N-5: 9% → ~0.8% GPU active.
  - N-6: maps are separate.
  - T-S1c / study-stage-b-native: "other key" comes from `key = 0` clearing.
  - study-heavy-frame-map row 9b: these are preshadows.
  - study-heavy-frame-map phase 10: translucency writes its own RT, not SceneColor.
  - The CURRENT STATE gain line.

### Launches (each needs the user's go; re-read medievil.ini first; one change per launch)

- **L1.** Step-0 counter build, current defaults (reuse off). Light area, then heavy standing 60 s, pan, fight. GOW3_FRAME_STATS, GOW3_SLOW_FRAMES and GOW3_SECTIONS on. This re-baselines everything and classifies the oscillation (section 3).
- **L2.** Same spot, sections off: the true FPS / 1% / 0.1% baseline for the A/B runs.
- **L3.** `GOW3_UE4_CVARS='r.Shadow.Preshadows=0'`: the item 1 upper bound, with screenshots.
- **L4.** Item 1 variant (Fade/MinPreShadow 32/16, or ForceSingleSample).
- **L5.** Item 2 bulk descriptors, as a runtime toggle A/B inside one launch.
- **L6.** LTO exe, A-B-A at a fixed spot.
- **L7.** GOW3_GPU_PROFILE alone, to check the profiler's perturbation (<5%) and get per-class costs on current defaults. Then the fog A/B (item 7) if Z2 justifies it.
- **Later, gated on L1 counters:** items 3, 4, 8 and 10. Items 9, 11 and 12 only after item 1 settles.

### Open questions
- What doubles in narrow-2 (Z1)?
- Do the 1 Hz-locked GPU-completion frames survive with reuse off (L1)? And what triggered the N-15 spikes?
- How many preshadows does today's heavy view have? The t-pan5 pass count suggests fewer than in the captures.
- Does a GridPixelSize set through GOW3_UE4_CVARS take effect against the device profile?
- Is the decile-2 copy window the stencil-plane copy?
- What causes the 0.1–0.2 ms per frame of PCIe-bound low-issue compute?
- What causes the render-state 0.5–2 ms spikes (lock wait, driver allocation, or target creation)?
- Is the play-clock GPU ceiling really ~4.0–4.2 ms in the heavy view? There is no same-spot GPU Trace yet.
# B. Completeness critic

I found 14 gaps. Four of them change the plan's conclusions. I closed several myself by reading data and code, read-only. My scripts are in `C:\Users\Ty\AppData\Local\Temp\claude\c--Users-Ty-MediEvil\78f7734e-4522-4580-95d1-94046d666589\scratchpad\research\critic\` (`clk.py`, `clk2.py`, `clk3.py`, `burst.py`, `ratio.py`, `pc.py`).

## A. Findings that contradict the plan or the docs (closed by me)

**1. The constant ring has a stamp-publication deadlock. N-15's "ring was not full in the spiky runs" is wrong for `t-gpu1-tonight-off`.** [V]
- **Data.** In `t-gpu1-tonight-off.log`, from line 450 to the end (17 windows of 5 s), the ring reports "2,572,537–8,540,629 allocations found it full… held epochs 42, position 202463440, completed 201304704". Those values are identical in every window, so the ring stayed full for the whole run. "Mapped writes" shows constant ring 0.00 MB and stream buffer 5–8.7 MB per frame (lines 459–948).
- **Code.**
  - Stage A sets `packet.ring_end = Position()` (vk_rasterizer.cpp:656).
  - `Stamp()` returns early when `end == pending_end` (vk_constant_ring.h:116-126). It publishes the pending stamp only when a different end arrives.
  - Once the ring is full, `position` stops advancing, so every later packet has the same end. The last pending stamp is never published, and `completed` stays below the held epoch's `until` (:150-155).
  - The result: full forever, plus one `Retire()` (with `semaphore->Refresh()`) for every failed allocation. That is 0.5–1.7 M per second.
- **Scope.** With reuse off there are no held floors, so this can't stick in practice [?]. With reuse on it is a real bug.
- **Another candidate cause of the spikes in the other runs [?].** In `t-pan5` and `t-flip1` the single report shows completed 72.9 MB behind position in the 96 MiB ring. `retire_scans` is printed only when the ring is full, so a retire-scan storm in stage A in the other spiky runs would be invisible.
- **How to close.**
  - Publish the pending stamp when it is the same end on a new tick, or at submit.
  - Print `retire_scans` every 5 s regardless of fullness.
  - Then run one reuse-on A/B.
- **Why it matters.** If fixed, reuse could come back for its BAR-write saving (13.3 MB per frame without reuse, ~4 MB with it, N-7), which helps goal 3. The plan's line "don't re-enable without a cause" should now name this cause.

**2. Missing oscillation class: bursts of 30–80 back-to-back GPU-bound ~10 ms frames in fights.** [V] The plan's section 3 treats slow frames as isolated. The data shows sustained runs:
- **nsys fight** (`nsys-analysis\frames.csv`, `burst.py`): one run of 53 frames at 10.3 ms, inside ~78 elevated frames. Compared with normal frames:

  | Counter | Burst | Normal |
  |---|---|---|
  | GPU busy (ms) | 10.2 | 5.9 |
  | GPU idle (ms) | 0.17 | — |
  | Pixel warps per cycle | 45 | 27 |
  | SM issue (%) | 45 | 31 |
  | Pipeline binds | 2438 | 1957 |
  | Compute warps per cycle | 3.5 | 6.0 |

  That is GPU-bound and not lockstep. The 776/483 lockstep counts should be split into burst and non-burst frames.
- **`t-ab-tonight-off.log`** (reuse off), lines 906 and 939: runs of 34×10.4 ms and 29×10.1 ms. That window has 74 frames over 1.5× the median out of ~860, which by itself pushes the 1% low to ~100 FPS.
- **`ngfx-narrow-2\game.log`** lines 868 and 998: runs of 29×10.1 and 28×8.6 ms, while the windows around them show a median ~5.0 ms.
- **How to close.** In L1, use run length ≥10 as its own class, note the game event each time (hit, spell, kill), and add per-frame class totals from GOW3_GPU_PROFILE for burst frames.

**3. The narrow-2 counters don't support "extra work in depth-only / masked passes".** [V] Comparing `GPUTRACE_FRAME.xls` averages per frame for narrow-2 and fight-1 (`ratio.py`):

| Counter | narrow-2 / fight-1 |
|---|---|
| SM instructions | 2.11× |
| Input pixels | 1.22× |
| Zcull samples | 0.96× |
| Primitives | 0.76× |
| Draws | 0.79× (1723 vs 2189) |
| 3D registers allocated | 1.76× |

- Instructions per input pixel are therefore ~1.7× higher.
- About 2.7e9 extra instructions per frame over ~1.45e8 extra depth-passed pixels is ~600 warp-instructions per extra warp. Depth or stencil passes, which have no or tiny pixel shaders, can't produce that.
- **Effect on item 1.** The "preshadow receiver-stencil" link from item 1 to the 10 ms frames is weak.
- **Effect on Z1.**
  - Only trace 3 (`02_12_32`) was exported; BASE files are timestamped 02:12:32.
  - Traces 1 and 2 (`02_11_57`, `02_12_03`) come from the same session and scene, and their log windows show no burst (lines 1049 and 1071: longest run 1–2 frames).
  - Exporting and comparing trace 3 against traces 1/2 is a much cleaner A/B than comparing it with fight-1, which is a different day and scene.

**4. Heavy passes are partly clock-limited.** This settles the plan's "[? clock scaling]" item. [V] nsys GPU_METRICS (`clk2.py`, `clk3.py`):
- Busy GPC clock: p50 2733, p10 2351, p1 2060 MHz. 100 ms medians always stay ≥2665, so dips are short (streaks 50–300 µs).
- 11.9% of busy samples are below 2.4 GHz. In those samples SM issue is 69% (vs 39%) and DRAM read 48% (vs 5%).
- So the heaviest phases run 15–25% below peak clock, and ngfx traces (clocks "Locked to Base", REPRO_INFO.xls) hide this.
- **How to close (no build).** Log `nvidia-smi --query-gpu=clocks.sm,power.draw,clocks_event_reasons.active --format=csv -lms 100` during L1.
- Cutting heavy-shader work may also lift the clock in those phases [?].

## B. Measurement and yardstick gaps

5. **"stage A CPU" in Slow-frame / Frame-sample lines is quantized to 15.625 ms.** It uses `GetThreadTimes` (videoout/driver.cpp:461). In t-pan5 it reads 0.00 708 times and 15.62 369 times [V]. Add it to the Step-0 "do not use" list, or switch it to `QueryThreadCycleTime`.
6. **Slow-frame durations are bimodal (t-pan5, reuse on, frames with >1000 draws).**

   | Group | Frames | Mean ms | Ticks wait (ms) | Busy |
   |---|---|---|---|---|
   | Tight cluster, 8.5–10 ms | 254 | 9.15 | 3.99 | 94% |
   | 6.5–8.5 ms | 115 | 7.48 | 1.74 | — |

   [V] Use the 9.1 ms cluster as the N-15 signature in the L1 histogram. Its disappearance with reuse off is the decisive check, rather than the Rayleigh test alone.
7. **A reuse-off per-second baseline already exists.** `t-ab-tonight-off.log` (fight, stats on, "0.00 MB more reused", 25 windows, 172–237 FPS) is reuse-off, as is `t-gpu1` (although that run fell back to the stream buffer, see #1). The caveat "almost every heavy log has reuse ON" should name it. No other unused data turned up:
   - `ngfx-narrow-1` (trace + game.log + .times) is unused.
   - `nsight captures\frame 4 analysis.yaml` is empty (`[]`).
   - WPR `cpu-osc-1.etl` is from an older build.
8. **frames_queued=2 "tried three times".** The N-15 trial was in the menu with reuse on, and possibly with the stuck ring. It is not valid evidence. T-W1 and T-Q2 stand.

## C. Plan items with unverified premises or missing parts

9. **Item 7 (GridPixelSize) will likely be a no-op through GOW3_UE4_CVARS.**
   - `ue4_override.py:134-137` writes to `[SystemSettings]`, which is SetBySystemSettingsIni.
   - `PS4DeviceProfiles.ini` sets `+CVars=r.VolumetricFog.GridPixelSize=16`, which is SetByDeviceProfile and outranks it in UE4 4.21 [? engine priority order, not checked locally].
   - Item 1 and item 11 cvars appear only in BaseScalability, which ranks lower, so they should take effect [V for which file sets them].
   - Skip the GOW3_UE4_CVARS fog test. Go straight to the device-profile override, and verify the fog dims as the plan already says.
10. **Items the user ordered in N-5 are neither ranked nor dropped:**
    - depth ping-pong (~60 of ~210 drains, per light);
    - pass breaks that reopen the same targets (~8–11 per frame; the cost is 4K load/store, not drains);
    - change 3 (shared barrier destination per flush);
    - placing barriers from the game's own PM4 sync (ACQUIRE_MEM / EVENT_WRITE).

    Each needs a gate or an explicit drop with a reason.
11. **The port's own GPU passes are not budgeted.** These are RCAS (`rcas=1`), the ImGui overlay (`overlay_fps` / `overlay_frametime` = 1), present / UI composition, and the scene-proxy resolve/fill TransferMarks. Add them as classes in the Step-0 profiler fix. Whether RCAS runs at native 100% is [?] (vk_presenter.cpp:515).
12. **Item 3 (sampler cache) is missing two cache keys.** `BindSamplers` passes `Regs().ta_bc_base` and the global `sampler_lod_bias` into `GetSampler` (vk_rasterizer.cpp:2724-2726) [V]. A cache in the memo entry must key on both.
13. **Line nits.** The srcStage OR is at vk_runtime.cpp:940, not :934. CopyBufferToStencil starts at :764. The other cites I checked hold: liverpool.cpp:1468, vk_rasterizer.cpp:2679/2692, vk_pipeline_common.cpp:86-114, texture_cache.cpp:1055/1065/1511, build.sh:23-24.
14. **The PCIe-RX low-issue compute is still open.** BAR1 *reads* are negligible (4,729 requests in 8.9 s, `clk.py`), so it is not the CPU reading VRAM. The next cheap check is aligning those windows with staging-upload copies in nsys.