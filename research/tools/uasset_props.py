"""Tagged properties of a cooked UE4 4.21 asset (.uasset + .uexp) from the game's paks, read only.

    py -3.14 -I research/tools/uasset_props.py <game dir> <asset path in the pak, .uasset> [export class ...]

Prints each export (class, name) with its properties; structs nest, arrays of structs are listed.
Used to find GPU particle emitters without fixed bounds and their spawn ranges (plan-0102.md, T-0102-5).
"""
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from ue4_override import paks_dir, read_index  # noqa: E402

TAG = 0x9E2A83C1
# Structs serialized natively (not as tagged properties) and their decoders.
NATIVE = {
    'Vector': ('<3f', 12), 'Vector2D': ('<2f', 8), 'Vector4': ('<4f', 16), 'Rotator': ('<3f', 12),
    'Quat': ('<4f', 16), 'LinearColor': ('<4f', 16), 'Color': ('<4B', 4), 'IntPoint': ('<2i', 8),
    'Guid': ('<4I', 16), 'Box': ('<6fB', 25),
}


class Reader:
    def __init__(self, data, pos=0):
        self.data, self.pos = data, pos

    def take(self, fmt):
        values = struct.unpack_from(fmt, self.data, self.pos)
        self.pos += struct.calcsize(fmt)
        return values if len(values) > 1 else values[0]

    def fstring(self):
        n = self.take('<i')
        if n < 0:
            text = self.data[self.pos:self.pos - 2 * n].decode('utf-16le')[:-1]
            self.pos += -2 * n
        else:
            text = self.data[self.pos:self.pos + n].decode('latin1')[:-1]
            self.pos += n
        return text


class Package:
    def __init__(self, header, exports_data):
        r = Reader(header)
        if r.take('<I') != TAG:
            raise ValueError('not a uasset')
        legacy = r.take('<i')
        if legacy != -4:
            r.take('<i')  # legacy UE3 version
        self.ue4, _licensee = r.take('<ii')
        for _ in range(r.take('<i')):
            r.take('<16si')  # custom versions (legacy -7: guid, version)
        self.header_size = r.take('<i')
        r.fstring()  # folder name
        _flags, name_count, name_offset = r.take('<Iii')
        _text_count, _text_offset = r.take('<ii')
        export_count, export_offset, import_count, import_offset = r.take('<iiii')
        r = Reader(header, name_offset)
        self.names = []
        for _ in range(name_count):
            self.names.append(r.fstring())
            r.take('<HH')  # hashes
        r = Reader(header, import_offset)
        self.imports = []
        for _ in range(import_count):
            _cpkg, cname, _outer, oname = r.take('<q'), r.take('<q'), r.take('<i'), r.take('<q')
            self.imports.append((self.name(cname), self.name(oname)))
        r = Reader(header, export_offset)
        self.exports = []
        for _ in range(export_count):
            cls, _sup, _tmpl, _outer = r.take('<iiii')
            oname = r.take('<q')
            _oflags, size, offset = r.take('<Iqq')
            r.take('<iii16sIii')
            r.take('<5i')
            self.exports.append((self.ref(cls), self.name(oname), offset - self.header_size, size))
        self.data = exports_data

    def name(self, value):
        index, number = value & 0xffffffff, value >> 32
        text = self.names[index]
        return f'{text}_{number - 1}' if number else text

    def ref(self, index):
        if index < 0:
            return self.imports[-index - 1][1]
        if index > 0:
            return self.exports[index - 1][1] if index - 1 < len(self.exports) else f'export{index}'
        return None

    def props(self, r, end):
        out = []
        while r.pos < end:
            name = self.name(r.take('<q'))
            if name == 'None':
                break
            ptype = self.name(r.take('<q'))
            size, index = r.take('<ii')
            extra = None
            if ptype == 'StructProperty':
                extra = self.name(r.take('<q'))
                r.take('<16s')
            elif ptype == 'BoolProperty':
                extra = r.take('<B')
            elif ptype in ('ByteProperty', 'EnumProperty', 'ArrayProperty', 'SetProperty'):
                extra = self.name(r.take('<q'))
            elif ptype == 'MapProperty':
                extra = (self.name(r.take('<q')), self.name(r.take('<q')))
            if r.take('<B'):
                r.take('<16s')
            start = r.pos
            out.append((name if not index else f'{name}[{index}]', self.value(r, ptype, extra, size)))
            r.pos = start + size
        return out

    def value(self, r, ptype, extra, size):
        if ptype == 'BoolProperty':
            return bool(extra)
        if ptype == 'FloatProperty':
            return r.take('<f')
        if ptype in ('IntProperty', 'UInt32Property'):
            return r.take('<i' if ptype == 'IntProperty' else '<I')
        if ptype == 'ObjectProperty':
            return self.ref(r.take('<i'))
        if ptype == 'NameProperty':
            return self.name(r.take('<q'))
        if ptype in ('ByteProperty', 'EnumProperty'):
            return r.take('<B') if size == 1 else self.name(r.take('<q'))
        if ptype == 'StructProperty':
            return self.struct(r, extra, r.pos + size)
        if ptype == 'ArrayProperty':
            count = r.take('<i')
            if extra == 'StructProperty':
                r.take('<q')  # inner tag: name
                r.take('<q')  # type
                r.take('<ii')
                inner = self.name(r.take('<q'))
                r.take('<16sB')
                return [self.struct(r, inner, r.pos + size) for _ in range(count)]
            if extra == 'ObjectProperty':
                return [self.ref(r.take('<i')) for _ in range(count)]
            if extra == 'FloatProperty':
                return [r.take('<f') for _ in range(count)]
            return f'<{count} x {extra}>'
        return f'<{ptype} {size} bytes>'

    def struct(self, r, sname, end):
        if sname in NATIVE:
            fmt, _n = NATIVE[sname]
            return r.take(fmt)
        return dict(self.props(r, end))

    def export_props(self, i):
        _cls, _name, offset, size = self.exports[i]
        return self.props(Reader(self.data, offset), offset + size)


def read(game, name):
    paks = sorted(paks_dir(game).glob('pakchunk*-ps4*.pak'), key=lambda p: int(p.stem.split('-')[0][8:]),
                  reverse=True)  # later chunks win (01.02 patch chunks override older copies)
    files = {}
    for wanted in (name, name[:-len('.uasset')] + '.uexp'):
        for pak in paks:
            entries = read_index(pak)
            if wanted in entries:
                offset, size, compression, blocks = entries[wanted]
                with open(pak, 'rb') as f:
                    if not compression:
                        f.seek(offset + 53)
                        files[wanted] = f.read(size)
                    else:
                        data = bytearray()
                        for start, end in blocks:
                            f.seek(offset + start)
                            data += zlib.decompress(f.read(end - start))
                        files[wanted] = bytes(data)
                break
    return Package(files[name], files.get(name[:-len('.uasset')] + '.uexp', b''))


def main():
    game, name, *classes = sys.argv[1:]
    package = read(game, name)
    for i, (cls, oname, _offset, _size) in enumerate(package.exports):
        if classes and cls not in classes:
            continue
        print(f'{cls} {oname}')
        for key, value in package.export_props(i):
            print(f'  {key} = {value}')


if __name__ == '__main__':
    main()
