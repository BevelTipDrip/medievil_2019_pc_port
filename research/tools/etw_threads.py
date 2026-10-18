"""Per-thread CPU of one process from an xperf dumper export (xperf -i x.etl -o x.csv -a dumper).

    py -3.14 etw_threads.py <dump.csv> <pid> [--exe out/medievil-runtime.exe] [--top 30]

SampledProfile events (1 kHz per CPU) are counted per thread and per module of the sampled address
("Unknown" = no Windows image: the game's own PS4 code in this port). Thread start addresses
(T-DCStart / T-Start) inside the executable are named with llvm-symbolizer, which tells the port's
threads apart (the trace has no thread names). Cores = samples / 1000 / seconds.
"""
import argparse
import collections
import re
import shutil
import subprocess
import sys

ADDR = re.compile(r'([^!,]+)!(0x[0-9a-fA-F]+|"Unknown"|[^,]+)')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump')
    ap.add_argument('pid')
    ap.add_argument('--exe', default='out/medievil-runtime.exe')
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--functions', type=int, default=0,
                    help='also list the N busiest functions of each of the first --threads threads '
                         '(needs a dump made with -symbols for Windows modules)')
    ap.add_argument('--threads', type=int, default=8)
    a = ap.parse_args()
    proc = re.compile(r'\(\s*' + re.escape(a.pid) + r'\)')
    samples = collections.Counter()
    modules = collections.defaultdict(collections.Counter)
    functions = collections.defaultdict(collections.Counter)
    starts, image_base = {}, None
    first = last = None
    with open(a.dump, encoding='utf-8', errors='replace') as f:
        for line in f:
            kind = line.split(',', 1)[0].strip()
            if kind not in ('SampledProfile', 'T-DCStart', 'T-Start', 'I-DCStart', 'I-Load'):
                continue
            p = [x.strip() for x in line.split(',')]
            if not proc.search(p[2]):
                continue
            if kind == 'SampledProfile':
                tid, ts = int(p[3]), int(p[1])
                first = ts if first is None else first
                last = ts
                samples[tid] += 1
                # ..., ThreadStartImage!Function, Image!Function, Count, SampledProfile type: names
                # may hold commas (templates), so the sampled function is read from the right.
                where = p[-3]
                functions[tid][where] += 1
                modules[tid][where.split('!')[0].strip('"') if '!' in where else where] += 1
            elif kind in ('T-DCStart', 'T-Start'):
                starts.setdefault(int(p[3]), p[-1])
            elif image_base is None and len(p) > 8 and p[8].strip('"').lower().endswith('medievil-runtime.exe'):
                # I-DCStart / I-Load: ImageBase, ImageEnd, Checksum, TimeDateStamp, DefaultBase, FileName
                image_base = int(p[3], 16)
    seconds = (last - first) / 1e6 if first is not None and last > first else 1.0
    names = {}
    tool = shutil.which('llvm-symbolizer') or r'C:\msys64\clang64\bin\llvm-symbolizer.exe'
    wanted = {tid: s for tid, s in starts.items() if 'medievil-runtime.exe!0x' in s}
    if wanted and image_base:
        pe_base = 0x140000000
        query = '\n'.join(f'0x{int(s.split("!")[1], 16) - image_base + pe_base:x}' for s in wanted.values())
        out = subprocess.run([tool, f'--obj={a.exe}', '--functions=linkage', '--demangle', '--no-inlines'],
                             input=query + '\n', capture_output=True, text=True).stdout.split('\n\n')
        for tid, block in zip(wanted, out):
            names[tid] = block.strip().split('\n')[0][:70]
    total = sum(samples.values())
    print(f'{total} samples over {seconds:.1f} s = {total / 1000 / seconds:.2f} cores; '
          f'{len(samples)} threads with samples, image base {hex(image_base) if image_base else "?"}')
    print(f'{"tid":>6} {"cores":>6}  {"game code":>9}  top modules  |  start')
    for tid, n in samples.most_common(a.top):
        game = modules[tid].get('Unknown', 0)
        top = ', '.join(f'{m} {c * 100 // n}%' for m, c in modules[tid].most_common(3))
        start = names.get(tid) or starts.get(tid, '?').split('!')[-1][:40]
        print(f'{tid:6d} {n / 1000 / seconds:6.2f}  {game * 100 / n:8.0f}%  {top[:60]:60}|  {start}')
    if not a.functions:
        return
    # The port's own addresses: grouped by function name (llvm-symbolizer), the rest as exported.
    runtime_addrs = sorted({w.split('!')[1] for tid, _ in samples.most_common(a.threads)
                            for w in functions[tid] if w.startswith('medievil-runtime.exe!0x')})
    named = {}
    if runtime_addrs and image_base:
        query = '\n'.join(f'0x{int(x, 16) - image_base + 0x140000000:x}' for x in runtime_addrs)
        out = subprocess.run([tool, f'--obj={a.exe}', '--functions=linkage', '--demangle', '--no-inlines'],
                             input=query + '\n', capture_output=True, text=True).stdout.split('\n\n')
        for addr, block in zip(runtime_addrs, out):
            named[addr] = 'runtime!' + block.strip().split('\n')[0].split('(')[0][:80]
    for tid, n in samples.most_common(a.threads):
        grouped = collections.Counter()
        for where, c in functions[tid].items():
            if where.startswith('medievil-runtime.exe!0x'):
                where = named.get(where.split('!')[1], 'runtime!?')
            elif where.startswith('"Unknown"!'):
                where = 'game code (PS4 image)'
            grouped[where] += c
        print(f'\n== thread {tid}: {n / 1000 / seconds:.2f} cores')
        for where, c in grouped.most_common(a.functions):
            print(f'   {c * 100 / n:5.1f}%  {where[:110]}')


if __name__ == '__main__':
    sys.exit(main())
