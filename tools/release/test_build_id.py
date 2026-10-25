"""A source archive gets its own stamp even inside a different Git checkout."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
CMAKE = shutil.which('cmake')
GIT = shutil.which('git')


@unittest.skipUnless(CMAKE and GIT, 'CMake and Git required')
class BuildIdTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / 'scratch')
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.source = self.root / 'platform/3ds'
        self.source.mkdir(parents=True)
        (self.root / 'VERSION').write_text('1.2.3\n', encoding='utf-8')
        contract = self.root / 'docs/release'
        contract.mkdir(parents=True)
        shutil.copyfile(ROOT / 'docs/release/build-contract.json', contract / 'build-contract.json')
        self.output = self.root / 'build_id.h'

    def run_cmake(self, *args):
        return subprocess.run([CMAKE, '-DSOURCE_DIR=' + self.source.as_posix(),
                               '-DOUT=' + self.output.as_posix(), '-DGIT_EXECUTABLE=' + GIT,
                               *args, '-P', str(ROOT / 'platform/3ds/build_id.cmake')],
                              stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=20)

    def test_nested_archive_ignores_parent_git_id(self):
        result = self.run_cmake()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('"archive-1.2.3"', self.output.read_text())

    def test_archive_aot_suffix_is_preserved(self):
        aot = self.root / 'aot'
        aot.mkdir()
        (aot / 'aot_modules.txt').write_text('tag aot4ch\n', encoding='utf-8')
        result = self.run_cmake('-DAOT_DIR=' + aot.as_posix())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('"archive-1.2.3+aot4ch"', self.output.read_text())

    def test_invalid_stamp_fails(self):
        (self.root / 'VERSION').write_text('broken"stamp\n', encoding='utf-8')
        result = self.run_cmake()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('VERSION must contain', result.stderr)

    def test_beta_archive_keeps_full_label_and_aot(self):
        (self.root / 'VERSION').write_text('1.0.0-beta.1\n', encoding='utf-8')
        aot = self.root / 'aot'
        aot.mkdir()
        (aot / 'aot_modules.txt').write_text('tag aot12ch\n', encoding='utf-8')
        result = self.run_cmake('-DAOT_DIR=' + aot.as_posix())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('"archive-1.0.0-beta.1+aot12ch"', self.output.read_text())

    def test_cia_version_overflow_fails(self):
        (self.root / 'VERSION').write_text('1.2.16\n', encoding='utf-8')
        result = self.run_cmake()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('CIA limits', result.stderr)

    def test_overlong_aot_id_fails_instead_of_truncating(self):
        aot = self.root / 'aot'
        aot.mkdir()
        (aot / 'aot_modules.txt').write_text('tag ' + 'x' * 40 + '\n', encoding='utf-8')
        result = self.run_cmake('-DAOT_DIR=' + aot.as_posix())
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('release contract holds 38', result.stderr)


if __name__ == '__main__':
    unittest.main()
