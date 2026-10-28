"""Precompute the renderer's per-item Smart smoothing verdicts from a data.win (smartcut, the first battle frame).

Usage: py -3 -I tools/aot/itemverdict.py <data.win> [--out <file>] [--items N,N,...] [--quiet]
       (build.sh writes the tables through tools/aot/fontverdict.py's build mode, beside the font tables.)

The 3DS renderer looks at every texture page item once, at its first draw under Smart smoothing (ctr_renderer.c's
itemVerdict): whether its art is in 2x2 blocks (smartTally, smartBestPhase, smartVerdict), else for items 8 to 96
texels a side whether its ink of at most 3 colours survives the shrink at some phase (smartInk: line art, placed by
that phase), else whether it is fine detail (kept sharp if it is a frame of the HP label, smartKeepSharp), and then
whether it averages clean (smartRegionClean). On the Old 3DS this costs several milliseconds an item at its first
sight; UNDERTALE's first battle frame looks at 8 items (about 11 ms on Azahar, as measured). This tool repeats
the analysis on the texels as the console stores them (tools/aot/fontverdict.py's page decode, tile grid and format
choice, for both colour modes), so the console reads the verdicts once per data.win instead.

Output file (little-endian): magic b"DTIV", u32 version 1, u32 data.win size, u32 GEN8 game ID, u64 GEN8 timestamp
(CtrPackKey), u32 item count (the TPAG chunk's), then 10 bytes per item: with rounded colours the item state byte
(ITEM_* bits as itemVerdict stores them; 0 = not known here, looked at on the console as before) and the ink box
(x0, y0, x1, y1 as smartInk leaves it, 0 0 0 0 where it doesn't look), then the same with exact colours.

Pure Python (the builder ships the embeddable Python, without numpy). Reads the game data; run with -I. Writes nothing
derived from the game into the repository: --out is a build output.
"""
import argparse
import hashlib
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script's folder off the path
import fontverdict as fv  # noqa: E402

MAGIC = b"DTIV"
VERSION = 1
ITEM_KNOWN, ITEM_FINE, ITEM_CLEAN, ITEM_BLACKEN, ITEM_PHASED = 0x80, 0x40, 0x20, 0x10, 0x04
SMART_ITEM_MIN, SMART_ITEM_MAX, SMART_COLOURS_MAX = 8, 96, 3
SMART_KEEP_SHARP_MAX = 8
# ctr_renderer.c's smartKeepSharpSprites (DELTARUNE's HP label), by the SHA-1 of the sprite's name.
KEEP_SHARP_SPRITES = ("8b4454af4e05cff77884432710f7504635638b0f",)
# Sprites whose texels the console rewrites as their tiles load, so the data's texels are not the ones itemVerdict
# looks at: DELTARUNE's attack-bar PRESS (ctr_pixfix_press.c, DELTARUNE builds, redraws its word in 2x2 blocks at
# each tile's flush; from the data the word is fine, on the console coarse). By the SHA-1 of the sprite's name, as
# ctr_pixfix_press.c finds it by name; its pages are analysed with the word rewritten the same way (press_words,
# RewrittenPage), so the table holds what the console works out after the rewrite.
REWRITTEN_SPRITES = ("8030f9bdac6a63cd4b7a5c69127d3e912f859ab8",)
# ctr_pixfix_press.c: the sprite's size, the word's first texel in it, its 32x10 texel box, at most PIXPRESS_MAX
# frames, and pixpressRows (one row of native pixels each, bit 15 the leftmost; each pixel a 2x2 texel block).
PRESS_SIZE, PRESS_AT, PRESS_BOX, PRESS_MAX = (75, 38), (42, 6), (32, 10), 4
PRESS_ROWS = (0xEEDB, 0xAA92, 0xEEDB, 0x8C89, 0x8ADB)
# White is all ones and black the alpha bits alone, as each tile format stores them (pixpressSize, pixpressBlack).
PRESS_WHITE = {fv.RGBA8: 0xFFFFFFFF, fv.RGBA5551: 0xFFFF, fv.RGB565: 0xFFFF, fv.RGB8: 0xFFFFFF}
PRESS_BLACK = {fv.RGBA8: 0x000000FF, fv.RGBA5551: 0x0001, fv.RGB565: 0, fv.RGB8: 0}
# Sprites whose items the console narrows when the renderer is initialised (ctr_pixfix_hpname.c, DELTARUNE builds):
# DELTARUNE's HP label, each frame laid out as shipped (16 wide, 8 or 11 tall, at the target's origin, unscaled) goes
# to its 14x8 core one column in. itemVerdict looks at the narrowed item, so the table does too. UNDERTALE's label
# (23x10) never matches.
NARROWED_SPRITES = ("8b4454af4e05cff77884432710f7504635638b0f",)
# Sprites the console keeps sharp before itemVerdict sees them (ctr_krsharp.c, UNDERTALE builds only): UNDERTALE's KR
# label, whose frames (the first KRSHARP_MAX) get ITEM_KNOWN alone each frame. The table says the same, so the two
# agree. By the SHA-1 of the sprite's name, as ctr_krsharp.c finds it by name.
KRSHARP_MAX = 4  # ctr_krsharp.c's KRSHARP_MAX
KRSHARP_SPRITES = ("6f1e8407773113149a7127da8aa3c6cf4c3a124d",)
# Items the console decides another way first (ctr_cmdlabel.c with ctr_renderer.c's Cmdlabel_smartMix, DELTARUNE
# builds): the battle panel's hero heads and their command emblems (sprites whose names start with the head prefix,
# by the SHA-1 of its 8 characters), each frame whose item is 8 to 96 texels a side and crosses a tile line
# (CTR_TILE_MAX), as cmdlabelMixInit marks them. smartMix reads such an item's colours one way where its tiles
# have two formats, so it can find fine (averaged) what itemVerdict, and the data, find coarse; it runs only while
# the item's state is 0. Those frames are left to the console (state 0), or a table entry would keep them sharp
# (Chapter 5's Susie emblem in the battle panel, round 16 identity).
MIX_PREFIX_SHA1, MIX_PREFIX_LEN = "8e928685bd9c5b38ce53fdc05657d3bc11a745c2", 8
MIX_MIN, MIX_MAX, CTR_TILE_MAX = 8, 96, 256  # ctr_cmdlabel.c's SMARTMIX_MIN and SMARTMIX_MAX, ctr_texture_pack.h


def tpag_items(win):
    ptrs = win.plist("TPAG")
    items = [fv.tpag_item(win, p) if p else None for p in ptrs]
    for i in sprite_frames(win, ptrs, NARROWED_SPRITES):
        it, p = items[i], ptrs[i]
        tx, ty, tw, th = win.u16(p + 8), win.u16(p + 10), win.u16(p + 12), win.u16(p + 14)
        if it["sw"] == 16 and it["sh"] in (8, 11) and tx == 0 and ty == 0 and tw == 16 and th == it["sh"] and it["page"] >= 0:
            items[i] = dict(it, sx=it["sx"] + 1, sw=14, sh=8)
    return ptrs, items


def keep_sharp_items(win, tpag_ptrs):
    """smartKeepSharp's list: the TPAG indices of every frame of the sprites it names, at most SMART_KEEP_SHARP_MAX."""
    return sprite_frames(win, tpag_ptrs, KEEP_SHARP_SPRITES, SMART_KEEP_SHARP_MAX)


def sprite_frames(win, tpag_ptrs, hashes, limit=None, prefix=None):
    """The TPAG indices of every frame of the sprites whose names have these SHA-1s (the first limit of them); with
    prefix, the SHA-1s of the names' first prefix characters."""
    index = {p: i for i, p in enumerate(tpag_ptrs) if p}
    out = []
    for p in win.plist("SPRT"):
        if not p:
            continue
        name_ptr = win.u32(p)
        if not name_ptr or name_ptr >= win.size:
            continue
        name = win.d[name_ptr:win.d.index(0, name_ptr)]
        if prefix is not None:
            name = name[:prefix] if len(name) >= prefix else b""
        if hashlib.sha1(name).hexdigest() not in hashes:
            continue
        # The sprite's texture list: a count followed by that many TPAG item pointers, after a header whose length
        # depends on the GameMaker version; the first such run in the entry.
        for k in range(p + 4, p + 256, 4):
            n = win.u32(k)
            if 0 < n <= 256 and all(win.u32(k + 4 + 4 * f) in index for f in range(n)):
                for f in range(n):
                    if limit is None or limit > len(out):
                        out.append(index[win.u32(k + 4 + 4 * f)])
                break
    return set(out)


def press_words(win, tpag_ptrs, items):
    """__wrap_CtrTextures_init's list: {page: [(x, y) of the word's top-left texel]} for the first sprite named by
    REWRITTEN_SPRITES that is PRESS_SIZE, each frame laid out unscaled with the word's box inside its item, at most
    PRESS_MAX entries, in frame order."""
    index = {p: i for i, p in enumerate(tpag_ptrs) if p}
    for p in win.plist("SPRT"):
        if not p:
            continue
        name_ptr = win.u32(p)
        if not name_ptr or name_ptr >= win.size:
            continue
        name = win.d[name_ptr:win.d.index(0, name_ptr)]
        if hashlib.sha1(name).hexdigest() not in REWRITTEN_SPRITES or (win.u32(p + 4), win.u32(p + 8)) != PRESS_SIZE:
            continue
        frames = []
        for k in range(p + 4, p + 256, 4):  # sprite_frames' texture list
            n = win.u32(k)
            if 0 < n <= 256 and all(win.u32(k + 4 + 4 * f) in index for f in range(n)):
                frames = [win.u32(k + 4 + 4 * f) for f in range(n)]
                break
        out, count = {}, 0
        for q in frames:
            if count == PRESS_MAX:
                break
            it = items[index[q]]
            tx, ty, tw, th = win.u16(q + 8), win.u16(q + 10), win.u16(q + 12), win.u16(q + 14)
            lx, ly = PRESS_AT[0] - tx, PRESS_AT[1] - ty
            if (it is None or it["page"] < 0 or it["sw"] != tw or it["sh"] != th or lx < 0 or ly < 0
                    or lx + PRESS_BOX[0] > it["sw"] or ly + PRESS_BOX[1] > it["sh"]):
                continue
            out.setdefault(it["page"], []).append((it["sx"] + lx, it["sy"] + ly))
            count += 1
        return out
    return {}


class RewrittenPage(fv.Page):
    """A page as the console holds it after pixpressTile: each tile's formats come from the data's texels (chosen
    before the flush), and inside each word box, tile by tile, the texels become the word's white and black blocks
    where every texel of the box in that tile is white or black already (else that tile is left as it is)."""

    def __init__(self, w, h, rgba, rows, words):
        super().__init__(w, h, rgba, rows)
        self.words = words
        self.over = {}  # exact -> {(x, y): white}

    def overlay(self, exact):
        over = {}
        for wx, wy in self.words:
            tiles = {}
            for y in range(PRESS_BOX[1]):
                for x in range(PRESS_BOX[0]):
                    px, py = wx + x, wy + y
                    if px < self.w and py < self.h:
                        tiles.setdefault((px // self.tw, py // self.th), []).append((x, y, px, py))
            for texels in tiles.values():
                ok = True
                for x, y, px, py in texels:
                    f, v = self.stored_as(over, exact, px, py)
                    if f == fv.EMPTY or v not in (PRESS_WHITE[f], PRESS_BLACK[f]):
                        ok = False  # not the data's PRESS: the tile is left as it is
                        break
                if ok:
                    for x, y, px, py in texels:
                        over[(px, py)] = (PRESS_ROWS[y >> 1] >> (15 - (x >> 1)) & 1) != 0
        return over

    def stored_as(self, over, exact, x, y):
        f, v = super().stored(exact, x, y)
        white = over.get((x, y))
        if white is not None:
            return f, PRESS_WHITE[f] if white else PRESS_BLACK[f]
        return f, v

    def stored(self, exact, x, y):
        over = self.over.get(exact)
        if over is None:
            over = self.over[exact] = self.overlay(exact)
        return self.stored_as(over, exact, x, y)


def ink(page, exact, x0, y0, w, h):
    """smartInk: (few, ink box or None where it doesn't write one, kept per phase)."""
    t = [page.texel(exact, x0 + i % w, y0 + i // w) for i in range(w * h)]
    colour, texels = [], []
    for c in t:
        if c in colour:
            texels[colour.index(c)] += 1
            continue
        if len(colour) == SMART_COLOURS_MAX + 1:
            return False, None, None
        colour.append(c)
        texels.append(1)
    opaque = sum(1 for c in colour if c != 0)
    most = 0
    for k in range(len(colour)):
        if texels[k] > texels[most]:
            most = k
    if opaque > SMART_COLOURS_MAX:
        return False, None, None
    common = colour[most]
    inked = 0
    lost = [0, 0, 0, 0]
    box = [w, h, 0, 0]
    for y in range(h):
        for x in range(w):
            c = t[y * w + x]
            if c == common:
                continue
            inked += 1
            box[0] = min(box[0], x)
            box[1] = min(box[1], y)
            box[2] = max(box[2], x + 1)
            box[3] = max(box[3], y + 1)
            for p in range(4):
                fx = max(x - ((x - (p & 1)) & 1), 0)
                fy = max(y - ((y - (p >> 1)) & 1), 0)
                if t[fy * w + fx] != c:
                    lost[p] += 1
    box = [max(v, 0) & 0xFF for v in box]
    kept = [lost[p] * 200 <= inked for p in range(4)]
    return inked > 0, box, kept


def in_range(item):
    return SMART_ITEM_MAX >= item["sw"] >= SMART_ITEM_MIN and SMART_ITEM_MAX >= item["sh"] >= SMART_ITEM_MIN


def verdict(page, exact, item, keep_sharp, index):
    """itemVerdict's state byte and ink box for one item (state 0 where the console wouldn't decide)."""
    x0, y0, w, h = item["sx"], item["sy"], item["sw"], item["sh"]
    # Coarse whatever its tally says (itemVerdict looks at the tally first, but both ways end in ITEM_KNOWN alone),
    # so only items 8 to 96 texels a side need their texels.
    if not in_range(item):
        return ITEM_KNOWN, None
    vis, uni = fv.tally(page, exact, x0, y0, w, h)
    p = fv.best_phase(vis, uni)
    if vis[p] > 0 and uni[p] * 100 >= vis[p] * 97:
        return ITEM_KNOWN, None
    few, box, kept = ink(page, exact, x0, y0, w, h)
    if not few:
        return ITEM_KNOWN, box
    k = 0
    while 4 > k and not kept[k]:
        k += 1
    if 4 > k:
        return ITEM_KNOWN | ITEM_PHASED | k, box
    if index in keep_sharp:
        return ITEM_KNOWN, box
    v = ITEM_KNOWN | ITEM_FINE
    clean = fv.region_clean(page, exact, x0, y0, w, h)
    if clean == 1:
        v |= ITEM_CLEAN
    elif clean == 3:
        v |= ITEM_BLACKEN
    return v, box


def page_dims(blob):
    """A texture page's (width, height) from its header, or None for a format the console doesn't decode either."""
    if blob[:4] == b"\x89PNG" and blob[12:16] == b"IHDR":
        return struct.unpack_from(">II", blob, 16)
    if blob[:4] in (b"fioq", b"2zoq"):
        return struct.unpack_from("<HH", blob, 4)
    return None


def page_job(path, page_id, off, size, look, keep_sharp, words=()):
    """One page's items 8 to 96 texels a side: (page_id, [(index, entry)] or None if undecodable, size, seconds). Reads
    the page from the file itself, so it runs in a worker process as well. words: press_words' boxes on this page."""
    t0 = time.time()
    with open(path, "rb") as f:
        f.seek(off)
        blob = f.read(size)
    dims = page_dims(blob)
    dec = fv.decode_page(blob, dims[1])
    if dec is None:
        return page_id, None, dims, time.time() - t0
    if (dec[0], dec[1]) != dims:
        raise SystemExit(f"itemverdict: page {page_id} header says {dims}, decodes to {dec[0]}x{dec[1]}")
    page = RewrittenPage(dec[0], dec[1], dec[2], dec[1], words) if words else fv.Page(dec[0], dec[1], dec[2], dec[1])
    out = []
    for i, item in look:
        entry = bytearray()
        for exact in (False, True):
            v, box = verdict(page, exact, item, keep_sharp, i)
            entry.append(v)
            entry += bytes(box) if box is not None else bytes(4)
        out.append((i, bytes(entry)))
    return page_id, out, dims, time.time() - t0


def default_jobs():
    return max(1, min(8, os.cpu_count() or 1))


def analyse(path, quiet=False, log=print, only=None, jobs=None, game=None):
    """(win, table, keep-sharp items): the table's 10-byte entry per item. Pages go to `jobs` worker processes
    (default: up to 8, one per CPU); the result doesn't depend on how many. game "undertale" adds ctr_krsharp.c's
    items (that build's console keeps them sharp); game "deltarune" leaves Cmdlabel_smartMix's items to the console
    and looks at the PRESS pages as ctr_pixfix_press.c rewrites them."""
    win = fv.Win(path)
    blobs = fv.texture_blobs(win)
    ptrs, items = tpag_items(win)
    keep_sharp = keep_sharp_items(win, ptrs)
    words = press_words(win, ptrs, items) if game == "deltarune" else {}  # ctr_pixfix_press.c: DELTARUNE builds
    table = [bytes(10)] * len(items)
    by_page = {}
    for i, it in enumerate(items):
        if it is None or it["page"] < 0 or it["page"] >= len(blobs) or blobs[it["page"]] is None:
            continue
        if only is not None and i not in only:
            continue
        by_page.setdefault(it["page"], []).append(i)
    work, inside_of = [], {}
    for page_id, idx in sorted(by_page.items()):
        off, size = blobs[page_id]
        dims = page_dims(win.d[off:off + 32])
        if dims is None:
            continue
        # Items past the page's edge get no blocks from smartTally (left to the console); the rest outside 8 to 96
        # texels a side are coarse without a look at their texels, and a page holding no others isn't decoded.
        inside = [i for i in idx
                  if dims[0] >= items[i]["sx"] + items[i]["sw"] and dims[1] >= items[i]["sy"] + items[i]["sh"]]
        for i in inside:
            if not in_range(items[i]):
                table[i] = bytes([ITEM_KNOWN, 0, 0, 0, 0, ITEM_KNOWN, 0, 0, 0, 0])
        look = [(i, items[i]) for i in inside if in_range(items[i])]
        if look:
            inside_of[page_id] = inside
            work.append((path, page_id, off, size, look, keep_sharp, tuple(words.get(page_id, ()))))
    jobs = default_jobs() if jobs is None else max(1, jobs)
    results = None
    if jobs > 1 and len(work) > 1:
        # Where worker processes can't start (a packaged Python without _multiprocessing, a blocked spawn), one
        # process does it all: the result is the same.
        try:
            import concurrent.futures
            with concurrent.futures.ProcessPoolExecutor(max_workers=min(jobs, len(work))) as pool:
                results = list(pool.map(page_job, *zip(*work)))
        except (ImportError, OSError, RuntimeError) as e:
            log(f"itemverdict: worker processes failed ({type(e).__name__}: {e}); one process instead")
    if results is None:
        results = [page_job(*w) for w in work]
    for page_id, out, dims, seconds in results:
        if out is None:
            for i in inside_of[page_id]:
                table[i] = bytes(10)  # the console can't decode it either
            continue
        for i, entry in out:
            table[i] = entry
        if not quiet:
            log(f"page {page_id}: {dims[0]}x{dims[1]}, {len(out)} of {len(by_page[page_id])} items looked at in "
                f"{seconds:.1f} s")
    if game == "undertale":
        for i in sprite_frames(win, ptrs, KRSHARP_SPRITES, KRSHARP_MAX):
            if only is None or i in only:
                table[i] = bytes([ITEM_KNOWN, 0, 0, 0, 0, ITEM_KNOWN, 0, 0, 0, 0])
    if game == "deltarune":
        for i in mix_items(win, ptrs):
            table[i] = bytes(10)  # Cmdlabel_smartMix decides these on the console
    return win, table, keep_sharp


def mix_items(win, tpag_ptrs):
    """cmdlabelMixInit's items: head-prefix sprite frames, MIX_MIN to MIX_MAX a side, across a tile line (the data's
    source box, as the console reads it before any change)."""
    out = set()
    for i in sprite_frames(win, tpag_ptrs, (MIX_PREFIX_SHA1,), prefix=MIX_PREFIX_LEN):
        it = fv.tpag_item(win, tpag_ptrs[i])
        x0, y0, w, h = it["sx"], it["sy"], it["sw"], it["sh"]
        if it["page"] < 0 or not (MIX_MIN <= w <= MIX_MAX and MIX_MIN <= h <= MIX_MAX):
            continue
        if x0 // CTR_TILE_MAX == (x0 + w - 1) // CTR_TILE_MAX and y0 // CTR_TILE_MAX == (y0 + h - 1) // CTR_TILE_MAX:
            continue
        out.add(i)
    return out


def write(path, win, table):
    out = bytearray(MAGIC + struct.pack("<IIIQI", VERSION, win.size, win.game_id, win.timestamp, len(table)))
    for e in table:
        out += e
    with open(path, "wb") as f:
        f.write(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("data_win")
    ap.add_argument("--out")
    ap.add_argument("--items", help="only these TPAG indices (comma separated), for checks")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--jobs", type=int, help=f"worker processes (default {default_jobs()}; 1 = none)")
    ap.add_argument("--game", choices=("undertale", "deltarune"), help="the build's game (UNDERTALE: the KR label)")
    args = ap.parse_args()
    only = {int(x) for x in args.items.split(",")} if args.items else None
    t0 = time.time()
    win, table, keep_sharp = analyse(args.data_win, args.quiet, only=only, jobs=args.jobs, game=args.game)
    if args.out:
        write(args.out, win, table)
    known = sum(1 for e in table if e[0])
    fine = sum(1 for e in table if e[0] & ITEM_FINE)
    print(f"itemverdict: {known} of {len(table)} items analysed ({fine} fine with rounded colours), keep-sharp items "
          f"{sorted(keep_sharp)}, in {time.time() - t0:.1f} s")
    if only is not None:
        for i in sorted(only):
            e = table[i]
            print(f"item {i}: rounded 0x{e[0]:02x} ink {list(e[1:5])}; exact 0x{e[5]:02x} ink {list(e[6:10])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
