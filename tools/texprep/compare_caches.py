"""Compares two texture cache folders file by file (pageNNN.dtx and complete).

Usage: py -3 -I tools/texprep/compare_caches.py <reference cache> <candidate cache> [...pairs]

Prints, per pair, how many page files are byte-identical, which differ (with the first differing offset and
the field it falls in), which are missing on either side, and whether the "complete" markers match.

Bytes 52-55 of a page file are the header struct's tail padding (PackHeader is 52 bytes of fields, 56 with the
alignment of its uint64_t). The console never initialises them (finishBuilder's designated initializer), so it
writes whatever was on its stack there, and nothing reads them. Files that differ only there count as
identical, and the count of such files is printed. --strict counts them as different.
Exit status 0 only if every pair is identical.
"""
from __future__ import annotations

import hashlib
import re
import struct
import sys
from pathlib import Path

PAGE = re.compile(r'page([0-9]+)\.dtx\Z')
HEADER_FIELDS = [('magic', 4), ('version', 4), ('dataWinSize', 4), ('gameId', 4), ('timestamp', 8), ('blobOffset', 4),
                 ('blobSize', 4), ('width', 2), ('height', 2), ('tileW', 2), ('tileH', 2), ('tilesX', 2), ('tilesY', 2),
                 ('payloadSize', 4), ('tableChecksum', 4), ('pad', 4)]


def field_at(data: bytes, offset: int) -> str:
    at = 0
    for name, size in HEADER_FIELDS:
        if offset < at + size:
            return f'header.{name}'
        at += size
    tiles_x, tiles_y = struct.unpack_from('<HH', data, 36)
    table_end = 56 + 12 * tiles_x * tiles_y
    if offset < table_end:
        return f'table[tile {(offset - 56) // 12}]'
    return 'tile data'


def files(folder: Path) -> dict[str, Path]:
    out = {}
    for p in folder.iterdir():
        if PAGE.fullmatch(p.name) or p.name == 'complete':
            out[p.name] = p
    return out


def compare(ref: Path, cand: Path) -> bool:
    a, b = files(ref), files(cand)
    same, differ, pad_only = 0, [], 0
    for name in sorted(set(a) & set(b)):
        x, y = a[name].read_bytes(), b[name].read_bytes()
        if x == y:
            same += 1
            continue
        if not STRICT and name != 'complete' and len(x) == len(y) >= 56 and x[:52] == y[:52] and x[56:] == y[56:]:
            same += 1
            pad_only += 1
            continue
        first = next((i for i in range(min(len(x), len(y))) if x[i] != y[i]), min(len(x), len(y)))
        where = field_at(x, first) if name != 'complete' else 'marker'
        differ.append(f'{name}: {len(x)} vs {len(y)} bytes, first difference at {first} ({where})')
    only_ref = sorted(set(a) - set(b))
    only_cand = sorted(set(b) - set(a))
    total = sum(p.stat().st_size for n, p in a.items() if n in b)
    digest = hashlib.sha256()
    for name in sorted(set(a) & set(b)):
        digest.update(name.encode() + b'\0' + b[name].read_bytes())
    ok = not differ and not only_ref and not only_cand
    print(f'{ref} vs {cand}: {same} identical of {len(set(a) | set(b))} files ({total} bytes compared, '
          f'{pad_only} differing only in the 4 padding bytes); '
          f'{"IDENTICAL" if ok else "DIFFERENT"}; candidate set sha256 {digest.hexdigest()[:16]}')
    for line in differ:
        print('  differs:', line)
    for name in only_ref:
        print('  only in reference:', name)
    for name in only_cand:
        print('  only in candidate:', name)
    return ok


STRICT = False


def main(argv: list[str]) -> int:
    global STRICT
    if argv and argv[0] == '--strict':
        STRICT = True
        argv = argv[1:]
    if len(argv) < 2 or len(argv) % 2:
        print(__doc__)
        return 2
    ok = True
    for i in range(0, len(argv), 2):
        ok = compare(Path(argv[i]), Path(argv[i + 1])) and ok
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
