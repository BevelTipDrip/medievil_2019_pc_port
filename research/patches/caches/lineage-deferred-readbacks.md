---
id: lineage-deferred-readbacks
title: Deferred GPU readbacks and stale readbacks (image downloads published when the GPU passes them; reads see the last copy)
area: caches
status: active
platforms: all
default_windows: mixed
default_deck: on
origin: upstream 1.3.2
class: C
commits: 7c1db31, cc3563a, 8600b28
files: gpu/shadps4/video_core/texture_cache/texture_cache.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.h, gpu/shim/core/emulator_settings.h, scripts/graphics_options.py, launcher/medievil_launcher.py, run.py
symbols: TextureCache::ProcessDownloadImages, TextureCache::ResolveReadbacks, TextureCache::RetireReadbacks, WatchReadback, UnwatchReadback, RecordImageDownload, SnapshotReadback, ReleaseOutside, deferred_readbacks, stale_readbacks, readback_linear_images, GetReadbacksMode, IsReadbackLinearImagesEnabled, INI_DEFAULTS, DECK_PRESET
switches: GOW3_DEFERRED_READBACK, GOW3_STALE_READBACK, GOW3_READBACK_LINEAR, GOW3_READBACKS
tests:
docs: plan-frame-pacing.md#the-launchers-gpu-data-settings-read-2026-10-18-the-user-asked, plan-frame-pacing.md#p0-results-2026-10-18-1122-1138-launched-by-claude-with-the-users-go, plan-linux-deck.md#proton-runs-3-4-2026-11-03-settings-changed-by-the-user-while-exploring
depends_on:
related: W2, P2, R1-stale-eop, R1-readback, C1-semaphore-poll, C5, hzb-remap, tiny-target-readback, readback-first-wait, gpu-write-subresources, upstream-texture-gc, D3, uffd
checked: 2026-11-07
---
## What it does

With `GOW3_READBACK_LINEAR=1` (run.py's MediEvil profile: "linear image readbacks fix its corrupted textures";
plan doc: "otherwise surfaces turn black"), every GPU-written linear render target or storage image is queued in
`download_images` (tiled ones only up to 8 texels wide, or tiny targets with tiny-target-readback). Deferred (`GOW3_DEFERRED_READBACK=1`):
`ProcessDownloadImages` at an end of pipe records every copy, watches its pages and pushes it on `pending_readbacks`.
`RetireReadbacks` publishes it (`TryWriteBacking`) once the GPU passes its tick. A CPU touch waits for that copy
only (`ResolveReadbacks`); without deferred, `scheduler.Finish()` per batch. Stale (`GOW3_STALE_READBACK=1`):
only writes are watched, so CPU reads see the copy published last (a frame late). `GOW3_READBACKS` (0 / 1
Relaxed / 2 Precise) is the separate buffer readback mode.

## Invariants

- `stale_readbacks` needs `deferred_readbacks` (`deferred_readbacks && env`). Writes always wait: no byte lost.
- The C++ default with no variable is off for both (`value[0] == '1'`). Launches get them from `run.py`, which
  `env.update`s them from medievil.ini (`graphics_options.environment`): a value set in the environment before
  `run.py` is replaced by the ini's.
- Pages are watched before the copy is pushed, and `PublishReadbackSpans` runs before `has_pending_readbacks` is
  set, so C5's lock-free pre-check never skips a range a copy covers. Fault paths keep the lock.
- Before pages are watched, the host copies reading them must finish (W2's `WaitHostCopiesFor`, or
  `WaitHostCopies`). Skipping that wait broke the torches (R1-stale-eop, V2).
- Byte rule (stale, `ReleaseOutside`, 64 KB): a CPU write next to a copy's bytes on a shared page does not wait.
- Over the pending limit (`CanFit`): `Finish()` and a synchronous publish.
- Class C: stale reads assume the game tolerates one-frame-old GPU data. A first copy has nothing published yet,
  so its reads see bytes no copy wrote (readback-first-wait was the test).
- `GOW3_READBACKS=2` (Precise) gives a black screen (#15): the launcher does not offer it. Relaxed is the default
  (FaceGen vertex explosions without it).

## Interactions

- W2, P2, R1-stale-eop, R1-readback, C1-semaphore-poll, C5 all change `ProcessDownloadImages` /
  `ResolveReadbacks` / `RetireReadbacks(polled)`.
- hzb-remap reads results via `SnapshotReadback`, which returns nothing with deferred readbacks off.
- tiny-target-readback and readback-first-wait add to `RecordImageDownload` / the first-copy watch.
- gpu-write-subresources, upstream-texture-gc: resolve readbacks before GPU writes / evictions.
- uffd: in stale mode only write watches remain (uffd's part), except a first copy's read watch with
  `GOW3_READBACK_FIRST_WAIT`; non-stale read watches stay mprotect.
- `default_windows` mixed: deferred is on. Stale is 0 in the launcher's `INI_DEFAULTS` (`ac900f8`, no reason
  given) but 1 in graphics_options' fallback and in the user's ini. `DECK_PRESET` sets both to 1.

## Evidence

- God of War III (`cc3563a`): stale mode 40.9 -> 66.5 FPS in its reference scene (not MediEvil).
- P0 (MediEvil, stale on): 575,820 copies queued and 0 waits; five tiny luminance-shaped images (8x8 to 1x1) made
  up almost all of them. `ProcessDownloadImages` was 16.7% of stage B, nearly all the host-copy wait.
- Deck Proton run 3, stale on: the user, "seemed to help a lot" (several changes at once, not a clean A/B).

## History and pitfalls

- Launcher default `stale_readback` 0 -> 1 (fix built `8600b28`, 2026-11-07, LANDMINES item 15): it had been 0 since upstream's
  import `ac900f8` (no reason given) while run.py and the Deck preset used 1. Inis that have the key are unchanged.
- From the God of War III port (`7c1db31`, `cc3563a`: launcher checkbox on), present at the fork base. Stale
  CPU reads do not fault, so "CPU accesses while pending" counts writes only (P0). Each watch / unwatch is an
  mprotect (`VirtualProtect`), part of stage B's protect cost.
