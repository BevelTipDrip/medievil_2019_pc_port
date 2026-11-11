---
id: save-backup-keep-all
title: Launcher save backups are never deleted (no keep-10 pruning)
area: launcher
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: d99acfc575
files: scripts/save_backup.py, launcher/medievil_launcher.py, tests/test_save_backup.py
symbols: backup, restore, test_keeps_every_backup, test_keeps_file_times
switches:
tests: T-SV3
docs: study-save-slots.md#also-found
depends_on:
related: save-date-last-write
checked:
---
## What it does

run.py copies `user/savedata` to `user/savedata_backups/<yyyymmdd-hhmmss>` before every start, and a restore first
backs up the current saves. Upstream kept the 10 newest folder **names**; now nothing is deleted. A backup is about
100-600 KB, so 1,000 launches cost 0.1-0.6 GB.

## Invariants

- Copies keep file times (`shutil.copytree` uses `copy2`; `test_keeps_file_times`): the game's save target is the
  newest slot by file time (save-date-last-write).
- Any future limit goes by age or size and counts only date-named folders. A count by name sorted letter-named
  hand-made backups first: with ten of them every automatic backup was deleted as it was made (found 2026-11-10).
- Hand-made backups belong in `user/savedata_backups_manual/`, outside the launcher's list.

## Interactions

- save-date-last-write: backups restored without times would scramble the slot order and the save target.

## Evidence

- 2026-11-10: `savedata_backups` held exactly ten letter-named folders and no dated ones after several launches.
- `tests/test_save_backup.py`: 8 tests pass. The full suite's 2 failures and 6 errors (test_packaged_vulkan,
  test_patches) fail the same without this change.

## History and pitfalls

- The automatic backup from 20:23 on 2026-11-10 (slot 6 before an overwrite) was pruned this way; the same state was
  still in slot 7.
