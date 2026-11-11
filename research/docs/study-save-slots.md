# Save slots: why a save went into another slot

Parent: [START-HERE.md](START-HERE.md). Cards: [runtime/save-date-last-write](../patches/runtime/save-date-last-write.md),
[runtime/save-trace](../patches/runtime/save-trace.md). Work queue: [plan-work-queue.md](plan-work-queue.md).

## The problem

The user, 2026-11-10: after loading an older slot, "all my listed save slots consisted of the newest one I loaded",
and "things can get over-written or deleted". Run logs showed a load of `@MediEvil0007` followed by a save into
`@MediEvil0006` (`data/crypt-light-grid-fix-0102.log`, and the 21:41 run). Before the date repair of 2026-11-10 six
slots also shared one param.bin time (a restore without dates on 2026-11-09), so their order was arbitrary.

## How MediEvil picks the slot it saves into

From the save trace (every save call with its guest caller):

- Boot: `sceSaveDataDirNameSearch('@%', key 3 = time, order 1 = newest first)`, then the first result is mounted
  read-only and read (guest+0x6cce73 / +0x6cd29e).
- Load: the Load list (our SaveDataDialog, type 2) returns the chosen dir; the game mounts it **read-write**
  (mode 0x2), reads `ue4savegame.ps4.sav` and writes the same bytes back. No SetParam.
- Save: search again (guest+0x6cb95c), take the **first (newest)** result, try to create it (mode 0x16, fails
  "exists"), mount it read-write (guest+0x6cbc62), write the save, SetParam title `SLOT DATA` and subtitle = the
  level name.
- New Game (the game's own menu): creates the next free number (`@MediEvil0008` with 7 slots) and saves there. No
  overwrite and no limit reached at 8 slots.

So the game saves into whichever slot is newest and relies on the load-time write-back to make the loaded slot the
newest. Our `read_param` dated a slot by param.bin's file time only, which a load does not change: the loaded slot
stayed old and the next save went into the newest other slot.

## T-SV1: first trace (2026-11-10, launched by Claude with the user's go, log `data/save-trace-1-0102.log`)

No slot loaded. The game's New Game created `@MediEvil0008` and saved there four times. The port's "New save" entry
in the Load list returned no save; the game reopened the list each time (the user: "the 'new save' from the ports
save menu didn't do anything"). Saves restored afterwards; the run's saves are in
`savedata_backups_manual/after-save-trace-1-20261110`.

## T-SV2: load a slot that is not the newest (2026-11-10, launched by Claude with the user's go, log `data/save-trace-2-0102.log`)

Chose `@MediEvil0007` (second in the list). Its file was rewritten unchanged at load; the next two saves went into
`@MediEvil0006` (subtitle "The Hilltop Mausoleum"). Afterwards slot 7's file was byte-identical to before and slot 6
held slot 7's state plus the new play. Reproduced with distinct dates on every slot. Saves restored afterwards.

## T-SV3: the date fix (2026-11-10, launched by Claude with the user's go, log `data/save-trace-3-0102.log`)

Build with `read_param` dating a slot by its newest file. The user loaded `@MediEvil0002`: it was first in the next
search (22:37:02, the load write-back), and both saves went into it (Cemetery Hill, 22:37 and 22:38). Every other
slot's file is byte-identical to the backup taken before the run; loading `@MediEvil0006` at the end only rewrote its
own bytes. The user: "It looks like it worked."

## Also found

- The launcher's save backups (`scripts/save_backup.py`) keep the 10 newest **by name**: hand-made backups named with
  letters sorted first and the automatic ones were deleted as soon as they were made. The hand-made ones now live
  in `user/savedata_backups_manual/`.

## T-SV4: "Continue" and slot numbers in the Load list (2026-11-10, launched by Claude with the user's go, log `data/save-continue-1-0102.log`)

The Load list's dead "New save" entry became "Continue" (the newest save, with its slot, level and date), each
save shows "Slot N" from its folder number, and missing save pictures are no longer looked up. The user chose
Continue (`@MediEvil0006`), then loaded the older `@MediEvil0001`: its save went into `@MediEvil0001` (log line
528) and it became the newest. The launcher's backup of this start (`savedata_backups/20261110-230059`) stayed. The
user: "Continue is the most recently loaded save, and the top slot is the most recent slot, all slots have their
name, I loaded an older save, nothing was deleted, and the loaded save got recognized as the newest one".

## T-SV5: details, level pictures and the game's menu look (2026-11-10, launched by Claude with the user's go, logs `data/load-details-1-0102.log`, `data/load-style-1-0102.log`, `data/load-style-2-0102.log`)

The Load list shows, per save, what it holds (runtime_saveinfo.c reads the GVAS save: LevelData's per-level
Unlocked / Completed / ChaliceCollected, Gold, HealthVials, PlayTime, LastLevelPlayed, BestiaryData), the loading
screen of the level it was saved in, and on Square every level with its state. The level names, pictures, the
Options menu frame, the PS4 button prompts and the two menu fonts come from the player's own paks at start
(scripts/game_ui_assets.py, `user/cache/game_ui/`); the list is drawn in the Options menu's look.

Found on the way:
- The save field `HealthVials` is the life bottle count (the user: "I currently have 5 Life bottles", the save
  says 5); the per-level `LifeBottleCollected` is 0 in every save seen and is not shown. Health vials (pick-ups)
  are not shown (the user).
- `LastLevelPlayed` is a ByteProperty with an enum name (UE4 4.21), not an EnumProperty.
- LevelsEnum's order (Level_NN_CODE) gives the save's NewEnumerator numbers: 0 Dan's Crypt ... 11 The Enchanted
  Earth, 12 The Ant Caves ... 22 Hall of Heroes; names from LevelDataTable.
- The UI textures are single-mip DXT1 / DXT5 / B8G8R8A8 in PS4 1D thin micro tiling (8x8 elements per tile,
  Morton order), padded to whole tiles; all 23 loading screens, the frame and 19 prompts decode.

Runs: the first (plain overlay look) showed life bottles from the wrong field; the second (game look) showed the
title screen through the panel and the prompts over the copyright line; the third, with an opaque fill, the panel
at 76% from 6% down and the details aligned under the picture: the user, "Yes, everything looks correct".
