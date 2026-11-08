---
id: particle-bounds
title: GPU particle bounds: a 40 m box for GPU emitters without fixed bounds (eboot patch)
area: ue4
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: M
commits: 548db4c4b8, bc93780e3c
files: patches/MediEvil.xml, launcher/medievil_launcher.py, research/tools/uasset_props.py, tests/test_medievil_patches.py
symbols: PATCHES_AFTER_SEEN_LIST, game_patch_selection, game_patches_seen
switches:
tests: T-0102-5, T-0102-6
docs: plan-baseline-issues.md#v3-effects-wrong-blending-misplaced-missing-at-an-angle-the-user-2026-10-31-play-session-on-3cea271, plan-0102.md#t-0102-5-the-dust-culled-by-angle-2026-11-01-2016-renderdoc-build-09af284-log-datat-0102-5log, plan-0102.md#the-gpu-particle-bounds-patch-2026-11-01-offline-to-test-in-game, plan-0102.md#t-0102-6-2026-11-01-2338-2352-launched-by-claude-with-the-users-go-build-548db4c-log-datat-0102-6log, fix-inventory.md#7-medievil-specific-ue4-config-eboot-patches-hooks
depends_on:
related: single-particle-pass, particle-flip-guard, game-update-0102, launcher-graphics-page, renderdoc-capture
checked: 2026-11-07
---
## What it does

UE 4.21's `FGPUSpriteParticleEmitterInstance::UpdateBoundingBox` (01.00 0x23828b0, 01.02 0x2385680) gives a GPU
sprite emitter without `bUseFixedRelativeBoundingBox` a box of origin -1 / +1 (no CPU particle positions), so the
whole system is frustum-culled once its component origin leaves the view, with its particles still on screen (also
on a PS4). The eboot patch "GPU particle bounds" (`patches/MediEvil.xml`, 01.00 and 01.02, `isEnabled="true"`)
re-points the two `vmovss` rel32 loads of the shared -1.0 / 1.0 constants at -2000.0 / 2000.0 already in the
constant pool: a 40 m box around the emitter origin. `bc93780`: the launcher turns on a default patch its Patches
page has not shown before (`game_patches_seen`, `PATCHES_AFTER_SEEN_LIST`).

## Invariants

- The new rel32 displacements are relative to each instruction's end and point at constants that must stay
  -2000.0 / 2000.0 in that eboot's rodata; the lines have no `Original` bytes, so only the eboot sha256 check
  (`eboot_matches` in run.py's `patched` and in patches.py) guards them; `GOW3_FORCE_PATCHES` skips that check and
  would write them into any eboot. A new game build needs a new entry, not these bytes.
- Systems with a fixed box are unaffected (UE4 uses their box); only the 8 of 180 GPU systems without one change.
- Still culled as on a PS4: the whole 40 m box off screen, distance / LOD, sublevel unloads.
- The patch must stay out of the shared -1.0 / 1.0 constants themselves (other code reads them); it changes only
  the two loads.
- Launcher: a default patch the user switched off stays off; a selection saved before 2026-11-02 counts every patch
  but `PATCHES_AFTER_SEEN_LIST` as seen (`game_patch_selection`). A future default-on patch must be added there if
  older selections should get it.

## Interactions

- single-particle-pass / particle-flip-guard: the other two of T-0102-1's three effect issues (the slow torch and
  the crypt ember); this one is the culling by angle (plan-baseline-issues V3c; not the caches card V3).
- game-update-0102: the 01.02 entry uses its function and constants (0x3d2c3c0 / 0x3d2ccc0).
- Cost: up to 8 small systems drawn while their box, not their particles, is in view (not measured beyond T-0102-6).

## Evidence

- T-0102-5 (RenderDoc, build `09af284`): in the "no dust" capture the dust draw (eid 4286, 112 particles into the
  separate translucency target) is never issued: CPU-side culling. Occlusion ruled out (t-0102-3 ran with
  `r.AllowOcclusionQueries=0`). The user confirmed on PS4 videos (2026-11-01): the game's own behaviour.
- Survey (`research/tools/uasset_props.py`): 8 of 180 GPU systems lack fixed bounds (`PS_air_sparkles`, ...); the
  artists' boxes for similar ambient systems are +/-10000.
- T-0102-6 (build `548db4c`): verified by the user, the dust no longer vanishes with the camera; a couple of levels
  played, no performance issues.

## History and pitfalls

- First thought a port fault (the user remembered the dust as new); it is the shipped game's behaviour. The user:
  disappearing particles look like a glitch that never got patched, not the artists' intent.
- Rejected alternative: a fixed box for `PS_air_sparkles` in the override pak (needs new name-map entries, one asset).
- Before `bc93780` a launcher start kept the saved patch list, so the new default patch stayed off until ticked.
- Default on for the Deck as well: the Deck preset does not touch the patch selection
  (`data/deck-baseline-1-ab.log` patch line).
