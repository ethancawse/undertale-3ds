"""SMDH identities are packaging metadata; incremental builds must refresh them."""
from pathlib import Path
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'platform/3ds/smdh.cmake'


@unittest.skipUnless(shutil.which('cmake') and shutil.which('git'), 'CMake and Git fixtures')
class SmdhMetadata(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'source with spaces'
        self.root.mkdir()
        (self.root / 'docs/release').mkdir(parents=True)
        shutil.copyfile(ROOT / 'docs/release/build-contract.json', self.root / 'docs/release/build-contract.json')
        shutil.copyfile(ROOT / 'VERSION', self.root / 'VERSION')
        self.env = dict(os.environ, GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull,
                        GIT_TERMINAL_PROMPT='0')
        for key in ('GIT_DIR', 'GIT_WORK_TREE', 'GIT_COMMON_DIR', 'GIT_INDEX_FILE', 'GIT_OBJECT_DIRECTORY',
                    'GIT_ALTERNATE_OBJECT_DIRECTORIES', 'GIT_NAMESPACE', 'CTRPORT_SMDH_BUILD_TAG'):
            self.env.pop(key, None)

    def command(self, args, cwd=None, check=True):
        result = subprocess.run(list(map(str, args)), cwd=cwd or self.root, env=self.env,
                                stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30)
        if check:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def git(self, *args):
        return self.command(['git', *args]).stdout.strip()

    def commit(self):
        if not (self.root / '.git').exists():
            self.git('init', '-q')
        self.git('-c', 'core.autocrlf=false', 'add', '.')
        self.git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                 '-c', 'commit.gpgsign=false', 'commit', '-qm', 'Invented metadata fixture')
        return self.git('rev-parse', '--short=7', 'HEAD')

    def query(self, game='undertale', model='o3ds', root=None, check=True):
        output = Path(self.temp.name) / 'description.txt'
        result = self.command(['cmake', '-DROOT=' + str(root or self.root), '-DGAME=' + game,
                               '-DMODEL=' + model, '-DGIT_EXECUTABLE=' + shutil.which('git'),
                               '-DDESCRIPTION_OUT=' + str(output), '-P', SCRIPT], check=check)
        return output.read_text() if result.returncode == 0 else result

    def test_all_player_models_use_contract_version_and_no_parent_identity(self):
        self.commit()
        archive = self.root / 'nested archive'
        shutil.copytree(self.root / 'docs', archive / 'docs')
        shutil.copyfile(self.root / 'VERSION', archive / 'VERSION')
        self.env['CTRPORT_SMDH_BUILD_TAG'] = 'stage73'
        version = (archive / 'VERSION').read_text().strip()
        for game in ('undertale', 'deltarune'):
            for model, label in (('o3ds', 'Old 3DS'), ('n3ds', 'New 3DS')):
                with self.subTest(game=game, model=model):
                    self.assertEqual(self.query(game, model, archive),
                                     game.upper() + ' for 3DS (port) - ' + version + ' - ' + label)

    def test_lab_label_and_commit_follow_incremental_changes(self):
        first = self.commit()
        self.env['CTRPORT_SMDH_BUILD_TAG'] = 'stage73'
        self.assertTrue(self.query().endswith(' - stage73 ' + first))
        (self.root / 'VERSION').write_text('1.0.0\n')
        self.assertIn('1.0.0 - Old 3DS - stage73 ' + first + '-dirty', self.query())
        second = self.commit()
        self.assertNotEqual(first, second)
        self.assertTrue(self.query().endswith(' - stage73 ' + second))
        self.env['CTRPORT_SMDH_BUILD_TAG'] = 'cand27'
        self.assertTrue(self.query().endswith(' - cand27 ' + second))

    def foreign_repository(self):
        foreign = Path(self.temp.name) / 'foreign repository'
        foreign.mkdir()
        (foreign / 'invented.txt').write_text('Foreign fixture; no game files.\n')
        self.command(['git', 'init', '-q'], cwd=foreign)
        self.command(['git', 'add', '.'], cwd=foreign)
        self.command(['git', '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                      '-c', 'commit.gpgsign=false', 'commit', '-qm', 'Foreign fixture'], cwd=foreign)
        return foreign

    def test_local_git_directory_or_file_ignores_foreign_repository_overrides(self):
        commit = self.commit()
        foreign = self.foreign_repository()
        self.env.update(GIT_DIR=str(foreign / '.git'), GIT_WORK_TREE=str(foreign),
                        GIT_COMMON_DIR=str(foreign / '.git'),
                        GIT_INDEX_FILE=str(foreign / '.git/index'),
                        GIT_OBJECT_DIRECTORY=str(foreign / '.git/objects'),
                        GIT_ALTERNATE_OBJECT_DIRECTORIES=str(foreign / '.git/objects'),
                        CTRPORT_SMDH_BUILD_TAG='stage73')
        for marker in ('directory', 'file'):
            if marker == 'file':
                metadata = Path(self.temp.name) / 'local metadata'
                shutil.move(self.root / '.git', metadata)
                (self.root / '.git').write_text('gitdir: ' + metadata.as_posix() + '\n')
            with self.subTest(marker=marker):
                self.assertTrue(self.query().endswith(' - stage73 ' + commit))
                self.assertEqual(self.env['GIT_DIR'], str(foreign / '.git'))

    def test_exported_builder_with_foreign_git_dir_keeps_player_description(self):
        self.commit()
        foreign = self.foreign_repository()
        sys.path[:0] = [str(ROOT / 'tools/release'), str(ROOT / 'tools/release/builder')]
        import package_builder
        archive = self.root / 'nested exported builder'
        for name, data in package_builder.inputs(ROOT, 'undertale').items():
            path = archive / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        self.assertFalse((archive / '.git').exists())
        # Run that copy's actual Build.prepare/mirror and production SMDH helper
        # in a fresh interpreter. Only the public descriptor is an invented fixture.
        child = r'''
from pathlib import Path
from types import SimpleNamespace
import json,subprocess,sys
archive,work=map(Path,sys.argv[1:3])
sys.path.insert(0,str(archive/'tools/release/builder'))
import pipeline,products
assert Path(pipeline.__file__).resolve().is_relative_to(archive.resolve())
contract=products.release_contract
descriptor=contract.source_descriptor('undertale',pipeline.release_version(archive),
    contract.payload_digest(pipeline.source_payload(archive,'undertale')))
descriptor['public_commit']='a'*40
(archive/contract.DESCRIPTOR).write_text(json.dumps(descriptor))
job=pipeline.Build(products.PRODUCTS['undertale'],SimpleNamespace(),str(work),'o3ds',str(work/'unused-game'),
                   source_root=str(archive))
job.prepare()
source=work/'src'
assert not (source/'.git').exists()
for game in ('undertale','deltarune'):
    for model in ('o3ds','n3ds'):
        output=work/(game+'-'+model+'.txt')
        subprocess.run([sys.argv[3],'-DROOT='+str(source),'-DGAME='+game,'-DMODEL='+model,
            '-DGIT_EXECUTABLE='+sys.argv[4],'-DDESCRIPTION_OUT='+str(output),'-P',
            str(source/'platform/3ds/smdh.cmake')],check=True,stdin=subprocess.DEVNULL,timeout=20)
        print(output.read_text())
'''
        self.env.update(GIT_DIR=str(foreign / '.git'), CTRPORT_SMDH_BUILD_TAG='cand28')
        version = (archive / 'VERSION').read_text().strip()
        for work_tree in (None, foreign):
            if work_tree is None:
                self.env.pop('GIT_WORK_TREE', None)
            else:
                self.env['GIT_WORK_TREE'] = str(work_tree)
            with self.subTest(GIT_WORK_TREE=work_tree):
                work = Path(self.temp.name) / ('work-' + str(work_tree is not None))
                result = self.command([sys.executable, '-I', '-B', '-c', child, archive, work,
                                       shutil.which('cmake'), shutil.which('git')])
                self.assertEqual(result.stdout.splitlines(), [
                    game.upper() + ' for 3DS (port) - ' + version + ' - ' + label
                    for game in ('undertale', 'deltarune')
                    for label in ('Old 3DS', 'New 3DS')])

    def test_foreign_build_id_stops_before_artifact_collection(self):
        foreign = self.foreign_repository()
        sys.path.insert(0, str(ROOT / 'tools/release'))
        import test_builder as fixtures
        fixture = fixtures.BuildTest(methodName='runTest')
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        self.addCleanup(fixture.tearDown)
        build, _ = fixture.make(['[100%] fixture compile complete\n'])
        original_popen = build.popen
        source = Path(build.work) / 'src'
        header = source / 'build/3ds-undertale-n3ds/generated/ctrport_build_id.h'
        aot = Path(self.temp.name) / 'invented aot';aot.mkdir()
        (aot / 'aot_table.c').write_text('const char aotBuildTag[] = "aot12";\n')
        def contaminated_compile(argv, **kw):
            process = original_popen(argv, **kw)
            # Exercise the real existing build-ID helper with inherited foreign
            # selection, then let Build.run's existing gate inspect that header.
            result = subprocess.run(['cmake', '-DSOURCE_DIR=' + str(source / 'platform/3ds'),
                '-DOUT=' + str(header), '-DGIT_EXECUTABLE=' + shutil.which('git'),
                '-DAOT_DIR=' + str(aot), '-P', ROOT / 'platform/3ds/build_id.cmake'],
                cwd=source, env=kw['env'], stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            return process
        build.popen = contaminated_compile
        with mock.patch.dict(os.environ, {'GIT_DIR': str(foreign / '.git'), 'GIT_WORK_TREE': str(source)}), \
                mock.patch.object(build, 'collect') as collect, \
                self.assertRaisesRegex(fixtures.pipeline.BuildFailed, 'not archive-0.1.0'):
            build.run()
        collect.assert_not_called()
        self.assertNotIn('archive-', header.read_text())
        self.assertFalse((Path(build.work) / 'out/n3ds').exists())
        self.assertFalse((Path(build.work) / 'out/build-undertale-n3ds.json').exists())

    def test_exact_tag_auto_detection_and_lab_fallback(self):
        commit = self.commit()
        self.assertTrue(self.query().endswith(' - lab ' + commit))
        self.git('tag', 'stage73')
        self.assertTrue(self.query().endswith(' - stage73 ' + commit))
        self.env['CTRPORT_SMDH_BUILD_TAG'] = 'cand27'
        self.assertTrue(self.query().endswith(' - cand27 ' + commit))

    def test_rejects_overflow_and_injected_labels(self):
        self.commit()
        for label in ('other', 'stage73;bad', 'stage73\n', 'stage' + '1' * 110):
            with self.subTest(label=label):
                self.env['CTRPORT_SMDH_BUILD_TAG'] = label
                self.assertNotEqual(self.query(check=False).returncode, 0)

    def test_build_rules_keep_titles_icons_author_and_refresh_both_formats(self):
        text = (ROOT / 'platform/3ds/CMakeLists.txt').read_text()
        self.assertIn('set(APP_NAME "undertale-3ds")', text)
        self.assertIn('set(APP_NAME "deltarune-3ds (unofficial)")', text)
        self.assertIn('set(APP_AUTHOR "Community contributors")', text)
        self.assertIn('assets/icon-${GAME_DIR_NAME}-${CTRPORT_TARGET}.png', text)
        self.assertNotIn('GameMaker runner (Butterscotch)', text)
        self.assertIn('add_dependencies(ctrport_3dsx legal_texts ctrport_smdh)', text)
        self.assertIn('add_dependencies(cia legal_texts ctrport_cia_icon)', text)

    @unittest.skipUnless(shutil.which('smdhtool'), 'Native SMDH packaging tool')
    def test_real_smdh_preserves_icon_and_refreshes_without_reconfigure(self):
        commit = self.commit()
        self.env['CTRPORT_SMDH_BUILD_TAG'] = 'stage73'
        out = Path(self.temp.name) / 'metadata.smdh'
        icon = ROOT / 'platform/3ds/assets/icon-undertale-o3ds.png'
        args = ['cmake', '-DROOT=' + str(self.root), '-DGAME=undertale', '-DMODEL=o3ds',
                '-DGIT_EXECUTABLE=' + shutil.which('git'), '-DMODE=3dsx',
                '-DTOOL=' + shutil.which('smdhtool'), '-DNAME=undertale-3ds',
                '-DAUTHOR=Community contributors', '-DICON=' + str(icon), '-DOUT=' + str(out), '-P', SCRIPT]
        self.command(args)
        raw = out.read_bytes()
        self.assertEqual(raw[:4], b'SMDH')
        def title(data, offset, length):
            return data[offset:offset+length].decode('utf-16le').split('\0')[0]
        self.assertEqual(title(raw, 8, 128), 'undertale-3ds')
        self.assertTrue(title(raw, 136, 256).endswith('stage73 ' + commit))
        self.assertEqual(title(raw, 392, 128), 'Community contributors')
        before = out.stat().st_mtime_ns
        self.command(args)
        self.assertEqual(out.stat().st_mtime_ns, before)
        self.env['CTRPORT_SMDH_BUILD_TAG'] = 'cand27'
        self.command(args)
        changed = out.read_bytes()
        self.assertTrue(title(changed, 136, 256).endswith('cand27 ' + commit))
        self.assertEqual(raw[0x2008:], changed[0x2008:])
        # Corrupt output must be repaired even if inputs still match the stamp.
        out.write_bytes(b'broken')
        self.command(args)
        self.assertEqual(out.read_bytes(), changed)


if __name__ == '__main__':
    unittest.main()
