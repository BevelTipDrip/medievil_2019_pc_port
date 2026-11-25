# Study: Sleeping Village hotspots, flat vs wave (2026-11-25)

Static analysis only: no launches, no builds, no source edits. Data: `research-data/sv-hunt-20261125/`. Scripts and
outputs: `research-data/sv-hunt-20261125/analysis/` (on D: through the junction; list at the end).
Status words: everything here is **found** (from samples and the eboot), nothing is fixed or verified.

## Summary

- **The waves are CPU-chain bound, and the game thread is the first limit.** Run 2 (trolls, no patch): the game
  thread goes from 18% busy (0.81 ms per frame) to **100% busy (4.59 ms per frame)**. In the wave it never waits
  for 0.5 ms or more (wait trace), so the frame time is its CPU time. Run 4 (nav patch on, a fight with many
  effects; two captures): game thread 73-79% to **95-96%** (4.5-4.8 ms per frame). The render thread also climbs,
  from 27-33% to **65-73%** (+1.9-2.3 ms per frame), and stage B goes from 54-64% to 79-84%.
- **Biggest game-thread costs in the waves** (ms per frame; run 2 wave / run 4 waves):
  - PhysX scene queries + the scene lock: 1.05 / 0.95-0.98.
  - **UE4 task-graph queues and events: 0.65 / 1.10-1.29.** This is the largest increase in run 4, about +0.7.
  - Blueprint VM: 0.68 / 0.74-0.77.
  - Navigation path purge: **0.72 without the patch**, 0.02 with it.
  - Cascade particles: 0.05 / 0.18-0.24.
  - AI, Detour and movement code itself: only 0.13-0.16.
- **The lock-free loop at 0x6e7c22 is UE4's `TLockFreePointerListFIFOBase::Pop`**, the queue under every task-graph
  thread [high]. On the game thread it runs from `FNamedTaskThread::ProcessTasksNamedThread` /
  `ProcessTasksUntilIdle`: the game thread draining its own named queue (tick-function tasks, completions,
  subsequents). Its partners grow with it: the FIFO `Push`, `FStallingTaskQueue::Push`, the subsequents
  `PopAllAndClose` and FEvent wakes of worker threads. Per frame this tracks the number of tasks: ticking
  components, particle systems, animation. It is not the FEvent pool and not AI path queries.
- **Thread 6924 (run 4) / 23084 (run 2) is Wwise's `AK::EventManager`** (the audio thread). Its vector math at
  0x358dff9 is Wwise Spatial Audio geometry [med]: ray / polygon plane tests inside a recursive tree walk, next to
  the `AK::SpatialAudio::SetGeometry` code. Plus ATRAC9 decoding. It is off the critical path: 1.8-1.95 ms per
  frame in waves on its own thread.
- **Fenced atomic reads (`mfence; mov; mfence`)**: the image has 6,282 mfences in 3,305 groups, 2,914 of them
  AtomicReads. Samples inside fence groups take **0.9-1.2 ms per frame of the game thread** in waves and
  1.6-2.2 ms per frame across all threads. Candidates like the nav patch:
  1. `FRHIResource::FlushPendingDeletes`' NumRefs read (render thread, 0.17-0.21 ms per frame in waves): the same
     loop shape as the nav purge.
  2. The task-graph FIFO Pop / Push reads (0.5-0.8 ms per frame on the game thread, about 1.1 across threads):
     larger, but core code; it needs a careful review.
- **Waits (run 2)**: flat frames are paced by the flip. The game thread waits about 3.4 ms per frame in timed
  FEvent waits, the RHI thread about 2.7 ms on a GnmRHI present mutex, and the submit thread about 3.2 ms on the
  GPU equeue. In the wave the game thread never waits. The RHI and submit threads wait for work instead. Task-graph
  workers are woken about 3 times a frame instead of about 0.25.

## 1. Data and method

| Capture | Run | Exe | Windows (ETW s) | Frames (flip trace) | Game thread tid |
|---|---|---|---|---|---|
| cap1 = `sv-trolls-1125-1.etl` | run 2, no patch | dev | flat 1.5-5.0, wave 6.0-12.0 | 780 (mean 4.49 ms; one 146 ms hitch) / 1304 (4.60 ms) | 10324 |
| cap3 = `sv-navpatch-1125-1.etl` | run 4, nav patch | release | flat 6.0-12.5, wave 14.0-21.5 | 1528 (4.25 ms) / 1503 (4.99 ms) | 28860 |
| cap4 = `sv-navpatch-1125-2.etl` (newly exported) | run 4, nav patch | release | flat 112-118, wave 128-139.4 (01:39:40-01:39:52) | 1422 (4.22 ms) / 2441 (4.67 ms) | 28860 |

- Samples are 1 kHz with stacks, so 1 sample is 1 ms of CPU. "ms per frame" means samples in the window divided by
  the game frames (submit to submit, counted in the flip trace through its `wall` row).
- `cap1-frames.txt` lists only 660 of the 780 flat frames. The per-frame numbers here use the flip trace.
- **Guest stacks are one frame deep.** The eboot has no unwind data in the samples' walk, so a pure-guest sample has
  only its leaf. Samples in runtime or system code carry the runtime frames, then one guest return address.
  Costs are therefore **exclusive** (self time) per function. Callers come from the static call graph, not from
  samples. Inclusive "how much of the frame is NPC logic" cannot be measured from these captures; section 4
  estimates it by code area.
- Samples without stack records (about 0.5 ms per frame of the cap1 wave game thread) now use the sample's IP.
  They are mostly guest code.
- **Code areas.** UE4 is linked module by module, so neighbours share a source file or module. Areas come from:
  - 1,793 UClasses found from the 7,688 `GetPrivateStaticClassBody` (0x9b8cd0) call sites, each with:
    - package, name, constructor, primary vtable, super class;
    - 11,756 virtuals attributed to the class that introduces or overrides them.
  - 4,730 source-path strings (PhysX asserts, ICU, UE4 `__FILE__`).
  - Hand-checked ranges: TaskGraph, ScriptCore, Renderer, RHI, GnmRHI, Detour, Wwise.

  Area labels between two different classes are marked `?A|B` in the raw outputs and are weak.
- Confidence: **[high]** = code shape plus strings, vtables or constants match UE 4.21; **[med]** = strong context;
  **[low]** = neighbourhood only.

## 2. Which thread is which

Run 2 names come from the wait trace (tid to guest thread name). Run 4 has no wait trace: each thread's
guest-function profile was matched to run 2's named threads (cosine similarity, in brackets). Task-graph workers of
one priority class are interchangeable.

| UE4 / port thread | run 2 tid | run 4 tid (match) | Role |
|---|---|---|---|
| `main` | 10324 | 28860 (0.53, next best 0.33) | **game thread** |
| `RenderThread 1` | 20984 | 25836 (0.89) | rendering thread |
| `RHIThread` | 2144 | 15812 (0.90) | RHI thread (labelled "stageA:2144" in `cap1-frames.txt`: wrong, it is guest RHI code) |
| `AK::EventManager` | 23084 | 6924 (0.99) | Wwise audio thread (DSP, spatial audio, ATRAC9 decode) |
| `TaskGraphThreadNP 0-3` / `HP 4-7` / `BP 8-11` | 26732, 17972, 20588, 29880 / 14916, 5704, 27964, 19388 / 18968, 30156, 15860, 10084 | 20652, 18876, 23868, 7192 / 28008, 19200, 9652, 19848 / 15136, 10472, 10996, 2440 | task-graph workers: normal, high and background priority |
| `SubmitGPUCommandsAsyncTaskThread` | 26060 | 20516 (1.00) | PS4 RHI GPU submission (waits on the GPU equeue) |
| `AudioThread` | 25560 | 17284 (0.99) | UE4 audio thread |
| `PoolThread 0-15`, `FImgMediaSchedulerThread 0-7`, `RHIFrameOffsetThread`, `SceFios*`, `Toolkit::NP::*` | (wait trace) | | mostly idle |
| host: stage B (`DrawPipe::Run`) | 26376 | 21452 | |
| host: VkRecorder | 27092 | 7160 | |
| host: stage A (`Liverpool::ProcessGraphics`) | 22800 | 27592 | |
| host: 9 draw-prep workers | 16088 ... | 4132 ... | |
| host: NVIDIA driver thread | 5236 | 27920 (probably) | `nvoglv64` only |

### Busy per thread (% of wall time; ms per frame)

| Thread | cap1 flat | cap1 wave | cap3 flat | cap3 wave | cap4 flat | cap4 wave |
|---|---|---|---|---|---|---|
| game (`main`) | 18% / 0.81 | **100% / 4.59** | 79% / 3.35 | **95% / 4.76** | 73% / 3.07 | **96% / 4.49** |
| stage B | 57% / 2.56 | 61% / 2.82 | 64% / 2.74 | 84% / 4.17 | 54% / 2.29 | 79% / 3.69 |
| VkRecorder | 55% / 2.48 | 61% / 2.79 | 65% / 2.76 | 81% / 4.06 | 55% / 2.31 | 78% / 3.62 |
| RenderThread 1 | 23% / 1.01 | 35% / 1.63 | 33% / 1.38 | **73% / 3.63** | 27% / 1.15 | **65% / 3.01** |
| stage A | 39% / 1.76 | 42% / 1.93 | 42% / 1.80 | 55% / 2.76 | 38% / 1.61 | 53% / 2.47 |
| AK::EventManager | 3% / 0.12 | 39% / 1.81 | 37% / 1.59 | 39% / 1.95 | 26% / 1.09 | 39% / 1.83 |
| RHIThread | 19% / 0.83 | 19% / 0.86 | 20% / 0.84 | 22% / 1.08 | 19% / 0.80 | 23% / 1.07 |
| all TaskGraph workers (sum) | 1.05 ms | 2.94 ms | 2.36 ms | 6.23 ms | | |

The two kinds of wave:

- **Run 2 (trolls, no patch): game-thread bound.** Only the game thread changes.
- **Run 4 (a fight with effects, nav patch on): everything rises together.** The game thread is still the
  busiest, with the render thread, stage B and the recorder close behind.

## 3. Game thread

### By area (ms per frame; exclusive)

| Area | cap1 flat | cap1 wave | cap3 flat | cap3 wave | cap4 flat | cap4 wave |
|---|---|---|---|---|---|---|
| PhysX (scene queries + scene lock) | 0.10 | **1.05** | 0.85 | **0.95** | 0.85 | **0.98** |
| Task-graph queues / events (Core) | 0.06 | 0.65 | 0.52 | **1.29** | 0.43 | **1.10** |
| Blueprint VM (ScriptCore, FindFunctionByName) | 0.15 | 0.68 | 0.65 | 0.77 | 0.62 | 0.74 |
| Navigation path purge | 0.06 | **0.72** | 0.01 | 0.02 | 0.06 | 0.02 |
| Engine, other | 0.13 | 0.41 | 0.36 | 0.42 | 0.34 | 0.44 |
| Core / CoreUObject, other (incl. host mutex calls) | 0.09 | 0.29 | 0.22 | 0.28 | 0.19 | 0.25 |
| Particles (Cascade) | 0.00 | 0.05 | 0.04 | **0.24** | 0.02 | **0.18** |
| AI / navigation (Detour) / movement | 0.03 | 0.13 | 0.12 | 0.16 | 0.14 | 0.15 |
| Animation / skeletal mesh | 0.01 | 0.08 | 0.08 | 0.08 | 0.06 | 0.09 |
| Tick dispatch (TickTaskManager) | 0.02 | 0.07 | 0.08 | 0.09 | 0.07 | 0.09 |
| Audio (AkAudio component code) | 0.01 | 0.08 | 0.09 | 0.11 | 0.06 | 0.11 |
| UI (Slate / UMG) | 0.09 | 0.10 | 0.11 | 0.10 | 0.10 | 0.10 |
| Host only (runtime / kernel, no guest frame) | 0.01 | 0.18 | 0.11 | 0.11 | 0.01 | 0.11 |
| **Total** | **0.81** | **4.59** | **3.35** | **4.76** | **3.07** | **4.49** |

### Hottest functions (ms per frame)

Exclusive cost, plus `(rt)` for time inside the runtime called from that guest function.

| Guest fn | Identification [conf] | cap1 flat / wave | cap3 flat / wave | cap4 flat / wave |
|---|---|---|---|---|
| 0x2c94b30 | `ANavigationData::TickActor`, `PurgeUnusedPaths` inlined [high] | 0.06 / **0.72** | 0.01 / 0.02 | 0.06 / 0.02 |
| 0x6e7bf0 | `TLockFreePointerListFIFOBase<FBaseGraphTask,64>::Pop` [high] | 0.02 / 0.20 | 0.15 / **0.41** | 0.14 / **0.36** |
| 0x6ea260 | `TLockFreePointerListFIFOBase<...>::Push` [high] | 0.01 / 0.11 | 0.09 / 0.23 | 0.07 / 0.19 |
| 0x720480 (rt) | FEvent (pthread) Trigger, futex wake: waking workers [med] | 0.01 / 0.09 | 0.05 / 0.14 | 0.04 / 0.11 |
| 0x6ea0c0 | `FStallingTaskQueue<FBaseGraphTask,64,2>::Push` [high] | 0.01 / 0.05 | 0.05 / 0.13 | 0.03 / 0.11 |
| 0x6f4380 | `TClosableLockFreePointerListUnorderedSingleConsumer::PopAllAndClose` (subsequents) [high] | 0.00 / 0.06 | 0.06 / 0.11 | 0.04 / 0.10 |
| 0x6e9c50 | `FNamedTaskThread::ProcessTasksUntilIdle` (ProcessTasksNamedThread inlined) [high] | 0.01 / 0.05 | 0.03 / 0.08 | 0.03 / 0.07 |
| 0x6e8c00 | `FTaskGraphImplementation::WaitUntilTasksComplete` (IsComplete polls) [med-high] | 0.00 / 0.03 | 0.03 / 0.05 | 0.02 / 0.04 |
| 0x6d3730 | `FGraphEvent::DispatchSubsequents` (67 callers = `TGraphTask<T>::ExecuteTask`s) [high] | 0.00 / 0.01 | 0.01 / 0.04 | 0.02 / 0.04 |
| 0x312ad40 | PhysX `Sq` AABB-tree ray / sweep traversal (2 KB node stack, per-hit callback) [med] | 0.02 / 0.12 | 0.09 / 0.13 | 0.13 / 0.14 |
| 0x2ff14d0, 0x2ff9c80, 0x2ff0c00 | PhysX GeomUtils top-level query tests (sweep / overlap), before `convex/GuConvexMesh.cpp` [low] | 0.01 / 0.18 | 0.16 / 0.16 | 0.15 / 0.18 |
| 0x306c760, 0x306e710, 0x306f500, 0x3082560 | PhysX `GuBV4` triangle-mesh queries (between `GuBV4.cpp` and `GuTriangleMesh.cpp`) [med] | 0.01 / 0.10 | 0.10 / 0.10 | 0.08 / 0.10 |
| 0x319c9c0 (rt) | PhysX `shdfnd::MutexImpl::lock`: the scene lock taken per query (host mutex) [med] | 0.01 / 0.10 | 0.04 / 0.04 | 0.04 / 0.03 |
| 0xad51f0 | `UObject::execLocalVariable` (string "Attempted to access missing local variable") [high] | 0.02 / 0.11 | 0.11 / 0.13 | 0.09 / 0.10 |
| 0xad3210 (+rt) | ScriptCore: script function call that zeroes its locals (memset, runtime precise write) [low] | 0.03 / 0.17 | 0.14 / 0.17 | 0.14 / 0.15 |
| 0x9c5610 (+rt) | `UClass::FindFunctionByName`, recursive into the super class, FRWLock read lock = host rwlock per call [med] | 0.00 / 0.08 | 0.07 / 0.08 | 0.06 / 0.09 |
| 0x2334a70, 0x23367b0 | `UParticleSystemComponent::TickComponent` (vtable slot 100 = `UActorComponent::TickComponent`) [high]; helper with 51 callers [low] | 0.00 / 0.02 | 0.01 / 0.10 | 0.01 / 0.08 |
| 0x2775040 | `TGraphTask<FTickFunctionTask>::ExecuteTask` [med] | 0.00 / 0.03 | 0.02 / 0.02 | 0.02 / 0.03 |
| 0x2c57ab0 ... | Detour (`dtDistancePtSegSqr2D` at 0x2c33080 [high]; region 0x2c25000-0x2c8ea90 [med]) | 0.01 / 0.08 | | |

### What the lock-free loop at 0x6e7c22 is, and who calls it

- 0x6e7bf0 is the Michael-Scott FIFO pop of `TLockFreePointerListFIFOBase` [high]:
  - `Head` sits at +0x40 and `Tail` at +0x88, each behind a 64-byte contention pad.
  - The link index is 26 bits (0x3ffffff).
  - Link pages hold 16,384 links (index & 0x3fff, >> 14).
  - Links are 24-byte `FIndexedLockFreeLink`s.
  - `FPlatformProcess::Sleep(0.001f)` is called on ABA-counter wrap (0x8da6e0).
  - It ends with `FLockFreeLinkPolicy::FreeLockFreeLink` (0x6dbd20, allocator global 0x580fd40).
  - Each pass of the retry loop reads Head, Tail, Head->Next and Head again, each between two `mfence`s: 8 fences
    per pass, even when the queue is empty.
- Its 8 call sites (static call graph), all in the task graph:
  - `FStallingTaskQueue::Pop` (0x6e7aa0), from `FNamedTaskThread::ProcessTasksNamedThread` (0x6e9fb0, vtable
    0x4ed97d0 slot 7).
  - `FNamedTaskThread::ProcessTasksUntilIdle` (0x6e9c50, slot 8).
  - `FTaskGraphImplementation::FindWork` (0x6d3350), from `FTaskThreadAnyThread::ProcessTasks` (0x6e97b0, vtable
    0x4ed9670).
  - The vtables follow FRunnable's order, then the task-thread virtuals: `ProcessTasksUntilQuit`,
    `ProcessTasksUntilIdle`, `EnqueueFromThisThread`, `RequestQuit`, `EnqueueFromOtherThread`, `WakeUp`,
    `IsProcessingTasks`, `Tick` [high].
- **On the game thread**, the pops come from the named-thread path: the game thread processing its own queue while
  it waits for tick groups and task completions. Every game-thread task popped costs one pass. Each drain ends with
  empty pops on both priorities.
- Next to it on the game thread:
  - FIFO `Push` and `FStallingTaskQueue::Push`: queueing tasks to workers and named threads.
  - `PopAllAndClose` on each completed task's subsequents list.
  - `WaitUntilTasksComplete`'s IsComplete polls.
  - Worker wake-ups through `FEvent::Trigger`.
- So the cost tracks **tasks per frame**:
  - tick functions run as `FTickFunctionTask`s;
  - particle-system async tick and finalize tasks;
  - parallel animation evaluate and complete tasks;
  - their completions dispatched back to the game thread.
- Fights add ticking components and effects, and the task-graph share of the game thread doubles
  (+0.6-0.8 ms per frame in run 4).
- It is **not** the FEvent pool (a LIFO `TLockFreePointerListUnordered`). It is **not** AI async path queries:
  4.21 runs those as one task per frame.

## 4. How much of the wave is NPC-related (estimate)

Exclusive costs only (section 1). Grouped by what drives them:

| Run 2 wave minus flat (+3.78 ms per frame on the game thread) | ms per frame | Driven by NPCs? |
|---|---|---|
| Navigation path purge (`ActivePaths` of the nav-data actor) | +0.66 | **directly** (paths of AI move requests) |
| AI module, Detour, movement components, animation | +0.18 | directly |
| PhysX scene queries + scene lock | +0.95 | **mostly, inferred**: character-movement capsule sweeps, AI and Blueprint traces; the per-query host mutex adds to it |
| Blueprint VM | +0.53 | mostly, inferred (enemy Blueprints run per tick) |
| Task-graph queues / events | +0.58 | mostly, inferred (per ticking component / task) |
| Engine other, Core other, host-only, tick dispatch | +0.69 | mixed |
| Particles, audio, UI | +0.11 | effects / audio |

- In run 2 about **0.85 ms per frame is NPC code itself** and about **2.1 ms is work NPCs cause in shared systems**
  (queries, scripts, tasks). That is roughly 75-80% of the wave's extra game-thread time.
- Rendering setup on the game thread is small: no Renderer or RHI code is sampled on it, and the Engine
  scene-component code shows only tenths.
- Run 4 (nav patch on) has a smaller game-thread delta (+1.4 ms):
  - task graph +0.7-0.8;
  - particles +0.16-0.2;
  - PhysX, Blueprint and Engine about +0.1 each.
- Run 4 also has a large **render-thread** rise (+1.9-2.3 ms per frame):
  - Renderer / RHI / GnmRHI +0.8-1.0 (visibility, mesh batches, `FRHIResource` releases);
  - Cascade render-side +0.27-0.36;
  - Core +0.26-0.34;
  - task graph +0.19-0.27.

  Those fights are effects-heavy as much as NPC-heavy.

**Scaling with frame rate.**

- Almost all of this is **per frame**: ticks, the queries ticks make, Blueprint ticks, one task per tick function,
  per-frame render work. A frame costs about the same at any cap, and CPU per second grows linearly with FPS.
- At the 237 cap (4.22 ms) a wave needs 4.5-4.8 ms on the game thread, so it cannot hold. It settles at about
  210-220 FPS, which is the observed wave.
- A cap near 200 FPS (5.0 ms) would have about 0.2-0.5 ms of headroom in these waves.
- The nav purge's per-frame cost grows with `ActivePaths`, not with FPS. Its per-second cost still scales with FPS.
- The task-graph overhead per task is fixed. It mattered little at the console's 30 FPS and is now a real part of a
  4.2 ms frame.

## 5. Other threads

- **RenderThread** (run 4 waves +1.9-2.3 ms per frame):
  - Renderer region (0x10adab0-0x167b3c0, the `r.*` scene cvars) [med], e.g.:
    - 0x158ef60, a 20 KB function next to `r.DoLazyStaticMeshUpdate`, `vr.RoundRobinOcclusion`,
      `r.TemporalAAFilterSize`: SceneVisibility.cpp, likely `ComputeViewVisibility` [low];
    - 0x14e9e10 [low].
  - RHI / RenderCore (0xf40000-0x1060000) [med]:
    - **0xf7c010 = `FRHIResource::FlushPendingDeletes`** [high]: 0.03 to **0.21** ms per frame in run 4 waves, see
      section 6;
    - 0x418b80 = `FRHIResource::Release` (out of line, 654 callers: lock dec, then a lock-free LIFO push onto
      PendingDeletes) [high].
  - Cascade render-side (ParticleModule / ParticleSystemRender region) +0.27-0.36.
  - Its own named-queue pops (0x6e7bf0, 0.19 ms per frame).
  - GnmRHI command context (0x66c0c0-0x6c5000) [med].
- **AK::EventManager** (Wwise):
  - 0x358dfc0 / 0x358d790 (recursive) / 0x3585ce0, 0x358e880, 0x358f5c0: 0.5-0.6 ms per frame in waves.
    0x358dfc0 transforms a point by a 3x3 basis and tests it against up to 4 edge planes with `vmovmskps` masks
    (ray against convex polygon); its caller 0x358d790 recurses (tree walk). The only nearby string is
    `AK::SpatialAudio::SetGeometry - Triangle %i formed by vertices...` at 0x3585e90 [med]. So this is **Wwise
    Spatial Audio geometry ray casting** (reflections / diffraction), not animation, particles or crowd avoidance.
  - 0x3516790 calls the runtime's ATRAC9 decoder (`Dct4`, `ReadHuffmanValue`, `PeekInt`): the Wwise ATRAC9 source
    [med-high], 0.2-0.3 ms per frame.
  - In run 2 it goes 3% to 39% in the troll fight. In run 4 it is busy in both windows (1.1-1.95 ms per frame).
  - It is not in the CPU chain; it costs a core.
- **Task-graph workers** (sum, cap3): 2.4 to 6.2 ms per frame:
  - Renderer / RHI (parallel command-list tasks) 0.7 to 2.0;
  - their own queue traffic 0.3 to 1.1;
  - PhysX 0.6 to 0.9;
  - Cascade 0.08 to 0.75;
  - animation 0.26.
- **RHIThread**: flat or nearly flat. GnmRHI present and submission plus host calls.

## 6. Fenced atomic reads (`mfence; load; mfence`)

**Inventory (01.02, `mfence_scan.py`).**

- 6,282 mfences in 3,305 groups:
  - **2,914 AtomicReads** (`mfence; mov/cmp r, [m]; mfence`, sometimes with scheduled instructions in between);
  - 351 lone barriers (`FPlatformMisc::MemoryBarrier`);
  - 37 store + fence.
- 492 groups are `mfence; mov r32, [reg+8]; mfence` (refcount reads: `TSharedPtr` / `TWeakPtr` controllers,
  `FRHIResource::NumRefs`).
- The nav patch's exact 9 bytes `0faef08b40080faef0` occur **138 times** (the card's number).
- Samples whose leaf falls inside a fence group (to 6 bytes after the last fence, where a retiring fence's samples
  land):

| ms per frame in fence groups | cap1 flat | cap1 wave | cap3 flat | cap3 wave | cap4 flat | cap4 wave |
|---|---|---|---|---|---|---|
| game thread | 0.10 | **1.21** (0.72 purge) | 0.42 | **1.01** | 0.40 | **0.88** |
| all threads | 0.27 | 1.62 | 0.72 | 2.25 | 0.64 | 1.87 |

**Hot groups** (all threads; full list in `analysis/mfence-sites.txt`):

| Site | Function | cap1 wave | cap3 wave | cap4 wave | Threads |
|---|---|---|---|---|---|
| 0x2c94b9f | nav purge refcount read (patched in run 4) | 0.72 | 0.02 | 0.02 | game |
| 0x6e7c22 | FIFO Pop: Head, Tail | 0.17 | 0.42 | 0.32 | game, render, workers |
| 0x6e7c72 | FIFO Pop: Head->Next, Head again | 0.18 | 0.38 | 0.34 | game, render, workers |
| 0x6ea303, 0x6ea2c2 | FIFO Push: Tail->Next, Tail | 0.17 | 0.39 | 0.32 | game, workers |
| **0xf7c1d8** | `FRHIResource::FlushPendingDeletes`: NumRefs read per pending resource | 0.04 | **0.21** | **0.17** | render |
| 0x6ea0cd, 0x6ea0e0 | `FStallingTaskQueue::Push`: MasterState read (compiler-scheduled between the fences) | 0.07 | 0.20 | 0.16 | game, workers |
| 0x6f43c0 | `PopAllAndClose`: subsequents head read before its CAS | 0.04 | 0.07 | 0.06 | game, RHI |
| 0x418c20 (+ inlined copies at `[r+0x48]` in GnmRHI / RHI: 0x6a5b70, 0x671840, 0xf88e90, 0x69ca10) | `FRHIResource::Release` to PendingDeletes LIFO head read before its CAS | 0.02 | 0.15 | 0.12 | render, workers |
| 0x6e9cc0 | `ProcessTasksUntilIdle`: MasterState read | 0.04 | 0.05 | 0.05 | game |
| 0x6e8dda | `WaitUntilTasksComplete`: IsComplete poll (`SubsequentList` closed bit 26) | 0.02 | 0.05 | 0.04 | game |

**Candidates in the nav patch's sense: a read where the fences order nothing.** For each one below:

- the load is one aligned access, atomic on x86;
- x86-TSO already keeps load-to-load order;
- the decision that matters is made by a later `lock cmpxchg` (a full barrier) or by a re-check.

A StoreLoad fence would only matter for a Dekker-style "store my flag, then read yours" pattern; the notes say why
none applies. No code or patch is proposed here; each needs its own card and A/B.

1. **0xf7c1d8, `FRHIResource::FlushPendingDeletes`** [high identification; best candidate].
   - Same shape as the nav purge: a loop over a popped `TArray<FRHIResource*>`, one scattered `NumRefs` (+8) load
     per element between two fences.
   - The loop's only stores are `CurrentlyDeleting`, which nothing reads concurrently in a way the fence would
     order, and `MarkedForDelete = 0`, followed by its own `lock or [rsp],0` barrier.
   - The list was taken with a locked PopAll before the loop.
   - The "resurrected by a cache" race that Epic's comment accepts is not closed by these fences either.
   - Render thread only. It rises with resource churn: 0.03 to 0.17-0.21 ms per frame in run 4 waves.
   - The 9-byte pattern `0faef08b47080faef0` occurs **20 times** in the image. Patch only this site, by address,
     like the nav card.
2. **TaskGraph FIFO Pop / Push** (0x6e7c22, 0x6e7c72, 0x6ea2c2, 0x6ea303) [high identification; largest gain;
   higher risk].
   - Between the locked CASes these are pure load sequences, so the Michael-Scott algorithm needs only load order
     and the CAS.
   - The task payload store before `Push` is published by the CAS, and TSO keeps store order.
   - Stall / wake goes through `MasterState` CASes, so the producer and consumer flags are both locked operations.
   - Up to 0.5-0.8 ms per frame of the game thread in run 4 waves and about 1.1 across threads, but the Head and
     Tail lines are shared between threads. Part of the cost is cache-line traffic the fences only expose.
   - The patterns occur 2-4 times each (other inlined copies).
   - Every thread depends on this code. Review it against the 4.21 source before any trial.
3. **Read-then-CAS or poll sites** (0x6f43c0, 0x6ea0cd/0x6ea0e0 MasterState, 0x6e9cc0, 0x6e8dda, 0x418c20 and its
   `[r+0x48]` copies) [med]: redundant for the same reasons. Small each (0.04-0.2 ms per frame).
4. **Not candidates**: lone `MemoryBarrier`s and store + fence groups (they order stores). Reads whose value
   gates a following plain store without a CAS, unless checked one by one.

## 7. Waits (run 2 wait trace, cap1 windows; ms of wait per game frame, waits per frame)

The wait trace covers every guest wait of 0.5 ms or more (`wait_study.py`; windows converted through the flip
trace's `wall` row and the ETL start 01:01:14.794).

| Thread | Site (function) | Flat | Wave |
|---|---|---|---|
| **main** (game) | cond_timedwait guest+0x70df3a (`FEvent::Wait` with timeout: frame-end sync with the render thread / RHI) | **3.36 ms (3.3 waits)** | **0.00** (no wait of 0.5 ms or more in the whole wave) |
| RHIThread | mutex_lock guest+0x6795a1 (fn 0x679560: GnmRHI present / end-of-viewport path, `r.VSync`, `sceVideoOutSetWindowModeMargins`) | 2.70 (1.0) | 0.00 |
| RHIThread | cond_wait 0x70e02c (`FEvent::Wait`: waiting for work) | 0.04 | 2.61 (1.0) |
| SubmitGPUCommandsAsyncTaskThread | equeue_wait 0x68af47 (fn 0x68acc0: "async submitCommandBuffers", GPU EOP) | 3.17 (1.0) | 0.00 |
| SubmitGPUCommandsAsyncTaskThread | cond_wait 0x70e02c (waiting for work) | 1.22 | 3.79 |
| RenderThread 1 | cond_wait 0x70e02c | 3.37 (1.1) | 2.84 (**2.1**) |
| TaskGraphThreadHP 4-7 | cond_wait 0x70e02c | 3.9-4.4 (0.25-2.0) | 2.0-3.8 (**1.6-3.3**) |
| TaskGraphThreadNP 0-2 | cond_wait 0x70e02c | 4.0-4.2 (1.0-1.2) | 3.8-3.9 (1.0-**3.0**) |
| AK::EventManager | eventflag_wait 0x3517baf | 4.36 (0.5) | 2.78 (1.1) |
| RHIFrameOffsetThread | equeue_wait 0x69eb0c (flip-status / vblank wait) | 4.39 (1.0) | 3.81 (2.6) |

- **Flat frames are flip-paced.** The submit thread waits on the GPU or flip, the RHI thread blocks in the present
  path behind it, and the game thread waits in timed waits at frame end.
- **In the wave the chain inverts.** The game thread never blocks, so the frame time is its CPU time. The RHI and
  submit threads now wait for work, and the render thread waits twice as often (for the game thread and its
  tasks).
- Workers are woken 3-10 times more often, which is the task traffic seen in section 3.
- Inside the wave, long and short frames show no difference in the game thread's waits: it has none.
- There is no wait trace for run 4.

## 8. Next steps (ideas; each launch needs the user's go, one change per launch)

1. A card for the `FlushPendingDeletes` NumRefs fences (section 6 item 1). Same safety argument as
   `nav-purge-nofence`, render thread only. Patch by address, check the original bytes. A/B in a run-4-type
   effects fight: the render thread's busy share, `0xf7c1d8` samples, lows.
2. Review the task-graph FIFO fence removal against the UE 4.21 `LockFreeList.h` / `TaskGraph.cpp` before any
   trial. It is the largest fence cost, but core code.
3. Fewer tasks rather than cheaper tasks:
   - tick batching or intervals for Blueprint actors and Cascade components (cvars or Blueprint data);
   - whether `r.GPUParticle` / Cascade async-tick settings change the task count. Check what 4.21 exposes in
     this build before proposing anything.
4. PhysX: the per-query scene lock is a host mutex (0x319c9c0). Its cost (0.03-0.10 ms per frame) is small next to
   the queries themselves. Callers of the queries need deeper stacks: a guest frame-pointer walk in a dev-build
   sampler would give inclusive costs, and that is what the NPC estimate in section 4 lacks.
5. Whether `ActivePaths` grows without bound in long fights (paths held until GC) is still not checked.

## Files

`research-data/sv-hunt-20261125/analysis/` (scripts read only `out/eboot.elf`, the exports' pickles and the
traces):

| File | What |
|---|---|
| `eimg102.py` | 01.02 eboot loader: image, FDE starts, direct call graph, RELATIVE relocations, import slots (cache `eimg102.pickle`) |
| `xrefs.py`, `edis.py`, `callers.py`, `vt.py`, `ctx.py` | rip-relative xrefs; disassembly with names / strings; callers; vtables around a slot; context (strings, neighbours) |
| `classmap.py`, `pathmap.py`, `classify.py`, `names.py` | UClass map (`GetPrivateStaticClassBody` sites, constructors, vtables, owned virtuals), source-path anchors, area / category labels, hand names with confidence |
| `parse_samples.py` | one pass over an xperf export, giving `capN-samples.pickle` (cap4 = `sv-navpatch-1125-2.etl` 112-118 s and 128-139.8 s; the CSVs were deleted, re-export with `etw_export.py`) |
| `profile.py`, `topfn.py`, `rollup.py`, `busy.py`, `hostonly.py`, `threadmap.py` | per-thread profiles, flat vs wave; thread names (`threads.json`) |
| `mfence_scan.py` | fence inventory plus sample join, giving `mfence-sites.txt` |
| `wait_study.py` | run 2 wait trace, flat vs wave, giving `wait-study.txt` |
| `cap*-profile.txt`, `cap*-busy.txt`, `rollups.txt`, `gt-topfn.txt`, `threadmap.txt`, `pathmap-coarse.txt` | outputs quoted above |
