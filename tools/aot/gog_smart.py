"""GOG-only Smart compatibility metadata; no pixels, strings or game code.

Content SHA-256 plus stable sprite/frame identity was approved (10-23). The 49 entries all need
Steam's coarse verdict. Validate the complete profile against the measured GOG container before emitting
an optional 257-byte DTFV trailer. Any mismatch omits the entire trailer; ordinary GOG analysis remains.
Atlas coordinates are used only to read a decoded crop, never to select its verdict.

The profile's item_entries (gogqa, 11-05) give the item-verdict table (itemverdict.py's .dtiv) Steam's 10-byte entry
for 59 GOG items whose art is Steam's, decoded texel for texel, but which GOG's repacked atlas puts in tiles of other
formats, so the analysis of the stored texels ends elsewhere (two battle sprites on Azahar: smoothed on GOG, sharp on
Steam). Each takes Steam's entry only where it is safe on GOG's stored texels: the same stored texels, or Steam's
entry doesn't average the item, or both stores' texels are equally clean. One item fails that (sprite 2017 frame 1
with rounded colours: 805 stored texels differ, and GOG's tile keeps 73 of them partly transparent where Steam's tile
binarises the alpha) and keeps its own verdict. Validated like the trailer (digest, sprite/frame identity, size,
decoded crop); any mismatch leaves the analysed table whole.
"""
import hashlib
import json
from pathlib import Path
import struct
import zlib

PROFILE = Path(__file__).with_name('gog-smart-1.08.json')
COUNT = 49
CRC32 = 0x8B0D1702  # Exact validated sprite/frame/resolved-item packet; runtime checks the same value.
ITEM_COUNT = 59  # item_entries: [sprite, frame, width, height, GOG item, Steam's entry (20 hex), decoded crop SHA-256]


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
    check_container(win, profile)
    encoded = encode(profile)
    check_items(win, [(sprite, frame, width, height, resolved, profile['content_sha256'][content])
                      for sprite, frame, width, height, content, resolved in profile['entries']])
    return encoded


def check_container(win, profile):
    # Both recognition and profile binding require the full digest; the CtrPackKey alone is insufficient.
    if (not win.gog_smart or profile['target_data_sha256'] != '946c738c6d6182f683431320a0e32470bc3deb50f3e8887e61998df0c4e3abc8'
            or hashlib.sha256(win.d).hexdigest() != profile['target_data_sha256']
            or profile['source_data_sha256'] != 'b718f8223a5bb31979ffeed10be6140c857b882fc0d0462b89d6287ae38c81c7'):
        raise ValueError('wrong container')


def check_items(win, rows):
    """Each (sprite, frame, width, height, item, content SHA-256): the sprite's frame is that item, of that size, and
    its decoded crop has that digest. Raises ValueError at the first that isn't."""
    import fontverdict
    sprites, items = win.plist('SPRT'), win.plist('TPAG')
    blobs = fontverdict.texture_blobs(win)
    by_page = {}
    for sprite, frame, width, height, resolved, digest in rows:
        p = sprites[sprite]
        if not p or frame >= win.u32(p + 56) or win.u32(p + 60 + frame * 4) != items[resolved]:
            raise ValueError('sprite/frame identity mismatch')
        t = fontverdict.tpag_item(win, items[resolved])
        if (t['sw'], t['sh']) != (width, height) or width <= 0 or height <= 0:
            raise ValueError('dimensions mismatch')
        if type(digest) is not str or len(digest) != 64 or bytes.fromhex(digest).hex() != digest:
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


def item_overrides(win, table, profile_path=PROFILE):
    """(table, entries applied): the item-verdict table with the profile's validated item_entries, or the table as it
    was (0) for any other container or any mismatch. Steam and DELTARUNE never read the profile."""
    if not win.gog_smart:
        return table, 0
    try:
        found = validated_items(win, json.loads(Path(profile_path).read_text(encoding='utf-8')), len(table))
    except (OSError, ValueError, KeyError, TypeError, IndexError, struct.error):
        return table, 0  # never a partial profile
    out = list(table)
    for item, entry in found:
        out[item] = entry
    return out, len(found)


def validated_items(win, profile, count):
    """[(GOG item, Steam's 10-byte entry)] for the whole item profile, or ValueError."""
    check_container(win, profile)
    rows = profile['item_entries']
    if len(rows) != ITEM_COUNT or rows != sorted(rows):
        raise ValueError('incomplete or unordered item profile')
    if len({r[4] for r in rows}) != ITEM_COUNT or len({tuple(r[:2]) for r in rows}) != ITEM_COUNT:
        raise ValueError('duplicate item identity')
    found = []
    for row in rows:
        if len(row) != 7 or any(type(v) is not int or v < 0 for v in row[:5]) or row[4] >= count:
            raise ValueError('invalid item identity')
        entry = row[5]
        if type(entry) is not str or len(entry) != 20 or bytes.fromhex(entry).hex() != entry:
            raise ValueError('invalid item entry')
        value = bytes.fromhex(entry)
        if not (value[0] & 0x80 and value[5] & 0x80):
            raise ValueError('item entry not known in both colour modes')
        found.append((row[4], value))
    check_items(win, [(r[0], r[1], r[2], r[3], r[4], r[6]) for r in rows])
    return found


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
