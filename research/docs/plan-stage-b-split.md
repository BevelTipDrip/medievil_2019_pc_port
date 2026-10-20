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

## T-W1: frames queued 2 (2026-10-19 21:12-21:16, launched by Claude with the user's go; build `3b30fe9`; log `data/t-w1-frames-queued.log`)

Live A/B through the in-game menu (Performance → Frames queued), switched to 2 at ~21:14:44 (menu open
21:14:23-38). With 1 the presenter's frame-limit wait (`vk_presenter.cpp:553`) was **0.02-0.03 ms per 5 s** while
panning: the GPU kept up, so there was nothing to remove. With 2 it rose to 120-510 ms per 5 s right after the switch,
then 16-36 ms; panning 184-195 FPS, p99 9.7-10.4 ms (with 1: 190-201 FPS, p99 8.7-11.3 ms). The user: "No real
difference, it seemed to hurt if anything." **W1 rejected; 1 stays.** T-P3's 15.7% flip wait came from a GPU-heavy
stretch, not from panning. Note: the in-game menu's save rewrote `medievil.ini` with ~40 extra default keys (some
God of War leftovers: orb multipliers, `cheat_rage`); restored from the pre-launch backup, the game's copy kept as
`medievil.ini.game-saved-t-w1`.

Next: T-W2 (`GOW3_EOP_TARGETED=1`, live A/B with toggle bit 60).

## T-W2: targeted end-of-pipe wait, queries on (2026-10-19 21:17-21:21, launched by Claude with the user's go; build `3b30fe9`; log `data/t-w2-eop-targeted.log`)

`GOW3_EOP_TARGETED=1` with O2 / O3 (T-O3c settings). The end-of-pipe wait (`texture_cache.cpp:235`, ~72,000 waits and
~830 ms per 5 s in T-O3c) is **gone**; part of it moved to the end-of-shader event's full wait (`liverpool.cpp:347`:
16,700 waits, ~200 ms per 5 s, was ~0), net ~600 ms per 5 s less waiting. But stage B stays ~91% busy and the frame
rate did not move (still ~185 FPS, p99 ~7 ms; panning lows "still drop into the 80s/low 90s"). Likely: the old wait was
not idle time, the waiting thread ran pending copy jobs itself (`Gow3Copy::WaitAll` helps); that work now runs
elsewhere and stage B gains nothing. The user: "No real improvement to the 1% lows while panning … So less stable (and
lower during pans) than before the culling work."

**Decision (the user):** test runs go back to occlusion queries off (T-O1); O2 / O3 / W2 stay in the code, opt-in.
Next: check whether the fixes made for queries on (W2; D2b is always on) help with queries off (T-W2b, live A/B of
bit 60), then the stage B split (`BB_SECTIONS` first, then option a).

## T-W2b: queries off, W2 A/B (2026-10-19 21:33-21:38, launched by Claude with the user's go; build `f748f80`; log `data/t-w2b-queries-off.log`)

Queries off (the test default again), W2 on, off at 21:35:21 (`mask=1152921504606846976`), back on 21:37:13. No visible
difference either way. The user: "there's still some intermittent frametime dips that weren't here previously, we had
a frametime graph that was almost completely flat." At this spot (~980 draws a frame, ~210 FPS): **write-fault
downloads 11-18 a second** (`buffer_cache.cpp:288` Finish, up to ~4 ms each), 30-90 frames over 1.5x median per 5 s.
Across runs: T-O1 (flat; heavy view) 0.1/s mean, 0 of 101 windows above 5/s; T-D2d 1.5/s, 7 of 62; T-W2b 3.1/s, 33 of
140: the same direct writers (guest+0x20da4d0, 0xba9a49, 0x68e109), much busier at this spot. **W2 kept, opt-in (no
gain, no harm).**

## D3: downloads on a readback queue (built `5a5d73f`)

Port of the Bloodborne port's `GpuWriteTicks` / `FastReadback` (0.3): the newest GPU write's submission tick per
64 KiB page (shader writes and D2's arena uploads); a download waits for that submission only and copies on a second
queue (a compute family without graphics; arenas shared by both families), not behind the frames queued on the
graphics queue. `GOW3_READBACK_QUEUE=0` off; frame stats: "on the readback queue N". Test T-D3: queries off, the spot
of T-W2b.

## T-D3b: downloads on the readback queue (2026-10-19 22:14-22:21, launched by Claude with the user's go; build `1afe70a`; log `data/t-d3b-readback-queue.log`)

(First launch `t-d3-readback-queue` restarted: the user's monitor was off.) Queries off. D3 takes every write-fault
download ("on the readback queue" = the write-fault count, 9-20 a second in the light view), but each still costs
~2.8 ms: the wait is for the write's own submission (`buffer_cache.cpp:487`, `Scheduler::Wait(write_tick)`, max
3.3 ms), because the shader wrote those pages in the frame still being recorded. The worst light-view frames
(7.5-9.8 ms at a 4.6 ms median) each hold a GPU-tick wait of 1.7-5.5 ms and nothing else. The user: "Still some
fairly large dips I don't recall being there previously. They show up, even when I'm not panning."

Not new code: in light-view windows (900-1,050 draws), T-O1 (build `42a4943`, before the culling work) 4 windows,
0.9 downloads/s, stddev 0.27 ms; **T-D2d (same build) 3 windows, 10.3/s, stddev 0.70 ms**; T-W2b 18 windows, 8.6/s,
0.60 ms; T-D3b 63 windows, 15.0/s, 0.56 ms. The only change active with queries off since `42a4943` is D3 (and its
shared arenas); O2 / O3 do nothing without occlusion events, W2 is opt-in. The dips depend on the spot and moment
(T-O1 caught quiet windows). **D3 kept** (each wait now only for the write's submission).

Remaining fix: the Bloodborne port's **GPU write twins** (`BB_GPU_WRITE_TWINS`): a guest write into a page holding a
few GPU-written bytes does not wait at all; the guest's copy of those bytes is kept as a twin, the page becomes
CPU-modified, uploads keep the GPU's bytes where the guest's still match their twin (pages with at most 1 KiB of
GPU-written bytes there). Optional check first: `GOW3_READBACK_QUEUE=0` A/B at this spot (D3's shared arenas).

## T-T1: GPU write twins (2026-10-19 23:10-23:16, launched by Claude with the user's go; build `2a40666`; log `data/t-t1-twins.log`)

Queries off, twins on (default). **Light view (~1,150 draws): 213 FPS, stddev 0.06-0.14 ms, p99 4.8-5.2 ms**
(T-D3b ~210 FPS, ~0.56 ms, ~7 ms), write-fault downloads **0** and even 0 twinned in these windows: with twins the
pages stop bouncing between GPU- and CPU-owned, so the game's writes no longer fault. The user: "Much better … It's not
perfect, there's a few spikes here or there, especially panning past heavier areas." Those slow frames (2,544,
median 6.75 ms): stage B 92% busy, no waits, ~342 render passes ended (186 in samples): the draw recording ceiling.
**Twins kept, on by default.** Object motion is off in these runs (log: "Object motion: off"), so its per-draw cost
is not a lever here.

Next: the split (port `BB_SECTIONS` for per-section numbers, then option a), plus two items from T-P3: the runtime
memory-map lock contention in `ResolveVertexBuffers` → `runtime_memory_clamp` (~5% of stage B, waiting, not lookup
cost), and fewer render pass breaks (barrier batching, part of a).

## L2 / T-L2: region caches that answer during writes (2026-10-19 23:23-23:30, launched by Claude with the user's go; build `03c363b`; `-Samples`; log `data/t-l2-region-buckets.log`)

L1 (per-16 MiB-bucket generations, `0705e49`, reverted `bc60b8b`) re-applied, plus: `host_map` / `host_unmap` /
`host_keep_view` bump a range's buckets before its host mapping changes, so cached answers for other ranges stay
valid while a writer holds the lock (L1 refused all cached answers then, and writers hold the lock across the OS
calls). `GOW3_REGION_BUCKETS=0` off (bit 62 went to B1). Profile (`gow3:DrawRec`, 186,171 samples, queries off, heavy
view panning): stage B's lock waits in `ResolveVertexBuffers` → `runtime_memory_clamp` **~2.4%** (T-P3: 4.7%);
`ProcessDownloadImages` host copy wait 22.9% (W2 off by default), flip wait (`PrepareFrame`) 13.1%, `DrawRecord`
12.2%, `BindResources` 7.9%. Heavy view (~2,700 draws): 130-133 FPS, p99 8.5-9.2 ms, stddev 0.33-0.55 ms, no frames
over 1.5x median in those windows (T-O1: 126-135 FPS, p99 8.4-9.2). **L2 kept** (half the lock waits, no change in
FPS visible).

## T-S1: stage B's time per packet (2026-10-19 23:33-23:41, launched by Claude with the user's go; build `c4bf2dc`; `GOW3_SECTIONS=1`; log `data/t-s1-sections.log`)

Queries off, heavy view, 330-407k packets a second, stage B 92-94% busy, **2.3-2.8 us per packet**:

| part (inclusive) | us / packet |
|---|---|
| ordered tasks | 0.64-1.17 (~35%) |
| of which end of pipe (`ProcessDownloadImages`, its host copy wait) | 0.48-0.62 (~22%) |
| draw (`DrawRecord`) | 1.48-1.55 (~60%) |
| bind (`BindResources`): textures 0.31, buffers 0.13 | 0.51-0.53 |
| vertex (`ResolveVertexBuffers`, with `ObtainBuffer`) | 0.24-0.25 |
| ObtainBuffer (all callers) | 0.19-0.20 |
| descriptors (`Pipeline::BindResources`) | 0.20-0.21 |
| begin render | 0.12 |
| dynamic state | 0.075 |
| render state | 0.065 |
| index | 0.065 |
| emit vertex | 0.05 |
| kick (`ResetBindings` + hand-over) | 0.03 |
| barriers | 0.004 |
| before record (D2 queue) | 0.008 |

Reading: a third of stage B is ordered tasks, two thirds of that the end-of-pipe wait, which is a sync with the Vulkan
recorder: small host copies (`SmallGuestCopy` → `RecordHostCopy`) run on `gow3:VkRecorder` in command order, so
`WaitHostCopies` waits for the recorder to replay everything so far (~113 times a frame); W2 only moved that wait to the
next fence. Within the draw, no single part dominates; what a second recording stage (option c) could take is ~0.45 us
of 1.5 (descriptors, dynamic, emit, kick, the draw record). **First lever: decouple small host copies from the
recorder**: toggle bit 19 (`PoolSmallCopies`) sends them in batches to the copy threads instead (live A/B, T-S2).

## T-S2: small host copies to the copy threads (bit 19) (2026-10-19 23:43-23:49, launched by Claude with the user's go; build `c4bf2dc`; log `data/t-s2-small-copies.log`)

Bit 19 (`PoolSmallCopies` disabled: small copies batched to the copy threads, not run by the recorder) on 23:45:32-23:48:47.
Heavy-view windows (>= 2,400 draws) only after the switch; against T-S1 (same settings, bit 19 off): FPS 126 (T-S1
~130), tasks 0.56 us/packet (0.64-1.17), end of pipe 0.37 (0.48-0.62), **draw 1.61 (1.48-1.55)**, stage A blocked on
copy threads 15.8% (11.6%). The end-of-pipe wait shrinks but the copies' queueing lands in the draw and the copy
threads become the limit: no gain. The user: "Seems about the same, maybe slightly worse panning through the heavy
area." **Bit 19 stays off.** The fence / end-of-pipe waits are ordering cost (every copy before a guest-visible write
must be done), not where the copies run.

## T-S3: Record calls and ordered tasks (2026-10-19/20 23:57-00:02, launched by Claude with the user's go; build `ba9c433`; log `data/t-s3-record-tasks.log`)

Heavy view (~2,780 draws, 127-129 FPS, 2.18-2.23 us per packet): **7.5 `Record` calls per packet, 0.094 us in all**
(~12.5 ns each): merging a draw's records into one would save <= ~0.09 us (~4%); the descriptor time (0.22 us) is the
copy of the descriptor data, which any design does on stage B. Ordered tasks 0.52-0.53 us per packet, by function:
**`RunEventWriteEop` 0.44-0.45 us (~52,000 a second, ~410 a frame)**, `RunEventWriteEos` 0.05 (~7,800/s),
`SignalFlip` 0.03 (~127/s), `RunWriteData` 0.005. So the end-of-pipe fence's readback check
(`ProcessDownloadImages` → `WaitHostCopies`, ~8.5 us each, nearly always with no readback work) is ~20% of stage B;
the fence itself is already deferred (`SignalAfterHostCopies`). In T-W2 (W2 on) the wait moved to `RunEventWriteEos`,
which waits for all copies synchronously.

**Revised order:** (c) as one record per draw would gain little (the Record overhead is small); first **E1**: the
end-of-shader fence deferred like the end-of-pipe one (signalled by the recorder after the copies before it; the
`GdsStore` case keeps its synchronous path), with W2 on, so stage B no longer waits for host copies at fences. Then
re-measure; (c) as a true second thread for the draw's data copies only if stage B stays the limit.

## T-E1: end-of-shader fences deferred + W2 (2026-10-20 00:05-00:13, launched by Claude with the user's go; build `cb1da6c`; log `data/t-e1-eos-deferred.log`)

Queries off, `GOW3_SECTIONS=1`, E1 on, W2 on (`GOW3_EOP_TARGETED=1`); W2 off 00:09:18-00:12:14 (bit 60). Heavy view:

| | W2 on (+E1) | W2 off (+E1) |
|---|---|---|
| packet | 2.03-2.06 us | ~2.15-2.2 us |
| ordered tasks | **0.23** (end of pipe 0.04, EOS 0.007, once-a-frame `RunWriteData` 0.15) | 0.49-0.51 |
| draw | 1.70-1.72 | 1.55-1.61 |
| FPS (draws) | 126-139 (2,570-2,840) | 129.5-136 (2,460-2,704) |
| p99 / worst | 8.2-9.7 / 8.7-11.3 ms | 8.4-9.6 / 8.9-11.4 ms |
| stage A blocked on copy threads | 0% | 6-12% |

The fence waits are mostly gone from stage B (the remainder moved to `RunWriteData`'s `WaitDeferredSignals`, ~1 ms
once a frame), ~5% more packets a second (~372k vs ~351k), stage B still 92-94% busy, stage A waiting on it 25-38%:
**stage B's own draw work (1.6-1.7 us per packet, no dominant part) is the heavy view's limit.** The user: "roughly the
same … There may have been a couple frame spikes … when it was on" then "I don't think the spikes were real, I can't
repeat them. 1% lows in light areas seem really good, I'm seeing 220 FPS with 200 1% lows, very good." **E1 kept (on
by default); W2 to become the default.**

Where stage B stands: what remains is draw recording; option (c) as a true second thread would move the data copies
and closure building (~0.3-0.45 of ~1.7 us), a large refactor for ~15-20% in heavy views.
