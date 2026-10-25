"""Bakes the loading screen's art for the 3DS build from the player's own game files (platform/3ds/build.sh).

usage: loadscreen_art.py --out FILE [--data DIR] [--game deltarune|undertale]

The game (default DELTARUNE) folder is --data, else $CTRPORT_DELTARUNE_DATA or $CTRPORT_UNDERTALE_DATA, else the game
in a Steam library (as tools/aot/prepare.py finds it). The folder is only read. FILE goes into the build's RomFS as
romfs:/loadscreen.bin and is never committed: it holds pixels from the player's files (loadscreen).

Both games:
- The first time setup's line (packscreen): SETUP_LINE, our own words, drawn with the game's
  own font (UNDERTALE's fnt_maintext, DELTARUNE's fnt_main) at 1:1 as a 1-bit bitmap, the message for target 255. The
  3DS shows it under the loading screen's bar while it prepares the texture pages (z_loadscreen.c's end). UNDERTALE's
  file holds only this message. Without the font's glyphs for it there is no such message, and the bar shows alone.

DELTARUNE:

- The messages DELTARUNE's own obj_screen_loading shows (the chapter select, and each chapter), one 1-bit bitmap each,
  drawn with the launcher's fnt_mainbig at 1:1 the way draw_text centres them. The strings come from that object's
  code, found by their place in it, so no game text is written here.
- The dog: Chapter 3's spr_dog_sway frames at their own size, colours as drawn over black, and the sprite's speed,
  only when chapter3_windows/data.win is there and allow-listed.
- Its rope (dogcheck): the object that shows the dog hangs it from a line, and the facts for
  drawing that come from the object's own code: the sprite's origin, the line's colour and width (Draw's
  draw_set_color and draw_line_width), its length (Step's lengthdir_x) and the swing's speed in degrees a frame
  (Create's anglechange). They go in a 16-byte trailer ("DSW1") after the frames, and the header's frame time becomes
  the drawing tick (33 ms) so that the loading screen redraws the swinging dog every tick; the trailer keeps the
  sprite's own frame time. Without all of them there is no trailer and the dog stands as before.
Every data.win used must be in platform/3ds/data_versions.inc (size and SHA-256): without a known launcher there is no
file, and the 3DS draws the bar alone. Nothing is regenerated while the inputs are the ones FILE was made from.

Exit status 0 whether or not there is art (a line says which), 2 on bad arguments.
"""
import bz2
import json
import mmap
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import data_versions  # noqa: E402
import gmlaot as G  # noqa: E402

FORMAT_VERSION = 1
FONT_NAME = "fnt_mainbig"
SETUP_LINE = "First time setup. Please wait."
SETUP_TARGET = 255
SETUP_FONT = {"deltarune": "fnt_main", "undertale": "fnt_maintext"}
DOG_SPRITE = "spr_dog_sway"
MAX_CHAPTER = 9
MAX_COLOURS = 15


class Wad:
    """The chunks of a data.win, read in place."""

    def __init__(self, path):
        self.file = open(path, "rb")
        self.d = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
        if self.d[0:4] != b"FORM":
            raise ValueError("not a data.win: %s" % path)
        self.chunks = {}
        pos, end = 8, 8 + self.u32(4)
        while pos + 8 <= end:
            self.chunks[self.d[pos:pos + 4].decode("latin-1")] = (pos + 8, self.u32(pos + 4))
            pos += 8 + self.u32(pos + 4)

    def close(self):
        self.d.close()
        self.file.close()

    def u16(self, p):
        return struct.unpack_from("<H", self.d, p)[0]

    def i16(self, p):
        return struct.unpack_from("<h", self.d, p)[0]

    def u32(self, p):
        return struct.unpack_from("<I", self.d, p)[0]

    def i32(self, p):
        return struct.unpack_from("<i", self.d, p)[0]

    def cstr(self, p):
        return self.d[p:self.d.find(b"\0", p)].decode("utf-8", "replace")

    def table(self, name):
        start, _ = self.chunks[name]
        return [self.u32(start + 4 + 4 * i) for i in range(self.u32(start))]

    def tpag(self, p):
        v = struct.unpack_from("<10Hh", self.d, p)
        return dict(sx=v[0], sy=v[1], sw=v[2], sh=v[3], tx=v[4], ty=v[5], tw=v[6], th=v[7], bw=v[8], bh=v[9], page=v[10])

    def page_rows(self, page, rows):
        """The texture page as RGBA rows, decoded only as far as row `rows`."""
        tbase, tsize = self.chunks["TXTR"]
        entry = self.table("TXTR")[page]
        for k in range(0, 40, 4):
            p = self.u32(entry + k)
            if tbase <= p < tbase + tsize and self.d[p:p + 4] in (b"2zoq", b"fioq", b"\x89PNG"):
                return decode_page(self.d, p, rows)
        raise ValueError("texture page %d not found" % page)


def decode_page(d, p, rows):
    magic = d[p:p + 4]
    if magic == b"\x89PNG":
        return png_rows(d, p, rows)
    if magic == b"2zoq":
        bz = d.find(b"BZh", p + 8, p + 32)
        dec = bz2.BZ2Decompressor()
        buf = bytearray()
        pos = bz
        while not dec.eof and pos < len(d):
            buf += dec.decompress(d[pos:pos + (1 << 20)])
            pos += 1 << 20
        return qoi_rows(bytes(buf), rows)
    return qoi_rows(bytes(d[p:p + 12 + struct.unpack_from("<I", d, p + 8)[0]]), rows)


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def qoi_rows(buf, rows):
    """GameMaker's QOI variant (magic 'fioq', little-endian header), to the end of row `rows`."""
    w, h, length = struct.unpack_from("<HHI", buf, 4)
    px, total = buf[12:12 + length], w * min(h, rows) * 4
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
        out[i:i + 4] = bytes((r, g, b, a))
        i += 4
    return w, out


def png_rows(d, p, rows):
    """An 8-bit RGBA or RGB PNG, to the end of row `rows`."""
    o, idat = p + 8, bytearray()
    while True:
        ln = struct.unpack_from(">I", d, o)[0]
        kind, data = d[o + 4:o + 8], d[o + 8:o + 8 + ln]
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack_from(">IIBB", data)
        elif kind == b"IDAT":
            idat += data
        elif kind == b"IEND":
            break
        o += 12 + ln
    if depth != 8 or ctype not in (2, 6):
        raise ValueError("unsupported PNG page")
    ch = 4 if ctype == 6 else 3
    raw, stride = zlib.decompress(bytes(idat)), w * ch
    out, prev = bytearray(), bytearray(stride)
    for y in range(min(h, rows)):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            left = line[x - ch] if x >= ch else 0
            up, ul = prev[x], prev[x - ch] if x >= ch else 0
            if f == 1:
                line[x] = (line[x] + left) & 255
            elif f == 2:
                line[x] = (line[x] + up) & 255
            elif f == 3:
                line[x] = (line[x] + ((left + up) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(up - ul), abs(left - ul), abs(left + up - 2 * ul)
                line[x] = (line[x] + (left if pa <= pb and pa <= pc else up if pb <= pc else ul)) & 255
        prev = line
        out += line if ch == 4 else b"".join(bytes(line[x:x + 3]) + b"\xff" for x in range(0, stride, 3))
    return w, out


def font_glyphs(wad, name, first=1):
    """first: the first count of optional words before the glyph list to try (UNDERTALE's GameMaker 1.4 data has none)."""
    for fp in wad.table("FONT"):
        if wad.cstr(wad.u32(fp)) != name:
            continue
        tpag = wad.tpag(wad.u32(fp + 28))
        for trial in range(first, 6):
            ls = fp + 40 + 4 * trial
            n = wad.u32(ls)
            if 0 < n < 0x10000 and wad.u32(ls + 4) == ls + 4 + 4 * n:
                break
        else:
            raise ValueError("glyph list of %s not found" % name)
        glyphs = {}
        for gp in (wad.u32(ls + 4 + 4 * j) for j in range(n)):
            c, x, y, w, h = (wad.u16(gp + k) for k in range(0, 10, 2))
            kern = {}
            for k in range(wad.u16(gp + 14)):
                kern[wad.i16(gp + 16 + 4 * k)] = wad.i16(gp + 18 + 4 * k)
            glyphs[c] = dict(x=tpag["sx"] + x, y=tpag["sy"] + y, w=w, h=h, shift=wad.i16(gp + 10), offset=wad.i16(gp + 12),
                             kern=kern)
        return tpag["page"], glyphs
    raise ValueError("font %s not found" % name)


def loading_strings(path):
    """The two English strings obj_screen_loading's get_text picks: the chapter prefix (a number follows it) and the
    chapter select's. Found by their shape: the only ASCII strings with a line break the object's Create code pushes."""
    dw = G.DataWin(path)
    entry = next((e for e in dw.code if e and e["name"] == "gml_Object_obj_screen_loading_Create_0"), None)
    if entry is None:
        raise ValueError("obj_screen_loading not found")
    start, size = dw.chunks["STRG"]
    strs = []
    blob = bytes(dw.data[entry["abs"]:entry["abs"] + entry["length"]])
    a = 0
    while a < len(blob):
        ins = G.Instr(blob, a)
        if ins.op in (G.OP_PUSH, G.OP_PUSHLOC, G.OP_PUSHGLB, G.OP_PUSHBLTN) and ins.t1 == G.T_STRING:
            p = dw.u32(start + 4 + 4 * ins.extra)
            s = bytes(dw.data[p + 4:p + 4 + dw.u32(p)]).decode("utf-8", "replace")
            if "\n" in s and s.isascii() and s not in strs:
                strs.append(s)
        a = ins.next
    prefix = [s for s in strs if s.endswith(" ")]
    select = [s for s in strs if not s.endswith(" ")]
    if len(prefix) != 1 or len(select) != 1:
        raise ValueError("unexpected obj_screen_loading strings (%d)" % len(strs))
    return prefix[0], select[0]


def render(text, glyphs, page_w, page):
    """1-bit bitmap of `text`, lines centred like draw_text with fa_center, trimmed to the ink."""
    stride = max(g["h"] for g in glyphs.values())
    ink = {}
    lines = text.split("\n")
    for row, line in enumerate(lines):
        codes = [ord(c) for c in line]
        width = 0
        for i, c in enumerate(codes):
            g = glyphs[c]
            width += g["shift"] + (g["kern"].get(codes[i + 1], 0) if i + 1 < len(codes) else 0)
        x = -(width // 2)
        for i, c in enumerate(codes):
            g = glyphs[c]
            for gy in range(g["h"]):
                base = ((g["y"] + gy) * page_w + g["x"]) * 4
                for gx in range(g["w"]):
                    if page[base + gx * 4 + 3] >= 128:
                        ink[(x + g["offset"] + gx, row * stride + gy)] = 1
            x += g["shift"] + (g["kern"].get(codes[i + 1], 0) if i + 1 < len(codes) else 0)
    xs, ys = [p[0] for p in ink], [p[1] for p in ink]
    x0, y0, w, h = min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1
    row_bytes = (w + 7) // 8
    bits = bytearray(row_bytes * h)
    for (x, y) in ink:
        bits[(y - y0) * row_bytes + (x - x0) // 8] |= 0x80 >> ((x - x0) % 8)
    return w, h, bytes(bits)


def sprite(wad, name):
    for p in wad.table("SPRT"):
        if not p or wad.cstr(wad.u32(p)) != name:
            continue
        w, h = wad.u32(p + 4), wad.u32(p + 8)
        q, speed, speed_type = p + 56, 1.0, 1
        count = wad.i32(q)
        q += 4
        if count == -1:
            version, kind = wad.u32(q), wad.u32(q + 4)
            q += 8
            if kind != 0:
                raise ValueError("%s is not a bitmap sprite" % name)
            speed, speed_type = struct.unpack_from("<f", wad.d, q)[0], wad.u32(q + 4)
            q += 8 + (4 if version >= 2 else 0) + (4 if version >= 3 else 0)
            count = wad.i32(q)
            q += 4
        return w, h, speed, speed_type, [wad.tpag(wad.u32(q + 4 * i)) for i in range(count)]
    raise ValueError("sprite %s not found" % name)


def shown_speed(path, wad, sprite_index):
    """image_speed as set by the Create event of the object that shows the sprite (its first constant from 0 to 1),
    or None. The sprite's own speed applies otherwise."""
    names = {wad.cstr(wad.u32(p)) for p in wad.table("OBJT") if p and wad.i32(p + 4) == sprite_index}
    wanted = {"gml_Object_%s_Create_0" % n for n in names}
    dw = G.DataWin(path)
    for entry in dw.code:
        if not entry or entry["name"] not in wanted:
            continue
        blob = bytes(dw.data[entry["abs"]:entry["abs"] + entry["length"]])
        pos = 0
        while pos < len(blob):
            ins = G.Instr(blob, pos)
            if ins.op == G.OP_PUSH and ins.t1 == G.T_DOUBLE:
                value = struct.unpack_from("<d", blob, pos + 4)[0]
                if 0 < value <= 1:
                    return value
            pos = ins.next
    return None


def rope_facts(path, sprite_index):
    """(rgb, width, length, degrees a frame x 100) of the line the dog's object hangs it from, or None."""
    wad = Wad(path)
    try:
        names = {wad.cstr(wad.u32(p)) for p in wad.table("OBJT") if p and wad.i32(p + 4) == sprite_index}
    finally:
        wad.close()
    dw = G.DataWin(path)
    dw.patch_operands()

    def code(name):
        entry = dw.code_by_name.get(name)
        if not entry or entry["offset"] != 0:
            return []
        blob = bytes(dw.data[entry["abs"]:entry["abs"] + entry["length"]])
        out, pos = [], 0
        while pos < len(blob):
            ins = G.Instr(blob, pos)
            value = None
            if ins.op == G.OP_PUSHI:
                value = ins.inst
            elif ins.op == G.OP_PUSH and ins.t1 == G.T_INT32:
                value = struct.unpack_from("<i", blob, pos + 4)[0]
            elif ins.op == G.OP_PUSH and ins.t1 == G.T_DOUBLE:
                value = struct.unpack_from("<d", blob, pos + 4)[0]
            call = dw.funcs[ins.extra & 0xFFFFFF]["name"] if ins.op == G.OP_CALL else None
            var = dw.vars[ins.var_index()]["name"] if ins.op == G.OP_POP and ins.extra is not None else None
            out.append((value, call, var))
            pos = ins.next
        return out

    for name in sorted(names):
        draw, step, create = (code("gml_Object_%s_%s_0" % (name, e)) for e in ("Draw", "Step", "Create"))
        rgb = width = length = speed = None
        for i, (_, call, _) in enumerate(draw):
            if call == "draw_set_color" and rgb is None:
                last = [v for v, _, _ in draw[:i] if v is not None]
                if last:
                    bgr = int(last[-1])
                    rgb = (bgr & 0xFF) << 16 | (bgr >> 8 & 0xFF) << 8 | (bgr >> 16 & 0xFF)
                    after = [v for v, _, _ in draw[i + 1:] if v is not None]
                    width = int(after[0]) if after else None
            if call == "draw_line_width" and rgb is not None:
                break
        else:
            continue
        for i, (_, call, _) in enumerate(step):
            if call == "lengthdir_x":
                last = [v for v, _, _ in step[:i] if v is not None]
                length = int(last[-1]) if last else None
                break
        for i, (_, _, var) in enumerate(create):
            if var == "anglechange" and i > 0 and create[i - 1][0] is not None:
                speed = int(round(create[i - 1][0] * 100))
                break
        if None not in (rgb, width, length, speed) and 0 < width < 16 and 0 < length < 65536 and 0 < speed < 1000:
            return rgb, width, length, speed
    return None


def dog_frames(path):
    wad = Wad(path)
    try:
        w, h, speed, speed_type, frames = sprite(wad, DOG_SPRITE)
        index = next(i for i, p in enumerate(wad.table("SPRT")) if p and wad.cstr(wad.u32(p)) == DOG_SPRITE)
        entry = wad.table("SPRT")[index]
        origin = (wad.i32(entry + 48), wad.i32(entry + 52))
        image_speed = shown_speed(path, wad, index)
        if image_speed is not None:
            speed *= image_speed
        pages = {}
        for t in frames:
            pages[t["page"]] = max(pages.get(t["page"], 0), t["sy"] + t["sh"])
        decoded = {page: wad.page_rows(page, rows) for page, rows in pages.items()}
    finally:
        wad.close()
    palette, out = [], []
    for t in frames:
        if (t["sw"], t["sh"]) != (t["tw"], t["th"]):
            raise ValueError("scaled sprite frame")
        page_w, page = decoded[t["page"]]
        frame = bytearray(w * h)
        for y in range(t["sh"]):
            for x in range(t["sw"]):
                i = ((t["sy"] + y) * page_w + t["sx"] + x) * 4
                r, g, b, a = page[i:i + 4]
                if a == 0:
                    continue
                # As drawn over the black screen: the colour times its alpha (black then looks like the background).
                rgb = (r * a // 255, g * a // 255, b * a // 255)
                if rgb == (0, 0, 0):
                    continue
                if rgb not in palette:
                    if len(palette) == MAX_COLOURS:
                        raise ValueError("%s has more than %d colours" % (DOG_SPRITE, MAX_COLOURS))
                    palette.append(rgb)
                frame[(t["ty"] + y) * w + t["tx"] + x] = palette.index(rgb) + 1
        out.append(bytes(frame))
    fps = speed if speed_type == 0 else speed * 30.0
    ms = int(round(1000.0 / fps)) if fps > 0 else 125
    ms = max(33, min(ms, 1000))
    trailer = b""
    rope = rope_facts(path, index)
    if rope is not None and 0 <= origin[0] < w and 0 <= origin[1] < h:
        rgb, width, length, speed = rope
        trailer = struct.pack("<4sBBHIHH", b"DSW1", origin[0], origin[1], ms, rgb, width, length) + struct.pack("<HH", speed, 0)
        ms = 33
    return w, h, ms, palette, out, trailer


def known(path, part, revision, table, cache):
    try:
        good, _, _ = data_versions.check_file(path, "deltarune", part, revision, table, cache)
        return good
    except OSError:
        return False


def baker_hash():
    """A change to the baker rebakes the art."""
    with open(os.path.abspath(__file__), "rb") as f:
        return zlib.crc32(f.read())


def setup_message(path, game):
    """The first time setup's line in the game's font as a message for SETUP_TARGET, or None (a line says why)."""
    font = SETUP_FONT[game]
    wad = Wad(path)
    try:
        page_id, glyphs = font_glyphs(wad, font, 0 if game == "undertale" else 1)
        if any(ord(c) not in glyphs for c in SETUP_LINE):
            raise ValueError("%s lacks characters of it" % font)
        rows = max(glyphs[ord(c)]["y"] + glyphs[ord(c)]["h"] for c in SETUP_LINE)
        page_w, page = wad.page_rows(page_id, rows)
    except ValueError as error:
        # The rest of the art doesn't depend on this line.
        print("Loading screen: no setup line (%s), so the bar alone while preparing" % error)
        return None
    finally:
        wad.close()
    w, h, bits = render(SETUP_LINE, glyphs, page_w, page)
    return struct.pack("<BBHHH", SETUP_TARGET, 0, w, h, 0) + bits


def bake_undertale(folder, out):
    """UNDERTALE's loadscreen.bin: the setup line alone (no palette, no dog)."""
    revision, table = data_versions.load_table()
    path = next((os.path.join(folder, n) for n in ("data.win", "game.ios") if os.path.isfile(os.path.join(folder, n))), None)
    inputs = [(path, os.stat(path).st_size, os.stat(path).st_mtime_ns)] if path else []
    stamp_path = out + ".inputs"
    stamp = json.dumps({"revision": revision, "baker": baker_hash(), "game": "undertale", "inputs": inputs})
    if os.path.isfile(stamp_path) and open(stamp_path, encoding="utf-8").read() == stamp and os.path.exists(out):
        print("Loading screen: art is up to date")
        return
    for p in (out, stamp_path):
        if os.path.exists(p):
            os.remove(p)
    good = False
    if path:
        try:
            good, _, _ = data_versions.check_file(path, "undertale", "root", revision, table, {})
        except OSError:
            good = False
    setup = setup_message(path, "undertale") if good else None
    if setup is None:
        if not good:
            print("Loading screen: the UNDERTALE data is missing or not a known version, so the bar alone while preparing")
        open(stamp_path, "w", encoding="utf-8").write(stamp)
        return
    head = struct.pack("<4sHHHHHHBBH", b"DTLS", FORMAT_VERSION, 1, 0, 0, 0, 0, 0, 0, 0)
    body = head + setup + bytes(-len(setup) % 4)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out + ".tmp", "wb") as f:
        f.write(body)
    os.replace(out + ".tmp", out)
    open(stamp_path, "w", encoding="utf-8").write(stamp)
    print("Loading screen: the setup line, %d bytes" % len(body))


def bake(folder, out):
    revision, table = data_versions.load_table()
    root = os.path.join(folder, "data.win")
    ch3 = os.path.join(folder, "chapter3_windows", "data.win")
    chapters = [n for n in range(1, MAX_CHAPTER + 1) if os.path.isfile(os.path.join(folder, "chapter%d_windows" % n, "data.win"))]
    inputs = [(p, os.stat(p).st_size, os.stat(p).st_mtime_ns) for p in (root, ch3) if os.path.isfile(p)]
    stamp_path = out + ".inputs"
    stamp = json.dumps({"revision": revision, "baker": baker_hash(), "chapters": chapters, "inputs": inputs})
    if os.path.isfile(stamp_path) and open(stamp_path, encoding="utf-8").read() == stamp and os.path.exists(out):
        print("Loading screen: art is up to date")
        return
    for p in (out, stamp_path):
        if os.path.exists(p):
            os.remove(p)
    cache = {}
    if not os.path.isfile(root) or not known(root, "root", revision, table, cache):
        print("Loading screen: the launcher data.win is missing or not a known version, so the bar alone")
        open(stamp_path, "w", encoding="utf-8").write(stamp)
        return
    prefix, select = loading_strings(root)
    messages = [(0, select)] + [(n, prefix + str(n)) for n in chapters]
    wad = Wad(root)
    try:
        page_id, glyphs = font_glyphs(wad, FONT_NAME)
        missing = sorted({c for _, t in messages for c in t if c != "\n" and ord(c) not in glyphs})
        if missing:
            raise ValueError("%s lacks %d characters" % (FONT_NAME, len(missing)))
        rows = max(glyphs[ord(c)]["y"] + glyphs[ord(c)]["h"] for _, t in messages for c in t if c != "\n")
        page_w, page = wad.page_rows(page_id, rows)
    finally:
        wad.close()
    blobs = []
    for target, text in messages:
        w, h, bits = render(text, glyphs, page_w, page)
        blobs.append(struct.pack("<BBHHH", target, 0, w, h, 0) + bits)
    setup = setup_message(root, "deltarune")
    if setup:
        blobs.append(setup)
    dog = None
    if os.path.isfile(ch3) and known(ch3, "chapter3_windows", revision, table, cache):
        dog = dog_frames(ch3)
    elif os.path.isfile(ch3):
        print("Loading screen: Chapter 3 is not a known version, so no dog")
    dw_, dh_, ms, palette, frames, trailer = dog if dog else (0, 0, 0, [], [], b"")
    head = struct.pack("<4sHHHHHHBBH", b"DTLS", FORMAT_VERSION, len(blobs), len(frames), dw_, dh_, ms, len(palette), 0, 0)
    pal = b"".join(bytes(c) for c in palette)
    pal += b"\0" * (-len(pal) % 4)
    body = head + pal + b"".join(b + b"\0" * (-len(b) % 4) for b in blobs) + b"".join(frames) + trailer
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out + ".tmp", "wb") as f:
        f.write(body)
    os.replace(out + ".tmp", out)
    open(stamp_path, "w", encoding="utf-8").write(stamp)
    print("Loading screen: %d messages%s%s, %d bytes" % (len(blobs), ", the Chapter 3 dog (%d frames, %d ms)" % (len(frames), ms)
                                                         if frames else ", no dog", ", on its rope" if trailer else "", len(body)))


def main(argv):
    args = dict(zip(argv[1::2], argv[2::2]))
    game = args.get("--game", "deltarune")
    if "--out" not in args or set(args) - {"--out", "--data", "--game"} or game not in SETUP_FONT:
        print(__doc__)
        return 2
    import prepare
    folder = prepare.find_data(game, args.get("--data"))
    if not folder:
        print("Loading screen: no %s game data found, so the bar alone" % game.upper())
        return 0
    try:
        (bake_undertale if game == "undertale" else bake)(folder, args["--out"])
    except (OSError, ValueError, G.Reject) as error:
        print("Loading screen: art not made (%s), so the bar alone" % error)
        for p in (args["--out"],):
            if os.path.exists(p):
                os.remove(p)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
