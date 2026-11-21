---
id: test-patterns
title: Test patterns for a colorimeter: a local socket asks for raw patches (written before presentation) or game patches (a scene value through the game's grading over the tonemapper's output); a meter script
area: tooling
status: diagnostic
platforms: all
default_windows: off
default_deck: off
origin: ours
class: M
commits:
files: gpu/shim/gow3_patterns.h, gpu/shim/gow3_patterns.cpp, gpu/shim/gow3gpu.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_hdr.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_hdr.h, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h, gpu/shadps4/video_core/host_shaders/ue4_pattern.frag, research/tools/meter/measure_patches.py
symbols: Gow3Patterns, RecordRawPatch, RunPatch, PatternAfterTonemap
switches: GOW3_PATTERN_SERVER, GOW3_PATTERN_PORT
tests:
docs: plan-meter-measurement.md, plan-hdr-sdr-match.md, study-hdr.md
depends_on: hdr-output
related:
---
## What it does

**Dev build only since 2026-11-20** (H35: release has the checks as compile-time constants; there
`GOW3_PATTERN_SERVER` only logs that it needs the development build; launcher: Advanced > Diagnostics > Development
build). `GOW3_PATTERN_SERVER=1` starts a thread listening on TCP 127.0.0.1:21977 (`GOW3_PATTERN_PORT`); `=pgen` also listens
on TCP 85 (all interfaces) and answers HCFR's PGenerator discovery on UDP 1977. Messages end in `\x02\r` (PGenerator
framing) or a newline; a patch is answered `OK` two presents after it changed.

- **raw** (`GOW3:RAW;r,g,b[;area[;black]]`, PGenerator `RGB=RECTANGLE` / `TESTTEMPLATERAMDISK`): normalized code values
  of the swapchain's encoding (SDR codes, HDR10 PQ codes) copied into the swapchain image after the frame's blit
  (`RecordRawPatch`, a buffer-to-image copy: exact bits for any 8-bit or 10-bit format), centred, `area` of the screen;
  optionally the rest cleared. Measures the port's output, Windows, the GPU and the display.
- **game** (`GOW3:GAME;x[,y,z][;area]`): after the game's tonemapper draw (SDR `3632d4c4faaa85d2`, HDR
  `12178a9314cb761e`) a scissored host draw over its output (`Ue4HdrGrading::RunPatch`, `ue4_pattern.frag`): the scene
  value graded with this frame's block (the Color tab applied; SDR the game's device 0 with its Brightness, HDR our
  SDR-look encoding) and written as the tonemapper writes (blue first). Then the UI composite and presentation.
- `research/tools/meter/measure_patches.py`: drives the game or a full-screen desktop window (the reference) with the
  same patches and timing, reads ArgyllCMS `spotread -e -x -O` (3.5.0 outside the repo), HCFR's low-light averaging,
  CSV in `research-data/meter`. Sequence `hdr-nearblack` (raw PQ, 0.005-30 nits) with `--write-correction` writes
  `hdr-black-correction.txt` beside medievil.ini for the near-black correction (card present/hdr-output).

## Invariants

- Off (variable unset): no thread, no socket; the presenter and the draw path test one relaxed atomic each.
- The game patch needs a grading block from this session (the LUT builder draws every frame) and, in HDR, the HDR look
  "SDR grading kept" (logged once otherwise).
- The raw patch is drawn on the frame-blit present path only; formats other than B8G8R8A8 / R8G8B8A8 / A2B10G10R10 /
  A2R10G10B10 are logged once and skipped.

## Interactions

- hdr-output: the game patch reuses `Ue4HdrGrading` (its pipeline layout, `ue4_grade.glsl`) and the grading block the
  HDR pass reads at the LUT builder draw; the hook now also runs in SDR while a game patch is shown.
- The overlay draws after the raw patch, so the menu and FPS counter can cover it: keep them closed while measuring.

## Evidence

Run 2026-11-17 / 18 with the user's ColorMunki Display (study-hdr.md H29): raw patches on black match a desktop
reference window within 1-3% from 30% up; game patches at neutral Brightness match the game's predicted grading within
1-4% in absolute cd/m2; the game's Brightness slider centred is a power of 0.758 on the graded colour. Raw patches over
the game's image read low (50-83 at white): the display's brightness limiting; "black" (the default in the script) fixes
it.

## History and pitfalls

- Planned from the HCFR source and ArgyllCMS docs (plan-meter-measurement.md): HCFR's 10% window, ~0.5 s settle,
  low-light averaging and grey levels are copied so the readings compare with HCFR's.
- The desktop reference is SDR only: the Windows HDR desktop maps SDR content, so it is not a PQ reference.
