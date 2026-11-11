---
id: python-test-upkeep
title: Python upkeep: test suite (stale GoW3 tests, Linux-only and Pillow-less skips, version pin) and the Windows package's Python (3.10+)
area: tooling
status: chore
platforms: all
default_windows: n/a
default_deck: n/a
origin: ours
class: G
commits: 4e5696e9a8, d75c1c8c30, e8a9d16af2
files: tests/test_patches.py, tests/test_packaged_vulkan.py, tests/test_ue4_glyphs.py, tests/test_launcher_controls.py, packaging/windows/package.sh
symbols: rules_file, LINUX_PATHS
switches:
tests:
docs:
depends_on:
related: button-prompts, key-capture
checked:
---
## What it does

Keeps `python -m unittest discover -s tests` green: with the launcher's Python 3.14 on Windows (170 tests OK) and
WSL's Python 3.13 (158 OK), 2026-11-11.

## Invariants

- test_patches tests `patches/MediEvil.xml` (the only built-in patch file since `316f552`); the selection rules it
  has no example of (exclusive "Resolution Patch" names replacing the texture fix, VBlank notes) use the inline
  `RULES` file.
- test_packaged_vulkan's two tests with real temporary paths in ":"-separated lists are skipped on Windows
  (`LINUX_PATHS`); `vulkan_check` runs only in the AppImage.
- test_ue4_glyphs skips without Pillow; ReleaseVersionTest pins `launcher.VERSION` (bump it with each release).
- Python 3.8 is not a target (the port needs 3.10+): `Path.is_relative_to`, `TestCase.enterContext` and `X | None`
  fail there.

## Evidence

- 2026-11-11: before, 8 failures with Python 3.14 (6 GoW3 profile tests, 2 Linux-path tests); after, none.

## History and pitfalls

- 2026-11-11: the Windows packages up to 2.2.0 were frozen with Python 3.8 (`package.sh` took the last
  `Python3*` folder in name order: Python38 after Python310), so the shipped launcher and scripts ran on 3.8 and
  Pillow 10.4: `mods.py`'s `Path.is_relative_to` failed, the DXT5 prompts fell back to PlayStation. `package.sh`
  now takes the newest Python 3.10+ (also the Python install manager's `pythoncore-3.*`), refuses older ones and
  rebuilds `out/pyenv` when its version differs; 2.2.1 is frozen with 3.14 (checked inside the frozen exe: Pillow
  13 encodes DXT5, both prompt styles build all 11 files).
