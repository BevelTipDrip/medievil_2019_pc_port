---
id: extra-env-file
title: run.py: extra-env.txt in the data folder sets test environment variables on every start
area: launcher
status: active
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: a15b5cb0f4
files: run.py
symbols: load_extra_env, EXTRA_ENV_PREFIXES, GOW3_DERIVED
switches:
tests:
docs: plan-linux-deck.md#test-settings-by-file-2026-11-03-the-users-request-a15b5cb, study-deck-patch-audit.md#3-test-plan, study-deck-build-optimization.md#1-what-the-build-does-today-code-bin
depends_on:
related: deck-preset, perf-patches-ini, presentation-settings, U2, deck-test-tooling
verified: 2026-11-07
---
## What it does

At the start of `main`, before anything reads the environment, `run.py` calls `load_extra_env` on
`<GOW3_DATA_DIR>/extra-env.txt`. The file has one `KEY=VALUE` per line, with `#` comments. Keys must start with one
of `EXTRA_ENV_PREFIXES` (`GOW3_`, `RADV_`, `MESA_`, `AMD_`, `VK_`, `SDL_`) and contain only letters, digits and `_`;
other lines are skipped and logged as "ignored". Valid lines overwrite the inherited environment and are logged in
one line, `Extra environment (path): K=V ...`. It works however the game is started (Steam, the launcher, `--play`),
so Deck tests need no Steam launch options: Claude writes the file over SSH (`deck.sh env`). Absent file: nothing
happens. Same path on Windows (the port's data folder). On `deck-lab` only (not on `lab`).

## Invariants

- The file must be applied first and must overwrite (`env[key] = ...`). Most of what follows uses `setdefault` or
  `if key not in env`, so its keys win over medievil.ini: `perf_environment`, `present_environment`, the
  patch-derived `GOW3_DMEM_MB` / `GOW3_VBLANK_HZ`, and the launcher's `DECK_SWITCHES` (which reach run.py as
  inherited environment).
- Exceptions, where run.py overwrites a key the file set [code]: `GOW3_DEFERRED_READBACK`, `GOW3_STALE_READBACK` and
  `GOW3_ASYNC_SHADERS` (`env.update(graphics_options.environment(...))` from medievil.ini), `GOW3_PATCHES_ONLY=1`
  (when patched), `GOW3_UE4_PRESET_CVARS`, `GOW3_UE4_SETRES_CVAR`, `GOW3_DERIVED` and `GOW3_RESTART_COMMAND`; and
  `GOW3_UE4_SETRES` / `GOW3_INTERNAL_WIDTH` / `GOW3_INTERNAL_HEIGHT` are removed when the eboot has no `r.SetRes`.
  A readback or async-shader test must change medievil.ini, not this file.
- Only the listed prefixes. `LD_PRELOAD`, `LLVM_PROFILE_FILE` and `GLIBC_TUNABLES` cannot be set this way
  (study-deck-build-optimization.md notes this for the PGO / allocator steps). Widening the list widens what a stray
  file can do on every start.
- Read from the code, not seen in a run: on an in-game "Apply and restart", run.py loads the file and then pops the
  previous start's `GOW3_DERIVED` keys. A key added to the file between the start and the restart is therefore lost
  for that restart if the previous start had derived it (for example `GOW3_VBLANK_HZ`). The reverse also holds:
  the restarted run.py inherits the game's environment, so a key removed from the file between the start and an
  in-game restart stays set for that restart. A fresh start is not affected.

## Interactions

- The baseline trap (study-deck-patch-audit, plan-linux-deck.md "Deck test order"): every Deck run since U2 had
  `GOW3_ARENA_HOST_VISIBLE=0` in extra-env.txt. That key is in `DECK_SWITCHES` now, but any key left in the file
  silently changes the baseline of later runs. `deck.sh env` replaces the whole file, so every `env` line must carry
  the baseline keys.
- deck-preset / perf-patches-ini / presentation-settings: the file overrides all of them without touching the ini,
  and the launcher does not show it.

## Evidence

- Deployed in AppImage `e91d5b79` (`a15b5cb`). Native run 6 (`GOW3_UFFD=1`) was the first run set this way, and
  the log confirmed userfaultfd took effect ("GPU: memory tracking with userfaultfd write-protection").
- No measurement applies: it is a delivery mechanism.

## History and pitfalls

- The user asked for it so that Steam launch options stay plain (2026-11-03).
- Keys that stayed in the file: `GOW3_STAGE_B_SPIN=1` (2026-11-04, "the user keeps it on") and
  `GOW3_ARENA_HOST_VISIBLE=0`. On 2026-11-05 the flat-60 note in plan-linux-deck.md says "extra-env.txt still has
  test keys". Check `deck.sh status` before comparing any Deck run with another.
- The launcher's old free-text "extra_env" setting is ignored (`test_environment`). This file replaces it.
