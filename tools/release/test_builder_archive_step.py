"""The builder's archive step (builder/arwrap.py) and fresh build folders (Build.fresh_build_tree).

11-17 playtester (N14 kit, Windows): ar could not create its temporary file beside libbenchromfs_mount.a in a build
folder reused from an earlier kit with another devkitPro path, on every try. All paths here are invented."""
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / 'builder'))
import arwrap  # noqa: E402
import pipeline  # noqa: E402
import test_builder as fixtures  # noqa: E402

UT = pipeline.products.PRODUCTS['undertale']

# A stand-in ar/ranlib: refuses to write in a folder named by FAKEAR_BLOCK (as the playtester's ar did), otherwise
# writes a deterministic archive (member basenames and bytes) named by its target argument, relative to its cwd.
FAKE_AR = r'''
import os, sys
args = sys.argv[1:]
block = os.environ.get('FAKEAR_BLOCK')
if block and os.path.normcase(os.path.abspath(os.getcwd())) == os.path.normcase(os.path.abspath(block)):
    sys.stderr.write('ar.exe: could not create temporary file whilst writing archive: Permission denied\n')
    sys.exit(1)
if os.environ.get('FAKEAR_FAIL'):
    sys.stderr.write('ar.exe: fixture failure\n')
    sys.exit(1)
if args[0] in ('qc', 'q'):
    target, objects = args[1], args[2:]
    names = []
    for o in objects:
        if o.startswith('@'):
            names += [n.strip('"') for n in open(o[1:]).read().split()]
        else:
            names.append(o)
    data = open(target, 'rb').read() if args[0] == 'q' and os.path.exists(target) else b'!<arch>\n'
    for n in names:
        data += os.path.basename(n).encode() + b'/' + open(n, 'rb').read()
    open(target + '.tmp', 'wb').write(data)
    os.replace(target + '.tmp', target)
else:
    target = args[0]
    data = open(target, 'rb').read() + b'/index'
    open(target + '.tmp', 'wb').write(data)
    os.replace(target + '.tmp', target)
'''


class ArchiveStepTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        script = self.root / 'fakear.py'
        script.write_text(FAKE_AR, encoding='utf-8')
        if os.name == 'nt':
            self.ar = self.root / 'fakear.cmd'
            self.ar.write_text('@"%s" -I "%s" %%*\r\n' % (sys.executable, script), encoding='utf-8')
        else:
            self.ar = self.root / 'fakear'
            self.ar.write_text('#!/bin/sh\nexec "%s" -I "%s" "$@"\n' % (sys.executable, script), encoding='utf-8')
            self.ar.chmod(0o755)
        self.build = self.root / 'build'
        (self.build / 'CMakeFiles/x.dir').mkdir(parents=True)
        for name in ('a.c.obj', 'b.c.obj'):
            (self.build / 'CMakeFiles/x.dir' / name).write_bytes(name.encode() * 3)
        self.env = mock.patch.dict(os.environ, {'CTRPORT_ARCHIVE_TMP': str(self.root / 'work/artmp')})
        self.env.start()
        os.environ.pop('FAKEAR_BLOCK', None)
        os.environ.pop('FAKEAR_FAIL', None)
        self.cwd = os.getcwd()
        os.chdir(self.build)

    def tearDown(self):
        os.chdir(self.cwd)
        self.env.stop()
        for path in self.root.rglob('*'):
            try:
                path.chmod(stat.S_IWRITE | stat.S_IREAD)
            except OSError:
                pass
        self.tmp.cleanup()

    def archive(self, objects=('CMakeFiles/x.dir/a.c.obj', 'CMakeFiles/x.dir/b.c.obj')):
        rc = arwrap.main(['create', str(self.ar), 'qc', 'libx.a'] + list(objects))
        rc = rc or arwrap.main(['finish', str(self.ar), 'libx.a'])
        return rc

    def plain(self):
        """The archive the stand-in ar makes on its own in the build folder (CMake's default rules)."""
        env = dict(os.environ)
        env.pop('FAKEAR_BLOCK', None)
        subprocess.check_call([str(self.ar), 'qc', 'plain.a', 'CMakeFiles/x.dir/a.c.obj', 'CMakeFiles/x.dir/b.c.obj'],
                              env=env, cwd=self.build)
        subprocess.check_call([str(self.ar), 'plain.a'], env=env, cwd=self.build)
        data = (self.build / 'plain.a').read_bytes()
        (self.build / 'plain.a').unlink()
        return data

    def test_archive_is_made_in_a_private_folder_when_ar_cannot_write_beside_it(self):
        expected = self.plain()
        os.environ['FAKEAR_BLOCK'] = str(self.build)
        with self.assertRaises(subprocess.CalledProcessError):     # the bare tool fails there, as on the player's PC
            subprocess.check_call([str(self.ar), 'qc', 'libx.a', 'CMakeFiles/x.dir/a.c.obj'], cwd=self.build,
                                  stderr=subprocess.DEVNULL)
        self.assertEqual(self.archive(), 0)
        self.assertEqual((self.build / 'libx.a').read_bytes(), expected)
        self.assertEqual(list((self.root / 'work/artmp').iterdir()), [])      # private folders are removed

    def test_a_read_only_stale_archive_is_replaced(self):
        stale = self.build / 'libx.a'
        stale.write_bytes(b'stale archive from an earlier kit')
        stale.chmod(stat.S_IREAD)
        self.assertEqual(self.archive(), 0)
        self.assertEqual(stale.read_bytes(), self.plain())

    def test_response_file_entries_relative_to_the_build_folder(self):
        rsp = self.build / 'CMakeFiles/x.dir/objects1.rsp'
        rsp.write_text('CMakeFiles/x.dir/a.c.obj CMakeFiles/x.dir/b.c.obj\n', encoding='utf-8')
        self.assertEqual(self.archive(['@CMakeFiles/x.dir/objects1.rsp']), 0)
        self.assertEqual((self.build / 'libx.a').read_bytes(), self.plain())

    def test_append_keeps_the_archive_so_far(self):
        self.assertEqual(arwrap.main(['create', str(self.ar), 'qc', 'libx.a', 'CMakeFiles/x.dir/a.c.obj']), 0)
        self.assertEqual(arwrap.main(['append', str(self.ar), 'q', 'libx.a', 'CMakeFiles/x.dir/b.c.obj']), 0)
        self.assertEqual(arwrap.main(['finish', str(self.ar), 'libx.a']), 0)
        self.assertEqual((self.build / 'libx.a').read_bytes(), self.plain())

    def test_failure_names_target_errors_attributes_writability_and_the_likely_causes(self):
        os.environ['FAKEAR_FAIL'] = '1'
        (self.build / 'libx.a').write_bytes(b'old')
        err = []
        with mock.patch.object(arwrap.sys, 'stderr') as stream:
            stream.write.side_effect = err.append
            self.assertNotEqual(self.archive(), 0)
        text = ''.join(err)
        self.assertIn(arwrap.FAILED + ' for ' + str(self.build / 'libx.a'), text)
        self.assertIn('target: a file exists, 3 bytes', text)
        self.assertIn('read-only no', text)
        self.assertIn('target folder: writable', text)
        for words in ('antivirus', 'Protection history', 'Controlled Folder Access', 'OneDrive'):
            self.assertIn(words, text)
        self.assertEqual((self.build / 'libx.a').read_bytes(), b'old')     # a failed step leaves the old file alone

    def test_a_target_that_cannot_be_replaced_reports_the_os_error(self):
        err = []
        with mock.patch.object(arwrap.os, 'replace', side_effect=PermissionError(13, 'fixture denied')), \
                mock.patch.object(arwrap.os, 'rename', side_effect=PermissionError(13, 'fixture denied')), \
                mock.patch.object(arwrap.time, 'sleep'), mock.patch.object(arwrap.sys, 'stderr') as stream:
            stream.write.side_effect = err.append
            self.assertEqual(arwrap.main(['create', str(self.ar), 'qc', 'libx.a', 'CMakeFiles/x.dir/a.c.obj']), 1)
        text = ''.join(err)
        self.assertIn('error: errno 13', text)
        self.assertIn('target: no file with that name', text)


class FreshBuildFolderTests(unittest.TestCase):
    setUp = fixtures.BuildTest.setUp
    tearDown = fixtures.BuildTest.tearDown
    outputs = fixtures.BuildTest.outputs
    make = fixtures.BuildTest.make

    def build_dir(self):
        return Path(self.work) / 'src/build/3ds-undertale-n3ds'

    def stale_tree(self, toolchain_root):
        build = self.build_dir()
        (build / 'CMakeFiles').mkdir(parents=True)
        (build / 'CMakeCache.txt').write_text('CMAKE_TOOLCHAIN_FILE:FILEPATH=%s/cmake/3DS.cmake\n' % toolchain_root)
        stale = build / 'libbenchromfs_mount.a'
        stale.write_bytes(b'stale')
        stale.chmod(stat.S_IREAD)
        return build

    def test_a_tree_from_another_kit_or_toolchain_is_replaced_and_inputs_and_caches_stay(self):
        build = self.stale_tree('Q:/old-devkitPro')
        keep = [Path(self.work) / 'ccache/keep.bin', Path(self.work) / 'data/keep.bin']
        for path in keep:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'keep')
        job, seen = self.make(['[100%] y\n'])
        job.run()
        log = Path(job.log_path).read_text(encoding='utf-8')
        self.assertRegex(log, r'builder: build folder (new|replaced)')    # the new kit source already clears it
        self.assertNotIn('old-devkitPro', (build / 'CMakeCache.txt').read_text())   # the fake build's own cache
        self.assertFalse((build / 'libbenchromfs_mount.a').exists())
        self.assertEqual(list(Path(self.work).glob('stale-build-*')), [])
        for path in keep:
            self.assertEqual(path.read_bytes(), b'keep')
        stamp = json.loads((build / pipeline.BUILD_STAMP).read_text(encoding='utf-8'))
        self.assertEqual(stamp['source_sha256'], job.release_source['source_sha256'])
        self.assertTrue(stamp['toolchain_file'].endswith(os.path.normcase(os.path.join('cmake', '3DS.cmake'))))
        self.assertEqual(seen['env']['CTRPORT_ARCHIVE_WRAPPER'], os.path.join(self.work, 'bin', 'arwrap.py'))
        self.assertEqual(seen['env']['CTRPORT_ARCHIVE_TMP'], os.path.join(self.work, 'artmp'))
        self.assertEqual(Path(self.work, 'bin/arwrap.py').read_bytes(), Path(pipeline.ARCHIVE_WRAPPER).read_bytes())

    def test_the_same_kit_and_toolchain_keep_the_tree_and_a_changed_toolchain_replaces_it(self):
        job, _ = self.make(['[100%] y\n'])
        job.run()
        marker = self.build_dir() / 'kept.o'
        marker.write_bytes(b'x')
        job, _ = self.make(['[100%] y\n'])
        job.run()
        self.assertIn('builder: build folder kept', Path(job.log_path).read_text(encoding='utf-8'))
        self.assertTrue(marker.exists())
        job, _ = self.make(['[100%] y\n'])
        job.tc.root = os.path.join(self.work, 'other devkitPro')
        job.run()
        self.assertIn('builder: build folder replaced (devkitpro, toolchain_file changed)',
                      Path(job.log_path).read_text(encoding='utf-8'))
        self.assertFalse(marker.exists())

    def test_the_library_wrapper_folder_is_the_toolchain_file_the_stamp_records(self):
        def run(shims):
            with mock.patch.object(pipeline.library_pins, 'select', return_value={'packages': [], 'shims': shims}):
                job, _ = self.make(['[100%] y\n'])
                job.run()
            return Path(job.log_path).read_text(encoding='utf-8')
        first = os.path.join(self.work, 'bin', 'libraries', 'run-a')
        run(first)
        self.assertIn('builder: build folder kept', run(first))
        self.assertIn('builder: build folder replaced (toolchain_file changed)',
                      run(os.path.join(self.work, 'bin', 'libraries', 'run-b')))

    def test_same_kit_with_a_stale_cache_from_another_toolchain_and_a_read_only_archive_starts_fresh(self):
        job, _ = self.make(['[100%] y\n'])
        job.run()                                   # the mirror now matches this kit, so only the stamp decides
        (self.build_dir() / pipeline.BUILD_STAMP).unlink()
        self.stale_tree_in_place('Q:/old-devkitPro')
        job, _ = self.make(['[100%] y\n'])
        job.run()
        self.assertIn('builder: build folder replaced (it had no build stamp)',
                      Path(job.log_path).read_text(encoding='utf-8'))
        self.assertFalse((self.build_dir() / 'libbenchromfs_mount.a').exists())
        self.assertNotIn('old-devkitPro', (self.build_dir() / 'CMakeCache.txt').read_text())

    def stale_tree_in_place(self, toolchain_root):
        build = self.build_dir()
        (build / 'CMakeCache.txt').write_text('CMAKE_TOOLCHAIN_FILE:FILEPATH=%s/cmake/3DS.cmake\n' % toolchain_root)
        stale = build / 'libbenchromfs_mount.a'
        stale.write_bytes(b'stale')
        stale.chmod(stat.S_IREAD)

    def test_an_archive_failure_names_antivirus_controlled_folder_access_and_onedrive(self):
        job, _ = self.make(['Linking C static library libbenchromfs_mount.a\n',
                            'archive step: FAILED for libbenchromfs_mount.a\n'], rc=2)
        with self.assertRaises(pipeline.BuildFailed) as caught:
            job.run()
        for words in ('antivirus', 'Controlled Folder Access', 'OneDrive', 'devkitPro folder', 'work folder'):
            self.assertIn(words, str(caught.exception))

    def test_a_tree_that_cannot_be_moved_aside_stops_with_the_hint(self):
        self.stale_tree('Q:/old-devkitPro')
        job, _ = self.make(['[100%] y\n'])
        with mock.patch.object(pipeline.os, 'rename', side_effect=PermissionError(13, 'fixture denied')):
            with self.assertRaises(pipeline.BuildFailed) as caught:
                job.run()
        self.assertIn('old build folder could not be replaced', str(caught.exception))
        self.assertIn('Controlled Folder Access', str(caught.exception))


if __name__ == '__main__':
    unittest.main()
