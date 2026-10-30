"""Synthetic and negative fixtures for the approved bounded pool adapter."""
from pathlib import Path
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import colexact_compile as adapter


def group(label, word):
    return '\t.section\t.rodata.str1.4\n\t.align\t2\n.LC'+str(label)+':\n\t.ascii\t"'+word+r'\000"'+'\n'


CODE = '\t.section\t.text.fixture,"ax",%progbits\nfixture:\n\tbx lr\n\t.word .LC11\n'
FIRST = group(11, 'distance_to_point')
SECOND = group(12, 'distance_to_object')
ANCHOR = group(13, 'ini_section_exists')
LAST = group(14, 'unchanged_string')
CANONICAL = FIRST+SECOND+ANCHOR+CODE+LAST
DIGEST = adapter.fingerprint(['"'+x+r'\000"' for x in (*adapter.KEYS, adapter.ANCHOR, 'unchanged_string')])


class PoolTest(unittest.TestCase):
    def restore(self, text):
        return adapter.restore_pool(text, DIGEST, 4)

    def test_moves_only_complete_groups_and_preserves_labels_and_code(self):
        actual, receipt = self.restore(ANCHOR+CODE+FIRST+SECOND+LAST)
        self.assertEqual(actual, CANONICAL)
        self.assertTrue(receipt['changed'])
        self.assertEqual(receipt['labels']['distance_to_point'], '.LC11')

    def test_idempotent(self):
        actual, receipt = self.restore(CANONICAL)
        self.assertEqual(actual, CANONICAL)
        self.assertFalse(receipt['changed'])

    def test_each_missing_anchor_fails(self):
        for block in (FIRST, SECOND, ANCHOR):
            with self.subTest(block=block), self.assertRaisesRegex(AssertionError, 'missing/duplicate'):
                self.restore(CANONICAL.replace(block, ''))

    def test_duplicate_fails(self):
        with self.assertRaisesRegex(AssertionError, 'missing/duplicate'):
            self.restore(CANONICAL+FIRST)

    def test_foreign_directive_in_moved_group_fails(self):
        with self.assertRaisesRegex(AssertionError, 'unqualified'):
            self.restore(CANONICAL.replace(FIRST, FIRST+'\t.word 123\n'))

    def test_other_string_change_fails(self):
        with self.assertRaisesRegex(AssertionError, 'unexpected canonical'):
            self.restore(CANONICAL.replace('unchanged_string', 'changed_string'))

    def test_other_pool_order_change_fails(self):
        with self.assertRaisesRegex(AssertionError, 'unexpected canonical'):
            self.restore(LAST+FIRST+SECOND+ANCHOR+CODE)

    def test_guard_and_delegate(self):
        args = ['delegate', adapter.MARKER, 'gcc', '-c', '/engine/src/vm_builtins.c', '-o', 'vm.o',
            '-DENABLE_WAD16', '-DCTRPORT_BBOX_ROUND', '-DCTRPORT_LAYERFIX_RELEASE_LAYOUT']
        self.assertTrue(adapter.selected(args[2:]))
        for flag in ('-DENABLE_WAD16', '-DCTRPORT_BBOX_ROUND', '-DCTRPORT_LAYERFIX_RELEASE_LAYOUT'):
            with self.subTest(flag=flag), mock.patch.object(adapter, 'run', return_value=7) as run:
                command = [a for a in args if a != flag]
                self.assertEqual(adapter.main(command), 7)
                run.assert_called_once_with(['delegate']+command[2:])


if __name__ == '__main__':
    unittest.main()
