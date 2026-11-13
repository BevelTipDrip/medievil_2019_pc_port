# Plan: AMD FSR in place of UE4's TAA (FSR 3.x, FSR 4.x)

Status (2026-11-12): step 1 on branch `fsr`, verified on the PC (T-F1); its Deck check goes with step 5. Parent: [START-HERE.md](START-HERE.md). Related:
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

## To do (the user, 2026-11-12)

- **Sharpening with an upscaler** (fix built, F2b): the sharpening slider (menu Image tab, launcher `rcas_strength`) should set the
  upscaler's sharpening too, not only the presenter's RCAS. Today RCAS is forced off while an upscaler runs
  (`vk_presenter.cpp` `!Ue4Upscaler::Running()`), and DLSS gets `sharpness = 0`. FSR 3.1: its own RCAS
  (`enableSharpening`, `sharpness` 0-1). DLSS: NVIDIA deprecated its sharpening (newer DLSS ignores the
  parameter) [to check against our nvngx_dlss.dll version], so for DLSS the presenter's RCAS after DLSS, at the
  slider's strength, instead of being blocked. Verified for FSR (T-F6).
- **A separate FSR sharpening slider** (the user, after T-F6): FSR's sharpening gets its own strength, apart from the
  presenter's RCAS (CAS) slider; TAA needs far more RCAS than FSR needs its own sharpening.

## Step 1 (verified on the PC, T-F1)

`Ue4Upscaler` (vk_ue4_upscaler.{h,cpp}, renamed from vk_ue4_dlss.*): the front end keeps the input lookup, resets,
own output image, copy / blit into TAA's target and the texture cache bookkeeping; a backend
(`Ue4UpscalerBackend`: Name, Available, Has, Create, Evaluate) only creates its context and records the evaluation.
`DlssBackend` wraps `Dlss`. The hooks are created on every GPU (`GOW3_UE4_HOOKS=0` removes them) and only work while
`ue4_upscaler` is not taa (`Ue4Upscaler::On`, checked per draw as before). Switching upscalers resets the history.
Settings: `ue4_upscaler` (gow3_settings `Ue4UpscalerKeys`), `ue4_fsr_available`; the older `ue4_dlss=1` and
`GOW3_UE4_DLSS=1` still mean dlss (the new key wins in a file with both). Menu: Image > "Anti-aliasing / upscaler"
lists what the system runs; F9 cycles it. Launcher: Graphics > "Anti-aliasing / upscaler" (`UPSCALERS`).

T-F1 (2026-11-12, `4457918`, RTX 4090, 4K output at 75%): the old `ue4_dlss=1` came up as DLSS ("UE4 upscaler:
NVIDIA DLSS; DLSS available, FSR not available"), the DLSS context 2859x1620 -> 3812x2160 (TAAU), F9 between DLSS
and UE4 TAA with one reset per return to DLSS; the ini now has `ue4_upscaler=dlss`. The user: DLSS still works, the
switch works as before.

## Lows with an upscaler (T-F2, 2026-11-12)

T-F2 (`536dfd3`, research/docs/data/fsr-t-f2.log): FSR 3.1 ran at 75%, 50% and native (no failure; context 200-380 MB).
The user, on the map screen and in the graveyard: with an upscaler the 1% / 0.1% lows are worse than expected for the
GPU headroom. Native with UE4's TAA 226 / 209; DLSS at 50% 226 / 198; FSR at 50% only slightly better than native on
the 0.1%. FSR shows DLSS's steady small spikes at a higher amplitude. Suspected: CPU work the upscaler path adds per
frame (the hook on every draw while On, the motion pass setup, two image views a frame, the render pass ended for the
upscaler, FSR's recording of ~10 passes). Measured next with `GOW3_UPSCALER_STATS=1` (CPU time per frame of each part,
logged every 5 s), map screen, TAA / DLSS / FSR at 50%, about 60 s each.

T-F3 (`9bb4d1f`, research/docs/data/upscaler-stats-t-f3.log, map screen, 50%): per frame the rasterizer's hook took
~115 us (FSR) / ~124 us (DLSS) on stage B over ~970 calls, worst frame 290-570 us per 5 s; of it the motion setup
~3-5 us and the front end ~2.5-3.5 us. The recording thread: FSR ~80 us, DLSS ~50 us. Frames 4.22 ms on average,
spikes about 5.2 ms (the user; the log's worst frames 5.2-5.8 ms). Found: the hook's texture and buffer walk on every
draw is the cost. Fix built (F3): the walk only for compute dispatches and the TAA draws (by hash); every other draw
is told apart by its targets. To check: the same stats line, and the lows against TAA.

T-F4 (`330a5e0`, research/docs/data/upscaler-stats-t-f4.log up to the user's level load; map screen, 50%): the hook
~52 us a frame for FSR and DLSS (from ~115 / ~124), worst 200-450 us per 5 s (from 290-570); the log's worst frames
4.7-5.2 ms (from 5.2-5.8). Part of the 52 us is the stats' own two clock reads on ~800 draws. The user: the small
spikes left are the base game's (a work-queue item). Fix verified.

T-F4 level (research/docs/data/upscaler-stats-t-f4-level.log): in the graveyard near the level start, with
`GOW3_UPSCALER_STATS=1` and `GOW3_FPS_LOG=1`, DLSS at 75%: 1% / 0.1% lows 168 / 143 at 226 average, irregular spikes,
worst frames 9-12 ms in several 5 s windows. T-F5, the same spot and build with both lines off (the user's quiet
extra-env): the graph as in the user's earlier runs there tonight. **The stats perturb what they measure**: about two
steady_clock reads per draw (~2,400 a frame on stage B) and / or the 5 s FPS line. T-F3 / T-F4's lows with the stats on
are pessimistic; their hook before / after holds (the same stats in both). Before the stats are used again: time
one frame in N, or a cheaper clock.

T-F5 (research/docs/data/upscaler-quiet-t-f5.log, stats and FPS line off): the user: with the hook fix, DLSS's and
FSR's 0.1% lows are up significantly. F3 verified for the lows.

F2b (`b4e3620`): the menu's sharpening (rcas, rcas_strength) is FSR's own RCAS while FSR runs
(`Ue4UpscaleFrame::sharpness` = strength / 100: FSR's attenuation 2 - 2s equals the presenter's 2 (1 - p / 100)), and
the presenter's RCAS stays off then (`Ue4Upscaler::SharpensItself`); over DLSS and UE4's TAA the presenter's RCAS
runs (before: off over DLAA). The user: DLSS is already sharp; people may want it softer, which is the DLSS model
(preset) choice (roadmap), not RCAS.

T-F6 (2026-11-12, `b4e3620`, RTX 4090, 75%): the user: "I tested FSR sharpening, it works!" FSR with its sharpening
is clearer than UE4's TAA at 75%; to match that clarity, TAA needs so much RCAS that grass and fences are
oversharpened. F2b verified for FSR; RCAS over DLSS was not reported on. Next (the user): **a separate FSR sharpening
slider**, not shared with the presenter's RCAS (CAS) strength: the two want different strengths.
