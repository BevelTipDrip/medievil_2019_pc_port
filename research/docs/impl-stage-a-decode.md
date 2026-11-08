# Implementation: stage A's per-packet cost on the Steam Deck (A1, A2, A3)

Status: built 2026-11-05 on a branch from `deck-lab` `912c52f` (Windows build and the Linux AppImage
`dist/MediEvil-x86_64-stagea.AppImage`), unit tests pass. **Not run in game: nothing here is verified.**
Commits: `be214c8` (A1), `6fefc55` (A2), `3283c10` (A3), `b17f99b` (PublishGraphics, no switch), `bfbd495` +
`5e4e4b6` (runtime tests link again; `runtime-test --backing-copies`), and this page.
Parent: [plan-linux-deck.md](plan-linux-deck.md) ("Per-draw cuts R1 / R2 / T1 / T2 on the Deck": the limit moved
to stage A and the power-capped GPU). Inputs: `research-data/perf/deck-v3-perf-report.txt` (10 s user-space
`perf record`, v3 build, before the hybrid drain and R1), `data/deck-perdraw-1.log` (heavy view, `c71e87c`, hybrid
drain and R1/R2/T1/T2 on), the code on `deck-lab`.

Marks: **[code]** read in the tree. **[log]** a number from a Deck log. **[inferred]** reasoning or an estimate.

| # | Cut | Where | Switch (start) | Live bit (second word, set = off) | Default | Expected (heavy view) |
|---|---|---|---|---|---|---|
| A1 | GPU-side guest copies through the backing view without the region table lock (Linux) | `src/runtime_memory.c` (`runtime_memory_copy_backing`), `gpu/shim/gow3gpu.cpp` (`CopySparseSerial`) | `GOW3_BACKING_COPY=1` | **1048576** | off | stage A -0.3 to -0.5 ms a frame, the recorder -0.2 to -0.4 ms (CPU) |
| A2 | Pending-write check answered from a page filter before the scan | `vk_rasterizer.cpp` (`PendingWriteOverlaps`, `NotePendingGpuWrite`), `gpu/shim/gow3_stage_a.h` | `GOW3_PENDING_FILTER=1` | **2097152** | off | stage A -0.1 to -0.25 ms |
| A3 | Register-packet checksum two words per step | `liverpool.cpp` (`HashRegisterPacket`), `gow3_stage_a.h` | `GOW3_REG_HASH_PAIRED=1` | none (restart-only since `adfeb67`; 4194304 is no longer read) | off | ~-0.1 ms on stage A, about as much again on the scanner and the workers together |
| - | `PublishGraphics` returns when nothing is pending | `vk_pipeline_cache.cpp` | none (same behaviour) | - | on | unknown, below the profile's cut-off (< ~0.1 ms) |

All three are opt-in and live: start with the three variables, then `deck.sh ab` flips the bits (section 5). Every
bit only switches its cut off, as the earlier ones do. **Windows / PC behaviour is unchanged by default**: A1 is
Linux code only; A2 and A3 run only with their variables set. All cuts remove work; none moves work to a helper.
Together [inferred]: stage A about -0.5 to -0.9 ms of its ~9.6 ms of CPU a frame (5-9%), the process about -0.7 to
-1.3 ms of CPU a frame (~0.05-0.1 of a core at ~79 FPS). FPS: +0 to +2% (stage B waits ~1 ms a frame on stage A's
packets; the GPU gates too).

New `GOW3_FRAME_STATS=1` lines (kept by `deck_summary.py`):
- `Stage A cuts per frame: ring buffers N (reused N, copied N); pending-write checks N (bounds N, filter (A2
  on|off|toggled off) N, scans N of N entries each); register words hashed N (A3 on|off|toggled off)`
- `Backing copies per frame (A1 on|off|toggled off): without the lock N (from the index N, searched N), the locked
  way N` (all threads; "the locked way" counts only the copies A1 tried and gave back, so 0 when off)

## 1. What stage A does per draw now

`shadPS4:GpuComm` (stage A) per PM4 packet and per draw, on `deck-lab` [code]:

| Step | Per | Work | Profile (v3, % of all samples) |
|---|---|---|---|
| `ProcessGraphics` loop | packet | `ProcessCommands()` (an atomic count), the opcode switch, the draw-pipe drain check for non-pipelined opcodes | `ProcessGraphics` 0.65 |
| `ApplyGraphicsRegisterPacket` | register packet | copy into `regs`, dirty blocks, **the running checksum: one multiply chain step per word** | 0.80 (and 0.65 on `gow3:DrawScan`, ~0.16 on each `gow3:DrawPrep`) |
| `Draw` -> `GetGraphicsPipeline` | draw | `SeedWorkerCaches`, `PublishShaders` (vector), **`PublishGraphics` (an empty robin_map walked from bucket 0)**, `PumpWarmUp` (a flag) | not listed (< 0.15) |
| `TryPreparedPipeline` | draw, per stage | `GetParams` (`SearchBinaryInfo`: the shader's footer, token at word 0), `RefreshFlatBuf` (the SRT walker over guest memory), a memcmp with the prepared copy, then `graphics_pipelines.find` (XXH3 of a ~350-byte key) | 0.38 + `SearchBinaryInfo` 0.21 |
| `PostDraw` | draw | dirty register blocks into the packet, per stage the user data and flat buffer, the packet header, `Commit` (a seq_cst store; a futex wake only when stage B sleeps) | 0.77 |
| `CollectRingBindings` | draw, per stage, per buffer | the V# (prepared or fetched), `ClampRangeSize` -> `runtime_memory_clamp`; written buffers -> `NotePendingGpuWrite` (scans the pending list); read-only ones -> `IsRegionGpuModified` (inlined tracker bits), **`PendingWriteOverlaps`**, the ring-reuse table, else `Allocate` + **`CopySparseMemory`** + `Flush` | 1.23 self; `runtime_memory_clamp` 0.81, `cached_region` 0.49, `runtime_memory_region` 0.42, `pthread_rwlock_unlock` 0.42, `read_lock` 0.17, `PendingWriteOverlaps` 0.43, libc 0x170bc5 0.80 + 0x170df3 0.21 |
| `DrawPipe::Drain` | drain | today the hybrid wait (futex), was 4096 pauses + `sched_yield` | 0.28 (v3: before the hybrid) |

What is already gone or cheaper on `deck-lab` than in the profile's build:
- **R1** (region index, `GOW3_REGION_INDEX=1`, in the Deck preset): the clamp and the region lookups answer from the
  per-thread index: 14,271 index hits a frame vs 310 lock-free searches and 0 locked [log, deck-perdraw-1]. So most
  of `runtime_memory_clamp` / `cached_region` / `runtime_memory_region` is now an index probe and a bucket stamp sum.
- The hybrid drain: `DrawPipe::Drain`'s spin and the yields' kernel time.

Still per copy on Linux, R1 or not [code]: `CopySparseSerial` asks `runtime_memory_region` for the region, then
`runtime_memory_read_backing`, which takes the region table's **read lock** (`pthread_rwlock_tryrdlock` + `unlock`:
two atomic read-modify-writes on the lock's line, which every GPU-side label write, page protection and locked lookup
also writes) and **searches ~13,000 regions again** (`vma_index`) under it. That is the 0.59% of rwlock samples on
stage A, and on the recorder (`pthread_rwlock_unlock` 0.23, `tryrdlock` 0.16, `runtime_memory_region` 0.23: its
deferred stream copies go through the same function; `runtime_memory_region` has no other caller). -> **A1.**

**The libc hotspot.** `0x170bc5` and `0x170df3` are 0x22e apart in one function that has no exported name (exported
ones such as `pthread_rwlock_unlock` are named in the same report), on stage A and on the recorder. glibc's memcpy is
an IFUNC whose implementations (`__memmove_avx_unaligned_erms` and siblings, ~0x700 bytes with the `rep movsb` and
4-vector loops) are local symbols, so it is **almost certainly memcpy/memmove** [inferred]. On stage A the large
copies are the constant ring's: 2.18 MB a frame copied (8.01 MB more reused) [log], from guest memory into the
write-combined ring; the packet copies are a few hundred bytes a draw. That copy is the work itself (the bytes the
GPU reads); ring reuse already avoids 79% of it by bytes. Not cut. To confirm on the Deck (no launch needed):
`ssh deck "objdump -d /usr/lib/libc.so.6 --start-address=0x170b80 --stop-address=0x170c00 | head -40"` (a
`rep movsb` or `vmovdqu` loop = memmove).

**`PendingWriteOverlaps`** runs for every read-only buffer of every draw that is not GPU-modified (most constant
buffers) and scans the pending write list whenever the range is inside the list's bounds. The bounds are one min/max
over all entries, and the entries are spread (end-of-pipe labels near 0x2fb15600, 0x30000000a0, UAVs elsewhere), so
the bounds rarely answer [inferred]. -> **A2.**

**`ApplyGraphicsRegisterPacket`'s checksum** is a dependent chain (xor, 64-bit multiply, xor-shift: ~6 cycles a word
on Zen 2) over every word of every register packet, on stage A and again on the scanner and every worker (each
re-applies the packets from its queue position). -> **A3.**

**`SearchBinaryInfo` / `TryPreparedPipeline`**: per stage of every prepared draw, stage A re-checks that the
prepared stage still matches: the program's hash from the shader's footer, the SRT walker over guest memory, and the
flat buffer compared. This is what makes a prepared draw safe (the worker read registers and memory earlier, possibly
before a WRITE_DATA or the CPU changed them) and not redundant. The footer read is a fast path (token at word 0, one
read). The `graphics_pipelines.find` that follows hashes a ~350-byte key (XXH3); 845 of 1,152 pipeline binds a frame
are the previous draw's pipeline [log], so a one-entry memo (memcmp of the key against the last one) would save
~50-100 ns a draw (~0.05-0.1 ms a frame). Not done: smaller than A1-A3.

**`PublishGraphics`** (no switch, `b17f99b`): `tsl::robin_map::begin()` walks the buckets from 0 to the first full
one [code: `robin_hash.h` `begin()`], and the map keeps the buckets it grew to while up to 64 compilations were
queued. Every draw walked them all to find nothing. An empty-map check before the loop gives the same result.

## 2. Why stage B waits ~1 ms a frame on stage A

From `deck-perdraw-1` (heavy view, all per-draw cuts on) [log]:
- Stage B sleeps on the packet ring **0.83 ms a frame** (1.02 ms in the summary's windows), on GPU waits 1.48 ms.
- Stage A drains **2.75 times a frame, 2.98 ms in all**, every one a "compute" drain (`ProcessCompute:2023`: stage A
  drains before each packet of the async compute queue; the queue's dispatches are pipelined, but its WRITE_DATA,
  RELEASE_MEM, WAIT_REG_MEM and DMA need stage B idle and the deferred signals written).
- Hybrid drain: wake latency **51 us average (1.5 ms max)** against an early wake 2 packets ahead (~13 us of stage B
  work at 6.45 us a draw): after a sleeping drain stage B usually runs dry before stage A is running again (~40 us a
  drain, ~0.07 ms a frame).
- Stage A's CPU: ~76% of a core [log, impl-cpu-waste.md] of a 12.6 ms frame = ~9.6 ms for 1,168 draws, **~8 us of
  stage A per draw** (everything included) against stage B's 6.45 us.

So the waits are not a batching delay: `Commit` publishes every packet at once (one store; it wakes stage B only when
it sleeps) [code]. After each drain the ring is empty, and from then on stage A produces packets more slowly than
stage B records them, so stage B waits packet by packet until the next drain. The wait shrinks with stage A's cost per
draw, which is what A1-A3 cut. Two more things would shorten it, neither done here:
- **`GOW3_DRAIN_WAKE_AHEAD=6`** (exists, restart-only): wake stage A ~6 packets (~40 us) before the end, more than the
  measured 51 us average latency minus the 30 us spin. Cheap to try in its own launch; read "wake latency" and
  stage B's packet-ring sleeps.
- **Compute-queue packets without a drain** where they do not need one (SetShReg, Nop, AcquireMem, IndirectBuffer,
  the pipelined dispatches). It would only help if the queue's batches had no RELEASE_MEM / WRITE_DATA (which still
  drain); what MediEvil's compute batches contain is not logged. Larger, riskier (ordering against the graphics
  queue), not done.

## 3. The cuts

### A1: backing copies without the table lock (`be214c8`; Linux; `GOW3_BACKING_COPY=1`, bit 1048576)

`runtime_memory_copy_backing(address, data, size)` returns 1 with the bytes copied or 0 (the caller then runs
today's loop, which overwrites the whole destination):

1. Bail out (0) if the switch is off, the lock-free reads are off (`GOW3_OPTIMISTIC_CLAMP=0` / the menu), this thread
   holds the table lock, or `table_generation` is odd (a writer holds the lock).
2. **From the R1 index:** the entry for the address's 64 KiB page, if it covers the whole range and its bucket stamp
   is unchanged since it was filled. `CachedRegion` now keeps the region's backing offset (`phys`), filled with the
   region in the same checked windows as before (lock-free searches, `cache_region` under the lock).
3. **Else one lock-free search** (the `vma_seq` window as in `clamp_optimistic`): the pieces of the range (mapped:
   backing offset; gap or reserved: zero) are read into a small array, the window is checked, offsets are checked
   against the backing view's size, and only then copied.
4. After the copy: `table_generation` must be the same even value as in step 1, else 0.

Why it gives exactly the locked path's bytes [code]:
- `table_generation` is incremented when a writer takes the table lock and again when it releases it (odd while held).
  Every edit that changes what a backing offset holds is made with the lock held: table entries (`vma_insert`,
  `vma_erase`, `split_at`), pool pages given back (`pool_give_locked`) or taken (`pool_take_locked`), released and
  freed memory's holes punched (`host_discard` in `direct_release`, `flex_free` via `drop_range`). The D4 pool path
  maps the guest view outside the lock, but the backing view does not change there, and the table and pool pages
  change only in its locked steps.
- So "the same even generation before and after" means no writer held the lock at any time during the lookup and the
  copy: the copy saw the table and the backing pages a copy under the read lock taken at step 1 would have seen.
  Anything else falls back to the locked path, unchanged.
- The index entry is valid when its stamp matches: the stamp is a sum of counters of the 16 MiB buckets the region
  spans, bumped before any edit of a mapping in them (as R1 and L2 rely on).
- The bytes: mapped ranges from `backing_base + phys`, everything else zero, as `CopySparseSerial` does (gaps and
  reserved ranges `memset`, and `runtime_memory_region` returning "not found" past the last mapping).
- Safety: the backing view is one `MAP_SHARED | MAP_NORESERVE` mapping of the whole memfd for the process's life
  (punched holes read as zero), so even a stale offset cannot fault; offsets are bounds-checked against it anyway,
  and torn table reads are caught by the `vma_seq` check before any byte is read.
- Windows: not used (`CopySparseSerial` there copies from the guest mapping with no lock: kept views).

Saved per copy [inferred]: the read lock's two atomic RMWs on a contended line (~50-200 ns together), the
13,000-region search under it (~14 dependent loads, ~50-100 ns), and the separate region lookup (an index probe,
~20-30 ns). Copies a frame are not counted today; the new line gives them ("ring buffers ... copied N" on stage A,
"Backing copies per frame" on all threads). From the profile's share (rwlock 0.59% + part of the region lookups on
stage A, ~3-4% of its user time) about **0.3-0.5 ms a frame on stage A**, and on the recorder (0.39% rwlock + 0.23%
region) **~0.2-0.4 ms**. Stage A's packets then reach stage B sooner after drains.

Test: `out/gpu/runtime-test --backing-copies` (Linux): copies at 8 offsets x 7 sizes across a mapped / unmapped /
mapped range compared with the locked loop byte for byte (before and after the clamp filled the index), the live bit,
and copies while another thread maps and unmaps a range at a fixed address 300 times (each copy all the range's
bytes or all zero, never a mix; a stable range always exact; with the writer stopped every copy goes without the
lock). WSL, 3 runs: ~30,000 copies without the lock each (~22,000 mapped, ~8,300 unmapped), the rest given back
while the other thread held the lock, no mismatch. (The full `runtime-test` stops earlier today, in
`posix_mutexes`; `sema-test` in its `setup`; `pad-test` does not compile: all three before and apart from this work.)

### A2: pending-write filter (`6fefc55`; `GOW3_PENDING_FILTER=1`, bit 2097152)

`Gow3StageA::PendingPages`: 4096 bits, the 64 KiB pages of every pending write range hashed into them.
`NotePendingGpuWrite` marks the new range (a merge is the union of two touching ranges, so the entry grows by at most
the new range); the prune in `PendingWriteOverlaps` (every 64th call) rebuilds the bits from the entries left; a range
wider than 16 MiB sets `all` (no answers until the next rebuild). The check runs **after** the prune: "all pages of the
query clear" means no entry shares a byte with it, so the scan would return false; the entry list, its pruning
cadence and every answer are today's. Size-0 queries and queries over more than 16 pages are not filtered. The bits
are kept whatever the switch, so it can be switched on live.

Expected [inferred]: `PendingWriteOverlaps` is ~0.43% of all samples (~0.3 ms a frame of stage A); the filter
replaces most scans by 1-2 bit tests when the constant buffers' pages hold no pending write: **-0.1 to -0.25 ms**.
The new line gives the share ("filter N" vs "scans N of N entries each").

Test: `pending-pages-test`: a model of the two functions (same merge, prune and scan rules, a stage B position that
lags) run with and without the filter, 2 million random steps x 3 seeds (labels, buffers, rare 20 MiB writes, queries
next to and inside written ranges, size 0): the same answer at every query and the same list after every step;
~49% of queries answered by the filter, ~7.5% overlapping.

### A3: paired register checksum (`3283c10`; `GOW3_REG_HASH_PAIRED=1`, bit 4194304)

The checksum (`gfx_reg_checksum` on stage A, the same fold on the scanner and the workers) is only compared between
stage A and the prepared draws, all through `Liverpool::HashRegisterPacket`. `HashWordsPaired` folds two words per
step (`(c ^ (w0 | w1 << 32)) * K`, then `c ^= c >> 32`; an odd last word as before): still a bijection of the 64-bit
state per step, half the steps. Only the values change. The one-word hash is the same code as before (moved to
`gow3_stage_a.h` so it can be tested).

Equivalence: a prepared draw is used only when its checksum equals stage A's and the per-stage checks of
`TryPreparedPipeline` pass, as before. A live flip can make a worker and stage A hash one submission differently:
those draws take the unprepared path (correct, slower). **Correction (2026-11-07): nothing reseeds them.** The scanner
carries its own checksum chain and is seeded from stage A only at the first submission or a rebase, so after a live
flip no draw is prepared for the rest of the launch (logs: `deck-stagea-1.log` from line 2646, `deck-launch15.log`
from 9198 and 20691). A false match needs a 64-bit collision, as today.

Test: `stage-a-hash-test`: the one-word hash equals the old loop (100,000 random packets); both hashes tell apart all
one-bit flips and neighbour swaps of a packet and 2 million near-identical packet variants (0 collisions);
**0.63 vs 1.03 ns a word** on the 7800X3D (-38%).

Expected [inferred]: words hashed a frame are unknown (the new line counts stage A's). At ~60-80 words a draw,
~1,170 draws: ~70-95k words a frame, ~0.15-0.25 ms of chain on stage A at the Deck's ~3 GHz, -38%: **~-0.06 to
-0.1 ms on stage A**, about as much again on the scanner and the workers together. Check that "Draw preparation: N
prepared pipelines used, M not" keeps its ratio in both states (a drop would mean the two sides disagree).

## 4. Considered, not done

| Item | Why not |
|---|---|
| Ring reuse looked up before the clamp / GPU-modified / pending checks | not the same: a range written by the GPU after its first copy in the epoch must go to stage B (today's order skips the reuse then) |
| Ring reuse across command buffers | the CPU may write constants between submissions; no exact "unchanged" test for untracked pages |
| A memo of `graphics_pipelines.find` (last key) | ~0.05-0.1 ms a frame; next candidate |
| Flat-buffer ring copies reused when equal to the stage's last one | needs the hold rules of ring reuse for address-less copies; small |
| Batched `Commit`s (one seq_cst store per N packets) | adds latency where stage B already waits on stage A |
| Lock-free `store_label` / `write_backing` | writes: a stale offset would write into another allocation; cannot be checked after the fact |
| Compute-queue packets without a drain | section 2 |

## 5. Deck A/B (one launch, the user's go)

Settings as the per-draw run (`deck.sh ini logging`, 67%, heavy graveyard view, standing still). The Deck preset's
switches (drain hybrid, R1, T2, decode pacing, early EOP, arena host-visible 0) stay as they are; A1 uses the R1
index when R1 is on.

Before (game closed):
1. `deck.sh deploy /root/src/medievil MediEvil-x86_64-stagea.AppImage` (the WSL tree is on this branch's commit), then
   `deck.sh status`.
2. `deck.sh env "stage A decode A/B" GOW3_FRAME_STATS=1 GOW3_ARENA_HOST_VISIBLE=0 GOW3_DRAIN_HYBRID=1
   GOW3_TEXSET_WIDE=1 GOW3_REGION_INDEX=1 GOW3_BACKING_COPY=1 GOW3_PENDING_FILTER=1 GOW3_REG_HASH_PAIRED=1` plus the
   lines extra-env.txt has now (`deck.sh status` shows them; `deck.sh env` replaces the file).
3. `deck.sh toggle "0 0"` (all on).

In game (heavy view; no SSH sampling while `ab` runs):
4. `deck.sh ab "0 0" "0 3145728" 30 5` (A1 + A2 off vs on; A3 is restart-only since `adfeb67`: a live flip stopped
   draw preparation for the rest of the launch, see the correction below).
5. One pair per cut, same launch: `deck.sh ab "0 0" "0 1048576" 20 3` (A1), `"0 0" "0 2097152"` (A2), A3 only as a restart pair (`GOW3_REG_HASH_PAIRED=1` vs unset).
6. CPU and power in their own windows afterwards: `deck.sh toggle "0 3145728"`, ~10 s, `deck.sh threads 5` and
   `ssh deck python3 ~/MediEvil/apu_power.py`; then `deck.sh toggle "0 0"`, ~10 s, the same; twice each.
7. `deck.sh pull deck-stagea-1`, `deck.sh summary deck-stagea-1 --min-draws 1000`.

What to read per state:
- Stage A: `shadPS4:GpuComm` CPU% in `deck.sh threads` (want lower with "0 0"); "stage A waited" in the Draw pipe line.
- Stage B: "Stage B sleeps per frame: ... packet ring" (want lower), busy %, FPS, p99.
- A1: "Backing copies per frame": most copies "without the lock", few "the locked way"; `gow3:VkRecorder` CPU% lower.
  **Look at the picture while toggling 1048576**: constants come through this path (lighting, materials, UI, fog);
  any flicker or wrong colours only in one state means a bug.
- A2: "filter N" a large share of the checks, "scans" down.
- A3: "Draw preparation: N prepared pipelines used, M not" ratio the same in both states (~83% used in
  deck-perdraw-1); `gow3:DrawScan` / `gow3:DrawPrep*` CPU% a little lower.
- All: the CPU / GPU W split at the cap.

Pass: each cut's line shows it working, the picture is the same, FPS not worse and stage A's (and for A1 the
recorder's) CPU lower. Then candidates for the Deck preset's switch list; A2 (exact, cross-platform) could also become
a default.

## 6. Builds and tests (2026-11-05)

- Windows (`research/tools/build-clang64.sh`): builds, no new warnings. `ninja -C out/gpu stage-a-hash-test
  pending-pages-test resident-blocks-test texset-table-test draw-pipe-drain-test`: all pass (and on Linux in WSL).
- Linux (WSL `/root/src/medievil`, switched from `deck-lab` `d20f010` to the local branch `stage-a-decode`; `CC=clang CXX=clang++ GOW3_CPU_LEVEL= bash build.sh --test`, then the AppImage written as
  `dist/MediEvil-x86_64-stagea.AppImage` by a copy of `appimage.sh` with only the output name changed): builds (LTO,
  static, baseline CPU level, no warnings); **131,344,888 bytes, sha256
  `718990178e7174e6e76a6d4c055b0dcf62a4254b4d638a921bd6c896de83b45e`**, `--help` runs; every other `dist/` file's
  sha256 checked unchanged. Built from `5e4e4b6` (the commits after it are docs only).
- Python (`py -3.14 -m unittest discover -s tests`): 143 run, 3 failures + 6 errors, all in tests untouched here
  (`test_patches` x6, `test_packaged_vulkan` x2, and `test_mods` overlay run: "No eboot.bin in ../CUSA01623", a game
  folder relative to the checkout, so it fails from a worktree).

## Unverified

Everything in game: the gains (estimates from the profile and per-operation costs), the share of constant buffers
the A2 filter answers, the number of copies and register words a frame, and that the picture is unchanged (A1 and A3
are exact by the arguments above and the tests; A2 is exact by construction and the model test). The libc hotspot
being memmove (one objdump on the Deck settles it).
