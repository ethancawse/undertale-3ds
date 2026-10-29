"""Synthetic codec gates; no game data. Run: py -3 -I tools/audio/test_dspadpcm.py."""
import array
import ctypes
import math
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dspadpcm as dsp


class CodecTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib = dsp.native()

    def source(self, frames, channels):
        pcm = array.array('h', (round(18000 * math.sin(i * (0.061 + ch*0.007))
                                    + 4000 * math.sin(i * 0.413))
                               for i in range(frames) for ch in range(channels)))
        if sys.byteorder != 'little':
            pcm.byteswap()
        return pcm.tobytes()

    def test_signed_nibbles_and_saturation(self):
        pcm, _ = dsp.decode_channel(bytes([12, 0x7f, 0x88, 0, 0, 0, 0, 0]), 14, [0]*16)
        self.assertEqual(pcm[:4], [28672, -4096, -32768, -32768])
        pcm, _ = dsp.decode_channel(bytes([15, 0x78, 0, 0, 0, 0, 0, 0]), 2, [0]*16)
        self.assertEqual(pcm, [32767, -32768])

    def test_predictor_history_and_rounding(self):
        coefficients = [2048, 0] + [0]*14
        pcm, history = dsp.decode_channel(bytes(8), 14, coefficients, (1000, 900))
        self.assertEqual(pcm, [1000]*14)
        self.assertEqual(history, (1000, 1000))
        pcm, _ = dsp.decode_channel(bytes(8), 1, [1024, 0] + [0]*14, (-1, 0))
        self.assertEqual(pcm, [0])

    def test_sample_counts_channels_and_block_edges(self):
        with tempfile.TemporaryDirectory(dir=dsp.ROOT / 'scratch/adpcm') as temp:
            path = Path(temp) / 'synthetic.dtad'
            for channels in (1, 2):
                for frames in (1, 13, 14, 15, 2043, 2044, 2045, 4089):
                    with self.subTest(channels=channels, frames=frames):
                        original = self.source(frames, channels)
                        row = dsp.encode(original, 44100, channels, path, self.lib)
                        rendered, rate, count = dsp.read(path)
                        self.assertEqual((rate, count, len(rendered)), (44100, channels, len(original)))
                        self.assertEqual(row['frames'], frames)
                        if frames >= 2043:
                            a, b = dsp.pcm_array(original), dsp.pcm_array(rendered)
                            signal = sum(x*x for x in a)
                            error = sum((x-y)**2 for x, y in zip(a, b))
                            self.assertGreater(10*math.log10(signal / max(error, 1)), 25)

    def test_deterministic_encoding(self):
        with tempfile.TemporaryDirectory(dir=dsp.ROOT / 'scratch/adpcm') as temp:
            a, b = Path(temp) / 'a.dtad', Path(temp) / 'b.dtad'
            original = self.source(5000, 2)
            dsp.encode(original, 48000, 2, a, self.lib)
            dsp.encode(original, 48000, 2, b, self.lib)
            self.assertEqual(a.read_bytes(), b.read_bytes())

    def test_reject_invalid_sources_and_files(self):
        with tempfile.TemporaryDirectory(dir=dsp.ROOT / 'scratch/adpcm') as temp:
            path = Path(temp) / 'bad.dtad'
            for pcm, rate, channels in ((b'', 44100, 1), (b'0', 44100, 1),
                                        (b'00', 0, 1), (b'00', 44100, 3)):
                with self.assertRaises(ValueError):
                    dsp.encode(pcm, rate, channels, path, self.lib)
            dsp.encode(self.source(2050, 2), 44100, 2, path, self.lib)
            good = path.read_bytes()
            for mutated in (good[:20], good[:-1], good + b'x', b'BAD!' + good[4:],
                            good[:128] + b'\xff' + good[129:],
                            good[:130] + b'\xff\xff' + good[132:]):
                path.write_bytes(mutated)
                with self.assertRaises(ValueError):
                    dsp.read(path)

    def test_independent_thp_dsp_decoder(self):
        ffmpeg = shutil.which('ffmpeg')
        if not ffmpeg:
            self.skipTest('ffmpeg is not available for the independent decoder gate')
        with tempfile.TemporaryDirectory(dir=dsp.ROOT / 'scratch/adpcm') as temp:
            folder = Path(temp)
            frames = 280
            samples = dsp.pcm_array(self.source(frames, 1))
            cpcm = (ctypes.c_int16 * frames).from_buffer(samples)
            # THP's software decoder truncates the predictor; our DSP reference
            # rounds it. Integer predictors let this gate check byte coding,
            # sign, scale and carry independently of that rounding difference.
            coefs = (ctypes.c_int16 * 16)(2048, 0)
            history = (ctypes.c_int16 * 2)()
            encoded = (ctypes.c_uint8 * (frames // 14 * 8))()
            self.assertEqual(self.lib.dtad_encode(cpcm, frames, 1, 0, coefs, history, encoded), 1)
            payload = bytes(encoded)
            values, _ = dsp.decode_channel(payload, frames, coefs)
            expected = array.array('h', values).tobytes()
            # THP audio packets carry the same DSP frame coding and coefficients.
            # A small PCM-WAV is only a packet transport: force FFmpeg's THP
            # decoder explicitly. The whole packet fits in its first read.
            packet = struct.pack('>II16h2h', len(payload), frames, *coefs, 0, 0) + payload
            standard = folder / 'thp-packet.wav'
            dsp.write_wav(standard, packet, 32000, 1)
            decoded = folder / 'decoded.pcm'
            subprocess.run([ffmpeg, '-v', 'error', '-nostdin', '-y', '-c:a', 'adpcm_thp', '-i', str(standard),
                            '-f', 's16le', '-acodec', 'pcm_s16le', str(decoded)],
                           stdin=subprocess.DEVNULL, check=True, timeout=30)
            self.assertEqual(decoded.read_bytes(), expected)


if __name__ == '__main__':
    unittest.main()
