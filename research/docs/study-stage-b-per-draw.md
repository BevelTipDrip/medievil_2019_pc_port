# Study: stage B's CPU cost per draw, for the Steam Deck (2026-11-04)

Status: offline study (code and logs, no launch) plus one implemented cut (C1, `f03ee52`, not run yet).
Parent: [plan-linux-deck.md](plan-linux-deck.md) ("Native run 3: stage B by part", "Decode pacing D0-D2").
Related: [study-frame-cost-2026-10-29.md](study-frame-cost-2026-10-29.md) (the PC's ranked stage B fixes),
[plan-work-queue.md](plan-work-queue.md) (stage B queue), [fix-inventory.md](fix-inventory.md).

## Answer

On the Deck, stage B (`gow3:DrawRec`) is the limit in the graveyard heavy view: ~65 FPS, 91% busy, ~1,400 draws a
frame. Most parts of a draw cost 4-5x the PC's. That is what Zen 2 at 3.5 GHz with a 4 MB L3 costs against a
7800X3D. The ordered tasks are the exception. **Each end-of-pipe / end-of-shader event costs 17-26x the PC's**
(4.6 us vs 0.28 us), and its texture-cache part costs 3.3 us vs 0.13 us. There are ~200 of these events a frame.
Inside each one, the readback retirement asks the GPU's timeline semaphore for its tick. On Linux (RADV) that query
is an ioctl; on the PC (NVIDIA, Windows) it is a memory read. The excess is ~0.5-0.8 ms a frame, the largest
Deck-specific item that is not a wait.

Ranked cuts (Deck us per draw; at ~1,400 draws, 1 us/draw = 1.4 ms/frame):

| # | Cut | Deck saving (us/draw) | Effort | Risk | State |
|---|---|---|---|---|---|
| C1 | Throttle the GPU-tick query in the end-of-pipe readback retirement (once per 100 us) | 0.15-0.3 | small | low | **done, `f03ee52`, toggle 2nd word 16, default on** |
| C2 | Recorder wake-ups forced by every event (futex wake, recorder spins only 20 us on 8 threads) | 0.1-0.15 | small-medium | medium (fence latency) | **built, `ff54428`: recorder lean mode, toggle 2nd word 32, on at <= 8 threads** ([impl-deck-cpu-bundle.md](impl-deck-cpu-bundle.md)) |
| C3 | Slow texture binds (~70 a frame at ~15 us each on the Deck): memoize the dominant cause | 0.3-0.5 (cause-dependent) | medium | medium | cause counters in `f03ee52` |
| C4 | One recorded command per draw (push constants, descriptors, vertex input/buffers, index, pipeline, draw) | 0.2-0.4 | medium-high | medium | open |
| C5 | Lock-free pre-check of pending readbacks in `ObtainBuffer` (3-4 calls a draw) | 0.08-0.1 | small | low | open, next low-risk |
| C6 | Descriptor *set* updates (pipelines over 32 bindings: 163 a frame, 1.2 us each, device call on stage B) moved into the recorded command | ~0.1 | small-medium | low-medium | open |
| C7 | Vertex input / vertex buffer / index rebind elision | 0.1-0.15 (repeat rate unknown) | medium | medium | open, needs a repeat counter |
| C8 | Descriptor write elision / delta pushes | 0.1-0.2 | medium | medium-high (spec, layouts) | open, low |
| C9 | Pipeline bind elision | ~0 on stage B | - | - | **don't** |
| C10 | Own-target texture-set memo (PC study item 4) | <0.05 on the Deck | medium | medium | park |

C1 plus the fixable part of C2 come to roughly half of the Deck's excess in ordered tasks. C3 and C4 are the larger
per-draw items. The Deck test for C1 is in section 6.

## 1. Data, and three caveats

- **Deck:** `data/deck-native-3.log` (AppImage `56e323b`, `GOW3_SECTIONS=1`), steady heavy windows (lines 3600-3810:
  ~100k packets/s, 63.5 FPS, stage B busy 91-92%, 1,311 draws and ~1,570 packets a frame; packets include ~56
  dispatches and ~200 tasks). `data/deck-decode-D2.log` (D2 build, no sections): binds, memo and descriptor lines.
- **PC:** `data/t-sampler-1.log` (4K heavy, 228 FPS, ~1,890 packets a frame, line 2862).
- **Caveat 1: sections cost time.** Every `GOW3_SECTION` scope adds two `rdtsc` and a locked add (~100 cycles on
  Zen 2). There are ~25 scopes per draw, so the instrumentation adds ~0.5-0.8 us per draw on the Deck. Small parts
  (Record calls, emit vertex, kick) are inflated the most. The Deck/PC ratios still hold, because both machines
  carry the same instrumentation.
- **Caveat 2: per packet, not per draw.** The sections print us per packet. Run 3's table divides by draws.
  Here I use the steady per-packet lines; multiply by ~1.2 for per draw.
- **Caveat 3: "busy" includes waits.** Stage B busy counts every cycle spent inside a packet. Two tasks are mostly
  waiting: `flip_present` (section 3) and `RunWriteData` (C11). The 91% is not all CPU work.

Deck vs PC, us per packet (steady heavy windows):

| Part | PC | Deck | x | What runs |
|---|---|---|---|---|
| packet | 1.861 | 9.154 | 4.9 | everything below plus the register-block copy and user-data snapshots |
| draw | 1.547 | 6.793 | 4.4 | `DrawRecord` |
| bind | 0.552 | 2.679 | 4.9 | `BindResources` (buffers, textures, and vertex/index resolve while textures bind) |
| textures | 0.371 | 1.752 | 4.7 | `BindTextures` |
| texset (memo hit) | 0.150 | 0.851 | 5.7 | memoized set: lookup, image checks, emit loop with `Transit` |
| texslow (no memo) | 0.103 | 0.692 | **6.7** | full bind: `FindImage` via the desc cache, `FindTexture`, layouts, samplers |
| samplers | 0.098 | 0.157 | 1.6 | `BindSamplers` with the sampler memo (`5c752e8`) |
| ObtainBuffer | 0.257 | 1.217 | 4.7 | buffer cache lookups (vertex ranges, index, storage buffers) |
| obt_sync | 0.062 | 0.388 | 6.3 | uploads of CPU-written ranges |
| obt_readbacks | 0.029 | 0.131 | 4.5 | `ResolveReadbacks` (mutex and scan, readbacks always queued) |
| vertex | 0.212 | 1.197 | 5.6 | `ResolveVertexBuffers` (prepared inputs, merged ranges, `ObtainBuffer`) |
| index | 0.097 | 0.372 | 3.8 | `ResolveIndexBuffer` |
| emit vertex | 0.064 | 0.257 | 4.0 | `setVertexInputEXT` + `bindVertexBuffers` recorded (2 Record, 4 RecordData) |
| descriptors | 0.181 | 0.758 | 4.2 | `Pipeline::BindResources`: push constants + bulk push-descriptor copy |
| dynamic | 0.075 | 0.340 | 4.5 | `UpdateDynamicState` |
| begin render | 0.118 | 0.379 | 3.2 | render-state memo (93.7% hit on the Deck) |
| render state | 0.061 | 0.239 | 3.9 | `PrepareRenderState` |
| Record calls | 0.080 | 0.385 | 4.8 | `Scheduler::Record` (6.2 a packet) |
| **tasks** | 0.221 | 1.738 | **7.9** | ordered tasks (section 3) |
| **end of pipe** | 0.022 | 0.421 | **19** | `TextureCache::ProcessDownloadImages` inside the event tasks |
| flip_present | 0.133 | 0.804 | 6.0 | the guest's flip handler (mostly waits) |

## 2. The per-draw path, part by part

Stage A (`gow3:GpuCmd`) decodes PM4. It picks the pipeline (`GetGraphicsPipeline`, prepared by 4 workers, 97.8%
prepared on the Deck), copies constants into the constant ring (`CollectRingBindings`, reuse on: 8.9 of 11.3 MB a
frame reused) and hands over a packet (`PostDraw`). Stage B (`DrawPipe::Run` → `RunDrawPacket`) copies the dirty
register blocks, installs the user-data snapshots and runs `DrawRecord`. Each `Scheduler::Record` closure is
placed in a 128 KiB chunk and runs later on `gow3:VkRecorder`. So **the Vulkan calls themselves (`vkCmd*`) are the
recorder's cost**; stage B pays to build the arguments and copy them into the chunk.

In order, per draw on stage B:

1. `BeforeRecord`, `PopPendingOperations` (one atomic load per draw; a semaphore query every 32 polls when work is
   pending), `FrameCapture::Poll`, frame-start detection. Small.
2. `PrepareRenderState`: render targets from the registers, through two memos. Changes only at target switches.
3. `BindResources`: `MakeUserData`, then per stage `BindBuffers` (ring bindings: no lookup; other buffers:
   `ObtainBuffer` + `IsBufferAccessed`), `BindTextures` (memoized set, else the slow path), `BindSamplers`
   (memoized handles). Vertex and index buffers are resolved inside it (`EarlyDrawInputs`): `ResolveVertexBuffers`
   copies the prepared attributes/bindings and calls `ObtainBuffer` per merged range; `ResolveIndexBuffer` calls
   `ObtainBuffer` once.
4. `BeginRendering`: the render-state memo returns the open pass's state when the signature matches.
5. `EmitVertexBuffers` / `EmitIndexBuffer`: 3 Record, 4 RecordData.
6. `FlushBarriers` if a transition was queued.
7. `Pipeline::BindResources`: one Record for push constants, then the push descriptors. These are 17.6 writes on
   average, as one bulk copy of writes + image infos + buffer infos (3 RecordData) and one Record. On the Deck,
   ~163 draws a frame use a pipeline with more bindings than RADV's 32 push descriptors. These go to
   `desc_heap.Commit` + `vkUpdateDescriptorSets` **on stage B** (1.2 us each; 0.4 on the PC).
8. `UpdateDynamicState`: records only the dynamic state that changed.
9. `scheduler.BeginRendering`, then one Record: `bindPipeline` + `draw`.
10. `ResetBindings` (image binding flags, buffer access tracking), `KickRecording` (a hand-off every 32 KiB).

**Redundant from one draw to the next in MediEvil** (D2 heavy view; PC similar):

- Pipeline: 77% of binds have the same handle as the previous draw (1,081 of 1,418). The bind costs stage B
  nothing apart from its share of the draw's closure.
- Render state: 93.7% memo hits (already skipped).
- Texture sets: 99.3% memo hits (already skipped to the emit loop). ~70 sets a frame take the slow path (C3).
- Descriptor contents: never byte-identical (t-base1: 0 repeats). The flatbuf and small stream copies get a new
  stream-buffer offset every draw, and ring constants are reused only inside one guest command buffer. Within a
  same-pipeline run, most of the image and sampler writes are likely the same. Unmeasured.
- Vertex input layout: likely the same within a same-pipeline run (same fetch shader). The buffers differ per mesh.
  Unmeasured.
- Index buffer: per mesh. Unmeasured.

**Already skipped or cheapened:** prepared draws (pipeline, sharps, vertex inputs), constant ring with reuse,
small read-only buffers from the arena (59%), render-state memo, texture-set memo (two-way, region stamps), image
desc cache, sampler memo, bulk descriptor copy, `KickRecording` batching (32 KiB), lean barriers, early draw inputs.

## 3. What runs as tasks on stage B

Tasks are PM4 events that must run in stream order with the draws (`Rasterizer::RunInOrder`). Their costs per
heavy frame, matched by rate (run 3's `exe+...` kinds; Cache actions in D2: EOP 180 a frame, EOS 20):

| Kind (run 3) | Per frame | Deck each | PC each | What |
|---|---|---|---|---|
| `c4c0` = `RunEventWriteEop` | ~178 | 4.6 us | 0.28 us | `ProcessDownloadImages` (3.3 us of it; PC 0.13), then `SignalAfterHostCopies`: a closure into the chunk + `KickRecording(true)` |
| `c190` = `RunEventWriteEos` | ~20 | 6.3 us | 0.30 us | the same, for end-of-shader |
| `cf60` = `RunWriteData` | 2 | ~250 us | ~35 us | waits for the deferred signals: **a wait for the recorder thread**, not CPU (T-S1c) |
| `be40` = `SignalFlip` | 1 | ~1.26 ms | ~0.25 ms | the guest's flip handler: `PrepareFrame` → `GetRenderFrame` waits for a free presentation frame and its present fence, and `SyncRecording` for the recorder: **mostly waits** |
| `cc30` (rare) | <1 | - | - | `RunDmaData` / occlusion |

Since `c4a0147`, `OnSubmit` (texture GC, downloads, `TickFrame`) also runs on stage B once a frame
(`GOW3_ONSUBMIT_STAGE_B`). It is not in run 3.

Why the events are 17-26x on the Deck:

- **`ProcessDownloadImages` → `RetireReadbacks` → `Scheduler::IsFree` → `Semaphore::Refresh` →
  `vkGetSemaphoreCounterValue`.** With deferred readbacks on (the default), small readbacks (HZB, exposure) are
  almost always queued. The front one is usually not finished: the GPU runs ~1 frame behind, fence lag p50 18 ms
  on the Deck. So nearly every event queries the semaphore. RADV with kernel timeline syncobjs answers through
  `DRM_IOCTL_SYNCOBJ_QUERY`: a syscall, plus Spectre mitigations and the DRM handle lookup. `PopPendingOperations`
  already limits its own queries to 1 in 32 for this reason ("Querying the timeline semaphore is an ioctl").
- **`KickRecording(true)` per event** after the signal closure. The recorder thread spins only 20 us before
  sleeping on machines with fewer than 12 threads. Events come every ~75 us on average, so many kicks wake a
  sleeping recorder: a futex wake on stage B, and a context switch on 8 threads.
- Smaller items: the targeted host-copy waits take `host_copy_sources_mutex` once per queued readback range, the
  two cache mutexes, a `std::function` allocation per signal, and `Gow3Copy::FlushBatch` (a copy-thread wake when a
  batch is pending).

Excess: the event tasks would cost ~0.2 ms/frame at the usual 4.5x and cost ~0.95 ms. That leaves ~0.7 ms/frame
(~0.5 us per draw) that is not CPU speed. `f03ee52` adds counters that split it: the "Stage B syscalls per frame"
line (semaphore queries and recorder wakes, with their cost).

## 4. Ranked cuts

Savings are scaled from PC measurements by run 3's per-part ratios, or computed from Deck counts × Deck cost.

**C1. End-of-pipe readback retirement: query the GPU's tick at most once per 100 us.** (Done, section 5.)
~200 events a frame, ~1 query each. A query costs ~1-2.5 us on the Deck (the counter will say), so ~0.2-0.5 ms a
frame. The throttle skips the queries made less than 100 us after any other thread's query. Events are clustered,
so expect 50-80% skipped: **~0.15-0.3 us/draw**. Risk low: the answer is only ever "not finished yet". A finished
copy retires at a later event, at most ~100 us late, or the guest's touch resolves it (`ResolveReadbacks` waits as
before). With stale readbacks on, a guest read in that window sees the previous frame's copy, which is already the
designed behaviour for unfinished copies. `GOW3_SEMAPHORE_POLL_US=250` / `500` skips more.

**C2. Recorder wake-ups forced by every event.** `SignalAfterHostCopies` → `KickRecording(true)` → `notify_one`
when the recorder sleeps. Options:
- (a) A longer recorder spin while stage B has packets queued (it knows; this costs a hardware thread on the Deck).
- (b) No forced kick for EOPs without an interrupt. Their labels are polled by the guest, so this needs care: fence
  latency (the D0-D2 work shows the game waiting on labels).

Saving ~0.1-0.15 us/draw if half the wakes go. Gate: the new "recorder wakes ... (us each)" counter. Do it only if
the wakes are ≥100 a frame at ≥1.5 us.

[study-deck-other-threads.md](study-deck-other-threads.md) §3.1 and its item B reach the same mechanism from the
recorder's side (7.3k switches/s; it proposes no spin on ≤8 threads plus a larger hand-off). The two interact: a
shorter spin means more of the wakes counted here. Measure the wake counter before and after that change.

**C3. Slow texture binds.** ~1.1 ms a frame on the Deck (0.69 us/packet), 6.7x the PC. D2: "not memoizable" 64 a
frame, misses ~5 a frame, so ~15 us a call. That class (`texture_set_unkept[7]`) is *not* the own-target sets: it
is unprepared draws (~24 a frame: 7,814 / 5 s), dispatches, written or mip-fallback images, or sets over 16 images.
`f03ee52` prints the split per 1000 draws, with the time each ("slow texture binds" line). Then:
- unprepared draws: let the binds of a draw without a prepared draw use the memo, keyed on the sharps it reads.
- dispatches with storage images: memoize the sampled part only.
- over 16 images: raise `MaxImages`.

Up to 0.3-0.5 us/draw if the main cause is memoizable. Medium risk (layouts, invalidation, as for the memo).

**C4. One recorded command per draw.** Today a draw makes 6.2 `Record` and ~6 `RecordData` calls. Each one does a
draw-recorder sync check, the `ChunkGuard` spin lock, a reserve check, allocation and prefetch. One closure for
push constants, push descriptors, vertex input, vertex buffers, index buffer, pipeline and draw, with one data
block, keeps the barrier and `BeginRendering` order: those stay separate and before it. ~0.2-0.4 us/draw (section
overhead inflates "Record calls"; measure with sections off, A/B by toggle). Medium-high effort, because
`Pipeline::BindResources` is shared with dispatches and indirect draws. Medium risk: the recording order changes
for vertex binds and descriptors, which do not interact with barriers.

**C5. `ResolveReadbacks` pre-check.** `ObtainBuffer` calls it 3-4 times a draw. With readbacks always queued, each
call takes `readback_mutex` and scans the queue (obt_readbacks 0.131 us/packet). Keep the bounding range of the
queued copies in two atomics (updated under the lock on push, retire and cancel), and return early when the
buffer is outside it. Keep the stale-mode CPU-write path (`ReleaseOutside`) under the lock as it is: it runs for
CPU writes only. ~0.08-0.1 us/draw, low risk: the filter can only send more calls to the lock. **The next
low-risk candidate.**

**C6. Descriptor set updates off stage B.** RADV allows 32 push descriptors, and ~163 draws a frame need more, so
they take the set path: `desc_heap.Commit` and `vkUpdateDescriptorSets` on stage B, 1.2 us each (PC 142 at 0.4
us). Copy the writes into the chunk as the push path does, and run the update on the recorder just before
`bindDescriptorSets`. The set is new, so no command buffer uses it. Stage B keeps the copy (~0.3 us): ~0.15 ms a
frame, **~0.1 us/draw**. The recorder takes the 1.2 us; check its headroom first (C2's counters, `RunWriteData`
waits).

**C7. Vertex input / vertex buffer / index rebind elision.** Skip `setVertexInputEXT` when the bindings and
attributes equal the previous draw's in the same command buffer; likewise the buffers and the index binding.
Stage B saves 1-3 Record + 2-4 RecordData per repeat (emit vertex 0.257 us/packet in all). Repeat rates are
unmeasured. The input layout probably repeats in most same-pipeline runs (77%); buffers repeat only for instanced
pieces. Invalidate at every new command buffer (submission, `Flush`), every direct-mode `CommandBuffer()` use, and
the host passes that set their own vertex input or buffers (FSR, post-processing, camera motion, fault manager:
study 2026-10-29 item 2 lists ~12 sites). Gate: a repeat counter. ~0.1-0.15 us/draw at best.

**C8. Descriptor write elision / delta pushes.** Whole-push repeats are 0%, so full elision does not apply. Delta
pushes (only the writes that changed since the previous draw with the same pipeline) have two open questions:
- Whether push-descriptor contents persist across pushes. In practice they depend on pipeline layout compatibility
  and on the driver (RADV keeps the CPU copy and re-uploads the whole set at each draw, so the recorder gains
  little). Check the Vulkan spec, and test on RADV and NVIDIA, before relying on it.
- The cost of comparing ~18 writes, about the same as copying them.

Stage B saves perhaps a third of the copy on same-pipeline draws: ~0.1-0.2 us/draw. Medium-high risk. Low.

**C9. Pipeline bind elision: don't.** The bind is inside the draw's own closure (`bindPipeline` + `draw` in one
Record), so stage B gains nothing. On the recorder, RADV's `vkCmdBindPipeline` already returns early for the bound
pipeline, after re-applying its static state. The second "square" pipeline and host passes in between would need
invalidation for no stage B gain.

**C10. Own-target texture-set memo (PC study item 4): park for the Deck.** In the D2 heavy view only ~5 sets a frame
are dropped for their own colour / depth target (185 + 1,480 per window of 262,144 draws). The "~65 not memoizable
a frame" in the brief is C3's class, not this one. <0.05 us/draw.

**C11. Waits counted as busy (not per-draw work):** `flip_present` (~1.26 ms a frame: presentation frame pool,
present fence, recorder sync) and `RunWriteData` (2 a frame, ~250 us each, waiting for the recorder's deferred
signals). Both shrink when the recorder keeps up (C2, C4, C6 move work on or off it) and with present-path work
(native Wayland, presentation frames). Add a sub-timer to `GetRenderFrame` before working on them.

## 5. Implemented: C1 (`f03ee52`)

- `Scheduler::IsFreePolled(tick)` (`vk_scheduler.h`): answers from the known tick. It queries the semaphore only
  when no thread queried it within `GOW3_SEMAPHORE_POLL_US` (default 100; 0 disables). `Semaphore::Refresh` stamps
  each query, `Semaphore::RefreshedWithin` reads the stamp.
- Used only by the end-of-pipe readback retirement: `TextureCache::ProcessDownloadImages` → `RetireReadbacks(true)`,
  and the opt-in idle-skip check. All other `IsFree` callers (fault paths, staging, signals heuristics,
  `ResolveReadbacks`, `SnapshotReadback`, the `Finish` fallbacks) still query as before.
- **Toggle:** second word bit 16 (`Gow3Toggle2::SemaphorePollInterval`), **default on** (bit set: a query at every
  event again). Toggle file: `0 16` = off, `0 0` = on. Default on because the change can only delay a retirement by
  at most the interval and never makes one early. Pacing effects (readbacks seen later) are the thing to watch.
- **Counters (`GOW3_FRAME_STATS=1`):**
  - "Stage B syscalls per frame": semaphore queries on stage B and on other threads, with us each; queries skipped by
    the poll interval (on/off), and the time saved (skips × stage B's own cost per query); recorder wakes on stage B
    and on other threads, with us each (C2's gate).
  - "slow texture binds per 1000 draws (us each)" (with the texture-set lines): dispatch, unprepared draw, written
    or mip image, over 16 images, memo off or no image, memo miss (C3's gate).
- Built: Windows (`build-clang64.sh`, LTO) and Linux (WSL Debian, clang, AppImage). Unit tests: 124 run, the 8
  known failures, no new ones.

## 6. The Deck test (one launch, the user's go)

Settings: as D2 (67%, auto clocks, `GOW3_FRAME_STATS=1`, decode pacing as configured), toggle file as in
extra-env.txt. Sections off, so the timings stay clean; `GOW3_SECTIONS=1` only if a second run is wanted for
"end of pipe".

1. Start with `toggles.txt` = `0 0` (C1 on). Go to the graveyard heavy view and stand still.
2. Every ~30 s, switch the second number: `0 16` (off), `0 0` (on), `0 16`, `0 0`: 4-5 windows each way (each
   window is 5 s of stats; drop the first after each switch). The log prints
   `Runtime: disabled optimizations second word=16` at each switch.
3. Compare on vs off:
   - **FPS** ("Perf:" / "Frame stats") and **stage B busy** ("Draw pipe: ... stage B busy").
   - The "Stage B syscalls" line: queries on stage B a frame (expect ~200 off, a fraction of that on), us each
     (confirms the ioctl cost), skips and ms saved.
   - The game's ident-64 wait ("Guest event waits") and the frame lead: must not get worse.
   - "Fence lag" and readback behaviour: the HZB remap line (keys hidden / visible, age): no change in culling.
   - Visuals: torches and occlusion popping (the readbacks drive HZB occlusion).
4. Also read, for C2 and C3 (both directions): recorder wakes a frame on stage B and their us each; the slow
   texture bind causes per 1000 draws.

Pass: on vs off, stage B busy lower or FPS higher by ≥ the ms saved the counter reports. Fence lag, culling and
visuals the same. If the queries turn out cheap (<0.5 us each), C1 is worth little: drop it and move to C5 / C3.
