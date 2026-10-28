"""Build the per-room phase tables for the RomFS (romfs:/roomplan/phases.dtph), beside the room plan.

    py -3 -I tools/roomplan/build_phases.py --game <undertale | deltarune> --out <phases.dtph> [--data <game folder>]
        [--phases <dir>]

The room plan (build_plan.py) seeds what a room draws in its first frames: its first-use frames stop at 254 and each
scene at the Old 3DS budget, so a long fight draws the rest for the first time in play, one SD load each (Omega
Flowey, DELTARUNE's Flowery in room 226). A phase record (tools/roomplan/phases/*.json, the records' format: per room,
each texture page item with the frame it was first drawn since the room change, over every recorded visit; asset
numbers only) gives the whole visit. This splits each recorded room's tiles, in first-use order, into phases wherever
GAP_FRAMES or more pass without a new one. A phase's marker is its first tile. The runtime takes the first draw of a
phase's tile as the phase's start and readies the next phase's tiles in the background
(platform/3ds/source/z_phaseplan.c).

Format (little-endian, 4-byte aligned): one table per data.win with a phase record (DELTARUNE: one per chapter), end
to end. The runtime takes the table whose key is its texture cache's.
    header  u32 magic 'DTPH', u16 version 1, u16 rooms, then the data.win's key as the room plan has it (u32 size,
            u32 game id, u64 timestamp), u32 tile max, u32 grid version, u32 the data.win's room count, u32 body size,
            u32 FNV-1a of the body (44 bytes)
    room    u16 room, u16 phases, then per phase:
    phase   u16 first frame, u16 last frame (capped at 65535), u16 tiles, u16 0, u32 marker, u32 tiles[tiles]
A tile is the room plan's entry without its late bit (page << 8 | tile, the tile numbered row by row in the 3DS tile
grid). A room keeps at most MAX_PHASES phases and a phase at most MAX_TILES tiles. The same inputs always give the
same bytes.

Exit status: 0 written, 3 no game data or no phase record for it (the build goes on without one; a stale file is
removed), 2 an error.
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'aot'))
import build_plan  # noqa: E402
import dwinfo  # noqa: E402

MAGIC = 0x48505444  # 'DTPH'
VERSION = 1
GAP_FRAMES = 60
HEADER = '<IHHIIQIIIII'
MAX_PHASES = 32   # PHASEPLAN_PHASES (z_phaseplan.c)
MAX_TILES = 512   # PHASEPLAN_TILES


def phases_of(tiles, gap=GAP_FRAMES):
    """tiles: [(dframe, page, tile)] in first-use order -> [(first, last, [(page, tile)])]."""
    out = []
    for df, p, t in tiles:
        if not out or df - out[-1][1] >= gap:
            out.append([df, df, []])
        out[-1][1] = df
        out[-1][2].append((p, t))
    return [(a, b, ts) for a, b, ts in out]


def encode(info, record):
    rooms = []
    for scene in sorted(record['scenes'], key=lambda s: s['room']):
        if scene['sub'] != -1:
            continue
        ph = [(a, b, ts[:MAX_TILES]) for a, b, ts in phases_of(build_plan.scene_tiles(info, scene))[:MAX_PHASES]]
        body = struct.pack('<HH', scene['room'], len(ph))
        for first, last, ts in ph:
            entries = [p << 8 | t for p, t in ts]
            body += struct.pack('<HHHHI', min(first, 0xFFFF), min(last, 0xFFFF), len(entries), 0, entries[0])
            body += struct.pack('<%dI' % len(entries), *entries)
        rooms.append(body)
    body = b''.join(rooms)
    head = struct.pack(HEADER, MAGIC, VERSION, len(rooms), info.size & 0xFFFFFFFF, info.game_id, info.timestamp,
                       dwinfo.TILE_MAX, dwinfo.GRID_VERSION, info.room_count, len(body), build_plan.fnv1a(body))
    assert len(head) == 44
    return head + body


def table_for(path, records):
    """(0, table); (3, None): no phase record for this data.win; (2, None): a record that doesn't fit it."""
    info = dwinfo.DataWinInfo(path)
    record = build_plan.find_record(records, info.sha256)
    if record is None:
        print('Phase plan: no phase record for %s (SHA-256 %s)' % (path, info.sha256))
        return 3, None
    if record.get('format') != 1 or record.get('room_count') != info.room_count or record.get('tpag_count') != len(info.tpag):
        print('Phase plan: the record for SHA-256 %s does not fit this data.win' % info.sha256, file=sys.stderr)
        return 2, None
    data = encode(info, record)
    print('Phase plan: %s, %d rooms, %d bytes' % (path, struct.unpack_from('<H', data, 6)[0], len(data)))
    return 0, data


def write_phases(paths, records, out):
    """0 written, 3 no phase record for any of the data.win files (nothing written, a stale file removed), 2 an error."""
    tables = []
    for path in paths:
        rc, data = table_for(path, records)
        if rc == 2:
            return 2
        if data is not None:
            tables.append(data)
    if not tables:
        if os.path.exists(out):
            os.remove(out)
        return 3
    data = b''.join(tables)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    tmp = out + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, out)
    print('Phase plan: %s, %d tables, %d bytes' % (out, len(tables), len(data)))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default='undertale', choices=['undertale', 'deltarune'])
    ap.add_argument('--data', help='the game folder (default: as tools/aot/prepare.py finds it)')
    ap.add_argument('--data-win', help='the data.win itself (tests)')
    ap.add_argument('--phases', default=os.path.join(HERE, 'phases'))
    ap.add_argument('--out', required=True)
    a = ap.parse_args(argv)
    if a.data_win is not None:
        found = [('root', a.data_win, 0)]
    else:
        import prepare  # tools/aot/prepare.py: the same search as the AOT step
        import corpus
        folder = prepare.find_data(a.game, a.data)
        found = corpus.parts(a.game, folder) if folder else []
    found = [f for f in found if os.path.isfile(f[1])]
    if not found:
        print('Phase plan: no %s data.win found' % a.game)
        return 3
    if a.game == 'undertale':
        found = found[:1]
    return write_phases([path for _part, path, _chapter in found], a.phases, a.out)


if __name__ == '__main__':
    sys.exit(main())
