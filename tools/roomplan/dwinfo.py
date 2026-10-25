"""What the room plan needs from a data.win, read-only: its SHA-256 and texture key, the texture page items, each page's
size and tile grid, the room names and counts. Numbers only leave this module (room names stay in memory for the
battle-room test). Follows engine/src/data_win.c and platform/3ds/source/ctr_texture_pack.c (CtrPack_layoutGrid)."""
import hashlib
import struct

TILE_MAX = 256      # CTR_TILE_MAX (platform/3ds/source/ctr_texture_pack.h)
GRID_VERSION = 1    # ROOMPLAN_GRID_VERSION: the grid rule below
PAGE_MAX_TILES = (4096 // TILE_MAX) ** 2


def tile_size_for(size):
    p = 8
    while size > p and TILE_MAX > p:
        p <<= 1
    return p


class Page:
    def __init__(self, width, height):
        self.width, self.height = width, height
        self.tile_w, self.tile_h = tile_size_for(width), tile_size_for(height)
        self.tiles_x = (width + self.tile_w - 1) // self.tile_w
        self.tiles_y = (height + self.tile_h - 1) // self.tile_h

    def tiles_in(self, x0, y0, x1, y1):
        """Tile numbers covering the page rectangle [x0, x1) x [y0, y1), row by row."""
        x0, y0 = max(0, x0), max(0, y0)
        x1, y1 = min(self.width, x1), min(self.height, y1)
        if x1 <= x0 or y1 <= y0:
            return []
        return [ty * self.tiles_x + tx for ty in range(y0 // self.tile_h, (y1 - 1) // self.tile_h + 1)
                for tx in range(x0 // self.tile_w, (x1 - 1) // self.tile_w + 1)]


def image_size(blob):
    """(width, height) of a texture page image: PNG, GameMaker QOI or BZip2+QOI."""
    if blob[:8] == b'\x89PNG\r\n\x1a\n':
        return struct.unpack_from('>II', blob, 16)
    if blob[:4] in (b'fioq', b'2zoq'):
        return struct.unpack_from('<HH', blob, 4)
    raise ValueError('unknown texture page image format %r' % blob[:4])


class DataWinInfo:
    def __init__(self, path):
        with open(path, 'rb') as f:
            d = f.read()
        self.size = len(d)
        self.sha256 = hashlib.sha256(d).hexdigest()
        if d[:4] != b'FORM':
            raise ValueError('not a data.win')
        self.chunks = {}
        o = 8
        while o + 8 <= len(d):
            name = d[o:o + 4].decode('ascii', 'replace')
            n = struct.unpack_from('<I', d, o + 4)[0]
            self.chunks[name] = (o + 8, n)
            o += 8 + n
        self._d = d
        self._gen8()
        self.room_count = len(self._plist('ROOM'))
        self.code_count = len(self._plist('CODE'))
        self.code_owner = self._code_owners()
        self.tpag = [struct.unpack_from('<10Hh', d, p) for p in self._plist('TPAG')]
        self.pages = self._pages()
        self._d = None

    def _plist(self, name):
        if name not in self.chunks or self.chunks[name][1] < 4:
            return []
        base = self.chunks[name][0]
        n = struct.unpack_from('<I', self._d, base)[0]
        return list(struct.unpack_from('<%dI' % n, self._d, base + 4))

    def _code_owners(self):
        """Per code entry, the object whose event it is, or None: GameMaker names an object's event code
        gml_Object_<object>_<event>_<number>, so the name less its last two parts. Kept as bytes, never stored."""
        d = self._d
        out = []
        for p in self._plist('CODE'):
            name = b''
            try:
                at = struct.unpack_from('<I', d, p)[0]
                name = d[at:d.index(0, at)]
            except (struct.error, ValueError):
                pass
            parts = name.rsplit(b'_', 2)
            out.append(parts[0] if name.startswith(b'gml_Object_') and len(parts) == 3 else None)
        return out

    def _gen8(self):
        d = self._d
        base, size = self.chunks['GEN8']
        wad = d[base + 1]
        if wad <= 8 and size < 108:
            raise ValueError('compact WAD8 GEN8 is not supported')
        self.game_id = struct.unpack_from('<I', d, base + 20)[0]
        if wad <= 12:
            self.timestamp = struct.unpack_from('<i', d, base + 92)[0] & 0xFFFFFFFFFFFFFFFF
        else:
            self.timestamp = struct.unpack_from('<Q', d, base + 92)[0]

    def _pages(self):
        d = self._d
        ptrs = self._plist('TXTR')
        if not ptrs:
            return []
        esize = ptrs[1] - ptrs[0] if len(ptrs) >= 2 else 8
        pages = []
        for p in ptrs:
            blob = struct.unpack_from('<I', d, p + esize - 4)[0]
            pages.append(Page(*image_size(d[blob:blob + 24])) if blob else None)
        return pages

    def item_tiles(self, tpag, rect=None):
        """(page, [tiles]) for a texture page item, or for rect = (x, y, w, h), part of it in its texels counted from its
        first one (drawSpritePart's source offsets; room tiles reach the renderer that way)."""
        if not 0 <= tpag < len(self.tpag):
            return None, []
        sx, sy, sw, sh, _tx, _ty, _tw, _th, _bw, _bh, page = self.tpag[tpag]
        if not 0 <= page < len(self.pages) or self.pages[page] is None:
            return None, []
        if rect is None:
            x0, y0, x1, y1 = sx, sy, sx + sw, sy + sh
        else:
            x, y, w, h = rect
            x0, y0 = sx + max(0, x), sy + max(0, y)
            x1, y1 = sx + min(sw, x + w), sy + min(sh, y + h)
        return page, self.pages[page].tiles_in(x0, y0, x1, y1)

    def tile_bytes(self, page):
        p = self.pages[page]
        return p.tile_w * p.tile_h * 4
