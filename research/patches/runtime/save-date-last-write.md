---
id: save-date-last-write
title: A save's date is its last write (param.bin or any save file), so MediEvil saves into the slot it loaded
area: runtime
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: M
commits: 0061d7b06c
files: src/runtime_savedata.c
symbols: read_param
switches:
tests: T-SV2, T-SV3
docs: study-save-slots.md#t-sv3-the-date-fix-2026-11-10-launched-by-claude-with-the-users-go-log-datasave-trace-3-0102log
depends_on:
related: save-trace
checked:
---
## What it does

`read_param` dates a save by the newest of param.bin's file time and the file times of the files in the save
directory, as the PS4 dates a save by its last write. MediEvil saves into the newest slot of a time-ordered
`sceSaveDataDirNameSearch` and makes a loaded slot the newest by writing its file back at load (no SetParam). With
param.bin's time alone, the next save after loading any slot but the newest went into the newest other slot,
replacing its contents.

## Invariants

- The date that sorts `save_search` (key 3) and the one the Load list shows (`save_label`, param byte 1288) both come
  from `read_param`: they must stay the same value, or the list order and the game's save target disagree.
- Only regular files directly in `<dir>` count (MediEvil writes one, `ue4savegame.ps4.sav`). The meta path must end
  in `.sce_sys`, or only param.bin's time is used.
- File times are the slot order: a copy of `user/savedata` without times (plain `cp -r`) gives every slot one date
  and the game an arbitrary save target. Copies keep times (`cp -rp`, `shutil.copytree`).

## Interactions

- save-trace: the log lines that found it and show each save's target.
- `scripts/save_backup.py` (launcher backups) copies with `copytree`, which keeps times.

## Evidence

- T-SV2 (before the fix): loaded `@MediEvil0007`; both saves went into `@MediEvil0006`; slot 7 byte-identical after.
- T-SV3 (PC, 01.02): verified. Loaded `@MediEvil0002`; it came first in the next search and both saves went into it;
  every other slot byte-identical. The user: "It looks like it worked."

## History and pitfalls

- 2026-11-09 a restore without times gave six slots one date; the 2026-11-10 repair set param.bin times from the
  time stored inside param.bin. With this patch the list dates become each slot's last write (a load counts).
- The user's first reading was list reordering plus equal dates; T-SV2 showed a real overwrite with distinct dates.
