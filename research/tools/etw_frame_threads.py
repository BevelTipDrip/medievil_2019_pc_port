"""Per-frame thread behaviour around slow frames: one row per frame of a flip trace inside an ETW export, with
the frame and game times, stage A's time, and per busy thread of the game the ms it ran in that game frame (r)
and the ms between being readied and running (d, scheduling delay), plus all CPUs' DPC / interrupt ms.

    py -3.14 etw_frame_threads.py <export.csv> <trace.etl> <flip-trace.csv> <from_s> <to_s> [slow_ms 5.3]

<from_s>-<to_s> are ETW seconds (the export's own times, from the ETL start). Needs the flip trace's `wall` row
and a light profile (wpr-jitter.wprp: switches without stacks); the built-in CPU profile is too heavy (wpr-capture
card). Thread roles come from each thread's sampled functions (stageA, stageB, recorder, prep, guest, ?).
"""
import bisect
import collections
import sys
import etw_common as E

dump, etl, flip, t0, t1 = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4]), float(sys.argv[5])
slow_ms = float(sys.argv[6]) if len(sys.argv) > 6 else 5.3
pid = E.detect_pid(dump)
smp = E.samples(dump, pid)
base, hits, of = E.runtime_base(smp)
print(f'pid {pid}, runtime base {base:#x} ({hits}/{of} hot addresses resolve)')
tag = f'({pid})'
on = collections.defaultdict(list); running = {}; readies = collections.defaultdict(list); dpc = []
for kind, p in E.rows(dump):
    if kind == 'CSwitch':
        ts, cpu = int(p[1]), int(p[16])
        if cpu in running:
            tid, s = running.pop(cpu); on[tid].append((s, ts))
        if p[2].replace(' ', '').endswith(tag):
            running[cpu] = (p[3], ts)
    elif kind == 'ReadyThread' and p[4].replace(' ', '').endswith(tag):
        readies[p[5]].append(int(p[1]))
    elif kind in ('DPC', 'Interrupt', 'DPCTmr'):
        dpc.append((int(p[1]), float(p[2]), kind, p[-1].split('!')[0], int(p[3])))
sym = E.symbolize([a for _, _, s in smp for a in s[:10] if base <= a < base + 0xaca5000], base)
fn_of = collections.defaultdict(collections.Counter)
for _, tid, s in smp:
    n = next((sym[a][0] for a in s[:10] if a in sym), None)
    fn_of[tid][n or ('guest code' if any(E.GUEST_LO <= a < E.GUEST_HI for a in s[:10]) else 'system')] += 1
def role(tid):
    txt = ' '.join(fn_of[tid])
    for pat, r in (('DrawPipe::Run', 'stageB'), ('RecorderThread', 'recorder'), ('Liverpool::Process', 'stageA'),
                   ('ProcessGraphics', 'stageA'), ('PrepareGraphicsPipeline', 'prep'), ('GpuReadyThread', 'gpuready')):
        if pat in txt:
            return r
    return 'guest' if fn_of[tid] and fn_of[tid].most_common(1)[0][0] == 'guest code' else '?'
to_us = E.flip_to_etw(flip, E.etl_span(etl)[0])
fr = E.flip_frames(flip)
frames = sorted((a for a, e in fr.items() if t0 * 1e6 <= to_us(e['presented']) <= t1 * 1e6), key=lambda a: fr[a]['presented'])
key = sorted([t for t in on if sum(e - s for s, e in on[t]) > 0.15 * (t1 - t0) * 1e6], key=lambda t: -sum(e - s for s, e in on[t]))[:9]
ins = {t: sorted(s for s, e in on[t]) for t in key}
print('threads:', '; '.join(f'{role(t)}:{t} [{", ".join(n for n, _ in fn_of[t].most_common(2))}]' for t in key))
print(f'\n{"ETW t":>10} {"frame":>6} {"game":>6} {"A":>5} | ' + ' '.join(f'{(role(t) + ":" + t)[:13]:>13}' for t in key) + ' | DPC+ISR ms, largest')
for i in range(1, len(frames)):
    e, p = fr[frames[i]], fr[frames[i - 1]]
    fa, fb = to_us(p['submit']), to_us(e['submit'])
    ft, game = (e['presented'] - p['presented']) / 1e6, (e['submit'] - p['submit']) / 1e6
    cells = []
    for t in key:
        r = sum(max(0, min(b, fb) - max(a, fa)) for a, b in on[t]) / 1000
        d = 0.0
        for ts in readies[t]:
            if fa <= ts < fb:
                j = bisect.bisect_left(ins[t], ts)
                if j < len(ins[t]): d += (ins[t][j] - ts) / 1000
        cells.append(f'{r:5.2f}r{d:4.2f}d')
    win = [x for x in dpc if fa <= x[0] < fb]
    big = max(win, default=None, key=lambda x: x[1])
    mark = ' <== SLOW' if ft > slow_ms or game > slow_ms + 0.5 else ''
    print(f'{to_us(e["presented"]) / 1e6:10.4f} {ft:6.2f} {game:6.2f} {(e["irq"] - e["submit"]) / 1e6:5.2f} | ' + ' '.join(f'{c:>13}' for c in cells)
          + f' | {sum(x[1] for x in win) / 1000:4.2f} ' + (f'{big[2]} {big[3]} {big[1]:.0f}us cpu{big[4]}' if big else '') + mark)
