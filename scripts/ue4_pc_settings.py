"""PC Settings in MediEvil's pause menu (research/docs/study-pc-settings-entry.md).

The pause menu (UI/PauseMenu) keeps a hidden, fully wired button from development, "Switch to Classic
Controls" (Button_Controls: collapsed, in the highlight arrays, with a click handler and a label set from
the string table at start). The override pak (ue4_override.apply, medievil.ini pc_settings_entry) turns it
into "PC Settings", fifth in the list after Options; the runtime (src/pc_settings_hook.h) opens the port's
settings menu when it is clicked. All edits are computed from the player's own files, for 01.00 and 01.02:

- PauseMenu.uexp, same size: the button's Visibility tag gets another name (an unknown property is skipped
  on load, so the button keeps the default Visible), the label's two string-table keys become keys of the
  same length added below, the click handler's cheat-mode jump goes to its own `pop` (the click only plays
  the confirm sound: the controls are not switched), and the row moves after Options (the list's slot order
  and the highlight arrays).
- STR_GENERAL: the two new keys, source "PC Settings".
- Game.locres of every culture: the two keys translated (LABELS).
"""
import struct

PAUSE_MENU = 'OVERBITE/Content/UI/PauseMenu'
STRING_TABLE = 'OVERBITE/Content/Texts/OST/STR_GENERAL'
LOCRES_DIR = 'OVERBITE/Content/Localization/Game/'
NAMESPACE = 'General'  # STR_GENERAL's namespace (its locres entries)
SOURCE = 'PC Settings'
# The label keys the pause menu sets (Update KeyMap) -> new keys of the same length (bytecode keeps its size).
# The old keys stay: Options > Button Layout uses them too.
KEYS = {'OPTIONS_MODERN_CONTROLS_SWITCH': 'PCPORT_PC_SETTINGS_MODERN_CTRL',
        'OPTIONS_CLASSIC_CONTROLS_SWITCH': 'PCPORT_PC_SETTINGS_CLASSIC_CTRL'}
# "PC Settings" per culture, worded as the game words its settings (its "Colorblind Settings" and
# "Options" strings; the characters all occur in the culture's own strings).
LABELS = {
    'ar': 'إعدادات الكمبيوتر', 'bg': 'Настройки за PC', 'cs': 'Nastavení PC', 'da': 'PC-indstillinger',
    'de': 'PC-Einstellungen', 'el': 'Ρυθμίσεις PC', 'en': 'PC Settings', 'en-GB': 'PC Settings',
    'es': 'Ajustes de PC', 'es-MX': 'Configuración de PC', 'fi': 'PC-asetukset', 'fr': 'Paramètres PC',
    'hr': 'Postavke za PC', 'hu': 'PC-beállítások', 'it': 'Impostazioni PC', 'ja': 'PC設定',
    'nl': 'Pc-instellingen', 'no': 'PC-innstillinger', 'pl': 'Ustawienia PC', 'pt': 'Definições do PC',
    'pt-BR': 'Configurações do PC', 'ro': 'Setări PC', 'ru': 'Настройки ПК', 'sv': 'PC-inställningar',
    'tr': 'PC Ayarları',
}
# The click handler: EX_JumpIfNot(InCheatMode) to the statement after its `pop` -> to the `pop`. One variant
# per build (01.00, 01.02): jump target, EX_InstanceVariable InCheatMode, EX_PopExecutionFlow.
CLICK_JUMPS = [(bytes.fromhex('073e1400000146000000') + b'\x4d', bytes.fromhex('073d1400000146000000') + b'\x4d'),
               (bytes.fromhex('07191500000147000000') + b'\x4d', bytes.fromhex('07181500000147000000') + b'\x4d')]
TAG = 0x9E2A83C1
LOCRES_MAGIC = bytes.fromhex('0e14747567 4a03fc4a15909dc3377f1b'.replace(' ', ''))


class Reader:
    def __init__(self, data, pos=0):
        self.data, self.pos = data, pos

    def take(self, fmt):
        values = struct.unpack_from(fmt, self.data, self.pos)
        self.pos += struct.calcsize(fmt)
        return values if len(values) > 1 else values[0]

    def fstring(self):
        """(text, wide): wide is UTF-16 on disk."""
        n = self.take('<i')
        if n < 0:
            text = self.data[self.pos:self.pos - 2 * n].decode('utf-16le')[:-1]
            self.pos += -2 * n
            return text, True
        text = self.data[self.pos:self.pos + n].decode('latin1')[:-1]
        self.pos += n
        return text, False


def fstring(text, wide=None):
    """FString bytes: ANSI when every character is below 0x80 (UE4's IsPureAnsi), else UTF-16."""
    if text == '' and not wide:
        return struct.pack('<i', 0)
    if wide is None:
        wide = any(ord(c) > 0x7f for c in text)
    if wide:
        data = (text + '\0').encode('utf-16le')
        return struct.pack('<i', -(len(data) // 2)) + data
    data = text.encode('latin1') + b'\0'
    return struct.pack('<i', len(data)) + data


class Package:
    """The parts of an unversioned cooked UE 4.21 package (.uasset header + .uexp) these edits use: the name
    table, the export table (with the header offset of each SerialSize) and BulkDataStartOffset."""

    def __init__(self, header, exports):
        self.header, self.exports_data = bytearray(header), bytearray(exports)
        r = Reader(self.header)
        if r.take('<I') != TAG:
            raise ValueError('not a uasset')
        if r.take('<i') != -4:
            r.take('<i')
        r.take('<ii')
        for _ in range(r.take('<i')):
            r.take('<16si')
        self.header_size = r.take('<i')
        r.fstring()
        _flags, name_count, name_offset = r.take('<Iii')
        r.take('<ii')
        export_count, export_offset, import_count, import_offset = r.take('<iiii')
        r.take('<i')  # depends
        r.take('<ii')  # soft package references
        r.take('<i')  # searchable names
        r.take('<i')  # thumbnail table
        r.take('<16s')
        for _ in range(r.take('<i')):
            r.take('<ii')
        for _ in range(2):  # saved by / compatible with engine version
            r.take('<HHHI')
            r.fstring()
        r.take('<I')
        if r.take('<i'):
            raise ValueError('compressed package')
        r.take('<I')
        for _ in range(r.take('<i')):
            r.fstring()
        r.take('<i')  # asset registry data
        self.bulk_at = r.pos
        r = Reader(self.header, name_offset)
        self.names = []
        for _ in range(name_count):
            self.names.append(r.fstring()[0])
            r.take('<HH')
        r = Reader(self.header, import_offset)
        self.imports = []
        for _ in range(import_count):
            r.take('<qq')
            outer = r.take('<i')
            self.imports.append((outer, self.name(r.take('<q'))))
        self.exports = []
        for i in range(export_count):
            at = export_offset + i * 104
            cls, _super, _template, outer = struct.unpack_from('<iiii', self.header, at)
            name, = struct.unpack_from('<q', self.header, at + 16)
            size, offset = struct.unpack_from('<qq', self.header, at + 28)
            self.exports.append({'class': cls, 'outer': outer, 'name': self.name(name), 'size': size,
                                 'offset': offset - self.header_size, 'size_at': at + 28})

    def name(self, value):
        index, number = value & 0xffffffff, value >> 32
        return self.names[index] + (f'_{number - 1}' if number else '')

    def name_index(self, text):
        return self.names.index(text)

    def path(self, index):
        """Dotted object path of an export (index > 0) or import (index < 0)."""
        if index > 0:
            e = self.exports[index - 1]
            outer, name = e['outer'], e['name']
        elif index < 0:
            outer, name = self.imports[-index - 1]
        else:
            return ''
        parent = self.path(outer)
        return f'{parent}.{name}' if parent else name

    def find(self, path):
        hits = [i + 1 for i in range(len(self.exports)) if self.path(i + 1) == path]
        if len(hits) != 1:
            raise ValueError(f'{path}: {len(hits)} exports')
        return hits[0]

    def data(self, index):
        e = self.exports[index - 1]
        return self.exports_data[e['offset']:e['offset'] + e['size']]

    def resize_last(self, index, new_data):
        """Replaces the data of export `index`, which must be the last one in the .uexp."""
        e = self.exports[index - 1]
        end = e['offset'] + e['size']
        if any(o['offset'] >= end for o in self.exports):
            raise ValueError('only the last export can change size')
        delta = len(new_data) - e['size']
        self.exports_data[e['offset']:end] = new_data
        e['size'] = len(new_data)
        struct.pack_into('<q', self.header, e['size_at'], e['size'])
        bulk, = struct.unpack_from('<q', self.header, self.bulk_at)
        struct.pack_into('<q', self.header, self.bulk_at, bulk + delta)

    def tags(self, index):
        """[(name, type, uexp offset of the tag, offset of the value, value size)] of an export's tagged
        properties."""
        e = self.exports[index - 1]
        r = Reader(self.exports_data, e['offset'])
        out = []
        while r.pos < e['offset'] + e['size']:
            at = r.pos
            name = self.name(r.take('<q'))
            if name == 'None':
                return out
            kind = self.name(r.take('<q'))
            size, _array_index = r.take('<ii')
            if kind == 'StructProperty':
                r.take('<q16s')
            elif kind == 'BoolProperty':
                r.take('<B')
            elif kind in ('ByteProperty', 'EnumProperty', 'ArrayProperty', 'SetProperty'):
                r.take('<q')
            elif kind == 'MapProperty':
                r.take('<qq')
            if r.take('<B'):
                r.take('<16s')
            out.append((name, kind, at, r.pos, size))
            r.pos += size
        raise ValueError(f'export {index}: no end of properties')


def replace_once(data, old, new, what):
    if len(old) != len(new) or data.count(old) != 1:
        raise ValueError(f'{what}: unexpected contents (game version?)')
    return data.replace(old, new)


def pause_menu(header, uexp):
    """The edited PauseMenu.uexp (same size; the header is unchanged)."""
    p = Package(header, uexp)
    data = bytearray(uexp)
    button = p.name_index('Button_Controls')
    # (a) Visible: in both trees (the class's and the fast-path template's), the Visibility tag's name.
    for path in ('PauseMenu_C.WidgetTree.Button_Controls', 'WidgetArchetype.WidgetTree_0.Button_Controls'):
        index = p.find(path)
        tags = [t for t in p.tags(index) if t[0] == 'Visibility']
        if len(tags) != 1 or tags[0][1] != 'EnumProperty' or tags[0][4] != 8:
            raise ValueError(f'{path}: no collapsed Visibility (game version?)')
        _name, _kind, at, value_at, _size = tags[0]
        if p.name(struct.unpack_from('<q', data, value_at)[0]) != 'ESlateVisibility::Collapsed':
            raise ValueError(f'{path}: Visibility is not Collapsed (game version?)')
        struct.pack_into('<q', data, at, button)
    data = bytes(data)
    # (b) The label's keys.
    for old, new in KEYS.items():
        data = replace_once(data, old.encode() + b'\0', new.encode() + b'\0', 'PauseMenu label key')
    # (c) The click plays the confirm sound only.
    for old, new in CLICK_JUMPS:
        if data.count(old) == 1:
            data = data.replace(old, new)
            break
    else:
        raise ValueError('PauseMenu click handler: unexpected contents (game version?)')
    # (d) After Options: the list's slot order in both trees, and the highlight arrays (AddButtonsToArray:
    # EX_InstanceVariable operands, adjacent in list order).
    data = bytearray(data)
    for tree in ('PauseMenu_C.WidgetTree', 'WidgetArchetype.WidgetTree_0'):
        box = p.find(f'{tree}.VerticalBox_61')
        controls = p.find(f'{tree}.VerticalBox_61.VerticalBoxSlot_3')
        options = p.find(f'{tree}.VerticalBox_61.VerticalBoxSlot_5')
        slots = [t for t in p.tags(box) if t[0] == 'Slots' and t[1] == 'ArrayProperty']
        if len(slots) != 1:
            raise ValueError(f'{tree}: no slot list (game version?)')
        value_at, size = slots[0][3], slots[0][4]
        count, = struct.unpack_from('<i', data, value_at)
        items = list(struct.unpack_from(f'<{count}i', data, value_at + 4))
        if size != 4 + 4 * count or items.index(options) != items.index(controls) + 1:
            raise ValueError(f'{tree}: unexpected slot order (game version?)')
        at = items.index(controls)
        items[at], items[at + 1] = options, controls
        struct.pack_into(f'<{count}i', data, value_at + 4, *items)
    data = bytes(data)
    for first, second in (('Button_Controls', 'Button_Options'), ('Text_Controls', 'Text_Options')):
        a, b = (struct.pack('<Bi', 0x01, p.find(f'PauseMenu_C.{n}')) for n in (first, second))
        data = replace_once(data, a + b, b + a, 'PauseMenu highlight arrays')
    return data


def string_table(header, uexp):
    """(header, uexp) of STR_GENERAL with the new keys, source SOURCE."""
    p = Package(header, uexp)
    if len(p.exports) != 1:
        raise ValueError('STR_GENERAL: not one export')
    blob = bytes(p.data(1))
    r = Reader(blob)
    if p.name(r.take('<q')) != 'None':
        raise ValueError('STR_GENERAL: unexpected properties')
    r.take('<i')
    namespace, _wide = r.fstring()
    if namespace != NAMESPACE:
        raise ValueError(f'STR_GENERAL: namespace {namespace!r}')
    count_at = r.pos
    count = r.take('<i')
    keys = set()
    for _ in range(count):
        keys.add(r.fstring()[0])
        r.fstring()
    if any(k not in keys for k in KEYS) or any(k in keys for k in KEYS.values()):
        raise ValueError('STR_GENERAL: unexpected keys (game version?)')
    added = b''.join(fstring(key) + fstring(SOURCE) for key in KEYS.values())
    new = blob[:count_at] + struct.pack('<i', count + len(KEYS)) + blob[count_at + 4:r.pos] + added + blob[r.pos:]
    p.resize_last(1, new)
    return bytes(p.header), bytes(p.exports_data)


class Locres:
    """A .locres file, version 1 (Compact) as the game ships it: namespaces of keys (source hash, index into
    one string array). to_bytes() of an unchanged file gives the same bytes."""

    def __init__(self, data):
        if data[:16] != LOCRES_MAGIC or data[16] != 1:
            raise ValueError('not a version 1 locres')
        r = Reader(data, 17)
        strings_at = r.take('<q')
        self.namespaces = []
        for _ in range(r.take('<I')):
            namespace = r.fstring()
            keys = []
            for _ in range(r.take('<I')):
                key = r.fstring()
                keys.append([key, r.take('<I'), r.take('<i')])
            self.namespaces.append([namespace, keys])
        if r.pos != strings_at:
            raise ValueError('locres: strings not after the keys')
        self.strings = [r.fstring() for _ in range(r.take('<i'))]
        if r.pos != len(data):
            raise ValueError('locres: data after the strings')

    def get(self, namespace, key):
        for (name, _wide), keys in self.namespaces:
            if name == namespace:
                for (k, _w), source_hash, index in keys:
                    if k == key:
                        return source_hash, self.strings[index][0]
        return None

    def add(self, namespace, key, source, text):
        if self.get(namespace, key):
            raise ValueError(f'locres: {key} already there')
        try:
            index = [s for s, _wide in self.strings].index(text)
        except ValueError:
            index = len(self.strings)
            self.strings.append((text, None))
        for (name, _wide), keys in self.namespaces:
            if name == namespace:
                keys.append([(key, None), str_crc32(source), index])
                return
        self.namespaces.append([(namespace, None), [[(key, None), str_crc32(source), index]]])

    def to_bytes(self):
        body = struct.pack('<I', len(self.namespaces))
        for namespace, keys in self.namespaces:
            body += fstring(*namespace) + struct.pack('<I', len(keys))
            for key, source_hash, index in keys:
                body += fstring(*key) + struct.pack('<Ii', source_hash, index)
        strings = struct.pack('<i', len(self.strings)) + b''.join(fstring(*s) for s in self.strings)
        return LOCRES_MAGIC + b'\x01' + struct.pack('<q', 25 + len(body)) + body + strings


_CRC = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ (0xEDB88320 if _c & 1 else 0)
    _CRC.append(_c)


def str_crc32(text):
    """UE4's FCrc::StrCrc32 (the locres source hash): four byte steps per character."""
    crc = 0xFFFFFFFF
    for ch in text:
        c = ord(ch)
        for _ in range(4):
            crc = (crc >> 8) ^ _CRC[(crc ^ c) & 0xFF]
            c >>= 8
    return crc ^ 0xFFFFFFFF


def locres(data, culture):
    """A culture's Game.locres with the new keys; cultures without a translation get the English source."""
    table = Locres(data)
    for key in KEYS.values():
        table.add(NAMESPACE, key, SOURCE, LABELS.get(culture, SOURCE))
    return table.to_bytes()


def cultures(names):
    """The cultures of the game's Game.locres files among pak file names."""
    found = []
    for name in names:
        if name.startswith(LOCRES_DIR) and name.endswith('/Game.locres'):
            found.append(name[len(LOCRES_DIR):-len('/Game.locres')])
    return sorted(found)


def files(read, names):
    """{pak path: edited bytes}: read(name) gives the game's file; names are the game's pak file names."""
    out = {f'{PAUSE_MENU}.uexp': pause_menu(read(f'{PAUSE_MENU}.uasset'), read(f'{PAUSE_MENU}.uexp'))}
    header, uexp = string_table(read(f'{STRING_TABLE}.uasset'), read(f'{STRING_TABLE}.uexp'))
    out[f'{STRING_TABLE}.uasset'], out[f'{STRING_TABLE}.uexp'] = header, uexp
    for culture in cultures(names):
        name = f'{LOCRES_DIR}{culture}/Game.locres'
        out[name] = locres(read(name), culture)
    return out
