"""Waits of one process's threads on kernel resources (WrResource, WrPushLock, WrGuardedMutex...) and
which process readied them, from the CSwitch / ReadyThread records of an xperf dumper export: lock
holders outside the game (monitoring tools reading its memory) stall its threads.

    py -3.14 etw_locks.py <dump.csv> <pid> [--min-ms 1] [--list 20]
"""
import collections
import sys

dump, pid = sys.argv[1], sys.argv[2]
min_ms = float(sys.argv[sys.argv.index('--min-ms') + 1]) if '--min-ms' in sys.argv else 1.0
listed = int(sys.argv[sys.argv.index('--list') + 1]) if '--list' in sys.argv else 20
LOCKS = ('WrResource', 'WrPushLock', 'WrGuardedMutex', 'WrFastMutex', 'WrMutex', 'WrKeyedEvent')
tag = f'({pid})'
out = {}          # tid -> (switch-out ts, reason)
readier = {}      # tid -> process of the last ReadyThread
total = collections.defaultdict(lambda: [0, 0.0, 0.0])  # (reason, readier process) -> count, ms, max
events = []
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
        if len(p) > 5 and p[4].replace(' ', '').endswith(tag):
            readier[p[5]] = p[2].split('(')[0].strip()
        continue
    new_proc, new, old_proc, old = p[2].replace(' ', ''), p[3], p[8].replace(' ', ''), p[9]
    if new_proc.endswith(tag) and new in out:
        at, reason = out.pop(new)
        ms = (ts - at) / 1000
        if reason in LOCKS and ms >= min_ms:
            who = readier.get(new, '?')
            t = total[(reason, who)]
            t[0] += 1; t[1] += ms; t[2] = max(t[2], ms)
            events.append((ms, (at - first) / 1e6, new, reason, who))
    if old_proc.endswith(tag) and p[12] == 'Waiting':
        out[old] = (ts, p[13])
print(f'kernel lock waits >= {min_ms:g} ms of process {pid}, by reason and readying process')
for (reason, who), (n, ms, mx) in sorted(total.items(), key=lambda kv: -kv[1][1]):
    print(f'  {reason:16s} {who:28s} {n:6d} waits  {ms:9.1f} ms total  max {mx:6.1f} ms')
print(f'longest {listed}:')
for ms, at, tid, reason, who in sorted(events, reverse=True)[:listed]:
    print(f'  {ms:7.1f} ms at {at:7.3f} s  tid {tid:>6s}  {reason:14s} readied by {who}')
