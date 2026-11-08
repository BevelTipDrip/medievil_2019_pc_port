---
id: 4k-memory
title: 4K memory sizes: 7,104 MB guest memory and the 512 MB framebuffer pool above 1080p (perf_4k_memory)
area: ue4
status: active
platforms: all
default_windows: on
default_deck: off
origin: ours
class: M
commits: c8615eaab3, 1e657ca5ef, 601aec623b, cbb3e91d24, 9753e2f70d, c1db412306
files: scripts/graphics_options.py, scripts/ue4_override.py, scripts/patches.py, run.py, src/runtime_memory.c, launcher/medievil_launcher.py
symbols: output_environment, POOL_PATCH, PERF_SWITCHES, patch_requirements, pool_size_bytes
switches: perf_4k_memory, GOW3_PS4_MEMORY, GOW3_DMEM_MB
tests: T-R3
docs: fix-inventory.md#7-medievil-specific-ue4-config-eboot-patches-hooks, fix-inventory.md#caveats-in-the-evidence, plan-frame-pacing.md#panning-stall-anatomy-2026-10-19-pan-1etl-no-launch, study-live-switches.md#found-on-the-way
depends_on: upstream-4k-framebuffer-pool
related: I1, pool-kept-pages, perf-patches-ini, mausoleum-toggle-bisect, deck-preset
checked: 2026-11-07
---
## What it does

For outputs above 1920x1080 (by area) `output_environment` forces the 512 MB framebuffer-pool patch (already on by
default at every size: `isEnabled="true"`, the Patches page) and, unless `GOW3_PS4_MEMORY=1`, sets
`GOW3_DMEM_MB=7104` (2 GiB over the retail 5,056 MiB, 1.5 GiB over the 5,568 the pool patch's note asks for).
The game allocates the framebuffer pool (512 MiB) and then takes **all remaining direct memory as one memory pool**
(5,056 -> 6,592 MiB), committing Garlic or Onion pages from it, so `GOW3_DMEM_MB` is the GPU-memory knob. `perf_4k_memory=0` sets `GOW3_PS4_MEMORY=1` (PERF_SWITCHES). Restart only.

## Invariants

- `pool_size_bytes` (runtime_memory.c) accepts 5,056-16,384 MiB and falls back to 5,056 otherwise; it is read once.
- The `setdefault` in `output_environment` runs before run.py's `patch_requirements`, which only fills
  `GOW3_DMEM_MB` when unset: the 7,104 wins over the pool note's 5,568; with `GOW3_PS4_MEMORY=1` the note's 5,568
  applies (pool 5,056 MiB, the PS4's).
- At or below 1080p this patch does nothing: `GOW3_PS4_MEMORY` is read only in `output_environment` (and the
  in-game menu's display of the switch, gow3_settings.cpp `perf_off`). The Deck preset's 1280x720 therefore runs the
  stock sizes with the switch on or off: the default-on pool patch's note gives 5,568 MiB, the 512 MiB framebuffer
  pool plus a 5,301,534,720-byte (~5,056 MiB) memory pool (`data/deck-baseline-1-ab.log`).
- No memory cvars in the pak: `r.GarlicHeapSizeMB` exists in neither eboot, and `r.Streaming.PoolSize` from
  `[SystemSettings]` (SetBySystemSettingsIni) loses to the PS4 device profile's 2000 (SetByDeviceProfile; read
  statically, fix-inventory Caveats, not confirmed in game). Do not re-add them as if they sized anything (comment
  in `ue4_override.apply`).
- Assumption (class M): the game sizes its pool from `sceKernelGetDirectMemorySize` at boot (no live change).

## Interactions

- upstream-4k-framebuffer-pool: the crash fix at 4K (pool patch + its DMEM note) is upstream's; this card adds the
  forced selection above 1080p and the extra 1.5 GiB (7,104 instead of the note's 5,568).
- I1: the same `output_environment` decides boot size and memory; 1800p got the sizes in `cbb3e91`, every size
  above 1080p by area in `9753e2f`.
- pool-kept-pages: a smaller pool means more page reuse. The Deck showed the leak at the stock pool, but that came
  from its 720p output, not from the preset's `perf_4k_memory=0` (inert at 720p, see Invariants); the
  pool-kept-pages card's "GOW3_PS4_MEMORY (Deck preset) means more reuse" overstates it.
- perf-patches-ini: the launcher / menu toggle (`27ec51e`, `1e657ca`); deck-preset sets `perf_4k_memory=0`.
- mausoleum-toggle-bisect: 4K memory was one of the suspects ruled out.

## Evidence

- S2 (2026-10-19, plan-frame-pacing.md "S2 result"; fix-inventory calls it T-S2, not the stage-b T-S2): the game took the memory (pool +6,912,147,456
  bytes); the user: small back-and-forth pans no longer re-trigger the stutter, a larger pan away and back does. The
  stalls were the same asset groups with both pools: region streaming, not the texture pool; the run also had the
  two inert cvars, so the gain (if any) came from `GOW3_DMEM_MB` (unverified).
- 2026-11-03 (plan-work-queue.md): 4K run without the two cvars, pool +6,592 MiB as before, the 237 FPS cap held,
  ~6 GB VRAM (the user).
- T-R3 (1800p): DMEM 7104, pool +6.6 GB, the user "everything seemed to work as expected".

## History and pitfalls

- Started as test-script defaults (`168ba15`, run-game.ps1), moved to run.py in `c8615ea` (4K only), 1800p too in
  `cbb3e91`, every size above 1080p by area in `9753e2f`, a launcher toggle in `27ec51e`, the ini key in `1e657ca`. `c1db412` dropped
  `r.GarlicHeapSizeMB=4600` / `r.Streaming.PoolSize=1600` (inert, fix-inventory Caveats).
- "The PS4's 400-800 MB streaming pool thrashed at 4K" (old comments, T-S2) is wrong: the PS4 profile already had 2000.
