# Handoff 2026-11-07: patch map built

Parent: [START-HERE.md](START-HERE.md).

## What exists

- [research/patches/INDEX.md](../patches/INDEX.md): 198 cards, one line each (status, Windows / Deck default,
  branch, switches). Every code commit on `lab` / `deck-lab` (248) and every switch, toggle bit and ini key of ours
  (171) belongs to a card. Rejected and reverted attempts have cards.
- Each card was written by one agent and checked against the code and the docs by a separate one (`verified:
  2026-11-07`). Checkers fixed wrong defaults, commits, invariants and numbers on most cards.
- [LANDMINES.md](../patches/LANDMINES.md): what the checkers found (code reading, not run). The first item matters
  most: **A3 flipped live disables draw preparation for the rest of the launch**, so several Deck live A/Bs
  (round 1 launch 7, Launch 15's later rounds) ran mostly without draw preparation in both states.
- `research/tools/patchmap.py`: `file` / `switch` / `commit` / `test` / `find` / `show` / `doc` lookups,
  `facts`, `check`, `index`. Format: [README.md](../patches/README.md).

## Next

1. Go through LANDMINES.md with the user: decide which to confirm (A3's draw-prep loss first: a re-read of the
   Launch 15 conclusions; then the PC toggle-script file mismatch).
2. Deck settings review (the user re-enabled the Deck switches and finer culling; needs the Deck on). The cards'
   `default_deck` column and LANDMINES items 14, 18, 22 feed it.
3. Windows pool leak fix on `lab` (LANDMINES item 9) with its regression pass.
4. START-HERE still has the 2026-10-18 state table (now marked stale); the repo CLAUDE.md still calls `origin`
   the public upstream: ask the user before editing it.
5. Some cards run 60-90 lines (the guideline is ~60); the README has no "not built" status (pgo-build, U5 use
   rejected).
