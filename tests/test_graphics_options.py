import json
import tempfile
import unittest
from pathlib import Path

import paths  # noqa: F401  (puts scripts/ on sys.path)
import graphics_options as g


class GraphicsOptionsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.ini = Path(self.temp.name) / 'gow3.ini'
        self.state = Path(self.temp.name) / 'state.json'

    def tearDown(self):
        self.temp.cleanup()

    def test_defaults_and_invalid_values(self):
        self.ini.write_text('render_resolution=8K\nengine_fps=90\nstale_readback=yes\n', encoding='utf-8')
        options, reverted = g.prepare(self.ini, self.state)
        self.assertFalse(reverted)
        self.assertEqual(options, {'render_resolution': 'native', 'engine_fps': '60', 'deferred_readback': '1',
                                   'stale_readback': '1', 'async_shaders': '1', 'output_resolution': '1920x1080',
                                   'render_scale': '100'})

    def test_output_resolution_and_render_scale(self):
        self.assertEqual([g.parse_output(v) for v in ('2560x1440', ' 3441X1440 ', 'display', '8K', '100x100')],
                         ['2560x1440', '3440x1440', 'display', '1920x1080', '1920x1080'])
        self.assertEqual([g.parse_render_scale(v) for v in ('67', '200', '201', 'x')], [67, 200, 100, 100])
        self.assertEqual(g.output_size('1280x720'), (1280, 720))

    def test_output_environment(self):
        pool, boot4k = g.POOL_PATCH, g.BOOT_4K_PATCH
        self.assertEqual(g.output_environment({}, 1920, 1080), {'GOW3_PATCHES': ''})
        self.assertEqual(g.output_environment({'GOW3_PATCHES': f'Skip intro;{boot4k}'}, 2560, 1440),
                         {'GOW3_PATCHES': f'Skip intro;{pool};Boot display size 2560x1440', 'GOW3_DMEM_MB': '7104',
                          'GOW3_INTERNAL_WIDTH': '2560', 'GOW3_INTERNAL_HEIGHT': '1440'})
        self.assertEqual(g.output_environment({}, 3840, 2160),
                         {'GOW3_PATCHES': f'{pool};{boot4k}', 'GOW3_DMEM_MB': '7104',
                          'GOW3_INTERNAL_WIDTH': '3840', 'GOW3_INTERNAL_HEIGHT': '2160'})
        self.assertEqual(g.output_environment({}, 1280, 720),
                         {'GOW3_PATCHES': 'Boot display size 1280x720',
                          'GOW3_INTERNAL_WIDTH': '1280', 'GOW3_INTERNAL_HEIGHT': '720'})
        live = g.output_environment({'GOW3_UE4_BOOT_SIZE': '0', 'GOW3_PS4_MEMORY': '1'}, 3200, 1800)
        self.assertEqual((live['GOW3_UE4_SETRES'], live['GOW3_PATCHES'], 'GOW3_DMEM_MB' in live),
                         ('3200x1800', pool, False))

    def test_resolution_migration(self):
        # The single render_resolution / ue4_upscale of earlier versions become the two settings.
        self.assertEqual(g.migrate_resolution({'render_resolution': '4K_native', 'ue4_upscale': 'quality'}),
                         {'output_resolution': '3840x2160', 'render_scale': '67'})
        self.assertEqual(g.migrate_resolution({'render_resolution': '1440p'}),
                         {'output_resolution': '1920x1080', 'render_scale': '133'})
        self.assertEqual(g.migrate_resolution({'render_resolution': 'native', 'output_resolution': '2560x1440'}), {})
        self.ini.write_text('render_resolution=1440p_native\nue4_upscale=balanced\n', encoding='utf-8')
        options, _ = g.prepare(self.ini, self.state)
        self.assertEqual((options['output_resolution'], options['render_scale']), ('2560x1440', '58'))
        self.assertEqual(g.read_ini(self.ini)['output_resolution'], '2560x1440')

    def test_write_keys_keeps_other_lines(self):
        self.ini.write_text('# comment\nshow_fps=1\nengine_fps=60\n', encoding='utf-8')
        g.write_keys(self.ini, {'engine_fps': '240', 'vsync': '0'})
        self.assertEqual(self.ini.read_text(encoding='utf-8'), '# comment\nshow_fps=1\nengine_fps=240\nvsync=0\n')

    def test_unconfirmed_restart_is_tried_once_then_reverted(self):
        self.ini.write_text('render_resolution=1440p\nengine_fps=120\n', encoding='utf-8')
        good, _ = g.prepare(self.ini, self.state)  # a normal, confirmed launch
        g.write_keys(self.ini, {'render_resolution': '4K', 'engine_fps': '240', 'restart_unconfirmed': '1'})
        tried, reverted = g.prepare(self.ini, self.state)  # the launch of "Apply and restart"
        self.assertFalse(reverted)
        self.assertEqual((tried['render_resolution'], tried['engine_fps']), ('4K', '240'))
        back, reverted = g.prepare(self.ini, self.state)  # it never confirmed: back to the good ones
        self.assertTrue(reverted)
        self.assertEqual(back, good)
        ini = g.read_ini(self.ini)
        self.assertEqual((ini['render_resolution'], ini['restart_unconfirmed']), ('1440p', '0'))

    def test_confirmed_restart_becomes_the_last_good(self):
        g.prepare(self.ini, self.state)
        g.write_keys(self.ini, {'engine_fps': '240', 'restart_unconfirmed': '1'})
        g.prepare(self.ini, self.state)
        g.write_keys(self.ini, {'restart_unconfirmed': '0'})  # the game confirmed
        g.prepare(self.ini, self.state)
        self.assertEqual(json.loads(self.state.read_text())['last_good']['engine_fps'], '240')

    def test_patch_names(self):
        launcher = ['Bug Fix - Texture Corruption Fix', 'Frame Rate Patch - 120 FPS', 'Skip Any Video With X Button']
        options = g.startup_options({'render_resolution': '1440p', 'engine_fps': '240'})
        self.assertEqual(g.patch_names(launcher, options),
                         ['Skip Any Video With X Button', 'Resolution Patch - 1440p', 'Frame Rate Patch - 240 FPS'])
        native = g.startup_options({'engine_fps': '60'})
        self.assertEqual(g.patch_names(['Resolution Patch - 4K', 'Skip Any Video With X Button'], native),
                         ['Bug Fix - Texture Corruption Fix', 'Skip Any Video With X Button'])

    def test_environment(self):
        env = g.environment(g.startup_options({'stale_readback': '0'}))
        self.assertEqual(env, {'GOW3_DEFERRED_READBACK': '1', 'GOW3_STALE_READBACK': '0', 'GOW3_ASYNC_SHADERS': '1'})


    def test_present_environment(self):
        # defaults: the tested best (Reflex limiter, tick 960, precise tick, presents on the vblank thread)
        self.assertEqual(g.present_environment({}),
                         {'GOW3_PRESENT_THREAD': '0', 'GOW3_PRECISE_TICK': '1', 'GOW3_VBLANK_HZ': '960',
                          'GOW3_REFLEX': '1', 'GOW3_REFLEX_SLEEP_BEFORE_PRESENT': '1', 'GOW3_PRECISE_LIMIT': '1',
                          'GOW3_LIMIT_HOLD': '1', 'GOW3_PRESENT_PRIORITY': '1'})
        ini = {'present_tick_hz': '120', 'present_mode': 'mailbox', 'present_inline': '0', 'present_precise_tick': '0',
               'present_no_frame_caps': '1', 'present_frame_trace': '1', 'present_limiter': 'standard'}
        self.assertEqual(g.present_environment(ini, 'trace.csv'),
                         {'GOW3_VBLANK_HZ': '120', 'GOW3_PRESENT_MODE': 'mailbox', 'GOW3_NO_FRAME_CAP': '1',
                          'GOW3_FLIP_TRACE': 'trace.csv'})
        # 0 / invalid tick rates and unknown modes stay automatic
        plain = {'present_inline': '0', 'present_precise_tick': '0', 'present_limiter': 'standard'}
        self.assertEqual(g.present_environment({**plain, 'present_tick_hz': 'x', 'present_mode': 'vsync'}), {})
        self.assertEqual(g.present_environment({**plain, 'present_tick_hz': '0', 'present_mode': 'auto'}), {})
        self.assertTrue(g.no_frame_caps(ini))


if __name__ == '__main__':
    unittest.main()
