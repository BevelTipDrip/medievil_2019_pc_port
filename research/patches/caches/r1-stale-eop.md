---
id: R1-stale-eop
title: In stale-readback mode, skip the host copy wait at end of pipe (reverted: caused V2 black torch boxes)
area: caches
status: reverted
platforms: all
default_windows: n/a
default_deck: n/a
origin: ours
class: C
commits: e384aa121a, 6b86c75f8a
files: gpu/shadps4/video_core/texture_cache/texture_cache.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.h
symbols: TextureCache::ProcessDownloadImages, skip_copy_wait, WaitHostCopies
switches: GOW3_STALE_SKIP_COPY_WAIT
docs: plan-frame-pacing.md#r1-ab-2026-10-18-a-1146-1153-b-1154-1201-launched-by-claude-with-the-users-go, plan-frame-pacing.md#r1-reverted-performance-degrades-with-new-content-2026-10-18-1440-1510, fix-inventory.md#10-tried-and-dropped, plan-baseline-issues.md#v2-fire--torches-broken-more-box-shadows-the-user-2026-10-18-run-perfdiag-heavy
depends_on: lineage-deferred-readbacks
related: W2, P2, R1-readback
verified: 2026-11-07
---
## What it does

(Code removed in `6b86c75`.) `GOW3_STALE_SKIP_COPY_WAIT=1`, honoured only with `GOW3_STALE_READBACK=1`, made
`TextureCache::ProcessDownloadImages` skip its `scheduler.WaitHostCopies()` at every end of pipe. The reasoning was
that stale mode only write-protects pending readback pages, so upload workers can go on reading them, and the next
`SubmitExecution` still waits for the copies it reads. In the plans it is "R1"; plan-roadmap.md calls it L6.

## Invariants

- (Kept by the revert.) Before an end of pipe retires readbacks into guest memory (`RetireReadbacks` ->
  `TryWriteBacking`) and protects the pages of new downloads (`WatchReadback`), every host copy that reads those
  ranges must be finished. Without that the torches render as black flickering boxes (V2).
- Today that wait is W2's targeted form: `ProcessDownloadImages` collects the `pending_readbacks` ranges and the
  `download_images` ranges under the locks, then `WaitHostCopiesFor` each one, outside the locks.
  `GOW3_EOP_TARGETED=0` (also `perf_gpu_data_no_waits=0`) or toggle bit 60 (`IdleEopSkip`) falls back to the full
  `WaitHostCopies`. The only skip left is P2's opt-in `GOW3_IDLE_EOP_SKIP=1`, and only when there is no image to
  download and no copy ready to retire. Stale mode does not make the wait optional.
- The assumption that failed (class C): "a copy thread reading a page while `RetireReadbacks` writes it only sees
  newer GPU output". The docs never showed the exact race. They only showed that the visual bug comes and goes
  with the switch.

## Interactions

- W2 is the kept answer to the same cost. It waits only for copies that read the ranges this end of pipe writes or
  protects, instead of every host copy. P2 (skipping the wait when there is no readback work) is a related opt-in.
- It needed stale readbacks (lineage-deferred-readbacks). V2a (stale off) and V2b (stale on, R1 off) both drew
  correct torches, so stale mode alone is safe.
- Not the same as R1-readback (submit after a readback copy) or R1-region-index. The letter R1 was reused.

## Evidence

- Profile before it: ~15% of stage B (15.3-16.7%) sat in `RunEventWriteEop` -> `ProcessDownloadImages` ->
  `WaitHostCopies`, to read back ~5 tiny exposure images a frame.
- R1 A/B (2026-10-18, `r1-a-stale.log` / `r1-b-skip.log`, both stale on): the wait (19.7% of stage B) was gone and
  stage B idled 34.5% instead of 24.3%. FPS at matching draw counts did not change (~1,150 draws: ~144 vs ~136-138;
  ~2,800-3,000: 88-91 vs 82-93). The user, run B: no additional visual issues.
- V2 (the user, later the same day, `perfdiag-heavy`): "It looks like fire/torches were broken". Then: "black
  transparent flickering boxes". The bisect (V2a stale off, V2b stale on with R1 off) gave correct torches both
  times. The cause was R1, reverted in `6b86c75`.

## History and pitfalls

- plan-frame-pacing: "Things not to retry: skipping `ProcessDownloadImages`' host-copy wait, even in stale mode."
  fix-inventory section 10: "Do not retry".
- The first A/B found no visual issue, because V2 shows only near torches in some areas. Check fire and
  translucency views before keeping any change to readback ordering.
- To retry, the wait would have to cover every copy that reads the retired or newly protected ranges (W2 already
  does this). On top of that, a gain would have to show at the same draw counts. With stage B no longer the limit
  after the skip, none did.
