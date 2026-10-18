# PR: <title>

Status: draft | proposed | PR #n (<url>) | merged <commit> | rejected (<why>)
Branch: `feat/<topic>` from `main` at `<commit>`. Plan: <plan doc>. Checklist: [pr-checklist.md](pr-checklist.md).

## What

The problem, with evidence (`file:line`, log lines). Who is affected (game builds, hardware, settings).

## How

The mechanism. Enough for a reviewer to follow without running it.

## How it fits in

| File | Change | Why |
|---|---|---|
| | | |

Existing mechanisms used or changed. Defaults. What happens on unsupported builds / hardware. What callers see.

## Proof

| Check | Result | Evidence |
|---|---|---|
| Build (Windows, CLANG64) | | `out/build.log` |
| `build.sh --test` | | |
| In game: <scenario> (eboot sha256 `<...>`, settings) | | user: "<quote>" / log lines |
| Before / after (performance changes) | | `data/<file>` |

## Risks and limits

What could break, and what was not tested.

## Attribution

Code or ideas from other projects (repo, commit, files), licence.
