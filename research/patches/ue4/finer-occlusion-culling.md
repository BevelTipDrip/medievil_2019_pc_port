---
id: finer-occlusion-culling
title: Finer occlusion culling: HZB at the scene's size (eboot patch), default for scenes up to 1280x800
area: ue4
status: active
platforms: all
default_windows: mixed
default_deck: on
origin: ours
class: M
commits: 93d1a58cde, 8131ecba4b
files: patches/MediEvil.xml, scripts/graphics_options.py, scripts/patches.py, run.py, tests/test_graphics_options.py
symbols: HZB_FULL_SIZE_PATCH, HZB_FULL_SIZE_AUTO_MAX, hzb_full_size_environment, scene_size, original_mismatches
switches:
tests:
docs: impl-draw-reduction.md#1-finer-occlusion-culling-patchesmedievilxml, study-ue4-draw-reduction.md#4-full-resolution-hzb, plan-linux-deck.md#finer-occlusion-culling-on-the-deck-2026-11-04-appimage-a4250a2-baseline-settings--gow3_hzb_full_size1-mailbox-log-datadeck-hzb-1log, plan-linux-deck.md#decisions-after-the-culling-run-the-user-2026-11-04
depends_on: hzb-remap
related: I1, live-render-scale, mausoleum-toggle-bisect, game-update-0102
---
## What it does

UE 4.21's `BuildHZB` sizes the HZB's mip 0 as `2^(ceil(log2 W) - 1)` per axis and stretches the view over it. At 67%
of 720p (858x482) that gives a 512x256 pyramid, against 1024x512 at 100%. The patch nops the two `- 1` (`dec r14d`
-> 3-byte nop, `dec eax` -> `xchg ax, ax`; 01.00 0x15357ba / 0x1535812, 01.02 0x153843a / 0x1538492), so small scenes
get the full-size pyramid. `hzb_full_size_environment` adds it when the startup scene (output x render scale) is at
most 1280x800. `GOW3_HZB_FULL_SIZE=1` / `0` forces it on or off (run.py, extra-env.txt; Python-only, so not in
switches.tsv).

## Invariants

- The occlusion test reads the pyramid size from the view, so a larger mip 0 stays consistent (study section 4,
  static reading) [class C assumption].
- A binary patch, decided once at start from the startup scene: a live render scale change keeps the start's choice.
- In auto mode (switch unset) it is added even when the launcher's Patches page leaves it unticked. The page can
  only add it, so `GOW3_HZB_FULL_SIZE=0` is the only way off.
- Each line carries `Original` bytes (`41ffce`, `ffc8`). `original_mismatches` skips the patch on other bytes, even
  under `GOW3_FORCE_PATCHES` (which bypasses the sha256 gate). `Original` was added to `patches.py` for this patch.
- Keep the 1280x800 limit: at 4K the patch would build a 4096x4096 pyramid for nothing. Scenes above ~81% of 720p
  already get 1024x512.

## Interactions

- hzb-remap: the culling gain exists only with HZB occlusion on (`r.HZBOcclusion=1`, set by `ue4_override` with the
  remap). Where queries are off (occlusion-queries-off) only the HZB build cost changes [inferred].
- I1 / output size: the decision uses the same startup output size (`run.py` calls it after `output_environment`).
- Deck Optimized (1280x720 at 80% = 1024x576) gets it automatically.
- mausoleum-toggle-bisect: ruled out as a Hilltop Mausoleum suspect.

## Evidence

- Deck, `data/deck-hzb-1.log` (17 writes, heavy graveyard view, vs baseline 1): HZB hidden ~60 -> ~119 a frame,
  draws 1,241 -> ~1,151 (base pass 523 -> 460, velocity 74 -> 57, prepass 232 -> 222), FPS 74.2 -> ~77.6 (+4.6%), pacing
  stddev / p99 / worst 1.54 / 17.6 / 18.9 -> 1.42 / 16.4 / 17.3 ms. The user: "the spikes are lower amplitude on the
  graph". Decisions (2026-11-04): no pop-in over most of the graveyard level, GPU use visibly higher, so on by default
  for small scenes (`8131ecb`).
- PC: not run at small scenes. At 1080p and above the patch is not added.

## History and pitfalls

- The estimate was ~120-145 fewer draws. Measured ~90: two hidden objects save about one and a half draws.
- Risk: more culling means more exposure to the latent (3 frames late) result pop-in. Check fast turns and walking.
- A still finer HZB "may be tried much later" (the user).
