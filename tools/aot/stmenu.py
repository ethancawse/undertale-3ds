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
    z_smalltext.c places them (nominal x, carry, row shift). Text is black on a white bubble, as in battle;
  - the Crisp cards (round 18, playtest 10-25: Mettaton EX's "HAPPY BREAKTIME" card "looked broken"): each frame of a
    lettered card sprite at half size, drawn with the Text part Crisp instead of Smart's averaged card (r16). The
    letters are derived from the player's data.win as glyphs (crisp_card; no hand-drawn pixel): the card's words are
    its row bands with ink and their letters the column runs with ink; each word gets one even 2:1 row split for all its
    letters and each letter an even 2:1 column split, both at the phase stglyphs picks (uniform_cuts, as fnt_papyrus),
    a pixel inked by block coverage with stroke rescue, counters kept open (keep_holes). The letters are set 1 px apart,
    the word centred on its PC centre halved and its top at its PC top halved; the rest of the card is its backdrop.

usage: py -3 -I tools/aot/stmenu.py <data.win> --out <file.dtsm>     (stpack.build calls menu_file for UNDERTALE)

File (little-endian): b"DTSM", u32 version 1, u32 data.win size, u32 GEN8 game ID, u64 GEN8 timestamp (the pack's
key), u16 soul count, u16 sharp sprite count, u16 preview count, u16 0; then per soul sprite: char name[24], u8 frames
(at most 4), u8 0 x 3, u32 colour of each of 4 frames (0x00BBGGRR, the draw colour's order), u8 8 row masks (bit x =
column x); then per sharp sprite: char name[24], u8 group (1 Bones, 2 Numbers), u8 0 x 3; then per preview: u8 part (0
Text, 1 Bones, 2 Numbers, 3 Soul), u8 Crisp (0 or 1), u16 width, u16 height, u16 0, then width * height u16 RGB565
pixels row by row, padded to 4 bytes. The header's last u16 (0 in the first files, which z_smalltext.c reads as no
cards) is the card count; then per card frame (the card is half the sprite's size, opaque: its pixels without a letter
are drawn black): char sprite name[24], u8 frame, u8 4 (entries), u8 height, u8 0, then 4 entries of z_smalltext.c's
atlas, 16 columns each from the left (the last one narrower): u8 0, u8 0, u8 width, u8 0 x 3, then per row a u16 ink
mask (bit x = column x); padded to 4 bytes.

bubble_file writes the bubble table beside it (<data.win folder>.dtsb; z_smalltext.c's bubbleclip keeps a writer's
Crisp line off its bubble's inner edge): b"DTSB", u32 version 2, u32 data.win size, u32 GEN8 game ID, u64 GEN8
timestamp, u32 sprite count; then per bubble sprite (each sprite named with "blcon", frame 0, whose edges are at most
255): u16 sprite index, u16 height, then height u8: the row's inner right edge, the first column right of the row's
first run of white pixels (alpha and red at least 128) that isn't white, at most the commonest such column (the body's
edge, so the tail never counts as room); 0 for a row without white. Then u32 font count and per font 112 bytes: its
name (16 bytes, NUL padded), then int8 px for chars 32..127: obj_base_writer's kerning, the px its Draw adds to a
letter's advance in that font (read from the writer's own code, writer_kerning; English text).

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
# Lettered cards with a Crisp remake (white letters on an opaque black card): Mettaton EX's break card.
CARD_SPRITES = ('spr_happybreaktime',)
CARD_PARTS = 4  # z_smalltext.c draws a card as four 16-pixel atlas entries, so 64 pixels wide at most
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


def bands(v):
    """[(start, end)] of the runs of true in v."""
    out, i = [], 0
    while i < len(v):
        if v[i]:
            j = i
            while j < len(v) and v[j]:
                j += 1
            out.append((i, j))
            i = j
        i += 1
    return out


def crisp_card(ink):
    """The half-size card of a frame's letter ink (rows of bools; see the top), or None when it has none."""
    h, w = len(ink), len(ink[0])
    params = sg.font_params('fnt_papyrus')
    card = [[False] * (w // 2) for _ in range(h // 2)]
    words = bands([any(r) for r in ink])
    for y0, y1 in words:
        letters = bands([any(ink[y][x] for y in range(y0, y1)) for x in range(w)])
        cells = [[row[a:b] for row in ink[y0:y1]] for a, b in letters]
        rg = sg.row_map(dict(enumerate(cells)), (y1 - y0 + 1) // 2, y1 - y0, uniform=2)
        glyphs = []
        for a in cells:
            gaps = [(g0, g1, 1.0) for row in sg.apply_rows(a, rg) for g0, g1 in sg.interior_gaps(row)]
            glyphs.append(sg.finish(a, None, rg, sg.uniform_cuts(sg.width(a), 2, gaps), params))
        total = sum(sg.width(g) for g in glyphs) + len(glyphs) - 1
        x = int(round((letters[0][0] + letters[-1][1]) / 4.0 - total / 2.0))
        for g in glyphs:
            for ry, row in enumerate(g):
                for rx, v in enumerate(row):
                    if v and 0 <= y0 // 2 + ry < len(card) and 0 <= x + rx < len(card[0]):
                        card[y0 // 2 + ry][x + rx] = True
            x += sg.width(g) + 1
    return card


def cards(dw, names):
    """[(name, frame, rows of bools)] of the card sprites' frames: opaque cards whose ink is white letters on black."""
    out = []
    for name in CARD_SPRITES:
        if name not in names:
            continue
        w, h, frames = sprite_frames(dw, names[name])
        if not 16 * (CARD_PARTS - 1) < w // 2 <= 16 * CARD_PARTS or not 0 < h // 2 < 256 or not frames:
            continue
        if any(px[3] != 255 or px[:3] not in ((0, 0, 0), (255, 255, 255)) for a in frames for row in a for px in row):
            continue
        for i, a in enumerate(frames):
            out.append((name, i, crisp_card([[px[0] == 255 for px in row] for row in a])))
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
    card_list = cards(dw, names)
    w = dw.win
    out = bytearray(MAGIC + struct.pack('<IIIQHHHH', VERSION, w.size, w.game_id, w.timestamp, len(soul_list), len(sharp),
                                        len(prev), len(card_list)))
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
    for name, frame, rows in card_list:
        h, wd = len(rows), len(rows[0])
        out += name.encode('ascii')[:23].ljust(24, b'\0') + struct.pack('<BBBB', frame, CARD_PARTS, h, 0)
        for x0 in range(0, 16 * CARD_PARTS, 16):
            pw = max(0, min(16, wd - x0))
            out += struct.pack('<BBBBBB', 0, 0, pw, 0, 0, 0)
            out += b''.join(struct.pack('<H', sum(1 << x for x in range(pw) if row[x0 + x])) for row in rows)
        out += bytes(-len(out) % 4)
    if not quiet:
        print(f'smoothmenu: {len(soul_list)} soul sprites, {len(sharp)} sharp sprites, {len(prev)} previews, '
              f'{len(card_list)} card frames')
        for name, frame, rows in card_list:
            print(f'smoothmenu: Crisp card {name} {frame} ' + ' '.join(''.join('#' if v else '.' for v in r) for r in rows))
        if design is not None:
            print('smoothmenu: Crisp soul ' + ' '.join(''.join('#' if v else '.' for v in r) for r in design))
    return bytes(out)


BUBBLE_MAGIC = b'DTSB'


def bubble_edges(rows, w):
    """The bubble table's rows of a sprite's frame (RGBA rows, width w); see the top."""
    raw = []
    for row in rows:
        x = next((x for x in range(w) if row[x][3] >= 128 and row[x][0] >= 128), None)
        if x is not None:
            while x < w and row[x][3] >= 128 and row[x][0] >= 128:
                x += 1
        raw.append(x)
    seen = [e for e in raw if e is not None]
    if not seen:
        return None
    body = max(set(seen), key=lambda e: (seen.count(e), -e))
    return [0 if e is None else min(e, body) for e in raw]


WRITER_DRAW = 'gml_Object_obj_base_writer_Draw_0'


def writer_kerning(path):
    """obj_base_writer's kerning, {font index: {char: px}}: in its Draw, after a letter is drawn, blocks of the shape
    `if (myfont == F) { if (myletter == "c") letterx += n; ... }` (several letters before one change share it) move the
    next letter's place, myx. Blocks under a global.typer or global.language test (one typer's own placement, other
    languages) are left out. {} when the data has no such writer."""
    import gmlaot as ga
    win = ga.DataWin(path)
    e = win.code_by_name.get(WRITER_DRAW)
    if e is None:
        return {}
    win.patch_operands()
    blob = bytes(win.data[e['abs']:e['abs'] + e['length']])
    seq, addr = [], e['offset']
    while addr < len(blob):
        i = ga.Instr(blob, addr)
        seq.append(i)
        addr = i.next

    def var(i):
        if i.extra is None or not (i.op in (ga.OP_PUSHLOC, ga.OP_PUSHGLB, ga.OP_POP) or (i.op == ga.OP_PUSH and i.t1 == ga.T_VAR)):
            return None
        return win.vars[i.var_index()]['name'] if i.var_index() < len(win.vars) else None

    def at(k):
        return seq[k] if k < len(seq) else None

    font, letters, out = None, [], {}
    for k, i in enumerate(seq):
        name = var(i)
        if i.op == ga.OP_PUSHGLB and name in ('typer', 'language'):
            font, letters = None, []
        elif name == 'myfont' and at(k + 1) and at(k + 1).op == ga.OP_PUSHI and at(k + 2) and at(k + 2).op == ga.OP_CMP:
            font, letters = at(k + 1).inst, []
        elif (i.op == ga.OP_PUSH and i.t1 == ga.T_STRING and k and var(seq[k - 1]) == 'myletter' and at(k + 1)
              and at(k + 1).op == ga.OP_CMP):
            text = win.strg(i.extra)
            if text is not None and len(text) == 1:
                letters.append(chr(text[0]))
        elif (name == 'letterx' and i.op == ga.OP_PUSH and at(k + 3) and at(k + 1).op == ga.OP_PUSHI
              and at(k + 2).op in (ga.OP_ADD, ga.OP_SUB) and at(k + 3).op == ga.OP_POP and var(at(k + 3)) == 'letterx'):
            if font is not None:
                n = at(k + 1).inst if at(k + 2).op == ga.OP_ADD else -at(k + 1).inst
                for c in letters:
                    out.setdefault(font, {})[c] = n
            letters = []
    return out


def bubble_file(path):
    """The .dtsb of the data.win at path (see the top)."""
    dw = sg.GameData(path)
    recs = []
    for index, p in enumerate(dw.plist('SPRT')):
        if not p or 'blcon' not in dw.string(dw.u32(p)):
            continue
        w, h, frames = sprite_frames(dw, p)
        edges = bubble_edges(frames[0], w) if frames else None
        if edges is not None and max(edges) <= 255:
            recs.append(struct.pack('<HH', index, h) + bytes(edges))
    fonts = []
    names = [dw.string(dw.u32(q)) if q else None for q in dw.plist('FONT')]
    for index, table in sorted(writer_kerning(path).items()):
        name = names[index] if 0 <= index < len(names) else None
        if name is None or len(name.encode('ascii', 'replace')) > 15:
            continue
        px = [max(-128, min(127, table.get(chr(c), 0))) for c in range(32, 128)]
        fonts.append(name.encode('ascii', 'replace').ljust(16, b'\0') + struct.pack('<96b', *px))
    w = dw.win
    return (BUBBLE_MAGIC + struct.pack('<IIIQI', 2, w.size, w.game_id, w.timestamp, len(recs)) + b''.join(recs)
            + struct.pack('<I', len(fonts)) + b''.join(fonts))


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
