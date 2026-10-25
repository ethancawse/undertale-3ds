"""Roomplan source-command extension, from public ROOM/BGND/TPAG structures.

No game code, strings, pixels or recorded scene lists are inspected. Legacy
GMS1 only; unsupported data uses room-load construction. Same DTRP identity
header as build_plan.py, version 2 for source commands rather
than version 1's prefetch records. Files remain private build products.
"""
from pathlib import Path
import argparse
import math
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dwinfo import DataWinInfo

MAGIC = 0x50525444
VERSION = 2
HEADER = struct.Struct('<II32sIIQIIIIII16s')  # RoomPlanHeader: 96 bytes
COMMAND = struct.Struct('<ii8fIf')          # UtpCommand: 48 bytes
TILE = struct.Struct('<5i2IiI2fI')          # public readRoomTiles fields
MAX_SIZE = 16 * 1024 * 1024
MAX_ROOM_TILES = 65536


def fnv1a(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def command(tile, page, tpag):
    """Canonical source helper, with explicit float32 arithmetic.

    Only exact integer arithmetic/unit scales and opaque alpha are emitted.
    All other cases carry valid=-1 and use the C room-load constructor.
    """
    x, y, _bg, sx, sy, width, height, _depth, _id, scale_x, scale_y, color = tile
    if (scale_x != 1.0 or scale_y != 1.0 or (color >> 24) not in (0, 255)
        or any(abs(v)>1048576 for v in (x,y,sx,sy))
        or width>65535 or height>65535):
        return struct.pack('<i', -1)+bytes(COMMAND.size-4)
    if tpag < 0:
        return bytes(COMMAND.size)
    _px, _py, pw, ph, tx, ty, *_rest = page
    dx, dy = f32(x), f32(y)
    if tx > sx:
        dx = f32(dx + f32((tx - sx) * scale_x))
        width -= tx - sx
        sx = tx
    if ty > sy:
        dy = f32(dy + f32((ty - sy) * scale_y))
        height -= ty - sy
        sy = ty
    width = min(width, tx + pw - sx)
    height = min(height, ty + ph - sy)
    if width <= 0 or height <= 0:
        return bytes(COMMAND.size)
    alpha = (color >> 24) & 255
    alpha = 1.0 if alpha == 0 else f32(alpha / 255.0)
    return COMMAND.pack(1, tpag, sx-tx, sy-ty, width, height, dx, dy,
                        scale_x, scale_y, color & 0xffffff, alpha)


def build(path):
    info = DataWinInfo(path)  # existing roomplan digest/key/grid parser
    data = Path(path).read_bytes()
    gen = info.chunks['GEN8'][0]
    major = struct.unpack_from('<I', data, gen+44)[0]
    if major >= 2:
        return None  # modern layers/animation use today's path

    def table(offset):
        if not 0 <= offset <= len(data)-4:
            raise ValueError('pointer table out of bounds')
        n, = struct.unpack_from('<I', data, offset)
        if n > (len(data)-offset-4)//4:
            raise ValueError('truncated pointer table')
        return struct.unpack_from('<%dI' % n, data, offset+4)

    tpag_ptrs = table(info.chunks['TPAG'][0])
    lookup = {offset: i for i, offset in enumerate(tpag_ptrs)}
    backgrounds = table(info.chunks['BGND'][0])
    bg_tpag = []
    for offset in backgrounds:
        if offset == 0:
            bg_tpag.append(-1)
        else:
            # parseBGND: name, transparent, smooth, preload, TPAG offset.
            pointer, = struct.unpack_from('<I', data, offset+16)
            bg_tpag.append(lookup.get(pointer, -1))
    rooms = table(info.chunks['ROOM'][0])
    scenes = []
    for index, offset in enumerate(rooms):
        if offset == 0:
            continue
        tiles_ptr, = struct.unpack_from('<I', data, offset+13*4)
        tiles = table(tiles_ptr)
        if not tiles or len(tiles)>MAX_ROOM_TILES:
            continue
        commands = []
        for tile_ptr in tiles:
            tile = TILE.unpack_from(data, tile_ptr)
            tpag = bg_tpag[tile[2]] if 0 <= tile[2]<len(bg_tpag) else -1
            page = info.tpag[tpag] if tpag>=0 else None
            if not all(math.isfinite(v) for v in tile[9:11]):
                break  # explicit runtime fallback for unusual source data
            commands.append(TILE.pack(*tile)+command(tile, page, tpag))
        else:
            scenes.append(struct.pack('<II', index, len(commands))+b''.join(commands))
    body = b''.join(scenes)
    header = HEADER.pack(MAGIC, VERSION, bytes.fromhex(info.sha256), info.size,
        info.game_id, info.timestamp, 256, 1, len(rooms), len(scenes), len(body),
        fnv1a(body), b'roomplan src1')
    result = header+body
    if len(result)>MAX_SIZE:
        raise ValueError('source plan exceeds bounded loader capacity')
    return result


def prepare(game, folder, output):
    """Builder/build.sh entry; missing/unsupported inputs leave no stale plan."""
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    # Only legacy UT is eligible. DR is deliberately live, not a failed plan.
    dest = output/(game+'-source.dtp')
    if game != 'undertale':
        dest.unlink(missing_ok=True)
        return False
    # UT packaging has a dependency even when data is unavailable. The loader
    # rejects an empty marker; modern games do not package this extension.
    dest.write_bytes(b'')
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'aot'))
    import prepare as aot_prepare
    folder = aot_prepare.find_data(game, folder)
    if not folder:
        return False
    path = Path(folder)/'data.win'
    if not path.is_file():
        return False
    payload = build(path)
    if payload is None:
        return False
    temporary = dest.with_suffix('.part')
    temporary.write_bytes(payload)
    temporary.replace(dest)
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--game', choices=('undertale', 'deltarune'), required=True)
    parser.add_argument('--data')
    parser.add_argument('--out', required=True)
    args = parser.parse_args()
    try:
        used = prepare(args.game, args.data, args.out)
        print('Roomplan source commands:', 'prepared' if used else 'room-load/live fallback')
    except (ValueError, OSError, struct.error, OverflowError, KeyError, IndexError) as exc:
        # Optional preparation must never prevent a build. The runtime falls
        # back to the current loaded room; no partially written file is used.
        print('Roomplan source commands: room-load fallback:', exc)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
