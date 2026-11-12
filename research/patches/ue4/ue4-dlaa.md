---
id: ue4-dlaa
title: DLSS in place of UE4's TAA - DLAA at render scale 100 (U3), upscaling at the TAAU dispatch below it (U4); F9 switches live
area: ue4
status: diagnostic
platforms: windows
default_windows: off
default_deck: n/a
origin: ours
class: M
commits: 7b74b91, ef9d584, b0cef35, f25a373, 81a1fc9, c91cfd3, 517547f, 4fc3d16, 3819019, e51573f, d169e73
files: gpu/shim/gow3_settings.h, gpu/shim/gow3_settings.cpp, launcher/medievil_launcher.py, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_upscaler.h, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_upscaler.cpp, gpu/shim/gow3_overlay.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_motion.h, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_motion.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_dlss.h, gpu/shadps4/video_core/renderer_vulkan/vk_dlss.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h, gpu/shadps4/video_core/renderer_vulkan/vk_ue4_frame_map.h, gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp, gpu/shim/gow3_overlay.cpp
symbols: Ue4Upscaler, Ue4UpscalerBackend, StatsOn, NoteHook, NoteMotion, NoteFrame, HookTimer, DlssBackend, Ue4UpscalerKeys, ue4_upscaler, ue4_fsr_available, ue4_dlss_available, CycleJitterSign, Notice, DlssNotice, CameraJumpPixels, OnColorTarget, OnTaaDraw, OnTaauDispatch, Run, Running, GetPass, depth_inverted, ue4_dlss, target_rgba16f
switches: GOW3_UE4_DLSS, GOW3_UE4_UPSCALER, GOW3_UE4_HOOKS, GOW3_UPSCALER_STATS
tests: u3-dlaa, u3-dlaa-2, u3-dlaa-3, u3-dlaa-4, u4-dlss, u4-dlss-0102, u4-jitter-0102
docs: plan-ue4-motion-vectors.md#u3-design-dlaa-at-render-scale-100, plan-dlss.md#d0-2026-11-08-bridge-built-support-checked-offline
depends_on: ue4-motion-vectors
related: ue4-frame-map
---
## What it does

Step U3. With `GOW3_UE4_DLSS=1` (and `gow3_dlss.dll` + `nvngx_dlss.dll` beside the exe), at the first native TAA
draw (`fs e88c8fcbe08ea0b6`) DLSS runs as DLAA (quality 0, input = output = the View size) on:
- colour: TAA's colour input, the last single RGBA16F target drawn before TAA (the separate translucency composite),
  accepted only if the TAA draw samples its address;
- depth: the scene depth U2's motion pass read, `depth_inverted` (new field of `Dlss::FeatureDesc`);
- motion: U2's image (render pixels, previous minus current, scale 1);
- jitter: View `TemporalAAJitter.xy` as pixels (`x * W / 2`, `-y * H / 2`); HDR with auto exposure; sharpness 0.

Its output (an RGBA16F image of the port) is copied into TAA's colour target, and both TAA draws are skipped. Resets:
the first frame after UE4's TAA ran, a View frame number that is not the last + 1 (the game's own cut signal: run
`cut-evidence`, plan-ue4-motion-vectors.md "Camera cuts"), a new feature. No camera-motion rule. F9 switches DLAA and UE4's TAA live. RCAS is off while DLAA ran in the last 100 ms
(the user's decision). Under TAAU (the compute TAA) and without the DLLs nothing changes.

U4 (below render scale 100): at the TAAU dispatch (`cs b373f39908ae979f`, or any dispatch writing one image while
sampling the velocity target: UE4's permutations per upscale ratio; before `BindResources`) DLSS upscales from
the View size to the dispatch's output T# size (quality from the ratio, `Dlss::QualityForScale`), into the
dispatch's storage output (copied, or blitted when its format is not RGBA16F), and the dispatch is skipped
(`ResetBindings(true)`). The output image is found from the written image's T# (`FindImage`) only while DLSS is on.

**Setting (ninth commit):** ini `ue4_dlss` (default 0; `GOW3_UE4_DLSS=1` sets it), live: the in-game menu (Image tab,
"NVIDIA DLSS", disabled without DLLs / RTX), the launcher (Graphics, "NVIDIA DLSS"), F9 (saved). The hooks exist
whenever the DLSS DLLs are beside the exe; the per-draw observation runs only while the setting is on (or the U1
frame map / U2 view is). Periodic counter lines follow `GOW3_FPS_LOG=0`.

## Invariants

- U4: the TAAU output gets `FindTexture` with its storage binding (GPU-modified, aliases told) before DLSS writes
  it, as the skipped dispatch's storage write would.
- DLSS's copy target gets `FindRenderTarget` (not Undefined, GPU-modified, aliases told) before DLSS writes it,
  as the skipped draw's colour target would: without it the texture cache can refresh it from guest memory.
- Skipping a TAA draw returns from `DrawRecord` after `PrepareRenderState`, clearing `bind_prepared` and calling
  `ResetBindings(false)`, before `BindResources`: nothing of the draw is recorded.
- The second TAA draw of a frame is skipped only when DLSS ran for that View frame number.
- DLSS evaluation and the copy are recorded with `scheduler.Record` (in order, on the recording thread; everything
  the closure uses is captured by value). Only feature creation (first frame, size change) records directly:
  `CommandBuffer()` (the recording thread has run everything recorded, so it is not inside `Evaluate`), then
  `WaitSubmitted`, release, create. A failed evaluation on the recording thread sets `evaluate_failed`; the next
  TAA draw switches to UE4's TAA.
- `Dlss::FeatureDesc::depth_inverted` defaults to false: Bloodborne's upscaler path is unchanged.
- The NVIDIA DLLs never go in git. For testing, NVIDIA's development `nvngx_dlss.dll` replaces the release one in
  `out\` (the release copy kept as `out\nvngx_dlss.dll.release`): Ctrl+Alt+F12 debug overlays, Ctrl+Alt+F9 / F10
  jitter sign / swap.

## Interactions

- ue4-motion-vectors: `GOW3_UE4_DLSS` creates the motion pass too (view mode 0); its `GetPass` hands over the image
  and depth of the same View frame.
- RCAS (presenter): off while DLAA runs.

## Evidence

- Run `u3-dlaa` (PC, 2026-11-08, build `3c7f562`, 1.0 game, render scale 100, development `nvngx_dlss.dll`,
  `data/u3-dlaa.log`): DLAA features 3840x2160 and 3812x2160 (cutscene) created, ~45,000 DLAA frames, F9 switched
  DLAA / UE4 TAA live. The user: "DLAA looked much better than native TAA, significantly sharper without
  over-sharpening. With RCAS off, the difference was even clearer" (with the reset bug below still in). The four jitter settings (Ctrl+Alt+F9) looked the same at native, as in
  MGS2, where the sign was settled only when upscaling (+x +y, as passed here).
- Found in that run: **2,617 resets**, nearly all "camera jump" at 1.01-1.11 (the rule: a ClipToPrevClip element
  1.0 from identity): ordinary movement crossed it, so DLSS lost its history about every fifth frame while moving.
  Fix built (this card's second commit): reset when five screen points, at infinity and at 10 m, move more than a
  quarter of the view width in one frame. Also Ctrl+Alt+F9 switched DLAA off (our F9 ignored modifiers): F9, F10
  and F12 now only without Ctrl / Alt.
- Run `cut-evidence` (frame map): every camera cut, skip and level load moves the View frame number by more than
  one, fast pans never; the quarter-width camera rule was replaced by it (this card's third commit).
- Run `u3-dlaa-2` (build `53df9e6`, `data/u3-dlaa-2.log`): 10 resets in ~84,000 DLAA frames, each at a feature
  creation, the menu cut, a level load or an F9 switch. Found: after a level load, pans "judder ... like the camera
  was resetting over and over" until the user switched to UE4's TAA and back; frame pacing stayed flat (median 5.1 ms,
  p99 < 7.3 ms). Cause [inferred]: the skipped TAA draw's colour target never got a render target's texture cache
  bookkeeping, so a TAA output made at a load stayed "new" and the tonemap's read refreshed it from guest memory (an
  old frame) over DLSS's output. Fix built (fourth commit): `FindRenderTarget` on it before DLSS writes.
- Run `u3-dlaa-3` (build `ac81130`, `data/u3-dlaa-3.log`): DLAA on from the start, no F9 until in a level (the case
  the fix covers): no judder (the user: "it didn't come back"; not reproduced in the run before either, so seen
  again or not is not proof). 7 resets, all at the start, the cutscene, loads; then F9 switches. Pacing per 5 s:
  UE4 TAA median 4.22 ms (the 237 FPS cap), stddev 0.20; DLAA median 5.0-5.2 ms, stddev 0.15-0.27, p99 5.3-5.8.
- Evaluation moved onto the recording thread (fifth commit): the per-frame `CommandBuffer()` made the GPU thread wait
  for the recording thread once a frame (the user saw a small frame-time saw-tooth once with DLAA, not again).
  Run `u3-dlaa-4` (build `7bc6bc8`, `data/u3-dlaa-4.log`): no failed evaluation, 12 resets (feature creations, a load,
  F9 switches); DLAA median 4.9-5.2 ms per 5 s against 5.0-5.2 before, UE4 TAA 4.22 (the cap): the cost is DLSS's GPU
  work, not the sync.
- U4 built (sixth commit). Run `u4-dlss` (build `10a15cc`, `data/u4-dlss.log`; the launch before it died at boot,
  S1): DLAA at 100 as before; below it (the user lowered the render scale, probably under 80) no DLSS, no watermark,
  no log line, UE4's TAAU at the cap. Cause [inferred]: UE4 compiles a TAAU permutation per upscale ratio and only
  `cs b373f39908ae979f` (80-92%) was matched. Fix built (seventh commit): the TAAU dispatch also by role (writes one
  image, samples the velocity target), its hash remembered and logged once.
- Run `u4-dlss-0102` (build `1a47075`, **game 01.02**, `data/u4-dlss-0102.log`): the role rule found 01.02's TAAU
  shaders `cs 9c89e98d32a43b49` (cutscene, 3812x2160 out) and `cs 8996eb8d7eff6a69` (gameplay); DLSS ran ~159,000
  frames, Quality 2573x1448 -> 3840x2160 at 67%, Performance 1920x1080 -> 3840x2160 at 50%; 7 resets. The user:
  "DLSS looks much much better. 50% resolution looks like native." The whole run was below 100: 01.02's native TAA
  draws are not checked yet (the frame map counted none; its own TAA test is by hash only). The F9 log line says
  "DLAA" for "DLSS on" at any scale (misleading; now "DLSS on" on screen). NVIDIA's Ctrl+Alt+F9 jitter setting cannot
  be read back (not in the overlay or the watermark): F8 added (eighth commit), the jitter sign handed to DLSS cycled
  and shown on screen for 3 s, logged, DLSS history reset.
- Run `u4-jitter-0102` (build `dca48e2`, game 01.02, `data/u4-jitter-0102.log`): **01.02's native TAA is the same
  pair of draws** (`fs e88c8fcbe08ea0b6` / `fs e5588ed466d8f2f3`, frame map) and DLAA ran there (feature 3840x2160);
  TAAU at 50% Performance. **Jitter sign settled: +x +y** (F8 option 0, the default): the user went through the four
  options three times at 50% and judged +x +y best, as in MGS2. The user: "It works at 100%".
- The setting (ninth commit): launcher tests 54 OK (test_launcher_controls, test_deck_preset, test_graphics_options,
  test_prepare, test_upscaler_assets). The user's play setup 2026-11-09: release `nvngx_dlss.dll` back in `out\`,
  `ue4_dlss=1` in medievil.ini (backup `medievil.ini.bak-before-dlss-setting-20261109`), `extra-env.txt`
  `GOW3_FPS_LOG=0` (also silences test runs' FPS lines until removed). Not run.
- U4 before the fix (sixth commit): DLL: SDK 310.9.16 (DLSS 4.5 generation), preset 0 = driver default; DLAA costs
  ~0.85 ms a frame at 4K on the RTX 4090 (the user: DLSS 4.5 is heavy, ~40 FPS here).

## History and pitfalls

- F3 (`330a5e0`): with an upscaler on, the rasterizer's hook walked every texture and buffer of every draw:
  ~115-124 us a frame on stage B, worst frames 0.3-0.6 ms (T-F3, `GOW3_UPSCALER_STATS=1`), which cost the lows at
  237 FPS. Now the walk is only for compute dispatches and the TAA draws: ~52 us with the stats' own clock reads
  (T-F4), worst frames 4.7-5.2 ms instead of 5.2-5.8. Verified.
- Pitfall: `GOW3_UPSCALER_STATS=1` itself costs frame time (two clock reads per draw); in the graveyard with it and
  the FPS line on, the lows fell to 168 / 143 and came back with both off (T-F5). Lows measured with the stats on are
  pessimistic; compare only runs with the same stats.

- F1 (2026-11-12, branch `fsr`, plan-fsr.md step 1): the front end is generic, `Ue4Upscaler` in
  vk_ue4_upscaler.{h,cpp} (renamed from vk_ue4_dlss.*); DLSS is one backend (`DlssBackend`: context and evaluation
  only), FSR comes next. The hooks exist on every GPU (no longer only with the DLSS DLLs; `GOW3_UE4_HOOKS=0` removes
  them) and only work while an upscaler other than TAA is chosen. The setting is `ue4_upscaler` = taa / dlss / fsr
  (menu: one choice listing what the system runs; F9 cycles them; launcher: Graphics > Anti-aliasing / upscaler);
  `ue4_dlss=1` (ini, launcher) and `GOW3_UE4_DLSS=1` still mean dlss. Log lines now start "UE4 upscaler:".
  T-F1 (PC): DLSS through the new front end, F9, the old setting carried over; the user: works as before. Verified.

- 2.3.0 (`d169e73`, upstream `b396f85` cherry-picked; with `7127e10` / `6cec84f`, build_dlss.sh finds the
  CLANG64 headers and Visual Studio 2022 or 2026 by itself): the zip carries NVIDIA's DLSS SDK licence next to
  `gow3_dlss.dll` (`bin/NVIDIA-DLSS-LICENSE.txt`, which build_dlss.sh copied to `out/` all along but package.sh
  left out). Releases up to 2.2.1 shipped the bridge without it.

- First version. CameraCut is never set by this game (U1, U2), hence the frame number / camera jump resets.
