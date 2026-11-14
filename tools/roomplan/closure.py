"""What a room can draw beyond what its record saw, for the room plan's whole set (UNDERTALE: GameMaker 1.x data.win).

Read-only. Numbers only leave this module. Follows engine/src/data_win.c (parseSPRT, parseBGND, parseOBJT, parseROOM)
for the GameMaker 1.x layouts UNDERTALE uses; any other layout (a special sprite type, a chunk missing, a pointer out
of range) gives no closure at all, and the plan is built as before.

For a scene the closure adds:
- every frame of each sprite the record drew from (a recording sees the frames its run happened to show: other
  expressions of a face, other frames of an animation);
- the room's own content: its enabled backgrounds, its tiles, and every frame of the sprite of each visible object
  placed in it;
- for a battle group, every frame of the sprite of each object whose code the group ran, visible or not
  (code_sprite: UNDERTALE's monster objects are invisible, and the sprite they name is the one drawn when they are
  hit; build_plan.whole_set).
- for an ordinary room, the sprite of each object whose Create event first ran in a shown frame
  (code_create_sprite): these instances did not exist when the room's placed objects were marked.
- for any whole set containing a tile of the player's normal walk family, every direction in that family.
  The family is resolved from the player's default sprite and its four named siblings in this file; names and
  store-specific asset numbers never enter the plan. Unvisited player rooms get the same family.
- for a battle group, the other sprites of each numbered series it draws from (series_of): UNDERTALE's scripts pick a
  pose from a numbered run of sprites (Mettaton EX's arm poses are eight sprites that differ in their last digit), and
  a recording of one run saw some of the numbers. A series is the sprites whose names are the same but for the digits
  they end in, two at least (change utmett: the pose a run first drew late in the fight was one the record never saw, a
  14 KB tile read on the game thread in the middle of an attack).
- for an ordinary room, the art its own objects can draw that the record did not see (placed art rule, a partial
  preload: codeclose.CodeClosure.placed_art_sprites, build_plan.placed_art): the lab's Alphys, her portraits and
  Mettaton in the quiz, the floor's Napstablook on the headphones. Every room but build_plan.PLACED_ART_SKIP_ROOMS.
- for every battle group, the spare puffs (spare_items; build_plan.spare_art): any fight can end in a spare, and no
  group's record saw them (the playtest's spared first fight, 11-07: a 21 ms load on the game thread at the spare).
- the speech bubbles' font (bubble_font_items; build_plan.bubble_font), for the battle groups build_plan names.
- sprite_hash: each sprite's number by the FNV-1a hash of its name (build_plan.fnv1a), for build_plan.py's menu
  levels, which name their sprites by those hashes only; a hash two names share maps to None.
"""
import hashlib
import struct

# The spare puffs' sprite, found by the SHA-256 of its name so that no name is written here.
SPARE_SPRITE_SHA256 = 'be9f34130038570df413ea8f87723e97894f039bb422c5f8096364336e9cee39'
# The battle speech bubbles' font, the same way.
BUBBLE_FONT_SHA256 = 'bf6d0788d535ca1655b2a90fb0ae90b79173eda74c64b8d44ebb62ab12c155d2'


def _u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def _i32(d, o):
    return struct.unpack_from('<i', d, o)[0]


def _fnv1a(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


class Closure:
    def __init__(self, path):
        self.ok = False
        with open(path, 'rb') as f:
            d = f.read()
        try:
            self._parse(d)
            self.ok = True
        except (ValueError, struct.error, IndexError, KeyError):
            self.sprite_of, self.sprite_items, self.plan_items, self.plan_parts = {}, [], [], []
            self.code_sprite = []
            self.code_create_sprite = []
            self.sprite_series = []
            self.walk_objects = []
            self.walk_items = []
            self.spare_items = []
            self.bubble_font_items = []
            self.sprite_hash = {}

    def _parse(self, d):
        chunks = {}
        o = 8
        while o + 8 <= len(d):
            n = _u32(d, o + 4)
            chunks[d[o:o + 4]] = (o + 8, n)
            o += 8 + n
        for name in (b'SPRT', b'BGND', b'OBJT', b'ROOM', b'TPAG'):
            if name not in chunks:
                raise ValueError('no %s chunk' % name.decode())

        def inside(p, name, size=4):
            base, n = chunks[name]
            if not base <= p <= base + n - size:
                raise ValueError('pointer out of %s' % name.decode())
            return p

        def plist(name, at=None):
            base = chunks[name][0] if at is None else at
            if base == 0:
                return []
            c = _u32(d, inside(base, name))
            if c > 100000:
                raise ValueError('count')
            return [_u32(d, inside(base + 4 + 4 * i, name)) for i in range(c)]

        tindex = {p: i for i, p in enumerate(plist(b'TPAG'))}
        self.sprite_items = []
        for p in plist(b'SPRT'):
            at = inside(p + 14 * 4, b'SPRT')
            count = _i32(d, at)
            if count < 0 or count > 4096:
                raise ValueError('not a GameMaker 1.x sprite')
            items = []
            for k in range(count):
                t = tindex.get(_u32(d, inside(at + 4 + 4 * k, b'SPRT')))
                if t is None:
                    raise ValueError('sprite frame is not a texture page item')
                items.append(t)
            self.sprite_items.append(items)
        self.sprite_of = {}
        for s, items in enumerate(self.sprite_items):
            for t in items:
                self.sprite_of.setdefault(t, s)
        # Numbered series: the sprites whose names are the same but for the digits they end in.
        stems = {}
        for s, p in enumerate(plist(b'SPRT')):
            name = self._name(d, _u32(d, inside(p, b'SPRT')))
            stem = name.rstrip(b'0123456789') if name is not None else b''
            if name is not None and len(stem) > 4 and stem != name:
                stems.setdefault(stem, []).append(s)
        self.sprite_series = [[] for _ in self.sprite_items]
        for members in stems.values():
            if len(members) > 1:
                for s in members:
                    self.sprite_series[s] = [m for m in members if m != s]
        backgrounds = [tindex.get(_u32(d, inside(p + 16, b'BGND')), -1) for p in plist(b'BGND')]
        # An object's sprite, and whether it is visible (an invisible one's sprite is its mask, never drawn by itself).
        objects = [(_i32(d, inside(p + 4, b'OBJT')), _u32(d, inside(p + 8, b'OBJT'))) for p in plist(b'OBJT')]
        players = {i for i, p in enumerate(plist(b'OBJT'))
                   if self._name(d, _u32(d, inside(p, b'OBJT'))) == b'obj_mainchara'}
        names = [self._name(d, _u32(d, inside(p, b'SPRT'))) for p in plist(b'SPRT')]
        named = {name: s for s, name in enumerate(names) if name is not None}
        self.spare_items = sorted({item for s, name in enumerate(names)
                                   if name is not None and hashlib.sha256(name).hexdigest() == SPARE_SPRITE_SHA256
                                   for item in self.sprite_items[s]})
        # A font's texture page item is the word at 0x1C of its entry (GameMaker 1.x FONT).
        self.bubble_font_items = []
        for p in plist(b'FONT') if b'FONT' in chunks else []:
            name = self._name(d, _u32(d, inside(p, b'FONT')))
            item = tindex.get(_u32(d, inside(p + 0x1C, b'FONT')))
            if name is not None and item is not None and hashlib.sha256(name).hexdigest() == BUBBLE_FONT_SHA256:
                self.bubble_font_items.append(item)
        self.sprite_hash = {}
        for s, name in enumerate(names):
            if name is not None:
                h = _fnv1a(name)
                self.sprite_hash[h] = None if h in self.sprite_hash else s
        self.walk_items = []
        for obj in sorted(players):
            sprite = objects[obj][0]
            name = names[sprite] if 0 <= sprite < len(names) else None
            if name and name[-1:] in (b'd', b'u', b'r', b'l'):
                family = [named.get(name[:-1] + direction) for direction in (b'd', b'u', b'r', b'l')]
                if all(s is not None for s in family):
                    self.walk_items = sorted({item for s in family for item in self.sprite_items[s]})
        self.code_sprite = self._code_sprites(d, chunks, plist, objects)
        self.code_create_sprite = self._code_sprites(d, chunks, plist, objects, create_only=True)
        self.plan_items, self.plan_parts, self.walk_objects = [], [], []
        for p in plist(b'ROOM'):
            items, parts, walkers = set(), [], set()
            bgs, insts, tiles = (_u32(d, inside(p + off, b'ROOM')) for off in (40, 48, 52))
            for q in plist(b'ROOM', bgs):
                enabled, index = _i32(d, inside(q, b'ROOM')), _i32(d, inside(q + 8, b'ROOM'))
                if enabled and 0 <= index < len(backgrounds) and backgrounds[index] >= 0:
                    items.add(backgrounds[index])
            for q in plist(b'ROOM', insts):
                index = _i32(d, inside(q + 8, b'ROOM'))
                if index in players:
                    walkers.add(index)
                if 0 <= index < len(objects):
                    sprite, visible = objects[index]
                    if visible and 0 <= sprite < len(self.sprite_items):
                        items.update(self.sprite_items[sprite])
            for q in plist(b'ROOM', tiles):
                _x, _y, index, sx, sy, w, h = struct.unpack_from('<7i', d, inside(q, b'ROOM', 28))
                if 0 <= index < len(backgrounds) and backgrounds[index] >= 0:
                    parts.append((backgrounds[index], sx, sy, w, h))
            self.plan_items.append(sorted(items))
            self.plan_parts.append(sorted(set(parts)))
            self.walk_objects.append(sorted(walkers))

    def walk_frames(self, room):
        """Normal walk family for an unrecorded room with a placed player, or none."""
        return self.walk_items if 0 <= room < len(self.walk_objects) and self.walk_objects[room] else []

    def walk_tiles(self, info, tiles):
        """Close a whole set containing any walk-family tile, including aliases and battle subscenes."""
        family = {(p, t) for item in self.walk_items
                  for p, ts in [info.item_tiles(item)] if p is not None for t in ts}
        return family if family.intersection(tiles) else set()

    @staticmethod
    def _name(d, at):
        """The NUL-ended string at file offset at, or None. Compared here and never kept."""
        if not 0 < at < len(d):
            return None
        end = d.find(b'\0', at)
        return d[at:end] if end > at else None

    def _code_sprites(self, d, chunks, plist, objects, create_only=False):
        """Per code entry, the sprite of the object whose event it is (GameMaker names an object's event code
        gml_Object_<object>_<event>_<number>, as dwinfo.code_owner reads it), or -1. Empty without a CODE chunk."""
        if b'CODE' not in chunks:
            return []
        sprite = {}
        for p, (s, _visible) in zip(plist(b'OBJT'), objects):
            name = self._name(d, _u32(d, p))
            if name is not None and 0 <= s < len(self.sprite_items):
                sprite.setdefault(name, s)
        out = []
        for p in plist(b'CODE'):
            name = self._name(d, _u32(d, p)) or b''
            parts = name.rsplit(b'_', 2)
            owner = parts[0][len(b'gml_Object_'):] if name.startswith(b'gml_Object_') and len(parts) == 3 else None
            if create_only and parts[-2:] != [b'Create', b'0']:
                owner = None
            out.append(sprite.get(owner, -1))
        return out

    def series_of(self, tpag):
        """The other sprites of the numbered series tpag's sprite belongs to, or [] (none, or not a sprite's)."""
        s = self.sprite_of.get(tpag)
        return self.sprite_series[s] if s is not None and s < len(self.sprite_series) else []

    def frames(self, tpag):
        """Every frame of the sprite tpag is a frame of, or [] if it isn't a sprite's."""
        s = self.sprite_of.get(tpag)
        return self.sprite_items[s] if s is not None else []
