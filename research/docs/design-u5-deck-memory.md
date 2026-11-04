# Design: U5 on the Deck: guest memory as GPU-visible memory, buffers read in place

Status: design draft, 2026-11-03, `deck-lab` (`32a4b3d`). Nothing built, launched or committed. Parent:
[plan-linux-deck.md](plan-linux-deck.md) ("Direction": unified memory first; "U5 gate 1" passed). Input:
[study-fence-waits.md](study-fence-waits.md) (fence precondition, instrumentation, gate 2). Background:
[study-guest-memory-direct.md](study-guest-memory-direct.md) (PC study and its precondition), [fix-inventory.md](fix-inventory.md),
[gpu-threading-reference.md](gpu-threading-reference.md). Bloodborne: `bloodborne_pc` at `3c57da4` (0.51) and
`origin/master` `0fc2921` (0.7, "BB 0.7", read with `git show`).

Marks as in the study: **[code]**, **[log]**, **[doc]**, **[BB]**, **[web]**, **[inferred]**. Nothing here is verified in
game.

## Summary

- **What.** On Linux with RADV on an integrated GPU, guest direct memory is allocated as Vulkan memory, which the GPU can
  read and write in place:
  - memory type 5, `HOST_VISIBLE|HOST_COHERENT|HOST_CACHED` on the Deck;
  - exported as dma-buf and mapped by the runtime at the guest addresses, instead of the memfd;
  - **bound in place** into the buffer cache's sparse arena.
  - Buffers are then read and written where the game keeps them: no uploads, no write tracking, no downloads.
  - Textures and render targets stay images (detiled, in the texture cache), but detile from guest memory in place.
- **Why the Deck.**
  - Gate 1: the GPU reads type-5 memory at 41.2 GB/s vs 40.8 device-local, so there is no penalty for leaving data in
    place and no hot-block moves are needed (plan-linux-deck.md "U5 gate 1") [log].
  - The copy model's costs fall on the scarce resource, CPU time on 4 Zen 2 cores:
    - ~760 write faults a frame stretch the game's frames (runs 5-6);
    - host copies take 4-5% of stage B;
    - stream copies run at 47k/s [log].
  - Bloodborne 0.7 runs this way on the Deck: `0 write faults/s`, `host copies 0/frame` (`docs/unified_memory.md`) [BB 0.7].
- **Precondition.** Guest fences must signal in GPU order (study-fence-waits.md). The fence work comes first, under the
  copy model, so its cost is measured before any memory change.
- **PS4 memory type as an input.**
  - Onion (0) and Garlic (3, 10) pages follow different policies on the PC (section 3).
  - On the Deck both end up in place. The type decides the **order of rollout** (whichever type holds the faults goes
    first) and what the instrumentation reports.
- **Switches.**
  - `GOW3_U5=1` (memory model, decided at start; default 0 = today);
  - `GOW3_GPU_FENCES=1` (fence mode; a toggle bit switches it at a frame boundary for A/B in one launch);
  - one toggle bit per in-place class (Onion buffers, Garlic buffers, GPU-written buffers, texture sources, constants), so
    each class can be A/B'd live where safe.
  - Off on Windows, on discrete GPUs, and on drivers without the extensions.

## 1. Scope

| In | Out (unchanged) |
|---|---|
| Linux, RADV, `IsIntegrated()` (the Deck; other AMD APUs likely) | Windows (no Vulkan memory at fixed guest addresses: no placed maps, study-guest-memory-direct.md §2b); discrete GPUs; NVIDIA |
| Guest **direct** memory (`sceKernelAllocateDirectMemory`, the memory pool) | Flexible memory (`[POOL_SIZE, +1 GiB)` of the memfd, `src/runtime_memory.c:146`, `:440-449`): CPU-only in practice; Bloodborne never puts it in a chunk [BB] |
| Buffers: vertex, index, constant, structured, shader-written, labels | Images: render targets, depth, sampled textures stay `VkImage`s; image readbacks stay copies (into guest memory by the GPU, step U5-8) |
| GPU-ordered guest fences | Presentation, pacing (present path unchanged) |

## 2. What we know going in

**Gate 1 (Deck, SteamOS 3.9.5, RADV 26.1.99)** [log]:
- Type 5 backs a sparse arena (128 MiB bound, 4 KiB sparse block). CPU write → GPU read → GPU write → CPU read all correct.
- GPU read: 41.2 GB/s in place vs 40.8 GB/s device-local.
- memfd import fails (`VK_ERROR_INVALID_EXTERNAL_HANDLE`).
- Vulkan-allocated dma-buf memory (type 5, 512 MiB in 57.5 ms):
  - maps twice, including `MAP_FIXED` at an offset; aliases; coherent; takes write faults and `read()`;
  - GPU reads it at 22.6 GB/s as an imported plain buffer.
- CPU writes: ordinary memory 7.7 GB/s, **memfd (today's guest memory) 1.0 GB/s**, dma-buf view 4.9 GB/s; reads ~14.5 GB/s
  for all three. Whether the memfd figure includes first-touch faults is not recorded [inferred: likely].

**Not measured yet:**
- GPU **write** bandwidth into type 5;
- how soon a GPU marker write is visible to a polling CPU;
- how many GiB of type 5 the Deck allows (GTT is 8 GB; the guest pool alone is 5.4 GiB);
- `VK_AMD_buffer_marker` presence on this RADV. Our `vk_instance.cpp` does not enable it today [code].

**Bloodborne's road and what went wrong** (study-guest-memory-direct.md §1; subagent read of `bloodborne_pc`) [BB]:

| Their step | Result | Lesson for us |
|---|---|---|
| 0.3: in place without GPU fences | dropped: "unsafe unless fences wait for real GPU completion" (`docs/parallel_gpu.md:189-197`) | fences first |
| 0.4-0.5: AMD model, dma-buf chunks (`HOST_VISIBLE\|HOST_CACHED`, 256 MiB grid, `bbport_guest_memory.cpp:46-65`, `:94-175`), bound in place into the sparse arena (`BindInPlace`, `buffer_cache.cpp:1953-2007`); Garlic in VRAM, Onion in place, **everything in place on an integrated GPU** (`buffer_cache.cpp:406-408`, `:474-480`) | works on AMD; on a discrete card **all in place 48 vs 210 FPS** (`BB_FIXED_ARENA`), hence VRAM copies of hot Garlic | the Deck needs no VRAM copies (gate 1) |
| Honest labels: `WriteLabelOnGpu` (`vkCmdWriteBufferMarkerAMD`, bottom of pipe, high half then low, `vk_rasterizer.cpp:294-355`); EOP with IRQ, GPU idle and RELEASE_MEM by a signal thread on the timeline (`:429-484`) | guest heap corruption when GPU idle came before deferred fences (7/7 crashes → 0, `docs/ROADMAP.md:39-41`); torn 64-bit labels (atomic store); spin forever on unsubmitted signals (flush in WAIT_REG_MEM, idle submit after 0.2 ms); device loss looked like a freeze on the **Steam Deck** (bounded waits + 10 s watchdog); submit cost ~65 us each, hence batching | each is a failure mode below (section 9) |
| Honest submission lock | 101 FPS; overlap K=2 (`BB_SUBMIT_AHEAD`) 154; K ≥ 8 leaked the game heap, a watchdog turns overlap off (`gnmdriver.cpp:110-175`) | overlap with a small K, with a leak watchdog |
| NVIDIA sparse rebinding froze the GPU 10 s+ | model AMD-only, then 0.6 moved everything to the **layer memory module** (plain chunk buffers + device-address page table, no sparse); on AMD faster than the arena too (234 vs 213 FPS, `docs/CHANGES_0.6.md:13-21`) [BB 0.7] | sparse is fine on RADV for a first version; the layer module is the later option (section 5) |
| 0.7 "one memory model"; graphics heaps grow on demand (~1.5 GB allocated never used); **Steam Deck lazy-memory imports counted twice in GTT: device lost after ~80 min** (`docs/CHANGES_0.7.md:42-49`, `:142`) [BB 0.7] | allocate chunks lazily and log the GTT budget |
| 0.7 "Stage A no longer waits for the GPU before reading descriptor tables when the frame's tasks only read the CPU's data" (Deck 63.5 → 65 FPS) [BB 0.7] | in place, the CPU-side decoder must wait for GPU writes it reads: a new wait class (section 7) |
| Texture sampling from in-place arena memory: "smeared and ghosted" (`buffer_cache.cpp:426-433`); textures detile from the chunk (`BB_CHUNK_TEXTURES`) | images stay images; only their detile source moves |

## 3. The PS4 memory type as the policy input

**Values and meaning** (study-fence-waits.md §2.4):
- 0 = `WB_ONION`: CPU cached, coherent, low GPU bandwidth by design;
- 3 = `WC_GARLIC`: CPU write-combined, full GPU bandwidth, not coherent;
- 10 = `WB_GARLIC`.

**Where the runtime already has the type** [code]:
- `Block.type` from `direct_allocate` (`src/runtime_memory.c:663-683`);
- `Vma.type` per mapping, from `map_direct_locked` (`block_type`, `:687-693`) and per `sceKernelMemoryPoolCommit(type)`
  (`pool_commit_impl`, `:1027-1046`);
- `sceKernelGetDirectMemoryType` (`:863`).
- The PS4 prot bits sit beside it (`Vma.prot`; 0x10 GPU read, 0x20 GPU write, e.g. the framebuffer pool's `prot=0x32`,
  `deck-native-4.log:78`) [log].

**MediEvil:**
- framebuffer pool 512 MiB type 3;
- the rest of direct memory as one pool block reserved as type 0, with commits of either type
  (`deck-native-4.log:77-80`; fix-inventory "Caveats") [log][doc].
- So one direct allocation holds both types. **The Vulkan memory of a chunk cannot follow the PS4 type** (it is chosen at
  allocation, before the commits). The type acts at binding, tracking and policy time, per mapping.

**What each type means for the GPU side:**

| PS4 type | What games put there [inferred: PS4 guidance as reported, not checked against Sony docs] | Deck (U5) | PC (copy model today; possible later) |
|---|---|---|---|
| Onion (0), GPU prot set | data the CPU reads back, or the GPU must see coherently; small by design, because the bus was slow | in place, no write tracking | candidate for host-visible memory read over PCIe like an upload heap (Bloodborne's Onion policy). Its PS4 budget was ~10-20 GB/s, inside PCIe 4.0's ~25 GB/s. Needs import on Windows (unproven) + GPU-ordered fences (cheap on the PC: GPU lag 0.44 ms) |
| Garlic WC (3), GPU prot set | geometry, textures, render targets, and CPU-streamed GPU-read data (WC writes are fast) | in place; textures detile from it | copy model with VRAM (today), write tracking stays |
| Garlic WB (10) | rare | in place | copy model |
| any type, no GPU prot bits | CPU-only | not bound (never reaches the arena) | not tracked |
| flexible | CPU-only | memfd as today | as today |

**The open question this answers.** The PS4 guidance does not tell us whether MediEvil's ~760 per-frame faults sit in
Onion or in Garlic-WC pages. UE4 may stream per-frame constants through WC Garlic [inferred]. The **fault map by type**
(study-fence-waits.md §5.5) answers it in one run, and the step plan orders the rollout by it (U5-5 / U5-6). If the faults
sit in Onion pages, Onion-in-place alone removes most of them on the Deck. On the PC, "Onion pages untracked, read through
host-visible memory" would replace the heuristic hot-pages plan (plan-linux-deck.md run 6, "hot pages once a frame").

## 4. Guest memory allocation

**Who allocates.**
- The GPU side owns the Vulkan memory; the runtime owns the address space, as in Bloodborne.
- New `gpu/shim/gow3_guest_memory.cpp` (port of `bbport_guest_memory.cpp`, GPL-2.0, Bloodborne hash in the commit)
  registers a chunk allocator with the runtime: `runtime_memory_set_guest_chunk_allocator(fn)` (as `bloodborne_pc
  src/runtime_memory.c:716-724`).
- It is called from the `Rasterizer` constructor. The GPU is up before the game's first direct allocation: Vulkan init and
  "Buffer residency" at `deck-native-4.log` lines 33-58, the first direct allocation at line 77 [log].

**Chunks.**
- 256 MiB, on a grid over the direct-memory offset space `[0, POOL_SIZE)`.
- On the Deck POOL_SIZE is 5,568 MiB: 512 type 3 + 5,056 pool (`deck-native-4.log:77-80`). At the PC's 4K settings it is
  7,104 MiB. U1 should shrink it at 720p/800p: the 512 MiB framebuffer pool patch is a 4K need.
- Each chunk: `vkAllocateMemory` (type 5, `VkExportMemoryAllocateInfo` dma-buf) + a `VkBuffer` over it (usage
  storage | uniform | vertex | index | indirect | transfer src/dst | shader device address) + `vkGetMemoryFdKHR`.
- **Lazily**: a chunk is created on the first map or commit of a physical range inside it, not at `direct_allocate`. The
  memfd is sparse today and pages appear on first touch; a 5.4 GiB eager allocation would claim GTT the game never uses
  (Bloodborne 0.7: ~1.5 GB unused, and a GTT double count lost the device after ~80 min).

**Runtime side** (`src/runtime_memory.c`, Linux branch `:440-465`):
- `pool()` keeps the memfd for flexible memory and as the fallback.
- `host_map(address, size, prot, reserved, phys)` maps from the chunk's dma-buf fd at `phys − chunk.phys` when the range
  is direct and U5 is on, split at chunk boundaries (Bloodborne `map_phys`, `runtime_memory.c:182-220`).
- `backing_base` (the unprotected second view, `:130`, used by `runtime_memory_write_backing` / `read_backing`
  `:1247-1275` and the fence writers' `TryWriteBacking`) becomes per-chunk views mapped `MAP_SHARED` from the same fds at
  `backing_base + chunk.phys`, so every backing-view user keeps working unchanged.
- **Decommit / discard:**
  - `host_discard` (`:463-465`) punches holes in the memfd. On a dma-buf it must `memset` or do nothing (Bloodborne
    memsets).
  - `pool_decommit_impl` (`:1060-1092`) unmaps and remaps reserved on Linux: L2 ("keep pool views on Linux") applies
    equally to dma-buf views and should land first or with U5-3.
- **Commit:** unchanged, except that the first commit in a chunk creates the chunk.
- **Hooks to the GPU side** (`queue_hook` map/unmap/invalidate, `:466-471`, `runtime_memory_set_gpu_hooks` `:1236-1245`)
  are unchanged. The GPU side additionally asks `runtime_memory_type_at` (new, study §5.5) for type and prot.

**Budget.**
- Log `VK_EXT_memory_budget` for every heap at start and every 5 s with stats.
- Refuse U5 at start, with a log line and the memfd path, if the heap that holds type 5 cannot take POOL_SIZE plus a
  margin.
- The Deck's GTT is 8 GB, and the guest pool is 5.4 GiB of it.

## 5. Binding in place

**Arena, first version.**
- The buffer cache's sparse arenas stay (`buffer_cache.cpp:1222-1260`).
- `EnsureResident` (`:1262-1333`) binds a block in place when:
  - the block is direct memory in a U5 chunk;
  - its phys is block-aligned (16 KiB blocks: `MIN_BLOCK_SIZE`, `buffer_cache.h:49`; RADV's sparse alignment is 4 KiB);
  - its in-place class is switched on (type / GPU prot, section 3).
- How: `vk::SparseMemoryBind{block offset, chunk.memory, phys − chunk.phys}` instead of a residency-pool piece. The
  block's BDA page-table entry = chunk buffer device address + offset.
- Everything else (flexible memory, unaligned remainders, classes switched off) binds device memory as today, with
  uploads and tracking.

**Rebinding.**
- A guest unmap or remap of a range (pool decommit, commit at another phys) must rebind its arena blocks; in-place
  blocks follow the guest's phys.
- The runtime's unmap hook already reaches the buffer cache. Rebinding goes into `SubmitPendingArenaBinds`
  (`buffer_cache.cpp:1555-1589`), committed before the next submit.
- Measure `vkQueueBindSparse` time per call: Bloodborne logs calls ≥ 50 ms; NVIDIA's grew with the bound count.

**Later option: Bloodborne's layer module.**
- Plain chunk buffers + a device-address page table, no sparse.
- Faster on AMD discrete (234 vs 213 FPS) and their only model since 0.6 [BB 0.7].
- Needs shader-side paging for bindings that cross chunks. Only worth it if sparse binds show up in the Deck's frame
  costs.

## 6. Write tracking

| Range | Today | U5 |
|---|---|---|
| In-place buffer blocks | page protection (`page_manager.cpp`, `region_manager.h`), uploads on dirty | **off**: no protection, no `MarkRegionAsCpuModified`, no upload. Bloodborne: `WriteTracking()` off in place (`buffer.h:46-52`), writers that matter announce instead (`NoteAssetWrite`) [BB] |
| Pages under images (texture sources) | protected; a CPU write marks the image dirty | **kept**: a CPU write under a texture must re-detile (Bloodborne `BB_IMAGE_TRAPS`: "these traps fire on texture streaming, not every frame") |
| Pending image readback targets (deferred readbacks) | protected until published (`texture_cache.cpp:229-240`) | kept until U5-8 (GPU copies into guest memory in stream order), then off |
| Ranges stage A reads (descriptor tables, fetch shaders, indirect args the CPU parses) | GPU-modified ranges + downloads | see section 7: wait for the GPU write tick |
| Device-memory blocks (not in place) | as today | as today |

`CountWriteFaults` (`region_manager.h:238-260`) with the fault map by type (study §5.5) shows the tracking going away
class by class.

## 7. GPU writes to buffers

- **Stay in place.** Shader writes, DMA_DATA, fills and copies land in guest memory directly. Gate 1 checked GPU write →
  CPU read after completion. GPU write bandwidth into type 5 is a probe item (U5-0).
- **CPU reads by the game:** correct only after the GPU executed the write. With GPU-ordered fences that is the game's own
  rule (it reads after its fence), as on the PS4. The copy model's guards (D2 precise writes, D3 downloads, twins, N-13
  page downloads) do nothing for in-place ranges and must be bypassed there, not merely left on: a download would copy
  stale arena bytes over newer ones.
- **CPU reads by the port** (stage A / B parse guest memory: V#/T# tables, indirect counts, fetch shaders, constants
  hashed for memos):
  - such a read of a range with a pending GPU write must wait for that write's tick;
  - `WriteTicks()` per 64 KiB page exists (D3, `buffer_cache.cpp:42-100`);
  - counter: "port reads waiting for GPU writes".
  - Bloodborne 0.7 had to narrow exactly this wait (Deck 63.5 → 65 FPS) [BB 0.7].
- **Coherence:** type 5 is `HOST_COHERENT`, so no `vkInvalidateMappedMemoryRanges` / flush. Mid-stream visibility of a
  marker write to a polling CPU is assumed, not measured (U5-0).

## 8. Fences (GPU-ordered)

| Guest-visible write | U5 / `GOW3_GPU_FENCES=1` | Today |
|---|---|---|
| EOP label, no interrupt | **in place:** `vkCmdWriteBufferMarkerAMD` (bottom of pipe) into the chunk buffer, 64-bit as two markers, high half first (Bloodborne). **Copy model (U5-2):** the signal thread writes it once the tick completes | `SignalAfterHostCopies` → `SignalEop` (`liverpool.cpp:267-290`) |
| EOP with interrupt, EOS, compute RELEASE_MEM | signal thread `gow3:GpuSignals`: waits on the timeline semaphore (bounded 5 ms waits), writes the label through the backing view with one atomic store, raises the IRQ. Watchdog: "GPU stuck" vs "port threads waiting" after 10 s (Bloodborne problem 5, seen on the Deck) | deferred CPU write |
| WRITE_DATA (MediEvil: 4 and 8 bytes) | in place: a marker for ≤ 8 bytes (allowed inside a render pass), `vkCmdUpdateBuffer` above. Copy model: signal thread | `RunWriteData` waits on stage B (`:380-424`) |
| Flip | unchanged (IRQ on stage B after the frame's signals are issued). The present waits GPU-side | same |
| GPU idle / submission lock | after the last submission's tick completes (signal thread), **with an overlap of K submissions** (`GOW3_SUBMIT_AHEAD`, start at 2) and a leak watchdog (live heap allocations, as Bloodborne `gnmdriver.cpp:147-175`) | after stage A/B drained (`liverpool.cpp:185-200`) |
| WAIT_REG_MEM | pending-fence rule kept (in-order GPU ⇒ met); pending GPU labels stay "pending" until their tick completes; an unmet wait first **submits** unsubmitted signals, then yields (Bloodborne problem 4) | `:1350-1388` |
| Occlusion `ZPASS_DONE` | unchanged (synthetic at decode, or the HZB remap) | same |

**Not needed any more with GPU fences:**
- `WaitDeferredSignals` in drains and before writes;
- host-copy waits before fences;
- `RecorderFences` deferral, E1, W3.

**Still needed:** host copies before a *submit* that reads them, while copies exist.

**Submissions.**
- A label is visible only after its command buffer is submitted.
- Today 24-41 submissions a frame (run 3 slow-frame lines) [log]. If the instrumentation shows labels waiting on
  submission, flush after a label once ≥ N us of work is pending (Bloodborne `BB_HONEST_FLUSH_US`; a submit costs ~65 us
  there).

**Extension check.**
- `VK_AMD_buffer_marker` on RADV [inferred: Mesa exposes it on all GCN+; check in U5-0].
- Without it: the portable path (end the render pass, barrier, `vkCmdUpdateBuffer`). Bloodborne keeps that off on AMD for
  its pass breaks.

## 9. Textures, copy threads, staging, rings

- **Detile sources.**
  - `ObtainBufferForImage` (`buffer_cache.cpp:1079-1111`) copies guest bytes into staging (`CopySparseMemory`, `:1108`) for
    the detiler (`texture_cache/tile_manager.cpp`).
  - U5-8: when the range is in a chunk, return the chunk buffer + offset (Bloodborne `BB_CHUNK_TEXTURES`). No CPU copy;
    the GPU detiles from guest memory.
  - Image write traps stay (section 6).
- **Image readbacks** (`ProcessDownloadImages`, `texture_cache.cpp:303-460`): U5-8 copies image → guest memory in stream
  order (Bloodborne `texture_cache.cpp:125-139`); the game's GPU-ordered EOP covers it. The deferred-readback publish
  (CPU memcpy, protection, `Finish()` when full) goes for in-place targets.
  - `stale_readback` (previous frame's copy) stays as an option. It hides the latency the game expects to pay, and with
    honest fences it may no longer be needed (measure).
- **Copy threads** (`gpu/shim/gow3_copy.cpp:26-30`, 2 on the Deck): they serve uploads, stream copies and image staging.
  Once U5-6 and U5-9 are in, they go to 1 or 0 (a Deck-audit A/B).
- **Staging pool** (`vk_staging_buffer_pool.cpp:23-53`, 512 MiB kept, 16 MiB blocks): only image uploads and readbacks are
  left; keep 64-128 MiB (U1, `GOW3_STAGING_KEEP_MB`).
- **Stream buffer (128 MiB) and constant ring (96 MiB):** small read-only buffers (≤ 16 KiB, `STREAM_THRESHOLD`
  `buffer_cache.h:54`) are copied per bind today. In place (U5-9) they bind the arena directly. There is no PCIe penalty on
  the Deck, unlike Bloodborne's +3% GPU time on a discrete card. This needs GPU-ordered fences: the copies are today's
  snapshot that makes early fences safe.
- **Residency pool** (32 MiB chunks, `buffer_cache.cpp:365-460`): only non-in-place blocks use it.

## 10. Step plan

One change per launch, each launch with the user's go. Every step has its own switch; the earlier steps stay
switchable.

| Step | Change | Test (Deck, graveyard route, `GOW3_FRAME_STATS=1`) | Success criterion |
|---|---|---|---|
| **U5-0** | No game. Extend the probes (`research/tools/deck/`, from `bloodborne_pc/tools/sparse_host_probe.c`): `VK_AMD_buffer_marker` present; marker-visible latency (CPU polls a type-5 word while the GPU runs a long dispatch, then a marker); GPU **write** bandwidth to type 5 vs device-local; allocate 256 MiB type-5 chunks to 6 GiB and read `VK_EXT_memory_budget`; `vkQueueBindSparse` time for 1k / 10k binds | probe output | marker present; visible within 100 us of the GPU passing it; GPU write ≥ 70% of device-local; 6 GiB allocatable inside budget; binds < 1 ms per 1k |
| **U5-1** | Stats-only instrumentation (study-fence-waits.md §5), incl. fault map by PS4 type | **gate 2 run** (study §6) | lines print; FPS within noise of run 5 at the same spot; the estimate line and the per-type fault shares read |
| **U5-2** | GPU-completion fences **under the copy model**: signal thread on the timeline for EOP, EOS, WRITE_DATA, RELEASE_MEM; honest GPU idle with overlap K=2; `GOW3_GPU_FENCES` + toggle bit (switched at GPU idle) | live A/B with `toggle-ab.ps1`'s Linux twin, 20 s per state | ΔFPS ≥ −2%, 1% / 0.1% lows not worse; stage B busy down by ≥ 5 points (no WRITE_DATA / host-copy fence waits); 30 min without heap growth or crash (Bloodborne's corruption class) |
| **U5-3** | Guest direct memory from type-5 dma-buf chunks (`GOW3_U5_MEMORY=1`), **copy model otherwise unchanged** (the arena still device-local, tracking on) | launch A/B vs U5-2 | boots, 30 min play; write faults/s unchanged; game-thread time not worse (dma-buf CPU writes 4.9 vs memfd 1.0 GB/s may even help); GTT within budget |
| **U5-4** | EOP labels without IRQ and WRITE_DATA as GPU markers in place | live toggle | fence lag p50 drops from tick-completion to marker time; FPS ≥ U5-2 |
| **U5-5** | **First in-place class**: the PS4 type holding most faults (from U5-1: Onion if it holds them). Its buffer blocks bound in place; tracking and uploads off for them | live toggle (bit per class; a switch-off rebinds to device memory and re-uploads, at a frame boundary) | that type's faults → ~0; uploads MB/s down by its share; game-frame excess in slow frames down; **the user sees no corruption** (vertices, flicker, particles) |
| **U5-6** | Second type in place (Garlic WC/WB buffers) | live toggle | total write faults ≤ texture-streaming level (Bloodborne: 0/s standing); "host copies" blocked → ~0 |
| **U5-7** | GPU-written buffers in place; D2/D3/twins/N-13 bypassed for in-place ranges; port reads wait on write ticks | live toggle | downloads/twins/precise writes 0; "port reads waiting for GPU writes" small; no visual change |
| **U5-8** | Texture detile sources from chunks; image readbacks by GPU copy into guest memory | launch A/B | image upload CPU time ~0; level-load stalls shorter; HZB / torches correct (V2's black-box torches must not come back) |
| **U5-9** | Small read-only buffers / constants in place (stream buffer and ring off for in-place ranges) | live toggle | stream copies 47k/s → ~0; stage B `ObtainBuffer` us per packet down; GPU time not up > 3% |
| **U5-10** | Footprint and threads: staging keep 128 MiB, stream / ring 32 MiB, copy threads 1, `GOW3_DMEM_MB` for 800p | launch A/B | same FPS; GTT use down; one more core's worth of idle |

## 11. Failure modes and how each shows up

| Failure | Shows as | Detection / guard |
|---|---|---|
| A fence still signals before the GPU read it (any path left on the CPU-ordered model) | corrupted geometry, flicker, particles jumping: the guest rewrote data in flight | `GOW3_U5_VERIFY=1` (debug): hash bound in-place ranges at record time and again in a callback after their tick (Bloodborne `BB_WRITE_VERIFY`); log the first mismatch with its fence history |
| GPU idle before late labels | guest heap corruption after minutes (Bloodborne 7/7 crashes) | idle only after the signal thread drained; heap watchdog; long runs |
| Torn 64-bit labels | rare hangs / frees of live objects | one atomic store; markers high half first |
| WAIT_REG_MEM on an unsubmitted label | stage A spins, the frame stops | flush in the wait path; counter "WRM waited on unsubmitted" |
| Device lost / GPU hang | frozen picture, audio on (as Proton run 1) | bounded waits, watchdog line naming what waits |
| Submission lock serialises game and GPU | FPS drop ~⅓, the GPU idles between frames ("between submissions: GPU idle" in the profile) | `submit_lock` counters; K overlap; leak watchdog turns K off |
| Leak with overlap | live heap allocations climb, mmap growth | watchdog (Bloodborne: +100k allocations → K = 0) |
| An upload or download path still runs on an in-place block | stale data overwrites newer guest bytes (a staging snapshot copied over memory the CPU rewrote) | assert in `UploadCopies` / `DownloadMemory` that no copy targets an in-place block (debug), counter in release |
| GTT exhaustion | allocation failure at a level load, or device lost after long play | budget log; lazy chunks; refuse U5 at start if POOL_SIZE does not fit |
| Sparse bind stalls | frame spikes at streaming | bind ms counter, ≥ 5 ms logged |
| Snooped-memory contention (CPU busy + GPU reads of cached memory) | GPU time up in heavy views vs device-local | per-pass GPU profile A/B (`GOW3_GPU_PROFILE`) with the class toggles |
| Port reads of GPU-written memory without a wait | wrong draws from stale descriptor tables / counts | "port reads waiting" counter; Bloodborne's 0.7 case |
| Decommit cost on dma-buf views | game-thread spikes during streaming | L2 keep-views; decommit ms counter (Linux) |

## 12. Gate 2 first, then the decision

**Gate 2** = U5-1 (one stats run, study-fence-waits.md §6), then U5-2 (a live A/B of GPU-completion fences under the copy
model). U5-2 is the only direct measurement of the fence cost: it runs the real waits with the real lag, while the memory
stays as safe as today.

**Decision rule:**
- **Proceed to U5-3 onward** if U5-2 costs ≤ 2% average FPS and no 1% / 0.1% low loss, or if it **gains** (stage B
  freed). The expected savings that justify it:
  - stage B: host-copy and WRITE_DATA waits ~8-9% [log];
  - `obt_sync` + uploads ~0.6 ms a frame [log];
  - the game threads: ~760 faults a frame, run 6's +5.3-8.9 ms slow-frame excess [doc].
- **Stop** if U5-2 costs > 5% or the lows drop with K up to 3, because then the game serialises on its fences. Fall back
  to U2 (direct arena writes into host-visible device memory) and the fault fix keyed by the PS4 type.
- **Between 2 and 5%:** U5 only if U5-1's fault map puts ≥ 50% of the faults in one type, and an estimate from run 6
  (faults stretch frames) predicts more than the fence cost.

## 13. The PC fixes under U5 on the Deck (input to the "Deck audit of the PC fixes")

From fix-inventory.md. "In place" = U5 on for that class.

| Fix (fix-inventory section) | Under U5 on the Deck | Why |
|---|---|---|
| D2 + D2b precise CPU writes (§3, toggle bit 63) | **harmful for in-place ranges**: bypass | routes guest `memcpy` into GPU-written pages through the backing view + a queued GPU copy that would overwrite in-place bytes |
| D3 readback-queue downloads (§3) | unnecessary for buffers in place; keep for the rest | no arena copy to download |
| GPU write twins (§3) | unnecessary / **harmful** in place (the twin would mask the GPU's bytes) | no faults on GPU-written pages |
| N-13 fault page download (§3) | unnecessary in place | — |
| D1 unmap discard (§3) | moot | — |
| C3, N-11, N-24 (undefined / parked targets, §3) | keep | images |
| V3 volumes, V4 stencil plane (§3, §5) | keep | images |
| R1 readback submit (§3) | keep until U5-8; then moot | — |
| E1, W2, B1, W3 / W3b (§1, §8, §10) | unnecessary with GPU fences (E1, B1, W3 order fences after host copies; W2 still guards image readbacks until U5-8) | fences follow the GPU |
| Constant-ring reuse (§2) | moot once constants are in place (U5-9); keep before | its premise (bytes fixed within a command buffer) is about copies |
| S2 small read-only buffers from the arena (§2, `GOW3_STREAM_CLEAN`) | **harmful in place** if its "upload once" path writes an in-place block; bypass | the arena block *is* guest memory |
| Stream-class table (§2) | keep until U5-9 | — |
| Hot pages (opt-in), `GOW3_UFFD` (U4) | moot for buffers | no buffer tracking |
| Residency chunks allocated ahead (§2) | keep for non-in-place blocks; smaller | — |
| Staging keep 512 MiB, stream 128 MiB, ring 96 MiB | shrink (U5-10, U1) | GTT footprint |
| Copy threads (2) | 1 or 0 after U5-6 / U5-9 | — |
| Lock-free region reads, R2/R3, L2 region caches (§4) | keep | CPU work stays |
| M1 / decommit keeps access (§4, Windows) | Linux equivalent L2 still needed | dma-buf views have the same unmap/remap cost |
| L5 drain before commands (§5) | keep | correctness of stage A/B |
| HZB remap (§7) | keep | the pipeline stays deeper than the PS4's whatever the fences |
| `stale_readback` | A/B after U5-8 | may become unnecessary with GPU-copied readbacks + honest fences |
| `frames_queued=1` | keep; re-test with GPU fences (the guest's own waits then bound the queue too) | — |
| Precise vblank tick / precise limiter spins (§6) | not U5; already harmful on 4 cores (Proton run 4: precise tick off → flat 60) | Deck audit item |
| Draw-preparation workers (4 + scanner) | not U5; A/B (Bloodborne 0.7: on < 12 threads draws prepared on the threads that use them, Deck 68.6 → 72 FPS [BB 0.7]) | Deck audit item |

## 14. Where the type policy could help the PC build

- **Onion in host-visible memory over PCIe.**
  - Bloodborne's discrete-GPU rule: Onion in place, like an upload heap; Garlic in VRAM with tracking.
  - Onion data was slow on the PS4 too, so reading it over PCIe costs about what it cost there.
  - It needs Windows import of the section view (`VK_EXT_external_memory_host`; the one-day probe of
    study-guest-memory-direct.md §5) and GPU-ordered fences. Those are cheap on the PC: the GPU lag is 0.44 ms, against
    15.6 on the Deck.
- **The fault fix keyed by type.** If the faults sit in Onion pages, "untrack Onion pages, upload them once a frame" is a
  precise replacement for the heuristic hot-page set that failed (T: 14k pages, 33 FPS).
- **Cache actions as download points.** EOPs with an L2 flush and RELEASE_MEM with writeback are where the game expects
  GPU data in memory. Downloading GPU-written ranges there, instead of on a fault, removes the fault-time wait. This holds
  for either build (study §2.4).

## Open questions

1. Which PS4 type holds the ~760 faults a frame (U5-1).
2. What `equeue_wait` guest+0x6885e7 waits for, and whether the submission lock blocks MediEvil's submit thread (U5-1).
3. RADV `VK_AMD_buffer_marker`, marker visibility, GPU write bandwidth and GTT budget for 5.4 GiB (U5-0).
4. Should U1 shrink `GOW3_DMEM_MB` and drop the 512 MiB framebuffer-pool patch at 720p/800p before U5-3 (smaller chunks
   to allocate)?
5. Bloodborne 0.7 (`0fc2921`) rewrote `bbport_guest_memory.cpp` (+891 lines: `_chunks`, `_lazy`, `_probes`) and the layer
   module. Read it before porting U5-3; this draft rests on 0.51's code and 0.7's docs.
