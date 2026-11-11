import unittest

import paths  # noqa: F401 (scripts/ on sys.path)
import ue4_camera as camera


def fake_game(pitch_variant=0, drop=None):
    """Game files holding each edit's original bytes once, between filler; `drop` leaves one file out."""
    pad = b'\xaa' * 16
    pitch = camera.PITCH_RETURN[0][pitch_variant][0]
    free = pad + b''.join(old + pad for old, _, _ in camera.FOLLOW + camera.RUN_ZOOM) + pitch + pad
    files = {camera.FREE: free, camera.SIMPLE_IN_PAK: free}
    for move in ('swing', 'suggest', 'keepout'):
        (name, edits), = camera.MOVES[move].items()
        files[name] = pad + edits[0][0] + pad
    files.pop(drop, None)

    def read(name):
        if name not in files:
            raise FileNotFoundError(name)
        return files[name]
    return read


class CameraMoveTests(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(camera.parse(' run_zoom, follow ,,'), ['follow', 'run_zoom'])
        self.assertEqual(camera.parse(''), [])
        with self.assertRaises(ValueError):
            camera.parse('follow,chase')

    def test_every_move_edits_same_size(self):
        read = fake_game()
        edited, done = camera.files(read, list(camera.MOVES))
        self.assertEqual(done, list(camera.MOVES))
        self.assertEqual(set(edited), {camera.FREE, camera.SIMPLE, camera.SUGGEST, camera.BASE, camera.MANAGER})
        for name, data in edited.items():
            original = read(camera.READ_NAMES.get(name, name))
            self.assertEqual(len(data), len(original))
            self.assertNotEqual(data, original)
        free = edited[camera.FREE]
        self.assertIn(bytes.fromhex('1ecaf24971'), free)    # 400 -> 1e30
        self.assertIn(bytes.fromhex('1e00000000'), free)    # InputRotation x 0
        self.assertIn(bytes.fromhex('1606300b0000'), free)  # run zoom: Jump
        self.assertIn(bytes.fromhex('160675020000'), free)  # pitch return: Jump

    def test_one_move_only_touches_its_files(self):
        edited, done = camera.files(fake_game(), ['swing'])
        self.assertEqual((done, set(edited)), (['swing'], {camera.MANAGER}))

    def test_01_00_pitch_variant(self):
        edited, done = camera.files(fake_game(pitch_variant=1), ['pitch_return'])
        self.assertEqual(done, ['pitch_return'])
        self.assertIn(bytes.fromhex('160650020000'), edited[camera.FREE])

    def test_missing_bytes_skip_that_move_only(self):
        edited, done = camera.files(fake_game(drop=camera.SIMPLE_IN_PAK), ['follow', 'keepout'])
        self.assertEqual(done, ['keepout'])
        self.assertEqual(set(edited), {camera.BASE})


if __name__ == '__main__':
    unittest.main()
