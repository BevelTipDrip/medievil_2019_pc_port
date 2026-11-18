# HDR that keeps the SDR look: our own grading LUT and UI composite (design 2026-11-17; step 1 built and run, H24-H28)

Parent: [study-hdr.md](study-hdr.md) (route A, runs H1-H22, "RenoDX's Unreal Engine approach"). Shader map:
[notes-ue421-lut-builder-map.md](notes-ue421-lut-builder-map.md) (line-referenced, from the game's own translated
shaders). Tags: [V] verified in code / data, [I] inference.

## Why

The user's reports (2026-11-17): raised blacks and low contrast against SDR (H3, H14); the HUD and UI washed out; intro
movies over-bright and washed out; menus that dim the game turn the background black. All four come from two game
shaders:

- **Grading LUT builder** (`fs a4152b3be93c832a`, UE 4.21 CombineLUTs, every frame). In HDR (output device 3) it takes
  the graded colour *before* blue correction and the film curve and sends it through UE's ACES RRT + ODT 1000 [V]. The
  SDR chain (blue correction, film curve, polynomial, colour scale / overlay, InverseGamma) is skipped, so the game's
  look, its black levels and contrast, never reach HDR. The whole SDR chain is still computed in the shader for every
  texel [V], so the SDR-graded colour G is available.
- **UI composite** (`fs c401ca35f9bceb8e`) with its LUT (`fs c401ca35ab81eb39`, built once). The UI texture's values go
  straight into the LUT as `PQ(ui * 1e-4)`; the LUT multiplies by 1.5 and runs ACES RRT + ODT 1000 [V]. There is no sRGB
  or 2.2 decode in either shader [V]; if the UI target is UNORM (to check), gamma-encoded colours are treated as linear,
  which lifts every mid-tone: washed out [I]. Movies are drawn as UI and get the same treatment [I]. Where the UI is
  translucent the composite multiplies the scene by `1 + a * (L / (1 + Y / L) - 1)` with Y in nits and L the UI level
  (1.0): every scene brightness is squashed towards ~1 nit before the usual `(1 - a)` dim, so dimmed menus show black
  [V maths, I that it is the black background].

## How (RenoDX's approach, MIT; our own GLSL)

The port already replaces a game draw with host passes (`Rasterizer::ObserveFrameMap`: DLSS replaces UE4's TAA draw and
skips it). Two more replacements, HDR output only (SDR keeps the game's draws untouched):

### 1. Grading LUT pass (replaces the `a4152b3be93c832a` draw)

A host shader that writes the same 32-layer LUT target. Its parameter block is the game's (the 576 bytes the draw binds,
our Color tab's changes applied as now), so every area keeps its grading.

- **Input decode:** the LUT texel's coordinate as the game's HDR tonemapper indexes it: inverse PQ x 10000 (scene
  linear) [V].
- **The game's chain, ported from the translated shader** (not Epic source): white balance, expand gamut, ColorCorrectAll
  -> **U** (untonemapped); blue correction, film curve (with FilmBlackClip forced to 0, as RenoDX does), inverse blue
  correction, polynomial, colour scale / overlay, InverseGamma -> **G** (SDR graded, linear BT.709).
- **Upgrade (RenoDX `UpgradeToneMap`):** N = a neutral SDR curve of U (100-nit peak, 0.18 -> 0.18); ratio = Y(U) / Y(N)
  below it, else (Y(G) + Y(U) - Y(N)) / Y(G), faded in by saturate(Y(U)); out = G * ratio with OKLab hue correction
  towards G. Up to SDR white the result is the SDR picture; above it only the brightness the SDR curve removed returns.
- **Display mapping:** luminance-based roll-off to the peak (RenoDX's Reinhard scalable: mid-grey kept, peak reached at
  a white clip), gamma 2.2 emulation (`decode2.2(encode_sRGB(c))`: SDR's black levels on a 2.2 display), x game
  brightness (paper white), BT.709 -> BT.2020, PQ, / 1.05 (the tonemapper multiplies by 1.05) [V].
- **Settings (Display > HDR):** Peak brightness (nits; the user's AW3225QF: ~1000 in its 1000 mode with ABL, ~450 in
  HDR400), Game brightness (paper white, nits), Gamma correction (2.2 / sRGB), a strength for the upgrade (0 = SDR look
  only). Defaults (the user, 2026-11-17): peak Auto (Windows' reported peak) or a value, game brightness 203, gamma 2.2,
  all adjustable; UI brightness stays the existing setting (default 100), adjustable.

### 2. UI composite pass (replaces the `c401ca35f9bceb8e` draw)

A host full-screen pass with the draw's two images (scene PQ, UI) into its display target, as an SDR screen would blend:
scene PQ -> nits / UI nits -> encode 2.2; UI decoded as the SDR display would (2.2, or sRGB as an option); premultiplied
blend `ui + scene * (1 - a)` in that 2.2 space (the SDR dim, no squash); decode, x UI brightness (our setting,
`hdr_ui_nits`), clamp to peak, PQ. The UI LUT and `r.HDR.UI.Level` are no longer used (the setting moves into our pass;
the overlay keeps reading it). Movies land at UI brightness with the SDR decode.

## Costs

Both replace one game draw each with one host draw: the LUT is 32 x 32 x 32 texels a frame (tiny); the composite is the
same full-screen pass the game runs. CPU: one hash compare per draw (already there for the Color tab), one host pass
record per frame each. SDR: nothing changes. Diagnostics behind `GOW3_DEV_TOOLS`.

## Steps (each its own build and run, the user's go per launch)

1. **SDR-match check:** the grading pass outputs G only (no upgrade, no roll-off) at a paper white. An HDR JXR against
   an SDR PNG of the same view must match below SDR white (ratio of luminance per block ~ constant, no hue shift). This
   proves the port of the game's chain.
2. **Upgrade + roll-off + 2.2 emulation**, with the settings. Check: blacks as SDR, highlights above SDR white up to the
   peak, no clipping edge; the user's eyes on 1000 and 450.
3. **UI composite:** UI colours against SDR (same menu, PNG / JXR), the movies, a dimmed menu.
4. Then the meter test chart and calibration, the SDR Brightness setting (dw4 / dw5 InverseGamma: probably UE's
   DisplayGamma, 2.335 in the capture [I]), the regression A/B against copy-fault-trace.

## Open before step 1

- ~~The UI target's format~~: `R8G8B8A8_UNORM`, no sRGB view (H23) [V]: the UI's gamma-encoded values are read as linear.
- dw5 (InverseGamma) in SDR: no SDR capture of the block yet.
- The LUT input in HDR is PQ over 0-10000 scene units: the SDR range uses only ~10 of the 32 texels per axis [I].
  RenoDX upgrades the LUT to FP16 but keeps UE's indexing; if banding shows, the tonemapper's index is the next lever.
