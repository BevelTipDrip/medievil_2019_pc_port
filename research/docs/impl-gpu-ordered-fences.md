# Implementation: GPU-ordered fences under the copy model (U5-2, `GOW3_GPU_FENCES=1`)

Status: implemented on `deck-lab`, 2026-11-03. Built on Windows (CLANG64) and Linux (WSL Debian, AppImage); unit
tests unchanged. **Not launched**: nothing here is measured or verified in game. Parent: [plan-linux-deck.md](plan-linux-deck.md)
("Gate 2 A/B attempt": the naive switch `f97479f`, reverted in `ba95907`). Background:
[study-fence-waits.md](study-fence-waits.md) (every guest wait, the cost model, runs 8-9),
[study-bloodborne-07-memory.md](study-bloodborne-07-memory.md) sections 1.6, 1.7, 3 (#1-#4), 4 item 6, 5 step 3.

Bloodborne reference: `bloodborne_pc` `origin/master` = `0fc2921` (0.7), read with `git show`. Files below without a repo
are 0.7's: `gpu/render/raster_signals.cpp`, `gpu/cp/command_processor.cpp`, `gpu/cp/cp_graphics.cpp`,
`gpu/cp/cp_compute.cpp`, `gpu/cp/cp_tasks.cpp`, `gpu/platform/gnm/gnm_driver.cpp`.

## What it is for

The A/B the gate-2 decision rule asks for (study-fence-waits.md section 6, run 9): the game's
`SubmitGPUCommandsAsyncTaskThread` waits once a frame on an EOP interrupt that today fires ~14 ms before the GPU has
run the frame (Deck). With the switch on, every guest-visible fence follows the GPU, as under U5, while the memory
model stays the copy model. The run measures what that costs MediEvil, with and without 0.7's frame overlap.

## Switches

All read once at start. Off by default; with `GOW3_GPU_FENCES` unset every new path is skipped (the only code that
runs either way: one counter per Vulkan submission and the report lines, both behind `GOW3_FRAME_STATS=1`, and the
runtime's pool-commit byte counter).

| Variable | Default | What |
|---|---|---|
| `GOW3_GPU_FENCES` | off | `1`: the model below |
| `GOW3_SUBMIT_AHEAD` | `2` | submissions of a frame that do not wait for the previous frame; `0` = no overlap (the console's semantics) |
| `GOW3_SUBMIT_LOCK` | decode | `gpu` (diagnostic): the console lock instead of the pacing: submits after `sceGnmSubmitDone` wait for the (now GPU-ordered) GPU idle interrupt. 0.7: `BB_SUBMIT_LOCK=gpu`, "101 vs 154 FPS" |
| `GOW3_SIGNAL_IDLE_US` | `200` | the GPU command thread submits when it has idled this long with urgent signals unsubmitted (0.7 `BB_HONEST_IDLE_US`) |
| `GOW3_SIGNAL_FLUSH_US` | `2000` | at a guest submission's end, submit for its signals at most this often while the GPU has work (0.7 `BB_HONEST_FLUSH_US`) |
| `GOW3_SIGNAL_IDLE_BACKLOG` | `65536` | ... but at once when the GPU is idle, unless stage B has this many bytes of packets queued (0.7 `BB_HONEST_IDLE_BACKLOG`) |
| `GOW3_SIGNAL_IDLE_FLUSH`, `GOW3_SIGNAL_DRAIN_FLUSH` | on | `0`: no submission at once when the GPU is idle / on its last submission (0.7 `BB_HONEST_IDLE_FLUSH`, `BB_HONEST_DRAIN_FLUSH`) |
| `GOW3_WAIT_REG_MEM_GIVEUP_MS` | `0` (never) | pass a WAIT_REG_MEM not met after this long (see "Differences", 4) |
| `GOW3_LEAK_WATCH` | `log` | `stop`: turn the overlap off for the session once guest memory grew `GOW3_LEAK_LIMIT_MB` over its low (0.7's behaviour); `0`: no watch |
| `GOW3_LEAK_LIMIT_MB` | `1024` | the growth that `log` reports once and `stop` acts on |

Do not combine with `GOW3_WRITEDATA_DEFERRED=1` or `GOW3_FLIP_DEFERRED=1` (W3 / W3b, copy-ordered deferrals: they could
overtake the GPU-ordered labels). `GOW3_ASYNC_FENCES` and the `RecorderFences` / `AsyncFences` toggle bits do not
apply to the fences the switch handles.

## What was ported, from where

| Piece | Ours | 0.7 | Notes |
|---|---|---|---|
| Signal queue: `SignalAfterGpu(signal, label, value, urgent)` | `renderer_vulkan/vk_gpu_signals.cpp` | `raster_signals.cpp:52-78` | the tick is `CurrentTick()` read by the recording thread (stage B, or stage A after draining B) after the work it follows was recorded, so that work is in that command buffer or an earlier one |
| Signal thread `gow3:GpuSignals` (`GpuSignalLoop`) | same file | `:407-479` | bounded 2 ms `vkWaitSemaphores`; a tick not yet in a submission (`tick >= CurrentTick()`) gets a flush request every 50 ms (`Liverpool::SendCommand` -> `Rasterizer::Flush` on stage A); signals run in tick order and stay queued while they run; device lost reported |
| Watchdog | `GpuWatchdog`, same file | `:481-507` | logs once per tick after 10 s: GPU stuck / submission waiting / not submitted, with the queue, the draw pipe and the pacing state. Also in the pacing waits (10 s) and WAIT_REG_MEM (500 ms and 10 s). Never kills |
| `HasUnsubmittedSignals`, `PendingSignalValue` | `vk_rasterizer.h`, `vk_gpu_signals.cpp` | `:96-104`, `:396-405` | |
| `SubmitForSignalsNow` (batching) | same file, called at the end of every top-level graphics submission via `RunInOrder` (stage B) | `:346-394`, `cp_graphics.cpp:170-180` | GPU idle -> now; GPU on its last submission -> now; else at most every 2 ms (the last submission's time from `Scheduler::SubmitTime`) |
| Decoder idle flush | `Liverpool::Process`, top of the loop | `command_processor.cpp:241-261` | 0.2 ms without work and urgent signals unsubmitted -> `Flush()` |
| EOP (with and without IRQ), EOS, compute RELEASE_MEM | `liverpool.cpp` `RunEventWriteEop`, `RunEventWriteEos`, `ProcessCompute` ReleaseMem | `cp_tasks.cpp:127-154`, `:162-195`; `cp_compute.cpp` `ReleaseMemory` | label then interrupt on the signal thread; GDS stores: EOS keeps its `Finish()` + read and writes the value as a signal (in order); RELEASE_MEM records the GPU copy in stream order and only the interrupt waits. No host-copy waits: the submission holding the tick waits for the copies before `vkQueueSubmit` (`SubmitExecution` -> `WaitHostCopies`) |
| `StoreLabel` (one atomic store) | `MemoryManager::StoreLabel` (`shim/gow3gpu.cpp`), `runtime_memory_store_label` (`src/runtime_memory.c`) | `cp_tasks.cpp` `StoreLabel` | EOP / EOS through the backing view as today (`TryWriteBacking` used `memcpy`); RELEASE_MEM at its address as today. A label whose memory is no longer mapped is dropped and counted |
| GPU idle with generation, `work_retired` | `Liverpool::Process` batch end, `SignalGpuIdle` | `command_processor.cpp:521-573`, `:611-623` | after the drain, a signal `SignalGpuIdle(generation)`: GPU idle IRQ and `work_retired` only if nothing was submitted since (submission numbers, `submit_counter`) |
| Frames retired | `Liverpool::SignalDecodedFrames`, `SubmitDone(frame)` | `:575-609` | a frame retires once all its submissions are decoded on every queue (oldest undecoded submission number > the frame's last), stage B drained, then a signal + `Flush()` |
| Pacing `decode` + `GOW3_SUBMIT_AHEAD` | `shim/gow3_gpu_fences.cpp`, `gnmdriver.cpp` (`sceGnmSubmitDone`, `SubmitCommandBuffers[ForWorkload]`, `SubmitAndFlip...`, `DingDong`) | `gnm_driver.cpp:84-196`, `:538-567` | `sceGnmSubmitDone`: wait until the GPU finished every frame before, then the frame number. After each submission is handed over: the frame's first K return, later ones wait for the previous frame |
| WAIT_REG_MEM | `liverpool.cpp` graphics and compute, `GpuOrderedWait` | `cp_graphics.cpp:739-827`, `cp_compute.cpp:370-385` | pending value (draw pipe or signal queue, incl. the high half of a 64-bit label) counts as met; else drain, flush unsubmitted signals, yield |
| Leak watch | `WatchOverlapLeak` in `gow3_gpu_fences.cpp`, `runtime_memory_usage` | `gnm_driver.cpp` `WatchOverlapLeak` | see "Differences", 6 |

## Differences from 0.7, and why

1. **No per-submission hold on stage B.** 0.7 keeps stage B's signals in `local_signals` until the submission's end
   (`PublishSignals`). Here they go straight into the shared queue (a mutex, a notify only when it was empty): one less
   state to forget, and nothing publishes late. Stage B "reached" a fence therefore means "queued", which is what
   `PendingFenceValue` relies on (0.7 needs `SignalsPublished` for the same).
2. **Urgent and non-urgent signals.** Only urgent ones (labels, interrupts, frame retire) count for
   `HasUnsubmittedSignals`. The batch-end GPU idle signal is non-urgent under the `decode` pacing: nothing waits for it
   (the lock is never armed), and as urgent it would force a submission after every guest submission the command
   thread finishes (the decoder idle flush). Under `GOW3_SUBMIT_LOCK=gpu` it is urgent. It still runs after the GPU,
   at the next submission (or the signal thread's 50 ms request).
3. **The port's own ordering waits stay off the GPU (as 0.7).** `WaitDeferredSignals` (drains, `RunWriteData`,
   `SignalFlip`) still waits only for copy-ordered signals, none of which the switch creates. Making them wait for the
   GPU-ordered queue would stall stage B ~the GPU lag twice a frame (WRITE_DATA) and at every flip; 0.7's CPU
   WRITE_DATA (`cp_tasks.cpp` `WriteDataTask`, fallback path) and its flip (`FlipTask`) do not wait either. The cost:
   a WRITE_DATA flag or the flip interrupt can reach the guest before earlier labels. Counted as "CPU writes over
   pending labels" (see Risks).
4. **WAIT_REG_MEM give-up.** 0.7 passes a wait only in repaired command buffers or within 30 s of a corrupt EOP packet
   (500 ms). We have neither mechanism, and passing an unmet wait in general can corrupt silently, so the default is to
   keep waiting and log (500 ms, 10 s). `GOW3_WAIT_REG_MEM_GIVEUP_MS` exists for diagnosis only. The wait also yields
   the core after 256 steps (`std::this_thread::yield`): 0.7 only pauses, but the Deck has 4 cores and the value may
   take a GPU frame.
5. **WRITE_DATA unchanged** (the task's rule): 0.7 writes it on the GPU (`WriteDataOnGpu`, `vkCmdUpdateBuffer` into
   the in-place target), which needs the target in a GPU-visible chunk (U5-3/U5-4). Today's CPU write on stage B stays.
6. **Leak watch: log by default.** 0.7 counts Bloodborne's live libc allocations (`BbHeapSites`) and turns the overlap
   off past +100,000. MediEvil's heaps sit on the runtime's memory: the watch sums direct memory allocated, flexible
   memory mapped and pool pages committed (`runtime_memory_usage`, read without the lock), and after 600 frames
   tracks the growth over the lowest sum seen. Level loads move that sum by hundreds of MiB, so an automatic switch-off
   would likely fire on the first level change and silently turn a K=2 run into K=0. Default: report every 5 s, plus
   one line when the growth passes `GOW3_LEAK_LIMIT_MB`; `GOW3_LEAK_WATCH=stop` acts like 0.7.
7. **Frame end across queues.** 0.7 compares a decoded counter with the frame's submission count; graphics and compute
   queues decode interleaved, so a later compute submission finishing first could retire a frame early. Here every
   submission has a number and a frame retires once the oldest undecoded number is past it.
8. **Pacing counts only real submissions** (`DingDong` with nothing new returns before; 0.7 counts it).
9. **The fence lag probe** (gate 2) keeps working: `SignalAfterGpu` records the tick and the time the signal ran. With
   the switch on, its lag should be ~0 and "GPU done first" close to n.

## What "frame" means here

One `sceGnmSubmitDone` to the next, as 0.7 and the console's lock. MediEvil calls it once per presented frame (Deck run
9: 386 calls in a 77.1 FPS window, 343 in a 68.6 FPS one; `data/deck-native-9.log`), from
`SubmitDoneAsyncTaskThreadPS4`, while `SubmitGPUCommandsAsyncTaskThread` submits. The flip is not used: it is a packet
inside a submission, its interrupt stays at recording time (as 0.7), and the presenter's own GPU-side waits already
order the image.

## Flow with the switch on

```
guest submit ─> gnmdriver: hand over, AfterSubmit (K+1th.. wait for frame F-1 retired)
stage A: decode ─RunInOrder─> stage B: EOP/EOS task -> SignalAfterGpu(tick = CurrentTick)
                                   end of a guest submission -> SubmitForSignalsNow (idle / last / 2 ms)
stage A: frame F's submissions decoded -> drain B -> SignalAfterGpu(retire F) -> Flush
stage A: batch end -> drain B -> SignalAfterGpu(GPU idle, generation) [non-urgent]
stage A: idle 0.2 ms with urgent signals unsubmitted -> Flush
gow3:GpuSignals: wait tick (2 ms steps; unsubmitted for 50 ms -> SendCommand(Flush)) -> labels, IRQs, retire, idle
guest sceGnmSubmitDone(F+1) -> wait until F retired
```

## Stats (`GOW3_FRAME_STATS=1`, in the 5 s block after "Cache actions")

```
GPU fences (5 s, GOW3_GPU_FENCES=1): signals N published, N run (queue max Q); signal thread waited X ms/s (W waits);
  flushes for signals: signal thread a, command thread idle b, WAIT_REG_MEM c, frame end d, submission end e (GPU idle
  e1, last submission e2, interval e3); submits S/frame; CPU writes over pending labels: WRITE_DATA w, flip f; labels
  to unmapped memory u
Gnm pacing (5 s, decode, ahead K): SubmitDone n, waited m (avg, max, ms/s); submissions s (per frame), waited t (avg,
  max, ms/s); WAIT_REG_MEM met by pending signals p, waited q (avg, max), r flushes, y yields
Guest memory (5 s): direct D MiB allocated (the pool's included), pool P MiB committed, flexible F MiB; sum ±x MiB this
  window[, ±g MiB over the low since the warm-up (most h); frames overlap / do not overlap]
```

With the switch off the first line is `GPU fences (5 s): off (GOW3_GPU_FENCES); submits S/frame` and the pacing line
is absent; the memory line prints both ways (the A/B compares it). The gate-2 lines stay: "Fence lag" (sanity: ~0
with the switch), "Guest event waits" (ident 64 is the critical wait), "Submission lock" (SubmitDone count only under
the pacing). One-off lines: `GPU fences watchdog: ...`, `GPU fences: WAIT_REG_MEM on ... not met after ... ms`,
`GPU fences: guest memory in use grew ...`, `GPU fences: device lost ...`.

## Known risks

1. **Heap corruption class (Bloodborne #32's family, study 1.7).**
   - Closed as in 0.7: GPU idle and frames retire only after the labels before them; labels are single atomic stores;
     a label whose memory was unmapped is dropped, not written (counted).
   - Open, by design (as 0.7): with K > 0 the first K submissions of frame F+1 return while frame F's labels may still
     be pending. If UE4 frees or reuses per-frame label memory after its first submission of the next frame, a late
     label lands in reused memory. 0.7 saw a leak, not corruption, from the overlap. K=0 has the console's semantics.
   - Open: WRITE_DATA flags and the flip interrupt overtake pending labels (difference 3). If the guest frees label
     memory on seeing a WRITE_DATA flag, the same class. Watch "CPU writes over pending labels" (expected: most
     WRITE_DATA and every flip, since ~180 labels a frame are always pending) together with crashes and
     "labels to unmapped memory".
   - Symptom to look for: a guest fault in an allocator (a free-list head of garbage), minutes in.
2. **Deadlocks.** Every wait on GPU-ordered signals now has a submitter: the decoder idle flush (0.2 ms), the batching
   at submission ends, the frame-end flush, WAIT_REG_MEM's flush, and the signal thread's 50 ms request (the boot
   deadlock of `f97479f`: an unbounded wait for a tick nobody submitted). Residual: a cycle the guest closes itself
   (e.g. a WAIT_REG_MEM in frame F on a CPU write the game makes after a later submission that the pacing holds); the
   console's lock has the same rule, so the game should not do it. The watchdogs name the side that waits.
3. **Stage A spins.** WAIT_REG_MEM on a GPU-ordered value now spins for up to the GPU lag (~15 ms on the Deck) instead
   of the stage B lag; it yields the core after 256 steps. Watch "WAIT_REG_MEM waited" and stage A's share in the
   frame stats.
4. **More submissions.** ~180 urgent labels a frame keep the decoder idle flush busy. 0.7: ~65 us of GPU time per
   submission. Watch "submits S/frame" against the switch-off run (24-41 a frame in run 3) and tune
   `GOW3_SIGNAL_IDLE_US` / `GOW3_SIGNAL_FLUSH_US`.
5. **Extra flush per frame**: the frame-end `Flush()` follows the existing `sceGnmSubmitDone` flush, often with
   little in it.
6. **Leak watch false positives** with `GOW3_LEAK_WATCH=stop` (level loads): see difference 6.
7. Untested paths: compute RELEASE_MEM with a GDS store or an interrupt, EOS GDS stores, `GOW3_SUBMIT_LOCK=gpu`
   (MediEvil: compute RELEASE_MEM ~2 a frame, EOS ~8 CS_DONE a frame, run 7).

## Test plan for the Deck

Same AppImage for every run; settings as run 9 (67%, graveyard, `GOW3_FRAME_STATS=1`); one change per launch, each
with the user's go. Stand still 60 s in the same spot, then walk the same route 60 s.

| Run | Variables | Pass / reading |
|---|---|---|
| A | none (switch off) | baseline for this build: FPS, 1% lows, "submits/frame", ident-64 wait, "Guest memory" |
| B0 | `GOW3_GPU_FENCES=1 GOW3_SUBMIT_AHEAD=0` | boots (first check: the menu appears; no watchdog line). Fence lag ~0 and "GPU done first" ≈ n (sanity). The honest case |
| B2 | `GOW3_GPU_FENCES=1` (K=2) | the 0.7 configuration |
| soak | B2 for 30 min, moving between areas | no crash, no "labels to unmapped memory", "Guest memory" sum returns to the switch-off run's range after loads |

Numbers that decide (per window, graveyard; compare lows with lows):
- **ΔFPS and 1%/0.1% lows** of B2 and B0 against A. The design's U5-2 rule: ΔFPS ≥ −2 %, lows not worse, no heap
  growth or crash. Study's band: 0 to +3 ms a frame with K=2, up to +15.6 ms without overlap.
- **Where the time went**: ident-64 wait (avg ms per frame) against A's ~5.6 ms; "Gnm pacing" waits (SubmitDone and
  submissions, ms/s); stage B busy (should drop: no host-copy fence waits, study: −0.6 to −1.3 ms a frame); submits
  per frame; WAIT_REG_MEM waited.
- If B2 passes: GPU-ordered fences are affordable, U5's fence precondition holds (go for U5-3). If B0 collapses and B2
  holds: the overlap is what makes it work (keep K=2, watch the corruption risk in the soak). If both collapse
  (> +4 ms a frame, the ident-64 wait grows by ~the fence lag): NO-GO for U5's fences as designed (plan-linux-deck.md
  run 9's alternatives).

Settings by file (plan-linux-deck.md "Test settings by file"; deploy the AppImage as section 6.4), over SSH, no launch:
```bash
# A
ssh deck 'printf "GOW3_FRAME_STATS=1\n" > ~/.local/share/medievil-pc/extra-env.txt'
# B0
ssh deck 'printf "GOW3_FRAME_STATS=1\nGOW3_GPU_FENCES=1\nGOW3_SUBMIT_AHEAD=0\n" > ~/.local/share/medievil-pc/extra-env.txt'
# B2 (and the soak)
ssh deck 'printf "GOW3_FRAME_STATS=1\nGOW3_GPU_FENCES=1\n" > ~/.local/share/medievil-pc/extra-env.txt'
```
The log's `Extra environment (...)` line confirms what applied. Read off:
`grep -E "Extra environment|Frame stats|GPU fences|Gnm pacing|Guest memory|Fence lag|Guest event waits" <log>`.
