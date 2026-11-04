# Study: the Steam Deck's memory budget (spending RAM to cut CPU, stutter and loading)

Status: research only, 2026-11-04, branch `deck-lab` (`3573a88`). Nothing built, launched or committed; no SSH.
Parent: [plan-linux-deck.md](plan-linux-deck.md). Companions, whose items are not repeated here:
[study-deck-platform-optimization.md](study-deck-platform-optimization.md) (THP, M0-M4),
[design-u5-deck-memory.md](design-u5-deck-memory.md) (U5, budget section 4),
[study-stage-b-per-draw.md](study-stage-b-per-draw.md) (C1-C11), [fix-inventory.md](fix-inventory.md) (the caches).

Marks: **[code]** read in this tree. **[log]** a number from `data/` (Deck logs unless named otherwise). **[doc]** from
another research doc. **[file]** read from the game files on this PC. **[inferred]** reasoning or estimate, not measured.
Nothing here is "verified" in the project's sense.

The yardstick (plan-linux-deck.md, decode pacing D0-D2): graveyard heavy view, 64-65 FPS at 720p / 67%, stage B
~80-93% busy, ~1,200-1,400 draws a frame. The hitches: **0.5-1.3 s frames when the view turns toward a new area**
(native run 1: "0.6-1.1 s hitches, first visit"; the stall lines of every stats run since).

## Answer

1. **The Deck has RAM to spare: about 4.5-7.5 GiB of the ~14 GiB stay free** (estimate, section 1). Guest memory
   peaks at ~2.4-2.9 GiB resident (pool commits 1.2 GiB at the menu, 2.3-2.4 GiB peak in four runs), the port's GPU
   memory at ~2-3.4 GiB, the OS and Steam ~1.5-2.5 GiB. The port's own caches are not short of memory: the texture
   collector has never evicted an image on the Deck, the rings never wait, the pipeline cache never evicts.
2. **So bigger caches buy nothing.** Texture GC thresholds, ring sizes, the pipeline cache, readback pools, guest
   direct memory (`GOW3_DMEM_MB`): none of them is a limit today (section 2.1, 2.4, 2.8). The memo tables should get
   *denser*, not bigger: the texture-set memo is a 52 MB table on a CPU with a 4 MB L3 and an 8 MB TLB reach, and its
   hit path costs 5.7x the PC's (section 2.9).
3. **Where RAM does buy time is page faults in the streaming hitches.** The hitch frames show **68,000-86,000 process
   page faults in one ~1 s frame** while the guest pool grows by 400-600 MiB. A first touch of the guest memfd ran at
   1.0 GB/s in the Gate 1 probe (~4 us per 4 KiB page, zeroing included). Populating the pool at boot and populating
   each commit's pages in one call (plus the plan's L2) should take ~50-200 ms off each hitch and some of the game
   thread's per-frame cost, for no steady-state RAM: pool pages are never given back to the kernel today anyway.
4. **The paks are stored uncompressed** (index read of four paks: 0 compressed entries). A group of ~200 new textures
   (~360 MB) is then ~360 MB of reads. From the page cache that is ~50 ms of copying; from the NVMe at the guest's
   queue depth of 1-2 it can be a large part of a first-visit hitch. The kernel already caches what was read, and there
   is room for a level's chunk (0.6-1.8 GB). First measure the disk share (`/proc/<pid>/io`, no build), then a prefetch
   of the area's recorded reads. `mlock`, `MAP_POPULATE` of the paks and io_uring do not fit (section 2.7).
5. **Where RAM costs:** the GPU side, not the total. TTM caps GPU system-memory pages at about half the RAM [inferred],
   and the 1 GiB carve-out holds only part of the ~2-3 GiB of "device-local" allocations, so the rest lives in GTT and
   may be moved back and forth at submits. Both are unmeasured and cheap to read (section 3, 5). Anything that adds
   GPU memory (texture keeping, U5's guest memory as Vulkan memory) counts against the TTM cap; the page cache does not.

## 1. Today's budget

### 1.1 How the driver presents the Deck's memory [log]

`data/deck-u2-ab-1.log`, RADV 26.1.99:

```
GPU memory types: 0: DEVICE_LOCAL heap 1 (6144 MiB); ... 2: HOST_VISIBLE|HOST_COHERENT heap 0 (3072 MiB);
3: DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT heap 1 (6144 MiB); ... 5: HOST_VISIBLE|HOST_COHERENT|HOST_CACHED heap 0 (3072 MiB) ...
```

- 6144 + 3072 = 9216 MiB = the 1 GiB carve-out + the 8 GiB GTT (plan-linux-deck.md, "The Deck as found"). RADV
  reports the sum, two thirds as the device-local heap and one third as the host heap [inferred from the numbers; RADV
  does this on APUs so that games asking for a minimum VRAM size run].
- So "device-local" on the Deck means "carve-out first, GTT when it is full" [inferred: amdgpu lets VRAM buffers fall
  back to GTT]. The carve-out and GTT have the same GPU bandwidth: the Gate 1 probe read cached host memory at 41.2 GB/s
  and device-local at 40.8 GB/s (plan-linux-deck.md, U5 gate 1).
- The texture collector counts both heaps on integrated GPUs (`vk_instance.cpp:916-924` [code]).

### 1.2 Where the memory goes

| Consumer | Size | Lives in | Mark |
|---|---|---|---|
| SteamOS Game Mode, Steam client, steamwebhelper, gamescope | ~1.5-2.5 GiB | anonymous + file pages | [inferred] |
| **Guest direct memory touched**: the pool (5,056 MiB allocated) + the 512 MiB framebuffer pool | **2.4-2.9 GiB at peak**. "Guest memory" lines: pool **1,213-1,215 MiB committed at the menu, 2,302-2,403 MiB peak** (D2, helpers run 1, U2 A/B, U5-2 A); fault map "Onion 1,241 MiB mapped, Garlic-WC 1,593 MiB mapped" at the peak | memfd (shmem, swappable) | [log] |
| Guest flexible memory, eboot image + modules, guest stacks | ~0.15 GiB (flexible 22 MiB; image 96.9 MB; 67 guest threads) | memfd / anonymous | [log] |
| Arena residency (buffer cache) | **0.38-0.48 GiB**: 32 MB chunks 12-15 after 5-12 min ("Buffer residency: 32 MB chunk N allocated ahead"); **never released**, grows with the session | device-local (type 0, or 3 with U2) | [log] [code `buffer_cache.cpp:441-506`] |
| Arena BDA page table | ~0.13 GiB | device-local | [doc, plan-linux-deck.md C4, inferred] |
| Images: textures (the guest's ~1.1 GiB of Garlic textures mirrored as Vulkan images), 720p targets, parked targets (cap 768 MiB, `texture_cache.cpp:762`), spare block 64 MiB | ~1-2 GiB | device-local | [inferred] |
| Stream buffer + constant ring | 0.22 GiB (128 + 96 MiB) | device-local, host-visible | [code `buffer_cache.cpp:419`, `vk_constant_ring.h:28-36`] |
| Staging upload ring | 128 MiB populated at start, up to 512 MiB kept (`GOW3_STAGING_KEEP_MB`) | write-combined GTT | [code `vk_staging_buffer_pool.cpp:24-56`] |
| Downloads, readbacks (64 copies, 64 MiB pending cap) | < 0.1 GiB (PC: peak pending < 0.1 MiB, `p0-stale-on.log`) | cached GTT | [code] [log, PC] |
| Port heaps: texture-set memo 52 MB, region table, shader and pipeline objects; Mesa, SDL, FFmpeg | ~0.3-0.8 GiB | anonymous | [code for the memo; the rest inferred] |
| **Total** | **~6.3-9.5 GiB** | | |
| **Left: page cache and headroom** | **~4.5-7.5 GiB** of ~14 GiB | | |

Notes:
- The port's GPU memory, ~2-3.4 GiB, is 1-2.4 GiB more than the carve-out: that part lives in GTT [inferred].
- The user's overlay showed "VRAM ~1 GB lower" with GPU-ordered fences on (plan-linux-deck.md, U5-2 A/B): fewer frames
  in flight hold fewer copies. Which counter the overlay reads (carve-out only, or carve-out + GTT) is not known.
- Guest memory pages stay resident once touched: **a pool decommit on Linux unmaps the view but never punches a
  hole** (`pool_decommit_impl`, `runtime_memory.c:1062-1092` [code]); holes are punched only on a direct release
  (`:763`) and a flexible free (`:500-503`). (study-deck-platform-optimization.md 1.2 says "decommit =
  `fallocate(PUNCH_HOLE)`": that holds for releases, not for UE4's pool decommits.) So the memfd sits at the pool's
  high-water mark, ~2.4 GiB, for the rest of the session.
- Not measured anywhere: the process RSS, `MemAvailable`, carve-out / GTT use, swap. Section 5 lists the reads.

### 1.3 The port's own thresholds against this budget [code] [log]

- **Texture collector:** on integrated GPUs the trigger is 70% of the driver's current budget of both heaps, pressure
  85%, critical 95% (`texture_cache.cpp:1997-2003`). With ~2-3.4 GiB used against a ~9 GiB budget it never triggers.
  With `GOW3_FRAME_STATS` an eviction prints a "Texture cache: collecting" line: **none in any of the ~20 Deck stats
  logs.**
- **Rings:** the stream buffer takes 0.3-3.3 MB a frame of its 128 MiB, the constant ring 0.4-2.5 MB (+ up to 8.9 MB
  reused) of 96 MiB ("Mapped writes per frame", D2). Stall lines: "staging 0.0, waiting for host copies 0.0".
- **Guest pool:** peak 2.4 of 5.06 GiB committed. The game sizes its streaming by its own settings
  (`r.Streaming.PoolSize=2000` in the PS4 profiles, fix-inventory.md "Caveats"), not by free memory.

## 2. Where spending RAM could save CPU or stutter

### 2.1 Texture cache GC and eviction thresholds: no gain [code] [log]

- `RunGarbageCollector` runs once per `sceGnmSubmitDone` (~321 per 5 s at 64 FPS: once a frame), on stage B since
  decode pacing (impl-decode-pacing.md, "OnSubmit on stage B"; `vk_rasterizer.cpp:1607`).
- Each call reads the driver's usage and budget (two `vkGetPhysicalDeviceMemoryProperties2` with the budget chain,
  `vk_instance.cpp:1010-1036`; on RADV these read the kernel's heap usage [inferred: info ioctls]), then returns at
  the trigger check. The sampler collector returns at its count check.
- Measured: the stall lines' "GC" is 0.0-0.1 ms per frame (`Gow3Stats::t_gc` covers the queries).
- Verdict: raising the thresholds changes nothing (nothing is evicted). The only saving is the query: read the budget
  once per 100 ms instead of twice per call. Up to ~0.05 ms of stage B a frame [inferred]; nearly free; low priority.

### 2.2 Keeping uploaded images instead of re-uploading them [log] [doc]

- The Deck's hitch frames (D2, helpers run 1) are the PC's "panning stalls" (plan-frame-pacing.md, T-C2, S1, S2): a
  fixed group of assets loads when the view turns toward an area and unloads when it turns away (UE4 level streaming,
  not the texture pool; a 4x larger pool did not stop it on the PC).
  - Deck: 879-1,163 ms frames with **196-316 images registered, 156-266 MB image uploads, 18-31 MB buffer uploads**,
    "GPU thread on CPU" 99-287 ms, readback waits 31-58 ms, "image create" 5-7 ms, "protect" 19-55 ms.
  - PC (`pan-1.etl`): the game thread fully busy for 0.7 s, of which 48% game code, 12% memcpy / memset, **14% page
    faults / memory manager**, 14% thread synchronisation.
- The groups come back **at new guest addresses** (S1), so the texture cache cannot match them by address the way
  parked targets (N-24) are matched.
- A content-keyed keep: when a guest unmap frees a sampled texture, keep its image (budget e.g. 512 MiB-1 GiB, oldest
  out first); when a new image of the same size, format and tiling registers, hash its guest bytes and take the kept
  image on a match.
  - Saves: image creation and views, the staging copy's GPU side, the detile dispatch, and GPU time (the GPU is ~90%
    busy). The hash costs about what the staging memcpy costs, so the CPU saving is the creation and recording part.
  - Estimate: 30-150 ms of the GPU thread's 99-287 ms per hitch [inferred]. The game's own loading work stays.
  - RAM: the budget, in GTT, counted by TTM (section 3.1).
  - Effort M-L, risk M (stale content on a hash collision, aliasing, interaction with the region stamps of the
    texture-set memo).
- First, a free measurement: `GOW3_TEXCACHE_STATS=1` prints "freed by guest unmaps N (MiB); created again within ~30 s
  N (MiB)" every 5 s (`texture_cache.cpp:2181-2185`). On the PC at 4K: 2.5-128 MiB re-created per 5 s while panning
  (`t-c2-recreate.log`). If the Deck shows little re-creation, drop this item.

### 2.3 Detiling once and keeping the result: already the case

- The texture cache keeps a detiled image while its guest memory is unchanged, and nothing evicts (2.1). Detiling runs
  on the GPU (`tile_manager`), once per image life.
- A detiled copy kept across sessions (on disk) would be slower to read than to detile. Only 2.2's keep across unmaps
  is left.

### 2.4 A larger pipeline / shader cache: no gain [log] [code]

- MediEvil starts pipelines on demand ("Pipeline startup: on demand, no bulk warm-up"; the background warm-up drew
  levels black, `vk_presenter.cpp:171-178`). Built pipelines stay in memory for the session; nothing evicts them.
- The pipeline binary cache hits ("256 hits, 0 compiled"); the hitch frames carry **4-15 ms of compiles** of their
  0.5-1.2 s.
- Steam's Fossilize layer is loaded for the shortcut and records pipelines (`Fossilize INFO: Overriding serialization
  path: .../shadercache/3709424537/...`, every Deck log). This corrects the platform study's R6 inference that
  Fossilize does not cover the AppImage shortcut, at least for recording; whether Steam replays it is unknown.
- Keep R6 of the platform study (`MESA_SHADER_CACHE_MAX_SIZE`): it is disk, not RAM.

### 2.5 Guest memory: first touches and recommits (the RAM item that pays) [code] [log]

**What happens today:**
- The guest memory is one memfd (`runtime_memory.c:443-451`). A commit maps a view of it (`mmap MAP_SHARED|MAP_FIXED`,
  `:453-457`). A pool decommit unmaps the view and maps a reserved range (`:1080-1085`). The pages behind it stay in the
  memfd, but **the next commit maps a new view with empty page tables**: every 4 KiB page the game touches faults once.
  `pool_take_locked` hands out the first free pages, so a recommit rarely gets its old pages (T-H1).
- A page the memfd never held is allocated and zeroed in that fault. Gate 1 probe: CPU first-touch writes ran at
  **1.0 GB/s into a memfd vs 7.7 GB/s into anonymous memory** (~4 us vs ~0.5 us per page, faults included).
- UE4 commits and decommits 64 KiB pool pages ~6,000 times a second (T-H1, PC).
- File reads into guest memory first touch every destination page from user mode (`touch_for_write`,
  `runtime_file.c:293-300`), so a load's reads fault the same way.

**Measured on the Deck** (stall lines, "GPU thread page faults N (process M)"):

| Stall frame | process page faults | in that 5 s window |
|---|---|---|
| 879-1,163 ms (area loads) | **68,487-85,682** | pool +391 to +587 MiB committed, new high-water |
| 482-660 ms | 17,188-27,357 | |
| 58.7 / 99.1 ms (gameplay) | 1,537 / 3,690 (~26-37k/s) | |

- At ~1 us per fault on an already-held page and ~4 us on a new one, the 70-86k faults are **~70-300 ms of loader-
  thread CPU in a ~1 s hitch** [inferred]. That matches the PC's 14% of the game thread in page faults during pans.
- In gameplay, 26-37k faults/s would be ~3% of a core, ~0.4-0.6 ms of game-thread time a frame [inferred]. That rate
  comes from stall frames; the rate in ordinary frames is not logged.

**Options (one switch each):**
- **(a) Populate the pool at boot.** `fallocate(pool_fd, 0, offset, N)` (mode 0 allocates and zeroes shmem pages) for
  the first N = 2.5-3 GiB of the pool's physical range, on a background thread during the logos. Pages are handed out
  lowest first (`free_hint`), so the game's commits land in populated pages. First touches become ~1 us minor faults.
  - RAM: 2.5-3 GiB from boot instead of growing to ~2.4 GiB within the first areas; steady state +0-0.6 GiB.
  - Cost: ~0.3-0.6 s of one core at boot, and memory bandwidth during the logos [inferred].
- **(b) Populate each commit's pages in one call.** After `host_map` of a commit, `madvise(MADV_POPULATE_WRITE)` (Linux
  5.14+; `MADV_POPULATE_READ` for read-only commits, since a write populate fails on them). One syscall replaces up to
  16 faults per 64 KiB commit.
  - RAM: none beyond (a); it populates pages the game may not touch, which are in the memfd already after (a).
  - Risk: commits whose pages are protected for write tracking right after (the populate must come before the
    `mprotect`); measure the syscall cost per commit (the counter L2 needs anyway).
- **(c) L2, keep views across a decommit on Linux** (plan-linux-deck.md L2; design-u5 step "L2 on Linux"). A recommit at
  the same address keeps its page tables: no fault, no `munmap` / `mmap`, no TLB shootdown.
  - RAM: kept pages stay tied to their addresses and are handed out last, so the pool's high-water may rise (Windows:
    regions levelled off at ~13,400 views, T-M1). Within the 5 GiB pool on a 14 GiB machine, acceptable.
  - Groups that come back at new addresses still fault; (a) + (b) cover those.
- Expected together: ~50-200 ms off each area hitch, ~0.2-0.5 ms of game-thread CPU a frame [inferred]. The game's
  own frame is where the long frames come from (run 5: game +5.3 ms excess).
- Huge pages do not help here: 64 KiB commits at unrelated addresses cannot be PMD-mapped (platform study M4).
- Test: one launch per switch. Read the stall lines' "process" faults, the hitch frames' length at the same turn in the
  graveyard, and the process's minor faults per 5 s (section 5, no build).

### 2.6 Claiming GPU memory at boot instead of during play [code] [log]

The user's direction (plan-linux-deck.md, "The user's direction on performance", item 1). Today:
- **Arena chunks:** 32 MB each, the next one allocated by a worker once the current one is half used
  (`buffer_cache.cpp:441-506`); stage B waits on `ready_cv` if it gets there first (`:486-488`). Chunks 11-15
  appear minutes into play. The kernel allocates and zeroes each.
- **Image memory:** on AMD every image goes through `vmaCreateImage` (shared image memory is NVIDIA-only,
  `image.cpp:346-363`: "Texture cache: shared image memory off (... RADV VANGOGH)"). New VMA blocks cost **27-37 ms**
  ("Slow image create", two per run, in the first area load); the spare-block thread then makes 64 MiB spares in 19-36 ms
  on its own thread (`image.cpp:234-328`).
- Options:
  - **`GOW3_RESIDENCY_CHUNK_MB=256`** (existing switch, no build): the first 256 MiB at start, the second at half use;
    two chunks cover a 12-minute session. RAM: the same ~0.5 GiB, claimed at boot.
  - A VMA pool for images with `minBlockCount` (e.g. 4 x 256 MiB) created at start: the first area load stops making
    blocks. Effort S. RAM: ~1 GiB from boot, about what the images reach anyway [inferred].
- Expected: the 2 x ~29 ms creates leave the first load; mid-play chunk allocations leave the workers; stage B's rare
  chunk wait goes. Small: "ms in: resident" is 0.0-0.5 ms per stall frame.
- Test: the "Buffer residency", "Slow image create" and "spare image memory block" lines stop after boot.

### 2.7 The paks in RAM [file] [code]

**Facts:**
- 01.02 is 43.8 GB; `pakchunk1` 14.4 GB, `pakchunk0` 11.0 GB, the other chunks 0.6-1.8 GB each [file].
- **Stored uncompressed**: the indexes of `pakchunk0`, `3`, `5` and `20` list 39,102 / 1,131 / 1,555 / 1,218 entries,
  none compressed [file, read with a copy of `ue4_override.read_index`]. A 360 MB texture group is ~360 MB of reads.
- The guest's Fios2 (a real guest module, `game_profiles.py:13`) reads through `sceKernelRead` / `Pread` from its I/O
  threads (`SceFiosIO`, `SceFiosIO /`), which the runtime turns into plain `read` / `pread` on the host file
  (`runtime_file.c:301-318`): synchronous, through the page cache, queue depth 1-2 [code; inferred for the depth].
- The runtime counts opens, reads and bytes only at exit (`runtime_file.c:520-523`); no log holds them.

**What the RAM can do:**
- The kernel already keeps what was read while memory is free: a second visit in the same session reads from the
  page cache. Section 1 leaves 4.5-7.5 GiB, enough for an area's chunks. So RAM helps **first visits** (and the first
  play after a reboot), not repeats.
- Cold, 360 MB at QD 1-2 on the Deck's NVMe is ~0.25-0.5 s [inferred: 0.7-1.5 GB/s]; warm it is ~50 ms of copying.
  The PC's repeat pans still took 0.5-0.6 s warm, so the CPU part is large too.
- **Measure first** (no build): `/proc/<pid>/io` every 5 s. `rchar` is what the game read, `read_bytes` what came from
  the disk, `syscr` the read count (average read size = rchar / syscr). Compare first and second visits of the same
  turn: if the second hitch is about as long, I/O is not the limit and this item closes.
- If I/O is a large share:
  - **(a) Read hints** (effort S): `posix_fadvise(POSIX_FADV_SEQUENTIAL)` on pak opens doubles the kernel's readahead
    window. Costs nothing.
  - **(b) Prefetch an area's recorded reads** (effort M): record (file, offset, size) per area on a first play (keyed by
    the `OpenLevel` hook and the stream order). Later, issue `posix_fadvise(WILLNEED)` for the next area's ranges from a
    low-priority thread. RAM: page cache, 1-2 GiB, evictable.
  - Cold-start variant: `WILLNEED` on the current area's chunk at boot. The chunk ↔ area map comes from a
    `GOW3_FILE_TRACE` run plus offsets.
- **Not these:**
  - `mlock`: pins memory the GPU and the guest need, and `RLIMIT_MEMLOCK` is small by default.
  - `MAP_POPULATE` of a pak mapping: the guest reads with `pread` into its own memory, so a populated mapping is a
    second copy.
  - io_uring: the guest's API is synchronous and the guest's Fios decides the queue depth. A prefetcher needs no more
    than `fadvise`.
  - Whole-pak preloading: 25 GB in the two large chunks.

### 2.8 Larger rings: no gain; smaller ones are free RAM [log] [code]

- No ring waits (1.3). The staging ring keeps up to 512 MiB; the stream buffer is 128 MiB; the constant ring 96 MiB
  (sized for 4K ring reuse: "32 ran full", fix-inventory.md).
- Already proposed for U5-10 / U1: staging keep 128-256 MiB (`GOW3_STAGING_KEEP_MB`), parked-target cap down at 720p.
  These free 0.3-0.6 GiB of GTT. No CPU change. Keep the constant ring at 96 MiB until a Deck run shows ring-reuse
  numbers at 32 (`GOW3_CONSTANT_RING_MB`).

### 2.9 Memoization tables: denser, not larger [code] [doc]

- Texture-set memo: `std::array<TextureSet, 32768>` (`vk_rasterizer.h:330-349`). One `TextureSet` is ~1.55 KiB (16
  entries of ~88 bytes, 16 hashes, a 56-byte header) [code, sizes by hand], so the table is **~52 MB**. Lookups are
  hashed, so a frame's ~2,800 stage sets land on ~2,800 different 4 KiB pages.
- The Deck's L2 DTLB covers 8 MB with 4 KiB pages; its L3 is 4 MB (platform study 1.1). So a hit can be a page walk to
  DRAM plus cache misses.
- Measured: the hit path ("texset", lookup + image checks + emit loop) costs **0.851 us a draw on the Deck vs 0.150 on
  the PC (5.7x)** (study-stage-b-per-draw.md, the parts table), above the 4-5x of most parts. Hit rate 99.3%; the misses
  are "other key" conflicts (1,605 of 1,606 per 5 s, helpers run 1). The render-state memo (93.7%) is a one-entry
  signature, which more RAM cannot help.
- Options:
  - **(a) Shrink the table** to 4,096 or 8,192 sets (6.5-13 MB), behind a restart switch. A few more conflict misses
    against fewer TLB and cache misses.
  - **(b) Keep the size, back it with huge pages**: a 2 MiB-aligned allocation + `MADV_HUGEPAGE` (26 huge pages), or
    the platform study's M3 (`glibc.malloc.hugetlb=1`) if the table is moved out of the `Rasterizer` object.
- Expected: 0.1-0.4 us a draw of the 0.85, so 0.15-0.5 ms of stage B in the heavy view [inferred]. Stage B is the
  wall, so this is the most valuable microsecond on the list, and it frees RAM instead of spending it.
- Test: the platform study's M0 (`perf stat` dTLB misses on `gow3:DrawRec`, no build). Then an A/B by restart with
  `GOW3_FRAME_STATS=1 GOW3_SECTIONS=1 GOW3_SLOW_FRAMES=1.4`, reading "texset" us per draw and the hit rate.
- Other fixed tables are small: image description cache 4,096, ring reuse 4,096, find-image cache 1,024 entries.

## 3. Where RAM use costs

### 3.1 GPU memory and the TTM cap [inferred]

- The kernel's TTM layer limits the system pages GPU buffers may hold to about **half the RAM** by default (~7 GiB here),
  below amdgpu's 8 GiB GTT size. Past the cap TTM swaps buffers out to shmem, with stalls; Bloodborne 0.7 lost the
  device after ~80 min on a GTT double count (design-u5-deck-memory.md, section 4). [inferred from kernel source as
  remembered; read `/sys/module/ttm/parameters/pages_limit` to confirm.]
- Today: ~2-3.4 GiB of GPU memory, well inside the cap.
- **Counted against it:** every GPU-side item: image keeping (2.2), boot claims (2.6), U5. U5 moves the guest's
  2.4-2.9 GiB touched (5.4 GiB if eager) into Vulkan memory. With lazy chunks that is ~5-6 GiB of GPU memory in total:
  inside the cap, not by much. U5 should shrink `GOW3_DMEM_MB` at 720p / 800p first (design-u5 open question 4).
- **Not counted against it:** the page cache (2.7) and the guest memfd today.

### 3.2 Carve-out or GTT placement [inferred]

- Bandwidth is the same (1.1), so placement only costs when the kernel moves buffers. With more "device-local" memory
  than the 1 GiB carve-out, amdgpu places the rest in GTT. It may try to move it back at command submission within a
  per-submit byte budget, and evict others to make room. That is CPU in the submit ioctl and GPU copy time. Unmeasured.
  - Stage B's out-of-proportion "tasks" (7.6x the PC) and "flip_present" (6.5x) include the submits (run 3). That
    does not show moves; it is only where moves would show.
- Measure (no root, no build): `mem_info_vram_used` / `mem_info_gtt_used` in sysfs every 5 s. If the carve-out sits
  full, a small code reader of amdgpu's "bytes moved" and "evictions" counters (`AMDGPU_INFO_NUM_BYTES_MOVED`,
  `AMDGPU_INFO_NUM_EVICTIONS` on the render node) tells whether moves happen.
- If they do (effort S, RAM 0):
  - put the arena, stream buffer, constant ring and BDA table in host-heap types (heap 0: type 2 write-combined or
    type 5 cached), free for the GPU on this machine;
  - leave the carve-out to render targets and images.
- **BIOS UMA frame buffer 4 GB:** would hold everything in the carve-out, but takes 3 GiB from the OS for good. Only if
  moves are measured and the type change is not enough.

### 3.3 First touches and zeroing [log]

- Every new page is zeroed by the kernel. For guest memory that happens in the guest's fault (2.5). For GPU memory it
  happens inside the allocation call: ~29 ms for a new image memory block on the Deck.
- So "spend RAM up front" is right only when done **at boot or behind a loading screen**, never on a hot thread
  mid-play. 2.5 and 2.6 are written that way.

### 3.4 Page-cache pressure, THP compaction, swap [inferred]

- Filling free memory with page cache (2.7) leaves no free 2 MiB blocks. Huge pages then need compaction:
  - with THP `defrag` = `always`, faults stall on it (the platform study's M1 keeps `defer+madvise`);
  - `MADV_COLLAPSE` and `MADV_HUGEPAGE` regions compact synchronously.
  Keep 1-2 GiB free beyond the prefetch, and prefetch only an area or two ahead.
- GPU allocations and the guest's first touches also wait on reclaim when free memory is low. Clean page cache is cheap
  to reclaim, but direct reclaim is still on the allocating thread.
- **Swap:** the guest memfd is shmem and swappable. If the total ever exceeds RAM, guest pages go to swap and the game
  stalls for seconds. Read `swapon --show`, `/proc/meminfo` and memory PSI in a long run (section 5).
- The Deck's CPU and GPU share one memory bus and one power budget. Bulk populating, zeroing and prefetch use both, so
  run them in loading screens.

## 4. Ranked list for the Deck

Step 0, no build and no RAM: section 5's reads ride along on the next planned launch. They decide 2, 4 and 6.

| Rank | Item | RAM spent | CPU / stutter saved (all estimates) | Effort | Risk | Test |
|---|---|---|---|---|---|---|
| 1 | **Guest pool populated at boot (2.5a) + populate per commit (2.5b)**; then L2 (2.5c) as planned | 2.5-3 GiB from boot; steady state +0-0.6 GiB | ~50-200 ms off each area hitch (70-86k faults now); ~0.2-0.5 ms game-thread CPU a frame | S + S (L2: M) | L | stall lines' "process" faults, hitch length at the same turn, minor faults per 5 s; one launch per switch |
| 2 | **Texture-set memo denser (2.9)**: 4-8k sets, or huge pages | **-40 to -45 MB** (or 0) | 0.15-0.5 ms of stage B in the heavy view | S | L | M0 dTLB counters; restart A/B, "texset" us per draw + hit rate |
| 3 | **Carve-out placement (3.2)**: rings / arena / BDA table to host-heap types, only if the carve-out is full and moves are seen | 0 | 0-1 ms of stage B submit time, if moves exist | S | L-M | sysfs VRAM / GTT used; bytes-moved counter; A/B by switch |
| 4 | **GPU memory claimed at boot (2.6)**: `GOW3_RESIDENCY_CHUNK_MB=256`; image VMA pool with a minimum block count | ~0.5 + ~1 GiB, earlier, not more | 2 x ~29 ms block creates out of the first load; chunk allocations and rare stage B waits out of play | none / S | L | "Buffer residency", "Slow image create", "spare image memory block" lines after boot |
| 5 | **Paks: read hints, then an area prefetch (2.7)**, only if `/proc/<pid>/io` shows disk reads in hitches | 1-2 GiB page cache (evictable) | first-visit hitches: up to ~0.25-0.5 s of a cold 360 MB group | S / M | L | `/proc/<pid>/io` per 5 s; first vs second visit hitch length; cold (after reboot) vs warm launch |
| 6 | **Texture keep across guest unmaps, keyed by content (2.2)**, only if `GOW3_TEXCACHE_STATS` shows large re-creation | 0.5-1 GiB GTT (TTM-counted) | 30-150 ms of the GPU thread per area hitch, plus GPU detile time | M-L | M | `GOW3_TEXCACHE_STATS` "created again within ~30 s"; the stall lines' images registered and upload MB |
| 7 | GC budget read once per 100 ms (2.1) | 0 | ≤ 0.05 ms of stage B a frame | S | L | stall lines' "GC" (already ~0) |
| — | **Shrink, no CPU change:** staging keep 512 → 128-256 MiB, parked-target cap 768 → 256 MiB at 720p (2.8) | **-0.3 to -0.6 GiB** | none | none (env) / S | L | "Guest memory", sysfs GTT used |
| — | **No gain, do not do:** larger texture GC thresholds or budget, larger rings, larger pipeline cache, larger `GOW3_DMEM_MB`, larger memo tables, `mlock` / `MAP_POPULATE` of paks, io_uring, BIOS 4 GB carve-out (unless 3.2 measures moves) | | | | | |

Order of launches after step 0: 4 (env only), then 1 (a then b), then 2; 3, 5 and 6 only as the measurements decide.
One change per launch.

## 5. Reads to collect (no build, no root; light enough to run during a run, but tell the user it runs)

Once before launch, and every 5 s during a run. A few `cat`s; no `perf`, no tracing.

```sh
# once
cat /proc/meminfo | grep -E 'MemTotal|MemAvailable|Cached|Shmem:|AnonHugePages|SwapTotal|SwapFree'
swapon --show; cat /proc/sys/vm/swappiness
cat /sys/module/ttm/parameters/pages_limit 2>/dev/null          # TTM cap in pages (3.1)
d=$(ls -d /sys/class/drm/card*/device | head -1); cat $d/mem_info_vram_total $d/mem_info_gtt_total
# every 5 s during the run
pid=$(pgrep -o medievil-runti)
grep -E 'VmRSS|RssAnon|RssFile|RssShmem' /proc/$pid/status
awk '{print "minflt",$10,"majflt",$12}' /proc/$pid/stat          # page faults (2.5)
cat /proc/$pid/io                                                # rchar / read_bytes / syscr (2.7)
cat $d/mem_info_vram_used $d/mem_info_gtt_used                   # carve-out full? (3.2)
cat /proc/pressure/memory /proc/pressure/io 2>/dev/null          # reclaim and I/O stalls (3.4)
```

- With `GOW3_FRAME_STATS=1` the log already gives "Guest memory", "Buffer residency", "Slow image create", the stall
  lines' faults, and "Mapped writes per frame". `GOW3_TEXCACHE_STATS=1` adds the re-creation counts for item 6.
- Useful port-side additions, if a build happens anyway: the process's minor faults per 5 s in the frame stats
  (`getrusage` is already read for the stall lines); the runtime's file reads per 5 s with time spent in
  `read` / `pread`; sysfs VRAM / GTT used in the "Guest memory" line.

## 6. Notes for other docs

- study-deck-platform-optimization.md 1.2: the guest memfd's "decommit = `fallocate(PUNCH_HOLE)`" applies to direct
  releases and flexible frees; UE4's pool decommits only unmap the view (`runtime_memory.c:1062-1092`), so pool pages
  stay resident and each recommit faults on a new view (2.5).
- study-deck-platform-optimization.md R6: Steam's Fossilize layer is loaded for the AppImage shortcut and records
  pipelines (2.4).
- design-u5-deck-memory.md section 4 "Budget": add the TTM cap (3.1) to the budget check, beside `VK_EXT_memory_budget`.
- plan-frame-pacing.md S1 / S2 and this study's 2.2 / 2.5: the Deck's area hitches are the PC's panning stalls; the
  Deck's stall lines give their faults, which the PC traces showed only as a share of the game thread.

## Sources

Repo: `src/runtime_memory.c:443-466`, `:500-503`, `:740-767`, `:1004-1092`; `src/runtime_file.c:284-318`, `:520-523`;
`gpu/shadps4/video_core/texture_cache/texture_cache.cpp:34-55`, `:106-112`, `:141-169`, `:762-763`, `:1974-2088`,
`:2134-2190`; `texture_cache/image.cpp:234-428`; `renderer_vulkan/vk_instance.cpp:905-924`, `:1010-1036`;
`renderer_vulkan/vk_rasterizer.h:330-349`; `renderer_vulkan/vk_rasterizer.cpp:1596-1609`;
`renderer_vulkan/vk_staging_buffer_pool.cpp:24-56`; `renderer_vulkan/vk_constant_ring.h:28-36`;
`renderer_vulkan/vk_presenter.cpp:167-199`; `buffer_cache/buffer_cache.cpp:419`, `:441-506`; `scripts/ue4_override.py:61-100`.

Logs: `data/deck-decode-D2.log`, `deck-helpers-run1.log`, `deck-u2-ab-1.log`, `deck-u5-2-A.log`, `deck-waitsites-1.log`
(memory types, "Guest memory", stall lines, residency chunks, memo lines, slow image creates); PC `t-c2-recreate.log`,
`p0-stale-on.log`.

Docs: plan-linux-deck.md (the Deck as found, Gate 1 probe, runs 3-9, U5-2, decode pacing); plan-frame-pacing.md
(T-H1, T-M1, T-C2, S1, S2, panning stall anatomy); fix-inventory.md; impl-decode-pacing.md ("OnSubmit on stage B");
study-stage-b-per-draw.md (parts table); study-deck-platform-optimization.md (TLB, THP); design-u5-deck-memory.md.

Game files: `E:\MediEvil\CUSA11227-0102\overbite\content\paks\` (sizes; index of chunks 0, 3, 5, 20).

Kernel behaviour stated from memory, to confirm by the section 5 reads: TTM's default page cap (`ttm.pages_limit`,
about half of RAM); amdgpu's GTT fallback for VRAM buffers and its per-submit move budget; `MADV_POPULATE_READ` /
`MADV_POPULATE_WRITE` (Linux 5.14, madvise(2)); `posix_fadvise(2)` readahead hints; `/proc/<pid>/io` fields
(proc(5)); PSI (`/proc/pressure/*`).
