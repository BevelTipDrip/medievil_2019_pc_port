# Patch map

One card per patch: what it does, the code it changes, its switches and defaults, **what must stay true for
it to work (invariants)**, how it interacts with other patches, the evidence, and what went wrong before.
Rejected and reverted attempts have cards too, so nobody tries them again without knowing why they failed.

## Use it before touching code

```
python research/tools/patchmap.py file src/runtime_memory.c     # every patch in a file, with its invariants
python research/tools/patchmap.py switch GOW3_POOL_KEEP_PAGES   # its patch, default and code sites
python research/tools/patchmap.py commit 7246f88                # the patch a commit belongs to
python research/tools/patchmap.py test T-F6                     # where a test run is written up
python research/tools/patchmap.py find "kept views"             # search the cards
python research/tools/patchmap.py doc plan-linux-deck.md        # a doc's outline with anchors
```

[INDEX.md](INDEX.md) lists every card on one line; [TESTS.md](TESTS.md) every test ID and its write-up. Both are
generated (`patchmap.py index`), so do not edit them.

## Keep it true

After a session that changes code: write or update the card (same commit as the code, or right after), then

```
python research/tools/patchmap.py facts    # recompute _facts/ from git, the code and the docs
python research/tools/patchmap.py check    # errors, plus code commits and switches no card covers
python research/tools/patchmap.py index    # rebuild INDEX.md and TESTS.md
```

`check` fails on a card naming a file, symbol, switch, commit, doc or anchor that does not exist, or a card
referring to an unknown card. `check --list` prints the uncovered commits and switches.
`_facts/` is generated: `commits.tsv` (every commit since `upstream/main` on the local branches, its kind,
branch and files), `switches.tsv` (every `"GOW3_*"` string in the code, ours or upstream's, the toggle bits of
`gpu/shim/gow3_toggles.h` and the `perf_*` / `present_*` ini keys), `headings.tsv` (every doc heading and
its anchor).

## Card format

`research/patches/<area>/<id-in-lower-case>.md`. Front matter is `key: value`, lists comma-separated:

| Key | Value |
|---|---|
| `id` | short and unique, the name the docs use where there is one (`M1`, `D2`, `T2`, `pool-kept-pages`) |
| `title` | one line |
| `area` | the folder: `memory`, `runtime`, `stage-a`, `stage-b`, `caches`, `present`, `ue4`, `deck`, `launcher`, `tooling` |
| `status` | `active`, `rejected` (measured, not kept; code may stay behind an off switch), `reverted` (code gone), `superseded` (+ `superseded_by: <id>`), `diagnostic`, `chore` (refactors, build fixes, comments) |
| `platforms` | `all`, `windows`, `linux` |
| `default_windows`, `default_deck` | `on`, `off`, `n/a`, `mixed` (explain under Interactions). Deck = the launcher's Deck Optimized preset with its switches |
| `origin` | `ours`, `bloodborne <commit>`, `upstream <version>`, `spacepepperoni <commit>`, `shadps4` |
| `class` | `G` general, `C` conditional (assumption under Invariants), `M` MediEvil-specific (fix-inventory.md's classes) |
| `commits` | short hashes, oldest first |
| `files` | repo-relative paths it changes (main files, not every touched line) |
| `symbols` | functions, types or variables it adds or changes (each must occur in one of `files`) |
| `switches` | `GOW3_*` variables, ini keys (`perf_*`, `present_*`), toggle bits as `toggle:Name` / `toggle2:Name` |
| `tests` | test IDs (`T-F6`, `N-30`, `Launch 15`) |
| `docs` | `file.md#anchor` under `research/docs/` (anchors: `patchmap.py doc <file>`) |
| `depends_on`, `conflicts_with`, `supersedes`, `related` | card IDs; the reason goes under Interactions |
| `verified` | date a separate agent or session last checked the card against the code (`2026-11-07`) |

Body, in this order (all required except for `chore` cards):

- `## What it does`: two to five lines, mechanism first.
- `## Invariants`: what must stay true for it to be correct, and what breaks if it does not. Bullets, each one
  checkable in the code. The part to read before changing anything nearby.
- `## Interactions`: other patches it needs, conflicts with, masks or is masked by, with the reason.
- `## Evidence`: test IDs with the numbers, and what the user confirmed in game (quoted).
- `## History and pitfalls`: what went wrong, what was tried and dropped, which bug it caused or fixed.

Keep a card under about 60 lines. Its detail lives in the docs it links.
