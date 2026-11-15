"""What one thread of the game spent its time on, per window of an ETW export: share of wall time in guest code,
in our runtime's lock / wait functions (with the file:line), other runtime code and system code, then the top
runtime call chains with the guest return address that called into the runtime.

    py -3.14 etw_thread_profile.py <export.csv> <tid> <name>:<from_s>-<to_s> [<name>:<from_s>-<to_s> ...]

Samples are 1 kHz, so the shares are of wall time (100% = never off the CPU). Example (jitter-1, a streak):
    py -3.14 etw_thread_profile.py j1-64.csv 24168 before:64.40-64.72 streak:64.725-64.885
"""
import collections
import sys
import etw_common as E

dump, tid = sys.argv[1], sys.argv[2]
wins = [(w.split(':')[0], *map(float, w.split(':')[1].split('-'))) for w in sys.argv[3:]]
pid = E.detect_pid(dump)
base, _, _ = E.runtime_base(E.samples(dump, pid))
smp = E.samples(dump, pid, tid)
sym = E.symbolize([a for _, _, s in smp for a in s[:16] if base <= a < base + 0xaca5000], base)
LOCKS = ('acquire', 'mutex_lock', 'futex', 'cond_wait', 'spin', 'lock_impl')
def category(s):
    if not s: return 'system'
    if E.GUEST_LO <= s[0] < E.GUEST_HI: return 'guest code'
    rt = next((sym[a] for a in s[:8] if a in sym), None)
    if not rt: return 'system'
    return f'runtime lock/wait ({rt[0]} @ {rt[1]})' if any(x in rt[0] for x in LOCKS) else 'runtime other'
for name, a, b in wins:
    sel = [s for ts, t, s in smp if a * 1e6 <= ts < b * 1e6]; ms = (b - a) * 1000
    cat = collections.Counter(category(s) for s in sel)
    print(f'== {name} ({ms:.0f} ms): busy {len(sel) / ms * 100:.0f}%')
    for k, v in cat.most_common(10): print(f'   {v / ms * 100:5.1f}%  {k}')
    chains = collections.Counter()
    for s in sel:
        if not s or E.GUEST_LO <= s[0] < E.GUEST_HI: continue
        rt = [sym[x][0][-48:] for x in s[:16] if x in sym][:4]
        if rt:
            g = next((x - E.GUEST_LO for x in s[:16] if E.GUEST_LO <= x < E.GUEST_HI), None)
            chains[(' <- '.join(rt), f'{g:#x}' if g is not None else '-')] += 1
    for (c, g), v in chains.most_common(8): print(f'      {v / ms * 100:4.1f}%  guest {g:>9}  {c}')
