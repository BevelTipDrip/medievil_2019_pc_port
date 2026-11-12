# Plan: AMD FSR in place of UE4's TAA (FSR 3.x, FSR 4.x)

Status (2026-11-12): step 1 fix built on branch `fsr`, not run. Parent: [START-HERE.md](START-HERE.md). Related:
[plan-dlss.md](plan-dlss.md), [plan-ue4-motion-vectors.md](plan-ue4-motion-vectors.md) (U2-U4, the inputs),
[upscaler-reference.md](upscaler-reference.md) (the God of War III upscaler), [plan-fsr-frame-generation.md](plan-fsr-frame-generation.md).

## The user's goals (2026-11-12)

- FSR 3.x: native AA ("FSRAA") at render scale 100, upscaling below it, and for the Steam Deck. Offer it to Deck
  players regardless of cost; whether a lower render scale with FSR beats the game's TAAU there is measured.
- FSR 4.x, **targeting native runs on NVIDIA** (the 4090). The RX 6900 XT can test AMD later (SSH to be set up).
- Community FSR 4 builds are acceptable (the user accepts the licensing risk of user-fetched models); the user found
  [Astyyyyy/fsr4-vk](https://github.com/Astyyyyy/fsr4-vk) and
  [FireBurn/UniversalScalers-Vulkan](https://github.com/FireBurn/UniversalScalers-Vulkan).

## What the repo has (code research, 2026-11-12)

- `gpu/third_party/fsr-vulkan`: FireBurn/FSR-Vulkan `c64f093` (MIT), the FSR 3.1.4 upscaler from AMD's MIT source
  (`ffx-vulkan::portable`), 3.1.5 / 3.1.6 parts, and an FSR 4 "v07" INT8/DOT4 provider without its shaders or
  weights. Patches 0001 (timings), 0002 (`<mutex>`).
- All FSR code (vk_temporal_upscaler, vk_fsr4, fsr411) belongs to the God of War III upscaler, off for MediEvil
  (`GOW3_UPSCALER=none`). Only FSR 1 EASU + RCAS on the presented image runs.
- The DLSS path (U2-U4) finds every input FSR needs and replaces UE4's TAA / TAAU (their cost goes away): colour
  (the RGBA16F composite TAA samples), D32 reversed infinite depth, U2's motion vectors (render pixels, previous minus
  current: FSR's convention, scale 1), jitter from the View, resets by View frame number, the output copied or
  blitted into TAA's target.
- FSR also needs: context flags (depth inverted, infinite, HDR, auto exposure), the near plane (View 2112), the
  vertical FOV (ViewToClip[1][1]), the frame time; ideally a reactive mask (UE4's responsive-AA stencil bit 3).
- Before step 1 the hooks existed only with the DLSS DLLs, so not on the Deck or on AMD / Intel PCs.

## Outside (web research, 2026-11-12; sources in the agent report, key ones here)

- FidelityFX SDK 1.1.x (FSR 3.1.1-3.1.4) had Vulkan; SDK 2.0 dropped it; **FSR SDK 2.4 (28 Oct 2026) brings Vulkan
  back** with FSR 4.1.2: FP8 on RX 9000, INT8 on RX 6000/7000, FSR 3.1.6 on everything else (NVIDIA included). Signed
  Windows DLLs only (`amd_fidelityfx_loader_vk.dll`, `amd_fidelityfx_upscaler_vk.dll`), MIT-listed.
  https://gpuopen.com/learn/amd-fsr-sdk-2-4-rx-6000-ryzen-apus/ [unverified here]
- FSR 3.1 inputs: motion vectors in pixels pointing to the previous frame, without jitter; 32-bit depth, inverted
  infinite recommended; optional reactive / transparency masks; exposure or auto exposure; Halton(2,3) jitter.
  https://gpuopen.com/manuals/fsr_sdk/techniques/super-resolution-upscaler/
- FSR 4 on NVIDIA: community only. FireBurn/UniversalScalers-Vulkan (MIT; FSR 4.1.2 graph as `i8_2` / `i8` /
  `lite_i8`, plus v07; integer dot products, no cooperative matrix; models produced locally, not committed; tested
  on an RX 6800M only). Astyyyyy/fsr4-vk is **GPL-3.0** (our project is GPL-2.0-or-later: reference or separate
  tool, not merged code). No published NVIDIA timings or image-quality data.
- Deck: no FSR 3.1 number published (an estimate of 1.5-2.5 ms at 800p is unmeasured; the user doubts it); FSR 4
  by hand on a Deck cost about 25% of the frame rate (user reports).

## Steps (one change per launch; the user's order, 2026-11-12)

| # | Step | Test |
|---|---|---|
| 1 | The hooks on every GPU, one `ue4_upscaler` setting (taa / dlss / fsr), DLSS as a backend of a generic front end (`Ue4Upscaler`) | DLSS unchanged (F9, DLAA and TAAU, resets, frame times); TAA unchanged; Deck: hooks present with TAA, frame times as before |
| 2 | FSR 3.1 native AA at render scale 100 (vendored FSR-Vulkan, MIT source) | F9 TAA / DLAA / FSR; stills, pans, ghosting; GPU ms |
| 3 | FSR 3.1 upscaling below 100, live render scale | 80 / 67 / 50 %, jitter sign (F8), cutscenes |
| 4 | Reactive mask from the responsive-AA stencil bit | torches, fire, death effects |
| 5 | Deck: FSR 3.1 vs TAAU at a fixed GPU clock | GPU ms, lows; then the Deck default |
| 6 | Official FSR 4 (SDK 2.4 DLLs, Windows) | RX 6900 XT later; on the 4090 only the 3.1.6 fallback |
| 7 | FSR 4 on NVIDIA: UniversalScalers-Vulkan INT8 (`i8_2` / `lite_i8`), user-fetched models | 4090: image and ms against DLSS |

## Step 1 (fix built)

`Ue4Upscaler` (vk_ue4_upscaler.{h,cpp}, renamed from vk_ue4_dlss.*): the front end keeps the input lookup, resets,
own output image, copy / blit into TAA's target and the texture cache bookkeeping; a backend
(`Ue4UpscalerBackend`: Name, Available, Has, Create, Evaluate) only creates its context and records the evaluation.
`DlssBackend` wraps `Dlss`. The hooks are created on every GPU (`GOW3_UE4_HOOKS=0` removes them) and only work while
`ue4_upscaler` is not taa (`Ue4Upscaler::On`, checked per draw as before). Switching upscalers resets the history.
Settings: `ue4_upscaler` (gow3_settings `Ue4UpscalerKeys`), `ue4_fsr_available`; the older `ue4_dlss=1` and
`GOW3_UE4_DLSS=1` still mean dlss (the new key wins in a file with both). Menu: Image > "Anti-aliasing / upscaler"
lists what the system runs; F9 cycles it. Launcher: Graphics > "Anti-aliasing / upscaler" (`UPSCALERS`).
