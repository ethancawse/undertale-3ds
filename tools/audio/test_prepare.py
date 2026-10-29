"""Build-time conversion gates use synthetic data only, without a 3DS."""
import contextlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import wave

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prepare
import dspadpcm as dsp


def fixture(blob):
    sond = struct.pack('<II9I', 1, 24, 72, 0x65, 72, 72, 0, 0, 0, 0, 0)
    strings = struct.pack('<I', 4) + b'test'
    audo = struct.pack('<III', 1, 92, len(blob)) + blob
    chunks = [(b'SOND', sond), (b'STRG', strings), (b'AUDO', audo)]
    body = b''.join(name + struct.pack('<I', len(data)) + data for name, data in chunks)
    return b'FORM' + struct.pack('<I', len(body)) + body


class PrepareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib = dsp.native()

    def test_embedded_external_dedup_resume_and_fallback(self):
        with tempfile.TemporaryDirectory(dir=dsp.ROOT / 'scratch/adpcm') as temporary:
            root = Path(temporary)
            source, out = root / 'source', root / 'out'
            source.mkdir()
            stream = io.BytesIO()
            with wave.open(stream, 'wb') as sound:
                sound.setparams((1, 2, 32000, 0, 'NONE', 'not compressed'))
                sound.writeframes(struct.pack('<32h', *range(32)))
            blob = stream.getvalue()
            (source / 'data.win').write_bytes(fixture(blob))
            # Content sniffing is deliberate; duplicate source bytes reuse one object.
            (source / 'copy.ogg').write_bytes(blob)
            (source / 'bad.ogg').write_bytes(b'unsupported')
            with contextlib.redirect_stdout(io.StringIO()):
                result = prepare.prepare(source, out, lib=self.lib)
            self.assertFalse(result['runtime_enabled'])
            self.assertFalse(result['approved'])
            self.assertEqual(result['counts'], dict(encoded=1, reused=0, fallback=1))
            self.assertEqual(len(result['objects']), 1)
            self.assertEqual(result['sounds'][0]['frames'], 32)
            payload = out / result['sounds'][0]['object']
            expected = payload.read_bytes()
            with contextlib.redirect_stdout(io.StringIO()):
                reused = prepare.prepare(source, out, lib=self.lib)
            self.assertEqual(reused['counts']['reused'], 1)
            self.assertEqual(reused['counts']['encoded'], 0)
            payload.write_bytes(b'corrupt cached object')
            with contextlib.redirect_stdout(io.StringIO()):
                repaired = prepare.prepare(source, out, lib=self.lib)
            self.assertEqual(repaired['counts']['encoded'], 1)
            self.assertEqual(payload.read_bytes(), expected)
            self.assertEqual(json.loads((out / 'manifest.json').read_text())['version'], 1)

    def test_deleted_slots_and_bounds(self):
        with tempfile.TemporaryDirectory(dir=dsp.ROOT / 'scratch/adpcm') as temporary:
            file = Path(temporary) / 'data.win'
            body = b'SOND' + struct.pack('<3I', 8, 1, 0)
            file.write_bytes(b'FORM' + struct.pack('<I', len(body)) + body)
            self.assertEqual(prepare.catalog(file), [dict(sound_index=0, status='absent')])
            file.write_bytes(fixture(b'RIFF'))
            bad = bytearray(file.read_bytes())
            struct.pack_into('<I', bad, 20, len(bad) + 1)
            file.write_bytes(bad)
            with self.assertRaises(ValueError):
                prepare.catalog(file)

    def test_output_and_external_path_guards(self):
        with tempfile.TemporaryDirectory(dir=dsp.ROOT / 'scratch/adpcm') as temporary:
            root = Path(temporary).resolve()
            with self.assertRaises(ValueError):
                prepare.prepare(root, root / 'out')
            for name in ('../escape.ogg', '/escape.ogg', 'C:/escape.ogg'):
                with self.assertRaises(ValueError):
                    prepare.resolve_external(name, root / 'data.win', root)


if __name__ == '__main__':
    unittest.main()
