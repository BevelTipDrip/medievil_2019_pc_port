---
id: driver-memory-off-stage-b
title: GPU driver memory calls off stage B: image frees on a worker, residency from 32 MB chunks allocated ahead
area: caches
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: b1dae86097
files: gpu/shadps4/video_core/texture_cache/image.cpp, gpu/shadps4/video_core/texture_cache/image.h, gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp, gpu/shadps4/video_core/buffer_cache/buffer_cache.h, gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp
symbols: ImageReleaser, DrainImageReleases, UniqueImage::Destroy, ResidencyPool, BufferCache::EnsureResident, residency_pool
switches: GOW3_IMAGE_RELEASE_THREAD, GOW3_RESIDENCY_CHUNK_MB
docs: fix-inventory.md#2-draw-translation-cost-stage-b-per-draw, plan-frame-pacing.md#cpu-oscillation-waits-and-hitches-trace-cpu-osc-1-2026-10-25, plan-frame-pacing.md#stage-bs-gpu-memory-calls-moved-off-it-2026-10-26-launched-by-claude-with-the-users-go
related: R2-residency-bitmap, U2, park-unmapped, upstream-texture-gc
checked: 2026-11-07
---
## What it does

Both changes take kernel calls that hold the GPU driver's lock (`vkFreeMemory`, `vkAllocateMemory`) off stage B.
1. `UniqueImage::Destroy` queues `vmaDestroyImage` to `ImageReleaser`, thread `gow3:ImageRelease`. VMA frees an
   empty block with `vkFreeMemory`.
2. `EnsureResident` takes arena memory from `ResidencyPool`: 32 MB chunks handed out in order, with thread
   `gow3:Residency` allocating the next chunk once the current one is half used. A range across a chunk end
   becomes two backings and two sparse binds.
`GOW3_IMAGE_RELEASE_THREAD=0` and `GOW3_RESIDENCY_CHUNK_MB=0` restore the old inline paths.

## Invariants

- An image reaches `Destroy` only once the GPU is done with it (the callers' existing rule). The worker only delays
  the free. It must never become a way to free earlier.
- `UniqueImage::Create`: when `vmaCreateImage` fails, `DrainImageReleases()` runs and the create is retried once,
  because images still queued may hold the memory. Remove the drain and a full budget becomes a crash
  (`ASSERT_MSG`).
- `Instance::~Instance` drains the releases before `vmaDestroyAllocator`. A new shutdown path must do the same.
- The VMA allocator is created without `VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT`, so frees from the
  worker thread are safe. Adding that flag would break this.
- Resident blocks are never released. `ResidencyPool::Take` only moves forward through a chunk and has no free.
  If residency ever shrinks, the pool (and R2's bitmap) must change.
- `Take` waits on `ready_cv` only if the chunk ahead is still being allocated, and allocates "on demand" (on
  stage B, the old exposure) only if no chunk is ready and none was requested. Each chunk prints "Buffer
  residency: 32 MB chunk N allocated at start|ahead|on demand[, mapped]"; any "on demand" line means the worker
  fell behind.

## Interactions

- R2-residency-bitmap sets its bits in the same loop that adds the pool pieces (one backing per piece).
- U2 (`GOW3_ARENA_HOST_VISIBLE`) makes the pool allocate a host-visible type and map each chunk (`Chunk::host`).
  U2 stays off when the first chunk cannot be mapped (`FirstChunkMapped`).
- Image frees come in bursts with texture-cache churn (guest unmaps, GC). park-unmapped and upstream-texture-gc
  change how often they happen, not where.

## Evidence

- Trace cpu-osc-1 (Windows, MSI Afterburner on): 11 lock waits in 20 s, 62 ms in total, max 33 ms, all but one on
  stage B. 254 stage B records inside `D3DKMTCreateAllocation` (88, from `EnsureResident`) or
  `D3DKMTDestroyAllocation2` (166, from `~UniqueImage`) [V].
- img-release-1: 1 wait / 1.5 ms (on a driver thread); stage B in `D3DKMTDestroyAllocation2` 166 -> 0 [V]. The
  t-ir2 A/B without Afterburner was equal within noise (FPS 194 / 195).
- t-rp1 (Afterburner on): 21 chunks, all but the first allocated ahead, none on demand; heavy area 193 FPS, p99
  6.84 ms, the same as runs without Afterburner [V].
- The user, before the fix: "MSI was dragging down performance quite a bit". Not measured on the Deck (on there).

## History and pitfalls

- The cause was any GPU monitor that polls through NVAPI driver escapes, not the port. The patch removes stage B's
  exposure to it, and helps without a monitor only by moving kernel round trips off stage B.
- The first note blamed a heap grow in the `stream_class` insert. That was wrong: the insert was only stage B's
  next sample after the wait.
