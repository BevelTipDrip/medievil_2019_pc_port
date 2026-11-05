# Implemented: stage B per draw, round 2 (R1, R2, T1, T2) (2026-11-05)

Status: built (Windows, `build-clang64.sh`), unit tests pass, **not run**. Branch `stage-b-per-draw-2` (from
`deck-lab` `8e83bdd`): `e30bb42` (sections), `1f384e5` (R2), `a8b0c13` (T1), `3d42992` + `35f3930` (T2), `ccfa08a`
(R1), `9ed8157` (tools). Linux paths: no `#ifndef _WIN32` code changed; R1's C code uses only `pthread` / `__atomic` /
`calloc` (checked by reading, not built).
Inputs: [study-stage-b-per-draw.md](study-stage-b-per-draw.md) (C3, C4, C6), [plan-linux-deck.md](plan-linux-deck.md)
(latest sections), `data/deck-texset-1.log` (heavy view, 1,213 draws, `GOW3_SECTIONS=1`).

| # | Cut | Where | Switch (second toggle word) | Default | Expected (Deck heavy frame) |
|---|---|---|---|---|---|
| R2 | Residency check from a bitmap of resident blocks | `gow3_resident_blocks.h`, `buffer_cache.cpp/.h` | **65536** (set: interval search again) | **on** (exact, unit-tested) | obt_resident 0.32 -> ~0.05 ms: **-0.2 to -0.25 ms** |
| T1 | Texture binds of stages without images skip the slow path's set-up | `vk_rasterizer.cpp` (`BindTextures`) | **131072** (set: slow path again) | **on** (exact) | ~970 binds x ~0.1 us: **-0.08 to -0.12 ms** |
| T2 | Texture-set memo for sets of 17-32 images and for draws without a prepared draw | `vk_rasterizer.cpp/.h`, `gow3_texset_table.h` | **262144** (set: off) | **off**: `GOW3_TEXSET_WIDE=1` at start | **-0.2 to -0.35 ms** (texslow 0.92 ms today) |
| R1 | Region cache index, filled by the lock-free searches | `src/runtime_memory.c` | **524288** (set: off) | **off**: `GOW3_REGION_INDEX=1` at start | **-0.1 to -0.6 ms** (new `clamp` section says; stage A and copy threads gain too) |
| - | Sections `obt_upload`, `clamp` | `gow3_sections.h`, `memory_tracker.h`, `buffer_cache.cpp`, `gow3gpu.cpp` | `GOW3_SECTIONS=1` | - | diagnostics |

All four: about **-0.6 to -1.3 ms** of stage B's ~8.9 ms draw recording a heavy frame (sections on; they inflate
small parts), so +4-9% FPS while stage B is the limit. Each cut **removes** work (no work moved to another thread),
so it saves CPU power too. Every bit only switches its item off, as the others do; bits 4096-32768 are left to the
parallel work.

New `GOW3_FRAME_STATS=1` lines (added to `deck_summary.py`'s extra lines):
- `Stage B cuts per frame: residency bitmap (R2 on|off) N, searched N; no-image texture binds (T1 on|off) N; wide
  texture-set memo (T2 on|off|toggled off): over 16 images N lookups, N hits; no prepared draw N lookups, N hits;
  table N MB`
- `Region cache per frame (R1 on|off|toggled off): index hits N, ring hits N, searched lock-free N, locked N; index
  fills N (N threads)` (all threads; printed after "Region table per frame")

`sections_frames.py`'s by-state table now also shows vertex, ObtainBuffer, obt_resident, obt_sync, obt_upload,
clamp.

## 1. Where the per-draw time goes (read from the code and deck-texset-1)

Per heavy frame: 1,213 draws, ~2,900 `ObtainBuffer` calls (the C5 counter: 2,876 buffer-cache readback asks a
frame), ~920 texture-set lookups (98.25% hits), ~1,000 descriptor pushes (16.8 writes each), ~136 descriptor set
updates (1.2 us each).

**vertex (1.72 ms, 1.4 us a draw).** `ResolveVertexBuffers`: copies the prepared attributes / bindings / V#s, then
per merged range (1-2 a draw) `ClampRangeSize` -> `runtime_memory_clamp`, `ObtainBuffer`, `IsBufferAccessed`, then
the host buffer / offset / size / stride arrays. Most of it is `ObtainBuffer` (~0.6 us a call with sections) and the
clamp: the clamp's per-thread region cache is filled only on the locked path, and since the lock-free search
(T-P2) nearly every lookup takes that one, so the cache is nearly always empty and each clamp binary-searches
~13,000 regions (520 KB of table, the Deck's L3 is 4 MB): **R1**. The new `clamp` section measures it.

**ObtainBuffer (1.71 ms).** Per call: the C5 readback pre-check, the small-buffer GPU-modified check (`obt_regions`),
the stream copy for small dynamic buffers (`obt_stream`), `GetArena`, `EnsureResident` (`obt_resident` 0.32 ms: a
binary search of `resident_ranges`, a sorted vector of a few thousand backings, and a gap walk, although residency
only ever grows: **R2**), `SynchronizeMemory` (`obt_sync`).

**obt_sync (0.67 ms) with almost nothing uploaded.** ~2,100 calls a frame only do the tracker's lock-free "any page
CPU-written?" bit check (~0.1 us). The rest is ~4 calls a frame that do upload: ~3.1 MB a frame (`Arena writes`:
staged 4.2 a frame, 3.12 MB), the pages of the ~750 guest write faults a frame (58k/s x 4 KiB). Each of those takes
the tracker lock, clears the CPU bits and **re-protects the pages** (`UpdateProtection` -> `runtime_memory_gpu_protect`
-> `mprotect` runs: a TLB shootdown on every core the process runs on), handles twins, stages the copy on the copy
threads and records the copy (3 render pass ends a frame). The new `obt_upload` section (inside `obt_sync`) measures
that part. Not cut: the re-protection is what makes the guest's next write fault so the next upload happens (hot
pages, which skip it, were rejected: 14k pages re-uploaded on every use).

**texslow (0.92 ms), by cause** ("slow texture binds per 1000 draws", scaled to 1,213 draws):

| Cause | Per frame | us each | ms | Cut |
|---|---|---|---|---|
| over 16 images | ~54 | 7.04 | 0.38 | T2 (memoized up to 32) |
| unprepared draw (the workers had not reached it; mostly the particle-sim run) | ~156 | 1.80 | 0.28 | T2 (hashes at the bind) |
| memo off or no image (stages without images: most vertex shaders) | ~970 | 0.18 | 0.17 | T1 |
| dispatch | ~34 | 1.56 | 0.05 | - |
| memo miss | ~18 | 2.92 | 0.05 | see below |

**Why ~16 texture sets a frame miss.** All ~16 are sets the slow path cannot keep because they sample the pass's own
target ("not kept: own colour target 56%, own depth target 44%", 3,808 of 3,808 misses per 262,144 draws): the image
is bound as the attachment (feedback or read-only depth layout), so the set is dropped (key 0) and the next frame's
lookup misses again ("other key"). It is the study's C10 (own-target memo); at ~0.05 ms a frame it stays parked.

**Also looked at, not done here:**
- Descriptors (0.99 ms): push constants + the bulk push copy (~1.4 KB a draw) + 136 set updates. The set updates
  could run on the recorder (C6), which moves work rather than removing it. Descriptor update templates
  (`vkCmdPushDescriptorSetWithTemplateKHR`) would halve the copy and help RADV, but need a template per layout and the
  mip-array layouts: a larger change.
- texset hits (1.05 ms): the per-image validation and emit touch each `Image` object (cache misses); the compact
  layout showed the table itself is not the cost. No exact cheap stamp for image state exists yet.
- C4 (one recorded command per draw) and C7 (vertex input / index rebind elision): C7 needs every host pass that binds
  a pipeline without dynamic vertex input to invalidate; C4 touches the shared `Pipeline::BindResources`. Both larger.

## 2. The cuts

### R2: residency bitmap (`1f384e5`, second word 65536, default on)

`Gow3Resident::Blocks`: a bitmap of arena blocks (chunks of 2^16 blocks, 8 KiB each, made on first use; 1,024
chunk pointers for 2^40 bytes at 16 KiB or more a block). `EnsureResident` sets the blocks of every backing it adds
to `resident_ranges`, in the same place and whatever the toggle; it returns at once when every block of the request
is set. Exact: residency never shrinks (nothing removes a range), so "all bits set" is the same answer as "ForEachGap
finds no gap", after which the old code returned with nothing to bind. Same thread discipline as `resident_ranges`.
Test: `ninja -C out/gpu resident-blocks-test` (1.2M checks against `IntervalList` over random ranges added the way
`EnsureResident` adds gaps, plus word / chunk / top-of-bitmap edges): passes.

### T1: no-image texture binds (`a8b0c13`, second word 131072, default on)

A stage without images (most vertex shaders) took the slow path: a 16-entry resolved array zeroed, proxy flags,
registry and scene queries, the stats timer, all for images. Now it makes the slow path's only effects with an empty
image list (reset the binding lists, overflow and description storage, bump the bind epoch) and binds the samplers.
Exact by inspection: the memo lookup returns before taking a slot when there is no image, so nothing else in the slow
path has an effect. The `texslow` section no longer includes these binds (they stay in `textures`).

### T2: wide texture-set memo (`3d42992`, `GOW3_TEXSET_WIDE=1`, second word 262144 off)

- Sets of 17-32 images: a table of their own (`WideTextureSet`, the table-as-before layout with 32 entries, 1,024
  slots two-way, ~3 MB made on first use) through the same `BindTexturesFromSetIn` (now generic over the table size:
  `WaysOf(key, two_way, slots)`), validation, keep and drop as every memoized set.
- Stages of draws without a prepared draw (the workers had not reached it, or prepared it from other registers;
  indirect draws): their T# hashes are computed at the bind (`ImageDescHash` of the sharps the slow path reads, as the
  workers compute them), and the memo is used with them (either table). Dispatches keep the slow path (`35f3930`).
- The slow path's resolved images moved to a member array; a call with a set slot resets its images' entries (the only
  ones read), as the zeroed local array did (calls without a slot no longer zero 1.4 KB).

Same results: a hit replays a set the slow path resolved for the same stage and T# hashes, under the checks every
memoized set gets today (registry generation or region stamps, scene generation, each image's backing, rebind, target,
up-to-date, proxy, upscaler redirect). With the switch off every condition and table is as before. Opt-in because it
binds more kinds of passes from the memo (multi-texture post / lighting passes, the particle sim): look at the
picture in the A/B. Expected: over-16 sets ~54 a frame from ~7 us to ~3-5 us (a hit still validates and emits each
image), unprepared ~150 a frame from ~1.8 to ~1 us. `texset-table-test` also checks the slot choice over 1,024 slots.

### R1: region cache index (`ccfa08a`, `GOW3_REGION_INDEX=1`, second word 524288 off)

`runtime_memory_clamp` / `_region` / `_is_mapped` also try one of 1,024 per-thread entries picked by the address's
64 KiB page (made on first use, freed at thread exit). The lock-free searches fill it with the region they found and
its bucket stamp, read inside the search's window. A fill is kept only when the table generation was the same even
value before and after the search: no writer held the table lock meanwhile, and every table edit happens under that
lock and bumps the edited range's buckets first, so the entry is one `cache_region` could have made under the read
lock, and it is checked by its stamp exactly as those are (any later edit of the region changes the stamp: the stamp
is a sum of counters that only grow). First-word bit 1 (no region cache) turns it off too. Opt-in because it changes
the runtime's memory core (stage A and the copy threads use it too); no unit test (the runtime tests build on Linux
only). The counters say how often the index answers; the `clamp` section says what it saves on stage B.

## 3. Deck test (one launch, the user's go)

Settings as the texset run (67%, auto clocks, logging ini `deck.sh ini logging`, heavy graveyard view, standing
still). Build the AppImage from this branch first and `deck.sh deploy` it.

1. Keep every line extra-env.txt has now (`deck.sh status` shows them; the baseline trap: `GOW3_ARENA_HOST_VISIBLE=0`
   stays) and add the two opt-ins and the sections, for example:
   `deck.sh env "stage B per draw 2" GOW3_FRAME_STATS=1 GOW3_ARENA_HOST_VISIBLE=0 GOW3_TEXSET_WIDE=1
   GOW3_REGION_INDEX=1 GOW3_SECTIONS=1 GOW3_SLOW_FRAMES=1.4` (plus the lines already there).
2. Start with `deck.sh toggle "0 0"` (all four on). In the heavy view:
   - `deck.sh ab "0 0" "0 983040" 30 5` (all four off vs on; 983040 = 65536 + 131072 + 262144 + 524288).
   - Then one pair each, same launch: `deck.sh ab "0 0" "0 65536" 20 3` (R2), `"0 0" "0 131072"` (T1),
     `"0 0" "0 262144"` (T2), `"0 0" "0 524288"` (R1).
3. `deck.sh pull deck-per-draw-2`, then `deck.sh summary deck-per-draw-2` (FPS, stage B busy, the new lines by state)
   and `py -3.14 research/tools/sections_frames.py research/docs/data/deck-per-draw-2.log 1000` (draw, vertex,
   ObtainBuffer, obt_resident, obt_sync, obt_upload, clamp, texslow by state).

What to read:
- R2: `obt_resident` drops to near zero; "residency bitmap" ~2,100 a frame, "searched" a handful.
- T1: "no-image texture binds" ~970 a frame; `texslow` down ~0.15 ms.
- T2: "over 16 images" ~54 lookups a frame with hits close to that; "no prepared draw" ~150 lookups mostly hits;
  `texslow` down further, `texset` up a little (hits cost too). **Look at the picture**: lighting, fog, post, torches
  and particles while toggling `0 262144`.
- R1: "index hits" vs "searched lock-free" (want most lookups from the index); `clamp` and `vertex` down. Fence lag,
  culling and pop-in the same.
- All: FPS and stage B busy by state; the sum of the parts' drops should match.

Pass: each cut lowers its part by about the expected ms with the picture unchanged. Then the defaults: R2 and T1 stay
on; T2 and R1 become default-on candidates if they pass (drop the opt-in env check).

## 4. Build and tests (2026-11-05, Windows)

- `bash research/tools/build-clang64.sh`: builds after every commit (ThinLTO). The worktree needed
  `git -c core.longpaths=true checkout` in `gpu/third_party/fsr-vulkan` (paths over 260 characters).
- `ninja -C out/gpu resident-blocks-test texset-table-test`: both pass.
- `py -3.14 -m unittest discover -s tests`: 129 run, 2 failures + 6 errors (the 8 known), 29 skipped.
