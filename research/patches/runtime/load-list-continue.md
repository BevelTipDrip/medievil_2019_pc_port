---
id: load-list-continue
title: Load list: "Continue" (the newest save) in place of the dead "New save" entry; "Slot N" on every save
area: runtime
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: M
commits:
files: src/runtime_services.c, src/runtime_savedata.c
symbols: save_open, save_status, save_label, slot_name, save_time, runtime_savedata_icon
switches:
tests: T-SV1, T-SV4
docs: study-save-slots.md#t-sv4-continue-and-slot-numbers-in-the-load-list-2026-11-10-launched-by-claude-with-the-users-go-log-datasave-continue-1-0102log
depends_on: save-date-last-write
related: save-trace
checked:
---
## What it does

Our SaveDataDialog list (type 2, Load) showed MediEvil's new-save item as "New save"; choosing it returned no save and
the game reopened the list (T-SV1). It is now "Continue": the newest save by `read_param`'s date, shown with its slot,
level and date, first and focused. Each save shows "Slot N" from its folder number (`@MediEvil0003` = slot 3; the
game's title is "SLOT DATA" on every save), then the level and date. A save's picture path is passed only when
icon0.png exists (MediEvil never saves one; the list logged a warning per slot).

## Invariants

- Continue picks the newest by the same date the game's save search sorts by (`save_time` = param byte 1288 from
  `read_param`), so it loads the slot the game would save into next.
- `lead` = entries before the saves (0 or 1); a choice maps to `dirs[choice - lead]`, Continue to `continue_dir`.
- Other dialog types keep "New save" when the game passes a new item (MediEvil only opens type 2).
- The slot number is the folder's trailing digits; a folder without them shows the param title.

## Interactions

- save-date-last-write: the order and Continue follow the last write; a load makes the loaded slot the newest.

## Evidence

- T-SV4 (PC, 01.02): verified. Continue loaded `@MediEvil0006`; the older `@MediEvil0001` loaded and saved into
  itself and became the newest. The user: "it looks like it's working as intended".

## History and pitfalls

- Next on the same list: details beside each slot (chalices, levels, play time), Square for full detail, a level
  picture, MediEvil's menu style (work queue).
