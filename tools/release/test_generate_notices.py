"""Notice generation uses source texts, fails closed, and follows the export scope."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('notices', Path(__file__).with_name('generate_notices.py'))
notices = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notices)


class NoticeTests(unittest.TestCase):
    def setUp(self):
        (notices.ROOT / 'scratch').mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=notices.ROOT / 'scratch')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'engine/vendor/example').mkdir(parents=True)
        self.license = self.root / 'engine/vendor/example/LICENSE'
        self.license.write_text('Actual upstream terms\n', encoding='utf-8')
        self.manifest = self.root / 'manifest'
        self.manifest.write_text('engine/vendor/example/LICENSE\n', encoding='utf-8')
        self.catalogue = self.root / 'catalogue'
        self.catalogue.write_text(json.dumps({'components': [{
            'name': 'example', 'license': 'Example', 'paths': ['engine/vendor/example/'],
            'provenance': 'test fixture', 'texts': [{'file': 'engine/vendor/example/LICENSE'}]
        }]}), encoding='utf-8')

    def render(self):
        return notices.generate(self.root, self.manifest, self.catalogue)

    def test_source_changes_propagate_deterministically(self):
        self.assertEqual(self.render(), self.render())
        self.assertIn('Actual upstream terms', self.render())
        self.license.write_text('Changed upstream terms\n', encoding='utf-8')
        self.assertIn('Changed upstream terms', self.render())
        self.assertNotIn('Actual upstream terms', self.render())

    def test_manifest_selects_components(self):
        self.manifest.write_text('', encoding='utf-8')
        self.assertNotIn('Actual upstream terms', self.render())

    def test_unknown_vendor_is_rejected(self):
        source = self.root / 'engine/vendor/unknown.c'
        source.write_text('int x;', encoding='utf-8')
        self.manifest.write_text('engine/vendor/unknown.c\n', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'uncatalogued'):
            self.render()

    def test_missing_notice_source_is_rejected(self):
        self.catalogue.write_text(self.catalogue.read_text().replace('example/LICENSE"}', 'example/MISSING"}'))
        with self.assertRaisesRegex(ValueError, 'absent from export'):
            self.render()

    def test_traversal_is_rejected(self):
        self.manifest.write_text('../outside\n', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'unsafe'):
            self.render()

    def test_checked_in_bundle_is_current(self):
        for game in ('ut', 'dr'):
            self.assertEqual((notices.ROOT / 'THIRD_PARTY_NOTICES').read_text(encoding='utf-8'),
                             notices.generate(notices.ROOT, notices.ROOT / ('tools/release/manifest-' + game + '.txt'),
                                              notices.ROOT / 'tools/release/notices.json'))

    def test_empty_notice_and_empty_source_fail(self):
        self.license.write_text(' \n', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'empty licence'):
            self.render()
        data = json.loads(self.catalogue.read_text())
        data['components'][0]['texts'] = []
        self.catalogue.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'no licence source'):
            self.render()

    def test_file_prefix_cannot_cover_another_vendor_file(self):
        data = json.loads(self.catalogue.read_text())
        data['components'][0]['paths'] = ['engine/vendor/example/LICENSE']
        self.catalogue.write_text(json.dumps(data))
        (self.root / 'engine/vendor/example/LICENSE-extra').write_text('fixture')
        self.manifest.write_text('engine/vendor/example/LICENSE-extra\n')
        with self.assertRaisesRegex(ValueError, 'uncatalogued'):
            self.render()

    def test_linked_library_without_notice_fails_including_variable_lists(self):
        path = self.root / 'platform/3ds/CMakeLists.txt'
        path.parent.mkdir(parents=True)
        path.write_text('find_library(OGG_LIBRARY ogg)\nset(AUDIO ${OGG_LIBRARY})\n'
                        'list(APPEND AUDIO unknownlib)\ntarget_link_libraries(app PRIVATE ${AUDIO})\n')
        self.manifest.write_text(self.manifest.read_text() + 'platform/3ds/CMakeLists.txt\n')
        with self.assertRaisesRegex(ValueError, 'linked libraries without notices: .*unknownlib'):
            self.render()
        data = json.loads(self.catalogue.read_text())
        data['components'][0]['link_names'] = ['ogg', 'unknownlib', 'gcc', 'c', 'sysbase', '3dsx_crt0']
        self.catalogue.write_text(json.dumps(data))
        self.assertIn('Actual upstream terms', self.render())
        path.write_text('target_link_libraries(app PRIVATE ${MISSING})\n')
        with self.assertRaisesRegex(ValueError, 'unresolved linked-library'):
            self.render()


if __name__ == '__main__':
    unittest.main()
