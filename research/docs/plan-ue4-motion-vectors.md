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

(Fill in at M0.)

## Risks

- PS4 UE4 build may differ (velocity format, compute TAA, View UB layout). M0 decides.
- `r.SetRes` live resolution changes and 1440p/1800p/4K modes change target sizes: identify by role, not size.
