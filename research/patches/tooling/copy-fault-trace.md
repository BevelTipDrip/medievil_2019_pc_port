---
id: copy-fault-trace
title: GOW3_COPY_FAULT_LOG: a guest fault inside a host copy logged before and after it is handled (thread, copy, protection, age); dev build only
area: tooling
status: diagnostic
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: 3a84125, 21f7fe2
files: gpu/shadps4/video_core/page_manager.cpp, gpu/shim/gow3_copy.h, gpu/shim/gow3_copy.cpp, gpu/shim/gow3gpu.cpp, gpu/shim/gow3_decode_pacing.h, gpu/shim/gow3_decode_pacing.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.h, gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp
symbols: CopyFaultTrace, ReadingGuest, ReadingGuestScope, QueuedFrameScope, reading_guest, fault_log_enabled, MayHaveReadback, FramesEnded, FramesRetired, GuestFaultSignalHandler
switches: GOW3_COPY_FAULT_LOG
tests:
docs: plan-work-queue.md
depends_on:
related: deferred-copy-backing, decode-pacing, wait-sites
checked:
---
## What it does

`CopySparseSerial` marks the range it reads in a thread-local (`ReadingGuestScope`); a deferred copy also records
the frames ended when it was recorded (`QueuedFrameScope`, `FramesEnded`). `GuestFaultSignalHandler` builds a
`CopyFaultTrace`: a fault inside such a copy prints `Copy fault #N: <thread> read|write at <addr>, in a copy of
<source> +<offset> (<size> bytes); page GPU-modified: yes/no, image readback: maybe/no; recorded N frames ago;
frames ended E, retired R; handled inline | sent to the GPU command thread`, then `Copy fault #N handled in X ms`
when the handler returns. A fault line without a "handled" line never came back (a deadlock).

## Invariants

- **Dev build only (P5, 2026-11-24).** The release build has no copy-fault trace: `fault_log_enabled` is
  `inline constexpr false` there (gow3_copy.h `#if GOW3_DEV_TOOLS`), so the scopes, the frame capture in
  `SmallGuestCopy` and `CopyFaultTrace` fold away and `GOW3_COPY_FAULT_LOG` is not read. (Until then: release default
  off, `=1` on.)
- Dev build: default on; `GOW3_COPY_FAULT_LOG=0`: the scopes and the frame capture do nothing (`fault_log_enabled`, a
  namespace-scope constant read once); one predictable branch per copy remains.
- Lines: the first 20 faults, then each power of two: not periodic, but a run that faults often still shows the count.
- The "before" line is printed before handling, so it reaches the log even when the thread then blocks.
- `MayHaveReadback` is the texture cache's lock-free seqlock read: "maybe" also while the span list is republished.

## Interactions

- deferred-copy-backing: with it, the recorder's and copy threads' copies no longer fault; the lines left are
  stage A's own (handled inline).
- decode-pacing: `FramesEnded` / `FramesRetired` are 0 without it ("recording frame not known").

## Evidence

- 2026-11-15, 4K 200% before the fix: the deadlock's line (deferred-copy-backing, Evidence).
- 150% (10 min, 8 level loads): six stage A faults, all handled inline (0.002-0.2 ms), pages neither
  GPU-modified nor an image readback. 200% after the fix: stage A faults of 1.05 and 2.85 ms (plan-work-queue.md row
  "Stage A copy faults that stall for 1-3 ms").

## History and pitfalls

- First version: the per-copy marker ran with the trace off (two thread-local writes per copy); the user asked
  that a switched-off diagnostic cost nothing, so the marker moved behind the same constant.
