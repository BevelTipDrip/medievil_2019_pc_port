#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The game's Brightness setting in its saves: read it, or set it to neutral.

MediEvil keeps Brightness (B) in every save slot's UE4 save (<user>/savedata/.../ue4savegame.ps4.sav, GVAS, no
checksum) as a FloatProperty, and runs UE4's `gamma <B * 4.5 + 0.5>` (research/docs/study-hdr.md item 3). The
default, 0.3778, is display gamma 2.2: neutral. The slider steps by 0.02 from it. UE4 writes a property only when it
differs from the default, so a slot without one is at neutral already.

In HDR the scene ignores Brightness (the port's grading pass), but Unreal's UI shaders still apply it to the HUD and
menus, so the launcher offers "Set game Brightness to neutral": back the saves up first (save_backup.py, file times
kept), then write 0.3778 over each slot's value in place, 4 bytes, and put each file's access / modified times back
(the game picks its slot by file time, research/docs/study-save-slots.md).
"""
from pathlib import Path
import os
import struct

import save_backup

NEUTRAL = 0.3778
_NAME = struct.pack('<i', 11) + b'Brightness\x00'
_TYPE = struct.pack('<i', 14) + b'FloatProperty\x00'


def display_gamma(value):
    return value * 4.5 + 0.5


def _value_offset(data):
    """Offset of the Brightness float, None when the save has none; ValueError when the layout is not the expected
    one (then nothing is written)."""
    first = data.find(_NAME)
    if first < 0:
        return None
    if data.find(_NAME, first + 1) >= 0:
        raise ValueError('more than one Brightness property')
    at = first + len(_NAME)
    if data[at:at + len(_TYPE)] != _TYPE:
        raise ValueError('Brightness is not a FloatProperty')
    at += len(_TYPE)
    size = struct.unpack_from('<q', data, at)[0]
    if size != 4 or data[at + 8] != 0:  # 4-byte payload, no property GUID
        raise ValueError(f'unexpected Brightness payload (size {size})')
    return at + 9


def slots(user_dir):
    """(save file, Brightness or None) per slot, newest first."""
    root = Path(user_dir) / 'savedata'
    files = sorted(root.rglob('ue4savegame.ps4.sav'), key=lambda p: p.stat().st_mtime, reverse=True) \
        if root.is_dir() else []
    result = []
    for path in files:
        data = path.read_bytes()
        try:
            at = _value_offset(data)
        except ValueError:
            at = None
        result.append((path, struct.unpack_from('<f', data, at)[0] if at is not None else None))
    return result


def set_neutral(user_dir):
    """Backs the saves up, then sets Brightness to neutral in every slot that stores another value.
    Returns (backup folder or None, slots changed, slots skipped as [(path, reason)])."""
    plan, skipped = [], []
    for path, _ in slots(user_dir):
        data = path.read_bytes()
        try:
            at = _value_offset(data)
        except ValueError as error:
            skipped.append((path, str(error)))
            continue
        if at is not None and abs(struct.unpack_from('<f', data, at)[0] - NEUTRAL) > 1e-6:
            plan.append((path, at))
    if not plan:
        return None, 0, skipped
    backup = save_backup.backup(user_dir)
    if backup is None:
        raise OSError('the save backup failed; nothing was changed')
    for path, at in plan:
        times = os.stat(path)
        with open(path, 'r+b') as f:
            f.seek(at)
            f.write(struct.pack('<f', NEUTRAL))
        os.utime(path, ns=(times.st_atime_ns, times.st_mtime_ns))
        check = path.read_bytes()
        if abs(struct.unpack_from('<f', check, at)[0] - NEUTRAL) > 1e-6 or os.stat(path).st_mtime_ns != times.st_mtime_ns:
            raise OSError(f'{path}: the value or its file time did not stick; restore backup {backup.name}')
    return backup, len(plan), skipped


if __name__ == '__main__':
    import sys
    user = sys.argv[1] if len(sys.argv) > 1 else str(Path(__file__).resolve().parent.parent / 'user')
    for path, value in slots(user):
        print(path.parent.name, 'default (neutral)' if value is None else f'{value:.4f} = gamma {display_gamma(value):.2f}')
