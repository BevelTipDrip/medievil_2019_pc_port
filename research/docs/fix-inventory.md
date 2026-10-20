# Fix inventory: meaningful and novel (2026-10-20)

Parent: [START-HERE.md](START-HERE.md). Evidence per fix: [plan-frame-pacing.md](plan-frame-pacing.md),
[plan-baseline-issues.md](plan-baseline-issues.md), [plan-stage-b-split.md](plan-stage-b-split.md); PR plan:
[patches-and-pr-proposal.md](patches-and-pr-proposal.md).

**Meaningful** = a test with numbers or a visible bug fixed, confirmed by the user. **Novel** = not taken from the
Bloodborne port (deadinside28, `C:\Users\Ty\MediEvil\bloodborne_pc`), the original MediEvil port (bc160201983,
`upstream`) or shadPS4. Checked against the commit messages and the Bloodborne source (its rwlock registry walks a
list, its address-space hint is a different mechanism, its dirty registers are scanned bit by bit, it has no kept
pool views, stencil-plane copy, undefined-target or volume-view code).

## Meaningful and novel (11)

| Fix | What it did | Evidence | Commits |
|---|---|---|---|
| R2 + R3 | rwlock handles through a hash set; the memory pool's free-page search starts at a per-chunk hint | heavy view +17% median FPS, p99 -13% (T-R23) | `cf9e201`, `2097554` |
| P1 | dirty register blocks scanned by word | ~+8%, "1% lows are improved" (T-P1) | `5cd037e` |
| V3 | volumes (lighting volumes, colour LUT) served through 2D array views instead of rebuilt | map ~70 -> ~200 FPS (T-V3); upstream 1.3.4 shipped the same idea a day later, with sampled views (its 1.3.5 AMD fallback) | `1f18217` |
| M1 + M1b | memory pool views kept across a decommit (Windows) | the 2-5 s kernel trim episodes gone (T-M1, T-M1b) | `40a416a`, `3c5704e` |
| V4 | shader writes to a depth buffer's stencil plane copied into the image | the box shadows gone (T-V4, RenderDoc) | `769de12`, `423efc7` |
| C3 | targets made where a guest unmap freed images skip the first upload | torch-into-view spikes 19 -> 7-12 ms, "Massive improvement" (T-C3b) | `1024224`, `d9949cb` |
| D2 + D2b | guest libc writes into shader-written memory intercepted before the write (page bitmap, backing view, exact bytes queued for the GPU copy); bytes written no longer GPU-only | downloads 27/s -> 0, heavy-view stddev 1.1-1.5 -> 0.32-0.48 ms, p99 10.9-13 -> 8.6-9.5 ms (T-D2b, T-D2c, T-D2d) | `1bc2dda`, `42a4943` |
| I1 | 4K native boots at 4K (the game's own 4K display-metrics branch, found in the binary) | 4K menus navigable; upstream 1.3.3 fixed #11 later another way (both kept) | `ebf85f4` |
| E1 + W2 | end-of-shader fences deferred like end-of-pipe ones; the end-of-pipe readback check waits only for overlapping copies | stage B's ordered tasks 0.52 -> 0.23 us per packet, ~5% more packets/s (T-E1) | `cb1da6c`, `3b30fe9`, `852ec13` |
| Queries off (finding) | the port answers occlusion queries "visible" (shadPS4), so their box draws cost time and cull nothing | ~150 fewer draws a frame, +~5% (T-O1) | `c8615ea` (default) |
| 4K memory (finding) | 7,104 MB guest memory, 4,600 MB GPU heap, 1,600 MB streaming pool at 4K | small pans stop re-streaming (T-S2) | `c8615ea` (default) |

**Partly novel:** D2's idea, exact CPU bytes into the GPU's copy instead of a readback, follows the Bloodborne port's
`NotePreciseUpload`; Bloodborne learns of the write afterwards (no page protection), ours intercepts it before the
write in the libc imports, with its own bitmap and ordering.

## Meaningful, derived (credit in commits and PRs)

| Fix | Source | Evidence |
|---|---|---|
| L5 drain before every queued command (crash / freeze race) | Bloodborne 0.3 `4d4d208` | no crash or freeze since (T-L5 and later) |
| D3 downloads on a readback queue | Bloodborne 0.3 (`GpuWriteTicks`, `FastReadback`) | each download waits only for its write (T-D3b) |
| GPU write twins | Bloodborne 0.4 (`BB_GPU_WRITE_TWINS`) | light view stddev ~0.56 -> 0.06 ms, ~200 FPS 1% lows (T-T1) |

## Novel, not (yet) meaningful

- **O2 / O3** real occlusion results (completion thread instead of Bloodborne's GPU-side write; early submission):
  correct, up to 68% hidden, but cost more than they save while panning (T-O3c). Off.
- **R1** readback submitted when recorded: only acts with precise readbacks (HZB, T-HZ3). Inactive.
- **L2** region caches answering during writes: lock waits halved, no FPS change (T-L2).
- **D1** unmap discard: never triggered in MediEvil (T-D1b).
- Diagnostics: slow-frame report, download causes and sites, render-pass ends by caller, host copy waits by caller,
  AvPlayer trace, upload rates, occlusion latency split.

## Derived, not meaningful

- **B1** targeted WRITE_DATA waits (Bloodborne): skipped 1 wait a frame (T-B1).
- **GOW3_SECTIONS** (Bloodborne's `BB_SECTIONS`): diagnostics.

## Upstream 1.3.3-1.3.5, merged on `merge-1.3.5` (`b7b78fb`, to be tested)

Kept as they are: EU version, languages, VSync limit at 60 Hz, screenshots, update notice, the level-3 crash fix
(`SyncWithDrawRecorder`, complements L5), texture GC on the driver budget, slot reallocation fixes, deferred sampler
destruction, stencil proxy check, 7.1 -> stereo downmix, 60 FPS default, exact volume-slice clears
(`ClearVolumeSlices`). Ours kept over theirs: V3's view rule (attachment-only, no AMD extension needed; theirs kept for
other 3D views), I1's 4K boot as the default with theirs as the fallback. To check in the test session: the 60 FPS
default (the user plays at 240), and the VSync frame-limit change.
