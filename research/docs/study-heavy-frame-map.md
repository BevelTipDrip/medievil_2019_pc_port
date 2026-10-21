# Study: heavy-view frame map (2026-10-21)

Status: offline RenderDoc analysis only. Nothing built, launched or committed. Parent: [START-HERE.md](START-HERE.md);
follows [study-stage-b-native.md](study-stage-b-native.md) §2 and H3.
Captures: `research-data\renderdoc\heavy_frame41595.rdc` (A) and `heavy_frame42818.rdc` (B). Heavy view, camera still,
4K, HZB occlusion through the remap, S2 on, `r.EarlyZPass=0` (default since `fca0d85`).
Scripts and raw outputs: `research-data\renderdoc\heavy-analysis\` (`rd_dump.py` -> `dump-*.json`; `analyze_phases.py`
-> `phases-*.txt`; `analyze_passes.py` -> `passes-*.txt`; `analyze_redundancy.py`; `analyze_extra.py`;
`analyze_passbreaks.py`; `rd_bufdump.py` + `analyze_h3_tiles.py` -> `h3-*.txt`; `rd_texdump.py`;
`analyze_shadow_views.py`; `disasm_gpusim.py` -> `gpusim-disasm.txt`). Event ids below are A / B.

## 1. Frame map

Totals: **A 2,256 draws + 42 dispatches, 370 render passes. B 2,709 draws + 44 dispatches, 459 passes.**

| # | Phase (targets) | A draws (passes) | B draws (passes) | eids A / B |
|---|---|---|---|---|
| 0 | pre-frame quads, 1920x1080 RGBA8_SRGB (B also 1024² R16F/RGBA16F) | 2 | 6 | 220-233 / 253-312 |
| 1 | **GPU particle sim**: 1024² RGBA32F + RGBA16F, P146812, 48 idx; inject 1024² x4 | 194+1+194 (3) | 194+1+194 (3) | 246-2971 / 325-3050 |
| 2 | **depth prepass**, no colour, DS 3840x2160 D32S8, 30 pipelines | **628** (33) | **739** (36) | 2994-7515 / 3073-8348 |
| 3 | occlusion boxes (36 idx, P144886) | 18 | 11 | 7522-7641 / 8394-8464 |
| 4 | HZB: 2048² R16F (5 draws) + 64²..2² R16F images, 3 readback copies; HZB test 256² RGBA8 + 32x32 dispatch | 12 | 12 | 7656-7918 / 8479-8751 |
| 5 | **per-object shadow depths**, 512² D16, one image and pass per shadow (A also 1 point light 256²[6], B one 1024² D16) | 182 into 26 maps | 301 into 34 maps | 7934-9769 / 8766-11642 |
| 5a | attachment-less 3-vertex draw before each shadow map (P148151, no RT, no DS, renderArea 32768², binding 0 null) | 26 (26) | 34 (34) | 7479-9754 / 8358-11587 |
| 6 | DBuffer decals: 3x 3840x2160 RGBA8 + DS | 4 (+2 depth) | 2 (+2) | 9800-9868 / 11673-11721 |
| 7 | GBuffer: 6 RT (RGBA16F, RGB10A2, RGBA8, RGBA8_SRGB, RGBA8, RGBA8) + D32S8 | 630 (2) | 742 (2) | 9892-14335 / 11745-16978 |
| 8 | velocity: R16G16_UNORM + DS | 59 | 85 | 14348-14771 / 16991-17602 |
| 9 | lighting: tiled compute (240x135 dispatches), 64³ RGBA16F x2 translucency volume (7/6) | 15 + 7 disp | 11 + 7 disp | 14788-16776 / 17619-20128 |
| 9a | shadow projection: boxes into 4K RGBA8 light attenuation (P145294) | 56 (56) | 74 (74) | 14911-16646 / 17742-20056 |
| 9b | projection stencil: frustum boxes (P145289) + **subject meshes redrawn into scene stencil** | 18 + 66 (64) | 14 + 99 (84) | 14803-16632 / 17634-20042 |
| 10 | translucency/particles into SceneColor RGBA16F + DS; B also distortion (4) | 83 (65) | 118+4 (86) | 16837-17641 / 20189-21336 |
| 11 | post: fog/SSR/etc (5), bloom 1920..30x34 R11G11B10F, eye adaptation 1x1, LUT 32³, tonemap 4K SRGB | 27 | 27 | 16789-17901 / 20141-21596 |
| 12 | UI (894x52, 1370x70, 1340x35, 28 into 4K SRGB) + present B8G8R8A8 (5) | 39 | 39 | 17917-18403 / 21612-22150 |

Other dispatches: P1437 (32640,1,1) after the particle sim, then a CopyBufferToImage of D32S8's stencil (eid 2976-2978);
22-24 (1,1,1) dispatches P1592 after the HZB (7794-7899 / 8617-8732). These look like the port's own work (stencil
copy, buffered queries).

**The depth prepass is a full prepass although `r.EarlyZPass=0`.** Its draws repeat the GBuffer draws:
625 of 630 (A) / 737 of 742 (B) GBuffer draws have a prepass draw with the same index count, first index, base
vertex and instance count; 14.55M vs 14.55M indices (A), 16.76M vs 16.76M (B). That is UE4's `DDM_AllOpaque`. In UE4
4.21 `FScene::UpdateEarlyZPassMode`, `ShouldForceFullDepthPass()` overrides `r.EarlyZPass`: DBuffer decals,
`r.StencilForLODDither`, early-pass material masking, or forward shading force it. DBuffer is on in these frames (phase 6;
UE4 default `r.DBuffer=1`, and the game config does not set it, plan-stage-b-split.md T-Z1). So **`r.EarlyZPass=0` has
no effect here**, and T-Z1's "~120 fewer draws" is not visible in these frames (route variance, or a different view).

## 2. H3: GPU-particle double run

Present in both frames: **two runs of 194 draws**, same pipeline P146812, 48 indices each, same 553 tiles (4x4 texels),
same index buffer (arena R1479), same tile lists (binding 19), per-emitter params (binding 0) and binding 2.
Ping-pong: A run 1 reads 181710/181718 and writes 181712/181716; run 2 reverses it. B starts on the other parity.

Between the runs there is no scene capture, reflection capture, second view or HZB. There are only two calls:
CopyBuffer of 1,044,480 bytes into the arena (1599 / 1678) and **one inject draw** (1609 / 1688: P146800, 6 idx x7
instances, writes run 1's targets plus render/sim attribute textures 181476/181478).

Readers: run 1's output is read only by run 2 (usage: 181712 is ColorTarget at 246-1609 and PS_Resource at 1620-2971,
nothing else). Run 2's output is read by particle rendering in translucency (VS_Resource 16847-17604 / 20199-21275) and by
next frame's run 1. Neither run's output is unread.

**Runs are not identical.** Binding 1 (per emitter) float [8] is the step dt: run 1 uses the frame's dt (A 0.0615 s,
B 0.1169 s; these are capture-stall frames) and run 2 uses exactly 1/30, on the same 38 / 66 draws (83 / 144 tiles).
The other 155 / 127 draws have dt=0 in both runs. (The earlier study compared only dt=0 draws, so it saw them as equal.)

**Run 2 is a bit-exact copy.** Texture dumps after each step (`h3-tiles-*.txt`): run 2's output equals its input on
all 553 tiles (position and velocity), including the dt=1/30 tiles. Run 1 changes 72 / 136 position tiles. The dt=0
tiles are identity in both runs.

Why (eboot, `gpusim-disasm.txt`, emitter tick guest+0x2382e50): with `FixDeltaSeconds` > 0 (default 1/30, 0x2383b42),
emitters without flag 8 swap the two fields. They get one "fixed" iteration with the real dt ([+0x108]=dt,
[+0x10c]=1) and the variable step disabled ([+0x110]=1/30, [+0x114]=0). Run 1 is that fixed iteration. Run 2 is the
variable phase: every tile is redrawn with dt 1/30 and does nothing, and only flips the buffers back. With
`FixDeltaSeconds`=0 the branch is skipped: 0 iterations, variable step on, real dt, so one run. A and B start on
opposite parities, so the run count is not always even. Not every frame has two runs. How often the second run fires
during normal play is not measured.

## 3. Redundancy (same measure as study-stage-b-native §2; `redundancy-*.txt`)

Mode (a) counts only the calls RenderDoc attaches to the draw (the earlier study's method). Mode (b) counts every call
since the previous draw (state recorded before BeginRendering included).

| call | repeats previous, A / B (a) | (b) | light captures |
|---|---|---|---|
| vkCmdSetVertexInputEXT | 93.8 / 92.4% | 86.5 / 83.8% | 96% |
| vkCmdPushConstants | 84.5 / 80.3% | 74.3 / 70.5% | 86.5% |
| vkCmdBindPipeline | 74.3 / 70.5% | 74.2 / 70.5% | 73-74% |
| vkCmdPushDescriptorSet exact / same except buffer offsets | 0% / 63.6, 59.6% | 0% / 55.0, 51.7% | 0% / 79-83% |
| vkCmdBindVertexBuffers | 38.7 / 41.2% | 34.7 / 36.8% | 10-14% |
| vkCmdBindIndexBuffer | 45.1 / 38.4% | 40.7 / 34.9% | 1-5% |

S2 is visible: 55-58% of vertex binds and 64-58% of index binds now point into the arena R1479 (stream R241 the rest),
so binds repeat more. 147 / 183 draws use cached sets (vkCmdBindDescriptorSets, never a consecutive repeat). Draws whose
state is all repeats except the push-descriptor offsets: 347 / 353 (b). **No two consecutive draws are identical**
(pipeline, descriptors incl. ring offsets, push constants, vertex/index binds, args, targets): 0 in both.

## 4. Other large or possibly redundant work

- **Per-object shadows**, about 15% / 19% of draws. 182 / 301 depth draws (26 / 34 maps, 45 / 64 passes),
  26 / 34 attachment-less draws, 66 / 99 subject meshes redrawn into stencil (same index counts as the casters, e.g.
  4902/17070, 10767/5082), and 74 / 88 projection draws. Projection alternates depth-only and RGBA8+DS passes: about one
  pass per draw (120 / 158 passes).
- **Duplicated shadow map**, one per frame. A: R181998 (8056-8065) and R182002 (8092-8101) have the same
  casters, the same primitive constants and the same 96-byte shadow-pass constants (only a padding float differs), and
  both are projected (15051, 15023). B: R159822 / R181998 (8864 / 8900). Cost about 2 draws + 1 clear draw + 2 passes.
  Probably two light interactions with the same setup.
- **Attachment-less 3-vertex draws** (5a): a BeginRendering with no attachments, binding 0 null. Nothing visible comes
  out of them. The following shadow pass clears depth with its load op (e.g. 7967 `D=Clear`).
- **Translucency pass restarts**: 63 / 81 passes end and restart on the same attachments with only a barrier on
  the scene depth (D32S8 R6411, layout READ_ONLY -> READ_ONLY, tests -> shader read and back; 122 such barriers in A)
  between them (e.g. 16852, 16862, 16872). Translucency: 83 draws in 65 passes (A), 118 in 82 (B).
- **Unread writes** in the frame: HZB 64², 32², 16² R16F (7722-7746 / 8545-8569; their 8², 4², 2² siblings are
  copied out, these 3 are not), the 64³ volume pair at 16768 (A), pre-frame 1920x1080 SRGB quads (2 / 3) and B's 1024²
  R16F/RGBA16F quads (3). About 3-8 draws. Some may be read next frame.
- Velocity pass (59 / 85) redraws movable meshes. The upscaler plan needs it, so keep it.

## 5. Ranked: what could go, draws per frame (A / B)

| # | Item | Draws saved | How | Confidence |
|---|---|---|---|---|
| 1 | Full depth prepass | **628 / 739 (28 / 27%)** (+33 / 36 passes) | game cvar: find what forces it (`r.DBuffer` first, also `r.StencilForLODDither`, `r.EarlyZPassOnlyMaterialMasking`). Then `r.EarlyZPass=0` applies | high that it duplicates the base pass; medium that DBuffer forces it. Cost: DBuffer decals off (2-4 in view), base pass loses depth-equal (more 4K GPU overdraw), occlusion moves after the base pass |
| 2 | GPU-particle run 2 | **194 (8.6 / 7.2%)** in frames with two runs | `r.GPUParticle.FixDeltaSeconds=0` (one variable step with the real dt, the same maths run 1 does now) | high that run 2 is a no-op copy; medium-high on the cvar's effect (disassembly, not tested). Emitters with flag 8 would lose fixed stepping. How often it fires during normal play is unknown |
| 3 | Attachment-less draws before shadow maps | 26 / 34 (+26 / 34 passes) | port: skip draws with no colour/depth attachment and no writable binding (keep any HTILE/clear bookkeeping) | medium (confirm the draw is the shadow clear the next pass already does) |
| 4 | Translucency pass restarts | 0 draws, 63 / 81 passes, ~120 barriers | port: do not end the pass for a READ_ONLY -> READ_ONLY depth barrier when the draw does not write depth | medium-high |
| 5 | Per-object shadows | up to ~350 / 520 (fewer with cvars) | game cvars that cull or limit per-object shadows (e.g. `r.Shadow.RadiusThreshold`) | low-medium, quality trade |
| 6 | Projection pass ping-pong | 0 draws, ~100-150 passes | port: depth-only draws inside the RGBA8+DS pass with colour write mask 0 | low-medium |
| 7 | Duplicate per-object shadow map | ~3-5 | game side (duplicate light/interaction) | medium that it is a duplicate, low value |
| 8 | Unread HZB mips / volume / pre-frame quads | ~3-8 | port HZB remap (64²..16² mips) | low value |
