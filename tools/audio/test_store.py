"""Exercise the production SD reader with private synthetic staged files."""
import ctypes as c
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_catalog
import receipts
from test_runtime import Info

ROOT = Path(__file__).resolve().parents[2]


class Store(c.Structure):
    _fields_ = [('index', c.c_void_p), ('catalog', test_catalog.Catalog),
                ('root', c.c_char * 512), ('dataDir', c.c_char * 512),
                ('receipt', c.c_void_p), ('receiptBytes', c.c_uint32),
                ('stampMode', c.c_uint32), ('receiptResult', c.c_int)]


class File(c.Structure):
    _fields_ = [('file', c.c_void_p), ('bytes', c.c_uint32), ('sourceBytes', c.c_uint32),
                ('info', Info), ('mono', c.c_bool), ('header', c.c_uint8 * 128)]


def runtime(test_unknown=None):
    compiler = shutil.which('gcc') or shutil.which('clang')
    if not compiler:
        compiler = next((str(p) for p in (Path('C:/msys64/ucrt64/bin/gcc.exe'),
            Path('C:/devkitPro/msys2/ucrt64/bin/gcc.exe')) if p.is_file()), None)
    if not compiler:
        raise RuntimeError('Host C compiler required')
    sources = ['platform/3ds/source/z_adpcm_' + name + '.c' for name in ('store', 'catalog', 'format')]
    sources += ['engine/vendor/sha1/sha1.c', 'tools/audio/test_store.c']
    headers = ['platform/3ds/source/ctr_adpcm_' + name + '.h' for name in ('store', 'catalog', 'format')]
    switch = [] if test_unknown is None else ['-DCTRPORT_ADPCM_TEST_UNKNOWN_MTIME=' + str(int(test_unknown))]
    key = hashlib.sha256(b''.join((ROOT / p).read_bytes() for p in sources + headers)
                         + repr(switch).encode()).hexdigest()[:16]
    out = ROOT / 'scratch/adpcm/host' / ('store-' + key + ('.dll' if os.name == 'nt' else '.so'))
    out.parent.mkdir(parents=True, exist_ok=True)
    if not out.exists():
        subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
            '-DCTR_ADPCM_HOST', '-DCTR_ADPCM_STORE_TEST_RESIZE', *switch,
            '-Iengine/vendor/sha1', '-Iplatform/3ds/source', '-shared',
            *(['-Wl,--export-all-symbols'] if os.name == 'nt' else []), *sources, '-o', str(out)],
            cwd=ROOT, stdin=subprocess.DEVNULL, check=True, timeout=25)
    dirs = [os.add_dll_directory(str(Path(compiler).parent))] if os.name == 'nt' else []
    try:
        lib = c.CDLL(str(out))
    finally:
        for directory in dirs:
            directory.close()
    lib.CtrAdpcmStore_open.argtypes = [c.POINTER(Store), c.c_char_p, c.c_void_p, c.c_uint32, c.c_void_p]
    lib.CtrAdpcmStore_openFile.argtypes = [c.POINTER(Store), c.c_char_p, c.POINTER(File), c.POINTER(test_catalog.Asset)]
    lib.CtrAdpcmStore_openSound.argtypes = [c.POINTER(Store), c.c_uint32, c.POINTER(File), c.POINTER(test_catalog.Asset)]
    lib.CtrAdpcmStore_lookupFile.argtypes = [c.POINTER(Store), c.c_char_p, c.POINTER(test_catalog.Asset)]
    lib.CtrAdpcmStore_close.argtypes = [c.POINTER(Store)]
    lib.CtrAdpcmStore_closeFile.argtypes = [c.POINTER(File)]
    lib.CtrAdpcmStore_readAt.argtypes = [c.c_void_p, c.c_uint64, c.c_void_p, c.c_uint32]
    lib.CtrAdpcmStore_readAt.restype = c.c_bool
    lib.CtrAdpcmStore_mono.argtypes = [c.POINTER(File), c.POINTER(test_catalog.Asset)]
    lib.CtrAdpcmStore_mono.restype = c.c_bool
    lib.testStore_clock.argtypes = [c.c_int, c.c_int, c.c_uint64]
    return lib


class StoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        test_catalog.CatalogTests.setUpClass()
        cls.lib = runtime()

    def setUp(self):
        # Share only the synthetic WAV/data.win/staging fixture, not its tests.
        self.fixture = test_catalog.CatalogTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.lib.testStore_fail(0)
        self.lib.testStore_resizeMode(0, 0)
        self.lib.testStore_clock(0, 0, 0)
        self.store = Store()
        self.addCleanup(self.lib.CtrAdpcmStore_close, c.byref(self.store))
        self.file = File()
        self.addCleanup(self.lib.CtrAdpcmStore_closeFile, c.byref(self.file))
        self.asset = test_catalog.Asset()
        self.game = self.fixture.source
        shutil.copytree(self.fixture.out, self.game / 'audio-adpcm')
        self.index_path = next((self.game / 'audio-adpcm/index').glob('*.dtai'))
        result = receipts.publish(self.game, self.game / 'audio-adpcm', self.index_path)
        self.receipt_path = Path(result['path'])

    def open(self, data_win=None, sha=None):
        return self.lib.CtrAdpcmStore_open(c.byref(self.store),
            (data_win or self.game / 'data.win').as_posix().encode(),
            sha or self.fixture.sha, 1, self.lib.testStore_allocate)

    def file_open(self, name=None):
        return self.lib.CtrAdpcmStore_openFile(c.byref(self.store),
            (name or self.game / 'copy.ogg').as_posix().encode(), c.byref(self.file), c.byref(self.asset))

    def test_verified_file_random_access_and_independent_lifetime(self):
        self.assertEqual(self.open(), 0)
        allocations = self.lib.testStore_calls()
        self.assertEqual(self.file_open(), 0)
        self.assertEqual(self.lib.testStore_calls(), allocations)
        key = c.string_at(self.asset.key, 32).hex()
        payload = (self.game / 'audio-adpcm/objects' / (key + '.dtad')).read_bytes()
        self.assertEqual((self.file.bytes, self.file.info.frames, self.file.info.rate, self.file.info.channels),
                         (len(payload), 2045, 32000, 2))

        # Closing the catalog does not close a reader's dedicated descriptor.
        self.lib.CtrAdpcmStore_close(c.byref(self.store))
        for offset, size in ((128, 53), (0, 128), (len(payload) - 37, 37), (111, 221)):
            out = c.create_string_buffer(size)
            self.assertTrue(self.lib.CtrAdpcmStore_readAt(c.byref(self.file), offset, out, size))
            self.assertEqual(out.raw, payload[offset:offset + size])
        self.assertEqual(self.lib.testStore_calls(), allocations)
        self.assertEqual(self.lib.CtrAdpcmStore_openFile(c.byref(self.store), b'x', c.byref(self.file),
                                                       c.byref(self.asset)), 1)

    def test_metadata_preflight_is_not_source_verification(self):
        self.assertEqual(self.open(), 0)
        path = self.game / 'copy.ogg'
        path.unlink()
        self.assertEqual(self.lib.CtrAdpcmStore_lookupFile(c.byref(self.store), path.as_posix().encode(),
            c.byref(self.asset)), 0)
        self.assertFalse(self.file.file)
        self.assertEqual(self.file_open(), 6)

    def test_compaction_preserves_external_records_after_forced_pointer_moves(self):
        self.lib.testStore_resizeMode(0, 1)
        before = self.index_path.read_bytes(), self.receipt_path.read_bytes()
        self.assertEqual(self.open(), 0)
        self.assertEqual((self.store.catalog.sounds, self.store.catalog.files), (0, 2))
        self.assertEqual(self.store.receiptBytes, 128 + 2 * 32)
        expected_index = len(before[0]) - 128  # one embedded sound record
        self.assertEqual(self.lib.testStore_resizeCalls(), 2)
        self.assertEqual(self.lib.testStore_resizeBytes(), expected_index + self.store.receiptBytes)
        self.assertEqual(self.file_open(), 0)
        self.assertEqual((self.asset.frames, self.asset.path, self.asset.rate), (2045, b'copy.ogg', 32000))
        self.assertEqual((self.index_path.read_bytes(), self.receipt_path.read_bytes()), before)

    def test_failed_shrink_closes_all_metadata_and_falls_back(self):
        for fail_at in (1, 2):
            self.lib.testStore_resizeMode(fail_at, 0)
            with self.subTest(fail_at=fail_at):
                self.assertEqual(self.open(), 4)
                self.assertFalse(self.store.index)
                self.assertFalse(self.store.receipt)
                self.assertFalse(self.file.file)

    def test_discarded_embedded_records_are_still_fully_validated(self):
        data = bytearray(self.fixture.index)
        data[128:132] = (0).to_bytes(4, 'little')  # invalid nonzero body of an absent entry
        data[64:84] = hashlib.sha1(data[128:]).digest()
        self.index_path.write_bytes(data)
        self.assertEqual(self.open(), 3)
        self.assertFalse(self.store.index)

    def test_discarded_receipt_record_corruption_is_not_ignored(self):
        data = bytearray(self.receipt_path.read_bytes())
        data[128] ^= 1
        self.receipt_path.write_bytes(data)
        os.utime(self.receipt_path, (receipts.PROBE_UTC, receipts.PROBE_UTC))
        self.assertEqual(self.open(), 0)
        self.assertEqual(self.store.receiptResult, 12)
        self.assertEqual(self.file_open(), 12)
        self.assertFalse(self.file.file)

    def test_missing_corrupt_index_and_heap_failure_are_distinct(self):
        index = next((self.game / 'audio-adpcm/index').glob('*.dtai'))
        index.unlink()
        self.assertEqual(self.open(), 2)
        index.write_bytes(b'bad')
        self.assertEqual(self.open(), 3)
        self.assertFalse(self.store.index)
        index.write_bytes(self.fixture.index)
        self.lib.testStore_fail(1)
        self.assertEqual(self.open(), 4)
        self.assertFalse(self.store.index)
        self.lib.testStore_fail(0)
        self.assertEqual(self.open(sha=b'\0' * 32), 2)  # identity is in filename
        self.assertEqual(self.open(), 0)
        self.assertEqual(self.open(), 1)  # cannot overwrite a live catalog

    def test_chapter_parent_index_and_relative_source_lookup(self):
        chapter = self.game / 'chapter4_windows'
        chapter.mkdir()
        self.assertEqual(self.open(chapter / 'data.win'), 0)
        self.assertEqual(self.store.root.decode(), self.game.as_posix())
        self.assertEqual(self.file_open(), 0)

    def test_unconverted_path_and_sibling_prefix_rejected(self):
        self.assertEqual(self.open(), 0)
        for path in (self.game / 'bad.ogg', self.game / '../copy.ogg',
                     Path(str(self.game) + 'suffix') / 'copy.ogg', self.game / 'copy.ogg/extra'):
            self.assertEqual(self.file_open(path), 5)
            self.assertFalse(self.file.file)

    def test_chapter_parent_music_path_is_canonicalized_without_allocation(self):
        chapter = self.game / 'chapter1_windows'
        chapter.mkdir()
        self.assertEqual(self.open(chapter / 'data.win'), 0)
        allocations = self.lib.testStore_calls()
        for relative in ('chapter1_windows/../copy.ogg', './copy.ogg',
                         'chapter1_windows/./../copy.ogg', 'chapter1_windows//../copy.ogg'):
            self.assertEqual(self.file_open(self.game / relative), 0, relative)
            self.assertEqual(c.string_at(self.asset.path), b'copy.ogg')
            self.lib.CtrAdpcmStore_closeFile(c.byref(self.file))
        self.assertEqual(self.lib.testStore_calls(), allocations)

    def test_normalization_cannot_escape_and_reenter_or_accept_bad_components(self):
        chapter = self.game / 'chapter1_windows'
        chapter.mkdir()
        self.assertEqual(self.open(chapter / 'data.win'), 0)
        for suffix in ('chapter1_windows/../../' + self.game.name + '/copy.ogg',
                       'chapter1_windows/../copy.ogg/..',
                       'chapter1_windows/../bad:dir/../copy.ogg',
                       'chapter1_windows/../bad\\dir/../copy.ogg',
                       'x' * 512 + '/../copy.ogg'):
            self.assertEqual(self.file_open(self.game / suffix), 5, suffix)
            self.assertFalse(self.file.file)

    def test_chapter_basename_does_not_alias_a_root_asset(self):
        chapter = self.game / 'chapter1_windows'
        chapter.mkdir()
        shutil.copy2(self.game / 'copy.ogg', chapter / 'copy.ogg')
        self.assertEqual(self.open(chapter / 'data.win'), 0)
        self.assertEqual(self.file_open(chapter / 'copy.ogg'), 5)
        self.assertFalse(self.file.file)

    def test_component_walks_match_rooted_lexical_resolution(self):
        import itertools
        self.assertEqual(self.open(), 0)
        # Metadata only: synthetic directories need not exist to test lookup.
        for components in itertools.product(('', '.', '..', 'branch'), repeat=4):
            stack, escaped = [], False
            for item in components:
                if item == '..':
                    if not stack:
                        escaped = True
                        break
                    stack.pop()
                elif item and item != '.':
                    stack.append(item)
            suffix = '/'.join((*components, 'copy.ogg'))
            expected = 0 if not escaped and not stack else 5
            path = self.game.as_posix() + '/' + suffix
            self.assertEqual(self.lib.CtrAdpcmStore_lookupFile(c.byref(self.store), path.encode(),
                c.byref(self.asset)), expected, suffix)

    def test_changed_source_bytes_or_length_fall_back(self):
        self.assertEqual(self.open(), 0)
        source = self.game / 'copy.ogg'
        stamp = source.stat().st_mtime
        original = source.read_bytes()
        for data in (original[:-1], original + b'!', original[:-1] + bytes([original[-1] ^ 1])):
            source.write_bytes(data)
            os.utime(source, (stamp + 4, stamp + 4))
            self.assertEqual(self.file_open(), 13)
            self.assertFalse(self.file.file)
        source.write_bytes(original)
        os.utime(source, (stamp, stamp))
        self.assertEqual(self.file_open(), 0)

    def test_changed_payload_and_metadata_disagreement_fall_back(self):
        self.assertEqual(self.open(), 0)
        target = next((self.game / 'audio-adpcm/objects').glob('*.dtad'))
        original = target.read_bytes()
        stamp = target.stat().st_mtime
        for data in (original[:-1], original + b'!', original[:-1] + bytes([original[-1] ^ 1])):
            target.write_bytes(data)
            os.utime(target, (stamp + 4, stamp + 4))
            self.assertEqual(self.file_open(), 14)
            self.assertFalse(self.file.file)
        # Re-hash a changed payload to prove metadata, not just its SHA, is checked.
        changed = bytearray(original)
        changed[12:16] = (44100).to_bytes(4, 'little')
        target.write_bytes(changed)
        index = bytearray(self.fixture.index)
        # Locate the external copy.ogg record rather than relying on file order.
        records = 3
        for slot in range(1, records):
            start = 128 + slot * 128
            off = int.from_bytes(index[start + 20:start + 24], 'little')
            path = index[128 + records * 128 + off:].split(b'\0', 1)[0]
            if path == b'copy.ogg':
                index[start + 72:start + 92] = hashlib.sha1(changed).digest()
        index[64:84] = hashlib.sha1(index[128:]).digest()
        self.lib.CtrAdpcmStore_close(c.byref(self.store))
        next((self.game / 'audio-adpcm/index').glob('*.dtai')).write_bytes(index)
        # A synthetic receipt with matching catalog and metadata cannot bypass
        # the independent, bounded DTAD header check in the runtime.
        r = bytearray(self.receipt_path.read_bytes())
        r[32:52] = hashlib.sha1(index).digest()
        self.receipt_path.write_bytes(r)
        os.utime(self.receipt_path, (receipts.PROBE_UTC, receipts.PROBE_UTC))
        os.utime(target, (stamp, stamp))
        self.assertEqual(self.open(), 0)
        self.assertEqual(self.file_open(), 7)

    def test_missing_partial_stale_receipt_and_timestamp_form(self):
        original = self.receipt_path.read_bytes()
        for data in (None, original[:127], original[:-1], original + b'!',
                     original[:4] + (2).to_bytes(4, 'little') + original[8:],
                     original[:-1] + bytes([original[-1] ^ 1]),
                     original[:32] + b'\0' * 20 + original[52:]):
            self.receipt_path.unlink(missing_ok=True)
            if data is not None:
                self.receipt_path.write_bytes(data)
                os.utime(self.receipt_path, (receipts.PROBE_UTC, receipts.PROBE_UTC))
            self.assertEqual(self.open(), 0)  # catalog remains available
            self.assertEqual(self.file_open(), 12)
            self.assertFalse(self.file.file)
            self.lib.CtrAdpcmStore_close(c.byref(self.store))
        self.receipt_path.write_bytes(original)
        os.utime(self.receipt_path, (receipts.PROBE_UTC + 4, receipts.PROBE_UTC + 4))
        self.assertEqual(self.open(), 0)
        self.assertEqual(self.file_open(), 12)

    def test_missing_payload_falls_back_without_descriptor(self):
        self.assertEqual(self.open(), 0)
        payload = next((self.game / 'audio-adpcm/objects').glob('*.dtad'))
        payload.unlink()
        self.assertEqual(self.file_open(), 7)
        self.assertFalse(self.file.file)

    def test_receipt_heap_failure_falls_back_without_file_allocation(self):
        self.lib.testStore_fail(2)
        self.assertEqual(self.open(), 0)
        self.assertEqual(self.file_open(), 4)
        self.assertFalse(self.file.file)

    def test_bounds_short_reads_and_idempotent_close(self):
        self.assertEqual(self.open(), 0)
        self.assertEqual(self.file_open(), 0)
        out = c.create_string_buffer(129)
        for offset, count in ((self.file.bytes, 1), (self.file.bytes - 5, 6), (2**64 - 1, 1)):
            self.assertFalse(self.lib.CtrAdpcmStore_readAt(c.byref(self.file), offset, out, count))
        self.assertTrue(self.lib.CtrAdpcmStore_readAt(c.byref(self.file), self.file.bytes, None, 0))
        self.assertFalse(self.lib.CtrAdpcmStore_readAt(c.byref(self.file), 0, None, 1))
        # A short underlying read is failure even when it passes source bounds.
        actual = self.file.bytes
        self.file.bytes += 1
        self.assertFalse(self.lib.CtrAdpcmStore_readAt(c.byref(self.file), actual, out, 1))
        self.file.bytes = actual
        self.lib.CtrAdpcmStore_closeFile(c.byref(self.file))
        self.lib.CtrAdpcmStore_closeFile(c.byref(self.file))
        self.assertFalse(self.lib.CtrAdpcmStore_readAt(c.byref(self.file), 0, out, 1))

    def test_identical_channel_view_exact_bytes_including_final_short_block(self):
        self.assertEqual(self.open(), 0)
        self.assertEqual(self.file_open(), 0)
        key = c.string_at(self.asset.key, 32).hex()
        payload = (self.game / 'audio-adpcm/objects' / (key + '.dtad')).read_bytes()
        self.asset.flags &= ~4
        self.assertFalse(self.lib.CtrAdpcmStore_mono(c.byref(self.file), c.byref(self.asset)))
        self.asset.flags |= 4
        self.assertTrue(self.lib.CtrAdpcmStore_mono(c.byref(self.file), c.byref(self.asset)))
        self.assertFalse(self.lib.CtrAdpcmStore_mono(c.byref(self.file), c.byref(self.asset)))
        header = bytearray(payload[:128])
        header[16:20] = (1).to_bytes(4, 'little')
        header[80:128] = b'\0' * 48
        # 2044 full frames then a one-frame final block, no inserted samples.
        expected = bytes(header) + payload[128:128 + 1176] + payload[128 + 2352:128 + 2352 + 16]
        self.assertEqual(self.file.sourceBytes, len(expected))
        for offset, size in ((0, len(expected)), (100, 100), (128 + 1170, 20), (len(expected), 0)):
            out = c.create_string_buffer(size)
            self.assertTrue(self.lib.CtrAdpcmStore_readAt(c.byref(self.file), offset, out, size))
            self.assertEqual(out.raw, expected[offset:offset + size])
        self.assertEqual((self.file.info.frames, self.file.info.channels), (2045, 1))


class UnknownTimestampTests(StoreTests):
    @classmethod
    def setUpClass(cls):
        test_catalog.CatalogTests.setUpClass()
        cls.lib = runtime(1)

    def test_known_timestamp_modes_keep_both_record_offsets(self):
        original = self.receipt_path.read_bytes()
        for mode in (0, 1):
            data = bytearray(original)
            for offset in range(128, len(data), 32):
                data[offset:offset + 32] = b''.join(value.to_bytes(8, 'little')
                    for value in (100, 200, 100, 200))
            data[52:72] = hashlib.sha1(data[128:]).digest()
            self.receipt_path.write_bytes(data)
            probe = int.from_bytes(data[72 + mode * 8:80 + mode * 8], 'little')
            self.lib.testStore_clock(0, 1, probe)
            self.assertEqual(self.open(), 0)
            self.assertEqual(self.store.stampMode, mode)
            self.lib.testStore_clock(0, 1, (mode + 1) * 100)
            self.assertEqual(self.file_open(), 0)
            self.lib.CtrAdpcmStore_closeFile(c.byref(self.file))
            self.lib.testStore_clock(0, 1, (2 - mode) * 100)
            self.assertEqual(self.file_open(), 13)
            self.lib.CtrAdpcmStore_close(c.byref(self.store))

    def test_unknown_time_requires_emulator_detection(self):
        self.lib.testStore_clock(0, 1, 946684800)
        self.assertEqual(self.open(), 0)
        self.assertEqual(self.file_open(), 12)
        self.assertFalse(self.file.file)
        self.lib.CtrAdpcmStore_close(c.byref(self.store))
        self.lib.testStore_clock(1, 1, 946684800)
        self.assertEqual(self.open(), 0)
        self.assertEqual(self.store.stampMode, 2)
        self.assertEqual(self.file_open(), 0)

    def test_unknown_time_still_checks_size_and_exact_header(self):
        self.lib.testStore_clock(1, 1, 946684800)
        self.assertEqual(self.open(), 0)
        source = self.game / 'copy.ogg'
        original = source.read_bytes()
        source.write_bytes(original + b'!')
        self.assertEqual(self.file_open(), 13)
        source.write_bytes(original)
        payload = next((self.game / 'audio-adpcm/objects').glob('*.dtad'))
        data = bytearray(payload.read_bytes())
        data[12:16] = (44100).to_bytes(4, 'little')
        payload.write_bytes(data)
        self.assertEqual(self.file_open(), 7)

    def test_emulator_detection_does_not_accept_other_unknown_timestamps(self):
        for stamp in (0, 946684804):
            self.lib.testStore_clock(1, 1, stamp)
            self.assertEqual(self.open(), 0)
            self.assertEqual(self.file_open(), 12)
            self.lib.CtrAdpcmStore_close(c.byref(self.store))

    def test_timestamp_must_stay_unknown_after_receipt_admission(self):
        self.lib.testStore_clock(1, 1, 946684800)
        self.assertEqual(self.open(), 0)
        self.assertEqual(self.store.stampMode, 2)
        self.lib.testStore_clock(1, 1, 946684804)
        self.assertEqual(self.file_open(), 13)
        self.assertFalse(self.file.file)


if __name__ == '__main__':
    unittest.main()
