# Fix inventory: what this port fixed and how to take it (2026-11-02)

Parent: [START-HERE.md](START-HERE.md). Evidence per fix: [plan-frame-pacing.md](plan-frame-pacing.md) (T-* of
2026-10-18/19, N-1 to N-32), [plan-stage-b-split.md](plan-stage-b-split.md) (T-W*, T-HZ*, T-S1a-c, T-Z*, T-P1-5,
T-M1-3, T-L*, T-V120*, T-P120, T-P960, T-F1-27, T-R1-3), [plan-baseline-issues.md](plan-baseline-issues.md),
[plan-0102.md](plan-0102.md) (T-0102-*); background: [study-frame-cost-2026-10-29.md](study-frame-cost-2026-10-29.md),
[study-upstream-commits.md](study-upstream-commits.md), [study-live-switches.md](study-live-switches.md); PR plan:
[patches-and-pr-proposal.md](patches-and-pr-proposal.md). Covers `lab` up to 2026-11-02 (`8187eb0`).

## For other ports: how to read this

**Lineage.** shadPS4's video core -> the Bloodborne PC port (deadinside28, `C:\Users\Ty\MediEvil\bloodborne_pc`) -> the
God of War III PC port -> the MediEvil PS4 port (bc160201983, `upstream`) -> this branch (`lab`). Ports with the same
lineage share `gpu/shadps4/` (PM4 decode = stage A, draw recording = stage B, the Vulkan recorder, buffer and texture
caches), `gpu/shim/` (toggles, settings, overlay) and the HLE runtime in `src/`, so most commits below apply with
little change. MediEvil is UE 4.21; everything was measured on one machine (RTX 4090 + Ryzen 7 7800X3D, Windows,
NVIDIA driver, mostly 4K native), so AMD / Intel GPUs and Linux are untested unless said.

**Classes.**
- **G (general):** shadPS4 video core or loader / runtime code that does not rely on MediEvil's behaviour.
- **C (conditional):** general code, but it relies on an assumption that held for MediEvil (named in the row). Check
  the assumption in your game before turning it on by default.
- **M (MediEvil-specific):** UE4 config in the override pak, eboot patches, hooks into the game's code, tuned sizes.
  The idea may transfer to another UE4 game; the code and addresses do not.

**Origin.** "ours" unless the row names the Bloodborne port, upstream MediEvil (bc160201983), the Uncharted port
(through upstream) or shadPS4. Bloodborne code is GPL-2.0-or-later like this port: keep its headers and name its
commit (patches-and-pr-proposal.md section 3). Novelty was checked on 2026-10-20 against the commit messages and the
Bloodborne source (its rwlock registry walks a list, its address-space hint is a different mechanism, its dirty
registers are scanned bit by bit, it has no kept pool views, stencil-plane copy, undefined-target or volume-view code);
later "ours" rows rest on the commit messages and the docs.

**Evidence.** A test ID (find it in the docs above) with the numbers the docs record; "the user" = confirmed by the
user in game. Numbers are the docs' own; a value marked (unverified) was not measured.

**Switches.** Almost every fix has one, so you can A/B it in your port:
- Environment variables `GOW3_*` (`=0` turns a default-on fix off unless said).
- `medievil.ini` keys: the launcher's Performance patches (`perf_*`, `scripts/graphics_options.py` `PERF_SWITCHES`
  turns `perf_*=0` into the variables), presentation keys (`present_*`, `PRESENT_DEFAULTS`), graphics keys.
- The live toggle file (`GOW3_TOGGLE_FILE=<path>`, re-read every 250 ms, `src/runtime_memory.c`): a decimal mask, a
  set bit switches that fix **off**; bits 32+ are `Gow3Toggle` in `gpu/shim/gow3_toggles.h`. An optional second number
  is `Gow3Toggle2` (bulk descriptor copy 1, sampler memo 2, ring reuse 4, U2 direct arena writes 8, end-of-pipe
  semaphore poll interval 16: study-stage-b-per-draw.md C1; recorder lean mode 32, on at <= 8 threads:
  impl-deck-cpu-bundle.md). Bit 31 has two meanings (narrow image
  stages and the M1 pool views): writing 2147483648 switches both off.
- `research/tools/toggle-ab.ps1` flips a bit every 20 s in one launch; `toggle_ab_sections.py` /
  `flip_toggle_ab.py` split the frames by state. Same-launch A/B is how N-30 to N-32 were measured.

**Paths** are under `gpu/shadps4/` unless they start with `src/`, `gpu/shim/`, `scripts/`, `patches/`, `run.py` or
`launcher/`. **Judge smoothness only from runs without the diagnostics** (`GOW3_SLOW_FRAMES`, `GOW3_SECTIONS`,
`GOW3_PERF_DIAG`, the flip trace): they changed timing in N-8, N-23 and N-32.

## 1. GPU sync, barriers and render-pass breaks

Background: Nsight Graphics GPU Trace (N-2) showed the front end waiting for idle 44-66% of each fight frame, with
312-487 GPU drains a frame; `GOW3_PERF_DIAG` (N-3) put them at ~174 barriers and ~310 render-pass ends a frame, almost
every barrier with `ALL_COMMANDS` on one side. In fights, kills' effects doubled the pass breaks (326 -> 560-660 a
frame, stage B 92-97% busy, T-F1). The drain *count* did not move with narrower masks (N-5): the driver drains at
layout transitions and pass breaks whatever the masks. What helped was removing barriers and breaks.

| Fix | What it does | Evidence | Commits · main files | Switch (default) | Class |
|---|---|---|---|---|---|
| **Fewer render pass breaks** (3 parts) | (a) a read after a read in the same layout records no barrier; the reads merge into the image's tracked state so the next write waits for all; read-only depth attachments are recorded as reads (they were writes, so (a) never applied). (b) depth sampled while bound read-only stays in `DepthReadOnlyStencilAttachment` (Vulkan allows depth-aspect reads there) instead of a layout switch. (c) re-binding the stencil attachment after only attachment use and in-place depth reads records no barrier | T-F6: barrier pass-breaks 89-131k / 157k -> 48-70k / 84k per 5 s (calm / kill window), same-target reopens 20-48k / 68k -> 4.4-6.5k / 11.7k, slow frames' stage B busy 92-97% -> 48%. T-F7 (fight, 120 cap, no stats): 1% / 0.1% lows 107.6 / 100.5 -> **119.7 / 117.4**, no frame over 10 ms; the user: no artefacts, GPU usage steady | `4b5d65c`, `63cfac3`, `48bede5`, `575c160`, `5ecf53b` (default) · `texture_cache/image.cpp`, `renderer_vulkan/vk_rasterizer.cpp/.h`, `vk_runtime.cpp/.h`, `texture_cache/image_view.cpp/.h` | `perf_lean_barriers` (on); `GOW3_SKIP_READ_BARRIERS=0`, `GOW3_DEPTH_SAMPLE_IN_PLACE=0` (b and c) | G |
| **Narrow image stages** (N-4) | sampled / storage image transitions wait for the reading shader stages (compute, or pre-rasterization + fragment) instead of `eAllCommands`, stored as the image's last stage; a read by another shader stage gets its own read-after-read barrier | N-5 (same spot, 5 frames a side): GPU frame mean 5.71 -> 5.22 ms, drains 208-214 vs 204-211 (unchanged). **Corrected in N-16: GPU active time -0.8%** (the rest was idle variance). No visual problems (the user) | `d6a1285` · `vk_rasterizer.cpp`, `texture_cache/image.cpp` | `GOW3_NARROW_IMAGE_STAGES` (on); toggle bit 31 | G |
| Depth writes only with the depth test on (N-6) | a depth target counts as written only with the depth test on or a pending clear (the pipeline already used the effective state); before, `depth_write_enable` alone chose the writable layout | N-6: ping-pong unchanged (those draws really write); kept as correct and free | `d6a1285` · `vk_rasterizer.cpp` | `GOW3_DEPTH_WRITE_EFFECTIVE` (on) | G |
| **E1** end-of-shader fences deferred | the end-of-shader label is written by the Vulkan recording thread after the copies queued before it, as end-of-pipe labels already were (`RecorderFences`); GDS stores stay synchronous | T-E1 (with W2): stage B's ordered tasks 0.52 -> 0.23 us per packet, ~5% more packets/s | `cb1da6c` · `amdgpu/liverpool.cpp` | `GOW3_EOS_DEFERRED` (on); part of `perf_gpu_data_no_waits` | G |
| **W2** targeted end-of-pipe wait | the end-of-pipe host-copy wait before `ProcessDownloadImages` waits only for copies reading the ranges the queued readbacks write (needs B1's source table, section 8) | T-P3: the full wait was ~21% of stage B; with E1, T-E1 as above | `3b30fe9`, `852ec13` (default) · `texture_cache/texture_cache.cpp`, `readback_queue.h` | `GOW3_EOP_TARGETED` (on); toggle bit 60; part of `perf_gpu_data_no_waits` | G |

## 2. Draw translation cost (stage B, per draw)

Stage B (the draw recording thread) was the limit in heavy views for most of this work (T-P3, T-HZ4f/g, N-17: ~4.3 ms
of a 4.8 ms heavy frame). Each row is measured per draw where the docs allow.

| Fix | What it does | Evidence | Commits · main files | Switch (default) | Class |
|---|---|---|---|---|---|
| **P1** dirty registers by word | dirty register blocks scanned as a 26-word array with `countr_zero` instead of testing 1,664 `std::bitset` bits per draw (libc++ has no `_Find_first`) | T-P1: heavy view 114-123 -> 124-131 FPS (~+8%), the user: "1% lows are improved" | `5cd037e` · `amdgpu/regs.h`, `vk_rasterizer.cpp` | none (same behaviour) | G (libc++ builds; neutral on glibc) |
| **Texture-set memo by region stamps, two-way** | a memoized texture set stays valid while the (un)registration counts of the 4 MiB regions of its images and T#s are unchanged (was: any image anywhere voided every set); stamps are checked only after a registry change; two slots per key | T-S1b: hit rate 83.6 -> 92.3%, generation misses ~15,300 -> ~48 a window; T-S1c: textures 0.396 -> 0.369 us per packet; T-texset-both-off: the user, lows worse without it | `641134c`, `2492874`, `61440d7` · `vk_rasterizer.cpp/.h`, `texture_cache/texture_cache.cpp/.h` | `perf_texture_memo` (on); `GOW3_TEXSET_REGIONS=0`, `GOW3_TEXSET_TWO_WAY=0` | G (the memo itself is inherited; the region stamps and two-way slots are ours) |
| **Bulk descriptor copy per push** | when every descriptor write of a push points into the rasterizer's `image_infos` / `buffer_infos`, the writes and each array are copied into the recording chunk once and the pointers rebased: 3 `RecordData()` calls instead of ~16 | N-30 (A/B in one launch, ~1,550 draws): descriptors 0.337 -> 0.217 us a draw, ~-0.18 ms of stage B a heavy frame | `e0c7566`, `ffd5d90` · `renderer_vulkan/vk_pipeline_common.cpp/.h` | `perf_lean_binding` (on, live); `GOW3_LEAN_BINDING=0`; toggle word 2 bit 1 | G · from the Bloodborne port (0.3) |
| **Sampler handle memo** | a per-thread direct-mapped memo keyed on the S#, border colour base, depth and LOD bias reuses a handle `GetSampler` returned in the current GC period (no XXH3 hash, no sampler mutex); the first use each period still touches the LRU | N-31 (A/B in one launch): samplers 0.126 -> 0.041 us a draw, ~-0.13 ms a heavy frame | `5c752e8`, `ffd5d90` · `vk_rasterizer.cpp`, `texture_cache/texture_cache.h` | `perf_lean_binding` (on); toggle word 2 bit 2 | G |
| **Stream-class table** | the buffer cache's small-buffer state by address was an `unordered_map` cleared at 262,144 entries (ring buffers bring new addresses every frame); now a fixed 65,536-entry direct-mapped table | N-8: `ObtainBuffer` in hitch frames 10-12 ms -> ~0.5 ms; the user: "a big improvement". A/B of the night's changes together (this, ring reuse, narrow stages, async logging): 197.2 -> 207.8 FPS, windows with a frame > 12 ms 4 of 11 -> 1 of 16 | `de30a39` · `buffer_cache/buffer_cache.cpp/.h` | none | G |
| **Constant-ring reuse** | within one guest command buffer (graphics queue), a constant range (address, size) bound again binds its first ring copy; the table resets at each command buffer and at `WRITE_DATA`, `DMA_DATA`, `WAIT_REG_MEM`, `MEM_SEMAPHORE`, `REWIND`; copies are held until the GPU completes their epoch. Needs the ring at 96 MiB (32 ran full) and the stamp fix (`ConstantRing::Stamp` never published the last stamp of a full ring, which then stayed full) | N-7: BAR writes ~17.8 -> ~4.0 MB a frame (81% reused). N-32 (A/B in one launch, no stats): 1% / 0.1% lows ~188 / ~153 vs ~179 / ~145; heavy-area 0.1% lows ~145 -> ~185 (`b46ff97`). History: N-14 (ring full at 32 MiB), N-15 (spikes, turned off, cause never found), N-16 (stamp fix) | `d6a1285`, `4038564`, `31a6c1a`, `32586bc`, `9fb2d6c`, `b46ff97` · `vk_rasterizer.cpp`, `vk_constant_ring.h`, `amdgpu/liverpool.cpp` | `perf_ring_reuse` (on, live); `GOW3_RING_REUSE=0`; toggle word 2 bit 4; `GOW3_CONSTANT_RING_MB` (96) | C: assumes a guest range's bytes do not change within one guest command buffer (the CPU wrote them before submitting); GPU-written ranges are skipped |
| **S2** small read-only buffers from the arena | a small read-only range the CPU has not written since its last upload is bound from the arena (no copy, no pass break); a CPU-written one is uploaded once outside a render pass; written again, it stays on the stream copy | T-S2b (with the HZB remap): heavy-view panning ~185 FPS, 1% ~150, 0.1% ~125, the user: best so far. Before: ~1.6 copies and ~5 KB per draw (~2 GB/s) | `8a0e908`, `57dc5c4` · `buffer_cache/buffer_cache.cpp/.h` | `perf_small_buffers` (on, live); `GOW3_STREAM_CLEAN=1` (run.py sets it for MediEvil only) | C: relies on page protection catching CPU rewrites; a range the CPU rewrites often costs a write fault once each. Off by default outside MediEvil |
| **GPU driver memory calls off stage B** | image memory freed on a worker (`vmaDestroyImage` can `vkFreeMemory` a block), buffer residency taken from 32 MB chunks a worker allocates ahead; both calls take the driver's lock, which GPU monitors (MSI Afterburner's NVAPI polling) held for up to 33 ms | trace cpu-osc-1: 11 lock waits / 62 ms in 20 s, max 33 ms, all but one on stage B; img-release-1: 1 / 1.5 ms, stage B in `D3DKMTDestroyAllocation2` 166 -> 0; t-rp1: 21 chunks, all but the first allocated ahead, heavy area with Afterburner as without | `b1dae86` · `texture_cache/image.cpp/.h`, `buffer_cache/buffer_cache.cpp/.h`, `vk_instance.cpp` | `GOW3_IMAGE_RELEASE_THREAD` (on), `GOW3_RESIDENCY_CHUNK_MB` (32; 0: old path) | G |
| ThinLTO build | `GOW3_LTO=ON` (CMake IPO, `-flto=thin`) in `build.sh` | N-29: stage B per draw median 4-7% better, mean ~3% worse: within noise; kept (no downside) | `ca5636f` · `build.sh`, `research/tools/build-pdb.sh` | `GOW3_LTO=OFF` | G (build) |

## 3. Memory sync, uploads and readbacks

The port tracks guest memory per page and protects pages the GPU wrote; a CPU write into such a page used to fault
and download it with a full GPU wait (`DownloadMemory` -> `Finish()`), the regular microstutter (T-SF1: ~40 a second,
up to 5.8 ms each).

| Fix | What it does | Evidence | Commits · main files | Switch (default) | Class |
|---|---|---|---|---|---|
| **D2 + D2b** precise CPU writes | the runtime keeps a bitmap of shader-written pages; guest libc `memcpy` / `memset` / `memmove` into them hand their bytes to `BufferCache::PreciseCpuWrite`: written through the backing view (no fault) and queued as exact bytes for the GPU copy before the next draw or dispatch. D2b: bytes so written leave the GPU-modified ranges, so the game's own inlined copies stop faulting | T-D2b / T-D2d: downloads 27/s -> 0, heavy-view stddev 1.1-1.5 -> 0.32-0.48 ms, p99 10.9-13 -> 8.6-9.5 ms; the user: "a significant improvement" | `1bc2dda`, `42a4943` (built on diagnostics `057afc6`, `537d86e`) · `buffer_cache/buffer_cache.cpp/.h`, `vk_rasterizer.cpp/.h`, `src/runtime.c/.h`, `src/runtime_memory.c`, `gpu/shim/gow3gpu.cpp` | toggle bit 63; `GOW3_PRECISE_WRITE_RELEASE=0` (D2b off) | G (writes by inlined guest code still fault). Idea after the Bloodborne port's `NotePreciseUpload` (no code taken: Bloodborne learns of the write afterwards, ours intercepts it before) |
| **D3** downloads on a readback queue | the submission tick of the newest GPU write is kept per 64 KiB page; a download waits only for that submission and copies on a second queue from a compute-only family | T-D3b: each download waits only for its write | `5a5d73f` · `buffer_cache/buffer.cpp`, `buffer_cache.cpp`, `vk_instance.cpp/.h` | `GOW3_READBACK_QUEUE` (on); part of `perf_gpu_data_no_waits` | G · from the Bloodborne port (0.3, `GpuWriteTicks`, `FastReadback`). Needs a compute-only queue family |
| **GPU write twins** | on a write fault into a page holding a few GPU-written bytes whose write is not done, the guest's copy of those bytes is kept as a twin and the page becomes CPU-modified at once (no download); uploads keep the GPU's bytes where the guest's still match the twin | T-T1: light-view stddev ~0.56 -> 0.06 ms, ~200 FPS 1% lows | `2a40666`; limit 1 -> 16 KiB in `94b31c3` · `buffer_cache/buffer_cache.cpp/.h`, `src/runtime_memory.c` | `GOW3_GPU_WRITE_TWINS` (on), `GOW3_GPU_WRITE_TWINS_MAX` (16384) | G · from the Bloodborne port (0.4, `BB_GPU_WRITE_TWINS`); the 16 KiB limit is ours |
| **Write faults without unrelated waits** (N-13) | a page whose GPU bytes are finished downloads alone without waiting (was: a 512 KiB window waiting for the newest GPU write anywhere in it); a page with no GPU-written bytes needs no download at all | N-13 (level-select map, bone-matrix writes sharing a page with a 20-byte GPU-written block): t-map3 0.1-0.6 ms of waits a second vs 127-158 ms when it oscillated 100 <-> 200 FPS | `94b31c3` · `buffer_cache/buffer_cache.cpp` | `GOW3_FAULT_PAGE_DOWNLOAD` (on) | G |
| **C3** undefined targets after an unmap | images created at an address a guest unmap freed are flagged Undefined; a render / depth target binding drops the flag and the dirty bit without uploading; any other first use uploads as before | T-C3b: torch-into-view spikes 19 -> 7-12 ms (live A/B), the user: "Massive improvement" | `1024224`, `d9949cb` · `texture_cache/texture_cache.cpp/.h`, `image.h`, `vk_rasterizer.cpp` | toggle bit 61 | C: assumes memory freed by an unmap holds nothing worth uploading into a render target |
| **New images undefined** (N-11) | every new image is Undefined until its first use; a first use as a render / depth target skips the upload (C3's path), other first uses upload | N-11: t-up5 no 4K render-target uploads (32-64 MB each) in the refresh list, gameplay image upload peaks 6.5-8.1 MB/s vs 12.5-14.1 (t-up3b); no visual problems (the user) | `40a7825` · `texture_cache/texture_cache.cpp/.h`, `vk_rasterizer.cpp` | `GOW3_NEW_IMAGES_UNDEFINED` (on) | C: assumes a new image first bound as a render / depth target has no CPU-written contents the pass needs (no load of CPU data under it) |
| **Park targets a guest unmap frees** (N-24) | a render / depth target (not aliased, not a scene proxy, <= 128 MiB) freed by a guest unmap is parked (unregistered, image and views kept) and taken back by the next image with the same address and geometry (Undefined, as C3); evicted after 10 s or over 768 MiB | t-park1: 95-100% of parked targets taken back, up to ~200 MiB parked; t-park2 (clean): the user: "the big spikes there are gone ... the .1% lows in the heavy area are the best they've been" | `fdd12e8` · `texture_cache/texture_cache.cpp/.h` | `GOW3_PARK_UNMAPPED` (on) | C: as C3, plus UE4's render-target pool returning targets at the same address; sizes tuned for MediEvil at 4K |
| **V3** volumes through 2D array views | a 2D render target with one layer per slice of a same-sized 3D image renders through an attachment-only 2D array view of the volume (no extension), instead of `ResolveOverlap` rebuilding and re-uploading it on every switch | T-C1: ~9,760 expands and ~31 GB uploaded per 5 s before; T-V3: map ~70 -> ~200 FPS, heavy-view 1% lows ~60 -> ~100, uploads 0-15 MiB per 5 s | `1f18217` · `vk_runtime.cpp`, `texture_cache/image.cpp`, `image_view.cpp`, `texture_cache.cpp` | toggle bit 58; `GOW3_VOLUME_VIEWS=0` (upstream's switch, also off) | G. Upstream 1.3.4 reached the same idea independently (its commit 2026-10-19 07:24 UTC, public; ours 2026-10-18 19:33 UTC, first pushed 2026-10-19 22:20 UTC; no shared code: sampled views, its own switch, `ClearVolumeSlices`, the 1.3.5 AMD fallback). Merged: our view rule, their exact slice clears |
| R1 readback submitted when recorded | a readback copy recorded at an end of pipe is submitted once the readback locks are released, not at the frame's end | T-HZ2b/T-HZ3: only acts with precise readbacks (HZB); no gain measured in normal play | `088d8c2`, `9dea0af` · `vk_rasterizer.cpp`, `texture_cache/texture_cache.cpp/.h` | `GOW3_READBACK_SUBMIT` (on) | G |
| D1 unmap discard | a guest unmap drops GPU-written buffer data instead of downloading it | T-D1b: never triggered in MediEvil | in `057afc6` (mixed with diagnostics) · `buffer_cache/buffer_cache.cpp` | toggle bit 56 | C: assumes the guest never reads GPU data back from memory it unmapped |

## 4. Runtime (loader, HLE, guest memory)

| Fix | What it does | Evidence | Commits · main files | Switch (default) | Class |
|---|---|---|---|---|---|
| **R2 + R3** | rwlock handles validated through an open-addressing hash set (was a list walk under one mutex, 11.8% of the main thread); the memory pool's free-page search starts at a per-chunk hint | T-R23: heavy view median FPS 81.2 -> 94.7 (+17%), p99 -13% | `cf9e201`, `2097554` · `src/runtime_rwlock.c`, `src/runtime_memory.c` | none (same behaviour; unit test `tests/test_rwlock_registry.c`) | G |
| **M1 + M1b** pool views kept across a decommit (Windows) | a decommit keeps the guest-memory view (no access) and a commit at that address takes the same pages back; pages of kept views are handed out last | T-M1, T-M1b: the 2-5 s kernel trim episodes gone (views ~13,400 -> ~3,400) | `40a416a`, `3c5704e` · `src/runtime_memory.c` | toggle bit 31 (shared, see the intro) | G (Windows) |
| **Decommit keeps access** | the kept view keeps its access instead of flipping to `PAGE_NOACCESS` (each flip rewrites page entries and flushes TLBs on all cores; UE4 decommits and recommits the same pool pages ~5-6 times a frame) | T-M2: decommits ~320 -> ~41 ms per 5 s, table lock held exclusively ~330 -> ~105 ms; T-M3: the user, "better with it on" | `6158dad`, `a9f6ac8` · `src/runtime_memory.c` | `perf_cheap_frees` (on, live); `GOW3_DECOMMIT_KEEP_ACCESS=0` | C: a read of decommitted memory sees stale data instead of faulting; assumes the game never relies on that fault |
| **Lock-free region reads** | `runtime_memory_clamp` and `runtime_memory_region` search the region table without the lock (sequence count odd only while entries change; table allocated once at 131,072 entries); they spin through an edit for up to ~100 us before taking the lock | T-P4: stage A `CollectRingBindings` 13.4% -> 7.1%, lock waits 6.4% -> 3.0%; T-P5: the user, "quite good, the best it's been", panning included | `e15a859`, `45eca39`, `76de0f5` · `src/runtime_memory.c` | `perf_lockfree_memory` (on; only this part live); `GOW3_OPTIMISTIC_CLAMP=0` | G |
| L2 region caches during writes | the GPU threads' region caches are kept per 16 MiB bucket, bumped before the host mapping changes, so they answer for other ranges while a writer holds the lock | T-L2: region-cache lock waits halved, no FPS change | `03c363b` · `src/runtime_memory.c` | `perf_lockfree_memory`; `GOW3_REGION_BUCKETS=0` (restart) | G |

## 5. Correctness and stability

| Fix | What it does | Evidence | Commits · main files | Switch (default) | Class |
|---|---|---|---|---|---|
| **L5** drain before every queued command | `Liverpool::ProcessCommands` drains stage B before each queued command (a command queued after the first check ran on stage A beside stage B: freezes, and a draw split across command buffers crashing the NVIDIA driver) | T-race (pipe off ~9 min clean vs freeze at ~2 and crash at ~4 min); T-L5: ~19 min, no fault; no C1 / H1 since; cost ~37 drains/s | `bc0e1d6` · `amdgpu/liverpool.cpp` | none (`GOW3_DRAW_PIPE=0` avoids the race) | G · from the Bloodborne port (0.3, `4d4d208`). Complements upstream's `SyncWithDrawRecorder` (section 8) |
| **V4** stencil plane writes | GCN keeps a depth buffer's stencil in its own plane; a compute dispatch that rewrites a whole known plane (starts at it, within 1/16 of its size) marks it and the next bind copies the plane into the image's stencil aspect | T-V4 / T-V4b: the box shadows gone (live A/B), RenderDoc shadow mask clean | `769de12`, `423efc7` · `vk_rasterizer.cpp/.h`, `vk_runtime.cpp/.h` | toggle bit 59 | C: exact only for uniform fills (UE4's stencil clear); other shader-written stencil contents are logged, not converted |
| **HZB small mips rendered** (N-22) | `ImageInfo::MipOf` accepts a 1D-thin render target for a mip that the macro-tiled parent's own layout made micro-tiled as a view of that mip; before, mips smaller than a macro tile (64x64 down for UE4's 2048x2048 HZB) became separate images, so the HZB's small mips held nothing rendered and large boxes always read visible | t-hzb2 vs t-hzb1, same route: 30-67% of tested keys hidden; ~850-1,460 draws a frame vs ~1,150-2,120; 186-237 FPS vs 162-215; the user (t-hzb3): lows "significantly higher almost everywhere"; screenshots: no missing geometry | `464d925` · `texture_cache/image_info.cpp` | `GOW3_MICRO_MIP_TARGETS` (on) | G (any game rendering into its own mips one by one) |
| 16x anisotropic filtering | every filtered, linearly mipmapped, non-depth sampler gets 16x when the game's samplers become Vulkan samplers | none measured (a quality choice of the user) | `6fa6819` · `texture_cache/sampler.cpp` | `GOW3_FORCE_ANISO` (on) | G (changes the look) |

## 6. Frame pacing and present

Findings that shaped these (T-L2 to T-F27, [study-vrr-present-pacing.md](study-vrr-present-pacing.md)): a present-side
limiter released frames up to half a vblank early on the 480 Hz tick (T-L2: 6.3-10.4 ms apart at a 120 cap);
Windows' high-resolution timer wakes on a ~0.5 ms grid here (T-L5 run 3), so slot waits need a short spin; RTSS's
best mode was its NVIDIA Reflex limiter (T-F16); a VRR OLED flickers with any limiter at exactly half its refresh
(T-F26/T-F27). Defaults now (`scripts/graphics_options.py` `PRESENT_DEFAULTS`, T-F27): Reflex limiter with the sleep
before the present, 960 Hz precise tick, presents on the vblank thread, frame limit "Auto: below half refresh".

| Fix | What it does | Evidence | Commits · main files | Switch (default) | Class |
|---|---|---|---|---|---|
| **NVIDIA Reflex frame limiter** | `VK_NV_low_latency2` with present ids: the frame limit as `minimumIntervalUs`, `vkLatencySleepNV` right before `vkQueuePresentKHR` (as RTSS's hook does), latency markers; frames go to the driver as soon as they are queued; falls back to the Precise limiter without the extension | T-F18: driver sleep ended 8.327 / 8.340 ms (1% / 99%); T-F19: on-screen equal to RTSS's Reflex limiter (PresentMon); T-F24 (sleep before the present): present starts 8.329 / 8.333 / 8.337 ms, best lows so far (up to 118); T-F27 (+ VSync on, 119 cap on 240 Hz): the user, flicker gone, "huge improvement" | `966d23d`, `8858c4f`, `047cb7b`, `0c357bc` · `core/libraries/videoout/driver.cpp`, `vk_instance.cpp/.h`, `vk_presenter.cpp/.h`, `vk_swapchain.cpp/.h` | `present_limiter=reflex` (default); `GOW3_REFLEX=1`, `GOW3_REFLEX_SLEEP_BEFORE_PRESENT=1` | G (NVIDIA only) |
| **Precise limiter** | the slot is waited for with a high-resolution waitable timer plus a 0.7 ms spin, capped frames are released only at their slot (never early); with "hold" the vblank thread is held until the slot as RTSS does; presenting threads run time-critical + MMCSS "Games" (`SetCurrentThreadRealtime` is a no-op on Windows) | T-L4: capped 120, presented p99 9.05 ms (T-L2: 9.86 / p99.9 10.43); T-F8 (0.7 ms spin): releases 8.03 / 8.63 ms from 7.6 / 9.1; T-F16 (hold + priority): releases 8.333 / 8.334, 1% lows 112-117. First version (T-L1b, spin on the present thread) spiked in heavy views | `5231983`, `c552021`, `c67b7e1`, `5ecf53b`, `6353aaa`, `6e95734` · `core/libraries/videoout/driver.cpp` | `present_limiter=precise` (also Reflex's fallback); `GOW3_PRECISE_LIMIT`, `GOW3_LIMIT_HOLD`, `GOW3_PRESENT_PRIORITY`, `GOW3_PACER_SPIN_US` (700) | G |
| **Precise vblank tick, tick rate as a setting** | the vblank tick on a fixed grid (high-resolution timer + spin) instead of `AccurateTimer` (+-0.7 ms); tick rate chosen in the launcher (UE4 waits for the vblank, so a tick paces the game's frame starts). Rule found: tick = cap, or a high tick + limiter with exact timing, never a coarse 480 Hz tick + limiter | T-V120p: ticks p1 / p99 8.32 / 8.34 ms; T-P120 (120 tick + inline present + Immediate): the user, flat graph, 1% lows 118-119 at 4K native, G-Sync on or off; T-P960 (960 tick + 120 limit): fight 1% / 0.1% 107.6 / 100.5, dips ~95 FPS instead of 60 | `181c4c1`, `44994b9`, `0c357bc` · `core/libraries/videoout/driver.cpp` | `present_precise_tick=1`, `present_tick_hz=960` (defaults); `GOW3_PRECISE_TICK`, `GOW3_VBLANK_HZ`, `GOW3_TICK_SPIN_US` (700) | G (the precise tick spins ~0.7 of every 1.04 ms at 960 Hz) |
| **Present on the vblank thread** | the vblank thread presents itself instead of handing the frame to a swap thread | T-P120 (with the precise tick: the user's two essential settings, ablation runs 0c / 0d: 165.0 avg, 1% 158.1, 0.1% 151.8, 0 frames > 9 ms in 75 s) | `44994b9` (launcher), `0c357bc` (default) · `core/libraries/videoout/driver.cpp` | `present_inline=1` (default); `GOW3_PRESENT_THREAD=0` | G · the switch is inherited (Bloodborne / God of War III lineage); the finding and the default are ours |
| **Auto frame limits for VRR** | "Auto: below refresh" (Reflex's rule, refresh - refresh^2 / 3600) and "Auto: below half refresh" (refresh / 2 - 1); without VRR the refresh and refresh / 2 | T-F26 / T-F27: at exactly half the refresh the VRR display flickered with any limiter (RTSS at 119.98 too), at 119 not | `9b92c4a`, `50290a5` · `core/libraries/videoout/driver.cpp`, `gpu/shim/gow3_graphics.h`, launcher | `fps_limit` (launcher default: below half refresh), `display_vrr` (on) | G |
| Two small bugs | any frame limit 20-1000 is accepted (`ParseFpsLimit` turned an unlisted value such as 119 into unlimited, T-F26); `GOW3_PRESENT_MODE` is read by `Swapchain::FindPresentMode` (it was unused) | T-F26 log: Reflex interval 0 at a 119 setting | `fafe997`, `44994b9` · `gpu/shim/gow3_graphics.h`, `vk_swapchain.cpp` | none | G |

## 7. MediEvil-specific (UE4 config, eboot patches, hooks)

Useful to other UE 4.21 ports as ideas; addresses are for MediEvil 01.00 (eboot sha256 `a7e976d1…3b828`) and 01.02.

| Fix | What it does | Evidence | Commits · main files | Switch (default) | Class |
|---|---|---|---|---|---|
| **I1** boot at the output size | 4K boots like a PS4 Pro on a 4K TV (the game's own 4K display-metrics branch, two `je` -> `nop`), other sizes through "Boot display size WxH" (the display-metrics default rects), instead of a live `r.SetRes` that left the menus' hit-test grid at 1080p; `output_resolution` + `render_scale` replace the old list | T-I1b: 4K menus navigable (the user); T-R2: 1440p, T-R3: 1800p at 67% (the user); upstream 1.3.3 fixed #11 another way (kept as `GOW3_UE4_BOOT_4K=0`) | `ebf85f4`, `c8615ea`, `601aec6`, `cbb3e91`, `9753e2f` · `patches/MediEvil.xml`, `run.py`, `scripts/graphics_options.py`, `gpu/shim/window.cpp` | on; `GOW3_UE4_BOOT_4K=0`, `GOW3_UE4_BOOT_SIZE=0` | M |
| **4K memory sizes** | above 1080p: 7,104 MB guest memory (`GOW3_DMEM_MB`), the 512 MB framebuffer-pool patch, `r.GarlicHeapSizeMB=4600`, `r.Streaming.PoolSize=1600` | T-S2: small pans stop re-streaming (the user). See "Caveats": the two pak values are probably inert | `168ba15`, `c8615ea`, `9753e2f` · `scripts/graphics_options.py`, `scripts/ue4_override.py` | `perf_4k_memory` (on); `GOW3_PS4_MEMORY=1` | M |
| **No forced depth prepass** | `r.DBuffer=0` + `r.EarlyZPass=1`: DBuffer decals force UE 4.21's full depth prepass (628-739 draws, the G-buffer twice) for decals invisible here (~3,500 pixels of Dan's boots by 1-4/255); without them the PS4's partial prepass applies | T-Z2-T-Z5: ~600 fewer draws, the best heavy-view 1% lows then (the user) | `cd72da1` · `scripts/ue4_override.py` | `perf_no_decal_prepass` (on); `GOW3_DECAL_PREPASS=1` | M |
| **HZB occlusion through a remap hook** | the game reads its HZB results a frame after testing by per-frame indices while the port's GPU runs ~a frame behind; a hook replaces `FHZBOcclusionTester::MapResults` and serves the newest finished readback copy, remapped by (PrimitiveId, CustomIndex); a second hook numbers copies by Submit calls; with `r.AllowOcclusionQueries=1`, `r.HZBOcclusion=1`, `r.NumBufferedOcclusionQueries=3` | T-HZ4: heavy view 160-178 FPS, p99 6.7-7.3 ms (queries off ~130), no pop-in; T-HZ4d: the game's Sleep(0) map spin (~430,000/s) gone; T-HZ4e: buffered queries remove the query spin; with N-22's mip fix see section 5 | `aceb5a4`, `dfdc5fe`, `57dc5c4`, `3cf6d69`, `1e8150f` · `gpu/shim/medievil_hzb.cpp`, `src/hzb_hook.h`, `src/probe.c`, `texture_cache/readback_queue.h`, `texture_cache.cpp/.h` | `perf_hzb_occlusion` (on where the eboot matches); `GOW3_HZB_REMAP=0` | M |
| Occlusion queries off (finding) | the port answers occlusion queries "visible" (shadPS4's synthetic `ZPASS_DONE`, from the shared root commit), so their box draws cost time and cull nothing; queries off where the HZB remap cannot run | T-O1: ~150 fewer draws a frame, ~+5% | `c8615ea` · `scripts/ue4_override.py` | default for eboots without the remap | M (the finding is G: any shadPS4-lineage port culls nothing by occlusion queries) |
| **Intro movie suspended at a level change** | MediEvil plays its intro on a persistent media player and never stops it after the skip; a byte-checked hook on `execOpenLevel`'s `OpenLevel` call pauses every player still playing (pause flag only) | T-M1 (2026-10-26): image uploads ~90 MiB/s for ~200 s -> 0-5 MB/s from ~5 s after the skip | `f0ced8c` · `src/level_hook.h`, `src/probe.c`, `core/libraries/avplayer/*` | `GOW3_MOVIE_SUSPEND` (on) | M (the AvPlayer pause is G) |
| **Effects resolution** | `r.SeparateTranslucencyScreenPercentage` (UE4 separate translucency, where enemy death effects draw) at 100 / 75 / 50, switched live through the game's console variable | N-19/N-20: death effects +2.36 / +2.91 ms of GPU in their windows at 100%, <= 0.09 ms at 50%; N-26: re-sized live; the user: 50% "slightly less sharp", 75% "very difficult to tell" | `32586bc`, `9cc915e` · `scripts/ue4_override.py`, `src/runtime_extra.c`, `gpu/shim/gow3_settings.*`, `gow3_overlay.cpp` | `effects_resolution=75` | M |
| **Preshadows as a live switch** | `r.Shadow.Preshadows` set live through the game's console variable (`runtime_ue4_request_cvar`: object and registration bytes checked first) | N-25: off ~130-150 fewer draws (~12%, ~0.4 ms of stage B) in one view; t-park3 (off): the Graveyard horizon held the cap; the user: the difference is small (Dan's self-shadow on his shoes) | `fdd12e8` · `src/runtime_extra.c`, `gpu/shim/gow3_settings.*` | `preshadows=1` (on) | M |
| Live graphics options | 13 options (shadows, view distance, LOD, AO, reflections, fog, post effects) set live through 16 more console variables; "Preset" restores the boot preset | none (a feature; not yet verified one by one in game, plan-live-switches.md) | `6fa6819`, `3cea271` · `src/runtime_extra.c`, `gpu/shim/gow3_settings.*`, `run.py`, `scripts/ue4_override.py` | in-game menu / launcher | M |
| **GPU particle bounds** | UE 4.21 GPU sprite emitters without a fixed box get a 1-unit box at the component origin, so the whole system is culled once that point leaves the view (also on a PS4); the patch points the two constant loads at -2000 / 2000 (a 40 m box) | T-0102-5 (RenderDoc: the dust draw is never issued); survey: 8 of 180 GPU systems lack fixed bounds; T-0102-6: the user, the dust no longer vanishes, no performance issues | `548db4c`, `bc93780` · `patches/MediEvil.xml` | eboot patch "GPU particle bounds" (on) | M |
| Single GPU particle pass (now off) | `r.GPUParticle.FixDeltaSeconds=0`: one particle simulation pass a frame instead of the fixed 1/30 s step | T-H3b: "slightly better", inconclusive; T-0102-4: it slowed lone torches to a crawl and left an ember in Dan's crypt, so off by default since 2026-11-01 | `27ec51e`, `8d85929` · `scripts/ue4_override.py`, `scripts/graphics_options.py` | `perf_particle_single_pass` (off) | M |
| **Game update 01.02** | every 01.00-only site (HZB and level hooks, 18 live cvars, display-metrics and boot-size patches, SSE4a sites) mapped to the US 01.02 eboot by masked instruction search; 01.02's `t.MaxFPS 30/60` commands written as `t.MaxFPS 00` above 60 FPS; the HZB remap chosen by the installed version | T-0102-1: the user, "the best performance I've seen", heavy-area 0.1% lows ~200 (RTSS 222 avg, 0.1% 198-222 in the crypt); drive and version not separated | `1e8150f`, `3cf6d69` (+ upstream PR #26 `c7a9ef3`) · `src/hzb_hook.h`, `src/level_hook.h`, `src/runtime_extra.c`, `src/runtime_sse4a.c`, `patches/MediEvil.xml`, `scripts/patches.py`, `scripts/ue4_override.py` | per build, by signatures | M |

## 8. Derived and inherited

### From the Bloodborne port (credited in the commits)

Listed in their sections above: L5 (0.3, `4d4d208`), D3 (0.3), GPU write twins (0.4), bulk descriptor copy (0.3), the
idea of D2 (`NotePreciseUpload`). Also:

| Fix | What it does | Evidence | Commits · files | Switch (default) | Class |
|---|---|---|---|---|---|
| B1 targeted `WRITE_DATA` waits | deferred host-copy sources noted in a 64 KiB-granule table; a `WRITE_DATA` waits only when a copy may still read its range (W2 reuses the table) | T-B1: 1 of ~115-123 waits a frame skipped; no gain of its own | `a2a81fe` · `vk_scheduler.cpp/.h`, `amdgpu/liverpool.cpp`, `buffer_cache.cpp` | toggle bit 62 | G · Bloodborne (#1 of its list, `daa7165`) |
| `GOW3_SECTIONS` | stage B time per packet by part (rdtsc scopes) | the tool behind T-S1, T-HZ4g, N-8, N-28-N-31 | `c4bf2dc` and later | `GOW3_SECTIONS=1` | G (diagnostic) · Bloodborne `BB_SECTIONS` |

### From upstream MediEvil 1.3.3-1.3.8 (bc160201983 and contributors), merged into `lab`

1.3.3-1.3.5 merged on `merge-1.3.5` (`b7b78fb`), 1.3.6-1.3.8 on `merge-1.3.8` (`e308c4c`), both in `lab` since
2026-10-30. Merge test (plan-work-queue.md item 1, N-27): no regression against the pre-merge `lab` (heavy normal frames
4.45-4.48 vs 4.55 ms; fight 1% low 103 vs 99-105); upstream's host switches off made single-frame stalls more frequent
(5.3/min vs 2.8/min at the cap), so they stay on.

| Fix | Ver. / credit | Switch (default) | Class |
|---|---|---|---|
| `SyncWithDrawRecorder` (level 3 crash on NVIDIA: stage A recording while stage B had queued draws); `RecordData` waits before copying so data and command share a chunk | 1.3.4 / 1.3.5 | `GOW3_RECORD_DATA_FIX=0` (the 1.3.4 order) | G (complements L5) |
| Volume 2D-array views (= our V3) and exact `ClearVolumeSlices`; without `VK_EXT_image_2d_view_of_3d` (AMD Polaris crash #18) 2D views of 3D stay attachment-only | 1.3.4 / 1.3.5 | `GOW3_VOLUME_VIEWS=0`, `GOW3_NO_2D_VIEW_OF_3D=1` (test) | G |
| Texture GC limits from the driver's memory budget; slot reallocation use-after-free fixes; deferred sampler destruction; stencil proxy check | 1.3.4, from the Uncharted port | none | G |
| VSync frame-limit rule (#3): VSync on and limit >= refresh - 1 leaves the cap to VSync | 1.3.3 | none (applied before Reflex in `driver.cpp`) | G |
| 7.1 -> stereo downmix on stereo devices (#14); every controller merged, background pad events, stick range (#25, #36) | 1.3.4, 1.3.6 | `GOW3_STICK_RANGE` | G |
| F12 PNG screenshots (Shift+F12 raw dump) | 1.3.3 | key | G |
| Safe GPU mode for older AMD drivers (no maintenance5/8, feedback loops, 2D-of-3D); GPU / driver / feature log line; crash reports list the barrier batch being recorded and mark destroyed images | 1.3.6 | `GOW3_VULKAN_COMPAT=1`, `GOW3_NO_MAINTENANCE5/8=1` | G |
| Frame analyzer crash on flatbuf ring bindings (address 0 passed `IsValidGpuMapping`) | PR #28, Porticus | none | G |
| At most 128 image barriers per call (AMD's older driver crashed at 3,696) | 1.3.8 (#45, #49) | none | G |
| Hitch work: 1 ms timer, no power throttling, a spare GPU memory block per memory type, pageable video memory, futex condition variables, merged barrier ranges (gap 64), opt-in layer-range barrier merging; upstream: worst frame on 4 cores 31-32 -> 18 ms | 1.3.8 (#20), from the Uncharted port | `GOW3_TIMER_RES`, `GOW3_POWER_THROTTLE`, `GOW3_SPARE_IMAGE_BLOCK`, `GOW3_PAGEABLE_MEMORY`, `GOW3_FUTEX_COND`, `GOW3_TRACKER_MERGE_GAP` (64) all on; `GOW3_BARRIER_MERGE=1` opt-in | G |
| Linux build and AppImage | PR #13, nydal91 | build | G |
| 01.02 / EU support: per-version patch profiles, NpToolkit2 imports as logged no-ops, `r.SetRes` slot found per eboot, lowercase override pak name | PR #26 / #29 (Porticus), `1ae17cd`, 1.3.7-1.3.8 | per version | M |
| EU version, languages, voice language, AA option (`ue4_aa`), 60 FPS default, portable mode | 1.3.3-1.3.8 | launcher | M |

Ours kept over theirs: V3's view rule (attachment-only, no extension needed; theirs kept for other 3D views); I1's
boot at the output size with their live `r.SetRes` as the fallback; our GPU-profiler frame start (theirs would count
frames twice, study-upstream-commits.md H2); the one-time `engine_fps` reset to 60 left out.

## 9. Diagnostics worth taking

All off unless set; none changes behaviour. `GOW3_PERF_DIAG` (host-copy waits, render-pass ends and switches by
caller, barriers by site with stages / layouts / image size, write-fault downloads by guest instruction and by who
made the page GPU-written), `GOW3_SECTIONS` (above), `GOW3_SLOW_FRAMES=<x>` (what slow frames did against samples),
`GOW3_FLIP_TRACE=<csv>` (per-frame pipeline stages from submit to present, `gpu_ready`; tools `research/tools/flip_*.py`),
`GOW3_LONG_WAITS=1` (GPU waits >= 1 ms by site), `GOW3_GPU_PROFILE` (fixed: frames end at the display pass, timestamps
read without waiting, slow passes by growth), `GOW3_TEXCACHE_STATS`, `GOW3_HOST_MAP_STATS`, `GOW3_WAIT_SITES`, the
background log writer (`gpu/shim/gow3_async_log.h`: ~30 unbuffered printf a slow frame on the flip path were the
stutter in logged runs, N-8), Nsight support (`GOW3_SKIP_TOOL_INFO=1`: Nsight Systems' layer crashes on
`vkGetPhysicalDeviceToolProperties`; `GOW3_STDOUT_FILE`; `run-game.ps1 -Nsys` / `-Ngfx`), a minidump on any fatal fault
including faults inside winpthreads (`b01661e`, `5247265`, `513ca7f`; `GOW3_CRASH_DUMP=0` / `=full`).

## 10. Tried and dropped

| What | Switch / commits | Why (test) |
|---|---|---|
| L4 re-kick chunks while waiting for host copies | `684cd8f`, reverted `a07f709` | symptom fix for H1; L5 removed the cause |
| Skip the end-of-pipe wait in stale-readback mode | `e384aa1`, reverted `6b86c75` | no FPS gain and torches as black flickering boxes (V2). Do not retry |
| L1 region caches per 16 MiB bucket | `0705e49`, reverted `bc60b8b` | T-L1: no difference (123-125 vs 120-126 FPS) |
| P2 skip the end-of-pipe wait with no readback work | `GOW3_IDLE_EOP_SKIP=1` (opt-in) | T-P2: +2%, but worst frames 29-42 ms vs <= 19 ms |
| O2 / O3 real occlusion query results (completion thread; early submission) | `GOW3_OCCLUSION=1` (opt-in) | correct, up to 63-68% hidden, but cost more than they save while panning (T-O3c); the HZB remap replaced them |
| `r.NumBufferedOcclusionQueries=2` (R4a) | config | heavy view 94 -> 75 FPS |
| W3 `WRITE_DATA` flags written after the deferred fences; W3b flip interrupt likewise | `GOW3_WRITEDATA_DEFERRED=1` (opt-in), `GOW3_FLIP_DEFERRED=1` (opt-in, do not use) | W3 alone moves the wait to the flip, lows worse (T-W3b, T-W3d); W3b deadlocked at boot (T-W3c). Do not use W3b |
| Stage B on a dedicated core | `GOW3_STAGE_B_CORE=1` | T-HZ4h: slow frames unchanged, pacing worse |
| Frames queued 2 | `frames_queued=2` | T-W1, T-Q2, N-15: hurt the lows. N-25: the user found it smoother after the HZB fix and parking; default stays 1 |
| `r.EarlyZPass=0` alone | `fca0d85` | T-Z1 gain was real but the full prepass came from DBuffer decals; replaced by `r.DBuffer=0` + `r.EarlyZPass=1` |
| `rhi.SyncInterval=0`; `t.MaxFPS` / engine frame-rate range as a cap | config | T-L0 no effect; T-Z4, T-Z4c/d: UE4's own cap does not engage in this build |
| Newest-frame mode | `GOW3_NEWEST_FRAME=1` | T-L5 runs 1-3: works, but the present thread's timing kept the wobble; removed from the launcher |
| Tick locked to the display's vblank | `GOW3_VBLANK_SYNC=1` | T-V120s: under VRR there is no steady vblank (~52 FPS, menus 1.7 FPS 1% low) |
| Display times through `VK_KHR_present_wait` | `GOW3_DISPLAY_TRACE=1` | T-V120b: NVIDIA's present wait returns at the present (no information) |
| Limiter wait inside / after `vkQueuePresentKHR` | `GOW3_LIMIT_IN_PRESENT=1`, `GOW3_LIMIT_AFTER_PRESENT=1` | T-F10-T-F12: exact present starts but persistent VRR flicker |
| One clock (tick derived from the limit) v1 and v2 | `GOW3_ONE_CLOCK=1` | T-F14 (grid re-anchored), T-F15 (late tick processing): lows worse, stalls |
| Skip the upload when the pass fast-clears the target; new images where a GPU-written image was freed | `GOW3_CLEAR_SKIPS_UPLOAD`, `GOW3_UNDEFINED_GPU_FREED` (on, harmless) | N-11: 0 hits in MediEvil (t-up3b, t-up4) |
| Single GPU particle pass as a default | `perf_particle_single_pass` | T-0102-4: slow torches and a crypt ember; off since 2026-11-01 |
| `r.Streaming.AmortizeCPUToGPUCopy` (the streaming test called S1 on 2026-10-19) | config | T-S1: did not split the panning bursts |
| FindImage generation-only misses (study item 10) | not built | N-28: 3-9 a frame, under the gate of 50 |
| Dropped on analysis only, not launched (study-frame-cost §4) | not built | mid-frame split submits, Bloodborne-style idle flush, cutting the recorder's 200 us spin, an ObtainBuffer memo, shadow-map backing rename (N-6 premise wrong: 26-34 separate maps), dropping the shadow flushes' global memory barrier |

## Caveats in the evidence

- **Narrow image stages:** N-4/N-5 reported ~9% shorter GPU frames; N-16 corrected it to -0.8% GPU active time (the
  rest was idle variance). Kept for correctness of the stored stages, not as a GPU win.
- **4K memory (checked statically 2026-11-03):** the crash fix at 4K is upstream's "Native 4K framebuffer pool
  (512 MB)" eboot patch with `GOW3_DMEM_MB` (the PS4's 128 MB pool ran out); both stay. The two pak values were ours
  (S2, 2026-10-20) and do nothing useful: `r.GarlicHeapSizeMB` exists in neither eboot (01.00, 01.02; UTF-16 or ASCII,
  while the same search finds `r.Streaming.PoolSize`; "Garlic" appears only in memory-report strings). This build has no
  fixed Garlic heap: at boot it allocates the framebuffer pool (512 MiB, type 3) and then **all the remaining direct
  memory as one memory pool** (6,592 MiB = 7,104 - 512 at 4K, `user/last_run.log`), committing Garlic or Onion pages
  from it per `sceKernelMemoryPoolCommit` type, so `GOW3_DMEM_MB` is the GPU-memory knob; `OVERBITE/Config/PS4/PS4DeviceProfiles.ini` sets
  `r.Streaming.PoolSize=2000` for PS4, PS4_Neo and PS4_Neo_4k, and the eboot applies Engine.ini's `[ConsoleVariables]`
  with SetBy `0x04000000` (01.00 `0x837f61`, `ApplyCVarSettingsFromIni` at `0x837fb0`; UE 4.21: SetBySystemSettingsIni,
  below SetByDeviceProfile `0x05000000`), so the pak's `[SystemSettings]` 1600 loses to the profile's 2000 (had it won,
  it would have shrunk the pool). Not yet confirmed in game (the runtime's `GetFlags` on the cvar would show the
  SetBy). T-S2's "400-800 MB PS4 pool thrashing" is wrong for the same reason; its gain, if it came from these settings, came
  from `GOW3_DMEM_MB` (unverified).
- **study-frame-cost §4** lists separate translucency at 50% and ring reuse as dropped; both were later reversed by
  measurements (N-20 / N-26: effects resolution, default 75%; N-32: ring reuse on).
- **Occlusion queries:** the 2026-10-20 inventory listed "queries off" as the default; since `57dc5c4` the default is
  queries on with HZB through the remap, queries off only where the remap's hooks do not match the eboot.
- **"Lock-free memory map lookups"** switches only `GOW3_OPTIMISTIC_CLAMP` live; `GOW3_REGION_BUCKETS` needs a restart.
- **IDs are reused** across docs and days (S1 / S2, T-S1 / T-S2, T-L1 / T-L2, T-M1, T-P1-T-P5, T-R1): the doc and date
  in this file's evidence say which.
- **01.02 gain:** T-0102-1 ran on another drive than earlier runs; version and drive were not separated. The
  comparison with upstream 1.3.8 as shipped (plan-0102.md: 0.1% low 211.9 mid-fight here vs 44.7 standing there)
  mixes game versions too.
