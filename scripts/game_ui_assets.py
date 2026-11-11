#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The game's own UI pieces for the port's overlay (Load list, button glyphs), from the player's
own game: one cache any overlay part can use.

Writes <user>/cache/game_ui/:
  levels.tsv              "index<TAB>code<TAB>name", index = LevelsEnum's NewEnumerator number
                          as saves store it (LevelsEnum order, LevelDataTable names)
  levels/level_NN.png     the level's loading screen, 480x270
  menu/frame_options.png  the Options menu frame (transparent middle)
  prompts/PS4_*.png       every PS4 button prompt (UI/Textures/ButtonPrompts), 100x100
  fonts/title.ttf, fonts/text.ttf  the Options menu's title (Fontesque) and text (Champ Fleury)
Nothing of the game is shipped: this reads the installed paks at start, once per game folder and
VERSION.

The UI textures are single-mip DXT1 / DXT5 / B8G8R8A8 in PS4 1D thin micro tiling: 8x8 elements
(4x4 blocks for DXT) per micro tile in Morton order (x0 y0 x1 y1 x2 y2), tiles row by row, the
surface padded to whole micro tiles (shadPS4 tiling.comp, ComputeSurfaceAddrFromCoordMicroTiled).
Pillow decodes DXT ("bcn"). Any other format or layout is skipped, never guessed.
"""
import re
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ue4_override import paks_dir, read_index  # noqa: E402

VERSION = '3'
PICTURE = (480, 270)
LEVELS_ENUM = 'OVERBITE/Content/Blueprints/GameInstance/LevelsEnum'
LEVEL_TABLE = 'OVERBITE/Content/Blueprints/GameInstance/LevelDataTable'
SCREENS = 'OVERBITE/Content/UI/LoadingScreens/'


class Paks:
    """The game's paks, each index read once."""

    def __init__(self, game):
        self.where = {}
        for pak in sorted(paks_dir(game).glob('pakchunk*-ps4.pak')):
            for name, entry in read_index(pak).items():
                self.where.setdefault(name, (pak, entry))

    def read(self, name):
        pak, (offset, size, compression, blocks) = self.where[name]
        with open(pak, 'rb') as f:
            if not compression:
                f.seek(offset + 53)  # the entry header in front of the data
                return f.read(size)
            data = bytearray()
            for start, end in blocks:
                f.seek(offset + start)
                data += zlib.decompress(f.read(end - start))
            return bytes(data)


def strings(data):
    """The ASCII FStrings of a cooked asset, in order."""
    found, i = [], 0
    while i < len(data) - 5:
        n = int.from_bytes(data[i:i + 4], 'little', signed=True)
        if 2 <= n <= 200 and i + 4 + n <= len(data) and data[i + 3 + n] == 0:
            text = data[i + 4:i + 3 + n]
            if all(32 <= c < 127 for c in text):
                found.append(text.decode())
                i += 4 + n
                continue
        i += 1
    return found


def levels(paks):
    """[(index, code, name)]: LevelsEnum's order (Level_NN_CODE) and LevelDataTable's names."""
    order = [s for s in strings(paks.read(LEVELS_ENUM + '.uexp')) if re.fullmatch(r'Level_\d\d_\w+', s)]
    table = strings(paks.read(LEVEL_TABLE + '.uexp'))
    names = {table[i].upper(): table[i + 1] for i in range(len(table) - 1) if re.fullmatch(r'LEVEL_\d\d_\w+', table[i])}
    return [(i, row.split('_', 2)[2], names.get(row.upper(), row)) for i, row in enumerate(order)]


def screen_names(index, code):
    """Asset names that may hold a level's loading screen, most specific first."""
    return [f'LoadScreen_{code}', f'LoadingScreen_{index:02d}_{code}', f'LoadScreen_{code.rstrip("0123456789")}']


# Pixel formats: (pixels per element side, bytes per element, Pillow "bcn" variant or None = BGRA).
FORMATS = {'PF_DXT1': (4, 8, 1), 'PF_DXT5': (4, 16, 3), 'PF_B8G8R8A8': (1, 4, None)}


def texture(uexp):
    """(pixel format, width, height, payload offset, pitch, rows) of a single-mip texture in a
    format of FORMATS, or None. pitch and rows: the padded surface in elements."""
    at = uexp.find(b'PF_')
    end = uexp.find(b'\0', at)
    if at < 16 or end < 0:
        return None
    pixel_format = uexp[at:end].decode('latin1')
    if pixel_format not in FORMATS:
        return None
    width, height = struct.unpack_from('<ii', uexp, at - 16)  # SizeX, SizeY, packed slices, name length
    side, element, _bcn = FORMATS[pixel_format]
    if width <= 0 or height <= 0:
        return None
    # The surface is padded to whole 8x8-element micro tiles.
    pitch, rows = ((width + side - 1) // side + 7) // 8 * 8, ((height + side - 1) // side + 7) // 8 * 8
    size = pitch * rows * element
    mark = uexp.find(struct.pack('<ii', size, size), end)
    if mark < 0 or mark + 16 + size > len(uexp):
        return None
    return pixel_format, width, height, mark + 16, pitch, rows


def _tiled_order(pitch, rows):
    """For each element of a linear surface, its index in the tiled one. Tiles go row by row, so
    any run of whole tile rows (rows a multiple of 8) is a surface of its own."""
    tiles_per_row = pitch // 8
    for y in range(rows):
        ybits = (y & 1) << 1 | ((y >> 1) & 1) << 3 | ((y >> 2) & 1) << 5
        tile_row = (y >> 3) * tiles_per_row
        for x in range(pitch):
            yield (tile_row + (x >> 3)) * 64 + ((x & 1) | ((x >> 1) & 1) << 2 | ((x >> 2) & 1) << 4 | ybits)


def detile(raw, pitch, rows, element):
    """1D thin tiled elements to linear (row by row)."""
    linear = bytearray(pitch * rows * element)
    for dst, src in enumerate(_tiled_order(pitch, rows)):
        linear[dst * element:(dst + 1) * element] = raw[src * element:(src + 1) * element]
    return bytes(linear)


def tile(linear, pitch, rows, element):
    """Linear elements to 1D thin tiling: the inverse of detile."""
    raw = bytearray(pitch * rows * element)
    for src, dst in enumerate(_tiled_order(pitch, rows)):
        raw[dst * element:(dst + 1) * element] = linear[src * element:(src + 1) * element]
    return bytes(raw)


def decode(uexp):
    """A texture's RGBA picture (single mip, 1D thin tiling), or None for any other format."""
    from PIL import Image
    info = texture(uexp)
    if not info:
        return None
    pixel_format, width, height, at, pitch, rows = info
    side, element, bcn = FORMATS[pixel_format]
    linear = detile(uexp[at:at + pitch * rows * element], pitch, rows, element)
    if bcn:
        picture = Image.frombytes('RGBA', (pitch * side, rows * side), linear, 'bcn', bcn)
    else:
        picture = Image.frombytes('RGBA', (pitch, rows), linear, 'raw', 'BGRA')
    return picture.crop((0, 0, width, height))


TEXTURES = {'menu/frame_options.png': 'OVERBITE/Content/UI/Textures/Options/UI_Options_Frame'}
PROMPTS = 'OVERBITE/Content/UI/Textures/ButtonPrompts/'
FONTS = {
    'fonts/title.ttf': 'OVERBITE/Content/UI/Fonts/FontesqueOffcPro-Extrabold.ufont',
    'fonts/text.ttf': 'OVERBITE/Content/UI/Fonts/1529_Champ_Fleury_Pro-Reg.ufont',
}


def build(game, user_dir, log=print):
    """Writes the cache when missing or made from another game folder; returns its folder."""
    out = Path(user_dir) / 'cache' / 'game_ui'
    stamp = out / 'source.txt'
    source = f'{VERSION}\n{Path(game).resolve()}\n'
    if stamp.is_file() and stamp.read_text(encoding='utf-8') == source:
        return out
    from PIL import Image
    for folder in ('levels', 'menu', 'prompts', 'fonts'):
        (out / folder).mkdir(parents=True, exist_ok=True)
    paks = Paks(game)
    rows = levels(paks)
    (out / 'levels.tsv').write_text(''.join(f'{i}\t{code}\t{name}\n' for i, code, name in rows), encoding='utf-8')
    pictures = 0
    for index, code, _name in rows:
        for name in screen_names(index, code):
            path = SCREENS + name + '.uexp'
            if path not in paks.where:
                continue
            picture = decode(paks.read(path))
            if picture:
                picture.convert('RGB').resize(PICTURE, Image.LANCZOS).save(out / 'levels' / f'level_{index:02d}.png')
                pictures += 1
                break
    textures = dict(TEXTURES)
    for name in paks.where:
        if name.startswith(PROMPTS) and name.endswith('.uexp'):
            textures['prompts/' + name[len(PROMPTS):-5] + '.png'] = name[:-5]
    made = 0
    for file, asset in textures.items():
        picture = decode(paks.read(asset + '.uexp')) if asset + '.uexp' in paks.where else None
        if picture:
            picture.save(out / file)
            made += 1
    for file, asset in FONTS.items():
        if asset in paks.where:
            (out / file).write_bytes(paks.read(asset))
            made += 1
    stamp.write_text(source, encoding='utf-8')
    log(f'Game UI: {len(rows)} level names, {pictures} level pictures, {made} of '
        f'{len(textures) + len(FONTS)} menu, prompt and font files in {out}')
    return out


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('usage: game_ui_assets.py <game dir> <user dir>')
    build(sys.argv[1], sys.argv[2])
