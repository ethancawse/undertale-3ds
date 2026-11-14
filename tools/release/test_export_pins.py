"""Content pins survive the export: a pinned file the cleanup changes fails unless its pin lists the new bytes."""
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('export_pins', Path(__file__).with_name('export_pins.py'))
pins = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pins)


def digest(data):
    return hashlib.sha256(data).hexdigest()


SOURCE = b'// internal note\nint x;\n'
CLEAN = b'// note\nint x;\n'


def tool(*digests):
    return ('PINS = ' + repr(list(digests)) + '\n').encode()


class ExportPinTests(unittest.TestCase):
    def test_cleanup_of_a_pinned_file_breaks_its_pin(self):
        source = {'src/a.c': SOURCE, 'tools/build.py': tool(digest(SOURCE))}
        exported = {'src/a.c': CLEAN, 'tools/build.py': tool(digest(SOURCE))}
        [(name, before, after, pinners)] = pins.broken(source, exported)
        self.assertEqual((name, before, after, pinners), ('src/a.c', digest(SOURCE), digest(CLEAN), ['tools/build.py']))
        with self.assertRaisesRegex(ValueError, 'export breaks content pins: src/a.c'):
            pins.check(source, exported)

    def test_unchanged_pinned_file_and_a_listed_new_digest_pass(self):
        source = {'src/a.c': SOURCE, 'tools/build.py': tool(digest(SOURCE))}
        self.assertEqual(pins.broken(source, dict(source)), [])
        exported = {'src/a.c': CLEAN, 'tools/build.py': tool(digest(SOURCE), digest(CLEAN))}
        self.assertEqual(pins.broken(source, exported), [])

    def test_unpinned_changes_and_identifier_approvals_are_not_pins(self):
        policy = 'tools/release/identifier-allowlist.json'
        source = {'src/a.c': SOURCE, policy: ('{"sha256": "%s"}' % digest(SOURCE)).encode()}
        exported = {'src/a.c': CLEAN, policy: ('{"sha256": "%s"}' % digest(CLEAN)).encode()}
        self.assertEqual(pins.broken(source, exported), [])
        self.assertEqual(pins.broken({'src/a.c': SOURCE}, {'src/a.c': CLEAN}), [])

    def test_a_pin_in_a_file_left_out_of_the_export_does_not_count(self):
        source = {'src/a.c': SOURCE, 'tools/dev_only.py': tool(digest(SOURCE))}
        self.assertEqual(pins.broken(source, {'src/a.c': CLEAN}), [])

    def test_longer_hex_runs_are_not_pins(self):
        source = {'src/a.c': SOURCE, 'tools/build.py': ('X = "%s0"\n' % digest(SOURCE)).encode()}
        self.assertEqual(pins.broken(source, {'src/a.c': CLEAN, 'tools/build.py': source['tools/build.py']}), [])

    def test_pins_are_found_in_any_letter_case(self):
        for pin in (digest(SOURCE).upper(), digest(SOURCE)[:32] + digest(SOURCE)[32:].upper()):
            source = {'src/a.c': SOURCE, 'tools/build.cmake': ('set(PIN %s)\n' % pin).encode()}
            exported = {'src/a.c': CLEAN, 'tools/build.cmake': source['tools/build.cmake']}
            [(name, _, _, pinners)] = pins.broken(source, exported)
            self.assertEqual((name, pinners), ('src/a.c', ['tools/build.cmake']))
            exported['tools/build.cmake'] += ('set(CLEAN %s)\n' % digest(CLEAN).upper()).encode()
            self.assertEqual(pins.broken(source, exported), [])


GIT = ['git', '-c', 'user.name=t', '-c', 'user.email=t@invalid', '-c', 'core.autocrlf=false']


def write(root, files):
    for name, data in files.items():
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        (root / name).write_bytes(data)


def cli(tree, commit, repo, *extra):
    return subprocess.run([sys.executable, '-I', str(Path(pins.__file__)), str(tree), '--source-commit', commit,
                           '--repo', str(repo)] + list(extra), capture_output=True, text=True,
                          stdin=subprocess.DEVNULL, timeout=60)


class CommandLineTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.repo, self.tree = Path(temp.name, 'repo'), Path(temp.name, 'tree')
        write(self.repo, {'src/a.c': SOURCE, 'tools/build.py': tool(digest(SOURCE))})
        for step in (['init', '-q'], ['add', '-A'], ['commit', '-q', '-m', 'pins']):
            subprocess.run(GIT + ['-C', str(self.repo)] + step, check=True, capture_output=True,
                           stdin=subprocess.DEVNULL, timeout=60)

    def test_changed_pinned_file_fails_and_unchanged_passes(self):
        write(self.tree, {'src/a.c': CLEAN, 'tools/build.py': tool(digest(SOURCE))})
        run = cli(self.tree, 'HEAD', self.repo)
        self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
        self.assertIn('BROKEN src/a.c', run.stdout)
        write(self.tree, {'src/a.c': SOURCE})
        run = cli(self.tree, 'HEAD', self.repo)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('2 files, 2 from commit', run.stdout)

    def test_missing_tree_fails(self):
        run = cli(self.tree, 'HEAD', self.repo)
        self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
        self.assertIn('not a directory', run.stdout)

    def test_empty_tree_fails(self):
        (self.tree / 'empty').mkdir(parents=True)
        run = cli(self.tree, 'HEAD', self.repo)
        self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
        self.assertIn('has no files', run.stdout)

    def test_unknown_commit_fails(self):
        write(self.tree, {'src/a.c': SOURCE, 'tools/build.py': tool(digest(SOURCE))})
        for ref in ('0' * 40, 'no-such-ref', 'HEAD:src'):
            run = cli(self.tree, ref, self.repo)
            self.assertEqual(run.returncode, 2, ref + ': ' + run.stdout + run.stderr)
            self.assertIn('does not resolve', run.stdout)

    def test_polluted_tree_fails_and_names_its_extras(self):
        write(self.tree, {'src/a.c': SOURCE, 'tools/build.py': tool(digest(SOURCE)),
                          'tools/__pycache__/build.cpython-314.pyc': b'\0junk', 'stray/x.txt': b'x'})
        run = cli(self.tree, 'HEAD', self.repo)
        self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
        self.assertIn('2 tree files are not in', run.stdout)
        self.assertIn('stray/x.txt, tools/__pycache__/build.cpython-314.pyc', run.stdout)
        many = {'junk/%02d.txt' % i: b'x' for i in range(25)}
        write(self.tree, many)
        run = cli(self.tree, 'HEAD', self.repo)
        self.assertIn('27 tree files are not in', run.stdout)
        self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
        self.assertIn('junk/19.txt, ...', run.stdout)
        self.assertNotIn('junk/20.txt', run.stdout)

    def test_named_extra_passes_and_is_listed(self):
        write(self.tree, {'src/a.c': SOURCE, 'tools/build.py': tool(digest(SOURCE)), '.github/x.yml': b'on: push\n'})
        run = cli(self.tree, 'HEAD', self.repo, '--allow-extra', '.github/x.yml')
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('EXTRA (allowed) .github/x.yml', run.stdout)
        self.assertIn('3 files, 2 from commit', run.stdout)
        self.assertIn('1 allowed extra, 0 broken', run.stdout)

    def test_stale_or_needless_allowance_fails(self):
        write(self.tree, {'src/a.c': SOURCE, 'tools/build.py': tool(digest(SOURCE))})
        for name in ('not/there.txt', 'src/a.c'):
            run = cli(self.tree, 'HEAD', self.repo, '--allow-extra', name)
            self.assertEqual(run.returncode, 2, name + ': ' + run.stdout + run.stderr)
            self.assertIn('not extra files in this tree: ' + name, run.stdout)

    def test_wildcard_and_loose_allowances_are_refused(self):
        write(self.tree, {'src/a.c': SOURCE, 'tools/build.py': tool(digest(SOURCE)), 'x/y.txt': b'y'})
        for name in ('x/*', '*', 'x/?.txt', '/x/y.txt', 'x/../x/y.txt', 'x\\y.txt', 'C:x/y.txt', ''):
            run = cli(self.tree, 'HEAD', self.repo, '--allow-extra', name)
            self.assertEqual(run.returncode, 2, repr(name) + ': ' + run.stdout + run.stderr)
            self.assertIn('takes one exact relative tree path', run.stdout)

    def test_partial_overlap_never_passes(self):
        write(self.tree, {'src/a.c': SOURCE, 'new/b.c': CLEAN})
        run = cli(self.tree, 'HEAD', self.repo)
        self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
        self.assertIn('1 tree files are not in', run.stdout)

    def test_a_file_at_a_directory_path_is_a_type_mismatch(self):
        write(self.tree, {'src': SOURCE})  # zero blob overlap: the only name is a tree in the commit
        run = cli(self.tree, 'HEAD', self.repo)
        self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
        self.assertIn('type mismatch: 1 exported files are not files in', run.stdout)
        self.assertIn('src (tree)', run.stdout)
        write(self.tree, {'tools/build.py': tool(digest(SOURCE))})  # mixed: one blob, one tree
        for extra in ([], ['--allow-extra', 'src']):
            run = cli(self.tree, 'HEAD', self.repo, *extra)
            self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
            self.assertIn('src (tree)', run.stdout)

    def test_a_file_at_a_submodule_path_is_a_type_mismatch(self):
        head = subprocess.run(GIT + ['-C', str(self.repo), 'rev-parse', 'HEAD'], check=True, capture_output=True,
                              text=True, stdin=subprocess.DEVNULL, timeout=60).stdout.strip()
        for step in (['update-index', '--add', '--cacheinfo', '160000,%s,sub' % head], ['commit', '-q', '-m', 'sub']):
            subprocess.run(GIT + ['-C', str(self.repo)] + step, check=True, capture_output=True,
                           stdin=subprocess.DEVNULL, timeout=60)
        write(self.tree, {'src/a.c': SOURCE, 'tools/build.py': tool(digest(SOURCE)), 'sub': b'x'})
        run = cli(self.tree, 'HEAD', self.repo)
        self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
        self.assertIn('sub (commit)', run.stdout)

    def junction(self, target, link):
        if sys.platform != 'win32':
            self.skipTest('junctions are Windows only')
        import _winapi
        link.parent.mkdir(parents=True, exist_ok=True)
        _winapi.CreateJunction(str(target), str(link))

    def outside(self):
        outside = self.tree.parent / 'outside'
        write(outside, {'src/a.c': SOURCE, 'x.txt': b'x'})
        return outside

    def test_junction_out_of_the_tree_fails(self):
        write(self.tree, {'tools/build.py': tool(digest(SOURCE))})
        self.junction(self.outside() / 'src', self.tree / 'src')
        self.assertEqual((self.tree / 'src' / 'a.c').read_bytes(), SOURCE)  # the control: it does reach outside
        run = cli(self.tree, 'HEAD', self.repo)
        self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
        self.assertIn('symlink, junction or reparse point: src', run.stdout)

    def test_dangling_junction_fails(self):
        write(self.tree, {'src/a.c': SOURCE, 'tools/build.py': tool(digest(SOURCE))})
        gone = self.tree.parent / 'gone'
        gone.mkdir()
        self.junction(gone, self.tree / 'docs' / 'old')
        gone.rmdir()
        run = cli(self.tree, 'HEAD', self.repo)
        self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
        self.assertIn('symlink, junction or reparse point: docs/old', run.stdout)

    def test_allowance_does_not_exempt_a_junction(self):
        write(self.tree, {'src/a.c': SOURCE, 'tools/build.py': tool(digest(SOURCE))})
        self.junction(self.outside(), self.tree / 'ext')
        run = cli(self.tree, 'HEAD', self.repo, '--allow-extra', 'ext/x.txt')
        self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
        self.assertIn('symlink, junction or reparse point: ext', run.stdout)

    def test_symlink_fails(self):
        write(self.tree, {'tools/build.py': tool(digest(SOURCE))})
        (self.tree / 'src').mkdir()
        try:
            os.symlink(self.outside() / 'src' / 'a.c', self.tree / 'src' / 'a.c')
        except (OSError, NotImplementedError) as error:
            self.skipTest('this host cannot create a symlink: %s' % error)
        run = cli(self.tree, 'HEAD', self.repo)
        self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
        self.assertIn('symlink, junction or reparse point: src/a.c', run.stdout)

    def test_commit_sharing_no_files_with_the_tree_fails(self):
        write(self.tree, {'other/b.c': CLEAN})
        run = cli(self.tree, 'HEAD', self.repo)
        self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
        self.assertIn('none of the 1 exported files', run.stdout)


if __name__ == '__main__':
    unittest.main()
