---
id: game-update-0102
title: MediEvil 01.02 (US update): every 01.00-only hook mapped by masked instruction search; t.MaxFPS 30/60 lifted
area: ue4
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: M
commits: 1e8150f109, 3cf6d69ceb
files: src/hzb_hook.h, src/level_hook.h, src/probe.c, src/runtime_extra.c, src/runtime_sse4a.c, patches/MediEvil.xml, scripts/patches.py, scripts/ue4_override.py, run.py, tests/test_medievil_patches.py
symbols: medievil_hzb_builds, medievil_hzb_find, medievil_level_builds, medievil_level_find, ue4_cvars, GAMES, game_app_version, eboot_matches, MAX_FPS_COMMANDS, without_max_fps, GAME_INSTANCE
switches:
tests: T-0102-1
docs: plan-0102.md#the-ports-0100-only-sites-mapped, plan-0102.md#first-launch-by-the-user-2026-10-31-1430-launcher-userlast_runlog, plan-0102.md#t-0102-1-2026-10-31-1431-1453-launched-by-claude-with-the-users-go-build-3cf6d69--nostats-log-datat-0102-1log, fix-inventory.md#7-medievil-specific-ue4-config-eboot-patches-hooks, fix-inventory.md#caveats-in-the-evidence
depends_on: upstream-0102-eu-support
related: hzb-remap, movie-suspend, I1, live-graphics-options, effects-resolution, preshadows-live, single-particle-pass, particle-flip-guard, finer-occlusion-culling, engine-cap-cvars
---
## What it does

The US update PKG's eboot (sha256 `fe2f88ba`) is upstream's 01.02 code: all ten 4K-pool sites match, so it joins
the 01.02 hash tuple in `GAMES`. Every 01.00-only site of this port was found in 01.02 by a masked instruction search
(rel targets and RIP displacements masked, one match): HZB remap and level hook site tables (01.02 at +0x2c80 for the
HZB), 18 live cvars (`ue4_cvars[].at[1]`, by name and registration), 01.02 blocks for the display-metrics and
boot-size patches, and 8 SSE4a INSERTQ sites (Intel CPUs). `3cf6d69`: 01.02's game instance runs `t.MaxFPS 30` /
`t.MaxFPS 60`. Above 60 FPS both are written as `t.MaxFPS 00` (`without_max_fps`). It also chose the HZB remap by
the installed version.

## Invariants

- Each hook uses the build whose signatures all match. Calls are checked by their target because rel32 differs
  between builds. An unknown build gets no hook. A new hook must carry a row for both builds.
- Built-in XML patches apply only to an eboot whose sha256 is in its version's `GAMES` entry (`eboot_matches`).
  Lines with `Original` bytes are also checked per line.
- `run.py` picks the HZB profile with `game_profile(title, game_app_version(game))`. Before `3cf6d69` it took the
  first profile (01.00), so 01.02 never got the remap.
- The `t.MaxFPS` edit keeps the length ("30"/"60" -> "00") and runs only when `fps > 60`. It reuses
  `files[GAME_INSTANCE]` when the voice-language edit already took that asset (both edit the same file).
- 01.02 code moves are not uniform: the HZB code sits at +0x2c80, the particle code and SSE4a sites at
  +0x2dd0 / +0x2dc0, and data at +0x40. Map each site; do not apply one offset to all.

## Interactions

- upstream-0102-eu-support is the base (per-version patch profiles, NpToolkit2 no-ops, the r.SetRes slot finder).
- hzb-remap, movie-suspend (level hook), live-graphics-options / effects-resolution / preshadows-live (cvar rows),
  I1 (boot patches): each now covers both builds. single-particle-pass and particle-flip-guard added their 01.02
  rows later with the same method.
- engine-cap-cvars: on 01.02 `t.MaxFPS 60`, issued by the game, did cap the game at 60. A `t.MaxFPS` from the pak
  config had no effect on 01.00 (T-Z4).

## Evidence

- First launch (2026-10-31): "60 FPS only" and "No HZB remap", both fixed in `3cf6d69`.
- T-0102-1: every hook reports "game build 01.02", no faults. The user: "the best performance I've seen". The
  graveyard's first-pan spikes were gone, heavy-area 0.1% lows stayed around 200 (RTSS 222 avg, 0.1% 198-222 in the
  crypt), and there were almost no stutters outside level loads.
- Caveat: that run had the game on its own NVMe (E:). Drive and version were not separated. An A/B with 01.00 on E:
  would settle it.

## History and pitfalls

- Mapping was checked by compiling `hzb_hook.h` / `level_hook.h` against both images (scratchpad `hooks_check.c`).
- 01.02's DefaultInput.ini maps R3 to "RecenterCamera", the debug menu's button. The debug menu on 01.02 is still
  unchecked.
- The comparison with upstream 1.3.8 as shipped (0.1% low 211.9 here vs 44.7 there) also mixes versions: upstream
  applies its patches only on 01.00.
