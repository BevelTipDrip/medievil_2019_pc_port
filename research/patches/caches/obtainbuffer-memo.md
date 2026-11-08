---
id: obtainbuffer-memo
title: ObtainBuffer memo (S4): (addr, size) -> arena binding for repeated large vertex buffers (not built)
area: caches
status: rejected
platforms: all
default_windows: n/a
default_deck: n/a
origin: ours
class: G
commits:
files: gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp, gpu/shadps4/video_core/buffer_cache/buffer_cache.h
symbols: BufferCache::ObtainBuffer, GetArena, EnsureResident, SynchronizeMemory, ResolveReadbacks, NoteArenaUse, STREAM_THRESHOLD
switches:
tests:
docs: study-deck-rescore-rejected.md#c-analysed-gated-or-ranked-never-built, study-frame-cost-2026-10-29.md#4-dropped-ideas-do-not-re-propose, study-stage-b-native.md#3-candidates, fix-inventory.md#10-tried-and-dropped
depends_on:
related: S2, C5, R2-residency-bitmap, stream-class-table, lockfree-region-reads, hot-pages, U2
checked: 2026-11-07
---
## What it does

Never built. The idea (study-stage-b-native.md S4): a memo from `(address, size)` to the arena binding that
`BufferCache::ObtainBuffer` returns, for the large static vertex buffers bound again and again. A hit skips the
arena path: `ResolveReadbacks`, `GetArena`, `EnsureResident`, `SynchronizeMemory` per call.

## Invariants

- A hit is correct only while nothing could have changed the range since the memo entry was made: no CPU write
  (no CPU-modified page), no GPU write that needs a sync, no pending image readback over it, still resident, same
  arena. A hit that misses any of these binds stale vertex data.
- That is the blocker: there is no CPU-write generation to validate against. CPU writes are seen only as page bits
  (`IsRegionCpuModified`) when the range is synchronized. Checking them per hit is most of the work the memo
  would skip.
- If it is ever built: invalidate on every path that marks pages CPU- or GPU-modified, on residency changes, and on
  readback queueing (C5's spans are the readback part). Key on `is_written` and `is_texel_buffer` too: both change
  `SynchronizeMemory`'s work.
- A hit must still call `NoteArenaUse` when `arena_mappings` is on (U2): it records the tick the range is used at,
  which the next direct write waits for. Read-only ranges up to `STREAM_THRESHOLD` (16 KB) mostly take the stream
  copy (S2 sends some to the arena), so the large vertex buffers are the memo's target.

## Interactions

- S2 already keeps small read-only buffers in the arena without copies; the study counts S4 as "covered by S2's
  memo" for that part.
- C5, R2-residency-bitmap, stream-class-table, lockfree-region-reads: made the sub-steps a hit would skip cheaper
  and lock-free, which is why the PC gain vanished.
- hot-pages: pages counted always CPU-modified would never be memo hits (inferred).
- U2: shares the arena path's per-use tick (`NoteArenaUse`), see Invariants.

## Evidence

- Study estimate: about 0.05-0.1 us per packet on its own (study-stage-b-native.md).
- Dropped on the PC (study-frame-cost section 4): "the paths it would skip are already lock-free. No CPU-write
  generation exists for invalidation. Its slow-frame claim was wrong (+10%, not +23-38%)."
- Deck rescore C8: -0.3 to -0.6 ms a frame estimated (ObtainBuffer 4.3x slower than on the PC). "Partly wrong for
  the Deck": the PC argument was about locks, the Deck's cost is memory latency, which a hit skips. "The
  invalidation problem stands". Needs a design first. Not measured; not verified in game.

## History and pitfalls

- Proposed in study-stage-b-native.md as S4, after S2; dropped on analysis only, never launched.
- Worth retrying only on the Deck (or under U5's in-place model) with a cheap, exact validity check. One example
  is a per-region write generation bumped by every CPU-modified marking. Measure `ObtainBuffer`'s share
  (`GOW3_SECTIONS` ObtainBuffer split) first.
