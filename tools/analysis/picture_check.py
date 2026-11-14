"""Byte-level facts about benchmark pictures, for any claim that a picture is missing a layer or is black.

    py -3 -I tools/analysis/picture_check.py <result dir or picture> ... [--ref DIR ...] [--json out.json]
        [--black-level 16] [--strict]

Inputs are 3DS result directories (a `files/` folder inside one is used when the pictures are there, as in
`dist/remote/<row>/`), Azahar harness folders, or single pictures: `top_`, `right_`, `shot_` and `bottom_`
captures, BMP (what the console writes) or PNG (what the Azahar harness keeps). No packages are needed.

For each picture:
- md5 of the file, and its size;
- non-black share: the percentage of pixels with any channel above --black-level (default 16);
- for a top (or right-eye) capture with a `shot_` of the same frame: how the application surface sits in it, as the
  renderer's present() places it, and the share of the top's pixels in that rectangle that differ from it.
  `1:1` is Native drawing: the surface centred at floor((topW - w) / 2), floor((240 - h) / 2), which is the 40 px
  margin in Standard (400 px) and 240 px in Wide (800 px). `scaled` is Full drawing (topScale; Wide doubles the horizontal scale), compared by
  sampling the first texel of each block: approximate, as the detail-keeping shrink can take the second texel and
  smoothing averages. Zoom mode's 1:1 crop (retired 2026-10-19) is not recognised: an old Zoom capture reads as
  `scaled` with a large mismatch.
- against every --ref directory (the other model, a repeat, the desktop runner): the matching frame's pixel-diff
  count, peak channel delta and bounding box. A reference without a 3DS prefix (the desktop runner's
  --screenshot pattern) counts as the application capture of the last number in its name. A reference that is
  an integer multiple of the picture's size is sampled at the first pixel of each block (labelled `sampled`).

Identical bytes with opposite verdicts mean the verdict is wrong (docs/3ds-hardware.md, "A reviewer's "missing
layer" needs a byte check before it counts"). A real missing layer is a pixel difference from a reference; a black
top is a non-black share near zero.

Exit code: 0 on success; with --strict, 1 when any 1:1 top differs from its application capture or any
top, right or shot capture differs from a reference (bottom_ captures are the log console and are left out); 2 on
bad input.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys
import zlib

TOP_H = 240
NAME = re.compile(r'^(top|right|shot|bottom)_(\d+)(.*)$')
KIND_ORDER = {'top': 0, 'right': 1, 'shot': 2, 'bottom': 3}


class Picture:
    """An RGB picture: rows of bytes, three per pixel, top row first."""

    def __init__(self, width, height, rows, md5='', size=0, path=''):
        self.width, self.height, self.rows = width, height, rows
        self.md5, self.size, self.path = md5, size, path


def read_bmp(data):
    if data[:2] != b'BM' or len(data) < 54:
        raise ValueError('not a BMP')
    off = struct.unpack_from('<I', data, 10)[0]
    w, h = struct.unpack_from('<ii', data, 18)
    bits = struct.unpack_from('<H', data, 28)[0]
    comp = struct.unpack_from('<I', data, 30)[0]
    if bits not in (24, 32) or comp not in (0, 3):
        raise ValueError('BMP with %d bits, compression %d: only 24/32-bit uncompressed' % (bits, comp))
    step = bits // 8
    stride = (abs(w) * step + 3) & ~3
    w, bottom_up, h = abs(w), h > 0, abs(h)
    if off + stride * (h - 1) + w * step > len(data):
        raise ValueError('BMP truncated')
    rows = []
    for y in range(h):
        sy = h - 1 - y if bottom_up else y
        r = data[off + sy * stride: off + sy * stride + w * step]
        out = bytearray(w * 3)
        out[0::3], out[1::3], out[2::3] = r[2::step], r[1::step], r[0::step]
        rows.append(bytes(out))
    return w, h, rows


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)


def read_png(data):
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    p, idat, palette, ihdr = 8, [], None, None
    while p + 8 <= len(data):
        n, kind = struct.unpack_from('>I4s', data, p)
        body = data[p + 8:p + 8 + n]
        if kind == b'IHDR':
            ihdr = struct.unpack('>IIBBBBB', body)
        elif kind == b'PLTE':
            palette = body
        elif kind == b'IDAT':
            idat.append(body)
        elif kind == b'IEND':
            break
        p += 12 + n
    if ihdr is None:
        raise ValueError('PNG without IHDR')
    w, h, depth, ctype, _, _, interlace = ihdr
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(ctype)
    if depth != 8 or channels is None or interlace != 0 or (ctype == 3 and palette is None):
        raise ValueError('PNG depth %d, colour type %d, interlace %d: only 8-bit non-interlaced' % (depth, ctype, interlace))
    raw = zlib.decompress(b''.join(idat))
    bpp, st = channels, w * channels
    if len(raw) < (st + 1) * h:
        raise ValueError('PNG image data truncated')
    prev, rows = bytearray(st), []
    for y in range(h):
        f = raw[y * (st + 1)]
        line = bytearray(raw[y * (st + 1) + 1:(y + 1) * (st + 1)])
        if f == 1:
            for i in range(bpp, st):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            for i in range(st):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(st):
                line[i] = (line[i] + (((line[i - bpp] if i >= bpp else 0) + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(st):
                line[i] = (line[i] + _paeth(line[i - bpp] if i >= bpp else 0, prev[i], prev[i - bpp] if i >= bpp else 0)) & 255
        elif f != 0:
            raise ValueError('PNG filter %d' % f)
        prev = line
        if ctype == 2:
            rows.append(bytes(line))
        elif ctype == 6:
            out = bytearray(w * 3)
            out[0::3], out[1::3], out[2::3] = line[0::4], line[1::4], line[2::4]
            rows.append(bytes(out))
        elif ctype in (0, 4):
            g = bytes(line[0::channels])
            out = bytearray(w * 3)
            out[0::3] = out[1::3] = out[2::3] = g
            rows.append(bytes(out))
        else:
            rows.append(b''.join(palette[i * 3:i * 3 + 3] for i in line))
    return w, h, rows


def load(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:2] == b'BM':
        w, h, rows = read_bmp(data)
    elif data[:8] == b'\x89PNG\r\n\x1a\n':
        w, h, rows = read_png(data)
    else:
        raise ValueError('%s: neither BMP nor PNG' % path)
    return Picture(w, h, rows, hashlib.md5(data).hexdigest(), len(data), path)


def nonblack_share(pic, level=16):
    """Percentage of pixels with any channel above level."""
    table = bytes(1 if v > level else 0 for v in range(256))
    lit = 0
    for r in pic.rows:
        t = r.translate(table)
        n = len(t) // 3
        if n == 0:
            continue
        a = int.from_bytes(t[0::3], 'big') | int.from_bytes(t[1::3], 'big') | int.from_bytes(t[2::3], 'big')
        lit += a.to_bytes(n, 'big').count(1)
    return 100.0 * lit / (pic.width * pic.height) if pic.width and pic.height else 0.0


def crop(pic, x0, y0, w, h):
    return Picture(w, h, [r[x0 * 3:(x0 + w) * 3] for r in pic.rows[y0:y0 + h]])


def sample(pic, w, h, src_x, src_y):
    """A w x h picture whose pixel (x, y) is pic's (src_x(x), src_y(y))."""
    xs = [src_x(x) for x in range(w)]
    rows = []
    for y in range(h):
        r = pic.rows[src_y(y)]
        out = bytearray(w * 3)
        for i, sx in enumerate(xs):
            out[i * 3:i * 3 + 3] = r[sx * 3:sx * 3 + 3]
        rows.append(bytes(out))
    return Picture(w, h, rows)


def diff(a, b):
    """Pixel-diff count, peak channel delta and bounding box (x0, y0, x1, y1 inclusive) of two equal-size pictures."""
    if (a.width, a.height) != (b.width, b.height):
        raise ValueError('sizes differ: %dx%d vs %dx%d' % (a.width, a.height, b.width, b.height))
    count, peak, box = 0, 0, None
    for y, (ra, rb) in enumerate(zip(a.rows, b.rows)):
        if ra == rb:
            continue
        for x in range(a.width):
            i = x * 3
            if ra[i:i + 3] != rb[i:i + 3]:
                count += 1
                peak = max(peak, abs(ra[i] - rb[i]), abs(ra[i + 1] - rb[i + 1]), abs(ra[i + 2] - rb[i + 2]))
                if box is None:
                    box = [x, y, x, y]
                else:
                    box = [min(box[0], x), min(box[1], y), max(box[2], x), max(box[3], y)]
    return {'pixels': count, 'share': 100.0 * count / (a.width * a.height) if a.width and a.height else 0.0,
            'peak_delta': peak, 'bbox': box}


def top_vs_app(top, shot):
    """How the application capture sits in the top capture (renderer present()), and the mismatch there."""
    top_w = top.width
    if shot.width <= top_w and shot.height <= TOP_H:
        x0, y0 = (top_w - shot.width) // 2, (TOP_H - shot.height) // 2
        d = diff(crop(top, x0, y0, shot.width, shot.height), shot)
        return dict(d, method='1:1', x=x0, y=y0, w=shot.width, h=shot.height)
    # Full drawing: topScale.
    scale = min(400.0 / shot.width, float(TOP_H) / shot.height)
    if scale >= 1.0:
        scale = float(int(scale))
    sx = scale * 2.0 if top_w == 800 else scale
    w, h = int(shot.width * sx), int(shot.height * scale)
    x0, y0 = (top_w - w) // 2, (TOP_H - h) // 2
    if w <= 0 or h <= 0 or w > top_w or h > TOP_H:
        return {'method': 'none', 'note': 'application capture does not fit the top'}
    shrunk = sample(shot, w, h, lambda x: min(shot.width - 1, int(x / sx)), lambda y: min(shot.height - 1, int(y / scale)))
    d = diff(crop(top, x0, y0, w, h), shrunk)
    return dict(d, method='scaled', x=x0, y=y0, w=w, h=h)


def against(pic, ref):
    """Diff of pic against a reference picture of the same frame; an integer multiple in size is sampled."""
    out = {'ref': ref.path, 'ref_md5': ref.md5, 'identical_bytes': pic.md5 == ref.md5}
    if (ref.width, ref.height) == (pic.width, pic.height):
        out.update(diff(pic, ref), method='same size')
        return out
    kx, ky = ref.width // pic.width if pic.width else 0, ref.height // pic.height if pic.height else 0
    if kx >= 1 and ky >= 1 and kx * pic.width == ref.width and ky * pic.height == ref.height:
        out.update(diff(pic, sample(ref, pic.width, pic.height, lambda x: x * kx, lambda y: y * ky)), method='sampled 1/%dx1/%d' % (kx, ky))
        return out
    out.update(method='size differs', note='%dx%d vs %dx%d' % (pic.width, pic.height, ref.width, ref.height))
    return out


def picture_key(path, desktop=False):
    """(kind, frame, suffix) from a capture's file name; a desktop picture is the application capture of its last number."""
    stem = os.path.splitext(os.path.basename(path))[0]
    m = NAME.match(stem)
    if m:
        return m.group(1), int(m.group(2)), m.group(3)
    if desktop:
        nums = re.findall(r'\d+', stem)
        if nums:
            return 'shot', int(nums[-1]), ''
    return None


def list_pictures(path, desktop=False):
    """{(kind, frame, suffix): file} for a directory (or its files/ folder) or a single picture."""
    if os.path.isfile(path):
        k = picture_key(path, True)
        return {k: path} if k else {}
    found = {}
    for d in (path, os.path.join(path, 'files')):
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if os.path.splitext(name)[1].lower() not in ('.bmp', '.png'):
                continue
            k = picture_key(name, desktop)
            if k and k not in found:
                found[k] = os.path.join(d, name)
        if found:
            break
    return found


def check(inputs, refs=(), level=16):
    """The report: one entry per input picture, sorted by input, frame, suffix and kind."""
    ref_sets = [(r, list_pictures(r, desktop=True)) for r in refs]
    cache = {}

    def get(p):
        if p not in cache:
            cache[p] = load(p)
        return cache[p]

    report = []
    for src in inputs:
        pics = list_pictures(src)
        if not pics:
            raise ValueError('%s: no top_/right_/shot_/bottom_ pictures' % src)
        for key in sorted(pics, key=lambda k: (k[1], k[2], KIND_ORDER[k[0]])):
            kind, frame, suffix = key
            pic = get(pics[key])
            entry = {'input': src, 'file': pics[key], 'kind': kind, 'frame': frame, 'suffix': suffix, 'md5': pic.md5,
                     'bytes': pic.size, 'width': pic.width, 'height': pic.height,
                     'nonblack_share': round(nonblack_share(pic, level), 3)}
            if kind in ('top', 'right') and ('shot', frame, suffix) in pics:
                entry['top_vs_app'] = top_vs_app(pic, get(pics[('shot', frame, suffix)]))
            entry['refs'] = []
            for rdir, rpics in ref_sets:
                rp = rpics.get(key)
                if rp is None:
                    entry['refs'].append({'ref_dir': rdir, 'missing': True})
                else:
                    entry['refs'].append(dict(against(pic, get(rp)), ref_dir=rdir))
            report.append(entry)
        cache.clear()
    return report


def _ref_text(r):
    if r.get('missing'):
        return 'no frame'
    if 'pixels' not in r:
        return r['method'] + ' ' + r.get('note', '')
    if r['identical_bytes']:
        return 'identical md5'
    if r['pixels'] == 0:
        return 'same pixels'
    b = r['bbox']
    return '%d px (%.2f%%) peak %d box %d,%d-%d,%d%s' % (r['pixels'], r['share'], r['peak_delta'], b[0], b[1], b[2], b[3],
                                                         '' if r['method'] == 'same size' else ' ' + r['method'])


def _tva_text(t):
    if t is None:
        return '-'
    if t['method'] == 'none':
        return t['note']
    s = '%s @%d,%d %dx%d: ' % (t['method'], t['x'], t['y'], t['w'], t['h'])
    if t['pixels'] == 0:
        return s + 'match'
    b = t['bbox']
    return s + '%d px (%.2f%%) peak %d box %d,%d-%d,%d' % (t['pixels'], t['share'], t['peak_delta'], b[0], b[1], b[2], b[3])


def format_table(report):
    lines = []
    for e in report:
        name = os.path.basename(e['file'])
        lines.append('%-24s %s %3dx%-3d lit %5.1f%% | top/app %s' % (name, e['md5'][:10], e['width'], e['height'],
                                                                  e['nonblack_share'], _tva_text(e.get('top_vs_app'))))
        for r in e['refs']:
            lines.append('    vs %-40s %s' % (r['ref_dir'][-40:], _ref_text(r)))
    return '\n'.join(lines)


def failed(report):
    for e in report:
        if e['kind'] == 'bottom':  # the log console (bottom_mode=1): its text differs between runs
            continue
        t = e.get('top_vs_app')
        if t and t['method'] == '1:1' and t['pixels']:
            return True
        if any(r.get('pixels') for r in e['refs']):
            return True
    return False


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('inputs', nargs='+', help='result directories or pictures')
    ap.add_argument('--ref', action='append', default=[], help='reference directory or picture (repeatable)')
    ap.add_argument('--json', help='write the full report here')
    ap.add_argument('--black-level', type=int, default=16, help='a pixel is lit when any channel exceeds this (16)')
    ap.add_argument('--strict', action='store_true', help='exit 1 on any top/app mismatch at 1:1 or any reference difference')
    a = ap.parse_args(argv)
    try:
        report = check(a.inputs, a.ref, a.black_level)
    except (OSError, ValueError, zlib.error) as e:
        print('picture_check: %s' % e, file=sys.stderr)
        return 2
    print(format_table(report))
    if a.json:
        with open(a.json, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=1)
    return 1 if a.strict and failed(report) else 0


if __name__ == '__main__':
    sys.exit(main())
