"""Who holds the guest mutexes threads wait on, and what the holder does meanwhile (card tooling/mutex-holders).

    py -3.14 mutex_holders.py <wait-trace.csv> [--site guest+0x6795a1] [--top 12]

Reads a wait trace written with GOW3_WAIT_TRACE=<file> GOW3_MUTEX_HOLDERS=1 (dev build): m rows (a long mutex wait
and its holder), h rows (long holds), s rows (the holder sampled while someone waits on it, Windows), the exe row
(exe+0x... offsets are named with llvm-symbolizer from that exe's debug info).
  1. mutex waits by waiter site: count, total, median / p90, the holders and their lock sites
  2. the chosen site (--site, default the one with the most wait time): holders, lock sites, hold so far
  3. long holds by thread and lock / unlock site
  4. samples of that site's holders: where the holder is (pc), the port function it is in or under (first exe
     frame), and the first game frame. Frames come from Windows' unwinder (exe and DLL code) and the rbp chain (game
     code); frames marked ~ come from a stack scan when those stopped early, and can be stale.
Guest sites are eboot offsets (Ghidra: add 0x400000)."""
import argparse, collections, csv, os, shutil, statistics, subprocess

ap = argparse.ArgumentParser()
ap.add_argument('trace'); ap.add_argument('--site'); ap.add_argument('--top', type=int, default=12)
args = ap.parse_args()

exe = None
m_rows, h_rows, s_rows = [], [], []
with open(args.trace, newline='', encoding='utf-8', errors='replace') as f:
    for r in csv.reader(f):
        if not r: continue
        if r[0] == 'exe' and len(r) >= 3: exe = r[1]
        elif r[0] == 'm' and len(r) >= 10:
            m_rows.append(dict(end=int(r[1]), waiter=r[2], wname=r[3], wsite=r[4], holder=r[5], hname=r[6], hsite=r[7],
                               held=int(r[8]), waited=int(r[9])))
        elif r[0] == 'h' and len(r) >= 7:
            h_rows.append(dict(end=int(r[1]), tid=r[2], name=r[3], lock=r[4], unlock=r[5], held=int(r[6])))
        elif r[0] == 's' and len(r) >= 4:
            s_rows.append(dict(t=int(r[1]), tid=r[2], frames=r[3:]))
if not m_rows:
    raise SystemExit('No m rows: was the trace written with GOW3_MUTEX_HOLDERS=1 on a dev build?')
span = (max(x['end'] for x in m_rows + h_rows) - min(x['end'] for x in m_rows + h_rows)) / 1e9 or 1

# exe+0x... -> function names
symbolizer = shutil.which('llvm-symbolizer') or r'C:\msys64\clang64\bin\llvm-symbolizer.exe'
names = {}
def name_all(labels):
    offs = sorted({l.lstrip('~') for l in labels if l.lstrip('~').startswith('exe+0x') and l.lstrip('~') not in names})
    if not offs or not exe or not os.path.isfile(symbolizer):
        return
    exe_path = exe if os.path.isfile(exe) else exe.replace('/', '\\')
    out = subprocess.run([symbolizer, '--obj=' + exe_path, '--no-inlines', '--functions=linkage', '--demangle'],
                         input='\n'.join(hex(0x140000000 + int(o[4:], 16)) for o in offs), capture_output=True, text=True).stdout
    blocks = [b for b in out.split('\n\n') if b.strip()]
    for o, b in zip(offs, blocks):
        fn = b.split('\n')[0].split('.llvm.')[0]
        names[o] = fn if fn != '??' else o
def label(l):
    mark = '~' if l.startswith('~') else ''
    return mark + names.get(l.lstrip('~'), l.lstrip('~'))

def ms(ns): return ns / 1e6
def stats(v):
    v = sorted(v)
    return statistics.median(v), v[min(len(v) - 1, int(len(v) * 0.9))]

print(f'{args.trace}: {len(m_rows)} long mutex waits, {len(h_rows)} long holds, {len(s_rows)} holder samples, '
      f'{span:.0f} s, exe {exe or "?"}')
print('\n1. Mutex waits by waiter site')
by_site = collections.defaultdict(list)
for x in m_rows: by_site[x['wsite']].append(x)
order = sorted(by_site, key=lambda s: -sum(x['waited'] for x in by_site[s]))
for s in order[:args.top]:
    xs = by_site[s]; med, p90 = stats([x['waited'] for x in xs])
    waiters = collections.Counter(x['wname'] for x in xs).most_common(2)
    holders = collections.Counter(x['hname'] + '@' + x['hsite'] for x in xs).most_common(3)
    print(f'  {s:<18} {len(xs) / span:7.1f}/s  {ms(sum(x["waited"] for x in xs)) / span:7.1f} ms/s  median {ms(med):6.2f} ms  '
          f'p90 {ms(p90):6.2f}  waiters {", ".join(f"{n} {c}" for n, c in waiters)}')
    print(f'  {"":<18} holders: ' + ', '.join(f'{n} ({c})' for n, c in holders))

site = args.site or order[0]
xs = by_site.get(site, [])
print(f'\n2. {site}: {len(xs)} waits')
pairs = collections.defaultdict(list)
for x in xs: pairs[(x['hname'], x['holder'], x['hsite'])].append(x)
for (hn, ht, hs), v in sorted(pairs.items(), key=lambda kv: -len(kv[1]))[:args.top]:
    med, p90 = stats([x['waited'] for x in v]); hmed, _ = stats([x['held'] for x in v])
    print(f'  holder {hn} (tid {ht}) locked at {hs}: {len(v)} waits, waited median {ms(med):.2f} ms p90 {ms(p90):.2f}; '
          f'held {ms(hmed):.2f} ms when the wait began (median)')

print('\n3. Long holds by thread and lock / unlock site')
holds = collections.defaultdict(list)
for x in h_rows: holds[(x['name'], x['lock'], x['unlock'])].append(x['held'])
for (n, l, u), v in sorted(holds.items(), key=lambda kv: -sum(kv[1]))[:args.top]:
    med, p90 = stats(v)
    print(f'  {n:<24} {l:<18} -> {u:<18} {len(v) / span:7.1f}/s  median {ms(med):6.2f} ms  p90 {ms(p90):6.2f}  '
          f'{ms(sum(v)) / span:7.1f} ms/s')

tids = {x['holder'] for x in xs}
tid_names = {x['holder']: x['hname'] for x in m_rows}
samples = [s for s in s_rows if s['tid'] in tids]
print(f'\n4. Samples of the holders of {site}: {len(samples)}')
if samples:
    name_all([f for s in samples for f in s['frames']])
    def first(frames, pred):
        return next((f for f in frames if pred(f.lstrip('~'))), '-')
    def module(f):
        f = f.lstrip('~')
        return 'game code' if f.startswith('guest+') else label(f) if f.startswith('exe+') else f.split('+')[0]
    for title, key in (('where the holder is (module; exe: function)', lambda fr: module(fr[0])),
                       ('where the holder is (pc)', lambda fr: label(fr[0])),
                       ('port function it is in or under (first exe frame)', lambda fr: label(first(fr, lambda f: f.startswith('exe+')))),
                       ('first game frame', lambda fr: first(fr, lambda f: f.startswith('guest+'))),
                       ('pc + first exe frame', lambda fr: label(fr[0]) + '  <-  ' + label(first(fr[1:], lambda f: f.startswith('exe+'))))):
        print(f'  {title}:')
        for k, c in collections.Counter(key(s['frames']) for s in samples).most_common(args.top):
            print(f'    {100 * c / len(samples):5.1f}%  {k}')
    by_tid = collections.Counter(s['tid'] for s in samples)
    print('  holders sampled: ' + ', '.join(f'{tid_names.get(t, t)} (tid {t}) {c}' for t, c in by_tid.most_common(6)))
