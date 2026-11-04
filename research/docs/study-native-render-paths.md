# Study: native render paths, or "cutting out the middle man" (2026-11-04)

Status: read-only research (docs, code on `deck-lab`, Bloodborne `origin/master`). Nothing built, launched or
committed; no SSH. Parent: [plan-linux-deck.md](plan-linux-deck.md) (stage B saturated on the Deck);
companions: [study-stage-b-per-draw.md](study-stage-b-per-draw.md) (C1-C11),
[study-stage-b-native.md](study-stage-b-native.md) (S1-S4, H1-H5),
[study-heavy-frame-map.md](study-heavy-frame-map.md) (passes and draw counts),
[study-deck-rescore-rejected.md](study-deck-rescore-rejected.md), [fix-inventory.md](fix-inventory.md).
Question (the user): which of the game's GPU work could skip PM4 → stage B → Vulkan translation and be done natively
(own pipelines, cached descriptors, replay), at what effort and risk.

Marks: **[V]** seen in a doc, log, the code or Bloodborne's history (where given). **[I]** inferred (method given).
Nothing here is "verified" in the project's sense.

## Answer

1. **A separate native renderer does not pay.** Stage B's cost is resource resolution and validation:
   textures, `ObtainBuffer`, vertex/index resolve, descriptors, render state. A native path would need the same
   resolution and the same invalidation for the same guest resources, because UE4 rewrites constants and dynamic
   buffers every frame and can move pool targets. So it would cost about what a fast path costs, and it would add
   visual divergence and a second renderer to maintain. This agrees with study-stage-b-native H1/H5 and with
   Bloodborne's own path. Its "own renderer" (0.6-pre1) **rewrote shadPS4's modules on the same PM4 → stage A/B →
   Vulkan design, with byte-identical SPIR-V**. It did not bypass translation (section 3).
2. **The useful middle ground is a "run replay" fast path inside stage B.** Stage B already pays for a run's state
   on its first draw. When the next draws keep the same pipeline, texture set and targets, they can reuse that state
   and redo only what changed: constant/vertex buffer offsets, push constants and the draw itself. It keeps the
   game's translated shaders, so the image is unchanged when the invalidation is right. One class fits it unusually
   well:
   - **GPU-particle simulation: 194 draws a run, one or two runs a frame (14-27% of the Deck's ~1,414 draws [I]).**
     One pipeline, the same 1024² targets, the same index buffer and the same textures for the whole run; only the
     per-tile vertex buffer and the ring constants change [V, heavy-frame-map §2, stage-b-native §2].
3. **The only "native" item with a large payoff removes draws.** That is the HZB build at twice the resolution
   (or the size patch that gives the same thing). The Deck's 67% render scale halves the HZB per axis and costs
   about 145 culled objects a frame [V, plan-linux-deck "HZB resolution and culling"]. Fullscreen post, TAA,
   tonemap, UI, the HZB build itself and clears together are under 100 draws: under ~0.5 ms of Deck stage B even if
   made free [I].
4. **Ranked plan:** section 4. First a class counter (one build, stats only). Then the particle-sim run replay as
   the pilot. Then the cheap no-op draw skip and the HZB size test. Widen the run replay to every class only if the
   counter shows long runs elsewhere. Park post/UI replay. Don't build a native RHI or native shaders.

## 1. The heavy frame by class (question 1)

**Counts.** The heavy-frame map ([V], PC 4K, captures A / B) predates three changes:
- the full prepass is gone (`r.DBuffer=0` + `r.EarlyZPass=1`, ~600 fewer draws, T-Z2);
- preshadows are off (N-25, −260 to −445 draws);
- HZB culling works (N-22 and the remap).

The Deck column is an estimate [I]: the map's counts minus those changes, scaled to the Deck's measured ~1,414 draws
(run 3) and the PC's ~1,466-1,480 (`t-sampler-1`). **No current per-class count exists**: that is the first step.

**Cost.** The Deck's DrawRecord is ~7-8 us a draw (run 3: 7.96 us with sections on, which add ~0.5-0.8 us), and the
ordered tasks come on top. Per-class us a draw [I]: depth-only draws skip the texture work (textures are 2.0 of the
7.96 us), dynamic-vertex and multi-texture classes cost more. 1 us a draw = ~1.4 ms a frame.

| Class | PC A / B draws [V] | Deck draws [I] | Deck stage B now [I] | Repetitive / recognisable? | Native or fast path would need | Divergence risk |
|---|---|---|---|---|---|---|
| **GPU-particle sim** (1024² RGBA32F + RGBA16F, P146812, 48 idx, + 1 inject) | 194 + 1 + 194 | 195-389 (run 2 fires in some frames only) | ~6 us → **1.2-2.3 ms** | **yes**: one pipeline, the same targets, index buffer, textures and tile lists for 194 draws; per-emitter constants and the tile-offset vertex buffer change [V] | run replay: capture state at the run's first draw, then per draw only buffer offsets + push + draw | low with replay (same shaders); high with own shaders (UE4's sim maths, curl noise, depth collision) |
| GBuffer (6 RT + D32S8) | 630 / 742 | ~450-600 | ~8 us → 3.6-4.8 ms | runs of the same material (UE4 drawing policies: 744x5, 8232x3 index-count runs [V, stage-b-native §3 S1]); many materials and textures | generic run replay (same predicate) | low (replay), not a native candidate |
| Depth prepass (partial now) | 628 / 739 full | ~50-150 [I: EarlyZPass=1 keeps large occluders] | ~5 us → 0.25-0.75 ms | VS-only (masked materials have a PS); same meshes as the GBuffer | already skips textures by nature; the rest is buffer resolve = generic replay | low |
| Per-object shadow depth (512² D16) | 182 / 301 (with preshadows) | ~80-180 | ~5 us → 0.4-0.9 ms | one pass per map; casters repeat GBuffer meshes | as the prepass | low |
| Attachment-less 3-vertex draws before shadow maps | 26 / 34 | ~10-34 | ~4 us → 0.05-0.15 ms (+ one pass each) | **yes**: no colour, no depth, binding 0 null [V] | **skip** in `FilterDraw` (no attachment, no writable binding) | low-medium: confirm it is the shadow clear the next pass's load op already does [V, map §4 item 3] |
| Shadow projection + stencil | 56+18+66 / 74+14+99 (preshadows on) | ~40-100 | ~6-7 us, a pass each → 0.3-0.7 ms | boxes, few pipelines | generic replay; the passes are already merged where safe ("fewer render pass breaks") | low |
| Velocity (R16G16) | 59 / 85 | ~60-85 | ~6 us → 0.4-0.5 ms | redraws movables; needed for DLSS/FSR | generic replay | low |
| Lighting (tiled compute, volumes) | 15 + 7 disp | ~22 | ~0.15 ms | fixed per frame | not worth it | - |
| Translucency / particle sprites | 83 / 118 | ~80-120 | ~8-9 us (CPU-written sprite vertices, own-depth reads, slow texture binds) → 0.7-1.1 ms | partly (Cascade GPU sprites one pipeline per emitter); CPU sprites are dynamic | the dynamic uploads stay; the depth-read sets are C3's "not memoizable" class | medium (blend, soft-particle depth) |
| Post: fog/SSR, bloom chain, eye adaptation, LUT, **TAA (2)**, **tonemap** | 27 | ~27 | ~8 us, a pass each → ~0.2-0.25 ms | **yes**: fixed chain, fixed shaders, fixed targets every frame [V, mgs2 study §3.2] | fixed-layout replay (descriptors cached per pass; images per frame from the pool) | low (replay); medium-high for own shaders (bit-exact lost); TAA goes anyway when DLSS/FSR replace it |
| UI (UMG off-screen + 28 into the display buffer) | 34 (+5 port present) | ~34 | ~9 us (CPU-written Slate vertices) → ~0.3 ms | yes by target (display buffer after the tonemap) | the CPU vertex uploads stay | low for replay; the value is HUD-less capture for FG, not speed |
| HZB build + occlusion boxes | 12 + 11-18 | ~25 | ~0.2 ms | yes (fixed shaders, 2048² → 2² mips at 4K) | own single-pass downsample (SPD) | medium: the HZB test and the remap must read the same pyramid |
| Clears, resolves, depth copies, compute copies/clears | - | few | small | already handled natively: `FilterDraw` (fast-clear elimination, FMask skip, resolve, depth/stencil copy) `vk_rasterizer.cpp:152-197`; compute image copy / meta clear / image clear by resource shape `:1752`, `:2153-2300` [V] | done | - |

Sum of the Deck column: ~8-12 ms. Stage B busy (91% of 15.45 ms = ~14 ms) minus the tasks and flip (~3.3 ms) leaves
~10.7 ms. The estimates are consistent [I].

**What a native path needs for any class:**
- **State capture:** pipeline key, targets and their generation (`BeginSignature`), blend/depth/viewport dynamic
  state, vertex input layout.
- **Resource mapping per frame:** UE4's render-target pool hands out the same targets in a steady view, but
  constants are rewritten every frame (ring) and CPU-written vertex data moves. So even the replay must resolve
  buffers each draw, and images whenever the texture registry generation changes.
- **Invalidation:** texture registry generation, image `needs_rebind` / `is_target`, render-scale and resolution
  changes (live switches), level loads, a command-buffer cut (invariant 3: no state across a cut), host passes
  recorded in between (FSR, presenter, camera motion, fault manager: the ~12 sites the per-draw study lists for C7),
  pending barriers, frame capture.

**Corrections to the brief's precedents.**
- The copy-shader HLE keys on `0xfefebf9f` (`vk_shader_hle.cpp:19, 237-245`). That is Bloodborne's shader; no doc
  shows it firing in MediEvil [I: probably never].
- `UiComposition`'s hashes are Bloodborne's Scaleform shaders.
- The HZB remap is a CPU hook in the game's `MapResults` (`gpu/shim/medievil_hzb.cpp`), not a GPU replacement.
- The decal-prepass "skip" is a cvar: the game stops issuing the draws (fix-inventory "No forced depth prepass").
- So the GPU-side recognitions that fire in MediEvil today are the generic ones, by resource shape: FilterDraw and
  the compute copy/clear detectors.
- The port's large draw-count wins (prepass −600, preshadows −260-445, HZB culling) all came from making **the game
  issue less work**, not from replaying issued work more cheaply.

## 2. Fast paths inside stage B vs a separate native renderer (question 2)

| | Run replay inside stage B | Separate native renderer (own pipelines / shaders per recognised pass) |
|---|---|---|
| Recognition | the previous draw's pipeline, prepared image hashes, `BeginSignature`, non-user-data register blocks; optionally a class key (VS/PS hash + target shape) | shader hash + pass structure, a profile per game version (01.00 / 01.02) |
| What it skips | texture-set emit (memo hit 0.85 us/packet on the Deck), render state + begin render (0.62), dynamic state (0.34), vertex input emit, index resolve when the index buffer is the same, most descriptor writes (images and samplers unchanged), ~half the `Record` calls | the same, plus the translated shader if replaced |
| What it cannot skip | ring constant offsets, per-draw vertex buffers (`ObtainBuffer`, or an address → arena memo), push descriptors for buffers, the draw | the same: guest constants and buffers must still be mapped each frame |
| Visual risk | none when invalidation is right (same shaders, same resources); failure = stale binding, visible at once | bit-exactness lost with own shaders; any engine-state change (settings, cutscenes, menus) needs mapping |
| Effort | medium (one class) to medium-high (all classes) | high per pass, plus maintenance per game version |
| Recorder | fewer `vkCmd*` too (skips the pipeline bind, vertex input and index rebinds that equal the last ones) | similar |

The existing proposals are pieces of this: S1 (delta-draw fast path), and the per-draw study's C4 (one recorded
command per draw), C7 (vertex input/buffer/index rebind elision) and C8 (descriptor write elision). **The run replay
is S1 with an explicit "run" state object**. A class key lets the pilot start narrow (the particle sim) and widen to
the generic predicate later. Per-class "fast paths" that skip a step outright are safe only where the step is
provably empty:
- **Vertex-only depth draws** already bind no textures (an empty image list). They gain only from the generic replay.
- **Fullscreen passes** are ~30 a frame: a fixed layout saves ~0.15 ms on the Deck. Too small to justify their
  own path.
- **No-op draws** (attachment-less, no writable binding) can be skipped outright, like `FilterDraw`'s existing cases.

Where a native path changes the **shape** of the work, it can beat replay: fewer draws or a merged GPU pass.
- Collapsing the 194 sim draws into one instanced draw needs the recompiler to fetch per-instance constants. High
  effort and risk; the GPU side of those draws is tiny. Not now.
- A finer HZB pyramid: more culling, fewer draws everywhere (item N3).
- Merged post passes: GPU only, and the Deck's GPU is ~90% busy at auto clocks [V, plan-linux-deck wait sites]. Worth
  revisiting with the upscaler work, not for stage B.

## 3. What Bloodborne's "own renderer" replaced (question 3)

- **What:** `72e5c8c` (0.6-pre1) message: "own command processor, GCN definitions, shader translator, renderer and
  caches instead of shadPS4 (byte-identical SPIR-V on the game's shaders)" [V].
  - `docs/CHANGES_0.6-pre1.md:66-68`: "The port's own code instead of shadPS4 (except the online module) … The
    translator gives byte-identical SPIR-V on the game's shader corpus" [V].
  - `gpu/VENDOR.txt`: `gpu/` is "the port's own code … started from shadPS4 … rewritten part by part; gpu/shadps4
    was removed on 2026-10-13", checked by `tests/test_shader_corpus.cpp` [V].
- **How:** module by module, each behind an interface with the old code behind it first
  (`docs/SHADPS4_EXIT_PLAN.ru.md` §7, header "Итог (2026-10-13)") [V]. The frame path is unchanged:
  - game PM4;
  - `gpu/cp` command processor;
  - stage A (`raster_pipe.cpp`: pipeline, ring constants, packet);
  - stage B (`raster_bind.cpp`, `raster_textures.cpp`, `raster_targets.cpp`, `raster_state.cpp`);
  - `VkRecorder`.

  (`docs/ARCHITECTURE.ru.md`, "Путь кадра на GPU") [V].
- **Why, by its own measurements** (SHADPS4_EXIT_PLAN §2, RX 7800 XT + 5700X3D) [V]:
  - Bloodborne is GPU-bound there, and stage B ran at 2.54 us a packet, 72% busy.
  - "the CPU part of shadPS4 is enough with margin (stage B ceiling ~240 FPS at this draw count)".
  - The goals were ownership, generality for other games, and the GPU timeline.
  - §7 step 6: the recompiler "costs nothing per draw, and replacing it is the biggest risk for the image", so it
    was kept bit-exact.
- **What it gained:** 0.6-pre1 credits no FPS to the rewrite itself [V, changelog]. Measured gains came afterwards
  from specific items:
  - 0.7 Deck "draws prepared on the threads that use them …; register state reset only where touched; fetch-shader
    decodes cached; texture sets reused without draw preparation": 68.6 → 72 FPS (`docs/CHANGES_0.7.md:64-66`).
  - One syscall per draw removed: +3% (`:69`).
  - Ring dedup + "only the copies that change something run": +1-4% (`:72-75`).
  - PGO retrained: +15-20% (`:62-63`) [V].
- **Its planned per-draw cut is not done:** §6/§7 step 2 ("stage B only records": bindings decided on stage A
  workers, `VK_EXT_descriptor_buffer`, an identity image registry; target ~1 us a packet instead of 2.5, rated
  "high risk (image overlaps)") [V]. `git grep` on `origin/master` finds no `VK_EXT_descriptor_buffer` use in `gpu/`
  [V].
- **Hash or shape recognition in Bloodborne, and its fate** [V]:
  - The copy-shader HLE (`gpu/render/shader_hle.cpp:273-298`) is now **off by default**: the game's own translated
    shader runs once the memory model allows it (`BB_COPY_SHADER_NATIVE=0` keeps the old copies).
  - Image copy / metadata clear through memory are recognised **by resource shape**, not hash
    (`raster_compute_hle.cpp`). The 960x540 copy-through-memory case took the frame from 12.5 to 10.4 ms, GPU side
    (`docs/CHANGES_after_0.6-pre1.md:84-92`).
  - Scaleform shader hashes (`upscale/ui_composition.h`) compose the UI at output resolution: quality, not speed.

  The lesson: replacements of the game's work stayed where they removed GPU or bus work; per-draw CPU gains came
  from caching inside the generic path.

## 4. Ranked plan (question 4)

Deck yardstick: graveyard heavy view, ~65 FPS (15.45 ms), stage B 91% busy, ~1,414 draws; 1 us a draw = ~1.4 ms.
Stage B savings become FPS only while stage B stays the limit. The GPU is ~90% busy at auto clocks, so expect part of
any gain as better lows or lower power rather than average FPS [I].

| # | Item | Deck stage B saved / frame [I] | Effort | Risk | First step |
|---|---|---|---|---|---|
| N0 | **Class counter** (stats only): draws and DrawRecord rdtsc per class (VS/PS hash + target shape: depth-only, MRT count, size); run lengths under the S1 predicate (same pipeline, image hashes, `BeginSignature`); sim draws per frame and how often run 2 fires; attachment-less draws | 0 (enables N1-N5) | small | none | one build, one Deck stats run in the graveyard; same spot on the PC |
| N1 | **Particle-sim run replay** (pilot): key = sim VS/PS hash + 1024² RGBA32F target pair + 48 indices; the first draw takes the full path and saves its state; the next draws reuse image/sampler writes, render state, dynamic state, vertex input and index binding, and rebuild only the ring offsets, the tile vertex buffer and the push; the run ends at any mismatch, generation change, cut, barrier or host pass | **0.6-1.5 ms** (~3-4 us × 195-389 draws) | medium | low-medium: wrong parity or stale binding shows at once as frozen or flickering particles | N0's sim count; then behind a toggle (default off), A/B live; look at lone torches, embers and Dan's crypt (A6's failure spots) |
| N2 | **Skip no-op draws**: attachment-less, no writable binding (the 3-vertex draws before shadow maps) in `FilterDraw` | 0.05-0.3 ms (+ a pass each) | small | low-medium | N0's count; one RenderDoc look at what the PS4 bound (HTILE clear?) |
| N3 | **HZB at 2x resolution** (the game's HZB size is `2^(ceil(log2 size) − 1)` per axis in UE 4.21 [I, from the engine source, unchecked in the eboot]: 512x256 at 67% vs 1024x512 at 100%); a size patch next to the existing HZB hooks, or a native build into a larger pyramid | **~1 ms** from ~145 fewer draws, if 67% culls like 100% (runs 4 / 5: ~63 vs ~122 hidden) | medium (Ghidra on 01.02) | medium: the test pass, the remap and the pyramid must agree, otherwise over-culling (missing geometry) | find the size computation next to `FHZBOcclusionTester`; compare "HZB hidden / frame" at 67% + patch vs 100% |
| N4 | **Generic run replay** (N1 widened by predicate: GBuffer, shadows, prepass, velocity) | 0.5-1.5 ms on top of N1 (S1's PC 0.25-0.3 us/packet × 4-5; overlaps C4/C7/C8) | medium-high | medium (every class; invalidation sites) | only if N0 shows mean runs ≥ 3 outside the sim; share code with C4 |
| N5 | Fixed-layout replay of post / TAA / tonemap / UI | 0.1-0.3 ms | medium | low (replay) | park; revisit with DLSS / FSR (TAA replaced) and FG (HUD-less capture) |
| N6 | Native RHI, own shaders per pass, instanced sim | ~0 more for stage B than N1/N4 | very high | high | don't |

Dependencies:
- N1 overlaps the identity-sim skip (frame-cost item 8, rescore C6): measure N1 first. It keeps the semantics; the
  skip does not.
- N4 subsumes C7 and part of C8 in the per-draw study. Build N4 and C4 together or not at all.
- N3 is the plan-linux-deck "full-resolution HZB" item, in the user's order after the drains and the render-scale
  re-test.

## How this was checked

- Docs: START-HERE, gpu-threading-reference, study-heavy-frame-map (§1-5), study-stage-b-per-draw (§1-4),
  study-stage-b-native (§1-4), plan-linux-deck (answer, run 3, U5-2, wait sites, decode pacing, HZB resolution, E1),
  study-mgs2-temporal-for-ue4 §3, fix-inventory, study-frame-cost-2026-10-29 (items 1, 8), study-deck-rescore-rejected,
  study-deck-platform-optimization.
- Code (`deck-lab`): `vk_rasterizer.cpp` `FilterDraw` :152-197, `DrawRecord` :1073-1309, `BindResources` :1750-1755;
  `vk_shader_hle.cpp`; `vk_draw_prep.h` `PreparedStage` / `PreparedDraw`.
- Bloodborne (read-only, `git show origin/master:`):
  - `72e5c8c` message;
  - `docs/CHANGES_0.6-pre1.md`, `docs/CHANGES_0.7.md`, `docs/CHANGES_after_0.6-pre1.md`;
  - `docs/SHADPS4_EXIT_PLAN.ru.md`, `docs/ARCHITECTURE.ru.md`;
  - `gpu/VENDOR.txt`, `gpu/render/shader_hle.cpp`, `gpu/render/raster_compute_hle.cpp`, `gpu/upscale/ui_composition.h`;
  - `git grep` for descriptor-buffer use.
- Open:
  - every Deck per-class number above (N0);
  - whether the Deck preset's effects quality changes the sim's 194 draws;
  - how often run 2 fires at 65 FPS;
  - the HZB size code in 01.02.
