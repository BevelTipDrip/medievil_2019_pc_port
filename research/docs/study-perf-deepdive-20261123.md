# Study: performance deep dive after the HDR work (2026-11-23)

Status: research done (four read-only agents plus traces), nothing built. Parent: [START-HERE.md](START-HERE.md).
Related: [plan-hdr-final.md](plan-hdr-final.md), [study-hdr.md](study-hdr.md) H43-H49,
[study-tooling-off-cost.md](study-tooling-off-cost.md). Agent notes with full tables:
`research-data/perf-deepdive-20261123/{hdr,memory,history,offcost}/`. [V] seen in code or data, [I] inferred.

## Trigger and measurements

The user saw lower 1% / 0.1% lows on the newest build (hdr-final `c254378`) in SDR and HDR, against the best state
of 2026-11-15 (same static spot near the start of Cemetery Hill, RTSS):

| Run (SDR unless noted) | current | 1% low | 0.1% low |
|---|---|---|---|
| 11-15 19:28, best run (exe 18:37:53: e12ffaa's code, a dev build; logs off) | 236.6 | 233.2 | 227.7 |
| 11-23 21:43, c254378, flip trace on | 227.5 | 213.1 | 207.9 |
| 11-23 22:08 / 22:09, c254378, trace off (two readings, one launch) | 227.2 / 226.5 | 217.4 / 217.0 | 207.2 / 212.8 |
| 11-23 22:20, c254378, trace off, GOW3_HDR_GAME commented out | 230.3 | 223.9 | 220.9 |

- **GOW3_HDR_GAME had no effect in these runs** [V]: every SDR run logged `GOW3_HDR off` (launcher "Allow HDR output"
  unticked), and every reader of GOW3_HDR_GAME also needs GOW3_HDR (video-out capability, pak cvars,
  `Ue4HdrGrading` creation). The 207 -> 221 difference is run-to-run variance: single RTSS readings at this spot
  vary by several percent. Decisions need repeat runs or flip traces over the same stretch.
- **FanControl** (found earlier the same day): its WPF window drew with D3D about once a second; each draw lined up
  with a burst of 6-8 slow frames (GPU +1-1.5 ms, dxgkrnl DPCs x3). Closed for every later run.
- **Flip traces** (HDR run 20:22, SDR run 21:39, step-1 reference 20:00): frames are GPU-bound (the presenter waits
  ~4.0 ms of ~4.4 ms for the GPU; recording 0.06 ms; threads busy ~2 ms a frame, no CPU starvation). In SDR with the
  same settings the current build equals step 1 (`e32c55f`): median 4.33 vs 4.36 ms, p99.9 4.72 vs 4.75 ms. The SDR
  gap to 11-15 is a tight distribution sitting ~3% above the 4.22 ms the 237 cap needs, with ~1 s level shifts, not
  spikes.
- **No code commit since the best run plausibly costs 2-4% in SDR** (history agent; per-draw HDR hooks are a few ns,
  `ue4_hdr` is null without GOW3_HDR). Least proven: `21886b6` (the release / dev split, a whole-binary change) and
  `e32c55f` (per-draw hooks); neither measured trace-off at the static spot.
- **Leading environmental lead** [V log line, I effect]: since 11-17 every run logs "HDR10 surface offered", i.e.
  Windows HDR on for the monitor; the 11-15 run predates that line (HDR work had not started: probably off). An SDR
  swapchain on an HDR desktop goes through Windows' SDR-to-HDR conversion and may lose independent flip. Also
  `display_vrr` 1 (11-15) -> 0 (now): no runtime effect at a 237 limit, maybe a G-Sync change.

## Next measurements (no builds)

0. The current exe with Windows HDR off, same spot, SDR. Back near 236 / 233 / 228: environmental.
1. Else the best-run recipe exe that was never run: `C:\Users\Ty\MediEvil\medievil_bestrun\out\gpu\medievil-runtime.exe`
   (e12ffaa, dev, 11-20 21:56), copied into the main `out\` next to the DLSS DLLs (`cp -p`), same conditions.
   As good as 11-15: a code regression; then copy-fault-trace `22744f1` release, then `21886b6` release and
   `step1-fa136ee` (history agent's plan in `history/commit-history-and-bisect.md`). As low as now: environment.
2. Repeat any deciding run (A/A) before trusting a difference under ~3% on RTSS lows.
3. PresentMon `PresentMode` in SDR and HDR (independent flip or composed).

**Result (the user, 2026-11-23, Windows HDR off):** the current build against the best-run recipe exe
(`medievil_bestrun`, e12ffaa dev): the gap shrinks to 1-2% on the 0.1% lows, repeatable on steady held views and when
hunting for the worst camera angle (best-run exe slightly better). Most of the earlier SDR gap was the HDR desktop
(Windows' SDR-to-HDR path). **Decision (the user):** too small to bisect reliably against run-to-run variance; turn to
the improvements and clean-up below, which likely cover it or make it irrelevant.

## Improvement candidates (beyond what was lost), ranked within each area

### Memory, transfers, synchronisation (memory agent; frame-stats run 2026-11-16)

1. **Guest write faults that need no GPU work still go through stage A with a full draw-pipe drain** (twin,
   finished-page, no-GPU-bytes and unmap-discard branches of `InvalidateRegion`, buffer_cache.cpp:950-1023, via
   `SendCommand<true>` :1055). Up to ~70-100/s in effect-heavy scenes, ~0.3-1 ms each on a guest thread [I]. Inline
   them; only real downloads keep the command. Targets 0.1% lows in combat. Risk medium (locking against stage B's
   `ObtainBuffer`).
2. **Dynamic read-only buffers > 16 KB through the arena**: each `Runtime::CopyBuffer` ends the render pass and adds
   a transfer barrier: 4-8.6 extra pass ends a frame. A stream-ring path (as the constant ring for <= 16 KB) could
   save ~0.05-0.4 ms GPU a frame if they split 4K passes [I]. Measure first (`GOW3_PERF_DIAG` same-target reopens,
   `GOW3_BUFFER_STATS`).
3. **HZB results published into guest memory out of habit**: 256 KB written to guest memory with two VirtualProtect
   flips a frame only for our own hook to read back (medievil_hzb.cpp:216-218). Serve the hook from the staging copy;
   optionally retire on the GPU signal to cut the 3-4 frame age. Risk low-medium.
4. **`ProcessDownloadImages` on every EOP** (220-330 a frame) takes two mutexes each time: a lock-free early-out when
   nothing is pending, ~0.05-0.2 ms of stage B a frame [I]. Risk low.
5. Copy-thread `CopySparseMemory` reading the guest mapping (buffer_cache.cpp:1983-1993, gow3gpu.cpp:178-189): read
   the backing (a stall / deadlock class). Grade and tonemapper blocks from the stage-A ring copy (robust under early
   frame EOP, LANDMINES 29). Staging idle trim by time, not 3000 frames (~13 s at 237 FPS). Drop the warning-only
   `IsMapped` (recursive lock with a heap allocation) from `UpdatePageWatchers` in release.

### Off-costs in the release build (off-cost agent; release exe disassembled)

1. **ObserveFrameMap with DLSS on** builds frame-map-only `Pass` fields and image descriptions per draw / dispatch
   (vk_rasterizer.cpp:5297-5318, 5413-5442): ~10-30 us a frame on stage B [I]. Trim. Risk low.
2. **Runtime C log lines go out one character at a time** on unbuffered stdout (`_IONBF`, probe.c:1337;
   `__mingw_printf` -> `fputc`), plus stage-B printfs (Stencil plane copy, Buffer residency): write storms at save
   menus, dialogs, loads. One write per line; stage-B lines async or dev-only. Check first with Process Monitor.
3. **Mouse reader**: ~670 wakes a second with 0 reports, and its `wakes` counter shares a cache line with the region
   table (`vma_seq` / `vma_count`, read on every region lookup): false sharing on a hot structure [V layout, I cost].
4. **LOG_DEBUG is not compiled out** (the gate went into shadps4/common/logging/log.h; every GPU file includes
   shim/common/logging/log.h first): `ShouldLog` calls from 881 sites remain; card `tooling/dev-tools-build` is wrong.
   Staging counters' unconditional `lock add` per staging request (vk_staging_buffer_pool.cpp:62); `gpu_frames`;
   many runtime-switchable dev diagnostics still in the exe (per draw / wait / frame static guards). A
   "release hygiene" bundle; expect no visible change alone.
5. Particle flip-guard summary every ~5 s in any default launch (only GOW3_FPS_LOG=0 silences it). Swapchain clear
   before a blit that covers it (GPU ~5-35 us). Guest mutex lock saves / restores xmm6-15 (ABI artifact, 3-5 ns per
   lock at 100-300k a second).

### HDR path (HDR agent)

- No CPU-GPU synchronisation anywhere in our HDR code (no fences, readbacks, waits) [V]; "a frame behind" has
  nothing to defer.
- Worst-frame hitches: every HDR pipeline is created synchronously at first use (first HDR frame, first Hilltop,
  first blur menu), and the overlay's HDR pipeline per UI-brightness value: pre-create when HDR turns on.
- The game's own HDR-only work per frame: a 4K copy of the display buffer into a scene texture (event 1058 in
  `hdr-final-step2-20261122/menu_frame11835.rdc`: draw, dispatch or DMA unknown) and a UI-target clear; ~0.1-0.25 ms a
  frame [I], consistent with H34's -1.7%. Bigger gains: our tonemapper writes the scene texture directly, or one fused
  tonemap + UI composite pass. Needs a RenderDoc study first (what event 1058 is; who reads the display buffer in
  between).
- The area solve (40 serial GradeScene calls + Newton in one fragment, ~50-200 us) as a parallel compute shader
  (~10-20 us), relevant for blend-volume levels if they rebuild often (unchecked: Inside the Asylum, Lake, Haunted
  Ruins).
- Small: Hilltop's level-LUT lookup and pass break before the rebuild-key check; `WriteGameLut` re-drawn every frame
  when ours declines; per-pipeline HDR role byte instead of four hash compares per draw; tonemapper loadOp DontCare
  when the rectangle covers the target.
- Correctness: make HDR automatic (report the capability only when Windows HDR is on for the window's monitor, e.g.
  DXGI `IDXGIOutput6::GetDesc1`); `RequestHdrCurve` checks the environment flags instead of the real output.
- Unchecked: whether the HDR10 swapchain keeps independent flip (PresentMon).

## Incidents during the research

- One helper agent ran `taskkill /F /IM grep.exe`, which kills every grep on the machine. Nothing was left running
  afterwards. Future briefs: never kill processes by image name.
- The fog research (2026-11-22) overlapped a game launch with a RenderDoc replay for a few minutes; replays since use
  a watchdog.
