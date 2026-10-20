# Plan: stage B's work across threads (feasibility study)

Status: study, 2026-10-19. Nothing built. Parent: [START-HERE.md](START-HERE.md). Evidence: profile T-P3 (below) and a
read-only code map of stage B (a sub-agent's report, references checked where quoted). Related:
[plan-gpu-threading.md](plan-gpu-threading.md) (Bloodborne's items; its #8 is the Vulkan recorder, not stage B).

## Why

With real occlusion results (O2 / O3) the heavy view runs at 205-221 FPS with p99 ~5.5 ms while the camera is still,
but panning brings the lows down (T-O3c: 183 FPS, p99 9.8 ms; every slow frame has stage B 85-97% "busy"). The user
asked for the stage B split as the next step (2026-10-19).

## Profile T-P3 (2026-10-19 21:00-21:02, launched by Claude with the user's go; build `32eaea9`; `-Samples`; log `data/t-p3-stageb-profile.log`, samples in `research-data\samples\t-p3-stageb-profile`)

Settings of T-O3c; heavy view, mostly panning. `gow3:DrawRec`, 60,336 samples (whole run, menus included):

| Stage B time | Share | Where |
|---|---|---|
| End-of-pipe readback check waiting for host copies | **21.0%** | `RunEventWriteEop` (liverpool.cpp:273) → `TextureCache::ProcessDownloadImages` (texture_cache.cpp:236) → `Scheduler::WaitHostCopies` → `Gow3Copy::WaitAsync` / `SwitchToThread` |
| Flip waiting for the GPU to finish the previous frame | **15.7%** | `SubmitFlip` → `Presenter::PrepareFrame` → `Scheduler::Wait` → `vkWaitSemaphores` (one frame queued) |
| Idle (no packet) | 12.3% | `DrawPipe::Run` atomic wait |
| Runtime memory-map lock contention | 5.4% | `ResolveVertexBuffers` → `runtime_memory_clamp` (runtime_memory.c:420) → `RtlAcquireSRWLockShared` wait |
| Draw work (DrawRecord, BindResources, vertex buffers, descriptors, RecordData copies) | ~40% | spread, no single hotspot |

The draw pipe's "stage B busy" counts the handler's whole time, waits included: stage B's real draw work is ~40%,
and **~37% is two waits that have nothing to do with the split.**

## Stage B's per-draw path (code map; `vk_rasterizer.cpp` = R)

Stage A already picks the pipeline (`Draw`, R:922-928) and copies the constant ring; DrawPrep workers prepare pipelines,
flattened user data, sharps and vertex inputs (vk_draw_prep.cpp:381-484). Stage B (`RunDrawPacket` R:688-820 →
`DrawRecord` R:939-1145) then does, per draw: `BeforeRecord` (D2 queue, O3 submit); frame-start hooks (camera / object
motion, upscaler); `PrepareRenderState` (`FindImage` for targets); `BindResources` (R:1535-1666: `ObtainBuffer` per
buffer with uploads, texture lookups + `UpdateImage` + `Transit`, samplers); `BeginRendering` memo or
`BeginRenderingFull` (R:2732-3077); vertex / index resolve and emit; `FlushBarriers` when needed; object motion (palette
hash, index range); push descriptors + push constants (`Pipeline::BindResources`); dynamic state diff; `scheduler.
BeginRendering`; the draw `Record`; `ResetBindings` (barrier tracking) and `KickRecording`.

Order-dependent shared state (each draw depends on the ones before): the CPU-write queue, memory tracker bits /
`gpu_modified_ranges` / arenas / residency, upload `CopyBuffer`s (they end the render pass), the barrier tracker
(`IsBufferAccessed` / `AccessBuffer` / `FlushBarriers`), image layouts (`backing->state`) and per-draw `binding` flags on
shared `Image`s, the texture registry and metas (clear vs load), stage-B-local memos, the scheduler's render pass /
dynamic state / record chunks, the occlusion segments (render pass hook), `ConstantRing::Stamp`, per-frame camera /
motion / upscaler state. Pure per draw: register / user data install, dynamic-state values (not the diff), descriptor
skeletons, draw parameters, closure packing. Only primary command buffers exist (`vk_resource_pool.cpp:86`), one
recorder (`gow3:VkRecorder`), no render pass suspend / resume.

## Options

| | What | Movable share of stage B (estimate) | Risk |
|---|---|---|---|
| **W** | **Remove the two waits first** (not a split): W1 frames queued 2 (the flip stops waiting for the previous frame); W2 the end-of-pipe readback check waits only for host copies that read the memory its readbacks write (B1's source table, `WaitHostCopiesFor`), usually none | **up to ~37% of stage B's time** (measured) | low / medium |
| a | More per-draw work onto DrawPrep / stage A: dynamic-state values, descriptor skeletons, push-data base, draw offsets; object-motion hashing onto stage A (it reads guest memory at decode time) | ~10-20% (+~10% motion) | low (prepared-data fallback exists) |
| c | Two stages inside B: B1 resolves (all order-dependent state), B2 (or the VkRecorder itself, from POD records) builds closures, descriptors, dynamic state, motion, the draw | ~20-35%, ~1.25-1.5x headroom | medium (one ordered stream for B1's and B2's records) |
| b | Several stage-B threads over in-order segments (per-thread contexts, partitioned caches, predicted layouts at segment start, render pass resume, per-thread command pools) | near-linear in theory for pass-continuing draws, much less in practice (shared cache checks) | high, a very large refactor |

Neither port has a stage B split. The Bloodborne port parallelised the Vulkan recorder instead (segments of 64 KiB on
several recording threads, passes resumed with LOAD; heavy scene 95.5 → 115-124 FPS, "now limited by the draw
recording stage"), and lists (b)- and (c)-like ideas as not started. Its `BB_SECTIONS` per-section cycle counters
(`bloodborne_pc/gpu/shim/bbport_sections.h`) would give this port the per-section numbers (a) and (c) need. Fork/join
per draw does not pay here (the texture bind helper measured neutral: task 6,200 cycles vs 4,600 inline + join).

## Order

1. **W1** frames queued 2 (`medievil.ini` `frames_queued=2`; a setting, one launch). Watch input latency and the
   judder the user reported.
2. **W2** targeted end-of-pipe host-copy wait (a code change behind a switch; P2's skip made stutter worse because it
   dropped the wait, this keeps it where a copy could read the readback's memory).
3. Re-profile (T-P4). If stage B is still the panning limit: port `BB_SECTIONS`, then **(a)**, then **(c)**; **(b)**
   stays research.
