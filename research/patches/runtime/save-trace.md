---
id: save-trace
title: "Save trace:" log lines for every save call (search, mount, file writes, set param, dialogs) with the guest caller
area: runtime
status: diagnostic
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: 0061d7b06c
files: src/runtime_savedata.c, src/runtime_services.c, src/runtime_file.c, src/runtime.h
symbols: runtime_save_trace_caller, trace_mount, save_path, save_open, save_result, msg_open
switches:
tests: T-SV1, T-SV2, T-SV3
docs: study-save-slots.md#how-medievil-picks-the-slot-it-saves-into
depends_on:
related: save-date-last-write
checked:
---
## What it does

Always-on log lines starting `Save trace:`: each save search (pattern, key, order, the names returned with their
dates and level), mount (mode, result, `guest+0x...` caller), unmount, set param, icon, delete, each file opened for
reading or writing under `/savedataN` and each unlink / rename there, what the SaveDataDialog is given (mode, type,
listed saves, focus, new-save item, message text) and what it returns, and MsgDialog texts.

## Invariants

- Saves are rare (a few dozen lines a session, most from searches at the title screen): no switch. A hot path
  calling `sceSaveDataDirNameSearch` every frame would flood the log; none seen in T-SV1-3.
- The caller is `__builtin_return_address(0)` of the ABI function, printed relative to `runtime_image_base()` when
  within 512 MB of it.
- Dialog field offsets follow `OrbisSaveDataDialogParam` / `Items` / `MsgDialogParam` as commented in
  runtime_services.c; a wrong offset prints garbage, it does not change behaviour.

## Interactions

- save-date-last-write: found with it (study-save-slots.md).

## Evidence

- T-SV1-3 logs: the game's save path (search newest first, create fails, mount read-write) and the load write-back.

## History and pitfalls

- The first edit wrote the `\n` escapes as real line breaks (a Python heredoc); fixed before the build.
