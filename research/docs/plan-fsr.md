# Plan: AMD FSR in place of UE4's TAA (FSR 3.x, FSR 4.x)

Status (2026-11-14): steps 1-3 verified on the PC (on deck-lab since `f4de291`), step 5 run on the Deck (FSR 3.1 kept as an
option, not the Deck default); next step 4 (reactive mask), then 6-7 (FSR 4); later FSR 2.x for the Deck and the side
items (CAS bug, DLSS preset, cheaper upscaler stats). Parent: [START-HERE.md](START-HERE.md). Related:
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
  slider's strength, instead of being blocked. Verified for FSR and for RCAS over DLSS (T-F6).
- **A separate FSR sharpening slider** (the user, after T-F6): FSR's sharpening gets its own strength, apart from the
  presenter's RCAS (CAS) slider; TAA needs far more RCAS than FSR needs its own sharpening. Verified (F2c, T-F7):
  `fsr_sharpness` 0-100 (0 = off, default 25), menu Image tab and launcher Graphics "AMD FSR sharpening"; the
  Sharpening (RCAS) box and strength are only the presenter's RCAS (the game's TAA, DLSS), off while FSR runs.
- **FSR 2.x for the Deck** (the user, 2026-11-14, after T-FD1; later): FSR 3.1 is expensive on the Deck; FSR 2.1 is
  much cheaper there (the user's experience) and should not look much worse than the game's TAA. A further choice
  beside FSR 3.1, not a replacement. Source to check: AMD's FidelityFX FSR 2 (MIT, had a Vulkan backend).

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
oversharpened. F2b verified for FSR; the user: RCAS over DLSS works too (verified). Next (the user): **a separate FSR sharpening
slider**, not shared with the presenter's RCAS (CAS) strength: the two want different strengths.

F2c (`43d6e0f`): FSR's sharpening has its own setting, `fsr_sharpness` (0-100, 0 = off, default 25:
`Gow3Graphics::ParseFsrSharpness`), in the menu's Image tab and the launcher's Graphics page as "AMD FSR sharpening".
`Ue4UpscaleFrame::sharpness` comes from it; the RCAS box and strength no longer reach FSR. The menu's Information
row "Sharpening" says "AMD FSR, N%" or "AMD FSR, off" while FSR runs. To check: FSR's sharpness follows the new
slider live, the RCAS strength no longer changes FSR, and RCAS over TAA / DLSS is as before.

T-F7 (2026-11-12, `43d6e0f`, RTX 4090, FSR at 75%): the user: "it all works as expected", in the game and the
launcher: FSR's sharpening follows its own slider live (0 = off), the RCAS strength leaves FSR alone, RCAS over TAA /
DLSS as before. F2c verified.

Step 3 (2026-11-13, the user): verified. Several render scales and cutscenes work; the default jitter sign is right.
F8 (cycle the jitter sign) stays in place for later testing. Step 4 (reactive mask): no smearing noticed on torches,
fire or death effects so far, still the next logical step after step 5 (the user's order: 5, 4, then 6-7).

T-FD1, step 5 (2026-11-14, Deck, build `60de4b3` deployed over SSH, no logging, quiet extra-env; the user watched frame
times in the overlay, a level with a fixed camera, TAA and FSR switched in the menu at several fixed GPU clocks): **FSR 3.1
is fairly expensive on the Deck** (no numbers recorded). Image: at 67% render scale FSR clearly beats the game's TAA, at 80%
it looks great. The user: a good option for players targeting 30 or 45 FPS (the Deck runs arbitrary refresh rates).
Decision: FSR stays available on the Deck as an option; the Deck default stays the game's TAAU. A cheaper FSR 2.x
option for the Deck later (To do).

F4 (step 4, fix built 2026-11-14, not run): the reactive mask from UE4's responsive-AA stencil bit (bit 3 of the scene
depth-stencil, the bit the game's TAA tests; study-taau-frame-map.md). U2's motion pass gets a REACTIVE variant that
also reads the stencil and writes an R8 mask (`fsr_reactive` / 100 on marked pixels, 0 elsewhere); FSR gets it as its
reactive mask. Only while FSR runs with `fsr_reactive` above 0 (menu Display "AMD FSR reactive mask", launcher
Graphics; 0-100, default 50 provisional); DLSS and TAA keep the plain pass. Cost (estimate, before measuring): FSR
without a mask read its own 1x1 blank texture; now one byte a render pixel in its first pass, plus ~2 bytes a pixel
in the motion pass: about 0.03 ms or less on the Deck. The motion debug view's mode 5 (`GOW3_UE4_MOTION_VIEW=5`,
F10 cycles) paints the marked pixels red: first, how much of the game is marked at all.
Tests: T-F8 (PC): mode 5 on torches, fire, death effects; then the slider 0 vs 50 / 100 on the smearing. T-F9
(Deck, later): the slider 0 vs on at a fixed GPU clock, frame times.

T-F8 run (2026-11-14, PC, the Graveyard): **no pixel marked**, and the user saw no smearing on torches under FSR at 75%
even with violent camera shakes (NVIDIA screenshots). Follow-up `bfc5a9e`: mode 5 shows the raw stencil (red bit 3,
green bits 0-2, blue bits 4-7) and logs pixels per bit every 480 passes. Run at 75 / 67%: **0 pixels for every bit**, on
the velocity pass's and the G-buffer's depth. Capture C (`mausoleum-pc_frame21929.rdc`, scratchpad scripts
stencil_trace.py / stencil_groups.py, 2026-11-14) explains it, **confirmed**:
- the base pass writes only bits 4-7 (write mask 0xf0, ref 128 / 160 / 176 / 0; 236 draws, eid 3267-4928);
- eid 8797 (the V4 stencil-plane copy = the game's compute clear) sets every pixel to 0 before lighting;
- lighting: light-volume IncWrap / DecWrap + Zero (455 draws) and Replace ref 1 (21 draws), all back to 0;
- no draw in the frame can set bit 3; the separate-translucency depth-stencil (8699, 2880x1620) has no stencil writes;
  at the TAA draws (16004 / 16012) the stencil is 0 on all 8,294,400 pixels.
So MediEvil never uses UE4's responsive AA: the TAA's bit-3 test (draw 16012, `b373` / `9c89`; the 75% TAAU `a080`
does not even read the stencil) never passes. The read path works; the stencil source is empty by design.
Open: keep the stencil mask (it stays 0) or take the mask from the separate-translucency target's alpha (UE4 writes
transmittance there: 1 - alpha = how much particles / fire / smoke cover a pixel), read in the same motion pass.

T-F8b / T-F8c (2026-11-14, PC, 67%): with effects resolution 100 the game **does** mark death (blood) effects: bit 3 on
5.6-11.4% of the screen in those frames, the mask works. Torches are never marked. Trace `5a796af` (draws that can write
bit 3, per depth-stencil): below 100, UE4 writes bit 3 (write mask 0x8) into the separate-translucency depth-stencil
(1288x724 at effects 50, colour target the same size), never copied to the scene stencil, so neither FSR's mask nor the
game's own TAA sees it. (The trace's 0xff-mask lines on the scene stencil are the lighting draws, not marks.)
**Decision (the user): stop here.** It covers one effect in busy scenes, only at an effects resolution kept below 100
for the 0.1% lows (heavy combat ~110 FPS at a 220+ average); not worth more complexity. Not done: reading the
separate-translucency stencil in the motion pass (one extra fetch while FSR runs with the slider above 0).
Then (the user): `fsr_reactive` defaults to 0 everywhere and its menu and launcher sliders are gone (ini only, for
tests); the user's ini set to 0. Audit of step 4's code: the motion pass's first barrier had widened from compute to
all commands for every DLSS / FSR frame (fix built: back to compute only); 8585c5a flipped the line endings of 8
files (restored; research/tools/eol_guard.py and its pre-commit hook now refuse such commits).
