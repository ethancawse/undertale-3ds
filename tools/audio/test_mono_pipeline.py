"""Exact mono policy through preparation, catalog C parser and post-copy receipt."""
import contextlib
import ctypes as c
import hashlib
import io
import json
from pathlib import Path
import shutil
import struct
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mono_sfx as mono
import prepare
import receipts
import stage
import test_catalog


class PipelineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        test_catalog.CatalogTests.setUpClass()

    def setUp(self):
        self.fixture = test_catalog.CatalogTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.root = self.fixture.source
        self.assets = self.root / 'audio-adpcm'
        with contextlib.redirect_stdout(io.StringIO()):
            self.manifest = prepare.prepare(self.root, self.fixture.prepared,
                lib=self.fixture.encoder, mono_sfx=True)
            self.staged = stage.stage(self.root, self.fixture.prepared, self.fixture.out)
        shutil.copytree(self.fixture.out, self.assets)
        self.index = next((self.assets / 'index').glob('*.dtai'))
        self.target = self.assets / 'receipts' / self.index.with_suffix('.dtar').name

    def publish(self):
        return receipts.publish(self.root, self.assets, self.index)

    def test_projected_embedded_and_unprojected_alias_have_distinct_keys(self):
        row = self.manifest['sounds'][0]
        external = next(r for r in self.manifest['files'] if r['path'] == 'copy.ogg')
        self.assertEqual((row['channels'], row['source_channels'], row['source_bits']), (1, 2, 16))
        self.assertEqual(row['object_key'], mono.object_key(self.fixture.blob))
        self.assertNotEqual(row['object_key'], external['object_key'])
        self.assertEqual(external['object_key'], row['source_sha256'])
        self.assertEqual(len(self.staged['objects']), 2)
        data = self.index.read_bytes()
        ok, catalog = self.fixture.parse(data)
        self.assertTrue(ok)
        asset = test_catalog.Asset()
        self.assertTrue(self.fixture.lib.CtrAdpcmCatalog_sound(c.byref(catalog), 0, c.byref(asset)))
        self.assertEqual((asset.flags, asset.channels, asset.sourceChannels, asset.frames), (11, 1, 2, 2045))
        self.assertEqual((asset.sourceFileBytes, asset.sourceBits), ((self.root / 'data.win').stat().st_size, 16))
        result = self.publish()
        self.assertEqual(result['verified_files'], 4)
        self.assertNotEqual(self.target.read_bytes()[128:160], bytes(32))

    def test_catalog_refuses_inconsistent_projection(self):
        original = self.index.read_bytes()
        for offset, value in [(0, 3), (12, 2), (36, 1), (112, 0), (120, 24), (124, 0), (124, 65537)]:
            data = bytearray(original)
            struct.pack_into('<I', data, 128 + offset, value)
            data[64:84] = hashlib.sha1(data[128:]).digest()
            self.assertFalse(self.fixture.parse(bytes(data))[0], offset)

    def test_staging_checks_source_policy_and_original_pan(self):
        original = json.loads(json.dumps(self.manifest))
        for field, value in [('source_channels', 1), ('projection', 'unrecognized'),
                             ('object_key', '0' * 64), ('frames', 2044), ('seam_max_step', -1)]:
            damaged = json.loads(json.dumps(original))
            damaged['sounds'][0][field] = value
            (self.fixture.prepared / 'manifest.json').write_text(json.dumps(damaged))
            with self.assertRaises(ValueError):
                stage.stage(self.root, self.fixture.prepared, self.fixture.out)

    def test_receipt_rechecks_seam_statistic(self):
        data = bytearray(self.index.read_bytes())
        struct.pack_into('<I', data, 128 + 124, 65536)
        data[64:84] = hashlib.sha1(data[128:]).digest()
        self.index.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'seam statistic differs'):
            self.publish()
        self.assertFalse(self.target.exists())

    def test_receipt_never_publishes_for_stale_embedded_source_or_read_failure(self):
        self.publish()
        with patch.object(receipts, 'verify_span', side_effect=OSError('embedded read failure')):
            with self.assertRaises(OSError):
                self.publish()
        self.assertFalse(self.target.exists())
        source = self.root / 'data.win'
        blob = bytearray(source.read_bytes())
        blob[-1] ^= 1
        source.write_bytes(blob)
        with self.assertRaisesRegex(ValueError, 'hash differs'):
            self.publish()
        self.assertFalse(self.target.exists())

    def test_true_stereo_cannot_claim_projection_even_with_updated_hashes(self):
        source = self.root / 'data.win'
        blob = bytearray(source.read_bytes())
        blob[-2] ^= 1
        source.write_bytes(blob)
        span = bytes(blob[96:])
        data = bytearray(self.index.read_bytes())
        data[128 + 40:128 + 72] = bytes.fromhex(mono.object_key(span))
        data[128 + 92:128 + 112] = hashlib.sha1(span).digest()
        data[64:84] = hashlib.sha1(data[128:]).digest()
        self.index.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'not the declared exact mono projection'):
            self.publish()
        self.assertFalse(self.target.exists())


if __name__ == '__main__':
    unittest.main()
