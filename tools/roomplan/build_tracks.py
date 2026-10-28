"""Build the New 3DS DELTARUNE track plan (romfs:/roomplan/tracks.dtp; read by platform/3ds/source/z_drresid.c).

    py -3 -I tools/roomplan/build_tracks.py --out <dir> [--data <game folder>] [--records <file.json>]

A track is what one fight in an oversized room draws after the room's first frames: its tiles in first-use order, each
with its first-use frame counted from the track's activation (the fight's first tile no other track of the room draws).
A track with no activation tiles is the room's entry track: it starts at the room change, with the tiles every recorded
scene of the room draws before its own fight starts. A track with no tiles only claims its fight: once one of its
activation tiles is drawn, no other track of the room starts. The records (tools/roomplan/tracks/deltarune.json) are indices only, from our New 3DS recorder runs; each chapter's
tracks are written only for the exact data.win they were recorded on (SHA-256), and the runtime uses a section only
for the texture pack key (data.win size, game ID, timestamp) and room count it was made for.

File (little-endian, 4-byte aligned):
    u32 magic "DTRK", u32 version, u32 sectionCount
    per section: u32 dataWinSize, u32 gameId, u64 timestamp, u32 roomCount, u32 trackCount
    per track:   u16 room, u16 activationCount, u16 tileCount, u16 0,
                 activationCount x u32 entry, tileCount x u32 entry, tileCount x u16 frame (padded to 4)
    entry: page << 8 | tile (rooms.dtm's encoding without the late bit)

Exit status: 0 written, 3 no game data or no record for it (the build goes on without), 2 an error.
"""
import argparse
import hashlib
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'aot'))
import dwinfo  # noqa: E402

MAGIC = 0x4B525444  # "DTRK"
VERSION = 1
TILES_MAX = 512
ACT_MAX = 16


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def encode_track(track, pages):
    act = [p << 8 | t for p, t in track['activation']][:ACT_MAX]
    tiles = track['tiles'][:TILES_MAX]
    for p, t, *_ in [*track['activation'], *tiles]:
        if p >= len(pages) or t > 255:
            raise ValueError('track %s: tile %d/%d outside the data.win' % (track['track'], p, t))
    body = struct.pack('<HHHH', track['room'], len(act), len(tiles), 0)
    body += struct.pack('<%dI' % len(act), *act)
    body += struct.pack('<%dI' % len(tiles), *[p << 8 | t for p, t, _ in tiles])
    frames = struct.pack('<%dH' % len(tiles), *[min(f, 0xFFFF) for _, _, f in tiles])
    return body + frames + b'\0' * (-len(frames) % 4)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', help='the game folder (default: as tools/aot/prepare.py finds it)')
    ap.add_argument('--records', default=os.path.join(HERE, 'tracks', 'deltarune.json'))
    ap.add_argument('--out', required=True)
    a = ap.parse_args(argv)
    if not os.path.isfile(a.records):
        print('Track plan: no records')
        return 3
    with open(a.records, 'r', encoding='ascii') as f:
        records = json.load(f)
    if records.get('format') != 1:
        print('Track plan: unknown record format', file=sys.stderr)
        return 2
    import prepare  # tools/aot/prepare.py: the same search as the AOT step
    import corpus
    folder = prepare.find_data('deltarune', a.data)
    found = [p for p in (corpus.parts('deltarune', folder) if folder else []) if os.path.isfile(p[1])]
    by_size = {}
    for _part, path, _chapter in found:
        by_size.setdefault(os.path.getsize(path), []).append(path)
    sections = []
    for chapter in records['chapters']:
        for path in by_size.get(chapter['data_size'], []):
            if sha256(path) != chapter['data_sha256']:
                continue
            info = dwinfo.DataWinInfo(path)
            body = b''.join(encode_track(t, info.pages) for t in chapter['tracks'])
            sections.append(struct.pack('<IIQII', info.size & 0xFFFFFFFF, info.game_id, info.timestamp, info.room_count,
                                        len(chapter['tracks'])) + body)
            print('Track plan: chapter %d, %d tracks' % (chapter['chapter'], len(chapter['tracks'])))
    if not sections:
        print('Track plan: no record for this game data')
        return 3
    data = struct.pack('<III', MAGIC, VERSION, len(sections)) + b''.join(sections)
    os.makedirs(a.out, exist_ok=True)
    out = os.path.join(a.out, 'tracks.dtp')
    with open(out + '.tmp', 'wb') as f:
        f.write(data)
    os.replace(out + '.tmp', out)
    print('Track plan: %s, %d bytes' % (out, len(data)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
