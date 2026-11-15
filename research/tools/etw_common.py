"""Shared helpers for the ETW (WPR / xperf dumper) analysis tools: export a time range, read an export, find the
game's PID and the runtime's load address, symbolize runtime addresses, and line a flip trace up with an ETL.

The game's runtime is built with clang (DWARF, no PDB), so xperf shows its frames as raw addresses
(`medievil-runtime.exe!0x00007ff6...`); llvm-symbolizer resolves them against out\\medievil-runtime.exe once the
load address is known (`runtime_base`: the 64 KiB-aligned base under which the hottest addresses resolve to the
pipeline's known functions). Guest (eboot) code sits at 0x800000000 + offset.
"""
import collections
import csv
import datetime
import os
import re
import subprocess

XPERF = r'C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\xperf.exe'
SYMBOLIZER = r'C:\msys64\clang64\bin\llvm-symbolizer.exe'
RUNTIME = os.path.join(os.path.dirname(__file__), '..', '..', 'out', 'medievil-runtime.exe')
IMAGE_BASE = 0x140000000          # the exe's preferred base (llvm-objdump -p)
GUEST_LO, GUEST_HI = 0x800000000, 0x820000000


def export(etl, from_s, to_s, out_csv):
    """xperf dumper export of [from_s, to_s] (seconds from the trace start) with stacks; ~1 min for a 3 GB ETL.
    -tle -tti: tolerate lost events and time inversions (full CPU profiles report "corrupt" without them)."""
    subprocess.run([XPERF, '-tle', '-tti', '-i', etl, '-o', out_csv, '-symbols', '-a', 'dumper', '-stacktimeshifting',
                    '-range', str(int(from_s * 1e6)), str(int(to_s * 1e6))], check=True, capture_output=True)


def etl_span(etl):
    """(start, end) of the trace as local datetimes (xperf tracestats; 100 ns resolution)."""
    out = subprocess.run([XPERF, '-tle', '-tti', '-i', etl, '-a', 'tracestats'], capture_output=True, text=True).stdout
    get = lambda key: re.search(key + r'\s*:\s*(\d{4})/(\d\d)/(\d\d):(\d\d:\d\d:\d\d\.\d+)', out)
    def parse(m):
        return datetime.datetime.fromisoformat(f'{m.group(1)}-{m.group(2)}-{m.group(3)}T{m.group(4)[:15]}')
    return parse(get(r'Start time \(Local\)')), parse(get(r'End time \(Local\)'))


def rows(dump):
    """(kind, fields) for each event line after the header; fields stripped."""
    header = True
    with open(dump, errors='replace') as f:
        for line in f:
            if header:
                header = not line.startswith('EndHeader')
                continue
            k = line[:26].strip()
            if not k:
                continue
            yield k.split(',')[0], [x.strip() for x in line.split(',')]


def detect_pid(dump, name='medievil-runtime.exe'):
    c = collections.Counter()
    for kind, p in rows(dump):
        if kind == 'SampledProfile' and p[2].startswith(name):
            c[re.search(r'\(\s*(\d+)\)', p[2]).group(1)] += 1
            if sum(c.values()) > 2000:
                break
    return c.most_common(1)[0][0]


def samples(dump, pid, tid=None):
    """[(ts_us, tid, [addresses leaf first])] of the process's sampled stacks."""
    tag = f'({pid})'; out = []; cur = None
    for kind, p in rows(dump):
        if kind == 'SampledProfile':
            cur = None
            if p[2].replace(' ', '').endswith(tag) and (tid is None or p[3] == tid):
                cur = [int(p[1]), p[3], []]; out.append(cur)
        elif kind == 'Stack' and cur is not None and int(p[1]) == cur[0] and p[2] == cur[1]:
            cur[2].append(int(p[4], 16))
    return out


def symbolize(addrs, base, exe=RUNTIME):
    """{address: (function, file:line)} for runtime addresses loaded at `base`."""
    addrs = sorted(set(addrs))
    if not addrs:
        return {}
    out = subprocess.run([SYMBOLIZER, '--obj=' + exe, '-C', '--no-inlines', '--output-style=GNU'],
                         input='\n'.join(hex(IMAGE_BASE + a - base) for a in addrs), capture_output=True, text=True).stdout.split('\n')
    return {a: (out[2 * i].split('(')[0].split('.llvm')[0], os.path.basename(out[2 * i + 1].replace(chr(92), '/')))
            for i, a in enumerate(addrs)}


KNOWN = re.compile(r'DrawPipe|RecorderThread|Liverpool|Rasterizer|Scheduler|BufferCache|TextureCache|PipelineCache|'
                   r'posix_cond|futex|mutex_lock|thread_proxy|runtime_memory|DrawPreparation|ConstantRing|CopySparse|cached_region')


def runtime_base(sample_list, exe=RUNTIME, size=0xaca5000):
    """The runtime's load address: of the 64 KiB-aligned candidates, the one under which most of the 40 hottest
    runtime-looking addresses resolve to known pipeline functions. Returns (base, hits, of)."""
    cnt = collections.Counter(a for _, _, st in sample_list for a in st if 0x7ff000000000 <= a < 0x800000000000)
    top = [a for a, _ in cnt.most_common(40)]
    lo = min(cnt)
    bases = list(range((lo - 0x1000) & ~0xffff, max(0, lo - size) - 1, -0x10000))
    out = subprocess.run([SYMBOLIZER, '--obj=' + exe, '-C', '--no-inlines', '--output-style=GNU'],
                         input='\n'.join(hex(IMAGE_BASE + a - b) for b in bases for a in top), capture_output=True, text=True).stdout.split('\n')[0::2]
    best = max(((sum(bool(KNOWN.search(n)) for n in out[i * len(top):(i + 1) * len(top)]), b) for i, b in enumerate(bases)))
    return best[1], best[0], len(top)


def flip_to_etw(flip_csv, etl_start_local):
    """A function flip-trace ns -> ETW us (since the ETL start), from the trace's `wall` row (flip-trace card)."""
    with open(flip_csv, newline='') as f:
        r = csv.reader(f)
        next(r); next(r)
        wall = next(r)
    if wall[0] != 'wall':
        raise SystemExit('flip trace has no wall row (built before d032cd4): exact alignment needs it')
    wall_ns, wall_at = int(wall[1]), int(wall[2])
    etl0 = etl_start_local.timestamp() * 1e9
    return lambda ns: (wall_ns + ns - wall_at - etl0) / 1e3


def flip_frames(flip_csv, stages=('submit', 'irq', 'queued', 'gpu_ready', 'present_start', 'presented')):
    """{flip_arg: {stage: ns}} with every listed stage present."""
    ev = collections.defaultdict(dict)
    with open(flip_csv, newline='') as f:
        r = csv.reader(f); next(r)
        for st, arg, ns in r:
            if st in stages:
                ev[int(arg)].setdefault(st, int(ns))
    return {a: e for a, e in ev.items() if len(e) == len(stages)}
