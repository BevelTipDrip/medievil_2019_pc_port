# Plan: frame pacing (I2): stage B's waits and syscalls

Status: started 2026-10-18 (the user: "let's get moving on these performance fixes"). Parent:
[plan-roadmap.md](plan-roadmap.md); issue I2 in [plan-baseline-issues.md](plan-baseline-issues.md). Read
[gpu-threading-reference.md](gpu-threading-reference.md) before changing anything. Upstream PRs later (the user,
2026-10-18), structured per [pr-checklist.md](pr-checklist.md).

## Baseline (build `bc0e1d6`, race fix L5)

- Stats: T-L5 session 1, Graveyard idle, 4K boot, console, pipeline on
  ([data/t-l5-pipe-on-2026-10-18.log](data/t-l5-pipe-on-2026-10-18.log)): FPS per 5 s min 34.8 / p10 91.4 /
  median 102.3 / p90 133.4; frame-time stddev median 1.95 ms; p99 median 13.53 ms, max 39.68 ms.
- Profile (mixed session, build `684cd8f`, `research-data\samples\i2-profile-4k-2`): stage B 15.3% in
  `RunEventWriteEop` → `ProcessDownloadImages` yielding (6.7% in `WaitHostCopies`' loop, the rest likely
  `Gow3Copy::WaitAsync`); ~12% image churn `ResolveOverlap` → `FreeImage` → `VirtualProtect`; 5% flip GPU wait.

## Lead R: render-target readbacks (first)

Read (2026-10-18):
- `ProcessDownloadImages` (texture_cache.cpp:161-239, deferred-readback mode, the default `deferred_readback=1`) runs
  at EOP on stage B (liverpool.cpp:268, 348) and from stage A (vk_rasterizer.cpp:3147, drains first). It starts with an
  unconditional `scheduler.WaitHostCopies()` ("upload workers must finish reading RAM before these pages become
  inaccessible"), then records a download per queued image and write-protects its pages (`WatchReadback<true>`)
  until the copy is retired.
- What gets queued: storage images (`FindTexture`, :1030) and, with `readback_linear_images`, **every linear render
  target used** (`FindRenderTarget`, :1057-1060). MediEvil's profile sets `GOW3_READBACK_LINEAR=1` (run.py:179,
  game_profiles.py: "otherwise surfaces turn black"). So render targets are likely read back every frame whether the
  CPU reads them or not.
- The wait cannot simply be skipped when the queue is empty (likely rarely empty), and must not move under
  `readback_mutex`: a copy worker faulting on a watched page resolves through `ResolveReadbacks`, which takes that
  mutex (deadlock).

Measure first (P0): `GOW3_PERF_DIAG=1` prints every 5 s the readback profile (queued, completed, waited, canceled,
pending MiB) and the busiest images with **copies vs CPU accesses while pending**, plus readback sources and waits.
Images copied every frame with 0 CPU accesses are wasted readbacks.

Fix directions, by what P0 shows:
1. Read back only images the CPU actually touches (e.g. learn per image address: after N copies without a CPU
   access, stop queuing it; a later CPU fault on its pages downloads on demand through the existing path). Keeps the
   "surfaces turn black" fix for images the game reads.
2. If the waits are mostly `Gow3Copy::WaitAsync`: wait only for copies whose sources overlap the pages being
   protected (Bloodborne's targeted host-copy waits, plan-gpu-threading.md #1, `daa7165`).
3. Move readback recording off stage B (Bloodborne's readback queue, #2).

### The launcher's "GPU data" settings (read 2026-10-18, the user asked)

| Launcher | Setting / env | MediEvil value | Effect (code) |
|---|---|---|---|
| Deferred GPU readbacks | `deferred_readback` → `GOW3_DEFERRED_READBACK` (texture_cache.h:446-449) | on | Image downloads recorded in a batch, published when the GPU passes them, pages watched meanwhile; a CPU touch waits for that copy only (`ResolveReadbacks`). Off: `scheduler.Finish()` per batch (texture_cache.cpp:240-257). |
| Game reads GPU data without waiting | `stale_readback` → `GOW3_STALE_READBACK` (texture_cache.h:450-457), needs deferred | **off** (MediEvil's `ac900f8` changed the launcher default from on, no reason given; launcher hint: "switch it off if lighting or objects look wrong") | `WatchReadback` (texture_cache.cpp:80-86): off = pages read- and write-protected, a CPU read of a pending image faults and waits for the GPU; on = only writes blocked, reads see the copy published last (one frame stale). Byte rule (:108-114): a write outside a small copy's bytes on the same page does not wait. GoW3 (`cc3563a`): 40.9 → 66.5 FPS in its reference scene. `scripts/graphics_options.py:17` still defaults it to 1 when the ini lacks the key (one of the pre-existing test failures). |
| GPU readbacks (Relaxed / Off / Precise) | `GOW3_READBACKS` 1/0/2 (emulator_settings.h:30) | Relaxed | **Buffers**, not images. Relaxed: a CPU touch of a GPU-written region downloads it first (memory_tracker.h:85, region_manager.h:163). Precise: also read-protects pages when the GPU marks them (region_manager.h:106). Off: GPU buffer writes never reach the CPU. |
| Two-stage GPU pipeline | `GOW3_DRAW_PIPE` | Auto | Stage A / B split; stable since L5. |
| (not in the UI) | `GOW3_READBACK_LINEAR=1` from the game profile | on | Every linear render target used is queued for download (`FindRenderTarget`, texture_cache.cpp:1057-1060): "otherwise surfaces turn black". |

Each watch / unwatch is a `Protect` → `VirtualProtect`, so the per-frame render-target readbacks are likely part of
stage B's ~11% `VirtualProtect` too, not only `FreeImage`.

### P0 run setup (2026-10-18, the user: both in one launch)

`medievil.ini` backed up to `medievil.ini.bak-before-p0`, then **`stale_readback=0` → `1`** (byte-level edit, one line;
run.py:89 passes it as `GOW3_STALE_READBACK`). Launch: `run-game.ps1 -Samples -Env @{GOW3_UE4_BOOT_4K='1';
GOW3_PERF_DIAG='1'} -Tag p0-stale-on`. Compare with the T-L5 baseline (stale off, no diag); the diag adds some
overhead. The user watches for wrong lighting / objects.

### P0 results (2026-10-18 11:22-11:38, launched by Claude with the user's go)

Log [data/p0-stale-on.log](data/p0-stale-on.log), samples `research-data\samples\p0-stale-on\` (exe + PDB beside them).
~16 min, no fault, clean exit. 193 windows of 5 s.
- **Readbacks:** 575,820 queued, 575,813 completed, **0 waits** (stale on), pending peak ~0 MiB. Five images make
  up almost all of it:

  | Image | Size / format | Copies | Windows |
  |---|---|---|---|
  | 0x201faaaa00 / ab00 / ac00 | 8x8 / 4x4 / 2x2 R16Sfloat (one mip chain) | 143,931 each | 192 |
  | 0x2003180000 | 1x1 R32G32Sfloat | 37,091 | 100 |
  | 0x2000aa0000 | 1x1 R32G32Sfloat | 34,911 | 92 |

  Shapes suggest UE4's eye adaptation / average luminance (inferred, not checked in a capture). "CPU accesses while
  pending" = 0 for all, **but in stale mode CPU reads do not fault, so only writes are counted**: no CPU writes;
  reads unknown. The black-surfaces fix may also be about the GPU reading this memory another way (as a buffer),
  not the CPU: to find out before skipping readbacks.
- **Stage B profile** (557,930 samples): `RunEventWriteEop` → `ProcessDownloadImages` **16.7%**, all yielding inside
  the inlined `WaitHostCopies`: ~7.45% its host-copy loop, ~7.3% `Gow3Copy::WaitAsync` (shown under its
  `WaitTimer`). Same as before stale was on (15.3%): the wait does not depend on stale. `VirtualProtect` 8.4%
  (was 10.9%), `ResolveOverlap` 10.0%, `FreeImage` 5.7%, flip `Semaphore::Wait` 3.1%.
- **So ~15% of stage B is waiting for all copy work at every EOP to read back ~5 tiny images per frame.**
- The wait's reason (texture_cache.cpp:163: "upload workers must finish reading RAM before these pages become
  inaccessible") holds when pending pages are **read**-protected (stale off). With stale on they are only
  **write**-protected, so upload workers can still read them. Remaining question: `RetireReadbacks` writes guest
  memory (`TryWriteBacking`) that an upload worker could be reading at that moment.
- FPS: median 132.3 / p10 84.4 / min 20.5 vs T-L5 baseline 102.3 / 91.4 / 34.8, **not comparable** (gameplay vs idle,
  diag on). Needs the same scenario.
- Visuals with stale on: **the user to report.**

**The user (2026-10-18): stale on, "I didn't see any glaring issues."** The one visual bug seen, shadows sometimes
drawn as a big box instead of their shape, was there before (logged as V1 in plan-baseline-issues.md).

**R1 built (lab change L6, 2026-10-18 11:44):** `skip_copy_wait` (`GOW3_STALE_SKIP_COPY_WAIT=1`, needs stale) skips
`ProcessDownloadImages`' `WaitHostCopies`; startup line adds "; no host copy wait at end of pipe". Reasoning for the
retire question: upload copies only read guest memory and write staging; a worker reading a retired image's bytes
while `RetireReadbacks` writes them sees the GPU's newer output (as PS4 memory would), no fault (write protection
does not block reads), no deadlock; the next submission still waits for its copies in `SubmitExecution`.
Test (A/B, same scenario, both stale on, both `-Samples`, no perf diag): A = `-Tag r1-a-stale`, B = `-Env
@{GOW3_STALE_SKIP_COPY_WAIT='1'} -Tag r1-b-skip`; Graveyard, idle at the same spot ~4 min each.

### R1 A/B (2026-10-18, A 11:46-11:53, B 11:54-12:01, launched by Claude with the user's go)

Both stale on, `-Samples`, 4K boot, no perf diag. Logs [data/r1-a-stale.log](data/r1-a-stale.log),
[data/r1-b-skip.log](data/r1-b-skip.log); per-window tables with `research/tools/series.py <log> --rows`. No faults.
The two idle views differed (A ~1,136 draws/frame, B ~1,800), so compare windows of similar draw counts.

| | A (R1 off) | B (R1 on) |
|---|---|---|
| Stage B: `RunEventWriteEop` → `ProcessDownloadImages` | 19.7% | **gone** |
| Stage B idle (waiting for packets) | 24.3% | **34.5%** |
| Stage B other yields | (in the EOP wait) | `WaitDeferredSignals` ~8.5%, `RunEventWriteEos` ~1.8% |
| Stage A idle (delay + wait) | ~54% | ~53% |
| FPS ~1,150 draws/frame | ~144 (windows 71-94) | ~136-138 (39, 41, 46) |
| FPS ~1,800 draws/frame | ~96-103 (31-35) | 90-98 (21-25), ~110 (63-71) |
| FPS ~2,800-3,000 draws/frame | 88-91 (59-67) | 82-93 (48-49, 55-57) |
| Blocked on host copies / copy threads (all threads) | ~9-13% / ~10% | ~5-9% / ~5-6% |

R1 visuals (the user, 2026-10-18): run B showed no additional visual issues.

Reading: R1 removes its target completely, but FPS at matching draw counts is unchanged within the noise of
different views. Stage B now idles more; stage A was already idle about half the time. **Neither GPU-side thread is
the limit any more: work arrives too slowly from the game's own threads.** Prime suspect: 80-117k page-tracking
write faults per second on the game's threads (each a full Windows exception, VEH; cost not measured). Needs a
whole-process profile (Windows Performance Recorder, `wpr -start CPU`, admin). R1: keep (removes ~20% of stage B
for free); visuals in B: the user to confirm.

Menu vs game (the user: "in the menu the frame times are quite good"; run A): menu ~447 draws/frame, ~140 FPS,
stddev 0.5 / p99 9 ms, stage B ~90%, GPU-tick waits ~20%; Graveyard standing ~1,136 draws, ~144 FPS, 0.7 / 10 ms;
heavier views ~2,700-2,900 draws, 83-91 FPS, 1.0 / 15 ms. Stage B ~90% everywhere, so FPS follows draws per frame;
menu draws cost ~2.5x more stage-B time each. In the level the draw count changes with the view (the swing).
Separate: 300-3,700 ms windows at loads / transitions.

### Whole-process trace (2026-10-18 12:14-12:15, WPR CPU.light 20 s, the user recorded; build `e384aa1`, R1 on)

Trace `research-data\wpr\graveyard.etl` (198 MB; the first attempt, full CPU profile, 2.1 GB, came out "corrupt"
(0x80070570) with the disk nearly full. **Corrected twice; final, 13:50:** the traces were never corrupt. Full-profile
traces lose events (e.g. 17,250 in `graveyard-stacks2.etl`), and xperf, old and new, then refuses them with
0x80070570 unless given **`-tle -tti`** (tolerate lost events / time inversions); the same file reads with them. Not
the disk, not the format. Current WPT 10.0.28000 installed from the ADK (WPT feature only) anyway.) Tools: `xperf -a profile -detail` (modules / functions; a symbol path holding only the game
process's Windows modules, `research-data\symbols-game`, makes it take seconds instead of decoding every process);
`xperf -a dumper` + `research/tools/etw_threads.py` (per-thread cores and functions; the port's addresses resolved
with llvm-symbolizer; xperf 8.1 does not read the lld PDB). Output `research-data\wpr\graveyard-threads.txt`.

- The game process used **4.79 cores**: port 1.74, game's PS4 code ("Unknown" image) 1.10, kernel 0.67, ntdll 0.52,
  ucrtbase (memcpy etc.) 0.41, NVIDIA driver 0.17.
- **Not the faults:** exception dispatch ~0%, kernel page-fault entry ~1%, protection changes ~1.3% of the process.
  The "write faults/s" stat counts **pages**, not exceptions (`CountWriteFaults`, region_manager.h:239-246: one per
  protected page in a range marked CPU-modified; one fault unprotects a window of up to 64 pages). Suspect refuted.
- Per thread (no thread names in the trace; identified by their functions):

  | Thread | Cores | Is | Time |
  |---|---|---|---|
  | 44384 | 0.81 | busiest game thread | 56% game code; **13.1% NtDelayExecution + 12.1% syscall entry + 1.4% KeYieldExecution** (short sleeps / yields); **7.5% `pool_take_locked`** |
  | 37004 | 0.69 | stage B (`DrawPipe::Run`) | 15% spin in Run, 8% delay, 6.3% `MiSetWsleProtection`, draw work |
  | 46896 | 0.66 | stage A (`ProcessGraphics`, `PostDraw`) | decode; ~12% delay |
  | 35788 | 0.63 | Vulkan recorder | 61% in `RecorderThread` itself (spin-wait) |
  | 48692 | 0.34 | game main thread | 72% game code; **11.8% `find_locked`** |
  | 40592 | 0.25 | game thread | **28.7% `pool_take_locked`**; 20% game code |

- Reading: GPU-side threads spin / idle waiting for work; the busiest game thread does ~0.46 core of real game work
  and spends ~27% of its running time in short sleeps / yields. Two runtime algorithms on game threads:
  - **`find_locked`** (src/runtime_rwlock.c:42-45): every PS4 rwlock call validates its handle by walking a linked list
    of all rwlocks ever created, under one global mutex.
  - **`pool_take_locked`** (src/runtime_memory.c:716-727): every direct-memory commit scans the pool's page map byte by
    byte from page 0, under the runtime's global memory write lock (which the GPU threads' `vma_index` /
    `cached_region` lookups also need).
- "Frames queued" (= 1) bounds the **GPU command thread** to N frames ahead of the GPU (vk_presenter.cpp:538-556),
  not the game thread: less likely the cause; cheap test later.

Next: R2 rwlock handle lookup O(1); R3 pool free-run search with a hint; then find which runtime call makes thread
44384's sleeps / yields.

**R2 built (`cf9e201`):** live rwlocks in an open-addressing pointer set (≤ half full, rebuilt at 4x live; deleted
slots hold a private sentinel, never 1 = destroyed handle); same lock, holders and error codes.
`tests/test_rwlock_registry.c` (5000 locks, rebuilds, own-lock errors, garbage / destroyed / freed handles,
re-creation, lazy init, 4 threads) passes on the new code **and on the original file** (same behaviour).
**R3 built (`2097554`):** per-chunk `free_hint` (all pages below it used); same first-fit run as before. Checked
against the old functions on 200,000 random takes and partial gives: identical runs and page maps (scratch harness,
not in the repo). No switches: behaviour-identical data-structure changes, covered by those checks instead.
Built 12:42, installed. Test T-R23: the Graveyard spot of `wpr-graveyard` (its steady windows 149-164: ~3,010-3,030
draws/frame, ~82 FPS, R1 on, stale on) + a WPR trace to confirm `find_locked` / `pool_take_locked` are gone and see
what the game threads do next.

Candidate R1 (if stale looks right): in stale mode, skip `ProcessDownloadImages`' `WaitHostCopies` (switch to
restore it). If stale looks wrong: targeted waits instead (only for copies reading the protected pages, Bloodborne #1).

### T-R23 result (2026-10-18 12:55-13:00, launched by Claude with the user's go; build `2097554`)

First launch exited silently with code 5 at startup (S1 in plan-baseline-issues.md); the retry ran normally. The
user: "the 1% lows seem much much better. When looking at an area that previously dropped to 50-60 fps 1% lows, it's
closer to 80." Log [data/t-r23-retry.log](data/t-r23-retry.log), trace `research-data\wpr\graveyard-r23.etl`,
per-thread `graveyard-r23-threads.txt`.

Same Graveyard spot vs `wpr-graveyard` (R1 only), windows with worst frame < 200 ms:

| Draws/frame | Before: windows / FPS median / FPS min / p99 median / p99 worst | After |
|---|---|---|
| 2,600-3,100 | 70 / 81.2 / 71.3 / 16.7 ms / 31.7 ms | 70 / **94.7** / **80.7** / **14.6 ms** / 29.6 ms |
| 1,700-2,000 | 66 / 118.9 / 72.2 / 12.8 / 22.0 | 7 / 116.9 / 109.6 / 12.3 / 15.3 (too few windows) |

**+17% median FPS, +13% lowest window, p99 -13% in the heavy view.** Trace: `find_locked` gone (main thread 81.9%
game code, was 71.6% + 11.8%); `pool_take_locked` 0.3% on the busiest game thread (was 7.5%), gone from the
pool-heavy thread (was 28.7%). Process 5.91 cores (was 4.79); stage A / B / recorder ~0.9 core each (were ~0.65):
the GPU side is fed faster. Busiest game thread now **0.97 cores**: 62.7% game code, **13.9% NtDelayExecution +
12.8% syscall entry + 1.6% KeYieldExecution** (~28%: short sleeps / yields; which runtime call: next).

### Wait sites (2026-10-18 13:40, `GOW3_WAIT_SITES=1`, lab diagnostic `9f088f1`, run `wait-sites`)

Graveyard, standing (~112 FPS), every 5 s report the same ([data/wait-sites.log](data/wait-sites.log)):

| Calls/s | Kind | Site | Note |
|---|---|---|---|
| **~1,300,000** | usleep(0) | guest+0x689c89 | spin-wait loop (below) |
| ~450 | usleep (~7.7 ms) | guest+0x8d7b67 | |
| ~260-300 | usleep(0) | guest+0x8d7b67 | |
| ~80-90 | usleep(0) | guest+0x27273f9 | |
| 30 each | usleep (33 ms) | guest+0x5f315f5, 0x5f7180a (NP toolkit module), 0x170448d | periodic threads |

**guest+0x689c89 is a spin-wait:** function 0x689c10 (2 direct callers, 0x689fc6 / 0x68a054) reads a clock twice
(0x3bf0680), then loops `Sleep(0.0)` (call 0x8e51d0 → usleep(0); each a `SwitchToThread`) → `0x73ba40` ("is the
current thread the one in global [rip+0x50e0d1b]+0x44?") → if so a singleton's vtable +0x38 with a thread index
(pattern of UE4's task graph `ProcessThreadUntilIdle`) → checks `[rbx+0x58] == 2` / `[rbx+0x40]` and sets `*done`.
Inferred: a UE4 wait-for-task / fence loop; exact function not identified yet. 1.3 M yields/s is the busiest game
thread's ~28% short sleeps / yields, and on SMT (7800X3D) it can slow the sibling hyperthread too. The real question
is what it waits for: the stacks trace (`graveyard-stacks2.etl`, 858 MB) shows which thread spins and what the others
do.

## Lead C: texture-cache churn (second)

`BindResources` → `ResolveOverlap` → `FreeImage` → `SignalImpl::Protect` → `VirtualProtect` (~11% of stage B).
Measure which images are freed / re-created per frame and why the overlap repeats; compare Bloodborne's collector
fixes (#4).

## Lead F: flip waits on stage B (third)

`sceVideoOutSubmitEopFlip` → `SubmitFlip` → `Semaphore::Wait` (5%).

## Stages (one change per launch, behind a switch defaulting to the old path)

| Stage | What | Verify |
|---|---|---|
| P0 | Measurement: Graveyard route, `-Samples`, `GOW3_PERF_DIAG=1`, no code change | Readback profile: images, copies vs CPU accesses; stage B profile in gameplay |
| R1 | Per P0 (likely: skip readbacks of images the CPU never touches) | Stage B EOP share, FPS / p99 vs baseline; no black surfaces (the user) |
| C1x | Texture churn, per measurement | `VirtualProtect` share, FPS / p99 |
| R1 done | Skip the EOP host copy wait in stale mode (`e384aa1`) | Verified: stage B EOP share 19.7% → 0; FPS unchanged (limit moved upstream); no visual issues (the user) |
| R2 | rwlock handle lookup O(1) | `find_locked` share on game threads; FPS / p99 |
| R3 | Pool free-run search with a hint | `pool_take_locked` share; FPS / p99 |


