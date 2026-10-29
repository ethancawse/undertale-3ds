"""The builder and CMake consume one package template, including diagnostic text."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import sys
import os

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_contract

ROOT = Path(__file__).resolve().parents[2]


class SourceDescriptorTests(unittest.TestCase):
    def public_export(self, files):
        temp = tempfile.TemporaryDirectory(dir=ROOT / 'scratch')
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        def git(*args):
            return subprocess.check_output(['git', '-C', str(root), *args], stdin=subprocess.DEVNULL,
                                           stderr=subprocess.PIPE, timeout=20).decode().strip()
        git('init', '-q')
        for name, data in files.items():
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_bytes(data)
        git('-c', 'core.autocrlf=false', 'add', '.')
        message = root / '.git/fixture-message'
        message.write_text('Invented public export\n', encoding='utf-8')
        git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
            '-c', 'commit.gpgsign=false', 'commit', '-q', '-F', str(message))
        return root, git('rev-parse', 'HEAD')

    def test_public_tree_with_issue_templates_and_crlf_launcher(self):
        # Same shape as the public tree: repository-only issue templates and a
        # Windows launcher stored as LF but exported from a CRLF checkout.
        files = {'.gitattributes': b'* text=auto eol=lf\n*.cmd text eol=crlf\n',
                 'VERSION': b'1.0.0-beta.1\n',
                 'tools/release/builder/undertale-3ds-builder.cmd': b'@echo off\nexit /b\n'}
        root, commit = self.public_export(dict(files, **{
            '.github/ISSUE_TEMPLATE/bug_report.md': b'Fixture issue template\n',
            '.github/ISSUE_TEMPLATE/config.yml': b'blank_issues_enabled: false\n'}))
        (root / '.gitattributes').write_bytes(b'* text eol=lf\n')
        name = 'tools/release/builder/undertale-3ds-builder.cmd'
        for endings in (b'\n', b'\r\n'):
            export = dict(files, **{name: files[name].replace(b'\n', endings)})
            with self.subTest(endings=endings):
                result = release_contract.record_public_source(export, root, 'undertale',
                                                                '1.0.0-beta.1', 'd' * 64)
                self.assertEqual(result['public_commit'], commit)
                # A single content byte must still fail, including in CRLF mode.
                export[name] = export[name].replace(b'off', b'ofF')
                with self.assertRaisesRegex(ValueError, 'changed=' + name):
                    release_contract.record_public_source(export, root, 'undertale',
                                                          '1.0.0-beta.1', 'd' * 64)

    def test_line_endings_are_exact_without_committed_crlf_attribute(self):
        files = {'.gitattributes': b'* text eol=lf\n', 'example.cmd': b'fixture\n'}
        root, _ = self.public_export(files)
        # Working-tree attribute edits must not relax the committed policy.
        (root / '.gitattributes').write_bytes(b'*.cmd text eol=crlf\n')
        with self.assertRaisesRegex(ValueError, 'changed=example.cmd'):
            release_contract.record_public_source(dict(files, **{'example.cmd': b'fixture\r\n'}),
                                                  root, 'undertale', '1.0.0', 'd' * 64)

    def test_only_github_directory_is_excluded(self):
        for extra in ('unexpected.txt', '.github-extra/file', 'src/.github/file'):
            files = {'example': b'fixture\n'}
            root, _ = self.public_export(dict(files, **{extra: b'extra\n'}))
            with self.subTest(extra=extra), self.assertRaisesRegex(ValueError, 'extra='):
                release_contract.record_public_source(files, root, 'undertale', '1.0.0', 'd' * 64)

    def test_export_records_public_tree_commit_not_private_head(self):
        files = {'VERSION': b'1.0.0-beta.1\n', 'src/example.py': b'print("fixture")\n'}
        root, commit = self.public_export(files)
        for game in ('undertale', 'deltarune'):
            descriptor = release_contract.record_public_source(files, root, game, '1.0.0-beta.1', 'd' * 64)
            self.assertEqual(descriptor['public_commit'], commit)
            self.assertTrue(release_contract.validate_source(descriptor, game, '1.0.0-beta.1', 'd' * 64)
                            ['source_url'].endswith('/tree/' + commit))
        # Working-tree edits cannot substitute for a committed source revision.
        (root / 'VERSION').write_bytes(b'changed\n')
        self.assertEqual(release_contract.record_public_source(files, root, 'undertale', '1.0.0-beta.1',
                                                              'd' * 64)['public_commit'], commit)

    def test_public_commit_rejects_wrong_bytes_missing_extra_and_parent_repo(self):
        files = {'src/example': b'fixture\n'}
        root, _ = self.public_export(files)
        for bad in ({'src/example': b'changed'}, {}, dict(files, extra=b'extra')):
            with self.subTest(files=bad), self.assertRaisesRegex(ValueError, 'differs from export'):
                release_contract.record_public_source(bad, root, 'undertale', '1.0.0', 'd' * 64)
        with self.assertRaisesRegex(ValueError, 'own export repository'):
            release_contract.record_public_source(files, root / 'src', 'undertale', '1.0.0', 'd' * 64)

    def test_git_attributes_cannot_change_the_compared_source(self):
        files = {'.gitattributes': b'example export-subst\n', 'example': b'$Format:%H$\n'}
        root, _ = self.public_export(files)
        release_contract.record_public_source(files, root, 'undertale', '1.0.0', 'd' * 64)
        with self.assertRaisesRegex(ValueError, 'changed=example'):
            release_contract.record_public_source(dict(files, example=b'substituted'), root,
                                                  'undertale', '1.0.0', 'd' * 64)

    def test_descriptor_cannot_be_in_its_own_source_commit(self):
        files = {release_contract.DESCRIPTOR: b'{}'}
        root, _ = self.public_export(files)
        with self.assertRaisesRegex(ValueError, 'outside the public source commit'):
            release_contract.record_public_source(files, root, 'undertale', '1.0.0', 'd' * 64)

    def test_both_repositories_and_beta_final_labels(self):
        for game in ('undertale', 'deltarune'):
            for version in ('1.0.0-beta.1', '1.0.0'):
                d = release_contract.source_descriptor(game, version, 'd' * 64)
                with self.assertRaisesRegex(ValueError, 'no approved public source'):
                    release_contract.validate_source(d, game, version, 'd' * 64)
                d['public_commit'] = 'a' * 40
                resolved = release_contract.validate_source(d, game, version, 'd' * 64)
                self.assertEqual(resolved['source_url'],
                    'https://github.com/ethancawse/' + game + '-3ds/tree/' + 'a' * 40)
                self.assertEqual(resolved['tag'], 'v' + version)
                for key, bad in (('game', 'other'), ('version', '0.1.0'), ('source_sha256', 'e' * 64),
                                 ('repository', 'https://example.invalid/'), ('tag', 'vwrong'),
                                 ('schema', 2), ('public_commit', 'v1.0.0'), ('public_commit', '0' * 40)):
                    with self.subTest(key=key, bad=bad), self.assertRaises(ValueError):
                        release_contract.validate_source(dict(d, **{key: bad}), game, version, 'd' * 64)

    def test_digest_covers_paths_sizes_and_bytes(self):
        digest = release_contract.payload_digest
        self.assertEqual(digest({'b': b'2', 'a': b'1'}), digest({'a': b'1', 'b': b'2'}))
        for other in ({'a': b'1'}, {'a': b'1', 'c': b'2'}, {'a': b'1', 'b': b'3'}):
            self.assertNotEqual(digest({'a': b'1', 'b': b'2'}), digest(other))


@unittest.skipUnless(shutil.which('cmake'), 'CMake required')
class CompilerPolicyTests(unittest.TestCase):
    def test_reconfigure_never_caches_pin_eligibility(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'scratch') as directory:
            root = Path(directory)
            compiler = root / ('compiler.cmd' if os.name == 'nt' else 'compiler')
            compiler.write_text('@echo arm-none-eabi\n' if os.name == 'nt' else '#!/bin/sh\necho arm-none-eabi\n')
            compiler.chmod(0o755)
            script = root / 'policy.cmake'
            script.write_text(
                f'include("{(ROOT / "platform/3ds/compiler_policy.cmake").as_posix()}")\n'
                f'set(CMAKE_C_COMPILER "{compiler.as_posix()}")\n'
                'set(CMAKE_C_COMPILER_ID GNU)\n'
                'foreach(version 16.1.0 17.1.0 16.1.0)\n'
                '  set(CMAKE_C_COMPILER_VERSION "${version}")\n'
                f'  release_compiler_policy("{(ROOT / "docs/release/build-contract.json").as_posix()}")\n'
                '  message("pins:${version}:${CTRPORT_LAYOUT_PINS_ENABLED}")\n'
                'endforeach()\n')
            result = subprocess.run(['cmake', '-P', str(script)], stdin=subprocess.DEVNULL,
                                    capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr.count('pins:16.1.0:ON'), 2)
            self.assertIn('pins:17.1.0:OFF', result.stderr)
            self.assertIn('performance may differ slightly.', result.stdout)
            self.assertNotIn('CMake Warning', result.stderr)


@unittest.skipUnless(shutil.which('cmake'), 'CMake required')
class ReleaseContractTests(unittest.TestCase):
    def query_version(self, version, allocations=None):
        with tempfile.TemporaryDirectory(dir=ROOT / 'scratch') as directory:
            root = Path(directory)
            (root / 'docs/release').mkdir(parents=True)
            contract = json.loads((ROOT / 'docs/release/build-contract.json').read_text())
            if allocations is not None:
                contract['cia_prerelease_versions'] = allocations
            (root / 'docs/release/build-contract.json').write_text(json.dumps(contract))
            (root / 'VERSION').write_text(version + '\n')
            script = root / 'version.cmake'
            script.write_text(
                f'include("{(ROOT / "platform/3ds/release_contract.cmake").as_posix()}")\n'
                f'release_read_contract("{root.as_posix()}")\n'
                'message("${RELEASE_VERSION}|${RELEASE_NUMERIC_VERSION}|${RELEASE_CIA_MAJOR}.'
                '${RELEASE_CIA_MINOR}.${RELEASE_CIA_PATCH}|${RELEASE_CIA_INTEGER}")\n')
            return subprocess.run(['cmake', '-P', str(script)], stdin=subprocess.DEVNULL,
                                  capture_output=True, text=True, timeout=20)

    def test_beta_and_final_install_order(self):
        beta = self.query_version('1.0.0-beta.1')
        final = self.query_version('1.0.0')
        self.assertEqual(beta.returncode, 0, beta.stderr)
        self.assertEqual(final.returncode, 0, final.stderr)
        self.assertIn('1.0.0-beta.1|1.0.0|0.63.1|1009', beta.stderr)
        self.assertIn('1.0.0|1.0.0|1.0.0|1024', final.stderr)

    def test_unallocated_beta_fails(self):
        result = self.query_version('1.0.0-beta.2')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('No approved CIA allocation', result.stderr)

    def test_beta_cannot_equal_or_exceed_final(self):
        for cia in ('1.0.0', '1.0.1'):
            result = self.query_version('1.0.0-beta.1', {'1.0.0-beta.1': cia})
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('must precede', result.stderr)

    def test_allocations_must_rise_with_the_version(self):
        # A later prerelease at or below an earlier one's allocation would not install over it.
        good = {'1.0.0-beta.1': '0.63.1', '1.0.0-beta.2': '0.63.2', '1.0.0-rc.1': '0.63.8', '1.1.0-alpha.1': '1.0.9'}
        for allocations, message in ((good, None),
                                     ({'1.0.0-beta.1': '0.63.1', '1.0.0-beta.2': '0.63.1'}, 'must rise'),
                                     ({'1.0.0-beta.1': '0.63.2', '1.0.0-beta.10': '0.63.3', '1.0.0-beta.2': '0.63.1'},
                                      'must rise'),
                                     ({'1.0.0-beta.1': '0.63.1', '1.0.0-rc.1': '0.63.0'}, 'must rise'),
                                     ({'1.0.0-beta.1': '0.63.1', '1.0.1-beta.1': '1.0.1'}, 'must precede'),
                                     ({'1.0.0-beta.1': '0.63.1', '1.0.0': '0.63.2'}, 'not a prerelease')):
            with self.subTest(allocations=allocations):
                result = self.query_version('1.0.0-beta.1', allocations)
                with tempfile.TemporaryDirectory(dir=ROOT / 'scratch') as directory:
                    root = Path(directory)
                    (root / 'docs/release').mkdir(parents=True)
                    contract = json.loads((ROOT / 'docs/release/build-contract.json').read_text())
                    contract['cia_prerelease_versions'] = allocations
                    (root / 'docs/release/build-contract.json').write_text(json.dumps(contract))
                    (root / 'VERSION').write_text('1.0.0-beta.1\n')
                    if message is None:
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertIn('1.0.0-beta.1|1.0.0|0.63.1|1009', result.stderr)
                        self.assertEqual(release_contract.read(root)['cia_integer'], 1009)
                    else:
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn(message, result.stderr)
                        with self.assertRaisesRegex(ValueError, message):
                            release_contract.read(root)

    def test_cia_allocation_overflow_fails(self):
        result = self.query_version('1.0.0-beta.1', {'1.0.0-beta.1': '0.63.16'})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('CIA limits', result.stderr)

    def test_changed_template_drives_all_models_and_wrong_model_filename(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'scratch') as directory:
            root = Path(directory)
            (root / 'docs/release').mkdir(parents=True)
            contract = json.loads((ROOT / 'docs/release/build-contract.json').read_text())
            contract['package_stem'] = '{game}-port-{model}'
            (root / 'docs/release/build-contract.json').write_text(json.dumps(contract))
            (root / 'VERSION').write_text('0.1.0\n')
            python_contract = release_contract.read(root)
            self.assertEqual(python_contract['package_stem'], '{game}-port-{model}')
            script = root / 'names.cmake'
            script.write_text(
                f'include("{(ROOT / "platform/3ds/release_contract.cmake").as_posix()}")\n'
                f'release_read_contract("{root.as_posix()}")\n'
                'foreach(game undertale deltarune)\n'
                '  foreach(model n3ds o3ds)\n'
                '    release_package_names("${game}" "${model}")\n'
                '    message("${RELEASE_PACKAGE_STEM}|${RELEASE_O3DS_FILENAME}")\n'
                '  endforeach()\n'
                'endforeach()\n')
            result = subprocess.run(['cmake', '-P', str(script)], stdin=subprocess.DEVNULL,
                                    capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            for game in ('undertale', 'deltarune'):
                for model in ('n3ds', 'o3ds'):
                    stem = contract['package_stem'].format(game=game, model=model)
                    self.assertIn(stem + '|%s-port-o3ds.3dsx', result.stderr)
