---
id: hdr-output
title: HDR10 output through the game's own UE4 path (route A): log lines, GOW3_HDR_GAME reports an HDR display and sets the pak cvars, a 10-bit frame image, the UI composite LUT compiled on the spot
area: present
status: active
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits:
files: gpu/shadps4/core/libraries/videoout/video_out.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp, gpu/shim/core/emulator_settings.h, scripts/ue4_override.py, gpu/shadps4/video_core/renderer_vulkan/host_passes/pp_pass.cpp, gpu/shadps4/video_core/renderer_vulkan/host_passes/pp_pass.h, gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.h
symbols: sceVideoOutGetDeviceCapabilityInfo, sceVideoOutConfigureOutputMode_, sceVideoOutRegisterBuffers, FindPresentFormat, Swapchain::SetHDR, IsHdrGameForced, PostProcessingPass::HDR_FRAME_FORMAT, RecreateFrame, MustCompileSync
switches: GOW3_HDR, GOW3_HDR_GAME
tests:
docs: study-hdr.md, plan-work-queue.md
depends_on: particle-curve-sync-compile
related: ue4-frame-map, mausoleum-diagnostics, renderdoc-capture
checked: 2026-11-17
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

## Invariants

- The capability still needs `presenter->IsHDRSupported()` (the HDR10 pair offered **and** `GOW3_HDR`): the switch
  never tells the game HDR when the swapchain cannot show it.
- The pak cvars need both switches, matching the runtime. A pak with `r.AllowHDR=1` but no reported capability does
  nothing (PreInit sets both to 0 at SetByCode).
- The UI composite LUT is drawn once per HDR mode / output device change: any change that lets that draw be dropped
  (async path, a new skip in the draw path) brings the black UI back. `GOW3_DROP_LOG=1` names dropped draws.
- Not finished: the port's overlay is drawn in the SDR format into the PQ swapchain (RTSS likewise: its text reads
  as 10,000-nit red); FSR1 / RCAS's HDR mode squares PQ values; screenshots save raw PQ; no paper-white / peak /
  contrast control. Diagnostic runs only until those are done.

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
100 nits (H13, the user: "confirmed, that fixed it"). Blacks raised, contrast poor (the user, H3): open.

## History and pitfalls

- Black UI (H2-H13): not a cvar (`r.HDR.UI.Level=4` changed nothing, no eboot code writes it), not 3D LUTs in
  general (both tonemap LUTs worked), not a write in another shape (address watch): the builder draw was dropped by
  async compilation, first with all stages, then (H11) with only its vertex stage once the pixel program was listed.
- `r.HDR.UI.CompositeMode=0` shows the UI without the LUT but draws its 0-1 values straight into PQ (10,000-nit,
  oversaturated UI): only a diagnostic.
- RenderDoc runs: relative `GOW3_RENDERDOC` paths land under `out/` (renderdoccmd's working directory); the port's
  Vulkan object names are absent unless its markers are on, so find draws by event id and outputs.
