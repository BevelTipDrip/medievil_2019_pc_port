---
id: port-identity
title: Fork identity: version 2.0.0, update check and links on BevelTipDrip, new port icon, Start menu shortcut
area: launcher
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: dda748522a, c1db412306, 711b3968f1, 44a4036e93
files: launcher/medievil_launcher.py, run.py, packaging/linux/appimage.sh, packaging/windows/package.sh, launcher/medievil.ico, launcher/medievil.png, tests/test_launcher_controls.py, README.md
symbols: VERSION, RELEASES_API, RELEASES_PAGE, LINKS, version_tuple, latest_release, check_update, install_update, set_icon, shortcut, shortcut_script, GOW3_WINDOW_ICON
switches:
tests:
docs: plan-work-queue.md#the-users-quality-of-life-list-2026-11-03-after-the-v200-release, study-upstream-commits.md#semantic-hazards-in-files-that-auto-merge-mt
depends_on:
related: 4k-memory, upstream-launcher-features, cursor-hide
---
## What it does

`dda7485`: `VERSION = '2.0.0'`. The update check (`latest_release`, `RELEASES_API`), the release page, the GitHub
link (`LINKS`) and the AppImage homepage point at BevelTipDrip/medievil_2019_pc_port, not upstream bc160201983, and
the README has a "This fork" section. `c1db412`: Advanced has a "Start menu shortcut" button beside the desktop one
(`shortcut('Programs')`, built by `shortcut_script`). `711b396`: a new port icon (`launcher/medievil.ico` /
`.png`, 16-256 px) on the exes (.rc), the shortcuts and the Linux menu entry; `make_medievil_icon.py` was removed
so it cannot overwrite the design. `44a4036`: the launcher and game windows show the user's game icon again
(`sce_sys/icon0.png`: the launcher's `set_icon`, run.py's `GOW3_WINDOW_ICON` BMP), falling back to the port icon.

## Invariants

- The fork's version series must stay above upstream's (1.3.x): `check_update` offers a release only when
  `version_tuple(tag) > version_tuple(VERSION)`. A frozen build's `install_update` unpacks the release zip over the
  install, so pointing `RELEASES_API` at upstream again would install upstream's build over the fork (H4 in
  study-upstream-commits.md). `test_fork_release_is_newer_than_upstream_and_checks_the_fork` checks both.
- `version_tuple` compares the digit groups only. A tag like `v2.0.0-deck` is read as 2.0.0 and is not newer.
- `install_update` only runs frozen (PyInstaller) and with a `.zip` asset. A source tree or an install without the
  asset opens the release page instead.
- No game files are bundled. The window icons are made at run time from the user's own `icon0.png`, and the
  shortcuts and exes use the port's own icon.

## Interactions

- c1db412 also removed the inert 4K memory cvars (`r.GarlicHeapSizeMB=4600`, `r.Streaming.PoolSize=1600`) from
  `ue4_override.apply`. That part is on the 4k-memory card.
- upstream-launcher-features: the upstream launcher this rebrands. H4 in study-upstream-commits.md flagged that its
  update check would offer upstream's builds over ours.

## Evidence

- Packaging and branding only, with no performance claim. The user's v2.0.0 release followed (plan-work-queue.md,
  "after the v2.0.0 release").
- Not verified in game: the icon and shortcut changes. `44a4036` reverts `711b396` for the two windows only; the
  commit does not say who asked for it.

## History and pitfalls

- Before `dda7485` the launcher checked bc160201983's releases with the same version (1.3.8), so a later upstream
  release would have been offered as an update over the fork. The inherited release-version test expected God of
  War III's 2.1.3.
- `c1db412` added `launcher/medievil-game.ico` for the shortcuts. Eleven hours later `711b396` folded it back into
  `medievil.ico`, and `package.sh` now copies only `medievil.ico` / `.png`. A card or doc that names
  `medievil-game.ico` is out of date.
