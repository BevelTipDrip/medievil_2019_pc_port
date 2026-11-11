# Dan's Crypt water at effects resolution below 100: UE4's light grid lookup

Parent: [START-HERE.md](START-HERE.md). Card: [ue4/light-grid-scale](../patches/ue4/light-grid-scale.md).
Work queue: [plan-work-queue.md](plan-work-queue.md) ("Water refraction glitch in Dan's Crypt").

## The glitch

The user, 2026-11-09 / 10 (01.02): the cyan patches on the water in Dan's Crypt get hard edges and look wrongly
scaled. Not DLSS, not render scale; `effects_resolution` below 100 (the user: "turning it back up to 100 fixes
it"). The patches themselves are the level's own look ("The cyan patches are the levels natural effect, they're
supposed to be there, the issue is at 75% they end up getting a hard edge, and it seems like they're not scaled
properly").

## Cause (RenderDoc, 2026-11-10)

Captures (untracked, `research-data/dans-crypt-refraction-20261110/`): crypt_frame38561 and 46077 at 75%, 54294 at
100%; launcher log `data/crypt-refraction-renderdoc-0102.log`.

- At 75% the water draws into the separate translucency target (R16G16B16A16, 1932x1086, viewport 1929x1086) with
  the downsampled depth; the full view is 2573x1448.
- The edges sit on 64-pixel lines of the small target (x 768, 1280, row 320). Same primitive and alpha on both
  sides of an edge, colour about 6x apart (pixel history).
- The water's fragment shader (`fs_0xe3b625eaec7fca55`) picks reflection captures and local lights from UE4's
  forward light grid: `cell = uint(SvPosition.xy - ViewRectMin) >> LightGridPixelSizeShift`. ForwardLightData
  (b2): `LightGridPixelSizeShift` 6, grid 41x23x32 cells, 53 reflection captures. The grid is built for the full
  view.
- Separate translucency at 75% does not scale the position, so the shader reads the top-left 31x17 cells:
  reflections cut on cell lines and squeezed toward the top-left. It is UE4 4.21's own behaviour; the PS4 runs
  separate translucency at 100%.
- Ruled out: the downsampled depth (matches the full depth), the depth-fade UVs (`ScreenPositionScaleBias`
  right), the floor depth (flat).

## The fix

A shader recompiler pass (`LightGridScalePass`) finds `ShiftRightLogical32` by a non-constant amount of a
`ConvertU32F32` of a FragCoord-derived value and marks the shader (`uses_light_grid`). The pipeline cache records
the full view from each frame's G-buffer draws (`RecordLightGridScene`) and gives a marked shader drawn at the
effects-resolution size of that view a variant that multiplies the position by full / small per axis
(`LightGridScale`). Any percentage works; 100% and full-size passes keep the unchanged shader.

## T-LG1: the fix in game (2026-11-10, launched by Claude with the user's go, log `data/crypt-light-grid-fix-0102.log`)

01.02, 3840x2160 output, render scale 67, UE4 TAA, DLSS off, refraction 2; `effects_resolution=50` at start, then
75 and 100 live in the overlay, in Dan's Crypt at the water. First start after the cache version bump.

- The user: "Yes, all 3 now look correct!" and "Everything looks good".
- Six fragment shaders have the lookup: `e3b625eaec7fca55` (the water), `3aa786c1b5a3882f`, `591c5d142a39fbaf`,
  `6237d71f64d79c68`, `c32da5c3a0033c75`, `f1eef25b0c2ae13a`. Scaled variants compiled at 50% (2.0008 x 2.0000)
  and 75% (1.3339 / 1.3335 x 1.3333) for the water and three of the others.
- Not checked: the cauldron fog in the forest level (the user suspects the same cause), which effects the other
  five shaders draw.
