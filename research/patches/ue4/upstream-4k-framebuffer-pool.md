---
id: upstream-4k-framebuffer-pool
title: Upstream "Native 4K framebuffer pool (512 MB)" eboot patch with more direct memory (4K crash fix)
area: ue4
status: active
platforms: all
default_windows: on
default_deck: on
origin: upstream 1.3.0
class: M
commits:
files: patches/MediEvil.xml, scripts/patches.py, scripts/graphics_options.py, src/runtime_memory.c
symbols: patch_requirements, DMEM_RETAIL_MB, selected_patches, POOL_PATCH, pool_size_bytes
switches: GOW3_DMEM_MB
tests: T-R3, T-I1b
docs: fix-inventory.md#caveats-in-the-evidence, study-boot-resolution.md#framebuffer-pool-per-size
depends_on:
related: 4k-memory, I1, upstream-0102-eu-support, pool-kept-pages
verified: 2026-11-07
---
## What it does

The PS4 memory manager gives framebuffers a 128 MB pool at 0x4000000000 (a buddy allocator over 64 KB pages, 128 MB
of direct memory mapped at start). A 3840x2160 target (33.6 MB) takes a 64 MB block, so 4K ran out and crashed.
Upstream's patch (1.3.0 `3a16e0e`, 01.02 block in PR #26 `c7a9ef3`) makes the pool 512 MB with 11 writes: buddy
pages 0x800 -> 0x2000, the direct-memory allocation and mapping, the accounting, the pool capacity, and the two free
paths' pool test (`addr >> 27 == 0x800` -> `addr >> 29 == 0x200`). Its note "Requires setting DMEM to 512 MB" makes
`patch_requirements` ask for 5,056 + 512 = 5,568 MiB (`GOW3_DMEM_MB`).

## Invariants

- All 11 writes go together. The free paths must classify the whole 512 MB as framebuffer memory: a 4K target
  above 128 MB freed as Flexible gave a negative page index and crashed on the next resolution change.
- The direct memory must cover the pool. `patch_requirements` fills `GOW3_DMEM_MB` only when unset, so 4k-memory's
  7,104 above 1080p wins. `pool_size_bytes` accepts 5,056-16,384 MiB and falls back to 5,056 otherwise (then the
  game's general pool is 512 MiB smaller than intended) [inferred].
- Gated by the eboot sha256 per version only (`eboot_matches`). The lines have no `Original` bytes, so
  `GOW3_FORCE_PATCHES` on another build writes blind.
- `isEnabled="true"`: in the default selection at every resolution (`selected_patches`); at 1080p and below the
  launcher's Patches page can untick it. `output_environment` forces it above 1080p whatever the page says.

## Interactions

- 4k-memory: adds 2 GiB more direct memory above 1080p. The game takes all direct memory left after this pool as
  one memory pool, so `GOW3_DMEM_MB` is the GPU-memory knob.
- I1: 4K boot ("4K display metrics at boot") and every size above 1080p need it.
- upstream-0102-eu-support: the 01.02 block and the per-version selection.
- pool-kept-pages: the pool size decides how much memory the guest pool reuses.

## Evidence

- Deck at 720p (`data/deck-baseline-1-ab.log`): applied by default ("Patches: ... 'Native 4K framebuffer pool (512
  MB)'"), memory pool expanded by 5,301,534,720 bytes (5,568 - 512 MiB, less 64 KiB).
- 4K with 7,104 MiB: pool +6,592 MiB (fix-inventory Caveats; the logs give 6,912,147,456 bytes, again 64 KiB less). T-R3 (1800p): the user, "everything seemed to work as
  expected". T-I1b (4K menus): the user, "it can be navigated now".

## History and pitfalls

- The crash fix at 4K is upstream's. This port's 2026-10-20 pak values (`r.GarlicHeapSizeMB`,
  `r.Streaming.PoolSize`) did nothing and were dropped (4k-memory).
- The note's "512 MB" is extra memory on top of retail (`patch_requirements` reads it so), not the total.
