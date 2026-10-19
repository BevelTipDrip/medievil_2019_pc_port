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
