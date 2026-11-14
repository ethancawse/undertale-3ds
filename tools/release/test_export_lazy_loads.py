"""Deferred-input gate unit fixtures; no compiler, game data or mounted destinations."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parent))
import export_lazy_loads as probe


class DeferredLoads(unittest.TestCase):
    def test_registry_records_owners_and_reasons(self):
        self.assertTrue(probe.LAZY_LOADS)
        for owner,target,reason in probe.LAZY_LOADS:
            with self.subTest(target=target):
                self.assertTrue(owner.endswith('.py'))
                self.assertTrue(target.endswith('.py'))
                self.assertTrue(reason.strip())

    def test_missing_or_broken_deferred_module_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            with self.assertRaises(FileNotFoundError):probe.load(root,'missing.py')
            (root/'broken.py').write_text('raise RuntimeError("invented deferred failure")\n')
            with self.assertRaisesRegex(RuntimeError,'invented deferred failure'):
                probe.load(root,'broken.py')

    def test_direct_tool_sibling_imports_and_dataclasses_work_without_path_leaks(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            (root/'invented_sibling.py').write_text('VALUE=7\n')
            (root/'entry.py').write_text('from __future__ import annotations\n'
                'from dataclasses import dataclass\nimport invented_sibling\n'
                '@dataclass\nclass Example:\n    value: int = invented_sibling.VALUE\n')
            before=sys.path[:]
            try:
                self.assertEqual(probe.load(root,'entry.py').Example().value,7)
                self.assertEqual(sys.path,before)
                self.assertNotIn('export_probe_entry',sys.modules)
            finally:
                sys.modules.pop('invented_sibling',None)

    def test_a_module_outside_the_export_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'export').mkdir()
            (root/'outside.py').write_text('raise AssertionError("must not run")\n')
            with self.assertRaisesRegex(ValueError,'outside the export'):
                probe.load(root/'export','../outside.py')
