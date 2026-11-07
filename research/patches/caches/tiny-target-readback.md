---
id: tiny-target-readback
title: Copy tiny tiled targets back to guest memory in their tiled layout (Mausoleum suspect; off by default)
area: caches
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: db9b933906, 892a801ef4, 1097324745
files: gpu/shadps4/video_core/texture_cache/texture_cache.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.h
symbols: IsTinyTarget, tiny_target_readback, TextureCache::RecordImageDownload, TileImage, TextureCache::FindRenderTarget, TextureCache::FindTexture, NonZeroTexels
switches: GOW3_TINY_TARGET_READBACK
docs: plan-linux-deck.md#hilltop-mausoleum-lighting-pool-pages-leaked-on-linux-2026-11-0607-fixed-7246f88--f74e1a1
depends_on: lineage-deferred-readbacks
related: pool-kept-pages, readback-first-wait, mausoleum-diagnostics, mausoleum-toggle-bisect
verified: 2026-11-07
---
## What it does

GPU-written colour targets and storage images are queued for a copy back to guest memory (`download_images`)
only when they are linear or at most 8 texels wide (`readback_linear_images`). With `GOW3_TINY_TARGET_READBACK=1`,
`FindRenderTarget` / `FindTexture` also queue tiled targets of at most 64 texels and 64 KiB (`IsTinyTarget`).
`RecordImageDownload` then writes them back in the guest's tiled layout through the tiling shader
(`tile_manager.TileImage`, every mip, the whole `guest_size`), so a CPU read finds each texel where the game looks.
`892a801` adds a `GOW3_READBACK_LOG` dump of such a first copy, linear and tiled (`NonZeroTexels`).

## Invariants

- Off by default (`1097324`): only `GOW3_TINY_TARGET_READBACK=1` turns it on (static read). `db9b933` had shipped it
  on.
- When on, the copy must cover the image's whole guest range in the guest's tiling. A partial or linear copy over
  a tiled range would corrupt the bytes that the next upload of that range reads back.
- Depth targets and images without a guest address are never queued by the tiny rule (`IsTinyTarget`).
- Both queue sites are inside `readback_linear_images` (`GOW3_READBACK_LINEAR`, default off in
  `emulator_settings.h`; run.py sets it to 1 for MediEvil). Without it no target is copied back at all, and
  `GOW3_TINY_TARGET_READBACK=1` does nothing.
- Assumption: `TileImage` reproduces the PS4 tiled layout for these tile modes. This was never checked against a
  PS4 dump.

## Interactions

- Built as a suspect for the Hilltop Mausoleum lighting (UE4's 9x1 RGBA16F sky light SH target, read by the CPU
  once at a level's load). The real cause was leaked pool pages, fixed by pool-kept-pages.
- It goes with readback-first-wait (same commits): one gets the bytes back, the other makes a first read wait for
  them.
- It feeds the deferred readback queue (64 copies, 64 MiB). More copies a frame mean more queue pressure and more
  `scheduler.Finish()` budget waits [inferred].

## Evidence

- `1097324`: "frame cost on the Deck; did not fix the Mausoleum". The header comment says "it copies many small
  per-frame targets back every frame (a large frame cost on the Deck)", and the GPU's own sky light result there
  is NaN. The docs give no frame-time numbers.
- plan-linux-deck lists it under "Dead ends on the way (each ruled out by a run; kept as opt-in diagnostics)".

## History and pitfalls

- Part of the Deck suspect hunt (about 15 launches, per the pool-kept-pages card), which ran until tooling
  showed the images held never-written guest memory.
- Worth retrying only if a game's CPU really reads a tiny tiled GPU target and gets stale bytes (check with
  `GOW3_READBACK_LOG` / `GOW3_SKIPPED_TARGET_LOG`). Even then, limit it to the targets the CPU reads (watch faults)
  instead of every tiny target every frame.
