# Study: could the GPU use guest memory directly (no VRAM copy)?

Status: study, 2026-10-20. Read-only: no build, no launch, nothing committed. Parent: [START-HERE.md](START-HERE.md).
Related: [plan-frame-pacing.md](plan-frame-pacing.md) ("Upload audit", "Buffer copies and Resizable BAR / SAM", D2/D2b),
[plan-stage-b-split.md](plan-stage-b-split.md) (T-S1, D3, T-T1, T-L2, T-E1), [fix-inventory.md](fix-inventory.md).
Reference: `bloodborne_pc` at `3c57da4` (0.51).

## Answer

**No as a whole, and not worth it for the one class where it could work.** The Bloodborne port does not run its
GPU on guest memory either: on NVIDIA its default is the copy model this port already uses. Its "in place" mode
still copies everything the GPU reads often into VRAM. Putting all of guest memory in ReBAR VRAM was measured there
at 16 FPS. Here, the only class that could be read in place is small read-only buffers (constants), and that class
already costs stage B very little. Best case for stage B is about 0.05-0.2 us of the ~2.1 us per packet (2-10%). The
risk that it costs more than it saves is high: fences would have to wait for the GPU, which runs about a frame behind.
**Recommendation: do not start it.** At most, a one-day Windows probe of the import (end of this page) if the
question needs a definite answer.

## 1. What the Bloodborne port actually does

The Bloodborne port is Linux only: a memfd, `mmap` and dma-buf (`gpu/shim/bbport_guest_memory.cpp:11-12`, `361-372`).

| Mode | Switch | What the GPU reads | Default |
|---|---|---|---|
| 0.3 model (copies, page protection, readbacks) | `BB_PC_MODEL=0` | VRAM copies, the same design as this port | **NVIDIA, Intel and others** (`docs/CHANGES_0.5.md:18-20`, `bbport_guest_memory.cpp:500-536`) |
| New model, sparse arena | `BB_PC_MODEL=1`, `BB_LAYER_MEMORY=0` | Guest chunks are Vulkan **cached system memory exported as dma-buf** and mapped by the runtime at guest addresses (`AllocChunk` :94-175, `FindType` :46-65 picks HOST_VISIBLE\|HOST_CACHED, not device-local). They are bound in place into the sparse arena, and hot blocks are **moved to VRAM by sparse rebinding** | AMD |
| New model, layer memory module | `BB_LAYER_MEMORY=1` (auto off AMD when the new model is forced) | Guest chunks **imported with VK_EXT_external_memory_host** (`ImportHost` :206-261, `AllocImported` :265-305). Bound in place, but anything read often is **copied into VRAM "mirrors"**, and writes are trapped to keep the mirrors right (`docs/NVIDIA_MEMORY_MODEL.ru.md:21-30`, `:75-81`) | none (NVIDIA opt-in) |
| All guest memory in ReBAR VRAM | `BB_GUEST_VRAM=1` (`bbport_guest_memory.cpp:36-44`, memory type DEVICE_LOCAL\|HOST_VISIBLE) | VRAM directly | experiment: **16 FPS in the menu**, because the CPU reads it over PCIe (`docs/CHANGES_after_0.4.md:138`) |

Other switches: `BB_GUEST_GPU_MEMORY` / `BB_GUEST_IN_PLACE` (Install :701-718), `BB_GUEST_MEMORY=host|dmabuf`
(:671-675), `BB_FIXED_ARENA`, `BB_LAYER_GPU_WRITES`, `BB_CONSTANTS_IN_PLACE`, `BB_GPU_LABELS`.

Measured there:
- **Everything in place, no VRAM copies** (`BB_FIXED_ARENA=1`): **48 FPS against 210** on an RX 7800 XT
  (`bbport_guest_memory.cpp:509-510`). The NVIDIA tester saw 10-15 FPS with no mirrors, then 30-35 against 60-70 in
  the old model (`NVIDIA_MEMORY_MODEL.ru.md:133-142`).
- **GPU writes to host memory** (`BB_LAYER_GPU_WRITES=0`): 60-70 FPS against ~200 (`NVIDIA_MEMORY_MODEL.ru.md:84-87`).
  Shader output must stay in VRAM.
- **Constants in place** (`BB_CONSTANTS_IN_PLACE=1`): parsing 0.8 us faster per draw, GPU +3% (constants read over
  PCIe), **same FPS**, so off by default (`CHANGES_after_0.4.md:80-85`).
- **Sparse rebinding on NVIDIA** (Linux driver) held the GPU for 10 s or more (`bbport_guest_memory.cpp:503-510`).
  NVIDIA sparse buffers take only VRAM (`CHANGES_after_0.5.md:44-46`).
- **The precondition:** in-place reads are unsafe while fences signal before the GPU has run the work. The guest
  rewrites per-frame data while the GPU still reads it. Their first attempt was dropped for this reason
  (`docs/parallel_gpu.md:189-197`). The new model works only because the GPU itself writes EOP/EOS labels in
  stream order (`VK_AMD_buffer_marker`, `CHANGES_after_0.4.md:5-17`; `Rasterizer::WriteLabelOnGpu`,
  `vk_rasterizer.cpp:294`).

By resource class, in the Bloodborne port's in-place modes:
- **Textures** are still images: detiled, uploaded and kept in the texture cache.
- **Render targets** are images on the GPU only. The game's copy shader is special-cased to write both copies
  (`NVIDIA_MEMORY_MODEL.ru.md:88-91`).
- **Buffers** are bound in place only when they are cold. Hot read-only data lives in VRAM copies, and writes stay
  in VRAM for blocks the CPU never touched.

"Like Bloodborne" therefore means: read cold or per-frame buffers from cached host memory over PCIe, and keep a VRAM
copy (still with write tracking) of everything hot. It does not mean "no copies".

## 2. Feasibility here

Test PC (vulkaninfo, RTX 4090, driver 616.92):
- `VK_EXT_external_memory_host` is present, with `minImportedHostPointerAlignment` 0x1000.
- `VK_AMD_buffer_marker` is present (line 1162 of the dump); the Bloodborne port assumed NVIDIA lacks it.
- `VK_EXT_map_memory_placed` is **absent**.
- Memory type 4 (DEVICE_LOCAL\|HOST_VISIBLE\|HOST_COHERENT, not cached) is on heap 0 (23.57 GiB). Type 3 (cached
  host) is on heap 1 (31.81 GiB).
- sparseBinding / sparseResidencyBuffer are true.

How guest memory is built today:
- One pagefile-backed section, `CreateFileMapping2(SEC_COMMIT)`, holds the direct and flexible pool
  (`src/runtime_memory.c:355-363`).
- The section is mapped once unprotected as `backing_base` (:99, :361).
- Each guest mapping is another view at a fixed guest address, placed into a reserved placeholder: `VirtualAlloc2`
  with MEM_RESERVE_PLACEHOLDER (:207-209), then `MapViewOfFile3` with MEM_REPLACE_PLACEHOLDER (:213-218).
  `carve_host` splits and coalesces placeholders (:233-280).
- Page tracking is `VirtualProtect` on the guest views (`runtime_memory_gpu_protect` :1211-1221).
- M1 keeps views across a decommit (`pool_decommit` → `host_keep_view` :948-963, :343-347).
- The GPU side:
  - device-local sparse arenas (`buffer_cache.cpp:370-399`, memory type from eDeviceLocal);
  - a 128 MiB `Stream` buffer for read-only binds ≤ 16 KiB (`buffer_cache.cpp:329`, `buffer_cache.h:51`,
    `ObtainBuffer` :776-801). It is already ReBAR VRAM. It is copied on every bind, dirty or not, and is not tracked;
  - staging (`HostUncached`) + `CopyBuffer` for arena uploads (`SynchronizeMemory` :1064-1110, `UploadCopies` :1117).
- Guest fences signal **after the host copies queued before them**, not after GPU execution
  (`liverpool.cpp:278-288`, `Scheduler::SignalAfterHostCopies` `vk_scheduler.cpp:290-304`).

### (a) Import guest memory with VK_EXT_external_memory_host

- **What would be imported:** the backing view (`backing_base + phys`). It is never protected. Protection on the
  guest views does not affect GPU DMA, so page tracking keeps working where it is still needed. Alignment (4 KiB)
  is no issue: the section is 64 KiB aligned and PS4 pages are 16 KiB.
- **Unknown 1, Windows driver support:**
  - Whether NVIDIA's Windows driver accepts a pointer into a **file-mapping view** (not `VirtualAlloc` memory) is
    untested. The Bloodborne probe checks only a Linux memfd (`tools/host_import_probe.c`).
  - A 7.1 GiB import pins 7.1 GiB of RAM (heap 1 suggests 64 GB of system RAM) and may need chunks (for example
    256 MiB).
  - Pinning would also end the kernel trims M1 worked around, a side benefit.
- **Unknown 2, sparse interplay:**
  - NVIDIA sparse buffers take only VRAM (Bloodborne, above), so imported memory cannot go into the existing arenas.
  - It needs separate plain buffers per chunk, with a device address or a page table for binds that span chunks.
    That is the Bloodborne layer module's design: `buffer_cache_layer.cpp` 1,590 lines plus
    `bblayer_gpu_memory.*` 413 lines, plus weeks of correctness fixes (vertex explosions, stale mirrors,
    `NVIDIA_MEMORY_MODEL.ru.md:66-91`).
- **Blocker, fences:** in-place reads need GPU-completion fences (Bloodborne precondition above).
  - `VK_AMD_buffer_marker` exists here, so GPU-written labels are possible. But then every guest-visible write
    (WRITE_DATA, EOS, flip) needs GPU stream order too, and the game waits on those fences.
  - Evidence that fence latency costs this port: `GOW3_ASYNC_FENCES` measured slower (79 vs 83 FPS, "the guest waits
    on these fences", `liverpool.cpp:270-272`). The GPU runs about a frame behind the CPU (T-HZ3,
    plan-stage-b-split.md).
- **The GPU reading host memory:** PCIe 4.0 x16 is ~25 GB/s against ~1 TB/s for VRAM.
  - Stream-class reads, ~1.97 GB/s (below), fit, at Bloodborne's +3% GPU.
  - Arena-class *bound* ranges are ~35 GB/s in the heavy view (below). In place, those are Bloodborne's 48-FPS case:
    they must stay in VRAM, with tracking.

### (b) Allocate guest memory from ReBAR (type 4) and map it as the guest address space

**Not possible here, and not worth it if it were.**
- Vulkan memory is mapped only where `vkMapMemory` puts it. Without `VK_EXT_map_memory_placed` (absent) it cannot
  sit at fixed guest addresses.
- A `VK_KHR_external_memory_win32` handle is not a section `MapViewOfFile3` can map, so there is no aliasing either.
  The runtime needs both: one storage, many views at chosen addresses (`runtime_memory.c:213-216`), plus the backing
  view.
- Type 4 is not HOST_CACHED: the CPU reads write-combined memory over PCIe. The game's code runs on its direct
  memory. Bloodborne measured exactly this: **16 FPS** (`CHANGES_after_0.4.md:138`); its dma-buf path also rejects
  any mapping read below 1 GB/s (`bbport_guest_memory.cpp:626-638`).
- Narrow uses of ReBAR are already in place or small: the stream buffer is ReBAR VRAM; staging in VRAM
  (plan-frame-pacing.md option 4) saves no CPU work.

### (c) By resource class

| Class | Today | Could live GPU-only / in place? |
|---|---|---|
| Sampled textures | Images, detiled; uploads ~0 MB/s in gameplay after the intro movie (T-U1, upload audit) | No: images need tiling and formats anyway. Nothing to save |
| Render targets / depth | Images, GPU-only already; readbacks only of small images (luminance, 1x1-8x8, stale) | Already GPU-only |
| Shader-written buffers | Sparse VRAM arena; downloads **0/s**, precise CPU writes 58/s (0.42 MB/s), twinned 0 (T-E1 `t-e1-eos-deferred.log:7360` area; D2/D2b/twins) | Must stay in VRAM (Bloodborne: writes over the bus 60-70 vs ~200 FPS). Nothing left to save |
| Large read-only buffers (vertex / index pools, structured) | Arena, page-tracked, dirty ranges uploaded (staging + GPU copy) | No: ~35 GB/s of bound ranges would cross PCIe. Tracking stays |
| Small read-only buffers ≤ 16 KiB (UE4 constants, per-draw data) | CPU copy into the ReBAR stream buffer on **every bind**, ~620k copies / ~1.97 GB/s per second in the heavy view, untracked | **The only candidate** for in-place reads, and only with GPU-completion fences |
| Labels / fences / query results | Written by the CPU after copies | GPU-written labels possible (`VK_AMD_buffer_marker` is here); needed only if something is read in place |

Page tracking that would become unnecessary: **none**. The one class that could move in place (stream) is not
tracked today. Tracking serves the arena class, which has to stay.

## 3. What it costs today (heavy view, queries off, 4K)

| Cost | Measure | Source |
|---|---|---|
| Stage B per packet | 2.03-2.23 us, 92-94% busy, ~350-410k packets/s | T-S1 `t-s1-sections.log:399`; T-E1 `t-e1-eos-deferred.log` (sections lines) |
| `ObtainBuffer` (all callers, inclusive) | 0.19-0.23 us per packet (~9%), stream and arena paths together | T-S1 / T-E1 sections |
| Ordered tasks after E1 + W2 | 0.23 us: `RunWriteData` waiting for deferred signals 0.15 (about 1 ms once a frame), end of pipe 0.03-0.04 | plan-stage-b-split.md T-E1 |
| D2 queue (`before record`) | 0.008 us | T-S1 |
| Stream copies (CPU, on the recorder / copy threads) | ~620k/s, ~1.97 GB/s (0x2000-0x2060 regions summed) | `t-u1-uploads.log:9287-9295` |
| Arena binds | ~320k/s, ~35 GB/s of ranges bound | same |
| Buffer uploads (staging + GPU copy) | 380-585 MB/s (~2-3.3 MB a frame; median 504 MB/s over 320 windows of T-T1/T-L2/T-S1) | "Uploads:" lines |
| Render passes ended by upload copies | ~3 a frame (`vk_runtime.cpp:116`: 2,388 per 5 s) of ~500 | `t-u1-uploads.log` "Render pass ends" |
| Synchronous buffer downloads | 0/s | T-E1 "Buffer downloads/s" |
| Pages unprotected by write faults | 66k-200k 4 KiB pages/s (median 127k), in 256 KiB fault windows; handler time 0-7 ms inside 40-520 ms stall frames | "write faults/s", Stall "fault handlers" lines (T-E1, T-T1, T-L2, T-S1) |
| Stage B blocked on recorder / copy threads | 0.4% / 0% (E1 + W2) | T-E1 |

The copies themselves run on the recorder and copy threads, which are not the limit. Stage B pays only for queueing
them (part of `ObtainBuffer`) and for the once-a-frame ordering wait.

## 4. Expected gain and risks

If only the stream class is read in place, with GPU-written labels (the only variant that could be correct):
- **Stage B:**
  - The stream path of `ObtainBuffer` shrinks to a chunk lookup: about −0.03-0.06 us per packet. Stream binds are
    ~1.7 per draw. The reserve and closure cost a few tens of ns each, and the lookup plus `IsRegionGpuModified`
    stays.
  - The `RunWriteData` ordering wait (0.15 us) goes only if all guest-visible writes move to GPU stream order.
  - Arena uploads still pass through the copy threads, so part of that wait stays.
  - **Total about 0.05-0.2 us of ~2.1 (2-10%)**, if fences cost nothing.
- **Frame time:** heavy view 130 → at most ~133-143 FPS. Light views are proportionally the same.
  Bloodborne's measurement of the same change: −0.8 us per draw parsing, **same FPS**.
- **Risks:**
  1. **Guest fences would wait for real GPU completion.** They signal at copy time today. With the GPU about a frame
     behind (T-HZ3) and the game waiting on fences (`GOW3_ASYNC_FENCES` result), this can cost more than 10%.
  2. GPU +~3% (constants over PCIe).
  3. Import of a section view on Windows NVIDIA is unproven, and 7 GiB would be pinned.
  4. A separate non-sparse buffer path. The arenas cannot take imported memory on NVIDIA.
  5. A correctness class the port does not have today: GPU reads of data the guest is rewriting. Bloodborne's took
     weeks.
- **Confidence:**
  - High that (b) and "all in place" are worse: measured in the Bloodborne port, and (b) is structurally impossible
    without placed maps.
  - Medium on the 2-10% ceiling for (a): derived from the T-S1 / T-E1 sections, not measured.
  - Low on the fence cost: never measured with GPU-completion fences in MediEvil.

## 5. If it is pursued anyway: probe first (no game launch)

A standalone Win32 port of `bloodborne_pc/tools/host_import_probe.c`. It answers unknown 1 in a day:
1. `CreateFileMapping2(SEC_COMMIT)` + two `MapViewOfFile3` views. Import 256 MiB of the backing view as
   HOST_ALLOCATION. Does it succeed, which memory types, how long does the import take?
2. Coherence: CPU writes through the other view, GPU reads, and the reverse.
3. `VirtualProtect` on the guest view while imported: do write faults still fire, and is the GPU unaffected?
4. GPU read speed of imported memory compared with a VRAM copy.
5. Import of 7 GiB in chunks: does pinning fail?

Only if all five pass and stage B is still the limit after option (c): prototype constants in place behind a
switch, with GPU-written labels. Measure the guest's fence waits before anything else.

Cheaper ways to cut the same costs, if wanted:
- Option (c) of plan-stage-b-split.md (a second recording stage).
- Skipping stream copies of ranges that did not change since the last bind. ~1.97 GB/s is copied dirty or not. That
  costs the copy threads rather than stage B, so it matters only once they become a limit.
