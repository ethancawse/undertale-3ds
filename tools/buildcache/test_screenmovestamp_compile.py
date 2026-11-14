"""Reject stale coverage and changed stack/return ABIs for both move paths."""
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location('sms_adapter', Path(__file__).with_name('screenmovestamp_compile.py'))
ADAPTER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ADAPTER)
ASSEMBLY = '''.section .text.late.screens,"ax",%progbits
screensMoveGame:
    .fnstart
    push {r4, r5, r6, r7, r8, lr}
    bl CtrGpu_waitIdle
    bl CtrScreens_pacerWaitShown
    pop {r4, r5, r6, r7, r8, lr}
    mov r1,r3
    b logInfo(PLT)
    .word 123
    .size screensMoveGame,.-screensMoveGame
other:
    b logInfo
    .size other,.-other
scanoutMove:
    push {r4, r5, r6, r7, r8, r9, r10, fp, lr}
    mov r5,r1
    sub sp, sp, #20
    bl CtrGpu_waitIdle
    bne .Lsuccess
    mov r0,#0
.Lexit:
    add sp, sp, #20
    pop {r4, r5, r6, r7, r8, r9, r10, fp, pc}
.Lsuccess:
    bl scanoutLanded
    bl logInfo(PLT)
    mov r0,#1
    b .Lexit
    .size scanoutMove,.-scanoutMove
'''


class Routing(unittest.TestCase):
    def test_four_words_and_unchanged_waits_and_unrelated_log(self):
        result = ADAPTER.rewrite(ASSEMBLY)
        self.assertIn('bl CtrGpu_waitIdle\n', result)
        self.assertIn('bl CtrScreens_pacerWaitShown\n', result)
        self.assertIn('other:\n    b logInfo\n', result)
        before_trampoline = result[:-len(ADAPTER.TRAMPOLINE)]
        restored = before_trampoline.replace('b CtrScreenMoveStamp_enter',
            'push {r4, r5, r6, r7, r8, lr}').replace('b CtrScreenMoveStamp_marker', 'b logInfo')
        restored = restored.replace('b CtrScreenMoveStamp_scanoutEnter',
            'push {r4, r5, r6, r7, r8, r9, r10, fp, lr}').replace(
            'bl CtrScreenMoveStamp_scanoutMarker', 'bl logInfo')
        self.assertEqual(restored, ASSEMBLY)
        self.assertIn('b screensMoveGame+4', result)
        self.assertIn('b scanoutMove+4', result)
        self.assertIn('bl scanoutLanded\n', result)

    def test_changed_prologue_rejected(self):
        with self.assertRaises(AssertionError):
            ADAPTER.rewrite(ASSEMBLY.replace('push {r4, r5, r6, r7, r8, lr}', 'push {r4, lr}'))

    def test_non_tail_marker_rejected(self):
        with self.assertRaises(AssertionError):
            ADAPTER.rewrite(ASSEMBLY.replace('b logInfo(PLT)', 'bl logInfo(PLT)'))

    def test_unrestored_stack_rejected(self):
        with self.assertRaises(AssertionError):
            ADAPTER.rewrite(ASSEMBLY.replace('pop {r4, r5, r6, r7, r8, lr}', 'pop {r4, pc}'))

    def test_extra_stack_operation_rejected(self):
        with self.assertRaises(AssertionError):
            ADAPTER.rewrite(ASSEMBLY.replace('mov r1,r3', 'sub sp,sp,#8'))

    def test_repeated_marker_rejected(self):
        with self.assertRaises(AssertionError):
            ADAPTER.rewrite(ASSEMBLY.replace('b logInfo(PLT)', 'b logInfo(PLT)\n    b logInfo'))

    def test_repeated_application_rejected(self):
        with self.assertRaises(AssertionError):
            ADAPTER.rewrite(ADAPTER.rewrite(ASSEMBLY))

    def test_missing_active_path_rejected(self):
        with self.assertRaises(AssertionError):
            ADAPTER.rewrite(ASSEMBLY[:ASSEMBLY.index('scanoutMove:')])

    def test_changed_scanout_frame_rejected(self):
        for before, after in (('sub sp, sp, #20', 'sub sp, sp, #24'),
            ('r10, fp, lr}', 'r10, lr}'), ('add sp, sp, #20', 'add sp, sp, #24'),
            ('fp, pc}', 'fp, lr}')):
            with self.subTest(before=before), self.assertRaises(AssertionError):
                ADAPTER.rewrite(ASSEMBLY.replace(before, after))

    def test_changed_scanout_marker_or_return_rejected(self):
        for before, after in (('bl logInfo(PLT)', 'b logInfo(PLT)'),
            ('mov r0,#1', 'mov r0,#0'), ('b .Lexit', 'bx lr')):
            with self.subTest(before=before), self.assertRaises(AssertionError):
                ADAPTER.rewrite(ASSEMBLY.replace(before, after))

    def test_extra_scanout_stack_change_rejected(self):
        with self.assertRaises(AssertionError):
            ADAPTER.rewrite(ASSEMBLY.replace('bl scanoutLanded', 'sub sp,sp,#8\n    bl scanoutLanded'))

    def test_gcc_debug_epilogue_annotations_preserved(self):
        # Actual failing dv8u6 compiler shape: source/CFI directives and
        # a comment separate the target label, stack adjustment and pop.
        annotated = ASSEMBLY.replace('.Lexit:\n    add sp, sp, #20\n',
            '.Lexit:\n\t.loc 2 30051 1 view .LVU8024\n'
            '\tadd\tsp, sp, #20\n\t.cfi_remember_state\n'
            '\t.cfi_def_cfa_offset 36\n\t@ sp needed\n')
        result = ADAPTER.rewrite(annotated)
        epilogue = annotated[annotated.index('.Lexit:'):annotated.index('.Lsuccess:')]
        self.assertIn(epilogue, result)

    def test_success_cannot_bypass_or_delay_restoring_pair(self):
        variants = (
            ASSEMBLY.replace('b .Lexit', 'b .Lsuccess'),
            ASSEMBLY.replace('.Lexit:\n', '.Lexit:\n    mov r0,#0\n'),
            ASSEMBLY.replace('.Lexit:\n', '.Lexit:\n    b .Lsuccess\n'),
            ASSEMBLY.replace('.Lexit:\n', '.Lexit:\n.Lexit:\n'),
            ASSEMBLY.replace('b .Lexit', 'b .Lmissing'),
        )
        for assembly in variants:
            with self.subTest(assembly=assembly), self.assertRaises(AssertionError):
                ADAPTER.rewrite(assembly)


if __name__ == '__main__':
    unittest.main()
