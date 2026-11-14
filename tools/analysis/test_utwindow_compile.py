"""Fail-closed scope checks for the window-motion call router."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parent))
import utwindow_compile


def fixture(count=6, name='CtrRenderer_endFrame.part.0'):
    return ('presentBottom:\n\tbl\tblitApp\n\t.size presentBottom, .-presentBottom\n' +
            name + ':\n' + '\tbl\tblitApp\n' * count + '\t.word blitApp\n\t.size ' + name + ', .-' + name + '\n' +
            'other:\n\tbl\tblitApp\n\t.size other, .-other\n')


class Scope(unittest.TestCase):
    def test_s4_helper_retains_tail_storage_without_ordinary_bss(self):
        before = ('\t.section .aacout.text.dlgtop4,"ax"\nhelper:\n\tbx lr\n'
                  '\t.type dlgtopLease, %object\n'
                  '\t.section .bss.dlgtop4,"aw",%nobits\n'
                  'dlgtopLease:\n\t.space 16\n'
                  '\t.section .bss.late.unrelated,"aw",%nobits\n'
                  'unrelated:\n\t.space 64\n')
        after, sections = utwindow_compile.vine_extract.late_sections(before, 'dlgtop4')
        self.assertEqual(sections, ['.aacout.text.dlgtop4', '.bss.dlgtop4'])
        self.assertIn('dlgtopLease:\n\t.space 16', after)
        self.assertIn('\t.type dlgtopLease, %object', after)
        self.assertNotIn('unrelated', after)

    def test_s4_retargets_only_final_presentation_and_display_calls(self):
        for count, development, display_count in ((1, True, 1), (2, False, 3)):
            display = ('__wrap_CtrRenderer_setDisplay:\n' + '\tbl CtrDimlight_setDisplay\n' * display_count +
                       '\t.size __wrap_CtrRenderer_setDisplay, .-__wrap_CtrRenderer_setDisplay\n')
            viewport = 'ctrBeginFrame:\n\tbl setViewport\n\t.size ctrBeginFrame, .-ctrBeginFrame\n'
            destroy = 'ctrDestroy:\n\tbl CtrRenderer_releaseVideo\n\t.size ctrDestroy, .-ctrDestroy\n'
            before = (destroy + viewport + 'CtrRenderer_endFrame.part.0:\n' + '\tbl presentBottom\n' * count +
                      '\t.size CtrRenderer_endFrame.part.0, .-CtrRenderer_endFrame.part.0\n' + display +
                      'other:\n\tbl presentBottom\n\tbl CtrDimlight_setDisplay\n\t.size other, .-other\n')
            after = utwindow_compile.route_top_dialogue(before, development=development)
            self.assertEqual(after.replace('CtrDlgtop_present', 'presentBottom').replace(
                'CtrDlgtop_display', 'CtrDimlight_setDisplay').replace('CtrDlgtop_viewport', 'setViewport').replace(
                'CtrDlgtop_destroy', 'CtrRenderer_releaseVideo'), before)
            self.assertEqual(after.count('bl presentBottom\n'), 1)
            for mutant in (before.replace(display, ''), before + before,
                           before.replace('bl CtrDimlight_setDisplay', 'bl unknown'),
                           before.replace('\tbl presentBottom\n' * count, '\tbl unknown\n', 1),
                           before.replace(viewport, ''), before.replace('bl setViewport', 'bl unknown'),
                           before.replace(destroy, ''), before.replace('bl CtrRenderer_releaseVideo', 'bl unknown'),
                           before.replace('bl setViewport', 'bl setViewport\n\tbl setViewport')):
                with self.assertRaises(AssertionError): utwindow_compile.route_top_dialogue(mutant, development=development)
            for wrong_count in (0, 2, 4):
                mutant = before.replace(display, display.replace('\tbl CtrDimlight_setDisplay\n' * display_count,
                    '\tbl CtrDimlight_setDisplay\n' * wrong_count))
                with self.assertRaises(AssertionError): utwindow_compile.route_top_dialogue(mutant, development=development)

    def test_dialogue_public_routes_yield_only_and_diagnostics_select_white_query(self):
        before=('__wrap_CtrRenderer_yieldBottomScreen:\n\tbl CtrScreens_yieldHook\n'
                '\t.size __wrap_CtrRenderer_yieldBottomScreen, .-__wrap_CtrRenderer_yieldBottomScreen\n'
                'emitQuad:\n\tbl recordingSurface\n\tbl CtrDlgwhite_solidDrawIsNoop\n\tbl recordingSurface\n'
                '\t.size emitQuad, .-emitQuad\n'
                'ctrDestroy:\n\tbl CtrDialogue_free\n\t.size ctrDestroy, .-ctrDestroy\n')
        public=utwindow_compile.route_dialogue(before)
        self.assertEqual(public.replace('CtrDlgflash_yield','CtrScreens_yieldHook'),before)
        diagnostic=utwindow_compile.route_dialogue(before,diagnostic=True)
        self.assertEqual(diagnostic.count('bl recordingSurface\n'),1)
        self.assertIn('bl CtrDlgwhite_solidDrawIsNoop\n\tbl CtrDlgflash_recordingSurface',diagnostic)
        self.assertIn('bl CtrDlgflash_free',diagnostic)
        for mutant in (before.replace('CtrScreens_yieldHook','unknown'),before+before):
            with self.assertRaises(AssertionError):utwindow_compile.route_dialogue(mutant)

    def test_changes_only_the_six_qualified_operands(self):
        original = fixture()
        routed, count = utwindow_compile.route(original)
        self.assertEqual(count, 6)
        self.assertEqual(routed.replace('CtrUtWindow_blitApp', 'blitApp'), original)
        self.assertIn('presentBottom:\n\tbl\tblitApp', routed)
        self.assertIn('other:\n\tbl\tblitApp', routed)
        self.assertIn('\t.word blitApp', routed)

    def test_rejects_changed_compiler_shape(self):
        for text in (fixture(5), fixture(7), fixture(name='CtrRenderer_endFrame'), fixture() + fixture()):
            with self.subTest(text=text), self.assertRaises(AssertionError):
                utwindow_compile.route(text)

    def test_rejects_an_already_routed_body(self):
        with self.assertRaises(AssertionError):
            utwindow_compile.route(utwindow_compile.route(fixture())[0])

    def test_development_body_keeps_the_six_qualified_routes(self):
        before = fixture(name='CtrRenderer_endFrame')
        after, count = utwindow_compile.route(before, development=True)
        self.assertEqual(count, 6)
        self.assertEqual(after.replace('CtrUtWindow_blitApp', 'blitApp'), before)
        with self.assertRaises(AssertionError):
            utwindow_compile.route(fixture(5, name='CtrRenderer_endFrame'), development=True)

    def test_uninlined_present_and_size_commoned_calls(self):
        for name, count, size in [('present', 5, False), ('CtrRenderer_endFrame', 4, True)]:
            before = fixture(count, name=name)
            after, actual = utwindow_compile.route(before, development=True, size_optimized=size)
            self.assertEqual(actual, count)
            self.assertEqual(after.replace('CtrUtWindow_blitApp', 'blitApp'), before)
            with self.assertRaises(AssertionError):
                utwindow_compile.route(fixture(count-1, name=name), development=True, size_optimized=size)


if __name__ == '__main__':
    unittest.main()
