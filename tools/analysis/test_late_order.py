"""Single late-order contract and negative controls; no compiler or game data."""
import ast
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import late_order


class LateOrderTests(unittest.TestCase):
    def test_empty_and_comment_only_aac_lists_stop_at_their_own_closing_line(self):
        for body in ('', '\n', '    # future tenants\n'):
            text = ('set(aacout_text_inputs\n' + body + ')\n'
                    'set(aacout_rodata_inputs\n    .aacout.rodata.fixture # 4 B\n)\n')
            self.assertEqual(late_order.aac_inputs(text, 'text'), [])
            self.assertEqual(late_order.aac_inputs(text, 'rodata'), ['.aacout.rodata.fixture'])

    def test_registry_matches_linker_fragment(self):
        errors = late_order.check(late_order.LD.read_text(encoding='utf-8'), late_order.load())
        errors += late_order.check_aac(late_order.AAC.read_text(encoding='utf-8'), late_order.load())
        self.assertEqual(errors, [], '\n'.join(errors))

    def test_missing_entry_reports_the_one_registry_and_exact_selector(self):
        registry = late_order.load()
        missing = registry['text'][registry['text'].index('*(.repack_test)') - 1]
        registry['text'].remove(missing)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'missing.json'
            path.write_text(json.dumps(registry), encoding='utf-8')
            result = subprocess.run([sys.executable, '-I', str(Path(late_order.__file__)),
                '--registry', str(path)], stdin=subprocess.DEVNULL, capture_output=True,
                text=True, timeout=20)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(len(result.stdout.splitlines()), 1)
        self.assertIn('FAIL: late-order.json text input', result.stdout)
        self.assertIn('linker has ' + missing, result.stdout)
        self.assertEqual(result.stderr, '')

    def test_each_region_rejects_missing_duplicate_and_reordered_entries(self):
        text, registry = late_order.LD.read_text(encoding='utf-8'), late_order.load()
        for kind in late_order.KINDS:
            for mutation in ('missing', 'duplicate', 'swapped'):
                with self.subTest(kind=kind, mutation=mutation):
                    changed = copy.deepcopy(registry)
                    entries = changed[kind]
                    if mutation == 'missing':
                        entries.pop(2)
                    elif mutation == 'duplicate':
                        entries.insert(2, entries[2])
                    else:
                        entries[2], entries[3] = entries[3], entries[2]
                    errors = late_order.check(text, changed)
                    self.assertEqual(len(errors), 1)
                    self.assertIn('late-order.json ' + kind + ' input', errors[0])

    def test_keep_wrapper_and_object_selector_are_part_of_the_contract(self):
        registry, text = late_order.load(), late_order.LD.read_text(encoding='utf-8')
        for old, new in (('KEEP(*(.text_credclip))', '*(.text_credclip)'),
                         ('*z_hitcheck.o(.text .text.* .rodata .rodata.*)',
                          '*z_other.o(.text .text.* .rodata .rodata.*)')):
            with self.subTest(old=old):
                errors = late_order.check(text.replace(old, new), registry)
                self.assertEqual(len(errors), 1)
                self.assertIn(new, errors[0])

    def test_a_future_append_needs_only_registry_and_linker_changes(self):
        registry, text = late_order.load(), late_order.LD.read_text(encoding='utf-8')
        appended = 'KEEP(*(.text_future_lane))'
        point = registry['text'].index('*(.repack_test)')
        registry['text'].insert(point, appended)
        self.assertEqual(len(late_order.check(text, registry)), 1)
        text = text.replace('    *(.repack_test)', '    ' + appended + '\n    *(.repack_test)')
        self.assertEqual(late_order.check(text, registry), [])
        with mock.patch.object(late_order, 'load', return_value=registry):
            self.assertIn(appended, late_order.inputs(start='*(.text_hplcopy)', stop='*(.repack_test)'))
            self.assertIn('.text_future_lane', late_order.inputs(start='*(.text_dlgblank)',
                stop='*(.repack_test)', view='sections'))
            self.assertNotIn(appended, late_order.inputs(view='stars'))

    def test_multiline_selectors_and_comments_do_not_change_the_order(self):
        text = '  .late.text . :\n  {\n *foo.o(.text\n .rodata) /* ignored */\n KEEP(*(.text_probe))\n  }\n'
        self.assertEqual(late_order.linker_inputs(text, 'text'),
                         ['*foo.o(.text .rodata)', 'KEEP(*(.text_probe))'])

    def test_aac_missing_duplicate_and_future_append_controls(self):
        original = late_order.AAC.read_text(encoding='utf-8')
        for kind in late_order.AAC_KINDS:
            text, registry = original, late_order.load()
            # An empty production slot is valid. Seed only this isolated
            # control with a named input so missing/duplicate mutations apply.
            if not registry[kind]:
                entry = '.aacout.' + kind[4:] + '.slot_probe'
                registry[kind] = [entry]
                point = 'set(aacout_' + kind[4:] + '_inputs\n'
                self.assertEqual(text.count(point), 1)
                text = text.replace(point, point + '    ' + entry + '\n')
            self.assertEqual(late_order.check_aac(text, registry), [])
            entry = registry[kind][0]
            for mutation in ('missing', 'duplicate', 'appended'):
                changed = copy.deepcopy(registry)
                if mutation == 'missing':
                    changed[kind].remove(entry)
                else:
                    changed[kind].append(entry if mutation == 'duplicate'
                        else '.aacout.' + kind[4:] + '.future')
                with self.subTest(kind=kind, mutation=mutation):
                    errors = late_order.check_aac(text, changed)
                    self.assertEqual(len(errors), 1)
                    self.assertIn('late-order.json ' + kind, errors[0])
                    if mutation == 'appended':
                        last = registry[kind][-1]
                        future = changed[kind][-1]
                        updated = text.replace('    ' + last + '\n', '    ' + last + '\n    ' + future + '\n')
                        self.assertEqual(late_order.check_aac(updated, changed), [])
                        swapped = updated.replace(last, 'TEMP').replace(future, last).replace('TEMP', future)
                        self.assertEqual(len(late_order.check_aac(swapped, changed)), 1)

    def test_tail_registries_are_not_copied_back_into_tests(self):
        duplicated = []
        for folder in ('analysis', 'audio', 'release'):
            for path in (late_order.ROOT / 'tools' / folder).glob('test_*.py'):
                tree = ast.parse(path.read_text(encoding='utf-8'))
                for node in ast.walk(tree):
                    if isinstance(node, (ast.List, ast.Tuple, ast.Set)):
                        selectors = [part.value for part in node.elts if isinstance(part, ast.Constant)
                            and isinstance(part.value, str) and part.value.startswith(
                                ('*(', '*z_', 'KEEP(*', '.text_', '.text.', '.rodata.', '.bss.'))]
                        if len(selectors) > 10:
                            duplicated.append('%s:%d' % (path.relative_to(late_order.ROOT), node.lineno))
        self.assertEqual(duplicated, [], 'Use late-order.json slices: ' + ', '.join(duplicated))


if __name__ == '__main__':
    unittest.main()
