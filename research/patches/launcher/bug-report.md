---
id: bug-report
title: Launcher Submit Bug Report: a filled-in GitHub issue form and a redacted zip of logs and settings; issues into the work queue
area: launcher
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: 0d091ec
files: launcher/bug_report.py, launcher/medievil_launcher.py, .github/ISSUE_TEMPLATE/bug_report.yml, .github/ISSUE_TEMPLATE/feature_request.yml, .github/ISSUE_TEMPLATE/config.yml, research/tools/issue2card.py, tests/test_bug_report.py
symbols: redact, redaction_context, log_excerpt, issue_url, issue_fields, report_files, write_report, newest_dump, game_mode, build_report, refresh_report, submit_report, report_state, start_run_log, parse_form, queue_row, add_to_queue
switches:
tests:
docs: plan-work-queue.md
depends_on:
related: port-identity, key-capture
checked:
---
## What it does

The user's design choice (2026-11-11): a prefilled GitHub issue form plus a zip the player drags into it. That
needs no relay service, token or webhook of our own. Discord is the fallback for players without a GitHub
account.

**The button.** "Submit Bug Report" sits next to the GitHub and Discord buttons (`link_buttons`) and opens the
`report` page, which is not in the sidebar. It is a page rather than a window, because extra windows behave
badly under gamescope.

**The page.** It has:
- the kind: crash, graphics, performance, input, other bug or feature request;
- a "What happened" box;
- a preview of exactly what the issue will contain;
- the zip's file list with sizes;
- a checkbox for the newest crash dump, shown only if one exists (Windows: `<saves>/crashdumps`), unticked by
  default.

**Open GitHub.** It:
- writes `<saves>/bug-reports/medievil-report-<time>.zip` (`write_report`);
- opens that folder;
- opens `issues/new?template=bug_report.yml&...` with the form's fields (`issue_url`).

In Steam Deck Game Mode (`game_mode`), it says where the zip was saved instead of opening the folder.

**Other buttons.** "Copy summary" copies the preview text. "Discord instead" opens the Discord link. A feature
request uses `feature_request.yml` and sends no files.

**The zip.** It holds:
- `report.txt` (the preview text);
- `last_run.log`;
- `last_run.1.log`;
- `medievil.ini`, `settings.json`, `extra-env.txt`, `mods.json` and `patches.json`, the ones that exist;
- the crash dump, only if it was ticked.

**The previous run's log.** `start_run_log` moves the previous run's log to `last_run.1.log` at each start. That
way a relaunch after a crash keeps the crash's log. Both the launcher page and `--play` do this.

**Redaction** (`redact`) applies to every text that leaves the machine. It replaces:
- the home folder (both slash styles) with `~`;
- the name in `X:\Users\<name>`, `/home/<name>` and `/Users/<name>` paths with `<user>`;
- user, PC and player names as whole words with `<user>`, `<pc>` and `<player>`, but only if they are 3 characters
  or longer, because a short name would match parts of ordinary words;
- SteamID64 values with `<steamid>`;
- e-mail addresses with `<email>`.

Version numbers and addresses are left alone.

**The issue link** stays within 7,000 characters. `log_excerpt` keeps:
- the log's start up to the first `Runtime:` line;
- every error, fault, crash, device-lost and exit line;
- the last 30 lines.

It drops `Mouse:`, `Mouse look:` and `Save trace` lines and repeated lines. `issue_url` then cuts lines from the
middle of the excerpt, with a marker, until the link fits. The zip always has the whole log.

**Issues into the docs.** `research/tools/issue2card.py`:
- `list`: the issues labelled triage;
- `show N`: one issue's form, field by field;
- `queue N`: adds a row for the issue under "From GitHub issues" in plan-work-queue.md, once per issue.

It treats issue text as untrusted: only the title and the short fields go into the docs, flattened to one line,
with no pipes or backticks and no log text. A card is written by hand once something is found or built.

## Invariants

- Nothing leaves the machine until the player presses Submit on GitHub. The launcher only opens a URL and a folder.
- Every field `issue_url` fills in is an `id:` of its form. `test_url_fields_are_the_forms_ids` checks this, so
  renaming a form field breaks the test, not the prefill.
- The launcher's new strings are English only for now. lang.py falls back to English for texts missing from
  `KEYS`.

## Interactions

- port-identity: the link row.
- The run log rotation changes `last_run.log` handling. Research runs that copy `user/last_run.log` after a run are
  unaffected.

## Evidence

- `tests/test_bug_report.py` (12 tests) covers:
  - redaction of paths, names, Steam ID and e-mail, with driver versions kept;
  - the excerpt and the URL limit with both ends of the log kept;
  - feature requests sending no log;
  - the form ids;
  - the zip's contents, and the dump only when ticked;
  - Game Mode detection;
  - form parsing and work-queue rows in issue2card.
- A hidden-launcher run drove the page with a log that contains the real home path. The preview, file list and
  dump box appeared. Open GitHub saved the zip, opened its folder and opened a 604-character URL. The home path
  appeared in neither the URL nor the zip, and a feature request listed no files.
- Not run on the Steam Deck: whether the browser opens from Game Mode (the user: test later).

## History and pitfalls

- Designed 2026-11-11 by a design agent and agreed with the user:
  - zip in the saves folder;
  - previous log kept;
  - crash dumps offered, unticked;
  - a GitHub account needed, with Discord as the fallback;
  - work queue before cards;
  - English first.
