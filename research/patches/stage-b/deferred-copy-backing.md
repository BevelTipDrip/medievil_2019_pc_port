---
id: deferred-copy-backing
title: Windows: deferred guest copies (recorder, copy threads) read the backing view, never the guest mapping (200% deadlock)
area: stage-b
status: active
platforms: windows
default_windows: on
default_deck: n/a
origin: ours
class: G
commits: 3a84125
files: gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp, gpu/shim/gow3gpu.cpp, gpu/shim/core/memory.h, src/runtime_memory.c
symbols: RunGuestCopy, deferred_copy_backing, CopySparseSerial, CopySparseMemory, runtime_memory_copy_backing_deferred, copy_backing_lockfree
switches: GOW3_DEFERRED_COPY_BACKING
tests:
docs: plan-work-queue.md
depends_on: A1
related: copy-fault-trace, pool-small-copies, copy-thread-backing-view, decode-pacing, upstream-4k-framebuffer-pool
checked:
---
## What it does

`SmallGuestCopy` defers stream-buffer copies of guest memory: stage A reserves the space, the copy runs later on
`gow3:VkRecorder` (or a copy thread). `RunGuestCopy` now passes `backing` to `CopySparseMemory`: on Windows
`CopySparseSerial` tries the lock-free A1 copy (`runtime_memory_copy_backing_deferred`, no `GOW3_BACKING_COPY`
opt-in), then the locked `runtime_memory_read_backing` loop, and never `memcpy`s from the guest address. Stage A's
own copies (no `backing`) keep the direct `memcpy`. `GOW3_DEFERRED_COPY_BACKING=0` (read once at start): the guest
mapping, as before.

## Invariants

- No copy run off stage A / stage B may fault on the GPU side's page protection. The fault handler
  (`GuestFaultSignalHandler`) resolves faults inline only on stage A and stage B; any other thread hands
  `ReadMemory` to stage A and waits (`SendCommand<true>`), and stage A, resolving it, waits for stage B
  (`DrawPipe::Drain`) or the recorder (a download's submit runs `SyncRecording`): a deadlock by construction.
- The backing bytes are correct for a deferred copy: stage A defers only a range not GPU-modified at queue time
  (`small_gpu_modified`), so the draw needs the guest's CPU bytes, which the backing view holds.
- The fallback is the locked backing read, never the guest mapping.
- A1's own switch and toggle (`GOW3_BACKING_COPY`, `toggle2:BackingCopy`) still gate only Linux's stage A path.

## Interactions

- A1: shares `copy_backing_lockfree` (now compiled on Windows); its counters print on Windows with
  `GOW3_FRAME_STATS`, plus the locked reads' table-lock wait (`backing_read_wait_ns`).
- copy-thread-backing-view: Linux already read every copy through the backing view; it never deadlocked this way.
- decode-pacing: not involved in the caught fault (copy-fault-trace: "recorded 0 frames ago", no lead).
- upstream-4k-framebuffer-pool: 200% at 4K still runs out of the 512 MB framebuffer pool on a level load.

## Evidence

- Found 2026-11-15, 4K 200%: two hangs on a save load; cdb stacks (`research/docs/data/hang-200pct-stacks-20261115.txt`)
  and the copy-fault trace: `gow3:VkRecorder read at 0x2083261000, in a copy of 0x2083240000 +135168; page
  GPU-modified: no, image readback: no; recorded 0 frames ago ... sent to the GPU command thread`, no "handled".
- Run 2026-11-15 (fix build 10:37): 200% at 4K three times, no recorder copy fault, no hang (two framebuffer-pool
  OOMs, one run into Dan's Crypt). 4K 75% DLSS A/Bs against the baseline: inconclusive (static screenshots).

## History and pitfalls

- The first reading (a later GPU write protecting the page) was wrong: the caught fault was on a page neither the
  buffer cache nor an image readback protected; the deadlock comes from waiting on stage A from the recorder at
  all, whatever protected the page.
- Rejected before building: reading the backing view for every copy (stage A loses its correct inline downloads);
  a stage A wait when protecting a page with pending copy sources (puts the cost on stage A).
