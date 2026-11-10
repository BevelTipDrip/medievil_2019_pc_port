"""The override pak's UE 4.21 package edit: renaming a name map entry (the debug menu's button)."""
import struct
import unittest

import paths  # noqa: F401  (puts scripts/ on sys.path)
import ue4_override as u


def package(names):
    """A minimal cooked .uasset: summary, name map, one export entry (its data in the .uexp right after
    the header), then the depends / asset registry / preload tables."""
    def fstr(text):
        return u.fstring(text)

    def summary(name_offset, export_offset, import_offset, depends_offset, registry_offset, preload_offset, total):
        body = struct.pack('<Iiiiii', 0x9E2A83C1, -7, 0, 0, 0, 0)  # tag, legacy, UE3, UE4, licensee, custom
        body += struct.pack('<i', total) + fstr('None') + struct.pack('<I', 0x80000000)
        body += struct.pack('<ii', len(names), name_offset) + struct.pack('<ii', 0, 0)
        body += struct.pack('<ii', 1, export_offset) + struct.pack('<ii', 0, import_offset)
        body += struct.pack('<i', depends_offset) + struct.pack('<ii', 0, 0) + struct.pack('<ii', 0, 0)
        body += bytes(16) + struct.pack('<i', 0)
        body += (struct.pack('<HHHI', 4, 21, 0, 0) + fstr('')) * 2
        body += struct.pack('<I', 0) + struct.pack('<i', 0) + struct.pack('<I', 0) + struct.pack('<i', 0)
        body += struct.pack('<i', registry_offset) + struct.pack('<q', total + 100) + struct.pack('<i', 0)
        body += struct.pack('<i', 0) + struct.pack('<ii', 0, preload_offset)
        return body
    head_len = len(summary(0, 0, 0, 0, 0, 0, 0))
    name_map = b''.join(u.fstring(n) + struct.pack('<HH', *u.name_hashes(n)) for n in names)
    import_offset = head_len + len(name_map)  # an empty import map right after the name map
    export_offset = import_offset  # then one export entry
    depends_offset = export_offset + 104
    registry_offset = depends_offset + 4
    preload_offset = registry_offset + 4
    total = preload_offset
    export = struct.pack('<iiii', -1, 0, 0, 0) + struct.pack('<ii', 1, 0) + struct.pack('<I', 0)
    export += struct.pack('<qq', 100, total) + bytes(104 - 44)
    tables = struct.pack('<i', 0) + struct.pack('<i', 0)
    data = summary(head_len, export_offset, import_offset, depends_offset, registry_offset, preload_offset, total)
    return data + name_map + export + tables


class PackageNameTests(unittest.TestCase):
    def test_name_hashes_match_the_game(self):
        # Stored in 01.02's MainMenuPawn.uasset next to these names.
        self.assertEqual(u.name_hashes('None'), (1012, 3525))
        self.assertEqual(u.name_hashes('Gamepad_RightThumbstick'), (33202, 44597))
        self.assertEqual(u.name_hashes('InpActEvt_Gamepad_RightThumbstick_K2Node_InputKeyEvent'), (7586, 14121))

    def test_rename_shifts_every_offset_behind_the_name(self):
        data = package(['None', 'Gamepad_RightThumbstick', 'Other'])
        out = u.rename_package_name(data, 'Gamepad_RightThumbstick', 'Gamepad_Special_Left')
        self.assertEqual(len(out), len(data) - 3)
        fields, entries, serial = u.package_header(out)
        self.assertEqual([e[0] for e in entries], ['None', 'Gamepad_Special_Left', 'Other'])
        values = {f[0]: f[3] for f in fields}
        self.assertEqual(values['TotalHeaderSize'], len(out))
        self.assertEqual(struct.unpack_from('<q', out, serial[0])[0], len(out))
        start, end = entries[1][1], entries[1][2]
        self.assertEqual(struct.unpack_from('<HH', out, end - 4), u.name_hashes('Gamepad_Special_Left'))
        names_start = entries[0][1]
        self.assertEqual(out[names_start:start], data[names_start:start])  # earlier names unchanged

    def test_rename_needs_exactly_one_entry(self):
        data = package(['None', 'Other'])
        with self.assertRaises(ValueError):
            u.rename_package_name(data, 'Gamepad_RightThumbstick', 'Gamepad_Special_Left')


if __name__ == '__main__':
    unittest.main()
