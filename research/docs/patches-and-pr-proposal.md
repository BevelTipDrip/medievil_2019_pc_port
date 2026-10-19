# Patches on `lab` and a proposal for upstream pull requests

Status: written 2026-10-19 from the docs and `git log main..lab` (137 commits). Nothing here was run, built or pushed
for this write-up. Parent: [START-HERE.md](START-HERE.md). Rules for PRs: [pr-checklist.md](pr-checklist.md),
[pr-template.md](pr-template.md), [repo-boundaries.md](repo-boundaries.md).

## Executive summary

**When we started** (run 1, 2026-10-18, upstream 1.3.2 `783b25a`, CUSA11227 01.00, RTX 4090 + 7800X3D): at
`4K_native` the menus stopped responding part-way (I1); the frame rate swung between ~120 and ~50 FPS with the GPU
at 70-80% (I2: busy gameplay 51-54 FPS at 4K ultra, 64-70 FPS even at 1080p console); the game crashed now and then in
the NVIDIA driver (C1) or froze on a frame (H1); some shadows were drawn as large black boxes (V1).

**Now** (`lab`, 2026-10-19): the 4K menus work when the game boots at 4K (I1, the user verified). The crash and the
freeze have one cause, fixed (L5: no fault in the 19-minute test session or in any later session the docs record).
The heavy Graveyard view went from ~81 FPS (p99 16.7 ms, `wpr-graveyard`, already with stale readbacks) to
119-127 FPS with p99 8.8-9.8 ms and frame-time stddev 0.34-0.68 ms (T-D2b), through seven measured fixes (R2, R3, V3,
P1, M1, C3, D2). The box shadows are gone (V4). The user, after T-D2b: "Performance and micro-stutter in general are
VASTLY improved. The heavy area is where it's the worst, and even that's fairly acceptable right now."

Caveats on that comparison: the runs used different settings and routes over two days (run 1 was 4K ultra; most later
runs are 4K boot + `console` quality), and T-B1 to T-D2b ran without the 4K boot patch (see "Known corrections").
Each fix below is judged by its own like-for-like test, not by this end-to-end number.

**Upstream moved meanwhile** (`upstream/main` = `d4c9cf4`, 1.3.3-1.3.5). It fixed three of our areas in its own way:
the 4K menu bug (1.3.3, issue #11), the stage A / stage B recording race (1.3.4 / 1.3.5, issues #2 / #12 / #18) and
the 3D-volume rebuilds (1.3.4, the same idea as our V3). That changes the PR plan: V3 is dropped, I1 and L5 must be
re-tested on top of 1.3.5 before they are offered.

## How to read this

- **Confirmed working** = the docs record a named test with a result and/or the user's verdict. Each entry quotes it.
- **Toggle bits**: written as a decimal mask into the file named by `GOW3_TOGGLE_FILE` (re-read every 250 ms,
  `src/runtime_memory.c`); a set bit switches the fix **off** (restores the old path). Bits 32+ are the GPU-side
  `Gow3Toggle` enum in `gpu/shim/gow3_toggles.h`; bits below 32 are runtime-side.
- Logs are in `research/docs/data/` and cited by name; traces and samples are in `C:\Users\Ty\MediEvil\research-data\`.

### Known corrections (from plan-frame-pacing.md)

1. **Live toggles did not take effect in T-U1, T-D1b and T-D2b**: `run-game.ps1` did not set `GOW3_TOGGLE_FILE`, so
   the bit 62 A/B (T-U1), the bit 56 A/B (T-D1b) and the attempted bit 63 A/B (T-D2b) changed nothing. D2's evidence
   is therefore **across runs** (T-D1b vs T-D2b, same spot), not a live A/B. Earlier live A/Bs (T-V3 fog, T-V4,
   T-C3b, T-P2, T-L1) passed the file by `-Env` and did work (their logs show the old path returning).
2. **The 4K boot patch was missing from T-B1 to T-D2b** (logs show only `Native 4K framebuffer pool (512 MB)` in the
   `Patches:` line; I1 came back for the user). Comparisons inside T-B1 ... T-D2b are like for like; comparisons of
   those runs against T-L1 and earlier mix two boot modes.

## 1. Confirmed working changes

### Summary

| ID | Name | User-visible effect | Test | Commits | Off switch |
|---|---|---|---|---|---|
| I1 | Boot at 4K instead of a live `r.SetRes` | Menus navigable at 4K | T-I1b (+ log in `i2-profile-4k`) | `ebf85f4` | opt-in `GOW3_UE4_BOOT_4K=1` |
| L5 | Drain the draw pipe before every queued command | No freeze (H1), no driver crash (C1) | T-race, T-L5 session 1 | `bc0e1d6` | `GOW3_DRAW_PIPE=0` avoids the race; no switch for the fix |
| R2 | rwlock handles through a hash set | Higher FPS / lows in heavy views (with R3) | T-R23 | `cf9e201` | none (behaviour-identical) |
| R3 | Pool free-page search hint | (with R2) | T-R23 | `2097554` | none (behaviour-identical) |
| V3 | Volumes rendered through 2D array views | Map ~70 → ~200 FPS, heavy-view lows ~60 → ~100 | T-V3, fog A/B | `1f18217` | bit 58 |
| P1 | Dirty register blocks scanned by word | ~+8% heavy view | T-P1 | `5cd037e` | none (behaviour-identical) |
| M1 | Pool views kept across a decommit (+ M1b) | No 2-5 s pacing episodes | T-H1 → T-M1, T-M1b | `40a416a`, `3c5704e` | bit 31 |
| V4 | Stencil plane writes reach the depth image | Box shadows gone | T-V4, T-V4b, RenderDoc | `769de12`, `423efc7` | bit 59 |
| C3 | Undefined render targets skip the first upload | Torch-into-view spikes 19 → 7-12 ms | T-C3b (live A/B) | `1024224`, `d9949cb` | bit 61 |
| D2 | Precise CPU writes into shader-written memory | Regular microstutter gone | T-D2b (across runs) | `1bc2dda` | bit 63 |

### I1. Native 4K: start the game at 4K instead of a live r.SetRes

- **Problem**: with `render_resolution=4K_native`, the main menu and the options menu stop accepting input part-way
  ("you can't go down past the item that lets you see the controls"); the debug menu's right column is unreachable.
- **Cause**: the port changes resolution live (`r.SetRes 3840x2160` ~10 s in). UE 4.21 computes the display metrics
  and work area once, from VideoOut only when `r.PS4Allow4kOutput` is set; otherwise a cached 1920x1080. Widgets
  laid out for 2160p sit outside the 1080p hit-test grid, so gamepad navigation cannot reach them (mechanism inferred
  from UE4 knowledge; the cached rect and the cvar check were verified in the 01.00 eboot). T-I1a / run 3: menus work
  at `native` (no live change), so the live change is the trigger.
- **Fix**: boot like a PS4 Pro on a 4K TV: patch "4K display metrics at boot" (two `je` → `nop nop` at image
  0xb69032 / 0xb6d696, `patches/MediEvil.xml`, disabled by default), VideoOut reports 3840x2160, no
  `GOW3_UE4_SETRES` (`run.py`). `video_out.cpp`: warn when a second open is refused; `sceVideoOutClose` ignores an
  invalid handle instead of closing the main port.
- **Evidence**: T-I1b, the user: "that fixed the menu-bug for 4k output, it can be navigated now" (scripted run with
  `GOW3_UE4_BOOT_4K=1`; that run left no log because of a script bug). Log side confirmed later in
  `i2-profile-4k.log`: `Patches: CUSA11227; 13 writes from [... '4K display metrics at boot']`, no `Native output:
  r.SetRes`, no `second open refused`. Indirect confirmation: when T-B1 ... T-D2b lost the switch, the user saw the
  menu bug again.
- **Files**: `patches/MediEvil.xml`, `run.py`, `gpu/shadps4/core/libraries/videoout/video_out.cpp`.
- **Switch**: opt-in `GOW3_UE4_BOOT_4K=1` with `render_resolution=4K_native` (set by `research\tools\launcher.cmd`
  and `run-game.ps1`; the launcher UI has no setting for it).
- **Risks**: fixed 01.00 addresses (the patch is only valid for eboot sha256 `a7e976d1…3b828`; `patches.py` gates the
  pool patch by hash, this one rides on the same title profile). Only 4K is covered (not 1440p / 1800p native).
  **Upstream 1.3.3 fixed issue #11 differently** (keeps the live `r.SetRes`, reports `GOW3_INTERNAL_WIDTH/HEIGHT` at
  the output size). We have not tested upstream's fix; our eboot reading says the display metrics ignore VideoOut
  unless `r.PS4Allow4kOutput` is set, which would make upstream's change insufficient, but upstream reports it works
  on a 4K TV. To settle with one run of upstream 1.3.5 before proposing anything.

### L5. Liverpool: drain the draw pipe before every queued command

- **Problem**: rare freeze on a frame (H1, ~2 min idle in the Graveyard) and crash in `nvoglv64.dll` under
  `vkCmdDrawIndexed` on the recorder thread (C1, ~4 min; also run 2 at a level load).
- **Cause**: `Liverpool::ProcessCommands` drained stage B only if its first `num_commands` check saw a command. A
  command queued by a guest thread just after that check (a buffer readback with `Finish()`, a flip) ran on stage A
  beside stage B: two threads in the scheduler. Outcomes: host copies left in an unsent chunk (freeze), or a draw's
  buffer binds and its draw split across command buffers (null index buffer in the driver).
- **Fix**: drain before each queued command, as the Bloodborne port does since 0.3 (`4d4d208`,
  `liverpool.cpp`, attributed in the commit). The drain returns at once when stage B is idle.
- **Evidence**: T-race (`t-race-drawpipe-off.log`): with the two-stage pipeline off, the same idle scenario ran ~9 min
  clean, against a freeze at ~2 min and a crash at ~4 min with it on. T-L5 session 1 (`t-l5-pipe-on-2026-10-18.log`),
  pipeline on, ~19 min, no fault, no freeze; the user: "a pretty long run, no crashes". FPS per 5 s median 102.3,
  p99 median 13.5 ms (vs 85.2 / 16.8 ms pipe-off). Cost: ~37 drains/s, 0.1% of stage A. Every later session in the
  docs (P0 16 min, R1 A/B, T-R23 ... T-D2b) ran on this fix with no C1 / H1 reported.
- **Files**: `gpu/shadps4/video_core/amdgpu/liverpool.cpp` (12 lines).
- **Switch**: none for the fix itself; `GOW3_DRAW_PIPE=0` disables the two-stage pipeline (and the race).
- **Risks**: the roadmap still lists it as "one more session to call it verified"; the docs never formally upgraded
  it, though later sessions are clean. Revert of the interim L4 (`684cd8f` → `a07f709`) must not be carried
  separately: the pair cancels out. **Upstream 1.3.4 / 1.3.5 fixed the same race class differently**
  (`Scheduler::SyncWithDrawRecorder` before stage A records, and logs "recorded while the draw recorder had queued
  draws" when it cannot wait). The two are complementary: ours removes the window at its source in
  `ProcessCommands`; theirs catches any stage A recording. Applies cleanly on `upstream/main`.

### R2. Runtime: validate rwlock handles through a hash set

- **Problem**: part of the low FPS / 1% lows in busy views (game threads, not the GPU threads, were the limit).
- **Cause**: every PS4 rwlock call validated its handle by walking a list of all rwlocks ever created, under one
  global mutex (`find_locked`, `src/runtime_rwlock.c`): 11.8% of the game's main thread (WPR `graveyard.etl`).
- **Fix**: live rwlocks in an open-addressing pointer set (≤ half full, rebuilt at 4x live, private tombstone);
  same lock, holders and error codes.
- **Evidence** (with R3, one launch): T-R23 (`t-r23-retry.log`, trace `graveyard-r23.etl`), same Graveyard spot as
  `wpr-graveyard`, 2,600-3,100 draws/frame, 70 windows each: FPS median 81.2 → **94.7**, lowest window 71.3 → 80.7,
  p99 median 16.7 → 14.6 ms (**+17% / +13% / -13%**). `find_locked` gone from the trace (main thread game code 71.6% →
  81.9%). The user: "the 1% lows seem much much better. When looking at an area that previously dropped to 50-60 fps
  1% lows, it's closer to 80." New unit test `tests/test_rwlock_registry.c` passes on the new code and on the old file.
- **Files**: `src/runtime_rwlock.c`, `tests/test_rwlock_registry.c`.
- **Switch**: none (behaviour-identical data-structure change, covered by the test).
- **Risks**: R2 and R3 were measured together; their individual shares are only known from the trace. The first
  launch of that build exited silently with code 5 at boot (S1); it happened again later on a build without new
  boot-time code, so it is not attributed to R2 / R3, but it is unresolved.

### R3. Runtime: start the memory pool's free-page search at a per-chunk hint

- **Problem / cause**: every direct-memory pool commit scanned the page map byte by byte from page 0 under the global
  memory write lock (`pool_take_locked`, `src/runtime_memory.c`): 7.5% of the busiest game thread, 28.7% of another.
- **Fix**: per-chunk `free_hint` (all pages below it in use); same first-fit result.
- **Evidence**: T-R23 as above; trace: `pool_take_locked` 7.5% → 0.3% and gone from the pool-heavy thread. Checked
  against the old functions on 200,000 random takes / gives (identical runs and maps; scratch harness, not in repo).
- **Files**: `src/runtime_memory.c`. **Switch**: none. **Risks**: the equivalence harness is not in the repo; a PR
  should add it as a test.

### V3. Texture cache: render volumes through 2D array views instead of expanding

- **Problem**: frame rate fell the longer one played; map / level select ~70 FPS; GPU "80% but cool".
- **Cause**: UE4 renders 3D volumes (translucency lighting volumes, volumetric fog grid, colour grading LUT) slice by
  slice as a 2D array and samples them as 3D. `ResolveOverlap` expanded on every switch: a new image plus a full
  upload from guest memory, twice per use (T-C1: ~9,760 expands and ~31 GB uploaded per 5 s, ~6 GB/s over PCIe).
- **Fix**: a 2D render target with one layer per slice of a same-sized 3D image renders through a 2D array view of
  the volume; barriers / clears use layer 0 count 1 for volumes.
- **Evidence**: T-V3 (`t-v3-volume-views.log`): expands 0 per window; heavy view 97 → 115-123 FPS, uploads 31 GB →
  0-15 MiB per 5 s, GPU-tick waits 32% → 0.1-0.4%. The user: map / level select "~70 → ~200 FPS", heavy-view 1% lows
  "~60 → ~100". Fog A/B with bit 58 (`t-v3-fog-heavy.log`), the user: "I see no difference, fog is definitely
  present with the fix on".
- **Files**: `vk_runtime.cpp`, `texture_cache/image.cpp`, `image_view.cpp`, `texture_cache.cpp`,
  `gpu/shim/gow3_toggles.h`, `src/runtime_memory.c`. **Switch**: bit 58 (288230376151711744).
- **Risks**: validation-layer run never done; a partial-slice clear clears the whole volume (logged).
  **Superseded upstream**: 1.3.4 shipped the same mechanism (`GOW3_VOLUME_VIEWS`, ~4,700 → ~90 rebuilds per 5 s) and
  1.3.5 limited it to drivers with `VK_EXT_image_2d_view_of_3d` (AMD Polaris crash, #18). It conflicts with ours in
  `texture_cache.cpp`, `image_view.cpp` and `vk_runtime.cpp`. **Not proposed**; when `lab` is rebased, take
  upstream's version and check with `GOW3_TEXCACHE_STATS=1` that the 240x135x64 fog grid at `0x206c000000` no
  longer expands (upstream's comment names the lighting volumes and LUT, not the fog grid; their ~90 left per 5 s
  could be it).

### P1. regs: scan dirty register blocks by word instead of bit

- **Problem / cause**: after V3 the heavy view was bound by stage A; 13.1% of it in `std::bitset` test/ref. glibc's
  `bitset` has `_Find_first` / `_Find_next`, libc++ (our clang64 build) does not, so `PostDraw` tested all 1,664 bits
  per draw (~2,800 draws/frame).
- **Fix**: 26-word array with a `countr_zero` `ForEach` (also used by `RegDelta::Capture`).
- **Evidence**: T-P1 (`t-p1-bitset.log`), heavy view ~2,750 draws/frame: 114-123 (T-V3) → **124-131 FPS (~+8%)**. The
  user: "1% lows are improved". Across runs, same spot.
- **Files**: `gpu/shadps4/video_core/amdgpu/regs.h`, `renderer_vulkan/vk_rasterizer.cpp`. **Switch**: none
  (behaviour-identical). **Risks**: low; the gain matters on libc++ builds (Windows clang64); on glibc it should be
  neutral. Linux build not tried by us.

### M1 (+ M1b). runtime_memory: keep memory pool views across a decommit (Windows)

- **Problem**: every ~30 s a 2-5 s burst of unsteady frame times (FPS halving); a broken effect often visible then.
- **Cause**: UE4 commits / decommits 64 KiB pool pages ~6,000 times a second. Each decommit unmapped a view of the
  guest memory section and each commit mapped a new one: views grew to ~15,000, and the kernel's section trims
  (`MiTrimSection` / `MiViewMayContainPage` on a System worker for seconds) blocked `RenderThread 1` on a push lock
  (WPR `heavy-episode-1.etl`, `effect-2.etl`; `t-h1-hostmap.log`: 64 KiB maps taking 1-17 ms during an episode).
- **Fix**: M1 (`40a416a`): a decommit keeps the view (`PAGE_NOACCESS`), a commit at that address takes the same
  physical pages back and only changes protection. M1b (`3c5704e`): pages of kept views are handed out last, so the
  kept view is rarely stolen (each miss had cost an 8-37 ms unmap).
- **Evidence**: T-M1 (`t-m1-poolviews.log`): unmaps 0 per window, regions level off (~13,400), minutes of flat pacing
  (stddev 0.25-0.6 ms), **no 2-5 s episodes**. T-M1b (`t-m1b-kept.log`): views ~3,400 (was ~13,400), kept-view unmaps
  52 in the whole run (was ~50 per 5 s), 2 slow host calls (max 2.6 ms). The user on T-M1b: "no prolonged bursts any
  more" (the separate camera-pan hitch remained: that is UE4 streaming, see section 3).
- **Files**: `src/runtime_memory.c`. **Switch**: bit 31 (2147483648) restores the old path.
- **Risks**: Windows only; touches the core guest-memory mapping (a bug here corrupts guest memory or faults). The
  "first cause" of the trims (view deletion vs view count) was not separated. No long (1 h+) session recorded.

### V4. vk_rasterizer: apply shader writes to a depth buffer's stencil plane

- **Problem**: a black, semi-transparent box on the ground near Dan at light sources, moving with his idle animation
  (V1, "shadows drawn as a big box").
- **Cause** (RenderDoc `v1-shadow_frame24679.rdc`): GCN keeps a depth buffer's stencil in a separate plane at its own
  address; the texture cache's D32S8 image covers only the depth plane. UE4 clears the stencil by a compute fill of
  that plane, which never reached the image, so the stencil kept the base pass's 0x80 and the moon's per-object
  shadow projections (stencil != 0 inside their volume) darkened their whole box.
- **Fix**: each depth bind records its stencil plane; a compute dispatch that rewrites a whole known plane (must start
  at the plane and be within 1/16 of its size, `423efc7`) marks it; the next bind copies the plane into the image's
  stencil aspect (`Runtime::CopyBufferToStencil`). Exact for uniform fills; other contents are logged
  ("Stencil plane ... rewritten by a shader").
- **Evidence**: T-V4 (`t-v4-stencil.log`): box gone with V4, back with bit 59, live A/B twice. First version matched
  too widely (`423efc7` fixed the plane check); T-V4b (`t-v4b-stencil.log`): box gone. RenderDoc with V4 on
  (`v4-shadow_frame14784.rdc`, `_frame17750.rdc`): the shadow mask holds a clean silhouette of Dan, sword and grass,
  no box. The user's "dynamic torch shadows missing with V4" was explained by the capture: the torches are unshadowed
  point lights; the "torch shadows" were the leak itself. The docs do not record the user accepting that explanation
  in words.
- **Files**: `renderer_vulkan/vk_rasterizer.cpp/.h`, `renderer_vulkan/vk_runtime.cpp/.h`, `gpu/shim/gow3_toggles.h`.
- **Switch**: bit 59 (576460752303423488).
- **Risks**: non-fill stencil contents (a shader writing patterns) would need detiling, not done; only seen with
  MediEvil's clears. Adds a buffer→stencil copy per bind after a rewrite.

### C3. texture_cache: targets made where an unmap freed images skip the first upload

- **Problem**: a frame-time spike every time the first torch comes into view.
- **Cause**: UE4's render-target pool decommits a pass's targets while unused; `UnmapMemory` frees the images; the
  next binding creates new ones, which start dirty and upload their (undefined) guest memory: per torch a 64 MiB
  RGBA16F + 32 MiB RGBA8 4K pair, ~33 ms of image refresh in the worst frame (T-P2, T-R1 `t-r1-refresh.log`).
- **Fix**: images created at an address an unmap freed are flagged `Undefined`; a render / depth target binding drops
  the flag and the dirty bit (no upload); any other first use uploads as before. `d9949cb`: the discard updates the
  flag without the texture cache mutex (the first version, T-C3, hung at boot: callers already held it).
- **Evidence**: T-C3b (`t-c3b-undefined.log`), the user: "Massive improvement." Torch-test windows skip 64-288 MiB of
  uploads per 5 s; worst frames 7-11 ms (a few 15-18) vs 19-20 ms in T-R1. **Live A/B** (03:57:46 off, 03:58:30 on):
  off, the pair re-uploaded 3 times per window, worst frames 19.2 ms; on, no re-uploads, 7-12 ms. The user: torches,
  glow and haze look as before.
- **Files**: `vk_rasterizer.cpp`, `texture_cache/image.h`, `texture_cache.cpp/.h`, `gpu/shim/gow3_toggles.h`.
- **Switch**: bit 61 (2305843009213693952).
- **Risks**: keyed by address only (set capped at 16,384 entries); a game that writes a render target's initial
  contents from the CPU into freshly committed memory, then loads it as a target, would lose that data. Not seen in
  MediEvil.

### D2. gpu: guest libc writes into shader-written memory without a GPU wait

- **Problem**: regular microstutter ("a lot of little tiny spikes all of the time"), worst in the heavy
  long-distance view.
- **Cause**: T-SF1 (`t-sf1-slow-frames.log`): slow frames wait ~2.5 ms on GPU ticks, almost all at
  `BufferCache::DownloadMemory` → `scheduler.Finish()`. T-D1b (`t-d1b-unmap-discard.log`): every download is a guest
  **write** fault, 25.8-27.4/s, ~4 KiB each, ~73 ms of GPU waits per second. T-D2 (`t-d2-fault-sites.log`): the
  writers are the runtime's `guest_memcpy` / `memset` (the game's libc) writing into six pages a shader wrote.
- **Fix**: the runtime keeps a bitmap of shader-written pages (set where `ObtainBuffer` marks a written binding
  GPU-modified, cleared on download or unmap). `guest_memcpy` / `memset` / `memmove` check the destination's bits; a
  hit hands the bytes to `BufferCache::PreciseCpuWrite`: written into guest memory through the backing view (no
  fault) and queued; the recording thread copies exactly those bytes into the GPU arena before the next draw or
  dispatch. No download, no `Finish()`. Idea after the Bloodborne port's `NotePreciseUpload` (no code taken).
- **Evidence**: T-D2b (`t-d2b-precise-writes.log`): synchronous buffer downloads **27/s → 0** for the whole run;
  ~130-200 precise writes/s (1.4-1.7 MB/s). Heavy long-distance view (2,800-3,000 draws/frame), T-D1b → T-D2b: FPS
  114-121 → 119-127, median 7.7-8.2 → 7.9-8.3 ms, **stddev 1.1-1.5 → 0.34-0.68 ms, p99 10.9-13.0 → 8.8-9.8 ms**;
  light views stddev down to 0.08 ms. The user: "The 1% lows are much more stable looking at the heavy long distance
  area, a significant improvement. The frametime graph in general looks significantly better, even outside of that
  area." No broken visuals reported.
- **Files**: `videoout/driver.cpp` (stats), `buffer_cache/buffer_cache.cpp/.h`, `vk_rasterizer.cpp/.h`,
  `gpu/shim/gow3_toggles.h`, `gpu/shim/gow3gpu.cpp`, `src/runtime.c`, `src/runtime.h`, `src/runtime_memory.c`,
  `tests/test_gpu_runtime_stubs.cpp`.
- **Switch**: bit 63 (9223372036854775808).
- **Risks**: **no live A/B happened** (correction 1): the evidence is across two runs at the same spot, both without
  the 4K boot patch. Adds a bitmap check to every guest `memcpy` / `memset` / `memmove` (one load when the hook is
  set); reserves and commits a 32 MiB bitmap (`VirtualAlloc`, Windows API in `runtime_memory.c`: Linux build to
  check). Ordering between the queue, downloads and guest memory is argued in the doc, not tested beyond the game.
  Writes through other paths (guest code storing directly, e.g. guest+0x20da4d0) still fault and download. Built on
  top of two diagnostic commits (`057afc6`, `537d86e`) and needs to be rewritten without them for a PR.

## 2. Not confirmed, or not a fix

### Kept, but no measured gain

| ID | What | Commit | Switch | Why no gain is recorded |
|---|---|---|---|---|
| B1 | Targeted host-copy waits: `WRITE_DATA` waits only for copies reading its range (Bloodborne #1, from its `daa7165`) | `a2a81fe` | bit 62 | T-B1 (`t-b1-targeted-waits.log`): 1 of ~115-123 waits per frame skipped; WRITE_DATA is not where MediEvil's waits are. The T-U1 bit 62 A/B never took effect (correction 1). The user saw longer spikes in T-B1 (as with P2); the log has no spike-length measure. Unresolved. |
| D1 | Guest unmaps drop GPU-written buffer data instead of downloading it | in `057afc6` (mixed with diagnostics) | bit 56 | T-D1b: "unmaps discarded 0.0"; the path never ran. Correct and free, nothing measured. |

### Opt-in or partial

| ID | What | Commit | Switch | State |
|---|---|---|---|---|
| P2 | Skip the end-of-pipe host-copy wait when there is no readback work | `060743e`, made opt-in in `e385484` | `GOW3_IDLE_EOP_SKIP=1`, bit 60 | T-P2 (`t-p2-eopskip.log`): ~+2% in the heavy view, but the user: "stutter amplitude much higher" (worst frames 29-42 ms vs ≤ 19 ms). Off by default. Re-test after C3 (the spikes it deepened were C3's torch uploads) never done. |
| L1-dump | Minidump on a fatal fault, with fallback dump types | `b01661e`, `5247265` | `GOW3_CRASH_DUMP=0` / `=full` | On by default. Injected-fault test passed (193 MB dump opens in cdb). The one real crash since (`i2-profile-4k-2.log`) made the first version fail (`0x80070057`); the fallback was added after and has not seen a real crash. Draft record: [pr-crash-dump.md](pr-crash-dump.md). |
| UE4 cvars | `GOW3_UE4_CVARS="name=value;…"` through the override pak | `b2f9d26` | opt-in | A tool. Used for R4a (rejected) and S1 / S2. |
| S2 | Larger UE4 memory / streaming pool for 4K (`GOW3_DMEM_MB=7104`, `r.GarlicHeapSizeMB=4600`, `r.Streaming.PoolSize=1600`) | test script only (`168ba15`) | `run-game.ps1 -Ps4Memory` | T-S2 (`t-s2-pool.log`), the user: small pans no longer re-trigger the stutter, larger pans still do. Lives only in `run-game.ps1`; sizing the pool with the render resolution in `run.py` / `ue4_override.py` is not done. |

### Reverted or rejected

| ID | What | Commits | Why |
|---|---|---|---|
| L4 | Re-kick chunks while waiting for host copies | `684cd8f`, reverted `a07f709` | Symptom fix for H1; under the race it could turn the freeze into the crash; L5 removed the cause. |
| R1 | Skip the EOP host-copy wait in stale-readback mode | `e384aa1`, reverted `6b86c75` | Stage B -20% but no FPS gain (R1 A/B), and it caused V2 (torches as black flickering boxes; bisect V2a / V2b). Do not retry. |
| L1 | Region caches valid per 16 MiB bucket | `0705e49`, reverted `bc60b8b` | T-L1 (`t-l1-buckets.log`): live A/B 123-125 vs 120-126 FPS, the user: "roughly the same". |
| R4a | `r.NumBufferedOcclusionQueries=2` | config only | `r4a-occlusion2.log`: spin fell, heavy view 94 → 75 FPS. Rejected. |
| F1 | Frames queued 2 | not built | Declined by the user ("buffering extra frames isn't going to get us what we need"). |

### Diagnostics only (no behaviour change unless their switch is set)

| Switch / output | Commits | Upstream value |
|---|---|---|
| `GOW3_SAMPLE_FILE` also samples `gow3:VkRecorder` | `151dadb` | small, harmless; could go with a diagnostics PR |
| `GOW3_WAIT_SITES=1`: every guest wait / sleep / yield per call site, incl. event queues | `9f088f1`, `9380965`, `a2cb3a3`, `ff78bdf` | useful, but touches the pthread layer hot paths; lab only unless asked |
| `GOW3_TEXCACHE_STATS=1`: per 5 s cache activity, expands, re-creations, re-uploads | `c4c74f6`, `4035556`, `9a3127a`, part of `e385484` | overlaps upstream's new `GOW3_IMAGE_STATS` (1.3.4): lab only |
| `GOW3_HOST_MAP_STATS=1`: host map / unmap / protect calls and slow ones | `2618b9c` | lab only |
| `GOW3_PERF_DIAG=1`: host-copy waits by caller, render passes ended by caller, write-fault downloads by site | `338e0a0`, `fde3624`, `537d86e` | lab only |
| `GOW3_SLOW_FRAMES=<x>`: what slow frames did vs samples | `7d3d288` | lab only |
| Frame stats: longest run of slow frames; image / buffer upload MB/s; downloads by cause | `90d7604`, `55146e6`, part of `057afc6` | small additions to the existing pacing line; could be offered later, gated |
| AvPlayer call trace | `6fd8e15` | lab only; found the intro-movie bug below |
| `GOW3_RENDERDOC` in `run.py` (run under `renderdoccmd capture`) | `d560f43` | small dev convenience; optional |

### Findings worth reporting upstream as issues (no fix on `lab`)

- **The intro movie keeps decoding into gameplay**: `gameintro.mp4` is started at ~19 s and never stopped or closed
  (T-U1 `t-u1-uploads.log`); its NV12 frames are decoded and uploaded at 30/s for ~200 s (~85% of all texture
  re-upload bytes, ~90 MiB/s), well into gameplay. `AvPlayerSource::Stop` logs "already stopped" at start.
- **Occlusion queries are always "visible"** (`liverpool.cpp:1129`, synthetic ZPASS counts): UE4 draws everything in
  the frustum, and its render thread spin-waits (`Sleep(0)`, ~1.3 M calls/s) until stage A has decoded the queries.
  Real query results are a larger item.
- **S1**: two silent exits with code 5 at boot in ~40 launches (`t-r23.log`, `t-d1-unmap-discard.log`); cause unknown.
- Multi-second "hitches" with no GPU work are the Load Game dialog (the game presents nothing while the port's
  SaveDataDialog is open), not gameplay stalls.

## 3. What stays out of upstream

- **`research/`** (docs, logs, traces, Ghidra / RenderDoc / WPR scripts): never pushed (repo-boundaries.md; the fork
  is public).
- **Local test scripts**: `research/tools/run-game.ps1`, `launcher.cmd`, `tee_run.py`, `wpr-*.ps1`, the S2 memory
  defaults that live in `run-game.ps1`.
- **Lab-only diagnostics** listed above, unless a PR needs one counter to prove itself; then a minimal one in the
  existing `GOW3_FRAME_STATS` style.
- **Reverted pairs** (`684cd8f` / `a07f709`, `e384aa1` / `6b86c75`, `0705e49` / `bc60b8b`): history only.
- **V3**: superseded by upstream 1.3.4 / 1.3.5.
- **B1, D1, P2** until a test shows a gain without the side effects.

**Bloodborne-derived code.** `bc0e1d6` (L5) is Bloodborne 0.3's `liverpool.cpp` change; `a2a81fe` (B1) ports
Bloodborne 0.4's targeted waits; D2 borrows only the idea of `NotePreciseUpload`. The Bloodborne port's files carry
`SPDX-License-Identifier: GPL-2.0-or-later` and its README says "GNU GPL v2 or later" (repo-boundaries.md says
"GPL-2.0"; the files say "or later"), the same licence as this port, which itself forked from Bloodborne v0.2. So
its code may go upstream: keep the shadPS4 / Bloodborne copyright headers, name the Bloodborne commit and files in
the commit message and the PR record's Attribution section (pr-checklist.md). Nothing from MGS2-Lab or the MGS4 tool
is in any of these changes (they were read for the busy-wait idea only, which was not built).

## 4. Pull request proposal

All PRs: one `feat/<topic>` branch cut from `main` fast-forwarded to `upstream/main` (`d4c9cf4`), the change
re-applied by hand or cherry-pick and squashed, a `pr-<topic>.md` record from [pr-template.md](pr-template.md), clean
CLANG64 build, `bash build.sh --test` (12 Python tests already fail on clean upstream: GoW3 patch fixtures, Linux
packaging, launcher versions; note them, do not fix them in these PRs), and an in-game check **on 1.3.5** by the user
before the go. Upstream now defaults to 60 FPS: performance checks need `engine_fps` 120 or 240 to show anything.

Rebase check method: `git log main..upstream/main --stat`, `git diff main upstream/main --stat` (27 files), a
simulated merge of `lab` into `upstream/main` (`git merge-tree --write-tree`: conflicts in `vk_runtime.cpp`,
`vk_scheduler.cpp`, `image_view.cpp`, `texture_cache.cpp`, `run.py`), and per-commit `git apply --check -3` against
`upstream/main` in a scratch index. Files upstream touched in 1.3.3-1.3.5 that our fixes also touch:
`videoout/driver.cpp`, `vk_rasterizer.cpp`, `vk_runtime.cpp/.h`, `vk_scheduler.cpp/.h`, `vk_draw_pipe.h`,
`texture_cache.cpp/.h`, `image.cpp`, `image_view.cpp`, `run.py`, `patches/MediEvil.xml`, `scripts/ue4_override.py`.

Order: visible bugs and crashes first, then frame pacing by value / risk, then memory, then tooling.

| # | Title | Commits | Depends on | Rebase onto `d4c9cf4` | Conflicts |
|---|---|---|---|---|---|
| 1 | Liverpool: drain the draw pipe before every queued command | `bc0e1d6` | none | applies cleanly | none textual; overlaps 1.3.4's `SyncWithDrawRecorder` in purpose |
| 2 | Depth buffers: apply shader writes to the stencil plane | `769de12` + `423efc7` (squash) | none | yes | `vk_runtime.cpp` (adjacent to 1.3.4's `ClearVolumeSlices`), `vk_rasterizer.cpp/.h`, `gow3_toggles.h` (lab-only neighbouring bits): small, by hand |
| 3 | Texture cache: render targets made where an unmap freed images skip the first upload | `1024224` + `d9949cb` (squash), without the stats | none | yes | `texture_cache.cpp/.h` (1.3.4 changed `FindImage`, `FindDepthTarget`, GC); the commit also leans on lab stats (`CACHE_STAT`, `unmapped_recently`): rewrite needed |
| 4 | Buffer cache: guest libc writes into shader-written memory without a GPU wait | `1bc2dda`, rewritten without `057afc6` / `537d86e` diagnostics | none | yes | `driver.cpp` (stats line), `buffer_cache.cpp/.h`, `gow3_toggles.h`: by hand |
| 5 | Runtime: validate rwlock handles through a hash set | `cf9e201` | none | applies cleanly | none |
| 6 | Runtime: memory pool free-page search starts at a per-chunk hint | `2097554` (+ the equivalence check as a test) | none | applies cleanly | none |
| 7 | regs: scan dirty register blocks by word | `5cd037e` | none | applies cleanly | none |
| 8 | Runtime memory (Windows): keep pool views across a decommit | `40a416a` + `3c5704e` (squash), without `2618b9c` stats | none (6 touches the same file, different functions) | yes | `src/runtime_memory.c` context from the lab diagnostic `2618b9c`: by hand |
| 9 | Native 4K: boot at 4K (only if 1.3.5's #11 fix is not enough) | `ebf85f4` | upstream test first | yes | `run.py`: 1.3.3 rewrote the same block (eboot check, `GOW3_INTERNAL_WIDTH/HEIGHT`) |
| 10 | Windows: minidump on a fatal fault | `b01661e` + `5247265` (squash) | none | applies cleanly | none |
| (11) | Optional: `GOW3_UE4_CVARS`; `GOW3_RENDERDOC`; issue reports (intro movie, occlusion queries, S1) | `b2f9d26`, `d560f43` | none | clean | none |

### PR 1. Liverpool: drain the draw pipe before every queued command

- **Why first**: crash / freeze, 12 lines, the root cause, attributed Bloodborne code.
- **Framing for upstream**: their 1.3.4 / 1.3.5 work catches stage A recording while stage B has queued draws, and
  logs when it cannot wait ("Scheduler: recorded while the draw recorder had queued draws", under the texture cache
  lock). Ours closes the window where such commands start: `ProcessCommands` checked `num_commands` once.
- **Verify**: on 1.3.5 without the PR, Graveyard idle 15+ min with the two-stage pipeline on: count "recorded while
  the draw recorder had queued draws" / "landed in another chunk" lines; with the PR, the same run should show none
  and the `Draw pipe:` line's drains by reason show `commands` ~tens per second (ours: ~37/s, 0.1% of stage A).
  No fault, no freeze.
- **Risk**: low. If 1.3.5 alone already shows zero such lines over long runs, offer it as hardening with the analysis.

### PR 2. Depth buffers: apply shader writes to the stencil plane

- **Verify**: Dan standing near a torch in the Graveyard start area (the user's persistent repro): no black box on the
  ground; toggle bit 59 brings it back. Log: "Stencil plane ... rewritten by a shader" only for non-fill writes (none
  expected in MediEvil). A RenderDoc capture of the shadow mask (as in `v4-shadow_frame14784.rdc`) is the strongest
  proof for the record.
- **Risk**: medium-low; it changes stencil contents for every game using this port's depth path, only after a
  whole-plane rewrite.

### PR 3. Render targets made where an unmap freed images skip the first upload

- **Verify**: start area, swing the camera so the first torch enters the view repeatedly. With `GOW3_FRAME_STATS=1`
  the stall / pacing lines show the worst frame per window (ours: 19-20 ms → 7-12 ms) and image upload bytes drop
  (64 + 32 MiB per torch before). Bit 61 A/B live. Check torches, glow, heat haze unchanged.
- **Interaction**: upstream 1.3.4's GC now follows the driver's memory budget; that frees images by budget, not by
  unmap, so it should not mark targets Undefined, but the rewrite must keep the flag tied to `UnmapMemory` only.

### PR 4. Guest libc writes into shader-written memory without a GPU wait

- **Highest value, highest risk** of the pacing fixes. Live A/B of bit 63 with the 4K boot: **done, T-D2c**
  (2026-10-19; the user: "D2 definitely stays in"). Include **D2b** (`42a4943`): T-D2c showed D2 alone left the
  bytes it wrote marked GPU-only, so the game's inlined copies (guest+0x68e109) downloaded 5-10/s; with D2b, T-D2d:
  heavy view 0 downloads, stddev 0.32-0.48 ms, p99 8.6-9.5 ms, 127-130 FPS (the user: "the frametimes look great").
- **Verify**: heavy long-distance Graveyard view. The PR should keep one counter: synchronous buffer downloads per
  second (lab: "Buffer downloads/s (GPU waits)", ~27/s → 0) and "precise CPU writes N (MB)"; plus frame-time stddev
  and p99 from the pacing line (lab: 1.1-1.5 → 0.34-0.68 ms, 10.9-13.0 → 8.8-9.8 ms). Watch for wrong effects.
- **Must check**: Linux build (`VirtualAlloc` in `runtime_memory.c`; use the file's existing portable allocation if
  it has one), and the cost of the bitmap check on every guest `memcpy`.

### PR 5, 6. Runtime: rwlock hash set; pool free-page hint

- Two PRs (one change each), sharing the T-R23 evidence: +17% median FPS, +13% lowest window, -13% p99 in the heavy
  view, `find_locked` and `pool_take_locked` gone from the WPR trace.
- **Verify**: `tests/test_rwlock_registry.c` (PR 5); add the 200,000-operation equivalence check as a test (PR 6).
  In game, the heavy-view FPS / p99 from the pacing line; a WPR trace if the maintainer wants the hotspot view.
- **Risk**: low; no behaviour change, no switch needed.

### PR 7. regs: scan dirty register blocks by word

- **Verify**: heavy view FPS with `GOW3_FRAME_STATS=1` (ours: ~+8%, 114-123 → 124-131 FPS); the gain is specific to
  libc++ builds. No switch: identical behaviour.

### PR 8. Runtime memory (Windows): keep pool views across a decommit

- **Verify**: steady play several minutes in a level: no 2-5 s bursts of unsteady frame times; the PR should carry a
  minimal counter (host views live, kept / reused per report) in place of the lab's `GOW3_HOST_MAP_STATS`. Bit 31
  restores the old path.
- **Risk**: medium-high (core memory mapping, Windows only). Last of the fixes; ask the user for one long session
  (30+ min) before proposing.

### PR 9. Native 4K boot (conditional)

- First, one run of upstream 1.3.5 at `4K_native` with the DualSense: main menu, options, debug menu. If navigation
  works, drop this PR (offer only the `video_out.cpp` hardening: refuse-warning and invalid-handle close, as its own
  tiny PR, if the maintainer wants it). If it does not, propose the 4K boot as an alternative to the live change,
  rebased onto 1.3.3's eboot check in `run.py`.
- **Verify**: log `Patches: ... '4K display metrics at boot'`, no `Native output: r.SetRes`, no `second open refused`;
  menus navigable at 4K.
- Related idea (the user's, not started): replace fixed 01.00 addresses (r.SetRes, 4K pool) with pattern scans.

### PR 10. Minidump on a fatal fault

- Record exists ([pr-crash-dump.md](pr-crash-dump.md)). Missing proof: `build.sh --test`, a real crash through the
  fallback path. Upstream 1.3.2 added crash reports and 1.3.3 / 1.3.4 added scheduler diagnostics with backtraces;
  a dump complements them.

### Not proposed

V3 (upstream has it), B1 / D1 / P2 (no gain or worse stutter), the reverted changes, the diagnostics, `research/`.

One note on V3, an inference to check, **not a tested result**: our variant restricts every 2D array view of a 3D
image to attachment use (`image_view.cpp`, `eColorAttachment | eAttachmentFeedbackLoopEXT`) and samples the volume
through its own 3D view. Per the Vulkan spec, an attachment-only 2D array view of a 2D-array-compatible 3D image needs
no `VK_EXT_image_2d_view_of_3d`, which is the condition 1.3.5 falls back on for issue #18 (sampled / storage usage on
such views, undefined without the extension). So it *should* avoid that crash, but it has never run on AMD (or Intel)
hardware, and the V3 validation-layer run is still pending. Not checked either: whether
`eAttachmentFeedbackLoopEXT` is a valid usage for these views without the extension, and our volume-slice barriers and
clears on other drivers. Before telling upstream anything: run the validation layer (`run-game.ps1 -Validation`) with
V3 on, and ideally `GOW3_NO_2D_VIEW_OF_3D`-style testing on an AMD GPU (a tester with one).

## 5. Inconsistencies and gaps found in the docs

1. **Revert hash**: plan-frame-pacing.md (T-L1) says L1 was reverted in `47735b3`; the revert on `lab` is `bc60b8b`.
2. **Bit 62 used twice**: L1 (`0705e49`, reverted) and later `TargetedHostCopyWaits` (B1). Only B1 remains; old logs
   that mention bit 62 before T-B1 mean L1.
3. **R1 marked "Verified"** in the plan-frame-pacing.md Stages table ("no visual issues (the user)"), but R1 later
   caused V2 and was reverted. The table was not updated.
4. **plan-roadmap.md Progress table is stale**: no entries for R2, R3, V3, P1, M1, V4, C3, D2; I2 still says "R1 (L6)
   built, A/B next"; L1 says "uncommitted" in plan-baseline-issues.md though it is committed.
5. **START-HERE.md** "Current state" is from 2026-10-18 morning: says `research/` is not committed (it is, `a2be9fb`),
   build "not run in game yet", I2 as open; its rules line and CLAUDE.md call `origin` the public upstream
   (bc160201983), while repo-boundaries.md and the state table say `origin` is the BevelTipDrip fork and `upstream`
   is bc160201983. The handoff covers only 2026-10-18; no handoff for 2026-10-19.
6. **Licence**: repo-boundaries.md and START-HERE.md call the Bloodborne port "GPL-2.0"; its files and README say
   GPL-2.0-or-later.
7. **L5 status**: still "one more session to call it verified"; later clean sessions exist but were never recorded
   against it.
8. **I1 log gap**: the run the user verified left no log; log evidence comes from a later run (`i2-profile-4k`). Fine,
   but the PR record must say so. Upstream's different fix for the same issue is not mentioned anywhere in our docs.
9. **D2 has no live A/B** (correction 1), and its comparison runs lacked the 4K boot (correction 2).
10. **V4**: the user's in-game confirmation is "box gone" in the live A/B; the conclusion that missing "torch shadows"
    were the leak rests on RenderDoc, without a recorded user verdict on it.
11. **V3**: validation-layer run planned, never done (now moot for upstream).
12. **Commit messages vs docs**: D2's commit message says each download cost "~3 ms"; T-D1b says ~73 ms of waits per
    second at ~27/s (≈ 2.7 ms), T-SF1 says up to 5.8 ms each: consistent, but the PR should quote the measured
    range, not a round figure.
13. **Untested platforms**: no Linux build of any lab change; no AMD / Intel GPU runs (upstream's #18 shows AMD
    drivers differ on exactly the image-view rules V3 / V4 sit near).
