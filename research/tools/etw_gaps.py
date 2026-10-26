"""The longest off-CPU stretches of chosen threads, with the wait reason of the switch-out and the
thread that readied them, from the CSwitch / ReadyThread records of an xperf dumper export.

    py -3.14 etw_gaps.py <dump.csv> <tid>=<name> [<tid>=<name> ...] [--min-ms 20] [--top 15]

A frame hitch shows as a long gap in the threads that make a frame (stage A, stage B, the
recorder, the presenting thread); the readying thread at its end is what it waited for.
"""
import collections
import sys

dump = sys.argv[1]
names = dict(a.split('=', 1) for a in sys.argv[2:] if '=' in a and not a.startswith('--'))
min_ms = float(sys.argv[sys.argv.index('--min-ms') + 1]) if '--min-ms' in sys.argv else 20.0
top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 15
out_at, out_reason, last_ready = {}, {}, {}
gaps = collections.defaultdict(list)  # tid -> (ms, start s, reason, readier)
first = None
for line in open(dump, encoding='utf-8', errors='replace'):
    kind = line[:30].strip()
    if not (kind.startswith('CSwitch,') or kind.startswith('ReadyThread,')):
        continue
    p = [x.strip() for x in line.split(',')]
    try:
        ts = int(p[1])
    except ValueError:
        continue
    first = ts if first is None else first
    if kind.startswith('ReadyThread,'):
        # ReadyThread, TimeStamp, Process (PID), ThreadID, Rdy Process (PID), Rdy TID, ...
        if len(p) > 5 and p[5] in names:
            last_ready[p[5]] = f'{p[2].split("(")[0].strip()} {p[3]}'
        continue
    new, old = p[3], p[9]
    if new in names and new in out_at:
        ms = (ts - out_at.pop(new)) / 1000
        if ms >= min_ms:
            gaps[new].append((ms, (ts - first) / 1e6 - ms / 1000, out_reason.get(new, '?'),
                              last_ready.get(new, '?')))
    if old in names:
        out_at[old] = ts
        out_reason[old] = f'{p[12]}/{p[13]}'
for tid, name in names.items():
    found = sorted(gaps[tid], reverse=True)
    print(f'{name} ({tid}): {len(found)} off-CPU stretches >= {min_ms:g} ms')
    for ms, at, reason, readier in found[:top]:
        print(f'  {ms:8.1f} ms at {at:7.3f} s  out: {reason:32s} readied by {readier}')
