"""Offline converter source/format validation using synthetic data only."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import wave

sys.path.insert(0, str(Path(__file__).resolve().parent))
import convert
import dspadpcm as dsp


def fixture(flags=0x65):
    # SOND entry at 24; STRG length at 68, bytes at 72; AUDO entry at 92.
    sond = struct.pack('<II9I', 1, 24, 72, flags, 72, 72, 0, 0, 0, 0, 0)
    strings = struct.pack('<I', 4) + b'test'
    blob = b'RIFFsynthetic'
    audo = struct.pack('<III', 1, 92, len(blob)) + blob
    body = b'SOND' + struct.pack('<I', len(sond)) + sond
    body += b'STRG' + struct.pack('<I', len(strings)) + strings
    body += b'AUDO' + struct.pack('<I', len(audo)) + audo
    return b'FORM' + struct.pack('<I', len(body)) + body, blob


class SourceTests(unittest.TestCase):
    def test_embedded_sound_and_external_stale_index(self):
        with tempfile.TemporaryDirectory(dir=dsp.ROOT / 'scratch/adpcm') as temp:
            path = Path(temp) / 'synthetic.win'
            data, expected = fixture()
            path.write_bytes(data)
            blob, identity = convert.embedded(path, 'test')
            self.assertEqual(blob, expected)
            self.assertEqual(identity['sound_index'], 0)
            self.assertEqual(convert.embedded(path, '0')[0], expected)
            with self.assertRaises(ValueError):
                convert.embedded(path, 'absent')
            path.write_bytes(fixture(0x64)[0])
            with self.assertRaisesRegex(ValueError, 'External sound'):
                convert.embedded(path, '0')

    def test_invalid_source_tables(self):
        data, _ = fixture()
        for bad in (data[:-1], b'BAD!' + data[4:],
                    data[:20] + struct.pack('<I', 0xFFFFFFFF) + data[24:]):
            with tempfile.TemporaryDirectory(dir=dsp.ROOT / 'scratch/adpcm') as temp:
                path = Path(temp) / 'bad.win'
                path.write_bytes(bad)
                with self.assertRaises(ValueError):
                    convert.embedded(path, '0')

    def test_wave_source_conversion_keeps_count(self):
        with tempfile.TemporaryDirectory(dir=dsp.ROOT / 'scratch/adpcm') as temp:
            folder = Path(temp)
            source = folder / 'test.wav'
            pcm = struct.pack('<18h', *range(-9, 9))
            dsp.write_wav(source, pcm, 32000, 2)
            row = convert.convert(source, folder / 'out', listen=True)
            self.assertEqual((row['frames'], row['rate_hz'], row['channels']), (9, 32000, 2))
            self.assertFalse(row['approved'])
            with wave.open(str(folder / 'out.adpcm.wav'), 'rb') as result:
                self.assertEqual(result.getnframes(), 9)
            with self.assertRaisesRegex(ValueError, 'overwrite source'):
                convert.convert(folder / 'out.original.wav', folder / 'out')


if __name__ == '__main__':
    unittest.main()
