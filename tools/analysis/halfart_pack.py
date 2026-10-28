"""Half-resolution art (prototype, flowhalf; build.sh halfart=point|box): the listed sprites' frames stored at half
resolution in their own texture page, for art never drawn above 0.5 screen pixels per texel (native drawing shows a
1x item of a 640x480 room view that way). The console then keeps a quarter of the texels for them.

    py -3 -I tools/analysis/halfart_pack.py --list tools/analysis/halfart/undertale.txt --filter point|box
        --out <patch.dthr> --remap <remap.json> [--pages <patched pages>] [--data <game folder> | --data-win <data.win>]

The list has one sprite per line (# comments): the first 16 hex digits of the SHA-256 of its name (name_hash(); a
plain name works too, for development), all its frames or, as entry:0,3,4, those frames. The entries are matched
against the player's own data.win here, at build time, so a shipped list names no game identifiers; --hash NAME prints
the entry for a name. They become candidates unless a frame's page rectangle
overlaps an item that is not one (another sprite, a background, a font, a frame that other items share), which keeps
its full size. Each page with candidates gets:
  * its candidates' full-size rectangles cleared to transparent (the tiles they emptied are then never loaded), and
  * each candidate's half-size copy placed with a transparent texel around it, packed to the top of the page
    around the items kept (MaxRects, bottom-left), so whole tiles below the copies empty.
The copy is ceil(w/2) x ceil(h/2): point keeps texel (2x, 2y), the texel native drawing samples at even positions;
box averages the 2x2 block (texels past the frame's edge count as transparent; colour weighted by alpha; a frame with
at most 2% partly transparent texels, stored 16-bit with alpha >= 128 opaque, gets binary alpha the same way: opaque
where at least 2 of the 4 are, so its tiles stay 16-bit). Tiles with kept items keep their format (see build()).
The item's source rectangle becomes the copy and its target size 2 * ceil(w/2) x 2 * ceil(h/2), the same geometry
(one texel more at an odd edge, transparent in box, the last column or row again in point).

Writes nothing from the game into the repository: the patch, remap and pages (--pages: write_pages, for the release
builder's texture preparation on the PC) go to the build folder. Exit 3: no game data.
The patch (little-endian), read by platform/3ds/source/ctr_halfart.c:
  header  'DTHR', version 1, data.win size, game ID, timestamp (u64), items, pages, ops, 0 (u32 each but the timestamp)
  items   tpag index (u32); old source x, y, w, h, target w, h; new source x, y, w, h, target w, h (u16 each)
  pages   page id, FNV-1a 32 of the first 4096 bytes of its image, first op, op count (u32 each)
  ops     kind (u16: 0 clear, 1 copy), 0 (u16), x, y, w, h (u16), data offset (u32; copy: w*h RGBA8 texels)
  data    the copies' texels, row by row
A page's clears come before its copies.
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'aot'))
sys.path.insert(0, os.path.join(HERE, '..', 'roomplan'))
import fontverdict as fv  # noqa: E402  (data.win reader and page decoders)

TILE = 256
GAP = 1
PAD = 2  # GameMaker's padding around each item


def fnv1a(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def cstr(win, p):
    return win.d[p:win.d.index(0, p)].decode('utf-8', 'replace') if p else ''


def sprites(win):
    """name -> [tpag pointer] for GameMaker 1.x sprites (SPRT)."""
    out = {}
    for sp in win.plist('SPRT'):
        if not sp:
            continue
        name = cstr(win, win.u32(sp))
        p = sp + 4 * 14
        n = win.u32(p)
        if n > 4096:
            continue
        out[name] = [win.u32(p + 4 + 4 * k) for k in range(n)]
    return out


def other_users(win):
    """TPAG pointers that backgrounds and fonts use."""
    used = set()
    for bp in win.plist('BGND'):
        if bp:
            used.add(win.u32(bp + 16))
    for p in win.plist('FONT'):
        if p and win.u32(p + 28):
            used.add(win.u32(p + 28))
    return used


def overlaps(a, b):
    return a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3]


class MaxRects:
    """Free rectangles of one tile; place() takes best short side fit."""

    def __init__(self, x0, y0, w, h, blocked):
        self.free = [(x0, y0, w, h)]
        for r in blocked:
            self._cut(r)

    def _cut(self, r):
        out = []
        for f in self.free:
            if not overlaps(f, r):
                out.append(f)
                continue
            fx, fy, fw, fh = f
            rx, ry, rw, rh = r
            if rx > fx:
                out.append((fx, fy, rx - fx, fh))
            if rx + rw < fx + fw:
                out.append((rx + rw, fy, fx + fw - rx - rw, fh))
            if ry > fy:
                out.append((fx, fy, fw, ry - fy))
            if ry + rh < fy + fh:
                out.append((fx, ry + rh, fw, fy + fh - ry - rh))
        # Drop rectangles inside others.
        out = [a for i, a in enumerate(out) if a[2] > 0 and a[3] > 0 and not any(
            j != i and b[0] <= a[0] and b[1] <= a[1] and a[0] + a[2] <= b[0] + b[2] and a[1] + a[3] <= b[1] + b[3] and (b != a or j < i)
            for j, b in enumerate(out))]
        self.free = out

    def place(self, w, h):
        best = None
        for f in self.free:
            if f[2] >= w and f[3] >= h:
                # Bottom-left: the copies gather at the top of the page, leaving whole tiles below them empty.
                score = (f[1] + h, f[0])
                if best is None or score < best[0]:
                    best = (score, f)
        if best is None:
            return None
        x, y = best[1][0], best[1][1]
        self._cut((x, y, w, h))
        return x, y


def tile_format(rows, pw, x0, y0, w, h):
    """fontverdict.Page.choose (chooseFormat), faster: the soft count only when the partial share passes."""
    alphas = bytearray()
    for y in range(y0, y0 + h):
        o = (y * pw + x0) * 4
        alphas += rows[o + 3:o + 4 * w:4]
    visible = len(alphas) - alphas.count(0)
    if visible == 0:
        return fv.EMPTY
    partial = visible - alphas.count(255)
    if fv.f32(float(partial)) > fv.f32(fv.f32(float(visible)) * fv.PARTIAL_ALPHA_TOLERANCE):
        return fv.RGBA8
    soft = 0
    if partial:
        def pa(x, y):
            return 0 < alphas[y * w + x] < 255
        for i, a in enumerate(alphas):
            if 0 < a < 255:
                x, y = i % w, i // w
                if 0 < y < h - 1 and 0 < x < w - 1 and pa(x - 1, y) and pa(x + 1, y) and pa(x, y - 1) and pa(x, y + 1):
                    soft += 1
    return fv.RGBA5551 if fv.SOFT_AREA_MIN_PIXELS > soft else fv.RGBA8


def downsample(rows, pw, rx, ry, w, h, mode):
    """RGBA8 bytes of the ceil(w/2) x ceil(h/2) copy of page rectangle (rx, ry, w, h)."""
    hw, hh = (w + 1) // 2, (h + 1) // 2
    out = bytearray(hw * hh * 4)
    if mode == 'point':
        for y in range(hh):
            src = ((ry + 2 * y) * pw + rx) * 4
            row = rows[src:src + w * 4]
            o = y * hw * 4
            for x in range(hw):
                out[o + 4 * x:o + 4 * x + 4] = row[8 * x:8 * x + 4]
        return out
    # A frame with few partly transparent texels (chooseFormat's share) sits in 16-bit tiles, which keep alpha >= 128
    # as opaque and drop the rest: its copy is made the same way, so it stays 16-bit too.
    alphas = bytearray()
    for y in range(h):
        src = ((ry + y) * pw + rx) * 4
        alphas += rows[src + 3:src + w * 4:4]
    visible = len(alphas) - alphas.count(0)
    partial = visible - alphas.count(255)
    binary = fv.f32(float(partial)) <= fv.f32(fv.f32(float(visible)) * fv.PARTIAL_ALPHA_TOLERANCE)
    for y in range(hh):
        for x in range(hw):
            r = g = b = asum = n_opaque = 0
            for dy in (0, 1):
                yy = 2 * y + dy
                if yy >= h:
                    continue
                base = ((ry + yy) * pw + rx) * 4
                for dx in (0, 1):
                    xx = 2 * x + dx
                    if xx >= w:
                        continue
                    p = base + xx * 4
                    a = rows[p + 3]
                    if binary:
                        a = 255 if a >= 128 else 0
                    if a:
                        r += rows[p] * a
                        g += rows[p + 1] * a
                        b += rows[p + 2] * a
                        asum += a
                        n_opaque += a == 255
            o = (y * hw + x) * 4
            if asum == 0:
                continue
            if binary:
                if n_opaque < 2:
                    continue
                alpha = 255
            else:
                alpha = (asum + 2) // 4
            out[o] = (r + asum // 2) // asum
            out[o + 1] = (g + asum // 2) // asum
            out[o + 2] = (b + asum // 2) // asum
            out[o + 3] = alpha
    return out


HASH_DIGITS = 16


def name_hash(name):
    """A list entry for a sprite: the first 16 hex digits of the SHA-256 of its name (UTF-8). Lists shipped with the
    release name no game identifiers; the builder matches these against the player's own data.win (resolve())."""
    return hashlib.sha256(name.encode('utf-8')).hexdigest()[:HASH_DIGITS]


def is_hash(key):
    return len(key) == HASH_DIGITS and all(c in '0123456789abcdef' for c in key)


def resolve(names, sprite_names):
    """parse_list's dict with its hash entries replaced by the data.win sprite names they match (frames merged);
    an entry that matches none stays as it is (reported missing)."""
    by_hash = {}
    for n in sprite_names:
        by_hash.setdefault(name_hash(n), n)
    out = {}
    for key, frames in names.items():
        name = by_hash.get(key, key) if is_hash(key) else key
        if name in out and (out[name] is None or frames is None):
            out[name] = None
        elif name in out:
            out[name] = out[name] | frames
        else:
            out[name] = frames
    return out


def parse_list(lines):
    """entry (a sprite name, or name_hash() of one) -> set of frame indices, or None for all of them."""
    names = {}
    for line in lines:
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        name, _, frames = line.partition(':')
        name = name.strip()
        if frames.strip():
            names[name] = (names.get(name) or set()) | {int(f) for f in frames.replace(',', ' ').split()}
        else:
            names[name] = None
    return names


def build(data_win, names, mode, log=print, pages_out=None):
    """names: parse_list's. pages_out: a dict that gets each patched page as (w, h, RGBA8 rows) too."""
    win = fv.Win(data_win)
    tpag_ptrs = win.plist('TPAG')
    index = {p: i for i, p in enumerate(tpag_ptrs)}
    items = []
    for p in tpag_ptrs:
        sx, sy, sw, sh, tx, ty, tw, th, bw, bh = struct.unpack_from('<10H', win.d, p)
        items.append(dict(sx=sx, sy=sy, sw=sw, sh=sh, tx=tx, ty=ty, tw=tw, th=th, page=win.i16(p + 20)))
    spr = sprites(win)
    names = resolve(names, spr)
    missing = [n for n in names if n not in spr]
    cand = set()
    for n, frames in names.items():
        for k, p in enumerate(spr.get(n, [])):
            if p in index and (frames is None or k in frames):
                cand.add(index[p])
    # Frames shared by sprites outside the list, backgrounds and fonts are not candidates.
    users = {}
    for n, ptrs in spr.items():
        for p in ptrs:
            if p in index:
                users.setdefault(index[p], set()).add(n)
    for p in other_users(win):
        if p in index:
            users.setdefault(index[p], set()).add('(background or font)')
    shared = {t for t in cand if any(u not in names for u in users.get(t, ()))}
    cand -= shared
    # Items scaled by GameMaker (source != target) and empty ones keep their size.
    odd = {t for t in cand if items[t]['sw'] != items[t]['tw'] or items[t]['sh'] != items[t]['th'] or items[t]['sw'] < 2 or items[t]['sh'] < 2}
    cand -= odd
    by_page = {}
    for t in cand:
        by_page.setdefault(items[t]['page'], []).append(t)
    # A candidate whose rectangle overlaps any other item's (other than an identical candidate rectangle) keeps its size.
    clash = set()
    for page, ts in by_page.items():
        rects = [(i, (it['sx'], it['sy'], it['sw'], it['sh'])) for i, it in enumerate(items) if it['page'] == page and it['sw'] and it['sh']]
        for t in ts:
            r = (items[t]['sx'], items[t]['sy'], items[t]['sw'], items[t]['sh'])
            for i, o in rects:
                if i != t and overlaps(r, o) and not (i in cand and o == r):
                    clash.add(t)
                    break
    cand -= clash
    by_page = {}
    for t in sorted(cand):
        by_page.setdefault(items[t]['page'], []).append(t)
    blobs = fv.texture_blobs(win)
    patch_items, pages, ops, data = [], [], [], bytearray()
    report = dict(missing=missing, shared=sorted(shared), scaled=sorted(odd), clash=sorted(clash), pages={})
    for page in sorted(by_page):
        ts = by_page[page]
        off, size = blobs[page]
        blob = win.d[off:off + size]
        t0 = time.time()
        decoded = fv.decode_page(blob, 1 << 30)
        if decoded is None:
            log('halfart: page %d: cannot decode; its items keep their size' % page)
            continue
        pw, ph, rows = decoded
        rects = {}
        for t in ts:
            rects.setdefault((items[t]['sx'], items[t]['sy'], items[t]['sw'], items[t]['sh']), []).append(t)
        keep = [(it['sx'], it['sy'], it['sw'], it['sh']) for i, it in enumerate(items)
                if it['page'] == page and i not in cand and it['sw'] and it['sh']]
        tiles_x = (pw + TILE - 1) // TILE
        # GameMaker repeats each item's edge texels into the padding around it: kept items keep theirs.
        keep_pad = [(k[0] - PAD, k[1] - PAD, k[2] + 2 * PAD, k[3] + 2 * PAD) for k in keep]
        # The tiles kept items lie in keep the format the texture cache picks for them (chooseFormat), so kept art is
        # stored exactly as before: a tile that would go to 16 bits gets a 10x10 block of alpha-1 texels where no item
        # samples, and copies whose alpha would make a 16-bit tile 32-bit go elsewhere.
        kept_tiles = set()
        for k in keep:
            for ty in range(max(0, k[1]) // TILE, min(ph - 1, k[1] + k[3] - 1) // TILE + 1):
                for tx in range(max(0, k[0]) // TILE, min(pw - 1, k[0] + k[2] - 1) // TILE + 1):
                    kept_tiles.add((tx, ty))

        def tile_rect(tx, ty):
            return tx * TILE, ty * TILE, min(TILE, pw - tx * TILE), min(TILE, ph - ty * TILE)
        orig_fmt = {t: tile_format(rows, pw, *tile_rect(*t)) for t in kept_tiles}
        soft = {r: tile_format(rows, pw, *r) == fv.RGBA8 for r in rects}
        no_copy = set()
        for _attempt in range(8):
            blocked = [(k[0] - GAP, k[1] - GAP, k[2] + 2 * GAP, k[3] + 2 * GAP) for k in keep_pad]
            blocked += [tile_rect(*t) for t in sorted(no_copy)]
            # Copies of soft frames (32-bit art) go first, and the copies of 16-bit art keep out of their tiles, so a
            # tile of copies doesn't turn 32-bit for a few soft neighbours.
            order = sorted(rects, key=lambda r: (not soft[r], -((r[3] + 1) // 2), -((r[2] + 1) // 2), r[1], r[0]))
            placed = {}
            packer = MaxRects(0, 0, pw, ph, blocked)
            for phase in (True, False):
                if not phase:
                    soft_tiles = set()
                    for r, (nx, ny, hw, hh) in placed.items():
                        for ty in range((ny - GAP) // TILE, (ny + hh + GAP - 1) // TILE + 1):
                            for tx in range((nx - GAP) // TILE, (nx + hw + GAP - 1) // TILE + 1):
                                soft_tiles.add((tx, ty))
                    packer = MaxRects(0, 0, pw, ph, blocked + [tile_rect(*t) for t in sorted(soft_tiles)] +
                                      [(q[0] - GAP, q[1] - GAP, q[2] + 2 * GAP, q[3] + 2 * GAP) for q in placed.values()])
                for r in order:
                    if soft[r] != phase:
                        continue
                    hw, hh = (r[2] + 1) // 2, (r[3] + 1) // 2
                    spot = packer.place(hw + 2 * GAP, hh + 2 * GAP)
                    if spot is None:
                        continue
                    placed[r] = (spot[0] + GAP, spot[1] + GAP, hw, hh)
            # The full-size rectangles and their padding are cleared, short of kept items' padding.
            clears = {}
            for r in placed:
                for d in range(PAD, -1, -1):
                    x0, y0 = max(0, r[0] - d), max(0, r[1] - d)
                    c = (x0, y0, min(pw, r[0] + r[2] + d) - x0, min(ph, r[1] + r[3] + d) - y0)
                    if d == 0 or not any(overlaps(c, k) for k in keep_pad):
                        clears[r] = c
                        break
            page_ops, page_data = [], bytearray()
            for r in sorted(placed):
                c = clears[r]
                page_ops.append((0, c[0], c[1], c[2], c[3], 0))
            for r in sorted(placed, key=lambda r: (placed[r][1], placed[r][0])):
                nx, ny, hw, hh = placed[r]
                page_ops.append((1, nx, ny, hw, hh, len(page_data)))
                page_data += downsample(rows, pw, r[0], r[1], r[2], r[3], mode)
            patched = bytearray(rows)
            for k, x, y, w, h, o in page_ops:
                for yy in range(h):
                    d = ((y + yy) * pw + x) * 4
                    patched[d:d + w * 4] = bytes(w * 4) if k == 0 else page_data[o + yy * w * 4:o + (yy + 1) * w * 4]
            to_32 = set()
            keepers = []
            for t in sorted(kept_tiles):
                now = tile_format(patched, pw, *tile_rect(*t))
                if now == orig_fmt[t]:
                    continue
                if orig_fmt[t] == fv.RGBA8:
                    keepers.append(t)
                else:
                    to_32.add(t)
            if to_32 - no_copy:
                no_copy |= to_32
                continue
            busy = keep_pad + [(p[0] - GAP, p[1] - GAP, p[2] + 2 * GAP, p[3] + 2 * GAP) for p in placed.values()]
            for t in keepers:
                tx0, ty0, tw, th = tile_rect(*t)
                spot = None
                for y in range(ty0 + 1, ty0 + th - 11, 2):
                    for x in range(tx0 + 1, tx0 + tw - 11, 2):
                        r = (x - 1, y - 1, 12, 12)
                        if not any(overlaps(r, b) for b in busy) and all(
                                patched[((yy) * pw + x - 1) * 4 + 3:((yy) * pw + x + 11) * 4:4] == bytes(12) for yy in range(y - 1, y + 11)):
                            spot = (x, y)
                            break
                    if spot:
                        break
                if spot is None:
                    log('halfart: page %d tile %d,%d: no room to keep its 32-bit format' % (page, t[0], t[1]))
                    continue
                page_ops.append((1, spot[0], spot[1], 10, 10, len(page_data)))
                page_data += bytes((0, 0, 0, 1)) * 100
                for yy in range(10):
                    d = ((spot[1] + yy) * pw + spot[0]) * 4
                    patched[d:d + 40] = bytes((0, 0, 0, 1)) * 10
                assert tile_format(patched, pw, *tile_rect(*t)) == fv.RGBA8
            for t in sorted(kept_tiles):
                assert tile_format(patched, pw, *tile_rect(*t)) == orig_fmt[t], (page, t)
            break
        else:
            raise RuntimeError('halfart: page %d: tile formats would not settle' % page)
        for r in order:
            if r not in placed:
                log('halfart: page %d: no room for %dx%d; those items keep their size' % (page, (r[2] + 1) // 2, (r[3] + 1) // 2))
        first = len(ops)
        for k, x, y, w, h, o in page_ops:
            ops.append((k, x, y, w, h, len(data) + o if k == 1 else 0))
        data += page_data
        for r in sorted(placed, key=lambda r: (placed[r][1], placed[r][0])):
            nx, ny, hw, hh = placed[r]
            for t in rects[r]:
                it = items[t]
                patch_items.append((t, it['sx'], it['sy'], it['sw'], it['sh'], it['tw'], it['th'], nx, ny, hw, hh, 2 * hw, 2 * hh))
        report.setdefault('format_kept', {})[page] = dict(keepers=len(keepers), no_copy_tiles=len(no_copy))
        tiles_before = set()
        tiles_after = set()
        for r in placed:
            for ty in range(r[1] // TILE, (r[1] + r[3] - 1) // TILE + 1):
                for tx in range(r[0] // TILE, (r[0] + r[2] - 1) // TILE + 1):
                    tiles_before.add(ty * tiles_x + tx)
            nx, ny, hw, hh = placed[r]
            for ty in range(ny // TILE, (ny + hh - 1) // TILE + 1):
                for tx in range(nx // TILE, (nx + hw - 1) // TILE + 1):
                    tiles_after.add(ty * tiles_x + tx)
        report['pages'][page] = dict(items=sum(len(rects[r]) for r in placed), rects=len(placed),
                                     tiles_touched_before=len(tiles_before), tiles_touched_after=len(tiles_after),
                                     texels_before=sum(r[2] * r[3] for r in placed),
                                     texels_after=sum(placed[r][2] * placed[r][3] for r in placed),
                                     seconds=round(time.time() - t0, 1))
        pages.append((page, fnv1a(blob[:4096]), first, len(ops) - first))
        if pages_out is not None:
            pages_out[page] = (pw, ph, patched)
        log('halfart: page %d: %d rectangles (%d items) halved, %d -> %d tiles touched, %.1f s' % (
            page, len(placed), report['pages'][page]['items'], len(tiles_before), len(tiles_after), time.time() - t0))
    patch_items.sort()
    head = struct.pack('<4sIIIQIIII', b'DTHR', 1, win.size, win.game_id, win.timestamp, len(patch_items), len(pages), len(ops), 0)
    body = b''.join(struct.pack('<I12H', *it) for it in patch_items)
    body += b''.join(struct.pack('<4I', *p) for p in pages)
    base = len(head) + len(body) + 16 * len(ops)
    body += b''.join(struct.pack('<HH4HI', k, 0, x, y, w, h, base + o if k == 1 else 0) for k, x, y, w, h, o in ops)
    remap = {str(it[0]): dict(old=list(it[1:7]), new=list(it[7:13])) for it in patch_items}
    report['items'] = len(patch_items)
    return head + body + bytes(data), remap, report


def cache_run(patch):
    """The console's texture cache folder for a build with this patch is cache_lt_<n>, n = cache_run(patch) XOR the
    settings' texture_cache_run (0 in a release): platform/3ds/CMakeLists.txt CTRPORT_HALFART_CACHE_RUN."""
    return int(hashlib.sha256(patch).hexdigest()[:8], 16) | 0x80000000


def png_rgba(w, h, rows):
    """A plain PNG of RGBA8 rows (8-bit RGBA, no interlacing, filter 0 each row): what ctr_png.c decodes."""
    import zlib
    stride = w * 4
    raw = bytearray()
    for y in range(h):
        raw += b'\0'
        raw += rows[y * stride:(y + 1) * stride]

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))


def write_pages(path, data_win, pages, patch):
    """The patched pages, for the builder's texture preparation on the PC (tools/release/builder/texcache.py, which
    reads this format): each as a plain PNG of the pixels a halfart build's CtrPng_decode wrap hands the texture
    cache, with its blob's place in data_win. texcache.py puts them in a copy of the player's data.win, each in its
    own blob's space, so the console's own texture code (texprep) writes the folder a halfart console would.
        header  'DTHP', version 1, page count (u32 each), the patch's SHA-256 (32 bytes)
        pages   page id, blob offset, blob size, PNG size (u32 each), then the PNG
    Written beside the patch, in the build folder (game pixels: never in the repository)."""
    blobs = fv.texture_blobs(fv.Win(data_win))
    out = bytearray(struct.pack('<4sII', b'DTHP', 1, len(pages)) + hashlib.sha256(patch).digest())
    for page in sorted(pages):
        pw, ph, rows = pages[page]
        off, size = blobs[page]
        png = png_rgba(pw, ph, rows)
        out += struct.pack('<4I', page, off, size, len(png)) + png
    with open(path + '.tmp', 'wb') as f:
        f.write(out)
    os.replace(path + '.tmp', path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--list')
    ap.add_argument('--filter', choices=('point', 'box'))
    ap.add_argument('--out')
    ap.add_argument('--remap')
    ap.add_argument('--hash', nargs='+', metavar='NAME', help='print the list entries for these sprite names and stop')
    ap.add_argument('--report')
    ap.add_argument('--pages', help='also write the patched pages for texture preparation on the PC (write_pages)')
    ap.add_argument('--game', default='undertale')
    ap.add_argument('--data')
    ap.add_argument('--data-win')
    a = ap.parse_args()
    if a.hash:
        for n in a.hash:
            print(name_hash(n))
        return 0
    if not (a.list and a.filter and a.out and a.remap):
        ap.error('--list, --filter, --out and --remap are required')
    path = a.data_win
    if path is None:
        import prepare
        folder = prepare.find_data(a.game, a.data)
        path = os.path.join(folder, 'data.win') if folder else None
    if not path or not os.path.isfile(path):
        print('halfart: no %s data.win found' % a.game)
        return 3
    with open(a.list, encoding='utf-8') as f:
        names = parse_list(f)
    st = os.stat(path)
    with open(__file__, 'rb') as f:
        stamp = '%s %d %d %s %s %s\n' % (path, st.st_size, st.st_mtime_ns, a.filter, hashlib.sha1(f.read()).hexdigest(),
                                         hashlib.sha1(repr(sorted((n, sorted(v) if v else None) for n, v in names.items())).encode()).hexdigest())
    stamp_path = a.out + '.inputs'
    try:
        if (open(stamp_path, encoding='utf-8').read() == stamp and os.path.isfile(a.out) and os.path.isfile(a.remap) and
                (not a.pages or os.path.isfile(a.pages))):
            print('halfart: %s up to date' % a.out)
            return 0
    except OSError:
        pass
    t0 = time.time()
    pages = {} if a.pages else None
    patch, remap, report = build(path, names, a.filter, pages_out=pages)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    for p, content, mode in ((a.out, patch, 'wb'), (a.remap, json.dumps(remap), 'w')):
        with open(p + '.tmp', mode) as f:
            f.write(content)
        os.replace(p + '.tmp', p)
    if a.pages:
        write_pages(a.pages, path, pages, patch)
    if a.report:
        with open(a.report, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=1)
    with open(stamp_path, 'w', encoding='utf-8') as f:
        f.write(stamp)
    print('halfart: %d items halved (%s), %d bytes, %.1f s; not halved: %d shared, %d scaled, %d overlapping, %d names missing' % (
        report['items'], a.filter, len(patch), time.time() - t0, len(report['shared']), len(report['scaled']), len(report['clash']),
        len(report['missing'])))
    return 0


if __name__ == '__main__':
    sys.exit(main())
