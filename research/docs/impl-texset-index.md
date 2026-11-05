# Implementation: the texture-set memo in a compact layout, and stage B task / sleep instrumentation

Status: built 2026-11-04 on `deck-lab` (Windows and Linux AppImage). **Not run in game**: nothing here is verified
(verified = the user confirmed it in game). Commits: `320af9b` (instrumentation), `1e4ca3d` + `366d634` (compact
layout, Linux build fix), and this page with the per-state split of `sections_frames.py`.
Inputs: [study-deck-memory-budget.md](study-deck-memory-budget.md) 2.9 and rank 2,
[study-deck-patch-audit.md](study-deck-patch-audit.md) (c) item 4 and #25, the Deck sections log
`data/deck-sections-1.log` (heavy frames: stage B 12.95 ms, "texset" 1.09 of it, "tasks" 3.30).

| Item | Change | Where | Switch | Default |
|---|---|---|---|---|
| 1a | "Stage B sleeps per frame" gives each site's ms, not only the total | `driver.cpp` | `GOW3_FRAME_STATS=1` | - |
| 1b | GOW3_SECTIONS: one section per ordered-task kind | `gow3_sections.h`, `liverpool.cpp`, `vk_rasterizer.cpp`, `vk_gpu_signals.cpp`; `sections_frames.py` | `GOW3_SECTIONS=1` | - |
| 2 | Texture-set memo in a compact layout: same slots and decisions, 44.0 MB instead of 52.2, a hit reads ~half the cache lines, huge pages on Linux | `gow3_texset_table.h/.cpp`, `vk_rasterizer.h/.cpp`, `driver.cpp` | **second toggle word 2048** (set: the table as before, live) | **on** |

## 1. Instrumentation

### 1a. Stage B sleeps by site

The line now reads (new part in bold):

`Stage B sleeps per frame (spin off): over 0.2 ms N (packet ring N, recorder N, GPU waits N, present N); all N (T ms`
**`: packet ring ms, recorder ms, GPU waits ms, present ms`**`); spins N (ms, N ended the wait)`

The sites are `Gow3StageBWait::Site` (`gow3_stage_b_wait.h`): packet ring = no packet from stage A (`DrawPipe::Run`),
recorder = `Scheduler::SyncRecording`, GPU waits = timeline ticks (`Semaphore::Wait`: frames ahead, readbacks,
Finish), present = a free presentation frame / its fence (`Presenter::GetRenderFrame`). Only waits over 20 us count
(the line's "all"), as before.

### 1b. Ordered tasks by kind

"tasks" (`Gow3Sections::Task`, `Rasterizer::RunDrawPacket`) wraps every ordered task stage B runs from the packet ring
(`Rasterizer::RunInOrder`). Every caller of `RunInOrder` now has its own section (inclusive, stage B only, like the
others):

| Section | Task | Code |
|---|---|---|
| `task_eop` | EVENT_WRITE_EOP, or its tail when GOW3_EARLY_FRAME_EOP signalled it at decode | `RunEventWriteEop`, `RunEarlyEopTail` (`liverpool.cpp`) |
| `task_eos` | EVENT_WRITE_EOS | `RunEventWriteEos` |
| `task_wdata` | WRITE_DATA | `RunWriteData` |
| `wdata_wait` | within it: the wait for host copies reading the range and the deferred fences | `WaitHostCopiesFor` + `WaitDeferredSignals` |
| `task_dma` | DMA_DATA fills / copies | `RunDmaData` |
| `task_flip` | the flip (contains `flip_signals`, `flip_present`) | `SignalFlip` |
| `task_submit` | the frame-end step on stage B (OnSubmit on stage B, GOW3_DECODE_PACING) | `OnSubmitInOrder`'s task |
| `submit_flush` | within it: its `Flush` (the submission) | |
| `task_retire` | decode pacing: frame retirement and GPU idle steps | `Liverpool::SignalDecodedFrames` / `DeferGpuIdle` tasks |
| `task_signals` | GPU-ordered fences (GOW3_GPU_FENCES): the submission for pending signals | `SubmitForSignals` |
| `task_occl` | occlusion query events (ZPASS_DONE) | `OcclusionEvent` |

There is no task for WAIT_REG_MEM, ACQUIRE_MEM / RELEASE_MEM or index / indirect setup: those are not ordered tasks
(stage A handles them, or they ride in draw packets). `end of pipe` (`ProcessDownloadImages`) stays as it was; it is
called from the EOP, EOS and frame-end tasks, so it falls inside those.

`sections_frames.py` prints, per frame kind, a `tasks T ms: eop, eos, wdata, dma, flip, submit, retire, signals, occl,
not attributed X` line; "not attributed" is the section overhead and the packet dispatch around the task (expected
~0). The older per-function-address list ("stage B tasks (us per packet, per second): exe+0x...") is still printed.

Cost: one more `Scope` per task (a static flag test when GOW3_SECTIONS is off); tasks are tens per frame.

## 2. The compact texture-set memo

### What it was

`std::array<TextureSet, 32768>` inside `Rasterizer`. One `TextureSet` is **1,592 bytes** (checked with a
compile-time `sizeof` assert on this build): a 56-byte head, 16 hashes (128 bytes), 16 entries of 88 bytes. **Table:
52,166,656 bytes (52.2 MB).** A lookup hashes the stage's prepared T# hashes into a key, looks in slot `key % 32768`
and its neighbour `^ 1` (two-way), replaces the less recently used one on a miss. A hit then compares stage, count,
generation / region stamp, scene generation and the hashes, checks each image (backing, rebind, target, up to date,
proxy) and emits the views.

What a hit reads there (slots are not line-aligned): the head and hashes (`56 + 8n` bytes) and `88n` bytes of entries,
of which a hit uses ~48 per image (id, view, backing, range, proxy, depth aspect); the other 32 (the image's and the
T#'s guest ranges) are only for the region stamps. For 4 images ~9 cache lines on one or two 4 KiB pages; a key in the
second way first reads the first way's head, 1,592 bytes away. A frame's ~2,800 lookups land on ~2,800 pages of a
12,700-page table: past the Deck's 2,048-entry L2 TLB (8 MB reach) and its 4 MB L3.

### Why not a fingerprint index in front of the table

The plan suggested a small open-addressed index of (fingerprint, slot) sized to the live sets. In this memo the slot
is computed from the key, so there is nothing to find: an index would add one dependent read before the slot, and the
slot's data still has to be read for every hit (the hashes for the exact match, the image entries for the checks and
the emit). What a hit costs is the slot's size and spread. So the change is the slot layout, split by what each path
reads, plus huge pages, keeping the slot arithmetic.

### The layout (`vk_rasterizer.h`)

| Array | Per slot | Bytes | What reads it |
|---|---|---|---|
| index: `TextureSetHead[32768]`, `alignas(64)` | key, last use, generation, stamp, scene generation, stage, count | 64 (2,097,152 total) | every lookup; the two ways of a key are adjacent, one 128-byte aligned pair |
| images: `TextureSetHotRow[32768]` | 16 x `TextureSetHot`: T# hash, view, backing, subresource range, image id, proxy, depth aspect | 768 (25,165,824 total) | a hit: the hash match, the image checks, the emit (`48n` bytes from a line-aligned row) |
| region data: `TextureSetColdRow[32768]` | 16 x `TextureSetCold`: image and T# guest address / size | 512 (16,777,216 total) | the region stamp (after any image (un)registration) and the slow path's fill |
| **total** | | **44,040,192 bytes (44.0 MB, 42 MiB = 21 huge pages)** | |

Footprint: the index alone is **2.1 MB, 4.0% of the old table**; the whole compact memo is 44.0 MB vs 52.2 MB
(-8.1 MB). With the switch never set, the old table is never made (-52.2 MB of the old memory, +44.0 MB new).

Cache lines a hit reads (n images; head + entries; old layout unaligned, so a range for the old one):

| n | old | compact |
|---|---|---|
| 1 | 3-4 | 2 |
| 4 | 8-9 | 4 |
| 8 | 13-14 | 7 |
| 16 | 25-26 | 13 |

The image objects a hit checks and marks (texture cache `Image`s) are read either way; unchanged.

**Memory:** one allocation (`Gow3TexSet::TableMemory`, `gow3_texset_table.cpp`), 2 MiB aligned, made and written in
the `Rasterizer` constructor (as the old table's `{}` was: no first-touch faults on stage B). Linux: `MADV_HUGEPAGE`
before the first write, then `MADV_COLLAPSE` (Linux 6.1+) so the 21 pages are 2 MiB pages where the system allows it
(THP mode "always" or "madvise"; SteamOS ships "madvise" [inferred]). 21 huge pages fit the Zen 2 L1 DTLB (64 entries);
the old table needed ~12,700 4 KiB entries. Windows: `VirtualAlloc`, 4 KiB pages (large pages need a privilege); the
PC's 96 MB L3 holds the old table anyway.

At start the log says:
`Texture-set memo: compact layout, 44.0 MB (index 2.1, images 25.2, region data 16.8 MB; the table as before: 52.2 MB);
transparent huge pages: yes|no|unknown|n/a (system: always|madvise|never|n/a)` ("unknown": the kernel has no
`MADV_COLLAPSE`).

### Same decisions (why the default is on)

- One lookup for both layouts: `BindTexturesFromSetIn<Table>` (`vk_rasterizer.cpp`), a template over a small adapter
  (`LegacyTextureSets` / `CompactTextureSets`: head, hash, entry, stamp). The statements are those of the old
  `BindTexturesFromSet`, in the same order; the old layout runs through the same template. The hash comparison is a
  loop instead of `std::equal` (same result).
- The slot choice is `Gow3TexSet::Choose` + `WaysOf` (`gow3_texset_table.h`) for both layouts. `texset-table-test`
  runs the old code's slot choice verbatim beside `Choose` over 2,000,000 lookups (a 3,000-key hot set, new keys, keys
  forced into the same slot pair, 2% of sets dropped as the slow path does), one- and two-way: the same found / miss
  and the same slot every time, the same keys and last uses at the end.
- The fill (`KeepTextureSet`) stores the same values; the hashes stay where the miss wrote them, as before; the stamp
  is the same sum over the same fields (`TextureSetStamp(u32)`). A dropped set gets key 0, as before.
- Zeroed index entries are made with the old defaults (`generation = ~0`), so even the empty table is the same state.

So with the default on, the memo binds the same images, views and layouts in the same draws; only where the bytes
are differs. The cost that could be higher: none on a hit or a miss (fewer lines); the stamp path reads two arrays
instead of one slot, next to two `RegionStamp` calls per image it already makes.

### The switch

**Second toggle word 2048** (`Gow3Toggle2::TextureSetLegacy`; listed in `gow3_toggles.h` and the comment above
`runtime_disabled_optimizations` in `src/runtime_memory.c`): set = the old `TextureSet` table, read live per lookup.
It is made on its first use (52.2 MB value-initialised on stage B: a one-off ~20-80 ms on the Deck [inferred], in a
window `deck_summary` leaves out). Each layout keeps its own sets while the other is in use; switching back finds
older sets, which the same generation / region stamp / image checks validate, as for any set not used for a while.
The memo itself still switches off with first word 2^45 (`TextureSetMemo`), unchanged.

### Frame statistics

`GOW3_FRAME_STATS=1`, once per 5 s window, after "Stage B sleeps" (listed in `deck_summary.py`'s extra lines):

`Texture-set memo per frame (compact layout on|toggled off): lookups N (compact N, legacy table N), hits N (P%),
misses: other key N, generation N, image check N, other N; sets kept N, not kept N; memory: compact M MB (huge pages
yes|no|unknown|n/a), legacy table M MB`

- "legacy table" lookups are those served by the old table (the switch set): the fallback count.
- Misses by cause are the same split as the 262,144-draw "texture set misses" line (other key = not in either way,
  generation = region stamps changed, image check = an image changed since, other = stage, count, scene generation or
  hash differ); kept / not kept is the slow path's decision for the slot.
- Counters are written by the recording thread alone (no locked add) and only with GOW3_FRAME_STATS.

### Expected effect [inferred]

On the Deck heavy view (`deck-sections-1.log`): "texset" 1.09 ms a frame, of which "texset emit" 0.49 (that part
touches the texture cache's images and the barrier logic, unchanged). Of the ~0.6 ms lookup and checks, the memo's
own TLB and cache misses are the part this removes: the memory study's estimate, 0.1-0.35 us a draw, i.e.
**~0.15-0.5 ms of stage B a heavy frame**, most of it from the huge pages (no page walks) and the rest from half the
lines. If the log says "huge pages: no", expect the lower end. The PC (96 MB L3): no measurable change expected.
Hit rate and miss split must be identical between the two states (same decisions); a difference there is a bug.

## The Deck test (one launch, the user's go)

Same spot and settings as the sections run (720p, 67%, Low, graveyard heavy view, stand still).

1. `deck.sh deploy` (the AppImage in the WSL `dist/`).
2. `deck.sh env "texset A/B" GOW3_FRAME_STATS=1 GOW3_SECTIONS=1 GOW3_SLOW_FRAMES=1.4` plus the keys the current runs
   use (`GOW3_ARENA_HOST_VISIBLE=0 GOW3_EARLY_FRAME_EOP=1 GOW3_DECODE_PACING=1 GOW3_DECODE_PACING_LEAD=2
   GOW3_TOGGLE_FILE=/home/deck/.local/share/medievil-pc/toggles.txt`; `deck.sh env` replaces the file).
3. `deck.sh toggle "0 0"` (the build's defaults; add 1024 to every second word below if `GOW3_STAGE_B_SPIN=1` stays
   in the env and the spin is to stay off).
4. The user starts the game and stands in the heavy view. Then `deck.sh ab "0 0" "0 2048" 30 5`: B (old table) and
   A (compact) alternate every 30 s, five times, ending on A. The first switch to B makes the old table (one hitch).
5. `deck.sh pull texset-ab`, then:
   - `deck.sh summary texset-ab --min-draws 1000`: FPS, stage B busy per state, and the "Texture-set memo" line
     (hit % and misses equal in both; lookups under "compact" in A, "legacy table" in B; the start line's huge-page
     state);
   - `py -3.14 research/tools/sections_frames.py research/docs/data/texset-ab.log 1000`: the new "Frame samples by
     toggle state" table (frame, packet, draw, textures, **texset**, texset emit per state) and the tasks split.

Pass: "texset" lower in "0 0" than in "0 2048" (expected -0.1 to -0.5 ms), hit rate and miss counts the same, FPS and
stage B busy not worse, no new visual fault (same images bound by construction). If "texset" does not move, the cost
is in the image checks and the emit, not the table: the next step would be those (study-stage-b-per-draw.md).

The task split needs no A/B: read "tasks T ms: ..." from the same log, and the per-site sleep ms from "Stage B
sleeps per frame".

## Builds and tests

- Windows (`research/tools/build-clang64.sh`): builds, no new warnings in the changed files.
- Linux (WSL Debian `build.sh`, then `packaging/linux/appimage.sh` into `dist/MediEvil-x86_64.AppImage`): builds
  (LTO, static, baseline CPU level). The first Linux build found a braced list in a range `for` without
  `<initializer_list>` (`366d634`).
- `texset-table-test` (new, `ninja -C out/gpu texset-table-test`): passes on Windows and Linux (same decisions over
  2,000,000 lookups each way: 1,921,816 hits / 78,184 misses two-way, 1,764,825 / 235,175 one-way).
  `draw-classes-test`, `readback-queue-test`: pass on both.
- Huge pages, measured in WSL2 (kernel 6.18, THP "madvise"): `MADV_COLLAPSE` succeeds, and `/proc/self/smaps` shows
  the 44,040,192-byte table as `Size 43008 kB, Rss 43008 kB, AnonHugePages 43008 kB` (all of it in 2 MiB pages). The
  Deck's state is in the start line.
- Python (`py -3.14 -m unittest discover -s tests`): the 8 known failures (2 failures, 6 errors), nothing new.
- Not run in game.
