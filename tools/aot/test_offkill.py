"""Tests for tools/aot/offkill.py (romfs/offkill.bin, the early destroy's entries; source/z_offkill.c reads it).

    py -3 -I tools/aot/test_offkill.py

The synthetic data.win names no game identifiers. The last test checks the real entries against the player's
UNDERTALE data when it is installed (CTRPORT_UNDERTALE_DATA or Steam), and is skipped otherwise.
"""
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import offkill  # noqa: E402


def fake_data_win(objects, sprites):
    """A minimal FORM with OBJT (name, sprite) and SPRT (name, width, height, origin) chunks, strings in STRG."""
    strg = bytearray()
    names = {}
    for name in [n for n, _ in objects] + [s[0] for s in sprites]:
        if name not in names:
            names[name] = len(strg)
            strg += name.encode('utf-8') + b'\0'

    def chunk_list(records):
        head = struct.pack('<I', len(records))
        offsets, body = [], bytearray()
        for r in records:
            offsets.append(len(body))
            body += r
        return head, offsets, body

    # Absolute positions: FORM(8) + STRG chunk + OBJT chunk + SPRT chunk.
    strg_at = 8 + 8
    objt_at = strg_at + len(strg) + 8
    objt_rows = [bytearray(struct.pack('<Ii', 0, sprite) + b'\0' * 24) for _, sprite in objects]
    objt_head, objt_offsets, objt_body = chunk_list(objt_rows)
    objt_body_at = objt_at + len(objt_head) + 4 * len(objt_rows)
    sprt_at = objt_body_at + len(objt_body) + 8
    sprt_rows = [bytearray(struct.pack('<IIIiiiiIIIIIii', 0, w, h, 0, 0, 0, 0, 0, 0, 0, 0, 0, ox, oy) + struct.pack('<i', 0))
                   for _, w, h, ox, oy in sprites]
    sprt_head, sprt_offsets, sprt_body = chunk_list(sprt_rows)
    sprt_body_at = sprt_at + len(sprt_head) + 4 * len(sprt_rows)
    # Name pointers point at the string's first character.
    for i, (name, _) in enumerate(objects):
        struct.pack_into('<I', objt_rows[i], 0, strg_at + names[name])
    for i, s in enumerate(sprites):
        struct.pack_into('<I', sprt_rows[i], 0, strg_at + names[s[0]])
    objt = objt_head + b''.join(struct.pack('<I', objt_body_at + o) for o in objt_offsets) + b''.join(objt_rows)
    sprt = sprt_head + b''.join(struct.pack('<I', sprt_body_at + o) for o in sprt_offsets) + b''.join(sprt_rows)
    body = b'STRG' + struct.pack('<I', len(strg)) + strg
    body += b'OBJT' + struct.pack('<I', len(objt)) + objt
    body += b'SPRT' + struct.pack('<I', len(sprt)) + sprt
    return b'FORM' + struct.pack('<I', len(body)) + body


class OffKillTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def data(self, objects, sprites):
        path = os.path.join(self.tmp.name, 'data.win')
        with open(path, 'wb') as f:
            f.write(fake_data_win(objects, sprites))
        return offkill.Data(path)

    def entry(self, name, axis=0, sign=1, reach=10, shake=0, scale=1):
        return offkill.Entry(offkill.name_hash(name), axis, sign, reach, shake, scale, name)

    def test_reads_objects_and_sprites(self):
        d = self.data([('wall', 1), ('petal', 0)], [('petal_sprite', 6, 6, 3, 3), ('wall_sprite', 20, 10, 0, 0)])
        self.assertEqual(d.objects, [('wall', 1), ('petal', 0)])
        self.assertEqual(d.sprites[1], ('wall_sprite', 20, 10, 0, 0))
        self.assertAlmostEqual(d.corner(0, 1), 18 ** 0.5)
        self.assertAlmostEqual(d.corner(1, 1), (20 ** 2 + 10 ** 2) ** 0.5)
        self.assertAlmostEqual(d.corner(0, 2), 72 ** 0.5)

    def test_pack_layout(self):
        d = self.data([('a', 0), ('b', 0)], [('s', 16, 16, 8, 8)])
        rows = offkill.resolve(d, [self.entry('b', axis=1, sign=1, reach=17, shake=2),
                                   self.entry('a', axis=0, sign=-1, reach=13)])
        self.assertEqual([why for _, _, why in rows], [None, None])
        blob = offkill.pack(rows)
        self.assertEqual(len(blob), 16)
        self.assertEqual(blob[:8], bytes([1, 0, 1, 1, 17, 0, 0, 0]))
        self.assertEqual(blob[8:], bytes([0, 0, 0, 0xFF, 13, 0, 0, 0]))
        self.assertEqual(struct.unpack('<hbbB3x', blob[8:]), (0, 0, -1, 13))

    def test_reach_must_cover_sprite_and_twice_the_shake(self):
        d = self.data([('a', 0)], [('s', 16, 16, 8, 8)])  # corner 11.31
        ok = offkill.resolve(d, [self.entry('a', reach=17, shake=2)])  # need 16.31
        short = offkill.resolve(d, [self.entry('a', reach=16, shake=2)])
        scaled = offkill.resolve(d, [self.entry('a', reach=17, shake=2, scale=2)])  # need 35.63
        self.assertIsNone(ok[0][2])
        self.assertIn('reach 16 <', short[0][2])
        self.assertIn('reach 17 <', scaled[0][2])
        self.assertEqual(offkill.pack(short), b'')

    def test_missing_duplicate_and_spriteless_objects_are_left_out(self):
        d = self.data([('a', 0), ('a', 0), ('c', -1)], [('s', 4, 4, 2, 2)])
        rows = offkill.resolve(d, [self.entry('a'), self.entry('b'), self.entry('c')])
        self.assertEqual([why for _, _, why in rows],
                         ['object name not unique', 'object not found', 'object has no sprite'])
        self.assertEqual(offkill.pack(rows), b'')

    def test_too_many_entries(self):
        d = self.data([('a', 0)], [('s', 4, 4, 2, 2)])
        rows = offkill.resolve(d, [self.entry('a')] * (offkill.MAX_ENTRIES + 1))
        with self.assertRaises(ValueError):
            offkill.pack(rows)

    def test_write_keeps_same_bytes_and_removes_empty(self):
        romfs = os.path.join(self.tmp.name, 'romfs')
        path = os.path.join(romfs, 'offkill.bin')
        self.assertTrue(offkill.write(romfs, b'\1' * 8))
        self.assertFalse(offkill.write(romfs, b'\1' * 8))
        self.assertTrue(offkill.write(romfs, b'\2' * 8))
        with open(path, 'rb') as f:
            self.assertEqual(f.read(), b'\2' * 8)
        self.assertTrue(offkill.write(romfs, b''))
        self.assertFalse(os.path.exists(path))
        self.assertFalse(offkill.write(romfs, b''))

    def test_deltarune_has_no_entries(self):
        self.assertEqual(offkill.build('deltarune', None), (b'', []))

    def test_entry_table(self):
        self.assertLessEqual(len(offkill.ENTRIES), offkill.MAX_ENTRIES)
        for e in offkill.ENTRIES:
            self.assertEqual(len(e.key), offkill.HASH_DIGITS)
            self.assertIn(e.axis, (0, 1))
            self.assertIn(e.sign, (-1, 1))
            self.assertTrue(0 < e.reach < 256)
            # The margin rule: the reach covers at least twice the scene's largest shake past the sprite.
            self.assertGreaterEqual(e.reach, 1 + 2 * e.shake)

    def test_real_undertale_entries(self):
        import prepare
        found = prepare.find_data('undertale', None)
        if not found:
            self.skipTest('no UNDERTALE data installed')
        blob, lines = offkill.build('undertale', found)
        self.assertEqual(lines[-1], 'offkill: 5 of 5 entries, 40 B', lines)
        got = [struct.unpack_from('<hbbB3x', blob, 8 * i) for i in range(len(blob) // 8)]
        # UNDERTALE 1.08 (Steam and GOG): object indices 1204, 475, 480 and 1634.
        self.assertEqual(got, [(1204, 0, 1, 6), (475, 1, 1, 25), (480, 1, 1, 72), (1634, 0, 1, 91), (1634, 0, -1, 91)])


if __name__ == '__main__':
    unittest.main()
