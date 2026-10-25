"""Build the room plan for the RomFS (romfs:/roomplan/<game>.dtp; format: platform/3ds/source/roomplan_format.h).

    py -3 -I tools/roomplan/build_plan.py --game undertale --out <file.dtp> [--data <game folder>] [--records <dir>]
    py -3 -I tools/roomplan/build_plan.py --game deltarune --out <dir> [--data <game folder>] [--records <dir>]

DELTARUNE has a data.win per chapter: --out is a folder, and each chapter with a record gets <chapter folder>.dtp there
(romfs:/roomplan/chapter1_windows.dtp ...); chapters without one get none.

Finds the game's data.win (--data, $CTRPORT_UNDERTALE_DATA, or the Steam library, as tools/aot/prepare.py does),
picks the committed record (tools/roomplan/records/*.json) made on that exact file (SHA-256), resolves its texture
page items to page tiles in the 3DS tile grid, and writes, per scene in first-use order: the tiles (rooms.dtm's
encoding, late when first drawn after the scene's first drawn frame) with their first-use frame, and the code entries
first run from the room's first frame on (read back at the room change on the Old models, phase 2).
Each scene stops at ROOM_TILES_MAX tiles and at the Old 3DS texture budget (counted at 4 bytes a texel, an upper
bound); UNDERTALE's whole-set scenes at twice that (the budget at the 2 bytes a texel most tiles take: the runtime
counts the real formats and loads the start that fits, platform/3ds/source/z_utstall.c rule A, and keeps a RAM copy
of the pages the rest comes from, rule B). A battle room's room-level scene keeps the common tiles of its recorded
fallback groups (all groups unless the record fixes battle_frame_groups to an earlier cohort, preserving the
first-battle preload as coverage grows);
its groups are scenes of their own (their code entries; the runtime picks the group at the room change). Each group's
code also lists its objects' other events and the code SHARED_GROUPS groups ran (battle_code). The same inputs always
give the same bytes.

Exit status: 0 written, 3 no game data or no record for it (the build goes on without a plan), 2 an error.
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'aot'))
import closure as closure_mod  # noqa: E402
import dwinfo  # noqa: E402

MAGIC = 0x50525444
VERSION = 1
BUILDER = b'roomplan 1'
NO_SUB = 0xFFFF
TILES_MAX = 512            # ROOM_TILES_MAX (ctr_textures.c)
LATE = 0x80000000
RECORD_FRAMES = 45         # ROOM_RECORD_FRAMES: rooms.dtm keeps the first 45 frames; the runtime seeds those
CAP_BYTES = 20 * 1024 * 1024  # the Old 3DS texture budget is about 20.3 MiB; tiles counted at 4 bytes a texel
# UNDERTALE's whole sets (z_utstall.c rules A and B): 4 bytes a texel is twice what most tiles take (RGBA5551), and
# the 20 MiB cap cut the largest scene's attacks (fought from frame 850 on) and two of its first tiles, which at
# their formats fit with room to spare.
WHOLE_CAP_BYTES = 2 * CAP_BYTES
MAX_SIZE = 1024 * 1024     # ROOMPLAN_MAX_SIZE
LATER = 255                # the bucket of a tile first used after frame 254, or never seen (the whole set)
# A battle room's code that at least this many of its recorded battle groups ran is the battle machinery any fight can
# reach (a kill's dust, sparing, fleeing): every group's scene lists it. The record's UNDERTALE fights: 112 groups,
# only 16 of them ended in a kill, so the other 96 read the kill's code in play, in the frame the monster dies (o3ds:
# two reads, 43 ms). At 10 groups: about 52 more entries a group, 18 KB of bytecode.
SHARED_GROUPS = 10


def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def scene_tiles(info, scene):
    """[(dframe, page, tile)] first use of each tile, in first-use order."""
    first = {}
    for df, tpag in scene['items']:
        page, tiles = info.item_tiles(tpag)
        for t in tiles:
            k = (page, t)
            first[k] = min(df, first.get(k, df))
    for df, tpag, x, y, w, h in scene['parts']:
        page, tiles = info.item_tiles(tpag, (x, y, w, h))
        for t in tiles:
            k = (page, t)
            first[k] = min(df, first.get(k, df))
    return sorted((df, p, t) for (p, t), df in first.items())


def whole_set(info, closure, room, scene, tiles):
    """UNDERTALE: the tiles the room can draw that its record didn't see, as (LATER, page, tile) after the record's
    (platform/3ds/source/z_utstall.c, item 5): every frame of each sprite the record drew from, and the room's own
    content (tools/roomplan/closure.py). A battle group adds every frame of the sprite of each object whose code the
    group ran, visible or not: a monster's object is invisible, and its sprite is the one drawn when it is hit, a
    texture load on the game thread at the first hit (rule A). build() passes a group's battle_code
    list, so the objects whose other events or shared code the group may run (the FIGHT target and strike, the dust
    of a kill) count too. Sorted by page and tile."""
    if closure is None:
        return []
    have = {(p, t) for _df, p, t in tiles}
    extra = set()

    def add(tpag, rect=None):
        page, ts = info.item_tiles(tpag, rect)
        if page is not None:
            extra.update((page, t) for t in ts)
    for tpag in {t for _df, t in scene['items']} | {part[1] for part in scene['parts']}:
        for frame in closure.frames(tpag):
            add(frame)
    if 0 <= scene['sub'] < NO_SUB:
        code_sprite = getattr(closure, 'code_sprite', [])
        for c in sorted({c for df, c in scene['code'] if df >= 0}):
            s = code_sprite[c] if 0 <= c < len(code_sprite) else -1
            if 0 <= s < len(closure.sprite_items):
                for tpag in closure.sprite_items[s]:
                    add(tpag)
    if 0 <= room < len(closure.plan_items):
        for tpag in closure.plan_items[room]:
            add(tpag)
        for tpag, x, y, w, h in closure.plan_parts[room]:
            add(tpag, (x, y, w, h))
    return [(LATER, p, t) for p, t in sorted(extra - have)]


def capped(info, tiles, cap=CAP_BYTES):
    out, total = [], 0
    for df, p, t in tiles:
        b = info.tile_bytes(p)
        if len(out) >= TILES_MAX or total + b > cap:
            break
        out.append((df, p, t))
        total += b
    return out, total


def battle_code(info, subs):
    """{sub: [(dframe, code)]} for a battle room's groups: each group's own code, then its objects' other events (an
    object whose event ran can reach the rest: a monster's Destroy at the kill), then the code SHARED_GROUPS groups
    ran, most shared first. The added entries count as first run at LATER."""
    own = {s['sub']: [(df, c) for df, c in s['code'] if df >= 0] for s in subs}
    counts = {}
    for code in own.values():
        for c in {c for _df, c in code}:
            counts[c] = counts.get(c, 0) + 1
    shared = sorted((c for c, k in counts.items() if k >= SHARED_GROUPS), key=lambda c: (-counts[c], c))
    owners = getattr(info, 'code_owner', [])
    events = {}
    for c, o in enumerate(owners):
        if o is not None:
            events.setdefault(o, []).append(c)
    out = {}
    for sub, code in own.items():
        have = {c for _df, c in code}
        extra = []
        for o in sorted({owners[c] for c in have if 0 <= c < len(owners) and owners[c] is not None}):
            extra += [c for c in events[o] if c not in have]
        have.update(extra)
        extra += [c for c in shared if c not in have]
        out[sub] = code + [(LATER, c) for c in extra]
    return out


def encode_scene(info, room, sub, tiles, code, cap=CAP_BYTES):
    """tiles: [(dframe, page, tile)] in order. Late: first drawn after the scene's first drawn frame."""
    tiles, total = capped(info, tiles, cap)
    first = tiles[0][0] if tiles else 0
    entries = [(LATE if df > first else 0) | p << 8 | t for df, p, t in tiles]
    buckets = bytes(min(df, 255) for df, _p, _t in tiles)
    # Code first run in the room change itself (frame -1: the new room's Create events) has run before the read-ahead.
    code = [c for df, c in code if df >= 0 and 0 <= c < 0x10000]
    body = struct.pack('<HHHHI', room, sub, len(entries), len(code), total)
    body += struct.pack('<%dI' % len(entries), *entries)
    body += buckets + b'\0' * (-len(buckets) % 4)
    cb = struct.pack('<%dH' % len(code), *code)
    body += cb + b'\0' * (-len(cb) % 4)
    return body


def build(info, record, closure=None):
    """closure: a closure.Closure for UNDERTALE's whole sets, or None (DELTARUNE: the record's tiles only)."""
    scenes = []
    cap = WHOLE_CAP_BYTES if closure is not None else CAP_BYTES
    by_room = {}
    for s in record['scenes']:
        by_room.setdefault(s['room'], []).append(s)
    for room in sorted(by_room):
        group = by_room[room]
        whole = [s for s in group if s['sub'] < 0]
        subs = sorted((s for s in group if s['sub'] >= 0), key=lambda s: s['sub'])
        if whole:
            s = whole[0]
            tiles = scene_tiles(info, s)
            scenes.append(encode_scene(info, room, NO_SUB, tiles + whole_set(info, closure, room, s, tiles), s['code'], cap))
        elif subs:
            # New outlier fights must not shrink an established first-battle preload.
            frame_groups = record.get('battle_frame_groups', {}).get(str(room))
            frame_subs = subs
            if frame_groups is not None:
                by_sub = {s['sub']: s for s in subs}
                if not frame_groups or len(set(frame_groups)) != len(frame_groups) or any(g not in by_sub for g in frame_groups):
                    raise ValueError('invalid battle_frame_groups for room %d' % room)
                frame_subs = [by_sub[g] for g in sorted(frame_groups)]
            per = [{(p, t): df for df, p, t in scene_tiles(info, s)} for s in frame_subs]
            common = set(per[0])
            for m in per[1:]:
                common &= set(m)
            tiles = sorted((max(m[k] for m in per), k[0], k[1]) for k in common)
            scenes.append(encode_scene(info, room, NO_SUB, tiles, []))
        codes = battle_code(info, subs)
        for s in subs:
            if 0 <= s['sub'] < NO_SUB:
                tiles = scene_tiles(info, s)
                # Rule A reads the group's whole code list (battle_code): the battle's shared objects' sprites too.
                whole = whole_set(info, closure, room, dict(s, code=codes[s['sub']]), tiles)
                scenes.append(encode_scene(info, room, s['sub'], tiles + whole, codes[s['sub']], cap))
    body = b''.join(scenes)
    head = struct.pack('<II32sIIQIIIIII16s', MAGIC, VERSION, bytes.fromhex(info.sha256), info.size & 0xFFFFFFFF,
                       info.game_id, info.timestamp, dwinfo.TILE_MAX, dwinfo.GRID_VERSION, info.room_count, len(scenes),
                       len(body), fnv1a(body), BUILDER)
    assert len(head) == 96
    data = head + body
    if len(data) > MAX_SIZE:
        raise ValueError('plan of %d bytes is over %d' % (len(data), MAX_SIZE))
    return data


def find_record(records_dir, sha):
    if not os.path.isdir(records_dir):
        return None
    for name in sorted(os.listdir(records_dir)):
        if name.endswith('.json'):
            with open(os.path.join(records_dir, name), 'r', encoding='ascii') as f:
                record = json.load(f)
            if record.get('data_sha256') == sha:
                return record
    return None


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default='undertale', choices=['undertale', 'deltarune'])
    ap.add_argument('--data', help='the game folder (default: as tools/aot/prepare.py finds it)')
    ap.add_argument('--data-win', help='the data.win itself (tests)')
    ap.add_argument('--records', default=os.path.join(HERE, 'records'))
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
        print('Room plan: no %s data.win found' % a.game)
        return 3
    if a.game == 'undertale':
        return write_plan(found[0][1], a.records, a.out, whole=True)
    written = 0
    for part, path, _chapter in found:
        rc = write_plan(path, a.records, os.path.join(a.out, part + '.dtp'))
        if rc == 2:
            return 2
        written += rc == 0
    return 0 if written else 3


def write_plan(path, records, out, whole=False):
    """0 written, 3 no record for this data.win, 2 a record that doesn't fit. whole: add each scene's whole set (UNDERTALE)."""
    info = dwinfo.DataWinInfo(path)
    record = find_record(records, info.sha256)
    if record is None:
        print('Room plan: no record for %s (SHA-256 %s)' % (path, info.sha256))
        return 3
    if record.get('format') != 1 or record.get('room_count') != info.room_count or record.get('tpag_count') != len(info.tpag):
        print('Room plan: the record for SHA-256 %s does not fit this data.win' % info.sha256, file=sys.stderr)
        return 2
    closure = None
    if whole:
        closure = closure_mod.Closure(path)
        if not closure.ok:
            print('Room plan: no whole sets (not a GameMaker 1.x data.win this builder can read)')
            closure = None
    data = build(info, record, closure)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    tmp = out + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, out)
    print('Room plan: %s, %d scenes, %d bytes, record %s %s %s' % (out, struct.unpack_from('<I', data, 68)[0], len(data),
                                                              record['game'], record['store'], record['version']))
    return 0

if __name__ == '__main__':
    sys.exit(main())
