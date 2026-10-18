# Plan: DLSS SR on correct inputs, then DLSS frame generation (NGX Vulkan)

Status: planned 2026-10-18, nothing built. Needs [plan-ue4-motion-vectors.md](plan-ue4-motion-vectors.md) M2 first.
Parent: [plan-roadmap.md](plan-roadmap.md). Read [upscaler-reference.md](upscaler-reference.md) first.

## What is known today (read, 2026-10-18)

- DLSS SR exists: NGX directly through a run-time-loaded bridge with a C ABI (`gpu/dlss_bridge/gow3_dlss_bridge.*`,
  `GOW3_DLSS_BRIDGE_ABI 1`), built by `packaging/windows/build_dlss.sh` (MSVC 2022). Settings
  (`gow3_dlss_bridge.cpp:155-238`): MVLowRes, MVScale 1, PreExposure 1, render subrect, jitter, reset.
  Caller `RecordDlss` (`vk_temporal_upscaler.cpp:1989-2029`), falls back to FSR 3.1. Inputs are the Bloodborne ones
  (depth not inverted, `vk_dlss.cpp:207-209`).
- Bloodborne's bridge is the same code, newer (Linux `.so`, user-chosen NVIDIA library in `<user>/dlss`, separate
  search paths, a log-format fix). Nothing for FG.
- **DLSS FG backend: NGX Vulkan, not Streamline.** The DLSS SDK 310.9.1 (as in
  `MGS2-Lab\MGSHDFix\external\DLSS\include\nvsdk_ngx_helpers_dlssg_vk.h`) has `NGX_VK_CREATE_DLSSG`,
  `NGX_VK_EVALUATE_DLSSG`, `NGX_VK_ESTIMATE_VRAM_DLSSG`. MGS2 used NGX directly (D3D12) to keep its own swapchain
  and pacer; same reasons here. Decision row in plan-roadmap.md.

## Design

1. SR: depth inverted, the game's jitter, real MVs, reset from the camera cut. Optionally take Bloodborne's bridge
   loading improvements.
2. FG: extend the bridge ABI (create/evaluate DLSSG). Inputs as MGS2 `lab_dlssg.cpp:100-182`:
   `UserInterfaceRecompositionEnabled = 1` at creation; matrices in DLSS-G's `clip x M` layout; view-to-clip without
   jitter; jitter; `motionVectorsInvalidValue`; MV scale; HUD-less colour + UI colour/alpha.
3. Presentation and pacing shared with [plan-fsr-frame-generation.md](plan-fsr-frame-generation.md) (one FG present
   path, two backends).
4. Check inputs with NVIDIA's debug `nvngx_dlssg.dll` visualizer.

## Stages (one change per launch)

| Stage | What | Verify |
|---|---|---|
| D0 | Build the DLSS bridge on this machine | `gow3_dlss.dll` next to `out/medievil-runtime.exe`, log shows feature created |
| D1 | DLSS SR on the M-stage inputs | User compares with FSR 3 / UE4 TAAU |
| D2 | DLSSG feature create/evaluate, debug visualizer | Visualizer shows correct camera / MVs |
| D3 | DLSS FG presented through the shared FG path | Displayed frames double; user judges by eye |

## Risks

- NVIDIA DLLs are not redistributable in git: keep them out of the repo.
- Matrix layout / transpose (MGS2 trap 13), zero-initialised options (MGS2 trap 15).
