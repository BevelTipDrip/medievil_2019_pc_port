"""Off-CPU time per second of chosen threads, by the wait reason of their last switch-out, and the
threads that readied them, from the CSwitch / ReadyThread records of an xperf dumper export.

    py -3.14 etw_waits.py <dump.csv> <tid>=<name> [<tid>=<name> ...] [--seconds 10]
"""
import collections, sys
dump = sys.argv[1]
args = [a for a in sys.argv[2:] if '=' in a]
T = dict(a.split('=', 1) for a in args)
limit = int(sys.argv[sys.argv.index('--seconds') + 1]) if '--seconds' in sys.argv else 10
out_reason = {}                      # tid -> wait reason at its last switch-out
offcpu = collections.defaultdict(collections.Counter)    # (sec, thread) -> reason -> ms
readier = collections.defaultdict(collections.Counter)   # (sec, thread) -> readying thread
for line in open(dump, encoding='utf-8', errors='replace'):
    k = line[:30].strip()
    if k.startswith('CSwitch,'):
        p = [x.strip() for x in line.split(',')]
        try: ts = int(p[1])
        except ValueError: continue
        s = ts // 1000000
        if s >= limit: break
        new, old = p[3], p[9]
        if new in T:
            # WaitTime (us): how long it waited since it was readied; TmSinceLast: since it last ran
            offcpu[(s, T[new])][out_reason.get(new, '?')] += int(p[6]) / 1000
        if old in T and p[12] == 'Waiting':
            out_reason[old] = p[13]
    elif k.startswith('ReadyThread,'):
        p = [x.strip() for x in line.split(',')]
        try: ts = int(p[1])
        except ValueError: continue
        s = ts // 1000000
        if p[5] in T:
            readier[(s, T[p[5]])][p[2].split('(')[0].strip()[:14] + ':' + p[3]] += 1
for name in T.values():
    print(f'== {name}: off-CPU ms per second by wait reason (time since it last ran)')
    for s in range(limit):
        o = offcpu[(s, name)]
        print(f'  {s}s  total {sum(o.values()):6.0f}  ' + ', '.join(f'{r} {v:.0f}' for r, v in o.most_common(4))
              + '  | readied by: ' + ', '.join(f'{t} {c}' for t, c in readier[(s, name)].most_common(3)))
