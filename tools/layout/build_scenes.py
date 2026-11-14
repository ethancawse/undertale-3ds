"""Build UNDERTALE's scene table for the RomFS (romfs:/layout/undertale.lst; format and runtime:
platform/3ds/source/ctr_layout.h and z_layout.c, change layout S3).

    py -3 -I tools/layout/build_scenes.py --out <file.lst> [--data <game folder>] [--data-win <data.win>] [--spec <json>]

The spec (tools/layout/undertale-scenes.json, committed) names everything: rooms and objects by name, battle groups,
bands, lifts and classes. Nothing from the game is committed. This resolves those names against the player's own
data.win at build time and writes numbers only: room and object indices, classes and rows, keyed to that data.win
(its size, game id and timestamp, as CtrPack_keyFor reads them, and its room and object counts), so a table made for
other data is refused whole on the console.

A spec scene:
    room     a room name, or "*" for every room
    group    a battle group (0-65534); none: the room alone (the Flowey tutorial's kind of scene)
    split    true: admitted (the scene splits)
    band     native rows the top screen shows (1-255); none: the standard band
    lift     rows preset A moves the bottom up (0-254); none: the standard lift
    mode     placement mode (0-255); none: 0, standard. Others are reserved for battlesplit (close seam, ...)
    entries  "class[+soul]:object", class one of top, bottom, by_row, both, by_call, cover, original; "+soul" marks a
             SOUL object (bsNames' BS_SOUL); a trailing "*" takes every object whose name starts with the rest. An
             object takes its first matching entry (bsNames' rule), so each object is listed at most once per scene.
             original (battlesplit16, an encounter's scene only, no "+soul"): an end trigger, drawn as top; once it has
             an instance the rest of the battle is Original (bsEnds16; Asgore's ending). That holds until the room
             index changes, not per encounter: a restart of the battle room stays Original too. Group 91 hands over
             to group 92 that way (it sets the battle group and restarts the room), so an end trigger in group 91
             would keep 92 Original as well. The console watches at most
             ENDS objects of an encounter (class original and every unlisted child of one); a scene whose family is
             larger is left out (Original) with a line saying so, as the console would keep it Original.

A name the data.win doesn't have (another release, a patch) leaves out every scene naming it, whole: that encounter
stays Original and the rest are kept, one line each. Only the every-room row's names drop the table. So does a scene
with more end triggers than the console watches (ENDS).

Exit status: 0 written; 3 no game data, or a name of the every-room row the data.win doesn't have (no table:
every battle is Original, loudly, and the build goes on); 2 an error (a bad spec or data.win).
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'aot'))

SPEC = os.path.join(HERE, 'undertale-scenes.json')
MAGIC, VERSION = 0x594C5444, 1   # CTR_LAYOUT_TABLE_MAGIC ("DTLY"), CTR_LAYOUT_TABLE_VERSION
ANY = 0xFFFF                     # CTR_LAYOUT_ANY
LIFT_DEFAULT = 0xFF              # CTR_LAYOUT_LIFT_DEFAULT
SCENE_SPLIT = 1                  # CTR_LAYOUT_SCENE_SPLIT
TABLE_MAX = 65536                # CTR_LAYOUT_TABLE_MAX
HEADER = struct.Struct('<4IQ2I2H3I')  # CtrLayoutTableHeader, 48 B
SCENE = struct.Struct('<4H4B')        # CtrLayoutScene, 12 B
ENTRY = struct.Struct('<HBB')         # CtrLayoutEntry, 4 B
CLASSES = ('top', 'bottom', 'by_row', 'both', 'by_call', 'cover', 'original')
ORIGINAL = 6                          # BS_ORIGINAL
ENDS = 2                              # BS_ENDS16: end triggers the console watches per encounter
SOUL = 8                              # BS_SOUL
KEYS = {'room', 'group', 'split', 'band', 'lift', 'mode', 'entries'}


class Unresolved(Exception):
    """A name the data.win doesn't have."""


class TooManyEnds(str):
    """A left-out scene's line: more end triggers than the console watches (z_battlesplit.c's bsBuildClasses14 would
    keep that encounter Original whole; c26rev5's F1 on battlesplit16)."""


def fnv1a(data):
    """RoomPlan_fnv1a (platform/3ds/source/z_roomplan.c)."""
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


class Names:
    """A data.win's key and its room and object names (kept in memory only)."""

    def __init__(self, path):
        with open(path, 'rb') as f:
            d = f.read()
        if d[:4] != b'FORM':
            raise ValueError('not a data.win')
        self.size = len(d)
        chunks, o = {}, 8
        while o + 8 <= len(d):
            n = struct.unpack_from('<I', d, o + 4)[0]
            chunks[d[o:o + 4].decode('ascii', 'replace')] = o + 8
            o += 8 + n
        gen8 = chunks['GEN8']
        self.game_id = struct.unpack_from('<I', d, gen8 + 20)[0]
        # As tools/roomplan/dwinfo.py (the engine's DataWin.gen8.timestamp): 32-bit and sign-extended through WAD 12.
        if d[gen8 + 1] <= 12:
            self.timestamp = struct.unpack_from('<i', d, gen8 + 92)[0] & 0xFFFFFFFFFFFFFFFF
        else:
            self.timestamp = struct.unpack_from('<Q', d, gen8 + 92)[0]
        self.rooms = self._names(d, chunks.get('ROOM'))
        self.objects = self._names(d, chunks.get('OBJT'))
        self.parents = self._parents(d, chunks.get('OBJT'))

    @staticmethod
    def _names(d, base):
        if base is None:
            return []
        n = struct.unpack_from('<I', d, base)[0]
        out = []
        for p in struct.unpack_from('<%dI' % n, d, base + 4):
            at = struct.unpack_from('<I', d, p)[0]
            out.append(d[at:d.index(0, at)].decode('utf-8', 'replace'))
        return out


    @staticmethod
    def _parents(d, base):
        """Each object's parent index (OBJT item: name, sprite, visible, solid, depth, persistent, parent), or None
        when the items are too short to hold one (a names-only data.win, as the tests make)."""
        if base is None:
            return None
        n = struct.unpack_from('<I', d, base)[0]
        at = struct.unpack_from('<%dI' % n, d, base + 4)
        if any(b - a < 28 for a, b in zip(at, at[1:])) or (n and at[-1] + 28 > len(d)):
            return None
        return [struct.unpack_from('<i', d, p + 24)[0] for p in at]


def end_family(objects, parents, every, own):
    """The objects the console's classes make end triggers: own class original, or no own class (neither this scene
    nor the every-room row lists it) and the nearest listed ancestor's class original, as bsInherit walks (32 steps).
    The hazard ledger is left out: it only takes objects away from the family."""
    listed = dict(every)
    listed.update(own)
    out = []
    for i in range(len(objects)):
        j, c = i, None
        for _ in range(33):
            if j in listed:
                c = listed[j] & 7
                break
            if parents is None or not 0 <= j < len(parents):
                break
            j = parents[j]
            if not 0 <= j < len(objects):
                break
        if c == ORIGINAL:
            out.append(i)
    return out


def parse_entry(text):
    cls, sep, name = text.partition(':')
    soul = cls.endswith('+soul')
    if soul:
        cls = cls[:-5]
    if not sep or cls not in CLASSES or not name.rstrip('*') or '*' in name[:-1] or (soul and cls == 'original'):
        raise ValueError('bad entry %r' % text)
    prefix = name.endswith('*')
    return CLASSES.index(cls) | (SOUL if soul else 0), name.rstrip('*'), prefix


def _int(scene, key, low, high, default):
    v = scene.get(key)
    if v is None:
        return default
    if not isinstance(v, int) or isinstance(v, bool) or not low <= v <= high:
        raise ValueError('scene %r: bad %s %r' % (scene.get('room'), key, v))
    return v


def _resolve_names(sc, rules, rooms, names):
    """The scene's room index and {object: class}; raises Unresolved for a name the data.win doesn't have."""
    g = sc.get('group', 'any')
    if sc['room'] == '*':
        room = ANY
    elif sc['room'] in rooms:
        room = rooms[sc['room']]
    else:
        raise Unresolved('room %s (group %s)' % (sc['room'], g))
    cls = {}
    for c, name, prefix in rules:
        hit = False
        for i, own in enumerate(names.objects):
            if own.startswith(name) if prefix else own == name:
                hit = True
                cls.setdefault(i, c)
        if not hit:
            raise Unresolved('object %s (room %s, group %s)' % (name, sc['room'], g))
    return room, cls


def resolve(spec, names, dropped=None):
    """[(room, group, band, lift, mode, flags, [(object, class)])], sorted. A scene naming something the data.win
    doesn't have is left out whole (its message appended to dropped); the every-room row's raises Unresolved."""
    if spec.get('format') != 1 or spec.get('game') != 'undertale' or not isinstance(spec.get('scenes'), list):
        raise ValueError('not a format 1 UNDERTALE scene spec')
    rooms = {}
    for i, n in enumerate(names.rooms):
        rooms.setdefault(n, i)
    out, seen = [], set()
    for sc in spec['scenes']:
        # The whole scene is checked before any name is looked up, so a left-out scene can't hide a bad spec.
        if not isinstance(sc, dict) or set(sc) - KEYS or not isinstance(sc.get('room'), str):
            raise ValueError('bad scene %r' % sc)
        group = _int(sc, 'group', 0, ANY - 1, ANY)
        if (sc['room'], group) in seen:
            raise ValueError('scene %s group %s twice' % (sc['room'], sc.get('group')))
        seen.add((sc['room'], group))
        rules = [parse_entry(e) for e in sc.get('entries', [])]
        if group == ANY and any(c & 7 == ORIGINAL for c, _, _ in rules):
            raise ValueError('scene %r: an original entry needs a group' % sc['room'])
        if not isinstance(sc.get('split', False), bool):
            raise ValueError('scene %r: bad split' % sc['room'])
        band, lift = _int(sc, 'band', 1, 255, 0), _int(sc, 'lift', 0, 254, LIFT_DEFAULT)
        mode = _int(sc, 'mode', 0, 255, 0)
        try:
            room, cls = _resolve_names(sc, rules, rooms, names)
        except Unresolved as e:
            if sc['room'] == '*' and group == ANY:
                raise
            if dropped is not None:
                dropped.append(str(e))
            continue
        flags = SCENE_SPLIT if sc.get('split') else 0
        out.append((room, group, band, lift, mode, flags, sorted(cls.items())))
    every = [s[6] for s in out if s[0] == ANY and s[1] == ANY]
    kept = []
    for s in out:
        if s[1] != ANY and any(c & 7 == ORIGINAL for _, c in s[6]):
            fam = end_family(names.objects, getattr(names, 'parents', None), every[0] if every else [], dict(s[6]))
            if len(fam) > ENDS:
                if dropped is not None:
                    dropped.append(TooManyEnds('room %s group %d has %d end objects (class original and its unlisted '
                                               'children: %s), over the %d the console watches' %
                                               (names.rooms[s[0]], s[1], len(fam),
                                                ', '.join(names.objects[i] for i in fam), ENDS)))
                continue
        kept.append(s)
    kept.sort(key=lambda s: (s[0], s[1]))
    return kept


def build(spec, names, dropped=None):
    scenes = resolve(spec, names, dropped)
    body, entries = b'', b''
    first = 0
    for room, group, band, lift, mode, flags, objs in scenes:
        body += SCENE.pack(room, group, first, len(objs), band, lift, mode, flags)
        for o, c in objs:
            entries += ENTRY.pack(o, c, 0)
        first += len(objs)
    if len(scenes) > ANY or first > ANY:
        raise ValueError('too many scenes or entries')
    body += entries
    data = HEADER.pack(MAGIC, VERSION, names.size & 0xFFFFFFFF, names.game_id, names.timestamp, len(names.rooms),
                       len(names.objects), len(scenes), first, len(body), fnv1a(body), 0) + body
    if len(data) > TABLE_MAX:
        raise ValueError('the table is %d bytes, over %d' % (len(data), TABLE_MAX))
    return data


def write_table(path, spec_path, out):
    with open(spec_path, encoding='utf-8') as f:
        spec = json.load(f)
    names = Names(path)
    dropped = []
    try:
        data = build(spec, names, dropped)
    except Unresolved as e:
        print('Scene table: %s is not in %s: NO TABLE, every battle Original' % (e, path), file=sys.stderr)
        return 3
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    tmp = out + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, out)
    scenes, entries = struct.unpack_from('<2H', data, 32)
    for d in dropped:
        if isinstance(d, TooManyEnds):
            print('Scene table: WARNING: %s: that scene is left out (Original)' % d, file=sys.stderr)
        else:
            print('Scene table: %s is not in %s: that scene is left out (Original)' % (d, path), file=sys.stderr)
    print('Scene table: %s, %d scenes, %d entries, %d bytes' % (out, scenes, entries, len(data)))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', help='the game folder (default: as tools/aot/prepare.py finds it)')
    ap.add_argument('--data-win', help='the data.win itself (tests)')
    ap.add_argument('--spec', default=SPEC)
    ap.add_argument('--out', required=True)
    a = ap.parse_args(argv)
    if a.data_win is not None:
        path = a.data_win
    else:
        import prepare  # tools/aot/prepare.py: the same search as the AOT step
        import corpus
        folder = prepare.find_data('undertale', a.data)
        found = corpus.parts('undertale', folder) if folder else []
        path = found[0][1] if found else None
    if path is None or not os.path.isfile(path):
        print('Scene table: no undertale data.win found')
        return 3
    try:
        return write_table(path, a.spec, a.out)
    except (OSError, ValueError, KeyError, IndexError, struct.error) as e:
        print('Scene table: %s' % e, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
