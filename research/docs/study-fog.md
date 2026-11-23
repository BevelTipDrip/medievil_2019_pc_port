# Study: MediEvil's fog and how to control it

Status: research done 2026-11-22 (a research agent, read-only; reviewed by Claude), **tabled for v2.4.2** (the user,
2026-11-22: the plan is right, built after v2.4.1). Parent: [START-HERE.md](START-HERE.md). Queue:
[plan-queue-v2.4.2.md](plan-queue-v2.4.2.md) item 1. Related: [study-hdr.md](study-hdr.md) H39 / H41 (the static scan),
H49 (bloom strength: the user found bloom the biggest factor in the Graveyard's blown-out HDR look),
[plan-work-queue.md](plan-work-queue.md) rows "Volumetric / height fog controls" and "volumetric fog flattens contrast".
[V] seen in data or code, [I] inferred.

The complaint (the user, 2026-11-21/22): volumetric / height fog flattens and brightens scenes and lowers contrast, in
SDR and HDR ("technically correct, but looks wrong"); wanted: settings for light scattering intensity, fog
brightness, density.

Data and tools: `research-data/fog-research-20261122/` (D:\MediEvil-archive\port-research-data\fog-research-20261122):

- `scripts/fog_scan.py`: static pak scan, tag by tag per export, so each property is tied to its object and class;
  FColor printed R,G,B.
- `scripts/fog_table.py`: per-level table, UE 4.21 defaults filled in [I], and the GPU values each level should give.
- `scripts/match_values.py`: finds given floats in buffer dumps.
- `scripts/fog_params.py`: dumps and decodes the fog passes by pipeline hash (RD_CAPTURE, RD_OUT, RD_OUT_DIR,
  RD_PREFIX; check `tasklist` first). It labels LightScattering's bindings 3-7 by their position in RenderDoc's list,
  not the shader's buffer index, so those labels are approximate; the raw dumps are right.
- `scripts/fog_survey.py`: every draw / dispatch touching a 3D image (only partly run).
- `out/fog_scan.txt` / `.json`, `out/fog_table.txt` (full per-level detail), `out/gy16057_fog.txt` + `out/gy16057_fog/`
  (buffer dumps), `glsl/` (every compute shader and the candidate fragment shaders, spirv-cross from
  `user/shader/dumps`).

## 1. Fog features per level

- Exponential height fog: one `ExponentialHeightFogComponent` per level [V static]. Volumetric fog on everywhere except
  04 GY2, 05 SF, 19 EH [V static].
- Atmospheric fog: components in GY1, GY2, LA and the AV sequence, all `bVisible=False`: not rendered [V static].
- Fog sheets: `BP_FogSheet_C` (translucent material meshes, not engine fog) 4x in GY2, 1x in EE [V static].
  `BP_DC_StainGlass` has a "Fog Scattering" property; the MapScreen uses fog-clear volumes.
- 11 EE uses an inscattering colour cubemap (another fog pixel shader permutation); 14 LA sets
  `bOverrideLightColorsWithFogInscatteringColors`.

Values a level does not set are UE 4.21 defaults [I]; albedo sRGB converted to linear as the engine does:

| level | density | falloff | inscatter colour | max opacity | start | vol | g | albedo (lin) | emissive | extinction | vol dist | static scatter |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 00 DC | 0.02 | 2 | (0.45,0.64,1.0) | 0.75 | 2048 | on | 0.05 | (0.69,0.69,0.69) | 0 | 1 | 6000 | 1 |
| 01 GY1 | 0.0195 | 2 | (0.24,0.09,0.24) | 1 | 5000 | on | 0 | (0.96,0.34,0.98) | (0.114,0.019,0.125) | 0.2 | 10000 | **5.52** |
| 02 CH | 0.025 | 0.32 | (0.14,0.42,0.71) | 1 | 2667 | on | 0.16 | (0.12,0.43,0.65) | 0 | 5.29 | 10000 | 0.25 |
| 03 HM | defaults (0.02 / 0.2 / (0.45,0.64,1.0)) | | | | | on | 0.2 | white | 0 | 1 | 6000 | 1 |
| 04 GY2 | 1e-5 | 1.3 | (0,0.28,0.48) | 1 | 2619 | OFF | | | | | | |
| 05 SF | 0.0029 | 2 | (0.12,0.05,0) | 1 | 1095 | OFF | | | | | | |
| 06 PG | 0.095 | 0.15 | (0.56,0.22,0.13) | 1 | 5000 | on | -0.2 | (0.48,0.14,0) | 0 | 1.5 | 10000 | 2.5 |
| 07 PS | 0.047 | 1.16 | (0.38,0.17,0.04) | 1 | 0 | on | 0 | (1,0.67,0.22) | (0.099,0.029,0) | 0.76 | 10000 | 1.5 |
| 08 SV | 0.25 | 1.49 | default | **0.038** | 5000 | on | 0 | (0.85,0.9,1) | 0 | 1.5 | 10000 | 0.55 |
| 09 AG | 0.005 | 1.35 | (0,0,0.03) | 1 | 2000 | on | 0.11 | (0.03,0.14,0.18) | (0.041,0.009,0.073) | 0.48 | 10000 | **7.62** |
| 10 IA | 0.035 | 0.2 | default | 0.01 | 0 | on | 0.2 | white | 0 | 1 | 6000 | 1 |
| 11 EE | 0.15 | 0.4 | (0.1,0.61,0.6) + cubemap | 1 | 12500 | on | 0.35 | (0.03,0.17,0.11) | **(0.224,0.224,0.224)** | 3.5 | 10000 | 0.76 |
| 12 AC | 0.016 | 0.71 | (0.26,0.17,0.09) | 1 | 5000 | on | 0.1 | (0.39,0.19,0.09) | 0 | 1 | 4429 | 1 |
| 13 PD | 0.05 | 2 | (0.01,0.02,0.01) | 1 | 5000 | on | -0.11 | (0.01,0.03,0.02) | 0 | 1.8 | 13500 | 0.5 |
| 14 LA | **2.5** | 1.1 | (0,0.22,0.19) | 1 | 6500 | on | -0.11 | (0,1,0.7) | (0.003,0.2,0.167) | 0.8 | 7686 | 0 |
| 15 CC | 0.025 | 0.93 | (0,0.46,0.68) | 1 | 5000 | on | 0.25 | white | (0.03,0.04,0.04) | 1.58 | 5543 | 0.25 |
| 16 GG | 0.08 | 0.49 | (1,0.52,0.68) | 1 | 0 | on | 0.15 | (0.96,1,0.72) | 0 | 1.25 | 4500 | 0.5 |
| 17 HR | 0.08 | 0.65 | (0.32,0.69,1) | 1 | 5000 | on | -0.23 | (0.62,1,0.96) | (0.037,0.053,0.057) | 1.25 | 55000 | 0.08 |
| 18 GS | 0.1 | 0.095 | (0.04,0.09,0.16) | 0.76 | 0 | on | 0.11 | white | 0 | 5.5 | 12500 | 2.83 |
| 19 EH | 0.0025 | 0.55 | (0.68,1,0) | 0.04 | 0 | OFF | | | | | | |
| 20 TD | 0.035 | 0.35 | (0.11,0.2,0.4) | 1 | 5000 | on | -0.23 | (0.16,0.16,0.16) | (0.004,0.017,0.021) | **50** | 55000 | 0.55 |
| 21 ZL | 0.25 | 0.57 | (0,0,0) | 1 | 15000 | on | 0.03 | (0,0.1,0.1) | 0 | 4.3 | 7500 | 1.24 |
| 22 HH | 0.02 | 0.2 | default | 1 | 0 | on | 0.2 | white | 0 | 2.48 | 2800 | 0.3 |
| IN_MM | 0.0035 | 0.077 | (0,0.03,0.04) | 1 | 5000 | on | -0.04 | (0.62,0.86,1) | (0.004,0.113,0.25) | 2.5 | 18500 | 0 |
| MapScreen | 0.025 | 0.01 | (0,0.02,0.04) | 0.5 | 0 | on | 0.2 | white | 0 | 1 | 15000 | 1 |

- Directional inscattering: GY1 exponent 10 / start 0; PG colour (0.42,0.02,0); AG (0.02,0.08,0.17); TD exponent 64;
  IN_MM colour (0.009,0.116,0.141) (full detail: `out/fog_table.txt`).
- Lights' `VolumetricScatteringIntensity` (engine default 1) [V static]: many shadowed lights far above 1 (HM points
  120 / 300, spots 100; AC spots 350; TD 200; PD 200; DC spots 80 / 100; GY1 spot 40); hundreds of others 0. The sun
  is 0 in most levels (GY1, EE, HR, TD); HM's sun 50.
- So the fog's light here comes mostly from the static-lighting scatter (GY1 5.5, AG 7.6, GS 2.8, PG 2.5), a few
  very strong local lights and the emissive (a constant glow: GY1, EE, LA, IN_MM, PS, HR), not the sun [I from the
  values].
- H41's earlier scan printed FColor in stored byte order (B,G,R,A), so its albedos read B,G,R: CH's albedo is
  R98 G176 B211 (bluish), not (211,176,98). GY1's GPU albedo matches the R,G,B reading [V].

## 2. Where the values reach the GPU (Graveyard `cap_frame16057.rdc`; matched to the GY1 fog actor [V])

Render 2880x1620; fog grid 180x102x64 RGBA16F. All buffers come from the gow3 constant ring; dw = dword of the bound
range.

- **eid 8664, `cs_0x5ec79c2cb022e7ab` = FVolumetricFogMaterialSetupCS** (45x26x16 groups of 4^3; writes VBufferA =
  scattering / absorption, VBufferB = emissive):
  - buffer 0 (112 B, VolumetricFog global UB): dw 0-2 GridSizeInt, dw 8-10 GridZParams (0.000292, 0.9943, 32);
  - buffer 1: the View UB (3296 B);
  - buffer 2 (96 B, the pass globals): **dw 16-18 GlobalAlbedo** (0.95597, 0.33716, 0.98225 = sRGB 250,157,253
    linear) [V]; **dw 20-22 GlobalEmissive** (1.138e-5, 1.89e-6, 1.25e-5 = actor emissive x 1e-4) [V];
    **dw 23 GlobalExtinctionScale** 0.2 [V];
  - buffer 3 (144 B, FogStruct): dw 1 = 0.002 (falloff / 1000), dw 8 = 1.9524e-5 (density / 1000), dw 9 = 2371.471
    (fog height);
  - maths [V GLSL]: extinction = max(0, 0.5 x dw23 x dw8 x 2^(-dw1 (z - dw9))); scattering = albedo x extinction;
    absorption = max(extinction - luminance(scattering), 0).
- **eid 8669, `cs_0x27e12fb9da9f873c` = TVolumetricFogLightScatteringCS** (45x26x16): output = Lighting x
  VBufferA.rgb + VBufferB, blended with history:
  - buffer 2 (560 B): dw 32 HistoryWeight 0.9, dw 100 history-miss supersamples 4, dw 101 PhaseG 0 (Henyey-Greenstein
    g), dw 120 sky light volumetric scattering intensity 0 (GY1's skylight 0; meaning from code [I]), dw 136
    UseHeightFogColors 0, dw 137 UseDirectionalLightShadowing 1, **dw 138 StaticLightingScatteringIntensity 5.52381**
    (x the volumetric lightmap SH x 1/pi) [V];
  - ForwardLightData (640 B): dw 20-22 DirectionalLightColor (0.629, 1.25, 1.131), dw 23 directional light volumetric
    scattering intensity 0 (GY1's sun) [V]; FogStruct bound too;
  - other permutations in the dumps: `cs_0x3ab54396073f9987` (same fields +36 dwords: HistoryWeight 68, PhaseG 137,
    sky 156, static 174) [V code]; `d43d0169801a9d35`, `ec5bdf7b44f8d954` (PhaseG 116, sky 136, static 154) [V code,
    not captured]. The cost study had 27e12fb9 / 3ab54396 at 0.15-0.28 ms.
- **eid 8674, `cs_0xa5eaac3cdb11ab72` = FVolumetricFogFinalIntegrationCS** (23x13x1 groups of 8x8, integrates along Z):
  no intensity constants (64 B globals: a matrix) [V code].
- **eid 12076, `fs_0xbf9bcd9c07f3c6d0` (vs `0x7f76e748b9d60c12`) = TExponentialHeightFogPS
  <DirectionalLightInscatteringAndVolumetricFog>**, full screen, samples the integrated 3D texture:
  - buffer 0 = View: dw 700-702 VolumetricFogInvGridSize, 704-706 GridZParams, 708-709 SVPosToVolumeUV, 710
    VolumetricFogMaxDistance 10000 [V];
  - buffer 1 = FogStruct (144 B): **dw 0** collapsed density 7.19e-5, dw 1 falloff / 1000 0.002, dw 2 cos terminator
    0.99999, dw 3 StartDistance 5000; **dw 4-6 inscattering colour** (0.2396, 0.0898, 0.2376); dw 7 = 1 - max opacity
    (0); **dw 8** density / 1000 1.9524e-5, dw 9 height 2371.47, dw 10 cubemap flag 0, dw 11 cutoff 0; dw 12-14
    InscatteringLightDirection, dw 15 directional start distance (>= 0 enables); **dw 16-18
    DirectionalInscatteringColor** (0.2626, 0.2626, 0.1313 = default (0.25, 0.25, 0.125) x sun luminance 1.05), dw 19
    exponent 10; dw 20-21 cubemap rotation, dw 24-26 cubemap texture parameters, dw 27 ApplyVolumetricFog 1 [V];
  - output [V GLSL]: rgb = (colour (1 - T) + directional) x Tvol + volumetric.rgb, x the light-shaft mask; alpha =
    Tvol x T. The analytic fog starts at max(StartDistance, VolumetricFogMaxDistance): near the camera the volumetric
    fog does all of it.
  - `fs_0xff1529dd7fa4e7c3`: the HeightFogAndVolumetricFog variant (same FogStruct, no dw 12-19) [V code]. Not found
    in the dumps yet: the variants without volumetric fog (GY2 / SF / EH) and the inscattering-texture one (EE).
- Translucent materials: many translucent vertex shaders read FogStruct dw 0-14 (per-vertex fog) [V IR]; translucent
  pixel shaders (e.g. `fs_0x5c0ce8956cea89f3`) sample the integrated 3D texture through View dw 702-709 [V], so they
  follow any change made in the volumetric passes.

## 3. Cvars

In the eboot [V]: `r.Fog`, `r.VolumetricFog`, `r.VolumetricFog.` GridPixelSize / GridSizeZ / DepthDistributionScale /
HistoryWeight / HistoryMissSupersampleCount / InjectShadowedLightsSeparately / InverseSquaredLightDistanceBiasScale /
Jitter / TemporalReprojection / LightFunctionSupersampleScale / VoxelizationSlicesPerGSPass /
VoxelizationShowOnlyPassIndex, `r.FastVRam.VolumetricFog`, `r.SupportAtmosphericFog`. None scales fog intensity,
density or colour [I, 4.21 semantics]: `r.Fog` / `r.VolumetricFog` are on / off (volumetric off looks worse: pink
floors, a washed-out Mausoleum; study-fork-spacepepperoni); the rest change resolution, temporal behaviour, noise or
near-light falloff. GridPixelSize / GridSizeZ come from the PS4 device profile (16 / 64): a live row would be needed.
Only `r.VolumetricFog` is in `ue4_cvars[]`; each further cvar needs its object address and registration site per build.

## 4. Options

- **(a) Scale the fog constants on the CPU as the game's fog passes upload them** (recommended). Hook:
  `Rasterizer::CollectRingBindings` (stage A copy into the constant ring) plus the stage B stream fallback; precedent:
  the Color tab's grading (`Ue4GradeTarget` / `ApplyUe4Grade`, vk_rasterizer.cpp, ring reuse excluded, counters per
  path). Patch table by (pgm_hash, buffer): MaterialSetup `5ec79c2c` buffer 2 dw 16-18, 20-22, 23; height fog
  `bf9bcd9c` / `ff1529dd` buffer 1 dw 0, 4-7, 8, 16-18; optionally LightScattering (per permutation) dw 120 / 156 /
  136, 138 / 174 / 154, ForwardLightData dw 23. Cost: a few multiplies on ~3 bindings a frame and those copies not
  shared by ring reuse: effectively nothing. Live; scene-linear, before the tonemapper: the same in SDR and HDR [I:
  the fog passes run at eids 8664-12076, the tonemapper ~13000]. Risks: hashes per permutation and level (the
  non-volumetric variants, EE's cubemap variant, the 3ab54396 / ec5bdf7b layouts); LightScattering's history (0.9):
  a change settles over ~10-20 frames; translucent per-vertex fog keeps the unscaled FogStruct unless those ranges are
  patched too (only visible beyond VolumetricFogDistance); the ring-full fallback must patch too. A dev switch in the
  style of `GOW3_UE4_GRADE` (e.g. `GOW3_UE4_FOG="ms.d23*=0.5;hf.d4*=0.7"`) would allow tuning before menu rows.
- (a') A recompiler pass on FinalIntegration (`a5eaac3c`) scaling scattered rgb and extinction: exact for every
  consumer (fog volume materials too), but a continuous slider needs a push constant; the refraction-scale variant
  mechanism would recompile per value. More work than (a).
- (b) Memory patches of the fog actors: the render thread copies the properties into its scene info at registration,
  so a write needs `MarkRenderStateDirty` or the game's setters (addresses per version); light scattering is spread
  over hundreds of light components. High effort and risk: not recommended.
- (c) Cvars: no intensity control (section 3).
- (d) Override-pak edits of the maps (`scripts/ue4_override.py` `edited_asset` / `apply_edits`: same-length float edits
  in the lighting sublevel `.uexp`, ~34 MB; GY1's art map alone 21.6 MB): exact engine semantics, per-light
  scattering too, per-level presets; but restart only, only properties a level saves (HM, HH, DC leave many at their
  defaults), a larger pak. A possible later complement.

## 5. Plan (approved by the user 2026-11-22, tabled for v2.4.2)

Build (a). Rows, live, 0-200%, default 100%, global first (keyed per level / grading block later, with the v2.4.1
per-level settings):

1. **Fog density**: MaterialSetup buffer 2 dw 23 x k, and the fog pixel shader FogStruct dw 0 and dw 8 x k (the
   analytic fog beyond the volume and in the levels without volumetric fog). How much the fog veils and lowers
   contrast.
2. **Fog brightness (light scattering)**: MaterialSetup buffer 2 dw 16-18 x k (albedo): every light's in-scattering
   (sun, sky, static, local lights, shadowed injection) while the veil stays (extinction unchanged while albedo x k
   <= 1); and the fog pixel shader dw 4-6 x k (analytic colour). The main control for "brightens and flattens".
3. **Fog glow (emissive)**: MaterialSetup buffer 2 dw 20-22 x k: the constant self-lit fog (GY1 purple, EE grey 0.224,
   LA, IN_MM, PS, HR); a likely cause of lifted blacks [I].
4. Optional: ambient fog scattering (LightScattering static dw 138 / 174 / 154 x k; large in GY1 / AG / GS / PG) and
   sun inscattering (fog pixel shader dw 16-18 x k).

## 6. Before building

- Run `fog_params.py` on the other captures (DC 46077, HM 82572, SV 32266 / 48493, HH 30045): which LightScattering
  permutation each level uses and the 3ab54396 layout's values; whether the height-fog-only (GY2 / SF / EH) or
  inscattering-texture (EE) pixel shader appears, and its hash.
- Find the InjectShadowedLocalLight draws (into the 3D target) and any fog volume voxelization draws (fog volume
  materials would bypass the density scaling; brightness still applies through lighting x scattering).
- Which term dominates the flattening: dump VBufferA / VBufferB, the LightScattering output and the integrated texture
  at a foggy spot; compare emissive, static and local-light contributions to choose ranges and defaults.
- Whether 01.00 has the same shader hashes [I: likely].
- Incident (2026-11-22): the research's first replay was running when the user launched the game (~5 minutes in); it
  was stopped as soon as it was noticed. Future replays: a watchdog that stops the replay when the game starts.
