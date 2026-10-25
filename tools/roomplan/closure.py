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
"""
import struct


def _u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def _i32(d, o):
    return struct.unpack_from('<i', d, o)[0]


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
        backgrounds = [tindex.get(_u32(d, inside(p + 16, b'BGND')), -1) for p in plist(b'BGND')]
        # An object's sprite, and whether it is visible (an invisible one's sprite is its mask, never drawn by itself).
        objects = [(_i32(d, inside(p + 4, b'OBJT')), _u32(d, inside(p + 8, b'OBJT'))) for p in plist(b'OBJT')]
        self.code_sprite = self._code_sprites(d, chunks, plist, objects)
        self.plan_items, self.plan_parts = [], []
        for p in plist(b'ROOM'):
            items, parts = set(), []
            bgs, insts, tiles = (_u32(d, inside(p + off, b'ROOM')) for off in (40, 48, 52))
            for q in plist(b'ROOM', bgs):
                enabled, index = _i32(d, inside(q, b'ROOM')), _i32(d, inside(q + 8, b'ROOM'))
                if enabled and 0 <= index < len(backgrounds) and backgrounds[index] >= 0:
                    items.add(backgrounds[index])
            for q in plist(b'ROOM', insts):
                index = _i32(d, inside(q + 8, b'ROOM'))
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

    @staticmethod
    def _name(d, at):
        """The NUL-ended string at file offset at, or None. Compared here and never kept."""
        if not 0 < at < len(d):
            return None
        end = d.find(b'\0', at)
        return d[at:end] if end > at else None

    def _code_sprites(self, d, chunks, plist, objects):
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
            out.append(sprite.get(owner, -1))
        return out

    def frames(self, tpag):
        """Every frame of the sprite tpag is a frame of, or [] if it isn't a sprite's."""
        s = self.sprite_of.get(tpag)
        return self.sprite_items[s] if s is not None else []
