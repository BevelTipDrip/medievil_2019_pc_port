# Plan: real UE4 motion vectors, depth, jitter and camera for the temporal upscalers

Status: researched 2026-10-18, nothing built. Parent: [plan-roadmap.md](plan-roadmap.md).
Read [upscaler-reference.md](upscaler-reference.md) before changing anything here.

## What is known today (read, 2026-10-18)

- MediEvil sets `GOW3_UPSCALER=none` (`scripts/game_profiles.py:9-16`, `run.py:177-180`): FSR 3.1, FSR 4, DLSS SR and
  the port's TAA are all off. `none` turns off `TemporalUpscaler::enabled` (`vk_temporal_upscaler.cpp:175-177`) and
  `CameraMotion::for_upscaler` (`vk_camera_motion.cpp:56-59`).
- The only temporal upscaling running is UE4's own `r.TemporalAA.Upsampling` (override pak,
  `scripts/ue4_override.py:27-30, 317-319`), which uses the game's velocity internally; the port never sees it.
- FSR 1 + RCAS (spatial) runs on the display image (`vk_presenter.cpp:499-513`).
- The dormant temporal path is **Bloodborne's**, unchanged (matches `bloodborne_pc/docs/upscaler.md:86-116`):
  - trigger compute hash `0x9a9cf8a9` (`vk_temporal_upscaler.h:197`), UI vertex shader hash `0x34e8a281` (`.h:215`);
  - scene depth = pass with 5+ colour targets (`vk_rasterizer.cpp:265-270`); scene colour = full-size RGBA16F
    (`:274-295`);
  - camera = 864-byte constant buffer with `data[0]==3000` (`vk_camera_motion.cpp:221-241`);
  - motion vectors reconstructed from depth + camera, object motion from replayed skinning with Bloodborne buffer
    sizes (`motion_history.h:44-52`);
  - port-injected Halton jitter (`vk_temporal_upscaler.cpp:346-356`), `cameraFar = 3000` (`:1137, 1732`),
    DLSS `depth_inverted = 0` (`vk_dlss.cpp:207-209`).
- None of that fits UE4 (general UE4 behaviour, **not yet checked against this game**): reversed-Z with infinite far
  plane; R16G16 velocity written only for movable/skinned geometry (0 = reconstruct from camera); the game's own TAA
  jitter in the View uniform buffer (`TemporalAAJitter`), so port jitter would double-jitter.

## Design

1. **Map one frame** with `GOW3_CAPTURE_TRIGGER`: find the depth target, the velocity target, the TAA pass (pixel or
   compute; its shader hash), the post-process chain and the first UI draw, and the View uniform buffer
   (projection, `ClipToPrevClip`, `TemporalAAJitter`, near plane, camera-cut flag). Record it in a frame-map section
   below with shader hashes and formats.
2. **Hook at the TAA pass:** skip it and write the upscaler output into TAA's output, so `r.TemporalAA.Upsampling` /
   `r.ScreenPercentage` keep sizing everything and the hook stays before post-processing and UI.
3. **Motion vectors:** decode velocity to pixels (current to previous). Where velocity is 0, reconstruct from depth and
   `ClipToPrevClip` (adapt the existing camera-motion compute pass; the MGS2 reprojection math in
   `MGS2-Lab\...\lab_motion_vectors.cpp:25-150` is the reference). Drop the skinning replay for MediEvil.
4. **Jitter:** use the game's `TemporalAAJitter`; port jitter off. Check sign and units.
5. **Depth:** inverted + infinite far for FFX; `depth_inverted = 1` for DLSS; remove `cameraFar = 3000`.
6. **Exposure:** keep auto-exposure first; later feed UE4's eye-adaptation (1x1) texture.
7. **Reset:** camera cut flag or a jump in `ClipToPrevClip`; level loads, menus/movies, `r.SetRes` changes.
8. **Reactive mask:** skip first; later from translucency passes.
9. Put the identifiers in a MediEvil game profile, not over the Bloodborne ones.

## Stages (one change per launch)

| Stage | What | Verify |
|---|---|---|
| M0 | Frame map (capture only, no behaviour change) | Frame map section filled in, hashes listed |
| M1 | Identify passes at run time, log only | Log shows TAA/velocity/depth found every frame, in menus too |
| M2 | Debug view of decoded + reconstructed motion vectors | User sees static walls ~0, moving enemies coloured |
| M3 | FSR 3 upscaling replaces UE4 TAA (switch) | User compares against UE4 TAAU at the same scale |
| M4 | DLSS SR on the same inputs | See plan-dlss.md |

## Frame map

- Native (render scale 100): [study-mgs2-temporal-for-ue4.md](study-mgs2-temporal-for-ue4.md) sections 3.2-3.3 (capture
  A) and [study-taau-frame-map.md](study-taau-frame-map.md) (a second native capture, 2026-11-08): the TAA pixel
  shaders `fs 0xe88c8fcbe08ea0b6` (normal) / `fs 0xe5588ed466d8f2f3` (responsive), tonemap `fs 0x3632d4c4faaa85d2`,
  first UI `fs 0x4fe20af683865713`, same SPIR-V in both captures; View UB offsets hold.
- TAAU (render scale below 100): **no capture yet**; from the shader cache, most likely one compute dispatch
  `cs 0xb373f39908ae979f` with the responsive split inside the shader (inferred). A TAAU capture (one launch) settles it.

## U1: run-time frame map, log only

Built 2026-11-08 (card [ue4-frame-map](../patches/tooling/ue4-frame-map.md)), not run. `GOW3_UE4_FRAME_MAP=1`
(off by default) logs `UE4 frame map:` lines: the frame's velocity pass, TAA (draws or the TAAU compute dispatch),
the View buffer at TAA, the display buffer's draws (tonemap, first UI), and every shader that samples the velocity
target. Nothing in the frame changes.

What a run should show (the M1 / U1 check):

| Run | Expected | Answers |
|---|---|---|
| Native (render scale 100), gameplay | every frame: velocity, TAA draws x2, View 3840x2160, tonemap, UI; jitter values 8 | the hook of U3 holds at run time, not only in two captures |
| Render scale 80 (TAAU), gameplay | TAA compute `cs b373f39908ae979f`, its groups and output size; View size below the output; mip bias below 0; jitter values (more than 8?) | study-taau-frame-map.md open question 1 without a capture |
| Menus, pause, a movie, a load | which roles stay; camera cuts | open question 4 |
| Low quality at 100 | draws or compute | open question 3 |

### U1 run (2026-11-08, PC, `data/u1-frame-map.log`)

Build `ce9a833`, game **1.0** (`run-game.ps1`'s folder), 4K output, console quality; the user: native gameplay, render
scale 80, pause, main menu, a level load. 210 summaries (63,000 frames). Labels: checked = in this log.

- **Native: the U3 hook holds.** TAA is the two draws `fs e88c8fcbe08ea0b6` / `fs e5588ed466d8f2f3` in every frame
  with a 3D view (gameplay, intro cutscene, the main menu's 3D background), output 3840x2160, reading velocity and
  depth; 8 jitter values in every native summary (checked). Same hashes in the 1.0 game as in the captures.
- **TAAU is the compute dispatch** `cs b373f39908ae979f` (study open question 1, checked without a capture):
  480x270 groups of 8x8 = the 3840x2160 output, View 3072x1728 at 80%, mip bias -0.62 (= log2(0.8) - 0.3; 92%:
  -0.42, 86%: -0.52), 12 jitter values in a steady 300-frame window (so `StateFrameIndexMod8` does not index the
  table: read the jitter itself). The draws never ran under TAAU, and `cs a080f599034de78e` never ran at all.
- **Live scale change goes through 92% and 86%** with the View smaller than the buffer for a frame (`View 3533x1988 in
  3840x2160`) and two velocity sizes alternating: render size and sub-rectangle must come from the same frame's View,
  never from image sizes (checked).
- **Sub-rectangle at native too:** the intro cutscene renders `View 3812x2160 in 3840x2160` (checked).
- **CameraCut is never set** (0 in all 63,000 frames, level loads included), while the View frame number restarts at
  a load (4 gaps; frame 3 after a load, camera delta 5.17). U3's reset: frame number not +1, or a large
  ClipToPrevClip change; offset 2148 is either unused by this game or not CameraCut [inferred].
- **Frames without velocity draws** while TAA still samples the velocity target's address (whole summaries in the
  intro and menus: `velocity 0, TAA 300`). What that image holds then (a clear without draws, or last frame's) is
  open: U2 must check before trusting it.
- **Tonemap is not one shader:** the main menu's first display draw is `fs 829b4db85567b8f4`, not
  `fs 3632d4c4faaa85d2`. Cutscenes draw `fs 2905d487e88259ad`, `fs 2809e28fee22731b` into the display buffer after the
  tonemap and no `fs 4fe20af683865713`; gameplay sometimes `fs b1ee65f9237db63f` before the UI. The HUD-less point
  for frame generation needs a role rule, not "before the first UI shader" (open, for FG).
- **Other velocity readers:** `fs 8e275640edaa7846` (intro cutscene, with TAA), `fs 316cb5d46f2c2e12` (same suffix as
  the SSR-like `fs f8cfb0056f2c2e12`) and `fs bf9891cbd2f66a01` in a few frames with velocity but no TAA (around
  loads). Roles not known.
- Not shown by this run: low quality at 100% (open question 3), a movie.
- **The run hung at "quit to main menu"** (render scale 80): a recorder-thread read fault deadlock, not the frame map
  (stacks: `data/u1-hang-recorder-fault-20261108.txt`; plan-work-queue.md "Found on the way").

## U2: motion vectors and their debug view

Built 2026-11-08 (card [ue4-motion-vectors](../patches/ue4/ue4-motion-vectors.md)). Run `u2-motion` (render scale 80):
the vectors are right by the panning measurements (card Evidence); the camera cut flag stayed 0 through the user's
intro skip at the title (a real cut), so resets must come from the frame number or a camera jump. Debug colours were
red / blue swapped and menus had no pass: fixed in `461f249`, run `u2-check`: tint right, title screen covered,
mode 4 near black while moving. U2 done; the finer check is DLSS's own debug overlay in U3.
`GOW3_UE4_MOTION_VIEW=1` (off by default); F10 cycles the views. What the user should see (the M2 / U2 check):

| View | Right | Wrong |
|---|---|---|
| 1-2, camera still | grey everywhere, moving enemies / Dan coloured with a blue tint | colour on static walls |
| 1-2, panning | smooth colour gradients by depth, no seams where the blue (game velocity) and the rest meet | a jump of colour at those edges (sign or scale of one source wrong) |
| 4, panning or moving | mostly black; bright only at edges that were hidden last frame, particles, UI | the whole picture bright (sign, scale or matrix order wrong), or only moving objects bright (velocity wrong) |
| 4 at render scale 80 | as at 100 | worse than 100: view size / sub-rectangle wrong |

## U3 design: DLAA at render scale 100

Run `u3-dlaa` (2026-11-08): DLAA ran at 3840x2160 and in the 3812x2160 cutscene; the user: "much better than native
TAA, significantly sharper without over-sharpening". Resets misfired in ordinary movement (fix `ef9d584`, not run);
next: cut detection by **coverage** (the share of a screen grid, at several depths, whose ClipToPrevClip position is
in front of the camera and on screen; reset below ~10-20%), instant on the CPU, with the image difference of
U2's mode 4 counted on the GPU and logged a few frames late as its check (the user's idea, 2026-11-08: a true cut
is ~100% different; mouse flicks must not be taken for cuts).

- **Switch:** `GOW3_UE4_DLSS=1` (off by default), live on / off with a hotkey so the user compares DLAA and UE4's TAA
  on the same frame. Needs `gow3_dlss.dll` + `nvngx_dlss.dll` beside the exe (D0). Native only: under TAAU (the
  compute TAA) nothing changes; U4 handles that.
- **Hook:** at the first TAA draw (`fs e88c8fcbe08ea0b6`), after the U2 motion pass, record DLSS and skip both TAA
  draws. DLSS writes an RGBA16F image of the port, copied into TAA's colour target (TAA's output is next frame's
  history, the SSR-like pass's and bloom / tonemap's input: all keep reading the same image).
- **Inputs:** colour = TAA draw 1's colour input (the separate translucency composite's target: the last RGBA16F
  colour target drawn before TAA, accepted only if TAA samples its address); depth = the velocity pass's D32S8
  (depth aspect); motion = U2's image (render pixels, previous minus current, MV scale 1); layouts General.
- **Feature:** quality DLAA (input = output = View size), `hdr = 1` (linear HDR, auto exposure; PreExposure is 1),
  **`depth_inverted = 1`** (a new field in `Dlss::FeatureDesc`; Bloodborne's path keeps 0), sharpness 0 and RCAS off
  while DLSS runs (the user's decision).
- **Jitter:** the View's `TemporalAAJitter.xy` in render pixels (`x * W / 2`, `-y * H / 2`); the sign NGX wants is
  checked with the development DLL's hotkeys (below), not a switch of ours.
- **Test runs use NVIDIA's development `nvngx_dlss.dll`** (`tools\DLSS\lib\Windows_x86_64\dev`, copied over the
  release one in `out\` for the run, never in git). DLSS programming guide sections 5 and 8: Ctrl+Alt+F12 cycles
  debug overlays of DLSS's inputs (motion vectors, depth, jitter), Ctrl+Alt+F11 window / full screen, Ctrl+Alt+F9
  negates / scales the jitter (configs), Ctrl+Alt+F10 swaps jitter x / y, Ctrl+Alt+F6 accumulation / jitter debug,
  Ctrl+Alt+Y auto exposure. The production DLL has no overlay. For frame generation, the dev `nvngx_dlssg.dll`
  (the visualizer the user used for MGS2).
- **Reset:** the View frame number not last + 1, a large ClipToPrevClip change (the intro skip and level starts
  show 1.4-5.2 against ~0.5 when moving: threshold to choose from U1 / U2 data), feature (re)creation, View size
  change. CameraCut is never set by this game (U1, U2).
- **Sub-rectangle:** View size can be below the image size (cutscenes 3812x2160): DLSS input and output size from the
  View, offset (0, 0).
- **Verify:** the user compares DLAA against UE4 TAA (hotkey) standing still, panning and in combat: ghosting on
  Dan and enemies, shimmer on fences and foliage, the HUD unchanged.

## U4 design: DLSS below native, at the TAAU compute pass

From U1 (checked): below render scale 100 UE4's TAA is one dispatch of `cs b373f39908ae979f` (8x8 groups over the
**output** size, e.g. 480x270 for 3840x2160), reading the scene colour, velocity, depth, the stencil and last frame's
output (output size), writing one storage image at the output size. The View's `ViewSizeAndInvSize` is the render
size (3072x1728 at 80%), its jitter (12 values at 80%) in render-size NDC, mip bias already negative (-0.62).

- **Same switch** `GOW3_UE4_DLSS=1` and F9: DLAA at 100, DLSS upscaling below it; DLSS quality picked from the
  output / render ratio (`Dlss::QualityForScale`), the feature recreated when the live render scale changes.
- **Hook:** `DispatchRecord`, before `BindResources`: the TAAU dispatch is skipped (`ResetBindings(true)`) after DLSS
  is recorded; DLSS writes the dispatch's storage output (copied from the port's image, output size).
- **Inputs:** colour = the last RGBA16F single-target draw before the dispatch that the dispatch samples (as U3;
  history is output-sized and written by the dispatch, so never taken for it); depth = U2's; motion = U2's at render
  size; input rectangle = the View size from (0, 0) (sub-rectangle during a live scale change and in cutscenes).
- **Output:** the written image's T# -> `FindImage` + `FindTexture` with a storage binding (`GpuModified`,
  `UpdateImage`, `PublishAliasWrite`): the bookkeeping the skipped dispatch would have given it (U3's lesson).
  Output rectangle: that image's size (3840x2160; 3812x2160 in cutscenes).
- **Jitter sign: settled +x +y** (run `u4-jitter-0102`, 01.02, 50%: the user cycled the port's F8 options; NVIDIA's
  Ctrl+Alt+F9 setting cannot be read back). Same as MGS2.
- **Results (01.02):** DLSS Quality at 67% and Performance at 50% ("50% resolution looks like native"), DLAA at 100;
  01.02's TAAU shaders `cs 9c89e98d32a43b49` / `cs 8996eb8d7eff6a69` found by role, native TAA draws as 01.00.
- **Resets:** as U3 (View frame number, first frame, feature creation).
- **Presets (later; the user, 2026-11-08):** a lighter model for DLAA, DLSS 4 (preset K) for Quality, DLSS 4.5
  (L / M) for Balanced and Performance. Now preset 0 (driver default) everywhere; the bridge's `preset` field exists.
- **Verify:** the user compares DLSS against UE4's TAAU at the same render scale (F9), at 80 and lower; the
  development overlay's motion vector / depth views line up with the picture.

## Camera cuts: the game's own signal (run `cut-evidence`, 2026-11-08)

`data/cut-evidence.log` (frame map with cut evidence, build `22e9a9f`, render scale 100). The user: the title's intro
skip, an in-game cutscene with several camera cuts (played twice: before and after a level reload), fast pans for
about two minutes. **Every cut moves the View frame number by more than one; nothing else does** (checked):

| Event | View frame number | Camera move that frame |
|---|---|---|
| title -> intro swoop; level start; the user's reload | restarts (3499 -> 5, 3345 -> 3, 35678 -> 4) | 0 px |
| intro skip | +3 | 0 px |
| cutscene cuts (6, one hidden by the line cap) | +2 each (one +25) | 0-3 px |
| the same cutscene after the reload | the same cuts at nearly the same numbers (2531, 7433, 7754, 8035, 9081 vs 2493, 7390, 7716, 7997, 9050) | 0-2 px |
| fast pans, up to 101 px a frame at ~237 FPS | never | |

UE4's TAA never stopped reading last frame's output (0 history drops), so TAA's inputs carry no cut signal. TAA
parameters 26 / 27 change every moving frame and reset at cuts (role unknown). View.CameraCut (offset 2148) stayed 0.
Rule for DLSS: reset when the View frame number is not the last one + 1 (cuts, skips, loads), the first frame after
UE4's TAA ran, a new feature; no camera-motion rule (a mouse flick does not move the frame number). A death /
respawn was not tested (it would cost the user's save).

## Risks

- PS4 UE4 build may differ (velocity format, compute TAA, View UB layout). M0 decides.
- `r.SetRes` live resolution changes and 1440p/1800p/4K modes change target sizes: identify by role, not size.
