# UE4 console variables: the framedsc list against this build (2026-11-21)

Two research agents, 2026-11-21. Sources: framedsc's UE4 command list (https://framedsc.com/GeneralGuides/ue4_commands.htm,
a UE **4.27** dump, so names and defaults can differ from our 4.21) and Epic's 4.27 "Adjusting engine feature levels"
page (https://dev.epicgames.com/documentation/unreal-engine/adjusting-engine-feature-levels?application_version=4.27,
actually a short profiling cheat sheet). [V] checked in the eboot, the game's inis or the shader IR; gains are
inference until tested.

Saved outside the repo (third-party pages): `C:\Users\Ty\MediEvil\reference-pages\` holds the framedsc page (HTML and
markdown), Epic's page (markdown) and the agent's full findings (`framedsc-ue4-commands-findings.md`, with the
present-in-eboot list).

## Presence check [V]

`E:\MediEvil\CUSA11227-0102\eboot.bin`, whole null-terminated UTF-16LE strings equal to the cvar name: 1,420 of the
page's 4,594 entries are present (`r.*`: 772 of 1,737). All 54 cvars the port's code names are present. `ShowFlag.*`
names are built at run time: unknown. 181 cvars are in the eboot but not on the page (PS4 submit settings,
`r.Tonemapper.ConfigIndexOverride`, `r.Tonemapper.MergeWithUpscale.*`, `r.EarlyZPassMovable`, ...).

## Existing work

- Occlusion: `r.AllowOcclusionQueries` is off in the port (shadPS4 answers every query "visible", so they culled
  nothing; ~5% gained, card occlusion-queries-off). `r.HZBOcclusion` and the remap are on (hzb-remap).
  `r.NumBufferedOcclusionQueries=2` was rejected (R4a). The other occlusion cvars don't matter while every query
  answers "visible".
- Others with cards: `r.DBuffer` / `r.EarlyZPass` (no-decal-prepass), `r.GPUParticle.FixDeltaSeconds`
  (single-particle-pass), `r.SeparateTranslucencyScreenPercentage`, `r.Shadow.Preshadows`, the live graphics
  options, the rejected streaming / pool / engine-cap tests.
- New facts [V]: the PS4 device profile sets `r.VolumetricFog.GridPixelSize=16` / `GridSizeZ=64`, so the fog grid
  item needs a live row (the boot pak is outranked). No game ini sets the PS4 submit cvars (closes
  study-eop-wait-site's "ini overrides" check).

## Candidates (all present in this build; untested)

1. `r.ViewTextureMipBias.Offset/Min`: sharper textures when DLSS or FSR render below 100%.
2. `r.PS4CommandBufferSplitSize`, then `r.PS4ContinuousSubmits=0`: fewer submits per frame, maybe a shorter
   end-of-frame wait. Never `r.PS4EventBasedFrameEnd=0` (the submit loop would poll with Sleep(0)).
3. Volumetric fog grid (live row; see above), `HistoryMissSupersampleCount` 4 -> 2.
4. `r.EmitterSpawnRateScale` 0.5-0.75: less particle overdraw in Deck fights.
5. `r.GTSyncType=1`, then `r.OneFrameThreadLag=0`: Deck frame pacing and latency.
6. The never-run streaming set from plan-frame-pacing N-9 (start with `s.ForceGCAfterLevelStreamedOut=0`).
7. The draw-reduction set (study-ue4-draw-reduction): shadow fade resolution, per-object shadows, minimum light
   screen radius.
8. TAA sharpness: `r.TemporalAACatmullRom=1`, `r.TemporalAAFilterSize`, `r.TemporalAACurrentFrameWeight` (0.04 now),
   `r.TemporalAASamples`. The lead for a sharpness setting: the tonemapper has no sharpen path (study-hdr.md H38).
9. `FX.AllowGPUSorting=0`: the game has ~175 GPU sprite systems, but whether any sort is unknown; try only if a
   capture shows GPU sort passes. Risk: wrong layering in alpha-blended smoke and fire.
10. Lower value or higher risk: `r.DynamicRes.*` (steers by GPU time, emulated timestamps on the port), parallel
    command-list settings, LOD bias.

Test route: `GOW3_UE4_CVARS="name=value;..."` (boot only; card tooling/ue4-cvars-tool), one change per run, judged
with the flip trace for performance items.

## HDR and the tonemapper

- Keep tonemapper-variant cvars at their defaults (`r.Tonemapper.Quality`, `GrainQuantization`, `Sharpen`,
  `ConfigIndexOverride`, `MergeWithUpscale.*`, `r.TonemapperFilm`): each may switch to another cooked tonemapper with
  another hash, which the HDR patches key on. Keep `r.EyeAdaptation.PreExposureOverride` 0.
- `r.LUT.Size` alone cannot work: 32 is built into the tonemapper (0.96875 / 0.015625).
- The tonemapper's grain is `r.Tonemapper.GrainQuantization`, a cooked permutation: in HDR ~+-2 10-bit codes;
  removing it in HDR is a recompiler patch (H38).
- FXAA: the game runs TAA; in HDR the target's alpha has 2 bits, so FXAA would get a useless luma.
- No `r.HDR.Aces.*` or peak-luminance cvar in this build.
