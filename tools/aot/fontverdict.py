"""Precompute the renderer's per-font Smart smoothing analysis from a data.win (roomload fonts, item F2(a)).

Usage: py -3 -I tools/aot/fontverdict.py <data.win> [--out <file>] [--quiet]
       py -3 -I tools/aot/fontverdict.py --game <undertale|deltarune> --romfs <dir> [--data <game folder>]  (build.sh)

The 3DS renderer looks at every font's glyphs once, at its first draw (ctr_renderer.c):
- smartFontState: whether the glyphs are doubled (2x2 blocks, SMART_COARSE = 1) or single (SMART_FINE = 2), over up to 64
  glyphs, each in its own best phase (smartTally, smartBestPhase, smartVerdict);
- smartFontClean: whether the glyphs average correctly premultiplied (1), only once the coloured transparent texels are made
  black (3), not at all (2), or are of one colour with the shapes in alpha (4) (smartRegionClean, smartRegionOneColour).
Both read the texels as the console stores them: each texture page cut into tiles of up to 256x256, each tile in the
format ctr_texture_pack.c chooses (chooseFormat with rounded colours, chooseExactFormat with exact colours on the New 3DS),
so the verdicts depend on that choice. This tool decodes the pages the fonts use, repeats the tile choice and the
conversion, and repeats the analysis, for both colour modes. On the Old 3DS the analysis costs about 65 ms at the first
battle frame, which a table read at the chapter start replaces.

One verdict here goes further than the runtime's own look (smoothux): a font the 2x2 test calls single
is written as doubled (SMART_COARSE) when, in every glyph, every run of ink and every gap between ink is at least
THICK_MIN texels long across and down (font_thick). Smoothing off then keeps every glyph's ink and gaps at any phase,
so Smart Rule v3 shows it as smoothing off does. That is UNDERTALE's battle HUD font fnt_curs (glyph pixels of 3x3
texels, drawn at 1.5 screen pixels each, which the 2x2 test missed and Smart averaged into grey) and its damage
numbers' fnt_dmg (pixels of about 4x4 texels); no DELTARUNE font. Without the table (no game data or Python at build)
the console's own look still calls them single, as before.

Pure Python (the builder ships the embeddable Python, without numpy). Reads the game data; run with -I. Writes nothing
derived from the game into the repository: --out is a build output.

Output file (little-endian): magic b"DTFV", u32 version 1, u32 data.win size, u32 GEN8 game ID, u64 GEN8 timestamp
(CtrPackKey), u32 font count, then 4 bytes per font: smart rounded, clean rounded, smart exact, clean exact. 0 means
"not known here": the runtime then looks at the glyphs itself, as before.
"""
import argparse
import bz2
import struct
import sys
import time
import zlib

MAGIC = b"DTFV"
VERSION = 1
TILE_MAX = 256
PARTIAL_ALPHA_TOLERANCE = struct.unpack("<f", struct.pack("<f", 0.02))[0]
SOFT_AREA_MIN_PIXELS = 64
SAMPLE_BLOCKS = 48
SMART_COARSE, SMART_FINE = 1, 2
THICK_MIN = 3  # font_thick: shortest run of ink, or gap between ink, that smoothing off keeps at every phase
EMPTY, RGBA8, RGBA5551, RGB565, RGB8 = "empty", "rgba8", "rgba5551", "rgb565", "rgb8"

# CtrPack_init's tables.
EXACT_BITS = [0] * 256
for _v in range(32):
    EXACT_BITS[(_v << 3) | (_v >> 2)] |= 1
for _v in range(64):
    EXACT_BITS[(_v << 2) | (_v >> 4)] |= 2
BITS5 = [(_v * 31 + 127) // 255 for _v in range(256)]


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


class Win:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.d = f.read()
        d = self.d
        if d[:4] != b"FORM":
            raise SystemExit("not a data.win: " + path)
        self.size = len(d)
        self.chunks = {}
        pos, end = 8, 8 + struct.unpack_from("<I", d, 4)[0]
        while pos + 8 <= end:
            name = d[pos:pos + 4].decode("latin-1")
            size = struct.unpack_from("<I", d, pos + 4)[0]
            self.chunks[name] = (pos + 8, size)
            pos += 8 + size
        gen8 = self.chunks["GEN8"][0]
        self.wad = d[gen8 + 1]
        # parseGEN8: debug u8, wad u8, padding u16, file name, config, last object, last tile, game ID (offset 20),
        # GUID (16 B), name, major, minor, release, build, window width, height, info, licence CRC32, licence MD5 (16 B),
        # then the timestamp at offset 92: an int32 sign-extended up to WAD 12, a u64 after.
        self.game_id = struct.unpack_from("<I", d, gen8 + 20)[0]
        if self.wad <= 12:
            self.timestamp = struct.unpack_from("<i", d, gen8 + 92)[0] & 0xFFFFFFFFFFFFFFFF
        else:
            self.timestamp = struct.unpack_from("<Q", d, gen8 + 92)[0]
        self.major = struct.unpack_from("<I", d, gen8 + 44)[0]

    def u16(self, p):
        return struct.unpack_from("<H", self.d, p)[0]

    def i16(self, p):
        return struct.unpack_from("<h", self.d, p)[0]

    def u32(self, p):
        return struct.unpack_from("<I", self.d, p)[0]

    def plist(self, name):
        base, size = self.chunks[name]
        if size == 0:
            return []
        return [self.u32(base + 4 + 4 * i) for i in range(self.u32(base))]


def tpag_item(win, p):
    # parseTexturePageItem: source x, y, w, h, target x, y, w, h, bounding w, h, texture page id (u16 each).
    return {"sx": win.u16(p), "sy": win.u16(p + 2), "sw": win.u16(p + 4), "sh": win.u16(p + 6),
            "page": win.i16(p + 20)}


def fonts(win):
    """Each font of the FONT chunk: (index, name, tpag item, glyph rects), or None for an empty slot."""
    ptrs = win.plist("FONT")
    out = []
    if not ptrs:
        return out
    # parseFONT's probe for the optional fields between scaleY and the glyph list.
    optional = 1 if win.wad >= 17 else 0
    first = next(p for p in ptrs if p)
    for trial in range(optional, 5):
        start = first + 40 + 4 * trial
        n = win.u32(start)
        if n == 0 or n > 0x10000:
            continue
        if win.u32(start + 4) == start + 4 + 4 * n:
            optional = trial
            break
    for i, p in enumerate(ptrs):
        if p == 0:
            out.append(None)
            continue
        name_ptr = win.u32(p)
        name = win.d[name_ptr:win.d.index(0, name_ptr)].decode("utf-8", "replace") if name_ptr else ""
        tpag = tpag_item(win, win.u32(p + 28)) if win.u32(p + 28) else None
        start = p + 40 + 4 * optional
        glyphs = []
        for k in range(win.u32(start)):
            g = win.u32(start + 4 + 4 * k)
            # FontGlyph: character, source x, y, w, h (u16 each).
            glyphs.append((win.u16(g + 2), win.u16(g + 4), win.u16(g + 6), win.u16(g + 8)))
        out.append((i, name, tpag, glyphs))
    return out


def texture_blobs(win):
    """TXTR blobs in order: (offset, size); blob sizes from the successive offsets (parseTXTR)."""
    base, size = win.chunks["TXTR"]
    ptrs = win.plist("TXTR")
    # The blob offset is an entry's last field (parseTXTR): 8 bytes per entry before GameMaker 2, 12 with the generated
    # mips, 16 from 2022.3 (texture block size), 28 from 2022.9 (width, height, index in group).
    entry = 12 if win.major >= 2 else 8
    if len(ptrs) >= 2 and ptrs[0] and ptrs[1] and ptrs[1] - ptrs[0] in (8, 12, 16, 28):
        entry = ptrs[1] - ptrs[0]
    offsets = []
    for p in ptrs:
        q = win.u32(p + entry - 4) if p else 0
        offsets.append(q if base <= q < base + size else 0)
    blobs = []
    for i, off in enumerate(offsets):
        if off == 0:
            blobs.append(None)
            continue
        nxt = offsets[i + 1] if i + 1 < len(offsets) and offsets[i + 1] else base + size
        blobs.append((off, nxt - off))
    return blobs


def png_decode(buf, rows_needed):
    """RGBA rows of an 8-bit RGBA or RGB PNG, the first rows_needed of them."""
    o = 8
    idat = []
    w = h = ct = 0
    while o < len(buf):
        ln = struct.unpack_from(">I", buf, o)[0]
        typ = buf[o + 4:o + 8]
        if typ == b"IHDR":
            w, h, depth, ct, _, _, interlace = struct.unpack_from(">IIBBBBB", buf, o + 8)
            if depth != 8 or ct not in (2, 6) or interlace:
                return None
        elif typ == b"IDAT":
            idat.append(buf[o + 8:o + 8 + ln])
        elif typ == b"IEND":
            break
        o += 12 + ln
    ch = 4 if ct == 6 else 3
    stride = w * ch
    rows_needed = min(rows_needed, h)
    z = zlib.decompressobj()
    raw = z.decompress(b"".join(idat), (stride + 1) * rows_needed)
    out = bytearray(w * 4 * rows_needed)
    prev = bytearray(stride)
    for y in range(rows_needed):
        p = y * (stride + 1)
        f = raw[p]
        cur = bytearray(raw[p + 1:p + 1 + stride])
        if f == 1:
            for x in range(ch, stride):
                cur[x] = (cur[x] + cur[x - ch]) & 255
        elif f == 2:
            cur = bytearray((a + b) & 255 for a, b in zip(cur, prev))
        elif f == 3:
            for x in range(stride):
                left = cur[x - ch] if x >= ch else 0
                cur[x] = (cur[x] + ((left + prev[x]) >> 1)) & 255
        elif f == 4:
            for x in range(stride):
                if x >= ch:
                    a, c = cur[x - ch], prev[x - ch]
                else:
                    a = c = 0
                b = prev[x]
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                cur[x] = (cur[x] + (a if pa <= pb and pa <= pc else (b if pb <= pc else c))) & 255
        if ch == 4:
            out[y * w * 4:(y + 1) * w * 4] = cur
        else:
            row = bytearray(w * 4)
            row[0::4], row[1::4], row[2::4] = cur[0::3], cur[1::3], cur[2::3]
            row[3::4] = b"\xff" * w
            out[y * w * 4:(y + 1) * w * 4] = row
        prev = cur
    return w, h, out


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def qoi_decode(buf, rows_needed):
    """GameMaker's QOI ('fioq'): RGBA rows, the first rows_needed of them."""
    if buf[:4] != b"fioq":
        return None
    w, h, length = struct.unpack_from("<HHI", buf, 4)
    px = buf[12:12 + length]
    rows_needed = min(rows_needed, h)
    total = w * rows_needed * 4
    out = bytearray(total)
    index = [(0, 0, 0, 0)] * 64
    r = g = b = 0
    a = 255
    pos = run = i = 0
    n = len(px)
    while i < total:
        if run > 0:
            run -= 1
        elif pos < n:
            b1 = px[pos]
            pos += 1
            if b1 & 0xC0 == 0x00:
                r, g, b, a = index[b1]
            elif b1 & 0xE0 == 0x40:
                run = b1 & 0x1F
            elif b1 & 0xE0 == 0x60:
                run = (((b1 & 0x1F) << 8) | px[pos]) + 32
                pos += 1
            elif b1 & 0xC0 == 0x80:
                r = (r + sext((b1 >> 4) & 3, 2)) & 255
                g = (g + sext((b1 >> 2) & 3, 2)) & 255
                b = (b + sext(b1 & 3, 2)) & 255
            elif b1 & 0xE0 == 0xC0:
                m = (b1 << 8) | px[pos]
                pos += 1
                r = (r + sext((m >> 8) & 31, 5)) & 255
                g = (g + sext((m >> 4) & 15, 4)) & 255
                b = (b + sext(m & 15, 4)) & 255
            elif b1 & 0xF0 == 0xE0:
                m = (b1 << 16) | (px[pos] << 8) | px[pos + 1]
                pos += 2
                r = (r + sext((m >> 15) & 31, 5)) & 255
                g = (g + sext((m >> 10) & 31, 5)) & 255
                b = (b + sext((m >> 5) & 31, 5)) & 255
                a = (a + sext(m & 31, 5)) & 255
            else:
                if b1 & 8:
                    r = px[pos]
                    pos += 1
                if b1 & 4:
                    g = px[pos]
                    pos += 1
                if b1 & 2:
                    b = px[pos]
                    pos += 1
                if b1 & 1:
                    a = px[pos]
                    pos += 1
            index[(r ^ g ^ b ^ a) & 63] = (r, g, b, a)
        out[i] = r
        out[i + 1] = g
        out[i + 2] = b
        out[i + 3] = a
        i += 4
    return w, h, out


def decode_page(blob, rows_needed):
    if blob[:4] == b"\x89PNG":
        return png_decode(blob, rows_needed)
    if blob[:4] == b"fioq":
        return qoi_decode(blob, rows_needed)
    if blob[:4] == b"2zoq":
        # bz2HeaderSize: 12 bytes since GameMaker 2022.5 (the inflated length), 8 before; the "BZh" signature decides.
        for at in (12, 8):
            if blob[at:at + 3] == b"BZh":
                inner = bz2.decompress(blob[at:])
                return qoi_decode(inner, rows_needed)
    return None


def tile_size(size):
    p = 8
    while size > p and TILE_MAX > p:
        p <<= 1
    return p


class Page:
    def __init__(self, w, h, rgba, rows):
        self.w, self.h, self.rgba, self.rows = w, h, rgba, rows
        self.tw, self.th = tile_size(w), tile_size(h)
        self.tiles_x = (w + self.tw - 1) // self.tw
        self.formats = {}  # (exact, tx, ty) -> format

    def px(self, x, y):
        o = (y * self.w + x) * 4
        return self.rgba[o], self.rgba[o + 1], self.rgba[o + 2], self.rgba[o + 3]

    def format(self, exact, tx, ty):
        key = (exact, tx, ty)
        f = self.formats.get(key)
        if f is None:
            x0, y0 = tx * self.tw, ty * self.th
            w = min(self.tw, self.w - x0)
            h = min(self.th, self.h - y0)
            if y0 + h > self.rows:
                raise ValueError("page rows not decoded")
            f = self.choose_exact(x0, y0, w, h, w == self.tw and h == self.th) if exact else self.choose(x0, y0, w, h)
            self.formats[key] = f
        return f

    def choose_exact(self, x0, y0, w, h, full):
        # chooseExactFormat.
        visible, opaque, binary = False, full, True
        r5 = g5 = b5 = 3
        rgba, pw = self.rgba, self.w
        for y in range(y0, y0 + h):
            o = (y * pw + x0) * 4
            for k in range(o, o + 4 * w, 4):
                a = rgba[k + 3]
                if a:
                    visible = True
                if a != 255:
                    opaque = False
                    if a:
                        binary = False
                r5 &= EXACT_BITS[rgba[k]]
                g5 &= EXACT_BITS[rgba[k + 1]]
                b5 &= EXACT_BITS[rgba[k + 2]]
            if visible and not opaque and not (binary and (r5 & g5 & b5 & 1)):
                return RGBA8
        if not visible:
            return EMPTY
        if binary and (r5 & g5 & b5 & 1):
            return RGBA5551
        if opaque:
            return RGB565 if (r5 & b5 & 1) and (g5 & 2) else RGB8
        return RGBA8

    def choose(self, x0, y0, w, h):
        # chooseFormat: the share of partly transparent texels and the soft area (partly transparent texels whose four
        # neighbours inside the tile are too).
        visible = partial = soft = 0
        rgba, pw = self.rgba, self.w
        for y in range(h):
            o = ((y0 + y) * pw + x0) * 4
            alphas = rgba[o + 3:o + 4 * w:4]
            for x, a in enumerate(alphas):
                if a == 0:
                    continue
                visible += 1
                if a == 255:
                    continue
                partial += 1
                if 0 < y < h - 1 and 0 < x < w - 1:
                    def pa(xx, yy):
                        v = rgba[((y0 + yy) * pw + x0 + xx) * 4 + 3]
                        return 0 < v < 255
                    if pa(x - 1, y) and pa(x + 1, y) and pa(x, y - 1) and pa(x, y + 1):
                        soft += 1
        if visible == 0:
            return EMPTY
        if f32(float(partial)) <= f32(f32(float(visible)) * PARTIAL_ALPHA_TOLERANCE) and SOFT_AREA_MIN_PIXELS > soft:
            return RGBA5551
        return RGBA8

    def stored(self, exact, x, y):
        """(format, value as the tile stores the texel): the bit patterns smartTexel and friends read."""
        f = self.format(exact, x // self.tw, y // self.th)
        if f == EMPTY:
            return f, 0
        r, g, b, a = self.px(x, y)
        if f == RGBA8:
            return f, (r << 24) | (g << 16) | (b << 8) | a
        if f == RGBA5551:
            return f, (BITS5[r] << 11) | (BITS5[g] << 6) | (BITS5[b] << 1) | (a >> 7)
        if f == RGB565:
            return f, ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        return f, b | (g << 8) | (r << 16)  # RGB8: blue, green, red in memory

    def texel(self, exact, x, y):
        # smartTexel.
        f, v = self.stored(exact, x, y)
        if f == RGBA8:
            return v if v & 0xFF else 0
        if f == RGBA5551:
            return v if v & 1 else 0
        if f == RGB565:
            return 0x10000 | v
        if f == RGB8:
            return 0x1000000 | v
        return 0


def tally(page, exact, x0, y0, w, h):
    # smartTally.
    vis, uni = [0] * 4, [0] * 4
    if 2 > w or 2 > h or x0 + w > page.w or y0 + h > page.h:
        return vis, uni
    bw, bh = (w + 1) // 2, (h + 1) // 2
    sx = (bw + SAMPLE_BLOCKS - 1) // SAMPLE_BLOCKS
    sy = (bh + SAMPLE_BLOCKS - 1) // SAMPLE_BLOCKS
    for by in range(0, bh, sy):
        for bx in range(0, bw, sx):
            n = [page.texel(exact, x0 + min(2 * bx + i, w - 1), y0 + min(2 * by + j, h - 1)) for j in range(3) for i in range(3)]
            for p in range(4):
                o = (p >> 1) * 3 + (p & 1)
                q0, q1, q3, q4 = n[o], n[o + 1], n[o + 3], n[o + 4]
                if (q0 | q1 | q3 | q4) == 0:
                    continue
                vis[p] += 1
                if q0 == q1 == q3 == q4:
                    uni[p] += 1
    return vis, uni


def best_phase(vis, uni):
    best = 0
    for p in range(1, 4):
        if uni[p] * (vis[best] if vis[best] > 0 else 1) > uni[best] * (vis[p] if vis[p] > 0 else 1):
            best = p
    return best


def region_clean(page, exact, x0, y0, w, h):
    # smartRegionClean.
    if x0 + w > page.w or y0 + h > page.h:
        return 2
    clean = 1
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            f, v = page.stored(exact, x, y)
            if f == RGBA8:
                if (v & 0xFF) == 0xFF or v == 0:
                    continue
                if v & 0xFF:
                    return 2
                clean = 3
            elif f == RGBA5551:
                if (v & 1) == 0 and v != 0:
                    clean = 3
    return clean


def region_one_colour(page, exact, x0, y0, w, h, colour):
    # smartRegionOneColour: (one colour so far, colour).
    if x0 + w > page.w or y0 + h > page.h:
        return False, colour
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            f, v = page.stored(exact, x, y)
            if f == EMPTY:
                continue
            if f == RGBA8:
                rgb = v >> 8
            elif f == RGBA5551:
                rgb = 0x1000000 | (v >> 1)
            else:
                return False, colour
            if colour == 0xFFFFFFFF:
                colour = rgb
            elif colour != rgb:
                return False, colour
    return True, colour


def font_smart(page, exact, tpag, glyphs):
    # smartFontState for a font of the data (not a sprite font).
    uniform = visible = 0
    step = len(glyphs) // 64 if len(glyphs) > 64 else 1
    for g in range(0, len(glyphs), step):
        gx, gy, gw, gh = glyphs[g]
        if gw == 0 or gh == 0:
            continue
        vis, uni = tally(page, exact, tpag["sx"] + gx, tpag["sy"] + gy, gw, gh)
        p = best_phase(vis, uni)
        uniform += uni[p]
        visible += vis[p]
    return SMART_COARSE if visible > 0 and uniform * 100 >= visible * 97 else SMART_FINE, uniform, visible


def font_thick(page, exact, tpag, glyphs):
    """True when every glyph's runs of ink, and gaps between ink, are at least THICK_MIN texels across and down. Then the
    shrink's 2:1 pick keeps a texel of each run and each gap at every phase (any THICK_MIN >= 2 consecutive texels hold
    a picked one), so no glyph loses ink or merges strokes: Rule v3 leaves such a font as smoothing off shows it."""
    any_ink = False
    for gx, gy, gw, gh in glyphs:
        if gw == 0 or gh == 0:
            continue
        x0, y0 = tpag["sx"] + gx, tpag["sy"] + gy
        if x0 + gw > page.w or y0 + gh > page.h:
            return False
        ink = [[page.texel(exact, x0 + x, y0 + y) != 0 for x in range(gw)] for y in range(gh)]
        for line in ink + [[ink[y][x] for y in range(gh)] for x in range(gw)]:
            runs, cur, n = [], None, 0
            for v in line:
                if v == cur:
                    n += 1
                else:
                    if cur is not None:
                        runs.append((cur, n))
                    cur, n = v, 1
            runs.append((cur, n))
            for k, (v, n) in enumerate(runs):
                # Ink runs anywhere; gaps only between ink (the glyph box's margins don't count).
                if (v or 0 < k < len(runs) - 1) and THICK_MIN > n:
                    return False
                any_ink = any_ink or v
    return any_ink


def font_clean(page, exact, tpag, glyphs):
    # smartFontClean for a font of the data.
    clean = 1
    for gx, gy, gw, gh in glyphs:
        if clean != 1:
            break
        if gw == 0 or gh == 0:
            continue
        clean = region_clean(page, exact, tpag["sx"] + gx, tpag["sy"] + gy, gw, gh)
    if clean != 1:
        colour, one = 0xFFFFFFFF, True
        for gx, gy, gw, gh in glyphs:
            if not one:
                break
            if gw == 0 or gh == 0:
                continue
            one, colour = region_one_colour(page, exact, tpag["sx"] + gx, tpag["sy"] + gy, gw, gh, colour)
        if one and colour != 0xFFFFFFFF:
            clean = 4
    return clean


def analyse(path, quiet=False, log=print):
    win = Win(path)
    blobs = texture_blobs(win)
    table = []
    by_page = {}
    for entry in fonts(win):
        if entry is None or entry[2] is None or entry[2]["page"] < 0 or entry[2]["page"] >= len(blobs) or blobs[entry[2]["page"]] is None:
            table.append(None)
            continue
        table.append(entry)
        by_page.setdefault(entry[2]["page"], []).append(entry)
    verdicts = [(0, 0, 0, 0)] * len(table)
    for page_id, entries in sorted(by_page.items()):
        off, size = blobs[page_id]
        blob = win.d[off:off + size]
        # Whole tile rows down to the lowest glyph.
        bottom = max(e[2]["sy"] + max((g[1] + g[3] for g in e[3]), default=0) for e in entries)
        t0 = time.time()
        head = decode_page(blob, 1)
        if head is None:
            continue
        th = tile_size(head[1])
        rows = min(head[1], (bottom + th - 1) // th * th)
        dec = decode_page(blob, rows)
        page = Page(dec[0], dec[1], dec[2], rows)
        for i, name, tpag, glyphs in entries:
            rounded, exact = font_smart(page, False, tpag, glyphs), font_smart(page, True, tpag, glyphs)
            v = (rounded[0], font_clean(page, False, tpag, glyphs), exact[0], font_clean(page, True, tpag, glyphs))
            # Thick single glyphs are written as doubled (see the top); the log keeps the runtime's own verdict first.
            thick = (rounded[0] == SMART_FINE and font_thick(page, False, tpag, glyphs),
                     exact[0] == SMART_FINE and font_thick(page, True, tpag, glyphs))
            verdicts[i] = (SMART_COARSE if thick[0] else v[0], v[1], SMART_COARSE if thick[1] else v[2], v[3])
            if not quiet:
                # The runtime's "Renderer: font N has ... glyphs (U of V blocks one colour)" line gives the same counts.
                log(f"font {i} {name}: page {page_id}, {len(glyphs)} glyphs; rounded smart {v[0]} ({rounded[1]} of "
                    f"{rounded[2]} blocks) clean {v[1]}; exact smart {v[2]} ({exact[1]} of {exact[2]} blocks) clean {v[3]}"
                    + (f"; ink and gaps of {THICK_MIN}+ texels, written as smart {SMART_COARSE}"
                       f" ({'rounded' if thick[0] else ''}{' and ' if all(thick) else ''}{'exact' if thick[1] else ''})"
                       if any(thick) else ""))
        if not quiet:
            log(f"page {page_id}: {dec[0]}x{dec[1]}, {rows} rows decoded in {time.time() - t0:.1f} s")
    return win, verdicts


def write(path, win, verdicts):
    out = bytearray(MAGIC + struct.pack("<IIIQI", VERSION, win.size, win.game_id, win.timestamp, len(verdicts)))
    for v in verdicts:
        out += bytes(v)
    with open(path, "wb") as f:
        f.write(out)


def build_tables(game, data, romfs, target=None):
    """build.sh: one table per data.win of the game into <romfs>/fonts/<data.win folder>.dtfv, the name the runtime
    looks up (undertale, deltarune for the chapter select, chapterN_windows). Skipped when the inputs are unchanged.
    UNDERTALE, and DELTARUNE for New 3DS (target n3ds), also get symmetric halving's plain items
    (tools/aot/symverdict.py) beside each as <name>.dtsy."""
    import hashlib
    import os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script's folder off the path
    import corpus
    import prepare
    found = prepare.find_data(game, data)
    if not found:
        print(f"fonts: no {game.upper()} game data found; the renderer analyses its fonts at their first draw")
        return 0
    wins = [("undertale" if game == "undertale" else key, path) for key, path, _ in corpus.parts(game, found)]
    if game == "deltarune" and os.path.isfile(os.path.join(found, "data.win")):
        wins.insert(0, ("deltarune", os.path.join(found, "data.win")))
    out_dir = os.path.join(romfs, "fonts")
    os.makedirs(out_dir, exist_ok=True)
    with open(__file__, "rb") as f:
        stamp = ["generator " + hashlib.sha1(f.read()).hexdigest()]
    # Symmetric halving's table: UNDERTALE, and DELTARUNE's New 3DS builds (its Old builds compile z_symverdict.c empty,
    # so their tables and stamp stay as they were).
    sym = game == "undertale" or target == "n3ds"
    exts = (".dtfv", ".dtsy") if sym else (".dtfv",)
    if sym:
        import symverdict
        with open(symverdict.__file__, "rb") as f:
            stamp.append("generator " + hashlib.sha1(f.read()).hexdigest())
    for name, path in wins:
        st = os.stat(path)
        stamp.append(f"{name} {st.st_size} {st.st_mtime_ns}")
    # Beside the RomFS folder, so that it isn't packaged.
    stamp_path = os.path.join(os.path.dirname(os.path.abspath(romfs)), "fonts-inputs.txt")
    names = {name + ext for name, _ in wins for ext in exts}
    try:
        with open(stamp_path, encoding="utf-8") as f:
            same = f.read() == "\n".join(stamp) + "\n" and all(os.path.isfile(os.path.join(out_dir, n)) for n in names)
    except OSError:
        same = False
    if same:
        print(f"fonts: {len(wins)} tables up to date in {out_dir}")
        return 0
    for old in os.listdir(out_dir):
        if old.endswith((".dtfv", ".dtsy")) and old not in names:
            os.remove(os.path.join(out_dir, old))
    t0 = time.time()
    for name, path in wins:
        win, verdicts = analyse(path, quiet=True)
        write(os.path.join(out_dir, name + ".dtfv"), win, verdicts)
        if sym:
            win, bits = symverdict.analyse(path, game=game)
            symverdict.write(os.path.join(out_dir, name + ".dtsy"), win, bits)
    with open(stamp_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(stamp) + "\n")
    print(f"fonts: {len(wins)} tables written to {out_dir} in {time.time() - t0:.1f} s")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("data_win", nargs="?")
    ap.add_argument("--out")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--game", choices=("undertale", "deltarune"), help="build mode: every data.win of the game")
    ap.add_argument("--data", help="build mode: the game folder (else as tools/aot/prepare.py finds it)")
    ap.add_argument("--romfs", help="build mode: the RomFS folder to write fonts/<name>.dtfv into")
    ap.add_argument("--target", choices=("o3ds", "n3ds"), help="build mode: the build's target (DELTARUNE n3ds: .dtsy too)")
    args = ap.parse_args()
    if args.game:
        if not args.romfs:
            ap.error("--game needs --romfs")
        return build_tables(args.game, args.data, args.romfs, args.target)
    if not args.data_win:
        ap.error("give a data.win, or --game and --romfs")
    t0 = time.time()
    win, verdicts = analyse(args.data_win, args.quiet)
    if args.out:
        write(args.out, win, verdicts)
    known = sum(1 for v in verdicts if v[0])
    print(f"fontverdict: {known} of {len(verdicts)} fonts analysed in {time.time() - t0:.1f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
