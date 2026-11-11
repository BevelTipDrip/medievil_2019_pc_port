---
id: python-test-upkeep
title: Python test suite upkeep: stale God of War III tests moved to MediEvil, Linux-only and Pillow-less skips, version pin
area: tooling
status: chore
platforms: all
default_windows: n/a
default_deck: n/a
origin: ours
class: G
commits: 4e5696e9a8, d75c1c8c30
files: tests/test_patches.py, tests/test_packaged_vulkan.py, tests/test_ue4_glyphs.py, tests/test_launcher_controls.py
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
