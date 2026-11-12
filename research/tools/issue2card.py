#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""GitHub issues from the launcher's Submit Bug Report (and the feature request form) into the work queue.

    issue2card.py list          issues labelled triage
    issue2card.py show N        issue N's form, field by field
    issue2card.py queue N       a row for issue N under "From GitHub issues" in plan-work-queue.md (once)

Needs the GitHub CLI (gh) signed in. Issue text is untrusted: only the title and the form's short fields
reach the docs, flattened to one line; no log text does. A card (research/patches) is written by hand once
something is found or built, naming the issue (#N) in its text."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
QUEUE = ROOT / 'research' / 'docs' / 'plan-work-queue.md'
REPO = 'BevelTipDrip/medievil_2019_pc_port'
HEADING = '## From GitHub issues'
TABLE = '| Issue | Item | What is known | State |\n|---|---|---|---|\n'
NO_RESPONSE = '_No response_'


def gh(*args):
    return subprocess.run(['gh', *args, '-R', REPO], check=True, capture_output=True, text=True).stdout


def fetch_issue(number):
    return json.loads(gh('issue', 'view', str(number), '--json', 'number,title,body,labels,url,createdAt'))


def parse_form(body):
    """{label: value} of an issue form's body (### Label, then the value; _No response_ is empty)."""
    fields, label, lines = {}, None, []
    for line in (body or '').splitlines():
        heading = re.match(r'^### (.+?)\s*$', line)
        if heading:
            if label is not None:
                fields[label] = '\n'.join(lines).strip()
            label, lines = heading.group(1), []
        elif label is not None:
            lines.append(line)
    if label is not None:
        fields[label] = '\n'.join(lines).strip()
    return {k: '' if v == NO_RESPONSE else v for k, v in fields.items()}


def one_line(text, limit=120):
    """Untrusted text made safe for a Markdown table cell: one line, no pipes or backticks, cut at limit."""
    text = re.sub(r'\s+', ' ', str(text or '')).replace('|', '/').replace('`', "'").strip()
    return text if len(text) <= limit else text[:limit - 1].rstrip() + '…'


def system_values(text):
    """The 'name: value' lines of the form's Version and system field."""
    return dict(m.groups() for m in re.finditer(r'^([A-Za-z ]+): (.+)$', text or '', re.M))


def queue_row(issue):
    """The work queue row of an issue (bug or feature request)."""
    fields = parse_form(issue.get('body', ''))
    labels = {label['name'] for label in issue.get('labels', [])}
    system = system_values(fields.get('Version and system', ''))
    kind = 'feature request' if 'feature-request' in labels or 'The idea' in fields else \
        (fields.get('Kind') or 'bug').lower()
    known = '; '.join(one_line(v, 40) for v in (kind, system.get('Platform'), system.get('Launcher'),
                                                 system.get('Game')) if v)
    return f"| #{issue['number']} | {one_line(issue['title'])} | {known}; [issue]({issue['url']}) | open |"


def add_to_queue(row, number, path=QUEUE):
    """Appends row under HEADING (made at the end with its table when missing). False if #number is there."""
    text = path.read_text(encoding='utf-8')
    if re.search(rf'^\| #{number} \|', text, re.M):
        return False
    if HEADING not in text:
        text = text.rstrip('\n') + f'\n\n{HEADING}\n\nMade by research/tools/issue2card.py queue N.\n\n{TABLE}'
    head, _sep, rest = text.partition(HEADING)
    section_end = rest.find('\n## ')
    section, after = (rest, '') if section_end < 0 else (rest[:section_end], rest[section_end:])
    section = section.rstrip('\n') + '\n' + row + '\n'
    path.write_text(head + HEADING + section + after, encoding='utf-8', newline='\n')
    return True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('list')
    for name in ('show', 'queue'):
        sub.add_parser(name).add_argument('number', type=int)
    args = parser.parse_args(argv)
    if args.command == 'list':
        for issue in json.loads(gh('issue', 'list', '--label', 'triage', '--json', 'number,title,labels')):
            labels = ', '.join(label['name'] for label in issue['labels'])
            print(f"#{issue['number']}  {one_line(issue['title'], 80)}  [{labels}]")
        return 0
    issue = fetch_issue(args.number)
    if args.command == 'show':
        print(f"#{issue['number']} {issue['title']}\n{issue['url']}\n")
        for label, value in parse_form(issue['body']).items():
            print(f'== {label}\n{value}\n')
        return 0
    row = queue_row(issue)
    if add_to_queue(row, issue['number']):
        print(f'{QUEUE.relative_to(ROOT)}: {row}')
    else:
        print(f'#{issue["number"]} is in the work queue already')
    print(f'then: gh issue edit {issue["number"]} -R {REPO} --add-label queued --remove-label triage')
    return 0


if __name__ == '__main__':
    sys.exit(main())
