"""Call stacks of one process's CPU samples from an xperf dumper export with stacks
(xperf -tle -tti -i x.etl -symbols -o x.csv -a dumper -stacktimeshifting).

    py -3.14 etw_stacks.py <dump.csv> <pid> [--guest-base 0x800000000] [--through 0x689c89]
                          [--depth 8] [--top 15]

Each SampledProfile is followed by Stack records (same timestamp and thread, frame 1 = leaf). Prints
per thread the samples, and the most frequent stacks (guest addresses as guest+offset = eboot image
offsets). --through keeps only samples whose stack contains that guest offset (e.g. a wait site's
return address) and also counts, per thread, how many samples are inside it.
"""
import argparse
import collections
import re
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump')
    ap.add_argument('pid')
    ap.add_argument('--guest-base', type=lambda v: int(v, 16), default=0x800000000)
    ap.add_argument('--through', type=lambda v: int(v, 16))
    ap.add_argument('--depth', type=int, default=8)
    ap.add_argument('--top', type=int, default=15)
    a = ap.parse_args()
    proc = re.compile(r'\(\s*' + re.escape(a.pid) + r'\)')
    base, end = a.guest_base, a.guest_base + 0x20000000

    def name(addr, func):
        if base <= addr < end:
            return f'guest+0x{addr - base:x}'
        return func.split('!')[0].replace('.dll', '').replace('.exe', '') + '!' + func.split('!', 1)[-1] if '!' in func else func

    per_thread = collections.Counter()
    through_thread = collections.Counter()
    stacks = collections.Counter()
    current = None  # (ts, tid, frames)

    def flush():
        if not current:
            return
        ts, tid, frames = current
        per_thread[tid] += 1
        names = [name(addr, func) for addr, func in frames]
        if a.through is not None:
            if not any(base <= addr < end and addr - base == a.through for addr, _ in frames):
                return
            through_thread[tid] += 1
        stacks[(tid, ' <- '.join(names[:a.depth]))] += 1

    with open(a.dump, encoding='utf-8', errors='replace') as f:
        for line in f:
            kind = line[:24].strip().split(',', 1)[0]
            if kind == 'SampledProfile':
                flush()
                p = [x.strip() for x in line.split(',')]
                current = (p[1], int(p[3]), []) if proc.search(p[2]) else None
            elif kind == 'Stack' and current:
                p = [x.strip() for x in line.split(',', 5)]
                if p[1] != current[0] or int(p[2]) != current[1]:
                    continue
                current[2].append((int(p[4], 16), p[5]))
            elif kind in ('CSwitch', 'ReadyThread', 'T-DCStart', 'I-DCStart'):
                pass
        flush()
    total = sum(per_thread.values())
    print(f'{total} samples of pid {a.pid}; guest base 0x{base:x}')
    if a.through is not None:
        print(f'samples through guest+0x{a.through:x}: {sum(through_thread.values())}')
        for tid, n in through_thread.most_common(8):
            print(f'  thread {tid}: {n} of its {per_thread[tid]} samples ({n * 100 / per_thread[tid]:.0f}%)')
    print('most frequent stacks (leaf first):')
    for (tid, s), n in stacks.most_common(a.top):
        print(f'{n:7d}  [{tid}] {s}')


if __name__ == '__main__':
    sys.exit(main())
