"""The sound-effect pack's builder (sfxpack.py) and runtime reader (ctr_adpcm_pack.inc) agree. Run with -I."""
from pathlib import Path
import re
import struct
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sfxpack

RUNTIME = HERE.parents[1] / 'platform/3ds/source/ctr_adpcm_pack.inc'


def body(sound, size):
    """A stand-in payload: nonzero bytes that differ per sound."""
    return bytes(((sound * 7 + j) & 0xFF) | 1 for j in range(size))


def pack(rows, digest='ab' * 32, total=None, catalog=sfxpack.CATALOG_ID):
    count = len(rows)
    offset = sfxpack.HEADER_BYTES + sfxpack.ROW.size * count
    table, payloads = bytearray(), bytearray()
    for sound, bits, size in rows:
        payload = body(sound, size)
        head = sfxpack.ROW_HEAD.pack(sound, bits, 0, offset + len(payloads), size, 100, 22050, 7)
        table += head + struct.pack('<I', sfxpack.row_check(head, payload) if size % 4 == 0 else 0)
        payloads += payload
    size = offset + len(payloads) if total is None else total
    header = sfxpack.MAGIC + struct.pack('<III', sfxpack.VERSION, count, size) + catalog + digest.encode()
    return header + bytes(sfxpack.HEADER_BYTES - len(header)) + table + payloads


def damaged(image, how):
    """UT-012's damage inside the last payload, the header and table untouched: a flipped byte, a zeroed run (an SD
    sector lost to unsafe removal) or another sound's bytes in its place (a stale copy)."""
    image = bytearray(image)
    if how == 'flip':
        image[-5] ^= 0x10
    elif how == 'zero':
        image[-68:-4] = bytes(64)
    else:
        image[-100:] = body(1, 100)
    return bytes(image)


def retabled(image, how):
    """c26rev F3: damage in the table, every payload intact: the first row's sound ID one bit lower (3 -> 2, still
    sorted), or the first two rows' payload pointers swapped (offset, length, frames, rate, seam and check)."""
    image = bytearray(image)
    at, size = sfxpack.HEADER_BYTES, sfxpack.ROW.size
    if how == 'id':
        image[at] ^= 0x01
    else:
        first, second = bytes(image[at + 4:at + size]), bytes(image[at + size + 4:at + 2 * size])
        image[at + 4:at + size], image[at + size + 4:at + 2 * size] = second, first
    return bytes(image)


class Runtime(unittest.TestCase):
    def test_constants_match_runtime(self):
        text = RUNTIME.read_text(encoding='utf-8')
        block = re.search(r'apCatalog\[32\] MM_RO = \{(.*?)\};', text, re.S)[1]
        self.assertEqual(bytes(int(v, 16) for v in re.findall(r'0x([0-9a-f]{2})', block)), sfxpack.CATALOG_ID)
        self.assertIn('#define AP_HEADER %du' % sfxpack.HEADER_BYTES, text)
        self.assertIn('#define AP_ROW %du' % sfxpack.ROW.size, text)
        self.assertIn('#define AP_VERSION %du' % sfxpack.VERSION, text)
        self.assertIn('#define AP_MAGIC 0x%08xu' % struct.unpack('<I', sfxpack.MAGIC)[0], text)
        self.assertIn('apName[] MM_RO = "/%s"' % sfxpack.NAME, text)


class Read(unittest.TestCase):
    def check(self, image, digest=None):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / sfxpack.NAME
            path.write_bytes(image)
            return sfxpack.read(path, digest)

    def test_good(self):
        rows = self.check(pack([(3, 16, 200), (9, 8, 300)]), 'ab' * 32)
        self.assertEqual([r['sound'] for r in rows], [3, 9])

    def test_rejections(self):
        for image, digest in ((pack([(3, 16, 200)])[:-1], None),                # truncated (FTP cut short)
                              (pack([(3, 16, 200)], total=10_000), None),        # size field
                              (pack([(3, 16, 200)], catalog=bytes(32)), None),  # other converter
                              (pack([(3, 16, 200)]), 'cd' * 32),                # other data.win
                              (pack([(9, 16, 200), (3, 16, 200)]), None),       # unsorted
                              (pack([(3, 12, 200)]), None),                     # bits
                              (pack([(3, 16, 100)]), None),                     # no room for a DTAD header
                              (pack([(3, 16, 202)]), None),                     # not whole 32-bit words
                              (damaged(pack([(3, 16, 200)]), 'flip'), None),    # a payload byte flipped
                              (damaged(pack([(3, 16, 200)]), 'zero'), None),    # a zeroed run in a payload
                              (damaged(pack([(3, 16, 200)]), 'stale'), None),   # stale bytes in a payload
                              (retabled(pack([(3, 16, 200), (9, 16, 200)]), 'id'), None),     # a row's sound ID
                              (retabled(pack([(3, 16, 200), (9, 16, 200)]), 'swap'), None)):  # payloads swapped
            with self.assertRaises(ValueError):
                self.check(image, digest)


class Versions(unittest.TestCase):
    """The release table (tools/release/sfxpack-versions.json) lists every UNDERTALE data file the builder accepts."""
    def test_table_matches_data_versions(self):
        table = __import__('json').loads(sfxpack.VERSIONS.read_text(encoding='utf-8'))
        self.assertIn('names only', sfxpack.VERSIONS.read_text(encoding='utf-8')[:2048])
        known = {m[2]: int(m[1]) for m in re.finditer(r'DATA_VERSION\("undertale", "root", (\d+)ULL, "([0-9a-f]{64})"\)',
                 (HERE.parents[1] / 'platform/3ds/data_versions.inc').read_text(encoding='utf-8'))}
        self.assertEqual({r['data_sha256'] for r in table['packs']}, set(known))
        for r in table['packs']:
            self.assertEqual(r['data_size'], known[r['data_sha256']])
            self.assertRegex(r['sha256'], '^[0-9a-f]{64}$')
            self.assertTrue(r['size'] > sfxpack.HEADER_BYTES and r['sounds'] > 0)
        self.assertEqual(len({r['sha256'] for r in table['packs']}), len(table['packs']))

    def test_encoder_has_no_fused_multiply_add(self):
        # Same pack bytes on every builder host (arm64 compilers fuse a*b+c by default).
        self.assertIn('-ffp-contract=off', sfxpack.dsp.CFLAGS)

    def test_verify(self):
        first = __import__('json').loads(sfxpack.VERSIONS.read_text(encoding='utf-8'))['packs'][0]
        r = sfxpack.expected(first['data_sha256'])
        good = dict(data_win_sha256=r['data_sha256'], bytes=r['size'], pack_sha256=r['sha256'], sounds=r['sounds'])
        self.assertEqual(sfxpack.verify(good), 'match')
        self.assertEqual(sfxpack.verify(dict(good, data_win_sha256='00' * 32)), 'unlisted')
        with self.assertRaises(ValueError):
            sfxpack.verify(dict(good, pack_sha256='00' * 32))


if __name__ == '__main__':
    unittest.main()
