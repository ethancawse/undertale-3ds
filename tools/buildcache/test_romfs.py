"""Wire-format and cross-host ordering regression tests with invented payloads."""
import hashlib
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('canonical_romfs', Path(__file__).with_name('romfs.py'))
romfs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(romfs)


def contents(raw):
    h = struct.unpack_from('<10I', raw)
    assert h[0] == 40
    result = {}

    def visit(index, parent=''):
        _, sibling, child, first, _, length = struct.unpack_from('<6I', raw, h[3] + index)
        name = raw[h[3]+index+24:h[3]+index+24+length].decode('utf-16le')
        prefix = parent + name + '/' if name else parent
        while first != 0xffffffff:
            _, nxt, offset, size, _, length = struct.unpack_from('<IIQQII', raw, h[7]+first)
            name = raw[h[7]+first+32:h[7]+first+32+length].decode('utf-16le')
            result[prefix+name] = raw[h[9]+offset:h[9]+offset+size]
            first = nxt
        while child != 0xffffffff:
            nxt, = struct.unpack_from('<I', raw, h[3]+child+4)
            visit(child, prefix)
            child = nxt
    visit(0)
    return result


class Images(unittest.TestCase):
    def tree(self, root, reverse=False):
        files = [('z.txt', b'last'), ('A.txt', b'first'), ('empty', b''),
                 ('folder/b.dat', b'b'*4099), ('folder/a.dat', b'inner'),
                 ('\u00e9/\U0001f496.txt', b'heart'), ('.hidden', b'ignore')]
        for name, data in files[::-1] if reverse else files:
            path = root/name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            os.utime(path, (123 if reverse else 456,)*2)
        (root/'empty-directory').mkdir()
        return {n: data for n, data in files if not n.startswith('.')}

    def test_creation_order_and_mtimes_do_not_change_either_image(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            expected = self.tree(root/'a')
            self.tree(root/'b', reverse=True)
            for name in ('a', 'b'):
                romfs.build(root/name, root/(name+'.raw'), root/(name+'.ivfc'))
            self.assertEqual((root/'a.raw').read_bytes(), (root/'b.raw').read_bytes())
            self.assertEqual((root/'a.ivfc').read_bytes(), (root/'b.ivfc').read_bytes())
            self.assertEqual(contents((root/'a.raw').read_bytes()), expected)

    def test_explicit_case_sensitive_utf16_sort(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.tree(root/'in', reverse=True)
            romfs.build(root/'in', root/'raw', root/'ivfc')
            names = list(contents((root/'raw').read_bytes()))
            self.assertEqual(names, ['A.txt', 'empty', 'z.txt', 'folder/a.dat',
                                     'folder/b.dat', '\u00e9/\U0001f496.txt'])

    def test_ivfc_independent_hash_tree_validation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.tree(root/'in')
            romfs.build(root/'in', root/'raw', root/'ivfc')
            raw, blob = (root/'raw').read_bytes(), (root/'ivfc').read_bytes()
            self.assertEqual(blob[:4], b'IVFC')
            master_size, = struct.unpack_from('<I', blob, 8)
            levels = [struct.unpack_from('<QQII', blob, 12+24*i) for i in range(3)]
            self.assertTrue(all(l[2:] == (12, 0) for l in levels))
            raw_start = (96+master_size+4095)//4096*4096
            first = raw_start + (len(raw)+4095)//4096*4096
            second = first + (levels[0][1]+4095)//4096*4096
            self.assertEqual(blob[raw_start:raw_start+len(raw)], raw)
            chain = [blob[96:96+master_size], blob[first:first+levels[0][1]],
                     blob[second:second+levels[1][1]], raw]
            for expected, data in zip(chain, chain[1:]):
                actual = b''.join(hashlib.sha256(data[i:i+4096].ljust(4096, b'\0')).digest()
                                  for i in range(0, len(data), 4096))
                self.assertEqual(expected, actual)

    def test_empty_image_is_valid(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root/'in').mkdir()
            romfs.build(root/'in', root/'raw', root/'ivfc')
            self.assertEqual(contents((root/'raw').read_bytes()), {})

    def test_no_output_inside_source_or_same_outputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root/'in').mkdir()
            for raw, ivfc in ((root/'in/raw', root/'ivfc'), (root/'raw', root/'raw')):
                with self.assertRaises(ValueError):
                    romfs.build(root/'in', raw, ivfc)

    def test_symlink_refused_and_previous_outputs_preserved(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root/'in').mkdir()
            (root/'target').write_bytes(b'private')
            try:
                (root/'in/link').symlink_to(root/'target')
            except OSError:
                self.skipTest('Host does not allow fixture symlinks')
            for path in (root/'raw', root/'ivfc'):
                path.write_bytes(b'previous')
            with self.assertRaises(ValueError):
                romfs.build(root/'in', root/'raw', root/'ivfc')
            self.assertEqual((root/'raw').read_bytes(), b'previous')
            self.assertEqual((root/'ivfc').read_bytes(), b'previous')
            self.assertFalse(list(root.glob('*.part')))


if __name__ == '__main__':
    unittest.main()
