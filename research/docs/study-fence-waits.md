# Study: where the game waits on GPU work, and what GPU-ordered fences would cost on the Deck

Status: study, 2026-11-03, on `deck-lab` (`32a4b3d`). Read only: nothing built, launched or committed. Parent:
[plan-linux-deck.md](plan-linux-deck.md) ("Direction", U5 order step 2: "the fence precondition"). Companion:
[design-u5-deck-memory.md](design-u5-deck-memory.md). Background: [study-guest-memory-direct.md](study-guest-memory-direct.md),
[fix-inventory.md](fix-inventory.md), [gpu-threading-reference.md](gpu-threading-reference.md). Bloodborne reference:
`bloodborne_pc` at `3c57da4` (0.51), plus `origin/master` `0fc2921` (0.7) where marked "BB 0.7" (read with `git show`, not
checked out).

Marks: **[code]** read in this tree. **[log]** a number from a log in `research/docs/data/` read for this study (not
confirmed by the user in game). **[doc]** from our own docs. **[BB]** Bloodborne code or docs. **[web]** web source
(Sources at the end). **[inferred]** reasoning, not checked. Nothing on this page is "verified" in the project's sense.

## Answer

1. **Today every guest-visible fence signals when the port's CPU pipeline reaches it, not when the GPU runs it.** EOP and
   EOS labels and WRITE_DATA are written by the Vulkan recording thread after the host copies queued before them, the flip
   interrupt after those, GPU idle when stage A and B ran dry [code]. On the Deck the GPU finishes a frame **~15.6 ms after
   its recording ends** (flip trace, run 4) [log], against 0.44 ms on the PC. That gap is what GPU-ordered fences would add
   to any fence the game is waiting for when it signals.
2. **The waits that sit on the game's critical path** (section 3):
   - the **submission lock**: `sceGnmSubmitDone` arms it, the next `sceGnmSubmit*` blocks until "GPU idle"
     (`gnmdriver.cpp:2355-2367`, `:87-91`). Today GPU idle = stage A and B drained. Made honest without frame overlap,
     Bloodborne lost a third of its frame rate (101 vs 154 FPS) [BB];
   - the **event-queue wait once a frame** (PC heavy view: guest+0x6885e7, 3.8-4.1 ms per frame, T-W3) [doc]. Which event it
     waits for (end of pipe or flip) is not known;
   - **label spins** of the render thread (occlusion and timestamp queries, `RHIMapStagingSurface`), mostly removed by the HZB
     remap [doc].

   The port's own copy-model waits sit on **stage B**, the Deck's limit (92-94% busy), not on game threads. Together they
   take ~10% of stage B: host copies 4.0-4.9%, WRITE_DATA ~3.7%, plus the deferred-signal waits inside the drains [log].
   GPU-ordered fences remove those.
3. **Estimate.** Per frame, from **−1 ms** (stage B's fence waits gone, no game wait on the newest frame) to **+15.6 ms**
   (a per-frame wait on the newest frame's fence with no slack, or the submission lock without overlap).
   - With a Bloodborne-style overlap of the submission lock (the first K submissions of a frame do not wait) and the Deck
     GPU-bound, **0 to +3 ms a frame** is the likely band [inferred].
   - The formula is in section 4. The numbers it needs (slack per wait, fence lag per kind, lock hold times) are what the
     instrumentation in section 5 measures in one Deck run.
4. **The `GOW3_ASYNC_FENCES` result (79 vs 83 FPS) is not a MediEvil measurement.** The comment came in with the bb→gow3
   rename (`a7cf186`) and repeats Bloodborne's `BB_ASYNC_FENCES` line (`bloodborne_pc liverpool.cpp:473-476`) [code][BB].
   It shows that some PS4 game waits on these fences, but not how much MediEvil does.
5. **PS4 memory types:** 0 = WB Onion, 3 = WC Garlic, 10 = WB Garlic (shadPS4's `MemoryTypes` enum) [web].
   - The game's cache actions mark the native points where GPU writes become CPU-visible: EOP event type, RELEASE_MEM cache
     bits, ACQUIRE_MEM `cp_coher_cntl`, EVENT_WRITE flushes. The port decodes and drops all of them today [code].
   - They are candidate sync points (section 2.4).
   - The fault map by PS4 type (section 5.5) answers in one run whether the ~760 faults a frame sit in Onion pages.

## 1. How a guest fence travels today

Stages, Deck run 4 flip trace (`research-data/flip/deck-native-4.csv`, 6,704 frames of 14-40 ms, medians) [log][doc]:

| Step | What | Deck | PC (`t-lab-flip-1`) |
|---|---|---|---|
| game submit → irq | game registers the flip (`video_out.cpp:353`) → stage B reaches the flip packet (`driver.cpp:1033`) | **12.53 ms** | 0.26 |
| irq → queued | present prepared (`driver.cpp:1085`) | 1.79 | 0.06 |
| queued → gpu_ready | GPU has finished the frame (`vk_presenter.cpp:1090`) | **15.62 ms** | 0.44 |

- The guest sees its fences at stage B time, ~12.5 ms after the submit.
- Under GPU-ordered fences it would see them at GPU time, ~15.6 ms later still: **~30 ms after the submit, about 1.8
  frames at 60 FPS** [log, inferred sum].
- The GPU is busy almost the whole frame on the Deck (run 4), so a mid-frame fence lags by about the same amount as the
  end-of-frame one [inferred: no mid-frame data].

Signal mechanics [code]:
- `Scheduler::SignalAfterHostCopies` (`renderer_vulkan/vk_scheduler.cpp:338-353`) records a closure into the Vulkan
  recording stream. The recorder hands it to `Gow3Copy::AfterCopies`, so the fence is written once the copy threads have
  finished the copies queued before it.
- `WaitDeferredSignals` (`:355-368`) makes later writers wait for those closures.
- `WaitHostCopies` (`:423-480`) is the general wait for host copies, and runs again before every submission (`:730`).
- The GPU itself plays no part. `RecorderFences` (toggle bit 41) chooses between this deferral and synchronous waits on
  stage B.

## 2. Inventory of waits

Columns: **Who waits** is the thread that blocks (guest thread names from the Deck logs: `GameThread` (main), `RenderThread
0/1`, `RHIThread`, `RHIFrameFlipThread`, `RHIFrameOffsetThread`, `SubmitGPUCommandsAsyncTaskThread`,
`SubmitDoneAsyncTaskThreadPS4`, `deck-native-4.log` thread list [log]). **GPU-ordered** is when it would signal if fences
followed GPU execution. **Extra on the Deck** is the added latency of the signal (L = fence lag, ~15.6 ms at the end of a
frame). It turns into a wait only where the guest needs the fence before L has passed.

### 2.1 Guest (CPU-side) waits

| # | Wait | Where | Who waits | Signals today | GPU-ordered | Extra on the Deck | Evidence |
|---|---|---|---|---|---|---|---|
| G1 | **Submission lock** (GPU idle) | armed in `sceGnmSubmitDone` if `!IsGpuIdle()` (`core/libraries/gnmdriver/gnmdriver.cpp:2355-2367`); waited in `WaitGpuIdle` (`:87-91`) by `sceGnmSubmitCommandBuffers[ForWorkload]` (`:2342`), `SubmitAndFlip` (`:2299`), `DingDong` (`:302`); released by the GpuIdle IRQ (`ResetSubmissionLock`, `:81-85`, registered `:2934`) | the thread that submits: `SubmitGPUCommandsAsyncTaskThread` / RHI [inferred from names] | `Liverpool::Process` raises GpuIdle after stage A ran dry, `DrainDrawPipe(SubmissionEnd)` and `WaitDeferredSignals` (`amdgpu/liverpool.cpp:185-200`); `IsGpuIdle` = no submits and `work_retired` (`liverpool.h:117-123`, `GOW3_WORK_RETIRED`) | after the GPU executed the last submission (Bloodborne: signal thread on the timeline) | up to L per frame if the next frame's first submit comes before the GPU finishes the last one; **0 with an overlap of K submissions** (Bloodborne `BB_SUBMIT_AHEAD=2`) | Bloodborne: the game frees a frame's memory when the lock releases ("without it, its heap ran out after ~7 minutes"); honest idle without overlap 101 FPS, with K=2 154 (`bloodborne_pc gnmdriver.cpp:110-145`) [BB]. **MediEvil: never measured** (no counter on `WaitGpuIdle`) |
| G2 | **Event-queue waits** (`sceKernelWaitEqueue`) | `core/libraries/kernel/equeue.cpp:467-499`; events: GfxEop from `sceGnmAddEqEvent` (`gnmdriver.cpp:127-160`), triggered by `SignalEop`'s IRQ (`liverpool.cpp:253-264`); flip events from `CompleteFlip` (`videoout/driver.cpp:994-1004`); vblank events | RHI side, render thread [inferred] | EOP: at the deferred `SignalEop` (stage B time + host copies). Flip: when the present is released (`driver.cpp:1005`, not at display) | EOP: after GPU execution (+ submission granularity + signal-thread wake). Flip: unchanged (section 2.1 G4) | EOP-based: up to L for a wait on the newest frame's EOP; 0 for EOPs ≥ 1 frame old (they are GPU-done once stage B passes the next flip, `frames_queued=1`, G4) | PC T-W3 (heavy view, ~122 FPS): `equeue_wait` guest+0x6885e7 ~123/s = once a frame, **3.8-4.1 ms each** (calm 1.3 ms), 60 ms timeout; guest+0x69c1ac ~360/s at ~0.95 ms (plan-frame-pacing.md:710-711) [doc]. Which filter / ident: **unknown**. Deck: not measured |
| G3 | **Label spins** (guest polls memory a fence writes) | guest loops on `Sleep(0)` / `usleep(0)` (runtime `usleep`, wait-site kind 2) | `RenderThread 1`: query results (guest+0x689c89, 1.2-1.3 M calls/s on the PC before the HZB remap); `RHIMapStagingSurface` spin until stage B wrote a results copy's label (T-HZ4c) | label written by `SignalEop`/`SignalEos` deferred; occlusion counters synthetic at decode (`liverpool.cpp:1228-1254`); HZB results served from finished copies by the remap hook | GPU time | up to L per spin that waits on the newest frame | plan-frame-pacing.md:212-232, plan-stage-b-split.md:304 [doc]. HZB remap (T-HZ4d) removed the map spin. Deck: not measured. MediEvil fires ~3 EOPs a frame (`texture_cache.cpp:306-308` comment) [code], so few labels to spin on |
| G4 | **Flip / vblank** | `sceVideoOutWaitVblank` (`videoout/video_out.cpp:382`); flip events; flip label reset + `SignalVoLabel` in `CompleteFlip` (`driver.cpp:1006-1011`) | `RHIFrameFlipThread`, `RHIFrameOffsetThread` [inferred from names] | the present thread releases the frame (pacer slot). The presented image's GPU readiness is enforced GPU-side (`vk_presenter.cpp:979-982` semaphores) and by `frames_queued=1`: stage B waits for frame N-1's GPU tick before preparing frame N (`vk_presenter.cpp:680-691`) | unchanged: flip completion already follows the presenter, and the present needs the frame finished anyway. Bloodborne keeps the flip IRQ at record time [BB] | ~0 [inferred] | flip trace above |
| G5 | **Image readback waits** | CPU read of a pending readback range faults → `scheduler.Wait(tick)` (`texture_cache/texture_cache.cpp:229-240`) | game threads | already at GPU completion (deferred readbacks retire when their tick is free) | unchanged | 0 | Deck runs use `stale_readback=1` (the previous frame's copy, no wait): "waits: readback 0.00" in every slow-frame line of run 3 [log] |
| G6 | **Buffer downloads on write faults** | `BufferCache::DownloadMemory` (`buffer_cache/buffer_cache.cpp:800-878`): `scheduler.Wait(write_tick)` `:841` (D3), `Finish()` `:864` | the faulting game thread | at the GPU write's completion (already GPU-ordered) | unchanged in the copy model; gone for in-place ranges under U5 | 0 | Deck run 3: write-fault downloads 0.2/s, 0.7 ms per 5 s; twins 14/s [log] |

### 2.2 Command-processor waits (the GPU-side packets, run by stage A / B on the CPU)

| # | Wait | Where | Who waits | Signals today | GPU-ordered | Extra on the Deck | Evidence |
|---|---|---|---|---|---|---|---|
| P1 | **WAIT_REG_MEM, graphics** | `liverpool.cpp:1350-1388`. Three paths: (a) VO label: drain, then `WaitVoLabel` (`:1361-1366`); (b) value already pending in stream order: met (`PendingFenceValue`, `:1369-1377`, `:493-508`; toggle bit 39); (c) else drain and yield until memory matches (`:1380-1386`) | stage A | (a) flip completion; (b) at decode; (c) whoever writes the memory | (b) stays met by queue order (Bloodborne: 31k/s, all met, the CPU never waits [BB]); (c) later if the writer is a GPU-ordered fence of another queue | (b) 0; (c) up to L | Deck run 3: drains by reason `WaitRegMem` 190/s, 12.0% of stage A; stage A waits `ProcessGraphics:1364` (VO path) 11.1%, `:1382` (path c) 0.9% [log] |
| P2 | WAIT_REG_MEM, compute | `liverpool.cpp:1724-1730` (spins, no pending-fence rule) | stage A (compute) | writer | as P1 (c) | up to L | no counter |
| P3 | MEM_SEMAPHORE, REWIND, CE/DE counter | `liverpool.cpp:1320-1333`, `:1339-1348`, `:548-553`; compute `:1712-1722`, `:1611-1619` | stage A | CPU signal / guest | unchanged | 0 | no counter; use in MediEvil unknown |
| P4 | **RunWriteData waits** | `liverpool.cpp:380-424`: `WaitHostCopiesFor` + `WaitDeferredSignals` before the `memcpy` (B1 targeted, toggle bit 62); W3 deferral opt-in `GOW3_WRITEDATA_DEFERRED` | **stage B** | on stage B after the deferred fences | the GPU writes it (`vkCmdWriteBufferMarkerAMD` / `vkCmdUpdateBuffer`): stage B no longer waits | stage B **saves** ~0.6 ms a frame; the guest sees the flag up to L later | Deck run 4: `WriteData targets (5 s)`: `0x399e2420` / `0x399e2418` 4 B value 1, each every other frame, 58-66 ms per 5 s; `0x30000000a0` 8 B value 0, once a frame, 50-60 ms per 5 s. Total ~185 ms per 5 s = **3.7% of stage B** [log]. PC: two "done" flags a frame at 0x6d2a4c88/90 (comment `liverpool.cpp:383-386`). **Who reads them: unknown** |
| P5 | **Drains** (stage A waits for stage B) | non-pipelined opcodes `liverpool.cpp:866-877` (+ `WaitDeferredSignals`); compute `:1500`; ConstRam `:540`; queued commands `:100`; submission end `:193-194` | stage A | stage B reaching the point (+ deferred signals) | the `WaitDeferredSignals` part goes; the drains stay (they exist for register state and single-threaded scheduler use) | stage A gets a little faster | Deck run 3: stage A waited **47.0%**; `Process:193` (submission end) 24.2%, `ProcessCompute:1500` 10.0% [log] |
| P6 | EOS GDS store | `liverpool.cpp:458-465`: `Finish()` then read GDS | stage B | synchronous GPU wait | unchanged (or a GPU copy in stream order, Bloodborne `:681-689` [BB]) | 0 | rare [inferred] |
| P7 | Compute RELEASE_MEM | `liverpool.cpp:1732-1750`: `WaitHostCopies` + `WaitDeferredSignals`, then the label / pipe IRQ | stage A (compute) | after host copies | GPU-ordered (Bloodborne `:2582-2619` [BB]) | up to L for its consumer | no counter |

### 2.3 The port's own waits (copy model and pacing)

| # | Wait | Where | Thread | Exists because | Deck share | Under GPU-ordered fences |
|---|---|---|---|---|---|---|
| O1 | Host copies before fences and submits | `SignalAfterHostCopies`, `WaitDeferredSignals`, `WaitHostCopies` (`vk_scheduler.cpp:338-480`, `:730`) | stage B, recorder | the GPU reads staging copies the copy threads fill | "blocked: host copies **4.0-4.9%** (1/frame, 514-593/frame skipped), copy threads 0.3-0.7%" (runs 3, 4) [log] | fences no longer wait for them (GPU order implies the copies, which are recorded before). The submit-time wait stays while copies exist |
| O2 | End-of-pipe readback work | `TextureCache::ProcessDownloadImages` (`texture_cache.cpp:303-380`): W2 targeted waits, `Finish()` when the readback ring is full (`:370`, `:375`) | stage B | image readbacks published by the CPU | "end of pipe 0.38-0.72 ms" a frame (run 3 sections) [log] | unchanged (images), smaller with GPU copies into guest memory (U5) |
| O3 | **Write faults** | page protection (`page_manager.cpp`, `region_manager.h:238-260` counts them), handler in `gow3gpu_handle_fault` (`gpu/shim/gow3gpu.cpp:284`) from `src/probe.c:314-316` | **game threads** | the arena is a copy | **44-49k/s, ~760 a frame**; handler 18 ms/s; slow frames' game excess +5.32 ms (run 5), +8.93 ms with uffd (run 6) [log][doc] | unchanged by fences; gone for in-place ranges (U5) |
| O4 | Downloads | G6 | game threads | copy | ~0 | gone for in-place ranges |
| O5 | **Frames-ahead bound** | `vk_presenter.cpp:680-691` (`frames_queued=1`); `GetRenderFrame` fence wait `:1139-1170` | stage B (`flip_present` section) | pacing: stage B at most one frame ahead of the GPU | "GPU ticks 7.0-18.6%" blocked; `flip_present` 1.24 us per packet = ~1.75 ms a frame, 16-42 ms in slow frames (run 3) [log] | unchanged. This is where the GPU limit is felt today; it is correct back-pressure |
| O6 | Recorder sync | `SyncRecording` (`vk_scheduler.cpp:506`) | stage B | threaded recording | "blocked: recorder 0.1-0.2%" [log] | unchanged |

### 2.4 PS4 memory types and the game's cache actions as sync points

**Type values** (shadPS4 upstream `src/core/libraries/kernel/memory.h`, `MemoryTypes` enum) [web]:

| Value | Name | CPU side | GPU side (bus) | Native sync rule [web + inferred] |
|---|---|---|---|---|
| 0 | `ORBIS_KERNEL_WB_ONION` | write-back, cached | Onion: coherent with the CPU caches; the low-bandwidth path (~10-20 GB/s quoted) [web] | CPU writes are seen by GPU reads that bypass or invalidate GPU caches (Onion+, "volatile" L2 lines). GPU writes reach the CPU once the GPU's L2 lines are written back |
| 3 | `ORBIS_KERNEL_WC_GARLIC` | write-combined, uncached reads | Garlic: the full-bandwidth path (176 GB/s), not coherent with the CPU caches [web] | CPU writes reach memory once the WC buffers drain. GPU writes are CPU-visible only after an explicit L2 writeback. CPU reads are slow (uncached) |
| 10 | `ORBIS_KERNEL_WB_GARLIC` | write-back | Garlic, not snooped | the CPU must keep its caches clean for the GPU [inferred]; rarely used |

The vendored tree has no `kernel/memory.h`. Our runtime takes the values as plain ints:
- `direct_allocate` accepts 0-10 (`src/runtime_memory.c:663-683`) and stores them as `Block.type`.
- `pool_commit(type)` stores them per mapping as `Vma.type` (`:1019-1046`).
- `sceKernelGetDirectMemoryType` reports them (`:863`).
- The GPU side never reads them [code].
- Bloodborne does: Garlic (type 3) gets a VRAM copy, Onion stays in place, and on an integrated GPU everything stays in
  place (`bloodborne_pc buffer_cache.cpp:406-408`, `:474-480`, `:1943-1967`, `:3357`) [BB].

**MediEvil's layout:**
- Deck boot: framebuffer pool **512 MiB as type 3**, then **5,056 MiB as one type-0 allocation**, given to
  `sceKernelMemoryPoolExpand` (`deck-native-4.log:77-80`) [log].
- PC at 4K: 512 + 6,592 MiB (fix-inventory "Caveats") [doc].
- Pool pages are committed per `sceKernelMemoryPoolCommit(type)`. The per-page types are recorded but were never counted.

**The game's cache actions, as the port decodes them** [code]:

| Packet | Field that carries the cache action | Port today |
|---|---|---|
| EVENT_WRITE_EOP | `event_type` (`pm4_cmds.h:438-445`): `CacheFlushAndInvTsEvent` 20 (flush CB/DB, write back and invalidate L2, then the label) vs `BottomOfPipeTs` 40 (label only) [inferred: GCN convention] | ignored: only `data_sel` / `int_sel` are used (`pm4_cmds.h:466-510`, `liverpool.cpp:267-290`) |
| EVENT_WRITE_EOS | `event_type` (CS_DONE 47 / PS_DONE 48) | ignored |
| RELEASE_MEM (compute) | `tc_action_ena`, `tc_wb_action_ena`, `tcl1__action_ena`, `tc_vol_action_ena`, `tcl1_vol_action_ena`, `cache_policy` (`pm4_cmds.h:910-925`) | ignored (`liverpool.cpp:1732-1750`) |
| ACQUIRE_MEM | `cp_coher_cntl`, base, size (`pm4_cmds.h:514-521`): which caches to invalidate / write back over which range | no-op (`liverpool.cpp:1335-1338`, `:1608-1610`) |
| EVENT_WRITE | `CacheFlush` 6, `CacheFlushAndInvEvent` 22, `FlushAndInvCbDataTs` 45 ... (`pm4_cmds.h:358-401`); `inv_l2` bit (`:426`) | ignored except streamout and ZPASS (`liverpool.cpp:697-706`, `:1228-1254`) |
| SURFACE_SYNC (0x43) | (`pm4_opcodes.h:46`) | not handled: it would hit `UNREACHABLE` (`liverpool.cpp:1444-1446`), so MediEvil's graphics streams do not use it [code, inferred from no abort] |
| `sceGnmFlushGarlic` | — | stub (`gnmdriver.cpp:898-900`); not among MediEvil's 39 called Gnm imports (study-stage-b-native.md:27) [doc] |

**Why these matter.** On the PS4 a GPU write to Garlic memory reaches the CPU only after an L2 writeback. The game must
therefore put a flushing EOP or RELEASE_MEM (or an EVENT_WRITE flush) between a GPU write and its own CPU read [inferred].
That makes these packets the native "GPU data is in memory" points:
- **Under U5:** a GPU-ordered label after such a flush gives the native semantics exactly.
- **In the copy model:** they are where GPU-written ranges could be downloaded ahead of the CPU read, instead of on a
  fault. That is a candidate for the PC too.
- **ACQUIRE_MEM ranges** mark where the GPU is about to read memory the CPU wrote. They are candidate points for batching
  uploads.
- **The histogram of these fields** (5.4) tells which of them MediEvil actually uses.

## 3. Critical path

The Deck is near-balanced in the graveyard: stage B ~93%, GPU ~90% (runs 3-5) [log]. Long frames come from the game's own
frame (+5.3 ms mean excess), and the write faults stretch it (run 6) [doc].

| Wait | Thread | On the critical path? | Why |
|---|---|---|---|
| G1 submission lock | submit thread | **likely yes** [inferred] | if the game's next submission waits for GPU idle, the whole guest frame waits; Bloodborne's 101 vs 154 FPS |
| G2 equeue once a frame | RHI / render | **yes on the PC** (half of an 8.2 ms heavy frame, T-W3) [doc]; Deck unknown | the game's render side waits for the port's pipeline |
| G3 label spins | render thread | **was yes** (PC), now small | HZB remap; ~3 EOPs a frame |
| G4 flip / vblank | flip threads | pacing, not GPU | unchanged |
| G5 / G6 | game threads | no (stale readbacks, downloads ~0) | — |
| P1-P3 | stage A | no while stage A waits 47% for stage B | stage A has slack |
| P4, O1 | **stage B** | **yes** (stage B is the Deck's limit) | ~8-9% of stage B; GPU-ordered fences remove them |
| O3 write faults | **game threads** | **yes** | run 6 |
| O5 frames ahead | stage B | the GPU limit itself | correct back-pressure |

## 4. Cost model

Definitions for one fence f: `t_sig` = when it signals today; `t_gpu` = when the GPU has executed it (submission
completed, or a GPU timestamp); **L_f = t_gpu − t_sig** (≥ 0). For one guest wait i on fence f: `W_i` = how long it waits
today (0 if the fence was already signalled); `S_i` = the slack when it did not wait (time from `t_sig` to the guest's
call).

Extra wait under GPU-ordered fences:

```
extra_i = L_f              if W_i > 0      (the guest was already waiting: it now waits L longer)
extra_i = max(0, L_f − S_i) if W_i = 0
```

Per frame and per guest thread role r: `E_r = Σ_i extra_i` over the waits of r in the frame.

Frame-time change:

```
ΔT_frame ≈ max over critical roles r ( E_r )  +  ΔLock  −  G_stageB
ΔLock    = Σ over submissions that hit the lock of max(0, t_gpu(last tick of previous frame) − t_lock_wait_start) − W_lock_today
G_stageB = (WRITE_DATA waits + deferred-signal waits in drains, per frame) on stage B, counted only while stage B limits
```

Bounds and readings:
- **GPU-bound regime** (the Deck at 100% render scale): the GPU sets the frame rate. A guest wait on the GPU only turns
  queue time into guest wait, as long as at least one frame of work stays queued ahead of the GPU. Throughput then drops
  only when `E_r` exceeds that queue's slack (≈ one frame with `frames_queued=1`) [inferred].
- **CPU-bound regime** (render scale 67%, run 5: GPU ~90% with dips): every `E_r` on the critical role adds to the frame.
- Fences ≥ 1 frame old cost nothing extra: stage B passing frame N's flip implies frame N-1 is GPU-done
  (`vk_presenter.cpp:680-691`) [code].

**With today's numbers** (Deck, L ≈ 15.6 ms at the end of a frame):

| Case | ΔT per frame |
|---|---|
| No guest wait on the newest frame's fences, lock with overlap | **−0.6 to −1.3 ms** (WRITE_DATA 3.7% and host-copy fence waits 4-5% of a 16.7 ms stage B frame) [log, inferred] |
| One per-frame wait on the newest frame's EOP with slack S (PC T-W3: the guest already waits 3.8 ms, so S = 0 there) | +(15.6 − S), up to **+15.6 ms** (60 → ~31 FPS) |
| Submission lock honest, no overlap | Bloodborne: −34% FPS [BB]; on the Deck up to one GPU frame per frame [inferred] |
| Lock with overlap K=2, equeue wait on a fence ≥ 1 frame old | **0 to +3 ms** (submission granularity: 24-41 submits a frame [log] ≈ 0.4-0.7 ms; signal-thread wake; jitter) [inferred] |

## 5. Instrumentation plan (stats only)

Rules:
- Everything below is gated on `Gow3Stats::enabled` (`GOW3_FRAME_STATS=1`, `gpu/shim/gow3_toggles.h:171-175`) or its C twin
  in the runtime.
- Nothing changes behaviour or command streams, except the opt-in level 2 of 5.1.
- One new header `gpu/shim/gow3_fence_stats.h` (namespace `Gow3FenceStats`) holds the C++ counters. The runtime keeps its
  own counters in `src/runtime_memory.c` and exports one C function for the report.
- The report prints from the existing 5 s block in `videoout/driver.cpp` (after the "Frame stats" line, `:665-760`).
- Per-frame values come from the slow-frame / frame-sample lines where cheap.

### 5.1 Fence lag probe (signal → GPU done)

| Counter | Where | How |
|---|---|---|
| `NoteSignal(kind, tick)` with kinds `EopLabel`, `EopIrq`, `Eos`, `WriteData`, `Flip`, `GpuIdle`, `ComputeRelease` | `RunEventWriteEop` (`liverpool.cpp:267-290`): the tick is taken on stage B **before** `SignalAfterHostCopies`, and `SignalEop` stamps `t_sig` when it runs; `RunEventWriteEos` (`:436-466`); `RunWriteData` (`:380-424`); `SignalFlip` (`:326-349`); `Liverpool::Process` GpuIdle (`:193-200`); compute RELEASE_MEM (`:1732-1750`) | one entry `{kind, tick = scheduler.CurrentTick(), t_sig}` in a 4,096-entry ring (single writer per kind site, relaxed atomics) |
| `t_gpu` | thread `gow3:FenceProbe`, started only with stats: waits on the scheduler's timeline semaphore for the oldest pending tick (`vkWaitSemaphores`, 5 ms timeout), stamps every entry with `tick ≤ value` | **L = t_gpu − t_sig**, kept per kind as a 64-bucket log histogram (p50, p90, max per 5 s) |
| `t_sig_to_submit` | `Scheduler::SubmitExecution` (`vk_scheduler.cpp:707-740`) stamps when each tick is submitted | the share of L that is submission granularity (labels are visible to the GPU only once submitted) |
| Level 2 (opt-in `GOW3_FENCE_STATS=2`, never in timing runs) | a `vkCmdWriteTimestamp2` at each fence position + `VK_KHR_calibrated_timestamps` | exact GPU execution time instead of submission completion. Records a command per fence, so it is not stats-only |

Accuracy: level 1 measures up to the end of the submission that holds the fence, which is an upper bound. Overhead: ~3
EOPs, ~3 WRITE_DATA and 1 flip a frame (MediEvil) → <10 us a frame; the probe thread sleeps in the driver.

### 5.2 Guest consumption

| Counter | Where | What |
|---|---|---|
| `equeue[filter][ident][role]`: calls, waited (>20 us), wait ns total / max, timeouts, immediate returns, **slack** (`t_return − t_trigger` for immediate returns) | `sceKernelWaitEqueue` (`kernel/equeue.cpp:467-499`); `EqueueEvent` gets a trigger timestamp in `TriggerEvent` (`equeue.h`) | which events MediEvil waits for, on which thread, and how much slack it leaves (the `S_i` of section 4) |
| `submit_lock`: SubmitDone calls that armed it, guest waits (count, ns, max, per role), lock hold (`t_arm → ResetSubmissionLock`), tick at GpuIdle | `sceGnmSubmitDone` `gnmdriver.cpp:2355-2367`, `WaitGpuIdle` `:87-91`, `ResetSubmissionLock` `:81-85`; the GpuIdle tick also feeds 5.1 | `W_lock_today`; with 5.1, `ΔLock` |
| `spins[role]`: `usleep(0)` / `sched_yield` calls a second, time sampled 1 in 64 | the runtime's sleep / yield exports (where `GOW3_WAIT_SITES` hooks, `src/runtime_waitsites.c`), thread-local counters summed at the report | which thread spins and how much. The site and target come from a separate `GOW3_WAIT_SITES=1` run |
| thread role | set once per guest thread at creation from its name (`src/runtime_thread.c:313`): Game, Render, RHI, RHIFlip, RHIOffset, SubmitGPU, SubmitDone, TaskGraph, Other | a thread-local byte, read by the counters above |

### 5.3 Command-processor waits

| Counter | Where | What |
|---|---|---|
| WAIT_REG_MEM outcomes: VO label (count, drain ns, `WaitVoLabel` ns), pending-met, memory-met, waited (count, ns), top 4 addresses with the writer kind of the last pending fence | `liverpool.cpp:1350-1388`; compute `:1724-1730` | which P1 path costs stage A, and which waits a GPU-ordered fence would delay (cross-queue path c) |
| MEM_SEMAPHORE / REWIND / CE-DE spins: count, ns | `:1320-1348`, `:548-553`, compute `:1611-1622` | whether MediEvil uses them |
| Drains: already printed ("drains by reason", "stage A waits over 0.5%", `vk_rasterizer.cpp:958-983`) | — | split `WaitDeferredSignals` time inside drains from the drain itself (`liverpool.cpp:876`, `:194`) |

### 5.4 Cache actions histogram

| Counter | Where |
|---|---|
| EOP `event_type` × `int_sel` × `data_sel`; EOS `event_type` / command | `ProcessGraphics` cases `:1255-1273` (decode, before `RunInOrder`) |
| RELEASE_MEM cache bits (OR and counts per bit) | `:1732-1750` |
| ACQUIRE_MEM `cp_coher_cntl` (OR, counts per bit), base/size coverage (bytes a second) and the PS4 type of the range | `:1335-1338`, `:1608-1610` |
| EVENT_WRITE event types | `:1228-1254` |

Cost: a few hundred packets a frame at most, one increment each.

### 5.5 Fault map by PS4 memory type (the coordinator's addition)

| Counter | Where | What |
|---|---|---|
| `runtime_memory_type_at(va, &type, &prot, &phys, &kind)` | new, `src/runtime_memory.c`, lock-free like `runtime_memory_region` (`:1365-1424`, sequence count `vma_seq`) | the `Vma.type` / prot / phys of a guest address |
| write faults per bucket: Onion 0, Garlic-WC 3, Garlic-WB 10, other direct type, flexible, not found | `CountWriteFaults` (`buffer_cache/region_manager.h:238-260`), per counted page | faults/s per type |
| distinct pages per frame per bucket, and pages that faulted in the previous frame too | a u16 frame stamp per 16 KiB PS4 page, indexed by phys: (5,568 MiB + 1 GiB flexible) / 16 KiB = ~420k pages = 0.84 MB (PC 4K 7,104 MiB: ~1.1 MB); frame number from `Gow3Stats::gpu_frames` | "how many distinct pages a frame, how many every frame" (the open question of run 6) |
| committed bytes per type, and GPU-visible (prot & 0x30) bytes per type | `place()` / `drop_range()` for `KIND_DIRECT` (`src/runtime_memory.c:615-633`, `:604-614`), relaxed adds under the write lock already held | how much of each type exists; how large U5's chunks must be |
| buffer upload bytes and stream copies per type | `UploadCopies` (`buffer_cache.cpp:1388-1469`), `ObtainBuffer` stream path (`:1016-1043`) | which type the uploads come from |
| top 4 faulting guest instructions per type | `Gow3Stats::fault_rip` (already thread-local) | which game code writes them |

Overhead: one lock-free lookup (~50-100 ns) per fault against a 7-57 us fault; ~760 faults a frame → <0.1 ms a frame
spread over game threads.

### 5.6 Port waits (small additions)

- `flip_present` split: `frames_ahead_wait_ns` (`vk_presenter.cpp:689`), `render_frame_wait_ns` (`:1156`), the rest.
- Submissions per frame by caller (`Scheduler::Flush` / `Finish` callers, `std::source_location` as `GOW3_LONG_WAITS` already
  uses, `vk_scheduler.cpp:591-637`), top 5.
- The existing "GPU lag at frame start" histogram (`GOW3_BUFFER_STATS=1`, `vk_rasterizer.cpp:4128-4159`) goes under
  `GOW3_FRAME_STATS` too.

### 5.7 Printed lines (every 5 s, after "Frame stats")

```
Fence lag (5 s, signal -> GPU done, ms p50/p90/max, n): EOP label 15.1/16.4/31.2 (15), EOP irq ..., EOS ..., WRITE_DATA ..., flip ..., GPU idle ...; to submit p50 0.4
Guest fence waits (5 s): equeue GfxEop RHIThread 300 calls, 290 waited, 2.10 ms avg, slack p50 1.2 ms; Flip RHIFrameFlipThread ...; submit lock armed 300, waited 120 (SubmitGPU, 0.40 ms avg), held 3.1 ms avg; spins Render 0 41k/s ~1.1 ms/frame
CP waits (5 s): WAIT_REG_MEM 950: VO 300 (drain 2.1 ms, wait 0.3 ms), pending-met 350, memory-met 280, waited 20 (1.2 ms) top 0x... (EOP); compute 0; semaphores 0; drain signal waits 41 ms
Cache actions (5 s): EOP flush+inv 300 bottom 15; EOS cs_done 30; RELEASE_MEM -; ACQUIRE_MEM 900 coher 0x28c00000 (tc 900, k 900, icache 12) 2.1 GB covered (Garlic 95%); EVENT_WRITE cache_flush_inv 300
Fault map by PS4 type (5 s): Onion 44.1k/s (distinct 610/frame, 590 every frame) Garlic-WC 1.2k/s (...), flexible 0; committed Onion 1.21 GB, Garlic-WC 3.95 GB; uploads Onion 2.9 MB/frame
GPU-ordered estimate (5 s): +0.8 ms/frame (critical role RHI), lock +0.0 (K=2 would cover 300/300), stage B -0.9 ms
```

The last line evaluates section 4 from the window's own counters. It is an estimate, labelled as such.

## 6. Gate 2: what to run first, and the decision rule

1. One Deck run with the instrumentation:
   - setup: graveyard, the run-5 settings (67%) and the run-4 settings (100%), `GOW3_FRAME_STATS=1` only;
   - plus a separate run with `GOW3_WAIT_SITES=1`, to name the spinning sites and the equeue call sites;
   - each launch needs the user's go.
2. Read off, per 5 s window:
   - the fence lag per kind;
   - the slack and waits per role;
   - the lock hold vs the GPU lag;
   - the fault share per PS4 type;
   - the estimate line.
3. Decision:
   - **GO** for GPU-ordered fences (design U5 step 2) if the estimate is ≤ +1 ms a frame with an overlap of K=2, or if the
     critical-role waits are all on fences ≥ 1 frame old.
   - **Measure directly** (implement GPU-completion fences under the copy model behind a switch and A/B them) if the
     estimate falls between +1 and +4 ms.
   - **NO-GO** for U5 if it is > +4 ms a frame and the waits sit on the newest frame's fences with no slack. Instead: U2
     (direct arena writes) and the PC fault fix.
4. Independently: if ≥ 50% of the faults sit in one PS4 type, that type goes first in U5's step plan.

## Sources

- shadPS4 `MemoryTypes`: [src/core/libraries/kernel/memory.h](https://raw.githubusercontent.com/shadps4-emu/shadPS4/main/src/core/libraries/kernel/memory.h)
  (`ORBIS_KERNEL_WB_ONION = 0` write-back (Onion bus), `ORBIS_KERNEL_WC_GARLIC = 3` write-combining (Garlic bus),
  `ORBIS_KERNEL_WB_GARLIC = 10` write-back (Garlic bus)).
- Garlic / Onion / Onion+ and the volatile bit (Cerny interview, 2013, republished):
  [Mail & Guardian](https://mg.co.za/news/tech/2013-04-30-the-playstation-4-supercharged-pc-architecture/);
  [PC Watch on Garlic bandwidth](https://pc.watch.impress.co.jp/docs/column/kaigai/20131028_621178.html);
  [Beyond3D: Onion+ discussion](https://forum.beyond3d.com/threads/amd-kaveri-apu-features-the-onion-bus-like-the-playstation-4.54966/page-5).
  The exact Onion figures (10 vs 20 GB/s) are from 2013 press and forums, not from Sony.
- Bloodborne: `bloodborne_pc` `gnmdriver.cpp:85-175` (submission lock modes, `BB_SUBMIT_AHEAD`), `liverpool.cpp:473-476`
  (`BB_ASYNC_FENCES` 79 vs 83), `vk_rasterizer.cpp:294-355` (`WriteLabelOnGpu`), `:429-504` (signal thread, watchdog),
  `docs/CHANGES_2026-10-06.md:15-22`, `docs/parallel_gpu.md:549-578`, `docs/EMULATION_REMOVAL_PLAN.ru.md:101-108`.
