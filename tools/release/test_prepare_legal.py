"""Pagination preserves legal text and the bundle retains original source bytes."""
import builtins
import bz2
from contextlib import ExitStack
import hashlib
import json
import lzma
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib
import zipfile

from tools.release import prepare_legal as legal_module
from tools.release.prepare_legal import console_text, pack_records, paginate, prepare, reflow, split_bytes

ROOT = Path(__file__).resolve().parents[2]
# UNDERTALE's notices as legal.cmake selects them: the scoped template in the monorepo; the UNDERTALE source kit
# has no templates/ folder and ships that same text as its root THIRD_PARTY_NOTICES (manifest-ut.txt #map), so the
# goldens below also prove that fallback yields the full UNDERTALE notices. The DELTARUNE kit has neither.
UT_NOTICES = ROOT/'tools/release/templates/THIRD_PARTY_NOTICES-undertale.txt'
if not UT_NOTICES.is_file() and (ROOT/'tools/release/manifest-ut.txt').is_file():
    UT_NOTICES = ROOT/'THIRD_PARTY_NOTICES'


class LegalPagesTests(unittest.TestCase):
    def legal_documents(self):
        if not UT_NOTICES.is_file():
            self.skipTest('this source kit has no UNDERTALE notices')
        return [paginate(path.read_bytes(), 38, 22) for path in (ROOT/'LICENSE', UT_NOTICES)]

    def test_pinned_decoded_text_and_pack_golden(self):
        documents = self.legal_documents()
        self.assertEqual(hashlib.sha256(b''.join(documents)).hexdigest(),
                         '9bdd49e899450ee0a88e7a91047e487b1463806ef8b6de7ed8eb7026d965a060')
        packed = pack_records(documents)
        self.assertEqual(len(packed), 161829)
        self.assertLessEqual(len(packed), 262144)  # The unchanged console reader's hard bound.
        self.assertEqual(hashlib.sha256(packed).hexdigest(),
                         '1e5a8d14ae973e8fd06418b4629b3783f6ce83372b3ae0995e8d080bb80783ea')

    def test_no_host_compressor_import_or_compression(self):
        original_import = builtins.__import__

        def refuse_compressor(name, *args, **kwargs):
            if name.split('.')[0] in ('zlib', 'bz2', 'lzma', 'zipfile'):
                self.fail('prepare_legal must not import host compressor ' + name)
            return original_import(name, *args, **kwargs)

        def refuse_compress(*args, **kwargs):
            self.fail('prepare_legal must not call host compression')

        namespace = {'__name__': 'legal_without_host_compressors', '__file__': legal_module.__file__}
        source = Path(legal_module.__file__).read_text(encoding='utf-8')
        documents = self.legal_documents()
        with ExitStack() as guards:
            guards.enter_context(patch('builtins.__import__', refuse_compressor))
            for module, names in ((zlib, ('compress', 'compressobj')),
                                  (bz2, ('compress', 'BZ2Compressor')),
                                  (lzma, ('compress', 'LZMACompressor')),
                                  (zipfile, ('ZipFile', 'PyZipFile', '_get_compressor'))):
                for name in names:
                    guards.enter_context(patch.object(module, name, refuse_compress))
            exec(compile(source, legal_module.__file__, 'exec'), namespace)
            packed = namespace['pack_records'](documents)
        self.assertEqual(hashlib.sha256(packed).hexdigest(),
                         '1e5a8d14ae973e8fd06418b4629b3783f6ce83372b3ae0995e8d080bb80783ea')

    def test_encoder_round_trips_literals_overlaps_and_window_boundaries(self):
        noise = random.Random(192).randbytes(8192)
        cases = [b'', b'x', b'xy', bytes(range(256)), bytes(range(256))*32, b'\0'*8192,
                 b'abc'*3000, noise, noise+noise[:258], noise+b'x'+noise[:258]]
        for length in (3, 10, 11, 18, 19, 34, 35, 66, 67, 130, 131, 257, 258):
            cases.append(b'ab'+b'a'*length+b'b')
        for data in cases:
            with self.subTest(bytes=len(data), prefix=data[:4]):
                encoded = legal_module._zlib_fixed(data)
                self.assertEqual(encoded[:2], b'\x78\x01')
                self.assertEqual(encoded[2] & 7, 3)  # Final block, fixed-Huffman BTYPE.
                decoder = zlib.decompressobj()
                self.assertEqual(decoder.decompress(encoded)+decoder.flush(), data)
                self.assertTrue(decoder.eof)
                self.assertEqual(decoder.unused_data, b'')
                self.assertEqual(int.from_bytes(encoded[-4:], 'big'), zlib.adler32(data))
        self.assertEqual(legal_module._zlib_fixed(b''), b'\x78\x01\x03\x00\x00\x00\x00\x01')

    def test_pack_refuses_compressed_output_over_reader_limit(self):
        rng = random.Random(47)
        documents = [b'LGL1'+rng.randbytes(140000) for _ in range(2)]
        with self.assertRaisesRegex(ValueError, 'bounded reader format'):
            pack_records(documents)

    def test_pack_is_independent_of_python_hash_seed(self):
        self.legal_documents()
        code = ("import hashlib,sys; sys.path.insert(0,sys.argv[1]); "
                "from tools.release.prepare_legal import pack_records,paginate; "
                "from pathlib import Path; root=Path(sys.argv[1]); "
                "docs=[paginate(p.read_bytes(),38,22) for p in "
                "(root/'LICENSE',Path(sys.argv[2]))]; "
                "print(hashlib.sha256(pack_records(docs)).hexdigest())")
        for seed in ('0', '1', '9173'):
            env = {k: v for k, v in os.environ.items() if not k.startswith(('GIT_', 'PYTHON'))}
            env['PYTHONHASHSEED'] = seed  # Child deliberately omits -I so this seed actually takes effect.
            child = subprocess.run([sys.executable, '-c', code, str(ROOT), str(UT_NOTICES)], cwd=ROOT, env=env,
                                   capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=20)
            self.assertEqual(child.returncode, 0, child.stderr)
            self.assertEqual(child.stdout.strip(),
                             '1e5a8d14ae973e8fd06418b4629b3783f6ce83372b3ae0995e8d080bb80783ea')

    def test_pack_preserves_every_original_record_and_all_jump_targets(self):
        # Real public texts, including the selected UNDERTALE notices. Never game data.
        documents = self.legal_documents()
        packed = pack_records(documents)
        magic, size, block_size, first, second, checksum = struct.unpack('<4s5I', packed[:24])
        self.assertEqual((magic, size, block_size, first, second),
                         (b'NSP1', len(packed), 8192, *map(len, documents)))
        computed = 2166136261
        for value in packed[:20] + packed[24:]:
            computed = ((computed ^ value) * 16777619) & 0xffffffff
        self.assertEqual(checksum, computed)
        counts = [(len(data) + 8191) // 8192 for data in documents]
        offsets = struct.unpack_from('<%dI' % (sum(counts) + 1), packed, 24)
        self.assertEqual(offsets[0], 24 + 4 * len(offsets))
        self.assertEqual(offsets[-1], len(packed))
        self.assertLessEqual(len(packed), 262144)
        block = 0
        for data, count in zip(documents, counts):
            for i in reversed(range(count)):
                a, b = offsets[block + i:block + i + 2]
                self.assertLess(a, b)
                self.assertEqual(zlib.decompress(packed[a:b]), data[i * 8192:(i + 1) * 8192])
            block += count
        self.assertEqual(pack_records(documents), packed)

    def rows(self, output, columns):
        return [output[i:i + columns + 1].split(b'\0')[0]
                for i in range(8, len(output), columns + 1)]

    def test_wraps_exact_columns_without_losing_paragraphs(self):
        output = paginate(b'abcd\nefghijk\n\nlast\n', 4, 2)
        self.assertEqual(output[:4], b'LGL1')
        self.assertEqual(struct.unpack('<I', output[4:8])[0], 3)
        self.assertEqual(self.rows(output, 4), [b'abcd', b'efgh', b'ijk', b'', b'last', b''])
        self.assertEqual(len(output), 8 + 3 * 2 * 5)

    def test_console_pages_have_no_character_the_reader_shows_as_a_question_mark(self):
        # ctr_legal.c's luiLine draws anything outside 32..126 as '?'. Every byte of every shipped page, not just the two
        # names this was found on.
        contract = json.loads((ROOT / 'docs/release/build-contract.json').read_text(encoding='utf-8'))
        for name in ('LICENSE', 'THIRD_PARTY_NOTICES'):
            pages = paginate((ROOT / name).read_bytes(), contract['legal_columns'], contract['legal_lines'])
            rows = self.rows(pages, contract['legal_columns'])
            self.assertEqual([r for r in rows if any(not 32 <= c <= 126 for c in r)], [], name)

    def test_console_pages_spell_the_two_names_in_ascii_and_change_nothing_else(self):
        contract = json.loads((ROOT / 'docs/release/build-contract.json').read_text(encoding='utf-8'))
        source = (ROOT / 'THIRD_PARTY_NOTICES').read_bytes()
        self.assertEqual(sorted({c for c in source.decode('utf-8') if ord(c) > 126}), ['\u00a9', '\u00f6'])
        columns = contract['legal_columns']
        rows = self.rows(paginate(source, columns, contract['legal_lines']), columns)
        joined = b''.join(rows).replace(b' ', b'')
        self.assertIn(b'KungligaTekniskaHogskolan', joined)
        self.assertIn(b'Copyright(c)2005-2020RichFelker', joined)
        spelled = source.replace('\u00f6'.encode(), b'o').replace('\u00a9'.encode(), b'(c)')
        # The text itself, whitespace aside: nothing lost, added or reordered.
        self.assertEqual(joined, b''.join(spelled.expandtabs(4).split()))
        # And outside those two lines, the same rows as the unchanged wrap of the same text.
        self.assertEqual(paginate(spelled, columns, contract['legal_lines']),
                         paginate(source, columns, contract['legal_lines']))

    def test_a_character_without_an_ascii_spelling_stops_the_build(self):
        # Not a silent '?' on the console when a notice is regenerated with a new name.
        for text in ('Copyright \u00e9'.encode(), 'a \u2014 b'.encode(), 'x\x7fy'.encode()):
            with self.assertRaises(ValueError):
                paginate(text, 38, 13)

    def test_bundle_keeps_the_real_characters(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / 'out'
            prepare(ROOT, out)
            self.assertEqual((out / 'THIRD_PARTY_NOTICES').read_bytes(), (ROOT / 'THIRD_PARTY_NOTICES').read_bytes())
            self.assertIn('H\u00f6gskolan'.encode(), (out / 'THIRD_PARTY_NOTICES').read_bytes())

    def test_raw_romfs_inputs_refuse_every_carriage_return(self):
        for selected in ('LICENSE', 'THIRD_PARTY_NOTICES', 'selected-notices.txt'):
            for ending in (b'\r\n', b'\r', b'\rcontinued text\n'):
                with self.subTest(input=selected, ending=ending), \
                        tempfile.TemporaryDirectory(dir=ROOT / 'scratch') as temp:
                    root = Path(temp)
                    (root / 'docs/release').mkdir(parents=True)
                    (root / 'docs/release/build-contract.json').write_bytes(
                        (ROOT / 'docs/release/build-contract.json').read_bytes())
                    for name in ('LICENSE', 'THIRD_PARTY_NOTICES', 'selected-notices.txt'):
                        (root / name).write_bytes(b'Original public legal text.\n')
                    bad = root / selected
                    bad.write_bytes(b'Original public legal text.' + ending)
                    output = root / 'out'
                    notices = bad if selected == 'selected-notices.txt' else None
                    with self.assertRaisesRegex(ValueError, 'carriage return.*LF') as raised:
                        prepare(root, output, notices)
                    self.assertIn(str(bad), str(raised.exception))
                    self.assertFalse((output / ('LICENSE' if selected == 'LICENSE' else
                                                'THIRD_PARTY_NOTICES')).exists())

    def test_reflow_joins_hard_wrapped_prose_only(self):
        long = 'The GNU Affero General Public License is a free, copyleft license'
        self.assertEqual(reflow(['  ' + long, 'for software and other kinds of works.', '', 'next']),
                         ['  ' + long + ' for software and other kinds of works.', '', 'next'])
        kept = [long, '  1. Definitions.', long, 'Licence: MIT', long, 'Copyright (c) 2020 Someone', long,
                '        Preamble', long, '====', 'short line', 'stays']
        self.assertEqual(reflow(kept), kept)

    def test_empty_file_and_utf8_boundaries(self):
        self.assertEqual(self.rows(paginate(b'', 4, 1), 4), [b''])
        rows = split_bytes('ab\u00a9cd', 3)
        self.assertEqual(rows, ['ab'.encode(), '\u00a9c'.encode(), b'd'])
        self.assertEqual(b''.join(self.rows(paginate('ab\u00a9cd'.encode(), 3, 2), 3)), b'ab(c)cd')
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
                self.assertEqual(''.join(actual.split()), ''.join(console_text(original).split()))
