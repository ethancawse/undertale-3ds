"""First paint with invented settings and no installed game, on the executing host."""
from contextlib import ExitStack
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0,str(Path(__file__).resolve().parent/'builder'))
import gui
from gui_test_support import show_without_desktop_input


class FirstPaintTests(unittest.TestCase):
    def setUp(self):
        try:
            self.root=gui.tk.Tk()
            self.root.withdraw()
        except gui.tk.TclError as error:
            from gui_test_support import tk_unavailable
            tk_unavailable(error)

    def tearDown(self):
        if hasattr(self,'root'):
            for token in self.root.tk.call('after','info'):
                self.root.after_cancel(token)
            self.root.destroy()

    def check_start(self,mode):
        with tempfile.TemporaryDirectory() as tmp, ExitStack() as stack:
            work=Path(tmp)/'no-settings-yet'
            boring=mode!='default'
            if mode=='saved':gui.preferences.save(str(work),True)
            else:self.assertFalse(work.exists())
            seen=[]
            def watch(original):
                def construct(page,app):
                    self.assertEqual(app.theme.native,boring)
                    self.assertEqual(gui.ttk.Style(app.root).theme_use(),
                        app.theme.native_theme if boring else 'builder-fun')
                    seen.append(type(page).__name__)
                    original(page,app)
                return construct
            stack.enter_context(mock.patch.object(gui.discover,'find_copies',return_value=[]))
            stack.enter_context(mock.patch.object(gui.Theme,'load_from_game_dir'))
            for page in gui.App.PAGES:
                stack.enter_context(mock.patch.object(page,'__init__',watch(page.__init__)))
            app=gui.App(self.root,gui.Options(game='undertale',work=str(work),
                boring=True if mode=='explicit' else None))
            self.assertEqual(seen,[page.__name__ for page in gui.App.PAGES])
            self.assertEqual(app.boring.get(),boring)
            # Before Tk processes its first idle/frame, the actual heading and button renderer match the setting.
            self.assertEqual(str(app.next_btn.cget('style')),'TButton' if boring else 'Ut.TButton')
            self.assertEqual(bool(str(app.heading.cget('image'))),not boring)
            if not boring:self.assertEqual(self.root.cget('background'),'#000000')
            app.work=mock.Mock()  # Never start discovery, a build, a transfer or an update request.
            for value in (boring,not boring,boring):
                app.boring.set(value)
                app.toggle_boring()
                self.root.update_idletasks()
                self.assertEqual(app.theme.native,value)
                self.assertEqual(str(app.next_btn.cget('style')),'TButton' if value else 'Ut.TButton')
                for page in app.pages:
                    self.assertIs(page.frame._ut_theme,app.theme)
                    self.assertEqual(str(page.box.cget('highlightthickness')),'0' if value else '3')
            show_without_desktop_input(self,self.root)
            self.root.update_idletasks()
            self.assertEqual(bool(str(app.heading.cget('image'))),not boring)

    def test_a_tk_without_the_shift_tab_key_name_still_starts(self):
        # Tk 8.5 off X11 (Apple's own Python) refuses <ISO_Left_Tab>; Shift+Tab is Tab with Shift held there.
        bind_all=self.root.bind_all
        def old_tk(sequence=None,func=None,add=None):
            if sequence is not None and 'ISO_Left_Tab' in sequence:
                raise gui.tk.TclError('bad event type or keysym "ISO_Left_Tab"')
            return bind_all(sequence,func,add)
        with tempfile.TemporaryDirectory() as tmp, ExitStack() as stack:
            stack.enter_context(mock.patch.object(gui.discover,'find_copies',return_value=[]))
            stack.enter_context(mock.patch.object(gui.Theme,'load_from_game_dir'))
            stack.enter_context(mock.patch.object(self.root,'bind_all',old_tk))
            app=gui.App(self.root,gui.Options(game='undertale',work=str(Path(tmp)/'work')))
            app.work=mock.Mock()  # Never start discovery, a build, a transfer or an update request.
            self.assertEqual(app.page.title,'Welcome')
        self.assertIn('<KeyRelease-Tab>',bind_all())

    def test_an_old_tk_is_refused_with_instructions_before_any_window(self):
        import builder
        with mock.patch.object(gui.tk,'TkVersion',8.5), mock.patch.object(gui,'run') as run, \
                mock.patch('sys.stderr',new=io.StringIO()) as said:
            self.assertEqual(builder.main(['builder.py','--game','undertale']),2)
        run.assert_not_called()
        self.assertIn('needs Tk 8.6 or newer',said.getvalue())
        self.assertIn('--cli',said.getvalue())

    def test_no_settings_is_themed_before_every_page_builds(self):
        self.check_start('default')

    def test_explicit_boring_is_plain_from_first_frame(self):
        self.check_start('explicit')

    def test_saved_boring_is_plain_from_first_frame(self):
        self.check_start('saved')


if __name__=='__main__':unittest.main()
