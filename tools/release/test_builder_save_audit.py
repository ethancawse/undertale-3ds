"""Save import safety (outside audit, 2026-11-04). Invented files and loopback FTP only; never a console."""
from pathlib import Path
import sys
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE/'builder'))
from fake_ftpd import FakeFtpd  # public copy of the staging tests' fake ftpd
import ftp_delivery
import ftp_saves
import products
import saves

UT = products.PRODUCTS['undertale']


class FatFtpd(FakeFtpd):
    """Looks names up the way the 3DS card's FAT does, whatever the host's own file system does."""
    def local(self, arg):
        path = self.root
        for part in [p for p in arg.split('/') if p]:
            match = None
            if path.is_dir():
                match = next((child for child in path.iterdir() if child.name.casefold() == part.casefold()), None)
            path = match if match is not None else path/part
        return path


class SaveFixture(unittest.TestCase):
    server_class = FatFtpd

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.card = self.root/'card'
        (self.card/'3ds').mkdir(parents=True)
        (self.card/'Nintendo 3DS').mkdir()
        self.saves = self.card/UT.sd_save_dir()
        self.saves.mkdir(parents=True)
        self.pc = self.root/'pc'
        self.pc.mkdir()
        self.server = self.server_class(self.card)
        self.address = '127.0.0.1:%d' % self.server.port
        self.trace_timeouts()

    def trace_timeouts(self):
        """A client read that times out (the builder's own 10 s ftpd timeout, unchanged) gets a note naming the fake
        ftpd's command and step it waited on, and how late the waiting thread was (toolfix 11-11: a full-suite load
        flake). Nothing is retried and no timeout changes; the exception and its handling stay the builder's."""
        server = self.server

        def note(ftp, began, exc):
            if not any(str(n).startswith('fake ftpd:') for n in getattr(exc, '__notes__', ())):
                exc.add_note(server.describe_wait(began, time.monotonic(), getattr(ftp, 'timeout', None)))

        def traced(real):
            def call(ftp, *args, **kwargs):
                began = time.monotonic()
                try:
                    return real(ftp, *args, **kwargs)
                except TimeoutError as exc:
                    note(ftp, began, exc)
                    raise
            return call

        def traced_listing(real):
            def listing(ftp, *args, **kwargs):
                began = time.monotonic()
                try:
                    yield from real(ftp, *args, **kwargs)
                except TimeoutError as exc:
                    note(ftp, began, exc)
                    raise
            return listing

        from unittest import mock
        for name, wrap in (('getline', traced), ('retrbinary', traced), ('storbinary', traced),
                           ('mlsd', traced_listing)):
            patch = mock.patch.object(ftp_delivery.ConsoleFTP, name, wrap(getattr(ftp_delivery.ConsoleFTP, name)))
            patch.start()
            self.addCleanup(patch.stop)

    def tearDown(self):
        self.server.close()
        self.temp.cleanup()

    def live(self):
        return {p.name: p.read_bytes() for p in self.saves.iterdir() if p.is_file()}


class TimeoutNoteTests(SaveFixture):
    """The note a timed-out read carries names the fake ftpd's step (here with a 0.2 s test connection, not the
    builder's 10 s, so each case takes well under a second)."""

    def stalled_stor(self, step):
        import ftplib
        from unittest import mock
        ftp = ftp_delivery.ConsoleFTP()
        ftp.connect('127.0.0.1', self.server.port, timeout=0.2)
        ftp.login()
        source = self.root/'upload'
        source.write_bytes(b'NEW\n2\n0\n')
        real_write = Path.write_bytes
        stalled = []

        def slow_write(path, data):
            if path.name == 'file0.builder-part' and not stalled:
                stalled.append(path)
                time.sleep(0.6)
            return real_write(path, data)

        def slow_reply(cmd, text):
            if text.startswith('226 stored') and not stalled:
                stalled.append(cmd)
                time.sleep(0.6)
        if step == 'reply':
            self.server.before_reply = slow_reply
        with mock.patch.object(Path, 'write_bytes', slow_write if step == 'write' else real_write):
            with source.open('rb') as stream, self.assertRaises(TimeoutError) as caught:
                ftp.storbinary('STOR /%s/file0.builder-part' % UT.sd_save_dir(), stream)
            deadline = time.monotonic() + 5
            while self.server.trace[-1]['steps'][-1][0] != 'reply' and time.monotonic() < deadline:
                time.sleep(0.02)  # let the stalled step finish before the card folder is removed
        ftp.close()
        self.assertTrue(stalled)
        return '\n'.join(caught.exception.__notes__)

    def test_a_stalled_card_write_is_named(self):
        text = self.stalled_stor('write')
        self.assertIn('STILL RUNNING', text)
        self.assertRegex(text, r'STOR /\S+/file0\.builder-part: in step write for 0\.\d+ s \(after receive\)')

    def test_a_stalled_reply_is_named(self):
        text = self.stalled_stor('reply')
        self.assertRegex(text, r'STOR /\S+/file0\.builder-part: in step reply for 0\.\d+ s \(after write\)')

    def wait_for_end(self, cmd):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            ends = [r['steps'][-1][0] for r in self.server.trace if r['cmd'] == cmd]
            if ends and ends[-1] in ('reply', 'closed', 'aborted'):
                return ends[-1]
            time.sleep(0.02)
        self.fail('%s never ended' % cmd)

    def dropped_stor(self):
        import io
        (self.card/'drop').mkdir()
        self.server.drop_stor['/drop'] = 1
        ftp = ftp_delivery.ConsoleFTP()
        ftp.connect('127.0.0.1', self.server.port, timeout=5)
        ftp.login()
        with self.assertRaises((EOFError, OSError, ftplib_errors())):
            ftp.storbinary('STOR /drop/part', io.BytesIO(b'x' * 8192))
        ftp.close()
        deadline = time.monotonic() + 5
        while not (self.card/'drop'/'part').is_file() and time.monotonic() < deadline:
            time.sleep(0.02)  # the handler wrote its half: it returns straight after
        self.assertTrue((self.card/'drop'/'part').is_file())

    def test_a_dropped_upload_is_not_still_running_at_a_later_timeout(self):
        # c26rev4 FTP-1: the dropped STOR wrote its half and closed; a later stall elsewhere must not name it running
        self.dropped_stor()
        ftp = ftp_delivery.ConsoleFTP()
        ftp.connect('127.0.0.1', self.server.port, timeout=0.2)
        ftp.login()
        self.server.before_reply = lambda cmd, text: time.sleep(0.6) if cmd == 'NOOP' else None
        with self.assertRaises(TimeoutError) as caught:
            ftp.voidcmd('NOOP')
        self.wait_for_end('NOOP')
        ftp.close()
        text = '\n'.join(caught.exception.__notes__)
        self.assertNotRegex(text, r'STILL RUNNING conn \d+ STOR')
        self.assertRegex(text, r'STILL RUNNING conn \d+ NOOP : in step reply for 0\.\d+ s \(after start\)')
        self.assertRegex(text, r'ENDED WITHOUT AN ANSWER conn \d+ STOR /drop/part: closed after receive, '
                               r'\d+\.\d+ s before the wait began')
        self.assertEqual(self.wait_for_end('STOR'), 'closed')

    def test_an_answered_wait_after_a_dropped_upload_does_not_claim_every_command_answered(self):
        self.dropped_stor()
        self.assertEqual(self.wait_for_end('STOR'), 'closed')
        now = time.monotonic()
        text = self.server.describe_wait(now, now, 0.2)
        self.assertNotIn('STILL RUNNING', text)
        self.assertNotIn('the server had answered', text)
        self.assertIn('every command since the wait began was answered', text)
        self.assertIn('ENDED WITHOUT AN ANSWER', text)
        # a drop during the wait is not blamed on the waiting thread
        text = self.server.describe_wait(now - 30, now, 0.2)
        self.assertIn('a command ended without an answer while the client waited', text)

    def test_a_handler_that_raises_is_recorded_aborted(self):
        def boom(cmd, text):
            if cmd == 'NOOP':
                raise RuntimeError('injected')
        self.server.before_reply = boom
        ftp = ftp_delivery.ConsoleFTP()
        ftp.connect('127.0.0.1', self.server.port, timeout=5)
        ftp.login()
        with mock_handle_error(self.server), self.assertRaises((EOFError, OSError, ftplib_errors())):
            ftp.voidcmd('NOOP')
        self.assertEqual(self.wait_for_end('NOOP'), 'aborted')
        ftp.close()
        now = time.monotonic()
        text = self.server.describe_wait(now, now, 0.2)
        self.assertNotIn('STILL RUNNING', text)
        self.assertRegex(text, r'ENDED WITHOUT AN ANSWER conn \d+ NOOP : aborted after start, .* \(RuntimeError\)')

    def ended_after_the_timeout(self, cmd, client, hold):
        """c26rev4 FTP-2: the client's timeout time is taken while the server is still in a step; the command ends
        (unanswered) before describe_wait takes its snapshot. Returns the note for that timeout time."""
        import threading
        held, release = threading.Event(), threading.Event()
        hold(held, release)
        caller = threading.Thread(target=client, daemon=True)
        caller.start()
        self.assertTrue(held.wait(5))
        began = time.monotonic() - 0.1
        now = time.monotonic()   # the timeout's time, before the server's end
        release.set()
        end = self.wait_for_end(cmd)
        caller.join(5)
        return end, self.server.describe_wait(began, now, 0.1)

    def connected(self):
        ftp = ftp_delivery.ConsoleFTP()
        ftp.connect('127.0.0.1', self.server.port, timeout=5)
        ftp.login()
        return ftp

    def test_a_drop_that_closes_after_the_timeout_is_still_running_at_it(self):
        import io
        from unittest import mock
        (self.card/'drop').mkdir()
        self.server.drop_stor['/drop'] = 1
        ftp = self.connected()
        real_write = Path.write_bytes

        def hold(held, release):
            def write(path, data):
                if path.name == 'part':
                    held.set()
                    release.wait(5)
                return real_write(path, data)
            patcher = mock.patch.object(Path, 'write_bytes', write)
            patcher.start()
            self.addCleanup(patcher.stop)

        def client():
            try:
                ftp.storbinary('STOR /drop/part', io.BytesIO(b'x' * 8192))
            except (EOFError, OSError, ftplib_errors()):
                pass
        end, text = self.ended_after_the_timeout('STOR', client, hold)
        ftp.close()
        self.assertEqual(end, 'closed')
        self.assertNotIn('had answered', text)
        self.assertRegex(text, r'STILL RUNNING conn \d+ STOR /drop/part: in step write for \d+\.\d+ s \(after receive\); '
                               r'it ended after the timeout \(closed \d+\.\d+ s later\)')

    def test_a_handler_that_raises_after_the_timeout_is_still_running_at_it(self):
        ftp = self.connected()

        def hold(held, release):
            def boom(cmd, text):
                if cmd == 'NOOP':
                    held.set()
                    release.wait(5)
                    raise RuntimeError('injected')
            self.server.before_reply = boom

        def client():
            try:
                ftp.voidcmd('NOOP')
            except (EOFError, OSError, ftplib_errors()):
                pass
        with mock_handle_error(self.server):
            end, text = self.ended_after_the_timeout('NOOP', client, hold)
        ftp.close()
        self.assertEqual(end, 'aborted')
        self.assertNotIn('had answered', text)
        self.assertRegex(text, r'STILL RUNNING conn \d+ NOOP : in step reply for \d+\.\d+ s \(after start\); '
                               r'it ended after the timeout \(aborted \d+\.\d+ s later\) \(RuntimeError\)')


def ftplib_errors():
    import ftplib
    return ftplib.Error


def mock_handle_error(server):
    """socketserver prints a handler's exception to stderr; keep the injected one out of the test log."""
    from unittest import mock
    return mock.patch.object(server.server, 'handle_error', lambda request, address: None)


class CaseOnlyNameTests(SaveFixture):
    """AUD-02: card FILE0 and PC file0 are one FAT file; the import must not delete the save it just wrote."""

    def test_ftp_import_keeps_the_new_save_when_the_card_spells_it_in_capitals(self):
        (self.saves/'FILE0').write_bytes(b'OLD\n1\n0\n')
        (self.pc/'file0').write_bytes(b'NEW\n2\n0\n')
        expected = ftp_saves.inspect(self.address, UT)
        result = ftp_saves.copy(self.address, UT, saves.scan_saves(str(self.pc)), expected, overwrite=True)
        self.assertEqual(list(self.live().values()), [b'NEW\n2\n0\n'])
        self.assertEqual(result.copied, 1)
        backup = self.card/result.backup_dir.lstrip('/')
        self.assertEqual([p.read_bytes() for p in backup.iterdir()], [b'OLD\n1\n0\n'])

    def test_ftp_import_still_removes_card_files_the_pc_does_not_have(self):
        (self.saves/'FILE0').write_bytes(b'OLD\n1\n0\n')
        (self.saves/'FILE9').write_bytes(b'OLD\n1\n0\n')
        (self.pc/'file0').write_bytes(b'NEW\n2\n0\n')
        expected = ftp_saves.inspect(self.address, UT)
        ftp_saves.copy(self.address, UT, saves.scan_saves(str(self.pc)), expected, overwrite=True)
        self.assertEqual(list(self.live().values()), [b'NEW\n2\n0\n'])

    def test_pc_names_that_clash_on_fat_are_refused_before_anything_changes(self):
        scan = saves.SaveScan(str(self.pc), [saves.SaveFile('file0', str(self.pc/'a'), 8, 0.0, ''),
                                             saves.SaveFile('FILE0', str(self.pc/'b'), 8, 0.0, '')], True)
        (self.saves/'file0').write_bytes(b'OLD\n1\n0\n')
        with self.assertRaisesRegex(ftp_delivery.TransferError, 'differ only in capital letters'):
            ftp_saves.copy(self.address, UT, scan, ftp_saves.Snapshot({}, ''), overwrite=True)
        with self.assertRaisesRegex(saves.DeliverError, 'differ only in capital letters'):
            saves.copy_pc_saves(str(self.card), UT, scan, overwrite=True)
        self.assertEqual(self.live(), {'file0': b'OLD\n1\n0\n'})


class CheckedBytesTests(SaveFixture):
    """DEEP-06: the import writes the bytes the preview checked, never the PC file as it is later."""
    checked = {'file0': b'NEW\n1\n0\n', 'undertale.ini': b'[General]\nName="NEW"\nLove="1"\n'}

    def prepare(self):
        for name, data in self.checked.items():
            (self.pc/name).write_bytes(data)
        (self.saves/'file0').write_bytes(b'OLD\n1\n0\n')
        scan = saves.scan_saves(str(self.pc))
        self.assertFalse(scan.has_corrupt)
        return scan

    def test_sd_import_writes_the_checked_bytes_after_the_pc_file_turns_corrupt(self):
        scan = self.prepare()
        (self.pc/'file0').write_bytes(b'\0'*8)  # The PC file changes after the preview.
        result = saves.copy_pc_saves(str(self.card), UT, scan, overwrite=True)
        self.assertEqual(result.copied, 2)
        live = self.live()
        self.assertEqual({k: v for k, v in live.items() if k in self.checked}, self.checked)
        self.assertFalse(saves.inspect_save_file(str(self.saves/'file0')).corrupt)

    def test_sd_import_writes_the_previewed_player_not_a_newer_one(self):
        scan = self.prepare()
        (self.pc/'file0').write_bytes(b'OTHER\n7\n0\n')
        saves.copy_pc_saves(str(self.card), UT, scan, overwrite=True)
        self.assertEqual((self.saves/'file0').read_bytes(), self.checked['file0'])

    def test_sd_import_refuses_a_staged_copy_that_differs_from_the_checked_bytes(self):
        scan = self.prepare()
        real_utime = saves.os.utime
        def damage(path, *args, **kwargs):
            real_utime(path, *args, **kwargs)
            if str(path).endswith('file0.part'):
                Path(path).write_bytes(b'NEW\n1\n9\n')  # A card that keeps other bytes than it was given.
        from unittest import mock
        with mock.patch.object(saves.os, 'utime', side_effect=damage):
            with self.assertRaises(saves.DeliverError):
                saves.copy_pc_saves(str(self.card), UT, scan, overwrite=True)
        self.assertEqual((self.saves/'file0').read_bytes(), b'OLD\n1\n0\n')
        self.assertEqual([p.name for p in self.saves.iterdir() if p.is_file()], ['file0'])

    def test_ftp_import_sends_the_checked_bytes_after_the_pc_file_turns_corrupt(self):
        scan = self.prepare()
        expected = ftp_saves.inspect(self.address, UT)
        (self.pc/'file0').write_bytes(b'\0'*8)
        (self.pc/'undertale.ini').write_bytes(b'[General]\nName="OTHER"\nLove="5"\n')
        result = ftp_saves.copy(self.address, UT, scan, expected, overwrite=True)
        self.assertEqual(result.copied, 2)
        self.assertEqual(self.live(), self.checked)

    def test_a_scan_built_without_bytes_is_checked_again_before_copying(self):
        (self.pc/'file0').write_bytes(b'\0'*8)
        scan = saves.SaveScan(str(self.pc), [saves.SaveFile('file0', str(self.pc/'file0'), 8, 0.0, '')], True)
        with self.assertRaisesRegex(saves.DeliverError, "Can't read file0"):
            saves.copy_pc_saves(str(self.card), UT, scan, overwrite=True)
        with self.assertRaisesRegex(ftp_delivery.TransferError, "Can't read file0"):
            ftp_saves.copy(self.address, UT, scan, ftp_saves.Snapshot({}, ''), overwrite=True)
        self.assertEqual(self.live(), {})


class CancelCleanupTests(SaveFixture):
    def test_cancel_at_every_progress_event_removes_only_this_imports_parts_and_backup(self):
        old = {'file0': b'OLD\n1\n0\n', 'file9': b'OLD\n1\n0\n'}
        new = {'file0': b'NEW\n2\n0\n', 'file8': b'NEW\n2\n0\n',
               'file9': b'NEW\n2\n0\n', 'undertale.ini': b'[General]\nName="NEW"\nLove="2"\n'}
        for name, data in new.items():
            (self.pc/name).write_bytes(data)
        scan = saves.scan_saves(str(self.pc))
        def reset():
            for path in sorted(self.saves.rglob('*'), key=lambda p: len(p.parts), reverse=True):
                path.rmdir() if path.is_dir() else path.unlink()
            for name, data in old.items():
                (self.saves/name).write_bytes(data)
            kept = self.saves/'backup-keep'
            kept.mkdir()
            (kept/'file0').write_bytes(b'OLDER\n1\n0\n')
            return ftp_saves.inspect(self.address, UT)
        expected = reset()
        events = []
        ftp_saves.copy(self.address, UT, scan, expected, overwrite=True, progress=events.append)
        for cancel_at in range(1, len(events)+1):
            with self.subTest(cancel_at=cancel_at):
                expected = reset()
                seen = []
                def progress(event):
                    seen.append(event)
                with self.assertRaises(ftp_delivery.Cancelled) as caught:
                    ftp_saves.copy(self.address, UT, scan, expected, overwrite=True,
                        progress=progress, cancelled=lambda: len(seen) >= cancel_at)
                self.assertIn('No saves were changed.', str(caught.exception))
                self.assertIn('choose Next again', str(caught.exception))
                self.assertNotIn('Send', str(caught.exception))
                self.assertNotIn('update', str(caught.exception))
                self.assertEqual(self.live(), old)
                self.assertEqual(sorted(p.name for p in self.saves.iterdir()), sorted([*old, 'backup-keep']))
                self.assertEqual((self.saves/'backup-keep/file0').read_bytes(), b'OLDER\n1\n0\n')
                self.assertEqual({p.name: p.read_bytes() for p in self.pc.iterdir()}, new)


class StoppedPublishTests(SaveFixture):
    """DEEP-07: a failure while live saves are replaced says what the card holds, where the backup is, how to finish
    and how to undo; and the retry finishes."""
    old = {'file0': b'OLD\n1\n0\n', 'undertale.ini': b'[General]\nName="OLD"\nLove="1"\n', 'file9': b'OLD\n1\n0\n'}
    new = {'file0': b'NEW\n2\n0\n', 'undertale.ini': b'[General]\nName="NEW"\nLove="2"\n'}

    def prepare(self):
        for name, data in self.old.items():
            (self.saves/name).write_bytes(data)
        for name, data in self.new.items():
            (self.pc/name).write_bytes(data)
        return saves.scan_saves(str(self.pc)), ftp_saves.inspect(self.address, UT)

    def failing(self, verb, target, when):
        """An ftpd client whose publish-step DELE or RNTO of one save fails once, before or after the card acts."""
        import ftplib
        folder = '/' + UT.sd_save_dir() + '/'
        state = {'left': 1}

        def hit(path):
            name = path[len(folder):] if path.startswith(folder) else None
            return state['left'] and name is not None and name.removesuffix('.save-part').casefold() == target.casefold()

        class Client(ftp_delivery.ConsoleFTP):
            def delete(self, path):
                if verb == 'delete' and hit(path) and not path.endswith('.save-part'):
                    state['left'] = 0
                    if when == 'after':
                        super().delete(path)
                        raise ftplib.error_temp('421 fixture connection lost')
                    raise ftplib.error_perm('550 fixture refused')
                return super().delete(path)

            def rename(self, source, destination):
                if verb == 'rename' and source.endswith('.save-part') and hit(source):
                    state['left'] = 0
                    if when == 'after':
                        super().rename(source, destination)
                        raise ftplib.error_temp('421 fixture connection lost')
                    raise ftplib.error_temp('451 fixture save promotion failed')
                return super().rename(source, destination)
        return Client

    def backups(self):
        return sorted(p for p in self.saves.iterdir() if p.is_dir() and p.name.startswith('backup-'))

    def check_report(self, caught):
        exc = caught.exception
        self.assertIsInstance(exc, ftp_saves.IncompleteImport)
        backup = self.card/exc.backup.lstrip('/')
        self.assertEqual({p.name: p.read_bytes() for p in backup.iterdir()}, self.old)
        live = self.live()
        for name, state in exc.states.items():
            expect = {'new': self.new.get(name), 'old': self.old.get(name)}.get(state)
            if state in ('new', 'old'):
                self.assertEqual(live.get(name), expect, (name, state))
            elif state in ('waiting', 'removed'):
                self.assertNotIn(name, live, (name, state))
        for text in (str(exc), ftp_saves.error_text(exc)):
            self.assertIn(exc.backup, text)
            self.assertIn('copy every file from %s into /%s' % (exc.backup, UT.sd_save_dir()), text)
        self.assertIn('Click Next to finish copying.', ftp_saves.error_text(exc))
        self.assertIn('run the same command again with --overwrite-saves', str(exc))
        return exc

    def test_the_audit_case_reports_backup_and_what_was_replaced(self):
        scan, expected = self.prepare()
        with self.assertRaises(ftp_saves.IncompleteImport) as caught:
            ftp_saves.copy(self.address, UT, scan, expected, overwrite=True,
                           factory=self.failing('rename', 'undertale.ini', 'before'))
        exc = self.check_report(caught)
        text = ftp_saves.error_text(exc)
        self.assertIn('(451 fixture save promotion failed)', text)
        self.assertIn('Now the PC save: file0.', text)
        self.assertIn('May be missing: undertale.ini.', text)
        self.assertIn('Still the old 3DS save: file9.', text)
        self.assertNotIn('Nothing was replaced', text)

    def test_every_publish_step_failure_is_reported_and_the_wizard_retry_finishes(self):
        steps = [(verb, name, when) for name in ('file0', 'undertale.ini') for verb in ('delete', 'rename')
                 for when in ('before', 'after')] + [('delete', 'file9', when) for when in ('before', 'after')]
        for verb, name, when in steps:
            with self.subTest(verb=verb, name=name, when=when):
                for p in list(self.saves.rglob('*'))[::-1]:
                    p.rmdir() if p.is_dir() else p.unlink()
                scan, expected = self.prepare()
                with self.assertRaises(ftp_saves.IncompleteImport) as caught:
                    ftp_saves.copy(self.address, UT, scan, expected, overwrite=True,
                                   factory=self.failing(verb, name, when))
                exc = self.check_report(caught)
                # The wizard's Next: same scan, same confirmed snapshot, and the stopped import handed back.
                result = ftp_saves.copy(self.address, UT, scan, expected, overwrite=True, resume=exc)
                self.assertEqual(self.live(), self.new)
                self.assertEqual(result.backup_dir, exc.backup)
                self.assertEqual(result.backed_up, 3)
                self.assertEqual(len(self.backups()), 1)

    def test_back_then_next_also_finishes_with_the_original_backup(self):
        scan, expected = self.prepare()
        with self.assertRaises(ftp_saves.IncompleteImport) as caught:
            ftp_saves.copy(self.address, UT, scan, expected, overwrite=True,
                           factory=self.failing('rename', 'undertale.ini', 'before'))
        fresh = ftp_saves.inspect(self.address, UT)  # Back, then Next: the page checks the card again.
        result = ftp_saves.copy(self.address, UT, scan, fresh, overwrite=True, resume=caught.exception)
        self.assertEqual(self.live(), self.new)
        self.assertEqual(result.backup_dir, caught.exception.backup)
        self.assertEqual(len(self.backups()), 1)

    def test_cancelled_resume_keeps_the_backup_of_saves_already_replaced(self):
        scan, expected = self.prepare()
        with self.assertRaises(ftp_saves.IncompleteImport) as caught:
            ftp_saves.copy(self.address, UT, scan, expected, overwrite=True,
                           factory=self.failing('rename', 'undertale.ini', 'before'))
        stopped = caught.exception
        current = ftp_saves.inspect(self.address, UT)
        backup = self.card/stopped.backup.lstrip('/')
        seen = []
        with self.assertRaises(ftp_delivery.Cancelled):
            ftp_saves.copy(self.address, UT, scan, current, overwrite=True, resume=stopped,
                progress=seen.append, cancelled=lambda: bool(seen))
        self.assertEqual(self.live(), current.files)
        self.assertEqual({p.name: p.read_bytes() for p in backup.iterdir()}, self.old)
        self.assertFalse(list(self.saves.glob('*.save-part')))
        self.assertFalse(list(self.saves.glob('*.builder-part')))

    def test_command_line_retry_finishes_and_keeps_the_first_backup(self):
        scan, expected = self.prepare()
        with self.assertRaises(ftp_saves.IncompleteImport) as caught:
            ftp_saves.copy(self.address, UT, scan, expected, overwrite=True,
                           factory=self.failing('delete', 'undertale.ini', 'after'))
        first = self.card/caught.exception.backup.lstrip('/')
        result = ftp_saves.copy(self.address, UT, scan, ftp_saves.inspect(self.address, UT), overwrite=True)
        self.assertEqual(self.live(), self.new)
        self.assertNotEqual(result.backup_dir, caught.exception.backup)
        self.assertEqual({p.name: p.read_bytes() for p in first.iterdir()}, self.old)

    def test_a_changed_card_is_not_resumed(self):
        scan, expected = self.prepare()
        with self.assertRaises(ftp_saves.IncompleteImport) as caught:
            ftp_saves.copy(self.address, UT, scan, expected, overwrite=True,
                           factory=self.failing('rename', 'undertale.ini', 'before'))
        (self.saves/'file9').write_bytes(b'PLAYED\n3\n0\n')  # The player played on the 3DS in between.
        with self.assertRaises(ftp_saves.SavesChanged):
            ftp_saves.copy(self.address, UT, scan, expected, overwrite=True, resume=caught.exception)
        self.assertEqual((self.saves/'file9').read_bytes(), b'PLAYED\n3\n0\n')

    def test_a_missing_backup_is_not_trusted_for_a_resume(self):
        scan, expected = self.prepare()
        with self.assertRaises(ftp_saves.IncompleteImport) as caught:
            ftp_saves.copy(self.address, UT, scan, expected, overwrite=True,
                           factory=self.failing('rename', 'undertale.ini', 'before'))
        backup = self.card/caught.exception.backup.lstrip('/')
        (backup/'file9').unlink()
        fresh = ftp_saves.inspect(self.address, UT)
        result = ftp_saves.copy(self.address, UT, scan, fresh, overwrite=True, resume=caught.exception)
        self.assertEqual(self.live(), self.new)
        self.assertNotEqual(result.backup_dir, caught.exception.backup)  # A new backup of the card as it was.


class AddedSaveStopTests(SaveFixture):
    """R171: the PC brings a save the card didn't have (file9), and the card spells FILE0 in capitals. The undo the
    message gives must restore exactly the old set (F1), and a second stop must say what each file holds (F2)."""
    old = {'FILE0': b'OLD\n1\n0\n', 'undertale.ini': b'[General]\nName="OLD"\nLove="1"\n'}
    new = {'file0': b'NEW\n2\n0\n', 'file9': b'NEW\n2\n0\n', 'undertale.ini': b'[General]\nName="NEW"\nLove="2"\n'}

    prepare, failing, backups = StoppedPublishTests.prepare, StoppedPublishTests.failing, StoppedPublishTests.backups

    def folded(self, files):
        return {name.casefold(): data for name, data in files.items()}

    def check_report(self, caught):
        exc = caught.exception
        self.assertIsInstance(exc, ftp_saves.IncompleteImport)
        live = self.folded(self.live())
        for name, state in exc.states.items():
            expect = {'new': self.folded(self.new).get(name.casefold()),
                      'old': self.folded(self.old).get(name.casefold())}.get(state)
            if state in ('new', 'old'):
                self.assertEqual(live.get(name.casefold()), expect, (name, state))
            elif state in ('waiting', 'removed'):
                self.assertNotIn(name.casefold(), live, (name, state))
        return exc

    def undo(self, text):
        """Do exactly what the message says, the way a player would with a file manager on the FAT card."""
        import re
        def drop(folder, name):
            for p in folder.iterdir():
                if p.is_file() and p.name.casefold() == name.casefold():
                    p.unlink()
        copied = re.search(r'^To put the old saves back instead, copy every file from (\S+) into (\S+), replacing '
                           r'what is there\.$', text, re.M)
        if copied:
            backup, folder = (self.card/part.lstrip('/') for part in copied.groups())
            for p in backup.iterdir():
                drop(folder, p.name)
                (folder/p.name).write_bytes(p.read_bytes())
        deleted = re.search(r'^(?:Then delete|To go back to no saves instead, delete) (.+) from (\S+)\.$', text, re.M)
        if deleted:
            for name in deleted.group(1).split(', '):
                drop(self.card/deleted.group(2).lstrip('/'), name)
        return {p.name.casefold(): p.read_bytes() for p in self.saves.iterdir()
                if p.is_file() and not p.name.endswith('.save-part')}

    def test_the_undo_in_the_message_restores_the_old_saves_exactly(self):
        steps = [(verb, name, when) for name in ('file0', 'file9', 'undertale.ini') for verb in ('delete', 'rename')
                 for when in ('before', 'after')]
        for verb, name, when in steps:
            if verb == 'delete' and name == 'file9':
                continue  # Not on the card: nothing to delete.
            for wizard in (True, False):
                with self.subTest(verb=verb, name=name, when=when, wizard=wizard):
                    for p in list(self.saves.rglob('*'))[::-1]:
                        p.rmdir() if p.is_dir() else p.unlink()
                    scan, expected = self.prepare()
                    with self.assertRaises(ftp_saves.IncompleteImport) as caught:
                        ftp_saves.copy(self.address, UT, scan, expected, overwrite=True,
                                       factory=self.failing(verb, name, when))
                    exc = self.check_report(caught)
                    text = exc.describe(wizard=wizard)
                    self.assertEqual(self.undo(text), self.folded(self.old), text)

    def test_proven_empty_original_has_accurate_no_saves_and_undo_advice(self):
        self.old = {}
        scan, expected = self.prepare()
        with self.assertRaises(ftp_saves.IncompleteImport) as caught:
            ftp_saves.copy(self.address, UT, scan, expected, overwrite=False,
                           factory=self.failing('rename', 'undertale.ini', 'before'))
        text = ftp_saves.error_text(caught.exception)
        self.assertIn('There were no 3DS saves before this import.', text)
        self.assertIn('To go back to no saves instead, delete file0, file9, undertale.ini from /%s.' % UT.sd_save_dir(), text)
        self.assertEqual(self.undo(text), {})

    def test_a_second_stop_reports_saves_the_first_attempt_already_replaced(self):
        scan, expected = self.prepare()
        with self.assertRaises(ftp_saves.IncompleteImport) as first:
            ftp_saves.copy(self.address, UT, scan, expected, overwrite=True,
                           factory=self.failing('rename', 'undertale.ini', 'before'))
        self.check_report(first)
        self.assertEqual(first.exception.states, {'file0': 'new', 'file9': 'new', 'undertale.ini': 'unsure'})
        # Next, and this time the first delete is refused before the card acts.
        with self.assertRaises(ftp_saves.IncompleteImport) as second:
            ftp_saves.copy(self.address, UT, scan, ftp_saves.inspect(self.address, UT), overwrite=True,
                           factory=self.failing('delete', 'file0', 'before'), resume=first.exception)
        exc = self.check_report(second)
        self.assertEqual(exc.backup, first.exception.backup)
        self.assertEqual(exc.states, {'file0': 'unsure', 'file9': 'new', 'undertale.ini': 'waiting'})
        text = ftp_saves.error_text(exc)
        self.assertIn('Now the PC save: file9.', text)
        self.assertNotIn('Still the old 3DS save', text)
        self.assertIn('Then delete file9 from /%s.' % UT.sd_save_dir(), text)
        self.assertEqual(self.undo(text), self.folded(self.old))

    def test_two_stops_then_next_finishes_with_the_first_backup(self):
        scan, expected = self.prepare()
        with self.assertRaises(ftp_saves.IncompleteImport) as first:
            ftp_saves.copy(self.address, UT, scan, expected, overwrite=True,
                           factory=self.failing('rename', 'file9', 'after'))
        with self.assertRaises(ftp_saves.IncompleteImport) as second:
            ftp_saves.copy(self.address, UT, scan, ftp_saves.inspect(self.address, UT), overwrite=True,
                           factory=self.failing('rename', 'undertale.ini', 'before'), resume=first.exception)
        self.check_report(second)
        self.assertEqual(second.exception.states['file0'], 'new')
        result = ftp_saves.copy(self.address, UT, scan, ftp_saves.inspect(self.address, UT), overwrite=True,
                                resume=second.exception)
        self.assertEqual(self.folded(self.live()), self.folded(self.new))
        self.assertEqual(result.backup_dir, first.exception.backup)
        self.assertEqual(len(self.backups()), 1)
        backup = self.card/first.exception.backup.lstrip('/')
        self.assertEqual({p.name: p.read_bytes() for p in backup.iterdir()}, self.old)


class WizardResumeTests(unittest.TestCase):
    def test_next_after_a_stopped_import_hands_it_back_and_success_forgets_it(self):
        import threading
        from types import SimpleNamespace
        from unittest import mock
        import gui
        stopped = ftp_saves.IncompleteImport('451 fixture', '/3ds/x/saves', '/3ds/x/saves/backup-1',
                                             ftp_saves.Snapshot({'file0': b'OLD\n1\n0\n'}, 'OLD'), [], {})
        calls = []
        def copy(*args, **kwargs):
            calls.append(kwargs.get('resume'))
            if len(calls) == 1:
                raise stopped
            return saves.SaveCopyResult(1, 8, '/3ds/x/saves/backup-1', ['file0'], 1)
        widget = mock.Mock()
        scan = SimpleNamespace(has_corrupt=False, valid_files=[object()], player_summary='PC')
        app = mock.Mock(index=7)
        app.state = gui.State('undertale')
        app.state.save_scan, app.state.ftp_console = scan, '127.0.0.1:5000'
        app.state.deliver_how = 'wifi'
        app.opts.confirm_overwrite = lambda *args: True
        app.work = lambda job, after, errors=False: after(_run(job))
        page = gui.SavesPage.__new__(gui.SavesPage)
        page.app = app
        page.ftp_snapshot = ftp_saves.Snapshot({'file0': b'OLD\n1\n0\n'}, 'OLD')
        page.ftp_running, page.ftp_resume, page.ftp_cancel = False, None, threading.Event()
        page.status = page.ftp_cancel_button = page.scan_button = page.choose_button = widget
        page.ftp_job, page.ftp_importing = 0, False
        page.copy_controls = lambda: (widget,)
        page.bar = page.card = widget
        page.choice = mock.Mock()
        page.choice.get.return_value = 'wifi'
        page.addr = mock.Mock()
        page.addr.get.return_value = '127.0.0.1:5000'
        with mock.patch.object(ftp_saves, 'copy', side_effect=copy):
            page.import_ftp()
            self.assertIs(page.ftp_resume, stopped)
            widget.config.assert_called_with(text=stopped.describe(wizard=True))
            page.import_ftp()
        self.assertEqual(calls, [None, stopped])
        self.assertIsNone(page.ftp_resume)
        self.assertIs(app.state.current_save_result(), app.state.save_result)
        self.assertEqual(app.state.save_destination, '127.0.0.1:5000')


class SavesPageJobTests(unittest.TestCase):
    """DEEP-03: Scan again (or Choose folder) during a Wi-Fi import must not release the page or start other jobs,
    and an old job's completion must never release a newer one."""

    def setUp(self):
        import threading
        from types import SimpleNamespace
        from unittest import mock
        import gui
        self.gui, self.mock = gui, mock
        self.jobs = []
        state = gui.State('undertale')
        state.deliver_how, state.ftp_console, state.sd_root = 'wifi', '127.0.0.1:5000', None
        app = SimpleNamespace(state=state, index=7, update_nav=lambda: None, show=mock.Mock(),
                              opts=SimpleNamespace(probes=SimpleNamespace(pc_save_dir=lambda game: '/fixture-pc'),
                                                   confirm_overwrite=lambda *args: True))
        app.work = lambda job, done, errors=False: self.jobs.append((job, done))
        app.root = mock.Mock()
        self.scan_result = SimpleNamespace(has_corrupt=False, valid_files=[object()], files=[object()],
                                           corrupt_files=[], player_summary='PC', folder='/fixture-pc', exists=True,
                                           error=None)
        self.scan_result.files = []
        state.save_scan = self.scan_result
        page = gui.SavesPage.__new__(gui.SavesPage)
        page.app = app
        page.custom_dir, page.ftp_resume, page.ftp_running, page.ftp_importing = None, None, False, False
        page.ftp_job, page.ftp_cancel = 0, threading.Event()
        page.choice = mock.Mock()
        page.choice.get.return_value = 'wifi'
        page.last_method, page.completed_key, page.card_roots = 'wifi', None, {}
        for name in ('status', 'ftp_cancel_button', 'folder_label', 'notice', 'copy_radio', 'scan_button',
                     'choose_button', 'tree', 'card', 'addr', 'sd', 'wifi', 'bar', 'wifi_radio',
                     'skip_radio', 'card_button', 'refresh_button', 'check_button'):
            setattr(page, name, mock.Mock())
        page.addr.get.return_value = '127.0.0.1:5000'
        page.card.get.return_value = ''
        page.tree.get_children.return_value = []
        self.page = page
        self.old = ftp_saves.Snapshot({'file0': b'OLD\n1\n0\n'}, 'OLD')

    def scan(self):
        with self.mock.patch.object(self.gui.deliver, 'scan_saves', return_value=self.scan_result):
            self.page.scan()

    def test_scan_and_choose_during_an_import_are_refused_and_the_page_stays_busy(self):
        self.page.ftp_snapshot = self.old
        self.page.import_ftp()
        self.assertEqual(len(self.jobs), 1)
        self.assertTrue(self.page.busy())
        self.page.scan_button.state.assert_called_with(['disabled'])
        self.scan()
        with self.mock.patch.object(self.gui.filedialog, 'askdirectory') as ask:
            self.page.choose()
        ask.assert_not_called()
        self.page.import_ftp()  # A second import while the first runs.
        self.assertEqual(len(self.jobs), 1)
        self.assertTrue(self.page.busy())
        self.assertIs(self.page.ftp_snapshot, self.old)
        self.page.status.config.assert_called_with(text='Wait for the copy to finish, or click Cancel.')
        result = saves.SaveCopyResult(1, 8, None, ['file0'], 1)
        self.jobs[0][1](result)
        self.assertFalse(self.page.busy())
        self.page.scan_button.state.assert_called_with(['!disabled'])
        self.page.app.show.assert_not_called()
        self.assertIn('Copied 1 save to the 3DS over Wi-Fi', self.page.status.config.call_args.kwargs['text'])

    def test_an_older_check_finishing_late_is_ignored(self):
        self.scan()
        first_cancel = self.page.ftp_cancel
        self.scan()
        self.assertTrue(first_cancel.is_set())  # The superseded check is told to stop.
        self.assertFalse(self.page.ftp_cancel.is_set())
        self.jobs[0][1](ftp_saves.Snapshot({'file0': b'STALE'}, 'STALE'))
        self.assertTrue(self.page.busy())
        self.assertIsNone(self.page.ftp_snapshot)
        self.jobs[1][1](self.old)
        self.assertFalse(self.page.busy())
        self.assertIs(self.page.ftp_snapshot, self.old)

    def test_cancel_reaches_the_running_import_only(self):
        self.scan()
        self.jobs[0][1](self.old)
        self.page.import_ftp()
        self.assertFalse(self.page.ftp_cancel.is_set())
        self.page.ftp_cancel.set()  # What the Cancel button does: it always reads the current job's flag.
        self.scan()  # Scan again must not clear the Cancel the player just pressed.
        with self.mock.patch.object(ftp_saves, 'copy', side_effect=lambda *a, **k: k['cancelled']()) as copy:
            self.assertTrue(self.jobs[1][0]())
        copy.assert_called_once()


def _run(job):
    try:
        return job()
    except Exception as exc:
        return exc


if __name__ == '__main__':
    unittest.main()
