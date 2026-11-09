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

Built 2026-11-08 (card [ue4-motion-vectors](../patches/ue4/ue4-motion-vectors.md)), not run.
`GOW3_UE4_MOTION_VIEW=1` (off by default); F10 cycles the views. What the user should see (the M2 / U2 check):

| View | Right | Wrong |
|---|---|---|
| 1-2, camera still | grey everywhere, moving enemies / Dan coloured with a blue tint | colour on static walls |
| 1-2, panning | smooth colour gradients by depth, no seams where the blue (game velocity) and the rest meet | a jump of colour at those edges (sign or scale of one source wrong) |
| 4, panning or moving | mostly black; bright only at edges that were hidden last frame, particles, UI | the whole picture bright (sign, scale or matrix order wrong), or only moving objects bright (velocity wrong) |
| 4 at render scale 80 | as at 100 | worse than 100: view size / sub-rectangle wrong |

## Risks

- PS4 UE4 build may differ (velocity format, compute TAA, View UB layout). M0 decides.
- `r.SetRes` live resolution changes and 1440p/1800p/4K modes change target sizes: identify by role, not size.
