# Study: upstream commits since our fork (1.3.3-1.3.7) vs `lab`

Status: study, 2026-10-25. Nothing merged, built, run or committed. `git fetch upstream` brought `upstream/main` from
`d4c9cf4` (1.3.5) to `bb28370` (1.3.7) and the tags `v1.3.6`, `v1.3.7`. Everything else was read only (`git log`,
`show`, `diff`, `cherry`, `merge-tree`; `merge-tree --write-tree` only writes loose objects, no refs).
Parent: [START-HERE.md](START-HERE.md). Earlier pass over 1.3.3-1.3.5: [plan-upstream-merge.md](plan-upstream-merge.md)
(still valid; this study adds 1.3.6/1.3.7 and the PR commits, and re-checks everything against today's `lab`).

**Labels.** **[diff]** = read in the upstream diff. **[lab]** = checked in `lab`'s code. **[mt]** = seen in a
`git merge-tree --write-tree lab upstream/main` dry run (tree `0c5e5c4`). **[inf]** = my inference, not tested.
Paths relative to `gpu/shadps4/video_core/` unless they start with `src/`, `scripts/`, `launcher/`, `gpu/`, `run.py`.
Upstream line numbers are in `upstream/main`, `lab` line numbers in `lab` at `450df36`.

## Summary

- **Merge base `783b25a`** (MediEvil 1.3.2). `lab` was `121952a` at the brief and is now `450df36` (one more commit,
  pass-break diagnostics); the base is the same. `lab` has 248 commits after it.
- **Upstream after the base: 18 commits, 14 non-merge** (4 are PR merges: `cae2e61`, `8a7abb9`, `be3491a`,
  `2c20e99`). `git cherry lab upstream/main`: all 14 are `+` (none is in `lab`, not even patch-equivalent). Nothing
  upstream predates our fork that we lack (the base is upstream's own 1.3.2 commit).
  43 files, +2,050 / -200.
- **Dry-run merge [mt]: 10 files conflict** (5 more than on 2026-10-19): `vk_instance.cpp`, `vk_runtime.cpp`,
  `vk_scheduler.cpp`, `image_view.cpp`, `texture_cache.cpp`, `launcher/medievil_launcher.py`, `patches/MediEvil.xml`,
  `run.py`, `scripts/ue4_override.py`, `src/runtime_extra.c`. Against `merge-1.3.5` only 3 conflict (`MediEvil.xml`,
  `ue4_override.py`, `runtime_extra.c`), but that branch is 61 commits behind `lab`.
- **Nothing upstream touches the render-pass breaks, barriers inside passes, the buffer cache, readbacks, the memory
  map, the shader/pipeline cache or the present/vblank thread** beyond one frame-limit rule. Upstream's performance
  work is the volume-view fix (= our V3) and the texture GC budget (neutral on a 24 GB card). Its value for us is
  **stability fixes and crash diagnostics**, plus two **silent semantic hazards in the auto-merged files**.

## 1. Commits

| Commit | Ver. | What it changes (mechanism; files) | Class |
|---|---|---|---|
| `8a70032` | 1.3.3 | EU CUSA12982 profile; all languages; **VSync frame-limit rule** (`core/libraries/videoout/driver.cpp:687`: VSync on and limit >= refresh-1 -> no present-side limit); #11 4K menus via live `r.SetRes` + internal size; debug-menu AI resume; **F12 PNG screenshot** (`vk_presenter.cpp:577,600`, copy + miniz on a thread; Shift+F12 = raw dump); launcher update check; scheduler producer/interleave **diagnostics** (`vk_scheduler.h` `NoteProducer`, `ReportInterleave`) | mixed, see 2-4 |
| `dcde108` | | README | not relevant |
| `d0e1a2a` | | `ue4_override.py`: debug-menu resolution buttons keep 1080p with supersampling (+8 lines) | not relevant (we boot 4K) |
| `4d72031` | 1.3.4 | **`SyncWithDrawRecorder`**: stage A drains stage B before `EndRendering` (`vk_scheduler.cpp:137`), `CommandBuffer()`, `Record()`, and before the texture cache lock in `FindImage` (`texture_cache.cpp:902`) / `UpdateImage` (`texture_cache.h:187`); texture cache mutex becomes `OwnedMutex` (owner thread id stored per lock); volume 2D-array views (sampled) + `ClearVolumeSlices` (`vk_runtime.cpp:719`) + whole-volume barriers (`image.cpp`); **slot reallocation use-after-free fixes** (`texture_cache.cpp:592`, `FindDepthTarget`); **deferred sampler destruction** (`texture_cache.cpp:1644`); **stencil proxy check** (`texture_cache.cpp:1159`, `texture_cache.h IsStencilProxyCandidate`); GC limits from the driver budget on all GPUs (`texture_cache.cpp:1524`); 7.1->stereo downmix (`src/runtime_audio.c`); **60 FPS default with a one-time reset** (`scripts/graphics_options.py:23,74`); "Precise" readbacks hidden in the launcher; `GOW3_IMAGE_STATS` | mixed, see 2-4 |
| `d4c9cf4` | 1.3.5 | Volume views only with `VK_EXT_image_2d_view_of_3d` (AMD Polaris crash #18); 2D views of 3D attachment-only without it (`image_view.cpp`); **`RecordData` chunk fix**: the drain moves before `RecordData` so data and command share a chunk (`vk_scheduler.h:787` `RecordDataFix`, `ReportStaleRecordData`) | conflicts / overlap |
| `c7a9ef3` (PR #26) | | MediEvil **01.02**: per-version `GAMES` entries in `scripts/patches.py`, 01.02 block of the 4K pool patch in `patches/MediEvil.xml`, NpToolkit2 imports as logged no-op stubs (`src/probe.c`), unit test | not relevant now (we run 01.00); see 4 |
| `3a240c1` (PR #28) | | **Frame analyzer crash**: flatbuf ring bindings have address 0; `IsValidGpuMapping(0, 0)` passed and `FrameCapture::Buffer` read a null pointer. Uses `stage.FlatUserData()`, null guard, hex dump (`vk_rasterizer.cpp:2085`, `vk_frame_capture.cpp:305`); also guards `camera_motion->OnConstants` against address 0 | **worth porting** |
| `c167454` (PR #29) | | 01.02 native output: version-aware profile lookups in `run.py`, lowercase override pak name (`scripts/ue4_override.py:23`), `r.SetRes` slot moved to `0x5c28368`, repeats at 2/5/10/20 s | superseded by `1ae17cd` |
| `8cf5cc5` | 1.3.6 | **Safe GPU mode** `GOW3_VULKAN_COMPAT` (no maintenance5/8, no feedback-loop layouts, no 2D-of-3D; `vk_instance.cpp:227`); **GPU/driver/feature log line** (`vk_instance.cpp:675`); **crash reports dump the barrier batch being recorded** (`vk_runtime.cpp:936,951`, called from `src/probe.c:175,282`) and **mark destroyed images** (8,192-entry create/destroy ring, `texture_cache/image.cpp`); all controllers merged, background pad events (`gpu/shim/window.cpp:19`), stick range (`src/runtime_pad.c`); AA option `ue4_aa` -> `r.PostProcessAAQuality` (`scripts/ue4_override.py:347`); portable mode; **GPU profile frame start without camera motion** (#30, `vk_rasterizer.cpp:998`); NpToolkit2 stubs return 0 (`src/probe.c:86`) | mixed, see 2-4 |
| `cc8da6c`, `60230fd`, `4ec0013` (PR #13) | | Linux build, AppImage, pinned downloads; `libvulkan.so.1`; POSIX `nanosleep` in `src/runtime_extra.c`; Linux variants of the screenshot and scheduler report | not relevant |
| `1ae17cd` | | **Native output finds `r.SetRes` in each eboot** by a code pattern (`scripts/ue4_override.py:47 setres_slot`, passed as `GOW3_UE4_SETRES_CVAR`); US 01.00 slot is `0x5c28328` (what `lab` uses, `src/runtime_extra.c:581`), PR #29's `0x5c28368` was 01.02-only | worth porting later (low) |
| `bb28370` | 1.3.7 | README, launcher version | not relevant |

## 2. Same as ours (independent equivalents)

| Area | Upstream | `lab` | More complete |
|---|---|---|---|
| Volumes drawn as 2D slices (V3) | `4d72031` + `d4c9cf4`: 3D image served and **sampled** through a 2D array view; needs `VK_EXT_image_2d_view_of_3d`; `ClearVolumeSlices` clears only the addressed slices [diff] | `1f18217`: the 2D array view is attachment-only (`image_view.cpp:99`), sampled through its 3D view, no extension needed; whole-volume clear [lab] | Ours on the view rule (any driver); theirs on clears. Plan unchanged: ours + their `ClearVolumeSlices`. |
| Whole-volume barriers | `image.cpp`: layer 0, count `resources.layers` [diff] | `image.cpp:306-311`: layer 0, count 1 [lab] | Identical effect. **The merge keeps both blocks** (merged `image.cpp:345,353`) [mt]: drop one. |
| 4K menus (I1 / #11) | live `r.SetRes` + internal size at the output size; now with a per-eboot slot | `ebf85f4`: boot at 4K through the game's display-metrics branch (default), live `r.SetRes` as fallback (`run.py:157-182`) | Ours for navigation (plan-baseline-issues.md I1); keep theirs as the `GOW3_UE4_BOOT_4K=0` path. |
| GPU profile frame start (#30) | `8cf5cc5`: `NoteFrameStart()` when a draw's target becomes a display buffer (`vk_rasterizer.cpp:998`); also bumps `gpu_frames` [diff] | `0a038d3` + `121952a`: `profiler->BeginFrame()` at the first draw of a display pass (`vk_rasterizer.cpp:1012-1020`) [lab] | Same trigger in practice [inf]. Ours also reads timestamps without waiting (`1068344`). Theirs also feeds `Gow3Stats::gpu_frames` (only used by `GOW3_BUFFER_STATS`, `buffer_cache.cpp:708`). **Hazard below (H2).** |
| Recorder race, crash in level 3 (#2/#12, our L5/C1) | `SyncWithDrawRecorder` before stage A records (guards every recording call) | `bc0e1d6` L5: drain before every queued command in `ProcessCommands` (`amdgpu/liverpool.cpp:99`) | Complementary (see 3). |

## 3. Conflicts with ours

| Area | Upstream | `lab` | Merge note |
|---|---|---|---|
| **`SyncWithDrawRecorder` / `OwnedMutex` / `RecordData` order** (`4d72031`, `d4c9cf4`) | Every `Record()`, `RecordData()`, `CommandBuffer()` and `EndRendering()` calls the sync hook; on stage A with stage B busy it drains; off stage A/B (presenter, helpers) it only logs once per caller (static mutex + 16-slot scan per call); `NoteProducer` scans 8 thread-locals per `Record()` [diff] | Stage B is our bottleneck (92-97% busy in the fight, plan-stage-b-split.md T-F1); `EndRendering(std::source_location)` with pass-end stats (`vk_scheduler.cpp:174`) | Textual conflict in `vk_scheduler.cpp` (signature). Semantics: on stage B the hook returns at `DrawPipe::OnStageB()` but still costs a call + `NoteProducer` per `Record` (a few ns x thousands of records a frame) [inf]. On stage A, L5 already leaves the pipe idle before queued commands, so drains should be rare [inf]; count them with `drains_by_reason` "foreign record" and the "recorded while the draw recorder had queued draws" log. **D2 deadlock check from plan-upstream-merge.md still applies**: `DownloadMemory` holds `cpu_writes_mutex` (`buffer_cache.cpp:607`) and records a copy -> `EndRendering` -> drain, while stage B takes the same mutex in `ProcessCpuWrites`; drain before taking the lock. Recommendation: take it (stability), measure stage B per-packet time before/after (T-S1 sections). |
| **VSync frame-limit rule** (`8a70032`, `driver.cpp:687`) | VSync on and limit >= refresh-1: no present-side limit (issue #3, 60 Hz TVs) | Present thread rebuilt: precise limit, newest-frame mode, precise tick, inline present, tick = cap, 960 tick + limiter (`driver.cpp:1020-1070`) | **Auto-merges with no conflict marker [mt]** inside our `current_limit` lambda. With VSync (FIFO) and `fps_limit` >= refresh-1 it switches our precise limiter / newest-frame slotting off. Our tested T-P120/T-P960 sets use `vsync=0`, so they are unaffected; FIFO runs at 120 tick + 120 limit would change [inf]. Gate it: apply only without `GOW3_PRECISE_LIMIT`/`GOW3_NEWEST_FRAME` and with the automatic tick. |
| Safe GPU mode (`8cf5cc5`) | `vk_instance.cpp`: maintenance5/8 and feedback-loop extensions behind `VulkanCompat()` | `vk_instance.cpp:334-349`: `GOW3_DISPLAY_TRACE` present-wait block between the same lines | Textual only: keep both. Our V3 view mask keeps `eAttachmentFeedbackLoopEXT` (`image_view.cpp:102`); with feedback loops off the image never gets that usage bit, so the mask stays valid [inf]. |
| `image_view.cpp` usage rule | 2D view of 3D: attachment-only only without the extension | attachment-only always | Keep ours (as planned). |
| `texture_cache.cpp` (3 hunks) | `GOW3_VOLUME_VIEWS`, sampled-view overlap test, `image_stats` | `IsVolumeSliceTarget` (`texture_cache.cpp:63,796,1140`), `CACHE_STAT` | As in plan-upstream-merge.md section 2.4. |
| `vk_runtime.cpp` | `ClearVolumeSlices` | `CopyBufferToStencil` (V4) at the same spot | Keep both. |
| `run.py`, `scripts/ue4_override.py`, `patches/MediEvil.xml`, `src/runtime_extra.c` | per-version profiles, `setres_slot`, AA parameter, lowercase pak, 01.02 patch block, POSIX nanosleep | 4K boot/memory, HZB remap, decal prepass, particle pass, `hzb` parameter, display-metrics patch, `RUNTIME_WAIT_BEGIN` wait sites | Textual: keep both sides. Our 01.00-only hooks (HZB remap, display metrics, native output fixed address) should move to upstream's per-version profile (`game_profile(title, version)`). |
| `launcher/medievil_launcher.py` (2 hunks) | update check, 60 FPS default, safe GPU mode, portable mode, AA, stick range | PERF_PATCHES, Presentation section | Textual; see H3/H4 for behaviour. |

### Semantic hazards in files that auto-merge [mt]

- **H1. VSync rule** inside our limit lambda (above).
- **H2. GPU profiler frames counted twice.** The merged `vk_rasterizer.cpp` keeps our `profiler->BeginFrame()`
  (merged line 1036) **and** upstream's block that calls `NoteFrameStart()` -> `profiler->BeginFrame()` (merged
  lines 1062-1063, 3953): two frame starts per display pass, halving the per-frame GPU times we just fixed
  (`121952a`). Keep ours, add only the `gpu_frames` increment.
- **H3. engine_fps reset.** `scripts/graphics_options.py:74`: an ini without `fps_default_60` gets `engine_fps=60`
  once at the next start (the user plays at 240). Pre-seed `fps_default_60=1` in `medievil.ini` (back it up first)
  or drop the reset.
- **H4. Update check.** The launcher asks bc160201983's releases for a newer `VERSION`; from a source tree it only
  opens the browser, a frozen build would copy upstream's release over ours (`install_update`). Off for our builds.

## 4. Worth porting (ranked by benefit / risk)

| # | Item (commit) | Benefit | Risk | Effort | Files |
|---|---|---|---|---|---|
| 1 | **Slot reallocation use-after-free + deferred sampler destruction + stencil proxy check** (`4d72031`, from the Uncharted port) | Real crash fixes: `slot_images.insert` can move every `Image` while `cache_image` / `image` references are held (`ResolveDepthOverlap`, `FindDepthTarget`); samplers destroyed while submitted commands use them; color textures redirected to a depth-stencil image (GPU page fault). **Candidates for C1 (NVIDIA crash under a recorded draw) and the rare silent exits [inf]**. `lab` has none of them [lab] | low (small, local) | small | `texture_cache/texture_cache.cpp`, `texture_cache.h` |
| 2 | **Crash-report barrier dump + image lifetime ring + GPU log line** (`8cf5cc5`) | Next C1-type crash says which barrier/image (live or DESTROYED); log names GPU, driver, features. Complements our minidumps (`b01661e`, `5247265`) | low: one thread-local store per `FlushBarriers`, one relaxed atomic per image create/destroy | small | `vk_runtime.cpp`, `texture_cache/image.cpp/.h`, `gpu/gow3gpu.h`, `src/probe.c`, `vk_instance.cpp` |
| 3 | **Frame analyzer flatbuf fix** (`3a240c1`) | `GOW3_CAPTURE_TRIGGER` crashes on flatbuf ring bindings; the capture is the tool for mapping the UE4 frame (plan-ue4-motion-vectors.md) and the particle pass breaks. `lab` has the old code at `vk_rasterizer.cpp:2182-2190` [lab]. The `OnConstants` part only runs with camera motion (off for MediEvil: `gbuffer_draw` needs `camera_motion->Enabled()`, `vk_rasterizer.cpp:269`) | very low | tiny | `vk_rasterizer.cpp`, `vk_frame_capture.cpp` |
| 4 | **`SyncWithDrawRecorder` + `RecordData` order** (`4d72031`, `d4c9cf4`) | Guards any stage-A recording L5 does not cover; logs the caller with a backtrace when it cannot wait | medium: stage B hot-path cost, D2 lock-order deadlock (section 3) | medium | `vk_scheduler.h/.cpp`, `vk_rasterizer.cpp`, `texture_cache.cpp/.h`, `vk_draw_pipe.h` |
| 5 | **F12 PNG screenshot** (`8a70032`) | Full-resolution proof shots for "confirm screenshots with the user"; zero cost unless pressed | low (F12 meaning changes: raw dump moves to Shift+F12) | small | `vk_presenter.cpp`, `gpu/shim/gow3_overlay.cpp/.h` |
| 6 | Audio 7.1 -> stereo downmix (`4d72031`), pad merge + background events + stick range (`8cf5cc5`) | QoL; no `lab` changes in `src/runtime_audio.c` / `src/runtime_pad.c` [lab], merge cleanly [mt] | low | none (merge) | `src/runtime_audio.c`, `src/runtime_pad.c`, `gpu/shim/window.cpp` |
| 7 | `ClearVolumeSlices` exact volume clears (`4d72031`) | Correct partial clears (our whole-volume warning never fired) | low | small | `vk_runtime.cpp` |
| 8 | Per-version profiles + `setres_slot` (`c7a9ef3`, `1ae17cd`, `8cf5cc5` follow-ups) | 01.02 / EU players; the pattern our 01.00-only hooks need before any PR | low for 01.00 | medium (our hooks too) | `scripts/patches.py`, `run.py`, `scripts/ue4_override.py`, `src/runtime_extra.c`, `patches/MediEvil.xml` |
| 9 | AA option `ue4_aa` (`8cf5cc5`) | FXAA/off for sharpness tests; irrelevant once our upscalers need TAA | low | small (conflicts with our `hzb` parameter) | `scripts/ue4_override.py`, `run.py`, launcher |
| 10 | GC limits from the driver budget (`4d72031`) | none on the 4090 (our collector never ran, plan-upstream-merge.md); matters for 8 GB users and PRs | low | none (merge) | `texture_cache.cpp` |

## 5. Not relevant (bulk)

Linux build and AppImage (`cc8da6c`, `60230fd`, `4ec0013`, `2c20e99`); READMEs (`dcde108`, `bb28370`); EU/languages,
portable mode, safe GPU mode (AMD Polaris/Vega only; keep it, no effect on NVIDIA), NpToolkit2 stubs (01.02 only),
lowercase pak name (case-sensitive filesystems only), debug-menu 1080p buttons (`d0e1a2a`), cheats note in the
port's menu, `GOW3_IMAGE_STATS` churn report, launcher version strings, update check (disable, H4).

## 6. The topics in the brief

| Topic | Upstream |
|---|---|
| Frame pacing / present / vblank / limiter | Only the VSync rule (H1). No precise tick, inline present, newest-frame or pacer work: ours is ahead. |
| Render passes, pass breaks, feedback loops, sampled attachments, barriers in passes | **Nothing** for the particle-burst pass doubling. Related only: `SyncWithDrawRecorder` in `EndRendering` (correctness, not fewer breaks); `ClearVolumeSlices` records its own begin/end rendering outside the scheduler's counters; safe GPU mode can turn feedback-loop layouts off (then targets bound as textures use `eGeneral`, `vk_rasterizer.cpp:2561-2569`). Bloodborne (read only, pointer): 0.5-pre3 notes uploads before draws ending passes (`docs/CHANGES_0.5-pre3.md:33`), 0.5 copies breaking passes ~100x a frame (`docs/CHANGES_0.5.md:32`), and a render-state memo (`docs/parallel_gpu.md:458`). |
| Buffer / texture cache | Texture cache: crash fixes (item 1), GC budget, volume views. Buffer cache: untouched. |
| Readbacks | "Precise" hidden in the launcher (black screen #15); the variable still works. |
| Memory mapping | Untouched (our R2/R3, M1, D2, lock-free reads stand alone). |
| Shader / pipeline cache | Only a `vk::PipelineBinaryKeyKHR{}` spelling for GCC (`cc8da6c`). |
| Crashes / stability | Items 1-4. Nothing on the pthread boot crash (T-Z4) or the intro-load stall. |
| UE4 patches | `r.SetRes` slot finder, AA cvar, lowercase pak, 01.02 addresses. |
| Launcher / settings | 60 FPS default + reset (H3), update check (H4), safe GPU mode, portable mode, AA, stick range. |

## 7. What was verified and what is inferred

- **Verified in diffs [diff]:** every mechanism in sections 1-4 and the line numbers in `upstream/main`.
- **Verified in `lab` [lab]:** no slot-reallocation, sampler, stencil-proxy, barrier-dump or flatbuf fixes; V3 view
  rule and volume barrier; profiler frame start; D2 lock site; L5 drain sites; no `lab` edits to
  `runtime_pad.c` / `runtime_audio.c`.
- **Verified by dry run [mt]:** the 10 conflicting files, the auto-merged VSync rule, the doubled profiler frame
  start, the duplicate volume-barrier block.
- **Inferred [inf], to measure after a merge:** the stage-B cost of the sync hook, how often stage A drains for it,
  whether items 1-2 relate to C1 / S1, the effect of the VSync rule on FIFO + limiter runs.
