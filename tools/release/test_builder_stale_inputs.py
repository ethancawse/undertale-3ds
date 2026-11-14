"""A changed game file with the same size and date must never reuse old build inputs or generated code.
Outside audit DEEP-09 and scour SCOURT7-001 (2026-11-04). Invented bytes only."""
import contextlib
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE / 'builder')]
import products
import verify_game


@contextlib.contextmanager
def aot_prepare(directory):
    """tools/aot/prepare.py loaded by path, and 'prepare' only inside the block. In one pytest process over every
    tool, tools/audio/prepare.py can already be sys.modules['prepare'] or come first on sys.path, so a bare import may
    not be this file. sys.modules and sys.path are put back afterwards."""
    missing = object()
    old, path = sys.modules.get('prepare', missing), sys.path[:]
    spec = importlib.util.spec_from_file_location('prepare', os.path.join(directory, 'prepare.py'))
    module = importlib.util.module_from_spec(spec)
    sys.modules['prepare'] = module
    try:
        spec.loader.exec_module(module)
        yield module
    finally:
        sys.path[:] = path
        if old is missing:
            sys.modules.pop('prepare', None)
        else:
            sys.modules['prepare'] = old

UT = products.PRODUCTS['undertale']
STAMP = 1_700_000_000_000_000_000  # ns


class MacStagingTests(unittest.TestCase):
    """DEEP-09: a staged copy of game.ios with the same size and date as a changed game.ios must be replaced."""

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.game = self.root / 'mac'
        self.source = self.game / 'UNDERTALE.app/Contents/Resources/game.ios'
        self.source.parent.mkdir(parents=True)
        self.staging = self.root / 'private'

    def staged(self):
        self.assertIs(verify_game.layout_for(UT.game, str(self.game)), verify_game.MACOS_UNDERTALE)
        return Path(products.aot_data_dir(UT, str(self.game), str(self.staging))) / 'data.win'

    def test_a_same_size_same_date_copy_with_other_bytes_is_staged_again(self):
        self.source.write_bytes(b'REPAIRED GAME')
        staged = self.staging / verify_game.MACOS_UNDERTALE.key / 'data.win'
        staged.parent.mkdir(parents=True)
        staged.write_bytes(b'OLD BAD INPUT')
        os.utime(staged, ns=(STAMP, STAMP))
        os.utime(self.source, ns=(STAMP, STAMP))
        self.assertEqual(self.staged().read_bytes(), b'REPAIRED GAME')

    def test_where_links_fail_a_replaced_game_is_copied_again_in_the_same_run(self):
        with mock.patch.object(products.os, 'link', side_effect=OSError('no links here')):
            for data in (b'FIRST GAME 1', b'OTHER GAME 2'):
                self.source.write_bytes(data)
                os.utime(self.source, ns=(STAMP, STAMP))
                self.assertEqual(self.staged().read_bytes(), data)

    def test_an_unchanged_copy_is_kept(self):
        self.source.write_bytes(b'SAME GAME')
        with mock.patch.object(products.os, 'link', side_effect=OSError('no links here')):
            first = self.staged()
            before = first.stat().st_mtime_ns, first.stat().st_ino
            with mock.patch.object(products.os, 'replace', side_effect=AssertionError('copied again')):
                self.assertEqual(self.staged(), first)
        self.assertEqual((first.stat().st_mtime_ns, first.stat().st_ino), before)


class GeneratedCodeTests(unittest.TestCase):
    """SCOURT7-001: generated code must be made again when data.win changes within the same second."""

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.data = self.root / 'game' / 'data.win'
        self.data.parent.mkdir()
        self.out = self.root / 'out'
        self.runs = []

    def generate(self, args):
        self.runs.append(self.data.read_bytes())
        self.out.mkdir(parents=True, exist_ok=True)
        (self.out / 'aot_table.c').write_bytes(self.runs[-1])
        return 0

    def build(self, data, offset_ns):
        self.data.write_bytes(data)
        os.utime(self.data, ns=(STAMP + offset_ns, STAMP + offset_ns))
        argv = ['prepare.py', '--game', 'undertale', '--target', 'o3ds', '--out', str(self.out),
                '--data', str(self.data.parent)]
        with aot_prepare(HERE.parent / 'aot') as prepare, \
                mock.patch.object(prepare, 'run_gmlaot', side_effect=self.generate), \
                mock.patch.object(prepare, 'padrefs_file', return_value=None, create=True):
            self.assertEqual(prepare.main(argv), 0)

    def test_same_size_data_changed_within_the_same_second_regenerates(self):
        self.build(b'old input', 100_000_000)
        self.build(b'new input', 900_000_000)
        self.assertEqual(self.runs, [b'old input', b'new input'])
        self.assertEqual((self.out / 'aot_table.c').read_bytes(), b'new input')

    def test_same_bytes_with_another_date_is_up_to_date(self):
        self.build(b'same input', 100_000_000)
        self.build(b'same input', 5_000_000_000)
        self.assertEqual(self.runs, [b'same input'])


if __name__ == '__main__':
    unittest.main()
