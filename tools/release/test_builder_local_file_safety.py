"""Invented saves and documents only. No real card, installation or user folders."""
from pathlib import Path
import errno
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent/'builder'))
import cleanup
import pipeline
import products
import saves

UT = products.PRODUCTS['undertale']
OLD, NEW = b'OLD\n1\n0\n', b'NEW\n2\n0\n'


class LocalSafetyTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.work, self.card, self.pc = (self.base/n for n in ('work', 'card', 'pc'))
        self.live = self.card/UT.sd_save_dir()
        self.live.mkdir(parents=True)
        self.pc.mkdir()
        (self.live/'file0').write_bytes(OLD)
        (self.pc/'file0').write_bytes(NEW)
        self.scan = saves.scan_saves(str(self.pc), UT.key)

    def link(self, path, target, directory=True):
        path.parent.mkdir(parents=True, exist_ok=True)
        try:
            path.symlink_to(target, target_is_directory=directory)
        except OSError:
            if os.name != 'nt' or not directory:
                self.skipTest('Host does not allow file symlinks')
            # A junction inside the temporary fixture needs no elevation.
            command = "New-Item -ItemType Junction -LiteralPath '%s' -Target '%s' | Out-Null" % (
                str(path).replace("'", "''"), str(target).replace("'", "''"))
            # New-Item uses -Path, not -LiteralPath.
            result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command',
                command.replace('-LiteralPath', '-Path')], stdin=subprocess.DEVNULL,
                capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
        self.addCleanup(lambda: path.unlink() if path.is_symlink() else path.rmdir())

    def test_listing_permission_and_io_failures_never_authorize_replacement(self):
        original = os.listdir
        for number in (errno.EACCES, errno.EIO):
            for operation in ('preview', 'copy'):
                with self.subTest(errno=number, operation=operation):
                    def listing(path):
                        if Path(path) == self.live:
                            raise OSError(number, 'invented listing failure', str(path))
                        return original(path)
                    with mock.patch.object(saves.os, 'listdir', side_effect=listing):
                        with self.assertRaisesRegex(saves.DeliverError, 'could not be checked'):
                            if operation == 'preview':
                                saves.existing_sd_save_files(str(self.card), UT)
                            else:
                                saves.copy_pc_saves(str(self.card), UT, self.scan, overwrite=False)
                    self.assertEqual(list(self.live.iterdir()), [self.live/'file0'])
                    self.assertEqual((self.live/'file0').read_bytes(), OLD)
                    self.assertEqual((self.pc/'file0').read_bytes(), NEW)

    def test_real_missing_folder_imports_without_overwrite(self):
        (self.live/'file0').unlink()
        self.live.rmdir()
        result = saves.copy_pc_saves(str(self.card), UT, self.scan)
        self.assertEqual(result.backed_up, 0)
        self.assertIsNone(result.backup_dir)
        self.assertEqual((self.live/'file0').read_bytes(), NEW)

    def test_prepublication_listing_failure_preserves_live_and_backup(self):
        original = os.listdir
        calls = 0
        def listing(path):
            nonlocal calls
            if Path(path) == self.live:
                calls += 1
                if calls == 2:
                    raise OSError(errno.EIO, 'invented listing failure', str(path))
            return original(path)
        with mock.patch.object(saves.os, 'listdir', side_effect=listing):
            with self.assertRaisesRegex(saves.DeliverError, 'could not be checked'):
                saves.copy_pc_saves(str(self.card), UT, self.scan, overwrite=True)
        self.assertEqual((self.live/'file0').read_bytes(), OLD)
        self.assertEqual(next(self.live.glob('backup-*/file0')).read_bytes(), OLD)

    def test_save_folder_junction_is_refused_before_preview_or_write(self):
        (self.live/'file0').unlink()
        self.live.rmdir()
        outside = self.base/'outside'
        outside.mkdir()
        (outside/'file0').write_bytes(OLD)
        (outside/'only-copy.txt').write_bytes(b'PLAYER DOCUMENT')
        self.link(self.live, outside)
        for operation in (lambda: saves.existing_sd_save_files(str(self.card), UT),
                          lambda: saves.scan_sd_saves(str(self.card), UT),
                          lambda: saves.copy_pc_saves(str(self.card), UT, self.scan, overwrite=True)):
            with self.assertRaisesRegex(saves.DeliverError, 'points to another folder'):
                operation()
        self.assertEqual({p.name: p.read_bytes() for p in outside.iterdir()},
                         {'file0': OLD, 'only-copy.txt': b'PLAYER DOCUMENT'})

    def test_a_card_reached_through_a_linked_folder_imports_and_its_own_links_are_still_refused(self):
        # Some Linux systems list a card under /media, itself a link to /run/media; macOS reaches /var the same way.
        mounts = self.base/'mounts'
        self.link(mounts, self.base)
        card = mounts/'card'
        self.assertEqual(saves.existing_sd_save_files(str(card), UT), [os.path.normpath(os.path.join(str(card), UT.sd_save_dir(), 'file0'))])
        result = saves.copy_pc_saves(str(card), UT, self.scan, overwrite=True)
        self.assertEqual(result.backed_up, 1)
        self.assertEqual((self.live/'file0').read_bytes(), NEW)
        self.assertEqual(next(self.live.glob('backup-*/file0')).read_bytes(), OLD)
        # A link among the card's own folders is refused by this route too.
        outside = self.base/'outside'
        outside.mkdir()
        (outside/'only-copy.txt').write_bytes(b'PLAYER DOCUMENT')
        self.link(self.card/'inner', outside)
        with self.assertRaisesRegex(saves.DeliverError, 'points to another folder'):
            saves.require_sd_save_path(str(card), str(card/'inner'/'only-copy.txt'))
        self.assertEqual((outside/'only-copy.txt').read_bytes(), b'PLAYER DOCUMENT')

    def test_a_changed_card_root_refuses_instead_of_walking_forever(self):
        import install_record
        previous = self.base / 'previous-card' / 'file0'
        original_lstat = Path.lstat
        def bounded_lstat(path, *args, **kwargs):
            self.assertNotEqual(path, path.parent, 'save walk escaped the selected card')
            return original_lstat(path, *args, **kwargs)
        with mock.patch.object(install_record, 'target', return_value=previous), mock.patch.object(Path, 'lstat', bounded_lstat):
            with self.assertRaisesRegex(saves.DeliverError, 'No saves were replaced'):
                saves.require_sd_save_path(str(self.card), str(self.live / 'file0'))
        self.assertEqual((self.live / 'file0').read_bytes(), OLD)

    def test_staged_file_link_is_refused_without_touching_target(self):
        outside = self.base/'personal.txt'
        outside.write_bytes(b'KEEP')
        self.link(self.live/'file0.part', outside, directory=False)
        with self.assertRaises(saves.DeliverError):
            saves.copy_pc_saves(str(self.card), UT, self.scan, overwrite=True)
        self.assertEqual(outside.read_bytes(), b'KEEP')
        self.assertEqual((self.live/'file0').read_bytes(), OLD)

    def test_mirror_rejects_linked_src_before_removing_or_writing(self):
        outside = self.base/'game'
        (outside/'build').mkdir(parents=True)
        (outside/'build/only-copy.txt').write_bytes(b'KEEP')
        self.link(self.work/'src', outside)
        kit = self.base/'kit'
        (kit/'engine/src').mkdir(parents=True)
        (kit/'engine/src/probe.c').write_bytes(b'INVENTED')
        with self.assertRaisesRegex(ValueError, 'points to another folder'):
            pipeline.mirror_source(str(kit), str(self.work/'src'), UT.key)
        with self.assertRaises(pipeline.BuildFailed):
            pipeline.Build(UT, SimpleNamespace(root=None), str(self.work), 'o3ds', str(outside))
        self.assertEqual((outside/'build/only-copy.txt').read_bytes(), b'KEEP')
        self.assertFalse((outside/'engine').exists())
        self.assertFalse((self.work/'logs').exists())

    def test_nested_mutable_link_is_refused_before_lease_creation(self):
        outside = self.base/'outside'
        outside.mkdir()
        self.link(self.work/'src/build', outside)
        with self.assertRaises(pipeline.WorkLockError):
            pipeline.claim_work(str(self.work))
        self.assertFalse((self.work/pipeline.WORK_LEASE).exists())

    def test_unowned_older_work_is_kept_and_reported(self):
        path = self.work/'src/my-project/only-copy.txt'
        path.parent.mkdir(parents=True)
        path.write_bytes(b'PLAYER DOCUMENT')
        preview = cleanup.plan(self.work)
        deleted, kept = cleanup.remove(preview, ['build'])
        self.assertEqual(deleted, [])
        self.assertIn('no record that this builder created it', '\n'.join(kept))
        self.assertEqual(path.read_bytes(), b'PLAYER DOCUMENT')

    def test_recorded_build_files_removed_but_later_additions_and_edits_kept(self):
        root = self.work/'src'
        cleanup.record_before(self.work, [root], 'build')
        root.mkdir()
        generated, changed, personal = (root/n for n in ('generated.o', 'changed.o', 'only-copy.txt'))
        generated.write_bytes(b'GENERATED')
        changed.write_bytes(b'OLD BUILD')
        cleanup.record_after(self.work, [root])
        personal.write_bytes(b'PLAYER DOCUMENT')
        changed.write_bytes(b'PLAYER EDIT')
        cleanup.record_after(self.work, [root])  # Must never adopt additions on another visit.
        deleted, kept = cleanup.remove(cleanup.plan(self.work), ['build'])
        self.assertEqual(deleted, [str(generated)])
        self.assertEqual(personal.read_bytes(), b'PLAYER DOCUMENT')
        self.assertEqual(changed.read_bytes(), b'PLAYER EDIT')
        self.assertIn(str(personal), '\n'.join(kept))

    def test_a_later_tool_step_owns_what_it_adds_and_never_the_players_additions(self):
        root = self.base/'devkitpro'
        compiler, personal, library = (root/n for n in (
            'devkitARM/bin/arm-none-eabi-gcc', 'projects/my-homebrew/only-copy.c', 'libctru/lib/libctru.a'))
        cleanup.record_before(self.work, [root])
        compiler.parent.mkdir(parents=True)
        compiler.write_bytes(b'COMPILER')
        cleanup.record_after(self.work, [root])
        personal.parent.mkdir(parents=True)
        personal.write_bytes(b'PLAYER DOCUMENT')
        cleanup.record_before(self.work, [root])  # The same root's next step, as toolchain.run_step records it.
        library.parent.mkdir(parents=True)
        library.write_bytes(b'LIBRARY')
        cleanup.record_after(self.work, [root])
        deleted, kept = cleanup.remove(cleanup.plan(self.work), ['tools'])
        self.assertEqual({Path(path).resolve() for path in deleted}, {compiler.resolve(), library.resolve()})
        self.assertEqual(personal.read_bytes(), b'PLAYER DOCUMENT')

    def test_home_and_drive_root_are_refused_without_creating_a_lease(self):
        with mock.patch.object(Path, 'home', return_value=self.work):
            with self.assertRaisesRegex(pipeline.WorkLockError, 'home folder or a drive root'):
                pipeline.claim_work(str(self.work))
        with self.assertRaisesRegex(pipeline.WorkLockError, 'home folder or a drive root'):
            pipeline.claim_work(self.work.anchor)
        self.assertFalse(self.work.exists())

    def test_prepare_records_new_mirror_before_creation_and_allows_cleanup(self):
        build = pipeline.Build(UT, SimpleNamespace(root=None), str(self.work), 'o3ds', str(self.pc))
        def mirror(source, destination, *args, **kwargs):
            self.assertTrue((self.work/cleanup.JOURNAL).is_file())
            path = Path(destination)/'created.c'
            path.parent.mkdir()
            path.write_bytes(b'INVENTED')
            return 1, 0
        with mock.patch.object(pipeline, 'release_version', return_value='fixture'), \
             mock.patch.object(pipeline, 'release_source', return_value=dict(kind='release', source_sha256='fixture')), \
             mock.patch.object(pipeline, 'mirror_source', side_effect=mirror), \
             mock.patch.object(pipeline, 'write_python_shims'), \
             mock.patch.object(pipeline.library_pins, 'select', return_value={'packages': [], 'shims': None}), \
             mock.patch.object(pipeline.shutil, 'disk_usage', return_value=SimpleNamespace(free=10**12)):
            build.prepare()
        deleted, _ = cleanup.remove(cleanup.plan(self.work), ['build'])
        self.assertIn(str(self.work/'src/created.c'), deleted)

    def test_enoent_does_not_override_a_parent_that_names_the_save_folder(self):
        original = os.listdir
        def listing(path):
            if Path(path) == self.live:
                raise FileNotFoundError(errno.ENOENT, 'invented missing reply', str(path))
            return original(path)
        with mock.patch.object(saves.os, 'listdir', side_effect=listing):
            with self.assertRaisesRegex(saves.DeliverError, 'saves folder could not be checked'):
                saves.existing_sd_save_files(str(self.card), UT)
        self.assertEqual((self.live/'file0').read_bytes(), OLD)

    def test_enoent_and_unreadable_parent_do_not_prove_absence(self):
        original = os.listdir
        def listing(path):
            if Path(path) == self.live:
                raise FileNotFoundError(errno.ENOENT, 'invented missing reply', str(path))
            if Path(path) == self.live.parent:
                raise PermissionError(errno.EACCES, 'invented unreadable parent', str(path))
            return original(path)
        with mock.patch.object(saves.os, 'listdir', side_effect=listing):
            with self.assertRaises(saves.DeliverError):
                saves.existing_sd_save_files(str(self.card), UT)
        self.assertEqual((self.live/'file0').read_bytes(), OLD)


if __name__ == '__main__':
    unittest.main()
