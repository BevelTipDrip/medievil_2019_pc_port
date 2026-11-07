# Handoff 2026-11-07: patch map in progress

Parent: [START-HERE.md](START-HERE.md). The user ran low on usage while this was running; work was committed
mid-run so nothing is lost.

## What exists

- `research/tools/patchmap.py` (facts, index, check, lookups: file / switch / commit / test / show / find / doc).
- `research/patches/README.md`: card format. `research/patches/<area>/*.md`: cards. `_facts/`: generated.
- A workflow (run `wf_27d70c18-22c`, session `78f7734e`) was writing and then adversarially verifying one card
  per patch, in area batches of up to 12. It planned every code commit (251) and our switches (171) into patches.
  It cannot be resumed from another session. Cards with a `verified: 2026-11-07` line went through the checker;
  cards without it were only written.

## To finish (next session)

1. `python research/tools/patchmap.py facts && python research/tools/patchmap.py check --list`: broken
   references, uncovered commits and switches.
2. Cards without `verified:` (`grep -L '^verified:' research/patches/*/*.md`): run a checker pass over those areas
   (adversarial: commits really belong, defaults from the parsing code / PERF_SWITCHES / DECK_SWITCHES,
   invariants against the current code, evidence numbers against the docs).
3. Write cards for uncovered commits / switches; fix `check` errors.
4. `patchmap.py index`, skim INDEX.md with the user.
5. START-HERE: cut to rules + pointers, point at the patch map (`patchmap.py file <path>` before editing code),
   fix the stale lines (current state is from 2026-10-18; it calls `origin` the public upstream, but `origin` is the
   user's fork BevelTipDrip and `upstream` is bc160201983). The repo CLAUDE.md has the same stale `origin` line:
   ask the user before editing it.

## Other open items from the same session

- Deck settings review (the user re-enabled the Deck switches and finer culling; needs the Deck on).
- Windows: kept pool pages handed to another address un-zeroed once free pages run out (`pool_take_locked`
  second loop); fix on `lab` after regression testing. See `research/patches/memory/pool-kept-pages.md`.
- Optional: a write-up of the pool leak for spacepepperoni (offered, not accepted).
