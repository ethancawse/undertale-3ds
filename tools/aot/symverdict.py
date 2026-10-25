"""Which texture page items symmetric halving would only read to draw plain, from a data.win.

Usage: py -3 -I tools/aot/symverdict.py <data.win> [--out <file>] [--items N,N,...] [--jobs N] [--game deltarune]
       (build.sh writes the table through tools/aot/fontverdict.py's build mode, beside the font tables: UNDERTALE,
       and DELTARUNE on New 3DS builds.)

Without smoothing in full, the renderer draws a sprite at 1:1 that is mirror-symmetric about its middle from a copy of
half its width (ctr_renderer.c, "Symmetric halving"). Whether an item is such a sprite is looked at once, at its first
draw that way (symCopy): every texel of the item is read through symTexel, then compared. Nearly every item isn't,
and the read alone costs the game thread its size in texels: UNDERTALE's battle target (546x115 texels) took
18.3 ms at the first FIGHT of every battle on Azahar's Old 3DS, each battle button 1.4 ms at the first battle frame
(utfirstuse census). This tool repeats symCopy's look on the texels as the console stores them (fontverdict.py's page
decode, tile grid and format choice), for both colour modes, so the console can mark the items it would draw plain
before it draws them.

An item's bit is 1 when symCopy would leave it plain in both colour modes, whatever happens on the console: its size or
page rules it out (symCopy's first checks), or its tiles hold no texel, or its tiles' formats differ, or an opaque
format holds a transparent texel, or its texels aren't mirror-symmetric, or they are only in 2x columns. 0 leaves the
item to the console, as before: those it would halve (in either colour mode), and those this tool can't look at (a
page it can't decode, an item past its page's edge). The runtime (source/z_symverdict.c) marks the bit-1 items plain
(SYM_STATE_PLAIN) in the renderer's symState before they are drawn, which is what symCopy itself would have stored.

Output file (little-endian): magic b"DTSY", u32 version 1, u32 data.win size, u32 GEN8 game ID, u64 GEN8 timestamp
(CtrPackKey), u32 item count (the TPAG chunk's), then (count + 7) / 8 bytes, item i's bit at byte i / 8, bit i % 8.

Pure Python (the builder ships the embeddable Python, without numpy). Reads the game data; run with -I. Writes nothing
derived from the game into the repository: --out is a build output.
"""
import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script's folder off the path
import fontverdict as fv  # noqa: E402

MAGIC = b"DTSY"
VERSION = 1
# DELTARUNE sprites whose items the console changes before symCopy looks at them, so the data's texels or size are not
# the ones it reads; their frames are left to the console (bit 0). By the SHA-1 of the sprite's name, as
# tools/aot/itemverdict.py names them. The attack-bar PRESS: ctr_pixfix_press.c redraws its word at each tile's flush.
# The HP label: ctr_pixfix_hpname.c narrows its items when the renderer is made. UNDERTALE builds have neither file.
CONSOLE_CHANGED_SPRITES = {"deltarune": ("8030f9bdac6a63cd4b7a5c69127d3e912f859ab8",
                                         "8b4454af4e05cff77884432710f7504635638b0f")}
# What symCopy would do with an item in one colour mode.
PLAIN, HALVED = "plain", "halved"


def page_dims(blob):
    """A texture page's (width, height) from its header, or None for a format the console doesn't decode either."""
    if blob[:4] == b"\x89PNG" and blob[12:16] == b"IHDR":
        return struct.unpack_from(">II", blob, 16)
    if blob[:4] in (b"fioq", b"2zoq"):
        return struct.unpack_from("<HH", blob, 4)
    return None


def tpag_items(win):
    """Each TPAG item: (source x, y, w, h, target w, h, page), or None for an empty slot (parseTexturePageItem)."""
    out = []
    for p in win.plist("TPAG"):
        if not p:
            out.append(None)
            continue
        out.append((win.u16(p), win.u16(p + 2), win.u16(p + 4), win.u16(p + 6), win.u16(p + 12), win.u16(p + 14),
                    win.i16(p + 20)))
    return out


def sprite_frames(win, hashes):
    """The TPAG indices of every frame of the sprites whose names have these SHA-1s (itemverdict.py's reading)."""
    import hashlib
    index = {p: i for i, p in enumerate(win.plist("TPAG")) if p}
    out = set()
    for p in win.plist("SPRT"):
        name_ptr = win.u32(p) if p else 0
        if not name_ptr or name_ptr >= win.size:
            continue
        if hashlib.sha1(win.d[name_ptr:win.d.index(0, name_ptr)]).hexdigest() not in hashes:
            continue
        # The texture list: a count, then that many TPAG item pointers, after a header whose length depends on the
        # GameMaker version; the first such run in the entry.
        for k in range(p + 4, p + 256, 4):
            n = win.u32(k)
            if 0 < n <= 256 and all(win.u32(k + 4 + 4 * f) in index for f in range(n)):
                out.update(index[win.u32(k + 4 + 4 * f)] for f in range(n))
                break
    return out


def ruled_out(item):
    """symCopy's first checks that need no texels: true when it leaves the item plain for them."""
    sx, sy, w, h, tw, th, page = item
    return 0 > page or w != tw or h != th or (w & 1) != 0 or 4 > w


def look(page, exact, sx, sy, w, h):
    """symCopy on the item's texels in one colour mode: PLAIN or HALVED (when symMakeCopy has the memory)."""
    fmt = None
    for ty in range(sy // page.th, (sy + h - 1) // page.th + 1):
        for tx in range(sx // page.tw, (sx + w - 1) // page.tw + 1):
            f = page.format(exact, tx, ty)
            if f == fv.EMPTY:
                continue
            if fmt is None:
                fmt = f
            elif f != fmt:
                return PLAIN  # the formats mix: symRead can't read it, so every draw is plain
    if fmt is None:
        return PLAIN  # no texel: no buffer, plain for good
    opaque = fmt in (fv.RGB8, fv.RGB565)
    rows = []
    for y in range(h):
        row = [page.texel(exact, sx + x, sy + y) for x in range(w)]
        # symWanted's mirror test, a row at a time (the outcome is the same as comparing them all after the read).
        for x in range(w // 2):
            if row[x] != row[w - 1 - x]:
                return PLAIN
        rows.append(row)
    if opaque and any(v == 0 for row in rows for v in row):
        return PLAIN  # an opaque format can't hold an empty tile's transparent texels
    visible = any(v != 0 for row in rows for v in row[:w // 2])
    doubled = all(row[x] == row[x + 1] for row in rows for x in range(0, w, 2))
    return HALVED if visible and not doubled else PLAIN


def page_job(path, page_id, off, size, items):
    """One page's items: (page_id, [indices left plain in both modes] or None if undecodable). Reads the page from the
    file itself, so it runs in a worker process as well."""
    with open(path, "rb") as f:
        f.seek(off)
        blob = f.read(size)
    dims = page_dims(blob)
    dec = fv.decode_page(blob, dims[1]) if dims is not None else None
    if dec is None:
        return page_id, None
    if (dec[0], dec[1]) != tuple(dims):
        raise SystemExit(f"symverdict: page {page_id} header says {dims}, decodes to {dec[0]}x{dec[1]}")
    page = fv.Page(dec[0], dec[1], dec[2], dec[1])
    plain = []
    for i, (sx, sy, w, h) in items:
        if sx + w > page.w or sy + h > page.h:
            continue  # past the page's edge: left to the console
        if look(page, False, sx, sy, w, h) == PLAIN and look(page, True, sx, sy, w, h) == PLAIN:
            plain.append(i)
    return page_id, plain


def default_jobs():
    return max(1, min(8, os.cpu_count() or 1))


def analyse(path, log=print, only=None, jobs=None, game=None):
    """(win, bits): one bool per TPAG item, true where symCopy would leave it plain in both colour modes. Pages go to
    `jobs` worker processes (default: up to 8, one per CPU); the result doesn't depend on how many. `game` "deltarune"
    leaves its console-changed sprites' frames to the console."""
    win = fv.Win(path)
    blobs = fv.texture_blobs(win)
    items = tpag_items(win)
    bits = [False] * len(items)
    changed = sprite_frames(win, CONSOLE_CHANGED_SPRITES[game]) if game in CONSOLE_CHANGED_SPRITES else set()
    by_page = {}
    for i, it in enumerate(items):
        if it is None or (only is not None and i not in only) or i in changed:
            continue
        if ruled_out(it):
            bits[i] = True
            continue
        if it[6] >= len(blobs) or blobs[it[6]] is None:
            continue
        by_page.setdefault(it[6], []).append((i, it[:4]))
    work = [(path, page_id) + blobs[page_id] + (idx,) for page_id, idx in sorted(by_page.items())]
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
            log(f"symverdict: worker processes failed ({type(e).__name__}: {e}); one process instead")
    if results is None:
        results = [page_job(*w) for w in work]
    for page_id, plain in results:
        for i in plain or ():
            bits[i] = True
    return win, bits


def pack(win, bits):
    out = bytearray(MAGIC + struct.pack("<IIIQI", VERSION, win.size, win.game_id, win.timestamp, len(bits)))
    packed = bytearray((len(bits) + 7) // 8)
    for i, b in enumerate(bits):
        if b:
            packed[i >> 3] |= 1 << (i & 7)
    return bytes(out + packed)


def write(path, win, bits):
    with open(path, "wb") as f:
        f.write(pack(win, bits))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("data_win")
    ap.add_argument("--out")
    ap.add_argument("--items", help="only these TPAG indices (comma separated), for checks")
    ap.add_argument("--jobs", type=int, help=f"worker processes (default {default_jobs()}; 1 = none)")
    ap.add_argument("--game", choices=("undertale", "deltarune"), help="the build's game (DELTARUNE: its console-changed sprites)")
    args = ap.parse_args()
    only = {int(x) for x in args.items.split(",")} if args.items else None
    t0 = time.time()
    win, bits = analyse(args.data_win, only=only, jobs=args.jobs, game=args.game)
    if args.out:
        write(args.out, win, bits)
    print(f"symverdict: {sum(bits)} of {len(bits)} items plain in both colour modes, in {time.time() - t0:.1f} s")
    if only is not None:
        for i in sorted(only):
            print(f"item {i}: {'plain' if bits[i] else 'left to the console'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
