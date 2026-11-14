"""Check lossless capture orientation, padding and non-destructive migration."""
from pathlib import Path
import hashlib
import importlib.util
import tempfile
import struct
import unittest

try:
    from PIL import Image
except ImportError:
    Image = None

spec = importlib.util.spec_from_file_location('capture_png', Path(__file__).with_name('capture_png.py'))
png = importlib.util.module_from_spec(spec)
spec.loader.exec_module(png)


def fixture_bmp():
    rows = [bytes((1, 2, 3, 4, 5, 6, 7, 8, 9)), bytes((255, 254, 253, 24, 85, 31, 10, 20, 30))]
    # Three pixels: three padding bytes per row; BMP rows are bottom first/BGR.
    bmp = bytearray(54)
    bmp[:2] = b'BM'
    for offset, value in ((2, 78), (10, 54), (14, 40), (18, 3), (22, 2), (34, 24)):
        struct.pack_into('<I', bmp, offset, value)
    bmp[26], bmp[28] = 1, 24
    for row in reversed(rows):
        for at in range(0, len(row), 3):
            bmp.extend(row[at:at+3][::-1])
        bmp.extend(b'\xee\xee\xee')
    return bytes(bmp), rows


class CapturePngTests(unittest.TestCase):
    @unittest.skipUnless(Image, 'Optional independent Pillow decoder')
    def test_independent_decoder(self):
        with tempfile.TemporaryDirectory() as temp:
            bmp, rows = fixture_bmp()
            path = Path(temp) / 'frame.png'
            png.write_bmp_png(path, bmp)
            with Image.open(path) as image:
                self.assertEqual(image.size, (3, 2))
                self.assertEqual(image.convert('RGB').tobytes(), b''.join(rows))

    def test_bmp_orientation_padding_and_manifest(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'bmp'
            source.mkdir()
            bmp, rows = fixture_bmp()
            (source / 'frame.bmp').write_bytes(bmp)
            (source / 'aux.bin').write_bytes(b'unchanged raw evidence')
            before = {p.name: p.read_bytes() for p in source.iterdir()}
            report = png.convert_folder(source, root / 'png')
            self.assertEqual(report['pictures'], 1)
            self.assertEqual(report['pixel_mismatches'], 0)
            row = next(r for r in report['files'] if 'pixel_md5' in r)
            expected = hashlib.md5(b''.join(rows)).hexdigest()
            self.assertEqual(row['pixel_md5'], expected)
            self.assertEqual((root / 'png/pixel-md5.txt').read_text(), f'{expected}  frame.png\n')
            self.assertEqual(before, {p.name: p.read_bytes() for p in source.iterdir()})
            self.assertFalse((root / 'png/frame.bmp').exists())
            written = png.write_bmp_png(root / 'new.png', bmp)
            self.assertEqual(written['pixel_md5'], expected)
            with self.assertRaises(ValueError):
                png.write_bmp_png(root / 'new.png', bmp)

    def test_migration_rejects_collision_before_output(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'bmp'
            source.mkdir()
            bmp, _ = fixture_bmp()
            (source / 'frame.bmp').write_bytes(bmp)
            (source / 'frame.png').write_bytes(b'collision')
            with self.assertRaises(ValueError):
                png.convert_folder(source, root / 'png')
            self.assertFalse((root / 'png').exists())


if __name__ == '__main__':
    unittest.main()
