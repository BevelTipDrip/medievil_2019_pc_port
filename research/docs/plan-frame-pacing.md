# Plan: frame pacing (I2): stage B's waits and syscalls

Status: started 2026-10-18 (the user: "let's get moving on these performance fixes"). Parent:
[plan-roadmap.md](plan-roadmap.md); issue I2 in [plan-baseline-issues.md](plan-baseline-issues.md). Read
[gpu-threading-reference.md](gpu-threading-reference.md) before changing anything. Upstream PRs later (the user,
2026-10-18), structured per [pr-checklist.md](pr-checklist.md).

## Baseline (build `bc0e1d6`, race fix L5)

- Stats: T-L5 session 1, Graveyard idle, 4K boot, console, pipeline on
  ([data/t-l5-pipe-on-2026-10-18.log](data/t-l5-pipe-on-2026-10-18.log)): FPS per 5 s min 34.8 / p10 91.4 /
  median 102.3 / p90 133.4; frame-time stddev median 1.95 ms; p99 median 13.53 ms, max 39.68 ms.
- Profile (mixed session, build `684cd8f`, `research-data\samples\i2-profile-4k-2`): stage B 15.3% in
  `RunEventWriteEop` → `ProcessDownloadImages` yielding (6.7% in `WaitHostCopies`' loop, the rest likely
  `Gow3Copy::WaitAsync`); ~12% image churn `ResolveOverlap` → `FreeImage` → `VirtualProtect`; 5% flip GPU wait.

## Lead R: render-target readbacks (first)

Read (2026-10-18):
- `ProcessDownloadImages` (texture_cache.cpp:161-239, deferred-readback mode, the default `deferred_readback=1`) runs
  at EOP on stage B (liverpool.cpp:268, 348) and from stage A (vk_rasterizer.cpp:3147, drains first). It starts with an
  unconditional `scheduler.WaitHostCopies()` ("upload workers must finish reading RAM before these pages become
  inaccessible"), then records a download per queued image and write-protects its pages (`WatchReadback<true>`)
  until the copy is retired.
- What gets queued: storage images (`FindTexture`, :1030) and, with `readback_linear_images`, **every linear render
  target used** (`FindRenderTarget`, :1057-1060). MediEvil's profile sets `GOW3_READBACK_LINEAR=1` (run.py:179,
  game_profiles.py: "otherwise surfaces turn black"). So render targets are likely read back every frame whether the
  CPU reads them or not.
- The wait cannot simply be skipped when the queue is empty (likely rarely empty), and must not move under
  `readback_mutex`: a copy worker faulting on a watched page resolves through `ResolveReadbacks`, which takes that
  mutex (deadlock).

Measure first (P0): `GOW3_PERF_DIAG=1` prints every 5 s the readback profile (queued, completed, waited, canceled,
pending MiB) and the busiest images with **copies vs CPU accesses while pending**, plus readback sources and waits.
Images copied every frame with 0 CPU accesses are wasted readbacks.

Fix directions, by what P0 shows:
1. Read back only images the CPU actually touches (e.g. learn per image address: after N copies without a CPU
   access, stop queuing it; a later CPU fault on its pages downloads on demand through the existing path). Keeps the
   "surfaces turn black" fix for images the game reads.
2. If the waits are mostly `Gow3Copy::WaitAsync`: wait only for copies whose sources overlap the pages being
   protected (Bloodborne's targeted host-copy waits, plan-gpu-threading.md #1, `daa7165`).
3. Move readback recording off stage B (Bloodborne's readback queue, #2).

## Lead C: texture-cache churn (second)

`BindResources` → `ResolveOverlap` → `FreeImage` → `SignalImpl::Protect` → `VirtualProtect` (~11% of stage B).
Measure which images are freed / re-created per frame and why the overlap repeats; compare Bloodborne's collector
fixes (#4).

## Lead F: flip waits on stage B (third)

`sceVideoOutSubmitEopFlip` → `SubmitFlip` → `Semaphore::Wait` (5%).

## Stages (one change per launch, behind a switch defaulting to the old path)

| Stage | What | Verify |
|---|---|---|
| P0 | Measurement: Graveyard route, `-Samples`, `GOW3_PERF_DIAG=1`, no code change | Readback profile: images, copies vs CPU accesses; stage B profile in gameplay |
| R1 | Per P0 (likely: skip readbacks of images the CPU never touches) | Stage B EOP share, FPS / p99 vs baseline; no black surfaces (the user) |
| C1x | Texture churn, per measurement | `VirtualProtect` share, FPS / p99 |
