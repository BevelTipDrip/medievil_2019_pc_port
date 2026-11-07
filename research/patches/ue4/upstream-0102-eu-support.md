---
id: upstream-0102-eu-support
title: Upstream 01.02 / EU support: per-version patch profiles, NpToolkit2 no-ops, r.SetRes slot per eboot
area: ue4
status: active
platforms: all
default_windows: on
default_deck: on
origin: upstream 1.3.7-1.3.8
class: M
commits:
files: scripts/patches.py, patches/MediEvil.xml, src/probe.c, scripts/ue4_override.py, run.py, tests/test_medievil_patches.py
symbols: GAMES, game_profile, game_app_version, eboot_matches, optional_import, optional_stub, setres_slot, SETRES_CODE, PAK_NAME, OLD_PAK_NAME
switches: GOW3_UE4_SETRES_CVAR
tests:
docs: fix-inventory.md#from-upstream-medievil-133-138-bc160201983-and-contributors-merged-into-lab, plan-0102.md#the-eboot, study-boot-resolution.md#5-other-versions
depends_on:
related: game-update-0102, upstream-4k-framebuffer-pool, upstream-ue4-setres, I1, upstream-launcher-features
---
## What it does

Upstream commits, merged into `lab` (`merge-1.3.8`): Porticus PR #26 `c7a9ef3`, PR #29 `c167454`, and BC160201983
`1ae17cd`. `scripts/patches.py` keeps one `GAMES` profile per app version (01.00, 01.02), chosen by the game's
APP_VER and gated by that version's eboot sha256. `MediEvil.xml` gets a 01.02 block of the 4K pool patch (same 11
instructions, only call displacements differ). The 01.02 disc's libSceNpToolkit2 C++ imports become logged no-ops
returning 0 (`optional_import`, `optional_stub`). The override pak is written lowercase (`pakchunk99-ps4_p.pak`).
`setres_slot` finds r.SetRes's IConsoleVariable* in each eboot from RequestResolutionChange's code.

## Invariants

- Built-in patches write only to an eboot whose sha256 is in its version's entry (`eboot_matches`). Another build of
  the same version has its code elsewhere, and byte writes there corrupt it. `GOW3_FORCE_PATCHES` skips the gate
  and falls back to the first version's addresses. Only `Original`-checked lines are safe then.
- Only `#libSceNpToolkit2` imports become no-ops. Every other unresolved import stays fatal (`unresolved`).
- `setres_slot` must match the code pattern exactly once. Otherwise native output stays 1080p, `GOW3_UE4_SETRES` is
  dropped, and the runtime's `ue4_setres_slot` logs "not located".
- The pak name must be lowercase. On case-sensitive file systems (Linux, Deck) the old `_P` name was never mounted:
  screen percentage, quality, the debug menu and the ammo edit were all silently off. `OLD_PAK_NAME` is deleted.

## Interactions

- game-update-0102 maps this port's own 01.00-only hooks onto 01.02 and adds the US PKG build `fe2f88ba` to the
  01.02 tuple (upstream listed `e50c9b36`, PR #26's dump, and `cd273b59`, the EU update).
- upstream-4k-framebuffer-pool: the patch this per-version profile first made work on 01.02.
- upstream-ue4-setres: uses the slot found here (`GOW3_UE4_SETRES_CVAR`).

## Evidence

- Upstream `c7a9ef3`: CUSA12982 01.02 dump, 11 writes applied, boots through the intro without an unsupported-import
  stop. `1ae17cd`: 4K native output on US 01.00 gives 3840x2160 screenshots. `c167454`: verified on Linux with 01.02
  ("Native output: r.SetRes 3840x2160", an F12 shot at 3840x2160).
- This port: every run on 01.02 since T-0102-1 boots with these profiles. Not tested separately here.

## History and pitfalls

- PR #29 moved the r.SetRes slot to 0x5c28368 for every build. That is right for 01.02 but broke 01.00 native output
  (0x5c28328). `1ae17cd` replaced the fixed address with the code search.
- Before `c167454`, run.py looked up `GAMES` without the APP_VER, so a 01.02 eboot was compared with 01.00's hash and
  every native output was refused.
- Upstream's 01.02 is EU-tested. On the EU 01.02 the debug menu "does not open" (R3 is RecenterCamera there).
