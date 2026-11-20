---
id: hdr-output
title: HDR10 output through the game's own UE4 path (route A): log lines, GOW3_HDR_GAME reports an HDR display and sets the pak cvars, a 10-bit frame image, the UI composite LUT compiled on the spot, the overlay in HDR, UI brightness and colour grading in the menu
area: present
status: active
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits:
files: gpu/shadps4/video_core/renderer_vulkan/vk_ue4_hdr.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_hdr.h, gpu/shadps4/video_core/host_shaders/ue4_hdr_lut.vert, gpu/shadps4/video_core/host_shaders/ue4_hdr_lut.frag, gpu/shadps4/video_core/host_shaders/ue4_grade.glsl, gpu/shadps4/video_core/host_shaders/ue4_hdr_map.glsl, gpu/shadps4/video_core/host_shaders/ue4_ui_composite.frag, scripts/save_brightness.py, launcher/medievil_launcher.py, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shim/gow3_overlay.cpp, gpu/shim/gow3_overlay.h, gpu/shim/gow3_settings.cpp, gpu/shim/gow3_settings.h, gpu/shadps4/video_core/host_shaders/imgui_hdr.frag, gpu/patches/imgui/0001-vulkan-pipeline-for-another-format-and-fragment-shader.patch, src/runtime_extra.c, run.py, build.sh, gpu/shadps4/core/libraries/videoout/video_out.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp, gpu/shim/core/emulator_settings.h, scripts/ue4_override.py, gpu/shadps4/video_core/renderer_vulkan/host_passes/pp_pass.cpp, gpu/shadps4/video_core/renderer_vulkan/host_passes/pp_pass.h, gpu/shadps4/video_core/host_shaders/post_process.frag, gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.h
symbols: Ue4HdrGrading, ReplaceHdrGrading, ReplaceHdrComposite, RunComposite, PrepareHdrBlur, RunBackdrop, IsUiBlurFirstPass, set_neutral, GradeScene, HdrSceneToNits, HdrRollOff, LutDecodeHdrTexel, Ue4GradeOverride, ApplyUe4Grade, Ue4GradeTarget, HdrPipeline, ImGui_ImplVulkan_CreatePipelineForFormat, ColorTab, GradeAt, GradeKey, RequestHdrUi, RequestHdrCurve, hdr_ui_level, sceVideoOutGetDeviceCapabilityInfo, sceVideoOutConfigureOutputMode_, sceVideoOutRegisterBuffers, FindPresentFormat, Swapchain::SetHDR, IsHdrGameForced, PostProcessingPass::HDR_FRAME_FORMAT, RecreateFrame, MustCompileSync
switches: GOW3_HDR, GOW3_HDR_GAME, GOW3_UE4_GRADE, GOW3_UE4_LIVE_CVARS
tests:
docs: study-hdr.md, plan-work-queue.md
depends_on: particle-curve-sync-compile
related: ue4-frame-map, mausoleum-diagnostics, renderdoc-capture
checked: 2026-11-19
---
## What it does

Route A in [study-hdr.md](../../docs/study-hdr.md): the game's own UE4 HDR10 output (ACES, ST-2084, BT.2020).

- **Log lines (always on, start-up and resizes only):** `Swapchain: HDR10 surface offered|not offered, GOW3_HDR on|off;
  N formats: ...` (once); `VideoOut: device capability 0x.. (HDR10 surface usable|not usable, param.sfo HDR yes|no,
  GOW3_HDR_GAME on|off)` (first 4 calls); `VideoOut: output mode colorimetry N -> HDR on|off` (first 8; a refused
  value always); `VideoOut: register N buffers from S: <format> (0x........) WxH` (first 16); `Swapchain: HDR output
  on|off` on a switch.
- **`GOW3_HDR_GAME=1`** (with the launcher's "Allow HDR output", `GOW3_HDR=1`): the capability reports BT2020_PQ and
  `ConfigureOutputMode_` accepts colorimetry 12 although MediEvil's param.sfo has no HDR bit; `ue4_override.py` puts
  `r.AllowHDR=1` and `r.HDR.EnableHDROutput=1` in the pak.
- **10-bit frame image:** while the swapchain is HDR10, `RecreateFrame` makes the frame image `A2B10G10R10` and
  `pp_pass` draws it with a second pipeline for that format (before: the PQ frame was cut to 8 bits).
- **UI composite LUT:** UE4 builds the LUT its HDR UI composite (`fs c401ca35f9bceb8e`) reads with one draw at boot
  (`fs c401ca35ab81eb39`, vs `f7457b588f171402` / `f7457b588a024e47`). Async compilation dropped it, the LUT stayed
  zero and every UI element and movie was black. The program is in `MustCompileSync`, and a listed pixel program now
  compiles its whole draw on the spot ([particle-curve-sync-compile](../stage-b/particle-curve-sync-compile.md)).
- **`GOW3_UE4_GRADE="name=value;..."`** (diagnostic, read at start): replaces values of UE 4.21's combine LUT
  parameters (`fs a4152b3be93c832a` buffer 0, 144 dwords; map in study-hdr.md "LUT parameters") in the constant ring
  copy, or in a patched stream copy when the block is bound outside the ring; ring reuse is skipped for it. Names:
  `film.*`, `wb.temp` / `tint`, `{global,shadows,midtones,highlights}.{saturation,contrast,gamma,gain,offset}[.r|.g|.b|.w]`
  (a bare name sets `.w`), `shadows_max`, `highlights_min`, `blue_correction`, `expand_gamut`, `d<index>`. Log: the parsed
  values, the game's values at the first builder draw, counts at 600 / 6,000 draws (ring / stream / not patched).
  In HDR the grading acts (H17: contrast 1.3, floor 0.22 -> 0.08 nits), the film curve does not (H16).
- **The overlay in HDR:** while the swapchain is HDR10 the port's menu, FPS counter and notices draw with a second ImGui
  pipeline for `A2B10G10R10` (`ImGui_ImplVulkan_CreatePipelineForFormat`, a local patch to the upstream submodule in
  `gpu/patches/imgui`, applied by build.sh) whose `imgui_hdr.frag` decodes gamma 2.2, converts BT.709 to BT.2020 and
  encodes PQ with white at the UI brightness (its one specialization constant, written into the SPIR-V; one pipeline
  per value, kept). Log: `Overlay: HDR10 pipeline ready (PQ, white N nits)`.
- **UI brightness** (Display > HDR, shown while HDR10 is on; medievil.ini `hdr_ui_nits`, 50-400, default 100): the
  game's UI through `r.HDR.UI.Level` = nits / 100 (H19: linear, 1.0 measures ~100-105 nits), live through the
  runtime's cvar table and at boot through the pak (`ue4_override.hdr_ui_level`), and the overlay's white. One setting
  for both: OLED owners keep UI dim against burn-in (the user, 2026-11-17).
- **HDR curve** (Display > HDR; `hdr_curve` 1000 / 2000): `r.HDR.Display.OutputDevice` 3 / 4, set live only (the
  engine's HDR sink sets it at device-profile priority, above the pak) and only when the game started in HDR.
- **Color tab** (medievil.ini `grade_*`; `Gow3Settings::GradeAt`): white balance, then saturation / contrast / gamma /
  gain / offset for global, shadows, mid-tones and highlights, the shadow / highlight bounds, blue correction and gamut
  expansion, and Reset. Relative to the game's values, so each area's own grading stays: multipliers (1 = the game's)
  and additions (0). Per-channel values are ini only (`grade_<range>_<field>_r|_g|_b`). Applied in `ApplyUe4Grade`
  after `GOW3_UE4_GRADE`, in SDR and HDR (the same builder program runs in both). Neutral: the draw binds as before.
- **Not in the menu, settable by us:** the film curve and any dword through `GOW3_UE4_GRADE` (absolute); boot cvars
  through `GOW3_UE4_CVARS`; `GOW3_UE4_LIVE_CVARS="name=value;..."` sets cvars of the runtime's live table once the game
  reaches its menu (for the ones the pak cannot outrank). The table now has `r.HDR.UI.Level`,
  `r.HDR.Display.OutputDevice` / `ColorGamut`, `r.HDR.UI.CompositeMode`, `r.Tonemapper.Sharpen` / `GrainQuantization`
  / `Quality`, `r.TonemapperGamma`, `r.TonemapperFilm`, `r.Gamma`, `r.Color.Min` / `Mid` / `Max` (sites checked against
  the 01.02 eboot: each loads its own name; 01.00 from the same scan, unchecked). A Dev tab (dev build only) can use
  them later.
- **Color panel** ("Adjust while playing" in the Color tab): the Color tab's rows in a small copy of the game's Options
  frame in the bottom-right corner (30% of the width, 62% of the height), no dimming, rows 1.35x the frame's scale;
  Circle goes back to the full menu, L1 / R1 do nothing there. The full menu and the panel share `StyledBody` (the
  scrolled list).
- **HDR look "SDR grading kept"** (Display > HDR > HDR look, default; plan-hdr-sdr-match.md step 1): while the game
  outputs HDR10, its grading LUT draw (`fs a4152b3be93c832a`) is replaced (`Rasterizer::ReplaceHdrGrading`, before
  BindResources, like DLSS's TAA replacement) by `Ue4HdrGrading`: a layered draw (3 vertices x 32 instances,
  `gl_Layer`) into the same 32-slice target (`FindRenderTarget`: marked written), from the game's parameter block with
  the Color tab applied. `ue4_grade.glsl` is the game's chain (white balance, expand gamut, ColorCorrectAll, blue
  correction, film curve, polynomial, scale / overlay), ported from the translated shader and equal to it on all 32^3
  texels offline (`research-data/hdr-ui-20261117/port`, not in git). Output (`ue4_hdr_map.glsl`, shared with the
  meter's game patches): the SDR-graded colour, sRGB-encoded, decoded at Gamma (2.2 default; Off = sRGB), x Game
  brightness (203); above a knee at Y(U) = 0.5 the luminance continues the SDR look's slope at the knee (Highlights,
  plan-hdr-sdr-match.md step 2), then a luminance roll-off (identity to half the peak, extended Reinhard reaching the
  peak at 4x); BT.2020 PQ / 1.05. The game's Brightness (dword 5, InverseGamma.y) is forced to 1 in HDR (H26).
  Settings: `hdr_look`, `hdr_game_nits`, `hdr_gamma`, `hdr_peak_nits` (default 1000), `hdr_highlights` (default 100).
- **UI composite** (same HDR look; plan-hdr-sdr-match.md step 3): the game's composite draw (`fs c401ca35f9bceb8e`) is
  replaced (`Rasterizer::ReplaceHdrComposite`, `Ue4HdrGrading::RunComposite`, `ue4_ui_composite.frag`): scene and UI
  read through the game's own views (`bgra`: true RGB), the UI decoded at Gamma at UI brightness (`hdr_ui_nits`, live;
  `r.HDR.UI.Level` no longer matters here), BT.709 -> 2020, blended over the scene in the SDR signal, written `.bgr`.
  Movies go the same way. Log once: `UE4 HDR UI composite: ours (...)`, or `the game's kept: <why>`.
- **Background blur backdrop** (same HDR look): UE's menu blur samples what is drawn so far, in HDR the empty UI
  image (black menus). Before a blur first pass (`IsUiBlurFirstPass`: `8c6fbe78cd6698bb` full size,
  `8c6fbe7824203398` half-size downsample) that samples the last composite's UI image, `PrepareHdrBlur` has
  `RunBackdrop` write the UI so far over the scene in the SDR signal (4K RGBA8, true RGB) and that draw alone samples
  it. Log once: `background blur reads our backdrop`.
- **Game brightness** (launcher, Display): "Set to neutral" writes the game's default Brightness (0.3778, display
  gamma 2.2) into every save slot (`scripts/save_brightness.py`; saves backed up first, file times kept). In HDR the
  picture ignores the slider but Unreal's UI shaders apply it.
- **Near-black correction** (`hdr_black_correction`, Display > HDR > Near-black correction; off by default): the
  presenter reads `hdr-black-correction.txt` beside medievil.ini (pairs `requested_nits measured_nits` from the meter
  script's `hdr-nearblack --write-correction`; re-read once a second), inverts the measured response below its last
  point (at most 24 points, blended to identity over the top 15%) and passes it in the post-process push constants
  (128 bytes); `post_process.frag` applies it per channel in the HDR branch only. Logs one line per load
  ("HDR near-black correction: N points ...").
- **Release build** (`GOW3_DEV_TOOLS=0`): `GOW3_UE4_GRADE`, the grading coverage counts, `GOW3_UE4_LIVE_CVARS`, the
  frame map (with its shader traces), the image watch and the dropped-draw log are compiled out (strings absent from
  the release exe, present in the dev one). The HDR post-process pipeline is built only with `GOW3_HDR=1`. Kept: the
  capped start-up HDR log lines and the one "colour grading on" line.

## Invariants

- The UI composite reads the scene and UI through the game's views and writes `.bgr` (LANDMINES.md 28): through
  identity views the colour maths runs on swapped channels (greens turned mint, H32).
- The blur backdrop serves one draw: `hdr_blur_view` is cleared right after that draw's `BindResources`, the draw's
  textures bind on the draw thread (`TexturesBindableOnHelper`), and the kept texture set records the game's view.
  The backdrop is written in true RGB and sampled through an identity view.
- `save_brightness.set_neutral` must back up first and keep each save's file times (the game picks its slot by file
  time, study-save-slots.md).

- The capability still needs `presenter->IsHDRSupported()` (the HDR10 pair offered **and** `GOW3_HDR`): the switch
  never tells the game HDR when the swapchain cannot show it.
- The pak cvars need both switches, matching the runtime. A pak with `r.AllowHDR=1` but no reported capability does
  nothing (PreInit sets both to 0 at SetByCode).
- The UI composite LUT is drawn once per HDR mode / output device change: any change that lets that draw be dropped
  (async path, a new skip in the draw path) brings the black UI back. `GOW3_DROP_LOG=1` names dropped draws.
- `GOW3_UE4_GRADE` keys on the builder's program hash and on dword offsets of UE 4.21's parameter layout: another game
  build or engine version needs the map checked again (a RenderDoc dump of the builder's buffer 0).
- `hdr_curve` must never reach the runtime in SDR: OutputDevice 3 / 4 on an SDR swapchain would draw PQ as sRGB
  (`RequestHdrCurve` checks `HdrGame`).
- `Ue4HdrGrading::Wanted` reads dword 138 as an integer (the game tests `data[138] >= 3u`; a float read never matched,
  H24). The pass only replaces a 32^3 target; anything else is logged once and keeps the game's LUT.
- Rows that share a name (the four ranges' Saturation ... Offset) need their own `ImGui::PushID` scope: a styled row's
  ID is its name, and shared IDs move every copy at once (H20).
- Grading is relative: replacing the game's values instead would flatten every area's own post-process grading.
- The post-process push block is exactly 128 bytes (`static_assert` in `pp_pass.h`, the Vulkan minimum): more
  correction points need a buffer, not a bigger push block. The correction runs only when `black_count > 1` and the
  frame is HDR; SDR frames never touch it.
- The correction file describes one screen at one moment: it goes stale as the panel drifts (re-measure after pixel
  refreshes) and must be off while a system-wide profile already corrects the same thing.
- Not finished: RTSS's text reads as 10,000-nit red (its own drawing); FSR1 / RCAS's HDR mode squares PQ values;
  screenshots save raw PQ; no peak roll-off. Diagnostic runs only until those are done.

## Interactions

- particle-curve-sync-compile: the UI LUT fix lives in its `MustCompileSync` list and its `sync_all` rule.
- ue4-frame-map, mausoleum-diagnostics, renderdoc-capture: the tools that traced the black UI (H6-H12); their new
  switches (`GOW3_UE4_FRAME_MAP_SHADERS`, `GOW3_IMAGE_WATCH_ADDR`, `GOW3_DROP_LOG`) are in those cards.
- Off by default (`GOW3_HDR_GAME` unset): with the checkbox alone the game still sees an SDR display; only the log
  lines, the always-built HDR post-process pipeline and the sync-compiled LUT program (never drawn in SDR) remain.

## Evidence

Runs H1-H13 in [study-hdr.md](../../docs/study-hdr.md#runs-2026-11-17-release-build-of-the-uncommitted-step-1--2-code-4k-windows-hdr-on)
(2026-11-17): the switch works (game buffers `0x88740000`, colorimetry 12). Black UI traced through the frame map,
a shader dump, the image watch by address, a dropped-draw log and a RenderDoc capture (`research-data/hdr-ui-20261117`,
LUT all zero) to the dropped LUT draw; with the whole-draw sync compile the UI shows with the right colours, about
100 nits (H13, the user: "confirmed, that fixed it"). Blacks raised, contrast poor (the user, H3): open. H18: the overlay
in HDR, the user: "the menus look good, they're no longer over-saturated". H19: `r.HDR.UI.Level=0.5` halved the HUD
(health bar p99 105 -> 52 nits) and left the scene and the overlay as they were. H20 / H21: the Color tab and the HDR
section, every slider working once each range got its own ImGui ID scope (the user, 2026-11-17). H22: the Color panel
and the release build without the diagnostics, "everything functions as expected". H29 / H30: the meter runs (card
tooling/test-patterns); the near-black correction from the `20261118-111751` hdr-nearblack run, game-grey at 203 nits /
2.2 on 2026-11-18 11:30: scene 0.02 / 0.05 / 0.1 / 0.18 / 0.3 at 1.05 / 1.03 / 1.01 / 1.04 / 1.00 of target (the user:
"looks a lot better ... fairly close to my SDR + brightness slider turned down"); highlights read 5-15% low in that run,
not explained by the raw PQ run after (0.98 at 203): not seen again in H31. H31 (2026-11-19): Highlights 0 = step 1
within 1-4%; Highlights 100 identical up to the knee, smooth above it, flat at the peak (421 nits measured at Peak 450,
the screen's 0.94 tracking); the user: "it all looks good to me". H32 (2026-11-19): the UI composite; movies and menu
colours correct after the channel fix (the user); the black blurred-menu background traced to the blur reading the
empty HDR UI image. H33 (2026-11-20): the backdrop; the weapons tab fixed (the user); the map popups' half-size
first pass found in RenderDoc and added; the user: both popups work.

## History and pitfalls

- Black UI (H2-H13): not a cvar (`r.HDR.UI.Level=4` changed nothing, no eboot code writes it), not 3D LUTs in
  general (both tonemap LUTs worked), not a write in another shape (address watch): the builder draw was dropped by
  async compilation, first with all stages, then (H11) with only its vertex stage once the pixel program was listed.
- `r.HDR.UI.CompositeMode=0` shows the UI without the LUT but draws its 0-1 values straight into PQ (10,000-nit,
  oversaturated UI): only a diagnostic.
- RenderDoc runs: relative `GOW3_RENDERDOC` paths land under `out/` (renderdoccmd's working directory); the port's
  Vulkan object names are absent unless its markers are on, so find draws by event id and outputs.
