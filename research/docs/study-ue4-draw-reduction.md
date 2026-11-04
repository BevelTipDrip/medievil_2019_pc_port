# Study: UE 4.21 ways to cut draws (and GPU work) on the Steam Deck

Status: study, 2026-11-04, branch `deck-lab`. Read-only: docs, the RenderDoc heavy-frame dumps, a static scan of
`out\eboot.elf` (01.02, ELF sha256 `31074869…`) and of the 01.00 copy, the game's configs and level packages
(`E:\MediEvil\CUSA11227-0102`, read through `ue4_override.read_file`). Nothing built, launched or committed. Parent:
[START-HERE.md](START-HERE.md); background [plan-linux-deck.md](plan-linux-deck.md) ("HZB resolution and culling"),
[study-heavy-frame-map.md](study-heavy-frame-map.md), [study-frame-cost-2026-10-29.md](study-frame-cost-2026-10-29.md),
[study-live-switches.md](study-live-switches.md), [study-deck-platform-optimization.md](study-deck-platform-optimization.md)
(R3, VRS). Scripts (session scratchpad `drawred\`): `cvars.py` (UTF-16 cvar names), `help16.py` (the help text beside
each name), `leafind.py` / `str16.py` (strings and the code that loads them), `hzbsite.py` (HZB patch sites in both
builds), `cfg2.py` (configs), `umapscan.py` (level name tables); `dis_at.py` one level up.
**[V]** verified in the binary, the configs, the captures or a log; **[I]** inferred (UE 4.21 source knowledge or
reasoning); **[?]** estimate or guess. Addresses are eboot image offsets (`patches/MediEvil.xml` adds 0x400000).

Target: the Deck, heavy graveyard view (Level_01_GY1), Low preset as the user runs it (effects 50, preshadows off,
shadows 256 / short, view 0.7, LOD low, no fog / motion blur / DoF / lens flares), 1280x720 output at 67% (858x482
scene), ~1,414-1,460 draws a frame, stage B 91-94% busy. Stage B on the Deck costs ~8-11 us a draw (native run 3:
"draw" 7.96 us, packet 11.04 us) [V], so **100 fewer draws is roughly 0.8-1.1 ms of stage B** (less for cheap
depth-only draws) [I].

## Summary

1. **The HZB size is set only by the scene size, with power-of-two rounding; no cvar changes it** [V]. 4.21's
   `BuildHZB` (01.02 0x15383c0) makes mip 0 `2^(ceil(log2 W) - 1)` by `2^(ceil(log2 H) - 1)` and stretches the whole
   view over it. At 858x482 that is **512x256**; at 1280x720 it is **1024x512**: 4x the texels, which is why 100% hid
   ~122 objects a frame against ~63 at 67%. **Two one-instruction eboot edits (drop the `- 1`) give 67% the 1024x512
   pyramid**: expected ~+50-60 hidden, **~120-145 fewer draws** at 67%'s GPU cost [I]. Sites for both builds below.
   No-code check of the mechanism: **render scale 81%** (1037x583) already gets 1024x512; 72-80% gets 512x512.
2. **The graveyard heavy view has no cascaded sun shadows** [V, 4K captures: no CSM depth target, only 512² per-object
   maps and one cached 256² cube]. `r.Shadow.CSM.MaxCascades`, `r.Shadow.DistanceScale`, `r.Shadow.MaxCSMResolution`,
   `r.MinScreenRadiusForCSMDepth` and `r.Shadow.RadiusThreshold` (whole-scene casters only) cut ~0 draws there. The
   shadow draws are per-object (inset) shadows of movable objects and preshadows; their count is set by
   `r.Shadow.FadeResolution` and `r.Shadow.TexelsPerPixel`, not by the resolution cvars (the presets fix Min = Max).
3. **GY1 has a CullDistanceVolume and per-component draw distances** [V, level name table], so `r.ViewDistanceScale`
   culls there (live today). **No HLOD (`LODActor` in no level) and no precomputed visibility in GY1** [V]: both are
   build-time and unavailable.
4. The biggest non-cvar item stays the GPU-particle simulation: one run of 194 draws, of which 127-155 were dt=0
   identity copies in the captures; skipping copies already identical in both ping-pong buffers is port work
   (frame-cost study item 8), up to ~100-155 draws [?].
5. VRS (`RADV_FORCE_VRS` / QAM "Half rate shading") changes no draw count and leaves depth and the HZB alone. RADV
   skips forced VRS for pixel shaders that read the fragment position [I, Mesa], which covers UE4's deferred lighting,
   post-processing and TAA and likely many base-pass shaders: expect a smaller GPU saving than "half rate" suggests.
   After item 1 its role (afford a larger render scale for culling) mostly disappears.

Ranked list: section 6.

## 1. Where the Deck's draws go, and what cuts each class

No Deck frame capture exists. The class sizes below come from the 4K captures A / B (2,256 / 2,709 draws,
[study-heavy-frame-map.md](study-heavy-frame-map.md)), corrected for what changed since: DBuffer off and `r.EarlyZPass=1`
(partial prepass), one particle run, preshadows off, Low preset, 858x482, weaker HZB (63 hidden vs 171). They add up to
the measured ~1,414, but each row is **[?]** until a Deck capture or a per-pass draw counter exists.

| Class | 4K A / B [V] | Deck estimate [?] | What cuts it in 4.21 (eboot string [V]; semantics [I]) |
|---|---|---|---|
| Base pass (G-buffer) | 630 / 742 | ~650-750 | HZB culling (section 4), `r.ViewDistanceScale` (GY1 has cull distances), `r.DetailMode` (components flagged High/Medium), HZB-hidden objects also skip prepass and velocity |
| Depth prepass | 628 / 739 (forced full) | ~50-150 (partial, `r.EarlyZPass=1`) | `r.EarlyZPass=0` (none), `r.MinScreenRadiusForDepthPrepass` (default 0.03, occluders above it only), `r.EarlyZPassMovable=0` |
| GPU particle simulation | 194 x2 + inject | 195 | `r.GPUParticle.FixDeltaSeconds=0` (done); dt=0 copies: port skip (section 3); `r.GPUParticle.Simulate=0` freezes particles (diagnostic only) |
| Per-object shadows (depth, clear, frustum box, projection) | ~90 + preshadows ~260 / ~445 | ~40-120 | `r.Shadow.FadeResolution`, `r.Shadow.TexelsPerPixel`, `r.Shadow.PerObject=0` (all off); preshadows already off |
| Point / spot whole-scene shadows | 1 cached cube | ~1-10 | cached already (`r.Shadow.CacheWholeSceneShadows=1`); `r.Shadow.CachedShadowsCastFromMovablePrimitives=0` drops the copy and movable casters; `r.MinScreenRadiusForLights`, `r.LightMaxDrawDistanceScale` |
| CSM cascades | 0 | 0 | (none in this view) |
| Velocity | 59 / 85 | ~60-85 | none usable: `r.BasePassOutputsVelocity` needs cooked `GBUFFER_HAS_VELOCITY` base-pass shaders [I] |
| Translucency / particles | 83 / 118 | ~80-120 | `r.ParticleLODBias`, `r.DetailMode`, `r.EmitterSpawnRateScale` (particles, not emitters: no draws) |
| Occlusion boxes + HZB build + test | 18+12 / 11+12 | ~25 | keep (HZB is what culls) |
| Lighting, decals, post | ~40 | ~25 | Low already; DBuffer off |
| UI + present | 39 | 39 | none |
| Port-side extras | 26 / 34 attachment-less draws | ~10-30 | port (frame map item 3) |

### 1.1 Shadows: what actually controls the count (UE 4.21)

- **Per-object (inset) shadows**: one per movable primitive lit by a stationary light (the moon). Created when the
  fade alpha from `CalculateShadowFadeAlpha(ScreenRadiusPx * r.Shadow.TexelsPerPixel, r.Shadow.FadeResolution,
  r.Shadow.MinResolution)` exceeds 1/256 [I, `FSceneRenderer::CreatePerObjectProjectedShadow`]. With the presets'
  Min = Max (256 on Low) above the 64 default fade resolution, the rule reduces to **"screen radius x 1.27 > 64"**:
  only subjects above ~50 px radius get a shadow [I]. At 858x482 that already excludes most distant objects, so the
  Deck has far fewer per-object shadows than the 4K captures [I]. Because Min = Max fixes the map size,
  **`r.Shadow.TexelsPerPixel` and `r.Shadow.FadeResolution` are pure count knobs here**: fade resolution 128 or texels
  per pixel 0.64 doubles the cut-off radius to ~100 px. Shadows fade (`r.Shadow.FadeExponent`), they do not pop [I].
  Help texts [V]: "Resolution in texels below which shadows are faded out", "The ratio of subject pixels to shadow
  texels for per-object shadows". Each shadow costs its subject's sections + 1 clear-like draw (port) + 1 frustum box
  + 1 projection box, ~4-8 draws [V captures].
- **`r.Shadow.PerObject=0`** [V string: "Whether to render per object shadows (character casting on world)"]: removes
  the class. Dan and enemies lose their moonlight shadows: a large visual cost; a measurement, not a preset.
- **`r.Shadow.RadiusThreshold`** [V string: "Cull shadow casters if they are too small, value is the minimal screen
  space bounding sphere radius"] culls *casters* of whole-scene shadows (point / spot / CSM), not lights. The comment
  in `scripts/ue4_override.py:233` ("lights smaller than that on screen cast no shadow") describes it wrongly. The
  graveyard's only whole-scene shadow is a cached point light, so it changes ~0 draws there [I].
- **CSM** knobs: no cascades in this view [V]. They may matter in levels with a movable sun (Pumpkin Gorge reached
  ~6,000 draws a frame on the spacepepperoni fork); `r.Shadow.OcclusionCullCascadedShadowMaps` relies on hardware
  occlusion queries, which the port answers "visible" [V ue4_override.py:487-492], so it would do nothing.

### 1.2 Live or restart

Live today (`ue4_cvars[]`, `src/runtime_extra.c:641-664`, both builds): `r.ViewDistanceScale`,
`r.StaticMeshLODDistanceScale`, `foliage.LODDistanceScale`, the shadow resolution / distance / quality cvars,
preshadows. Everything else goes through the pak: **`GOW3_UE4_CVARS="name=value;..."` in `extra-env.txt`** (applied by
`run.py` into the override pak on every start, `ue4_override.py:511-518`; restart, no build). Live after one
`ue4_cvars[]` row each (object + registration site for 01.00 and 01.02, found with `cvar_find3.py`; values <= 7
chars): `r.Shadow.FadeResolution`, `r.Shadow.TexelsPerPixel`, `r.Shadow.PerObject`, `r.MinScreenRadiusForLights`,
`r.MinScreenRadiusForDepthPrepass`, `r.LightMaxDrawDistanceScale`, `r.ParticleLODBias` (all read per frame on the render
thread [I]). Restart-only: `r.EarlyZPass` (its help text says "Cannot be changed at runtime" [V]), foliage / grass density
(next level load), `r.DetailMode` (live but rebuilds all render state: a hitch).

## 2. Extra culling we do not have

| Kind | In MediEvil? | Usable? |
|---|---|---|
| Per-primitive max draw distance, cull distance volumes | **GY1 `01_GY1_MAIN_Art` and `01_GY1_04_Cliff_Copy` contain `CullDistanceVolume`, `CullDistances`, `LDMaxDrawDistance`, `CachedMaxDrawDistance`** [V name tables]; 19 level packages have one (GY1, GY2, SF, SV, AG, PD, HR, ...) | Yes: `r.ViewDistanceScale` multiplies every cached max draw distance [V help text] [I live]. 0.7 is already the Deck's value; the spacepepperoni fork saw walls and barrels vanish at 0.7 under the Sleeping Village's top-down camera |
| Global minimum screen size for the main view | none in 4.21 (`r.ScreenSizeCullRatioFactor` etc. absent [V]) | Only by an eboot patch in the frustum cull (treat `MaxDrawDistance` as `min(MaxDrawDistance, radius x K)`): a code cave, address not located, pop-in risk [I] |
| `r.MinScreenRadiusForLights` (0.03) | lights [V help] | Safe to raise to ~0.06: small / far lights stop lighting and shadowing; few draws in this view [?] |
| `r.MinScreenRadiusForDepthPrepass` (0.03) | prepass occluders [V help] | Safe: affects only which occluders pre-fill depth, never visibility [I] |
| `r.MinScreenRadiusForCSMDepth` (0.01) | CSM casters [V help] | No CSM here |
| Shadow caster culling for small / far objects | `r.Shadow.RadiusThreshold` (whole-scene), `r.Shadow.LightViewConvexHullCull` (present [V]) | Per-object shadows: `r.Shadow.FadeResolution` / `TexelsPerPixel` (1.1) |
| Precomputed visibility | no volumes anywhere; only `Level_17_HR` has the `bPrecomputeVisibility` name [V] | No (build-time) |
| HLOD / merged actors / instancing | no `LODActor` or HLOD package in any level [V] | No (build-time); `r.HLOD.*` would act on nothing |
| Hardware occlusion queries vs HZB | HZB on through the remap; queries answered "visible" | Keep HZB. Real query results cost a query segment per pass, extra submits and lag tails (T-O2 / T-O3, frame-cost item 12). Make the HZB finer instead (section 4) |
| Foliage / grass | `InstancedFoliageActor` in GY1 [V] | `foliage.DensityScale` fewer instances, not draws (HISM draws per cluster batch) [I]; next level load |

## 3. GPU-particle simulation (UE4 behaviour, port fix)

Every GPU emitter with tiles is simulated every frame because all of them share one pair of ping-pong state textures
[I, 4.21 `FFXSystem::SimulateGPUParticles`]; emitters that did not tick get dt=0 and only copy their tiles. In the
captures 155 / 127 of 194 sim draws were such copies [V]. No cvar removes them without freezing particles
(`r.GPUParticle.Simulate=0`: "Enable or disable GPU particle simulation" [V]). Port rule from the frame-cost study
(item 8): skip a sim draw when it is dt=0 with the same tile set for two frames running and no inject draw touched it
(both buffers then hold the same data). Up to ~100-155 draws [?] on the Deck, behind a toggle bit for a live A/B.

## 4. Full-resolution HZB

**Code** [V, 01.02 `BuildHZB` at 0x15383c0, disassembled]: width = ViewRect (+0x1458 - +0x1450) as float, log2 (library
call and constant), `vroundss ..., 0xa` (round up), `dec`, `max(.., 1)`; the same for the height; `HZBSize = 1 <<
NumMips{X,Y}`; pooled target "HZB" (R16F, lea 0x153847b); SetViewport(0, 0, HZBSize); `DrawRectangle(0, 0,
HZBSize.X, HZBSize.Y, ViewRect.Min, ViewRect.Size, HZBSize, BufferSize)` for mip 0 ("HZB_SetupMip0", lea 0x1538726),
so **the whole view is stretched over the power-of-two target**, not mapped 2:1. The test pixel shader (`FHZBTestPS`,
`HZBOcclusion.usf`) gets `HZBSize` / `HZBUvFactor`. No HZB-size cvar exists in this build (`r.HZB.BuildUseCompute` and
similar are later versions; absent [V]).

| Scene (1280x720 output) | Depth | HZB mip 0 | Depth texels per HZB texel |
|---|---|---|---|
| 50% / 58% / 67% | 640x360 / 742x418 / 858x482 | 512x256 | 1.25x1.41 / 1.45x1.63 / 1.68x1.88 |
| 72-80% | 922x518-1024x576 | 512x512 | ~1.8-2.0 x ~1.0-1.1 |
| **81%** | 1037x583 | **1024x512** | 1.01x1.14 |
| 100% (run 4) | 1280x720 | 1024x512 | 1.25x1.41 |
| **67% + patch** | 858x482 | **1024x512** | 0.84x0.94 |

(UE 4.21 rounds `ceil(log2)`, so width 1024 still gives 512; 81% is the first scale above it.)

**Patch** (both instructions are the `- 1`; 5 bytes, unique matches [V `hzbsite.py`]):

| Build | `dec r14d` (41 ff ce) -> `nop dword [rax]` (0f 1f 00) | `dec eax` (ff c8) -> `xchg ax, ax` (66 90) |
|---|---|---|
| 01.00 | 0x15357ba (XML 0x019357ba) | 0x1535812 (XML 0x01935812) |
| 01.02 | 0x153843a (XML 0x0193843a) | 0x1538492 (XML 0x01938492) |

Mip 0 becomes `2^ceil(log2 W)`: at least the depth resolution, every depth texel sampled (the stock 4-tap "imperfect
sampling" skips texels when the ratio approaches 2). Everything downstream reads `View.HZBMipmap0Size` [I], so the test
stays consistent; the chain gains one mip (one more ~3-vertex draw). The port's N-22 small-mip handling already copes
with the non-square 512x256 pyramid, so 1024x512 should be no different [I]. SSR and SSAO also read the HZB but are
off on Low. At 4K the patch would make a 4096x4096 pyramid (~43 MB, pointless): ship it as a Deck / small-scene patch,
not a default for every output.

**Expected at 67%:** the same pyramid size as the 100% run, built from a coarser depth. The test picks its mip from the
box's size in HZB texels, so a 2x finer pyramid lowers every test by one level; culling should land near the 100% run's
~122 hidden (+50-60), **~120-145 fewer draws (~9-10%), ~1-1.5 ms of stage B**, with no extra scene shading [I]. Doubling
again (2x the depth) adds no information. Risk: more culling also means more exposure to the latent-result pop-in
already accepted (results 3 frames late) and to the one over-culling run seen on the Deck (55-61k hidden per 5 s);
check screenshots.

## 5. Variable rate shading (UE 4.21 has none; driver-level only)

- UE 4.21 has no VRS path [I]. RADV forces it per pipeline: `RADV_FORCE_VRS=2x2`, or Steam's QAM toggle through
  `RADV_FORCE_VRS_CONFIG_FILE` (study-deck-platform-optimization R3). The config file is watched, so the QAM toggle
  switches in a running game [I].
- RADV leaves a pipeline at full rate when its pixel shader reads the fragment position ("breaks postprocessing in some
  games") or has no pixel shader [I, Mesa `radv_consider_force_vrs`]. UE4 full-screen passes (deferred lighting, fog,
  bloom, tonemap, TAA), the velocity pass and any base-pass shader that uses `SvPosition` (dither, screen position,
  pixel depth) read it once shadPS4 turns the GCN position inputs into `FragCoord` [I]. Depth-only passes (prepass,
  shadow depths, HZB build input) are unaffected, so **depth and the HZB keep full resolution and culling is unchanged**.
- What remains coarse: base-pass and translucency shaders that do not read the position. G-buffer values then come in
  2x2 blocks, lit per pixel by full-rate deferred lighting; edges stay sharp. With TAA's sub-pixel jitter the coarse
  grid samples the texture at moving positions, so detail partly recovers over frames, with possible shimmer on
  high-contrast textures where neighbourhood clamping rejects history [I].
- Draws: none saved. Its value was to buy a higher render scale for culling; with the HZB patch, 67% gets the 100%
  pyramid anyway.

## 6. Ranked for the Deck

Draws are per heavy graveyard frame. "One launch" = one game start with the user's go, at the run-5 spot.

| # | Item | Draws saved | Visual cost | Live? | Effort | Test in one launch |
|---|---|---|---|---|---|---|
| 1 | **HZB at depth resolution** (eboot, 2 sites, section 4) | **~120-145** [I] | none expected (conservative test) | restart | XML patch, both builds, Deck / small-scene only | patch on; HZB "hidden a map" and draws vs run 5 (~63 / ~1,430) and run 4 (~122 / ~1,290); screenshots for missing objects |
| 1a | (mechanism check, no code) render scale 81% | same as 1 | none; +46% pixels vs 67% | restart | `render_scale=81` | same counters; confirms the pyramid, not the depth, drives culling |
| 2 | GPU-particle dt=0 copies skipped (port) | ~100-155 [?] | none if exact | live (toggle bit) | stage B code, M | toggle every 30 s; draws, particle look while fighting |
| 3 | View distance below 0.7 (`r.ViewDistanceScale` 0.5) | unknown; GY1 has cull distances [V] | far props pop (0.7 already hid walls in SV) | **live** | menu choice or `GOW3_UE4_CVARS` | first measure the slope live today: Console (1.0) vs Near (0.7) in the heavy spot, draws per 5 s line |
| 4 | Drop the partial prepass (`r.EarlyZPass=0`) or raise `r.MinScreenRadiusForDepthPrepass` to 0.1 | ~50-150 [?] | none; base-pass overdraw on the GPU | restart | `GOW3_UE4_CVARS` | draws, GPU busy % and clock (MangoHud) vs run 5; the PC kept the prepass for GPU load at 4K |
| 5 | Fewer per-object shadows: `r.Shadow.FadeResolution=128` (or `r.Shadow.TexelsPerPixel=0.64`) | ~20-50 [?] | small / far movable objects lose moon shadows (faded) | restart now; live with a row | `GOW3_UE4_CVARS` | draws and shadow look on enemies at mid distance |
| 5x | `r.Shadow.PerObject=0` | ~40-120 [?] | no moon shadows on Dan / enemies | same | same | measurement only (sizes the class) |
| 6 | Effects: `r.ParticleLODBias=1`, `r.DetailMode=1` | 0-40 [?] | fewer / simpler effects; High-detail components hidden | restart | `GOW3_UE4_CVARS` | draws in a fight vs standing |
| 7 | Lights: `r.MinScreenRadiusForLights=0.06`, `r.LightMaxDrawDistanceScale=0.6` | 0-20 [?] | far / small lights switch off | restart now; live with rows | `GOW3_UE4_CVARS` | draws, look of distant torches |
| 8 | VRS 2x2 | 0 | softer shading; possible TAA shimmer | live (QAM) | none | QAM on/off in the spot: GPU busy %, FPS |
| - | CSM cvars, `r.Shadow.RadiusThreshold`, HLOD, precomputed visibility, LOD / foliage density, `r.BasePassOutputsVelocity`, hardware queries, `r.TiledDeferredShading=0` (adds a draw per light once 80+ lights are on screen [I]) | ~0 here | | | | do not test for draws |

**Order:** 1 (or 1a first if a patch build is not wanted yet), then the zero-build measurements 3 (live slope) and 4,
then 5. Items 1 + 2 + 4 + 5 together: roughly 300-450 fewer draws out of ~1,414 (20-30%) [?], several ms of stage B.
One change per launch; re-read `medievil.ini` first; extra cvars go in `extra-env.txt` as `GOW3_UE4_CVARS`.

## Open questions

- The Deck's per-class draw counts are estimates. A per-pass draw counter in the frame stats (depth-only, shadow
  atlas, 6-target base pass, translucency by target signature) or one Deck RenderDoc capture would replace the [?]s.
- Whether `r.EarlyZPass` takes effect after boot in this build (its help says no; the earlier study read it per frame).
- Whether forced VRS reaches any of the port's base-pass pipelines (count pipelines whose SPIR-V reads `FragCoord`).
