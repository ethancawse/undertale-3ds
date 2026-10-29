"""Opt-in offline DSP-ADPCM conversion. Derived audio belongs outside the repo.

The .dtad v1 container carries exact sample counts/rate, independent channel
coefficients and self-contained block histories. See FORMAT.md. Encoding is
native host C; decoding here is an independent integer reference.
"""
from __future__ import annotations

import array
import ctypes
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import wave

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BLOCK_FRAMES = 2044  # 146 complete DSP frames, below the runtime's 4096-frame buffers.
HEADER_BYTES = 128
HEADER = struct.Struct('<4sIIIIQIII')
CONTEXT = struct.Struct('<HhhH')


# -ffp-contract=off: no fused multiply-add, so the double-precision predictor training rounds the same on every host
# (clang on arm64 macOS and GCC on arm64 Linux fuse by default; x86-64 without -march does not). The release table
# (tools/release/sfxpack-versions.json) relies on byte-identical encodes on every builder platform.
CFLAGS = ['-std=c11', '-O3', '-ffp-contract=off', '-Wall', '-Wextra', '-Werror', '-shared']


def native(build=None, library=None):
    """The encoder library: `library` (a prebuilt dspadpcm built from dspadpcm.c with CFLAGS, for hosts without a C
    compiler) or one compiled into `build` (private output) with the host's gcc/clang."""
    if library is not None:
        return _load(Path(library))
    build = Path(build) if build else ROOT / 'scratch/adpcm/host'
    build.mkdir(parents=True, exist_ok=True)
    source = HERE / 'dspadpcm.c'
    library = build / ('dspadpcm.dll' if os.name == 'nt' else 'dspadpcm.so')
    stamp = library.with_suffix('.sha256')
    signature = hashlib.sha256(source.read_bytes() + ' '.join(CFLAGS).encode()).hexdigest()
    if not library.exists() or not stamp.exists() or stamp.read_text() != signature:
        compiler = shutil.which('gcc') or shutil.which('clang')
        if not compiler and os.name == 'nt':
            compiler = next((str(p) for p in (Path('C:/msys64/ucrt64/bin/gcc.exe'),
                Path('C:/devkitPro/msys2/ucrt64/bin/gcc.exe')) if p.is_file()), None)
        if not compiler:
            raise RuntimeError('A host C compiler is required for experimental ADPCM conversion')
        subprocess.run([compiler] + CFLAGS + (['-static-libgcc'] if os.name == 'nt' else [])
                       + ['-o', str(library), str(source), '-lm'],
                       stdin=subprocess.DEVNULL, check=True, timeout=60)
        stamp.write_text(signature)
    return _load(library)


def _load(library):
    # MinGW's DLL dependencies live beside the compiler; never alter global PATH.
    dll_dirs = []
    if os.name == 'nt' and hasattr(os, 'add_dll_directory'):
        for folder in ('C:/msys64/ucrt64/bin', 'C:/devkitPro/msys2/ucrt64/bin'):
            if Path(folder).is_dir():
                dll_dirs.append(os.add_dll_directory(folder))
    try:
        lib = ctypes.CDLL(str(library.resolve()))
    finally:
        for directory in dll_dirs:
            directory.close()
    i16 = ctypes.POINTER(ctypes.c_int16)
    lib.dtad_train.argtypes = [i16, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, i16]
    lib.dtad_train.restype = ctypes.c_int
    lib.dtad_encode.argtypes = [i16, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
                               i16, i16, ctypes.POINTER(ctypes.c_uint8)]
    lib.dtad_encode.restype = ctypes.c_int
    return lib


def pcm_array(pcm):
    values = array.array('h')
    values.frombytes(pcm)
    if sys.byteorder != 'little':
        values.byteswap()
    return values


def encode(pcm, rate, channels, output, lib=None):
    if channels not in (1, 2) or not 1 <= rate <= 192000:
        raise ValueError('Expected 1 or 2 channels at 1..192000 Hz')
    if not pcm or len(pcm) % (2 * channels):
        raise ValueError('PCM16 input must contain a positive whole number of frames')
    samples = pcm_array(pcm)
    frames = len(samples) // channels
    if frames > 0xFFFFFFFF:
        raise ValueError('Source exceeds the converter frame limit')
    lib = lib or native()
    cpcm = (ctypes.c_int16 * len(samples)).from_buffer(samples)
    coefficients = [(ctypes.c_int16 * 16)() for _ in range(channels)]
    histories = [(ctypes.c_int16 * 2)() for _ in range(channels)]
    for ch in range(channels):
        if not lib.dtad_train(cpcm, frames, channels, ch, coefficients[ch]):
            raise ValueError('Predictor training failed')
    blocks = (frames + BLOCK_FRAMES - 1) // BLOCK_FRAMES
    header = bytearray(HEADER_BYTES)
    HEADER.pack_into(header, 0, b'DTAD', 1, HEADER_BYTES, rate, channels, frames,
                     BLOCK_FRAMES, blocks, 0)
    for ch in range(channels):
        struct.pack_into('<16h', header, 40 + ch*40, *coefficients[ch])
        # Descriptor's loop/start context is zero: today's runtime loops to sample 0.
    output = Path(output)
    temporary = output.with_suffix(output.suffix + '.tmp')
    try:
        with temporary.open('wb') as destination:
            destination.write(header)
            for at in range(0, frames, BLOCK_FRAMES):
                count = min(BLOCK_FRAMES, frames - at)
                for ch in range(channels):
                    encoded = (ctypes.c_uint8 * (((count + 13) // 14) * 8))()
                    history = tuple(histories[ch])
                    ptr = ctypes.cast(ctypes.byref(cpcm, at * channels * 2), ctypes.POINTER(ctypes.c_int16))
                    if not lib.dtad_encode(ptr, count, channels, ch, coefficients[ch], histories[ch], encoded):
                        raise ValueError('Encoding failed')
                    destination.write(CONTEXT.pack(encoded[0], *history, 0))
                    destination.write(bytes(encoded))
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)
    return dict(rate_hz=rate, channels=channels, frames=frames,
                pcm_bytes=len(pcm), adpcm_bytes=output.stat().st_size,
                sha256=hashlib.sha256(output.read_bytes()).hexdigest())


def decode_channel(data, frames, coefficients, history=(0, 0)):
    """Independent reference: high nibble first, rounding before arithmetic shift."""
    if len(data) != ((frames + 13) // 14) * 8:
        raise ValueError('Incorrect DSP frame byte count')
    h0, h1 = history
    out = []
    for at in range(0, len(data), 8):
        pred, scale = data[at] >> 4, data[at] & 15
        if pred >= 8:
            raise ValueError('Invalid DSP predictor or scale')
        a, b = coefficients[pred*2:pred*2+2]
        for n in range(min(14, frames - len(out))):
            nibble = (data[at + 1 + n//2] >> (0 if n % 2 else 4)) & 15
            signed = nibble - 16 if nibble >= 8 else nibble
            value = ((signed * (1 << scale) << 11) + a*h0 + b*h1 + 1024) >> 11
            value = max(-32768, min(32767, value))
            out.append(value)
            h1, h0 = h0, value
    return out, (h0, h1)


def read(path):
    data = Path(path).read_bytes()
    if len(data) < HEADER_BYTES:
        raise ValueError('Truncated DTAD header')
    magic, version, header, rate, channels, frames, block_frames, blocks, flags = HEADER.unpack_from(data)
    if (magic != b'DTAD' or version != 1 or header != HEADER_BYTES or channels not in (1, 2)
            or not 1 <= rate <= 192000 or not frames or block_frames != BLOCK_FRAMES
            or blocks != (frames + BLOCK_FRAMES - 1) // BLOCK_FRAMES or flags):
        raise ValueError('Unsupported or invalid DTAD header')
    coefficients = [struct.unpack_from('<16h', data, 40 + ch*40) for ch in range(channels)]
    decoded = [[] for _ in range(channels)]
    cursor = HEADER_BYTES
    previous = [(0, 0) for _ in range(channels)]
    for at in range(0, frames, BLOCK_FRAMES):
        count = min(BLOCK_FRAMES, frames - at)
        size = ((count + 13) // 14) * 8
        for ch in range(channels):
            if cursor + 8 + size > len(data):
                raise ValueError('Truncated DTAD block')
            index, h0, h1, reserved = CONTEXT.unpack_from(data, cursor)
            body = data[cursor + 8:cursor + 8 + size]
            if reserved or index != body[0] or (h0, h1) != previous[ch]:
                raise ValueError('Invalid or discontinuous DTAD history')
            values, history = decode_channel(body, count, coefficients[ch], (h0, h1))
            decoded[ch].extend(values)
            previous[ch] = history
            cursor += 8 + size
    if cursor != len(data):
        raise ValueError('Unexpected trailing DTAD data')
    pcm = array.array('h', (decoded[ch][i] for i in range(frames) for ch in range(channels)))
    if sys.byteorder != 'little':
        pcm.byteswap()
    return pcm.tobytes(), rate, channels


def write_wav(path, pcm, rate, channels):
    with wave.open(str(path), 'wb') as output:
        output.setnchannels(channels)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(pcm)
