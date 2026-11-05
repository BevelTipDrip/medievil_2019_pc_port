"""Per-window and per-toggle-state summary of a game log with GOW3_FRAME_STATS=1 (deck.sh summary).

Each "Frame stats" line closes a window (~5 s). Windows are labelled with the toggle file's state
("Runtime: disabled optimizations mask=N" / "second word=N") and the render scale (run.py's "Render
resolution: ... render scale N%", the in-game menu's "Render scale: A% -> B% (live)"); a window in
which the state changed is marked and left out of the per-state medians (it mixes both). Heavy-view
windows: --min-draws.

    py -3.14 deck_summary.py <log> [--min-draws 1000] [--last N] [--windows]
"""
import argparse
import re
import statistics

FIELDS = [
    ('fps', r'^Frame stats: ([\d.]+) FPS', 'Frame stats'),
    ('worst', r'worst frame ([\d.]+) ms', 'Frame stats'),
    ('draws', r'([\d]+) draws/frame', 'Frame stats'),
    ('sd', r'stddev ([\d.]+) ms', 'Frame pacing'),
    ('p99', r'p99 ([\d.]+) ms', 'Frame pacing'),
    ('stageB', r'stage B busy ([\d.]+)%', 'Draw pipe'),
    ('stageA_wait', r'stage A waited ([\d.]+)%', 'Draw pipe'),
    ('eop_wait', r'filter -14 ident 64 [^:]+: \d+ calls, \d+ waited ([\d.]+) ms avg', 'Guest event waits'),
    ('hzb_hidden', r'keys \d+ \(\d+ read visible, (\d+) hidden', 'HZB remap'),
]


def parse(path):
    lines = open(path, encoding='utf-8', errors='replace').read().splitlines()
    mask, word2, changed, scale = 0, 0, False, ''
    windows, current = [], None
    for line in lines:
        m = re.match(r'Runtime: disabled optimizations mask=(\d+)', line)
        if m:
            changed |= windows != [] or current is not None
            mask = int(m.group(1))
            continue
        m = re.match(r'Runtime: disabled optimizations second word=(\d+)', line)
        if m:
            changed |= windows != [] or current is not None
            word2 = int(m.group(1))
            continue
        m = re.match(r'Render resolution: .*render scale (\d+)%|Render scale: \d+% -> (\d+)% \(live\)', line)
        if m:
            changed |= windows != [] or current is not None
            scale = f' {m.group(1) or m.group(2)}%'
            continue
        if line.startswith('Frame stats:'):
            if current:
                windows.append(current)
            current = {'state': f'{mask} {word2}{scale}', 'mixed': changed, 'extra': {}}
            changed = False
        if current is None:
            continue
        for key, pattern, prefix in FIELDS:
            if line.startswith(prefix) or line.startswith('  ' + prefix):
                m = re.search(pattern, line)
                if m and key not in current:
                    current[key] = float(m.group(1))
        for prefix in ('Recorder per frame', 'Stage B syscalls', 'Decode pacing (', 'OnSubmit on stage B',
                       'Arena writes', 'Early frame EOP', 'Thread ids per frame', 'Draw classes per frame',
                       'Readback pre-check per frame', 'Region table per frame', 'Stage B sleeps per frame',
                       'Guest pool faults', 'Texture-set memo per frame'):
            if line.startswith(prefix):
                current['extra'][prefix] = line
    if current:
        windows.append(current)
    return windows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('log')
    parser.add_argument('--min-draws', type=int, default=1000, help='heavy-view windows only (0: all)')
    parser.add_argument('--last', type=int, default=0, help='only the last N windows')
    parser.add_argument('--windows', action='store_true', help='print every window')
    args = parser.parse_args()
    windows = parse(args.log)
    if args.last:
        windows = windows[-args.last:]
    print(f'{len(windows)} windows; FPS: ' + ' '.join(str(round(w.get("fps", 0))) for w in windows))
    keys = [k for k, _p, _x in FIELDS]
    if args.windows:
        print('state            ' + ' '.join(f'{k:>11}' for k in keys))
        for w in windows:
            print(f'{w["state"]:<16}' + ('*' if w['mixed'] else ' ') +
                  ' '.join(f'{w.get(k, float("nan")):11.2f}' for k in keys))
    heavy = [w for w in windows if not w['mixed'] and w.get('draws', 0) >= args.min_draws]
    states = sorted({w['state'] for w in heavy})
    print(f'\nHeavy windows (>= {args.min_draws} draws, switch windows left out), medians by toggle state:')
    print(f'{"state":<17}{"n":>3} ' + ' '.join(f'{k:>11}' for k in keys))
    for state in states:
        group = [w for w in heavy if w['state'] == state]
        row = []
        for k in keys:
            values = [w[k] for w in group if k in w]
            row.append(statistics.median(values) if values else float('nan'))
        mean_fps = statistics.mean(w['fps'] for w in group if 'fps' in w)
        print(f'{state:<17}{len(group):>3} ' + ' '.join(f'{v:11.2f}' for v in row) + f'   (mean FPS {mean_fps:.1f})')
        last = group[-1]['extra']
        for prefix, line in last.items():
            # The class line is the per-class table itself: printed whole.
            print(f'    {line if prefix == "Draw classes per frame" else line[:220]}')


if __name__ == '__main__':
    main()
