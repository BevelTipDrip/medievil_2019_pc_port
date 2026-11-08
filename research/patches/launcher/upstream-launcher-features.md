---
id: upstream-launcher-features
title: Upstream EU version, languages, voice language, AA option (ue4_aa), 60 FPS default, portable mode
area: launcher
status: active
platforms: all
default_windows: on
default_deck: on
origin: upstream 1.3.3-1.3.8
class: M
commits:
files: launcher/medievil_launcher.py, scripts/ue4_override.py, scripts/patches.py, scripts/graphics_options.py, src/runtime_services.c
symbols: LANGUAGES, VOICE_LANGUAGES, AA_CHOICES, PORTABLE, CONFIG_DIR, ANTI_ALIASING, voice_language, VOICE_FUNCTION_SIZE, GAMES, ENGINE_FPS, DEFAULTS
switches:
tests: N-27
docs: fix-inventory.md#from-upstream-medievil-133-138-bc160201983-and-contributors-merged-into-lab, study-upstream-commits.md#semantic-hazards-in-files-that-auto-merge-mt
depends_on:
related: upstream-0102-eu-support, port-identity, live-render-scale, vsync-control, game-update-0102
verified: 2026-11-07
---
## What it does

Upstream bc160201983's launcher features, merged into `lab` (1.3.3-1.3.5 in `b7b78fb`, 1.3.6-1.3.8 in `e308c4c`):
- 1.3.3 `8a70032`: the EU version (CUSA12982 shares the profiles in `GAMES`, patches.py) and every game language
  (`LANGUAGES` -> `GOW3_LANGUAGE`, read by `language()` in runtime_services.c).
- 1.3.4 `4d72031`: 60 FPS as the default engine frame rate (`DEFAULTS`, `INI_DEFAULTS`), with 120 / 240 labelled
  experimental upstream. Ours labels them plain "120" / "240" (`ENGINE_FPS_CHOICES`, `ENGINE_CAP_CHOICES`).
- 1.3.6 `8cf5cc5`: the AA option (`ue4_aa`: taa / fxaa / off -> `ANTI_ALIASING` -> `r.PostProcessAAQuality`) and
  portable mode (`PORTABLE`: a `portable.txt` beside the launcher keeps `CONFIG_DIR` in `launcher-data`).
- 1.3.8 `424f3ea`: the voice language (`ue4_voice`, `voice_language`). This patches
  `GetWWiseLanguageFromLanguageID` in the override pak so that text and voices can differ.

## Invariants

- Ours left out upstream's one-time `engine_fps` reset to 60 (`FPS_RESET_KEY` / `fps_default_60`). It would have
  dropped the user's 240 once (study-upstream-commits.md H3). A later merge must not bring it back.
  `graphics_options.py` has no such key today.
- `ue4_aa` applies only at or above a 100% render scale (`aa_quality = None if scale < 100`). Temporal upsampling
  needs temporal AA, so below 100% it is always TAA. live-render-scale's restart rule depends on this.
- `voice_language` edits exactly `VOICE_FUNCTION_SIZE` (138) bytes and needs the function's end marker and the
  `JumpIfNot` exactly once, or it raises (game version check). A voice name must fit in `English(US)`'s 11
  characters plus NUL, because the remaining bytes are padded with `EX_Nothing`. All 15 entries of
  `VOICE_LANGUAGES` fit.
- `PORTABLE` can never apply inside the read-only AppImage (no `portable.txt` there).
- 01.00 and 01.02, US and EU, must all resolve to a profile in `GAMES`, or run.py starts without patches.

## Interactions

- upstream-0102-eu-support / game-update-0102: the eboot side of the EU and 01.02 builds (per-version patch
  profiles, NpToolkit2 no-ops, hook maps). This card is the launcher and the pak.
- port-identity replaced upstream's version and update source. vsync-control replaced upstream's VSync checkbox.
  Ours kept I1's boot at the output size over upstream's live `r.SetRes` (upstream-ue4-setres).

## Evidence

- Merge check of 1.3.8 (plan-work-queue.md, "The user's order", item 1): heavy normal frames 4.45 / 4.48 ms on the
  merge vs 4.55 ms on the pre-merge `lab`. N-27 (flip trace): worst 15-s fight 1% low 103 on the merge vs 99-105 on
  `lab`; no regression.
- The features themselves are upstream's and were not re-measured here.

## History and pitfalls

- Upstream's launcher `INI_DEFAULTS` has `stale_readback='0'` while `DEFAULTS` in graphics_options.py (what run.py uses for a missing key) has `'1'`. Ours inherits
  both, so a launcher save writes 0 while a start without the key uses 1 (read from the code; owned by
  lineage-deferred-readbacks).
- Upstream's update check pointed at its own releases until port-identity (H4).
