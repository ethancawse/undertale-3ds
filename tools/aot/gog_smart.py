"""GOG-only Smart compatibility metadata; no pixels, strings or game code.

Approved: content SHA-256 plus stable sprite/frame identity (10-23). The 49 entries all need
Steam's coarse verdict. Validate the complete profile against the measured GOG container before emitting
an optional 257-byte DTFV trailer. Any mismatch omits the entire trailer; ordinary GOG analysis remains.
Atlas coordinates are used only to read a decoded crop, never to select its verdict.
"""
import hashlib
import json
from pathlib import Path
import struct
import zlib

PROFILE = Path(__file__).with_name('gog-smart-1.08.json')
COUNT = 49
CRC32 = 0x8B0D1702  # Exact validated sprite/frame/resolved-item packet; runtime checks the same value.


def encode(profile):
    if profile['schema'] != 1 or profile['rounded_verdict'] != 128:
        raise ValueError('unsupported profile')
    entries = profile['entries']
    if any(len(r) != 6 or any(type(v) is not int or v < 0 for v in r) for r in entries):
        raise ValueError('invalid identity or dimensions')
    if len(entries) != COUNT or entries != sorted(entries):
        raise ValueError('incomplete or unordered profile')
    if len({r[5] for r in entries}) != COUNT or len({tuple(r[:2]) for r in entries}) != COUNT:
        raise ValueError('duplicate identity')
    out = b'GSI1' + struct.pack('<HBB', COUNT, 128, 0)
    out += b''.join(struct.pack('<HBH', r[0], r[1], r[5]) for r in entries)
    crc = zlib.crc32(out)
    if crc != CRC32:
        raise ValueError('profile packet changed without runtime qualification')
    return out + struct.pack('<I', crc)


def compatibility_tail(win, profile_path=PROFILE):
    if not win.gog_smart:
        return b''  # Steam/DR neither read the profile nor change their DTFV bytes.
    try:
        return validated_tail(win, json.loads(Path(profile_path).read_text(encoding='utf-8')))
    except (OSError, ValueError, KeyError, TypeError, IndexError, struct.error, zlib.error):
        return b''  # Missing/stale metadata is optional; never apply a partial table.


def validated_tail(win, profile):
    import fontverdict
    # Both recognition and profile binding require the full digest; the CtrPackKey alone is insufficient.
    if (not win.gog_smart or profile['target_data_sha256'] != '946c738c6d6182f683431320a0e32470bc3deb50f3e8887e61998df0c4e3abc8'
            or hashlib.sha256(win.d).hexdigest() != profile['target_data_sha256']
            or profile['source_data_sha256'] != 'b718f8223a5bb31979ffeed10be6140c857b882fc0d0462b89d6287ae38c81c7'):
        raise ValueError('wrong container')
    encoded = encode(profile)
    sprites, items = win.plist('SPRT'), win.plist('TPAG')
    blobs = fontverdict.texture_blobs(win)
    by_page = {}
    for sprite, frame, width, height, content, resolved in profile['entries']:
        p = sprites[sprite]
        if not p or frame >= win.u32(p + 56) or win.u32(p + 60 + frame * 4) != items[resolved]:
            raise ValueError('sprite/frame identity mismatch')
        t = fontverdict.tpag_item(win, items[resolved])
        if (t['sw'], t['sh']) != (width, height) or width <= 0 or height <= 0:
            raise ValueError('dimensions mismatch')
        digest = profile['content_sha256'][content]
        if len(digest) != 64 or bytes.fromhex(digest).hex() != digest:
            raise ValueError('invalid content digest')
        by_page.setdefault(t['page'], []).append((t, digest))
    for page, entries in sorted(by_page.items()):
        off, size = blobs[page]
        blob = win.d[off:off + size]
        head = fontverdict.decode_page(blob, 1)
        if head is None:
            raise ValueError('unreadable page')
        width, height, rgba = fontverdict.decode_page(blob, head[1])
        for t, digest in entries:
            x, y, w, h = t['sx'], t['sy'], t['sw'], t['sh']
            if x + w > width or y + h > height:
                raise ValueError('crop outside page')
            crop = b''.join(rgba[(row * width + x) * 4:(row * width + x + w) * 4] for row in range(y, y + h))
            if hashlib.sha256(crop).hexdigest() != digest:
                raise ValueError('decoded-content mismatch')
    return encoded


def build_enabled(path):
    """Select the compatibility build from the full container digest, never a path/store label."""
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest() == '946c738c6d6182f683431320a0e32470bc3deb50f3e8887e61998df0c4e3abc8'


if __name__ == '__main__':
    import argparse
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import corpus
    import prepare
    parser = argparse.ArgumentParser(description='Private GOG build selection by measured data digest.')
    parser.add_argument('--build-flag', action='store_true', required=True)
    parser.add_argument('--data')
    args = parser.parse_args()
    folder = prepare.find_data('undertale', args.data)
    parts = list(corpus.parts('undertale', folder)) if folder else []
    print('ON' if parts and build_enabled(parts[0][1]) else 'OFF')
