#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Startup graphics options of medievil.ini (gpu/shim/gow3_graphics.h) for run.py.

The in-game menu and the launcher write them; run.py turns them into game patches and
environment variables at every start, so "Apply and restart" uses the new values. After a
restart the game confirms them (restart_unconfirmed=0) once it shows frames; when a launch
never did, the next start goes back to the last confirmed values.
"""
import json
from pathlib import Path

RESOLUTIONS = ('native', '480p', '720p', '1440p', '1800p', '4K',
               '720p_native', '1440p_native', '1800p_native', '4K_native')
ENGINE_FPS = (60, 120, 240)
DEFAULTS = {'render_resolution': 'native', 'engine_fps': '60', 'deferred_readback': '1',
            'stale_readback': '1', 'async_shaders': '1'}
RESOLUTION_PREFIX = 'Resolution Patch'
FPS_PREFIX = 'Frame Rate Patch'
TEXTURE_FIX = 'Bug Fix - Texture Corruption Fix'


def read_ini(path):
    values = {}
    try:
        lines = Path(path).read_text(encoding='utf-8').splitlines()
    except OSError:
        return values
    for line in lines:
        if '=' in line and not line.lstrip().startswith('#'):
            key, value = line.split('=', 1)
            values[key.strip()] = value.strip()
    return values


def write_keys(path, updates):
    """Rewrites `updates` in place and appends missing keys, keeping every other line."""
    path = Path(path)
    try:
        lines = path.read_text(encoding='utf-8').splitlines()
    except OSError:
        lines = ['# MediEvil settings (in-game menu: Insert / R3+L2)']
    done, out = set(), []
    for line in lines:
        key = line.split('=', 1)[0].strip() if '=' in line and not line.lstrip().startswith('#') else None
        if key in updates:
            out.append(f'{key}={updates[key]}')
            done.add(key)
        else:
            out.append(line)
    out += [f'{k}={v}' for k, v in updates.items() if k not in done]
    path.write_text('\n'.join(out) + '\n', encoding='utf-8')


def startup_options(ini):
    """The startup options, normalized; unknown values fall back to the defaults."""
    options = {key: ini.get(key, value) for key, value in DEFAULTS.items()}
    if options['render_resolution'] not in RESOLUTIONS:
        options['render_resolution'] = 'native'
    if options['engine_fps'] not in {str(f) for f in ENGINE_FPS}:
        options['engine_fps'] = '60'
    for key in ('deferred_readback', 'stale_readback', 'async_shaders'):
        options[key] = '0' if options[key] == '0' else '1'
    return options


def prepare(config, state_path):
    """(options, reverted): the startup options of this launch, restoring the last confirmed
    ones when the previous launch after a restart never confirmed its own."""
    ini = read_ini(config)
    state = {}
    try:
        state = json.loads(Path(state_path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        pass
    options = startup_options(ini)
    reverted = False
    if ini.get('restart_unconfirmed') == '1':
        if state.get('attempt') and state.get('last_good'):
            options = startup_options(state['last_good'])
            write_keys(config, {**options, 'restart_unconfirmed': '0'})
            state['attempt'] = False
            reverted = True
        else:
            state['attempt'] = True
    else:
        state = {'last_good': options, 'attempt': False}
    Path(state_path).write_text(json.dumps(state, indent=2), encoding='utf-8')
    return options, reverted


def patch_names(names, options, texture_fix_available=True):
    """The launcher's patch list with the resolution and frame rate patches of `options`."""
    names = [n for n in names if not n.startswith(RESOLUTION_PREFIX) and not n.startswith(FPS_PREFIX)]
    resolution = options['render_resolution']
    if resolution != 'native':
        names = [n for n in names if n != TEXTURE_FIX] + [f'{RESOLUTION_PREFIX} - {resolution}']
    elif texture_fix_available and TEXTURE_FIX not in names:
        names.insert(0, TEXTURE_FIX)
    if options['engine_fps'] != '60':
        names.append(f'{FPS_PREFIX} - {options["engine_fps"]} FPS')
    return names


# Performance patches (launcher > Performance, in-game menu > Performance): medievil.ini key ->
# the environment that switches the patch off (perf_*=0). Missing keys mean on.
PERF_SWITCHES = {
    'perf_hzb_occlusion': {'GOW3_HZB_REMAP': '0'},
    'perf_no_decal_prepass': {'GOW3_DECAL_PREPASS': '1'},
    'perf_particle_single_pass': {'GOW3_PARTICLE_FIXED_STEP': '1'},
    'perf_small_buffers': {'GOW3_STREAM_CLEAN': '0'},
    'perf_texture_memo': {'GOW3_TEXSET_REGIONS': '0', 'GOW3_TEXSET_TWO_WAY': '0'},
    'perf_lockfree_memory': {'GOW3_OPTIMISTIC_CLAMP': '0', 'GOW3_REGION_BUCKETS': '0'},
    'perf_cheap_frees': {'GOW3_DECOMMIT_KEEP_ACCESS': '0'},
    'perf_gpu_data_no_waits': {'GOW3_READBACK_QUEUE': '0', 'GOW3_GPU_WRITE_TWINS': '0',
                               'GOW3_EOS_DEFERRED': '0', 'GOW3_EOP_TARGETED': '0'},
    'perf_4k_memory': {'GOW3_PS4_MEMORY': '1'},
}


# Presentation settings (launcher > Performance > Presentation): medievil.ini key -> how its value
# becomes environment variables (run.py; a variable already set wins, for tests). All default off.
PRESENT_MODES = ('auto', 'fifo', 'fifo_relaxed', 'mailbox', 'immediate')
PRESENT_FLAGS = {
    'present_no_frame_caps': {'GOW3_NO_FRAME_CAP': '1'},
    'present_inline': {'GOW3_PRESENT_THREAD': '0'},
    'present_precise_limit': {'GOW3_PRECISE_LIMIT': '1'},
    'present_newest_frame': {'GOW3_NEWEST_FRAME': '1'},
    'present_precise_tick': {'GOW3_PRECISE_TICK': '1'},
    'present_vblank_sync': {'GOW3_VBLANK_SYNC': '1'},
}


def present_environment(ini, trace_path=None):
    """The environment of the presentation settings in medievil.ini."""
    env = {}
    for key, on in PRESENT_FLAGS.items():
        if ini.get(key, '0') == '1':
            env.update(on)
    try:
        tick = int(ini.get('present_tick_hz', '0'))
    except ValueError:
        tick = 0
    if 1 <= tick <= 2000:
        env['GOW3_VBLANK_HZ'] = str(tick)
    mode = ini.get('present_mode', 'auto')
    if mode in PRESENT_MODES and mode != 'auto':
        env['GOW3_PRESENT_MODE'] = mode
    if ini.get('present_frame_trace', '0') == '1' and trace_path:
        env['GOW3_FLIP_TRACE'] = str(trace_path)
    return env


def no_frame_caps(ini):
    """Built-in frame caps off: the engine's tick-rate bound and the present thread's limit."""
    return ini.get('present_no_frame_caps', '0') == '1'


def perf_environment(ini):
    """The switches of the performance patches turned off in medievil.ini."""
    env = {}
    for key, off in PERF_SWITCHES.items():
        if ini.get(key, '1') == '0':
            env.update(off)
    return env


def environment(options):
    return {'GOW3_DEFERRED_READBACK': options['deferred_readback'],
            'GOW3_STALE_READBACK': options['stale_readback'],
            'GOW3_ASYNC_SHADERS': options['async_shaders']}
