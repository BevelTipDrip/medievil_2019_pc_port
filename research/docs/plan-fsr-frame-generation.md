# Plan: FSR 3 frame generation

Status: planned 2026-10-18, nothing built. Needs [plan-ue4-motion-vectors.md](plan-ue4-motion-vectors.md) M2 first.
Parent: [plan-roadmap.md](plan-roadmap.md). Read [upscaler-reference.md](upscaler-reference.md) first.

## What is known today (read, 2026-10-18)

- No frame generation code in this port or the Bloodborne port (Bloodborne only has a roadmap entry,
  `bloodborne_pc/docs/ROADMAP.md:195-201`).
- The FSR-Vulkan submodule this port already links (`gpu/CMakeLists.txt:86-89, 175`) has a FG API with no caller:
  `include/ffx_vk_portable.h:217-260, 339-374` (`ffxVkPortableFrameGenerationContextCreate/Prepare/RecordDispatch`;
  Prepare takes depth, MVs, jitter, MV scale, near/far/FOV, camera basis vectors; Dispatch takes colour, optional
  HUD-less colour, interpolation rect) and `include/ffx_vk_framegeneration_presenter.h` (acquire pair, present plan,
  present-mode choice). The host owns pacing (submodule README line 62).
- Present path: `Presenter::Present` (`vk_presenter.cpp:689-860`); ImGui overlay `Gow3Overlay::Render` (`:820`);
  swapchain image count `min+1` (`vk_swapchain.cpp:241`), present mode at `:199-211`.
- Pacing: `VideoOutDriver::PresentThread` (`driver.cpp:654-720`), 480 Hz virtual vblank, slot-based limits
  (`emulator_settings.h:31-52`, "half refresh" mode).

## Design

1. HUD-less colour: capture the post-tonemap image before the first UMG/Slate UI draw (frame map, M0).
2. In `Presenter::Present`: keep the previous real frame, run Prepare/Dispatch, present generated then real
   (two acquires/presents per game flip), redraw the ImGui overlay on both. Raise the swapchain image count.
3. Pacing after the port's limiter: generated frame at half the measured real-frame interval; output limit doubled,
   real frames still capped at the engine FPS. Feed real frame times, not vblank ticks.
   MGS2's two-thread present split (`PresenterLoop`) is the reference.
4. Share depth / MVs / camera with the upscaler (one source).

## Stages (one change per launch)

| Stage | What | Verify |
|---|---|---|
| F0 | HUD-less colour capture, debug view | User sees the frame without UI |
| F1 | FG context + dispatch, generated frame shown alone (debug) | Generated frame looks right, HUD absent |
| F2 | Real + generated presented, basic pacing | Displayed frame count doubles (PresentMon displayed times) |
| F3 | Pacing tuned with the limiter / half-refresh mode | User judges smoothness by eye |

## Risks

- HUD-less picture must match the real frame outside the HUD (MGS2 invariant 1).
- Interaction with the 480 Hz virtual vblank and VSync modes.
