---
id: upstream-ue4-setres
title: Upstream live r.SetRes for native output (1.3.0; per-eboot slot 1ae17cd), kept as the fallback
area: ue4
status: active
platforms: all
default_windows: mixed
default_deck: mixed
origin: upstream 1.3.0
class: M
commits:
files: src/runtime_extra.c, scripts/ue4_override.py, scripts/graphics_options.py, run.py, gpu/shim/gow3_overlay.cpp
symbols: ue4_setres_slot, ue4_set_cvar, ue4_setres, runtime_ue4_setres_poll, runtime_ue4_request_setres, setres_slot, SETRES_CODE, output_environment
switches: GOW3_UE4_SETRES, GOW3_UE4_SETRES_CVAR
tests:
docs: fix-inventory.md#from-upstream-medievil-133-138-bc160201983-and-contributors-merged-into-lab, study-boot-resolution.md#4-fixing-the-live-rsetres-instead, study-boot-resolution.md#summary
depends_on: upstream-0102-eu-support
related: I1, upstream-4k-framebuffer-pool, live-graphics-options, effects-resolution, preshadows-live, single-particle-pass
checked: 2026-11-13
---
## What it does

The base PS4 renderer makes its buffers at the display size at start, and only a live `r.SetRes` resizes them. From
`scePadReadState`, `runtime_ue4_setres_poll` sets the game's r.SetRes as `FSystemResolution::RequestResolutionChange`
does: `ue4_set_cvar` calls `Set(value, GetFlags() & 0xff000000)` through the vtable. Sources: upstream 1.3.0
`3a16e0e`, PR #29 `c167454` (repeats at 2/5/10/20 s), `1ae17cd` (slot found per eboot). 1.3.3 (#11) also reports the
output size as the display. Here it is the fallback: I1 boots at the output size, and `output_environment` sets
`GOW3_UE4_SETRES` only with `GOW3_UE4_BOOT_SIZE=0`. The in-game "Game resolution now" buttons used
`runtime_ue4_request_setres` until 2026-11-13; the menu's Frame buffer size is restart-only since (the user;
plan-settings-menu.md 9.3 lists what a live switch does not get), so `runtime_ue4_request_setres` has no caller and is
kept unused.
Every live cvar shares `ue4_set_cvar`.

## Invariants

- Flags are kept as RequestResolutionChange keeps them (`& 0xff000000`). The object's vtable and both methods
  (+0x20, +0x68) must lie inside the image (`lo..lo+0x6000000`), or nothing is called.
- The slot comes only from `GOW3_UE4_SETRES_CVAR` (run.py, `setres_slot`: the pattern found exactly once). 0 or out
  of range means no live resolution change, logged once.
- The start value is requested at 120 / 300 / 600 / 1200 pad reads. Any menu request stops the repeats; one made
  before 120 reads is dropped, not deferred.
- Runs on the game's pad thread. Values are at most 23 characters (UTF-16 buffer of 24).

## Interactions

- I1 replaced it as the default: a live `r.SetRes` leaves the menus' gamepad hit-test area at 1080p (UE4's display
  metrics are cached at boot).
- upstream-0102-eu-support: the slot finder and the 01.02 slot.
- live-graphics-options, effects-resolution, preshadows-live, single-particle-pass: the same `ue4_set_cvar` and
  poll (cvars from `reads>=600`).
- upstream-4k-framebuffer-pool: a live switch to 4K needs the 512 MB pool.

## Evidence

- Upstream `1ae17cd`: 4K native output on US 01.00 gives 3840x2160 screenshots with the four requests and no hitches.
  `c167454`: verified on Linux 01.02.
- In this port the fallback start path (`GOW3_UE4_BOOT_SIZE=0`) has not been verified in game since I1. The live
  menu buttons were not written up as a separate check.

## History and pitfalls

- `c167454` stopped presetting `r.setres` in the override pak because the runtime's live request then saw the same
  value and did nothing. This port's `ue4_override.apply` still writes `r.setres = output` for non-1080p outputs.
  That is inert at boot (study-boot-resolution) but may turn the fallback's start request into a no-op [inferred,
  not tested].
- PR #29's fixed slot broke 01.00 (see upstream-0102-eu-support).
