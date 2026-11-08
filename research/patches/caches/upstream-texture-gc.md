---
id: upstream-texture-gc
title: Upstream texture GC limits from the driver's memory budget; slot reuse and sampler destruction fixes
area: caches
status: active
platforms: all
default_windows: on
default_deck: on
origin: upstream 1.3.4
class: G
commits: bf95396, 4d72031, b7b78fb
files: gpu/shadps4/video_core/texture_cache/texture_cache.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.h
symbols: TextureCache::GarbageCollectImages, TextureCache::GarbageCollectSamplers, TextureCache::RunGarbageCollector, trigger_gc_memory, pressure_gc_memory, critical_gc_memory, gc_tick, IsStencilProxyCandidate, GetAssociatedDepth, FindDepthTarget, ResolveDepthOverlap, DeferOperation
switches: GOW3_GC_BUDGET_MB, GOW3_GC_TOTAL_MB, GOW3_GC_STARTUP_RULE
tests: T-C0
docs: fix-inventory.md#from-upstream-medievil-133-138-bc160201983-and-contributors-merged-into-lab, plan-upstream-merge.md#1-where-upstream-and-lab-overlap, plan-frame-pacing.md#t-c0-result-2026-10-18-1510-1515-the-cache-re-creates-and-re-uploads-20-images-per-frame, plan-frame-pacing.md#lead-c-texture-cache-churn-second
depends_on:
related: sampler-memo, park-unmapped, driver-memory-off-stage-b, lineage-deferred-readbacks, V4, C3
checked: 2026-11-07
---
## What it does

Four fixes from the Uncharted port, taken in upstream 1.3.4. (1) `GarbageCollectImages` re-reads the driver's
current budget each run (`GetDeviceMemoryBudgetNow`) and sets trigger / pressure / critical to 70 / 85 / 95% of
it on every GPU. Before, only integrated GPUs did this (Bloodborne lineage `bf95396`, Deck). Discrete GPUs used
the constructor's startup rule. (2) Slot reuse: `slot_images.insert` can move every image, so
`ResolveDepthOverlap` and `FindDepthTarget` fetch their `Image&` again after it. (3) `GarbageCollectSamplers` moves an evicted sampler into a
`shared_ptr` kept by `scheduler.DeferOperation`, so it is destroyed once the GPU has passed it. (4) Stencil proxy
check: only an 8-bit or format-less image stands for a stencil plane (`IsStencilProxyCandidate`).

## Invariants

- The budget rule needs `instance.CanReportMemoryUsage()`. Without it: trigger 0, pressure / critical at the
  1.5 / 3 GB defaults. A budget read of 0 keeps the previous limits. Below the trigger the collector returns
  before taking the cache mutex: nothing is evicted.
- `GOW3_GC_STARTUP_RULE=1` restores the startup rule on discrete GPUs. `GOW3_GC_BUDGET_MB=N` forces a fixed
  budget on any GPU. `GOW3_GC_TOTAL_MB=N` feeds the startup rule as if the GPU had N MiB. All are test switches.
- Eviction (`clean_up`): a tiled image with GPU data is never evicted. A GPU-written image is evicted only under
  pressure, and is downloaded synchronously first (`DownloadImageMemory(id, true)`) while its pages are still
  protected; a deferred write-back once landed over the game's new data.
- No `Image&` may be held across `slot_images.insert` (or anything that registers an image). Fetch it again by ID.
- A sampler may be destroyed only after the GPU passes it (the `DeferOperation` hold). sampler-memo reuses a
  handle only while `memo.tick == gc_tick`. Samplers are freed only below `gc_tick - ticks_to_destroy` (16 / 80 /
  160 ticks), so a handle used in the current tick stays alive. `gc_tick` advances once per `RunGarbageCollector`.
- `GetAssociatedDepth` disassociates a non-candidate image: a colour texture that reuses a stencil's memory reads
  its own data.

## Interactions

- sampler-memo: keyed on `GcTick()`; depends on the tick and LRU rules above.
- park-unmapped: `RunGarbageCollector` evicts parked images (`EvictParkedImages`) before the image GC.
- driver-memory-off-stage-b: `FreeImage` from the GC goes through its worker frees, so the driver usage the next
  run reads drops only once the worker has freed the memory (inferred, not measured).
- lineage-deferred-readbacks: `clean_up` resolves pending readbacks (`ReadbackSource::CacheCleanup`) first.
- V4 / C3: the stencil proxy check and the unmap / undefined paths both touch depth-stencil association.

## Evidence

- Upstream 1.3.4 (code comment, Uncharted port, RTX 5060 Laptop 8 GB): the startup rule evicted images still in
  use: 40-75 ms frames while moving through a level.
- Ours before the merge (T-C0, 24 GB card): GPU memory 4.4 GB, collector trigger 7.1 GB: "never runs"
  (plan-upstream-merge.md: trigger 7,077 / pressure 17,430 MiB). N-27 merge test: no regression. The trigger
  under the budget rule on that card was not written up (by the code, 70% of the driver's budget: higher than
  7.1 GB on a 24 GB card, inferred). Not verified in game as a change of its own.

## History and pitfalls

- Lead C (plan-frame-pacing.md) suspected images piling up below a high trigger. T-C1 later showed the churn was
  volume expands (V3), not GC. A low trigger has the opposite risk: evicting live images costs re-uploads.
- On integrated GPUs the usage counts guest-memory buffer heaps. The startup budget evicted images every
  submission (`bf95396` comment), which is why the Deck compares against the current budget.
