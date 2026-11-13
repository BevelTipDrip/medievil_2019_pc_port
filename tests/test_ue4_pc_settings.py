"""PC Settings in the pause menu (scripts/ue4_pc_settings.py): locres / string-table / package round trips and the
edits on the player's own files of both builds. The game-file tests run where the games are unpacked (the
defaults below, or GOW3_TEST_GAME_0100 / GOW3_TEST_GAME_0102) and are skipped elsewhere."""
import os
import struct
import unittest
from pathlib import Path

import paths  # noqa: F401
import ue4_override
import ue4_pc_settings as pcs

GAMES = {'01.00': os.environ.get('GOW3_TEST_GAME_0100', r'C:\Users\Ty\MediEvil\Game Files\CUSA11227'),
         '01.02': os.environ.get('GOW3_TEST_GAME_0102', r'E:\MediEvil\CUSA11227-0102')}


def fake_locres(entries):
    """A version 1 locres of {(namespace, key): (source hash, text)} in UE4's layout."""
    strings, body, namespaces = [], b'', {}
    for (namespace, key), value in entries.items():
        namespaces.setdefault(namespace, []).append((key, value))
    body = struct.pack('<I', len(namespaces))
    for namespace, keys in namespaces.items():
        body += pcs.fstring(namespace) + struct.pack('<I', len(keys))
        for key, (source_hash, text) in keys:
            if text not in strings:
                strings.append(text)
            body += pcs.fstring(key) + struct.pack('<Ii', source_hash, strings.index(text))
    tail = struct.pack('<i', len(strings)) + b''.join(pcs.fstring(s) for s in strings)
    return pcs.LOCRES_MAGIC + b'\x01' + struct.pack('<q', 25 + len(body)) + body + tail


class FormatTests(unittest.TestCase):
    def test_str_crc32_is_the_locres_source_hash(self):
        # Hashes of the game's own locres entries (OPTIONS_CLASSIC_CONTROLS_SWITCH, MM_OPTIONS, PMENU_SCREEN_ADJUST).
        self.assertEqual(pcs.str_crc32('Switch to Classic Controls'), 0x4a90147c)
        self.assertEqual(pcs.str_crc32('Options'), 0xc0f02d5a)
        self.assertEqual(pcs.str_crc32('Screen Adjust'), 0x5d7cd8ab)

    def test_fstring_ansi_and_wide(self):
        self.assertEqual(pcs.fstring('Ab'), b'\x03\x00\x00\x00Ab\x00')
        self.assertEqual(pcs.fstring('Ä'), b'\xfe\xff\xff\xff\xc4\x00\x00\x00')
        self.assertEqual(pcs.fstring(''), b'\x00\x00\x00\x00')
        for text in ('PC Settings', 'Настройки ПК', 'PC設定', ''):
            self.assertEqual(pcs.Reader(pcs.fstring(text)).fstring()[0], text)

    def test_locres_round_trip_and_add(self):
        data = fake_locres({('General', 'A'): (1, 'One'), ('General', 'B'): (2, 'Zwei ä'), ('Other', 'C'): (3, 'One')})
        table = pcs.Locres(data)
        self.assertEqual(table.to_bytes(), data)
        table.add('General', 'NEW', 'PC Settings', 'Настройки ПК')
        again = pcs.Locres(table.to_bytes())
        self.assertEqual(again.get('General', 'NEW'), (pcs.str_crc32('PC Settings'), 'Настройки ПК'))
        self.assertEqual(again.get('General', 'B'), (2, 'Zwei ä'))
        self.assertEqual(again.get('Other', 'C'), (3, 'One'))
        with self.assertRaises(ValueError):
            again.add('General', 'NEW', 'PC Settings', 'x')

    def test_new_keys_keep_the_bytecode_size(self):
        for old, new in pcs.KEYS.items():
            self.assertEqual(len(old), len(new))
        self.assertEqual(sorted(pcs.cultures(['OVERBITE/Content/Localization/Game/de/Game.locres',
                                              'OVERBITE/Content/Localization/Game/pt-BR/Game.locres',
                                              'Engine/Content/Localization/Engine/de/Engine.locres'])), ['de', 'pt-BR'])


class GameFileTests(unittest.TestCase):
    """On the player's files of each build found."""

    def builds(self):
        found = [(build, game) for build, game in GAMES.items() if (Path(game) / 'overbite' / 'content' / 'paks').is_dir()]
        if not found:
            self.skipTest('no unpacked MediEvil (GOW3_TEST_GAME_0100 / GOW3_TEST_GAME_0102)')
        return found

    def test_locres_round_trip_and_translations(self):
        for build, game in self.builds():
            names = ue4_override.pak_names(game)
            cultures = pcs.cultures(names)
            self.assertEqual(len(cultures), 25, build)
            self.assertEqual(set(cultures) - set(pcs.LABELS), set(), build)
            for culture in cultures:
                data = ue4_override.read_file(game, f'{pcs.LOCRES_DIR}{culture}/Game.locres')
                self.assertEqual(pcs.Locres(data).to_bytes(), data, (build, culture))
                before, after = pcs.Locres(data), pcs.Locres(pcs.locres(data, culture))
                for (namespace, _w), keys in before.namespaces:
                    for (key, _kw), source_hash, index in keys:
                        self.assertEqual(after.get(namespace, key), (source_hash, before.strings[index][0]))
                label = pcs.LABELS[culture]
                for key in pcs.KEYS.values():
                    self.assertEqual(after.get(pcs.NAMESPACE, key), (pcs.str_crc32(pcs.SOURCE), label))
                # The label's letters are the culture's own (the menu font draws them) or plain ASCII.
                letters = set(''.join(s for s, _wide in before.strings))
                self.assertEqual([c for c in label if c not in letters and ord(c) > 0x7f], [], culture)

    def test_string_table(self):
        for build, game in self.builds():
            header = ue4_override.read_file(game, f'{pcs.STRING_TABLE}.uasset')
            uexp = ue4_override.read_file(game, f'{pcs.STRING_TABLE}.uexp')
            package = pcs.Package(header, uexp)
            package.resize_last(1, bytes(package.data(1)))  # unchanged: byte-identical
            self.assertEqual((bytes(package.header), bytes(package.exports_data)), (header, uexp), build)
            new_header, new_uexp = pcs.string_table(header, uexp)
            added = b''.join(pcs.fstring(k) + pcs.fstring(pcs.SOURCE) for k in pcs.KEYS.values())
            delta = len(new_uexp) - len(uexp)
            self.assertEqual(delta, len(added), build)
            # The header changes only in the export's size and the bulk data offset, each by delta.
            changed = [i for i in range(len(header)) if header[i] != new_header[i]]
            p, q = pcs.Package(header, uexp), pcs.Package(new_header, new_uexp)
            fields = set(range(p.exports[0]['size_at'], p.exports[0]['size_at'] + 8)) | set(range(p.bulk_at, p.bulk_at + 8))
            self.assertTrue(changed and set(changed) <= fields, build)
            self.assertEqual(q.exports[0]['size'], p.exports[0]['size'] + delta)
            # The uexp: the count + 2, the new entries before the table's tail, nothing else.
            at = new_uexp.find(added)
            self.assertGreater(at, 0, build)
            self.assertEqual(new_uexp[at + len(added):], uexp[at:], build)
            count_at = uexp.find(pcs.fstring(pcs.NAMESPACE)) + len(pcs.fstring(pcs.NAMESPACE))
            self.assertEqual(struct.unpack_from('<i', new_uexp, count_at)[0],
                             struct.unpack_from('<i', uexp, count_at)[0] + 2, build)
            self.assertEqual(new_uexp[:count_at], uexp[:count_at], build)
            self.assertEqual(new_uexp[count_at + 4:at], uexp[count_at + 4:at], build)
            with self.assertRaises(ValueError):
                pcs.string_table(new_header, new_uexp)  # once only

    def test_pause_menu_edits(self):
        for build, game in self.builds():
            header = ue4_override.read_file(game, f'{pcs.PAUSE_MENU}.uasset')
            uexp = ue4_override.read_file(game, f'{pcs.PAUSE_MENU}.uexp')
            edited = pcs.pause_menu(header, uexp)
            self.assertEqual(len(edited), len(uexp), build)
            p = pcs.Package(header, uexp)
            allowed = set()
            # (a) the Visibility tag names of both Button_Controls
            for path in ('PauseMenu_C.WidgetTree.Button_Controls', 'WidgetArchetype.WidgetTree_0.Button_Controls'):
                tag = [t for t in p.tags(p.find(path)) if t[0] == 'Visibility'][0]
                allowed |= set(range(tag[2], tag[2] + 8))
                self.assertEqual(p.name(struct.unpack_from('<q', edited, tag[2])[0]), 'Button_Controls')
                e = pcs.Package(header, edited)
                self.assertEqual([t for t in e.tags(e.find(path)) if t[0] == 'Visibility'], [], build)
            # (b) the label keys
            for old, new in pcs.KEYS.items():
                at = uexp.find(old.encode() + b'\0')
                allowed |= set(range(at, at + len(old)))
                self.assertEqual(edited[at:at + len(new) + 1], new.encode() + b'\0')
            # (c) the click handler's jump
            old, new = next((o, n) for o, n in pcs.CLICK_JUMPS if uexp.count(o) == 1)
            at = uexp.find(old)
            allowed |= set(range(at, at + len(old)))
            self.assertEqual(edited[at:at + len(new)], new)
            # (d) the slot order after Options in both trees, and the highlight arrays
            for tree in ('PauseMenu_C.WidgetTree', 'WidgetArchetype.WidgetTree_0'):
                box = p.find(f'{tree}.VerticalBox_61')
                _n, _k, _at, value_at, size = [t for t in p.tags(box) if t[0] == 'Slots'][0]
                allowed |= set(range(value_at, value_at + size))
                count, = struct.unpack_from('<i', edited, value_at)
                order = [p.path(i).split('.')[-1] for i in struct.unpack_from(f'<{count}i', edited, value_at + 4)]
                self.assertEqual(order, ['VerticalBoxSlot_2', 'VerticalBoxSlot_1', 'VerticalBoxSlot_4',
                                         'VerticalBoxSlot_5', 'VerticalBoxSlot_3', 'VerticalBoxSlot_0'], build)
            for first, second in (('Button_Controls', 'Button_Options'), ('Text_Controls', 'Text_Options')):
                a, b = (struct.pack('<Bi', 1, p.find(f'PauseMenu_C.{n}')) for n in (first, second))
                at = uexp.find(a + b)
                allowed |= set(range(at, at + 10))
                self.assertEqual(edited[at:at + 10], b + a, build)
            changed = {i for i in range(len(uexp)) if uexp[i] != edited[i]}
            self.assertTrue(changed and changed <= allowed, build)
            with self.assertRaises(ValueError):
                pcs.pause_menu(header, edited)  # every edit applies once only

    def test_pak_files(self):
        for build, game in self.builds():
            out = pcs.files(lambda name: ue4_override.read_file(game, name), ue4_override.pak_names(game))
            self.assertEqual(len(out), 3 + 25, build)
            self.assertIn(f'{pcs.PAUSE_MENU}.uexp', out)
            self.assertNotIn(f'{pcs.PAUSE_MENU}.uasset', out)  # the header stays the game's

    def test_override_pak(self):
        """ue4_override.apply with pc_settings writes the files into the pak (into a temporary folder here: the
        game's Paks folder is left alone)."""
        import tempfile
        for build, game in self.builds():
            names = ue4_override.pak_names(game)
            wanted = [ue4_override.CONFIG, f'{pcs.PAUSE_MENU}.uasset', f'{pcs.PAUSE_MENU}.uexp',
                      f'{pcs.STRING_TABLE}.uasset', f'{pcs.STRING_TABLE}.uexp'] + \
                     [f'{pcs.LOCRES_DIR}{c}/Game.locres' for c in pcs.cultures(names)]
            files = {name: ue4_override.read_file(game, name) for name in wanted}
            saved = ue4_override.paks_dir, ue4_override.read_file, ue4_override.pak_names
            with tempfile.TemporaryDirectory() as tmp:
                try:
                    ue4_override.paks_dir = lambda _game: Path(tmp)
                    ue4_override.read_file = lambda _game, name: files[name]
                    ue4_override.pak_names = lambda _game: names
                    summary = ue4_override.apply(game, pc_settings=True)
                    entries = ue4_override.read_index(Path(tmp) / ue4_override.PAK_NAME)
                finally:
                    ue4_override.paks_dir, ue4_override.read_file, ue4_override.pak_names = saved
            self.assertIn('PC Settings in the pause menu', summary, build)
            self.assertIn(f'{pcs.PAUSE_MENU}.uexp', entries, build)
            self.assertIn(f'{pcs.LOCRES_DIR}ja/Game.locres', entries, build)


if __name__ == '__main__':
    unittest.main()
