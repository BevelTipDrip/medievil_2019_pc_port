---
id: readback-first-wait
title: First-copy readback read wait: stale first readbacks as a Mausoleum suspect (off by default)
area: caches
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: e383f7ec49, db9b933906, 1097324745
files: gpu/shadps4/video_core/texture_cache/texture_cache.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.h, gpu/shadps4/video_core/texture_cache/readback_queue.h
symbols: readback_first_wait, readback_published, TextureCache::UnwatchReadback, UpdatePageWatchers, TextureCache::ProcessDownloadImages, readback_log
switches: GOW3_READBACK_FIRST_WAIT
docs: plan-linux-deck.md#hilltop-mausoleum-lighting-pool-pages-leaked-on-linux-2026-11-0607-fixed-7246f88--f74e1a1
depends_on: lineage-deferred-readbacks
related: tiny-target-readback, pool-kept-pages, C5, mausoleum-diagnostics, mausoleum-toggle-bisect
verified: 2026-11-07
---
## What it does

In stale mode a CPU read of a pending readback sees the last published copy. For a range's first copy nothing
was published yet, so the read sees bytes no copy wrote. With `GOW3_READBACK_FIRST_WAIT=1` (needs stale
readbacks), `ProcessDownloadImages` treats a copy whose address is not yet in `readback_published` as a first
copy. It pushes the copy with `reads = true` and watches its pages for CPU reads too
(`tracker.UpdatePageWatchers<true, true>`), so that read faults and waits for the GPU as in precise mode. Later
copies stay stale.

## Invariants

- Watch and unwatch must match. An entry pushed with `reads` must drop its read watch through `UnwatchReadback`
  (`UpdatePageWatchers<false, true>`) on every exit: retire, cancel, and stale mode's `ReleaseOutside`. A path
  that calls `WatchReadback<false>` for it leaves pages read-protected: endless faults or a hang.
- `readback_published` (under `readback_mutex`) gets an entry at every retire in stale mode and on the budget-full
  immediate path, whatever the switch. It only grows, by one entry per distinct readback address [code].
- Off by default since `1097324`. `db9b933` had made it default on (`!= '0'`) until then (`db9b933` and
  `892a801`, under an hour on 2026-11-07).

## Interactions

- C5's spans cover these entries (they stay `watched`), and the CPU fault paths keep the lock, so the read wait
  still happens with C5 on.
- Paired with tiny-target-readback as Mausoleum suspects. The cause was pool-kept-pages (leaked pool pages on
  Linux).
- The same first commit (`e383f7e`) added the `GOW3_READBACK_LOG` first-copy log and the frame map past the UI
  passes (mausoleum-diagnostics, renderdoc-capture).

## Evidence

- plan-linux-deck: "stale first readbacks (`GOW3_READBACK_FIRST_WAIT`)" is among the dead ends "each ruled out by a
  run; kept as opt-in diagnostics". `1097324`: off by default for "frame cost on the Deck; did not fix the
  Mausoleum". The docs give no numbers. Not verified in game as a fix (it was not one).

## History and pitfalls

- A read wait on first copies only costs at level loads and new targets. In MediEvil it did not change the
  lighting, so first-copy staleness was not the bug.
- Worth retrying for a game whose CPU reads a GPU result once (at load) and acts on it. Check with
  `GOW3_READBACK_LOG` ("Readback first copy queued / published") that the read comes before the publish.
