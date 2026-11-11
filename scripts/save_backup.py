#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Backups of the game's saves: <user>/savedata copied to <user>/savedata_backups/<time>.

run.py makes one before every start; the launcher lists them and restores one. A restore
first backs up the current saves, so it can always be undone.

None are deleted: a backup is about 100-600 KB, and the game's slot order follows file times
(research/docs/study-save-slots.md), so an old good state must stay available. Copies keep the
file times (copytree uses copy2). Any future limit must go by age or size and count only the
date-named folders: a count of all folders by name deleted every automatic backup once ten
hand-made ones (letter names sort first) existed.
"""
import datetime
from pathlib import Path
import shutil

STAMP = '%Y%m%d-%H%M%S'


def backups_dir(user_dir):
    return Path(user_dir) / 'savedata_backups'


def list_backups(user_dir):
    """Backup names, newest first."""
    root = backups_dir(user_dir)
    return sorted((p.name for p in root.iterdir() if p.is_dir()), reverse=True) if root.is_dir() else []


def _has_saves(savedata):
    return savedata.is_dir() and any(p.is_file() for p in savedata.rglob('*'))


def _copy(user_dir, now):
    savedata = Path(user_dir) / 'savedata'
    if not _has_saves(savedata):
        return None
    root = backups_dir(user_dir)
    root.mkdir(parents=True, exist_ok=True)
    base = (now or datetime.datetime.now()).strftime(STAMP)
    target, n = root / base, 2
    while target.exists():
        target, n = root / f'{base}-{n}', n + 1
    shutil.copytree(savedata, target)
    return target


def backup(user_dir, now=None):
    """Copies savedata; returns the new folder, or None when there is nothing to save."""
    return _copy(user_dir, now)


def restore(user_dir, name, now=None):
    """Replaces savedata with backup `name`; returns the backup of the saves it replaced."""
    if not name or name not in list_backups(user_dir):
        raise ValueError(f'No save backup named {name!r}')
    source = backups_dir(user_dir) / name
    savedata = Path(user_dir) / 'savedata'
    safety = _copy(user_dir, now)
    staging = Path(user_dir) / 'savedata.restoring'
    shutil.rmtree(staging, ignore_errors=True)
    shutil.copytree(source, staging)
    shutil.rmtree(savedata, ignore_errors=True)
    staging.rename(savedata)
    return safety

