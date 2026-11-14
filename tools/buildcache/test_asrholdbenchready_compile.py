"""Fail-closed assembly routing: old slots stay byte-for-byte reconstructible."""
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location('benchready_adapter', Path(__file__).with_name('asrholdbenchready_compile.py'))
ADAPTER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ADAPTER)

ASSEMBLY = '''.section .text.beginMeasuring,"ax",%progbits
beginMeasuring:
    push {r4, lr}
    bl CtrBench_paceOpen
    bl consoleSelect(PLT)
    bl consoleClear(PLT)
    b CtrLog_endConsole(PLT)
    .size beginMeasuring, .-beginMeasuring
.section .text.late.benchromfs.prewindow,"ax",%progbits
benchPreTick:
    bl BenchScript_tick(PLT)
    b BenchScript_tick
    .size benchPreTick, .-benchPreTick
.section .text.other,"ax",%progbits
other:
    bl consoleClear(PLT)
    .size other, .-other
'''


class RoutingTests(unittest.TestCase):
    def test_four_calls_only_and_unrelated_console_caller_kept(self):
        changed, sites = ADAPTER.rewrite(ASSEMBLY)
        self.assertEqual(sum(s['count'] for s in sites), 4)
        self.assertIn('    bl CtrBench_paceOpen\n', changed)
        self.assertIn('other:\n    bl consoleClear(PLT)\n', changed)
        restored = changed
        for caller in ADAPTER.ROUTES.values():
            for old, (new, _) in caller.items():
                restored = restored.replace(new, old)
        self.assertEqual(restored, ASSEMBLY)

    def test_missing_opening_clear_rejected(self):
        with self.assertRaises(AssertionError):
            ADAPTER.rewrite(ASSEMBLY.replace('    bl consoleClear(PLT)\n', '', 1))

    def test_extra_tick_target_rejected(self):
        with self.assertRaises(AssertionError):
            ADAPTER.rewrite(ASSEMBLY.replace('benchPreTick:\n', 'benchPreTick:\n    bl BenchScript_tick\n'))

    def test_missing_caller_rejected(self):
        with self.assertRaises(AssertionError):
            ADAPTER.rewrite(ASSEMBLY.replace('beginMeasuring:', 'unrelated:'))

    def test_data_or_bss_not_imported_and_local_labels_unique(self):
        names = ('CtrBench_readySuppressStatus', 'CtrBench_readySkipSelect',
                 'CtrBench_readySkipClear', 'CtrBench_readyScriptTick')
        helper = '.section .data\nold_data: .word 5\n'
        helper += '.section .text_asrholdbenchready,"ax",%progbits\n'
        helper += '\n'.join(name+':\n    bx lr' for name in names)+'\n'
        helper += '.L1:\n    .word .L1\n.section .bss\nold_state: .space 8\n'
        imported = ADAPTER.late_section(helper)
        self.assertNotIn('old_data', imported)
        self.assertNotIn('old_state', imported)
        self.assertIn('.LBenchReady_1:', imported)
        with self.assertRaises(AssertionError):
            ADAPTER.late_section(helper.replace('.word .L1', '.word .L99'))


if __name__ == '__main__':
    unittest.main()
