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

### Stacks trace (2026-10-18 13:46, `graveyard-stacks2.etl`, WPR CPU 15 s, read with `xperf -tle -tti`)

Exported with symbols (5.5 GB dump); `research/tools/etw_stacks.py` attaches Stack records to samples; the current
xperf also recovers thread names. Output `research-data\wpr\graveyard-stacks2-spin.txt`.
- **The spinner is UE4's `RenderThread 1`** (0.98 core: 56% game code, ~28% in the `guest+0x689c89` loop's
  `SwitchToThread` → `NtDelayExecution`, + 4.9% `runtime_wait_site`, the diagnostic's own cost). Others: stage B 0.89,
  stage A 0.88, VkRecorder 0.82, (unnamed, 81% game code: likely the game thread) 0.39, `RHIThread` 0.28.
- Stack walks stop at the first guest frame (no Windows unwind data for PS4 code): callers of 0x689c10 have to come
  from static analysis.
- Reading: the render thread is the busiest game-side thread and a third of its time is waiting. Inferred: on PS4 a
  render thread typically waits on GPU progress (a label / fence written at end of pipe before reusing command or ring
  memory); in this port those labels are written by the GPU-side pipeline, so late publishing would show as this wait.
- Next (no launch): decompile 0x689c10, its callers 0x689fc6 / 0x68a054 and the state at `[rbx+0x58]` in Ghidra (with
  GhidraOrbis); if it is a GPU label / fence, measure when the runtime writes it vs. when the render thread starts
  waiting (candidate R4: publish earlier).

### What the render thread waits for (2026-10-18, Ghidra 12.1.4 + GhidraOrbis, no launch)

Headless decompile (`research/tools/ghidra/DecompileAt.java`; project `research-data\ghidra\proj`, `-noanalysis`,
24 s; Ghidra's image base 0x1000000 = guest+0; output [data/ghidra-decompile-occlusion-wait.txt](data/ghidra-decompile-occlusion-wait.txt)):
- `0x689ed0` (2 call sites to the spin loop, reached indirectly) reads a result object `p` in two modes:
  state 1: 8 or 16 pairs of u64 at `p+0x38` (16 when `0x3bef580` returns 1, likely the PS4 Pro check: twice the
  render backends), ready when **bit 63 is set in all**, result = Σ(end − begin): **a GPU occlusion query** (per-RB
  ZPASS counters); state 2: u64 at `p+0x40` non-zero, scaled by a tick frequency: **a timestamp query**. If not
  ready and the caller asks to wait, it calls `0x689c10`: the `Sleep(0)` loop (with the task-graph processing and a
  timeout). Inferred: UE4's `RHIGetRenderQueryResult(…, bWait=true)` (no symbols; from the data layout).
- The port answers occlusion queries **synthetically when stage A decodes the event**: `liverpool.cpp:1129-1138`
  (`ZPASS_DONE` / `PixelPipeStatDump` → every RB counter written with the valid bit and an ever-growing count: always
  "visible"). So the render thread waits until the frame that issued the queries has been submitted (RHI thread) and
  **decoded by stage A**, which runs ~90% busy with a backlog.
- MGS2 / Peace Walker busy-wait fixes (read only; `MGS4-SSAA-AspectRatio-Enabler` `pw/src/features/busy_wait.cpp`,
  `MGS2-Lab` `src/fixes/busy_loop_fix.cpp`): PW's render thread spinning on `Sleep(0)` was fixed with an **event set by
  the producer** (2 ms timeout, shutdown-safe), not a timer ("it is not waiting on a clock"); timers only for clock
  waits (PW ticker, MGS2 frame wait: sleep min(remaining − 3 ms, 50 ms) on a high-resolution timer, spin the rest;
  `timeBeginPeriod(1)` held). PW: 122% → ~24% of a core.
- **Test R4a (no runtime change):** `r.NumBufferedOcclusionQueries` (in the binary, UE 4.21 default 1: "More frames
  reduces the chance of stalling the CPU waiting for results, but increases out of date query artifacts") = 2 through
  the override pak: UE4 reads queries two frames old, long decoded. Artifacts barely apply: the port reports
  everything visible. New opt-in `GOW3_UE4_CVARS="name=value;…"` (scripts/ue4_override.py) for it and later cvar
  experiments. Measure with `GOW3_WAIT_SITES=1` at the same spot vs run `wait-sites`. If the spin remains:
  `r.AllowOcclusionQueries=0`, or an event-style wait in the runtime (signal when stage A writes the results).

### R4a result (2026-10-18 14:05-14:15, launched by Claude with the user's go): **rejected**

`GOW3_UE4_CVARS='r.NumBufferedOcclusionQueries=2'` (+ 4K boot, R1, `GOW3_WAIT_SITES=1`), Graveyard. Log
[data/r4a-occlusion2.log](data/r4a-occlusion2.log). The spin at guest+0x689c89 fell from ~1.3 M to **~100 k calls/s**,
but the heavy view got slower:

| 2,900-3,300 draws/frame | T-R23 (no R4a) | R4a |
|---|---|---|
| windows | 161 | 23 |
| FPS median / min | **94.3 / 80.7** | 75.3 / 74.6 |
| p99 | **14.6 ms** | 16.2 ms |
| blocked on GPU ticks | **1.3%** | **21.9%** |

The user: GPU at ~50% in the busy area, frame rate "rapidly jumping between 75 FPS and" higher, "performance may be
somewhat worse … I don't believe we're actually waiting on the GPU." Right: "GPU ticks" is CPU waiting for submitted
work to finish (`Scheduler::Wait`), a sync stall, not GPU throughput. Inferred: the occlusion wait throttled the
render thread; without it the render thread runs into a harder sync point (likely "Frames queued = 1",
vk_presenter.cpp:538-556, or query / command memory reuse). **Do not retry R4a without a new reason** (e.g. together
with Frames queued 2, or after the sync point is found with `GOW3_PERF_DIAG=1`, which lists the waiting call sites).
Directions kept: faster stage A decode; answer occlusion queries earlier (the answer is synthetic, "always visible").

### GPU wait sites (2026-10-18 14:20, `GOW3_PERF_DIAG=1`, current build without R4a; run `perfdiag-heavy`)

Log [data/perfdiag-heavy.log](data/perfdiag-heavy.log), 156 windows (none reached ≥ 2,500 draws/frame). `Wait
profile` lines summed per 5 s window:

| Site | Calls | Semaphore wait | Max single | What |
|---|---|---|---|---|
| **vk_presenter.cpp:553** | ~837 | **~869 ms (~17%)** | 69.7 ms | "Frames queued" bound: the flip (on stage B) waits for the GPU frame `frames_ahead` (= 1) flips back (vk_presenter.cpp:538-556) |
| **buffer_cache.cpp:213** | ~31 | **~246 ms (~5%)** | **60.7 ms** | `DownloadMemory` → `scheduler.Finish()`: a full GPU drain per guest buffer readback (~8 ms average) |
| buffer.cpp:276, texture_cache.cpp:137 | | < 1 ms | | negligible |

Reading: with the GPU at ~50%, the presenter wait is latency (the GPU still has the previous frame queued), not
throughput; R4a's regression fits (more frames reached this wait). Buffer readbacks are rare but each drains the
GPU (up to 61 ms): a likely source of the spikes.
Next: **F1** Frames queued = 2 (existing setting, `frames_queued` / `GOW3_FRAMES_AHEAD`; +1 frame of input latency);
**R5** Bloodborne's readback queue (plan-gpu-threading.md #2: wait only for the GPU work that wrote the range, on its
own queue, instead of `Finish()`).

**F1 (Frames queued 2): declined by the user (2026-10-18):** "buffering extra frames isn't going to get us what we
need here, the issues are too systemic … a nice way to get some extra stability when all that's left is the
occasional stutter or frame drop."

**The user, same run (starting area):** "Looking at certain areas would give me a flat frame graph … other areas
caused an unsteady oscillating frame rate. So certain effects are likely one of the major contributors, something in
the CPU to GPU translator is probably taking forever to handle these. I imagine there is A LOT of repeated work."
Fits Lead C (stage B ~12% in `ResolveOverlap` → `FreeImage` → `VirtualProtect`): fire / heat haze / translucency
copy scene color or reuse render-target memory under another format each frame → overlapping images freed and
re-created every frame. Next: per-view A/B (flat vs oscillating view) with Lead-C counters (images freed / re-created
by size and format, protection changes, per 5 s) and optionally a frame capture of the bad view.

### R1 reverted; performance degrades with new content (2026-10-18 14:40-15:10)

- **R1 caused V2** (torches as black flickering boxes; bisect in plan-baseline-issues.md): reverted `6b86c75`.
  Things not to retry: skipping `ProcessDownloadImages`' host-copy wait, even in stale mode.
- Run V2b (stale on, R1 off; [data/v2b-stale-on-r1-off.log](data/v2b-stale-on-r1-off.log)), same view ~975-990
  draws/frame: before a level restart (windows 55-67) ~88 FPS, frame-time stddev ~3.5-3.9 ms, p99 ~16.2 ms,
  **blocked on GPU completion ~41%**, stage B ~56%; after the restart (70-80) **~108.6 FPS, stddev ~0.1 ms, p99
  ~9.4 ms**, blocked ~25%, stage B ~96%. Same draws, so not scene cost.
- The user: "If I stay in the same area after restarting the level, performance stays the same, but if I progress a
  bit, the frametimes start oscillating, and even returning to the earlier area shows the same oscillating."
  **Accumulation from new content**, cleared by a level restart.
- Suspect (Lead C, Bloodborne #4 "FPS fell the longer they played"): the image collector only runs above a GPU-memory
  trigger (texture_cache.cpp:1420: `if (total_used_memory < trigger_gc_memory) return;`), high on a 24 GB card, so
  images (stale variants, aliases) pile up; lookups that miss `find_image_cache` (invalidated by every registration,
  `registry_generation`) walk all images overlapping a range (`ForEachImageInRegion` → `ResolveOverlap`), and alias
  syncs / re-uploads are GPU copies with barriers (many small serialized GPU jobs: GPU ~50% busy yet the CPU waits).
  Inferred; to measure.
- Lab diagnostic **`GOW3_TEXCACHE_STATS=1`**: per 5 s, live registered images (+created −freed), slots, alias
  groups, lookups fast / slow (avg candidates walked), overlaps, expands, uploads (count, MiB), alias syncs / copies,
  GPU memory vs collector trigger / pressure. Test T-C0: start area 1-2 min, progress, return to the start area.

### T-C0 result (2026-10-18 15:10-15:15): the cache re-creates and re-uploads ~20 images per frame

Log [data/t-c0-texcache.log](data/t-c0-texcache.log); joined per window with `research/tools/texcache_series.py`.

| | Menu (rows 12-33) | Start area before progressing (39-42) | After progressing, start area (54-69) |
|---|---|---|---|
| FPS | 240 | 143-174 | **~91** (stddev ~4 ms) |
| live images | 79 | ~600 | **~850** |
| created = expanded per 5 s | 7,200 | 13-15 k | ~9.2 k (**~20 per frame**) |
| uploads per 5 s | ~9.9 GB | 22.6-27.1 GB (**~30 MB/frame**) | **~29.6 GB (~65 MB/frame, ~5.9 GB/s)** |
| blocked on GPU completion | ~57-62% | 15-25% | **36-39%** |
| GPU memory | 2.4 GB | 3.7 GB | 4.4 GB (collector trigger 7.1 GB: never runs) |

The user: "GPU sitting at 52C, but usage is 80%, that's indicative of massive quantities of data saturating the
bus." Matches: ~6 GB/s of guest-memory uploads, copy / bus bound. **Every image creation is an `ExpandImage`**
(created ≈ expands), which (texture_cache.cpp:859-876) creates a new image, **uploads all of its guest memory
(`RefreshImage` on the new, dirty image)**, copies the old one into it and frees the old one. Per-frame upload volume
doubles as content loads (30 → 65 MB): the accumulation. Bloodborne's `ExpandImage` / `ResolveOverlap` are identical
(not fixed there). Suspected ping-pong: `ResolveOverlap` expands on equal address + equal size when either side is
Color3D (:659-663): memory used as 2D and as 3D in one frame (UE4's color-grading LUT renders slices, then samples a
volume) would re-create and re-upload it at every switch. Also possible: block-compressed views (:655-657), bigger
resources (:684-687). The diagnostic now also lists the top expanded images per 5 s: address, reason (block view /
2D/3D / bigger), old → new size, type, format, levels / layers, KiB. Test T-C1: same route as T-C0.

**The user (2026-10-18), the lens for the fix:** "any fullscreen effect or filter that converts the framebuffer to a
texture (or similar action) and keeps it in system memory is okay on the PS4, because it has unified memory, but on
the PC, it goes across the PCI-E bus. If anything like that is going on, it will kill frametimes." Applies both ways
here: downloads (the per-frame render-target readbacks of `GOW3_READBACK_LINEAR`) and uploads (every `ExpandImage`
re-uploading guest memory). Goal: such images never leave GPU memory unless the CPU really reads them.

### T-C1 result (2026-10-18 15:20-15:30, launched by Claude with the user's go; build `4035556`; log `data/t-c1-expands.log`)

Every in-game expand is one pattern: **a volume switching between a 2D array (64 layers, the render target writing its
slices) and a 3D image (the texture sampling it)**, twice per use, each switch a full `RefreshImage` from guest memory.

| Address | Image | Likely UE4 resource | Expands / 5 s (in game) |
|---|---|---|---|
| `0x2010000000`, `0x200ee00000`, `0x201ba00000` | 64×64×64 RGBA16F, 2 MiB | translucency lighting volumes | 976–1952 each |
| `0x206c000000` | 240×135×64 RGBA16F, 16 MiB | volumetric fog grid (1/8 of 1080p × 64) | 976 = **15.6 GB** |
| `0x200ec80000` | 32×32×32 A2B10G10R10, 128 KiB | colour grading LUT | menu: 2400 |

In-game window (n=50): 9760 expands = 9760 uploads = 31.3 GB per 5 s (**≈6 GB/s over PCIe**), 97 FPS, GPU ticks 32%.
Menu: 7200 expands, 10 GB per 5 s. More of these volumes appear as the level progresses (fog grid, more lighting
volumes), consistent with the "degrades with new content" observation; T-C0's growing live-image count is separate
(GC never runs below the trigger) and harmless by itself. Cause (`ResolveOverlap`): equal address, equal
`guest_size`, either side `Color3D` → `ExpandImage`, regardless of direction, so it never settles.

**Fix V3 (`VolumeSliceViews`, toggle bit 58 = 288230376151711744 in `GOW3_TOGGLE_FILE` switches it off live):**
a 2D render target with one layer per slice of a same-sized 3D image (1 level, compatible format) renders into the
volume through a 2D array view; 3D images are already created `e2DArrayCompatible`. Needed with it:
`FindImage` keeps the volume (its 1 layer < 64 was "picked up wrong" → freed), `Image::GetBarriers` and
`Runtime::ClearImage` use layer 0 count 1 for volumes (Vulkan rule; depth slices cannot be selected), and a 2D array
view of a volume carries attachment usage only (spec). Expected: in-game uploads ≈0 for these, the first 2D→3D
switch still expands once. Report counts "volume views". Risks: a partial-slice clear now clears the whole volume
(logged as a warning); a storage-bound 2D side is not covered (would still show as expands). Test T-V3: same route,
`GOW3_TEXCACHE_STATS=1`, validation layer once for the view/barrier rules.

### T-V3 result (2026-10-18 15:45-16:10, launched by Claude with the user's go; build `1f18217`; log `data/t-v3-volume-views.log`)

Fix on for the whole run (toggle file `research-data\toggles.txt` = 0, never flipped). Expands **0** per window
in menu, map and level (only one-off mip growth of a 2048² R16F at boot); no partial-clear warnings, view-type
errors or "resolve failed". The user: main menu ~240 FPS; map / level select **~70 → ~200 FPS** (stats: 183-198,
stddev 0.1-0.3 ms); level "fairly good", most directions 200+ FPS; heavy graveyard view 1% lows **~60 → ~100**.

| Heavy view, 5 s window | T-C1 (fix off) | T-V3 |
|---|---|---|
| FPS | ~97 (911 draws/frame spot) | 115-123 |
| draws/frame | — | 2670-2890 |
| uploads | 31 GB | 0-15 MiB |
| blocked on GPU ticks | 32% | 0.1-0.4% |

The heavy view is now **CPU-bound on draw submission** (~330k draws/s through stage A + B; the user: GPU usage
55-60%): a different mechanism, next lead (GPU threading / per-draw cost, see plan-gpu-threading.md). Still open
for V3: volumetric fog correctness (the user: "I think it's working", wants checks): live A/B with toggle bit 58 at
a foggy spot with screenshots, and one validation-layer run. Remaining ~450 MiB / 5 s of ordinary refresh uploads on
the map screen: not investigated.

### Fog A/B and heavy-view trace (2026-10-18 16:00-16:20, launched by Claude with the user's go; build `1f18217`; log `data/t-v3-fog-heavy.log`)

Live A/B with toggle bit 58, twice, at a foggy spot. **The user: "I see no difference, fog is definitely present with
the fix on"**: V3 does not break volumetric fog. With the fix off the old path returns fully (windows 127-134:
~10,500 expands, ~33 GB uploads per 5 s, ~105 FPS; FPS "only halves"). The frame-time oscillation of earlier runs did
**not** return with the fix off (stddev 0.1-0.7 ms), so the uploads alone do not cause it; it needed more on top in
those runs (R1 build, growing live images, position): watch for it. Validation-layer run still to do.

Heavy view with the fix on (windows 250-254): 114 FPS, ~2800 draws/frame, GPU ticks 0.5-0.8%, uploads 5-9 MiB.
WPR CPU trace `research-data\wpr\heavy-v3.etl` (906 MB, 15 s, the user ran it facing the heavy direction).

Read (`etw_threads.py --functions 15 --threads 5`, output `research-data\wpr\heavy-v3-functions.txt`; `etw_stacks.py`
attached stacks to few samples this time, not used): four threads at about one core each, a serial pipeline.

| Thread | Cores | Main costs |
|---|---|---|
| `RenderThread 1` | 0.98 | 62% guest code, **~29% `NtDelayExecution`/yield** (the query-result spin, waits on stage A) |
| `gow3:DrawRec` (stage B) | 0.94 | `DrawPipe::Run` 19%, ~17% sleeping/yield: has slack |
| `shadPS4:GpuCommandProcessor` (stage A) | 0.91 | `PostDraw` 11.4%, **`std::bitset` test/ref 13.1%**, memcpy 6.9%, `vma_index` + `cached_region` 6.6%, sleep ~12% |
| `gow3:VkRecorder` | 0.85 | `RecorderThread` 59%, NVIDIA driver ~4% |

Stage A is the bottleneck (the render thread waits on it, stage B idles part of the time). **Fix P1** (no toggle,
same behaviour): `RegDirty::blocks` was a `std::bitset<1664>`; on glibc `PostDraw` walks it with `_Find_first` /
`_Find_next`, but libc++ (our clang64 build) has neither, so the fallback tested all 1664 bits per draw (~2800
draws/frame). Now a 26-word array with a `countr_zero` `ForEach` (also used by `RegDelta::Capture`). Next stage A
candidates: `PostDraw` itself, `vma_index`/`cached_region` (guest memory region lookups per draw).

### T-P1 result (2026-10-18 16:25-16:50, launched by Claude with the user's go; build `5cd037e`; log `data/t-p1-bitset.log`)

Heavy view (windows 28-32, ~2750 draws/frame): **124-131 FPS** (T-V3: 114-123, ~+8%); the user: "1% lows are
improved". New observation, the user: "every 30 seconds or so there's a big oscillation in the frametimes that lasts
2-5 seconds", later "it went away after a while"; at another spot (1057 draws/frame, ~190 FPS, GPU 65-80%) it came
back. **Not caused by P1**: the same windows exist before it (`t-v3-fog-heavy` windows 237-238: 91 / 83 FPS, stddev
3.9 / 4.5 ms). Signature (windows 140-141 vs 139 / 142): FPS halves (185 → 94), draws/frame unchanged, uploads 0,
GPU ticks 20% → 6%, and **stage B gets ~3x slower per draw** (busy 75% → 92% at 257k → 108k draws/s: ~2.9 → ~8.5
µs/draw); compute drains ~120 → ~580 µs each. The same work runs slower: CPU speed, core sharing, contention or a
wait counted inside stage B's busy time (`ChunkGuard` / `ReserveRecordDataLocked` = recorder ring space), not more
work.

WPR trace `heavy-p1.etl` (20 s at the second spot; `etw_threads --slice 1`, output `heavy-p1-slices.txt`): every
thread flat for all 41 s at 0.5-0.6 work cores, none saturated. CPU is a Ryzen 7 7800X3D (8 cores / 16 threads, no
E-cores; power plan Balanced): no slow-core migration. Per-second core placement (scratch `smt.py`): threads move
between cores too often (main core 20-35% of samples) to show SMT sharing either way. **Unresolved: the log had no
timestamps**, so whether windows 140-141 fall inside the trace is unknown. Tooling now: `tee_run.py` writes
`<log>.times` (line number → local time, at the tee, so ~flush resolution) and `wpr-capture.ps1` writes
`<tag>.times.txt` (start / stop). Next: capture during an episode with both timelines; compare stage B's functions
and off-CPU waits (a CSwitch-based profile would show the latter) inside vs outside it.

### Episode captured: the kernel trims the guest memory section (2026-10-18 17:25-17:37, launched by Claude with the user's go; build `5cd037e`; log `data/t-episode.log` + `.times`)

Hotkey captures (`wpr-hotkey.ps1`, F9, 30 s, profile CPU): `heavy-episode-1.etl` from 17:27:59.7 (window ending
17:28:03: 88 FPS, stddev 7.8 ms, p99 46 ms) and `heavy-episode-2.etl` from 17:29:25.0 (window ending 17:29:28: 134
FPS, p99 23 ms); both followed by steady 200+ FPS. An episode at 17:27:38-43 preceded both captures: not WPR.
**The user: both times the frame rate tanked, one of the broken transparent effects (V1) covered a good part of the
screen; it disappears after a few seconds and could not be brought back.**

Capture 1, seconds 0-2 (episode) vs 3+ (scratch `procs.py`, `sysfn.py`, `waits.py` over the CSwitch / ReadyThread
records):
- The game's threads do about a third of their normal work; the game has half its usual samples (off CPU).
- The `System` process uses one core (0.07 after): one `ExpWorkerThread` (tid 45048), **78% `MiViewMayContainPage`,
  6% `MiTrimSection`**: the memory manager trimming a section. RAM 64 GB, 41 GB free: not memory pressure.
- **`RenderThread 1` blocked on `WrPushLock` 200-300 ms per second, readied by that System thread** (~2000
  wake-ups/s); from second 3: ~0. The GPU threads then wait for work (`WrAlertByThreadId` 410-790 ms/s vs 130-320).
- On CPU, the game's threads spend <1% in memory-manager code: they wait, they do not work.

The guest memory is one pagefile-backed section (`runtime_memory.c` `pool`), mapped twice: `backing_base` (the whole
section) and one view per guest region. `carve_host` unmaps **whole** views overlapping a newly mapped / unmapped
range and maps the parts outside it again; unmapping a view makes the kernel trim its pages, checking each other
view of the section per page (`MiViewMayContainPage`). Suspect: the effect's guest map / unmap inside a large region.
**Diagnostic H1** (`GOW3_HOST_MAP_STATS=1`, lab): host map / unmap / protect calls, MiB, time and max per 5 s,
views carve_host unmapped and re-mapped, region count; any call over 1 ms printed with its size. Test T-H1: trigger
the effect where the user knows it appears, `GOW3_HOST_MAP_STATS=1`.

### T-H1 result (2026-10-18 17:50-18:00, launched by Claude with the user's go; build `2618b9c`; log `data/t-h1-hostmap.log` + `.times`)

Steady play, per 5 s: **~60,000 host maps, ~30,000 unmaps, ~30,000 protects** (~7 GB mapped; mostly 64 KiB), ~300 ms
of unmap time, max <0.7 ms; host regions (= section views) **4,734 at boot → ~14,900**. Episode at 17:58:49.5-52.9
(window ending 17:58:54: 96 FPS, p99 24 ms): the same 64 KiB maps / unmaps take **1-17 ms each** (~150 slow calls),
unmap time 1452 ms per 5 s. Ring-buffer capture `effect-2.etl` (saved 17:58:52.8, the user pressed F9 at the effect):
the System worker at one core for ~10 s, 55% `MiViewMayContainPage`, 4% `MiTrimSection`: the same signature.

Who maps: the log has 3 printed guest maps in the whole run, so not `sceKernelMapDirectMemory` / flexible. The 2:1
map:unmap ratio is **`sceKernelMemoryPoolCommit` / `Decommit`** (UE4's pooled allocator): a decommit unmaps the view
and maps a reserved placeholder (1 unmap + 1 map), a commit maps a view (1 map), and `pool_take_locked` hands out the
first free pages, so a recommit rarely gets its old pages and always creates a new view.

**Fix M1** (toggle bit 31 = 2147483648 restores the old path; Windows only): `pool_decommit` keeps the view
(`VirtualProtect` `PAGE_NOACCESS`, GPU unmap hook as before, VMA reserved); `pool_commit` first takes the pages a kept
view at that address maps (`pool_take_at_locked`) when they are free, and `host_map` then only changes the protection
of a view that already maps exactly that physical range (`host_view_reusable`). Otherwise the old path (the kept view
is unmapped by `carve_host`). Same data semantics (a commit never cleared pages). Stats: "pool views kept N, reused N".
Expected: unmaps and new views per 5 s near 0, region count stops growing, no episodes. Open question: whether the
trims are triggered by view deletion (expected to stop) or by the number of views (would need fewer views).

The user (2026-10-18): also specific places where panning the camera past them causes massive, repeatable frame time
spikes. To check under M1 with the same stats (pool commits in bursts, or something else).

### T-M1 result (2026-10-18 18:10-18:20, launched by Claude with the user's go; build `40a416a`; log `data/t-m1-poolviews.log` + `.times`)

M1 on for the whole run. **Unmaps 0 in every window**; kept ≈ reused (e.g. 18,900 / 18,900 per 5 s); regions level
off at ~13,400 (T-H1: still growing past 14,900). After the level settled: minutes of flat pacing (stddev 0.25-0.6 ms,
worst frames 8-12 ms) and **no 2-5 s episodes**. First ~20 s unsteady (the user): the level load (18:10:31-18:11:05,
regions 8,200 → 11,200, one 1 s frame). Heavy view 116-131 FPS at 2,500-3,000 draws/frame.

**The repeatable hitch** (the user: completely repeatable; a broken effect appears / disappears at that moment;
captures `m1-1.etl` saved 18:12:42.5, `m1-2.etl` saved 18:14:19.6): windows with worst frames 42-92 ms, and each has
slow host calls again, all the same kind: **a map that unmaps one kept view, 8-37 ms** (e.g. 18:14:06.3: 37.2 ms,
18:14:16.0: 20.2 ms). Reuse misses: the kept view's pages were handed to a commit elsewhere (`pool_take_locked` takes
the first free page, kept or not), so the next commit at its address has to unmap it, and a view unmap with ~13,000
views stalls on the kernel. ~50 misses per 5 s out of ~15,000 commits, each one a hitch frame.

**Fix M1b**: pool pages freed by a keeping decommit are `POOL_KEPT`, not free; `pool_take_locked` hands out free pages
first and kept ones only when none are free (same capacity); `pool_take_at_locked` takes free or kept. Expected: the
miss unmaps (and their hitches) near 0. The broken effect is likely the trigger, not the cause: when it spawns, UE4
commits memory for it (its rendering bug V1 is separate; a RenderDoc capture of it is still wanted).

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



### T-M1b result (2026-10-18 18:29-18:39, launched by Claude with the user's go; build `3c5704e`; log `data/t-m1b-kept.log` + `.times`)

95 in-game windows. **Views ~3,400** (T-M1 ~13,400; boot 2,356 vs 4,546), **52 kept-view unmaps in the whole run**
(T-M1 ~50 per 5 s), kept 436,826 vs reused 441,680, **2 slow host calls** in game (max 2.6 ms). Worst frame per
window: 41 ≤ 15 ms, 44 15-25 ms, 10 > 25 ms; none of the 10 has slow host calls. They are:
- 18:31:12-18:33:09: frames of 0.2-1.6 s with texture cache churn (+431..+756 images, ~1 GB uploads per 5 s), a few
  compiles: content loading / streaming. The `Stall:` lines attribute almost none of it to the port's waits
  (readback, GPU ticks, recorder, host copies, compiles): the time is spent elsewhere (likely the game's own loading;
  unconfirmed).
- 18:37:43 and 18:38:37: single 81-83 ms frames, no host calls, compiles, texture activity or port waits.
Asked the user where these happened (loading / transition / the camera-pan spots).

**The user, on T-M1b:** the repeatable hitch is still there, as are others when looking in certain areas; no
prolonged bursts any more; 18:31-18:33 was panning back and forth at the hitch spot (1% lows ~40 FPS, average ~200);
the broken effects are dynamic / spot-light shadows. The 18:33:07 stalls (`gamepad connected`) were the DualSense
being switched back on after it idled off: written off.

Stall reports of the panning hitches (full `Stall:` blocks): two kinds.
- **A, streaming**: 504-626 ms frames with **195-273 images registered and 300-400 MB of image uploads in that frame**
  (GPU thread 60-110 ms on CPU; image create 3-8 ms, refresh 9-13 ms, resident 5-17 ms): UE4 streaming textures /
  mips in. `TextureCache::UnmapMemory` frees every image in a guest range the game unmaps (pool decommits when mips
  stream out), so each stream-in is a new image and a full upload.
- **B, no GPU work**: 1,642 / 257 / 143 / 81 ms frames with 0 images, ~0 uploads and the port's waits near 0: time
  on the game's side or uncounted; needs a trace of one.
**Diagnostic C2** (`GOW3_TEXCACHE_STATS`): per report, images freed by guest unmaps and how many of them are created
again within ~30 s (count, MiB), and the top 8 created images by description (size, type, format, levels / layers,
tiled / linear, binding). If most stream-in uploads are re-creations, the cache to add is: keep images freed by an
unmap, and on re-creation at the same address with the same description and unchanged guest data (hash), reuse them
without an upload. Test T-C2: pan at the hitch spot; F9 (ring buffer) right after a kind B frame.

### T-C2 result (2026-10-18 18:50-18:53, launched by Claude with the user's go; build `9a3127a`; log `data/t-c2-recreate.log` + `.times`; ring trace `pan-1.etl` saved 18:51:41.6)

The user: the hitch happens when one of the broken shadows is generated; panning back and forth creates / destroys it
over and over; a working shadow would be created once. The create lists agree:
- **Shadow depth maps re-created repeatedly**: `512x512 D16Unorm depth target` 67 times in one 5 s window (18:52:34,
  68 re-creations of addresses freed by guest unmaps); `256x256 D16Unorm L1/6 depth target` (point-light cube shadow)
  31 times with `256x256 R16Unorm L1/6 storage` 24 times (18:51:24): the same memory used as depth and as a 16-bit
  colour storage image (compute). UE4 takes shadow depth targets from transient memory while the light is in view.
  The depth / colour R16 aliasing is also the **prime suspect for V1** (black box = the projection reading wrong data).
- **4K render targets re-created continuously** (18:51:44-18:52:14): `3840x2160 R8G8B8A8Unorm` and
  `R16G16B16A16Sfloat` render targets, 10 each per 5 s, ~860 MiB per 5 s freed by guest unmaps and created again,
  each new image uploading its guest memory first.
- **The 0.5 s panning stalls** (195-268 images, 300-400 MB in one frame): mostly first-time BC1 / BC5 textures at
  many sizes (texture streaming), only 2-35 re-creations in those windows; all uploaded within the frame.
- Kind B: a 2,775 ms frame at 18:51:16 with 0 images / uploads (in `pan-1.etl` if the ring held ~25 s).

Reading the code for these (no launch):
- New images start `Dirty`, so a re-created render / depth target first uploads its guest memory; the image freed by
  the unmap was never written back (`UnmapMemory`: "TODO: Download image data back to host"), so that memory only
  holds stale CPU-side data. Candidate fix C3: skip the first upload of an image created for a render / depth target
  binding (toggle). Saves ~170 MB/s here; not the 0.5 s stalls.
- `ResolveDepthOverlap`: a depth image bound as **storage** is re-created as a colour image (R16 for D16) and copied
  (`CopyColorAndDepth` → `CopyImageWithBuffer` via a 128 MB device buffer without maintenance8); bound as a depth
  target again, it is re-created as depth and copied back: a second ping-pong, like the volumes (V3), for the cube
  shadow maps. The copies handle layers and aspects correctly as written, so V1 is not simply a lost layer: needs a
  RenderDoc capture of the projection draw (what it samples, blend state).

`pan-1.etl` (ring buffer, ~95 s ending at the save 18:51:41.6; no thread-start events, so tids by role: 38532 render
thread, 2408 game thread, 41168 stage A, 45768 stage B, 3392 recorder; outputs `pan-1-slices.txt`): around the
2,775 ms frame (trace s 212.7-215.5) **no thread stops**: work cores flat, the render thread off CPU 170-250 ms/s in
its usual short waits, the game thread ~50% idle as always, stages A / B cycling normally. Kind B is not a stall of
the game or the GPU pipeline threads: the time is lost around presentation (flip / video out), or the frame is not
counted. Lower priority; next look: the presenter / flip path (`vk_presenter.cpp`, `videoout/driver.cpp`) and
PresentMon for the same moment.

### Panning stall anatomy (2026-10-19, `pan-1.etl`, no launch)

The ring trace holds ~60 s of data (its first ~35 s overwritten). The only sustained anomaly is the **game thread fully
busy for 0.7 s** (slices 59.1-59.8; 0.87 cores vs 0.14 normally), which matches the 654 ms stall at 18:51:04 if the ring
ends ~1 s after the F9 save print. Its samples: 48% game code (hottest block eboot 0xa22000, then 0x963200, 0x737400),
12% memcpy / memset, **14% page faults / memory manager** (normal 3%) and **14% thread synchronisation** (libwinpthread /
ntdll): mostly UE4's own streaming work on the game thread, ~28% port overhead (first touches of fresh pool pages,
the pthread layer); the GPU thread adds 60-110 ms for the 200-270 new images. (Ghidra: the project is imported without
auto-analysis, so DecompileAt created functions at the mid-function sample addresses; needs function starts first.)

UE4 config (`PS4Engine.ini` via the override pak; device profiles in pakchunk0): `r.GarlicHeapSizeMB=2600`,
`r.Streaming.MaxTempMemoryAllowed=35`; profiles set **`r.Streaming.PoolSize` 400 / 600 / 800**, and some
`r.Streaming.AmortizeCPUToGPUCopy=1` / `r.Streaming.MaxNumTexturesToStreamPerFrame=1`. A PS4 1080p pool at 4K output
thrashes: panning back and forth re-streams the same mips (T-C2: ~22 large re-creations per 5 s). Tests (cvars via
`GOW3_UE4_CVARS`, one per launch): **S1** amortize streaming (`r.Streaming.AmortizeCPUToGPUCopy=1;
r.Streaming.MaxNumTexturesToStreamPerFrame=4`), **S2** a larger pool (`r.Streaming.PoolSize=1600` with a larger
`r.GarlicHeapSizeMB` and `GOW3_DMEM_MB`; check the memory budget first).

**S1 result (2026-10-19 01:27-01:31, launched by Claude with the user's go; log `data/t-s1-amortize.log`):** the user:
"somewhat improved, but I can pretty consistently create a stutter in the same spot". Stall frames unchanged in kind:
panning bursts of 661-1,009 ms with 197-282 new images and 318-395 MB of uploads in one frame, and a 3,283 ms frame with
no GPU work (kind B). Amortizing does not split these: they are whole groups of textures evicted when panning away
(T-C2: up to 362 images / 709 MiB freed by unmaps per 5 s) and reloaded at new addresses. Memory budget for S2: the
game expands one memory pool with all direct memory (5,301,534,720 bytes = 5,056 MiB, `GOW3_DMEM_MB` unset; the port
accepts up to 16 GiB); `r.GarlicHeapSizeMB=2600` and the streaming pool live inside it. S2: `GOW3_DMEM_MB=7104`,
`r.GarlicHeapSizeMB=4600`, `r.Streaming.PoolSize=1600`.

**S2 result (2026-10-19 01:37-01:41, launched by Claude with the user's go; log `data/t-s2-pool.log`):** the game took
the memory (pool expanded by 6,912,147,456 bytes). The user: small back-and-forth pans no longer re-trigger the
stutter; a larger pan away and back does; GPU usage higher in a heavy area. Stalls: bursts of 502-623 ms (S1 661-1,009)
with the **same groups every time** (198 / 202 images ~360 MB, 277 / 282 images ~310 MB, in S1 and S2 alike); textures
freed by unmaps 5.0 GiB (S1 6.3). A fixed group of assets loads when the view turns toward an area and unloads when it
turns away, with hysteresis: UE4 visibility / region streaming, not the texture pool. Each costs ~0.5 s here (200 new
GPU images, ~360 MB uploads, the game thread's own loading work, page faults).

**Kind B is the Load Game dialog.** Every multi-second frame with no GPU work (T-M1b 1,642 ms, T-C2 2,775 ms, S1 3,283 ms,
S2 4,913 ms) ends the moment a save is chosen in the SaveDataDialog list; the game presents no frames while the port's
system dialog is open. Not a gameplay hitch.

**Test-run default from 2026-10-19 (the user: keep the larger pool, it probably needs to be at 4K):** `run-game.ps1`
sets `GOW3_DMEM_MB=7104` and `GOW3_UE4_CVARS=r.GarlicHeapSizeMB=4600;r.Streaming.PoolSize=1600` unless `-Ps4Memory`
is given; `-Env` cvars are appended. To do later in the port itself: size the pool with the render resolution
(`scripts/ue4_override.py`, `run.py` patch requirements), not only in the test script.

### The panning stall is a wait chain, not work (2026-10-19, `pan-1.etl` + Ghidra, no launch)

The game thread's hot "loop" (eboot 0xa22070) is a one-line `IsLocked` (`return atomic flag @0x6b15ed8 != 0`, MFENCE
around the load); its neighbours, reached through a function table at 0x5ababa0, are `Lock` (0xa21f50) and `Unlock`
(0xa22030), which act only on threads other than the game thread (thread id vs 0x684be08) and count holders at
0x6b15ee8. During the 0.7 s stall the game thread (2408) spins on it, switched in 1,632 times; the other threads add only
0.01-0.15 cores. Workers 47264 / 16152 / 31188 / 46496 / 28868 compute almost nothing (8-23 samples each in 0.7 s, mostly
the tracer's own stack walks, plus file-cache reads, memcpy, page faults) but are readied ~500-1,500 times each in that
second, all `UserRequest` waits: a chain of tiny handoffs (28868 → 46496 → workers → game thread), ~0.6 ms off-CPU per
wake, no scheduling delay. Guest sync maps to libwinpthread (`runtime_mutex.c`, `runtime_sema.c`, event flags in
`runtime_extra.c`); its timed waits use `WaitForSingleObject` with millisecond timeouts at the default Windows timer
resolution (the port never calls `timeBeginPeriod`), so a sub-millisecond guest timeout can take 1-15.6 ms. Port sleeps
(`compat_sleep_ns`) use a high-resolution waitable timer and are not affected.
Diagnostic `9380965`: `GOW3_WAIT_SITES=1` records every guest wait (sleep, yield, cond wait / timed wait, semaphore,
event flag, mutex lock) per call site with requested and actual time, reported by time spent per 5 s. Test T-W1: pan at
the hitch spot with it; compare the stall windows with calm ones.

**T-W1 (2026-10-19 02:15-02:21, launched by Claude with the user's go; build `9380965`; log `data/t-w1-waits.log`):**
4K memory defaults; panning stalls 517-703 ms (170-279 images, 309-396 MB). Wait sites (summed over all threads, so idle
waiters dominate the totals): `cond_timedwait` guest+0x70b37a ~3,500-4,000/s at 6-8.6 ms each, calm and stall alike;
`usleep` sites take their requested time (+0.3 ms: the port's sleeps are fine); in stall windows `cond_wait` at
guest+0x5ddc0f4 averages 9.6 ms (calm 0.7 ms) and guest+0x355152b 17-143 ms. The two busiest sites were host addresses:
the POSIX wrappers (`posix_cond_wait`, `posix_mutex_lock`, 115k-313k locks/s in stall windows) hid the guest call
sites. Fixed in the diagnostic (`posix_*` exports measure themselves; timed waits record time to the deadline).
Re-run needed (T-W2).

**T-W2 (2026-10-19 02:23-02:42, launched by Claude with the user's go; build `a2cb3a3`; log `data/t-w2-waits.log`):**
stalls 500-638 ms (195-280 images); heavy view 120-125 FPS at ~2,900 draws/frame, GPU ticks 1-3%.
- **No timer rounding:** timed waits end before their deadlines (requested 8-14 ms, actual 7-8 ms); sleeps exact.
- **Stall windows: waits get longer** at the same sites: `cond_wait` guest+0x70b46c (the busiest; likely UE4's
  event wait for task threads) 0.8-3.8 ms → 9.6 / 21 ms at 5-10k waits/s; guest+0x5ddc0f4 0.7 → 31 ms. Threads wait
  longer for a signal: what they wait for is late, not the wait itself.
- **Heavy view:** render thread's query spin `usleep(0)` guest+0x689c89 1.24 M/s; task handoffs at 0x70b46c 16.8k/s
  (0.84 ms); mutex guest+0xf5a7ce 179k/s; contended mutex guest+0x676c41 ~150/s at ~3 ms always.
- Not covered yet: `sceKernelWaitEqueue`, where UE4's RHI waits for GPU events (end of pipe, flips). Added (kind 10,
  `equeue_wait`). T-W3: same route; if equeue waits grow in stalls, the late signal comes from the port's GPU pipeline.

**T-W3 (2026-10-19 02:43-02:47, launched by Claude with the user's go; build `ff78bdf`; log `data/t-w3-waits.log`):**
- **Heavy view (the user's theory, confirmed):** `equeue_wait` guest+0x6885e7 (60 ms timeout) ~123/s = once per frame
  at ~122 FPS, **3.8-4.1 ms each** (calm ~1.3 ms); guest+0x69c1ac ~360/s at ~0.95 ms. Port counters: GPU ticks 1-3%.
  The game's render side waits about half of each ~8.2 ms frame for frame-completion events from the port's
  pipeline (stage A decode, stage B, recorder), not for the GPU; the render thread's `usleep(0)` query spin (1.2 M/s)
  is the same wait. Lever: stage A / B per-draw cost and parallelism (plan-gpu-threading.md, P1 follow-ups).
- **Panning stalls:** equeue waits do not grow (1-3.5 ms, as calm); the game's own waits do (guest+0x5ddc0f4 7-33 ms,
  guest+0x70b46c up to 5 ms). The stall chain is among the game's threads (asset loading), not the port's GPU pipeline;
  the port-specific share found so far is page faults (~14%) and pthread overhead (~14%) of the game thread's time.

### P2a profile and fix P2 (2026-10-19)

**P2a** (launched by Claude with the user's go; `run-game.ps1 -Samples`, heavy view windows 31-41: 125-134 FPS,
~2,800 draws/frame; samples `research-data\samples\p2a-heavy-profile`, symbolized `*-symbolized.txt`):
- **Stage A is no longer the limit:** ~19% yielding in `Liverpool::Process` (idle, waiting for submissions), ~9%
  condition-variable waits in `ProcessGraphics`; `PostDraw` 21.8% inclusive, of which `CollectRingBindings` 10-18%
  (constant buffer region lookups `cached_region` / `vma_index`, `CopySparseSerial` 4.4%), with **stage A blocked on
  the runtime memory lock** in `vma_index` (SRW shared acquire waits; the game's pool commits take the write lock).
- **Stage B (busy ~92%) is:** ~38% draw work (`RunDrawPacket`); **~20% spinning in `ProcessDownloadImages` →
  `WaitHostCopies` at every end of pipe** (`RunEventWriteEop`); ~13% `SubmitFlip` waits; ~12% GPU fence waits.
**Fix P2** (`texture_cache.cpp`, toggle bit 60 = 1152921504606846976): an end of pipe with no image to download and
no readback ready to publish skips the host copy wait (it protects nothing then); with readback work it still waits
(R1 skipped those too and broke the torches). The guest's fence keeps its order on stage B through
`SignalAfterHostCopies` (recording thread, after the copies, kicks recording). Stats: "end of pipe with readback work
... without (skipped)". Test T-P2: heavy view FPS / lows; torches, UI and particles must look unchanged; live A/B.
Next candidates: the runtime memory lock in stage A's region lookups; stage B's draw work.

**T-P2 (2026-10-19 03:19-03:32, launched by Claude with the user's go; build `060743e`; log `data/t-p2-eopskip.log`):**
96% of ends of pipe skip (menu: 82,813 skipped vs 3,753 waited per 5 s, ~17k/s). Live A/B at the same heavy spot
(~3,010 draws): off 118-122 FPS, on 120-124 (~+2%); worst frame per window off <= 19 ms, on 28.7 / 31.0 / 41.8 ms at
view transitions. The user: general performance and lows better outside the heavy area, **stutter amplitude much
higher**; "the big stutters happen when a torch goes from not being in view to being in view". **P2 made opt-in**
(`GOW3_IDLE_EOP_SKIP=1`). The 41.8 ms frame (torch coming into view): 3 images registered, **100.8 MB image uploads,
33 ms in image refresh**: whole render targets uploaded again (~3 x 32 MB 4K RGBA8 or ~1.5 x 64 MB RGBA16F). Suspect:
fire distortion (heat haze) sampling a scene colour copy that goes through guest memory (linear readback / write-back
marking it dirty), the user's framebuffer-through-system-memory pattern. Diagnostic: refresh report (top re-uploaded
images by bytes, usage, dirty reason). Test T-R1: torches in and out of view.

**T-R1 (2026-10-19 03:34-03:40, launched by Claude with the user's go; build `e385484`; log `data/t-r1-refresh.log`):**
the user: with no torch on screen, bringing even one into view spikes the frame time; with one already on screen,
more torches add nothing. Torch-test windows (03:37:11-03:37:46, worst frames 19-20 ms, under the 40 ms stall report):
per torch coming into view, **0x20a0000000 3840x2160 RGBA16F (64 MiB) and 0x2086000000 3840x2160 RGBA8 (32 MiB)
uploaded again, "CPU dirty"**, 2-5 times per 5 s, refreshed at the colour target binding (`vk_rasterizer.cpp`
UpdateImage before FindRenderTarget, so no usage flag yet). Also visible: the torch flames are a video (1920x1088 R8
and 960x544 R8G8 planes, six of each, refreshed ~5/s each: AvPlayer frames), which is why the first torch matters.
Cause: UE4's render target pool decommits a pass's targets while unused; `UnmapMemory` frees the images; the next
binding makes new images, which start dirty and upload their undefined guest memory. **Fix C3** (`1024224`, toggle bit 61
`UndefinedTargets` = 2305843009213693952): images made where an unmap freed images are `Undefined`; a render / depth
target binding skips their upload; any other first use uploads as before. Test T-C3: torches in and out of view, live
A/B; check that effects (fire, distortion) look the same.

**T-C3 (2026-10-19 03:49, build `1024224`): hung at boot** (the discard took `mutex`, which some callers of
FindRenderTarget / FindDepthTarget hold; std::mutex). Fixed in `d9949cb` (atomic flag update, no lock).
**T-C3b (2026-10-19 03:52-03:56, launched by Claude with the user's go; build `d9949cb`; log
`data/t-c3b-undefined.log`): the user: "Massive improvement."** Torch-test windows skip 64-288 MiB of uploads per 5 s
(mostly 96-98 MiB per torch coming into view: the 64 + 32 MiB pair); worst frames there 7-11 ms (a few 15-18 ms),
p99 4.8-10 ms (T-R1: worst 19-20 ms); heavy view ~120-134 FPS, worst ~11 ms.
The user: torches, glow and haze look as before. **Live A/B (03:57:46 off, 03:58:30 on), torch swings:** off: the
64 + 32 MiB pair re-uploaded 3 times per window, worst frames 19.2 ms; on: no re-uploads, worst frames 7-12 ms. C3
kept on by default.

### Step 1: heavy view, stage B (2026-10-19, the user: "1 then 2")

P2a stage B inclusive: `RunDrawPacket` 37.8% (`DrawRecord` 11.3%, `BindResources` 7.3%, `ResolveVertexBuffers` 5.6%
of which nearly all `vma_index`, `BindTextures` 2.6%); `vma_index` ~5% in stage A too, plus SRW lock waits. The region
cache (8 entries per thread) was valid for one table-wide generation, bumped at every exclusive lock: UE4's ~6,000 pool
commits / s emptied it constantly. **Fix L1** (`runtime_memory.c`, toggle bit 62 = 4611686018427387904): generations
per 16 MiB bucket (64K hashed counters) bumped in `vma_insert` / `vma_erase` / `split_at`; a cached region stays valid
while its buckets are unchanged; still no cached answers while a writer holds the lock. Test T-L1: heavy view FPS,
live A/B; watch for crashes (a stale "mapped" answer would fault a GPU thread).
**T-L1 (2026-10-19 04:08-04:14, launched by Claude with the user's go; build `0705e49`; log `data/t-l1-buckets.log`):**
live A/B at the same heavy spot (~2,900 draws/frame): L1 on 123-125 FPS, median 7.9-8.2 ms, p99 10.2-10.9 ms; off
120-126 FPS, 7.8-8.1 ms, 10.0-10.4 ms; stage B busy ~91% both. The user: roughly the same, 1% lows the same. **No
gain: reverted** (`bc60b8b`). In a steady view the region cache already hits; `vma_index`'s share comes from lookups
the cache does not serve. Stage B at ~425k draws/s is ~2.1 us per draw of which ~45% is waiting (end-of-pipe host copy
wait, flip, GPU fence) and ~55% draw work spread over `DrawRecord`, `BindResources`, `RunDrawPacket` itself: no single
hotspot left; the remaining lever is structural (splitting stage B's work across threads, plan-gpu-threading.md).

### Bloodborne item #1: targeted host copy waits (2026-10-19, the user: "port over anything from them that applies")

Bloodborne 0.3x notes the guest range every deferred host copy reads (64 KiB granules, 4096-slot table) and lets a
command-processor WRITE_DATA wait only when a pending copy reads its range; fences still wait for all. Ported as
`a2a81fe` (toggle bit 62 = 4611686018427387904 restores the full wait; frame stats print "N/frame skipped").
**T-B1 (2026-10-19 08:49-08:55, launched by Claude with the user's go; build `a2a81fe`; log
`data/t-b1-targeted-waits.log`):** heavy view (~2,830 draws/frame) 130-132 FPS, worst frame ~12 ms; host copies still
9% of stage A with 115-123 waits per frame, **1 per frame skipped**. WRITE_DATA is not where MediEvil's host copy
waits come from. Kept (harmless, correct); `338e0a0` adds a per-caller report of the waits (`GOW3_PERF_DIAG=1`,
"Host copy waits:" lines) to find the callers that do wait before porting the next item.
The user (T-B1): 1% lows higher on average in lighter areas, but the frame time spikes longer, as with P2. The log
does not show it: per-second p99 and worst frame by FPS band match or beat T-L1 / T-C3b (e.g. 180-230 FPS: p99
5.4 ms vs 8.3 / 5.8), but it has no measure of spike length and the runs took different paths. Only one WRITE_DATA
a frame skips its wait, so a mechanism would be that one wait used to finish the frame's copies before the next
fence. `90d7604` adds the longest run of slow frames to the pacing line; next: live A/B of bit 62 at a spike spot.

### Upload audit: what is copied from system RAM to the GPU (2026-10-19, logs only, no launch)

The user asked which large items are copied to VRAM that a PC build would keep there. From the `GOW3_TEXCACHE_STATS`
refresh sites of T-R1, T-C3b and T-L1:
- **The intro movie, every run.** Six 1920x1088 R8 + two 960x544 R8G8 images (the NV12 planes of a 1080p H.264
  frame), "CPU dirty", re-uploaded 30 times a second: ~85% of all texture re-upload bytes (11.7 GiB of Y planes per
  run), ~90 MiB/s. It lasts exactly 40 stats windows (200 s) in all three runs: `gameintro.mp4` is 1920x1080, 30 fps,
  197.2 s. So the intro plays to its end in the background, well into gameplay (the heavy view included), each frame
  decoded on the CPU (FFmpeg, then NV12 conversion), copied into guest memory and uploaded; the game also renders it
  (a refresh needs a bind). Twice per run, as it starts, `AvPlayerSource::Stop` logs "already stopped". Lead: the
  game's skip is not reaching the player (Stop refused, or never called). `6fd8e15` traces every player call
  ("AvPlayer ..." lines) for the next run. (The 1984x1088 / 992x544 pair: `credits.mp4` / the 1980-wide logo.)
- **After the movie ends, texture uploads are ~0** in steady gameplay (heavy view included): the textures stay on
  the GPU. The 3840x2160 RGBA16F / RGBA8 refreshes (a few per run) are the targets C3 already covers.
- **Buffers:** not measured per second until now (only in Stall lines). The ~115 host copy waits per frame are buffer
  uploads (UE4 constant and dynamic vertex buffers the CPU writes every frame); those are new data each frame, so they
  are not "kept in VRAM" candidates, but the double copy (guest memory -> staging -> device-local) is the PS4 shared
  memory model emulated. `55146e6` adds "Uploads: images / buffers MB/s" to the frame stats to size it. The PC
  answer would be letting the GPU read those pages directly (VK_EXT_external_memory_host import, or writing them into a
  host-visible stream buffer as Bloodborne does for uniforms); a later item.

### Buffer copies and Resizable BAR / SAM (2026-10-19, code + vulkaninfo, no launch)

The user: the buffer double copy sounds inefficient; SAM / ReBAR may help; as the CPU side gets faster the bulk copies
will be what is left. **ReBAR is on** on the test PC: memory type 4 (DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT)
sits on the full 23.57 GiB VRAM heap (without ReBAR NVIDIA exposes a 256 MiB heap for it).

How a guest buffer reaches the GPU today (`BufferCache::ObtainBuffer`):
- **Read-only, <= 16 KiB (`STREAM_THRESHOLD`), not GPU-written** (UE4 constants, most per-draw data): one CPU copy
  from guest memory straight into the 128 MiB stream buffer, `MemoryType::Stream` = VMA "prefer device" + host
  sequential write, which with ReBAR is VRAM. Already the SAM path: one copy, no GPU copy, no render pass break.
  Copied on every bind, dirty or not (no tracking).
- **Anything else** (larger or GPU-written: vertex / index / structured buffers): a sparse device-local arena; the
  dirty ranges (write tracking by page protection) are copied by the CPU into a host staging buffer
  (`HostUncached`, system RAM), then `Runtime::CopyBuffer` copies staging -> arena on the GPU. **That copy ends the
  open render pass** (`vk_runtime.cpp:116`): at 4K the targets are stored and loaded again.

What ReBAR can and cannot change:
1. *Arena writes straight into VRAM* (map the arena's backing memory from type 4, CPU writes in place): saves the
   GPU copy and the render pass break, not the CPU copy. Unsafe as is: the GPU may still be running earlier
   submissions that read the old contents (the staging copy is ordered on the GPU timeline; a CPU write is not), and
   our guest fences signal once the copies are done, not once the GPU is. It would need a wait for the GPU tick of
   the last reader of those pages: a stall.
2. *Larger read-only buffers through the stream buffer* (raise `STREAM_THRESHOLD`, a bigger ReBAR stream buffer):
   one CPU copy into VRAM, no staging, no GPU copy, **no render pass break**. Cost: stream copies are made on every
   bind, so a large static vertex buffer bound every frame would be copied every frame. Needs the per-region data
   (`GOW3_BUFFER_STATS=1`: stream vs arena bytes, bytes actually re-uploaded) to pick a threshold, or a hybrid
   (stream only ranges that are dirty).
3. *Zero copy* (import guest memory with VK_EXT_external_memory_host, the GPU reads system RAM over PCIe): removes the
   CPU copy too, the PS4's own model. Every GPU read then crosses PCIe (vertex data read per draw at 4K), and the
   guest may rewrite memory the GPU is still reading (same fence issue as 1). A research item, not a first step.
4. *Staging in VRAM* (type 4 for `HostUncached` uploads): the CPU copy writes over PCIe, the GPU copy is VRAM ->
   VRAM. Small: the CPU work is the same.

The CPU-side cost (the copy itself, the protection faults that track dirtiness, the waits) is the same for 1, 2 and 4;
the gains are on the GPU timeline (copies, render pass breaks) and in stage B (fewer copy commands and barriers).
Measure first, next launch: `GOW3_BUFFER_STATS=1` (stream / arena / re-uploaded MB per region), `GOW3_PERF_DIAG=1`
(`fde3624`: render passes ended by caller; host copy waits by caller), the new "Uploads: images / buffers MB/s".
If arena uploads break many passes per frame, option 2 (for read-only uploads during a pass) is the first change.

### T-U1: uploads, intro trace, bit 62 A/B (2026-10-19 17:1x-17:26, launched by Claude with the user's go; build `fde3624`; log `data/t-u1-uploads.log`)

- **Intro:** the game starts `gameintro.mp4` at 19.1 s (after the logo, stopped and closed normally) and **never
  stops or closes it**, not on the skip, not at its end, not at exit; its frames uploaded for 41 windows (205 s).
- **Heavy view** (~2,830 draws/frame, 127 FPS, median 7.8 ms, p99 10.5 ms, no frame over 11.7 ms): buffer uploads
  380 MB/s (~3 MB/frame), images 0. Render passes ended per frame: ~264 by barriers (`Runtime::FlushBarriers`),
  ~216 by target changes (`BeginRendering`), ~4 by buffer upload copies: uploads are not what breaks passes. Host
  copy waits: ~113/frame at the end-of-pipe readback check (`texture_cache.cpp:235`), ~17% of stage B.
- **Bit 62 A/B** (targeted waits off 17:22:56-17:25:28, same spot): median 7.74-7.85 vs 7.75-7.81 ms, p99
  10.1-10.7 vs 10.4-10.6 ms. The user: about the same. No effect either way.
- The user: very far draw distances bring the worst 1% lows with low GPU use; microstutter (a frame time graph with
  regular spikes) feels worse than a lower steady rate ("a steady 110 feels better than a stuttery 130").

**Occlusion culling is effectively off.** The port answers every occlusion query "visible" at decode
(`liverpool.cpp:1129`, see "What the render thread waits for"): UE4 then draws every primitive in the frustum,
hidden or not, and also draws a bounding box per query that does nothing here. With stage B the limit (~2.1 us per
draw), unneeded draws cost CPU, not GPU: the user's far views. Next run (`GOW3_SLOW_FRAMES=1.2`, build after
`fde3624`) counts the queries per frame and what the slow frames differ in. Then: A/B `r.AllowOcclusionQueries=0`
(removes the query boxes and the occlusion wait, culls nothing less than now), and the larger item: **real occlusion
results** (Vulkan occlusion queries between the ZPASS_DONE dumps, results written to the guest when the GPU has
them), which would let UE4 skip the hidden draws as on PS4.

### T-SF1: what the slow frames do (2026-10-19 ~17:35-17:44, launched by Claude with the user's go; build `7d3d288`; log `data/t-sf1-slow-frames.log`)

`GOW3_SLOW_FRAMES=1.2` (+ `GOW3_PERF_DIAG=1`). The user: the heavy view and the far-draw-distance view are the same
area. 3,115 slow frames (> 1.2x the window median) against 878 samples (every 100th frame; mostly lighter views):

| median | slow frames | samples |
|---|---|---|
| frame | 10.1 ms | 4.8 ms |
| occlusion queries / frame | 140 | 7 |
| render passes ended / frame | 507 | 149 |
| stage B busy | 72% | 80% |
| waiting on GPU ticks | **2.5 ms** (90% of slow frames > 1 ms) | 0 (6% > 1 ms) |

(Draws and stage A time read 0: a bug in the line, fixed in the next build.) The GPU tick waits are almost all one
site: **`buffer_cache.cpp:214`, `BufferCache::DownloadMemory` → `scheduler.Finish()`**: a GPU-written buffer range the
guest touches is copied back synchronously, waiting for the whole GPU. 3,950 downloads in the run, ~40 a second in
the heavy view, up to 5.8 ms each, **10.0 s of waiting** in total. That is the regular microstutter in the user's
frame time graph: every frame that takes a download stalls stage B for a GPU round trip.

Triggers (`ReadMemory`): a guest read fault on GPU-written memory, a guest write fault, the runtime's invalidation
hook, or a guest unmap (`Rasterizer::UnmapMemory` → `InvalidateMemory`). An unmap ends the contents, so reading the
GPU data back is wasted. **Fix D1** (`toggle bit 56` = 72057594037927936 restores the download): an unmap drops the
GPU-modified state and marks the range CPU-modified instead. The next build also counts downloads per cause and per
second ("Buffer downloads/s (GPU waits): read fault … write fault … invalidation … unmap …; unmaps discarded"). Test
T-D1: heavy view, live A/B of bit 56; if the downloads are not unmaps, the counts say which cause to fix next
(a read fault means the guest really reads GPU output: then wait only for the GPU work that wrote it, not `Finish`).

### T-D1b: the downloads are guest write faults (2026-10-19 17:49-17:53, launched by Claude with the user's go; build `057afc6`; log `data/t-d1b-unmap-discard.log`)

(First launch `t-d1-unmap-discard` exited silently at boot: S1, plan-baseline-issues.md.) "Buffer downloads/s":
**all write faults**, 25.8-27.4 a second, ~4 KiB each (0.1 MB/s), **~73 ms of GPU waits per second** (7% of the
time); read faults, invalidations and unmaps 0. So D1 (unmaps) never ran ("unmaps discarded 0.0"); the bit 56 A/B
(off 17:52:03-17:52:21) showed nothing, as the user saw ("a lot of little tiny spikes all of the time"). D1 stays
(correct, free). Heavy view slow frames as in T-SF1: median 9.9 ms, 2,780 draws, 49-51 **submissions per frame**,
~130 occlusion queries, ~495 passes ended, GPU tick wait 2.8 ms (89% of slow frames > 1 ms).

Mechanism: a shader writes a buffer (`ObtainBuffer(is_written)`, the only place that marks memory GPU-modified),
then guest code writes into the same page. The write fault downloads the GPU's data in that page's window (so the
bytes the guest does not overwrite keep the GPU's values when the page is uploaded again) and `Finish()` waits for
the whole GPU. Next build (`537d86e`): "Write-fault downloads: guest+0x… writing 0x… (64 KiB page): N/s" names the guest
code (Ghidra) and the memory. Then the fix depends on what it is: if the shader output there is never read again
(e.g. a ring the CPU refills), drop it like an unmap; if it is, wait only for the submission that wrote it instead
of `Finish()` (the GPU work recorded since then does not need to complete).

### T-D2: who writes into shader-written memory (2026-10-19 ~17:55-18:00, launched by Claude with the user's go; build `537d86e`; log `data/t-d2-fault-sites.log`)

"Write-fault downloads": nearly all at two host addresses, `0x7ff9b46de2bb` and `0x7ff9b46de4b3` (0x1f8 apart, in a
Windows system DLL: the CRT's `memcpy` / `memset`, which the runtime's `guest_memcpy` / `guest_memset` call), writing
six 64 KiB pages in turn (0x200d590000, 0x2012bf0000, 0x2015ba0000, 0x2033f80000, 0x2034850000, 0x2035e00000), ~1/s
each per site; a few from guest code (guest+0x20da4d0). So the game's libc copies write into memory a shader wrote
earlier (a pool reused, or a buffer the GPU and the CPU fill in turns).

**Fix D2** (`toggle bit 63` = 9223372036854775808 restores the fault path): the runtime keeps a bitmap of the pages a
shader wrote (set where `ObtainBuffer` marks a written binding GPU-modified, cleared where a download or an unmap
drops it). `guest_memcpy` / `memset` / `memmove` check the destination's bits (one load); a hit hands the bytes to
`BufferCache::PreciseCpuWrite`: they go into guest memory through the backing view (past the protection: no fault)
and are queued; the recording thread copies exactly those bytes into the arena before the next draw or dispatch
(`ProcessCpuWrites`, next to each `PopPendingOperations`). The page stays GPU-modified, so the shader's other data
in it is kept, and no download or GPU wait happens. Ordering: a download applies the queue first and holds the queue
lock until it has written guest memory, and a write is queued before it goes into guest memory, so neither can
overwrite the other's newer bytes. The idea follows the Bloodborne port's `NotePreciseUpload`, which learns of the
write afterwards (it runs without page protection); here it has to come before the write. Frame stats: "precise
CPU writes N (MB)". Test T-D2b: heavy view, live A/B of bit 63; watch for broken effects (wrong data in the GPU copy).

### T-D2b: D2 result (2026-10-19 18:02-18:13, launched by Claude with the user's go; build `1bc2dda`; log `data/t-d2b-precise-writes.log`)

Synchronous buffer downloads **0** for the whole run (write faults 0.0/s, was ~27/s); ~130-200 precise CPU writes a
second (1.4-1.7 MB/s). Heavy long-distance view (2,800-3,000 draws/frame), T-D1b → T-D2b: FPS 114-121 → 119-127,
median 7.7-8.2 → 7.9-8.3 ms, **stddev 1.1-1.5 → 0.34-0.68 ms, p99 10.9-13.0 → 8.8-9.8 ms**; light views stddev down to
0.08 ms. The user: "The 1% lows are much more stable looking at the heavy long distance area, a significant
improvement. The frametime graph in general looks significantly better, even outside of that area." No broken
visuals reported. **Kept, on by default.**

**Correction: the live toggles did not reach T-U1, T-D1b and T-D2b.** The runtime watches the toggle file only when
`GOW3_TOGGLE_FILE` is set; `run-game.ps1` did not set it (earlier runs passed it by `-Env`). So the bit 62 A/B in T-U1
and the bit 56 A/B in T-D1b changed nothing (the "about the same" verdicts compare identical builds; for bit 62 the
targeted waits skip ~1 wait a frame, for bit 56 D1 never ran, so neither conclusion changes), and the bit 63 A/B
attempted at 18:12:34 in T-D2b did not happen (precise writes kept counting). `run-game.ps1` now always sets the
file and resets it to 0 at each launch. The D2 comparison above is across runs, same spot.

**Correction 2: the 4K boot patch was missing from T-B1 to T-D2b.** The user (after T-D2b): "Performance and
micro-stutter in general are VASTLY improved. The heavy area is where it's the worst, and even that's fairly
acceptable right now. Though it appears we broke our 4k patch that fixes button inputs on menus somehow." Not the
code: the runs up to T-L1 passed `GOW3_UE4_BOOT_4K=1` by `-Env` (as `launcher.cmd` does); from T-B1 the launches
did not, so the logs show `Patches: ... ['Native 4K framebuffer pool (512 MB)']` without '4K display metrics at boot'
and I1 came back. `run-game.ps1` now sets it by default. Comparisons within T-B1 … T-D2b (T-D1b vs T-D2b for D2) are
like for like; comparisons of those runs against T-L1 and earlier mix the two boot modes.

### T-D2c: D2 live A/B with the toggle file and the 4K boot (2026-10-19 18:46-18:5x, launched by Claude with the user's go; build `8779190`; log `data/t-d2c-precise-ab.log`)

First run with `run-game.ps1` setting the toggle file (`mask=0` at start, `mask=9223372036854775808` at the switch:
the toggle took) and the 4K boot (`'4K display metrics at boot'` in the patch line). D2 off 18:53:07, on 18:54:43,
heavy view (~2,950 draws/frame).

| heavy view | D2 on (18:52:43-18:53:08) | D2 off (18:53:18-18:53:33) | D2 on (18:54:48-18:55:03) |
|---|---|---|---|
| FPS | 121-127 | 124-127 | 126-129 |
| stddev | 0.38-1.10 ms | 0.47-0.68 ms | 0.38-0.95 ms |
| write-fault downloads | 0-10/s (D2 on: direct guest writes) | 26.5/s at the switch, then 2-4/s | 0-3/s |

The user: off "was significantly worse when you first switched, but got a bit better after a few seconds"; on again
"much flatter … that's for sure, and there's no stutters while panning. **D2 definitely stays in.**"

Read: with D2 on, pages stay GPU-modified (D2 never clears that state), so switching off makes them all fault at once
(the burst the user saw). With the 4K boot the libc writes into shader-written pages are rarer than in T-D1b (no 4K
boot, ~27/s): 2-4/s remain with D2 off once the burst is over. With D2 on, **guest code writing directly** (not libc)
into the same pages, `guest+0x68e109` / `guest+0x68e149` / `guest+0x20da4d0` / `guest+0xba9a49` into page
0x2049ae0000 (beside the RHI query code at 0x689c10-0x689ed0), faults and downloads 5-10/s, more than with D2 off,
because the GPU-modified state D2 keeps covers bytes the CPU has already rewritten. **Refinement D2b:** once
`ProcessCpuWrites` has copied the CPU's bytes into the arena, both copies agree on them: subtract them from
`gpu_modified_ranges` (and clear the bitmap bits of pages left with none). A later direct write there finds nothing to
download, and `DownloadMemory` returns before `Finish()` (it does when it has no range). Then look at what
guest+0x68e109 writes (Ghidra) together with the occlusion work.

**T-D2c, after the A/B:** the user: "It seems to have 'settled' after being on for a bit, but then some frametime spikes
showed back up". D2 was on throughout (`mask=0` from 18:54:43); from ~18:56 the direct-write downloads came back at
5-10/s and p99 rose to 10.2-11.5 ms. Ghidra (`DisasmAround.java 68e109`): guest+0x68e109 / 0x68e149 are the 32-byte and
16-byte `vmovups` store loops of an **inlined copy** in the game's own code (a size switch at 0x68e0bf: > 64 bytes
elsewhere, >= 32 and >= 16 here, then 4-byte steps), not a libc call: D2 cannot see it. **D2b built** (`buffer_cache.cpp`
`ProcessCpuWritesLocked`; `GOW3_PRECISE_WRITE_RELEASE=0` keeps the first D2 behaviour; every toggle bit is in use).
Limit: D2b removes the downloads D2 itself caused (bytes D2 already rewrote still marked GPU-only); a direct write
into bytes a shader wrote since is a real first write and still downloads, as with D2 off (2-4/s here). Removing those
needs the writer's range before it writes (no libc call to hook) or a download that waits only for the work that
wrote the page; next to look at with the occlusion work, since the writer sits beside the RHI query code.

### T-D2d: D2b result (2026-10-19 ~19:00-19:04, launched by Claude with the user's go; build `42a4943`; log `data/t-d2d-release.log`)

Heavy view (~2,785 draws/frame), the last 80 s: **write-fault downloads 0**, 127-130 FPS, median 7.6-7.8 ms, **stddev
0.32-0.48 ms, p99 8.6-9.5 ms** (T-D2c, D2 only: 0.38-1.10 ms, p99 up to 11.5 ms with 5-10 downloads/s). Whole run:
downloads in 12 of 46 windows, all early, mean 2.0/s. The user: "Looks good. I think some of the initial spikes are
from the movie issue. The game has lots of little spikes when first loading in … It stops after a couple minutes. …
I've been in the heavy area for about a minute now, and the frametimes look great." The intro starts at 11.6 s
(AvPlayer trace) and decoded ~200 s in earlier runs: the movie fix is the next check for the early spikes.
**D2 + D2b kept.**

### T-O1: occlusion queries off (2026-10-19 ~19:20-19:26, launched by Claude with the user's go; build `42a4943`; log `data/t-o1-no-occlusion.log`)

`GOW3_UE4_CVARS='r.AllowOcclusionQueries=0'` (the log's cvar line has it; slow-frame lines: **0 occlusion queries**,
was ~130-160 a frame in the heavy view). Heavy view: ~2,635 draws/frame at 132-135 FPS, stddev 0.31-0.46 ms, p99
8.4-9.2 ms (a second spot: ~2,800 draws, 126-128 FPS); T-D2d (queries on): ~2,785 draws, 127-130 FPS, stddev
0.32-0.48, p99 8.6-9.5. ~150 fewer draws a frame (one bounding box per query) at the same draw rate (~350k draws/s
both): **~+5% from not drawing the query boxes**; no sign that the occlusion wait itself cost more. The user: "I don't
see any issues, nothing stands out to me, GPU usage seems about the same, maybe slightly higher.. but so are my 1%
lows and average FPS." Made the default of `run-game.ps1` (an O2 test passes `r.AllowOcclusionQueries=1`).

**Origin of the synthetic answer** (asked by the user): not MediEvil- or God of War-specific. shadPS4's own
`liverpool.cpp` (current main) has the identical block (`ZpassDone` / `PixelPipeStatDump` → every render backend's
counter written with the valid bit and an ever-growing `pixel_counter`, no comment) and no real Vulkan query path. It
came into this repo with the root commit `6849d33` (deadinside, 2026-09-25, "Native Bloodborne loader with HLE runtime,
shadPS4 video core and performance work"), which this port shares with the Bloodborne port. No note in shadPS4, the
Bloodborne port or upstream explains it; the evident reasons: "visible" can never hide what should be drawn and never
leaves a game waiting. The remaining win is **O2, real occlusion results**: the heavy view still draws every primitive
in the frustum, hidden or not; useful beyond MediEvil (shadPS4, Bloodborne), so worth offering upstream.

### O2: real occlusion query results (2026-10-19, `GOW3_OCCLUSION=1`, built, not run yet)

`vk_occlusion.{h,cpp}`. Each `ZPASS_DONE` (`PixelPipeStatDump`) goes to the recording thread in order with the draws
(`Rasterizer::OcclusionEvent` → `RunInOrder`) and ends a chain of Vulkan occlusion query **segments**: one is active
while a render pass is open (a `Scheduler::RenderPassHook` begins one after `beginRendering` and ends it before
`endRendering`; command buffers end outside passes), an event remembers how many segments ended before it. A
**completion thread** (`gow3:Occlusion`) waits on the timeline semaphore for the submission holding them, reads the
segments with `vkGetQueryPoolResults`, adds them up in order and writes each event's running total into the game's
memory through the backing view: the first depth block's counter gets the total, the others 0, all with the valid bit.
A begin event (16-byte aligned) clears its begin and end counters first, so an earlier use never reads as valid early.
Slots (16,384) are reset on the host after reading (`hostQueryReset`), so no reset is ever recorded; a segment that
finds no free slot is counted as 2^32 samples (that query reads visible, never wrongly hidden). Exact counts
(`occlusionQueryPrecise`, now enabled). Design after the Bloodborne port's `vk_occlusion.h` (0.5): there the GPU
writes the totals into guest memory itself (guest memory is GPU memory there); here the GPU works on a copy, so the
host writes them after the submission completes.

Risks: (1) **wrong culling** if our depth at query time differs from the PS4's (objects popping or missing); (2) the
game's render thread **waits** for results (`bWait`, `r.NumBufferedOcclusionQueries=1`: last frame's queries) until
the GPU has finished that frame's queries: if the pipeline is deep, the render thread waits longer than on PS4 (the R4a
lesson). Stats line every 5 s: "Occlusion queries: N events/s, N queries/s of which X% hidden; results written X ms
after the event (max); N segments missed".

Test T-O2: `-Env @{GOW3_OCCLUSION='1'; GOW3_UE4_CVARS='r.AllowOcclusionQueries=1'}`, heavy view and the far views:
draws per frame against T-O1 / T-D2d, FPS, slow frames, the hidden share and the latency; the user watches for popping.

**T-O2, first launch (2026-10-19 19:43-19:46, launched by Claude with the user's go; build `11eba14`; log
`data/t-o2-occlusion.log`): hang after the intro skip, before O2 ran.** The user: "it stalled after the menu movie
skip". The log stops at 19:44:37 in the menus (5 draws a frame, 0 occlusion events: `Rasterizer::OcclusionEvent` was
never reached, no `gow3:Occlusion` thread). Thread stacks taken with `cdb -pv` (noninvasive) at 19:46, saved as
[data/t-o2-hang-stacks.txt](data/t-o2-hang-stacks.txt): the port's GPU threads idle (stage A and `gow3:VkRecorder` on
their work condition variables, `gow3:DrawRec` on its atomic wait); the game's main thread, `RHIThread` and
`SubmitGPUComman` in `pthread_cond_wait`, `RenderThread 1` in `pthread_mutex_lock`: a lock-up among the game's
threads. Differences from the last good runs: `r.AllowOcclusionQueries=1` (the default of every run before T-O1) and
`occlusionQueryPrecise` enabled on the device. One occurrence: relaunch; if it repeats, separate the two (queries on
without O2, then O2 without precise counts).

### T-O2b: O2 runs (2026-10-19 19:47-19:52, launched by Claude with the user's go; build `11eba14`; log `data/t-o2b-occlusion.log`)

Relaunch of T-O2: no hang this time (the user: "Looks like it survived this time"). O2 works: up to ~23,000 events /
~11,600 queries a second, **39-63% of queries hidden** (the last windows 62-63%), results written 7-9 ms after the
event on average, 0 segments missed. The busiest windows: ~1,630-1,700 draws a frame (the heavy view had ~2,800 with
every query visible; whether these windows are the same spot is to be confirmed with the user), but **86-89 FPS**,
p99 12.6-16.4 ms. Why: the game's render thread waits for last frame's results (`bWait`,
`r.NumBufferedOcclusionQueries=1`), which land ~8 ms after the event (the GPU finishing that frame plus the completion
thread): stage A idle 53%, stage B busy only 40-46% (was ~90%), GPU tick waits ~0. The R4a cost, now with a reason.
Next: T-O2c with `r.NumBufferedOcclusionQueries=2` (results two frames old, long written by then; culling one frame
later). If that is not enough: shorten the latency (submission boundaries, the completion thread's wake-up).

### T-O2c: two-frame-old queries (2026-10-19 19:55-19:59, launched by Claude with the user's go; build `11eba14`; log `data/t-o2c-buffered2.log`)

`r.NumBufferedOcclusionQueries=2`. Heavy view: **1,450-1,660 draws a frame (was ~2,635-2,800), 168-192 FPS (T-O1:
126-135), median 5.2-5.8 ms (7.4-7.9)**, but p99 8.5-9.8 ms (8.4-9.2) and stddev 0.94-1.28 ms (0.31-0.46): the
average up ~40%, the lows about where they were, so relatively much worse. 63-68% of queries hidden, ~27,500 queries a
second, results 7.9-8.5 ms after the event on average (max 11.5-13.9 ms), 0 segments missed. Slow frames: stage B
73% busy with no waits of its own (samples 77%): starved, the render thread still waits when a result is later than
two frames (~11 ms at 5.5 ms a frame). The user: "framerates are much much higher, but the 1% lows are slightly worse
in the heavy area, probably 15-20%"; no pop-in noticed (T-O2b and T-O2c); menus and intro fine.

The user asked whether the results could go straight to memory the CPU reads (the shared / ReBAR memory) instead of
being copied. The copy itself is microseconds (eight u64 per event, one `vkGetQueryPoolResults`); a CPU-visible
buffer would still need copying into the game's memory. What would remove our step is the GPU writing into the game's
memory itself (as in the Bloodborne port, whose guest memory is GPU memory): importing the result pages
(`VK_EXT_external_memory_host`), unknown for our section-backed views. Measure first: the stats line now splits the
latency into event → submitted → GPU done (as seen) → written (build `1daae1a`). Next run: that, plus
`r.NumBufferedOcclusionQueries=3` (results three frames, ~16 ms, to arrive).

### T-O2d: three-frame-old queries, latency split (2026-10-19 20:04-20:09, launched by Claude with the user's go; build `1daae1a`; log `data/t-o2d-buffered3.log`)

`r.NumBufferedOcclusionQueries=3`. Heavy view: **~1,570 draws a frame, 188-203 FPS, median 4.7-5.0 ms, p99 7.7-8.75 ms**
(best yet; T-O1 8.4-9.2, T-O2c 8.5-9.8), stddev 0.65-0.96 ms, 20-60 frames over 1.5x median per 5 s, runs of two
~16.5 ms frames, occasional runs of 6 (49-53 ms). 67-68% of queries hidden. The user: "Avg FPS went even higher in
the heavy area (close to 200), 1% lows range anywhere from 75 to 135, lots of large spikes. So they're going lower
than they used to at times, and they're unsteady, but we are able to get bursts of very high 1% lows."

**Latency split** (average per event): event → submitted **2.6-3.0 ms**, submitted → GPU done **4.8 ms**, done → written
**0.00 ms**. So the user's direct-write idea would save nothing: the time is before the result exists. Max 13-16 ms
against three frames of ~4.8 ms (~14.4 ms): results still arrive late at times, and the higher the frame rate the less
time a fixed number of buffered frames covers.

Slow frames (3,835) vs samples: stage B 63% vs 89% busy; 23% of slow frames wait on GPU ticks > 1 ms. Waits:
`vk_presenter.cpp:553` ~19% of the time (the presenter waiting for the GPU, one frame queued: the GPU is now a limit at
~200 FPS in 4K); `buffer_cache.cpp:288` (`DownloadMemory` → `Finish`) 13-18/s, ~6%, up to 7.7 ms: write-fault
downloads from the game's direct writers (guest+0x20da4d0, 0x68e109, 0x68e149, 0xba9a49) into one page
(0x204c980000), the class D2b does not cover (genuine first writes after a shader write).

Next: **O3** submit the recorded queries sooner (the 2.6-3.0 ms before submission); **D3** a write-fault download waits
only for the submission that last wrote the range, not `Finish()` (the shader wrote those pages earlier in the frame,
usually in a batch the GPU has finished: often no wait at all).

### O3: submit the queries when their render pass ends (2026-10-19, built `b6dfc40`, not run yet)

A render pass that held occlusion events sets "submit wanted" when it ends (`OcclusionQueries::OnEndRendering`); the
recording thread makes the submission before its next draw or dispatch (`Rasterizer::BeforeRecord`, which also runs
the D2 queue). UE4 issues its queries in one batch, so ~1 extra submission a frame; stage B already submits when a
download's `Finish` runs there, and the submit callback (barriers, sparse arena binds) is not stage-specific.
`GOW3_OCCLUSION_SUBMIT=0` turns it off; the stats line counts "early submissions/s". Test T-O3: as T-O2d
(`r.NumBufferedOcclusionQueries=3`), expect "event → submitted" well under 2.6 ms, fewer starved slow frames.

### T-O3: a submission per query pass is too many (2026-10-19 20:21-20:25, launched by Claude with the user's go; build `2b4e699`; log `data/t-o3-early-submit.log`)

Results **2.08 ms after the event** on average (max ~4 ms; event → submitted 0.04 ms, → GPU done 2.04), 67-68% hidden,
but **~16,000 early submissions a second (~128 a frame)**: MediEvil draws each query box in its own render pass
(~10,000 `BeginRendering` pass changes a second), so every pass with an event asked for one. Each submission syncs the
Vulkan recorder and waits for host copies: stage B 95-98% busy (recorder waits 0.8-1.7 ms in slow frames), heavy view
**126-129 FPS, p99 13.7-14.1 ms, stddev ~2.1 ms**. The user: "the AVG and the 1% lows are about as bad as they've ever
been. Persistent oscillations on the frametime graph with a high amplitude." **Rate-limited** (`cdb43af`): a pass end
submits only when the oldest unsubmitted event is >= 1 ms old (`GOW3_OCCLUSION_SUBMIT_US`, default 1000, 0: off).
Expected: a few submissions a frame, results within ~3 ms. Test T-O3b, same settings.

### T-O3b: rate-limited O3 (2026-10-19 20:26-20:32, launched by Claude with the user's go; build `df29fe7`; log `data/t-o3b-rate-limited.log`)

~183 early submissions a second (~1 a frame); results **3.4 ms after the event** (max ~5 ms; event → submitted 1.3,
→ GPU done 2.1) in the heavy view, 5.5-5.8 ms (max 7.7, once 13) in a GPU-heavier stretch; 0 segments missed;
57-71% hidden. Heavy view: **181-185 FPS, p99 8.0-8.5 ms**, stddev 1.0-1.4 ms; write-fault downloads ~4/s. The user:
"Much better, but there's still a lot of lower amplitude less frequent spikes, sometimes the game will actually stall
for a solid second or two. … If I don't move the camera, 1% lows can sit between 110 and 140, with an average of
190. Panning the camera causes a lot of small stalls, and looking in certain directions can get a steady
oscillation."

- **The oscillation** (from 20:30:40): slow frames exactly every ~43 ms (~8 frames), each with ~1,840-1,930 draws and
  220-257 queries against ~1,560-1,600 draws and ~125-190 queries in the others, stage B ~95% busy: a periodic batch
  of ~250 extra draws, CPU-bound. Not in T-O1 or T-D2d (stddev 0.31-0.48): UE4's handling of hidden primitives now
  that it gets real answers (likely its grouped re-tests of occluded primitives: a group read visible draws all of
  them until each is tested again; UE4 knowledge, not verified in this binary). The binary's occlusion cvars are
  UE 4.21's (r.NumBufferedOcclusionQueries, r.HZBOcclusion, r.FramesToExpandNewlyOcclusionTestedBBoxes,
  r.ExpandNewlyOcclusionTestedBBoxesAmount, r.GFramesNotOcclusionTestedToExpandBBoxes, r.NeverOcclusionTestDistance,
  r.DownsampledOcclusionQueries, …).
- **Stalls:** 493 / 535 ms at 20:27:09 / :18: area loading (199 / 271 images registered, 376 / 260 MB of image
  uploads, compiles: the region-load item). **999.9 ms at 20:28:20**: stage B 0% busy, nothing uploaded, no waits of
  ours: the game produced no frame for a second. Not a missing occlusion result: every result in that stretch was
  written within 13 ms, and a missing one would block all later results (a permanent freeze, not one second).
  Unknown: `GOW3_WAIT_SITES=1` next run.

Next (T-O3c): `r.NumBufferedOcclusionQueries=2` (results now land within ~5 ms; two frames ~10.6 ms) + `GOW3_WAIT_SITES=1`.

### T-O3c: two-frame-old queries again, guest wait sites (2026-10-19 20:41-20:53, launched by Claude with the user's go; build `b6f9536`; log `data/t-o3c-buffered2.log`)

`r.NumBufferedOcclusionQueries=2`, `GOW3_WAIT_SITES=1`. **Heavy view, camera still: 205-221 FPS, median 4.5-4.7 ms, p99
5.4-6.0 ms, stddev 0.25-0.46 ms** (~1,400-1,520 draws a frame, 63-73% of queries hidden, results 3.3-4.4 ms after the
event): the best so far, and the 8-frame oscillation of T-O3b gone at this spot. No long stall. The user: "No long
stall, 0.1% lows looks a lot better, though there does appear to be a sort of 'judder' going on. It might be worth
enabling some sort of frame buffering, but it's clear something is still stalling out, there's a large delta on 1%
lows, and 0.1% lows are less than 50% of the average... But we're getting better." Then: "if I pan the camera at all
the lows tank."

- **Isolated long frames** (the 0.1% lows): e.g. 58.4 ms at 20:48:45, in the same window as an occlusion result that
  took **59.2 ms** (average 4.4): the game waited for an outlier. Which part of the latency blows up is not known yet
  (the stats give averages and one max).
- **Panning** (20:52:10-15: 183 FPS, p99 9.8 ms, 50 slow frames; still: 216-221 FPS, p99 5.6-6.0): every slow frame has
  **stage B 85-97% busy** with no waits of its own, 1,340-1,610 draws, 128-214 queries, host copies 0.5-1.0 ms a frame
  (still: ~0.4): the view changing brings draws and queries, and stage B, recording every draw at ~2 us, is the limit
  again. Not occlusion latency (3.3-4.3 ms throughout). The structural fix is the user's third item: stage B across
  threads.
- **Guest waits**: `usleep(0)` at **guest+0x689c89**, ~1.3 M calls/s, ~1.4 s per 5 s: the query-wait spin of "What the
  render thread waits for" (2026-10-18), or the same code waiting on GPU timestamp queries (state 2): to tell apart.

Next: (1) per-part maxima of the result latency (find the 59 ms outliers); (2) Ghidra: who spins at 0x689c89
(occlusion or timestamps); (3) the user's frame-buffering idea (`frames_queued=2`) for the judder, its own launch;
(4) the stage B multi-threading study.

## CPU oscillation, waits and hitches: trace cpu-osc-1 (2026-10-25)

Trace `research-data\wpr\cpu-osc-1.etl` (WPR full CPU profile, recorded by the user during gameplay with fights; 4K
borderless, Reflex limiter, 119 cap, engine Unlimited; game log `user\last_run.log`: worst frames 36 / 57 / 75 ms).
Exported with `xperf -tle -tti -i ... -symbols -a dumper -stacktimeshifting` (8.7 GB; `_NT_SYMBOL_PATH` =
`research-data\symbols-game;srv*C:\symbols*...`), filtered with grep to the game's samples and all CSwitch /
ReadyThread records. New tools: `research/tools/etw_gaps.py` (longest off-CPU stretches of chosen threads, wait
reason, readying thread), `research/tools/etw_locks.py` (kernel lock waits by the process that released them).

- **No 1 s rhythm in the game's CPU.** 0.1 s slices: 4.6 cores falling slowly to 2.5 with the scene, smooth within
  every second; sleep / yield spinning a steady ~0.2 core. System-wide per second: nothing oscillates either
  (background: MSI Afterburner a constant 0.22 core, short sqlservr / VS Code / Visual Studio bursts). The 10-27%
  swing seen in the overlay is not in the trace's 1 kHz samples **[V]**; a measurement artefact of the monitor is
  likely **[I]**.
- **The hitch: MSI Afterburner's GPU polling.** The only long stall (33 ms at 10.63 s) starts in stage B
  (`gow3:DrawRec`) switching out on `WrResource` (a kernel resource lock), readied by `MSIAfterburner.exe`; the
  recorder, the present thread and UE4's RHI / render threads wait behind stage B. Afterburner's thread at that moment:
  its window timer -> `RTHAL.dll` -> `nvapi_impl.dll` -> `NtGdiDdDDIEscape` -> `DXGADAPTER::DdiEscape` ->
  `nvlddmkm.sys` (its hardware monitoring through NVAPI driver escapes) **[V]**. In 20 s: 11 such waits, 62 ms in
  total (33, 5.1, 4.1, 3.9, 3.9, 3.1, 2.6, 2.4, 1.8, 1.1, 0.8 ms), all but one in stage B; no other process causes a
  lock wait over 0.7 ms **[V]**. **The blocked call** (the stack of stage B's switch-in, which xperf gives to the
  thread switched in): `BufferCache::EnsureResident` -> `vk::Device::allocateMemory` -> `nvoglv64` ->
  `D3DKMTCreateAllocation` -> `DXGDEVICE::CreateAllocation` -> `nvlddmkm` -> `ExAcquireResourceSharedLite`: a GPU
  memory allocation waiting for a driver lock that Afterburner's escape held **[V]**. (Stage B's first sample after
  the wait, at the `stream_class` insert in `ObtainBuffer`, is only where it was next; the heap-growth guess in the
  first version of this note was wrong.)
- **Stage B calls the GPU driver's memory manager during gameplay:** in the trace, 254 stack records of stage B
  inside `D3DKMTCreateAllocation` (88) or `D3DKMTDestroyAllocation2` (166), in bursts (69 and 41 in single seconds)
  **[V]**. Creates: buffer arenas made resident on first use, one `vkAllocateMemory` per new block range
  (`EnsureResident`). Destroys: `TextureCache::DeleteImage` -> `~UniqueImage` -> VMA `FreeVulkanMemory` (VMA frees
  a memory block once empty) **[V]**. Each takes the driver's lock, so any GPU monitor's polling (NVAPI) can stall
  stage B, and the calls themselves are kernel round trips.
- **Confirmed by the user (run `t-c1-no-afterburner`, Afterburner closed, RTSS alone):** 0.1% and 1% lows higher, no
  recurring frame time spikes on the graph.
- **The user, 2026-10-26, heavier area, uncapped, Afterburner closed, with T-M1 and the earlier fixes:** 0.1% lows
  ~170 FPS, average 200+; "MSI was dragging down performance quite a bit" (the lock waits throttle stage B, not
  only the 33 ms hitches).
- **Spinning / waiting that costs CPU:** the recorder (`gow3:VkRecorder`, 0.48 core) spends 58% of its samples in
  its own pre-sleep spin (`Scheduler::RecorderThread`: 200 us of `pause` after every chunk on >= 12 hardware
  threads, upstream 2026-09-25, never measured here) **[V]**; stage A ~17% in `Sleep(0)` / yield; NVIDIA's own
  Reflex pacing thread (starts in `nvoglv64.dll`, `NtDelayExecution` loop) 0.13 core. The `gow3:DrawPrep0-7`
  workers ~0.06 core each with ~40% in the kernel and ntdll (waits) **[V]**.
- **The intro movie decodes during gameplay:** `gameintro.mp4` (197 s) is started at 7.35 s and never stopped or
  closed (the logo movie is); `shadPS4:AvVideoDecoder` used ~0.2 core throughout the trace, much of it page faults /
  page zeroing (new frame memory per frame) **[V]**. The decoder only runs while the game takes frames, so the game
  keeps the player running after the intro is skipped, until the movie's end **[I]** (to confirm with the capture's
  time after launch).

Next: (1) done: confirmed without Afterburner; (2) keep the driver's memory calls off stage B: buffer residency in
larger pre-made chunks or from a pool filled ahead on another thread, and image memory freed on a worker (or empty
VMA blocks kept for reuse); (3) the recorder spin as a switch (200 / 20 / 0 us), CPU and pacing compared; (4) the
intro movie: in `t-c1` it was started at 14.9 s and again never stopped; the user played well past its 197 s in the
traced run, so it plays the whole session or restarts: confirm, then stop the decode once the game stops showing it.

### T-M1: intro movie suspended at the level change (2026-10-26, launched by Claude with the user's go; log `data/t-m1-movie-suspend.log`)

Study: [study-intro-movie.md](study-intro-movie.md) (the game plays both movies on one persistent UE4 MediaPlayer; the
only Stop/Close is in its AvPlayer object's destructor; the skip only ends the widget and opens the next level, so
the hidden intro kept decoding for 197 s). Change: `src/level_hook.h` redirects the call to
`UGameplayStatics::OpenLevel` in the Blueprint native `execOpenLevel` (0x290eeda, byte-checked) through a stub to
`medievil_level_change` -> `AvPlayer::SuspendPlayersForLevelChange`: every player still playing gets its source's
pause flag only (no state change, no event). `GOW3_MOVIE_SUSPEND=0`: off.
- Log: hook installed; the skip opened a level at 15.9 s and suspended `gameintro.mp4` at 187 frames; its frame count
  never moved again; the game itself called `Stop -> 0` at 206.9 s (its own clock reached the end), as predicted
  **[V]**. (Later level changes at 55 s and 121 s found the same player again; already-paused players are now
  skipped.)
- Image uploads: ~90 MiB/s for ~200 s before (T-U1); now 0-5 MB/s from ~5 s after the skip (ordinary streaming), the
  whole session **[V]**.

### Stage B's GPU memory calls moved off it (2026-10-26, launched by Claude with the user's go)

- **Step 1, image memory released on a worker** (`texture_cache/image.cpp` `ImageReleaser`, thread `gow3:ImageRelease`;
  `GOW3_IMAGE_RELEASE_THREAD=0`: inline as before). `UniqueImage::Destroy` queues `vmaDestroyImage` (VMA frees an
  empty shared block with `vkFreeMemory`); an allocation that fails drains the queue and retries; the instance drains
  it before `vmaDestroyAllocator`. Trace `img-release-1` (Afterburner on, `t-ir1-image-release.log`): lock waits on
  Afterburner 11 / 62 ms / max 33 ms -> 1 / 1.5 ms (a driver thread, not stage B); stage B in
  `D3DKMTDestroyAllocation2` 166 -> 0 (frees on the worker 33, stage A 8) **[V]**. A/B without Afterburner
  (`t-ir2-release-off.log`): heavy-area windows the same within noise (FPS 194 / 195, p99 6.9 / 6.6 ms); the user saw
  no real difference (0.1% lows 80s on vs high 90s off in one fight, likely run to run).
- **Step 2, buffer residency from a pool** (`buffer_cache.cpp` `ResidencyPool`, thread `gow3:Residency`;
  `GOW3_RESIDENCY_CHUNK_MB`, default 32, 0: one allocation per call as before). Resident blocks are never released,
  so `EnsureResident` takes pieces of 32 MB chunks in order (a range across a chunk end becomes two binds); the worker
  allocates the next chunk at half use. `t-rp1-residency-pool.log` (Afterburner ON): 21 chunks, all but the first
  allocated ahead, none on demand **[V]**; heavy-area windows FPS 193 (min 190), p99 6.84 ms, the same as the
  Afterburner-off runs. Trace `residency-1` (ring, 22:25:25-22:26:58): Afterburner lock waits 3 / 1.9 ms; the 321 ms
  stall is the Load Game dialog and level load (22:25:31-32); in the fight, a ~300 ms run of 7-9 ms frames was paced
  by NVIDIA Reflex's sleep (present thread readied by the driver's pacing thread, stage A waiting on presents):
  GPU-bound or GPU-busy, not a CPU stall **[V]** pattern, **[I]** cause. The user: the GPU's "usage" may not be real
  load (seen before); next: the frame trace (`gpu_ready`) and `GOW3_GPU_PROFILE` on the dips.

### N-1: Nsight Systems, GPU busy vs working hard in the fight (2026-10-27, launched by the user; report `research-data\nsys\nsys-fight-1-1.nsys-rep`, log `data/nsys-fight-1.log`)

Setup (`run-game.ps1 -Nsys`, Nsight Systems 2026.5.1, admin): Vulkan trace (per-command-buffer GPU ranges), GPU
metrics `ad10x` at 20 kHz, GPU/CPU context switches; 8.5 s from just before a big fight (01:14:44.354 session start).
Getting there: Nsight's Vulkan layer jumps to a null pointer at `vkGetPhysicalDeviceToolProperties` (boot crash;
skipped by `GOW3_SKIP_TOOL_INFO=1`, set by `-Nsys`); its WDDM trace hangs `nsys stop` forever (found with
`research/tools/nsys-bisect.ps1` on a dummy program; dropped); its own hotkey never reached the game (the script holds
F9 itself and runs `nsys start` / `nsys stop`). Reflex off for this run (precaution, not a cause). Tracing overhead
is high (median frame 6.4 ms vs ~5 ms normally; the profiler thread ~9 ms on-CPU per frame).

Result (subagent with Nsight's analysis skill: bounded SQL per present-to-present frame; method and queries in the
session scratchpad `nsys-analysis/frames.sql`): **in the fight's dips the GPU works hard, it is not just busy [V]**.

| Per frame (mean) | Ordinary (n=271) | Slow >= p95 (n=66) |
|---|---|---|
| Frame / GPU workload / GPU idle ms | 6.40 / 5.71 / 0.69 | 10.19 / 9.88 / 0.31 |
| Pipeline binds | 1,933 | 2,411 |
| Stage B (`gow3:DrawRec`) in vkWaitSemaphores ms | 0.13 | 2.58 |
| GR Active / SMs Active / SM Issue % | 85 / 64 / 30 | 94 / 77 / 43 |
| Pixel warps in flight (per cycle) | 26 | 44 |
| DRAM read / write % | 12 / 12 | 8 / 8 |
| GPC clock | 2.69 GHz | 2.65 GHz |

- The slow frames are one ~1 s stretch (01:14:45.1-46.1): ~25% more draws, ~2.7x the pixel-warp time; the gap
  between GR and SM active does not grow (21 -> 17 points), memory traffic falls. Extra pixel shading (effects /
  overdraw at 4K), probably latency-limited (high occupancy, issue 43%, low DRAM) **[I]**: per-pass attribution needs
  Nsight Graphics GPU Trace.
- Not the cause: sparse binds (17 in the stretch, 49-142 us CPU each; their GPU time is not in the trace), other
  processes on the GPU (explorer.exe, 0.05-2 ms about once a second), memory allocation (2 calls in the capture),
  clocks.
- A few frames are CPU-side: the slowest (11.6 ms) had stage B on-CPU 11.0 ms and the GPU idle ~1.15 ms; three of
  9.6-10.2 ms had 2.9-5.4 ms GPU idle. Tracing overhead may account for part of these **[I]**.
- Each frame is one large command buffer (stage B's, 4.7-10.7 ms), plus small ones from stage A and the present thread.
- **Correction, the same day: "working hard" holds only relative to ordinary frames.** In absolute terms the GPU is far
  from saturated [V]: in the slow stretch (1.00-1.45 s) SMs Active 78% while GR Active ~94% (17 points with the
  graphics engine busy and the SMs empty), SM Issue 45%, DRAM ~8%; warps in flight (Throughput %) pixel 47.1, compute
  3.6, vertex 1.0, unallocated in active SMs 27.2, so achieved occupancy on active SMs ~66% (~52% over all SMs); fast
  stretch (5-8.5 s) ~50%, SM Issue 27%. The user: GPU temperatures run far lower than other games at similar frame
  rates, and the core clocks closer to 3 GHz under real load (here 2.65 GHz, "prefer maximum performance").
  Hypothesis [I]: the slow frames end ~600 render passes (T-F1); if our Vulkan translation drains the GPU at pass
  boundaries / barriers, the SMs sit empty there. Matters most for weaker GPUs, where the slow frames would grow
  non-linearly. Next: Nsight Graphics GPU Trace per pass.

### N-2: Nsight Graphics GPU Trace: the GPU waits for idle most of each frame (2026-10-27, launched by the user; `research-data\ngfx\ngfx-fight-1`)

Setup: `run-game.ps1 -Ngfx` (run.py `GOW3_NGFX`: ngfx GPU Trace Profiler on medievil-runtime.exe, F11 traces 5
frames, Ada "Top-Level Triage", auto-export; `GOW3_SKIP_TOOL_INFO=1`, Reflex on). ngfx traps: `--platform` is
taken by Qt (ngfx does not start); `--args` is split on spaces and keeps only backslash-escaped quotes (run.py
passes `list2cmdline` of the game's arguments); the launched game inherits the environment (`ngfx-test.ps1`). GPU
Trace locks the clocks to base (~2.2 GHz) and forces V-Sync off. The first trace saved; the game ended during the
second (no Windows crash or driver-reset event; its output did not come through ngfx: `GOW3_STDOUT_FILE` added,
`-Ngfx` sets it to `<tag>\game.log`).

Per frame (5 frames, `BASE\GPUTRACE_FRAME.xls`) **[V]**:

| Metric | Range over the 5 frames |
|---|---|
| `fe__cycles_stalled_cmd_wfi_queue_sync` (front end stalled on wait-for-idle) | **44-66% of the frame** |
| `fe__output_ops_cmd_go_idle_queue_sync` (GPU drains) | **312-487** |
| `fe__draw_count` / `gr__dispatch_count` | ~2,190 / ~125 |
| `fe__output_ops_cmd_subchsw_queue_sync` (graphics <-> compute switches) | ~50 |
| `sm__throughput` (= SM instructions vs peak) | 21-34% |
| `dramc__throughput` / `lts__throughput` | 15-23% / 12-19% |
| `gr__cycles_active` | 63-95% |

For half or more of each frame the front end waits for the whole GPU to drain before issuing more work: the
"busy, not working" time, with its cause. Nsight Systems' Vulkan trace (N-1) counts ~102 `vkCmdPipelineBarrier2` a
frame, fewer than the 312-487 drains: other sources (render-pass begin/end, layout transitions, clears / copies,
graphics <-> compute switches, driver-inserted WFIs) **[I]**. Next: per-pass view of the trace in the Nsight
Graphics window (where the WFIs fall), and the barrier / pass sources in the renderer.

### N-3: where the drains come from, by code site (2026-10-27, launched by the user; `GOW3_PERF_DIAG=1`, log `data/t-b1-barrier-sites.log`)

Diagnostic widened: `Runtime::FlushBarriers` now counts every barrier by caller and masks ("(between passes)" when
no render pass was open); before, only barriers that ended an open pass. Survey of the renderer (subagent, read-only):
the shared memory barrier has a fixed destination `ALL_COMMANDS` / `MEMORY_READ|WRITE` (`vk_runtime.cpp:110-112`)
and a source that ORs every tracked access stage, reads included (`:934`); every flush ends the open pass (`:1004`);
sampled / storage textures transition with destination `eAllCommands` (`vk_rasterizer.cpp:2567`, `:2573`, `:2583`,
`:2818`), stored as the image's last stage, so the next transition starts from `ALL_COMMANDS` too.

Last 5 s window before quitting (the fight; 206.8 FPS, 1,034 frames, 1,455 draws/frame), per frame **[V]**:

| What | Per frame |
|---|---|
| Barriers (all) | **174** |
| Render-pass ends | **310**: pass switches 168 (`vk_scheduler.cpp:102`), barrier flushes 124 (`vk_runtime.cpp:1006`), buffer copies 7.5 (`:121`), dispatches 4 (`vk_rasterizer.cpp:1408`), `vk_runtime.cpp:331` 4 |
| Pass switches | other targets 142, same targets for a clear 25 |
| Passes reopened on the same targets right after an end (wasted) | 11 |

Top barrier sites per frame (all at the draw flush `vk_rasterizer.cpp:1074` unless noted):
- Depth ping-pong: `DepthAttachment -> DepthStencilReadOnly` early/late-z -> **all**: 29; back, **all** -> early/late-z:
  25 (+7 with a memory barrier from all-graphics). The depth buffer flips between written and sampled ~30 times a frame.
- Color target -> sampled, color-out -> **all**: 11 + 7 (2 / 3 images) + 2; sampled -> color target, **all** -> color-out: 8.5 (+1).
- Dispatch (`:1405`, between passes) storage image `General > General` **all -> all**: 10; sampled -> storage **all -> all**: 2 + 2.
- Feedback-loop targets (render target also sampled): ~4 + 3 + 3.
- Copies for a depth/stencil upload, transfer-source copies (`vk_runtime.cpp:342`), 1 each.

Almost every barrier has `all` on one side: each is a full GPU drain. With ~300 passes a frame this matches the GPU
trace's 312-487 drains (N-2). Plan: (1) destination = the shader stages that read (vertex/fragment, compute) instead
of `eAllCommands`, so the stored last stage is narrow too; (2) the depth ping-pong: a layout valid for both uses
(no transition) where the depth is only read; (3) the memory barrier's destination per flush; (4) uploads before
the pass (the 11 wasted reopens).

### N-4: change 1, image transitions to the reading shader stages (2026-10-27/28, launched by the user)

`GOW3_NARROW_IMAGE_STAGES` (default on; 0: upstream `eAllCommands`): sampled / storage image transitions in
`BindTextures` / `BindTexturesFromSet` wait for the reading shaders only (`ImageReadStages`: compute, or
pre-rasterization + fragment), stored as the image's last stage. `Image::GetBarriers`: a read by a shader stage not
covered by the stored shader-only stages gets its own read-after-read barrier (chains onto the first), keeping the
earlier readers in the stored stage (`ReadCovered`); stored stages with depth tests / attachments / all skip as
before (T-F2).
- `t-nb1-narrow-on.log`: no visual problems in a fight (the user). Fight windows ~170-220 FPS, p99 5.2-7.9 ms: not
  clearly different from before; the user sees a persistent oscillation on the frame-time graph that was not there
  before (open).
- `ngfx-narrow-1` (with `GOW3_PERF_DIAG=1`; game output in `research-data\ngfx\ngfx-narrow-1\game.log`): fight window
  (187.7 FPS, 939 frames) **163 barriers a frame** (174 before), now narrow: depth ping-pong `fragment <-> early/late-z`
  (~30 + ~29 a frame), color target -> sampled `color-out -> fragment`, storage `compute -> compute` **[V]**; **0
  read-after-read barriers added** (not the oscillation's cause) **[V]**; ~7% still with `all` / `all-graphics`, 14%
  with the memory barrier. The GPU trace saved (`medievil-runtime_2026_10_28_01_58_07.ngfx-gputrace`) but ngfx timed
  out ("Operation timeout") and ending its session closed the game: no auto-export (run.py now passes
  `--no-timeout`). Whether the GPU drains less: from that trace (wait-for-idle share vs N-2's 44-66%).

### N-5: same-spot A/B of change 1, and the PCIe split (2026-10-28, launched by the user; `research-data\ngfx\ngfx-ab-1`)

Build: change 1 live (toggle bit `Gow3Toggle::NarrowImageStages` = 2147483648 in `GOW3_TOGGLE_FILE`: all commands),
"Mapped writes per frame" line (stream buffer / constant ring / staging), shader profiler in `-Ngfx` traces. Two
traces at the same camera and game state, change 1 off (`BASE-2-off-025306`) then on (`BASE-3-on`), 5 frames each,
clocks locked ~2.18 GHz:

| Per frame | Off (all commands) | On (change 1) |
|---|---|---|
| GPU frame | 5.15-6.69 ms, mean 5.71 | 4.90-5.64 ms, mean 5.22 |
| Per draw | 3.30 us (1,731 draws) | 3.12 us (1,672 draws) |
| Go-idle (drains) | 208-214 | 204-211 |
| Graphics <-> compute switches | 47-49 | 47-49 |
| GR active / SM throughput / DRAM % | 72-95 / 24-31 / 23-30 | 86-98 / 28-33 / 28-32 |

Change 1: ~0.5 ms (~9%) a frame, ~5% per draw **[V]** (5 frames a side, suggestive); **the drain count does not move**:
the driver drains at the layout transitions and pass breaks whatever the stage masks **[I]**. Kept on.

PCIe, the port's own writes per frame (fight, ~1,620 draws) **[V]**: constant ring **13.8 MB** (VRAM through the BAR,
~8.5 KB a draw), stream buffer 4.0 MB, staging uploads 2.75 MB, downloads 0.26 MB: ~21 MB. GPU Trace's 60-80 MB
includes its own counter stream (4.3 GB PMA buffer in system memory) and driver command buffers **[I]**.
`CollectRingBindings` (`vk_rasterizer.cpp:490`) copies every small read-only buffer of every stage of every draw,
no reuse.

Order from here (the user, 2026-10-28):
1. Depth ping-pong: ~60 of ~210 drains a frame (depth written <-> sampled, per light **[I]**).
2. Constant-ring reuse: count repeated (address, size, unchanged) bytes first, then reuse copies while the guest
   pages are unwritten and the ring slot is live.
3. Pass breaks that reopen on the same targets (~8-11 a frame).
4. Change 3: the shared memory barrier's destination per flush (tracker redesign).

Further directions (the user): use the game's own sync (PM4 ACQUIRE_MEM / EVENT_WRITE, ignored today) to place
barriers where the game knew a dependency existed, instead of inferring them; be precise about what the CPU and GPU
really share (the PS4's memory is unified, so nothing is declared; the port tracks, re-uploads and copies
conservatively); with that cheaper, revisit culling (whether cheaper proper occlusion culling beats the HZB remap).
Survey of the MGS Peace Walker patches' call caching (DirectX) for Vulkan equivalents: pending (subagent).

MGS Peace Walker comparison (subagent, read-only; `C:\Users\Ty\MGS4-SSAA-AspectRatio-Enabler\MGS4Enabler\pw\src\features\state_cache.cpp`,
docs `MGS4Enabler\docs\pw\frame-pacing.md` 3.3a / 5.1): a D3D11 cache of state-object creation (sampler, blend,
depth-stencil, rasterizer), keyed field by field (not raw bytes: stack padding), ~350 creations a frame -> 0,
game-thread CPU -8%; the cost was waiting for the device lock held across Present, not the calls. Vulkan / port:
samplers already cached (`texture_cache.cpp:1467`), blend / depth / raster states are pipeline state (pipeline
cache) or dynamic state with value-compare setters. Open in the port: per-draw constant-ring copies (item 2 above),
descriptor pushes without comparing to the previous draw (`vk_pipeline_common.cpp:41-76`), `vkCmdBindPipeline`
every draw without a same-handle check (`vk_rasterizer.cpp:1198`, `:1333`), `updateDescriptorSets` (a device call)
on the recording thread (`vk_pipeline_common.cpp:79-84`): time them for lock waits against Submit / Present, the
method PW used.

### N-6: depth writes only with the depth test on (2026-10-28)

The depth target's "written" flag (`ImageViewInfo::is_storage` = DB_DEPTH_CONTROL `depth_write_enable`,
`image_view.cpp:86`) chose the writable layout even with the depth test off, where neither the PS4 nor Vulkan writes
depth (the pipeline already used the effective state, `vk_depth_stencil_state.h:30`). `GOW3_DEPTH_WRITE_EFFECTIVE`
(default on; 0: as before), `vk_rasterizer.cpp` depth target: writable only with `depth_enable` or a pending clear.
Expected: fewer of the ~30 + ~29 depth ping-pong transitions a frame, if those draws have the test off **[I]**.
- Result (`t-dw1-depth-effective.log`, `GOW3_PERF_DIAG=1`): no visual problems; **the ping-pong is unchanged**
  (29.7 + 29.2 a frame): those draws test and write depth for real **[V]**. Kept (correct, free). The barrier keys now
  name the image (`[WxH format]`, `Runtime::Transit`): the ping-pong image is **512x512 D16Unorm** (`t-rr1`), not the
  scene depth (3840x2160 D32S8): a shadow map reused per shadowed light **[I]**, ~38 transitions each way a frame.
  Candidate: rename its backing from a small pool when it is written again after being sampled (drops the
  read -> write half, lets light i+1's shadow overlap light i's pass); after the current items.

### N-7: constant-ring reuse (2026-10-28, launched by the user)

`GOW3_RING_REUSE` (default on; 0: a copy per binding): `CollectRingBindings` reuses a ring copy of the same guest
(address, size) within one guest command buffer (graphics queue only). The table resets (`Rasterizer::CloseRingReuse`)
at each top-level command buffer and at `WRITE_DATA`, `DMA_DATA`, `WAIT_REG_MEM`, `MEM_SEMAPHORE` waits and `REWIND`;
GPU-written ranges are skipped before the copy as before. `ConstantRing::Hold` keeps each reusable region from the
copy on; closed epochs are queued and released once the GPU completed every packet recorded before they closed.
- `t-rr1`: the first version merged closed epochs into one floor whose release point kept moving: never released, the
  ring stayed full (0 MB) and everything fell back to the stream buffer (~10 MB/frame, 142-168 FPS). Fixed (a queue).
- `t-rr2` (~1,160 draws/frame, at the 237 cap): constant ring **1.93 MB copied + 8.13 MB reused** a frame (81%
  reused), stream buffer 2.1 MB: **~4.0 MB a frame through the BAR, was ~17.8** (t-ab-1, ~1,620 draws) **[V]**; p99
  4.57 ms, stddev 0.15-0.19 ms; no visual problems (the user). The one ~8 ms frame per 5 s window was the
  `GOW3_PERF_DIAG` report.

### N-8: panning hitches, the stream-class table, and logging cost (2026-10-28, launched by the user)

- `t-rr3-clean` (no diagnostics): fights hold up (most windows 200-230 FPS, p99 ~5.5-7 ms); panning hitches 13-17 ms
  where new content enters view. The user: 1% / 0.1% lows higher in heavy fights than two days ago.
- `t-pan1` (`GOW3_SLOW_FRAMES=1.6`, `GOW3_SECTIONS=1`): gameplay hitches 13-15.5 ms: stage B **`ObtainBuffer` 10-12
  ms** (1.9 ms in ordinary frames with the same draws and uploads), stage A draining 12 ms. Not the residency pool
  (no chunk then), not the constant ring (it never fell back). The logging was heavy: ~30 unbuffered printf a slow
  frame on the flip path, 862 lines; now formatted and queued to a background writer (`gpu/shim/gow3_async_log.h`),
  the slow-frame lines and the `GOW3_PERF_DIAG` report alike.
- `t-pan2`: `ObtainBuffer` split into sub-sections (`obt_readbacks`, `obt_regions`, `obt_stream`, `obt_resident`,
  `obt_sync`): **`obt_regions` 8-12 ms** in the hitches **[V]**. The region bits are lock-free; the block's
  `stream_class` (small-buffer state by address) was an `unordered_map` cleared at 262,144 entries (ring buffers:
  new addresses every frame), the clear and each rehash on stage B **[I]**. Replaced by a fixed 65,536-entry
  direct-mapped table (`BufferCache::StreamClass`; an evicted address starts again as new).
- `t-pan3`: `ObtainBuffer` ~0.5 ms in every slow frame **[V]**; the user: a big improvement. Remaining stutters with
  33-100 MB of image uploads in one frame: `end of pipe` (`ProcessDownloadImages`) ~7 ms with a ~7 ms GPU tick wait
  on the next frame, and `begin render` 3.6-6.9 ms (uploads / detiling inside `BeginRendering`).
- A/B without diagnostics, same panning, gameplay windows (>900 draws) **[V]**:

  | | Tonight's changes off (`t-ab-tonight-off`) | On (`t-ab-tonight-on`) |
  |---|---|---|
  | FPS (median window) | 197.2 | 207.8 |
  | Worst frame (median / max) | 9.3 / 510 ms | 7.2 / 12.6 ms |
  | Windows with a frame > 12 ms | 4 of 11 | 1 of 16 |
  | p99 (median window) | 6.65 ms | 6.15 ms |

  Tonight's changes are a net gain; the large stutters in the logged runs were mostly the logging. **Judge
  smoothness only from runs without `GOW3_SLOW_FRAMES` / `GOW3_SECTIONS` / `GOW3_PERF_DIAG`.**
- HZB occlusion during panning: virtually all results 3 frames old, as two days ago (`t-rp1`); the 1-2-frame-old
  results appeared only on the level-select map.
- Level-select map oscillation (`t-pan1`, 03:51:26-46): bimodal frames (~4.7 ms and ~8.5-10 ms) at ~620 draws,
  stage B ~70% idle, stage A not waiting for submissions, no port-side wait growing. The user: the same screen is
  normally flat when moving between selections, so not the GPU load or the limiter alone: something intermittent.
  Open.
- Next (the user): image upload budgeting regardless (streaming costs much more in this port: detection, staging,
  detiling, upload, all on the pacing threads): first count what a burst is (re-uploads of images with a GPU copy:
  deferrable; first uploads: not), then a per-frame budget for deferrable ones, and uploads out of `BeginRendering`.
  The end-of-pipe readback wait after a burst.

### N-9: UE4 streaming / traversal settings for this game (2026-10-28, research subagent; to test)

Engine: **UE 4.21** (`++UE4+Release-4.21` in the binary, `plan-baseline-issues.md:41`). Every cvar below is in the
binary (UTF-16 strings in `out/boot-linked.bin`, checked 2026-10-28), so none is silently ignored. Defaults marked
(guess) are the subagent's memory of 4.21, not verified.

| Cvar | Effect | Default | Try | Cost |
|---|---|---|---|---|
| `r.Streaming.MaxEffectiveScreenSize` | caps the screen height used to pick wanted mips | 0 = actual (guess) | 1080 | softer close-ups at 4K; at 4K every texture wants ~1 mip more than on PS4 (~4x bytes) |
| `r.Streaming.HiddenPrimitiveScale` | wanted resolution for textures not recently seen | 0.5 (guess) | 1.0 | memory only; stops pan-away / pan-back stream-out and back in |
| `r.Streaming.MaxTempMemoryAllowed` | temp memory for in-flight mip updates: caps a batch | game 35 | 16-24 | slower sharpening |
| `r.Streaming.MinMipForSplitRequest` | load visible mips first, then hidden ones | 0 | 10 | none |
| `r.Streaming.NumStaticComponentsProcessedPerFrame` | components added per frame while a level becomes visible | 50 | 10-25 | levels appear a bit later |
| `s.AsyncLoadingTimeLimit` / `s.AsyncLoadingUseFullTimeLimit` / `s.PriorityAsyncLoadingExtraTime` | game-thread async loading budget | 5 / 1 / 15 (guess) | 2 / 0 / 5 | pop-in later |
| `s.LevelStreamingActorsUpdateTimeLimit`, `s.LevelStreamingComponents(Un)RegistrationGranularity`, `s.UnregisterComponentsTimeLimit` | time-slice actor / component (un)registration | 5 / 10 / 5 / 1 (guess) | 2 / 5 / 2 / 0.5 | levels finish later |
| `s.ForceGCAfterLevelStreamedOut` | GC right after a level streams out | 1 (guess) | 0 | memory freed later |
| `r.Shadow.MaxNum{Point,Spot}ShadowCacheUpdatesPerFrame` | caps shadow-cache refreshes a frame | -1 (guess) | 2-4 | briefly stale shadows |

Avoid: `r.Streaming.FullyLoadUsedTextures=1`, `r.Streaming.PoolSize=0` / `r.TextureStreaming=0` (the pool must fit
the Garlic heap), debug cvars. Already tested: S1 (`AmortizeCPUToGPUCopy`, 4 textures a frame) little effect; S2
(pool 1600) fixed small pans, not large ones. The "Epic" quality preset raises view / LOD distances (more draws and
wanted textures at 4K).

Stream-in in 4.21 allocates a new texture with more mips, copies the shared mips GPU-side (`RHICopySharedMips`),
uploads the new ones and swaps; the old texture renders until then. Port idea: such a new image whose low mips come
by GPU copy could keep the old image bound for a few frames while its upload is spread (correctness care needed).
Before any of this: `GOW3_TEXCACHE_STATS=1` names the images in the 33-100 MB bursts: C3 found a 64 + 32 MiB 4K
render-target pair per torch (UE4's render-target pool, undefined memory uploaded); if the bursts are such targets
first used other than as targets, extending C3 removes them outright.

### N-10: what the image upload bursts are (2026-10-28, launched by the user; `t-up1-refresh-sites.log`, `GOW3_TEXCACHE_STATS=1`)

Run 21:20:47-21:27:01; level started 21:23:19. **Not mainly texture streaming [V]:**
- Menu: the intro movie (`gameintro.mp4`, 21:21:11-21:23:19) re-uploads its YUV planes (1920x1088 R8, 960x544 R8G8,
  1984x1088 R8) ~30 times a second, 9.1 GB in all; suspended at the level change (T-M1) as designed.
- Gameplay (~3.5 min), the large single uploads: **4K render-target-pool images first used as a non-target**
  (3840x2160 RGBA16F x4 at 64 MB, RGBA8 x4 at 32 MB, + an RT pair): ~446 MB, one at a time at 23:23, 23:43, 24:33,
  26:03, 26:28, 26:58: the 33-100 MB frames; **128x128 RGBA16F 512-layer arrays** 86 MB each (23:23, 24:38);
  4096x4096 BC3 full chains 21 MB each (mostly at the level start).
- Real texture streaming (BC1 / BC5 1024-2048, full chains): ~150 MB, spread evenly (~5-15 MB per 5 s).
C3 skips the undefined upload only when the first use is as a render / depth target. Extending it to other first
uses is safe only if nothing real was written there (streamed textures also land in recycled memory). The refresh
list now adds ", undefined (unmap-freed)" and the first use's caller (`RefreshImage` / `UpdateImage` take a
`std::source_location`), to decide.

### N-11: render targets uploaded before being rendered into; the map oscillation's writer (2026-10-28, launched by the user)

- `t-up2`: the refresh list names the first use: the 4K uploads (3840x2160 RGBA16F at 64 MB, RGBA8 at 32 MB) come
  from `vk_rasterizer.cpp` color-target binding (`UpdateImage` before rendering into it), not flagged undefined.
- `GOW3_CLEAR_SKIPS_UPLOAD` (skip when the pass fast-clears the whole image): **0 hits** (`t-up3b`): those passes do
  not clear. Kept (harmless), no effect.
- `GOW3_UNDEFINED_GPU_FREED` (`FreeImage` remembers freed GPU-written images' addresses; a new image there is
  Undefined): 0 hits in the menu (`t-up4`): the 4K targets were in fresh memory, no image had been there.
- **`GOW3_NEW_IMAGES_UNDEFINED`** (default on; 0: off): every new image is Undefined until its first use; a first use
  as a render / depth target skips the upload (C3's path), any other first use uploads as before (textures the CPU
  loads unchanged). `t-up5`: **no 4K render-target uploads in the refresh list**; gameplay image upload peaks 6.5-8.1
  MB/s (12.5-14.1 in `t-up3b`); no visual problems (the user). (The "not uploaded first" counter now counts every
  render-target first use of an Undefined image, dirty or not: it overstates the saving.)
- **Map oscillation (`t-up4`, `t-up5`) [V]:** write-fault downloads only during the event: 14 -> 28 -> 31 -> 30/s,
  127-195 ms of GPU waits a second (0 otherwise); `GOW3_PERF_DIAG`: **`guest+0x20da4d0` writing page 0x2010450000**,
  23-27/s. Ghidra (`DisasmAround.java 20da4d0`): a loop that reads a u16 index, loads a 4x4 matrix (64 bytes) and
  stores it transposed as 3x4 (48 bytes) into a buffer returned by a virtual call (`[rax+0x468]`): UE4 GPU-skinning
  bone matrices **[I]**. The page counts as GPU-written, so each write downloads it first (~7 ms). Open: why the page
  is GPU-written (a shader writing there, a pool reused, or a writable binding that is never written).
  Also seen (no waits): `guest+0x68e109` / `0x68e149` / `0x1538f35` / `0xba9a49` on page 0x204aae0000.
- `run-game.ps1`: rolling logs (an existing `<tag>.log` moves to `<tag>.<end time>.log`; a re-run of the same command
  overwrote `t-up3-clear-skip`).

### N-12: why the map's bone page counts as GPU-written (2026-10-28, launched by the user; `t-map1-gpu-writer.log`, `GOW3_PERF_DIAG=1`)

New in the write-fault report: who last made each 64 KiB page GPU-written (`ObtainBuffer` caller, the shader for
shader bindings via `BufferCache::NoteGpuWriterShader`, the range, the count). The oscillation did not happen this run
(the user), but the faults did **[V]**: page 0x2015060000, written by `guest+0x20da4d0` (bone matrices), `0x1158f78`,
`0x1538f35`, up to ~60/s; **GPU-written by shader `0x86c25cc15153c173` (writable buffer binding,
`vk_rasterizer.cpp:2310`), range 0x2015060c00 +0x14** (20 bytes = 5 dwords: an indexed indirect-draw argument block
**[I]**), ~800-1,000 marks a second. This run the downloads were served without waiting (readback queue: 1.3 ms of
GPU waits a second; 127-195 ms in `t-up4` / `t-up5`): **the oscillation is the same faults landing while that
shader's GPU work is still in flight** **[I]**, which is why it comes and goes.
Cause: the game's allocator puts a 20-byte GPU-written block in the same page as a CPU-filled bone buffer; the port
protects per page, so every bone write faults and downloads bytes the CPU never touches. Options: (1) on a write fault,
download only when the address is near a GPU-written range, re-protecting later (care: later CPU writes into the
range); (2) keep those small ranges' twin / readback copy always ready, so a fault never waits (extends what already
served this run); (3) drop the mark when the shader's write did not happen (unlikely here). Start with (2).

### N-13: the map oscillation's waits (2026-10-28, launched by the user / by Claude at the user's request)

The write-fault paths (`BufferCache::ReadMemory`): a twin (no wait) when the faulting 4 KiB page holds <= 1 KiB of
GPU-written bytes still in flight; else a download widened to a 512 KiB window, which waits for the newest GPU write
anywhere in it. `t-up5` (oscillating): all 23-27 faults/s on the readback queue, twins 0, 127-158 ms of waits a
second; `t-map1` (flat): twins 30-52/s, 1-3 ms.
- `GOW3_FAULT_PAGE_DOWNLOAD` (default on): a page whose GPU bytes are finished downloads alone (no wait). `t-map2`:
  the oscillation came back with **0** such downloads: not the case.
- Then: a page with no GPU-written bytes needs no download (CPU-modified, its GPU mark cleared); the twin limit
  `GOW3_GPU_WRITE_TWINS_MAX` 1 KiB -> **16 KiB**; the cases left to the window counted (unknown tick, too big for a
  twin). `t-map3-fault-cases`: no oscillation; the map's ~35 faults/s **twinned** (33.8/s), the rest finished-page
  downloads; **0** faults in any of the other cases; 0.1-0.6 ms of waits a second **[V]**. So the oscillating runs'
  page most likely held more than 1 KiB of GPU bytes still in flight **[I]** (intermittent: one flat run does not
  prove it; the counters name the case if it returns).
