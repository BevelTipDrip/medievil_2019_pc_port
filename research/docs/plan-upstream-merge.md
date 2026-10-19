# Plan: merge upstream 1.3.3-1.3.5 into `lab`, then the next work items

Status: plan, 2026-10-19. Nothing merged yet. Parent: [START-HERE.md](START-HERE.md). The PR plan this feeds:
[patches-and-pr-proposal.md](patches-and-pr-proposal.md).

`lab` forked from upstream 1.3.2 (`783b25a`, = our `main`). `upstream/main` is now `d4c9cf4` (1.3.5): 5 commits,
27 files, +806 / -76. A dry run (`git merge-tree --write-tree lab upstream/main`) conflicts in 5 files; everything
else merges by itself.

## 1. Where upstream and `lab` overlap

| Area | Upstream | `lab` | Verdict |
|---|---|---|---|
| Volumes drawn as 2D slices (stutter) | 1.3.4: 3D image served through a 2D array view, **sampled** through it; 1.3.5: only with `VK_EXT_image_2d_view_of_3d` (AMD Polaris crash, issue #18); `GOW3_VOLUME_VIEWS=0` | **V3** (`1f18217`, bit 58): the same idea; the 2D array view is **attachment-only**, the volume is sampled through its own 3D view | Same fix, two variants. Ours *should* be valid without the extension (spec reading, **not tested** on AMD/Intel, validation run pending). Upstream's has a correct partial-slice clear (`ClearVolumeSlices`); ours clears the whole volume and warns (the warning never appeared in any of our logs). |
| 4K menus (issue #11 / I1) | 1.3.3: keep the live `r.SetRes`, report `GOW3_INTERNAL_WIDTH/HEIGHT` at the output size; only for the analysed 01.00 eboot | **I1** (`ebf85f4`, `GOW3_UE4_BOOT_4K=1`): report 3840x2160 **and** patch the game's 4K display-metrics branch on, no live `r.SetRes` | Same bug. Our research (plan-baseline-issues.md "I1 research") found the game's display metrics take the VideoOut size only when `r.PS4Allow4kOutput` is set; upstream reports the size without that patch. **Unknown which works here**: test upstream's path once (see 3). |
| Stage A / stage B recording race (crash, freeze; issues #2, #12) | 1.3.4: `SyncWithDrawRecorder` in `EndRendering` and the recording paths (stage A waits for stage B, except under the texture cache lock); 1.3.5: the wait comes before `RecordData` | **L5** (`bc0e1d6`, from the Bloodborne port): stage A drains stage B before every queued command (`ProcessCommands`) | Same race from two sides: ours removes the cause (commands queued by guest threads), theirs guards the recording calls. Keep both. |
| Torch-into-view stutter / texture re-uploads | 1.3.4: texture GC limits follow the driver's memory budget (8 GB GPUs evicted textures in use) | **C3** (`1024224` + `d9949cb`, bit 61): targets made where a guest unmap freed images skip the first upload | Different causes, no overlap: our collector never ran (~4 GB used vs trigger 7,077 / pressure 17,430 MiB). Both stay. C3 merges without conflict. |
| Stencil (shadows) | 1.3.4: stencil proxy check (from the Uncharted port: GPU page fault) | **V4** (`769de12` + `423efc7`, bit 59): shader writes to the stencil plane copied into the image | Different fixes; neither replaces the other. Test together (box shadows, torches). |

New upstream items with no `lab` counterpart (take them as they are): EU version (CUSA12982), all languages,
VSync frame limit at 60 Hz, F12 screenshots, launcher update check, level-3 crash diagnostics, debug-menu resolution
buttons, GC budget rule + slot reallocation fixes + deferred sampler destruction (Uncharted port), 7.1 → stereo
downmix, 60 FPS default, "Precise" readbacks removed from the launcher, `GOW3_IMAGE_STATS`.

## 2. The five conflicts and how to resolve them

1. **`vk_runtime.cpp`**: our `CopyBufferToStencil` (V4) and upstream's `ClearVolumeSlices` were added at the same
   spot. Textual only: keep both functions.
2. **`vk_scheduler.cpp`**: `EndRendering` gained `std::source_location caller` on our side (render-pass-end stats)
   and `SyncWithDrawRecorder(__builtin_return_address(0))` on theirs. Keep both: our signature, their call first.
3. **`image_view.cpp`**: both restrict the usage of 2D (array) views of 3D images. Keep **ours** (attachment-only
   always, with the feedback-loop bit) unless the validation run says that bit is invalid there; it covers
   upstream's no-extension case.
4. **`texture_cache.cpp`**, three hunks, all V3: (a) the overlap test that returns the 3D image for a slice target:
   keep our `IsVolumeSliceTarget` (toggle bit 58) and add upstream's `GOW3_VOLUME_VIEWS=0` env as a second off
   switch, or drop theirs; (b) `ExpandImage` diagnostics: keep ours (`CACHE_STAT` + expand sites) and theirs
   (`image_stats`) side by side; (c) the "picked up wrong" check: keep `IsVolumeSliceTarget`. Then take upstream's
   `ClearVolumeSlices` path for render-target clears of volume slices (correct partial clears) in place of our
   whole-volume clear, since it renders through an attachment view, which our usage rule allows.
5. **`run.py`** (I1 vs #11): keep both paths: `GOW3_UE4_BOOT_4K=1` → our 4K boot (patch, no live `r.SetRes`);
   otherwise upstream's (live `r.SetRes` + internal size at output, analysed eboot only, the 1080p fallback message).

**Merge-time check: the D2 lock and upstream's recorder sync.** `DownloadMemory` holds `cpu_writes_mutex` while it
records a copy (`CopyBuffer` → `EndRendering`). After the merge `EndRendering` calls `SyncWithDrawRecorder`, which on
stage A waits for stage B to drain; stage B takes the same mutex in `ProcessCpuWrites` before each draw. Today stage
B is already idle there (L5 drains before the queued command that runs the download), so no wait happens, but the
merged code must not be able to wait for stage B while holding that mutex: drain the draw pipe before taking it in
`DownloadMemory` (or release it around the copy), and check with a long session.

## 3. Test after the merge (one session, the user's go)

On a branch `merge-1.3.5` (never on `lab` directly until it passes):
- **Boot / menus**: 4K boot (`run-game.ps1` default), menus and debug menu navigable. Then one launch **without**
  `GOW3_UE4_BOOT_4K` (upstream's #11 path): are the menus navigable? Decides whether I1 is offered at all.
- **Stability**: 15+ minutes, Graveyard and the heavy view; no `Scheduler: recorded while the draw recorder had
  queued draws` lines (upstream's diagnostic), no crash.
- **Visuals**: fog / lighting volumes (V3 vs theirs), shadows (V4), a torch coming into view (C3 + upstream's slot
  reallocation), no broken effects (D2).
- **Numbers**: heavy view FPS, p99, stddev, slow frames (`GOW3_SLOW_FRAMES=1.2`), against T-D2b: like for like now
  that the 4K boot is default.
- **Validation layer**: a short `-Validation` session with V3 on (pending since V3).

## 4. Next work items, in order

1. **D2 live A/B (T-D2c)**, on the current `lab` (before the merge, so it isolates D2): heavy view, bit 63 off and
   on with the toggle file working and the 4K boot. Now a prerequisite for PR 4, not optional. Then re-check D2
   in the merge session.
2. **Occlusion queries off (T-O1)**, a cvar test, any time: `GOW3_UE4_CVARS='r.AllowOcclusionQueries=0'`. Expect
   fewer draws per frame (the query boxes go), the render thread's occlusion wait gone, and nothing culled less than
   now (the port answers every query "visible").
3. **Merge upstream** (sections 2-3).
4. **Real occlusion query results (O2)**, on the merged code: it records queries in stage B in draw order and reads
   results back when the GPU has them, in the scheduler and recording paths upstream just changed. Built before the
   merge it would conflict with them.
5. Intro movie decoding after the skip (T-U1), then PRs per patches-and-pr-proposal.md.
