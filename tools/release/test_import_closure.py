"""Synthetic closure fixtures. The checker never imports the source being inspected."""
from pathlib import Path
import posixpath
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import import_closure as gate


def build_driver_scripts(read):
    """Inventory repository script paths and recursively sourced shell files without executing Bash."""
    driver = 'platform/3ds/build.sh'
    here = posixpath.dirname(driver)  # build.sh sets $here; its sourced helpers inherit it.
    external_sources = {
        '/etc/profile.d/devkit-env.sh': 'Installed by devkitPro; supplied by the build toolchain, not the kit.',
    }
    scripts, visited = set(), set()

    def resolve(operand, owner):
        operand = operand.strip('\"\'')
        for prefix, base in (('$repo/', ''), ('${repo}/', ''), ('$here/', here + '/'), ('${here}/', here + '/')):
            if operand.startswith(prefix):
                operand = base + operand[len(prefix):]
                break
        assert '$' not in operand, (owner, 'Unresolved script path; resolve or document its scope', operand)
        path = posixpath.normpath(operand)
        assert not path.startswith(('/', '../')), (owner, 'Script path outside repository', operand)
        return path

    def scan(owner):
        if owner in visited:
            return
        visited.add(owner)
        active = '\n'.join(line for line in read(owner).splitlines() if not line.lstrip().startswith('#'))
        active = re.sub(r'''("(?:\\.|[^"\\])*"|'[^']*')|(?<!\S)#[^\n]*''',
                        lambda match: match[1] or '', active)  # Preserve quoted # characters.
        active = active.replace('""', '')  # Adjacent quoted segments are one shell word.
        # Bash joins a quoted variable and the following /suffix into one operand.
        active = re.sub(r'"(\$[^"\n]+)"(?=/)', r'\1', active)

        def cover(match):
            scripts.add(resolve(match[0], owner))
            return ' ' * len(match[0])

        # Inspect variable-rooted operands first, before a literal suffix could hide their indirection.
        uncovered = re.sub(r'\$(?:\{[^}\n]+\}|[A-Za-z_]\w*)[A-Za-z0-9_./${}-]*\.(?:py|sh)\b', cover, active)
        uncovered = re.sub(r'(?<![/\w${}.-])(?:tools|platform)/[A-Za-z0-9_./-]+\.(?:py|sh)', cover, uncovered)
        for external in external_sources:
            uncovered = re.sub(r'(?<![/\w${}.-])' + re.escape(external) + r'(?![/\w${}.-])',
                               lambda match: ' ' * len(match[0]), uncovered)
        # Any script name outside a resolved path fails, regardless of command or its position.
        bare = re.findall(r'\b\w[\w.-]*\.(?:py|sh)\b', uncovered)
        assert not bare, (owner, 'Unresolved bare script path; use $repo/ or $here/', bare)
        for operand in re.findall(r'(?:^|[;&])\s*(?:(?:then|do)\s+)?(?:source|\.)[ \t]+'
                                  r'("[^"]+"|\'[^\']+\'|[^\s;&]+)', active, re.M):
            if operand.strip('\"\'') in external_sources:
                continue
            sourced = resolve(operand, owner)
            scripts.add(sourced)  # Sourced files need not have .sh extensions.
            scan(sourced)

    scan(driver)
    return scripts


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
        for directory in ('tools/roomplan/records', 'tools/roomplan/tracks', 'tools/release/fixtures', 'tools/aot/selection'):
            self.files.update(path.relative_to(root).as_posix() for path in (root / directory).rglob('*') if path.is_file())

    def read(self, name):
        return (self.root / name).read_text(encoding='utf-8-sig')


class ClosureTests(unittest.TestCase):
    def test_every_listed_python_file_is_a_root_including_function_imports(self):
        files = {'tools/listed_test.py': 'def deferred():\n    import unlisted\n',
                 'tools/unlisted.py': 'raise RuntimeError("static check must not execute input")\n'}
        result = self.audit(files, exported=['tools/listed_test.py'], entries=())
        self.assertEqual([row['path'] for row in result['missing']], ['tools/unlisted.py'])

    def test_explicit_path_loaders_and_exec_reads_close_in_function_bodies(self):
        calls = (
            'import importlib.util\ndef run():\n    importlib.util.spec_from_file_location("x", HERE/"other.py")',
            'from importlib.util import spec_from_file_location as load\ndef run():\n    load("x", location=HERE/"other.py")',
            'from importlib.machinery import SourceFileLoader\ndef run():\n    SourceFileLoader("x", str(HERE/"other.py"))',
            'import runpy\ndef run():\n    runpy.run_path(str(HERE/"other.py"))',
            'from runpy import run_path as run\ndef deferred():\n    run(path_name=str(HERE/"other.py"))',
            'def run():\n    exec(open(HERE/"other.py").read())',
            'def run():\n    exec((HERE/"other.py").read_text())',
            'def run():\n    exec(open("other.py").read())',
        )
        for call in calls:
            with self.subTest(call=call):
                files = {'tools/listed.py': 'from pathlib import Path\nHERE=Path(__file__).parent\n'+call,
                         'tools/other.py': ''}
                result = self.audit(files, exported=['tools/listed.py'], entries=())
                self.assertEqual([row['path'] for row in result['missing']], ['tools/other.py'])

    def test_loader_target_absent_even_from_source_is_not_assumed_external(self):
        files = {'tools/listed.py': 'import runpy\nfrom pathlib import Path\n'
                 'runpy.run_path(str(Path(__file__).with_name("missing.py")))'}
        result = self.audit(files, entries=())
        self.assertEqual(result['missing'][0]['path'], 'tools/missing.py')
        self.assertTrue(result['missing'][0]['absent_from_source'])

    def test_py_filename_mentioned_only_in_an_assertion_is_not_a_load(self):
        for assertion in ('assert "other.py" not in []', 'self.assertNotIn("other.py", [])'):
            with self.subTest(assertion=assertion):
                files = {'tools/listed.py': assertion, 'tools/other.py': ''}
                self.assertEqual(self.audit(files, exported=['tools/listed.py'], entries=())['missing'], [])

    def test_subprocess_sibling_script_inside_a_function_is_a_dependency(self):
        files = {'tools/listed.py': 'import subprocess, sys\nfrom pathlib import Path\n'
                 'HERE=Path(__file__).parent\ndef run():\n'
                 '    subprocess.run([sys.executable, str(HERE / "helper.py")])\n',
                 'tools/helper.py': 'raise RuntimeError("do not execute inspected code")\n'}
        result = self.audit(files, exported=['tools/listed.py'], entries=())
        self.assertEqual([row['path'] for row in result['missing']], ['tools/helper.py'])

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
        for folder in ('selection', 'records', 'fixtures', 'tracks'):
            files = {'tools/start.py': 'import os\nHERE=os.path.dirname(__file__)\n'
                     'def find(name):\n    return os.path.join(HERE,"%s",name+".json")' % folder,
                     'tools/%s/required.json' % folder: '{}'}
            result = self.audit(files, exported=['tools/start.py'])
            self.assertEqual([r['path'] for r in result['missing']], ['tools/%s/required.json' % folder])
        self.assertFalse(gate.relevant('tools/roomplan/records/deltarune-version.json', 'undertale'))
        self.assertFalse(gate.relevant('tools/aot/selection/typed-deltarune.txt', 'undertale'))
        self.assertFalse(gate.relevant('tools/aot/selection/typed-priority-deltarune-n3ds.txt', 'undertale'))
        self.assertTrue(gate.relevant('tools/aot/selection/typed-priority-deltarune-n3ds.txt', 'deltarune'))
        self.assertFalse(gate.relevant('tools/aot/selection/typed-undertale-o3ds.txt', 'deltarune'))
        self.assertTrue(gate.relevant('tools/aot/selection/typed-undertale-o3ds.txt', 'undertale'))
        self.assertFalse(gate.relevant('tools/roomplan/tracks/deltarune.json', 'undertale'))

    def test_track_plan_driver_requires_its_default_record_only_for_deltarune(self):
        files = {'build.sh': 'python tools/roomplan/build_tracks.py --out output\n',
                 'tools/roomplan/build_tracks.py': 'import argparse, os\n'
                     'HERE=os.path.dirname(__file__)\n'
                     'ap=argparse.ArgumentParser()\n'
                     'ap.add_argument("--records", default=os.path.join(HERE,"tracks","deltarune.json"))\n',
                 'tools/roomplan/tracks/deltarune.json': '{}'}
        result = gate.check(Source(files), set(files) - {'tools/roomplan/tracks/deltarune.json'},
                            'deltarune', entry_points=(), drivers=('build.sh',))
        self.assertEqual([r['path'] for r in result['missing']], ['tools/roomplan/tracks/deltarune.json'])
        result = gate.check(Source(files), {'build.sh'}, 'undertale', entry_points=(), drivers=('build.sh',))
        self.assertEqual(result['missing'], [])

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

    def test_gog_font_helper_and_profile_are_required_only_for_undertale(self):
        files = {'build.sh': 'python tools/aot/fontverdict.py\npython tools/aot/gog_smart.py\n',
                 'tools/aot/fontverdict.py': 'def gog_only():\n    import gog_smart\n',
                 'tools/aot/gog_smart.py': 'from pathlib import Path\n'
                     'PROFILE=Path(__file__).with_name("gog-smart-1.08.json")\n'
                     'def read():\n    return PROFILE.read_text()\n',
                 'tools/aot/gog-smart-1.08.json': '{}'}
        for missing in ('tools/aot/gog_smart.py', 'tools/aot/gog-smart-1.08.json'):
            with self.subTest(missing=missing):
                result = self.audit(files, exported=set(files) - {missing}, entries=(), drivers=('build.sh',))
                self.assertIn(missing, {row['path'] for row in result['missing']})
        result = gate.check(Source(files), {'build.sh', 'tools/aot/fontverdict.py'}, 'deltarune',
                            entry_points=(), drivers=('build.sh',))
        self.assertEqual(result['missing'], [])
        self.assertNotIn('tools/aot/gog_smart.py', result['required'])

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
                 if (root / ('tools/release/manifest-%s.txt' % short)).is_file()
                 for line in (root / ('tools/release/manifest-%s.txt' % short)).read_text().splitlines()
                 if line.strip() and not line.lstrip().startswith('#')}
        repo = FilesystemSource(root, names)
        for game, short in (('undertale', 'ut'), ('deltarune', 'dr')):
            with self.subTest(game=game):
                path = root / ('tools/release/manifest-%s.txt' % short)
                if not path.is_file():
                    self.skipTest('This source export does not include the ' + short + ' manifest')
                manifest = {line.strip() for line in path.read_text().splitlines()
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
            with self.subTest(game=game):
                path = root / ('tools/release/manifest-%s.txt' % short)
                if not path.is_file():
                    self.skipTest('This source export does not include the ' + short + ' manifest')
                self.assertTrue(pipeline.source_wanted('tools/roomplan/closure.py', game))
                manifest = path.read_text().splitlines()
                self.assertIn('tools/roomplan/closure.py', manifest)
        self.assertTrue((root / 'tools/roomplan/closure.py').is_file())

    def test_packaged_build_driver_closure_survives_the_source_filter(self):
        root = Path(__file__).resolve().parents[2]
        sys.path.insert(0, str(root / 'tools/release/builder'))
        import pipeline
        names = {line.strip() for short in ('ut', 'dr')
                 if (root / ('tools/release/manifest-%s.txt' % short)).is_file()
                 for line in (root / ('tools/release/manifest-%s.txt' % short)).read_text().splitlines()
                 if line.strip() and not line.lstrip().startswith('#')}
        repo = FilesystemSource(root, names)
        for game, short in (('undertale', 'ut'), ('deltarune', 'dr')):
            with self.subTest(game=game):
                if not (root / ('tools/release/manifest-%s.txt' % short)).is_file():
                    self.skipTest('This source export does not include the ' + short + ' manifest')
                manifest = {line.strip() for line in (root / ('tools/release/manifest-%s.txt' % short)).read_text().splitlines()
                            if line.strip() and not line.lstrip().startswith('#')}
                mirrored = {name for name in manifest if pipeline.source_wanted(name, game)}
                # The compiler mirror runs build drivers, not all shipped analysis commands/tests.
                report = gate.check(repo, mirrored, game, entry_points=(), include_exported_python=False)
                self.assertEqual(report['missing'], [])
                if game == 'deltarune':
                    self.assertIn('tools/roomplan/build_tracks.py', report['required'])
                    self.assertIn('tools/roomplan/tracks/deltarune.json', report['required'])

    def assert_undertale_build_scripts_in_kit(self):
        root = Path(__file__).resolve().parents[2]
        sys.path.insert(0, str(root / 'tools/release/builder'))
        import pipeline
        manifest_path = root / 'tools/release/manifest-ut.txt'
        if not manifest_path.is_file():
            self.skipTest('This source export does not include the ut manifest')
        manifest = {line.strip() for line in manifest_path.read_text().splitlines()
                    if line.strip() and not line.lstrip().startswith('#')}
        driver = '\n'.join(line for line in (root / 'platform/3ds/build.sh').read_text().splitlines()
                           if not line.lstrip().startswith('#'))
        # The only shell dependency is disabled before its call for UT, even with default build flags.
        self.assertRegex(driver, r'if \[ "\$game" = undertale \]; then\s+openh264=OFF\s+fi\s+'
                                 r'if \[ "\$openh264" = ON \] && ! bash "\$repo/tools/deps/openh264/build\.sh"')
        def read(name):
            self.assertTrue((root / name).is_file(), name)
            return (root / name).read_text()

        # Include conditional steps, compiler launchers, $here paths and recursively sourced helpers.
        # Product scopes and optional developer packers follow the release closure checker's policy.
        scripts = {name for name in build_driver_scripts(read)
                   if gate.relevant(name, 'undertale') and name not in gate.OPTIONAL_PACKERS
                   and name != 'tools/deps/openh264/build.sh'}
        self.assertTrue(scripts, 'No build scripts discovered')
        self.assertEqual(sorted(scripts - manifest), [], 'build.sh scripts missing from the UT kit manifest')
        self.assertEqual(sorted(name for name in scripts if not pipeline.source_wanted(name, 'undertale')),
                         [], 'build.sh scripts dropped by the UT source filter')
        for name in sorted(scripts):
            with self.subTest(script=name):
                self.assertTrue((root / name).is_file(), name)
                if gate.GAME_ONLY.get(name) == 'undertale':
                    self.assertFalse(pipeline.source_wanted(name, 'deltarune'))
                    dr_manifest = root / 'tools/release/manifest-dr.txt'
                    if dr_manifest.is_file():
                        self.assertNotIn(name, dr_manifest.read_text().splitlines())

    def test_every_undertale_build_script_is_in_the_builder_kit(self):
        self.assert_undertale_build_scripts_in_kit()

    def test_missing_sourced_python_helper_fails_kit_check(self):
        root = Path(__file__).resolve().parents[2]
        original_read, original_exists = Path.read_text, Path.is_file
        helper = 'platform/3ds/python.sh'
        for missing in ('manifest entry', 'source file'):
            with self.subTest(missing=missing):
                def read(path, *args, **kwargs):
                    text = original_read(path, *args, **kwargs)
                    if missing == 'manifest entry' and path == root / 'tools/release/manifest-ut.txt':
                        text = '\n'.join(line for line in text.splitlines() if line.strip() != helper)
                    return text

                def exists(path):
                    return False if missing == 'source file' and path == root / helper else original_exists(path)

                with patch.object(Path, 'read_text', read), patch.object(Path, 'is_file', exists):
                    with self.assertRaisesRegex(AssertionError, re.escape(helper)):
                        self.assert_undertale_build_scripts_in_kit()

    def test_indirect_script_paths_fail_kit_check(self):
        root = Path(__file__).resolve().parents[2]
        original_read = Path.read_text
        for owner in ('platform/3ds/build.sh', 'platform/3ds/python.sh'):
            for operand in ('$aotdir/x.py', '${aotdir}/x.py', '${aotdir:-fallback}/x.py', '$script.py', '$shelldir/x.sh'):
                with self.subTest(owner=owner, operand=operand):
                    def read(path, *args, **kwargs):
                        text = original_read(path, *args, **kwargs)
                        if path == root / owner:
                            text += '\naotdir="$repo/tools/aot"\n"${python[@]}" "' + operand + '"\n'
                        return text

                    with patch.object(Path, 'read_text', read):
                        with self.assertRaisesRegex(AssertionError, 'Unresolved script path'):
                            self.assert_undertale_build_scripts_in_kit()

    def test_script_inventory_follows_dot_sources_and_here_paths(self):
        files = {'platform/3ds/build.sh': 'source "$here"/python.sh # env python ignored.py\n# ./ignored.sh\n',
                 'platform/3ds/python.sh': '. "${here}"/nested\npython "$here"/local.py\n',
                 'platform/3ds/nested': 'source "$here/python.sh"\npython "$repo""/tools/check.py"\n'}
        self.assertEqual(build_driver_scripts(files.__getitem__),
                         {'platform/3ds/python.sh', 'platform/3ds/nested',
                          'platform/3ds/local.py', 'tools/check.py'})
        files['platform/3ds/build.sh'] = 'if true; then source "$helpers/common"; fi\n'
        with self.assertRaisesRegex(AssertionError, 'Unresolved script path'):
            build_driver_scripts(files.__getitem__)

    def test_quote_split_and_cd_relative_calls_fail_kit_check(self):
        root = Path(__file__).resolve().parents[2]
        original_read = Path.read_text
        mutants = (
            ('quote split', 'python "$aotdir"/newguard3.py', 'Unresolved script path'),
            ('bare after cd', '(cd "$repo/tools/aot" && python newguard4.py)', 'Unresolved bare script path'),
        )
        for name, command, reason in mutants:
            with self.subTest(mutant=name):
                def read(path, *args, **kwargs):
                    text = original_read(path, *args, **kwargs)
                    return text + '\n' + command + '\n' if path == root / 'platform/3ds/build.sh' else text

                with patch.object(Path, 'read_text', read):
                    with self.assertRaisesRegex(AssertionError, reason):
                        self.assert_undertale_build_scripts_in_kit()

    def test_unresolved_script_tokens_fail_regardless_of_command_position(self):
        root = Path(__file__).resolve().parents[2]
        original_read = Path.read_text
        mutants = (
            'if python x.py; then :; fi',
            'for item in 1; do python x.py; done',
            'env python x.py',
            'exec python x.py',
            'time python x.py',
            'VAR=1 python x.py',
            '(cd "$repo/tools/aot" && ./x.py)',
            'python "$d""/x.py"',
        )
        for command in mutants:
            with self.subTest(mutant=command):
                def read(path, *args, **kwargs):
                    text = original_read(path, *args, **kwargs)
                    return text + '\n' + command + '\n' if path == root / 'platform/3ds/build.sh' else text

                with patch.object(Path, 'read_text', read):
                    with self.assertRaisesRegex(AssertionError, 'Unresolved (?:bare )?script path'):
                        self.assert_undertale_build_scripts_in_kit()


if __name__ == '__main__':
    unittest.main()
