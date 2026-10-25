"""Write the Smoothing menu's data for the 3DS RomFS (UNDERTALE): romfs/fonts/<data.win folder>.dtsm.

The options menu's Smoothing row (platform/3ds/source/ctr_optmenu.c) is Recommended or Custom; Custom opens a page that
sets four parts one by one, Smooth or Crisp, each with a preview of the part as a battle draws it.
This file holds what that needs from the player's own data.win, derived at build time
and never committed (stpack.build writes it beside the glyph pack):

  - the Crisp soul: a hand-picked 8x8 soul (drawn from the 2:1 pick with one pixel removed). It is the soul sprite's
    texels at even rows and columns (the pixels a whole-pixel draw at an even place shows) kept only where the
    left-right mirror image has ink too: the pick's one lone pixel beside the notch goes, and the shape is symmetric.
    The coloured and turned souls have the same outline as spr_heart (checked: identical alpha, or the
    same turned by a quarter, a half or mirrored), so each gets the same 8x8 turned the same way, in its own colour;
  - the sprites the Bones and Numbers parts keep sharp (group 1 and 2);
  - the previews: each part Smooth and Crisp, RGB565 at native size, as the battle draws it: Smooth is the 2x2 average
    of the texels over black (Smart smoothing at an even place; checked against Azahar captures of the soul, mean error
    0.04 of 255), Crisp the texels at even rows and columns, the Crisp soul, or the pack's text glyphs placed as
    z_smalltext.c places them (nominal x, carry, row shift). Text is black on a white bubble, as in battle.

usage: py -3 -I tools/aot/stmenu.py <data.win> --out <file.dtsm>     (stpack.build calls menu_file for UNDERTALE)

File (little-endian): b"DTSM", u32 version 1, u32 data.win size, u32 GEN8 game ID, u64 GEN8 timestamp (the pack's
key), u16 soul count, u16 sharp sprite count, u16 preview count, u16 0; then per soul sprite: char name[24], u8 frames
(at most 4), u8 0 x 3, u32 colour of each of 4 frames (0x00BBGGRR, the draw colour's order), u8 8 row masks (bit x =
column x); then per sharp sprite: char name[24], u8 group (1 Bones, 2 Numbers), u8 0 x 3; then per preview: u8 part (0
Text, 1 Bones, 2 Numbers, 3 Soul), u8 Crisp (0 or 1), u16 width, u16 height, u16 0, then width * height u16 RGB565
pixels row by row, padded to 4 bytes.

Pure Python (the builder ships the embeddable Python, without numpy). Reads the game data; run with -I.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script's folder off the path
import stglyphs as sg  # noqa: E402

MAGIC = b'DTSM'
VERSION = 1
SOUL_REF = 'spr_heart'
# The player's soul in every colour and turn (sprite names of UNDERTALE 1.08); each is checked against SOUL_REF.
SOUL_SPRITES = ('spr_heart', 'spr_heart_battle_pl', 'spr_ourheart', 'spr_confuseheart', 'spr_heartred_center',
                'spr_heartblue', 'spr_heartblue_r', 'spr_heartblue_l', 'spr_heartblue_u', 'spr_heartgreen',
                'spr_heartaqua', 'spr_heartorange', 'spr_heartyellow', 'spr_heartyellow_center', 'spr_heartyellow_flip',
                'spr_yellowheart_pl', 'spr_heartpurple', 'spr_heartpurple_center')
# Bones (group 1): Sans's bones (every sprite named with this prefix) and Papyrus's bone ends. Numbers (group 2): the
# damage numbers, MISS, and the big 9s of the last hit. The soul list above is the player's soul only (the rule for
# the Crisp soul): the six human souls and the monster souls (same outline) keep their sprites.
BONE_PREFIX = 'spr_s_bone'
BONE_SPRITES = ('spr_bonetop', 'spr_bonebottom')
NUMBER_SPRITES = ('spr_dmgnum_o', 'spr_dmgnum', 'spr_dmgmiss_o', 'spr_num9')
TEXT_LINES = (('fnt_plain', '* Hello! How are you?'), ('fnt_papyrus', 'HELLO THERE, HUMAN!'))
BUBBLE_PAD = 4  # native px of white around the text, as a bubble


# ---- sprites ----

def sprite_names(dw):
    out = {}
    for p in dw.plist('SPRT'):
        if p:
            out[dw.string(dw.u32(p))] = p
    return out


def sprite_frames(dw, p):
    """(width, height, [RGBA rows per frame]) of the sprite at p (bitmap sprites only)."""
    w, h = dw.u32(p + 4), dw.u32(p + 8)
    q = p + 56
    count = struct.unpack_from('<i', dw.d, q)[0]
    q += 4
    if count == -1:
        version, kind = dw.u32(q), dw.u32(q + 4)
        if kind != 0:
            return w, h, []
        q += 16 + (4 if version >= 2 else 0) + (4 if version >= 3 else 0)
        count = struct.unpack_from('<i', dw.d, q)[0]
        q += 4
    frames = []
    for i in range(count):
        t = dw.tpag(dw.u32(q + 4 * i))
        a = [[(0, 0, 0, 0)] * w for _ in range(h)]
        for y, row in enumerate(dw.region(t['page'], t['sx'], t['sy'], t['sw'], t['sh'])):
            for x, px in enumerate(row):
                if 0 <= t['ty'] + y < h and 0 <= t['tx'] + x < w:
                    a[t['ty'] + y][t['tx'] + x] = px
        frames.append(a)
    return w, h, frames


def alpha_mask(a):
    return [[px[3] > 127 for px in row] for row in a]


def turns(m):
    """The 8 turns and mirror images of square mask m: {name: mask}, each a function of m alone."""
    def rot(b):  # a quarter turn clockwise
        n = len(b)
        return [[b[n - 1 - x][y] for x in range(n)] for y in range(n)]

    def flip(b):
        return [list(reversed(r)) for r in b]
    out = {}
    b = m
    for k in range(4):
        out[(k, False)] = b
        out[(k, True)] = flip(b)
        b = rot(b)
    return out


def turned(m, key):
    k, mirrored = key
    for _ in range(k):
        n = len(m)
        m = [[m[n - 1 - x][y] for x in range(n)] for y in range(n)]
    return [list(reversed(r)) for r in m] if mirrored else m


def crisp_soul(m16):
    """The Crisp soul from spr_heart's 16x16 mask: the even rows and columns, kept where the mirror image has ink."""
    pick = [row[0::2] for row in m16[0::2]]
    return [[v and row[len(row) - 1 - x] for x, v in enumerate(row)] for row in pick]


def souls(dw, names):
    """[(name, [frame colours 0xBBGGRR], 8 row masks)] of the soul sprites whose every frame has spr_heart's outline
    (turned or mirrored) in one colour."""
    if SOUL_REF not in names:
        return [], None
    w, h, ref = sprite_frames(dw, names[SOUL_REF])
    if (w, h) != (16, 16) or not ref:
        return [], None
    ref_mask = alpha_mask(ref[0])
    design = crisp_soul(ref_mask)
    keys = turns(ref_mask)
    out = []
    for name in SOUL_SPRITES:
        if name not in names:
            continue
        w, h, frames = sprite_frames(dw, names[name])
        if (w, h) != (16, 16) or not frames or len(frames) > 4:
            continue
        key = None
        colours = []
        for a in frames:
            m = alpha_mask(a)
            hit = next((k for k, v in sorted(keys.items()) if v == m), None)
            inks = {px[:3] for row in a for px in row if px[3] > 127}
            if hit is None or (key is not None and hit != key) or len(inks) != 1:
                key = None
                break
            key = hit
            r, g, b = inks.pop()
            colours.append(r | g << 8 | b << 16)
        if key is None:
            continue
        out.append((name, colours, turned(design, key)))
    return out, design


def sharp_sprites(names):
    out = [(n, 1) for n in sorted(names) if n.startswith(BONE_PREFIX)]
    out += [(n, 1) for n in BONE_SPRITES if n in names]
    out += [(n, 2) for n in NUMBER_SPRITES if n in names]
    return out


# ---- previews: rows of (r, g, b) floats 0..255 ----

def blank(h, w, c=(0.0, 0.0, 0.0)):
    return [[c] * w for _ in range(h)]


def smooth(a):
    """Smart smoothing at an even place: each pixel the 2x2 texels' premultiplied average, over black."""
    h, w = len(a), len(a[0])
    out = []
    for y in range(0, h, 2):
        row = []
        for x in range(0, w, 2):
            s = [0.0, 0.0, 0.0]
            for dy in (0, 1):
                for dx in (0, 1):
                    if y + dy < h and x + dx < w:
                        px = a[y + dy][x + dx]
                        for c in range(3):
                            s[c] += px[c] * px[3] / 255.0
            row.append((s[0] / 4, s[1] / 4, s[2] / 4))
        out.append(row)
    return out


def crisp(a):
    """Crisp (no smoothing) at an even place: the texels at even rows and columns, over black."""
    return [[px[:3] if px[3] > 127 else (0, 0, 0) for px in row[0::2]] for row in a[0::2]]


def hcat(imgs, gap):
    h = max(len(i) for i in imgs)
    out = blank(h, sum(len(i[0]) for i in imgs) + gap * (len(imgs) - 1))
    x = 0
    for i in imgs:
        y0 = (h - len(i)) // 2
        for y, row in enumerate(i):
            out[y0 + y][x:x + len(row)] = row
        x += len(i[0]) + gap
    return out


def vcat(imgs, gap):
    w = max(len(i[0]) for i in imgs)
    out = []
    for k, i in enumerate(imgs):
        if k:
            out += blank(gap, w)
        x0 = (w - len(i[0])) // 2
        for row in i:
            out.append([(0.0, 0.0, 0.0)] * x0 + list(row) + [(0.0, 0.0, 0.0)] * (w - x0 - len(row)))
    return out


# Crisp text placement: smalltext_carry.py's model of z_smalltext.c (nominal_x, touch, Line).
def nominal_x(pen, left, ph):
    nx = (pen >> 1) + left
    e = -(pen & 1) - ph
    if e <= -2:
        nx += (-e) // 2
    elif e >= 2:
        nx -= e // 2
    return nx


def touch(a, ax, b, bx, dy=0):
    for r in range(len(a)):
        if a[r] == sg.NONE:
            continue
        right = a[r][1] + ax
        for q in (r - dy - 1, r - dy, r - dy + 1):
            if 0 <= q < len(b) and b[q] != sg.NONE and b[q][0] + bx - right <= 1:
                return True
    return False


class Line:
    def __init__(self, adv640):
        self.adv = adv640
        self.live = False

    def place(self, pen, nx0, native, pc, space_before=False):
        if not self.live or pen <= self.pen or pen - self.pen > 4 * self.adv:
            self.live, self.carry, self.prev = True, 0, None
        carry = self.carry
        nx = nx0 + carry
        p = self.prev
        if p is not None and not space_before and pen - self.pen <= self.adv + 1 and not touch(p[2], p[3], pc, pen):
            while carry > 0 and not touch(p[0], p[1], native, nx - 1):
                carry -= 1
                nx -= 1
            k = 0
            while k < 2 and touch(p[0], p[1], native, nx):
                k += 1
                nx += 1
            carry += k
        self.carry, self.pen = carry, pen
        self.prev = (native, nx, pc, pen)
        return nx


def crisp_line(s, pens, packed):
    """The Crisp glyphs of line s at 640-space pens as z_smalltext.c draws them: [(bitmap, native x)], and the row
    shift (fix W on, as the Text part turns it on)."""
    rows, glyphs, cell_h, adv = packed
    base = {ch: (g, left, ext) for ch, variant, g, left, ext in glyphs if variant == 0}
    fix = {ch: (g, left, ext) for ch, variant, g, left, ext in glyphs if variant == 1}
    line = Line(adv)
    space = False
    out = []
    for ch, pen in zip(s, pens):
        c = ord(ch)
        entry = fix.get(c) or base.get(c)
        if ch == ' ' or entry is None or not any(any(r) for r in entry[0]):
            space = True
            continue
        g, left, ext = entry
        nx0 = nominal_x(pen, left, sg.phase(ext, left, g))
        out.append((g, line.place(pen, nx0, sg.extents(g), ext, space_before=space)))
        space = False
    return out, sg.row_shifts(glyphs)[0]


def text_bubble(dw, name, s, mode, packed):
    """One line of text in font `name`, black on a white bubble, Smooth (the PC glyphs averaged) or Crisp (the pack's
    glyphs). packed: stpack.font_glyphs' (rows, glyphs, cell_h, adv). The bubble fits both, so the two cells match."""
    font = dw.fonts()[name]
    rows, glyphs, cell_h, adv = packed
    pens, x, right = [], 0, 0
    for ch in s:
        g = font['glyphs'].get(ord(ch))
        pens.append(x)
        if g and g['w']:
            right = max(right, x + g['offset'] + g['w'])
        x += g['shift'] if g else adv
    placed, shift = crisp_line(s, pens, packed)
    w = max((max(x, right) + 1) // 2, max((nx + sg.width(g) for g, nx in placed), default=0))
    h = (cell_h + 1) // 2
    out = blank(h + 2 * BUBBLE_PAD, w + 2 * BUBBLE_PAD, (255.0, 255.0, 255.0))
    if mode == 0:
        big = [[0.0] * (2 * w + 2) for _ in range(2 * h)]
        t = font['tpag']
        for ch, pen in zip(s, pens):
            g = font['glyphs'].get(ord(ch))
            if not g or not g['w']:
                continue
            for y, row in enumerate(dw.region(t['page'], t['sx'] + g['x'], t['sy'] + g['y'], g['w'], g['h'])):
                for xx, px in enumerate(row):
                    X = pen + g['offset'] + xx
                    if y < 2 * h and 0 <= X < len(big[0]):
                        big[y][X] = max(big[y][X], px[3] / 255.0)
        for y in range(h):
            for xx in range(w):
                cov = (big[2 * y][2 * xx] + big[2 * y][2 * xx + 1] + big[2 * y + 1][2 * xx] + big[2 * y + 1][2 * xx + 1]) / 4
                v = 255.0 * (1.0 - cov)
                out[BUBBLE_PAD + y][BUBBLE_PAD + xx] = (v, v, v)
        return out
    for g, nx in placed:
        for y, row in enumerate(g):
            for xx, v in enumerate(row):
                Y, X = BUBBLE_PAD + y + shift, BUBBLE_PAD + nx + xx
                if v and 0 <= Y < len(out) and 0 <= X < len(out[0]):
                    out[Y][X] = (0.0, 0.0, 0.0)
    return out


def previews(dw, names, design, packed):
    """{(part, crisp): image}."""
    out = {}
    lines = [(n, s) for n, s in TEXT_LINES if n in packed]
    if lines:
        for mode in (0, 1):
            out[(0, mode)] = vcat([text_bubble(dw, n, s, mode, packed[n]) for n, s in lines], 6)
    bones = []
    for n, crop in (('spr_s_bonestab_h', None), ('spr_s_bonewall', None)):
        if n in names:
            _, _, fr = sprite_frames(dw, names[n])
            if fr:
                bones.append(fr[0])
    if bones:
        for mode in (0, 1):
            out[(1, mode)] = hcat([(crisp if mode else smooth)(a) for a in bones], 8)
    if 'spr_dmgnum_o' in names:
        _, _, fr = sprite_frames(dw, names['spr_dmgnum_o'])
        if len(fr) >= 8:
            d3, d7 = fr[3], fr[7]  # "37", the digits 32 texels apart as the game draws them
            w = len(d3[0])
            a = [list(r) + [(0, 0, 0, 0)] * (w - 4) for r in d3]
            for y, row in enumerate(d7):
                for x, px in enumerate(row):
                    if px[3] > 0:
                        a[y][w - 4 + x] = px
            for mode in (0, 1):
                out[(2, mode)] = (crisp if mode else smooth)(a)
    if design is not None:
        _, _, fr = sprite_frames(dw, names[SOUL_REF])
        out[(3, 0)] = smooth(fr[0])
        r, g, b = next(px[:3] for row in fr[0] for px in row if px[3] > 127)
        out[(3, 1)] = [[(r, g, b) if v else (0, 0, 0) for v in row] for row in design]
    return out


def trim(img):
    """Cut black rows and columns from the edges (the cell centres what is left)."""
    ys = [y for y, row in enumerate(img) if any(max(px) > 4 for px in row)]
    xs = [x for x in range(len(img[0])) if any(max(img[y][x]) > 4 for y in range(len(img)))]
    if not ys:
        return img
    return [row[xs[0]:xs[-1] + 1] for row in img[ys[0]:ys[-1] + 1]]


def rgb565(px):
    r, g, b = (min(255, max(0, int(v + 0.5))) for v in px)
    return (r * 31 + 127) // 255 << 11 | (g * 63 + 127) // 255 << 5 | (b * 31 + 127) // 255


def menu_file(path, packed, quiet=False):
    """The .dtsm of the data.win at path; packed: {font name: stpack.font_glyphs(...)} for the fonts the pack has."""
    dw = sg.GameData(path)
    names = sprite_names(dw)
    soul_list, design = souls(dw, names)
    sharp = sharp_sprites(names)
    prev = previews(dw, names, design, packed)
    w = dw.win
    out = bytearray(MAGIC + struct.pack('<IIIQHHHH', VERSION, w.size, w.game_id, w.timestamp, len(soul_list), len(sharp),
                                        len(prev), 0))
    for name, colours, mask in soul_list:
        out += name.encode('ascii')[:23].ljust(24, b'\0') + struct.pack('<B3x', len(colours))
        out += struct.pack('<4I', *(colours + [0] * (4 - len(colours))))
        out += bytes(sum(1 << x for x, v in enumerate(row) if v) for row in mask)
    for name, group in sharp:
        out += name.encode('ascii')[:23].ljust(24, b'\0') + struct.pack('<B3x', group)
    for (part, mode), img in sorted(prev.items()):
        img = trim(img) if part != 0 else img
        h, wd = len(img), len(img[0])
        out += struct.pack('<BBHHH', part, mode, wd, h, 0)
        for row in img:
            for px in row:
                out += struct.pack('<H', rgb565(px))
        if (wd * h) & 1:
            out += b'\0\0'
    if not quiet:
        print(f'smoothmenu: {len(soul_list)} soul sprites, {len(sharp)} sharp sprites, {len(prev)} previews')
        if design is not None:
            print('smoothmenu: Crisp soul ' + ' '.join(''.join('#' if v else '.' for v in r) for r in design))
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('data_win')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    import stpack
    dw = sg.GameData(a.data_win)
    packed = {n: stpack.font_glyphs(dw, n) for n, _ in TEXT_LINES if n in dw.fonts()}
    blob = menu_file(a.data_win, packed)
    with open(a.out, 'wb') as f:
        f.write(blob)
    print(f'smoothmenu: wrote {a.out} ({len(blob)} B)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
