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

## T-HZ1: UE4's HZB occlusion (2026-10-20 00:22-00:27, launched by Claude with the user's go; build `852ec13`; log `data/t-hz1-hzb.log`)

`r.AllowOcclusionQueries=1;r.HZBOcclusion=1` ("Use HZB occlusion system (default, less GPU and CPU cost, more
conservative results)", in the binary): the bounding boxes are tested on the GPU against a depth pyramid in one pass
and the CPU reads back one 256x256 results texture, instead of a box draw and two ZPASS events per query. Heavy view:
**~2,200 draws a frame, 169-171 FPS, p99 6.9-7.1 ms, stddev 0.35-0.43 ms** (queries off: ~2,700 draws, ~130 FPS,
p99 ~9 ms), packet 1.95-2.0 us. But the user: "It definitely improves performance, but I think it's likely not getting
all the data it needs. Dan himself gets culled, sometimes the ground or enemies get culled."

Cause (inferred from the log): the results texture is the readback image **0x20010c0000, 256x256 R8G8B8A8Unorm**,
~1 copy a frame, "0 CPU accesses while pending": with `stale_readback=1` the game reads whatever the last finished
copy left in memory. UE4 assigns each tested primitive a pixel of that texture per frame; results from another frame
are laid out for other primitives: effectively random culling. Next: T-HZ2 with stale readbacks off
(`GOW3_STALE_READBACK=0`): if the culling is then right, make only that image precise (a targeted change) and keep
stale readbacks for the rest (luminance 2x2-8x8).

## T-HZ2b: HZB with precise readbacks (2026-10-20 00:28-00:33, launched by Claude with the user's go; build `a968802`; log `data/t-hz2b-hzb-precise.log`)

(`t-hz2-hzb-precise` did not test it: `run.py` overwrites `GOW3_STALE_READBACK` with the ini's `stale_readback`;
`medievil.ini` set to `stale_readback=0` for this run, backup `medievil.ini.bak-before-t-hz2b`.) The user: "that fixes
the culling glitches. Nothing is failing to draw now." But ~1 demand wait a frame (563-566 per 5 s,
`texture_cache.cpp:183`, 1.47 s per 5 s, max 3.1 ms): heavy view ~2,250 draws, **110-116 FPS**, p99 9.7-10.8 ms, GPU
tick waits ~30%. The game reads the results about a frame after asking; the copy, recorded at an end of pipe, was
submitted only at the frame's end and ran after the rest of the frame. **R1** (`088d8c2`): the recording thread submits
right after a readback copy is queued (precise readbacks only), outside the readback locks. Test T-HZ3: HZB +
precise + R1.

## T-HZ3: HZB + precise readbacks + R1 (2026-10-20 00:37-00:41, launched by Claude with the user's go; build `9dea0af`; log `data/t-hz3-readback-submit.log`)

Culling correct. The results wait shrinks but stays: 623-627 waits per 5 s (~1 a frame), 1.04 s per 5 s (~1.7 ms a frame,
max 2.25 ms; T-HZ2b ~2.6 ms), GPU tick waits ~21%. Heavy view, camera still: ~2,290 draws, **~125 FPS, p99 8.5 ms,
stddev ~0.2 ms**; panning p99 9-10.5 ms with a few slow frames a window. The user: "the heavy area is about 125 with 118
1% lows when the cam is static, but moving the camera has a heavy penalty." Why R1 cannot remove it: the game reads
frame N's HZB results at the start of N+1, while our GPU runs about a frame behind the CPU (still on frame N), so the
copy has not run yet however early it is submitted; UE4's HZB tester keeps one results texture and assumes, as on PS4,
that the GPU is done with a frame before the next begins. Avoiding the wait would mean double-buffering the results
inside UE4's code (a patch to the game). **HZB rejected for now; queries off stays the default.** R1 kept (only acts
with precise readbacks). `medievil.ini` restored (`stale_readback=1`).

Both culling routes meet the same limit: a pipeline deeper than the PS4's (queries add stage B work while panning, HZB
waits for its results).

## Defaults verified in a normal launch (2026-10-20 00:47-01:09, the launcher started by Claude with the user's go, without lab variables; build `c8615ea`)

`user/last_run.log`: `Patches: ... ['Native 4K framebuffer pool (512 MB)', '4K display metrics at boot']`, direct memory
512 MiB + 6,592 MiB = 7,104 MiB; the override pak holds `r.AllowOcclusionQueries=0`, `r.GarlicHeapSizeMB=4600`,
`r.Streaming.PoolSize=1600`. The user: "Everything ran well. I played through a whole level that's fairly heavy on
resources, and everything was pretty smooth, GPU usage was in the 70-90% range through most of it. It's clear that the
vulkan translation is the remaining bottleneck. Anything gained there will likely increase 1% lows, but from where we
started, it's in a much better state."

## T-HZ4: HZB remap (2026-10-20 20:00-21:48, launched by Claude with the user's go; builds `aceb5a4`, `53ade79`, `dfdc5fe`; logs `data/t-hz4*.log`)

Design: [study-hzb-double-buffer.md](study-hzb-double-buffer.md). `GOW3_HZB_REMAP=1` with
`r.AllowOcclusionQueries=1;r.HZBOcclusion=1` (`stale_readback=1`). The game's HZB results are served from the newest
*finished* readback copy, remapped by occlusion history key (PrimitiveId, CustomIndex) from the frame that produced
it to the indices the game reads now (`gpu/shim/medievil_hzb.cpp`, `src/hzb_hook.h`); the texture cache numbers the
copies of the results texture (`SnapshotReadback`).

| Run | Change | Result |
|---|---|---|
| T-HZ4 (`aceb5a4`) | v1: hook after the game's MapResults | Culling correct, no pop-in. Heavy view ~2,000-2,300 draws, **160-178 FPS, p99 6.7-7.3 ms** (queries off ~130, T-HZ3 ~125). Age always 2 copies, 0-170 keys a window unmatched, ~25 us a frame. The user: average 175-180, 1% lows ~145, GPU ~90%, "a handful of bigger spikes" into the 70s. |
| T-HZ4b (`53ade79`, `GOW3_SLOW_FRAMES=1.4`) | slow-frame lines (cap 600) | Two kinds of slow frame (vs the same window's samples): **draw-bound** (stage B >= 80% busy, x1.30 time, x1.12 draws) and **starved** (normal draws, stage B ~63% busy, no port wait: x1.77). Starved frames also exist with queries off (T-L2: x1.56, stage B 48%). |
| T-HZ4c (`GOW3_WAIT_SITES=1`) | wait sites | The game's `RHIMapStagingSurface` spun on Sleep(0) ~430,000/s (queries off: ~290/s) until stage B wrote the results copy's label. (First attempt stalled after an early intro skip, kept as `t-hz4c-wait-sites-stall.log`.) |
| T-HZ4d (`dfdc5fe`) | v2: MapResults replaced (the game's map only to synchronise), copies numbered by Submit calls | 0 syncs, 0 drift, age 3. The map spin gone; a second Sleep(0) spin appeared in `RHIGetRenderQueryResult` (0x689c10, ~130,000/s; T-W2 with queries off: ~1.2 M/s): the ~15-18 occlusion queries a frame still issued with HZB. The user: no pop-in, lows generally higher; spikes at the level start (loading, streaming). The 70 "compiles" per run are cached pipelines rebuilt at loads (220-650 ms load stalls), not new shaders. |
| T-HZ4e | + `r.NumBufferedOcclusionQueries=3` | The query spin gone (Sleep(0) ~400/s, the queries-off level). Gameplay windows median 187 FPS, p99 7.0 ms; starved frames ~11 a minute (T-HZ4c 5.8, T-HZ4d 10.7; routes differ, T-HZ4e has 2 heavy windows): **the render-thread spins were not the starved frames' cause.** The last ~5 minutes are the user's frame-limit / buffering tests (both worse), excluded. |

Next: the slow-frame line now splits stage A's time into waiting for the game's submits and draining for stage B
(`gpu_idle_total_ns`, `stage_a_drain_cycles`), to tell a late game from a late stage A in starved frames; then a
`-Samples` run if the game is late.

### T-HZ4f / T-HZ4g / T-HZ4h: where the remaining spikes come from (2026-10-20 21:57-22:25, launched by Claude with the user's go)

| Run | Build / change | Result |
|---|---|---|
| T-HZ4f | `615d583`: stage A idle-for-submits and draining per slow frame | The user, heavy view: ~200 average, 1% lows 130s, 0.1% 110s panning ("juddery"); static 190 / ~150 / ~130, "a persistent oscillation". Stage A waits for the game's submits ~0.03 ms a frame: **the game is never late**. 966 slow frames with stage B busy (+35% stage B time for +8% draws) vs 50 starved (stage A itself slower, often with image uploads). |
| T-HZ4g | `8061698`: stage B time by section per slow frame (`GOW3_SECTIONS=1`) | Slow frames vs samples, per draw: packet +22-27%, textures +39-43%, vertex +31-42%, render state +31-35%, bind +31-37%, ObtainBuffer +19-22%, descriptors +8-13%, tasks +1%: nearly every part, for ~5% more draws. |
| T-HZ4h | `e46dede`: `GOW3_STAGE_B_CORE=1` (stage B on its own physical core, the sibling kept free, priority above normal) | First launch: S1 silent exit at boot (third; plan-baseline-issues.md). Relaunch: per-draw cost of normal frames -4%, but slow frames still +22% (textures +34%): **not core sharing or preemption**. Pacing worse: gameplay p99 7.95 vs 7.28 ms, frames over 1.5x median 70 vs 15 a minute; the user: "a lot of large stutters", 1% and 0.1% lows worse. **Rejected, off.** |

Conclusion: the slow frames really cost more per draw (texture binding the most): work that varies with what comes
into view. Next: the stage B work of [study-stage-b-native.md](study-stage-b-native.md), S2 then S1.

### S2: small read-only buffers bound from the arena (opt-in `GOW3_STREAM_CLEAN=1`)

`BufferCache::ObtainBuffer`: a small read-only range the CPU has not written since its last upload is bound from the
arena (no copy, no upload, no render pass break); a CPU-written one is uploaded into the arena once when no render
pass is open; written again after that, it is marked dynamic and keeps the stream copy. Counters on the "Uploads"
line ("Small read-only buffers/s"). Expected (study): ~0.05-0.1 us per packet on stage B, ~2 GB/s less memcpy for
the recorder, repeated vertex/index binds (feeds S1). Risk: promoted ranges that the CPU rewrites add write faults
once each (watch "write faults/s").

### T-S1a: texture binding split (2026-10-20 23:12, launched by Claude with the user's go; build `60d27d4`; log `data/t-s1a-texture-sections.log`)

Defaults (HZB remap, S2) + `GOW3_SECTIONS=1`, 34 gameplay windows, us per packet: packet 2.35, draw 1.79, textures
0.396 = **slow path 0.200** (sets not memoized, ~12% of calls) + memoized sets 0.109 (lookup and checks 0.054, emit
loop 0.055) + samplers 0.083 (both paths). Texture-set misses: 53% "other key" (slot last held another set), **47%
"generation"**: `RegistryGeneration` changes when any image anywhere is (un)registered, so one texture streaming in
voided every memoized set (the panning link: new textures in view, all sets miss at once).

**Region stamps** (`GOW3_TEXSET_REGIONS`, default on; `=0` the generation as before): the texture cache counts
(un)registrations per 4 MiB region (`RegionStamp`, `BumpRegions`); a memoized set stores the sum of its entries'
stamps (each the bound image's range and the T#'s range) and is valid while that sum is unchanged. A set is not
memoized when any image was (un)registered during its bind (stricter than before).

### T-S1b: region stamps (2026-10-20 23:22-23:27, launched by Claude with the user's go; build `641134c`; log `data/t-s1b-region-stamps.log`)

Texture-set hit rate 83.6% -> **92.3%**, generation misses ~15,300 -> ~48 a window; but textures 0.396 -> 0.392 us
per packet: the slow path 0.200 -> 0.163, the memoized path 0.109 -> 0.137 (every hit summed its entries' region
stamps, render targets spanning several regions). Next build: a hit checks the stamps only when the registry
generation changed since the set was last confirmed (and then takes the new generation), and the memo is two-way
(either of two slots, the less recently used replaced) for the "other key" half of the misses.

### T-S1c / T-W3: texture memo two-way; the WRITE_DATA wait (2026-10-20 23:31-23:45, launched by Claude with the user's go; builds `2492874`, `d67ac70`; logs `data/t-s1c-two-way.log`, `data/t-w3-writedata-targets.log`)

T-S1c: textures 0.396 (T-S1a) -> 0.369 us per packet, slow path 0.152, hit rate 92.4%; "other key" misses unchanged
(~14,500 a window): new sets (~5.5% of draws), not slot conflicts. The user: no texture problems (T-S1b, T-S1c).
Ordered tasks by function (`exe+` symbolized with llvm-symbolizer): **`RunWriteData` ~260 us a call, ~2 a frame**,
`SignalFlip` ~105 us once a frame, `RunEventWriteEop` 0.4 us x ~50,000/s: both wait in `WaitDeferredSignals` (E1's
fences written by the Vulkan recording thread must land first).

T-W3 (`GOW3_FRAME_STATS` WriteData targets): `0x6d2a4c88` and `0x6d2a4c90` (4 B, value 1, user memory, once a frame
each) waited ~0.45 ms each; `0x30000000a0` (8 B, value 0, direct memory, twice a frame) ~0.03 ms. **W3**
(`GOW3_WRITEDATA_DEFERRED`, default on): on stage B, a WRITE_DATA of up to 64 bytes below 0x10'0000'0000 (outside
direct memory, where GPU resources and labels live) is written by the recording thread after the deferred fences,
in order, instead of waiting for them. Stage A's WAIT_REG_MEM already tolerates values that land late (E1).

T-W3b (2026-10-20 23:52-00:02, build `2c2c4ba`, `GOW3_SECTIONS=1`; log `data/t-w3b-writedata-deferred.log`): the flags
now wait 0 ms and `RunWriteData` 0.187 -> 0.013 us per packet, but `SignalFlip` 0.038 -> 0.28-0.46 (the whole
backlog, ~1 ms a frame) and ordered tasks 0.30 -> 0.68: the wait moved to the flip. The user: no issues loading
levels or in menus. **W3b** (`GOW3_FLIP_DEFERRED`, default on): the flip interrupt is raised by the recording thread
after the deferred fences instead of stage B waiting for them.

T-W3c (2026-10-21 00:05-00:07, build `6006996`, two launches): **both froze at boot** after the save-data mount
(where a good boot connects the gamepad and creates `RenderThread 1`). Stacks (`data/t-w3c-freeze-stacks.txt`,
symbolized): the deferred flip interrupt ran on `gow3:VkRecorder`, the guest's flip handler presented inside it
(`sceVideoOutSubmitEopFlip` -> `SubmitFlip` -> `Presenter::PrepareFrame` -> `Scheduler::CommandBuffer` ->
`SyncRecording`) and waited for the recording thread it was running on; `GpuCommand` waited in
`WaitDeferredSignals`. So most of the flip's stage B time is the present synchronising with the recording thread,
which it needs (both record into the same command stream). **W3b removed (opt-in only as a reference, do not use);
W3 made opt-in (`GOW3_WRITEDATA_DEFERRED=1`): alone it moves the wait to the flip (T-W3b worse).** Defaults as at
`d67ac70`. The frame-end recorder catch-up is the real cost; removing it needs the present recorded in stream order
by stage B (a redesign).

### T-W3d: W3 A/B without diagnostics (2026-10-21 00:12-00:28, launched by Claude with the user's go; build `807d4c8`; `-NoStats`)

The user, heavy area: W3 on (`GOW3_WRITEDATA_DEFERRED=1`): ~175 average, 1% ~120, 0.1% ~100. W3 off: ~175, 1% ~148,
0.1% ~124 standing, but panning drops the lows significantly, GPU into the 70s at times; both below T-S2b (~185 / 150 /
125 panning). The pak holds the same cvars as T-S2b. **W3 stays off.** The only other change since T-S2b: the texture
memo (region stamps `641134c`, two-way `2492874`); switch `GOW3_TEXSET_TWO_WAY=0` added. Next: both off (T-S2b's code).

T-texset-both-off (2026-10-21 00:33, build `61440d7`, `-NoStats`, `GOW3_TEXSET_REGIONS=0 GOW3_TEXSET_TWO_WAY=0`, i.e.
T-S2b's code): the user: 1% and 0.1% lows worse than with the memo work; "too much run to run variance at this level,
but it looks like the memo work is worth it". **Region stamps and the two-way memo stay on.** Lesson: at ~175-185 FPS
single feel runs differ by about as much as these changes; small stage B changes need logged A/B runs on the same
route (or several runs) to be judged.

### T-Z1: depth prepass off (2026-10-21 00:58-01:22, launched by Claude with the user's go; build `8acd280`; logs `data/t-z1-earlyz-off.log`, `data/t-z1-baseline.log`)

The game's PS4 config sets `r.EarlyZPass=1` (PS4Engine.ini; DefaultEngine.ini says 0; `r.DBuffer` not set). Same
heavy spot, panning then standing, frame stats only. Baseline (`r.EarlyZPass=1`): ~2,280-2,310 draws, 157-162 FPS,
p99 7.4-7.6 ms. `r.EarlyZPass=0`: ~2,180 draws at 176-178 FPS, p99 6.8-6.9 ms (and ~2,015 draws at 189-194 FPS, p99
5.9-6.2, in the standing view). **About 120 fewer draws, ~+10% FPS, lower p99; the user saw no visual artifacts.
Default since 2026-10-21 (`ue4_override.py`).** (A first T-Z1 started with the monitor off: a 60 Hz FIFO swapchain plus
a 60 FPS limit, median 16.67 ms, stddev 0.7, p99 18.2 ms, the user's lows 55/52: the frame limiter beating against
VSync, `data/t-z1-60hz-capped.log`; also seen at the 240 cap. A baseline launch exited with S1 at a save load.)

### T-L0: `rhi.SyncInterval=0` (2026-10-21 01:28-01:33, launched by Claude with the user's go; build `5231983`; log `data/t-l0-syncinterval0.log`)

Hypothesis: UE4 flips at every vblank (SyncInterval 1), so frame times would be quantized to the 480 Hz vblank's 2.08
ms steps (4.17 / 6.25 ms), the heavy view's oscillation. **Disproved:** window medians are continuous in this and
earlier runs (5.08, 5.16, 5.58, 5.85 ms...), the immediate-flip path presents frames when ready. T-L0 heavy windows ~160
FPS at ~2,300 draws, stddev 0.29-0.4 ms, like earlier runs; the user: no timing issues (fights fine), "quite possible
that setting has no effect". Not adopted. The 480 Hz tick only bounds how late a frame can be when it does wait.
Next: the frame limiter A/B at the cap (`GOW3_PRECISE_LIMIT`, `5231983`).

### T-L1b: precise frame limiter at 233 (2026-10-21 01:40, launched by Claude with the user's go; build `a297c6b`; log `data/t-l1b-precise-233.log`)

`GOW3_FPS_LIMIT=233 GOW3_PRECISE_LIMIT=1` (G-Sync on). The user: no visible difference in the light area (it did not
reach the cap there); in the heavy area more, larger spikes while panning. Log: heavy windows 152-173 FPS, 5 of 10 with
1-2 frames over 1.5x the median, worst 9.4-10.2 ms (T-L0: mostly none, worst 7.4-8.9). Likely: frames faster than the
slot spin up to 1 ms on the realtime present thread, taking a core from stage A/B. **Precise limiter stays opt-in,
off.** Upstream 1.3.5 (`d4c9cf4`, on `merge-1.3.5`): with VSync on, a limit at the refresh is left to VSync (their
issue #3), which covers the limit-at-refresh case. If cap pacing under G-Sync matters later: a high-resolution
waitable timer to the exact slot, no spin. T-L1c (VSync alone at 240) skipped; it comes with the merge.

### T-Z2-T-Z4b: DBuffer decals and the forced prepass (2026-10-21 02:00-03:14, launched by Claude with the user's go)

Heavy-view captures (`study-heavy-frame-map.md`): the depth prepass is a **full** prepass (628 / 739 draws, ~28%,
the G-buffer drawn twice) whatever `r.EarlyZPass` says: DBuffer decals force it (UE4 4.21). The decals in that view:
4 draws (eid 9800-9868) touching ~3,500 pixels of Dan's boots by 1-4/255 (invisible;
`research-data\renderdoc\heavy-analysis\decalshot\`). T-Z1's "~120 fewer draws" is not what the captures show.

| Run | Settings | Result |
|---|---|---|
| T-Z2 | `r.DBuffer=0` (prepass gone) | ~1,650 draws (-600). The user, standing: the best yet (1% ~165, 0.1% ~145). Panning: large spikes (1% 107, 0.1% 82 over a minute); log: worst frames 21-45 ms in panning windows, stage B waiting on the GPU 1.5-8% (was ~0). |
| T-Z3 | `r.DBuffer=0 r.EarlyZPass=1` (partial prepass) | Worst frames ~9-12 ms, more frequent; GPU waits up to 17%. The user: a 12 ms frame at ~180 average is a large spike; worse than most runs of the night. Hypothesis (the user): the GPU at 95-100% starves stage B as scenes get heavier. |
| T-Z4 | + `t.MaxFPS=120`, `GOW3_FPS_LIMIT=0` | First launch: boot crash, "STOP: unmapped host pthread error -1" (`runtime_mutex.c` orbis_error), then a null fault in `PageManager::OnGpuMap` during the exit (`data/t-z4-crash-pthread.log`, dump in `user/crashdumps/`): new, rare. Relaunch booted: **`t.MaxFPS` is ignored** (ran to 240; `data/t-z4-maxfps-ignored.log`). |
| T-Z4b | engine_fps=120 (UE4 SmoothedFrameRateRange; ini backed up and restored), `GOW3_FPS_LIMIT=0` | **Stuck on the intro loading screen** (3 draws a frame at 59 FPS for ~2 min; `data/t-z4b-stuck-loading.log`), like the first T-HZ4c attempt: an intermittent intro-load stall, cause unknown. |

Open: whether the T-Z2/T-Z3 panning spikes are GPU saturation (an engine-side cap test still to do); defaults unchanged
(`r.DBuffer` on, `r.EarlyZPass=0`). Port limiter note: the in-game frame limit and `GOW3_FPS_LIMIT` are the same
present-thread limiter; an external limiter (RTSS) sits at the end of a deep pipeline (see the reply of 03:00).

T-Z4c / T-Z4d (2026-10-21 03:16-03:25): engine_fps=120 (UE4 SmoothedFrameRateRange) with `GOW3_FPS_LIMIT=0` ran at
175-182 FPS (T-Z4c); adding `r.VSync=1` (T-Z4d) still uncapped (the user: "still 240"). **UE4's own tick ceiling does not
engage in this build from the config; every cap so far was the port's present limiter.** `medievil.ini` restored
(engine_fps=240). Next: GPU saturation tested by lowering GPU load instead (`r.ScreenPercentage=70` with T-Z3's settings).

T-Z5 (2026-10-21 03:30, `r.DBuffer=0 r.EarlyZPass=1 r.ScreenPercentage=50`; log `data/t-z5-screenpct50.log`): stage B
never waited on the GPU (0% every window), stage B 90% busy, yet the panning windows still spiked (up to 44 frames over
1.5x median, worst 9-12 ms); the user: the spikes stay at 50% resolution, 0.1% lows about the same. **The panning spikes
are stage B work, not GPU saturation.** **Default since 2026-10-21: `r.DBuffer=0`, `r.EarlyZPass=1`** (the user: "we found
a way to increase the 1% lows significantly in heavy areas"). Next: why panning costs stage B so much (slow-frame
sections while panning).

### T-P1 / T-P2: why panning costs (2026-10-21 03:35-19:40, launched by Claude with the user's go; build `166557c`; logs `data/t-p1-panning.log`, `data/t-p2-panning-samples.log`, samples `research-data\samples\t-p2-panning-samples`)

T-P1 (4K, slow frames + sections + per-frame misses): stage B never waited on the GPU in the worst frames. Two kinds:
**A, new content in view** (most 10-24 ms frames): stage B only 45-76% busy, stage A not waiting for the game and only
~2-3 ms on stage B, ~180-240 FindImage page-table lookups (0 normally), ~150-220 unprepared draws (36 normally),
texture-set misses ~200-370 (156); **B, bursts every 15-30 s**: 2-4 frames of 10-14 ms, stage B 95-98% busy, 3-4 ms of
host-copy waits in the end-of-pipe task, no new images. T-P2 (`-Samples`, 50% resolution, panning): stage A 13% in
`CollectRingBindings` -> `runtime_memory_clamp`, **5% of stage A blocked in `RtlAcquireSRWLockShared`** there: the region
table lock, held exclusively by `pool_commit` / `pool_decommit` across the host mapping calls while UE4 streams pool
pages (the region caches miss because those commits change their 16 MiB buckets).

**Lock-free clamp** (`GOW3_OPTIMISTIC_CLAMP`, default on): `vma_seq` odd only while table entries change (insert,
erase, split), not across host calls; the table is allocated at 131,072 entries up front and never moves (past that,
lock-free reads stop); `runtime_memory_clamp` searches without the lock and keeps the result when `vma_seq` was even
and unchanged, else retries, then takes the lock as before.

T-P3 (2026-10-21 19:45, build `e15a859`, `-Samples`, 50%, panning; samples `research-data\samples\t-p3-lockfree-clamp`):
the clamp no longer waits (0.46% self, no lock wait), but the same lock moved to `runtime_memory_region`, called by
`CopySparseMemory` in `CollectRingBindings`: 4.7% of stage A blocked in `RtlAcquireSRWLockShared`, 2% releasing it.
`runtime_memory_region` now also reads lock-free the same way (same switch).

T-P4 (2026-10-21 20:00, build `45eca39`, same routine; samples `research-data\samples\t-p4-lockfree-region`): stage A
`CollectRingBindings` 13.4% (T-P2) -> **7.1%**, lock waits 6.4% -> 3.0%, stage A waiting on stage B 26.9% -> **41.8%**
(stage A no longer the one falling behind). Left: `runtime_memory_clamp` still falls back to the lock (stage A 2.2%,
stage B 2.1% via ResolveVertexBuffers): a table edit is a `memmove` of half the ~13,000-entry sorted array (~260 KB,
tens of us, thousands a second while streaming), longer than the 4 short retries. Now the lock-free reads spin through
an edit for up to ~100 us before taking the lock. (Also seen: `runtime_memory_gpu_protect` ~1% of stage B on the same
lock; and the sorted-array table itself is a writer cost worth replacing later.)

T-P5 (2026-10-21 20:15, build `76de0f5`, 4K, `-NoStats`): the user: "quite good, the best it's been", panning included.
All of the above stays default (no further switch needed: the lock-free reads, region stamps, two-way memo, decals off
with the partial prepass, the HZB remap, S2 are on by default).

### T-M1 (table stats): what the region table costs (2026-10-21 21:25, launched by Claude with the user's go; build `c7e128e`; `-NoStats`, 50%, `GOW3_HOST_MAP_STATS=1`; log `data/t-m1-table-stats.log`)

Steady (panning or standing), per 5 s: pool commits ~5,430 (~69 ms, ~13 us each), **decommits ~5,420 (~320 ms, ~60 us
each)**, guest-map edits ~22,000 (~48 ms of memmove, ~2.2 us each: **the sorted array is not the cost**), host map ~5,500
(~21 ms), protect ~7,500 (~68 ms), table lock held exclusively ~33,000 times for ~330 ms (6.6% of the time). Kept ~=
reused views (~5,500): UE4 decommits and recommits the same pool pages ~5-6 times a frame. M1 (`40a416a`) made a
decommit keep its view but flip it to `PAGE_NOACCESS` (`host_keep_view`, not in the protect counter); revoking access
rewrites page entries and flushes TLBs on all cores. **`GOW3_DECOMMIT_KEEP_ACCESS=1`** (opt-in): the kept view keeps
its access (a read of decommitted memory sees stale data instead of faulting). Never tried before (M1 kept the fault).

T-M2 (2026-10-21 21:50, `GOW3_DECOMMIT_KEEP_ACCESS=1`, 50%, stats): decommits ~320 -> ~41 ms per 5 s (~60 -> ~8 us
each), commits ~69 -> ~104 ms, commits + decommits ~390 -> ~145 ms, table lock held exclusively ~330 -> ~105 ms; no
faults. T-M3 (4K, `-NoStats`, on vs off, same spot): the user: "better with it on"; remaining: smaller high-amplitude
spikes that drag the 0.1% lows. **Default since 2026-10-21** (`GOW3_DECOMMIT_KEEP_ACCESS=0`: the M1 no-access flip).

### T-S5 / T-Q2: the remaining spikes; frames queued 2 (2026-10-21 22:10-22:22, launched by Claude with the user's go; 50%, slow frames + sections; logs `data/t-s5-spikes.log`, `data/t-q2-frames-queued-2.log`)

T-S5 (defaults): median frame 4.78 ms (~210 FPS), only 23 frames >= 9 ms in ~2 min: 16 "new content" (two bursts of ~8
frames, 9-23 ms: ~100-250 FindImage page-table lookups, 150-2,250 unprepared draws, 150-330 texture-set misses, some
image uploads, stage B 48-96% busy), 2 host-copy waits (50-80 MB image uploads), 2 GPU waits. Stage B's profile
(T-P4): ~27% in the flip's frames-ahead wait (`PrepareFrame` -> `Scheduler::Wait`, `frames_queued=1`), ~11% in
`RunWriteData`'s deferred-signal wait: stage B's "busy %" includes these waits. T-Q2 (`GOW3_FRAMES_AHEAD=2`): FPS
213 vs 210, p99 6.49 vs 6.38, 18 frames >= 9 ms in ~35 s; the user: "buffering additional frames seems to hurt
stability every time, especially with 1% lows". **frames_queued stays 1.** The frames-ahead wait is slack, not a limit.
Next: the new-content bursts (draw preparation falling behind, FindImage page-table lookups).

### T-H3b: one GPU particle pass (2026-10-21 23:10, `r.GPUParticle.FixDeltaSeconds=0`, 4K, frame stats; log `data/t-h3b-particles-fixdelta0.log`)

The user: particles, fire, fog look fine; heavy area slightly better; low risk. Draw counts inconclusive (short, different
spot). **Default since 2026-10-21** (`GOW3_PARTICLE_FIXED_STEP=1`: the fixed step).

### Launcher: performance patch toggles (2026-10-21)

Performance page, "Performance patches": one switch per bundle, on by default, off sets its variables (HZB occlusion
`GOW3_HZB_REMAP=0`; decal prepass `GOW3_DECAL_PREPASS=1`; particle pass `GOW3_PARTICLE_FIXED_STEP=1`; small buffers
`GOW3_STREAM_CLEAN=0`; texture memo `GOW3_TEXSET_REGIONS=0` + `GOW3_TEXSET_TWO_WAY=0`; lock-free memory lookups
`GOW3_OPTIMISTIC_CLAMP=0` + `GOW3_REGION_BUCKETS=0`; cheap frees `GOW3_DECOMMIT_KEEP_ACCESS=0`; GPU data without waits
`GOW3_READBACK_QUEUE=0` + `GOW3_GPU_WRITE_TWINS=0` + `GOW3_EOS_DEFERRED=0` + `GOW3_EOP_TARGETED=0`; 4K memory
`GOW3_PS4_MEMORY=1`), each with its description and measured gain. **The launcher no longer sets `GOW3_FRAME_STATS=1`
by default** (it did: normal play had the stage B diagnostics on); Advanced > Frame statistics turns it on.

### In-game menu and the launcher share the settings (2026-10-22)

- **Save fix** (`e9d8699`): the in-game menu rewrote every key it knows from its memory, so test launches'
  environment overrides (frames queued, readback modes) became permanent and ~40 keys (God of War III's included) were
  added. It now writes only the settings changed in it since the load or the last save, in place.
- **Performance patches in medievil.ini** (`perf_*`, launcher > Performance and in-game menu > Performance; run.py
  turns `perf_*=0` into the switches, `graphics_options.PERF_SWITCHES`). In the menu, live: small buffers from VRAM
  (`perf_small_buffers`, read per draw by the buffer cache), lock-free memory lookups and cheap memory frees
  (`runtime_memory_set_perf`). On restart: HZB occlusion, the decal prepass and the particle pass (override pak cvars
  read at start), the texture memo and GPU data without waits (switching them mid-run would void state they built:
  texture sets validated by another rule, twins, the readback queue chosen with the Vulkan device), 4K memory (sizes
  set at boot). Also in the menu's restart section now: graphics quality and UE4 upscaling (MediEvil).
- Small buffers from VRAM stays MediEvil-only by default (on where run.py sets `GOW3_STREAM_CLEAN=1`).

### T-L2: flip trace, uncapped vs 120 cap (2026-10-22 00:20-00:29, launched by Claude with the user's go; build `1e657ca`; `-NoStats`, `GOW3_FLIP_TRACE`; trace `research-data\flip-trace-1.csv`)

The user: at the 120 cap (in-game limit) 0.1% lows fell from 200+ to 80-90. Trace (per flip: game submit, stage B
irq, queued, limiter release, presented, flip event). Uncapped (37,030 frames): the limiter waits 0.01 ms; intervals
follow the game's frame times. **Capped (12,190 frames): each frame waits ~8 ms in the queue for its slot (the game and
stage B have headroom), yet limiter releases come 6.3-10.4 ms apart: p99 9.86, p99.9 10.43 = 8.33 + one 480 Hz tick.**
The present-thread limiter releases a frame up to half a vblank early, or at a later tick when its slot falls past
the current one; the schedule stays anchored, so the next comes early. G-Sync shows it, and the game's flips follow it
(submit p99.9 10.7 ms). Fix (`GOW3_PRECISE_LIMIT=1`, reworked): the slot is waited for with a high-resolution
waitable timer (no spin, unlike T-L1b), released within ~0.1 ms of it, never early.

### T-L3 / T-L4: precise limiter (2026-10-22, launched by Claude with the user's go; `-NoStats`, `GOW3_PRECISE_LIMIT=1`; traces `flip-trace-2.csv`, `flip-trace-3.csv`)

T-L3 (`c552021`, high-resolution timer): no change, the tick path still released capped frames a tick late. T-L4
(slot-only release; `c67b7e1` restores the tick redraw, whose absence froze the save list): **capped 120: presented
interval median 8.28, p99 9.05, p99.9 9.39, max 12.5 ms; worst 1% average 9.23 ms (108 FPS), worst 0.1% 10.0 ms
(100 FPS); 3 of 9,411 frames over 10 ms.** Releases sit within +-0.25 ms of an ideal 120 Hz grid, a tail to +1 ms
(timer wake-ups). Motion error (|submit step - display step|) median 0.32, p99 1.26 ms; latency submit -> presented
16 ms (the game waits a frame in the queue). The user: "technically better", still dips into the 80s, 1% ~110 /
0.1% ~100, and **heavy flicker with G-Sync** (OLED VRR brightness following the frame-time spread at ~120 Hz). Stays
opt-in.

### T-L5: newest-frame mode (`GOW3_NEWEST_FRAME=1`, the user's proposal)

The game renders uncapped; at each slot the newest frame finished on the GPU is shown and every older one completes
its flip unshown at once (`DropFlip`: flip status, events and labels as a shown flip; the frame goes back to the
presenter's pool), so the cap never holds the game back. Frames finish on the GPU in queue order: the oldest queued
frame is dropped as soon as the one after it is finished (`Presenter::WaitFrameReady`, the frame's timeline
semaphore); before the slot the present thread waits for that GPU finish or a new frame, then the precise slot wait.
Implies `GOW3_PRECISE_LIMIT`. Expected: even presents and lower latency; risk: the game time shown advances unevenly
(the newest frame is 0 to one render interval old at its slot: Fast Sync micro-judder), and no GPU headroom is freed.
Trace stage `dropped`; analysis `newest_analyze.py` (display step, content step, their difference, age).

**T-L5 run 1 (`7cca894`, 2026-10-22 22:3x): not working.** The user: even the intro videos had capped-like lows.
Trace `flip-trace-4.csv`: 0 frames dropped, submits every 8.33 ms, each frame ~7.9 ms in the queue. The guest
cannot render its next frame until the previous flip completes (it waits for its display buffer's label), so with
completion at present no newer frame ever arrived. Fix: in this mode the guest's flip completes in
`SubmitFlipInternal` right after `PrepareFrame` copied the frame out (`Request::completed`; `newest_active` is set
by the present thread only while a limit applies); the present thread only chooses among the copies.

**T-L5 run 2 (`03a84b5`): the mode works, the wobble stays.** The user: "once again, it's not working". Trace
`flip-trace-5.csv` (last 3,000 frames): the game renders every 3.1 ms (~325 FPS), ~2 of 3 frames dropped, a finished
frame is always waiting at the slot, yet shown frames come median 8.29, p1 7.54, p99 9.24, p99.9 11.3 ms apart, the
same spread as T-L4. **So the remaining wobble is the present thread's own release timing**, not the game: the
same thread runs the 480 Hz vblank tick (guest events, coarse condition-variable and semaphore waits that wake 1-2 ms
late), and 8.33 ms is exactly four ticks, so slot and tick drift slowly against each other. Motion error (|content
step - display step|) median 0.99, p99 4.18 ms (T-L4: 0.32 / 1.26): the expected newest-frame cost.
Next (`GOW3_NEWEST_FRAME` now): a dedicated pacer thread (`gow3:Pacer`) releases at each slot with a per-thread
high-resolution timer plus a 0.1 ms spin; dropped frames return to the pool on the game's side
(`DropOlderFinished` in `SubmitFlipInternal`). The user also asked for tick-rate runs: `GOW3_VBLANK_HZ` 240 / 120
(a 120 limit at a 120 tick is off: the tick paces) / 60.

**T-L5 run 3 (`fbce36f`, pacer thread, 480 tick): the cause found.** The user: main menu lows ~110 (uncapped 180+).
Trace `flip-trace-6.csv`: releases median 8.26, p1 7.91, p99 8.76, p99.9 9.11 (tighter than run 2, not even). The
interval sequence is a **sawtooth**: three or four of ~8.1-8.3 ms, then one of ~8.6-8.7 ms. The high-resolution
waitable timer wakes on a ~0.5 ms grid on this system; with a 0.1 ms spin a release lands 0-0.5 ms after its slot,
and the slot's phase on that grid moves 8.333 mod 0.5 = 0.33 ms a frame. This also explains T-L4's flat +-0.25 ms
band. Fix: the pacer spins the last 0.7 ms (`GOW3_PACER_SPIN_US`, ~8% of one core at 120 FPS, on its own thread; the
present thread keeps 0.1 ms: it waits every tick).

### T-V120: 120 Hz tick, VSync (FIFO), no limiter, 50% (`ue4_upscale=performance`) (2026-10-23, launched by Claude with the user's go; `f4ffc01`; `GOW3_VBLANK_HZ=120`; trace `flip-trace-8.csv`)

The user: "1% and .1% lows were a solid 118-119 ... even looking in the heavy area. We weren't able to get a frametime
this steady before", then a big spike every 5-10 s growing denser and denser. Trace: between episodes the presented
interval p99 is **8.42 ms**. Episodes at ~245-300, 400-465 and 565-630 s: up to 170 a 5 s window of **exactly 12.50
ms followed by 4.17 ms** (three and one 240 Hz refreshes); release-to-present 4-4.7 ms for the long one. Presents
snap to a 4.167 ms grid; the 120 Hz tick (`AccurateTimer`, the PC clock, releases +-0.7 ms) slides against the
display's refresh, and when its phase nears a refresh boundary the jitter pushes frames across it: a beat. Fix to
try (`GOW3_VBLANK_SYNC=1`): the tick waits for the display's own vblank (DXGI `WaitForVBlank`, primary output,
every Nth for a tick below the refresh). Open: the user has G-Sync on (windowed too), yet these presents land on a
fixed 240 Hz grid while the newest-frame runs' did not.

**T-V120s (`c32a18e`, `GOW3_VBLANK_SYNC=1`, G-Sync on, the indicator confirmed it): rejected.** Log: "tick on
every 2 of the display's vblanks (240 Hz)". Presented median 19 ms (~52 FPS), cycling ~14.5 / 27.7 / 19 ms; the
user: videos stutter badly, main menu 1% low 1.7 FPS. Under VRR the display has no steady vblank (it refreshes when a
frame arrives, else at its minimum rate), so waiting for vblanks ties the tick to its own presents. The T-V120 waves
are therefore not a beat against a fixed refresh (the earlier explanation was wrong); with G-Sync confirmed on, why
presents snapped to a 4.167 ms grid is open. Next: the plain tick with the pacer's precise wait; if the waves stay,
measure display times with `VK_KHR_present_wait`.

**The 480 Hz tick and the 120 Hz path.** Upstream `55e1a76` (2026-10-14) set the 480 Hz tick plus a limiter because "a
120 Hz virtual vblank turned 9 ms frames into 17 ms ones": a frame that misses a tick waits for the next. That build
could not hold 120; the user's view (agreed): with today's pipeline (heavy-area uncapped 1% lows ~145 FPS, ~6.9 ms)
misses are rare, and the 120 Hz tick also paces the game's frame starts (UE4 waits for the vblank), which the 480
tick plus a present-side limiter never did. The 120 Hz tick is the front-runner for a capped mode.
Next: `GOW3_PRECISE_TICK=1` (`GOW3_TICK_SPIN_US`, default 700): the tick on a fixed grid with `PreciseWait`
instead of `AccurateTimer` (T-V120 ticks +-0.7 ms; suspect for the waves).

**T-V120p (`181c4c1`, `GOW3_PRECISE_TICK=1`, ~3 min):** ticks exact (p1/p99 8.32/8.34), present calls 7.8-8.9 ms;
the user: 1% lows in the 80s on the NVIDIA overlay (RTSS ~110). **The NVIDIA overlay is the reference from here**
(it matches the visible flicker and judder; RTSS and our "presented" stage time the present call, not the display).
**T-N120 native present** (`GOW3_VBLANK_HZ=120`, `GOW3_PRESENT_THREAD=0`, `GOW3_FRAMES_AHEAD=0`, trace
`flip-trace-11.csv`, ~11 min): present calls 1% 7.0 / 99% 9.7 ms throughout, **no 12.5 / 4.17 waves**; the user:
1% lows better than T-V120p, not T-V120's flat line. Next: T-V120's exact settings again, G-Sync indicator on, with
display times (`displayed` trace stage: `VK_KHR_present_id` + `VK_KHR_present_wait`, enabled only with
`GOW3_FLIP_TRACE`; a swapchain thread waits for each game present in order).

**T-V120b (`d18cfdf`, T-V120's settings + display times): not a repeat.** The user: unstable lows again. Trace
`flip-trace-12.csv`: `displayed` came 0.00 ms after `presented` for every frame (NVIDIA's present wait returns at the
present: no display information), and present calls spread 1% 7.06 / 99% 9.67 ms like the native run, not T-V120's
8.42 ms. Present ids are a change on the present path; display times now need `GOW3_DISPLAY_TRACE=1` (off), so the
trace behaves as in T-V120. The launch environment and `medievil.ini` otherwise match T-V120 (checked).

### Baseline recheck of T-V120 (2026-10-23 01:30-02:10, launched by Claude with the user's go each time)

All at 50% (`ue4_upscale=performance`), `GOW3_VBLANK_HZ=120`, VSync on (FIFO), borderless, `-NoStats`, flip trace.
Measure: share of presented intervals within 8.33 +- 0.1 ms ("locked"), and the present call's duration
(release -> presented).

| Run | Build | Display / other | Locked | Present call | The user |
|---|---|---|---|---|---|
| T-V120 (00:32, trace 8) | `f4ffc01` | G-Sync state unverified (no indicator), RTSS + NVIDIA overlay on, NVCP present method Auto (= layered on DXGI) | **71%** | 0.3 ms for ~4 min, then **2.4-4.0 ms** (falling), from a 218 ms hitch (the level load) | flat, best ever; waves of 12.5/4.17 ms |
| T-V120c (trace 13) | `8a96bc7` (lab) | G-Sync on, indicator on | 14% | ~0.3 ms | "completely different" |
| T-V120d (trace 14) | `8a96bc7` | NVCP present method native Vulkan, then back to DXGI | - | ~0.3 ms | not flat |
| T-V120e (trace 15) | **`f4ffc01`** | G-Sync on (toggled off mid-run) | 12% | ~0.3 ms | not the same |
| T-V120f | `f4ffc01` | G-Sync off | - | - | boot crash (pthread, null execute; dump `user/crashdumps/medievil-20261023-020029-28676.dmp`) |
| T-V120g (trace 16) | `f4ffc01` | G-Sync off from launch | 13% | 0.26 ms | lows identical to the last run |
| T-V120h (trace 17) | `f4ffc01` | G-Sync off, `GOW3_PRESENT_THREAD=0` | 12% | 0.19 ms | "that isn't it" |

**Established:** not the code (`f4ffc01` = lab today), not G-Sync on/off, not the NVCP present method, not the
overlays (both were on in T-V120). In T-V120's flat part our releases wobbled as everywhere (1% 7.1 / 99% 9.6 ms);
the present call waited 2.4-4 ms and frames reached Vulkan at 8.25-8.42 ms: the presentation itself was paced by the
display, which no run since has shown. **Open:** what put the presents in that state (a Windows / driver
presentation mode; candidates: independent flip vs composed, a queued frame). Next: PresentMon (presentation mode
per present) alongside a run; or deliberate pacing at a fixed offset from the display's refresh (G-Sync off only).

### Presentation settings in the launcher (2026-10-23)

Launcher > Performance > **Presentation** (all `medievil.ini`, applied at start by `run.py` through
`graphics_options.present_environment`; a variable already in the environment wins, so `run-game.ps1 -Env` tests
are unchanged; the values are `GOW3_DERIVED`, so an in-game restart takes new ones):

| Setting (ini key) | Variable | Default |
|---|---|---|
| Vblank tick rate, Hz (`present_tick_hz`, 0 = automatic) | `GOW3_VBLANK_HZ` | 0: 480 Hz + limiter above 60 FPS, else 60 |
| Present mode (`present_mode`: auto / fifo / fifo_relaxed / mailbox / immediate) | `GOW3_PRESENT_MODE` (now read by `Swapchain::FindPresentMode`; it was unused) | auto = follow VSync |
| No built-in frame caps (`present_no_frame_caps`) | `GOW3_NO_FRAME_CAP=1` (present thread: no limit of the port's own; the in-game Frame rate limit still applies) + engine bound 1000 (`run.py`: UE4 `SmoothedFrameRateRange`) | off |
| Present on the vblank thread (`present_inline`) | `GOW3_PRESENT_THREAD=0` | off |
| Precise frame limiter (`present_precise_limit`) | `GOW3_PRECISE_LIMIT=1` | off |
| Newest-frame mode (`present_newest_frame`) | `GOW3_NEWEST_FRAME=1` | off |
| Precise vblank tick (`present_precise_tick`) | `GOW3_PRECISE_TICK=1` | off |
| Tick on the display's vblank (`present_vblank_sync`) | `GOW3_VBLANK_SYNC=1` (broken with G-Sync) | off |
| Frame timing trace (`present_frame_trace`) | `GOW3_FLIP_TRACE=user/logs/flip-trace.csv` | off |

Already there: VSync, Frame rate limit, Frames queued, Engine frame rate, Display mode. The port's frame caps were
(1) the engine bound `SmoothedFrameRateRange` = Engine frame rate + 2 (`ue4_override.frame_rate_range`), (2) the
present thread's limit at min(display refresh, Engine frame rate) with the automatic 480 Hz tick
(`EmulatorSettings::GetFrameLimit`), (3) the debug menu's `SetFrameRateLimit(60)` (already patched out); "No built-in
frame caps" removes (1) and (2). An explicit tick rate below the frame rate still paces the game (UE4 waits for each
vblank).

### T-P120: the configuration that holds the cap (2026-10-23 ~03:00, the user, launcher settings)

**Result (the user, NVIDIA overlay): flat frame-time graph, 1% lows 118-119 at a 120 tick, no cyclical waves, with
G-Sync on or off, at 4K native, Ultra, no upscaling.** A 165 Hz tick kept 0.1% lows above 150 in the heaviest area.
`medievil.ini` (Launcher > Performance > Presentation):

| Setting | Value | Active at this tick? |
|---|---|---|
| `present_tick_hz` | 120 (also 165) | yes: the game is paced by its own vblank |
| `present_precise_tick` | 1 | yes: tick on a fixed grid, high-resolution timer + 0.7 ms spin |
| `present_inline` | 1 | yes: the vblank thread presents (no swap-thread hand-off) |
| `present_mode` | immediate (`vsync=0`) | yes: the present never waits |
| `present_no_frame_caps` | 1 | no effect with an explicit tick |
| `present_precise_limit`, `present_newest_frame` | 1 | inactive: an explicit tick switches the limit off (limit >= tick) |
| `fps_limit` 0, `engine_fps` 240, `frames_queued` 1, borderless | | |

The user's read: precise vblank tick and present on the vblank thread contribute most, Immediate possibly. No trace
yet. Interpretation: frames are released on an exact grid at the game's own frame start (UE4 waits for the vblank)
and reach the display with nothing in between adding jitter; T-V120's flat stretches got the same effect by
accident (the display's fixed refresh pacing a queued frame, drifting against the PC-clock tick, hence its cycle).
Earlier capped runs never combined the three: T-V120p had the precise tick with the hand-off and FIFO, T-N120 had
inline presents with the coarse tick, the 480 Hz tick + present-side limiter paced the present but not the game's
frame starts. The user's working rule: **tick rate = the desired cap (or the refresh rate), never 480 Hz plus a
limiter.**

Next (the user, next session): a trace of T-P120 (`present_frame_trace=1`); then one setting back per launch
(precise tick, inline, Follow VSync instead of Immediate) to find the essential ones; then make that set the
capped mode ("tick = cap"), and look again at the 480 Hz default for uncapped play. Also open: thread priority
(`SetCurrentThreadRealtime` is a no-op on Windows, `common/thread.cpp:76-78`), tearing with Immediate and G-Sync
off, and the research in [study-vrr-present-pacing.md](study-vrr-present-pacing.md).

### Ablation of the T-P120 settings (2026-10-23 evening; traces `research-data\flip-trace-18.csv` to `-24.csv`)

The user had spent ~2 h testing on their own first: their finding, precise vblank tick + present on the vblank
thread are required for any cap or VSync to pace well; Immediate helps. Runs (`-NoStats`, 4K native console, trace):

| Run | Settings | Result |
|---|---|---|
| 0 (trace 18) | 165 tick, Immediate, Fast Sync on in NVCP | invalid (Fast Sync); its flat end was the user alt-tabbed (DWM composition buffers frames) |
| 0b (19) | the same, Fast Sync off | intro/menus: 13-15% missed ticks (24 ms); not reproducible: 0d with identical ini and launch path was clean. External state at that moment |
| 0c (20) | 120 tick, the three inactive settings off | clean: 98% 8.12-8.54 ms, releases 8.331-8.336 |
| user (`user/logs/flip-trace.csv`) | 165, user's launcher | clean, 1% worst 6.26-6.37 ms |
| 0d (21) | 165, Immediate, my script | **reference**: 165.0 avg, 1% low 158.1, 0.1% 151.8, 0 frames > 9 ms in 75 s |
| 5 (22) | 165, Follow VSync + VSync on (FIFO) | 1% low 126.3, 0.1% 81.9: 27 frames of 12.1 ms (missed ticks) a minute; in a big fight 107 x 12.1 ms in 6 s with PrepareFrame waiting >= 3 ms on the GPU for 10% of frames |
| 5@120 (23) | 120, FIFO | the same fight: 120.0 avg, 2 frames > 9 ms; earlier 16.7 ms frames were game-side late submits in menus/loading |
| (24) | 120, FIFO, new `gpu_ready` stage | GPU finishes a frame ~4-6 ms after its flip is recorded; heavy moments up to ~9-10 ms |

Notes: launch path (launcher vs `run-game.ps1`): same priority (Normal), no power throttling, both in a job; patch
lists are the same (run.py adds "4K display metrics at boot" for 4K native regardless of `GOW3_PATCHES`). The
"80 FPS dips in fights" at 165 are frames whose GPU work exceeds 6.06 ms (two ticks = 82.5 FPS on our present
stamps); the user reports the same dip size at higher ticks, which our present stamps cannot see (open). Fights load
the GPU (effects, many animated enemies) unlike the panning tests: next, a frame capture in the fight.
New: `gpu_ready` trace stage (`Presenter::TraceGpuReady`, timeline semaphore of the frame's copy), analysis
`gpu_ready_analyze.py` (scratchpad).

### T-P960: high tick + limiter, on top of the T-P120 settings (2026-10-23, trace `flip-trace-25.csv`)

The user's idea: a much higher tick, paced on every Nth. `present_tick_hz=960`, `fps_limit=120` (8 ticks a slot),
`present_precise_limit=1`, precise tick, present on the vblank thread, Immediate (`vsync=0`). A frame that misses
its slot leaves at the next ~1 ms tick instead of a whole slot later. **Fight (last 40 s): no 16.7 ms frames (the
120 tick's exact-60 dips), 34 frames of 9-12 ms, 1% low 107.6, 0.1% low 100.5; presents 8.33-8.34 ms in most 5 s
windows. The user: matches, dips ~95 FPS instead of 60.** GPU finish times spread more than the presents (1 in
10 GPU steps 10-11 ms in the fight): the fight's GPU cost, next to profile (`GOW3_GPU_PROFILE=1`). Cost: the
precise tick spins ~0.7 of every 1.04 ms (two thirds of a core); to lower (`GOW3_TICK_SPIN_US`, or the precise
tick off at 960 since the precise limiter times the slots).
This is the 480-tick-plus-limiter design that failed before, now with exact timing and no hand-off: the old failure
was the coarse timing around it, not the high tick itself.

### T-F1: what makes the fight's frames slow (2026-10-25; logs `t-p960-gpu-profile-5`, `t-p960-slow-frames`)

GPU profiler (fixed: frames now end at the first draw of a display pass without the upscaler's motion vectors; the
results are read without waiting; the blocking read stalled the boot twice): averaged over 5 s the GPU was not
busier in the fight (~370-475 ms of GPU a second vs ~500 calm): the cost sits in single frames. Slow-frame lines
(`GOW3_SLOW_FRAMES=1.15`, 960 tick + 120 limit), last 45 s, burst of 24 slow frames at the kills (9.6-11.6 ms):

| Per frame (median) | Ordinary | Slow (kills) |
|---|---|---|
| Draws | 1,574 | 2,300-2,700 |
| **Render passes ended** | **326** | **560-660** |
| **Stage B busy** | 44% | **92-97%** |
| Stage A draining (waiting on stage B) | 0.8 ms | 4-6 ms |
| Uploads (buffers / images) | 3.2 / 0 MB | 2-3.7 / 0-3 MB |
| Readback / host-copy waits, protection faults | 0 / 0.07 ms, 0 | 0 / 0.04-0.15 ms, 0 |
| Render-state / texture-set misses | 322 / 106 | 560-650 / 220-290 |

Not copies or waits (the user's PCIe hypothesis does not fit): **stage B saturates on the death effects' extra
draws and nearly doubled render passes (~4 draws a pass)**; each break is a render-state miss on the CPU and, at 4K,
a store and reload of the targets on the GPU (the user's 95%+ GPU usage for single frames). Inference to check:
effects that sample attachments (soft particles reading depth) end the pass per draw. Next: `GOW3_PERF_DIAG=1` now
also prints "Pass switch: <what differed>" and "Pass reopened on the same targets after an end at <file:line>"
(`Scheduler::BeginRendering` / `EndRendering`).

### T-F2: which barriers break the passes (log `t-f2-barrier-breaks`, trace 33)

`GOW3_PERF_DIAG` "Barrier ended a pass" (site, stages, image layouts). Nearly all from the draw path
(`vk_rasterizer.cpp:1074`, `runtime.FlushBarriers()` after `BeginRendering`). Kill window vs the earlier windows:
barrier breaks 157k vs 89-131k a 5 s window, same-target reopens 68k vs 20-48k. Growth is the **depth buffer**:
DepthReadOnlyStencilAttachment -> DepthStencilReadOnly (2-3 images) ~64k (from ~120), DepthStencilReadOnly ->
DepthStencilReadOnly with no layout change (depth tests -> shader reads) ~36.5k (from ~90), DepthStencilReadOnly ->
DepthAttachment ~21.8k, color attachment <-> shader read ~15k. Many also carry a memory barrier "from all-graphics"
(cause not known yet). `Image::GetBarriers` skipped a barrier only for an identical layout *and* access mask.

Step 1 (`GOW3_SKIP_READ_BARRIERS=1`, off by default): a read after a read in the same layout records no barrier;
the reads are merged into the image's tracked state (access mask and stages) so the next barrier (a write or a
layout change) waits for all of them. Step 2 (to do): sample depth in DepthReadOnlyStencilAttachment instead of
switching layouts. A remaining memory barrier still breaks the pass, so step 1 alone cannot remove every break.

**T-F3 / T-F4 (`GOW3_SKIP_READ_BARRIERS=1`).** T-F3 barely changed anything: read-only depth attachments were
recorded with DepthStencilAttachmentWrite access, so no read followed a read (`63cfac3` records them as reads under
the switch). T-F4: the DepthStencilReadOnly -> DepthStencilReadOnly barriers are gone; same-target reopens 5.7-17k
in calm windows (from 20-48k) and 28k in the kill window (from 68k); barrier breaks 77-98k / 126k (from 89-131k /
157k); slow frames' stage B busy 82% (from 92-97%). The user: no visual issues, all death particles and effects
present. Remaining in the kill window: DepthReadOnlyStencilAttachment -> DepthStencilReadOnly ~42.7k (to sample
depth), DepthStencilReadOnly -> DepthAttachment ~19.8k, color attachment <-> shader read ~18k, barriers with a
memory barrier "from all-graphics" ~18k.
Step 2 (`GOW3_DEPTH_SAMPLE_IN_PLACE=1`): a depth-aspect sample of an image in DepthReadOnlyStencilAttachment keeps
that layout (Vulkan allows shader reads of the depth aspect there) and records the read in the tracked state
(`Runtime::SampleDepthInPlace`; the view's aspect: `ImageView::view_aspect`, `TextureSetEntry::depth_aspect`).

**T-F5 (both switches): the barrier moved.** DepthReadOnlyStencilAttachment -> DepthStencilReadOnly gone, but
DepthReadOnlyStencilAttachment -> the same layout (~43k a kill window, from in-place reads to the next stencil
attachment use) took its place: barrier breaks 134k, same-target reopens 36k. Per-image tracking saw "write after a
shader read"; the read was of the depth aspect (never written in this layout), the draw writes stencil only, and
attachment accesses between draws are ordered by rasterization order. `Runtime::KeepStencilAttachment` (same
switch): re-binding as that attachment after only attachment use and in-place depth reads records no barrier.
Left after it: depth attachment <-> depth read-only (~50k a kill window): depth written by one draw and sampled by
the next, a real dependency.

**T-F6 (all three barrier changes, diagnostics on):** barrier breaks 48-70k calm / 84k kill window (from 89-131k /
157k), same-target reopens 4.4-6.5k / 11.7k (from 20-48k / 68k); slow frames' stage B busy 48% (from 92-97%); kill
window GPU finish after the CPU side median 4.9 ms (from 6.7). The user: no artefacts.
**T-F7 feel run (`-NoStats`, trace 38), last 45 s (the fight):** 120.0 avg, **1% low 119.7, 0.1% low 117.4, no frame
over 10 ms** (T-P960 before the barrier changes: 107.6 / 100.5, 34 frames of 9-12 ms). The user: no issues, the
fights' dips gone, GPU usage steady instead of 60 -> 100% spikes (fans no longer ramp). **Now the default**
(launcher / in-game menu: Performance patches > "Fewer render pass breaks", `perf_lean_barriers`; off:
`GOW3_SKIP_READ_BARRIERS=0`, `GOW3_DEPTH_SAMPLE_IN_PLACE=0`).
**RTSS limiter in the same run (from ~210 s):** screen intervals 8.33/8.33-8.34 ms (1%/99%) instead of 7.5-7.8 /
9.0-9.2; the present call took 8.08 ms (RTSS waits inside it) and our limiter waited 0. Our releases in this mode
(960 tick + precise limiter) spread 7.6-9.1 ms: the slot wait spun only the last 0.1 ms on the ~0.5 ms timer grid
(the pacer got 0.7 ms in T-L5d, this path never did). Fix: the limiter's slot wait spins `GOW3_PACER_SPIN_US`
(default 700 us); to test against RTSS.

**T-F8 (limiter slot spin 0.7 ms, no RTSS, trace 39):** our releases 8.03 / 8.63 ms (1% / 99%; from 7.6 / 9.1),
screen 7.8-7.95 / 8.73-8.85; RTSS switched on at ~100 s: screen 8.33 / 8.33, present call 8.07 ms. The user: better,
not near RTSS, which raises the 1% and 0.1% lows themselves. With RTSS the vblank thread is blocked inside the
present until the slot, so no ticks or vblank events reach the game during the wait (the game's submits 8.0-8.6 ms
apart vs 7.5-9.1 with our limiter, whose 960 Hz ticks run on during the wait). Next (`GOW3_LIMIT_HOLD=1`, with the
precise limiter): a frame waiting for its slot holds the vblank thread until the slot, as RTSS does. Also open:
`SetCurrentThreadRealtime` is a no-op on Windows (normal priority).

**T-F9 (`GOW3_LIMIT_HOLD=1`, trace 40; RTSS on for the first ~minute by accident, then off; last minute a still
camera in a light area):** RTSS part: screen 8.33 / 8.34. Hold part: **our releases exact (8.333 / 8.334 ms)**, the
game's submits 8.0 / 8.6, but release -> presented 0.35 ms +- 0.25 and screen 8.05-8.11 / 8.56-8.61. The user: still
not near RTSS. The variable work after our release (PrepareFrame copy, acquire, blit, submit) lands on the frame
times; the RTSS limiter waits inside vkQueuePresentKHR, after that work, which absorbs it. Next
(`GOW3_LIMIT_IN_PRESENT=1`, with the precise limiter): frames are released when they arrive and
`Presenter::Present` waits for the slot (high-resolution timer + 0.7 ms spin) right before vkQueuePresentKHR,
outside `submit_mutex`; with presents on the vblank thread it blocks it as RTSS does.

**T-F10 (`GOW3_LIMIT_IN_PRESENT=1`, trace 41):** the present call took 8.06 ms (the wait now sits where RTSS's
does), our limiter waited 0; static camera, light area: screen 8.11 / 8.33 / 8.56 ms (1% / median / 99%), 1,087 of
3,589 frames off by > 0.1 ms, alternating long/short; the user: 0.1% low 115, 1% 117, small bumps between flat
stretches. With RTSS on later in the run: every interval 8.33 (max 8.34). Taking `submit_mutex` after the wait
(another thread mid-submit) is the likely +-0.2 ms; now the last 0.5 ms are spun holding it, and a
`present_start` trace stage marks the vkQueuePresentKHR call. **Open: the user saw intermittent flicker in this run
(RTSS on) that was not there before;** the RTSS part's pacing is exact, so a rendering cause (the barrier changes,
default since T-F8) is the first suspect; test with `perf_lean_barriers=0`.

**T-F11 (`GOW3_LIMIT_IN_PRESENT=1` + submit_mutex fix, RTSS off, trace 42):** vkQueuePresentKHR now *starts*
exactly every 8.333 ms (0 frames off by > 0.1 ms; the alternating bumps are gone), but the call takes 0.22-0.48 ms
(driver, DXGI layering), so it *returns* 8.14 / 8.53 ms apart; the user: 0.1% lows 115-116 in a light area and a
**persistent flicker** (also seen with RTSS stacked in T-F10; not seen with RTSS and the earlier limiter). Waiting
between the frame's acquire/blit and its present is rejected. RTSS's presents *return* 8.33 apart: it waits after
the driver's call (back edge), pacing the game's next frame start rather than the present. Next:
`GOW3_LIMIT_AFTER_PRESENT=1` (with `GOW3_LIMIT_IN_PRESENT=1`): present at once, then hold until the slot.

**T-F12 (`GOW3_LIMIT_IN_PRESENT=1` + `GOW3_LIMIT_AFTER_PRESENT=1`, back edge, trace 43):** every stage matched the
RTSS run (present returns 8.333 / 8.334, game submits 8.04 / 8.63, GPU finish 8.07 / 8.57); the user: about the same
as T-F11, larger intermittent spikes, **still flicker**, the graph less steady than with RTSS. Both present-side
modes rejected (they stay opt-in). NVCP present method in all of these runs: "Prefer native" (the user set it days
ago; Auto chose non-deterministically), so the 0.2-0.5 ms inside vkQueuePresentKHR is the native path, not DXGI.
**T-F13 (no new present switches, `GOW3_PACER_SPIN_US=100`, RTSS limiter at 119, trace 44):** our 120 limiter never
waits; present returns 8.402 / 8.404 ms (1% / 99%), 1% / 0.1% lows 118.8-119.0 / 117.0-118.9, no flicker. RTSS at 120
flickers: two limiters at the same nominal rate with independent phases fight (the user). Inference: our own modes
have two clocks too, the limiter's slots and the 960 Hz precise tick, out of step after each hold. Next: one clock
(tick and slots from the same grid). Also noted: CPU usage alternating 12-25% (spinning waits; to measure).

**One clock (`GOW3_ONE_CLOCK=1`, with the precise tick and a frame limit):** the tick runs at limit x N (N =
round(tick Hz / limit) whole ticks a frame: 960 / 120 -> 8, 1041.667 us; a 119 limit gives a 952 Hz tick); frames
are released on ticks only, on the Nth tick after the last release, else on the first tick a late frame arrives
(the count restarts there, the grid never moves). No separate slot timer, no holds, no waits between ticks.

**T-F14 (`GOW3_ONE_CLOCK=1`, RTSS off, trace 45): rejected.** The user: the worst of the night, 1% lows 90-107,
0.1% ~110, many stutters. Release intervals 8 ticks 3,523, **7 ticks 41, 9 ticks 36** (a frame never leaves after 7
by design): ticks with a release do the inline present (0.3-0.5 ms) and the vblank events inside a 1.04 ms tick;
when that overran the next tick, `next_tick = max(next + period, now - period)` re-anchored the grid, so the "one
clock" moved. Releases 7.82 / 8.85 ms, presents 7.78 / 8.88. A real single clock would need a grid that never
re-anchors and the present off the tick thread (or N = 2 at 240 Hz). Capped-play recommendation for now: RTSS at
119 with our limiter at 120 (idle), T-F13.

T-F14 split (the user: RTSS at 119 from ~415 s): one clock alone 1% lows 101-113 per 10 s window, releases 7 or 9
ticks apart in every window; with RTSS on top the screen held 8.40 / 8.40 / 8.40 ms (1% lows 118.9-119.0) while our
releases still jittered (7.7 / 9.1): RTSS's wait inside the present absorbs everything before it.
**One clock v2:** a fixed grid (tick k at start + k x period; a missed tick skips to the next grid point, same
phase), slots counted in grid ticks (`last_release_tick + N`), so a slow loop pass (the inline present) cannot move
a slot.

**T-F15 (one clock v2, RTSS off, trace 46): rejected.** The user: 1% / 0.1% lows better than v1, not RTSS's locked
pace, and real frame-time dips became full stalls. Releases still 7 or 9 ticks apart, in equal numbers (11-26 each
per 10 s window; releases 7.56-7.83 / 8.84-9.11 ms): with a fixed grid that only happens when a release is
processed about a tick late, i.e. the tick thread did not get the CPU on time. `SetCurrentThreadRealtime` is a
no-op on Windows (normal priority); RTSS hides such delays by waiting inside the present. Next: real priority for
the tick/present/pacer threads (time-critical + MMCSS), behind a switch, tested with the hold mode.
`GOW3_PRESENT_PRIORITY=1` (off by default): the vblank, pacer and swap threads call `SetThreadPriority(...,
THREAD_PRIORITY_TIME_CRITICAL)` and join the MMCSS "Games" task at high priority (`avrt.dll` loaded at run time);
the log prints the result per thread. First test: with the hold mode (`GOW3_LIMIT_HOLD=1`), RTSS off.

**T-F16 (`GOW3_LIMIT_HOLD=1` + `GOW3_PRESENT_PRIORITY=1`, trace 47): our best capped mode without RTSS.** The vblank
thread ran time-critical + MMCSS Games (log). Our releases 8.333 / 8.334 ms (1% / 99%), present calls start 8.313 /
8.354; the driver's present call takes 0.27-0.57 ms, so presents return 8.06-8.15 / 8.51-8.60 apart; 1% lows 112-117,
0.1% 85-116. The user: significantly better than the other runs, still behind RTSS; frames sometimes faster than
120, then slower. RTSS (119) on top from ~375 s: returns 8.401 / 8.405, lows 118.4-119.0 / 112.5-118.9. **The user's
RTSS framerate limiter is set to "NVIDIA Reflex"**: the driver paces, so its own present time is absorbed.
Next (`GOW3_REFLEX=1`): VK_NV_low_latency2 (+ present ids), `SwapchainLatencyCreateInfoNV`, the frame limit as
`minimumIntervalUs` (`vkSetLatencySleepModeNV`, re-applied on swapchain recreation); after each present
`vkLatencySleepNV` + a timeline semaphore wait before the flip completes (the game's next frame starts there);
markers: simulation start after the sleep, simulation end / render submit / present start and end around the
present. Our own limiter steps aside (one clock); trace stage `reflex_wake`.

**T-F17 (`GOW3_REFLEX=1`, RTSS off, trace 48):** Reflex enabled, the driver took the 8,333 us interval (log); the
user: 1% / 0.1% lows ~110 / ~108, behind RTSS. With our limiter off for Reflex the frames fell back to release on
the next 960 Hz tick (queued -> release 0.85 ms, up to 1.13; releases 5-9 ticks apart), so presents reached the
driver 7.91 / 8.91 ms apart; the driver's sleep woke 8.32 / 8.73. Fix: immediate flips in Reflex mode (frames go
to the driver when queued, as into RTSS's hook).

**T-F18 (`GOW3_REFLEX=1` + immediate release, RTSS off, trace 49):** our side at RTSS level: frames queued and
released 8.26 / 8.41 ms (1% / 99%), present calls 8.25 / 8.42, **the driver's sleep ended 8.327 / 8.340 ms**; the
user: 1% low 117, 0.1% 114, and a drop to ~60 every couple of seconds, not in our trace (max 9.58 ms).
PresentMon (`research-data\pm-reflex.csv`, the user's capture, last ~18 s): present mode **Hardware: Legacy Flip**
(0.5 s composited at the start: the PresentMon window took focus); present calls 8.14 / 8.52 ms, **display changes
8.18 / 8.48 ms (max 9.55, none over 12)**, present to display a constant 3.76-4.10 ms: the display follows the
present calls. Next: the same capture with RTSS's Reflex limiter at 119 (our Reflex off) for the target numbers.
Tool: `research/tools/presentmon-capture.ps1 -Tag <name>` (10 s delay, 30 s, elevated relaunch; output in
`research-data`).

**T-F19 (RTSS Reflex limiter at 119.98, our Reflex off; PresentMon `research-data\pm-rtss119.csv`):** display side
equal to our Reflex mode (T-F18, `pm-reflex.csv`): screen intervals 7.94 / 8.34 / 8.70 ms (1% / median / 99%; ours
7.98 / 8.33 / 8.70), on-screen 1% / 0.1% lows 109.6 / 102.7 (ours 111.4 / 104.2), frames over 9 ms 18 (ours 12),
both Hardware: Legacy Flip. **The user: RTSS's own statistics were measured at "frame start"** (which Reflex makes
exact); switched to "frame present" they match these numbers and show the occasional present-timing gaps behind the
flicker. 119.98 flickers visibly, 119 barely. So: frame starts are steady in both, the present (and display)
timing varies +-0.2-0.4 ms with the frame's render time.

**T-F20-T-F23 (our Reflex, RTSS measuring "frame present"):** similar present metrics to RTSS's, but intermittent
one-frame blips (traced: a present call starting 3 ms late after an on-time release; stage B 12-17 ms for single
frames; once the driver's sleep ~7 ms long) and persistent low-amplitude spikes (120 -> 116) with a matching
flicker, up and down (one late present, the next early). The user's controller switching off caused 180-320 ms
stalls (input bug, to fix). Process CPU swings 10-27% on a ~1 s rhythm; a weak ~375-500 ms rhythm in the present
wobble; the perf-stats sampler is not running (overlay items off). Power mode "Best performance" (plan still
Balanced): no change. RTSS's Reflex limiter on top clears the flicker. RTSS sleeps in its present hook, i.e.
before the real present; ours slept after it. Next: `GOW3_REFLEX_SLEEP_BEFORE_PRESENT=1` (the driver sleep inside
`Presenter::Present`, right before vkQueuePresentKHR), first with the flip trace off (its flushes run on the
presenting thread).

**T-F24 (`GOW3_REFLEX=1` + `GOW3_REFLEX_SLEEP_BEFORE_PRESENT=1`, Immediate, RTSS limiter off; the trace stayed on:
PowerShell drops an empty variable, so run.py took `present_frame_trace=1`; copy in
`research-data\flip-trace-55-reflex-sleep-before.csv`):** present calls start **8.329 / 8.333 / 8.337 ms** (1% /
median / 99%; 4 of 3,600 off by > 0.1 ms); the driver's call takes 0.18-0.55 ms, so returns 8.06 / 8.61; the GPU is
done ~3.7 ms before each present. The user: **best 1% / 0.1% lows so far (up to 118)** and the steadiest average
from our own pacing, but the flicker is still bad. Next suspect: Immediate (flips mid-scanout under G-Sync);
test VSync on (FIFO), as NVIDIA recommends for G-Sync with a cap below the refresh.
