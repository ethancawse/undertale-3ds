"""Operand-only routing and fail-closed caller inventories; no hardware I/O."""
from pathlib import Path
import importlib.util
import itertools
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('dimlight_compile', Path(__file__).with_name('dimlight_compile.py'))
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


def fixture(source, target=None):
    old, new, callers = adapter.ROUTES[source]
    target = target or old
    pieces = ['.section .text.late.screens,"ax"\nunchanged_before:\n\t.word 23\n']
    for caller, count in callers.items():
        pieces.append(f'{caller}:\n\tmov r0, #1\n')
        pieces.extend(f'\t{op}\t{target}(PLT)\n' for op in ('bl', 'blne', 'b')[:count])
        pieces.append(f'\tbx lr\n\t.size\t{caller}, .-{caller}\n')
    pieces.append(f'unrelated:\n\tbl {target}\n\t.word 99\n')
    return ''.join(pieces)


class DimlightCompile(unittest.TestCase):
    def test_adapter_writes_lf_assembly_through_the_real_entry(self):
        # Cover the exported compiler path, including Python-3.9-compatible I/O,
        # rather than testing only the pure operand function.
        original = fixture('ctr_rosalina.c') + '// UTF-8 comment: \u00e9\n'
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'watcher.o'
            calls = []
            def compiler(command):
                calls.append(command)
                if '-S' in command:
                    Path(command[command.index('-o')+1]).write_bytes(original.encode('utf-8'))
                else:
                    emitted = output.with_suffix('.dimlight1.s').read_bytes()
                    self.assertNotIn(b'\r', emitted)
                    self.assertIn('// UTF-8 comment: \u00e9\n'.encode('utf-8'), emitted)
                    self.assertEqual(emitted.count(b'CtrDimlight_watcherPower'), 3)
                    self.assertIn('-Wa,--fatal-warnings', command)
                    self.assertEqual(command[-2:], ['-o', str(output)])
                return 0
            with patch.object(adapter, 'run', side_effect=compiler):
                status = adapter.main(['delegate', adapter.MARKER, 'gcc', '-DENABLE_WAD16',
                                       '-c', 'ctr_rosalina.c', '-o', str(output)])
            self.assertEqual(status, 0)
            self.assertEqual(len(calls), 2)
            self.assertTrue(output.with_suffix('.dimlight.json').is_file())

    def test_backlight_complete_chunks_keep_original_order(self):
        prefix, suffix = '.file "unchanged"\n', '\n.section .rodata\n.word 0x1234\n'
        chunks = {name: (f'\n.section .text.late.menupause,"ax",%progbits\n'
                        f'{name}:\n\tmov r0, #{i}\n\tbl target_{i}\n\t.word local_{i}\n'
                        f'\t.size {name}, .-{name}\n')
                  for i, name in enumerate(adapter.BACKLIGHT_ORDER)}
        for order in itertools.permutations(adapter.BACKLIGHT_ORDER):
            original = prefix + ''.join(chunks[n] for n in order) + suffix
            restored, receipt = adapter.order_backlights(original)
            self.assertEqual(receipt['original_order'], list(order))
            self.assertTrue(restored.startswith(prefix) and restored.endswith(suffix))
            for name, chunk in chunks.items():
                self.assertEqual(restored.count(chunk.strip()), 1)
            positions = [restored.index(n+':') for n in adapter.BACKLIGHT_ORDER]
            self.assertEqual(positions, sorted(positions))
        for broken in (original.replace(adapter.BACKLIGHT_ORDER[0]+':', 'missing:'),
                       original + chunks[adapter.BACKLIGHT_ORDER[0]],
                       original.replace('.text.late.menupause', '.text.wrong', 1)):
            with self.assertRaises(AssertionError):
                adapter.order_backlights(broken)

    def test_outer_adapter_preserves_renderer_entry(self):
        command = ['gcc', '-DENABLE_WAD16', '-c', 'source/ctr_renderer.c', '-o', 'renderer.o']
        with patch.object(adapter, 'run', return_value=0) as run:
            self.assertEqual(adapter.main(['delegate', adapter.MARKER, *command]), 0)
        run.assert_called_once_with(['delegate', *command])

    def test_existing_picture_adapter_and_lcd_both_run(self):
        path = Path(__file__).resolve().parents[1] / 'analysis/utwindow_compile.py'
        spec = importlib.util.spec_from_file_location('utwindow_compile', path)
        window = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(window)
        picture = ('CtrRenderer_endFrame.part.0:\n' + '\tbl blitApp\n'*6 +
                   '\t.size CtrRenderer_endFrame.part.0, .-CtrRenderer_endFrame.part.0\n')
        original = picture + fixture('ctr_renderer.c')
        first, count = window.route(original)
        self.assertEqual(count, 6)
        final, receipt = window.route_lcd(first)
        self.assertEqual(receipt['changed_operands'], 4)
        self.assertEqual(final.count('CtrUtWindow_blitApp'), 6)
        self.assertEqual(final.count('CtrDimlight_topBacklight'), 4)
        with self.assertRaises(AssertionError):
            window.route_lcd(picture)  # bypassed/missing original LCD callers

    def test_only_expected_operands_change(self):
        for source in adapter.ROUTES:
            old, new, callers = adapter.ROUTES[source]
            for target in (old, old+'.isra.0'):
                with self.subTest(source=source, target=target):
                    original = fixture(source, target)
                    routed, receipt = adapter.route(original, source)
                    self.assertEqual(receipt['changed_operands'], sum(callers.values()))
                    before, after = original.splitlines(), routed.splitlines()
                    self.assertEqual(len(before), len(after))
                    changed = [(a, b) for a, b in zip(before, after) if a != b]
                    self.assertEqual(len(changed), sum(callers.values()))
                    self.assertTrue(all(a.replace(target, new) == b for a, b in changed))
                    self.assertTrue(routed.endswith(f'unrelated:\n\tbl {target}\n\t.word 99\n'))

    def test_inventory_mutants_fail(self):
        for source in adapter.ROUTES:
            original = fixture(source)
            old, _, callers = adapter.ROUTES[source]
            caller = next(iter(callers))
            mutants = (
                original.replace(f'\tbl\t{old}(PLT)\n', '', 1),
                original.replace(f'\tbl\t{old}(PLT)\n', f'\tbl\t{old}(PLT)\n'*2, 1),
                original.replace(caller+':\n', 'missing_caller:\n', 1),
                original + original,
                original.replace(f'\tbl\t{old}(PLT)', '\tbl different_target', 1),
            )
            for mutant in mutants:
                with self.subTest(source=source):
                    with self.assertRaises(AssertionError):
                        adapter.route(mutant, source)


if __name__ == '__main__':
    unittest.main()
