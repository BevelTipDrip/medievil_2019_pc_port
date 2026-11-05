# Implementation: less wasted CPU on the Steam Deck (stage A's drain wait, the game thread's nav purge)

Status: built 2026-11-05 on `deck-lab` (Windows and the Linux default AppImage). **Not run in game**: nothing here is
verified (verified = the user confirmed it in game). Commits: `6f4be46` (hybrid drain wait, its frame-stats line, a
unit test), `b98df73` (navigation purge patch), and this page.

Why: the Deck's APU sits at its 15 W cap (CPU ~7.4-7.9 W, GPU ~3.2-3.9 W, `gpu_metrics`; plan-linux-deck.md "CPU clocks
and the power cap"). CPU power spent anywhere costs GPU clock, so busy-waiting is not free even on idle cores. Inputs:
the heavy-view per-thread CPU (`deck.sh threads 5`) and a 10 s user-space `perf record`
(`research-data/perf/deck-v3-perf-report.txt`; kernel time not visible, `perf_event_paranoid=2`).

| Item | Change | Where | Switch | Default |
|---|---|---|---|---|
| 1 | Stage A's drain wait: a bounded pause spin, then a futex sleep that stage B ends near the last packet | `vk_draw_pipe.h`, `gow3_drain_wait.h/.cpp` | `GOW3_DRAIN_HYBRID=1` at start; **second toggle word 4096** switches it off live | **off** |
| 1a | "Stage A drain waits per frame" line (both paths) | `driver.cpp`, `deck_summary.py` | `GOW3_FRAME_STATS=1` | - |
| 2 | The game thread's per-frame navigation path purge without its two `mfence`s per entry | `patches/MediEvil.xml` (01.00 and 01.02), `graphics_options.py`, `run.py` | `GOW3_NAV_PURGE_NOFENCE=1` / `0` (or the launcher's Patches page); restart-only | **off** |

Knobs of item 1: `GOW3_DRAIN_SPIN_US` (pause spin before a sleep, default 30, 0-2000), `GOW3_DRAIN_WAKE_AHEAD`
(packets before the end at which stage B wakes a sleeping stage A, default 2, 0-15). With every switch off the build
behaves as before (the wait is the same code; Commit() stores one more word, Consumed() reads one more word).

## 1. Stage A's drain wait

### What it does today [read in the code]

`DrawPipe::Drain()` (stage A, `shadPS4:GpuComm`) waits until stage B (`gow3:DrawRec`) has run every committed packet:
4096 `pause`s (~90 us on Zen 2), then `std::this_thread::yield()` (a `sched_yield` system call) in a loop, for as long
as it takes. Deck logs (`data/deck-epp-perf-1.log`, heavy view): ~300-320 drains a second that waited, stage A waited
57-62% of the time, almost all "compute" drains (a dispatch runs on stage A after stage B ran dry): **~1.9 ms per
drain**. Lighter views: ~1,000 drains a second of ~0.5 ms. So most of the waiting is `sched_yield` calls: the
4096-pause part covers ~90 us of a ~2 ms wait. That fits the per-thread numbers: stage A ~109% CPU with only ~0.78 of
it in user space (the rest kernel: the yields) and 8.4k voluntary switches a second in the wait-sites run.

### Why GOW3_DRAIN_SLEEP lost (60.7 vs 64.0 FPS) [inferred from the code]

The opt-in `GOW3_DRAIN_SLEEP` (spacepepperoni `6ef7566`) slept on `consumed` itself: stage B's `Consumed()` called
`consumed.notify_one()` after **every packet** while stage A slept, and stage A's `consumed.wait(seen)` returned at
every packet (the value changed), checked, and slept again. A ~2 ms drain covers ~100+ packets: a futex wake system
call per packet on stage B, the bottleneck thread, and a context switch per packet for stage A. The design was right
(sleep); the wake rule was the cost.

### The hybrid wait (`GOW3_DRAIN_HYBRID=1`)

1. **Spin:** pauses only (no system call), the clock read every 64 pauses, for up to `GOW3_DRAIN_SPIN_US` (30 us).
   Short drains end here.
2. **Sleep:** stage A registers a wake position in `drain_target` and sleeps on a 32-bit futex word (`drain_wake`;
   Windows: `WaitOnAddress`). The position is the commit end of the packet `GOW3_DRAIN_WAKE_AHEAD` (2) before the last
   one (stage A keeps the head after each of the last 16 commits), so stage A is running again when stage B starts the
   last packets.
3. **Spin again:** after the wake, up to the spin time; if the end is still not reached, sleep again with the exact end
   as the position, then spin. Spurious wakes just loop.

Stage B's side, `Consumed(at)`: after storing `consumed` it reads `drain_target` (the same cache line it writes
anyway; `~0` when nobody sleeps). Only when `target <= at` does it take the target with a compare-exchange (one wake per
registration) and call `FutexWake`. Ordering: stage A stores the target and then loads `consumed`, stage B stores
`consumed` and then loads the target, all `seq_cst`: one of them sees the other, so a wake is not lost; stage A reads
the futex word before it registers, so a wake between the check and the sleep makes the futex call return at once.

Expected cost on stage B: one futex wake per sleeping drain (1-2 per drain with the early wake), ~3-5 us each on the
Deck (the recorder wakes from stage B measure 2.9-4.1 us, "Stage B syscalls" line): with ~5 drains a frame, ~20-40 us
of stage B a frame at worst (0.2-0.3%). Expected gain: stage A's waiting time stops using the core: up to ~0.3-0.6 of
a CPU in the heavy view (the yields and the long spin), i.e. less CPU power for the same work; on the 4-core Deck also
a free SMT sibling for stage B / the game more often. **Risk:** wake latency. Each drain ends with stage B idle until
stage A has woken and committed again; if stage A wakes late, stage B waits. The early wake is there to hide that; the
frame-stats line measures it (below). If the latency average exceeds ~2 packets of stage B time (~20-30 us), raise
`GOW3_DRAIN_WAKE_AHEAD` (3-4) rather than lengthen the spin.

### The frame-stats line

```
Stage A drain waits per frame (hybrid on, spin 30 us, wake 2 ahead): N (T ms); today's wait: within the pauses N,
sched_yield calls N; hybrid: N, ended in the spin N, slept N (sleeps N, woken ahead N, then ended by the spin N),
spin T ms, sleep T ms, wake latency U us avg U max, stage B wakes N (U us each)
```

- "N (T ms)": drains that waited, either path, and their wall time (should equal "stage A waited" of the Draw pipe line).
- Today's path: drains that ended within the 4096 pauses; the `sched_yield` calls of the rest (with
  `GOW3_DRAIN_SLEEP`, its sleep rounds).
- Hybrid: drains on that path; ended by the first spin; slept at least once (futex sleeps, of them woken at the early
  position, and of those the ones the following spin finished); spin and sleep time; wake latency = stage B's wake
  call to stage A running (average, max in the window); stage B's wake calls and their cost.
- `deck_summary.py` keeps the line per toggle state ("Stage A drain waits per frame").

### Unit test

`draw-pipe-drain-test` (gpu/CMakeLists.txt, `tests/test_draw_pipe_drain.cpp`): a real `DrawPipe` with a handler that
busy-waits 0.5-80 us a packet; 6,000 rounds of 1-40 packets and a drain; the second word's 4096 flipped every 500
rounds (both paths live); stage A sleeps 200 us now and then so stage B goes to sleep too. Each drain must return with
every packet run; a watchdog fails the test after 5 s without progress (a lost wake). Results (with
`GOW3_FRAME_STATS=1`):

| Platform | Default knobs (spin 5 us in the test, ahead 2) | Spin 0, ahead 0 |
|---|---|---|
| Windows (clang64) | OK; hybrid 2,997 drains: 2,935 slept, 3,352 sleeps, 2,767 woken ahead, 2,349 then ended by the spin; 3,352 stage B wakes. Today's path: 990,778 `yield` calls for the 2,202 drains that outlasted the pauses | OK; 2,998 sleeps, 2,999 wakes |
| Linux (WSL) | OK; 3,583 sleeps / 3,583 wakes, 2,759 woken ahead, 2,086 then ended by the spin. Today's path: 999,848 `sched_yield` calls for the 2,570 drains that outlasted the pauses | OK; 2,997 sleeps / 2,997 wakes |

The yield counts show the cost being removed: ~390-450 `sched_yield` calls per drain of this test's length.

## 2. The game thread's "tight loop" (task 2)

### Address mapping [read in the code and logs]

Guest code runs at **0x800000000 + eboot vaddr**: `src/probe.c:938` allocates the image through `runtime_low_map`,
which starts at `LOW_MIN 0x0800000000` (`src/runtime_memory.c:1414`), and the image is the first low mapping. The 01.02
eboot's text segment is vaddr 0, size 0x4d098c0: exactly the perf map's `800000000-804d0a000 r-xp`. The Deck log's
"Linked modules: libc.prx@0x5c70000, libSceFios2.prx@0x5da0000, libSceNpToolkit2.prx@0x5f30000"
(`data/deck-v3-1.log`) matches the maps at 0x805c70000 / 0x805da0000 / 0x805f30000. So perf address - 0x800000000 =
the `guest+0x...` offsets of the studies (and patch XML addresses are that + 0x400000).

### What 0x802c94ba2 / ba5 / ba8 is: not a wait [static analysis, 01.02, high confidence]

The function at 0x2c94b30 is **UE 4.21's `ANavigationData::TickActor`** with `PurgeUnusedPaths()` inlined: no direct
callers, referenced only from the vtables of `ANavigationData`, `AAbstractNavData`, `ANavigationGraph` and
`ARecastNavMesh` (identified by their `GetPrivateStaticClassBody` names); it first calls `AActor::TickActor`
(0x1c92d30); the fields match 4.21 (`ActivePaths` TArray at +0x3e8, `ObservedPaths` +0x3f8, `RepathRequests` +0x408,
`NextObservedPathsTickInSeconds` +0x418; `RegisterActivePath` at 0x2c958c0 appends to +0x3e8). The loop:

```
2c94b90: cmp  qword [rbx-8], 0      ; TWeakPtr.Object
2c94b95: je   2c94bc0               ; -> remove
2c94b97: mov  rax, [rbx]            ; ReferenceController
2c94b9a: test rax, rax
2c94b9d: je   2c94bc0
2c94b9f: mfence
2c94ba2: mov  eax, [rax+8]          ; SharedReferenceCount   <- 0.24% of all samples
2c94ba5: mfence                     ;                        <- 0.63%
2c94ba8: test eax, eax              ;                        <- 0.19%
2c94baa: jle  2c94bc0               ; count 0: remove
2c94bac: dec r14d; add rbx, -0x10; cmp r14d, 1; jg 2c94b90    ; next entry, from the end
2c94bc0: ... call 0x2d0bfd0         ; ActivePaths.RemoveAtSwap(i, 1, false)
```

It walks the nav-data actor's `ActivePaths` (16-byte `TWeakPtr<FNavigationPath, ThreadSafe>` entries) once a frame and
ends at index 0: **plain work, nothing polled**: no GPU label, no event, no yield or usleep import (which is why only
guest addresses show). The cost is the PS4 build's `FPlatformAtomics::AtomicRead`: an `mfence` before and after the
count load. One scattered cache miss per entry (each path has its own heap reference controller), and the fences stop
the core from overlapping those misses. ~1.06% of all samples, ~6% of the game thread. Whether `ActivePaths` grows
large (paths kept alive by Blueprint-held `UNavigationPath` objects until GC) is inferred, not checked.

01.00: byte-identical apart from displacements at 0x2c91d70 (loop 0x2c91dd0, fence at 0x2c91ddf). The 9-byte pattern
`0faef08b40080faef0` + `85c07e14` occurs twice in each eboot: this loop and an unreferenced out-of-line copy of the
purge (01.02 0x2c956df).

Other game-thread samples (lower confidence, for orientation): 0xad5230 / 0xad324e / 0xad367d / 0xad3a64 the Blueprint
VM (`execLocalVariable`, `CallFunction` / `ProcessScriptFunction`, the local-function step loop), 0x2775062
`TGraphTask<FTickFunctionTask>::ExecuteTask` (a miss on the tick function), 0x6dc200 the TaskGraph lock-free link
allocator. TaskGraph threads: 0x6a2a33 / 0x67556e / 0x685fdc the Gnm RHI command context (pre-draw commit, per-slot
resource binds), 0x312ba3c PhysX 3.4's scene-query AABB tree build. The game thread is tick-bound (Blueprint script,
tick dispatch, task allocation, this purge), not waiting.

### The patch (item 2; `GOW3_NAV_PURGE_NOFENCE=1`)

`mfence; mov eax, [rax+8]; mfence` (9 bytes) -> `mov eax, [rax+8]; nop word [rax+rax]` (`8b4008660f1f440000`), at
01.02 0x2c94b9f (XML 0x03094b9f) and 01.00 0x2c91ddf (XML 0x03091ddf), with the `Original` bytes checked by
`patches.py` (a different build skips the patch). No branch lands inside the 9 bytes.

Why it is safe: an aligned 32-bit load is atomic on x86; under x86's ordering the fences only add store-to-load
ordering for this thread, and the loop stores nothing whose order matters to the result; the count never rises from 0
(`Pin()` refuses to increment from 0), so a 0 read is final; the entry's weak reference keeps the controller allocated;
a path that dies right after the read is dropped next frame, exactly as with the fences. Only that one site: the image
has 138 `mfence; mov [r+8]; mfence` sites and ~6,300 `mfence`s, some of which may need sequential consistency.

Expected effect [inferred]: most of the loop's ~1% of all samples (the misses overlap); a small cut of the game
thread's CPU, not a stage B gain (stage B is the frame's bottleneck). Measure with `deck.sh threads` (the game thread's
CPU%) and, when allowed, a second `perf record` at the same spot: the three samples at 0x802c94ba2-ba8 should shrink.
A larger fix (purge every N frames through a host helper) would need a code cave and is not as clearly safe.

## 3. Every spin, yield and busy loop on the port's threads (task 1 inventory)

| # | Site | Thread | Kind | Hot in the heavy view? | Action |
|---|---|---|---|---|---|
| 1 | `vk_draw_pipe.h` `Drain` | stage A (`shadPS4:GpuComm`) | 4096 pauses, then `sched_yield` until done | **yes**: ~300-1000 drains/s, 0.5-2 ms each | **hybrid (item 1)** |
| 2 | `vk_draw_pipe.h` `WaitForSpace` | stage A | pauses while the 16 MB ring is full | no (never full in logs) | none |
| 3 | `vk_draw_pipe.h` `Run` idle | stage B | pauses 50 us (Deck), then futex sleep (Commit wakes only a sleeper) | yes, already a hybrid | none (`GOW3_STAGE_B_SPIN` A/B'd: neutral) |
| 4 | `vk_scheduler.cpp` `WaitDeferredSignals` | stage B (WRITE_DATA, EOP in order), stage A (non-pipelined opcodes, submission end) | runs queued copies, then `sched_yield` until the recorder / copy threads wrote the deferred fences | yes on stage B: `wdata_wait` ~0.85 ms a frame (2 waits) | **not converted**: stage B is the bottleneck; a sleep adds a wake latency per wait to the frame. Candidate once item 1's wake latency is measured on the Deck (below) |
| 5 | `vk_scheduler.cpp` `WaitHostCopies` | stage B / stage A | `sched_yield` until the recorder ran the queued host copies | some ("blocked: host copies" in frame stats) | same as 4 |
| 6 | `vk_scheduler.cpp` `RecorderThread` | `gow3:VkRecorder` | pause spin `RecorderSpin`, then a condition variable | lean mode on the Deck: no spin (0 spins in logs) | none |
| 7 | `vk_scheduler.h` `ChunkGuard` | recording threads | pause spin lock around a chunk hand-off | short sections | none |
| 8 | `vk_bind_helper.h` `Join` / `Run` | stage B / `gow3:TexBind` | pause spins, 100 us then futex | no (0 parallel binds in logs) | none |
| 9 | `gow3_copy.cpp` `Pool::WaitAll`, `Run` | caller of copy waits (stage B, recorder) | helps with tasks, then `sched_yield` | low ("copy threads 0.6%") | none |
| 10 | `liverpool.cpp` `GpuOrderedWait::Step` | stage A | `sched_yield` after 256 steps of an unmet WAIT_REG_MEM | only with `GOW3_GPU_FENCES` (off) | none |
| 11 | `equeue.cpp` `WaitForSmallTimer` | guest thread in `sceKernelWaitEqueue` with a high-resolution timer under the spin threshold | `sched_yield` loop for the whole timer | not seen (the game's equeue waits are the flip/vblank and the frame EOP) | none; would matter only for a game using HR timers |
| 12 | `common/spin_lock.cpp` | various | pause spin lock | short sections | none |
| 13 | `vk_presenter.cpp` `wait_until` | present thread | Windows: timer, then pauses (`GOW3_PACER_SPIN_US` 700); **Linux: pauses only, no sleep**, up to the frame slot | only with `GOW3_LIMIT_IN_PRESENT` (off) | none now; fix before that limiter is used on Linux (sleep to slot - spin, as `PreciseWait` does) |
| 14 | `driver.cpp` `PreciseWait` | vblank / pacer threads | sleep to slot - spin, then pauses (`GOW3_TICK_SPIN_US` / `GOW3_PACER_SPIN_US`, 700) | not on the Deck (no precise tick: "vblank 900 Hz", no "precise vblank tick" line) | keep the precise tick off on the Deck: at 900 Hz a 700 us spin would be ~63% of a core |
| 15 | `runtime_memory.c` `lock_contended_path` (D5) | any | up to 1000 pauses, then block | contended rarely | none |
| 16 | `runtime_memory.c` `clamp_optimistic` / `region_optimistic` | any | pauses while a table edit is in progress (~100 us max) | rare | none |
| 17 | `runtime.c` guard, `runtime_kernel.c` `thread_once` | guest threads | `sched_yield` while another thread initialises | start-up only | none |
| 18 | `runtime_kernel.c` `yield`, `runtime_thread.c` `thread_yield` | guest threads | the guest's own `sched_yield` / `scePthreadYield`, passed through | not in the wait-site top lists | none |
| 19 | `compat_win.c` `SwitchToThread` | guest (Windows) | 0 ns sleeps | Windows only | none |

## 4. Other clear waste in the profile (recommendations, not implemented)

1. **The Vulkan recorder's wakes (3,736 voluntary switches/s, 51% CPU).** Heavy view (`deck-epp-perf-1.log`): 238
   hand-offs, 22 sleeps and 28 wakes a frame in lean mode, 83 label kicks left asleep. The sleeps/wakes explain
   ~2k switches/s; the rest are likely RADV's submit ioctls and `pthread_rwlock` blocks (its profile shows
   `runtime_memory_region`, `pthread_rwlock_tryrdlock` / `unlock`). Each wake costs stage B ~4 us ("Stage B syscalls":
   27.5 wakes x 4.07 us = ~0.11 ms of stage B a frame). Recommendation: try a longer `GOW3_RECORDER_WAKE_US` and
   handing over at a larger batch size (fewer, bigger hand-offs), measured by the "Recorder per frame" and "Stage B
   syscalls" lines; the recording work itself (51%) is real.
2. **The region-table read lock on every label write.** `runtime_memory_store_label`, `write_backing`, `read_backing`,
   `gpu_protect`, `is_mapped` and `region` (when its lock-free path is off) take the table's `pthread_rwlock` (an
   atomic read-modify-write on one shared line) on every call; `runtime_memory_clamp` has a lock-free path, they do
   not. The frame writes ~30k end-of-pipe labels a second ("Cache actions: EOP 40:103087" per 5 s) from stage B, the
   recorder and the copy threads. Recommendation: the clamp's `vma_seq` optimistic lookup for `store_label` /
   `write_backing` too (the lock samples in stage A's and the recorder's profiles: `pthread_rwlock_unlock` 0.42% /
   0.23%, `read_lock` 0.17%).
3. **The four `gow3:DrawPrep` workers (16% each, ~900 involuntary switches/s each).** They run at lowered priority
   and are preempted by everything; each re-applies register deltas from the queue's tail state before preparing a
   buffer, and the scanner (`gow3:DrawScan`) applies the same packets too. On 8 hardware threads that duplicated decode
   is CPU power. Recommendation: the pending `GOW3_PREP_WORKERS=3` (and 2) launches of the patch audit, judged by
   "Draw preparation: N prepared pipelines used, M not" staying near today's ratio (~98%) and stage B not slower.
4. **The vblank tick at 900 Hz on the Deck** ("VideoOut: vblank 900 Hz"): 900 timer wakes a second on the vblank
   thread, each also signalling the guest's vblank events (the game's flip/vblank `equeue_wait` at guest+0x69eb0c,
   ~320 calls/s). The Deck runs uncapped in Mailbox, where the tick only paces vblank events. Recommendation: one
   launch at a lower tick (e.g. 180 Hz, 2 x the panel's 90) with `deck.sh threads` on the vblank / present threads;
   check flip pacing ("Frame pacing" line) does not change.
5. **"medievil-runtim" at 115%.** One thread cannot use more than 100% of a CPU: "medievil-runtim" is the process
   name, carried by every thread that never got a name of its own, so the figure likely adds the game thread and
   another unnamed thread (or a sampling edge). Check the `deck.sh threads` rows by thread id; if several share the
   name, naming them in the runtime would make the per-thread table (and this kind of profile) readable.
6. **Wwise (`AK::EventManager`, 35%)** is ATRAC9 decoding in software (`RunImdct`, `ajm_batch_start`,
   `ReadHuffmanValue`, `UnpackFrame`): real work the PS4 did in hardware. Possible later: decode-ahead in larger
   batches or a SIMD IMDCT; not waste in the sense of this page.
7. **libc at 0x170bc5 / 0x170df3** (0.80% / 0.21% in stage A, also in the recorder): two offsets 0x22e apart in one
   function: most likely `memmove` (packet and constant copies). Symbolize with the Deck's libc to confirm before acting.

## The Deck test (one launch, the user's go)

Same spot and settings as the baseline runs (720p, 67%, the logging ini, graveyard heavy view, stand still).

1. `deck.sh deploy` (default AppImage `dist/MediEvil-x86_64.AppImage`, built from `b98df73`).
2. `deck.sh env "cpu waste A/B" GOW3_DRAIN_HYBRID=1 GOW3_FRAME_STATS=1` plus the keys the current runs use (keep
   `GOW3_ARENA_HOST_VISIBLE=0` and `GOW3_TOGGLE_FILE=...`; `deck.sh env` replaces the file). Leave
   `GOW3_NAV_PURGE_NOFENCE` out of this launch (restart-only: its own launch, below).
3. `deck.sh toggle "0 0"` (hybrid on). If `GOW3_STAGE_B_SPIN=1` stays in the env, it is unaffected by 4096.
4. The user starts the game and stands in the heavy view. `deck.sh ab "0 0" "0 4096" 20 4`: B (today's wait) and A
   (hybrid) alternate every 20 s, ending on A. During one A and one B window: `deck.sh threads 5` (stage A's CPU% and
   voluntary switches) and the APU power read (`~/MediEvil/apu_power.py`: CPU vs GPU W, GPU MHz).
5. `deck.sh pull cpu-waste-ab`, then `deck.sh summary cpu-waste-ab --min-draws 1000`: FPS, stage B busy, stage A
   waited, and the "Stage A drain waits per frame" line per state.

Pass: FPS and stage B busy not worse in "0 0"; stage A's CPU% clearly lower (expected ~1.1 -> ~0.6-0.8); CPU W lower
and GPU MHz / W higher at the cap; "sched_yield calls" ~0 in A; wake latency average below ~20-30 us. If FPS drops:
read the wake latency; if it is high, retry with `GOW3_DRAIN_WAKE_AHEAD=4` (or a longer `GOW3_DRAIN_SPIN_US`, 50).
If the hybrid holds FPS and frees power, item 4 of the inventory (stage B's WRITE_DATA waits) is the next candidate for
the same treatment.

Second launch (restart-only): the same settings plus `GOW3_NAV_PURGE_NOFENCE=1` against a launch without it; the log's
"Patches: CUSA11227; N writes from [...]" line lists "Navigation path purge without fences (CPU)" when applied (and
"the eboot has other bytes at its addresses: not applied" if the build differs). Compare the game thread's CPU% in
`deck.sh threads 5` at the same spot (and FPS, expected unchanged: stage B bound).

## Builds and tests

- Windows (`research/tools/build-clang64.sh`): builds, no warnings in the changed files. `draw-pipe-drain-test`: OK.
- Linux (WSL `/root/src/medievil`, `GOW3_CPU_LEVEL= bash build.sh`, then `packaging/linux/appimage.sh`): builds (LTO,
  static, baseline CPU level), `dist/MediEvil-x86_64.AppImage` written; `dist/MediEvil-x86_64-v3.AppImage` left alone.
  `draw-pipe-drain-test`: OK.
- Python (`py -3.14 -m unittest discover -s tests`): 131 tests, the 8 known failures (`test_patches` x6,
  `test_packaged_vulkan` x2), new `test_nav_purge_without_fences` and `test_nav_purge_environment` pass.
