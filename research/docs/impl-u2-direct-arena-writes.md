# U2: direct arena writes (implementation, 2026-11-04)

Plan: [plan-linux-deck.md](plan-linux-deck.md) section 5.2 (copy sites C1-C10) and 5.3 (U2 row). Built on `deck-lab`,
not run yet. Code: `gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp` (search `U2`), `vk_scheduler.{h,cpp}`
(`SetHostAccessBarrier`, `DrawRecorderQuiet`), `vk_rasterizer.cpp` (`NoteDmaUse`, the draw-pipe sync callback),
`vk_runtime.cpp` (barrier count), `gpu/shim/gow3_toggles.h` (`Gow3Toggle2::ArenaDirectWrites`, stats),
`videoout/driver.cpp` (stats line).

## 1. What it does

With `GOW3_ARENA_HOST_VISIBLE` on, the buffer arena's residency chunks (32 MiB, `ResidencyPool`) come from a
`DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT` memory type and are mapped once, persistently. Every resident block's host
address goes into a lock-free two-level table (`ArenaMappings`, by block number; blocks are never released).

Then the two buffer copy sites write guest bytes **straight into the arena mapping** when the guard allows it:

| Site | Before | With U2 (guard passes) |
|---|---|---|
| C1 uploads (`SynchronizeMemory` > `UploadDirect`, before `UploadCopies`) | guest > staging (copy threads / recording-thread batch) > `vkCmdCopyBuffer` staging > arena (ends the render pass, transfer barrier before the next read) | guest > arena mapping on the same threads (groups of ~1 MiB on the copy threads, small ones in the caller's batch); no staging, no copy command, no pass end, no barrier |
| C2 precise CPU writes (`ProcessCpuWritesLocked`) | bytes > staging > `vkCmdCopyBuffer` | `memcpy` of the queued bytes into the mapping (small, on the calling thread) |

Copies that fail the guard take the old path. Within one upload the copies are decided one by one (some direct, the
rest repacked into one staging copy). Downloads (C5/U3), `WRITE_DATA`, fences and image uploads are unchanged.

## 2. The guard, and why it is correct

A direct write into `[addr, addr + size)` happens only if all of these hold (`DirectWriteBlocked`, plus two checks
before it):

1. **The live toggle is clear** (toggle file second word bit 8).
2. **The caller is in order with the queued draws** (`Scheduler::DrawRecorderQuiet`): it is stage B (the draw
   recording thread), or stage A with stage B's queue empty, or there is no draw pipe. A staged copy is ordered by
   `Record()`, which first waits for stage B (`SyncWithDrawRecorder`); a direct write records nothing, so without this
   check stage A could write bytes that draws still queued for stage B, recorded later in the same command buffer,
   should not see. The check never waits: any other case falls back ("off the recording thread").
3. **The GPU is done with every page of the range.** `UseTicks` holds, per page, the newest scheduler tick at which
   the GPU uses it. Noted (raised, never lowered) by:
   - `ObtainBuffer` for every arena binding (vertex, index, constant, storage, indirect, texel, copy and fill
     sources/destinations, HLE shaders, the fault manager, images uploaded from GPU-written buffers). Noted after the
     sync, with `CurrentTick()`, the tick of the command buffer the caller is recording into. Every GPU use of the
     arena goes through `ObtainBuffer` (checked: `grep buffer_cache.` in `video_core`), except the next item.
   - `NoteDmaUse`: a draw or dispatch reading through the BDA page table may read any resident block, so a global DMA
     tick blocks all direct writes until the GPU is past it (MediEvil's DMA use is rare).
   - Staged copies into the arena (C1 and C2 fallbacks) and the direct writes themselves.
   
   "Done" is `tick == 0 || (tick < CurrentTick() && scheduler.IsFree(tick))` (the timeline semaphore reached it).
   **Work recorded but not yet submitted** has the current tick, which is never free: an earlier draw in the open
   command buffer that read the range forces the staging path, whose copy the command buffer orders after that draw.
   Notes made while `SubmitExecution` drains stage B (after `NextTick`) carry the *next* tick: later than the work's
   real one, so only more conservative. Pages are 4 KiB for uses up to 64 KiB, 64 KiB up to 16 MiB, 1 MiB above; a
   query reads all three tables (the game sub-allocates per-frame data inside 64 KiB pages: one coarse page per draw
   would refuse the next upload beside it).
4. **Every block is mapped** (a chunk whose `vkMapMemory` failed leaves its blocks on the staging path).

**Ordering against earlier staged copies:** a staged copy notes its destination at the current tick, so a later
direct write to those pages waits until the GPU has run that copy (counted as "staged copy queued"). The reverse (a
direct write, then a staged copy) is ordered by time: the direct write ends before the submission, the staged copy
runs in it. Two direct writes to one range in one command buffer cannot happen (the first notes the current tick).

**Visibility to the GPU (host write > device read):** the chosen type is `HOST_COHERENT`; every direct write is done
before the `vkQueueSubmit` of the command buffer that reads it (the copy threads and the recording-thread batch are
the ones staging copies use, and `SubmitExecution` waits for them: `WaitHostCopies`). A queue submission performs an
implicit memory domain operation for host writes made before it (Vulkan spec, "Host Write Ordering Guarantees"),
so no barrier is needed for this direction. Guest fences already wait for the same host copies, so the guest cannot
rewrite memory a pending direct copy still reads (as today).

**Device write > host write (WAW):** a semaphore signal seen by the host is an execution dependency only; it does
not make device writes available to the host domain. A GPU write the guard saw finished (a shader writing the
range, a staged copy or fill into it) could otherwise still be written back from a GPU cache after a later direct
write. So with U2 available every submission ends with one global barrier `ALL_COMMANDS / MEMORY_WRITE > HOST /
HOST_READ|HOST_WRITE` (`Scheduler::SetHostAccessBarrier`, set at start whenever the arena is mapped, regardless of
the live toggle). C2 always writes GPU-written bytes, so it depends on this. Cost: one barrier per submission
(~2 a frame on the Deck); the kernel's end-of-submission fence already writes caches back on AMD in practice.

**Device read > host write (WAR):** the semaphore wait observed on the host (`IsFree`) is enough.

**Live toggle:** ticks are noted whenever the arena is mapped, toggle on or off, so switching it on mid-run never
lets a write pass a use recorded while it was off. Switching off simply returns to staging.

## 3. Memory types per platform

At start the log prints every memory type (`GPU memory types: ...`, also the U1 heap audit) and the arena's choice
(`Buffer arena: memory type N (...), ... direct arena writes (U2) available|off|unavailable`).

| Platform | Default | Expected type [inferred until the first log] |
|---|---|---|
| Steam Deck (RADV, Van Gogh APU) | `GOW3_ARENA_HOST_VISIBLE=1` (Linux + `IsIntegrated()`) | RADV on APUs exposes `DEVICE_LOCAL\|HOST_VISIBLE\|HOST_COHERENT` over the carve-out heap (the heap the arena's first `DEVICE_LOCAL` type already uses, so the footprint does not move). A `DEVICE_LOCAL` type that is also `HOST_CACHED` is preferred if one exists (CPU reads for U3); the gate-1 probe's type 5 (`HOST_VISIBLE\|HOST_COHERENT\|HOST_CACHED`) is GTT, not device-local, so it is not chosen |
| PC, RTX 4090 with ReBAR (Windows) | `0` (off). `GOW3_ARENA_HOST_VISIBLE=1` to try | `DEVICE_LOCAL\|HOST_VISIBLE\|HOST_COHERENT` (23.6 GiB heap). Writes cross PCIe as write-combined stores, about the cost of staging writes |
| Discrete without ReBAR | `0`; with `1` the 256 MiB BAR heap is refused (rule below) | none: today's type |
| Other Linux iGPUs | `1` | as the Deck |

Rule (`HostVisibleArenaType`): a type the sparse arena accepts (`memoryTypeBits`), with all three flags, not
lazily-allocated, protected, `DEVICE_COHERENT_AMD` or `DEVICE_UNCACHED_AMD`; in the arena's current heap or in a
heap of at least 4 GiB. `1` prefers `HOST_CACHED`, `2` prefers an uncached (write-combined) type. No such type: the
arena keeps the first `DEVICE_LOCAL` type and U2 is unavailable. If the first chunk cannot be mapped, U2 is
unavailable too (the chunks stay in the host-visible type, unmapped use is identical for the GPU).

## 4. Switches

| Switch | When | Values |
|---|---|---|
| `GOW3_ARENA_HOST_VISIBLE` | start only (memory type) | `1` host-visible, cached preferred; `2` host-visible, uncached preferred; `0` off. Default 1 on Linux integrated GPUs, 0 elsewhere |
| Toggle file second word, bit **8** (`Gow3Toggle2::ArenaDirectWrites`) | live, every 250 ms | set: direct writes off (staging path, as before); clear: on. File content `0 8` = off, `0 0` = on (keep any other bits you use: the first number is the first word) |
| `GOW3_RESIDENCY_CHUNK_MB=0` | start | one allocation per `EnsureResident`; mapped too |

## 5. Stats (`GOW3_FRAME_STATS=1`, every 5 s)

```
Arena writes per frame (U2 on|off|unavailable): direct D (X MB), staged S (Y MB); staged because: toggled off a,
GPU busy b, staged copy queued c, not mapped d, off the recording thread e; saved f copy commands, g pass ends;
pass ends by staged copies h; all pass ends i, barrier batches j
```

- **direct / staged**: calls per frame (one per `SynchronizeMemory` with uploads, one per C2 write) and MB. Staged
  is counted on every platform (the PC's "Uploads: buffers" split).
- **staged because**: per copy range. "GPU busy" = a draw / dispatch / DMA not finished uses the pages; "staged copy
  queued" = a staged copy into them is not finished (the ordering case; only told apart with stats on).
- **saved copy commands / pass ends**: whole `vkCmdCopyBuffer` calls avoided, and those that would have ended an open
  render pass. **pass ends by staged copies**: the uploads that still end a pass.
- **all pass ends / barrier batches**: every cause (`Scheduler::EndRendering`, `Runtime::FlushBarriers`): compare
  the U2-on and U2-off windows; the transfer barrier after each staged copy is not counted separately.
- Still from the existing lines: `Frame stats` (stage B busy, "blocked: host copies"), `Uploads: buffers MB/s`
  (includes direct bytes), `Mapped writes` (staging uploads MB fall with U2 on).

## 6. Risks

1. **A GPU use of the arena that bypasses `ObtainBuffer`** would not be noted, so a direct write could race it (wrong
   vertices, flicker). Checked callers today: rasterizer, HLE shaders, texture cache (`ObtainBufferForImage`), fault
   manager; DMA via `NoteDmaUse`. New arena users must note their use. The live toggle isolates it: flicker that
   goes away with `0 8` is this class.
2. **Hit rate.** Per-frame data the game rewrites while the GPU still reads the previous copy stays staged ("GPU
   busy"). With the Deck's GPU ~1 frame behind (fence lag ~14 ms), ranges reused every frame miss; UE4's ring buffers
   (several frames) should hit. The stats line decides.
3. **Host-copy waits do not shrink.** Direct writes run on the same copy threads and submission still waits for them;
   U2 removes the GPU copy, its barriers and pass ends, the staging allocation, and one memory hop, not the CPU
   memcpy. Stage B's "blocked: host copies" (4-5%) is not the target; render-pass ends, barriers and RADV's per-copy
   cost in the GPU frame are.
4. **GPU read speed of the chosen type.** A `HOST_CACHED` device-local type (if RADV exposes one) may be snooped
   (plan 5.1 caveat). Mitigation: `GOW3_ARENA_HOST_VISIBLE=2` (uncached), or `0`.
5. **CPU write speed.** Writes into write-combined memory are fast only when sequential; `CopySparseMemory` is a
   memcpy (sequential). Measured gate-1 CPU write bandwidth into Vulkan memory: 4.9-7.7 GB/s, far above ~0.5 GB/s of
   uploads.
6. **Downloads (unchanged, D3)** read the arena on the readback queue; a direct copy still pending on a copy thread
   could overlap a range being downloaded only for bytes both CPU- and GPU-modified (the twin case, filtered
   before). Today's staged copy is equally unordered against that readback.
7. **The end-of-submission barrier** costs a little on every submission whenever the arena is mapped (U2 toggled off
   included). The "off" side of the A/B therefore carries it too, which keeps the comparison fair for the copy path;
   a separate launch with `GOW3_ARENA_HOST_VISIBLE=0` is the true baseline.
8. **Memory:** the use-tick tables (a 512 KiB pointer array, 32 KiB leaves on demand: a few MB) exist only when the
   arena is mapped.

## 7. Deck test plan (one launch, the user's go)

Setup (Claude, over SSH, before the launch; nothing else running):
1. Deploy the AppImage from this commit as usual.
2. `~/.local/share/medievil-pc/extra-env.txt`: `GOW3_FRAME_STATS=1`, `GOW3_TOGGLE_FILE=/home/deck/.local/share/
   medievil-pc/toggles.txt` (no `GOW3_ARENA_HOST_VISIBLE` line: the Linux iGPU default is 1). Same settings as the
   U5-2 run A (67%, fences off, auto clocks).
3. `echo "0 0" > ~/.local/share/medievil-pc/toggles.txt` (U2 on at start).

Run:
1. The user starts the game, loads the graveyard, stands at the usual heavy view.
2. Check the log first: `GPU memory types` and `Buffer arena: ... direct arena writes (U2) available`. If it says
   off/unavailable, stop: the type table tells why (record it in this doc).
3. Every ~30 s, Claude flips the file over SSH: `0 8` (off), `0 0` (on), for 4-6 cycles. The log's
   `Runtime: disabled optimizations second word=` lines mark each switch; drop the first 5 s window after each.
4. The user watches for flicker, wrong or exploding vertices, missing UI or text, stale skinned meshes, and says which
   state it was in.

What decides (on vs off windows, same view):
- **Stage B busy %** (`Frame stats`) and **FPS**; **1% / 0.1% lows** from the user's overlay (short-window lows only:
  rtss-overlay-windows rule).
- **Arena writes line**: direct share of calls (the hit rate), "saved pass ends" vs "all pass ends", barrier
  batches per frame, "GPU busy" fallbacks.
- `blocked: host copies` % (expected about flat, risk 3).
- Pass rule: any visual problem with U2 on = fail (find the unnoted use). Otherwise keep U2 on by default on the
  Deck if FPS or lows improve by >= 2% with no regression in the other; if flat, keep it on only if pass ends per
  frame and stage B busy fall (cheaper GPU frame), else make it opt-in.
- Follow-ups: if "GPU busy" dominates, per-frame ring data is reused within the GPU's lag (risk 2); then a launch
  with `GOW3_ARENA_HOST_VISIBLE=2` if a cached type was chosen; then the PC ReBAR A/B
  (`GOW3_ARENA_HOST_VISIBLE=1`, same toggle steps).
