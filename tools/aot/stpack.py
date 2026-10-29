"""Write the smalltext glyph pack (Crisp text) for the 3DS RomFS: romfs/fonts/<data.win folder>.dtst.

The release builder's pack writer: plain Python (tools/aot/stglyphs.py derives the glyphs), byte-identical to the
analysis tool tools/analysis/smalltext_pack.py (numpy), which stays as the reference the pack is checked against.

The pack holds, for each data.win of the game, the derived native glyphs of the fonts the Crisp keys name, plus the
proposed W and `*` corrections as alternative glyphs. It is derived from the player's own data.win at build time and is
never committed. The runtime (platform/3ds/source/z_smalltext.c) reads it only when a Crisp key in settings.ini (or
--ini) is on.

usage: py -3 -I tools/aot/stpack.py --game <undertale|deltarune> --romfs <dir> [--data <game folder>]   (build.sh)
       py -3 -I tools/aot/stpack.py <data.win> --out <file.dtst>

File (little-endian): b"DTST", u32 version 2, u32 data.win size, u32 GEN8 game ID, u64 GEN8 timestamp, u32 font count;
then per font: char name[24] (NUL-padded), u8 key, u8 rows, u8 cell width, u8 glyph count, u8 PC rows (the font's cell
height in 640 space), u8 widest advance (640 space), i8 row shift for a line on an even and i8 for one on an odd surface
row (native rows added to every glyph's row 0, smalltext_carry.row_shifts); then per glyph: u16 char, u8 width, i8 left
(native px from the pen's native pixel), u8 variant (0 the derived glyph, 1 the W correction, 2 the `*` correction, 3
UNDERTALE's KR label in fnt_curs: the KR meter sprite's white texels at even rows and columns, char 0), i8 phase (the PC
glyph's leftmost ink column from the pen minus 2 * (left + the bitmap's first ink column), 640 px;
smalltext_carry.nominal_x), then `rows` u16 row masks (bit x = column x from the glyph's left), then per PC row i8
leftmost and i8 rightmost ink column from the pen (127, -128 for an empty row): the extents "carry" spacing tests
contact on (smalltext_carry.py).
Optional UNDERTALE ATX1 trailer (stasrtext.py): u32 font index, u32 text length,
128 u8 alternate-entry indices (entry + 1), two i8 row shifts, u8 first affected
line, u8 reserved, then that many bytes of preprocessed draw text. Variant 4
entries are selected only for an exact match and only after the first line.
Optional ASY1 extension: u32 font index, u32 text length, then original writer
text. Only that writer's first line uses original PC y in carry's PC-contact test.
Keys (z_smalltext.c, ini names): 1 crisp_ut_dialogue (fnt_plain), 2 crisp_dr_dialogue (fnt_dotumche), 3 crisp_ut_hud
(fnt_curs), 4 crisp_sans (fnt_comicsans), 5 crisp_papyrus (fnt_papyrus).

Pure Python (the builder ships the embeddable Python, without numpy). Reads the game data; run with -I.
"""
import argparse
import hashlib
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script's folder off the path
import stglyphs as sg  # noqa: E402

MAGIC = b'DTST'
VERSION = 2
# fnt_comicsans (Sans, key 4 crisp_sans): the PC-height remake (smoothux 10-24, sansfont 10-26), in the release tables.
# fnt_dotumche (DELTARUNE's enemy bubbles, key 2 crisp_dr_dialogue): in the release tables since DELTARUNE New's
# Crisp text (sansfont round 3, 10-26), with fnt_comicsans: each chapter's pack holds those two.
FONT_KEYS = {n: k for n, k in (('fnt_plain', 1), ('fnt_dotumche', 2), ('fnt_curs', 3), ('fnt_comicsans', 4), ('fnt_papyrus', 5),
                               ('fnt_main', 1), ('fnt_maintext', 1))
             if n in sg.FONT_PARAMS}
# Fonts packed for UNDERTALE only. DELTARUNE's chapters have an fnt_main that is a different font (1x) from UNDERTALE's
# 2x one (fnt_main, key 1: the game-over refusal line), packed as DR_LIKE says. Their fnt_comicsans is UNDERTALE's
# (the same ink and metrics; sansfont round 3), so it is packed for both.
# fnt_maintext (key 1): UNDERTALE's ending credits and small 1x text, our PC-height remake (sansfont 10-26). The runtime
# takes at most six fonts a pack (z_smalltext.c ST_FONTS); UNDERTALE's pack holds six with it.
UT_ONLY = ('fnt_maintext',)
# DELTARUNE's fnt_main (1x: the save menu, choices, small-face text) has UNDERTALE's fnt_maintext ink and metrics
# (chapters 2-5 byte for byte, chapter 1 the same ink in taller glyph boxes; drfonts 10-27), so its pack entry
# keeps its own name and key 1 and takes fnt_maintext's parameters and corrections (the approved PC-height remake).
DR_LIKE = {'fnt_main': 'fnt_maintext'}
VARIANT_FIX = {'W': 1, '*': 2}
VARIANT_KR = 3
OUT_EXTS = ('.dtst', '.dtsm', '.dtsb')  # the packs, and UNDERTALE's Smoothing menu data and bubble table (stmenu.py)
KR_SPRITE = 'spr_krmeter'  # UNDERTALE's KR label (23x10, white letters on black)


def kr_label(dw):
    """UNDERTALE's KR label for Crisp (round 4b: the KR text was blurry): the KR meter sprite's
    frame 0 (23x10) at its even rows and columns, the texels a whole-pixel draw at an even place shows: 5 rows of 12,
    or None when the sprite isn't there or isn't that shape."""
    for p in dw.plist('SPRT'):
        if not p or dw.string(dw.u32(p)) != KR_SPRITE:
            continue
        w, h = dw.u32(p + 4), dw.u32(p + 8)
        if (w, h) != (23, 10):
            return None
        t = dw.tpag(dw.u32(p + 60))
        texels = dw.region(t['page'], t['sx'], t['sy'], t['sw'], t['sh'])
        a = [[(0, 0, 0, 0)] * w for _ in range(h)]
        for y, row in enumerate(texels):
            for x, px in enumerate(row):
                a[t['ty'] + y][t['tx'] + x] = px
        white = [[px[3] > 127 and min(px[:3]) > 127 for px in row] for row in a]
        return [row[0::2] for row in white[0::2]]
    return None


def font_glyphs(dw, name, like=None):
    font = dw.fonts()[name]
    glyphs = {ch: g for ch, g in font['glyphs'].items() if 32 <= ch < 127}
    cell_h = max(g['h'] for g in glyphs.values())
    adv = max(g['shift'] for g in glyphs.values())
    rows = (cell_h + 1) // 2
    base = sg.derive(dw, name, adv / 2, rows, None, like=like)
    fixed = sg.derive(dw, name, adv / 2, rows, None, like=like, patches=True)
    pc = {}
    for ch in glyphs:
        a, off = sg.cell_bitmap(dw, font, ch, cell_h)
        pc[ch] = sg.extents(a, off)
    out = []
    for ch, (g, left) in sorted(base['glyphs'].items()):
        out.append((ch, 0, g, left, pc[ch]))
    for c, variant in VARIANT_FIX.items():
        # a font whose corrections are on by default (fnt_comicsans) has them in its derived glyphs already
        if c in fixed['patched'] and c not in base['patched']:
            g, left = fixed['glyphs'][ord(c)]
            out.append((ord(c), variant, g, left, pc[ord(c)]))
    if name == 'fnt_curs':
        kr = kr_label(dw)
        if kr is not None and len(kr) <= rows:
            out.append((0, VARIANT_KR, kr, 0, [sg.NONE] * cell_h))
    return rows, out, cell_h, adv


def pack(path, quiet=False, game=None, keep=None):
    """(pack bytes, font names); keep (a dict) gets each font's font_glyphs result (stmenu.py's previews use them)."""
    dw = sg.GameData(path)
    # game None (a data.win given by path): DELTARUNE is the data.win with fnt_dotumche
    deltarune = game == 'deltarune' if game else 'fnt_dotumche' in dw.fonts()
    names = [n for n in FONT_KEYS if n in dw.fonts() and not (deltarune and n in UT_ONLY)]
    w = dw.win
    out = bytearray(MAGIC + struct.pack('<IIIQI', VERSION, w.size, w.game_id, w.timestamp, len(names)))
    import stasrtext
    stacked = None if deltarune else stasrtext.source(path, dw)
    stacked_rule = None
    for name in names:
        rows, glyphs, pc_rows, adv = font_glyphs(dw, name, DR_LIKE.get(name) if deltarune else None)
        if keep is not None:
            keep[name] = (rows, glyphs, pc_rows, adv)
        # Additional entries are ignored by the default lookup and menu preview.
        # Only an exact match of this private pack's draw text selects them.
        shift = sg.row_shifts(glyphs)
        if name == 'fnt_plain' and stacked is not None:
            extra, stacked_rule = stasrtext.variants(dw, glyphs, rows, adv, stacked[1])
            glyphs = glyphs + extra
        width = max((sg.width(g) for _, _, g, _, _ in glyphs), default=0)
        if width > 16 or rows > 16 or len(glyphs) > 255 or pc_rows > 32 or adv > 255:
            raise SystemExit(f'smalltext: {name}: {width}x{rows} glyphs ({len(glyphs)}) do not fit the pack')
        out += name.encode('ascii')[:23].ljust(24, b'\0') + struct.pack('<BBBBBBbb', FONT_KEYS[name], rows, width,
                                                                          len(glyphs), pc_rows, adv, shift[0], shift[1])
        for ch, variant, g, left, ext in glyphs:
            h, gw = len(g), sg.width(g)
            ph = sg.phase(ext, left, g)
            if not -128 <= ph <= 127:
                raise SystemExit(f'smalltext: {name}: {chr(ch)!r} phase {ph} does not fit the pack')
            out += struct.pack('<HBbBb', ch, gw, left, variant, ph)
            for y in range(rows):
                m = 0
                if y < h:
                    for x in range(gw):
                        if g[y][x]:
                            m |= 1 << x
                out += struct.pack('<H', m)
            for lo, hi in ext:
                if not (-128 <= lo <= 127 and -128 <= hi <= 127):
                    raise SystemExit(f'smalltext: {name}: {chr(ch)!r} PC extents {lo}..{hi} do not fit the pack')
                out += struct.pack('<bb', lo, hi)
        if not quiet:
            fixes = ''.join(chr(ch) for ch, v, _, _, _ in glyphs if v in VARIANT_FIX.values())
            kr = ', KR label' if any(v == VARIANT_KR for _, v, _, _, _ in glyphs) else ''
            print(f'smalltext: {name}: {len(glyphs)} glyphs, {rows} rows, cell {width}, '
                  f'row shift {shift[0]}/{shift[1]}, corrections {fixes or "none"}{kr}')
    if stacked_rule is not None:
        out += stasrtext.trailer(stacked[0], stacked[1], stacked_rule, stasrtext.motion(path, dw))
    return bytes(out), names


def generator_stamp():
    """The generator lines of build()'s cache stamp: these generators' sources, and the font tables as stglyphs merged
    them, with the optional tools/analysis/stglyphs_extra.py (present in a repository checkout, absent from the
    release export) named by its contents or as absent. A change to any of them repacks (gptsmoothux2rev R4: the
    split-out tables were outside the stamp, so an edit there kept a stale pack)."""
    here = os.path.dirname(os.path.abspath(__file__))
    gen = hashlib.sha1()
    for f in ('stpack.py', 'stglyphs.py', 'fontverdict.py', 'stmenu.py', 'stasrtext.py'):
        with open(os.path.join(here, f), 'rb') as fh:
            gen.update(fh.read())
    tables = repr((sorted(sg.DEFAULT_PARAMS.items()), sorted(sg.FONT_PARAMS.items()), sorted(sg.PATCHES.items()),
                   sorted(sg.PATCHES_PC.items())))
    if os.path.isfile(sg._EXTRA):
        with open(sg._EXTRA, 'rb') as fh:
            extra = hashlib.sha1(fh.read()).hexdigest()
    else:
        extra = 'absent'
    return ['generator ' + gen.hexdigest(), 'tables ' + hashlib.sha1(tables.encode()).hexdigest(), 'extra ' + extra]


def build(game, data, romfs):
    """build.sh: one pack per data.win of the game into <romfs>/fonts/<name>.dtst (undertale, chapterN_windows), and
    for UNDERTALE the Smoothing menu's data and the bubble table beside it (<name>.dtsm, <name>.dtsb, stmenu.py). Skipped when the inputs and these
    generators are unchanged since the last build."""
    import corpus
    import prepare
    found = prepare.find_data(game, data)
    out_dir = os.path.join(romfs, 'fonts')
    os.makedirs(out_dir, exist_ok=True)
    # Beside the RomFS folder, so that it isn't packaged.
    stamp_path = os.path.join(os.path.dirname(os.path.abspath(romfs)), 'smalltext-inputs.txt')
    if not found:
        for old in os.listdir(out_dir):
            if old.endswith(OUT_EXTS):
                os.remove(os.path.join(out_dir, old))
        if os.path.isfile(stamp_path):
            os.remove(stamp_path)
        print(f'smalltext: no {game.upper()} game data found; Crisp text has no glyphs')
        return 0
    wins = [('undertale' if game == 'undertale' else key, p) for key, p, _ in corpus.parts(game, found)]
    stamp = generator_stamp() + ['game ' + game]
    for name, p in wins:
        st = os.stat(p)
        stamp.append(f'{name} {st.st_size} {st.st_mtime_ns}')
    try:
        with open(stamp_path, encoding='utf-8') as f:
            listed = f.read().splitlines()
        same = listed[:len(stamp)] == stamp and all(os.path.isfile(os.path.join(out_dir, n))
                                                     for n in listed[len(stamp):])
        same = same and sorted(o for o in os.listdir(out_dir) if o.endswith(OUT_EXTS)) == sorted(listed[len(stamp):])
    except (OSError, IndexError):
        same = False
    if same:
        print(f'smalltext: packs up to date in {out_dir}')
        return 0
    for old in os.listdir(out_dir):
        if old.endswith(OUT_EXTS):
            os.remove(os.path.join(out_dir, old))
    t0 = time.time()
    written = []
    for name, p in wins:
        keep = {}
        blob, names = pack(p, quiet=True, game=game, keep=keep)
        if not names:
            continue
        with open(os.path.join(out_dir, name + '.dtst'), 'wb') as f:
            f.write(blob)
        written.append(name + '.dtst')
        print(f'smalltext: {name}.dtst ({len(blob)} B): {", ".join(names)}')
        if game == 'undertale':
            import stmenu
            menu = stmenu.menu_file(p, keep, quiet=True)
            with open(os.path.join(out_dir, name + '.dtsm'), 'wb') as f:
                f.write(menu)
            written.append(name + '.dtsm')
            print(f'smalltext: {name}.dtsm ({len(menu)} B): souls, sharp sprites and previews for the Smoothing menu')
            bubbles = stmenu.bubble_file(p)
            with open(os.path.join(out_dir, name + '.dtsb'), 'wb') as f:
                f.write(bubbles)
            written.append(name + '.dtsb')
            print(f'smalltext: {name}.dtsb ({len(bubbles)} B): bubble edges for Crisp bubble text')
    with open(stamp_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(stamp + written) + '\n')
    print(f'smalltext: {len(written)} packs in {time.time() - t0:.1f} s')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('data_win', nargs='?')
    ap.add_argument('--out')
    ap.add_argument('--game', choices=('undertale', 'deltarune'))
    ap.add_argument('--romfs')
    ap.add_argument('--data')
    a = ap.parse_args()
    if a.game:
        if not a.romfs:
            ap.error('--game needs --romfs')
        return build(a.game, a.data, a.romfs)
    if not a.data_win or not a.out:
        ap.error('give a data.win and --out, or --game and --romfs')
    blob, _ = pack(a.data_win)
    with open(a.out, 'wb') as f:
        f.write(blob)
    print(f'smalltext: wrote {a.out} ({len(blob)} B)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
