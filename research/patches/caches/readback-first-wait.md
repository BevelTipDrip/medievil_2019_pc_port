---
id: readback-first-wait
title: "First-copy readback read wait: a stale first readback fed UE4's sky light brightness (Hall of Heroes / white walls); on by default"
area: caches
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: e383f7ec49, db9b933906, 1097324745
files: gpu/shadps4/video_core/texture_cache/texture_cache.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.h, gpu/shadps4/video_core/texture_cache/readback_queue.h
symbols: readback_first_wait, readback_published, TextureCache::UnwatchReadback, UpdatePageWatchers, TextureCache::ProcessDownloadImages, readback_log
switches: GOW3_READBACK_FIRST_WAIT
docs: plan-linux-deck.md#hilltop-mausoleum-lighting-pool-pages-leaked-on-linux-2026-11-0607-fixed-7246f88--f74e1a1
depends_on: lineage-deferred-readbacks
related: tiny-target-readback, pool-kept-pages, C5, mausoleum-diagnostics, mausoleum-toggle-bisect, M1
checked: 2026-11-09
---
## What it does

In stale mode a CPU read of a pending readback sees the last published copy. For a range's first copy nothing
was published yet, so the read sees bytes no copy wrote. With stale readbacks on, `ProcessDownloadImages` treats
a copy whose address is not yet in `readback_published` as a first copy. It pushes the copy with `reads = true`
and watches its pages for CPU reads too (`tracker.UpdatePageWatchers<true, true>`), so that read faults and waits
for the GPU as in precise mode. Later copies stay stale. On by default (`GOW3_READBACK_FIRST_WAIT=0`: off).

What it fixes: at each level load UE4 reduces the sky light cubemap to a 1x1 RGBA16F target and reads its average
brightness back once. MediEvil gets that target at a new address on every load, so the read was always a first
copy and saw leftover bytes. The brightness goes into the reflection constants (`ReflectionStruct`, the float at
+16 [inferred from UE4 4.21's layout]). Reflections are normalized against the baked lighting by dividing by a
brightness blended from it, so leftover zeros flooded the walls with reflection light: teal in the Hall of Heroes
anteroom on 01.02, white walls on 01.00. Leftover non-zero bytes gave a usable but wrong balance (the "good" loads,
flatter than intended).

## Invariants

- Watch and unwatch must match. An entry pushed with `reads` must drop its read watch through `UnwatchReadback`
  (`UpdatePageWatchers<false, true>`) on every exit: retire, cancel, and stale mode's `ReleaseOutside`. A path
  that calls `WatchReadback<false>` for it leaves pages read-protected: endless faults or a hang.
- `readback_published` (under `readback_mutex`) gets an entry at every retire in stale mode and on the budget-full
  immediate path, whatever the switch. It only grows, by one entry per distinct readback address [code] (about
  140 first copies in a 20-minute session of level reloads).
- Only first copies wait. Most appear at level loads; a new small target mid-level waits once (up to a GPU frame).

## Interactions

- C5's spans cover these entries (they stay `watched`), and the CPU fault paths keep the lock, so the read wait
  still happens with C5 on.
- tiny-target-readback stays off: its per-frame copy-back was the Deck frame cost noted in `1097324`.
- M1 / pool-zero (`GOW3_POOL_ZERO_FOREIGN`) is a separate leak; it zeroed 0 pages through a broken Hall load.

## Evidence

- Captures `hall_frame13615.rdc` (broken) / `hall_frame30045.rdc` (clean), research-data (untracked): the teal
  is added by the reflection environment pass (`fs_0xd3caef1f45107cdd`); cubemap array and capture data identical;
  the brightness float is 0.0 broken, 65535.0 clean (tools: cube_array_dump.py, cube_array_sheet.py,
  draw_buffers.py).
- `hall-glitch-readback-log-0102.log`: every level load queues its 1x1 RGBA16F first copy at a new address
  (7 loads, 7 addresses); the GPU value for the Hall is 0.046783, while the game read leftover bytes.
- `hall-glitch-first-wait-0102.log` (`GOW3_READBACK_FIRST_WAIT=1`): 10 loads, each "read by the CPU, GPU not done
  yet: waiting", then the real value (Hall 0.046783). Verified by the user (2026-11-09, 01.02, 8 Deck switches on):
  several Hall loads, no broken lighting; corners darker, the map less flat and washed out than earlier "good" loads.
- Deck: not run with it on since `1097324`; expected cost only at loads (check the frame-time graph).

## History and pitfalls

- `db9b933` made it default on, `1097324` off again with tiny-target-readback ("frame cost on the Deck; did not fix
  the Mausoleum"). On Linux the Mausoleum was the pool leak (pool-kept-pages), so this switch changed nothing there
  and was filed as a dead end. The Windows Hall of Heroes / Mausoleum lighting was this stale first read.
- Lesson: a nondeterministic lighting fault that changes per level load points at a value computed once per load.
  `GOW3_READBACK_LOG` shows whether the read comes before the publish.
