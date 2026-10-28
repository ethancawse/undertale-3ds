"""Write romfs/offkill.bin: UNDERTALE's bullets that the 3DS destroys once they have left the view for good.

platform/3ds/source/z_offkill.c reads it. Each listed object's PC code was read (in the offdraw
analysis): once one of its instances moves away from the view along the entry's axis and is past the entry's
reach beyond that edge of view 0, nothing in the game brings it back, reads it or counts it, so destroying it there
(without its Destroy event) changes nothing the player can see or that the game state carries (the gameplay-state rule, "Early
destroy"). The objects are named here by the first 16 hex digits of the SHA-256 of their names (as halfart's list),
matched against the player's own data.win at build time, so this file names no game identifiers; --hash NAME prints
the entry for a name.

Reach: the farthest a pixel of the instance's sprite can lie from its position (the farthest corner of the sprite's
box from its origin, at the scale the object's code keeps it at or below), plus 1 pixel, plus twice the largest view
shake along the entry's axis in the scene it is in. The build checks this against the object's sprite in the player's
data and leaves out an entry whose sprite would reach farther (or whose object it can't find).

Output: 8 bytes per entry, little-endian: int16 object index, int8 axis (0 x, 1 y), int8 direction (+1 right/down,
-1 left/up), uint8 reach in pixels, 3 zero bytes. No entries (DELTARUNE, no data): no file.

       py -3 -I tools/aot/offkill.py --game <undertale|deltarune> --romfs <build>/romfs [--data <game folder>]
       py -3 -I tools/aot/offkill.py --hash NAME
       py -3 -I tools/aot/offkill.py --report <data.win>
"""
import argparse
import hashlib
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

HASH_DIGITS = 16
MAX_ENTRIES = 8  # z_offkill.c's OK_MAX


class Entry:
    def __init__(self, key, axis, sign, reach, shake, scale, what):
        self.key, self.axis, self.sign, self.reach, self.shake, self.scale, self.what = (
            key, axis, sign, reach, shake, scale, what)


# UNDERTALE (Steam and GOG 1.08). shake: the scene's largest view shake in pixels; scale: the largest image scale its
# code sets (1 when it never scales the sprite up).
ENTRIES = (
    # The opera's cherry petals (room 179): Step moves them right and down, Outside Room destroys them; no view shake
    # in the opera. 6x6 sprite, origin (3, 3): 4.24 + 1 -> 6.
    Entry('f51f2e0a669ad60d', 0, +1, 6, 0, 1, "the opera's cherry petals"),
    # Asgore's helix flames (the battle room): vspeed set once, at least 3.8 down, never turned; Step moves x only; they
    # end below the room or with the turn. 16x16 sprite, origin (8, 8). The scene's largest vertical shake is the mercy
    # button breaking (6 px; the intro 3, a hit 2): 11.31 + 1 + 2 x 6 -> 25.
    Entry('a4afaf0605ac749e', 1, +1, 25, 6, 1, "Asgore's helix flames"),
    # The broken mercy button's 11 pieces x 2 (Asgore's battle room): Create sets gravity 1 (down) and vspeed -10 to
    # -16, the breaking button sets hspeed and friction 0.1 once; friction takes at most 0.1 a frame off vspeed, so
    # vspeed grows by at least 0.9 a frame and a piece moving down never turns back; Step only spins it; no other code
    # names the object (every other 480 in UNDERTALE's code is a number). The game keeps them to the end of the room.
    # 110x42 frames, origin (55, 21) (the _ja sprite is the same size): 58.87 + 1 + 2 x 6 px shake (the breaking
    # button's own) -> 72.
    Entry('2b28fc5313dc1fad', 1, +1, 72, 6, 1, "the broken mercy button's pieces"),
    # Photoshop Flowey's hand forks (room 294): the hand's Step sends them left or right at 16 px a frame (friction 0.2,
    # never turned), their own Step changes only size and scale (at most 1); collision with the soul is the only other
    # event. 117x72 sprite, origin (58, 36): 69.12 + 1 + 2 x 10 px shake -> 91.
    Entry('d0ddaed5e78070d4', 0, +1, 91, 10, 1, "Photoshop Flowey's hand forks (right)"),
    Entry('d0ddaed5e78070d4', 0, -1, 91, 10, 1, "Photoshop Flowey's hand forks (left)"),
)


def name_hash(name):
    return hashlib.sha256(name.encode('utf-8')).hexdigest()[:HASH_DIGITS]


class Data:
    """The OBJT and SPRT chunks of a GameMaker 1.x data.win: object names and sprites, sprite sizes and origins."""

    def __init__(self, path):
        with open(path, 'rb') as f:
            d = f.read()
        if d[:4] != b'FORM':
            raise ValueError('not a data.win: ' + path)
        chunks = {}
        o, end = 8, 8 + struct.unpack_from('<I', d, 4)[0]
        while o + 8 <= min(end, len(d)):
            n = struct.unpack_from('<I', d, o + 4)[0]
            chunks[d[o:o + 4]] = (o + 8, n)
            o += 8 + n
        for name in (b'OBJT', b'SPRT'):
            if name not in chunks:
                raise ValueError('no %s chunk' % name.decode())

        def u32(p):
            if not 0 <= p <= len(d) - 4:
                raise ValueError('pointer out of the file')
            return struct.unpack_from('<I', d, p)[0]

        def i32(p):
            return struct.unpack_from('<i', d, p)[0] if 0 <= p <= len(d) - 4 else 0

        def string(p):
            if not 0 < p < len(d):
                raise ValueError('string out of the file')
            return d[p:d.index(b'\0', p)].decode('utf-8', 'replace')

        def plist(name):
            base = chunks[name][0]
            count = u32(base)
            if count > 100000:
                raise ValueError('bad %s count' % name.decode())
            return [u32(base + 4 + 4 * i) for i in range(count)]

        # OBJT: name, sprite, ...; SPRT: name, width, height, 4 margins, 3 flags, bbox mode, separate masks, origin x, y.
        self.objects = [(string(u32(p)), i32(p + 4)) for p in plist(b'OBJT')]
        self.sprites = [(string(u32(p)), u32(p + 4), u32(p + 8), i32(p + 48), i32(p + 52)) for p in plist(b'SPRT')]

    def corner(self, sprite, scale):
        """The farthest corner of the sprite's box from its origin, at that scale, in pixels."""
        _, w, h, ox, oy = self.sprites[sprite]
        return math.hypot(max(abs(ox), abs(w - ox)) * scale, max(abs(oy), abs(h - oy)) * scale)


def resolve(data, entries=ENTRIES):
    """[(entry, object index, why not or None)]: each entry against the player's data."""
    by_hash = {}
    for i, (name, _) in enumerate(data.objects):
        by_hash.setdefault(name_hash(name), []).append(i)
    out = []
    for e in entries:
        found = by_hash.get(e.key, [])
        if len(found) != 1:
            out.append((e, -1, 'object not found' if not found else 'object name not unique'))
            continue
        index = found[0]
        sprite = data.objects[index][1]
        if not 0 <= sprite < len(data.sprites):
            out.append((e, index, 'object has no sprite'))
            continue
        need = data.corner(sprite, e.scale) + 1 + 2 * e.shake
        if e.reach < need or index > 0x7FFF:
            out.append((e, index, 'reach %d < %.2f' % (e.reach, need) if e.reach < need else 'object index too big'))
            continue
        out.append((e, index, None))
    return out


def pack(rows):
    """romfs/offkill.bin's bytes for the resolved entries that passed."""
    out = b''.join(struct.pack('<hbbB3x', index, e.axis, e.sign, e.reach) for e, index, why in rows if why is None)
    if len(out) > 8 * MAX_ENTRIES:
        raise ValueError('more than %d entries' % MAX_ENTRIES)
    return out


def build(game, data_dir):
    """(bytes, report lines) for the game; b'' means no file."""
    if game != 'undertale':
        return b'', []
    import corpus
    import prepare
    found = prepare.find_data(game, data_dir)
    parts = [p for _, p, _ in corpus.parts(game, found)] if found else []
    if not parts:
        return b'', ['offkill: no UNDERTALE data; no early destroy in this build']
    rows = resolve(Data(parts[0]))
    lines = ['offkill: %s left out (%s)' % (e.what, why) for e, _, why in rows if why is not None]
    blob = pack(rows)
    lines.append('offkill: %d of %d entries, %d B' % (len(blob) // 8, len(rows), len(blob)))
    return blob, lines


def write(romfs, blob):
    """Write (or remove, for no entries) romfs/offkill.bin; True when the file changed."""
    path = os.path.join(romfs, 'offkill.bin')
    try:
        with open(path, 'rb') as f:
            old = f.read()
    except OSError:
        old = None
    if not blob:
        if old is not None:
            os.remove(path)
        return old is not None
    if old == blob:
        return False
    os.makedirs(romfs, exist_ok=True)
    with open(path + '.tmp', 'wb') as f:
        f.write(blob)
    os.replace(path + '.tmp', path)
    return True


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', choices=('undertale', 'deltarune'))
    ap.add_argument('--romfs')
    ap.add_argument('--data')
    ap.add_argument('--hash', metavar='NAME', help="print an entry's key for an object name")
    ap.add_argument('--report', metavar='DATA_WIN', help="check each entry against a data.win and print it")
    a = ap.parse_args(argv)
    if a.hash:
        print(name_hash(a.hash))
        return 0
    if a.report:
        data = Data(a.report)
        for e, index, why in resolve(data):
            sprite = data.objects[index][1] if index >= 0 else -1
            corner = data.corner(sprite, e.scale) if 0 <= sprite < len(data.sprites) else float('nan')
            print('%-40s object %5d sprite %5d corner %6.2f need %6.2f reach %3d %s' % (
                e.what, index, sprite, corner, corner + 1 + 2 * e.shake, e.reach, why or 'ok'))
        return 0
    if not a.game or not a.romfs:
        ap.error('--game and --romfs are required')
    blob, lines = build(a.game, a.data)
    for line in lines:
        print(line)
    write(a.romfs, blob)
    return 0


if __name__ == '__main__':
    sys.exit(main())
