"""Post-copy verification and atomic receipt publication on synthetic files."""
import calendar
import hashlib
import os
from pathlib import Path
import shutil
import struct
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import receipts
import test_catalog


class ReceiptTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        test_catalog.CatalogTests.setUpClass()

    def setUp(self):
        self.fixture = test_catalog.CatalogTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.root = self.fixture.source
        self.assets = self.root / 'audio-adpcm'
        shutil.copytree(self.fixture.out, self.assets)
        self.index = next((self.assets / 'index').glob('*.dtai'))
        self.target = self.assets / 'receipts' / (self.fixture.sha.hex() + '.dtar')

    def publish(self, **kwargs):
        return receipts.publish(self.root, self.assets, self.index, **kwargs)

    def absent(self):
        self.assertFalse(self.target.exists())
        self.assertFalse(self.target.with_suffix('.dtar.tmp').exists())

    def test_post_copy_identity_paths_sizes_and_timestamp_forms(self):
        source = self.root / 'copy.ogg'
        os.utime(source, (1700000001, 1700000001))
        result = self.publish()
        r = self.target.read_bytes()
        self.assertEqual(result['verified_files'], 2)  # source plus unique payload
        self.assertEqual(struct.unpack_from('<4s6I', r), (b'DTAR', 1, 128, 32, 3, 224, 1))
        self.assertEqual(r[32:52], hashlib.sha1(self.index.read_bytes()).digest())
        self.assertEqual(r[52:72], hashlib.sha1(r[128:]).digest())
        self.assertEqual(r[88:120], self.fixture.sha)
        self.assertEqual(receipts.PROBE_UTC, calendar.timegm((2001, 2, 3, 4, 5, 6)))
        self.assertEqual(int(self.target.stat().st_mtime), receipts.PROBE_UTC)
        self.assertEqual(struct.unpack_from('<2Q', r, 72), receipts.stamps(self.target.stat()))
        # Embedded and absent slots cannot authorize external playback.
        self.assertEqual(r[128:160], bytes(32))
        entries = [struct.unpack_from('<4Q', r, 128 + n * 32) for n in (1, 2)]
        self.assertIn(receipts.stamps(source.stat()), [entry[:2] for entry in entries])
        self.assertIn((0, 0, 0, 0), entries)

    def test_stale_source_or_payload_rejects_before_publication(self):
        for path in (self.root / 'copy.ogg', next((self.assets / 'objects').glob('*.dtad'))):
            self.publish()
            old = path.read_bytes()
            path.write_bytes(old[:-1] + bytes([old[-1] ^ 1]))
            with self.assertRaisesRegex(ValueError, 'hash differs'):
                self.publish()
            self.absent()
            path.write_bytes(old)

    def test_cancelled_preparation_never_publishes(self):
        self.publish()
        calls = 0

        def cancelled():
            nonlocal calls
            calls += 1
            return calls >= 4

        with self.assertRaises(InterruptedError):
            self.publish(cancelled=cancelled)
        self.absent()
        # Cancellation just before the atomic replace also removes the tmp.
        real_replace = receipts.os.replace

        def cancel_before_replace(*args):
            raise InterruptedError('interrupted before publication')

        with mock.patch.object(receipts.os, 'replace', side_effect=cancel_before_replace):
            with self.assertRaises(InterruptedError):
                self.publish()
        self.absent()
        self.assertIs(receipts.os.replace, real_replace)

    def test_read_failure_and_unpreserved_probe_never_publish(self):
        self.publish()
        with mock.patch.object(receipts, 'verify', side_effect=OSError('read failure')):
            with self.assertRaises(OSError):
                self.publish()
        self.absent()
        with mock.patch.object(receipts.os, 'utime', return_value=None):
            with self.assertRaisesRegex(ValueError, 'probe'):
                self.publish()
        self.absent()

    def test_replacement_during_verification_never_publishes(self):
        real = receipts.verify
        source = self.root / 'copy.ogg'

        def changed(*args):
            result = real(*args)
            if args[0] != source:
                stamp = source.stat().st_mtime + 4
                os.utime(source, (stamp, stamp))
            return result

        with mock.patch.object(receipts, 'verify', side_effect=changed):
            with self.assertRaisesRegex(ValueError, 'before receipt publication'):
                self.publish()
        self.absent()

    def test_header_disagreement_and_escaped_paths_rejected(self):
        path = next((self.assets / 'objects').glob('*.dtad'))
        payload = bytearray(path.read_bytes())
        struct.pack_into('<I', payload, 12, 44100)
        path.write_bytes(payload)
        with self.assertRaisesRegex(ValueError, 'header'):
            self.publish()
        self.absent()
        with self.assertRaises(ValueError):
            receipts.publish(self.root, self.assets, self.fixture.index_path)


if __name__ == '__main__':
    unittest.main()
