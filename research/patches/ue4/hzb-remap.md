---
id: hzb-remap
title: HZB occlusion through a remap hook: serve the newest finished HZB readback by (PrimitiveId, CustomIndex)
area: ue4
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: M
commits: aceb5a4b42, dfdc5fe419, 02af1a5e60, 57dc5c4be3, 1e657ca5ef, 1e8150f109, 3cf6d69ceb
files: gpu/shim/medievil_hzb.cpp, src/hzb_hook.h, src/probe.c, gpu/shadps4/video_core/texture_cache/texture_cache.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.h, gpu/shadps4/video_core/texture_cache/readback_queue.h, scripts/ue4_override.py, run.py
symbols: medievil_hzb_builds, medievil_hzb_find, install_hzb_remap, medievil_hzb_map, medievil_hzb_submit, medievil_hzb_init, CollectKeys, Sync, ServeVisible, SnapshotReadback, ReadbackSnapshot
switches: GOW3_HZB_REMAP, perf_hzb_occlusion
tests: T-HZ1, T-HZ2b, T-HZ3, T-HZ4, T-HZ4f, N-22
docs: plan-stage-b-split.md#t-hz1-ue4s-hzb-occlusion-2026-10-20-0022-0027-launched-by-claude-with-the-users-go-build-852ec13-log-datat-hz1-hzblog, plan-stage-b-split.md#t-hz3-hzb--precise-readbacks--r1-2026-10-20-0037-0041-launched-by-claude-with-the-users-go-build-9dea0af-log-datat-hz3-readback-submitlog, plan-stage-b-split.md#t-hz4-hzb-remap-2026-10-20-2000-2148-launched-by-claude-with-the-users-go-builds-aceb5a4-53ade79-dfdc5fe-logs-datat-hz4log, study-hzb-double-buffer.md, fix-inventory.md#7-medievil-specific-ue4-config-eboot-patches-hooks, study-live-switches.md#perf_hzb_occlusion-restart--medium
depends_on: micro-mip-targets, lineage-deferred-readbacks
supersedes: O2-O3
related: occlusion-queries-off, R4a, finer-occlusion-culling, R1-readback, game-update-0102, perf-patches-ini
verified: 2026-11-07
---
## What it does

UE4 tests primitive bounds against its HZB on the GPU into a 256x256 results texture and reads it the next frame by
per-frame indices; the port's GPU runs ~a frame behind, so stale readbacks gave another frame's results under this
frame's indices (random culling, T-HZ1) and precise ones waited (T-HZ3). A hook on the one call of
`FHZBOcclusionTester::MapResults` (`medievil_hzb_map`) serves the newest **finished** copy
(`TextureCache::SnapshotReadback`, no wait), remapped by (PrimitiveId, CustomIndex) from the producing frame's
indices to the ones read now (`CollectKeys` walks the occlusion history TSet). v2 (`dfdc5fe`) skips the game's map
after one `Sync` and numbers copies by a second hook before `Submit` (`medievil_hzb_submit`). The pak then writes
`r.AllowOcclusionQueries=1`, `r.HZBOcclusion=1`, `r.NumBufferedOcclusionQueries=3`.

## Invariants

- Hooks only when `GOW3_HZB_REMAP=1`, the GPU is on (`gpu_enabled`) and `medievil_hzb_find` matches every
  signature (01.00 / 01.02 rows); run.py sets it only when the eboot's sha256 is the profile's (`eboot_matches`).
  The pak's cvars follow the env alone (`hzb=env.get('GOW3_HZB_REMAP') == '1'`), so hooks and cvars must never be
  decided apart: HZB on without the remap is T-HZ1's wrong culling. A `GOW3_HZB_REMAP=1` forced on an eboot whose
  signatures fail does exactly that (the hook logs "unavailable", the pak still turns HZB on).
- Once synced, a failure serves "all visible" (`ServeVisible`, the game's rule for missing results): a history walk
  out of bounds (`CollectKeys`), no snapshot (`SnapshotReadback` `!ok`), drift. Two paths fall back to the game's
  own `MapResults` instead (its wait, and the guest copy's results): no free tester slot (8, `testers`), and a `Sync`
  that cannot snapshot, which with `deferred_readback=0` (`SnapshotReadback` needs `deferred_readbacks`) is every
  frame. Keys whose producing copy left the 6-frame ring read visible (age "none").
- One results copy per Submit: `snap.pushed > seq` (an extra download) or a gap over 64 forces a resync. A change that
  downloads the results range at other times shows as `drift` in the 5 s line (`GOW3_FRAME_STATS`).
- The struct offsets (`TesterInViewState` 0x170, `ResultsBuffer` 0x40, history 0x58-byte entries, keys at 0x00 /
  0x44) are UE 4.21 as built in this eboot; the hash walk caps steps at 2^22 and indices at 65,536.
- `r.NumBufferedOcclusionQueries` stays 3 (T-HZ4e: the remaining queries are read 3 frames late, no spin).
- Results are only as good as the HZB: its mips 5-10 were never rendered until N-22 (micro-mip-targets).

## Interactions

- depends on micro-mip-targets: without it every box tested at mips 5-10 read visible (N-22: ~17% of draws culled
  before; 30-67% of tested keys hidden after).
- depends on lineage-deferred-readbacks (the readback queue and `tracked_readbacks`; `GOW3_DEFERRED_READBACK=1`,
  ini default 1, Deck preset 1); R1-readback only acts with precise readbacks (T-HZ3) and is not needed by the remap.
- perf-patches-ini: `perf_hzb_occlusion=0` -> `GOW3_HZB_REMAP=0` (launcher, in-game menu; restart).
- supersedes O2-O3 (real query results: correct but cost more than they saved while panning, T-O3c).
- occlusion-queries-off is the fallback where the remap cannot run (`perf_hzb_occlusion=0`, unknown eboot).
- finer-occlusion-culling builds the HZB at the scene's size; the remap serves those results unchanged.

## Evidence

- T-HZ4 (v1): culling correct, no pop-in; heavy view 160-178 FPS, p99 6.7-7.3 ms (queries off ~130, T-HZ3 ~125); the
  user: average 175-180, 1% lows ~145. T-HZ4d (v2): the game's Sleep(0) map spin (~430,000/s) gone, 0 drift.
- N-22 + the remap (t-hzb2): hidden 30-67% of keys, 186-237 FPS vs 162-215; no missing geometry in screenshots.
- Deck (plan-linux-deck.md): "HZB occlusion exonerated: worse without it".

## History and pitfalls

- T-HZ1 (stale readbacks): the user, "Dan himself gets culled, sometimes the ground or enemies". T-HZ2b (precise):
  "Nothing is failing to draw now" but 110-116 FPS; T-HZ3 (+R1): ~125, HZB rejected until the remap.
- Default since `57dc5c4` (2026-10-20); 01.02 rows and run.py's version choice in `1e8150f` / `3cf6d69`. Not live
  (study: install the hooks always and switch `r.AllowOcclusionQueries`; not built, by the user's scope).
