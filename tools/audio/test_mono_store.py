"""Production receipt-backed mono admission, using only synthetic sources."""
import ctypes as c
import os
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import receipts
import test_catalog
import test_mono_pipeline
import test_store


class MonoStoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        test_mono_pipeline.PipelineTests.setUpClass()
        cls.lib = test_store.runtime()

    def setUp(self):
        self.fixture = test_mono_pipeline.PipelineTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.publish()
        self.lib.testStore_fail(0)
        self.lib.testStore_resizeMode(0, 0)
        self.lib.testStore_clock(0, 0, 0)
        self.store = test_store.Store()
        self.addCleanup(self.lib.CtrAdpcmStore_close, c.byref(self.store))
        self.file = test_store.File()
        self.addCleanup(self.lib.CtrAdpcmStore_closeFile, c.byref(self.file))
        self.asset = test_catalog.Asset()

    def open(self):
        return self.lib.CtrAdpcmStore_open(c.byref(self.store),
            (self.fixture.root / 'data.win').as_posix().encode(),
            self.fixture.fixture.sha, 1, self.lib.testStore_allocate)

    def sound(self, sound=0):
        return self.lib.CtrAdpcmStore_openSound(c.byref(self.store), sound,
            c.byref(self.file), c.byref(self.asset))

    def payload(self):
        key = self.fixture.manifest['sounds'][0]['object_key']
        return self.fixture.assets / 'objects' / (key + '.dtad')

    def test_admission_retains_original_pan_and_exact_sample_metadata(self):
        self.assertEqual(self.open(), 0)
        self.assertEqual((self.store.catalog.sounds, self.store.catalog.files), (1, 2))
        self.assertEqual(self.store.receiptBytes, 128 + 3 * 32)
        allocations = self.lib.testStore_calls()
        self.assertEqual(self.sound(), 0)
        self.assertEqual(self.lib.testStore_calls(), allocations)
        self.assertEqual((self.asset.sourceChannels, self.asset.channels, self.asset.sourceBits), (2, 1, 16))
        self.assertEqual((self.file.info.frames, self.file.info.rate, self.file.info.channels), (2045, 32000, 1))
        data = self.payload().read_bytes()
        out = c.create_string_buffer(len(data))
        self.assertTrue(self.lib.CtrAdpcmStore_readAt(c.byref(self.file), 0, out, len(data)))
        self.assertEqual(out.raw, data)
        self.assertEqual(self.sound(), 1)  # Never overwrite a leased descriptor.

    def test_no_wav_scan_after_preparation_and_external_alias_unchanged(self):
        # Deliberately preserve all metadata while replacing the source body.
        # Receipts authorize content at preparation; runtime must not read/hash it.
        source = self.fixture.root / 'data.win'
        stamp = source.stat()
        blob = bytearray(source.read_bytes())
        blob[-1] ^= 1
        source.write_bytes(blob)
        os.utime(source, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        self.assertEqual(self.open(), 0)
        self.assertEqual(self.sound(), 0)
        self.lib.CtrAdpcmStore_closeFile(c.byref(self.file))
        self.assertEqual(self.lib.CtrAdpcmStore_openFile(c.byref(self.store),
            (self.fixture.root / 'copy.ogg').as_posix().encode(),
            c.byref(self.file), c.byref(self.asset)), 0)
        self.assertEqual((self.asset.channels, self.asset.sourceChannels), (2, 2))

    def test_stale_source_size_or_timestamp_falls_back(self):
        self.assertEqual(self.open(), 0)
        source = self.fixture.root / 'data.win'
        stamp = source.stat()
        os.utime(source, (stamp.st_atime, stamp.st_mtime + 4))
        self.assertEqual(self.sound(), 13)
        self.assertFalse(self.file.file)
        os.utime(source, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        with source.open('ab') as stream:
            stream.write(b'x')
        self.assertEqual(self.sound(), 13)
        self.assertFalse(self.file.file)

    def test_missing_partial_receipt_and_missing_payload_fall_back(self):
        original = self.fixture.target.read_bytes()
        self.fixture.target.unlink()
        self.assertEqual(self.open(), 0)
        self.assertEqual(self.sound(), 12)
        self.lib.CtrAdpcmStore_close(c.byref(self.store))
        self.fixture.target.write_bytes(original[:128])
        self.assertEqual(self.open(), 0)
        self.assertEqual(self.sound(), 12)
        self.lib.CtrAdpcmStore_close(c.byref(self.store))
        self.fixture.publish()
        self.assertEqual(self.open(), 0)
        self.payload().unlink()
        self.assertEqual(self.sound(), 7)
        self.assertFalse(self.file.file)

    def test_stale_payload_and_exact_header_validation(self):
        self.assertEqual(self.open(), 0)
        path = self.payload()
        stamp = path.stat()
        os.utime(path, (stamp.st_atime, stamp.st_mtime + 4))
        self.assertEqual(self.sound(), 14)
        blob = bytearray(path.read_bytes())
        struct.pack_into('<I', blob, 12, 32001)  # Valid container, wrong rate.
        path.write_bytes(blob)
        os.utime(path, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        self.assertEqual(self.sound(), 7)
        self.assertFalse(self.file.file)

    def test_out_of_range_and_legacy_embedded_never_admitted(self):
        self.assertEqual(self.open(), 0)
        self.assertEqual(self.sound(1), 5)
        self.assertEqual(self.sound(0xffffffff), 5)
        self.lib.CtrAdpcmStore_close(c.byref(self.store))
        # Existing legacy fixture converts embedded stereo without projection.
        legacy = test_store.StoreTests()
        legacy.lib = self.lib
        legacy.setUp()
        self.addCleanup(legacy.doCleanups)
        self.assertEqual(legacy.open(), 0)
        self.assertEqual(self.lib.CtrAdpcmStore_openSound(c.byref(legacy.store), 0,
            c.byref(self.file), c.byref(self.asset)), 5)


if __name__ == '__main__':
    unittest.main()
