"""Crisp text glyphs (smalltext): the fixed derivation of native glyphs from a GameMaker font, in plain Python.

This is the release builder's copy of tools/analysis/smalltext_derive.py (numpy; the analysis and sheet tool)
and of the parts of tools/analysis/smalltext_carry.py the pack needs. It gives the same glyphs bit for bit, so the pack
it writes (tools/aot/stpack.py) is byte-identical to tools/analysis/smalltext_pack.py's; the per-font parameters and
the approved corrections live here, and smalltext_derive.py imports them, so both read one table.

The algorithm (smalltext_derive.py's docstring has the long form): each glyph's ink from the player's own data.win
(alpha > 127, in its font cell), unbolded for GDI-bold fonts, split into one shared row map per font and a column map
per glyph, a target pixel inked by block coverage with stroke rescue, counters kept closed, two PC-distinct glyphs never
sharing one bitmap, then the corrections the font has approved. No glyph is drawn by hand or by a model.

Bitmaps are lists of rows of bools. Where numpy's float sums decide a choice (best_cuts' lost gap weight), the sum is
numpy's pairwise summation (pairwise_sum), so ties and near-ties break the same way.

Pure Python (the builder ships the embeddable Python, without numpy). Reads the game data; run with -I. Nothing derived
from the game is written anywhere by this module.
"""
import itertools
import os
import struct
import sys
from bisect import bisect_left, bisect_right

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script's folder off the path
import fontverdict  # noqa: E402

# ---- per-font parameters and corrections (smalltext_derive.py documents each) ----

# Per-font parameters. `unbold`: undo GDI emboldening. `merge`: how a target pixel is decided from its source block,
# 'cover' (coverage threshold plus stroke rescue, the default) or 'or' (any ink, the first round's rule). `cover`: the
# area threshold, strict. `colmap`: 'glyph' (each glyph picks its own column groups) or 'font' (one column map for the
# whole font from the pen position, for pixel fonts drawn at a fixed scale, so a stroke has the same width in every
# glyph). `pixel`: the integer scale of a pixel font (0: not one); its font-pixel boundaries are forced cuts.
# rowfit: 'fill' lets the row map spend the cell's empty rows on the ink (glyphs come out taller than drawn when the
# font has empty rows above its ink, as Comic Sans and Papyrus do), 'scale' keeps the drawn size (row_map).
# 'uniform' keeps the drawn size by an even split: every target row is the same number of source rows (1 / scale,
# so 2 at half size), at the phase that loses the least structure over the whole font (uniform_cuts); a decorative
# font's letters then keep their proportions, where the optimizer of 'scale' squeezes some rows and not others.
# 'pc' (round 5, a prototype for a glyph-height question): the even split of 'uniform' at the phase that puts a cut on
# the cap top (H's first ink row), so every letter is as tall as PC draws it at half size (fnt_plain caps 6 native
# rows for PC's 12 px, where 'fill' gives 7); the columns are unchanged, so widths, spacing and carry stay as they are.
# colfit: 'fit' (each glyph's columns split by the optimizer into its rounded width, the default) or 'uniform' (the
# same even split as rowfit 'uniform', per glyph, at its own best phase).
# holes: keep each enclosed counter of the source (O, A, B) closed in the derived glyph (keep_holes): an uninked
# block that closes a leaking counter is inked, the most covered first.
# boldthin (with unbold): derive the glyph twice, from the bold ink and from the unbolded ink, and keep the bold one
# thinned toward the other (guided_thin): a bold pixel the unbolded glyph lacks goes unless it holds the glyph
# together, closes a counter or ends a stroke. At 2:1 an unbolded curve's diagonal steps cover a quarter of their
# blocks and vanish (Papyrus' O came out open at the bottom); the bold curve covers half and stays.
# Round 4 (from a review of the touch sheet: the capital B was missing a pixel): `holes` is on for
# every font, so a counter the PC glyph encloses stays enclosed (B, D, P, R, Q, p, q lost a pixel of their bowl's
# rounded corner, which opened the counter).
# distinct (round 4): two glyphs that differ on PC never share one Crisp bitmap (distinct_pass).
# patches: apply PATCHES (below). Off by default: each correction waits for approval by eye.
# overshoot: a glyph whose ink passes the cap top or the baseline (from 'H') by less than one target row, as a round
# letter's or Papyrus' E's bottom bar does by one source pixel, has that overshoot folded onto the cap top or baseline
# row (fold_overshoot), so it lines up with its neighbours instead of gaining a target row of its own.
DEFAULT_PARAMS = dict(unbold=False, merge='cover', cover=0.5, colmap='glyph', pixel=0, rowfit='fill', colfit='fit',
                      holes=True, boldthin=False, overshoot=False, gap=False, distinct=True, patches=False)
FONT_PARAMS = {
    'fnt_plain': dict(unbold=True),
    # Round 3 (the O and W of the fill derivation looked odd): the drawn size by even 2:1 rows and
    # columns, counters kept closed.
    'fnt_papyrus': dict(unbold=True, rowfit='uniform', colfit='uniform', holes=True, boldthin=True, overshoot=True),
    'fnt_small': dict(unbold=True),
    # UNDERTALE's battle HUD (name, LV, HP numbers): a 4x5 pixel font stored at 3x, so one font pixel is 1.5 native px.
    'fnt_curs': dict(unbold=False, colmap='font', pixel=3),
}

# Proposed minimal corrections to derived Sharp glyphs, for glyphs that still fail after the algorithm. Instructions
# only, never glyph data: font -> glyph -> [(x, y, before, after)] in the derived glyph's own pixels (x from its left
# ink column, y from the cell top), 1 = ink. A glyph is patched only when every `before` matches what the derivation
# gave from the player's copy; otherwise it is left as derived. None is applied unless params['patches'] is set.
_W_BAND = [(1, 2, 1, 0), (3, 2, 1, 0), (1, 3, 1, 0), (3, 3, 1, 0)]
# DotumChe bold W has a solid band where its three strokes meet (bolding fills #.#.#.# to ########); at 5 px wide it
# survives as two full rows, and the W reads as '\' or an H (check: "reads as '\'"; by eye: an oversized H).
# Opening the band leaves three prongs over two feet.
_STAR_FEET = [(1, 3, 1, 0), (0, 4, 1, 0), (2, 4, 1, 0), (0, 3, 0, 1), (2, 3, 0, 1)]
# DotumChe bold '*' derives as a plus with a stem and two feet (by eye: a space invaders ship). Lifting the
# feet onto the stem row gives a three-row star. The check passes '*' already; this is a look-only proposal.
PATCHES = {
    'fnt_plain': {'W': _W_BAND, '*': _STAR_FEET},
}
# The same W correction for rowfit 'pc' (round 5), where the band sits a row lower (rows 3-4); its '*' already comes
# out as the three-row star the `*` correction draws, so it has none.
_W_BAND_PC = [(1, 3, 1, 0), (3, 3, 1, 0), (1, 4, 1, 0), (3, 4, 1, 0)]
PATCHES_PC = {
    'fnt_plain': {'W': _W_BAND_PC},
}

# Fonts outside UNDERTALE's release: Sans's PC-height remake (not approved, so it stays out
# of the release and its key stays off) and DELTARUNE's fonts (analysis only). Their parameters and corrections
# are in tools/analysis/stglyphs_extra.py, which the release export leaves out; a repository build packs them too (the
# test keys can draw them), the builder only the fonts above.
_EXTRA = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'analysis', 'stglyphs_extra.py')
if os.path.isfile(_EXTRA):
    import importlib.util
    _spec = importlib.util.spec_from_file_location('stglyphs_extra', _EXTRA)
    _extra = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(_extra)
    FONT_PARAMS.update(_extra.FONT_PARAMS)
    PATCHES.update(_extra.PATCHES)
    PATCHES_PC.update(_extra.PATCHES_PC)

# English letter frequencies (per cent, public tables), so the shared row map favours the letters text uses most.
# Capitals count a fifth of their lowercase letter, other printable characters 0.3.
LETTER_FREQ = dict(zip('etaoinshrdlcumwfgypbvkjxqz',
                       [12.7, 9.1, 8.2, 7.5, 7.0, 6.7, 6.3, 6.1, 6.0, 4.3, 4.0, 2.8, 2.8, 2.4, 2.4, 2.2, 2.0, 2.0,
                        1.9, 1.5, 1.0, 0.8, 0.15, 0.15, 0.1, 0.07]))


def char_weight(ch):
    c = chr(ch)
    if c in LETTER_FREQ:
        return LETTER_FREQ[c]
    if c.lower() in LETTER_FREQ:
        return LETTER_FREQ[c.lower()] / 5 + 0.3
    return 0.3


def font_params(name, **override):
    p = dict(DEFAULT_PARAMS)
    p.update(FONT_PARAMS.get(name, {}))
    p.update(override)
    return p


# ---- the game data: fonts, sprites and texture pages, read-only ----

class GameData:
    """The fonts, sprites and texture pages of a data.win (as tools/analysis/gmfont.py reads them), decoding only the
    page rows a caller asks for."""

    def __init__(self, path):
        self.path = path
        self.win = fontverdict.Win(path)
        self.d = self.win.d
        self._blobs = None
        self._pages = {}
        self._fonts = None

    def u32(self, o):
        return struct.unpack_from('<I', self.d, o)[0]

    def string(self, ptr):
        if ptr == 0:
            return None
        n = self.u32(ptr - 4)
        return self.d[ptr:ptr + n].decode('utf-8', 'replace')

    def plist(self, name):
        return self.win.plist(name)

    def tpag(self, ptr):
        sx, sy, sw, sh, tx, ty, tw, th, bw, bh = struct.unpack_from('<10H', self.d, ptr)
        page = struct.unpack_from('<h', self.d, ptr + 20)[0]
        return dict(sx=sx, sy=sy, sw=sw, sh=sh, tx=tx, ty=ty, tw=tw, th=th, bw=bw, bh=bh, page=page)

    def page(self, i, rows):
        """(width, height, RGBA bytes) of texture page i, with at least its first `rows` rows decoded."""
        have = self._pages.get(i)
        if have is not None and have[3] >= min(rows, have[1]):
            return have[:3]
        if self._blobs is None:
            self._blobs = fontverdict.texture_blobs(self.win)
        if not 0 <= i < len(self._blobs) or self._blobs[i] is None:
            raise SystemExit(f'smalltext: {self.path}: texture page {i} not found')
        off, size = self._blobs[i]
        dec = fontverdict.decode_page(self.d[off:off + size], max(rows, have[3] if have else 0))
        if dec is None:
            raise SystemExit(f'smalltext: {self.path}: texture page {i} is in a format this tool does not read')
        w, h, rgba = dec
        self._pages[i] = (w, h, rgba, len(rgba) // (4 * w) if w else 0)
        return w, h, rgba

    def fonts(self):
        if self._fonts is not None:
            return self._fonts
        ptrs = self.plist('FONT')
        wad = self.win.wad
        # How many optional u32 fields sit between scaleY and the glyph list (same probe as data_win.c).
        optional = 1 if wad >= 17 else 0
        first = next(p for p in ptrs if p)
        for trial in range(optional, 5):
            ls = first + 40 + 4 * trial
            cnt = self.u32(ls)
            if cnt == 0 or cnt > 0x10000:
                continue
            if self.u32(ls + 4) == ls + 4 + 4 * cnt:
                optional = trial
                break
        out = {}
        for p in ptrs:
            if not p:
                continue
            f = {'name': self.string(self.u32(p)), 'tpag': self.tpag(self.u32(p + 28))}
            q = p + 40 + 4 * optional
            glyphs = {}
            for j in range(self.u32(q)):
                g = self.u32(q + 4 + 4 * j)
                ch, x, y, w, h = struct.unpack_from('<5H', self.d, g)
                shift, off = struct.unpack_from('<hh', self.d, g + 10)
                glyphs[ch] = dict(ch=ch, x=x, y=y, w=w, h=h, shift=shift, offset=off)
            f['glyphs'] = glyphs
            out[f['name']] = f
        self._fonts = out
        return out

    def region(self, page, x0, y0, w, h):
        """The page's texels [y0, y0 + h) x [x0, x0 + w), cut at the page's edges: rows of (r, g, b, a)."""
        pw, ph, rgba = self.page(page, y0 + h)
        out = []
        for y in range(y0, min(y0 + h, ph)):
            o = y * pw * 4
            out.append([tuple(rgba[o + 4 * x:o + 4 * x + 4]) for x in range(x0, min(x0 + w, pw))])
        return out

    def glyph_ink(self, font, ch):
        """The glyph's ink (alpha > 127) as stored on the texture page: (rows, width)."""
        g = font['glyphs'][ch]
        t = font['tpag']
        x0, y0 = t['sx'] + g['x'], t['sy'] + g['y']
        pw, ph, rgba = self.page(t['page'], y0 + g['h'])
        x1 = min(x0 + g['w'], pw)
        rows = []
        for y in range(y0, min(y0 + g['h'], ph)):
            o = y * pw * 4 + 3
            rows.append([rgba[o + 4 * x] > 127 for x in range(x0, x1)])
        return rows, max(0, x1 - x0)


# ---- bitmaps: lists of rows of bools ----

def zeros(h, w):
    return [[False] * w for _ in range(h)]


def width(a):
    return len(a[0]) if a else 0


def any_ink(a):
    return any(any(r) for r in a)


def copy(a):
    return [list(r) for r in a]


def cols_with_ink(a):
    return [x for x in range(width(a)) if any(r[x] for r in a)]


def rows_with_ink(a):
    return [y for y, r in enumerate(a) if any(r)]


def sub(a, i, j, k, l):
    """a[i:j, k:l] (numpy slicing: cut at the edges)."""
    return [r[k:l] for r in a[i:j]]


def coverage(a, i, j, k, l):
    """float(a[i:j, k:l].mean()) if the block isn't empty, else 0.0."""
    rows = a[i:j]
    if not rows:
        return 0.0
    n = len(rows[0][k:l])
    if n == 0:
        return 0.0
    return sum(sum(r[k:l]) for r in rows) / (len(rows) * n)


def transpose(a, w=None):
    w = width(a) if w is None else w
    return [[r[x] for r in a] for x in range(w)]


def key_of(a):
    return (len(a), width(a), tuple(tuple(r) for r in a))


def pairwise_sum(v, lo=0, n=None):
    """numpy's float64 sum of a contiguous 1-D array (pairwise summation, 8 accumulators below 128 elements), so that a
    sum that decides between choices rounds as smalltext_derive.py's does."""
    if n is None:
        n = len(v)
        if n == 0:
            return 0.0
    if n < 8:
        r = -0.0
        for i in range(lo, lo + n):
            r += v[i]
        return r
    if n <= 128:
        r = list(v[lo:lo + 8])
        i = 8
        while i < n - (n % 8):
            for j in range(8):
                r[j] += v[lo + i + j]
            i += 8
        res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]))
        while i < n:
            res += v[lo + i]
            i += 1
        return res
    n2 = n // 2
    n2 -= n2 % 8
    return pairwise_sum(v, lo, n2) + pairwise_sum(v, lo + n2, n - n2)


# ---- the derivation (smalltext_derive.py, function for function) ----

def max_thickness(T):
    """The largest min(horizontal run, vertical run) over the ink pixels of T (smalltext_shapes.py's stroke width)."""
    best = 0
    h, w = len(T), width(T)
    for y in range(h):
        for x in range(w):
            if not T[y][x]:
                continue
            x0 = x
            while x0 > 0 and T[y][x0 - 1]:
                x0 -= 1
            x1 = x
            while x1 + 1 < w and T[y][x1 + 1]:
                x1 += 1
            y0 = y
            while y0 > 0 and T[y0 - 1][x]:
                y0 -= 1
            y1 = y
            while y1 + 1 < h and T[y1 + 1][x]:
                y1 += 1
            best = max(best, min(x1 - x0 + 1, y1 - y0 + 1))
    return best


def ink_points(a):
    return [(y, x) for y, r in enumerate(a) for x, v in enumerate(r) if v]


def distinct_pass(out, meta, src):
    """Two glyphs that differ on PC never share one Crisp bitmap (smalltext_derive.distinct_pass).
    Returns {ch: (before, after)} of the glyphs changed.
    meta: {ch: (source for block_error, row groups, col groups)}."""
    changed = {}
    for ch in sorted(out):
        T, left = out[ch]
        if not any_ink(T) or ch not in meta:
            continue
        key = key_of(T)
        twin = None
        for o in sorted(out):
            if o >= ch:
                break
            if key_of(out[o][0]) == key:
                sa, sb = src[o], src[ch]
                if not (key_of(sa[0]) == key_of(sb[0]) and sa[1] == sb[1]):
                    twin = o
                    break
        if twin is None:
            continue
        a, rg, cg = meta[ch]
        ta, trg, tcg = meta[twin]
        R, C = len(T), width(T)
        cov = [[0.0] * C for _ in range(R)]
        for r, (i, j) in enumerate(rg):
            for c, (k, l) in enumerate(cg):
                cov[r][c] = coverage(a, i, j, k, l)
        cand = [(r, c) for r in range(R) for c in range(C) if T[r][c] or cov[r][c] > 0.0]
        want = (len(components(set(ink_points(a)))), sum(1 for hole in holes(a) if len(hole) >= 3))

        def topo_dist(U):
            p, h = ink_topology(U)
            return abs(p - want[0]) + abs(h - want[1])
        limit, thick = topo_dist(T), max_thickness(T)
        others = {key_of(out[o][0]) for o in out if o != ch}
        best = None
        for n in (1, 2):
            for combo in itertools.combinations(cand, n):
                U = copy(T)
                for y, x in combo:
                    U[y][x] = not U[y][x]
                if not any_ink(U) or max_thickness(U) > thick:
                    continue
                ku = key_of(U)
                if ku in others or ku == key:
                    continue
                t = topo_dist(U)
                if t > limit:
                    continue
                e = block_error(U, a, rg, cg)
                k = (t, round(e - block_error(U, ta, trg, tcg), 9), round(e, 9))
                if best is None or k < best[0]:
                    best = (k, U)
            if best is not None:
                break
        if best is not None:
            changed[ch] = (T, best[1])
            out[ch] = (best[1], left)
    return changed


def apply_patches(name, glyphs, table=None):
    """Apply PATCHES[name] (or table[name]) to {ch: (bitmap, left)} in place. Returns the glyphs patched."""
    done = []
    for c, ops in (PATCHES if table is None else table).get(name, {}).items():
        if ord(c) not in glyphs:
            continue
        g, left = glyphs[ord(c)]
        if not all(0 <= y < len(g) and 0 <= x < width(g) and int(g[y][x]) == b for x, y, b, _ in ops):
            continue
        g = copy(g)
        for x, y, _, a in ops:
            g[y][x] = bool(a)
        glyphs[ord(c)] = (g, left)
        done.append(c)
    return done


def cell_bitmap(dw, font, ch, cell_h):
    """The glyph's ink in its cell: rows 0..cell_h-1 from the font's line top, columns from the pen position."""
    g = font['glyphs'][ch]
    rows, w = dw.glyph_ink(font, ch)
    out = zeros(cell_h, w)
    for y in range(min(cell_h, len(rows))):
        out[y] = list(rows[y])
    return out, g['offset']


def unbold(a):
    out = copy(a)
    w = width(a)
    for y, row in enumerate(a):
        x = 0
        while x < w:
            if row[x]:
                e = x
                while e < w and row[e]:
                    e += 1
                if e - x >= 2:
                    out[y][e - 1] = False
                x = e
            else:
                x += 1
    return out


def runs(v):
    """Number of ink runs in a 1-D bool vector."""
    return sum(1 for i, b in enumerate(v) if b and (i == 0 or not v[i - 1]))


def interior_gaps(v):
    """(first, last) of each gap run with ink on both sides, in a 1-D bool vector."""
    out = []
    idx = [i for i, b in enumerate(v) if b]
    if len(idx) < 2:
        return out
    prev = idx[0]
    for x in idx[1:]:
        if x > prev + 1:
            out.append((prev + 1, x - 1))
        prev = x
    return out


def best_cuts(n, t, gaps, place_weight, forced=()):
    """Split range(n) into t contiguous groups (t-1 interior cuts). A gap run [g0, g1] survives the OR when some group
    lies wholly inside it, that is when at least two cut positions (0 and n count) fall in [g0, g1 + 1]. Minimizes
    the weight of gaps lost, then the cuts' distance from their proportional places. Exhaustive: cells are small."""
    t = max(1, min(t, n))
    if t == n:
        return [(i, i + 1) for i in range(n)]
    ideal = [n * k / t for k in range(1, t)]
    g = [(a, b + 1, float(w)) for a, b, w in gaps]
    best = None
    forced = sorted({f for f in forced if 0 < f < n})[:t - 1]
    free = [c for c in range(1, n) if c not in forced]
    for extra in itertools.combinations(free, t - 1 - len(forced)):
        cuts = tuple(sorted(forced + list(extra)))
        pos = (0,) + cuts + (n,)
        lost = 0.0
        if g:
            lost = float(pairwise_sum([w for lo, hi, w in g if bisect_right(pos, hi) - bisect_left(pos, lo) < 2]))
        place = sum((c - i) ** 2 for c, i in zip(cuts, ideal))
        key = lost + place_weight * place
        if best is None or key < best[0]:
            best = (key, cuts)
    pos = (0,) + best[1] + (n,)
    return [(pos[k], pos[k + 1]) for k in range(t)]


def uniform_cuts(n, k, gaps, t=None, weight=None, phases=None):
    """Split range(n) into groups of k (the ends may be shorter), at the phase (0..k-1) that loses the least gap
    weight (best_cuts' measure), then the most even ends. With t given, end groups are merged into their neighbours
    until there are t, the end with less ink first (`weight`: ink per position)."""
    best = None
    for phase in range(k) if phases is None else phases:
        cuts = [c for c in range(phase, n, k) if 0 < c < n]
        pos = (0,) + tuple(cuts) + (n,)
        groups = [(pos[i], pos[i + 1]) for i in range(len(pos) - 1)]
        while t is not None and len(groups) > t:
            first = sum(weight[groups[0][0]:groups[0][1]]) if weight is not None else 0
            last = sum(weight[groups[-1][0]:groups[-1][1]]) if weight is not None else 0
            if first <= last:
                groups[0:2] = [(groups[0][0], groups[1][1])]
            else:
                groups[-2:] = [(groups[-2][0], groups[-1][1])]
        if t is not None and len(groups) < t:
            continue
        pos = [gr[0] for gr in groups] + [n]
        lost = 0.0
        for a, b, w in gaps:
            if sum(1 for p in pos if a <= p <= b + 1) < 2:
                lost += w
        ends = abs((groups[0][1] - groups[0][0]) - (groups[-1][1] - groups[-1][0]))
        key = (round(lost, 6), ends, phase)
        if best is None or key < best[0]:
            best = (key, groups)
    if best is None:  # more target rows than an even split gives (a larger scale): the optimizer's split
        return best_cuts(n, t, gaps, 0.02)
    return best[1]


def holes(a):
    """The enclosed counters of a bool image (ink 8-connected, so background 4-connected): a list of pixel sets."""
    h, w = len(a), width(a)
    seen = [[False] * (w + 2) for _ in range(h + 2)]
    pad = [[False] * (w + 2)] + [[False] + list(r) + [False] for r in a] + [[False] * (w + 2)]
    out = []
    for y0 in range(h + 2):
        for x0 in range(w + 2):
            if pad[y0][x0] or seen[y0][x0]:
                continue
            comp, stack, edge = [], [(y0, x0)], False
            seen[y0][x0] = True
            while stack:
                y, x = stack.pop()
                comp.append((y - 1, x - 1))
                if y in (0, h + 1) or x in (0, w + 1):
                    edge = True
                for yy, xx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
                    if 0 <= yy < h + 2 and 0 <= xx < w + 2 and not pad[yy][xx] and not seen[yy][xx]:
                        seen[yy][xx] = True
                        stack.append((yy, xx))
            if not edge:
                out.append(set(comp))
    return out


def keep_holes(a, T, rg, cg, min_area=3, most=3):
    """Close the counters that leak. Each source counter of at least `min_area` pixels should stay a counter of the
    derived glyph. While fewer survive, ink the smallest set (up to `most`) of uninked blocks with source ink that
    adds a counter, the set with the highest total coverage first."""
    want = sum(1 for hole in holes(a) if len(hole) >= min_area)
    if want == 0:
        return T
    R, C = len(T), width(T)
    cov = [[0.0] * C for _ in range(R)]
    for r, (i, j) in enumerate(rg):
        for c, (k, l) in enumerate(cg):
            cov[r][c] = coverage(a, i, j, k, l)
    for _ in range(want):
        have = len(holes(T))
        if have >= want:
            break
        cand = [(r, c) for r in range(R) for c in range(C) if not T[r][c] and cov[r][c] > 0.0]
        best = None
        for n in range(1, most + 1):
            for combo in itertools.combinations(cand, n):
                for y, x in combo:
                    T[y][x] = True
                gain = len(holes(T)) - have
                for y, x in combo:
                    T[y][x] = False
                key = (sum(cov[y][x] for y, x in combo), tuple(-q[0] * C - q[1] for q in combo))
                if gain > 0 and (best is None or key > best[0]):
                    best = (key, combo)
            if best is not None:
                break
        if best is None:
            break
        for y, x in best[1]:
            T[y][x] = True
    return T


def ink_topology(T):
    """(8-connected ink pieces, enclosed counters) of a bool image."""
    return len(components(set(ink_points(T)))), len(holes(T))


def guided_thin(bold, thin):
    """The bold derivation thinned toward the thin one: each pixel of bold that thin lacks is removed, in raster
    order and repeatedly, when that keeps the ink's pieces and counters (a simple point) and the pixel is no stroke
    end (it has two or more 8-neighbours)."""
    T = copy(bold)
    h, w = len(T), width(T)
    topo = ink_topology(T)
    changed = True
    while changed:
        changed = False
        for y in range(h):
            for x in range(w):
                if not T[y][x] or thin[y][x]:
                    continue
                n = sum(sum(r[max(0, x - 1):x + 2]) for r in T[max(0, y - 1):y + 2]) - 1
                if n < 2:
                    continue
                T[y][x] = False
                if ink_topology(T) == topo:
                    changed = True
                else:
                    T[y][x] = True
    return T


def finish(a, bold, rg, cg, params):
    """The target glyph for row groups rg and column groups cg: the shrink of `a`, counters kept when `holes`, and,
    given the bold ink, the bold glyph's shrink thinned toward it (guided_thin)."""
    T = shrink(a, rg, cg, params)
    if params['holes']:
        T = keep_holes(a, T, rg, cg)
    if bold is not None:
        B = shrink(bold, rg, cg, params)
        if params['holes']:
            B = keep_holes(bold, B, rg, cg)
        T = guided_thin(B, T)
    return T


def block_error(T, a, rg, cg):
    """How far a target glyph is from the source's block coverage (what the 2:1 box average, smoothing, shows):
    the sum over blocks of |target - coverage|."""
    e = 0.0
    for r, (i, j) in enumerate(rg):
        for c, (k, l) in enumerate(cg):
            e += abs(float(T[r][c]) - coverage(a, i, j, k, l))
    return e


def stub_columns(T):
    """Edge columns (the glyph's first or last inked one) holding a single ink pixel next to more ink."""
    cols = cols_with_ink(T)
    n = 0
    for c in {cols[0], cols[-1]} if len(cols) > 1 else ():
        ys = [y for y in range(len(T)) if T[y][c]]
        if len(ys) == 1:
            nb = c + 1 if c == cols[0] else c - 1
            if T[ys[0]][nb]:
                n += 1
    return n


def fold_overshoot(a, top, base, k):
    """Fold ink that passes the cap top row `top` or the baseline `base` (first row below the caps) by fewer than k
    rows onto that row (OR), when the glyph has ink on the far side of it too; deeper ink (a descender, an accent)
    stays."""
    rows = rows_with_ink(a)
    if not rows:
        return a
    a = copy(a)
    w = width(a)
    r0, r1 = rows[0], rows[-1] + 1
    if base < r1 <= base + k - 1 and r0 < base:
        for y in range(base, r1):
            a[base - 1] = [a[base - 1][x] or a[y][x] for x in range(w)]
            a[y] = [False] * w
    if top - k + 1 <= r0 < top and r1 > top:
        for y in range(r0, top):
            a[top] = [a[top][x] or a[y][x] for x in range(w)]
            a[y] = [False] * w
    return a


def row_map(cells, t, n_rows, place_weight=0.02, pixel=0, scale=0.0, uniform=0, phases=None):
    """One row partition for the whole font. cells: {ch: bitmap with n_rows rows}. The x-height's top and the baseline
    are always cuts (from the glyph x), so descenders and ascenders keep rows of their own. For a pixel font stored at
    scale `pixel`, every font-pixel boundary is a cut too (`pixel_cuts`)."""
    forced = pixel_cuts(list(cells.values()), 0, pixel) if pixel else []
    x = cells.get(ord('x'))
    if x is not None and any_ink(x):
        rows_x = rows_with_ink(x)
        forced += [rows_x[0], rows_x[-1] + 1]
    gaps = []
    for ch, a in cells.items():
        w = char_weight(ch)
        for col in transpose(a):
            for g0, g1 in interior_gaps(col):
                gaps.append((g0, g1, w))
    total = sum(w for _, _, w in gaps) or 1.0
    gaps = [(a, b, w / total * 100.0) for a, b, w in gaps]
    if uniform:
        weight = [sum(sum(a[y]) for a in cells.values()) for y in range(n_rows)]
        return uniform_cuts(n_rows, uniform, gaps, t, weight, phases)
    if pixel or not scale:
        return best_cuts(n_rows, t, gaps, place_weight, forced)
    # Keep the drawn size: the rows that hold ink in some glyph get round(span * scale) target rows, and the empty rows
    # above and below keep their own share.
    ink = [y for y in range(n_rows) if any(any(a[y]) for a in cells.values())]
    if not ink:
        return best_cuts(n_rows, t, gaps, place_weight, forced)
    r0, r1 = ink[0], ink[-1] + 1
    top = min(int(round(r0 * scale)), t - 1)
    t_ink = max(1, min(r1 - r0, int(round((r1 - r0) * scale)), t - top))
    bottom = t - top - t_ink
    inner = best_cuts(r1 - r0, t_ink, [(a - r0, b - r0, w) for a, b, w in gaps if a >= r0 and b < r1], place_weight,
                      [f - r0 for f in forced if r0 < f < r1])
    groups = [(a + r0, b + r0) for a, b in inner]
    if top:
        groups = [(a, b) for a, b in best_cuts(r0, top, [], place_weight)] + groups
    else:
        groups[0] = (0, groups[0][1])
    if bottom and n_rows > r1:
        groups += [(a + r1, b + r1) for a, b in best_cuts(n_rows - r1, bottom, [], place_weight)]
    else:
        groups[-1] = (groups[-1][0], n_rows)
    return groups


def pixel_cuts(arrays, axis, k):
    """Font-pixel boundaries of a pixel font stored at integer scale k: the phase (0..k-1) at which every ink run
    along `axis` starts, and every k-th position from it."""
    n = len(arrays[0]) if axis == 0 else width(arrays[0])
    votes = [0] * k
    for a in arrays:
        b = a if axis == 1 else transpose(a)  # rows of b run along `axis`
        for line in b:
            for s in range(1, len(line)):
                if line[s] and not line[s - 1]:
                    votes[s % k] += 1
    phase = votes.index(max(votes))
    return [c for c in range(phase, n, k) if 0 < c < n]


def apply_rows(a, groups):
    w = width(a)
    if not w:
        return zeros(len(groups), 0)
    out = []
    for i, j in groups:
        blk = a[i:j]
        out.append([any(r[x] for r in blk) for x in range(w)])
    return out


def col_map(a, w):
    """Partition a glyph's columns into w groups, keeping horizontal gaps (between stems) open where possible."""
    gaps = []
    for row in a:
        for g0, g1 in interior_gaps(row):
            gaps.append((g0, g1, 1.0))
    return best_cuts(width(a), w, gaps, 0.02)


def shrink_or(a, rg, cg):
    """The first round's rule: a target pixel is ink when its source block has any ink."""
    return [[any_ink(sub(a, i, j, k, l)) for k, l in cg] for i, j in rg]


def run_lengths_rows(b):
    """For each ink pixel of b, the length of its run along b's rows; 0 off ink."""
    out = []
    for row in b:
        o = [0] * len(row)
        x = 0
        while x < len(row):
            if row[x]:
                e = x
                while e < len(row) and row[e]:
                    e += 1
                for q in range(x, e):
                    o[q] = e - x
                x = e
            else:
                x += 1
        out.append(o)
    return out


def stroke_pixels(a, axis):
    """Pixels on a stroke along `axis` (0: vertical, 1: horizontal): a run along the axis two or more pixels long with
    at least half of its pixels thin across it."""
    h, w = len(a), width(a)
    at = transpose(a, w)
    if axis == 1:
        b, L, X = a, run_lengths_rows(a), transpose(run_lengths_rows(at), h)
    else:
        b, L, X = at, run_lengths_rows(at), transpose(run_lengths_rows(a), w)
    o = [[False] * len(r) for r in b]
    for y, row in enumerate(b):
        x = 0
        while x < len(row):
            if row[x]:
                n = L[y][x]
                if n >= 2 and 2 * sum(1 for q in range(x, min(x + n, len(row))) if X[y][q] < n) >= n:
                    for q in range(x, min(x + n, len(row))):
                        o[y][q] = True
                x += n
            else:
                x += 1
    return o if axis == 1 else transpose(o, h)


def shrink_cover(a, rg, cg, t):
    """Coverage threshold plus stroke rescue (smalltext_derive.shrink_cover documents the rule)."""
    R, C = len(rg), len(cg)
    ys = rows_with_ink(a)
    T = zeros(R, C)
    cov = [[0.0] * C for _ in range(R)]
    if not ys:
        return T
    top, bot = ys[0], ys[-1] + 1
    vstroke, hstroke = stroke_pixels(a, 0), stroke_pixels(a, 1)
    h, w = len(a), width(a)
    for r, (i, j) in enumerate(rg):
        i2, j2 = max(i, top), min(j, bot)
        if i2 >= j2:
            continue
        for c, (k, l) in enumerate(cg):
            blk = sub(a, i2, j2, k, l)
            if not any_ink(blk):
                continue
            area = coverage(a, i2, j2, k, l)
            cov[r][c] = area
            xs = range(max(0, k), min(l, w))
            vrows = vstroke[i2:j2]
            vert = any(all(row[x] for row in vrows) for x in xs)
            horiz = any(all(row[x] for x in xs) for row in hstroke[i2:j2])
            T[r][c] = area > t or vert or horiz
    rowof = [0] * h
    for r, (i, j) in enumerate(rg):
        for y in range(i, min(j, h)):
            rowof[y] = r
    colof = [0] * w
    for c, (k, l) in enumerate(cg):
        for x in range(k, min(l, w)):
            colof[x] = c
    ink = ink_points(a)
    nbrs = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            nbrs[y][x] = sum(sum(r[max(0, x - 1):x + 2]) for r in a[max(0, y - 1):y + 2]) - a[y][x]
    for _ in range(R * C + 1):
        loose = {p for p in ink if not T[rowof[p[0]]][colof[p[1]]]}
        changed = False
        for comp in components(loose):
            touched = set()
            for y, x in comp:
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        yy, xx = y + dy, x + dx
                        if 0 <= yy < h and 0 <= xx < w and a[yy][xx] and T[rowof[yy]][colof[xx]]:
                            touched.add((rowof[yy], colof[xx]))
            ends = [(y, x) for y, x in comp if nbrs[y][x] <= 1]
            if ends:
                for y, x in ends:
                    T[rowof[y]][colof[x]] = True
                changed = True
                continue
            if len(touched) >= 2:
                continue
            counts = {}
            for y, x in comp:
                k = (rowof[y], colof[x])
                counts[k] = counts.get(k, 0) + 1
            br, bc = max(sorted(counts), key=lambda k: (cov[k[0]][k[1]], counts[k]))
            T[br][bc] = True
            changed = True
        if not changed:
            break
    # Connectivity: each 8-connected source stroke must land as one 8-connected piece.
    for comp in components(set(ink)):
        blocks = {}
        for y, x in comp:
            k = (rowof[y], colof[x])
            blocks[k] = blocks.get(k, 0) + 1
        for _ in range(len(blocks)):
            pieces = components({k for k in blocks if T[k[0]][k[1]]})
            if len(pieces) <= 1:
                break
            label = {k: n for n, p in enumerate(pieces) for k in p}
            best = None
            for k in sorted(blocks):
                if T[k[0]][k[1]]:
                    continue
                touch = {label[(k[0] + dy, k[1] + dx)] for dy in (-1, 0, 1) for dx in (-1, 0, 1)
                         if (k[0] + dy, k[1] + dx) in label}
                key = (len(touch), cov[k[0]][k[1]], blocks[k])
                if touch and (best is None or key > best[0]):
                    best = (key, k)
            if best is None:
                break
            T[best[1][0]][best[1][1]] = True
    return T


def components(points):
    """8-connected components of a set of (y, x) points, each a list, in a fixed order."""
    left = set(points)
    out = []
    while left:
        seed = min(left)
        comp, stack = [], [seed]
        left.discard(seed)
        while stack:
            y, x = stack.pop()
            comp.append((y, x))
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    q = (y + dy, x + dx)
                    if q in left:
                        left.discard(q)
                        stack.append(q)
        out.append(comp)
    return out


def shrink(a, rg, cg, params):
    if params['merge'] == 'or':
        return shrink_or(a, rg, cg)
    return shrink_cover(a, rg, cg, params['cover'])


def _floor(v):
    i = int(v)
    return i - 1 if v < i else i


def _ceil(v):
    i = int(v)
    return i + 1 if v > i else i


def derive(dw, name, target_adv, target_rows, ink_limit_px, **override):
    font = dw.fonts()[name]
    params = font_params(name, **override)
    if params['rowfit'] == 'uniform' and params['boldthin'] and params.get('rowphase') is None:
        # every phase of the even row split, for the whole font: the one whose glyphs are nearest what smoothing
        # shows (the sum of the glyphs' block_error), as each glyph's columns are chosen
        tries = []
        for phase in range(2):
            d = derive(dw, name, target_adv, target_rows, ink_limit_px, **dict(override, rowphase=phase))
            if d.get('error') is not None:
                tries.append((d['error'], phase, d))
        if tries:
            return min(tries, key=lambda t: (t[0], t[1]))[2]
    glyphs = {ch: g for ch, g in font['glyphs'].items() if 32 <= ch < 127}
    cell_h = max(g['h'] for g in glyphs.values())
    adv = max(g['shift'] for g in glyphs.values())
    sx = target_adv / adv
    font_cols = params['colmap'] == 'font'
    cell_w = max(g['offset'] + g['w'] for g in glyphs.values()) if font_cols else 0
    src, ink_w, bold_src = {}, {}, {}
    boldthin = params['boldthin'] and params['unbold'] and not font_cols
    fold = None
    if params['overshoot'] and ord('H') in glyphs:
        hrows = rows_with_ink(cell_bitmap(dw, font, ord('H'), cell_h)[0])
        fold = (hrows[0], hrows[-1] + 1, max(1, int(round(1.0 / sx))))
    for ch in glyphs:
        a, off = cell_bitmap(dw, font, ch, cell_h)
        if fold is not None:
            a = fold_overshoot(a, *fold)
        inked = cols_with_ink(a)
        ink_w[ch] = inked[-1] + 1 - inked[0] if inked else 0
        if boldthin:
            # both trimmed to the bold ink's columns (unbolding only takes run ends, so the left edge is the same)
            b = sub(a, 0, len(a), inked[0], inked[-1] + 1) if inked else sub(a, 0, len(a), 0, 0)
            a = unbold(a)
            a = sub(a, 0, len(a), inked[0], inked[-1] + 1) if inked else sub(a, 0, len(a), 0, 0)
            bold_src[ch] = b
            src[ch] = (a, off + (inked[0] if inked else 0))
            continue
        if params['unbold']:
            a = unbold(a)
        if font_cols:
            # the whole cell from the pen position, so that every glyph shares one column map
            full = zeros(cell_h, cell_w)
            dst = range(*slice(off, off + width(a)).indices(cell_w))
            srcx = range(*slice(None, max(0, cell_w - off)).indices(width(a)))
            if len(dst) != len(srcx):
                raise SystemExit(f'smalltext: {name}: glyph {ch} does not fit its cell')
            for y in range(cell_h):
                for dx, sx_ in zip(dst, srcx):
                    full[y][dx] = a[y][sx_]
            src[ch] = (full, 0)
            continue
        # trim empty columns, remembering the left edge in source pixels from the pen position
        cols = cols_with_ink(a)
        if cols:
            a = sub(a, 0, len(a), cols[0], cols[-1] + 1)
            left = off + cols[0]
        else:
            a = sub(a, 0, len(a), 0, 0)
            left = off
        src[ch] = (a, left)
    k = max(1, int(round(1.0 / sx)))
    phases = None if params.get('rowphase') is None else [int(params['rowphase'])]
    if params['rowfit'] == 'pc' and phases is None and ord('H') in glyphs:
        # PC's height (round 5): the even split whose cut falls on the cap top, so H's 12 PC rows are 6 native rows
        hrows = rows_with_ink(cell_bitmap(dw, font, ord('H'), cell_h)[0])
        phases = [hrows[0] % k] if hrows else None
    rows = row_map({ch: a for ch, (a, _) in src.items()}, target_rows, cell_h, pixel=params['pixel'],
                   scale=sx if params['rowfit'] == 'scale' else 0.0,
                   uniform=k if params['rowfit'] in ('uniform', 'pc') else 0, phases=phases)
    shared_cols = None
    if font_cols:
        gaps = []
        for ch, (a, _) in src.items():
            for row in apply_rows(a, rows):
                for g0, g1 in interior_gaps(row):
                    gaps.append((g0, g1, char_weight(ch)))
        forced = pixel_cuts([a for a, _ in src.values()], 1, params['pixel']) if params['pixel'] else []
        shared_cols = best_cuts(cell_w, max(1, int(round(cell_w * sx))), gaps, 0.02, forced)
    out = {}
    meta = {}
    total_error = 0.0
    for ch, (a, left) in src.items():
        r = apply_rows(a, rows)
        if width(r) == 0 or not any_ink(a):
            out[ch] = (zeros(target_rows, 0), 0)
            continue
        if shared_cols is not None:
            out[ch] = (shrink(a, rows, shared_cols, params), 0)
            continue
        want = int(round(ink_w[ch] * sx))
        need = max(2 * runs(row) - 1 for row in r)
        base = max(1, _floor(glyphs[ch]['shift'] * sx) - 1) if ink_limit_px is None else ink_limit_px
        wide = max(base + 1, _ceil(glyphs[ch]['shift'] * sx - 1e-9))
        grow = max(need, want if want >= 2 and not params['gap'] else 0)
        limit = min(grow, wide) if grow > base else base
        w = max(1, min(max(want, min(need, limit)), limit, width(r)))
        if params['colfit'] == 'uniform' and boldthin:
            # every phase of the even split; the one whose glyph is nearest what smoothing shows (block_error)
            best = None
            aw = width(a)
            for phase in range(k):
                cuts = [c for c in range(phase, aw, k) if 0 < c < aw]
                pos = [0] + cuts + [aw]
                cg = [(pos[i], pos[i + 1]) for i in range(len(pos) - 1)]
                T = finish(a, bold_src[ch], rows, cg, params)
                e = block_error(T, bold_src[ch], rows, cg) + stub_columns(T)
                if best is None or e < best[0] - 1e-9:
                    best = (e, T, cg)
            out[ch] = (best[1], int(round(left * sx)))
            meta[ch] = (bold_src[ch], rows, best[2])
            total_error += best[0]
            continue
        if params['colfit'] == 'uniform':
            cgaps = [(g0, g1, 1.0) for row in r for g0, g1 in interior_gaps(row)]
            cg = uniform_cuts(width(r), k, cgaps)
        else:
            cg = col_map(r, w)
        T = finish(a, bold_src.get(ch) if boldthin else None, rows, cg, params)
        out[ch] = (T, int(round(left * sx)))
        meta[ch] = (bold_src[ch] if boldthin else a, rows, cg)
    redone = distinct_pass(out, meta, src) if params['distinct'] else {}
    patched = apply_patches(name, out, PATCHES_PC if params['rowfit'] == 'pc' else None) if params['patches'] else []
    return dict(font=font, rows=rows, cell_h=cell_h, glyphs=out, adv=adv, src=src, params=params, patched=patched,
                redone=redone, error=total_error if boldthin and params['colfit'] == 'uniform' else None)


# ---- carry (smalltext_carry.py: the parts the pack writes) ----

NONE = (127, -128)  # an empty row's (left, right)


def extents(bitmap, x0=0):
    """Per row (leftmost, rightmost) ink column + x0 of a bitmap, NONE for an empty row."""
    out = []
    for row in bitmap:
        xs = [x for x, v in enumerate(row) if v]
        out.append((xs[0] + x0, xs[-1] + x0) if xs else NONE)
    return out


def phase(pc, left, bitmap=None):
    """A glyph's phase (smalltext_carry.phase): its PC extents' leftmost ink column from the pen minus 2 * (left + the
    bitmap's first ink column); 0 for a glyph with no PC ink."""
    lo = [a for a, b in pc if a <= b]
    c0 = 0
    if bitmap is not None and any_ink(bitmap):
        c0 = cols_with_ink(bitmap)[0]
    return min(lo) - 2 * (left + c0) if lo else 0


def row_shifts(glyphs):
    """The font's row shift for a line on an even and on an odd surface row (smalltext_carry.row_shifts)."""
    letters = []
    for ch, v, g, left, ext in glyphs:
        if v or not (65 <= ch <= 90 or 97 <= ch <= 122) or not any_ink(g):
            continue
        ys = [i for i, (a, b) in enumerate(ext) if a <= b]
        rs = rows_with_ink(g)
        if ys:
            letters.append((rs[0], rs[-1] + 1, ys[0], ys[-1] + 1))
    out = []
    for q in (0, 1):
        def edges(k):
            return [(abs(2 * (i0 + k) - q - p0), abs(2 * (i1 + k) - q - p1)) for i0, i1, p0, p1 in letters]

        def cost(k):
            e = edges(k)
            bottom = sum(b for _, b in e)
            return 2 * bottom > 3 * len(e), sum(max(t, b) for t, b in e), abs(k), k
        out.append(min(range(-3, 4), key=cost) if letters else 0)
    return tuple(out)
