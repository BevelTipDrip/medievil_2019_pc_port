# Study: Bloodborne 0.7's "one memory model", and what ports to `deck-lab`

Status: study, 2026-11-03. Read only: nothing built, launched or committed. Branch read: `deck-lab` at `3efee7a` (gate 2
stats committed, not yet run). Parent: [design-u5-deck-memory.md](design-u5-deck-memory.md) (U5 draft; this page lists
the changes it needs, section 4, without editing it). Related: [study-fence-waits.md](study-fence-waits.md),
[plan-linux-deck.md](plan-linux-deck.md) (Deck facts, runs 1-6, "U5 gate 1"), [study-guest-memory-direct.md](study-guest-memory-direct.md)
(Bloodborne 0.5, now outdated), [fix-inventory.md](fix-inventory.md).

**Bloodborne refs.** `bloodborne_pc` `origin/master` = `0fc2921` (0.7, 2026-10-29), read with `git show`; the working
tree (0.51, `3c57da4`) was not touched. `path:line` without a commit means `@0fc2921`. 0.6 = `8185b88`, 0.6-pre1 =
`72e5c8c`. Their docs are partly Russian; quotes below are translated.

Marks: **[code]** read in their code at `0fc2921` or ours at `3efee7a`. **[doc-m]** a number their docs report from a
run (measured by them, not by us). **[doc-c]** a claim in their docs or code comments without a number or log.
**[inferred]** my reasoning. Nothing here is verified in our game.

## Answer

- **What 0.7 is.** One path on every GPU (`docs/CHANGES_0.7.md:44-47`):
  - the game's direct memory is Vulkan-visible system memory at the game's own addresses;
  - every buffer binding is resolved by the "layer's memory module" (`gpu/layer/`) to a range of a chunk's own
    `VkBuffer`. No sparse arena by default. Bindings of 64 MiB and more go through a page table read by the translated
    shaders;
  - on discrete GPUs, hot known data gets VRAM "mirrors". On an integrated GPU (the Deck) there are none
    (`GarlicInVram()` is off, `buffer_cache.cpp:119-125`): everything is read and written in place;
  - buffer write tracking by page protection is gone. Writers announce their writes; only pages under images keep
    traps;
  - fences, labels, GPU idle and frame pacing follow the GPU timeline (buffer markers and a signal thread).
  - If the driver cannot do this, the game stops; there is no fallback to the 0.3 copy model any more.
- **Deck results:**
  - standing in the world: `0 write faults/s, 0 mirrors, host copies 0/frame, write traps hit 0`
    (`docs/unified_memory.md:18-19`) [doc-m];
  - FPS: 68.6 → 72 from CPU-side work, 63.5 → 65 from narrowing one memory wait (section 2).
  - Their Deck problems were memory-model problems: GTT double counting (device lost after ~80 min); stage A waiting
    for the GPU on every constant-RAM dump (12% of stage A); a rare heap-corruption crash still open (#32).
- **What ports.**
  - Nearly as is: the fence and label machinery, the frame pacing, the dma-buf chunk allocator and its startup probe,
    and the runtime's chunk plumbing.
  - Adapted: binding in place. MediEvil's pool commits and the UE4 commit churn favour the sparse arena (still in 0.7
    as `BB_LAYER_MEMORY=0`) over the layer module. The write-after-read waits for command-processor writes over
    in-place memory are also adapted.
  - Not applicable on the Deck: mirrors, the Windows import paths, lazy udmabuf imports.
- **First step:** extend the U5-0 probes with 0.7's own dma-buf check (offset mapping, CPU read speed through the
  driver's mapping) and with `VK_AMD_buffer_marker`. Then run gate 2 (already committed in `3efee7a`). Then port the
  signal thread (GPU-ordered fences, GPU idle and frame pacing) under the copy model.

## 1. 0.7's memory model, piece by piece

### 1.1 Which way, per GPU class

`BbGuestMemory::MemoryModuleFits` turns the layer module on for every vendor (`gpu/shim/bbport_guest_memory.cpp:121-153`).
`Usable` picks how guest memory reaches Vulkan (`:173-285`). `Install` stops the game if none works (`:292-297`,
`gpu/buffers/memory_model.cpp:10-18`, exit 23).

| GPU class | Guest memory | Vulkan memory type | VRAM mirrors |
|---|---|---|---|
| AMD discrete, Linux (RADV) | Vulkan allocation exported as **dma-buf**, mapped by the runtime at the game's addresses (`AllocChunk`, `bbport_guest_memory_chunks.cpp:9-95`) | `HOST_VISIBLE\|HOST_CACHED`, not `DEVICE_LOCAL` (`FindType`, `bbport_guest_memory.cpp:66-85`); `eDeviceAddress` allocate flag for the module (`chunks.cpp:47-53`) | yes: known data (loader- or GPU-written) promoted to VRAM; GPU writes into VRAM only for blocks the CPU never touched |
| **Integrated / Steam Deck (RADV)** | same as AMD: dma-buf chunks | same (the Deck's type 5, `HOST_VISIBLE\|HOST_COHERENT\|HOST_CACHED`, gate 1) | **none**: `integrated_gpu` (`buffer_cache.cpp:62-63`, `:167`) makes `GarlicInVram()` false (`:119-125`). `BB_UNIFIED_MEMORY=1/0` overrides the device type (`docs/unified_memory.md:9-11`) |
| NVIDIA / Intel, Linux | **host import** (`VK_EXT_external_memory_host`) of the runtime's memfd backing view: the chunk stays in the memfd (`CHUNK_IMPORTED`, `AllocImported` `chunks.cpp:208-210`); dma-buf as fallback (NVIDIA's dma-buf maps at offset 0 only) | cached host type first, then any host-visible non-device-local (`FindHostType`, `chunks.cpp:126-140`) | yes |
| Windows, all vendors | host import of the section's view if the driver accepts it; else **private regions**: each direct allocation is `VirtualAlloc`ed memory placed at its guest address, its only view, imported (`bbport_guest_memory.cpp:225-243`, `src/runtime_memory.c:172-182`, `backing.c:219-254`). A second mapping of the same physical memory elsewhere stops the game (`backing.c:223-228`) | as above | yes |
| Diagnostics | `BB_GUEST_UNCACHED=1` (AMD device-uncached types, imitates NVIDIA), `BB_GUEST_VRAM` (ReBAR VRAM: 9-18 FPS, own CPU reads, `docs/SESSION_2026-10-24_25.ru.md`), `BB_GUEST_MEMORY=hostsim` | | |

**Lazy imports are off.** `BB_GUEST_LAZY` (udmabuf imports of the memfd per 32 MiB granule) has been off by default
since 2026-10-23: "buffers across its imports kept breaking: explosions, black surfaces, GPU faults"
(`bbport_guest_memory.cpp:191-195`).

### 1.2 How chunks map at guest addresses

Runtime: `src/runtime_memory.c` plus its parts in `src/runtime_memory/` (one translation unit).
- **Pool.** One memfd for direct + flexible memory, sealed so udmabuf could use it, with a `backing_base` second view
  (`backing.c:25-47`). It is advised `MADV_HUGEPAGE`: "4 KiB ones cost it 0.8 ms a frame on a Steam Deck (TLB)"
  (`backing.c:11-23`) [doc-c].
- **Chunk table.**
  - `chunk_fds` per 256 MiB grid cell (`runtime_memory.c:146-153`).
  - `whole_chunks` mode: one chunk per **direct allocation**, forced whenever the layer module is on (`Install`,
    `bbport_guest_memory.cpp:322-325`), because "a mapping then lies whole in one chunk buffer".
- **When chunks are made.**
  - Eagerly, at `sceKernelAllocateDirectMemory` (`kernel_api.c:8-55`: `import_chunks_unlocked` then `ensure_chunks`).
  - The Vulkan allocation is made with the runtime's write lock released: an import took 200-360 ms on a laptop, and
    every GPU query of the memory map waited for it (`backing.c:124-155`) [doc-m].
- **Mapping.** `map_phys` maps each guest mapping from the chunk's dma-buf fd at `phys − chunk.phys`, split where it
  crosses chunks; ranges without a chunk come from the memfd (`backing.c:256-300`). `add_region` maps the backing view
  from the same fd (`runtime_memory.c:207-221`), so every runtime write through `backing_base` lands in the chunk.
- **Release.**
  - Memory in a GPU chunk is cleared with `memset`, not punched (`zero_phys`, `backing.c:324-345`; `GPU_CHUNK`,
    `runtime_memory.c:153`).
  - Pinned memfd ranges are cleared too, under one mutex (`punch_or_clear`, `backing.c:302-322`). An import noted
    between a release's check and its punch kept old pages: "a label the guest waited for forever, 2026-10-23"
    (`runtime_memory.c:158-162`).
- **Heaps grow on demand** (`BB_HEAP_GROW`, default on).
  - Bloodborne's 2287 MiB GraphicsPrivate heap gets GPU memory for its first 512 MiB only (`kernel_api.c:39-45`,
    `runtime_memory_set_grow`).
  - A hook in the game's own range allocator (`gpu/games/bloodborne/hooks_heap.cpp`, header) carves the rest into
    320 MiB pieces with 64 KiB fences, "so every buffer the GPU binds lies whole in one Vulkan chunk" (`:195`). The
    next piece gets its chunk by `runtime_memory_commit`, which copies any touched memfd pages, maps the game's views
    onto the chunk and tells the GPU side (`heap_commit.c:70-167`).
  - Pieces are made ahead on their own thread: "a host memory import of 320 MiB took 120-170 ms" in the game's
    allocating thread (`heap_commit.c:10-69`) [doc-m].
  - Pieces free for 60 s are given back (`runtime_memory_decommit`, `heap_commit.c:204-281`), their Vulkan memory
    freed 3 s later (`RetiredChunks`, `bbport_guest_memory.cpp:330-363`).
  - Why: "about 1.5 GB of the PS4 heaps were allocated but never used" (`docs/CHANGES_0.7.md:48-50`).

### 1.3 How buffers are bound

- **Layer module** (`gpu/layer/bblayer_gpu_memory.h`, "independent of the shadPS4 code: plain Vulkan handles"):
  - sources are guest chunks (by physical address) and mirrors (by guest address);
  - `Resolve(va, size)` answers None / InPlace / Mirror / Mixed (`:45-56`). InPlace needs the range "whole in one guest
    source and physically contiguous" (`ResolveInPlace`, `:115-118`);
  - ranges of `WholeLimit` = 64 MiB and more go through a device-address page table in the translated shaders
    (`:76`, `layer_paged.cpp`; `Shader::paged_range_hook`).
- **ObtainBuffer** (`gpu/buffers/buffer_cache.cpp:391-598`). In layer mode, written bindings, bindings over 16 KiB,
  GPU-modified ranges and mirrored ranges go to `LayerBind` (`:459-470`, `gpu/layer/layer_bind.cpp:47-427`): the chunk
  buffer at `phys − chunk.phys`. Small read-only bindings (≤ `STREAM_THRESHOLD` 16 KiB, `buffer_cache.h:92`) are
  **still copied** into the stream buffer on a copy thread (`:537-570`). The reason (`:399-403`): "commands decoded
  later (WriteData, constant RAM dumps) change them on the CPU in decode order, and a draw decoded before must keep its
  values (buffer renaming)". `BB_CONSTANTS_IN_PLACE=1` binds them in place; it is off by default
  (`memory_model.cpp:27-30`).
- **Sparse arena** (`BB_LAYER_MEMORY=0`, AMD only). Still present: `BindInPlace` binds 16 KiB blocks of the arena to
  the chunk's `VkDeviceMemory` (`buffer_residency.cpp:619-668`, `MIN_BLOCK_SIZE` `buffer_cache.h:87`).
- **Why they left the arena** [doc-m]:
  - NVIDIA's `vkQueueBindSparse` held the GPU for 10 s or more (`bbport_guest_memory.cpp:130-134`);
  - on an RX 7800 XT the module was faster: Oedon Chapel 234 vs 213 FPS, a Yharnam route 4.30 vs 4.60 ms a frame
    (`docs/CHANGES_0.6.md:13-21`);
  - only the module was right without the game profile: a stale VRAM copy overwrote allocator bookkeeping, issue #119
    (`docs/CHANGES_after_0.5.md:178-188`). That is a mirror problem, so it cannot happen on the Deck.
- **No PS4 memory type in the policy.** VRAM eligibility is "every byte last written by a loader or by the GPU"; "Any
  memory type: what decides is that every write there is one we hear of" (`buffer_residency.cpp:608-616`). In 0.51
  `type == 3` (Garlic WC) still counted, but only with write tracking on
  (`3c57da4:.../buffer_cache.cpp:1947-1950`). In 0.7 the type is only reported to the game (`sceKernelGetDirectMemoryType`,
  `kernel_api.c:352`). WB Onion 0, WC Garlic 3 and WB Garlic 10 decide nothing on the GPU side.

### 1.4 What is still copied

| Data | 0.7 on the Deck | Ref |
|---|---|---|
| Vertex, index, structured, indirect, > 16 KiB constants | in place (chunk buffer) | `buffer_cache.cpp:459-470` |
| GPU-written buffers | in place | `layer_bind.cpp:398-412` |
| Small read-only buffers (≤ 16 KiB) | **copied** per binding into the stream buffer / constant ring (copy threads), with ring dedup (`BB_RING_DEDUP`, `raster_pipe.cpp:351-357`) | `buffer_cache.cpp:399-403`, `:537-570` |
| Texture sources | read in place by the GPU detiler (`ObtainBufferForImage` → `LayerBind`; `BB_CHUNK_TEXTURES`) | `buffer_cache.cpp:650-686`; `docs/unified_memory.md:20-22` |
| Textures, render targets | `VkImage`s (detiled, PS4 tiling); arena-bound sampling was "smeared and ghosted" (`BB_INPLACE_TEXTURES` off) | `buffer_cache.cpp:75-81`; `unified_memory.md:26-30` |
| GPU-written images the CPU needs | written back into guest memory (texture cache write-backs; on eviction synchronously) | `texture_cache_memory.cpp`; `docs/CHANGES_2026-10-03.md:32-36` |
| Staging | only for image uploads outside chunks and the non-chunk fallback | `buffer_cache.cpp:688-700` |

"host copies 0/frame" in the Deck line counts **waits** for host copies, not the copies themselves
(`frame_stats.cpp:427-439`, `blocked: ... host copies %.1f%% (%.0f/frame ...)`) [code]. The constant copies continue.

### 1.5 What write tracking remains

- **Runtime write watch.** A bitmap, one bit per 4 KiB page below 2^40 (`gpu_traps.c:36-78`). The writers the GPU
  side cannot see coming (libc imports, file reads, hooked game code) check the bits after writing and report
  (`runtime_memory_note_cpu_write`). File reads into GPU memory call `runtime_memory_note_write` first (`:16-35`). No
  page protection for this.
- **Write traps with one owner.** One reason byte per page (`trap_reasons`, `gpu_traps.c:79-101`): image traps
  (`BB_IMAGE_TRAPS`), the layer's mirror traps, and read traps (no access) for pages whose newest data is elsewhere.
  The texture cache and the layer trap the same pages, and "one's unprotect must not drop the other's trap". On the
  Deck only image traps fire, "on texture streaming, not every frame" (`unified_memory.md:28-30`).
- **`PageWatch` / `DirtyTracker`** (`gpu/memory/page_watch.h`, `dirty_tracker.h`): per 4 MiB region, CPU-modified
  and GPU-modified bits. Pages are watched by each image over them, and by the buffer tracker only while a CPU copy is
  clean (mirrors).
- **Diagnostics.** `BB_WRITE_VERIFY=1` protects watched pages to catch unannounced writes (`memory_model.cpp:22-25`).
  `fault_sites.cpp` maps faults to game functions "to replace page watching by explicit hooks".

### 1.6 Fences, labels, EOP, GPU idle, the submission lock

All in `gpu/render/raster_signals.cpp` unless noted.

| Guest-visible event | 0.7 | Ref |
|---|---|---|
| EOP, 32/64-bit data, **no interrupt** | `vkCmdWriteBufferMarkerAMD(BottomOfPipe)` into the chunk buffer, high half first; the target must be in a guest chunk (`GuestChunkSource`); occlusion results written before it; queued as a "signal after GPU" for WAIT_REG_MEM bookkeeping. Without the extension: end the render pass, barrier, `vkCmdUpdateBuffer` (portable path). Off on GFX8 (Polaris stall, #203) and with `BB_FREE_CHECK` | `WriteLabelOnGpu` `:255-324`; `cp/cp_tasks.cpp:127-154` |
| EOP with interrupt, other data, EOS, compute RELEASE_MEM | `SignalAfterGpu(fn, label, value)`: stage B holds them per tick (`local_signals`) until the end of the guest submission (`PublishSignals` `:18-50`). Then the `bb:GpuSignals` thread waits on the timeline semaphore and runs them in tick order (`GpuSignalLoop` `:407-479`) | `:52-78` |
| EOP timestamp | written by the GPU (`WriteTimestampOnGpu`) | `:243-253` |
| WRITE_DATA | `vkCmdUpdateBuffer` into the in-place target, also into a mirror holding it whole. Over 8 bytes it invalidates the texture at that address (FX history clears, "white after save"). Up to 8 bytes it is also a signal for WAIT_REG_MEM | `WriteDataOnGpu` `:142-174`; `buffer_cache.cpp:619-648` |
| GPU idle IRQ | `SignalAfterGpu(SignalGpuIdle(generation))` after the submission's last packet. It is idle only if nothing was submitted meanwhile (`generation`) | `cp/command_processor.cpp:521-573`, `:611-623` |
| Frame retired | `SignalAfterGpu(NoteFramesRetired(frame))` after a frame's last packet | `command_processor.cpp:575-609` |
| Submission lock (`BB_SUBMIT_LOCK`, default `decode`) | the lock releases when stage A has decoded everything (`ReleaseSubmissionLock`). A frame's first `BB_SUBMIT_AHEAD`=2 submissions go straight through; later ones, and `sceGnmSubmitDone`, wait until the GPU finished the previous frame (`WaitPreviousFrame`). "The game builds the next frame in the context of the frame before the previous one: Yahar'gul 1080p 101 → 154 FPS". Leak watchdog: +100,000 live game heap allocations, or any runtime heap growth, turns the overlap off for the session (counted in Bloodborne's libc, `BbHeapSites`) | `gpu/platform/gnm/gnm_driver.cpp:6-14`, `:84-196`, `:538-567` |
| WAIT_REG_MEM | a fence handed to the recording thread counts as written (`PendingFenceValue`). Else: drain the draw pipe while it is not idle, then **flush unsubmitted signals** and yield. Repaired command buffers or a recent corrupt EOP give up after 500 ms | `cp/cp_graphics.cpp:739-827` |
| Unsubmitted work | the signal thread waits in bounded 2 ms steps and asks for a flush every 50 ms while the tick is unsubmitted. The decoder flushes after 0.2 ms idle with signals pending (`BB_HONEST_IDLE_US`, `command_processor.cpp:241-261`) | `:420-452` |
| Submission batching | one flush per `BB_HONEST_FLUSH_US`=2000 us while the GPU has work. At once when the GPU is idle and less than 64 KiB of draws is queued, or when it is on its last submission (`BB_HONEST_DRAIN_FLUSH`). "A submission costs it ~65 us (heavy scenes: 13 a frame; 95 → 100 FPS with half as many)" | `SubmitForSignalsNow` `:346-394` |
| Device lost / hang | device lost reported from the wait; 10 s without progress: the watchdog says whether the GPU is stuck or the port's threads wait for each other (the Deck froze with audio playing before) | `:453-460`, `GpuWatchdog` `:481-507` |

**Ordering of GPU writes and CPU reads.**
- **The game's CPU reads** of GPU-written memory are ordered by the game's own fences, now GPU-ordered. Nothing is
  downloaded on the Deck: no mirrors, so no copy-backs.
- **Command-processor writes on the CPU over memory the GPU still reads in place** (write-after-read):
  - DMA fills and copies go to the GPU in stream order when the GPU "holds" the range (in place, GPU-modified or
    mirrored). On the CPU now, "it would land before earlier draws read the old data" (`gpu/render/raster_memory.cpp:6-9`,
    `:19-79`).
  - `DumpConstRam` (constant engine) writes on the CPU. It first waits:
    - for stage B to reach the last packet reading those bytes;
    - **for the GPU** only if stage B bound them in place (`gpu_reads`, filled by `BufferCache::in_place_read_hook`,
      `buffer_cache.h:172-179`, `rasterizer.h:863-866`; `WaitForPendingReads`/`WaitForInPlaceReads`,
      `raster_pipe.cpp:155-300`).
  - The regression this guards: "a dump while it is still queued on the GPU gave it the next draw's data ... Rare: ~1
    of 1000 dumps" (`raster_pipe.cpp:263-267`).
  - Ordered tasks that read guest memory count as recording-thread reads, not GPU reads. Before, "the frame's
    OnSubmit task made every later dump wait for the GPU: Deck 12 % of stage A" (`rasterizer.h:887-891`) [doc-m].
- **Stage A's own reads of GPU-written ranges.** GPU writes are noted by packet (`NotePendingGpuWrite`,
  `raster_pipe.cpp:102-110`). A small constant range with a pending GPU write is not copied but bound in place
  (`PendingWriteOverlaps`, `:336-342`, `:540-546`). Shader code the GPU copied is synchronised (`SyncProgramWrites`,
  `rasterizer.h:885`).

### 1.7 The guest heap corruption class, and how it was closed (mostly)

Symptom: guest fault at `0x263b8e7`, a free list head `0x0000005300000000`: something wrote into a block the game had
freed (`docs/parallel_gpu.md:549-563`).

| Cause found | Fix | Ref |
|---|---|---|
| GPU idle signalled at decode while deferred fences were pending | GPU idle only after the pipe drained and the labels are written (`work_retired`); now GPU-ordered (table above) | `docs/CHANGES_2026-10-02.md:84-91` [doc-m: 40 min clean] |
| Labels written with `memcpy`: two overlapping stores; the game freed the block between them ("frequent on Steam Deck") | one atomic store (`StoreLabel`) | `docs/CHANGES_2026-10-06.md:66-68` [doc-c: "not checked on the Deck"] |
| Evicted texture written back asynchronously after its pages were unprotected, over memory the game reused | synchronous write-back while protected | `docs/CHANGES_2026-10-03.md:32-36` |
| Images in a freed heap piece deleted only at the end of a batch; the game re-issued the address and the GPU drew there first | deletion before the first submission after the free (black blotches) | `docs/NIGHT_2026-10-25.ru.md:31-35` [doc-m: Deck 139 of 143 ranges late → 0] |
| Import noted between a release's check and its punch | pin mutex | `runtime_memory.c:158-162` |
| Diagnostics | `BB_FREE_CHECK` / `BB_POOL_WATCH`: every port write into guest memory checked against the label pool's free lists, GPU writes included (`bbport_free_check.h`) | |

**Still open:** issue #32, "rare guest fault at `0x263b8e7`" (`docs/CHANGES_0.7.md:155`).
- "Main open crash ... all reports on AMD, incl. Deck".
- Lead: the texture cache above its critical mark all session, 241-1032 evictions right before the crash.
- "Two-hour runs on the Deck did not catch it" (`docs/GitHub_review_2026-10-26.ru.md:89`).

### 1.8 Memory budget (GTT, pinned memory)

- **GTT double counting with lazy imports.** "The kernel counts every import whole in GTT, the same pages again in
  each: a Steam Deck run imported 6.9 GiB of a 4 GiB pool in 80 minutes, GTT reached 8.1 GiB and command submission
  failed (device lost)" (`bbport_guest_memory_lazy.cpp:205-210`) [doc-m]. A budget over the memfd size allowed 6.4 GiB
  (GTT 8.5 GiB) the next session (`:234-236`). Fix: overlap budget 1 GiB on ≤ 12 GiB GTT (`:211-233`), then lazy imports
  off altogether. This is specific to overlapping udmabuf imports; Vulkan-allocated chunks do not overlap.
- **Texture collector on integrated GPUs.**
  - Before: the budget was "all heaps minus 8 GB for the system" (~1 GB on the Deck), against GTT usage that holds
    ~4 GB of guest buffers. The collector evicted up to 40 images per submission (~66 a frame) and re-uploaded them
    (`docs/CHANGES_2026-10-03.md:22-31`) [doc-m].
  - Now the marks are 70 / 85 / 95% of the driver's current budget (`texture_cache_memory.cpp:171-180`). This port has
    it already (`texture_cache.cpp:1994`) [code].
- **Telemetry.** A `Memory:` line per stats window: the game's VRAM and GTT from `/proc/self/fdinfo` (`drm-memory-gtt`),
  RSS and the parts the port knows (`gpu/platform/videoout/frame_stats.cpp:57-125`).
- **Heap grow on demand** (1.2) and **smaller rings on cards with ≤ 3 GB** (not integrated: `buffer_cache.cpp:130-135`).

## 2. Their measured results

| Where | Result | Kind | Ref |
|---|---|---|---|
| **Deck**, standing in the world (2026-10-23) | `0 write faults/s`, `0 mirrors of 0 MiB`, `host copies 0/frame`, `write traps hit 0` (no FPS given) | doc-m | `docs/unified_memory.md:17-19` |
| **Deck**, standing (FSR 4.1.1) | 68.6 → 72 FPS: draw-preparation workers off below 12 threads, CLEAR_STATE only touched blocks, fetch-shader decode cache, texture sets reused. GPU clock 1345 → 1400 MHz (15 W shared budget); stage A 15.3 → 13.9 us a draw | doc-m | `CHANGES_0.7.md:64-66`; `NIGHT_2026-10-25.ru.md:9-10`, `:25-30` |
| **Deck** | 63.5 → 65 FPS: stage A no longer waits for the GPU before descriptor tables when the frame's tasks only read CPU data. GPU waits ~70 → 1-3 ms/s | doc-m | `CHANGES_0.7.md:67-68`; `SESSION_2026-10-24_25.ru.md:26-29` |
| **Deck** | FSR 1 59 → 65 FPS (PGO + constant sync); FSR 4.1.1 48.8 → 51.3 (PGO); FG 34.9 → 38.4 | doc-m | `SESSION_2026-10-24_25.ru.md:10-11`; `CHANGES_0.7.md:63`, `:71` |
| **Deck** limit | GPU 97% busy and the 15 W limit; stage B 10-15 us a draw vs 2.7 on the PC (L3 size) | doc-m | `NIGHT_2026-10-25.ru.md:53-56` |
| **Deck** regression | lazy imports: GTT 8.1 GiB, device lost after ~80 min → fixed (lazy off) | doc-m | `CHANGES_0.7.md:142`; `bbport_guest_memory_lazy.cpp:205-210` |
| **Deck** regression | heap shrink: black blotches, missing reflection (139 of 143 freed ranges deleted late) → fixed | doc-m | `NIGHT_2026-10-25.ru.md:31-35` |
| **Deck** | 4 KiB memfd pages cost the GPU 0.8 ms a frame (TLB) → `MADV_HUGEPAGE` | doc-c | `backing.c:11-13` |
| PC, RX 7800 XT | GPU-written labels: ~330 a frame (61k/s) moved to the GPU; same FPS (233; 4 cores ~217 vs ~215) | doc-m | `CHANGES_after_0.4.md:9-21` |
| PC | submission overlap K=2: 101 → 154 FPS (Yahar'gul 1080p); K 2-6 no change in another scene; flush interval 500-1000 us +0.5% (noise), 4000-8000 worse | doc-m | `gnm_driver.cpp:9-12`; `CHANGES_after_0.5.md:201-205` |
| PC | layer module vs sparse arena 234 vs 213 FPS | doc-m | `CHANGES_0.6.md:18-19` |
| PC | occlusion queries counted only while open: 164 → 189 FPS (~0.8 ms GPU a frame) | doc-m | `CHANGES_0.7.md:57-61` |
| PC (dGPU only) | frame-sized GPU write left in system memory 95 → 132 FPS; FPS −28% after 17 min of teleports (mirror marks never cleared) → fixed | doc-m | `CHANGES_after_0.5.md:337-355`, `:418-446` |
| Regression (dGPU) | vertex explosions where a buffer crossed the boundary between two pieces of a grown heap | doc-c | `CHANGES_0.7.md` Fixes |
| Regression | flicker: a constant-RAM dump while the reading draw was still queued (~1 of 1000 dumps) → GPU wait over in-place reads | doc-m | `raster_pipe.cpp:263-267` |
| Regression | a label the guest waited for forever (pin race) → mutex | doc-c | `runtime_memory.c:158-162` |
| Open | #32 heap corruption, rare, AMD incl. Deck | doc-c | `GitHub_review_2026-10-26.ru.md:89` |

Their Deck FPS changes come from CPU work and from narrowing one memory wait. **Their docs have no Deck A/B of the
memory model itself**, and none of GPU-ordered fences against CPU fences: the Deck has run GPU labels since 0.4/0.5.

## 3. Port map for MediEvil (`deck-lab`)

**Code base.**
- Ours: shadPS4 `b4e7ae7` vendored under `gpu/shadps4/` with `gow3:` changes (`gpu/VENDOR.txt`), plus the Bloodborne
  0.3-0.5 era fixes listed in fix-inventory.md.
- Theirs: started from the same `b4e7ae7`, rewritten part by part; `gpu/shadps4` was removed on 2026-10-13
  (`gpu/VENDOR.txt@0fc2921`). Command processor `gpu/cp/`, renderer `gpu/render/`, caches `gpu/buffers/` and
  `gpu/textures/`, the module `gpu/layer/`.
- So "(a) portable" below means the logic and most lines carry over, adapted to our class and file layout
  (`liverpool.cpp`, `vk_rasterizer.cpp`, `vk_scheduler.cpp`, `buffer_cache.cpp`).
- Our runtime has **no chunk allocator at all**: the Linux branch maps the memfd only (`src/runtime_memory.c:441-465`)
  [code].

Effort: S ≤ 1 day, M 2-4 days, L ≥ 1 week of port work with builds [inferred].

| # | Piece (Bloodborne source) | Class | What differs here | Effort | Risk |
|---|---|---|---|---|---|
| 1 | Signal thread: `SignalAfterGpu`, `PublishSignals`, `GpuSignalLoop`, bounded waits, flush requests, device-lost report, watchdog (`raster_signals.cpp:18-128`, `:407-507`) | (a) | replaces `SignalAfterHostCopies` / `WaitDeferredSignals` (`vk_scheduler.cpp:338-360`, `liverpool.cpp:267-290`, `:380-460`). Our timeline is the `b4e7ae7` master semaphore (same `IsFree` / `Wait` / `Refresh` idea). Stage B holding signals until the submission end needs our draw pipe's `OnStageB` (present) | M | M (deadlocks, late labels) |
| 2 | GPU idle and frames retired after the GPU (`command_processor.cpp:521-623`) + pacing `decode` with K=2 (`gnm_driver.cpp:84-196`, `:538-567`) | (b) | ours: `liverpool.cpp:185-202` (drain + `WaitDeferredSignals` + `work_retired`), `gnmdriver.cpp:71-95`, `:2371-2386`. **The leak watchdog counts Bloodborne's libc allocations** (`BbHeapSites`). MediEvil (UE4) needs its own signal: runtime heap growth (direct / flexible bytes) or RSS growth | M | M-H (heap corruption class) |
| 3 | WAIT_REG_MEM: pending values count, flush unsubmitted signals, then yield (`cp_graphics.cpp:739-827`) | (a) | ours `liverpool.cpp:1350-1388` (pending-fence rule exists) | S | M (stage A spins) |
| 4 | Submission flush policy (`SubmitForSignalsNow` `:346-394`; decoder idle flush `command_processor.cpp:241-261`) | (a) | today 24-41 submissions a frame (run 3) | S | L |
| 5 | EOP labels by buffer marker (`WriteLabelOnGpu` `:255-324`; `cp_tasks.cpp:127-154`) and WRITE_DATA by `vkCmdUpdateBuffer` (`:142-174`) | (a) after #7 | needs the label target in a chunk (`GuestChunkSource`): **impossible under the copy model** (the memfd is not GPU-visible). Needs `VK_AMD_buffer_marker` enabled in `vk_instance.cpp` (not today) | S | M |
| 6 | dma-buf chunk allocator + startup check (`bbport_guest_memory.cpp:66-111`, `_chunks.cpp:9-123`, `_probes.cpp:347-457` `DmaBufWorks`: offset mapping, CPU read speed ≥ 1 GB/s through the driver's mapping) | (a) | new `gpu/shim/gow3_guest_memory.cpp`; Linux dma-buf path only (drop host import, private regions, lazy, hostsim, ReBAR) | S-M | L |
| 7 | Runtime chunk plumbing: `chunk_fds`, `map_phys` split, backing view on the chunk fd, `memset` on release, chunk made with the lock released, made-ahead thread (`runtime_memory.c:146-221`, `backing.c:93-155`, `:256-345`, `heap_commit.c:10-69`, `gpu_interface.c:76-104`) | (b) | **MediEvil's pool:** one 5,056 MiB pool block (type 0) + the 512 MiB framebuffer pool. Bloodborne's whole-allocation chunks would claim ~5.4 GiB of the Deck's 8 GB GTT at start. Its grow-on-demand hook is in Bloodborne's own allocator. Here: lazy 256 MiB grid chunks made at the first `sceKernelMemoryPoolCommit` touching a cell (as design §4), **never per commit**: UE4 decommits and recommits the same pool pages ~5-6 times a frame (fix-inventory "Decommit keeps access"). `pool_commit_impl` / `pool_decommit_impl` (`runtime_memory.c:1027-1092`) take physical runs first-fit, so one commit may span several runs and cells | M-L | M (GTT, decommit cost, mapping bugs) |
| 8 | Binding in place | (b) | **Arena (0.5 code, still `buffer_residency.cpp:619-668`):** per 16 KiB block, indifferent to grid cells and fragmented commits; our arena and `EnsureResident` exist (`buffer_cache.cpp:1262-1333`). **Layer module (0.7 default):** a binding must lie whole in one chunk and one physically contiguous run, or use the shader page table (`layer_paged.cpp` + shader translator `paged_range_hook`). With grid cells and UE4's fragmented pool runs, more bindings would cross (Bloodborne needed 64 KiB fences in its heap for this, and still had "vertex explosions" across piece boundaries). Arena first | M (arena) / L (module) | M / H |
| 9 | Stable VA → phys across decommit / recommit (Linux L2 "keep pool views") | (b) | 0.7 has no equivalent need (Bloodborne does not unmap during play). Ours: `pool_keep_views()` returns 0 on Linux (`runtime_memory.c:1001-1008`). Without it every recommit can move a VA to other physical pages: arena rebinds (or layer re-resolves) 5-6 times a frame [inferred] | M | M |
| 10 | Copy-model guards off for in-place ranges | (b) | 0.7 has none to bypass. Ours: D2/D2b precise writes, D3 downloads, twins, N-13, S2 (`GOW3_STREAM_CLEAN`), hot pages, uffd (fix-inventory §2-3) must not run on in-place blocks (design §13) | M | H (stale bytes over newer ones) |
| 11 | Write-after-read waits: DMA/fill in stream order when the GPU holds the range (`raster_memory.cpp:19-79`); const-RAM dumps wait only for in-place reads (`raster_pipe.cpp:155-300`, `in_place_read_hook`); ordered tasks reading guest memory tracked (`:960-968`); small constants with a pending GPU write bound in place (`:336-342`, `:540-546`) | (b) | our stage A / B split, ring reuse (fix-inventory "Constant-ring reuse": assumes bytes fixed within a command buffer) and HZB remap. **Unknown whether MediEvil's UE4 RHI uses the constant engine (`DumpConstRam`)**: gate 2 does not count CE packets | M | H (rare flicker) |
| 12 | Texture detile from chunks (`ObtainBufferForImage`, `buffer_cache.cpp:650-686`), image traps with one owner (`gpu_traps.c:79-...`), WRITE_DATA > 8 bytes invalidates images | (b) | ours copies to staging (`ObtainBufferForImage` → `CopySparseMemory`), protection via `page_manager.cpp`. Our texture-set memo (region stamps) is keyed on (un)registration, not on writes: unaffected [inferred] | S-M | M |
| 13 | Readbacks / write-backs: synchronous eviction write-back, dead images deleted before the next submission after a free | (b) | ours: deferred readbacks and `stale_readback`, `GOW3_PARK_UNMAPPED` (N-24, parks targets a guest unmap frees: check it against "deleted before the next submission") | S-M | M |
| 14 | GTT telemetry (`frame_stats.cpp:57-125`) | (a) | add to our frame stats on Linux | S | L |
| 15 | `MADV_HUGEPAGE` on memfd views (`backing.c:11-23`) | (a) | helps what stays in the memfd (flexible memory, cells not yet chunked). Chunk pages come from the GPU driver's allocator, not the memfd [inferred] | S | L |
| 16 | Heap grow hook (`hooks_heap.cpp`) | (c) | Bloodborne allocator offsets; MediEvil's pool commits are the natural growth points (#7) | — | — |
| 17 | Lazy udmabuf imports (`bbport_guest_memory_lazy.cpp`) | (c) | off in 0.7 ("broke buffers"); GTT double counting | — | — |
| 18 | Mirrors, layer traps, watch, copy-back, VRAM budget shares (`gpu/layer/*` except the module core) | (c) on the Deck | discrete GPUs only | — | — |
| 19 | Windows host import / private regions | (c) for `deck-lab` | private regions need "one mapping per allocation", which MediEvil's pool breaks (recommits at other addresses). A PC idea would be a pool laid out so that VA offset = phys offset [inferred] | — | — |
| 20 | `BbFreeCheck` / `BB_POOL_WATCH` | (c) as is | knows Bloodborne's label pool layout. The idea (log every port write into guest memory with its source) fits our `gow3_write_log.cpp` | — | — |
| 21 | Deck CPU items (not memory): draw-prep workers off below 12 threads, fetch-shader decode cache, CLEAR_STATE touched blocks, `rdtsc` polling | (b) | Deck audit (design §13) | S each | L |

## 4. Where 0.7 contradicts or improves design-u5-deck-memory.md (changes to make there)

1. **Summary / §2 "Bloodborne's road".**
   - 0.7's Deck path is the layer module, not "bound in place into the sparse arena".
   - "host copies 0/frame" counts waits, and small constants are still copied on the Deck (1.4).
   - The ~80 min device loss came from overlapping **udmabuf imports**, not from eager allocation. The eager-allocation
     lesson is "~1.5 GB allocated never used" (heap grow).
2. **§3 PS4 type as the policy input.**
   - 0.7 dropped the type entirely (`buffer_residency.cpp:614-616`) and, on an integrated GPU, puts every class in
     place at once.
   - Keep the fault map by type as a diagnostic. Replace "one toggle bit per in-place class, ordered by type" with one
     buffers-in-place switch plus debug bits (read-only > 16 KiB / GPU-written / texture sources).
   - Re-order U5-5 / U5-6 by binding kind, not by PS4 type.
3. **§4 chunks.**
   - Keep the lazy 256 MiB grid, but give the MediEvil-specific reasons:
     - one 5 GiB pool block;
     - UE4 recommit churn (never a chunk per commit);
     - Bloodborne's whole-allocation chunks need its heap-grow hook.
   - Add 0.7's details:
     - make the Vulkan allocation with the runtime lock released;
     - an optional made-ahead thread;
     - `memset` on release;
     - the `DmaBufWorks` check incl. CPU read speed;
     - the GTT `Memory:` line.
4. **§5 binding.**
   - State the trade-off explicitly. The arena is per block and indifferent to grid cells and fragmented runs. The
     layer module needs whole-chunk, physically contiguous bindings or a shader page table; Bloodborne needed heap
     fences and still saw piece-boundary vertex explosions.
   - The arena first remains right for MediEvil, but it is now Bloodborne's legacy AMD-only path (less tested).
   - Add L2 (stable VA → phys over decommit / recommit) as a hard prerequisite of in-place binding, its own step.
5. **§7 "a new wait class".** 0.7's Deck item (63.5 → 65) was a **write-after-read** wait: CPU-side command-processor
   writes (`DumpConstRam`, DMA, fills) over memory the GPU still reads in place. It was not about stage A reading
   GPU-written data. Add:
   - DMA / fills on the GPU in stream order when the range is in place;
   - const-RAM dumps waiting only for in-place reads (`gpu_reads`), not for every queued packet;
   - ordered tasks reading guest memory counted as recording-thread reads;
   - pending-GPU-write constants bound in place.
   Keep the read-after-write part (`WriteTicks`) for stage A reads of GPU-written ranges.
6. **§8 fences.**
   - Replace "GPU idle after the last submission's tick, with an overlap of K submissions" with 0.7's pacing: the lock
     released at decode; a frame's first K=2 submissions free; later submissions and `sceGnmSubmitDone` wait for the
     previous frame's **GPU completion**; GPU idle IRQ via the signal thread with a generation check.
   - The leak watchdog needs a MediEvil signal (not Bloodborne's libc counts).
   - WRITE_DATA: `vkCmdUpdateBuffer` for all sizes (not a marker for ≤ 8 bytes); > 8 bytes invalidate images.
   - Batching defaults: 2000 us, idle flush under 64 KiB backlog, drain flush, decoder idle 0.2 ms.
   - EOP markers need the target in a chunk: U5-4 must follow U5-3, as planned. Under the copy model U5-2 is the signal
     thread only.
7. **§9 constants.** U5-9 contradicts 0.7's default. 0.7 keeps ≤ 16 KiB read-only bindings copied even on the Deck
   (buffer renaming in decode order); `BB_CONSTANTS_IN_PLACE` is opt-in. Make U5-9 an experiment after item 5's waits,
   and compare with ring dedup (`BB_RING_DEDUP`, hash-based, in our port the C-rated "ring reuse").
8. **§11 failure modes.** Add:
   - heap-piece / grid-boundary crossings;
   - texture eviction write-backs after unprotect;
   - dead images deleted after the address was reused;
   - the pin/punch race;
   - #32 still open (soak with the texture cache under pressure).
9. **§1 scope (Windows).** 0.7 does run in place on Windows (host import or private regions), so "no Vulkan memory at
   fixed guest addresses" is too strong. But MediEvil's pool does not fit "one mapping per allocation"; keep Windows
   out, with that reason.
10. **§2 table.** Update the 0.5 refs (`buffer_cache.cpp:406-408` etc.) to the 0.7 paths above.

## 5. Recommended port order on `deck-lab`

One change per launch, each with the user's go; every step behind a switch, the earlier ones kept switchable.

| Step | Change | Read / port first | Deck test | Pass |
|---|---|---|---|---|
| 0 | **Probes, no game** (U5-0 extended): `VK_AMD_buffer_marker` on RADV; marker visibility to a polling CPU; GPU write bandwidth into type 5; **0.7's `DmaBufWorks`** (offset mapping, CPU read speed through the driver's mapping); 256 MiB chunk allocation time; GTT growth to 6 GiB via `drm-memory-gtt`; `vkQueueBindSparse` 1k / 10k | `_probes.cpp:347-457`, `_chunks.cpp:9-95`, `tools/sparse_host_probe.c` | probe output | as design U5-0, plus CPU reads ≥ 1 GB/s |
| 1 | **Gate 2 run** (stats in `3efee7a`) + two counters: CE packets (`DumpConstRam` count / bytes) and Linux pool commit / decommit rate and sizes | — | one stats run, graveyard | lines print; tells whether item 11 matters and how hot the pool churn is |
| 2 | **L2 on Linux**: decommit keeps the view (VA keeps phys) | our `pool_decommit_impl`, Windows M1 | launch A/B | decommit ms down, no visual change |
| 3 | **GPU-ordered fences under the copy model**: signal thread (EOP, EOS, WRITE_DATA, RELEASE_MEM), GPU idle and frames retired on it, WAIT_REG_MEM flush, batching, pacing `decode` K=2 with a MediEvil leak watchdog; toggle at a GPU-idle boundary | `raster_signals.cpp`, `command_processor.cpp:203-261`, `:505-623`, `gnm_driver.cpp:84-196`, `:538-567`, `cp_graphics.cpp:739-827` | live A/B 20 s a side + 30 min soak | design's U5-2 rule (ΔFPS ≥ −2%, lows not worse, no heap growth / crash) |
| 4 | **Chunk memory** (U5-3): `gow3_guest_memory.cpp` + runtime chunk plumbing, lazy grid cells at first pool commit, `memset` on release; copy model otherwise | `bbport_guest_memory*.cpp`, `runtime_memory.c:146-221`, `backing.c`, `heap_commit.c:10-69` | launch A/B, 30 min, `Memory:` GTT line | boots, GTT within budget, faults/s unchanged, game-thread time not worse |
| 5 | **EOP labels by buffer marker + WRITE_DATA on the GPU** into chunks (U5-4) | `WriteLabelOnGpu`, `WriteDataOnGpu`, `cp_tasks.cpp:127-154` | live toggle | fence lag down, FPS ≥ step 3 |
| 6 | **Read-only buffers > 16 KiB in place** in the arena (integrated: all chunked blocks); guards (#10) bypassed there; DMA / fills in stream order over in-place ranges | `buffer_residency.cpp:619-668`, `raster_memory.cpp` | live toggle; the user watches for corruption | faults down by their share; no visual change |
| 7 | **GPU-written buffers in place**; D2 / D3 / twins / N-13 off for them; stage A reads wait on write ticks | `layer_bind.cpp:398-412` (logic), `raster_pipe.cpp:102-110`, `:336-342` | live toggle | downloads / twins 0; no visual change |
| 8 | **Const-RAM / ring write-after-read waits** (only if step 1 shows CE dumps or ring writes over in-place ranges) | `raster_pipe.cpp:155-300`, `buffer_cache.h:172-179` | live toggle, long run | no flicker; GPU waits ms/s small |
| 9 | **Texture sources from chunks**, image traps with one owner, synchronous eviction write-back | `buffer_cache.cpp:650-686`, `gpu_traps.c:79-...` | launch A/B | image upload CPU ~0; HZB / torches right |
| 10 | Footprint and threads (U5-10), huge-page advice on memfd views | — | launch A/B | same FPS, GTT down |
| 11 | Experiment: constants in place vs ring dedup | `memory_model.cpp:27-30`, `raster_pipe.cpp:351-400` | live toggle | keep only if stage B gains and GPU time < +3% |
| later | Layer module, only if sparse binds show in the Deck's frame costs | `gpu/layer/bblayer_gpu_memory.*`, `layer_paged.cpp` | — | — |

**Read first, in this order:**
1. `gpu/render/raster_signals.cpp` (whole);
2. `gpu/cp/command_processor.cpp:203-261` and `:505-623`;
3. `gpu/platform/gnm/gnm_driver.cpp:1-196` and `:538-567`;
4. `gpu/shim/bbport_guest_memory.cpp` and `_chunks.cpp`;
5. `src/runtime_memory.c:140-243` and `src/runtime_memory/backing.c`;
6. `gpu/render/raster_memory.cpp`;
7. `gpu/render/raster_pipe.cpp:100-345`.

All of these are at `0fc2921`. Cite `0fc2921` in each port's commit message (START-HERE rule).

## Open questions

1. Does MediEvil's UE4 RHI use the constant engine (`DumpConstRam`) or `DMA_DATA` over buffers the GPU reads? (Step 1.)
2. How often does UE4 recommit pool memory at another physical address on Linux, and does L2 make it stable? (Steps 1-2.)
3. RADV `VK_AMD_buffer_marker`, marker visibility, GPU write bandwidth into type 5, GTT for 5.4 GiB of chunks (step 0).
4. A MediEvil signal for the overlap leak watchdog (Bloodborne counts its libc's live allocations).
5. Is #32's lead (texture eviction under memory pressure) a risk here? The Deck's texture cache runs near its budget
   once chunks share GTT.
