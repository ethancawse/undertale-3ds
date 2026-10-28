"""Pagination preserves legal text and the bundle retains original source bytes."""
import json
from pathlib import Path
import struct
import tempfile
import unittest

from tools.release.prepare_legal import paginate, prepare, reflow

ROOT = Path(__file__).resolve().parents[2]


class LegalPagesTests(unittest.TestCase):
    def rows(self, output, columns):
        return [output[i:i + columns + 1].split(b'\0')[0]
                for i in range(8, len(output), columns + 1)]

    def test_wraps_exact_columns_without_losing_paragraphs(self):
        output = paginate(b'abcd\nefghijk\n\nlast\n', 4, 2)
        self.assertEqual(output[:4], b'LGL1')
        self.assertEqual(struct.unpack('<I', output[4:8])[0], 3)
        self.assertEqual(self.rows(output, 4), [b'abcd', b'efgh', b'ijk', b'', b'last', b''])
        self.assertEqual(len(output), 8 + 3 * 2 * 5)

    def test_reflow_joins_hard_wrapped_prose_only(self):
        long = 'The GNU Affero General Public License is a free, copyleft license'
        self.assertEqual(reflow(['  ' + long, 'for software and other kinds of works.', '', 'next']),
                         ['  ' + long + ' for software and other kinds of works.', '', 'next'])
        kept = [long, '  1. Definitions.', long, 'Licence: MIT', long, 'Copyright (c) 2020 Someone', long,
                '        Preamble', long, '====', 'short line', 'stays']
        self.assertEqual(reflow(kept), kept)

    def test_empty_file_and_utf8_boundaries(self):
        self.assertEqual(self.rows(paginate(b'', 4, 1), 4), [b''])
        rows = self.rows(paginate('ab\u00a9cd'.encode(), 3, 2), 3)
        self.assertEqual(b''.join(rows).decode(), 'ab\u00a9cd')
        for row in rows:
            row.decode('utf-8')

    def test_bad_dimensions_and_nuls_fail(self):
        for columns, lines in ((0, 22), (39, 22), (38, 23)):
            with self.assertRaises(ValueError):
                paginate(b'x', columns, lines)
        with self.assertRaises(ValueError):
            paginate(b'broken\0text', 38, 22)

    def test_original_texts_are_bundled_unchanged_and_not_rewritten(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'scratch') as directory:
            output = Path(directory) / 'legal'
            prepare(ROOT, output)
            before = {p.name: p.stat().st_mtime_ns for p in output.iterdir()}
            prepare(ROOT, output)
            self.assertEqual(before, {p.name: p.stat().st_mtime_ns for p in output.iterdir()})
            contract = json.loads((ROOT / 'docs/release/build-contract.json').read_text())
            for name in ('LICENSE', 'THIRD_PARTY_NOTICES'):
                self.assertEqual((output / name).read_bytes(), (ROOT / name).read_bytes())
                pages = (output / (name + '.pages')).read_bytes()
                count = struct.unpack('<I', pages[4:8])[0]
                self.assertEqual(len(pages), 8 + count * contract['legal_lines'] * (contract['legal_columns'] + 1))
                original = (ROOT / name).read_text(encoding='utf-8').expandtabs(4)
                # Wrapping changes whitespace only; every notice character survives.
                actual = b''.join(self.rows(pages, contract['legal_columns'])).decode()
                self.assertEqual(''.join(actual.split()), ''.join(original.split()))
