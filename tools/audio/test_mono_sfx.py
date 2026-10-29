"""Synthetic source equality/projection tests; no game data or runtime changes."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mono_sfx as mono


def wave(pcm, *, channels=2, bits=16, before=b''):
    width = channels * (bits // 8)
    fmt = struct.pack('<HHIIHH', 1, channels, 32000, 32000 * width, width, bits)
    body = b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt + before
    body += b'data' + struct.pack('<I', len(pcm)) + pcm
    if len(pcm) & 1:
        body += b'\0'
    return b'RIFF' + struct.pack('<I', len(body)) + body


class MonoTests(unittest.TestCase):
    def test_pcm16_equality_and_original_pan_channels(self):
        values = [-32768, -1, 0, 1, 32767]
        original = struct.pack('<10h', *(value for sample in values for value in (sample, sample)))
        projection = mono.project(wave(original))
        self.assertEqual(projection.pcm16, struct.pack('<5h', *values))
        self.assertEqual((projection.frames, projection.rate_hz, projection.source_bits), (5, 32000, 16))
        with tempfile.TemporaryDirectory(dir=mono.dsp.ROOT / 'scratch/adpcm') as folder:
            row = mono.encode(wave(original), Path(folder) / 'mono')
            self.assertEqual((row['source_channels'], row['channels'], row['frames']), (2, 1, 5))
            self.assertEqual(row['projection'], mono.POLICY)
            self.assertTrue(row['timing_eligible'])

    def test_single_bit_difference_is_true_stereo(self):
        for pcm in (struct.pack('<4h', 0, 1, 32767, 32767),
                    struct.pack('<4h', -32768, -32767, 0, 0),
                    struct.pack('<4h', 256, 0, 0, 0)):
            self.assertIsNone(mono.project(wave(pcm)))
            with self.assertRaisesRegex(ValueError, 'not an exact dual-mono'):
                mono.encode(wave(pcm), Path('must-not-be-created'))

    def test_pcm8_exact_unsigned_lift(self):
        values = [0, 127, 128, 129, 255]
        pcm = bytes(value for sample in values for value in (sample, sample))
        projection = mono.project(wave(pcm, bits=8))
        self.assertEqual(projection.pcm16, struct.pack('<5h', *((value - 128) * 256 for value in values)))
        self.assertEqual(projection.source_bits, 8)
        self.assertIsNone(mono.project(wave(b'\x80\x81', bits=8)))

    def test_original_mono_ogg_and_non_audio_do_not_opt_in(self):
        for data in (wave(struct.pack('<2h', 1, 2), channels=1), b'OggSnot-a-WAV', b''):
            self.assertIsNone(mono.project(data))

    def test_malformed_bounds_empty_and_partial_frames_decline(self):
        good = wave(struct.pack('<4h', 1, 1, 2, 2))
        cases = [good[:-1], good + b'x', wave(b''), wave(b'\0\0'),
                 b'RIFF' + struct.pack('<I', 12) + b'WAVEdata' + struct.pack('<I', 0)]
        for data in cases:
            with self.assertRaises(ValueError):
                mono.project(data)

    def test_padded_ancillary_chunk_is_ignored(self):
        pcm = struct.pack('<4h', 123, 123, -789, -789)
        ancillary = b'JUNK' + struct.pack('<I', 1) + b'x\0'
        self.assertEqual(mono.project(wave(pcm, before=ancillary)).pcm16, struct.pack('<2h', 123, -789))

    def test_projected_render_matches_existing_stereo_encoder_left(self):
        # Cross independent block histories and a final partial DSP frame.
        values = [(index * 997 % 64001) - 32000 for index in range(2044 * 2 + 3)]
        stereo = struct.pack(f'<{len(values) * 2}h', *(v for sample in values for v in (sample, sample)))
        with tempfile.TemporaryDirectory(dir=mono.dsp.ROOT / 'scratch/adpcm') as folder:
            folder = Path(folder)
            mono.encode(wave(stereo), folder / 'mono')
            mono.dsp.encode(stereo, 32000, 2, folder / 'stereo.dtad')
            rendered, _, _ = mono.dsp.read(folder / 'stereo.dtad')
            expected = bytearray(len(values) * 2)
            expected[0::2], expected[1::2] = rendered[0::4], rendered[1::4]
            actual, rate, channels = mono.dsp.read(folder / 'mono.dtad')
            self.assertEqual((rate, channels), (32000, 1))
            self.assertEqual(actual, expected)


if __name__ == '__main__':
    unittest.main()
