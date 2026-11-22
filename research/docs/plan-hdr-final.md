# Plan: the final HDR implementation (on copy-fault-trace)

Status: **reviewed by the user, answers below** (2026-11-22). Nothing here is built. Background: study-hdr.md H38-H43,
work queue rows "HDR output", "HDR consolidation before the merge", "Bloom strength control", "Volumetric / height fog
controls".

## Decisions already made (the user)

- Built on **copy-fault-trace** (`22744f1`, the known good state), not by merging `hdr`: only what the final design
  needs is carried over; regression-tested there (2026-11-22).
- Our curve, fed by the game's per-area grading instead of the game applying it: the user's controls (game brightness,
  peak, toe, mid contrast, ...) stay, as **adjustments on top of each area's own values**; global adjustments first,
  adjustments keyed by grading block only if more granularity is needed (H41-H42).
- The raised blacks of the levels with a grading LUT must be removable (H42).
- **Our own tonemapper pass** replaces the game's tonemapper draw: no x100 shader variant, no recognition pass, no
  per-draw pipeline check (H43).
- Release build: no logging, tooling, dumps or tests (`GOW3_DEV_TOOLS` only); allowed: crash logs, start-up lines,
  the light FPS / flip trace. Bloom and fog controls come after this work.

## Principles

1. **HDR off = copy-fault-trace.** With "Allow HDR output" off the release build runs the baseline's code paths:
   `Ue4HdrGrading` not created, the per-draw test a null check (as H35 / `88f5963` made it). Checked by A/B.
2. **No CPU-GPU synchronisation.** Nothing reads GPU data back; every new pass records into the same command stream.
3. **Work only on change.** The game rebuilds its grading LUT every frame (every level capture holds one); ours is
   rebuilt only when its inputs change (grading block hash, level LUT image, settings), otherwise reused.
4. **One look path.** The SDR-look curve (H29-H39: gamma emulation, highlight extension, roll-off variants) is not
   carried over; the piecewise curve with per-area values is the only "our look". The game's ACES look stays as the
   other choice of "HDR look" (open question 2).

## Design

### Per frame, in "our look"

1. **Grading LUT builder draw** (fs a4152b3be93c832a / 9f6bac6cba984956): skipped, as now. The CPU reads the
   parameter block (576 / 656 bytes, as now) and hashes it with the level LUT image id and the settings revision.
   Same hash as the last build: nothing else happens. Changed:
2. **Area values** (a tiny GPU pass, one invocation): about six GradeScene evaluations on greys with the level LUT,
   and six without it (H42 anchors: black, Y(U) 0.006 / 0.02 / 0.07 / 0.18 / 0.36), then a few Newton steps on our
   analytic curve: floor, grey level, gamma, toe length / strength / clamp, for the LUT and no-LUT variants. Written
   to a small persistent buffer.
3. **Our LUT** (one draw, as `Ue4HdrGrading::Run` now): reads the area values, blends them by **Level LUT tone**,
   applies the user's adjustments, evaluates the curve per texel with the game's colour (G re-toned, H40) and the
   shoulder to the peak; written into **our own LUT image** (format and size ours: RGBA16F; 32^3 to start, 64^3 a
   later option, H38), indexed by our own encoding (log2 of scene luminance, chosen for resolution near black).
4. **Tonemapper draw** (fs 12178a9314cb761e, map variant 52770387e0d6363a): skipped; **our tonemapper pass** writes
   its colour target instead. Inputs as the game's draw binds them (H41 survey): scene colour, bloom (half
   resolution), the 1x1 eye-adaptation texture (exposure), dirt mask; constants: scene colour scale, bloom scale
   (later x the user's bloom strength), vignette, dirt-mask tint (the map variant: the same fields 4 dwords later,
   plus grain). It computes what the game's does (H41: scene x scale + bloom x (scale + tint x mask), x exposure x
   vignette), samples our LUT at our index, and writes PQ BT.2020 plus the alpha the game writes (luma for later
   passes). No grain in HDR (queue: grain removal), so no `fract(sin())` per pixel.
5. **UI composite** (carried over, `1b168b5`) and the **menu blur backdrop** (`9331e2f`): as now.

### Settings and menu

- Kept: Allow HDR output (launcher), HDR look (ours / the game's ACES; ACES out of the menu before release), UI brightness, game brightness, peak, the
  Color tab and panel (relative UE4 grading: they change the block, so the area values follow).
- New / changed (adjustments, relative to the area): **Black floor** (0-100% of the area's, default 0), **Level LUT
  tone** (0-100%, default 100%, H42), **mid contrast** (x),
  **toe clamp** (+), **toe length** (x), **toe strength** (+); absolute: shoulder start / length (range widened past
  5 stops: the user's Hilltop setting sat at the end) / angle, highlight saturation. Defaults: no adjustment (each
  area as the game grades it, the LUT tone 100%) or the user's preferred starting point (open question 1).
- Dropped: gamma emulation and its mode, Highlights, Highlights begin at, Highlight contrast, Roll-off start, the
  Piecewise absolute rows, Tone curve choice.

### What carries over from `hdr`, what does not

| Carried over (re-applied on copy-fault-trace, reviewed line by line) | Not carried over |
|---|---|
| HDR10 swapchain / capability (`d25d4f5`), 10-bit frame image, UI composite LUT compiled on the spot | `HdrLutScalePass`, `uses_hdr_lut_index`, the x100 runtime info / pipeline key / serialization versions, MustCompileSync for it, `GOW3_HDR_LUT_X100` |
| Overlay in HDR10, UI brightness (`931a603`), Color tab / panel, `GOW3_UE4_GRADE` (dev) | SDR-look curve: `HdrSdrLook`, `HdrHighlightBoost`, `HdrRollOff`, gamma modes (H29-H39) |
| `ue4_grade.glsl` (the port of the game's grading chain, with the level LUT blend) | Near-black correction (already removed, H36) |
| The LUT pass infrastructure (`Ue4HdrGrading`: pipelines, push descriptors, the builder skip) | The tonemapper's structural recognition (`137a8a1`) |
| UI composite, blur backdrop, the launcher's neutral game Brightness | Test patterns and meter (left on `hdr`) |

Every log line on these paths is reviewed: release keeps start-up lines only.

## Steps (each one verified before the next)

1. **Branch `hdr-final` from copy-fault-trace.** Re-apply the carried-over infrastructure, HDR off untouched.
   Check: HDR off release build against the copy-fault-trace exe (`medievil_cft`), flip trace, lows and worst
   frames (as H34).
2. **Our tonemapper pass, with our LUT built by today's Piecewise curve** (absolute values, no area values yet).
   Check: the look equals the current `hdr` build's Piecewise at the same settings (screenshots); the map screen;
   menus and movies; GPU time of the pass against the game's draw (dev-build timestamps).
3. **Area values + adjustments + Level LUT tone.** Check offline first (the harness: the GPU pass's formulas against
   `h42/derive.py` on the captured blocks), then in game: each level at the defaults against its SDR look, the user's
   adjustments carrying across levels; Cemetery Hill (adaptive exposure) and the local volumes of Inside the Asylum /
   Lake / Haunted Ruins (blends: the area values follow smoothly, no LUT rebuild storm).
4. **LUT rebuild only on change.** Check: rebuild count per minute in the dev build (event log on change only).
5. **Performance A/B** (the user's runs): HDR on (our look) and HDR off against copy-fault-trace; lows, worst frames,
   panning and combat.
6. Then: adjustments keyed by grading block (if needed), bloom strength, fog (memory patches), grain / FXAA luma.

## Progress

- **Step 1** (2026-11-22, `e32c55f`, exe 06:05:42): built (release and dev), 221 Python tests pass. **Run: HDR-off A/B
  against copy-fault-trace** (exe 21:02:09, `medievil_cft`), the user's route, flip traces, both logs quiet (events
  only). Gameplay after 20 s, loading hitches (> 100 ms) excluded: hdr-final 233 s, average 225.2 FPS, 1% / 0.1% lows
  126.6 / 51.1, frames > 8 ms 18.8 / min, > 10 ms 17.0 / min, worst 47.1 / 31.6 / 31.6 ms; copy-fault-trace 244 s,
  224.1, 130.4 / 53.6, 19.7 / min, 17.5 / min, 46.2 / 32.3 / 32.3 ms. Frames > 6 ms 122.4 vs 79.8 / min, but isolated
  spikes (a frame > 6 ms among fast frames) 3.9 vs 3.7 / min: the rest lie inside slow, GPU-bound stretches (~140-165
  FPS), and the hdr-final run spent one more second in one (7 vs 6 seconds with a mean frame > 5.5 ms). The steady
  capped stretches match (237.2 FPS, 1% ~223 in both). No regression seen.
- **Step 2** (2026-11-22, `e07270f`, fix `7e18d53`): our tonemapper pass and our own LUT (study-hdr.md H44). Run 1:
  the scene black under the UI (a pending fast clear of the target; RenderDoc). Run 2 with the fix: works in levels
  and every menu (the user). Open: the screenshot comparison against the `hdr` build (same curve, the game's
  tonemapper) for transcription errors; the map screen keeps the game's tonemapper (coarse dark tones).

## Answers (the user, 2026-11-22)

1. **Defaults:** the lowest point of our curve sits at 0 (no black floor: the level LUT's lift is not kept at black),
   tapering up from there (the derived toe, LUT tone 100%); the user adjusts clamp / toe from there. So: the floor
   adjustment defaults to 0 (a "Black floor" row, 0-100% of the area's, for taste or debugging), Level LUT tone 100%.
2. **ACES look:** kept if it costs nothing (useful for debugging), removed from the menu before release. With ACES
   selected our code must do no per-frame work beyond the setting check (today `ReplaceHdrGrading` reads the block
   before testing the look: reorder).
3. **Test patterns / meter:** not carried over (they served their purpose; the module can be ported back if needed).
4. **Our LUT:** today the game's target: 32^3 `A2B10G10R10` (PQ, 10 bits a channel), sampled by the game's tonemapper
   with trilinear filtering; no banding seen. Ours: 32^3 kept; `RGBA16F` (256 KB) instead of 10-bit, so the LUT adds no
   quantization of its own before the 10-bit PQ output (the only real one). 64^3 only if banding shows.
