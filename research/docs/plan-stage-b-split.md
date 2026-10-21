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
