"""SMDH identities are packaging metadata; incremental builds must refresh them.

HOME Menu text (maintainer, 2026-11-04): the long description is the game's name, and the publisher line carries the rest:
"3lit3hax - Unofficial port" for UNDERTALE releases; DELTARUNE releases keep
"Unofficial port - v<version> - <model>". "Unofficial port - <label> <commit> - <model>"
from a Git checkout (label: a stage/candidate tag, else "source build"). Release text only when release packaging asks
for it (CTRPORT_SMDH_RELEASE=1, a kit's builder); an archive built by hand says "source build".
Homebrew Launcher text (the 3dsx only, maintainer, 2026-11-06): the description is "<build> (o3ds/o2ds)" or "<build> (n3ds/n2ds)",
where <build> is the same "v<version>" or "<label> <commit>" the publisher line uses.
"""
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
# Hang guards, not measures: nothing here times these calls, so a loaded host only makes them slower (toolfix 11-11:
# on the shared PC 30 s limits on git show and CMake steps expired during process-start stalls). META_S is for small
# metadata calls, 100+ times their normal time (at normal load: git show of one file max 0.05 s; test_release_ndebug's
# tiny CMake configure max 0.81 s, its build max 0.12 s).
# (here: git, and CMake -P runs of smdh.cmake and build_id.cmake).
META_S = 120
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
                    'GIT_ALTERNATE_OBJECT_DIRECTORIES', 'GIT_NAMESPACE', 'CTRPORT_SMDH_BUILD_TAG',
                    'CTRPORT_SMDH_SOURCE', 'CTRPORT_SMDH_RELEASE'):
            self.env.pop(key, None)

    def command(self, args, cwd=None, check=True):
        result = subprocess.run(list(map(str, args)), cwd=cwd or self.root, env=self.env,
                                stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=META_S)
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

    GAMES_AND_CONSOLES = tuple((game, model, consoles) for game in ('undertale', 'deltarune')
                               for model, consoles in (('o3ds', '(o3ds/o2ds)'), ('n3ds', '(n3ds/n2ds)')))

    def assert_launcher_build(self, build, root=None):
        """The 3dsx's description is "<build> <consoles>"; the CIA's stays the game's name."""
        for game, model, consoles in self.GAMES_AND_CONSOLES:
            with self.subTest(game=game, model=model, build=build):
                self.assertEqual(self.query(game, model, root, mode='3dsx')[0], build + ' ' + consoles)
                self.assertEqual(self.query(game, model, root, mode='cia')[0], game.upper())

    def test_an_archive_built_by_hand_is_a_source_build(self):
        # c26rev3 F3: a changed zip must not read as a release in a bug report. Only the exact opt-in counts.
        version = (self.root / 'VERSION').read_text().strip()
        for value in (None, '', '0', 'yes', 'ON', '11'):
            if value is None:
                self.env.pop('CTRPORT_SMDH_RELEASE', None)
            else:
                self.env['CTRPORT_SMDH_RELEASE'] = value
            self.assert_launcher_build('source build')
            for game, model, _ in self.GAMES_AND_CONSOLES:
                for mode in ('3dsx', 'cia'):
                    with self.subTest(value=value, game=game, model=model, mode=mode):
                        description, publisher = self.query(game, model, mode=mode)
                        label = 'Old 3DS' if model == 'o3ds' else 'New 3DS'
                        self.assertEqual(publisher, 'Unofficial port - source build - ' + label)
                        self.assertNotIn(version, description + publisher)

    def test_the_release_opt_in_never_outranks_a_source_identity(self):
        version = (self.root / 'VERSION').read_text().strip()
        self.env['CTRPORT_SMDH_RELEASE'] = '1'
        self.env['CTRPORT_SMDH_SOURCE'] = 'abcdef012345-dirty'
        self.assert_launcher_build('source build abcdef012345-dirty')
        del self.env['CTRPORT_SMDH_SOURCE']
        commit = self.commit()
        self.assert_launcher_build('source build ' + commit)
        self.assertEqual(self.query()[1], 'Unofficial port - source build ' + commit + ' - Old 3DS')
        (self.root / '.git').rename(Path(self.temp.name) / 'moved git')
        (self.root / '.git').write_text('gitdir: gone\n')
        self.assert_launcher_build('source build')
        self.assertNotIn(version, self.query()[1])

    def test_the_builder_asks_for_what_this_script_reads(self):
        # The two ends of the opt-in, so renaming one can't quietly turn every player's row into "source build".
        self.assertIn('env["CTRPORT_SMDH_RELEASE"] = "1"', (ROOT / 'tools/release/builder/pipeline.py').read_text(encoding='utf-8'))
        self.assertIn('"$ENV{CTRPORT_SMDH_RELEASE}" STREQUAL "1"', SCRIPT.read_text(encoding='utf-8'))

    def test_3dsx_description_shows_the_version_in_a_release_tree(self):
        self.env['CTRPORT_SMDH_RELEASE'] = '1'
        version = (self.root / 'VERSION').read_text().strip()
        self.assert_launcher_build('v' + version)
        self.assertEqual(self.query('undertale', 'o3ds', mode='3dsx')[0], 'v' + version + ' (o3ds/o2ds)')
        self.assertEqual(self.query('undertale', 'n3ds', mode='3dsx')[0], 'v' + version + ' (n3ds/n2ds)')
        # The publisher is the line maintainer sees bottom right, and the version moving up doesn't change it.
        for game, model, _ in self.GAMES_AND_CONSOLES:
            for mode in ('3dsx', 'cia'):
                with self.subTest(game=game, model=model, mode=mode):
                    self.assertEqual(self.query(game, model, mode=mode)[1], self.query(game, model)[1])

    def test_3dsx_description_never_shows_the_version_in_a_source_build(self):
        """R204, as for the publisher: only a release tree may claim the release version."""
        version = (self.root / 'VERSION').read_text().strip()
        self.env['CTRPORT_SMDH_SOURCE'] = 'abcdef012345-dirty'
        self.assert_launcher_build('source build abcdef012345-dirty')
        del self.env['CTRPORT_SMDH_SOURCE']
        commit = self.commit()
        self.assert_launcher_build('source build ' + commit)
        self.env['CTRPORT_SMDH_BUILD_TAG'] = 'stage73'
        self.assert_launcher_build('stage73 ' + commit)
        del self.env['CTRPORT_SMDH_BUILD_TAG']
        (self.root / 'VERSION').write_text(version + '\n\n')
        self.assert_launcher_build('source build ' + commit + '-dirty')
        (self.root / 'VERSION').write_text(version + '\n')
        for game, model, _ in self.GAMES_AND_CONSOLES:
            with self.subTest(game=game, model=model):
                self.assertNotIn(version, self.query(game, model, mode='3dsx')[0])

    def test_a_checkout_reached_through_a_linked_folder_names_its_commit(self):
        # Git reports the top level with links resolved (macOS's /var, a linked home or projects folder).
        commit = self.commit()
        link = Path(self.temp.name) / 'linked source'
        try:
            link.symlink_to(self.root, target_is_directory=True)
        except OSError:
            self.skipTest('Host does not allow directory symlinks')
        self.assert_launcher_build('source build ' + commit, root=link)

    def test_3dsx_description_in_a_checkout_git_cannot_read(self):
        (self.root / '.git').write_text('gitdir: gone\n')
        self.assert_launcher_build('source build')

    def test_longest_build_the_publisher_allows_still_fits_the_description(self):
        """The publisher's 63 characters fail a long build first, so the description's 127 can't be reached."""
        label = 'stage' + '7' * 22
        self.commit()
        self.env['CTRPORT_SMDH_BUILD_TAG'] = label
        description, publisher = self.query('undertale', 'n3ds', mode='3dsx')
        self.assertEqual(len(publisher), 63)
        self.assertLessEqual(len(description), 127)
        self.env['CTRPORT_SMDH_BUILD_TAG'] = label + '7'
        self.assertIn('publisher exceeds', self.query('undertale', 'n3ds', check=False, mode='3dsx').stderr)

    def query(self, game='undertale', model='o3ds', root=None, check=True, mode=None):
        """(description, publisher), or the failed process."""
        output = Path(self.temp.name) / 'description.txt'
        publisher = Path(self.temp.name) / 'publisher.txt'
        result = self.command(['cmake', '-DROOT=' + str(root or self.root), '-DGAME=' + game,
                               '-DMODEL=' + model, '-DGIT_EXECUTABLE=' + shutil.which('git'),
                               '-DDESCRIPTION_OUT=' + str(output), '-DPUBLISHER_OUT=' + str(publisher),
                               *(['-DMODE=' + mode] if mode else []), '-P', SCRIPT], check=check)
        return (output.read_text(), publisher.read_text()) if result.returncode == 0 else result

    def test_all_player_models_use_contract_version_and_no_parent_identity(self):
        self.commit()
        archive = self.root / 'nested archive'
        shutil.copytree(self.root / 'docs', archive / 'docs')
        shutil.copyfile(self.root / 'VERSION', archive / 'VERSION')
        self.env['CTRPORT_SMDH_BUILD_TAG'] = 'stage73'
        self.env['CTRPORT_SMDH_RELEASE'] = '1'
        version = (archive / 'VERSION').read_text().strip()
        for game in ('undertale', 'deltarune'):
            for model, label in (('o3ds', 'Old 3DS'), ('n3ds', 'New 3DS')):
                with self.subTest(game=game, model=model):
                    self.assertEqual(self.query(game, model, archive),
                                     (game.upper(), '3lit3hax - Unofficial port' if game == 'undertale'
                                      else 'Unofficial port - v' + version + ' - ' + label))

    def test_builder_source_identity_names_the_commit_in_a_tree_without_git(self):
        # R204 F1: the builder mirrors a checkout without .git and passes the commit it read as CTRPORT_SMDH_SOURCE.
        for value, model, label in (('abc1234', 'o3ds', 'Old 3DS'), ('abc1234-dirty', 'n3ds', 'New 3DS'),
                                    ('0123456789ab-dirty', 'n3ds', 'New 3DS')):
            with self.subTest(value=value):
                self.env['CTRPORT_SMDH_SOURCE'] = value
                self.assertEqual(self.query('undertale', model),
                                 ('UNDERTALE', 'Unofficial port - source build ' + value + ' - ' + label))
        for value in ('release', 'abc123', 'ABC1234', 'abc1234;x', 'abc1234-dirty-dirty', 'abc1234 ', 'a' * 13, 'a' * 40,
                      'abc1234\n'):
            with self.subTest(value=value):
                self.env['CTRPORT_SMDH_SOURCE'] = value
                failed = self.query(check=False)
                self.assertNotEqual(failed.returncode, 0)
                self.assertIn('SMDH source must be a commit', failed.stdout + failed.stderr)
        # In a Git checkout git itself is the truth; a stale value from the environment is not used.
        commit = self.commit()
        self.env['CTRPORT_SMDH_SOURCE'] = 'abc1234'
        self.assertEqual(self.query()[1], 'Unofficial port - source build ' + commit + ' - Old 3DS')

    def test_a_checkout_git_cannot_read_is_a_source_build_never_the_release(self):
        # R204 F1: Git missing, or a .git marker git refuses, used to fall back to the release version.
        self.commit()
        output = Path(self.temp.name) / 'd.txt'
        publisher = Path(self.temp.name) / 'p.txt'
        for git in ('', str(Path(self.temp.name) / 'no such git.exe')):
            with self.subTest(git=git):
                self.command(['cmake', '-DROOT=' + str(self.root), '-DGAME=undertale', '-DMODEL=n3ds',
                              '-DGIT_EXECUTABLE=' + git, '-DDESCRIPTION_OUT=' + str(output),
                              '-DPUBLISHER_OUT=' + str(publisher), '-P', SCRIPT])
                self.assertEqual(publisher.read_text(), 'Unofficial port - source build - New 3DS')
        (self.root / '.git').rename(Path(self.temp.name) / 'moved git')
        (self.root / '.git').write_text('gitdir: ' + (Path(self.temp.name) / 'missing').as_posix() + '\n')
        self.assertEqual(self.query(), ('UNDERTALE', 'Unofficial port - source build - Old 3DS'))

    def test_a_dangling_git_link_is_a_source_build_never_the_release(self):
        # A .git junction or symlink whose target is gone is still a checkout marker, though EXISTS says false: it
        # must never fall back to the release version, nor take a builder identity meant for a tree without .git.
        target = Path(self.temp.name) / 'gone git'
        target.mkdir()
        link = self.root / '.git'
        try:
            if sys.platform == 'win32':
                import _winapi
                _winapi.CreateJunction(str(target), str(link))
            else:
                os.symlink(target, link, target_is_directory=True)
        except OSError as error:
            self.skipTest('cannot make a directory link here: %s' % error)
        self.addCleanup(lambda: os.path.lexists(link) and (os.rmdir if sys.platform == 'win32' else os.unlink)(link))
        target.rmdir()
        self.assertFalse(link.exists())
        self.assertTrue(os.path.lexists(link))
        self.assertEqual(self.query(), ('UNDERTALE', 'Unofficial port - source build - Old 3DS'))
        self.env['CTRPORT_SMDH_SOURCE'] = 'abc1234'
        self.assertEqual(self.query(model='n3ds'), ('UNDERTALE', 'Unofficial port - source build - New 3DS'))

    def test_lab_label_and_commit_follow_incremental_changes(self):
        first = self.commit()
        self.env['CTRPORT_SMDH_BUILD_TAG'] = 'stage73'
        self.assertEqual(self.query(), ('UNDERTALE', 'Unofficial port - stage73 ' + first + ' - Old 3DS'))
        (self.root / 'VERSION').write_text('1.0.0\n')
        self.assertEqual(self.query()[1], 'Unofficial port - stage73 ' + first + '-dirty - Old 3DS')
        second = self.commit()
        self.assertNotEqual(first, second)
        self.assertEqual(self.query()[1], 'Unofficial port - stage73 ' + second + ' - Old 3DS')
        self.env['CTRPORT_SMDH_BUILD_TAG'] = 'cand27'
        self.assertEqual(self.query('undertale', 'n3ds'), ('UNDERTALE', 'Unofficial port - cand27 ' + second + ' - New 3DS'))

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
                self.assertEqual(self.query()[1], 'Unofficial port - stage73 ' + commit + ' - Old 3DS')
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
pipeline.library_pins.select = lambda *a, **kw: {'packages': [], 'shims': None}
assert Path(pipeline.__file__).resolve().is_relative_to(archive.resolve())
contract=products.release_contract
descriptor=contract.source_descriptor('undertale',pipeline.release_version(archive),
    contract.payload_digest(pipeline.source_payload(archive,'undertale')))
descriptor['public_commit']='a'*40
(archive/contract.DESCRIPTOR).write_text(json.dumps(descriptor))
job=pipeline.Build(products.PRODUCTS['undertale'],SimpleNamespace(root=None),str(work),'o3ds',str(work/'unused-game'),
                   source_root=str(archive))
job.prepare()
source=work/'src'
assert not (source/'.git').exists()
for game in ('undertale','deltarune'):
    for model in ('o3ds','n3ds'):
        output=work/(game+'-'+model+'.txt')
        publisher=work/(game+'-'+model+'-publisher.txt')
        subprocess.run([sys.argv[3],'-DROOT='+str(source),'-DGAME='+game,'-DMODEL='+model,
            '-DGIT_EXECUTABLE='+sys.argv[4],'-DDESCRIPTION_OUT='+str(output),'-DPUBLISHER_OUT='+str(publisher),'-P',
            str(source/'platform/3ds/smdh.cmake')],check=True,stdin=subprocess.DEVNULL,timeout=120,env=job.environment())
        print(output.read_text())
        print(publisher.read_text())
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
                    line
                    for game in ('undertale', 'deltarune')
                    for label in ('Old 3DS', 'New 3DS')
                    for line in (game.upper(), '3lit3hax - Unofficial port' if game == 'undertale'
                                 else 'Unofficial port - v' + version + ' - ' + label)])

    def test_exported_builder_from_a_source_checkout_names_its_commit(self):
        # R204 F1, end to end: the exported builder run from a Git checkout (no release descriptor) mirrors it
        # without .git; the environment Build.run gives build.sh makes the production SMDH helper name the commit.
        sys.path[:0] = [str(ROOT / 'tools/release'), str(ROOT / 'tools/release/builder')]
        import package_builder
        archive = Path(self.temp.name) / 'checkout of the public source'
        for name, data in package_builder.inputs(ROOT, 'undertale').items():
            path = archive / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        self.command(['git', 'init', '-q'], cwd=archive)
        self.command(['git', '-c', 'core.autocrlf=false', 'add', '.'], cwd=archive)
        self.command(['git', '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                      '-c', 'commit.gpgsign=false', 'commit', '-qm', 'Invented checkout'], cwd=archive)
        commit = self.command(['git', 'rev-parse', 'HEAD'], cwd=archive).stdout.strip()
        child = r'''
from pathlib import Path
from types import SimpleNamespace
import subprocess,sys
archive,work=map(Path,sys.argv[1:3])
sys.path.insert(0,str(archive/'tools/release/builder'))
import pipeline,products
pipeline.library_pins.select = lambda *a, **kw: {'packages': [], 'shims': None}
assert Path(pipeline.__file__).resolve().is_relative_to(archive.resolve())
assert not (archive/products.release_contract.DESCRIPTOR).exists()
job=pipeline.Build(products.PRODUCTS['undertale'],SimpleNamespace(root=None),str(work),'o3ds',str(work/'unused-game'),
                   source_root=str(archive))
job.prepare()
assert job.release_source['kind']=='source'
source=work/'src'
assert not (source/'.git').exists()
env=job.environment()
for model in ('o3ds','n3ds'):
    output=work/(model+'.txt'); publisher=work/(model+'-publisher.txt')
    subprocess.run([sys.argv[3],'-DROOT='+str(source),'-DGAME=undertale','-DMODEL='+model,
        '-DGIT_EXECUTABLE='+sys.argv[4],'-DDESCRIPTION_OUT='+str(output),'-DPUBLISHER_OUT='+str(publisher),'-P',
        str(source/'platform/3ds/smdh.cmake')],check=True,stdin=subprocess.DEVNULL,timeout=120,env=env)
    print(output.read_text())
    print(publisher.read_text())
'''
        for dirty in (False, True):
            if dirty:
                (archive / 'untracked fixture.txt').write_text('An added file is a local change.\n')
            with self.subTest(dirty=dirty):
                work = Path(self.temp.name) / ('source-work-' + str(dirty))
                result = self.command([sys.executable, '-I', '-B', '-c', child, archive, work,
                                       shutil.which('cmake'), shutil.which('git')])
                stamp = commit[:7] + ('-dirty' if dirty else '')
                self.assertEqual(result.stdout.splitlines(), [
                    'UNDERTALE', 'Unofficial port - source build ' + stamp + ' - Old 3DS',
                    'UNDERTALE', 'Unofficial port - source build ' + stamp + ' - New 3DS'])

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
                cwd=source, env=kw['env'], stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=META_S)
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
        self.assertEqual(self.query(), ('UNDERTALE', 'Unofficial port - source build ' + commit + ' - Old 3DS'))
        self.assertEqual(self.query('deltarune', 'n3ds'),
                         ('DELTARUNE', 'Unofficial port - source build ' + commit + ' - New 3DS'))
        self.git('tag', 'stage73')
        self.assertEqual(self.query()[1], 'Unofficial port - stage73 ' + commit + ' - Old 3DS')
        self.env['CTRPORT_SMDH_BUILD_TAG'] = 'cand27'
        self.assertEqual(self.query()[1], 'Unofficial port - cand27 ' + commit + ' - Old 3DS')

    def test_rejects_overflow_and_injected_labels(self):
        self.commit()
        for label in ('other', 'stage73;bad', 'stage73\n', 'stage' + '1' * 110):
            with self.subTest(label=label):
                self.env['CTRPORT_SMDH_BUILD_TAG'] = label
                self.assertNotEqual(self.query(check=False).returncode, 0)
        # The publisher holds 63 characters: "Unofficial port - " + label + " " + commit + " - New 3DS".
        longest = 'stage1' + 'a' * (63 - len('Unofficial port -  1234567 - New 3DS') - 6)
        self.env['CTRPORT_SMDH_BUILD_TAG'] = longest
        self.assertEqual(len(self.query('undertale', 'n3ds')[1]), 63)
        self.env['CTRPORT_SMDH_BUILD_TAG'] = longest + 'a'
        failed = self.query('undertale', 'n3ds', check=False)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn('publisher exceeds 63 characters', failed.stdout + failed.stderr)

    def test_build_rules_keep_titles_icons_author_and_refresh_both_formats(self):
        text = (ROOT / 'platform/3ds/CMakeLists.txt').read_text()
        self.assertIn('set(APP_NAME "UNDERTALE")', text)
        self.assertIn('set(APP_NAME "deltarune-3ds (unofficial)")', text)
        # The publisher line is smdh.cmake's own (release handle or source identity).
        self.assertNotIn('APP_AUTHOR', text)
        self.assertNotIn('Community contributors', text)
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
                '-DTOOL=' + shutil.which('smdhtool'), '-DNAME=UNDERTALE',
                '-DICON=' + str(icon), '-DOUT=' + str(out), '-P', SCRIPT]
        self.command(args)
        raw = out.read_bytes()
        self.assertEqual(raw[:4], b'SMDH')
        def title(data, offset, length):
            return data[offset:offset+length].decode('utf-16le').split('\0')[0]
        self.assertEqual(title(raw, 8, 128), 'UNDERTALE')
        self.assertEqual(title(raw, 136, 256), 'stage73 ' + commit + ' (o3ds/o2ds)')
        self.assertEqual(title(raw, 392, 128), 'Unofficial port - stage73 ' + commit + ' - Old 3DS')
        before = out.stat().st_mtime_ns
        self.command(args)
        self.assertEqual(out.stat().st_mtime_ns, before)
        self.env['CTRPORT_SMDH_BUILD_TAG'] = 'cand27'
        self.command(args)
        changed = out.read_bytes()
        self.assertEqual(title(changed, 136, 256), 'cand27 ' + commit + ' (o3ds/o2ds)')
        self.assertEqual(title(changed, 392, 128), 'Unofficial port - cand27 ' + commit + ' - Old 3DS')
        self.assertEqual(raw[0x2008:], changed[0x2008:])
        # Corrupt output must be repaired even if inputs still match the stamp.
        out.write_bytes(b'broken')
        self.command(args)
        self.assertEqual(out.read_bytes(), changed)

    @unittest.skipUnless(shutil.which('smdhtool') and shutil.which('bannertool'),
                         'Native tools for both SMDH formats')
    def test_real_release_smdh_english_publisher_in_both_formats(self):
        self.env['CTRPORT_SMDH_RELEASE'] = '1'
        for game in ('undertale', 'deltarune'):
            for model, label in (('o3ds', 'Old 3DS'), ('n3ds', 'New 3DS')):
                for mode, tool in (('3dsx', 'smdhtool'), ('cia', 'bannertool')):
                    with self.subTest(game=game, model=model, mode=mode):
                        out = Path(self.temp.name) / (game + model + mode + '.smdh')
                        self.command(['cmake', '-DROOT=' + str(self.root), '-DGAME=' + game,
                            '-DMODEL=' + model, '-DMODE=' + mode, '-DTOOL=' + shutil.which(tool),
                            '-DNAME=' + game.upper(), '-DICON=' + str(ROOT /
                                ('platform/3ds/assets/icon-' + game + '-' + model + '.png')),
                            '-DFLAGS=visible,allow3d,recordusage', '-DOUT=' + str(out), '-P', SCRIPT])
                        raw = out.read_bytes()
                        self.assertEqual(raw[:4], b'SMDH')
                        english = raw[8 + 512:8 + 1024]
                        fields = [english[start:end].decode('utf-16le').split('\0')[0]
                                  for start, end in ((0, 128), (128, 384), (384, 512))]
                        publisher = ('3lit3hax - Unofficial port' if game == 'undertale' else
                            'Unofficial port - v' + (self.root/'VERSION').read_text().strip() + ' - ' + label)
                        description = game.upper()
                        if mode == '3dsx':
                            description = ('v' + (self.root/'VERSION').read_text().strip() + ' ' +
                                ('(o3ds/o2ds)' if model == 'o3ds' else '(n3ds/n2ds)'))
                        self.assertEqual(fields, [game.upper(), description, publisher])


if __name__ == '__main__':
    unittest.main()
