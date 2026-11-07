"""APU clock / power trace beside a running game, on the Steam Deck (deck.sh apu-trace; no root).

Every --interval ms (default 4) one CSV row: CLOCK_MONOTONIC ns (the clock of the game's flip trace,
GOW3_FLIP_TRACE, so frame_clocks.py lines the two up), then from the amdgpu metrics table
(/sys/class/drm/card*/device/gpu_metrics, format 2.x: Van Gogh) the driver's fetch time, the GPU's
current and average clock and busy %, socket / CPU / SoC / GPU power (mW) and each CPU core's current
clock, the game process's total CPU time (all threads, utime + stime in ns, 10 ms resolution), then for
each traced thread the CPU it last ran on and its total run time (schedstat, ns). Files stay open and are
re-read with pread (the sampler cost 13-24% of a core at 4-8 ms when it reopened them every row).

Traced threads: --names (default stage A, stage B, the recorder, the vblank / present thread) and the
--top N busiest others over the first second (the game's own threads). Ends after --seconds or when
the game exits; prints how many rows the metrics table really changed on and the sampler's own CPU.

    python3 apu_trace.py [--seconds 60] [--interval 4] [--top 4] [--out FILE]
"""
import argparse
import glob
import os
import resource
import struct
import sys
import time

NAMES = ['shadPS4:GpuComm', 'gow3:DrawRec', 'gow3:VkRecorder', 'shadPS4:PresentThread']
# struct gpu_metrics_v2_x (linux/kgd_pp_interface.h), the same up to here for 2.2-2.4: offsets.
SYSTEM_CLOCK = 32                       # u64 ns, the driver's fetch time (boottime)
GFX_ACTIVITY = 28                       # u16 %
POWER = 40                              # u16 socket, cpu, soc, gfx (mW)
AVG_GFXCLK = 64                         # u16 MHz
CUR_GFXCLK = 76                         # u16 MHz
CUR_CORECLK = 88                        # u16[8] MHz
MIN_SIZE = CUR_CORECLK + 16


def metrics_path():
    paths = sorted(glob.glob('/sys/class/drm/card[0-9]/device/gpu_metrics'))
    if not paths:
        sys.exit('no gpu_metrics file')
    return paths[0]


_fds = {}


def pread(path, size=4096):
    """The file's current contents, from a descriptor kept open (procfs and sysfs regenerate at offset 0)."""
    fd = _fds.get(path)
    if fd is None:
        fd = _fds[path] = os.open(path, os.O_RDONLY)
    try:
        return os.pread(fd, size, 0)
    except OSError:
        os.close(_fds.pop(path))
        raise


def read_metrics(path):
    data = pread(path, 512)
    if len(data) < MIN_SIZE or data[2] != 2:
        sys.exit(f'gpu_metrics: format {data[2] if len(data) > 2 else "?"}.{data[3] if len(data) > 3 else "?"}, '
                 f'{len(data)} bytes: not 2.x (an APU)')
    return data


def game_pid():
    for pid in os.listdir('/proc'):
        if pid.isdigit():
            try:
                with open(f'/proc/{pid}/comm') as f:
                    if f.read().strip() == 'medievil-runtim':
                        return int(pid)
            except OSError:
                pass
    return None


def thread_info(pid, tid):
    """(name, cpu, run_ns), or None once the thread is gone."""
    try:
        stat = pread(f'/proc/{pid}/task/{tid}/stat').decode()
        run_ns = int(pread(f'/proc/{pid}/task/{tid}/schedstat').split()[0])
    except (OSError, ValueError, IndexError):
        return None
    name = stat[stat.index('(') + 1:stat.rindex(')')]
    cpu = int(stat[stat.rindex(')') + 2:].split()[36])  # field 39 (processor), after the comm
    return name, cpu, run_ns


TICK_NS = 1e9 / os.sysconf('SC_CLK_TCK')


def process_cpu_ns(pid):
    """The whole process's utime + stime (every thread), in ns."""
    try:
        stat = pread(f'/proc/{pid}/stat').decode()
    except OSError:
        return ''
    fields = stat[stat.rindex(')') + 2:].split()
    return int((int(fields[11]) + int(fields[12])) * TICK_NS)  # fields 14, 15 (utime, stime)


def pick_threads(pid, names, top):
    tids = [int(t) for t in os.listdir(f'/proc/{pid}/task')]
    before = {t: thread_info(pid, t) for t in tids}
    time.sleep(1.0)
    after = {t: thread_info(pid, t) for t in tids}
    chosen, seen = [], set()
    for want in names:
        for t in tids:
            if after[t] and after[t][0] == want and t not in seen:
                chosen.append(t)
                seen.add(t)
                break
    busy = sorted((after[t][2] - before[t][2], t) for t in tids
                  if t not in seen and before[t] and after[t] and t != os.getpid())
    for run, t in reversed(busy[-top:] if top else []):
        if run > 0:
            chosen.append(t)
    return [(t, after[t][0]) for t in chosen]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--seconds', type=float, default=60)
    parser.add_argument('--interval', type=float, default=4, help='ms between rows')
    parser.add_argument('--top', type=int, default=4, help='busiest other threads to trace too')
    parser.add_argument('--names', default=','.join(NAMES))
    parser.add_argument('--out', default=os.path.expanduser('~/.local/share/medievil-pc/user/apu-trace.csv'))
    parser.add_argument('--metrics', help="the metrics file (default: the first card's gpu_metrics)")
    args = parser.parse_args()
    path = args.metrics or metrics_path()
    first = read_metrics(path)
    cores = min(8, (os.cpu_count() or 8) // 2)
    pid = game_pid()
    if not pid:
        sys.exit('game not running')
    threads = pick_threads(pid, [n for n in args.names.split(',') if n], args.top)
    print(f'gpu_metrics {path} format {first[2]}.{first[3]}, {len(first)} bytes; {cores} cores; '
          f'game pid {pid}; threads: ' + ', '.join(f'{n} ({t})' for t, n in threads))
    columns = ['t_ns', 'smu_ns', 'gfx_mhz', 'gfx_avg_mhz', 'gfx_busy', 'socket_mw', 'cpu_mw', 'soc_mw',
               'gfx_mw'] + [f'core{c}_mhz' for c in range(cores)] + ['process.cpu_ns']
    for t, n in threads:
        columns += [f'{n}/{t}.cpu', f'{n}/{t}.run_ns']
    step = int(args.interval * 1e6)
    end = time.monotonic_ns() + int(args.seconds * 1e9)
    rows = changes = 0
    last_smu = None
    with open(args.out, 'w') as out:
        out.write(','.join(columns) + '\n')
        due = time.monotonic_ns()
        while due < end:
            now = time.monotonic_ns()
            if now < due:
                time.sleep((due - now) / 1e9)
            t_ns = time.monotonic_ns()
            data = read_metrics(path)
            smu = struct.unpack_from('<Q', data, SYSTEM_CLOCK)[0]
            changes += smu != last_smu
            last_smu = smu
            busy = struct.unpack_from('<H', data, GFX_ACTIVITY)[0]
            power = struct.unpack_from('<4H', data, POWER)
            avg_gfx = struct.unpack_from('<H', data, AVG_GFXCLK)[0]
            cur_gfx = struct.unpack_from('<H', data, CUR_GFXCLK)[0]
            core = struct.unpack_from(f'<{cores}H', data, CUR_CORECLK)
            row = [t_ns, smu, cur_gfx, avg_gfx, busy, *power, *core, process_cpu_ns(pid)]
            alive = False
            for t, _n in threads:
                info = thread_info(pid, t)
                alive |= info is not None
                row += [info[1], info[2]] if info else ['', '']
            out.write(','.join(map(str, row)) + '\n')
            rows += 1
            if not alive:
                print('game exited')
                break
            due += step
            if due < time.monotonic_ns() - step:  # fell behind (the sampler was not run): skip ahead
                due = time.monotonic_ns()
    usage = resource.getrusage(resource.RUSAGE_SELF)
    seconds = max(1e-9, (time.monotonic_ns() - (end - int(args.seconds * 1e9))) / 1e9)
    print(f'{rows} rows to {args.out}; metrics table changed on {changes} of them; sampler CPU '
          f'{(usage.ru_utime + usage.ru_stime) / seconds * 100:.1f}% of one core')


if __name__ == '__main__':
    main()
