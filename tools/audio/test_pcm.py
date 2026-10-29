"""Check incremental cache-piece decoding against the independent reference."""
import ctypes as c
import hashlib
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dspadpcm as dsp
import test_runtime as format_tests

Info = format_tests.Info


class Cursor(c.Structure):
    _fields_ = [('info', Info), ('data', c.c_void_p),
                ('bytes', c.c_uint32), ('position', c.c_uint32),
                ('blockFirst', c.c_uint32), ('blockFrames', c.c_uint32),
                ('channel', c.c_void_p * 2), ('history', (c.c_int32 * 2) * 2), ('ready', c.c_bool)]


def runtime():
    root = dsp.ROOT / 'platform/3ds/source'
    sources = [root / ('z_adpcm_' + name + '.c') for name in ('pcm', 'format')]
    headers = [root / ('ctr_adpcm_' + name + '.h') for name in ('pcm', 'format')]
    signature = hashlib.sha256(b''.join(p.read_bytes() for p in sources + headers)).hexdigest()[:16]
    path = dsp.ROOT / 'scratch/adpcm/host' / ('pcm-' + signature + ('.dll' if os.name == 'nt' else '.so'))
    path.parent.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which('gcc') or shutil.which('clang')
    if not compiler and os.name == 'nt':
        compiler = next((str(p) for p in (Path('C:/msys64/ucrt64/bin/gcc.exe'),
            Path('C:/devkitPro/msys2/ucrt64/bin/gcc.exe')) if p.is_file()), None)
    if not compiler:
        raise RuntimeError('Host C compiler required')
    if not path.exists():
        subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
            '-DCTR_ADPCM_HOST', '-shared', '-o', str(path), *map(str, sources)],
            check=True, stdin=subprocess.DEVNULL, timeout=25)
    directories = []
    if os.name == 'nt' and hasattr(os, 'add_dll_directory'):
        directories = [os.add_dll_directory(str(Path(compiler).parent))]
    try:
        lib = c.CDLL(str(path.resolve()))
    finally:
        for directory in directories:
            directory.close()
    lib.CtrAdpcmPcm_open.argtypes = [c.POINTER(Cursor), c.c_void_p, c.c_uint32]
    lib.CtrAdpcmPcm_read.argtypes = [c.POINTER(Cursor), c.c_uint32, c.c_uint32,
                                    c.POINTER(c.c_int16), c.c_uint32, c.POINTER(c.c_uint32)]
    lib.CtrAdpcmPcm_open.restype = lib.CtrAdpcmPcm_read.restype = c.c_bool
    return lib


class PcmTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.encoder, cls.lib = dsp.native(), runtime()

    fixture = format_tests.RuntimeTests.fixture

    def opened(self, data):
        cursor = Cursor()
        # Retain the bytes with the cursor: C deliberately borrows them.
        cursor.owner = data
        self.assertTrue(self.lib.CtrAdpcmPcm_open(c.byref(cursor), data, len(data)))
        return cursor

    def piece(self, cursor, first, count):
        output = (c.c_int16 * (count * cursor.info.channels + 4))()
        for i in range(len(output)):
            output[i] = 12345
        work = c.c_uint32(0xffffffff)
        self.assertTrue(self.lib.CtrAdpcmPcm_read(c.byref(cursor), first, count, output, count, c.byref(work)))
        self.assertEqual(list(output)[-4:], [12345] * 4)
        return bytes(output)[:count * cursor.info.channels * 2], work.value

    def test_sequential_pieces_do_not_redecode_block_prefixes(self):
        for channels in (1, 2):
            for frames in (1, 13, 14, 15, 2043, 2044, 2045, 8179):
                with self.subTest(channels=channels, frames=frames):
                    data, expected = self.fixture(frames, channels)
                    cursor = self.opened(data)
                    at, decoded, output = 0, 0, bytearray()
                    rng = random.Random(frames + channels)
                    while at < frames:
                        n = min(frames - at, rng.choice((1, 7, 14, 133, 217, 654, 1024)))
                        piece, work = self.piece(cursor, at, n)
                        self.assertEqual(work, n)
                        output.extend(piece)
                        decoded += work
                        at += n
                    self.assertEqual(output, expected)
                    self.assertEqual(decoded, frames)
                    self.assertEqual(cursor.position, frames)
                    self.assertEqual(self.piece(cursor, frames, 0), (b'', 0))

    def test_exact_seeks_loops_and_interleaved_instances(self):
        data, expected = self.fixture(8187, 2)
        cursors = [self.opened(data), self.opened(data)]
        ranges = [(2040, 17), (1, 1), (2030, 1024), (8179, 8), (0, 217), (217, 1), (4099, 13)]
        rng = random.Random(92)
        ranges += [(at := rng.randrange(8187), rng.randrange(min(1024, 8187 - at) + 1)) for _ in range(100)]
        for instance, cursor in enumerate(cursors):
            for at, n in ranges[::1 if instance == 0 else -1]:
                old = cursor.position
                old_block = cursor.blockFirst // 2044
                piece, work = self.piece(cursor, at, n)
                self.assertEqual(piece, expected[at * 4:(at + n) * 4])
                skip = at - old if old <= at and old_block == at // 2044 else at % 2044
                self.assertEqual(work, skip + n if n else 0)
            for _ in range(50):
                output = bytearray()
                for at in range(0, 8187, 654):
                    piece, _ = self.piece(cursor, at, min(654, 8187 - at))
                    output.extend(piece)
                self.assertEqual(output, expected)

    def test_rejects_malformed_cache_and_invalid_reads(self):
        data, _ = self.fixture(4097, 2)
        cursor = self.opened(data)
        snapshot = bytes(cursor)
        for offset in (0, 4, 72, 126, 128, 134, 136, len(data) - 9):
            bad = bytearray(data)
            bad[offset] = 0xff
            self.assertFalse(self.lib.CtrAdpcmPcm_open(c.byref(cursor), bytes(bad), len(bad)), offset)
            self.assertEqual(bytes(cursor), snapshot)
        self.assertFalse(self.lib.CtrAdpcmPcm_open(None, data, len(data)))
        self.assertFalse(self.lib.CtrAdpcmPcm_open(c.byref(cursor), None, len(data)))
        output = (c.c_int16 * 20)(*([12345] * 20))
        for first, count, capacity, buffer in ((4098, 0, 0, None), (4097, 1, 1, output),
                (0, 0xffffffff, 0xffffffff, output), (0, 5, 4, output), (0, 1, 1, None)):
            work = c.c_uint32(123)
            self.assertFalse(self.lib.CtrAdpcmPcm_read(c.byref(cursor), first, count, buffer, capacity, c.byref(work)))
            self.assertEqual(work.value, 0)
            self.assertEqual(bytes(cursor), snapshot)
            self.assertEqual(list(output), [12345] * 20)
        self.assertFalse(self.lib.CtrAdpcmPcm_read(None, 0, 0, None, 0, None))
        self.assertFalse(self.lib.CtrAdpcmPcm_read(c.byref(Cursor()), 0, 0, None, 0, None))
        self.assertTrue(self.lib.CtrAdpcmPcm_read(c.byref(cursor), 0, 0, None, 0, None))

    def test_full_coefficient_range_and_saturation(self):
        frames = 43
        for pair in ((32767, -32768), (-32768, 32767), (32767, 32767), (-32768, -32768)):
            header = bytearray(128)
            dsp.HEADER.pack_into(header, 0, b'DTAD', 1, 128, 44100, 1, frames, 2044, 1, 0)
            coefs = pair * 8
            struct.pack_into('<16h', header, 40, *coefs)
            body = bytes([0x7f, 0x87, 0x98, 0x76, 0x54, 0x32, 0x10, 0xfe]) * 4
            data = bytes(header) + struct.pack('<HhhH', body[0], 0, 0, 0) + body
            expected, _ = dsp.decode_channel(body, frames, coefs)
            cursor = self.opened(data)
            result = bytearray()
            for at in range(frames):
                piece, work = self.piece(cursor, at, 1)
                self.assertEqual(work, 1)
                result.extend(piece)
            self.assertEqual(result, struct.pack('<43h', *expected))


if __name__ == '__main__':
    unittest.main()
