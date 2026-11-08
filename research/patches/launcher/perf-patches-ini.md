---
id: perf-patches-ini
title: Performance patches as launcher toggles, stored in medievil.ini and shared with the in-game menu
area: launcher
status: active
platforms: all
default_windows: on
default_deck: mixed
origin: ours
class: G
commits: 27ec51e36d, 1e657ca5ef, e9d86996a4
files: scripts/graphics_options.py, launcher/medievil_launcher.py, run.py, gpu/shim/gow3_settings.cpp, gpu/shim/gow3_settings.h, gpu/shim/gow3_overlay.cpp, src/runtime_memory.c, tests/test_launcher_controls.py, tests/test_graphics_settings.cpp
symbols: PERF_SWITCHES, PERF_DEFAULT_OFF, perf_environment, PERF_PATCHES, PerfPatch, PerfKeys, PerfLive, PerfOn, perf_off, RestartChanges, PerfPatchesSection, runtime_memory_set_perf, TakeBaseline, Baseline, Serialize, collect
switches: perf_hzb_occlusion, perf_no_decal_prepass, perf_particle_single_pass, perf_small_buffers, perf_texture_memo, perf_lockfree_memory, perf_cheap_frees, perf_gpu_data_no_waits, perf_4k_memory, perf_lean_barriers, perf_lean_binding, perf_ring_reuse
tests: T-H3b, T-0102-4
docs: plan-stage-b-split.md#launcher-performance-patch-toggles-2026-10-21, plan-stage-b-split.md#in-game-menu-and-the-launcher-share-the-settings-2026-10-22, study-live-switches.md#summary, study-live-switches.md#found-on-the-way
depends_on:
related: hzb-remap, no-decal-prepass, single-particle-pass, S2, texset-region-stamps, lockfree-region-reads, L2, decommit-keep-access, D3, gpu-write-twins, E1, W2, 4k-memory, lean-barriers, bulk-descriptor-copy, sampler-memo, ring-reuse, deck-preset, extra-env-file, frame-stats-lines, launcher-graphics-page
verified: 2026-11-07
---
## What it does

Twelve `perf_*` keys in `medievil.ini`, one per patch bundle, shown as checkboxes in launcher > Performance
(`PERF_PATCHES`) and in the in-game menu > Performance (`PerfPatchesSection`). On every start `run.py` calls
`perf_environment`: a key set to `0` adds that bundle's off variables from `PERF_SWITCHES` (for example
`perf_gpu_data_no_waits=0` sets four variables). A missing key means on, except `PERF_DEFAULT_OFF`
(`perf_particle_single_pass`). Six bundles switch live in the menu on `deck-lab` (`PerfLive`; five on `lab`, where
the particle pass is still a restart change: it went live in `d20f010`); the others count as restart changes
(`RestartChanges`). Since `e9d8699` the in-game `Save` writes only the keys changed in the menu, in place.
`27ec51e` also stopped the launcher from setting `GOW3_FRAME_STATS=1` by default (frame-stats-lines).

## Invariants

- One bundle, four tables in step: `PERF_SWITCHES` (graphics_options.py) equals the variables in `PERF_PATCHES`
  (`test_performance_patches_are_ini_flags_with_switches` checks this). `PerfKeys` follows the `PerfPatch` order,
  the overlay's `titles` / `hints` have `PerfCount` entries, and `perf_off` in `Load` holds the first variable and
  value of each bundle. No test checks `perf_off`: a wrong entry makes the menu show the wrong state for a test
  start, and toggling the row then writes that wrong state to the ini.
- `Values::perf[12]` / `startup_perf[12]` are literal 12, not `PerfCount`. There is no static_assert, unlike
  `ue4_live`. A 13th patch needs both arrays and their initialisers changed.
- The in-game defaults are all `true`. With no `perf_particle_single_pass` key in the ini, the menu shows the
  particle pass off only because run.py sets `GOW3_PARTICLE_FIXED_STEP=1` and `Load` reads it through `perf_off`.
  Every start must go through `run.py` (AppRun `--run` and the launcher both do).
- run.py applies the perf variables with `setdefault`, so any variable already set wins over the ini. That covers
  extra-env.txt (loaded first) and `run-game.ps1 -Env` (`DECK_SWITCHES` holds no perf variable).
- The perf variables are not added to `GOW3_DERIVED`, unlike the present ones. An in-game "Apply and restart"
  inherits the previous start's environment, so a bundle that started off (its variable set) stays off after the
  user turns it back on in the menu and restarts: the ini says `1`, the inherited variable still says off, and the
  menu shows it off again. Turning one off by restart works. A fresh start fixes it [code, not seen in a run].
- Small buffers: the menu's state is on only when `GOW3_STREAM_CLEAN` starts with `1`. run.py sets it only for UE4
  profile games, so the ini key cannot turn S2 on for another game.
- Live rows act at once: `PerfOn` is a relaxed load per draw (ring reuse, lean binding); the buffer cache loads
  `perf[PerfSmallBuffers]` itself (`buffer_cache.cpp`). `runtime_memory_set_perf(optimistic, keep_access)` and `runtime_ue4_particle_single_pass`
  run each time the section draws. "Lock-free memory map lookups" live switches only `GOW3_OPTIMISTIC_CLAMP`;
  `GOW3_REGION_BUCKETS` (L2) needs a restart. The texture memo and GPU data without waits must stay restart-only,
  because they build state at start (memo validation rule, twins, readback queue chosen with the device).
- `Save` diffs `Serialize` against `Baseline`. `TakeBaseline` runs at the end of `Load`, after the environment
  overrides, so a test variable is never written unless the user changes that row.

## Interactions

- Each key fronts another card's switches: HZB occlusion (hzb-remap), decal prepass (no-decal-prepass), particle
  pass (single-particle-pass), small buffers (S2), texture memo (texset-region-stamps), lock-free lookups
  (lockfree-region-reads + L2), cheap frees (decommit-keep-access), GPU data without waits (D3, gpu-write-twins,
  E1, W2), 4K memory (4k-memory), lean barriers (lean-barriers), cheaper binding (bulk-descriptor-copy +
  sampler-memo), ring reuse (ring-reuse).
- deck-preset writes every key (`perf_4k_memory=0`, `perf_particle_single_pass=1`), hence `default_deck: mixed`.
  `perf_4k_memory=0` sets `GOW3_PS4_MEMORY=1`, which only `output_environment` reads, for outputs above 1080p: at
  the preset's 1280x720 it changes nothing [code].
- Some keys act only where run.py turns the patch on at all: HZB occlusion needs an eboot that matches a known
  profile (`GOW3_HZB_REMAP=1` is set only then), small buffers a UE4 profile game.
- Cheap frees acts on Windows only: `GOW3_DECOMMIT_KEEP_ACCESS` is read in `host_keep_view`, inside the `_WIN32`
  branch of runtime_memory.c (kept views). On Linux / the Deck the row and the key change nothing [code].
- extra-env-file overrides any key here without changing the ini (the baseline trap).

## Evidence

- The user's T-H3b result made the single pass a default on 2026-10-21. T-0102-4 made it default off: with
  `perf_particle_single_pass=0`, the user said "the slow torch is gone ... and so is the crypt ember / golden
  stream".

## History and pitfalls

- Before `e9d8699`, the menu rewrote every key from memory. Test overrides such as frames queued and readback modes
  became permanent that way, and about 40 keys were added, God of War III's among them.
- The launcher's `collect` still writes every `INI_DEFAULTS` key on Play, so an existing default is frozen into the
  ini at the first launcher save. `8d85929` (particle pass off) has no migration: an install the launcher saved
  before 2026-11-01 keeps `perf_particle_single_pass=1`. This was read from the code, not seen in a run.
- The in-game "4K memory sizes" hint still says "more graphics memory and texture pool at 4K native". `c1db412`
  fixed only the launcher text (the pool cvar is gone; it acts above 1080p), see launcher-chore.
