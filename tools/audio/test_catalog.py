"""Synthetic staging, runtime lookup and stale-asset rejection gates."""
import contextlib
import ctypes as c
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import wave

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dspadpcm as dsp
import prepare
import stage
from test_prepare import fixture


class Catalog(c.Structure):
    _fields_ = [('records', c.c_void_p), ('strings', c.c_void_p), ('sounds', c.c_uint32),
                ('files', c.c_uint32), ('stringBytes', c.c_uint32)]


class Asset(c.Structure):
    _fields_ = [(name, c.c_uint32) for name in
                ('flags', 'frames', 'rate', 'channels', 'bytes', 'sourceBytes', 'sourceChannels')]
    _fields_ += [('sourceOffset', c.c_uint64), ('path', c.c_char_p), ('key', c.c_void_p),
                 ('payloadSha1', c.c_void_p), ('sourceSha1', c.c_void_p)]
    _fields_ += [('sourceFileBytes', c.c_uint64), ('sourceBits', c.c_uint32), ('seamMaxStep', c.c_uint32)]


def runtime():
    compiler = shutil.which('gcc') or shutil.which('clang')
    if not compiler:
        compiler = next((str(p) for p in (Path('C:/msys64/ucrt64/bin/gcc.exe'),
            Path('C:/devkitPro/msys2/ucrt64/bin/gcc.exe')) if p.is_file()), None)
    if not compiler:
        raise RuntimeError('Host C compiler required')
    files = [dsp.ROOT / name for name in ('platform/3ds/source/z_adpcm_catalog.c',
        'platform/3ds/source/ctr_adpcm_catalog.h', 'engine/vendor/sha1/sha1.c', 'engine/vendor/sha1/sha1.h')]
    key = hashlib.sha256(b''.join(p.read_bytes() for p in files)).hexdigest()[:16]
    out = dsp.ROOT / 'scratch/adpcm/host' / ('catalog-' + key + ('.dll' if os.name == 'nt' else '.so'))
    out.parent.mkdir(parents=True, exist_ok=True)
    if not out.exists():
        subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-DCTR_ADPCM_HOST',
            '-Iengine/vendor/sha1', '-shared', 'platform/3ds/source/z_adpcm_catalog.c',
            'engine/vendor/sha1/sha1.c', '-o', str(out)], cwd=dsp.ROOT,
            stdin=subprocess.DEVNULL, check=True, timeout=25)
    directories = []
    if os.name == 'nt' and hasattr(os, 'add_dll_directory'):
        directories.append(os.add_dll_directory(str(Path(compiler).parent)))
    try:
        lib = c.CDLL(str(out))
    finally:
        for directory in directories:
            directory.close()
    lib.CtrAdpcmCatalog_parse.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32, c.POINTER(Catalog)]
    lib.CtrAdpcmCatalog_sound.argtypes = [c.POINTER(Catalog), c.c_uint32, c.POINTER(Asset)]
    lib.CtrAdpcmCatalog_file.argtypes = [c.POINTER(Catalog), c.c_char_p, c.POINTER(Asset)]
    for name in ('parse', 'sound', 'file'):
        getattr(lib, 'CtrAdpcmCatalog_' + name).restype = c.c_bool
    return lib


class CatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.encoder, cls.lib = dsp.native(), runtime()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir=dsp.ROOT / 'scratch/adpcm')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source, self.prepared, self.out = (self.root / p for p in ('source', 'prepared', 'sd-audio'))
        self.source.mkdir()
        stream = io.BytesIO()
        with wave.open(stream, 'wb') as sound:
            sound.setparams((2, 2, 32000, 0, 'NONE', 'not compressed'))
            sound.writeframes(struct.pack('<4090h', *(v for i in range(2045) for v in (i % 19, i % 19))))
        self.blob = stream.getvalue()
        (self.source / 'data.win').write_bytes(fixture(self.blob))
        (self.source / 'copy.ogg').write_bytes(self.blob)
        (self.source / 'bad.ogg').write_bytes(b'unsupported')
        with contextlib.redirect_stdout(io.StringIO()):
            self.manifest = prepare.prepare(self.source, self.prepared, lib=self.encoder)
            self.result = stage.stage(self.source, self.prepared, self.out)
        self.index_path = self.out / self.result['indexes'][0]['path']
        self.index = self.index_path.read_bytes()
        self.sha = bytes.fromhex(self.result['indexes'][0]['data_win_sha256'])

    def parse(self, data, *, sha=None, count=1):
        catalog = Catalog()
        ok = self.lib.CtrAdpcmCatalog_parse(data, len(data), sha or self.sha, count, c.byref(catalog))
        return ok, catalog

    def test_staged_sources_payloads_and_runtime_lookups(self):
        self.assertFalse(self.result['runtime_selected'])
        self.assertEqual(len(self.result['objects']), 1)
        ok, catalog = self.parse(self.index)
        self.assertTrue(ok)
        self.assertEqual((catalog.sounds, catalog.files), (1, 2))
        asset = Asset()
        self.assertTrue(self.lib.CtrAdpcmCatalog_sound(c.byref(catalog), 0, c.byref(asset)))
        self.assertEqual((asset.flags, asset.frames, asset.rate, asset.channels), (7, 2045, 32000, 2))
        self.assertEqual((asset.sourceChannels, asset.path, asset.sourceOffset, asset.sourceBytes),
                         (2, b'data.win', 96, len(self.blob)))
        self.assertEqual(c.string_at(asset.sourceSha1, 20), hashlib.sha1(self.blob).digest())
        key = c.string_at(asset.key, 32).hex()
        payload = (self.out / 'objects' / (key + '.dtad')).read_bytes()
        self.assertEqual(c.string_at(asset.payloadSha1, 20), hashlib.sha1(payload).digest())
        self.assertEqual(asset.bytes, len(payload))
        self.assertFalse(self.lib.CtrAdpcmCatalog_sound(c.byref(catalog), 1, c.byref(asset)))
        self.assertTrue(self.lib.CtrAdpcmCatalog_file(c.byref(catalog), b'copy.ogg', c.byref(asset)))
        self.assertEqual((asset.flags, asset.path, asset.sourceOffset), (5, b'copy.ogg', 0))
        self.assertFalse(self.lib.CtrAdpcmCatalog_file(c.byref(catalog), b'bad.ogg', c.byref(asset)))
        self.assertFalse(self.lib.CtrAdpcmCatalog_file(c.byref(catalog), b'../copy.ogg', c.byref(asset)))
        with contextlib.redirect_stdout(io.StringIO()):
            repeated = stage.stage(self.source, self.prepared, self.out)
        self.assertEqual(repeated, self.result)
        self.assertEqual(self.index_path.read_bytes(), self.index)

    def test_damaged_or_wrong_indexes_and_malicious_paths(self):
        self.assertFalse(self.parse(self.index, sha=b'\0' * 32)[0])
        self.assertFalse(self.parse(self.index, count=2)[0])
        self.assertFalse(self.parse(self.index[:-1])[0])
        for at in (0, 4, 8, 12, 16, 20, 24, 28, 32, 64, 84, 127, 128, len(self.index) - 1):
            damaged = bytearray(self.index)
            damaged[at] ^= 0x80
            self.assertFalse(self.parse(bytes(damaged))[0], at)
        # Recomputed checksums distinguish structural validation from damage detection.
        def checked(data):
            data[64:84] = hashlib.sha1(data[128:]).digest()
            return bytes(data)
        for field, value in ((0, 9), (4, 0), (8, 0), (12, 3), (16, 129), (20, 0xffffffff),
                             (32, 0), (36, 1), (112, 1)):
            damaged = bytearray(self.index)
            struct.pack_into('<I', damaged, 128 + field, value)
            self.assertFalse(self.parse(checked(damaged))[0], field)
        damaged = bytearray(self.index)
        damaged[128 + 128 + 112] = 1 # Nonzero fallback slot.
        self.assertFalse(self.parse(checked(damaged))[0])
        records = 128 * 3
        for path in (b'../a.win', b'/ata.win', b'a\\ta.win', b'c:ta.win', b'./ta.win', b'data.wi\x01'):
            damaged = bytearray(self.index)
            damaged[128 + records:128 + records + 8] = path
            self.assertFalse(self.parse(checked(damaged))[0], path)

    def test_changed_sources_and_corrupt_cache(self):
        target = next((self.out / 'objects').glob('*.dtad'))
        target.write_bytes(b'broken staged copy')
        with contextlib.redirect_stdout(io.StringIO()):
            stage.stage(self.source, self.prepared, self.out)
        self.assertEqual(hashlib.sha256(target.read_bytes()).hexdigest(),
                         next(iter(self.result['objects'].values()))['sha256'])
        original = (self.source / 'copy.ogg').read_bytes()
        (self.source / 'copy.ogg').write_bytes(original + b'changed')
        with self.assertRaisesRegex(ValueError, 'source changed'):
            stage.stage(self.source, self.prepared, self.out)
        (self.source / 'copy.ogg').write_bytes(original)
        (self.source / 'data.win').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'data.win changed'):
            stage.stage(self.source, self.prepared, self.out)

    def test_output_ownership_paths_and_prepared_hash(self):
        for out in (self.source / 'out', self.prepared / 'out', self.root):
            with self.assertRaises(ValueError):
                stage.stage(self.source, self.prepared, out)
        unowned = self.root / 'unowned'
        unowned.mkdir()
        (unowned / 'keep.txt').write_text('keep')
        with self.assertRaisesRegex(ValueError, 'not an owned'):
            stage.stage(self.source, self.prepared, unowned)
        self.assertEqual((unowned / 'keep.txt').read_text(), 'keep')
        for name in ('../bad', '/bad', 'C:/bad', 'a\\b', 'a/./b', 'a//b', 'a/../b', ''):
            with self.assertRaises(ValueError):
                stage.relative(name)
        (self.prepared / next(iter(self.manifest['objects'].values()))['object']).write_bytes(b'bad')
        with self.assertRaisesRegex(ValueError, 'payload hash'):
            stage.stage(self.source, self.prepared, self.out)

    def test_audio_group_changes_with_unchanged_data_win(self):
        data = bytearray((self.source / 'data.win').read_bytes())
        struct.pack_into('<i', data, 24 + 28, 1)
        (self.source / 'data.win').write_bytes(data)
        group = self.source / 'audiogroup1.dat'
        group.write_bytes(fixture(self.blob))
        with contextlib.redirect_stdout(io.StringIO()):
            prepare.prepare(self.source, self.prepared, lib=self.encoder)
            result = stage.stage(self.source, self.prepared, self.out)
        index = (self.out / result['indexes'][0]['path']).read_bytes()
        sha = bytes.fromhex(result['indexes'][0]['data_win_sha256'])
        ok, catalog = self.parse(index, sha=sha)
        self.assertTrue(ok)
        asset = Asset()
        self.assertTrue(self.lib.CtrAdpcmCatalog_sound(c.byref(catalog), 0, c.byref(asset)))
        self.assertEqual((asset.path, asset.sourceOffset), (b'audiogroup1.dat', 96))
        original_win = (self.source / 'data.win').read_bytes()
        group.write_bytes(fixture(self.blob[:-1] + b'changed'))
        with self.assertRaisesRegex(ValueError, 'source changed'):
            stage.stage(self.source, self.prepared, self.out)
        self.assertEqual((self.source / 'data.win').read_bytes(), original_win)

    def test_stale_encoder_and_inconsistent_alias_metadata(self):
        manifest = self.prepared / 'manifest.json'
        bad = dict(self.manifest, encoder_signature='0' * 64)
        manifest.write_text(json.dumps(bad))
        with self.assertRaisesRegex(ValueError, 'encoder changed'):
            stage.stage(self.source, self.prepared, self.out)
        bad = json.loads(json.dumps(self.manifest))
        bad['sounds'][0]['frames'] += 1
        manifest.write_text(json.dumps(bad))
        with self.assertRaisesRegex(ValueError, 'Inconsistent prepared object aliases'):
            stage.stage(self.source, self.prepared, self.out)


if __name__ == '__main__':
    unittest.main()
