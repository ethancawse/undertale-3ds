"""Exercise the production C reader and exact bounded seeks against Python."""
import ctypes as c
import hashlib
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dspadpcm as dsp


class Info(c.Structure):
    _fields_ = [('rate', c.c_uint32), ('channels', c.c_uint32), ('frames', c.c_uint32),
                ('blocks', c.c_uint32), ('fileBytes', c.c_uint64), ('coefficients', (c.c_int16 * 16) * 2)]


class History(c.Structure):
    _fields_ = [('index', c.c_uint16), ('history0', c.c_int16), ('history1', c.c_int16)]


class Block(c.Structure):
    _fields_ = [('channel', c.POINTER(c.c_uint8) * 2), ('history', History * 2),
                ('frames', c.c_uint32), ('payloadBytes', c.c_uint32)]


READ = c.CFUNCTYPE(c.c_bool, c.c_void_p, c.c_uint64, c.c_void_p, c.c_uint32)


def runtime():
    source = dsp.ROOT / 'platform/3ds/source/z_adpcm_format.c'
    header = source.with_name('ctr_adpcm_format.h')
    digest = hashlib.sha256(source.read_bytes() + header.read_bytes()).hexdigest()[:16]
    library = dsp.ROOT / 'scratch/adpcm/host' / ('runtime-' + digest + ('.dll' if os.name == 'nt' else '.so'))
    library.parent.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which('gcc') or shutil.which('clang')
    if not compiler and os.name == 'nt':
        compiler = next((str(p) for p in (Path('C:/msys64/ucrt64/bin/gcc.exe'),
            Path('C:/devkitPro/msys2/ucrt64/bin/gcc.exe')) if p.is_file()), None)
    if not compiler:
        raise RuntimeError('Host C compiler required')
    if not library.exists():
        subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
            '-DCTR_ADPCM_HOST', '-shared', '-o', str(library), str(source)],
            stdin=subprocess.DEVNULL, check=True, timeout=25)
    directories = []
    if os.name == 'nt' and hasattr(os, 'add_dll_directory'):
        directories = [os.add_dll_directory(str(Path(compiler).parent))]
    try:
        lib = c.CDLL(str(library.resolve()))
    finally:
        for directory in directories:
            directory.close()
    lib.CtrAdpcm_parse.argtypes = [c.c_void_p, c.c_uint32, c.c_uint64, c.POINTER(Info)]
    lib.CtrAdpcm_blockRange.argtypes = [c.POINTER(Info), c.c_uint32, c.POINTER(c.c_uint64),
                                      c.POINTER(c.c_uint32), c.POINTER(c.c_uint32)]
    lib.CtrAdpcm_readBlock.argtypes = [c.POINTER(Info), READ, c.c_void_p, c.c_uint32,
                                     c.c_void_p, c.c_uint32, c.POINTER(Block)]
    lib.CtrAdpcm_decodeSpan.argtypes = [c.POINTER(Info), c.POINTER(Block), c.c_uint32,
                                      c.c_uint32, c.POINTER(c.c_int16), c.c_uint32]
    for name in ('parse', 'blockRange', 'readBlock', 'decodeSpan'):
        getattr(lib, 'CtrAdpcm_' + name).restype = c.c_bool
    return lib


class RuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.encoder, cls.lib = dsp.native(), runtime()

    def fixture(self, frames, channels):
        rng = random.Random(frames * 2 + channels)
        pcm = struct.pack(f'<{frames * channels}h', *(rng.randrange(-32768, 32768) for _ in range(frames * channels)))
        with tempfile.TemporaryDirectory(dir=dsp.ROOT / 'scratch/adpcm') as folder:
            path = Path(folder) / 'fixture.dtad'
            dsp.encode(pcm, 44100, channels, path, self.encoder)
            rendered, _, _ = dsp.read(path)
            return path.read_bytes(), rendered

    def parse(self, data, *, length=None):
        info = Info()
        ok = self.lib.CtrAdpcm_parse(data, len(data), len(data) if length is None else length, c.byref(info))
        return ok, info

    @staticmethod
    def reader(data):
        def read_at(_, offset, out, count):
            if offset > len(data) or count > len(data) - offset:
                return False
            c.memmove(out, data[offset:offset + count], count)
            return True
        return READ(read_at)

    def test_native_views_and_exact_seek_spans(self):
        for channels in (1, 2):
            for frames in (1, 13, 14, 15, 2043, 2044, 2045, 4097):
                with self.subTest(channels=channels, frames=frames):
                    data, expected = self.fixture(frames, channels)
                    ok, info = self.parse(data)
                    self.assertTrue(ok)
                    self.assertEqual((info.rate, info.channels, info.frames), (44100, channels, frames))
                    storage, block = c.create_string_buffer(2352), Block()
                    full = bytearray()
                    reader = self.reader(data)
                    for index in range(info.blocks):
                        self.assertTrue(self.lib.CtrAdpcm_readBlock(c.byref(info), reader, None,
                            index, storage, len(storage), c.byref(block)))
                        pcm = (c.c_int16 * (block.frames * channels))()
                        self.assertTrue(self.lib.CtrAdpcm_decodeSpan(c.byref(info), c.byref(block),
                            0, block.frames, pcm, block.frames))
                        whole = bytes(pcm)
                        full.extend(whole)
                        # Both nibble phases, DSP-frame boundaries and the last exact sample.
                        for first in sorted({0, min(1, block.frames - 1), min(13, block.frames - 1),
                                             min(14, block.frames - 1), block.frames - 1}):
                            count = min(19, block.frames - first)
                            seeked = (c.c_int16 * (count * channels))()
                            self.assertTrue(self.lib.CtrAdpcm_decodeSpan(c.byref(info), c.byref(block),
                                first, count, seeked, count))
                            self.assertEqual(bytes(seeked), whole[first * channels * 2:(first + count) * channels * 2])
                    self.assertEqual(full, expected)

    def test_header_bounds_reserved_fields_and_overflow(self):
        data, _ = self.fixture(2045, 1)
        for offset in (0, 4, 8, 24, 28, 32, 36, 72, 80, 120, 127):
            damaged = bytearray(data)
            damaged[offset] ^= 0x80
            self.assertFalse(self.parse(bytes(damaged))[0], offset)
        self.assertFalse(self.parse(data[:127])[0])
        self.assertFalse(self.parse(data, length=len(data) + 1)[0])
        self.assertFalse(self.parse(data, length=0xffffffffffffffff)[0])
        for frames in (0, 0xffffffff):
            damaged = bytearray(data)
            struct.pack_into('<I', damaged, 20, frames)
            self.assertFalse(self.parse(bytes(damaged))[0])
        for rate, channels in ((0, 1), (192001, 1), (44100, 0), (44100, 3)):
            damaged = bytearray(data)
            struct.pack_into('<II', damaged, 12, rate, channels)
            self.assertFalse(self.parse(bytes(damaged))[0])

    def test_block_corruption_short_reads_and_output_capacity(self):
        data, _ = self.fixture(2045, 2)
        ok, info = self.parse(data)
        self.assertTrue(ok)
        storage, block = c.create_string_buffer(2352), Block()
        for offset in (128, 130, 134, 136, 136 + 8 * 145):
            damaged = bytearray(data)
            damaged[offset] ^= 0x80
            self.assertFalse(self.lib.CtrAdpcm_readBlock(c.byref(info), self.reader(bytes(damaged)),
                None, 0, storage, len(storage), c.byref(block)), offset)
        self.assertFalse(self.lib.CtrAdpcm_readBlock(c.byref(info), self.reader(data[:-1]), None,
            1, storage, len(storage), c.byref(block)))
        self.assertFalse(self.lib.CtrAdpcm_readBlock(c.byref(info), self.reader(data), None,
            0, storage, len(storage) - 1, c.byref(block)))
        self.assertFalse(self.lib.CtrAdpcm_readBlock(c.byref(info), self.reader(data), None,
            info.blocks, storage, len(storage), c.byref(block)))
        self.assertTrue(self.lib.CtrAdpcm_readBlock(c.byref(info), self.reader(data), None,
            0, storage, len(storage), c.byref(block)))
        pcm = (c.c_int16 * 8)()
        for first, count, capacity in ((block.frames, 1, 4), (0xffffffff, 1, 4), (0, 5, 4), (0, 0xffffffff, 4)):
            self.assertFalse(self.lib.CtrAdpcm_decodeSpan(c.byref(info), c.byref(block),
                first, count, pcm, capacity))


if __name__ == '__main__':
    unittest.main()
