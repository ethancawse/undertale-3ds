"""Synthetic closure fixtures. The checker never imports the source being inspected."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import import_closure as gate


class Source:
    def __init__(self, files):
        self.files = set(files)
        self.contents = files

    def read(self, name):
        return self.contents[name]


class FilesystemSource:
    """Audit the live source files without depending on the host's worktree pointer format."""
    def __init__(self, root, names):
        self.root = root
        self.files = {name for name in names if (root / name).is_file()}
        for directory in ('tools/aot', 'tools/roomplan', 'tools/buildcache', 'tools/release'):
            self.files.update(path.relative_to(root).as_posix() for path in (root / directory).rglob('*.py'))
        for directory in ('tools/roomplan/records', 'tools/release/fixtures', 'tools/aot/selection'):
            self.files.update(path.relative_to(root).as_posix() for path in (root / directory).rglob('*') if path.is_file())

    def read(self, name):
        return (self.root / name).read_text(encoding='utf-8-sig')


class ClosureTests(unittest.TestCase):
    def audit(self, files, exported=None, entries=('tools/start.py',), drivers=()):
        return gate.check(Source(files), set(files) if exported is None else set(exported),
                          'undertale', entry_points=entries, drivers=drivers)

    def git_fixture(self, directory, files):
        for name, text in files.items():
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding='utf-8')
        for command in (['git', 'init', '--quiet'], ['git', 'add', '--', *files]):
            subprocess.run(command, cwd=directory, stdin=subprocess.DEVNULL, capture_output=True,
                           timeout=30, check=True)

    def test_real_git_index_finds_an_omitted_transitive_input(self):
        root = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory(dir=root / 'scratch') as directory:
            self.git_fixture(Path(directory), {'tools/start.py': 'import helper', 'tools/helper.py': ''})
            repo = gate.Repository(directory)
            report = gate.check(repo, {'tools/start.py'}, 'undertale', entry_points=('tools/start.py',), drivers=())
            self.assertEqual([r['path'] for r in report['missing']], ['tools/helper.py'])

    def test_transitive_import_path_insert_and_cycle(self):
        files = {
            'tools/start.py': 'from pathlib import Path\nimport sys\n'
                              'sys.path.insert(0, str(Path(__file__).parent / "helpers"))\nimport first',
            'tools/helpers/first.py': 'import second',
            'tools/helpers/second.py': 'import first\nraise RuntimeError("never execute audited input")',
        }
        result = self.audit(files, exported=['tools/start.py', 'tools/helpers/first.py'])
        self.assertEqual([row['path'] for row in result['missing']], ['tools/helpers/second.py'])
        self.assertEqual(len(result['python_modules']), 3)

    def test_package_initializers_and_relative_imports(self):
        files = {'tools/start.py': 'import package.first',
                 'tools/package/__init__.py': 'from . import other',
                 'tools/package/first.py': 'from . import second',
                 'tools/package/second.py': '', 'tools/package/other.py': ''}
        result = self.audit(files, exported=['tools/start.py'])
        self.assertEqual({r['path'] for r in result['missing']}, set(files) - {'tools/start.py'})

    def test_fixed_file_and_function_local_read(self):
        files = {'tools/start.py': 'from pathlib import Path\nHERE=Path(__file__).parent\n'
                 'def read():\n    payload=HERE / "settings.json"\n    return payload.read_text()\n',
                 'tools/settings.json': '{}'}
        result = self.audit(files, exported=['tools/start.py'])
        self.assertEqual([r['path'] for r in result['missing']], ['tools/settings.json'])

    def test_fixed_directory_with_dynamic_filename_and_product_filter(self):
        for folder in ('selection', 'records', 'fixtures'):
            files = {'tools/start.py': 'import os\nHERE=os.path.dirname(__file__)\n'
                     'def find(name):\n    return os.path.join(HERE,"%s",name+".json")' % folder,
                     'tools/%s/required.json' % folder: '{}'}
            result = self.audit(files, exported=['tools/start.py'])
            self.assertEqual([r['path'] for r in result['missing']], ['tools/%s/required.json' % folder])
        self.assertFalse(gate.relevant('tools/roomplan/records/deltarune-version.json', 'undertale'))
        self.assertFalse(gate.relevant('tools/aot/selection/typed-deltarune.txt', 'undertale'))

    def test_write_only_outputs_and_player_inputs_are_not_required(self):
        files = {'tools/start.py': 'from pathlib import Path\nHERE=Path(__file__).parent\n'
                 'def run(player):\n    open(HERE/"out.json", "w")\n    Path(player).read_bytes()\n',
                 'tools/out.json': '{}'}
        self.assertEqual(self.audit(files, exported=['tools/start.py'])['missing'], [])

    def test_sound_plan_driver_import_and_nested_records_are_required(self):
        files = {'build.sh': 'python "$repo/tools/roomplan/build_sfxplan.py" --out output.dtp',
                 'tools/roomplan/build_sfxplan.py':
                     'import os, sys\nHERE=os.path.dirname(os.path.abspath(__file__))\n'
                     'sys.path.insert(0, os.path.join(HERE,"..","aot"))\nimport parser\n'
                     'RECORDS=os.path.join(HERE,"records","sounds")\n'
                     'def find(name):\n    return open(os.path.join(RECORDS,name)).read()\n',
                 'tools/aot/parser.py': '',
                 'tools/roomplan/records/sounds/undertale-invented.json': '{}'}
        exported = set(files)
        for missing in set(files) - {'build.sh'}:
            with self.subTest(missing=missing):
                result = self.audit(files, exported=exported - {missing}, entries=(), drivers=('build.sh',))
                self.assertIn(missing, {row['path'] for row in result['missing']})
        result = gate.check(Source(files), {'build.sh'}, 'deltarune', entry_points=(), drivers=('build.sh',))
        self.assertEqual(result['missing'], [])

    def test_driver_discovers_new_entry_and_missing_entry(self):
        files = {'build.sh': 'python tools/new.py\npython tools/missing.py\n',
                 'tools/new.py': 'import helper', 'tools/helper.py': ''}
        result = self.audit(files, exported=['build.sh'], entries=(), drivers=('build.sh',))
        self.assertEqual({r['path'] for r in result['missing']},
                         {'tools/new.py', 'tools/helper.py', 'tools/missing.py'})
        self.assertTrue(next(r for r in result['missing'] if r['path'] == 'tools/missing.py')['absent_from_source'])

    def test_dynamic_import_and_fixed_fingerprint_list(self):
        files = {'tools/start.py': 'import importlib\nimportlib.import_module("helper")\n'
                 'GENERATOR_FILES=("generator.py",)\n',
                 'tools/helper.py': '', 'tools/generator.py': ''}
        self.assertEqual(set(self.audit(files)['required']), set(files))

    def test_manifest_cannot_name_a_file_absent_from_the_revision(self):
        result = self.audit({'tools/start.py': ''}, exported=['tools/start.py', 'tools/absent.txt'])
        self.assertEqual(result['missing'], [dict(path='tools/absent.txt', required_by='manifest entry',
                                                  absent_from_source=True)])

    def test_scope_locals_do_not_overwrite_module_paths(self):
        files = {'tools/start.py': 'from pathlib import Path\nHERE=Path(__file__).parent\n'
                 'def unused():\n    HERE="/elsewhere"\n'
                 'def reader():\n    return (HERE/"input.json").read_bytes()\n',
                 'tools/input.json': '{}'}
        self.assertIn('tools/input.json', self.audit(files)['required'])

    def test_both_current_manifests_close(self):
        root = Path(__file__).resolve().parents[2]
        names = {line.strip() for short in ('ut', 'dr')
                 for line in (root / ('tools/release/manifest-%s.txt' % short)).read_text().splitlines()
                 if line.strip() and not line.lstrip().startswith('#')}
        repo = FilesystemSource(root, names)
        for game, short in (('undertale', 'ut'), ('deltarune', 'dr')):
            manifest = {line.strip() for line in (root / ('tools/release/manifest-%s.txt' % short)).read_text().splitlines()
                        if line.strip() and not line.lstrip().startswith('#')}
            report = gate.check(repo, manifest, game)
            self.assertEqual(report['missing'], [], game)
            self.assertIn('tools/aot/escape.py', report['required'])
            self.assertIn('tools/release/import_closure.py', report['required'])
            self.assertTrue(any(p.startswith('tools/aot/selection/') for p in report['required']))

    def test_release_gate_calls_closure_before_assembling_archive(self):
        root = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory(dir=root / 'scratch') as directory:
            temporary = Path(directory)
            self.git_fixture(temporary, {name: (root / name).read_text() for name in
                             ('tools/release/check_release.py', 'tools/release/import_closure.py',
                              'tools/release/generate_notices.py', 'tools/release/release_contract.py')})
            (temporary / 'scratch').mkdir()
            manifest = temporary / 'missing-inputs.txt'
            manifest.write_text('tools/release/import_closure.py\n', encoding='utf-8')
            output = Path(directory).name + '-blocked'
            result = subprocess.run([sys.executable, '-I', str(temporary / 'tools/release/check_release.py'),
                                     '--manifest', manifest.relative_to(temporary).as_posix(), '--output', output,
                                     '--game', 'undertale'],
                                    cwd=temporary, stdin=subprocess.DEVNULL, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertIn(b'release input closure is incomplete', result.stderr)
            self.assertFalse((temporary / 'scratch' / (output + '.zip')).exists())

    def test_both_source_mirrors_keep_upstream_roomplan_helper(self):
        root = Path(__file__).resolve().parents[2]
        sys.path.insert(0, str(root / 'tools/release/builder'))
        import pipeline
        for game, short in (('undertale', 'ut'), ('deltarune', 'dr')):
            self.assertTrue(pipeline.source_wanted('tools/roomplan/closure.py', game))
            manifest = (root / ('tools/release/manifest-%s.txt' % short)).read_text().splitlines()
            self.assertIn('tools/roomplan/closure.py', manifest)
        self.assertTrue((root / 'tools/roomplan/closure.py').is_file())


if __name__ == '__main__':
    unittest.main()
