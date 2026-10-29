"""Synthetic archive/policy regression tests. No owned game input is used."""
import importlib.util
import hashlib
import json
import re
import shutil
import subprocess
from pathlib import Path
import tempfile
import unittest
import zipfile
import sys
import contextlib
import io
from unittest import mock

sys.dont_write_bytecode = True

spec = importlib.util.spec_from_file_location('check_release', Path(__file__).with_name('check_release.py'))
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class ReleaseGateTests(unittest.TestCase):
    def test_product_selects_its_own_default_manifest(self):
        self.assertEqual(gate.export_product(None, None), ('tools/release/manifest-ut.txt', 'undertale'))
        for game, short in (('undertale', 'ut'), ('deltarune', 'dr')):
            manifest = 'tools/release/manifest-' + short + '.txt'
            self.assertEqual(gate.export_product(None, game), (manifest, game))
            self.assertEqual(gate.export_product(manifest, None), (manifest, game))
            self.assertEqual(gate.export_product(manifest, game), (manifest, game))
            other = 'deltarune' if game == 'undertale' else 'undertale'
            with self.assertRaisesRegex(ValueError, 'conflicts with'):
                gate.export_product(manifest, other)
            self.assertEqual(gate.export_product('custom.txt', game), ('custom.txt', game))
        with self.assertRaisesRegex(ValueError, 'custom manifest needs --game'):
            gate.export_product('custom.txt', None)

    def test_game_only_cli_checks_the_matching_manifest_closure(self):
        for game, short in (('undertale', 'ut'), ('deltarune', 'dr')):
            with self.subTest(game=game), tempfile.TemporaryDirectory(dir=gate.ROOT / 'scratch') as directory:
                root = Path(directory)
                manifest = root / ('tools/release/manifest-' + short + '.txt')
                manifest.parent.mkdir(parents=True)
                manifest.write_text('LICENSE\n')
                with mock.patch.object(gate, 'ROOT', root), \
                     mock.patch.object(sys, 'argv', ['check_release.py', '--game', game]), \
                     mock.patch.object(gate, 'read_manifest', return_value=(['LICENSE'], {}, {})) as reader, \
                     mock.patch.object(gate.import_closure, 'Repository'), \
                     mock.patch.object(gate.import_closure, 'check', return_value={'missing': []}) as closure, \
                     mock.patch.object(gate.generate_notices, 'generate', side_effect=ValueError('stop after closure')), \
                     contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(gate.main(), 2)
                reader.assert_called_once_with(manifest)
                self.assertEqual(closure.call_args.args[1:], ({'LICENSE'}, game))
                self.assertEqual(list((root / 'scratch').iterdir()), [])

    def test_release_refuses_stale_notices_before_creating_archive(self):
        with tempfile.TemporaryDirectory(dir=gate.ROOT / 'scratch') as directory:
            root = Path(directory)
            (root / 'THIRD_PARTY_NOTICES').write_text('stale')
            (root / 'manifest-ut.txt').write_text('THIRD_PARTY_NOTICES\n')
            stderr = io.StringIO()
            with mock.patch.object(gate, 'ROOT', root), \
                 mock.patch.object(sys, 'argv', ['check_release.py', '--manifest', 'manifest-ut.txt']), \
                 mock.patch.object(gate, 'read_manifest', return_value=(['THIRD_PARTY_NOTICES'], {}, {})), \
                 mock.patch.object(gate.import_closure, 'Repository'), \
                 mock.patch.object(gate.import_closure, 'check', return_value={'missing': []}), \
                 mock.patch.object(gate.generate_notices, 'generate', return_value='current'), \
                 contextlib.redirect_stderr(stderr):
                self.assertEqual(gate.main(), 2)
            self.assertIn('THIRD_PARTY_NOTICES is stale', stderr.getvalue())
            self.assertEqual(list((root / 'scratch').iterdir()), [])

    @classmethod
    def setUpClass(cls):
        (gate.ROOT / 'scratch').mkdir(exist_ok=True)

    def test_fresh_export_tree_matches_scanned_zip_and_cannot_be_reused(self):
        with tempfile.TemporaryDirectory(dir=gate.ROOT / 'scratch') as directory:
            root = Path(directory)
            path = root / 'candidate.zip'
            with zipfile.ZipFile(path, 'w') as archive:
                archive.writestr('src/main.c', b'int main(void) { return 0; }\n')
                archive.writestr('art/logo.png', b'\x89PNG\r\n\x1a\nsynthetic')
            members, hits = gate.scan_archive(path, ['src/main.c', 'art/logo.png'], [])
            self.assertEqual(gate.export_and_scan_tree(path, root / 'export', members, []),
                             (members, hits))
            self.assertEqual((root / 'export/src/main.c').read_bytes(),
                             b'int main(void) { return 0; }\n')
            with self.assertRaises(FileExistsError):
                gate.export_and_scan_tree(path, root / 'export', members, [])

    def test_upstream_ci_and_unlicensed_backend_cannot_be_exported(self):
        for path in ('engine/.github/workflows/tests.yml', '.github/workflows/other.yml',
                     'engine/src/renderer/ps3gl/render.c'):
            self.assertFalse(gate.export_path_allowed(path))
        self.assertTrue(gate.export_path_allowed('platform/3ds/source/main.c'))

    def scan(self, payload, name='innocent.txt', hashes=()):
        with tempfile.TemporaryDirectory(dir=gate.ROOT / 'scratch') as directory:
            archive = Path(directory) / 'candidate.zip'
            with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as output:
                output.writestr(name, payload)
            return gate.scan_archive(archive, [name], hashes)[1]

    def rules(self, payload, name='innocent.txt', hashes=()):
        return {h['rule'] for h in self.scan(payload, name, hashes)}

    def test_clean_source_archive(self):
        self.assertEqual(self.scan(b'int main(void) { return 0; }\n'), [])

    def test_renamed_and_prefixed_game_data(self):
        for prefix in (b'', b'prefix'):
            self.assertIn('game-container', self.rules(prefix + b'FORM\x10\0\0\0GEN8payload'))
        self.assertIn('media-or-output-signature', self.rules(b'OggS\x00payload'))

    def test_known_data_hash_text_binary_utf16_and_whole_member(self):
        import hashlib
        h = hashlib.sha256(b'synthetic game').hexdigest()
        for payload in (h.upper().encode(), bytes.fromhex(h), h.encode('utf-16-le'), h.encode('utf-16-be')):
            self.assertIn('known-data-sha256', self.rules(payload, hashes=[h]))
        self.assertIn('known-data-file', self.rules(b'synthetic game', hashes=[h]))

    def test_game_identifiers_in_payload_and_path(self):
        identifier = 'gml_Script_synthetic_probe'
        for encoding in ('utf-8', 'utf-16-le', 'utf-16-be'):
            self.assertIn('flagged-game-names', self.rules(identifier.encode(encoding)))
        self.assertIn('flagged-game-names', self.rules(b'content', name='room_synthetic.txt'))
        findings = self.scan(b'mus_caseA mus_casea')
        self.assertEqual(findings[0]['evidence']['names'], ['mus_caseA', 'mus_casea'])

    def test_working_name_only_in_the_old_sd_names(self):
        word = 'delta' + 'tale'
        for payload in (word, word.upper() + '_RENDER_TIMING', 'see ' + word + '-3ds', word.title() + ' builder'):
            for encoding in ('utf-8', 'utf-16-le', 'utf-16-be'):
                self.assertIn('internal-name', self.rules(payload.encode(encoding)))
        self.assertIn('internal-name', self.rules(b'content', name='docs/' + word + '.md'))
        for payload in ('"sdmc:/3ds/' + word + '"', '"' + word + '.ini";', 'settings.ini there is ' + word + '.ini.'):
            self.assertEqual(self.rules(payload.encode()), set())
        findings = self.scan('\n'.join(('a', word, 'b', word.upper() + '.ini2', '')).encode())
        self.assertEqual(findings[0]['evidence'], {'count': 2, 'lines': [2, 4]})

    def test_opaque_containers_and_large_embedded_blobs(self):
        self.assertIn('nested-container', self.rules(b'PK\x03\x04hidden'))
        self.assertIn('nested-container', self.rules(b'opaque', name='renamed.tar'))
        self.assertIn('large-binary', self.rules(b'\0' * gate.LARGE_BINARY))
        self.assertIn('embedded-blob', self.rules(b'0x01, ' * 4096))
        self.assertIn('embedded-blob', self.rules(b'A' * 8192))

    def test_media_assets_and_shader_names_are_review_hits(self):
        font = b'OTTO\x00\x01\x00\x10\x00\x00\x00\x00' + bytes(16)
        for payload in (b'\x89PNG\r\n\x1a\nsynthetic', b'RIFF\x20\0\0\0WAVE', font, b'3DSX\x20\0'):
            self.assertIn('media-or-output-signature', self.rules(payload))
        self.assertIn('flagged-game-names', self.rules(b'shd_synthetic_probe'))
        self.assertEqual(self.rules(b'BOTTOM_SCREEN checks RIFF and produces a 3DSX'), set())

    def test_forbidden_outputs_and_unsafe_paths(self):
        for name in ('credits.txt', 'renamed.ogg', 'cache/file.txt', 'aot_gen_0.c'):
            self.assertTrue(self.scan(b'x', name))
        for name in ('../outside', '/absolute', 'C:/outside', 'a\\b', 'a/./b', '*.c', 'a//b'):
            self.assertFalse(gate.safe_name(name))

    def test_archive_members_must_equal_manifest(self):
        with tempfile.TemporaryDirectory(dir=gate.ROOT / 'scratch') as directory:
            path = Path(directory) / 'bad.zip'
            with zipfile.ZipFile(path, 'w') as archive:
                archive.writestr('surprise.txt', b'content')
            with self.assertRaisesRegex(ValueError, 'differ from manifest'):
                gate.scan_archive(path, ['expected.txt'], [])

    def test_d2_only_allows_exact_reviewed_table_and_identifier_rules(self):
        name = 'tools/aot/selection/test.txt'
        data = b'# names only\nroot gml_Script_synthetic_probe\n'
        member = dict(path=name, sha256=hashlib.sha256(data).hexdigest())
        policy = dict(entries=[dict(member=name, **{'class': 'aot-selection'},
                                    sha256=member['sha256'])])
        hit = dict(member=name, rule='flagged-game-names', evidence='synthetic')
        media = dict(member=name, rule='game-container', evidence='synthetic')
        remaining, allowed = gate.apply_identifier_policy([hit, media], [member], policy,
                                                          lambda _: data)
        self.assertEqual(remaining, [media])
        self.assertEqual(allowed[0]['identifier_class'], 'aot-selection')
        # Appending code, removing the header, or moving the table needs review.
        for edited in (data + b'changed', data.replace(b'names only', b'no header')):
            changed = dict(member, sha256=hashlib.sha256(edited).hexdigest())
            self.assertEqual(gate.apply_identifier_policy([hit], [changed], policy,
                                                           lambda _: edited), ([hit], []))
        renamed = dict(hit, member='other.txt')
        self.assertEqual(gate.apply_identifier_policy([renamed], [dict(member, path='other.txt')],
                                                       policy, lambda _: data), ([renamed], []))
        missing_header = data.replace(b'names only', b'no header')
        changed = dict(member, sha256=hashlib.sha256(missing_header).hexdigest())
        policy['entries'][0]['sha256'] = changed['sha256']
        self.assertEqual(gate.apply_identifier_policy([hit], [changed], policy,
                                                       lambda _: missing_header), ([hit], []))

    def test_checked_in_d2_revisions_have_headers_and_match_the_policy(self):
        policy = gate.read_identifier_policy(gate.ROOT / 'tools/release/identifier-allowlist.json')
        for entry in policy['entries']:
            data = (gate.ROOT / entry['member']).read_bytes()
            self.assertEqual(hashlib.sha256(data).hexdigest(), entry['sha256'], entry['member'])
            self.assertTrue(entry['justification'].strip(), entry['member'])
            if entry['class'] not in gate.REVIEWED_CLASSES:
                self.assertIn('names only', data.decode('utf-8')[:2048], entry['member'])
            else:
                findings = [h for h in gate.inspect_member(entry['member'], data, gate.data_hashes())
                            if h['rule'] in gate.IDENTIFIER_CLASSES[entry['class']]]
                self.assertEqual({gate.evidence_sha256(h) for h in findings},
                                 set(entry['evidence_sha256']), entry['member'])

    def test_reviewed_source_binds_revision_evidence_and_identifier_rules(self):
        name = 'src/adapter.c'
        data = b'const char *key = "obj_fixture_lookup";\n'
        findings = gate.inspect_member(name, data, [])
        member = dict(path=name, sha256=hashlib.sha256(data).hexdigest())
        entry = dict(member=name, **{'class': 'engine-lookups'}, sha256=member['sha256'],
                     justification='Resolve the players own object.',
                     evidence_sha256=[gate.evidence_sha256(h) for h in findings])
        policy = dict(entries=[entry])
        media = dict(member=name, rule='game-container', evidence='synthetic')
        remaining, approved = gate.apply_identifier_policy(findings + [media], [member], policy, lambda _: data)
        self.assertEqual(remaining, [media])
        self.assertEqual(len(approved), 1)
        # Copying the entry, editing the file, or adding an identifier needs review.
        for edited in (data + b'// edit', data + b'\nobj_another_fixture'):
            revision = dict(member, sha256=hashlib.sha256(edited).hexdigest())
            new_hits = gate.inspect_member(name, edited, [])
            self.assertEqual(gate.apply_identifier_policy(new_hits, [revision], policy, lambda _: edited),
                             (new_hits, []))
        edited = data + b'\nobj_another_fixture'
        revision = dict(member, sha256=hashlib.sha256(edited).hexdigest())
        new_hits = gate.inspect_member(name, edited, [])
        # Even a revision-only bump must not silently approve new evidence.
        entry['sha256'] = revision['sha256']
        self.assertEqual(gate.apply_identifier_policy(new_hits, [revision], policy, lambda _: edited),
                         (new_hits, []))

    def test_symbol_and_synthetic_corrections_never_disable_real_detection(self):
        policy = gate.read_identifier_policy(gate.ROOT / 'tools/release/identifier-allowlist.json')
        entries = [e for e in policy['entries'] if e['class'] in gate.FALSE_POSITIVE_CLASSES]
        self.assertTrue(entries)
        for entry in entries:
            name = entry['member']
            data = (gate.ROOT / name).read_bytes()
            hits = gate.inspect_member(name, data, [])
            member = dict(path=name, sha256=hashlib.sha256(data).hexdigest())
            remaining, approved = gate.apply_identifier_policy(hits, [member], policy, lambda _: data)
            self.assertFalse(remaining, name)
            self.assertTrue(approved, name)
            edited = data + b'\nobj_unreviewed_fixture_content\n'
            new_hits = gate.inspect_member(name, edited, [])
            revision = dict(member, sha256=hashlib.sha256(edited).hexdigest())
            remaining, approved = gate.apply_identifier_policy(new_hits, [revision], policy, lambda _: edited)
            self.assertTrue(remaining, name)
            self.assertFalse(approved, name)
        # The same spelling in an unreviewed file is still flagged, in every encoding.
        for spelling in ('ROOM_HEADER_MAX_SIZE', 'sh_new_strdup', 'room_memory_reset', 'gml_Script_synthetic_probe'):
            for encoding in ('utf-8', 'utf-16-le', 'utf-16-be'):
                data = spelling.encode(encoding)
                hits = gate.inspect_member('unreviewed.txt', data, [])
                self.assertIn('flagged-game-names', {h['rule'] for h in hits})
                member = dict(path='unreviewed.txt', sha256=hashlib.sha256(data).hexdigest())
                self.assertEqual(gate.apply_identifier_policy(hits, [member], policy, lambda _: data), (hits, []))

    def test_flac_signature_literal_is_not_embedded_in_scanner_source(self):
        source = Path(gate.__file__).read_bytes()
        self.assertFalse(any(h['rule'] == 'media-or-output-signature'
                             for h in gate.inspect_member('scanner.py', source, [])))
        for prefix in (b'', b'prefix', b'const char *deceptive = '):
            self.assertIn('media-or-output-signature',
                          self.rules(prefix + bytes.fromhex('664c6143') + b'payload', name='renamed.c'))

    def test_original_asset_policy_is_separate_and_exact_bytes_only(self):
        name = 'platform/3ds/assets/banner.wav'
        data = b'RIFF\0\0\0\0WAVE' + b'\0' * gate.LARGE_BINARY
        hits = gate.inspect_member(name, data, [])
        member = dict(path=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        entry = dict(member=name, bytes=len(data), sha256=member['sha256'],
                     justification='Synthetic original asset fixture.',
                     evidence_sha256=[gate.evidence_sha256(h) for h in hits])
        policy = dict(entries=[entry])
        self.assertEqual(len(gate.apply_asset_policy(hits, [member], policy)[1]), 2)
        for edited in (data + b'x', data.replace(b'WAVE', b'FORM', 1)):
            revision = dict(member, bytes=len(edited), sha256=hashlib.sha256(edited).hexdigest())
            new_hits = gate.inspect_member(name, edited, [])
            self.assertEqual(gate.apply_asset_policy(new_hits, [revision], policy), (new_hits, []))
        for rule in ('game-container', 'known-data-file', 'flagged-game-names', 'embedded-blob', 'forbidden-path'):
            hit = dict(member=name, rule=rule, evidence='synthetic')
            self.assertEqual(gate.apply_asset_policy([hit], [member], policy), ([hit], []))
        # A clone at a different path is not original-asset clearance.
        moved = 'other.wav'
        moved_hits = gate.inspect_member(moved, data, [])
        self.assertEqual(gate.apply_asset_policy(moved_hits, [dict(member, path=moved)], policy),
                         (moved_hits, []))

    def test_checked_in_original_assets_and_generator_match_reviewed_policy(self):
        policy = gate.read_asset_policy(gate.ROOT / 'tools/release/original-asset-policy.json')
        self.assertEqual(len(policy['entries']), 10)
        self.assertEqual({e['member'] for e in policy['entries'] if '-deltarune-' in e['member']}, {
            'platform/3ds/assets/%s-deltarune-%s.png' % (kind, target)
            for kind in ('banner', 'icon') for target in ('o3ds', 'n3ds')})
        for entry in policy['entries']:
            data = (gate.ROOT / entry['member']).read_bytes()
            member = dict(path=entry['member'], bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
            hits = gate.inspect_member(entry['member'], data, gate.data_hashes())
            if entry['member'] == gate.PRESS_SOURCE:
                self.assertEqual(member['sha256'], entry['sha256'])
                self.assertEqual(member['bytes'], entry['bytes'])
                rows = re.search(rb'pixpressRows\[5\].*?=\s*\{(.*?)\}', data, re.S)[1]
                packed = b''.join(int(n, 16).to_bytes(2, 'little')
                                  for n in re.findall(rb'0x([0-9A-Fa-f]+)', rows))
                self.assertEqual(len(packed), 10)
                self.assertEqual(hashlib.sha256(packed).hexdigest(), entry['glyph_rows_sha256'])
                self.assertEqual(gate.apply_asset_policy(hits, [member], policy), (hits, []))
                continue
            remaining, approved = gate.apply_asset_policy(hits, [member], policy)
            self.assertFalse(remaining, entry['member'])
            self.assertEqual(len(approved), len(hits))

    def test_new_model_selection_comment_keeps_actual_generator_input_identical(self):
        sys.path.insert(0, str(gate.ROOT / 'tools/aot'))
        import prepare
        path = gate.ROOT / 'tools/aot/selection/deltarune-parts-n3ds.txt'
        data = path.read_bytes()
        comment, original = data.split(b'\n', 1)
        self.assertIn(b'names only', comment)
        with tempfile.TemporaryDirectory(dir=gate.ROOT / 'scratch') as directory:
            before = Path(directory) / 'before.txt'
            before.write_bytes(original)
            self.assertEqual(prepare.selection_parts(before), prepare.selection_parts(path))
        # Under the existing policy only the exact named revision is classified. Revisions, other paths,
        # private bench tables and missing headers still fail the pre-existing adversarial tests.
        hits = gate.inspect_member(path.relative_to(gate.ROOT).as_posix(), data, [])
        member = dict(path=path.relative_to(gate.ROOT).as_posix(), bytes=len(data),
                      sha256=hashlib.sha256(data).hexdigest())
        policy = gate.read_identifier_policy(gate.ROOT / 'tools/release/identifier-allowlist.json')
        remaining, approved = gate.apply_identifier_policy(hits, [member], policy, lambda _: data)
        self.assertFalse(remaining)
        self.assertTrue(approved)

    def test_roomplan_policy_does_not_allow_arbitrary_json_or_private_data(self):
        with tempfile.TemporaryDirectory(dir=gate.ROOT / 'scratch') as directory:
            path = Path(directory) / 'policy.json'
            for member in ('arbitrary.json', 'tools/roomplan/records/new.json',
                           'platform/3ds/bench/record.json', 'tools/analysis/census.json'):
                entry = dict(member=member, **{'class': 'roomplan-record'}, sha256='0'*64,
                             justification='Synthetic.', evidence_sha256=['1'*64])
                path.write_text(json.dumps(dict(schema=1, decision='D2, owner 2026-10-20', entries=[entry])))
                with self.assertRaises(ValueError):
                    gate.read_identifier_policy(path)
        policy = gate.read_identifier_policy(gate.ROOT / 'tools/release/identifier-allowlist.json')
        records = {e['member'] for e in policy['entries'] if e['class'] == 'roomplan-record'}
        self.assertEqual(len(records), 11)  # 9 room plan records (2 GOG), 1 phase record (phaseplan, DELTARUNE),
        # 1 track record (drresid, DELTARUNE New)
        for game, filename in (('undertale', 'manifest-ut.txt'), ('deltarune', 'manifest-dr.txt')):
            manifest = set((gate.ROOT / 'tools/release' / filename).read_text().splitlines())
            required = {record for record in records
                        if Path(record).name.startswith(game + '-') or Path(record).name == game + '.json'}
            self.assertTrue(required <= manifest)
            self.assertFalse((records - required) & manifest)
            self.assertIn('tools/roomplan/records.py', manifest)

    def test_invalid_source_and_asset_approvals_are_rejected(self):
        with tempfile.TemporaryDirectory(dir=gate.ROOT / 'scratch') as directory:
            path = Path(directory) / 'policy.json'
            entry = dict(member='source.c', **{'class': 'engine-lookups'}, sha256='0'*64,
                         justification='Reviewed.', evidence_sha256=['1'*64])
            for changes in ({'justification': ''}, {'evidence_sha256': []}, {'evidence_sha256': ['bad']}):
                path.write_text(json.dumps(dict(schema=1, decision='D2, owner 2026-10-20',
                                                entries=[dict(entry, **changes)])))
                with self.assertRaises(ValueError):
                    gate.read_identifier_policy(path)
            policy = gate.read_asset_policy(gate.ROOT / 'tools/release/original-asset-policy.json')
            for member in ('game.png', 'platform/3ds/assets/game.ogg', 'platform/3ds/assets/new.png'):
                bad = dict(policy, entries=[dict(policy['entries'][0], member=member)])
                path.write_text(json.dumps(bad))
                with self.assertRaises(ValueError):
                    gate.read_asset_policy(path)
            path.write_text(json.dumps(dict(policy, generator_sha256='0'*64)))
            with self.assertRaisesRegex(ValueError, 'generator revision'):
                gate.read_asset_policy(path)
            press = next(e for e in policy['entries'] if e['member'] == gate.PRESS_SOURCE)
            for changes in ({'sha256': '0'*64}, {'bytes': press['bytes'] + 1}):
                path.write_text(json.dumps(dict(policy, entries=[dict(press, **changes)])))
                with self.assertRaisesRegex(ValueError, 'PRESS source revision'):
                    gate.read_asset_policy(path)

    def test_d2_never_accepts_bench_census_or_arbitrary_rules(self):
        with tempfile.TemporaryDirectory(dir=gate.ROOT / 'scratch') as directory:
            path = Path(directory) / 'policy.json'
            for member, kind in (('platform/3ds/bench/test.txt', 'aot-selection'),
                                 ('tools/analysis/shader_handles.json', 'data-versions'),
                                 ('valid.txt', 'game-container')):
                path.write_text(json.dumps(dict(schema=1, decision='D2, owner 2026-10-20',
                    entries=[dict(member=member, **{'class': kind}, sha256='0'*64)])))
                with self.assertRaises(ValueError):
                    gate.read_identifier_policy(path)

    def test_manifest_map_lines_and_dangling_markdown_links(self):
        with tempfile.TemporaryDirectory(dir=gate.ROOT / 'scratch') as directory:
            path = Path(directory) / 'manifest.txt'
            path.write_text('# note\n#map README.md <- tools/release/templates/README-undertale.md\nREADME.md\n')
            self.assertEqual(gate.read_manifest_sources(path),
                             {'README.md': 'tools/release/templates/README-undertale.md'})
            for bad in ('#map README.md tools/x.md\n', '#map README.md <- a.md\n#map README.md <- b.md\n'):
                path.write_text(bad + 'README.md\n')
                with self.assertRaises(ValueError):
                    gate.read_manifest_sources(path)
        members = {'README.md', 'LICENSE', 'docs/release/build-contract.json', 'engine/README.md'}
        text = (b'[ok](LICENSE) [dir](docs/release) [up](../LICENSE) [web](https://example.com/x.md) '
                b'[anchor](#top) [gone](docs/playing.md) [deep](engine/README.md#x) [title](LICENSE "t")')
        found = [h['evidence'] for h in gate.dangling_links('README.md', text, members)]
        self.assertEqual(found, ['../LICENSE', 'docs/playing.md'])
        self.assertEqual(gate.dangling_links('engine/README.md', b'[up](../LICENSE)', members), [])
        self.assertEqual(gate.dangling_links('notes.txt', b'[gone](docs/playing.md)', members), [])

    def test_public_readme_comes_from_its_template(self):
        manifest = gate.ROOT / 'tools/release/manifest-ut.txt'
        self.assertEqual(gate.read_manifest_sources(manifest),
                         {'README.md': 'tools/release/templates/README-undertale.md'})
        names = manifest.read_text(encoding='utf-8').splitlines()
        self.assertIn('README.md', names)
        self.assertNotIn('tools/release/templates/README-undertale.md', names)

    def test_public_source_digest_hashes_the_mapped_export_bytes(self):
        # argstage59 F-5.1: DELTARUNE maps drawskip_deltarune.txt to a generic template. The descriptor's digest must
        # be the one package_builder computes from the exported tree (mapped bytes, builder.py's FIXED_GAME set).
        import package_builder
        for game, short in (('undertale', 'ut'), ('deltarune', 'dr')):
            names, _tracked, sources = gate.read_manifest(gate.safe_local('tools/release/manifest-%s.txt' % short))
            files = {name: gate.safe_local(sources.get(name, name)).read_bytes() for name in names}
            digest = gate.public_source_digest(package_builder.inputs(gate.ROOT, game), files, sources,
                                               package_builder.pipeline.source_payload(gate.ROOT, game))
            with tempfile.TemporaryDirectory() as tmp:
                export = Path(tmp)
                for name, data in files.items():
                    (export / name).parent.mkdir(parents=True, exist_ok=True)
                    (export / name).write_bytes(data)
                payload = package_builder.inputs(export, game)
                builder = gate.release_contract.payload_digest(
                    {r: payload[r] for r in package_builder.pipeline.source_payload(export, game)})
            self.assertEqual(digest, builder, game)
        member = 'platform/3ds/bench/drawskip_deltarune.txt'
        self.assertIn(member, sources)
        self.assertNotEqual(files[member], (gate.ROOT / member).read_bytes())
        wanted = package_builder.pipeline.source_payload(gate.ROOT, 'deltarune')
        self.assertIn(member, wanted)
        with self.assertRaisesRegex(ValueError, 'export lacks'):
            gate.public_source_digest({}, {n: files[n] for n in wanted if n != member}, sources, wanted)

    def test_export_manifest_omits_private_bench_scripts(self):
        manifest = (gate.ROOT / 'tools/release/manifest-ut.txt').read_text().splitlines()
        self.assertNotIn('platform/3ds/bench/undertale.txt', manifest)
        self.assertNotIn('platform/3ds/bench/sweep_undertale.txt', manifest)
        self.assertFalse(any('shader_handles' in name for name in manifest))

    @unittest.skipUnless(shutil.which('cmake'), 'CMake required')
    def test_archive_without_private_coverage_keeps_existing_bytes_or_empty_file(self):
        cmake = (gate.ROOT / 'platform/3ds/CMakeLists.txt').read_text()
        start = cmake.index('if(EXISTS "${COVERAGE_SCENE_SOURCE}")')
        end = cmake.index('set(COVERAGE_ROMFS_FILE', start)
        with tempfile.TemporaryDirectory(dir=gate.ROOT / 'scratch') as directory:
            root = Path(directory)
            source, output = root / 'coverage.txt', root / 'bench-supplement.txt'
            for content in (b'synthetic scene\r\n\n', None):
                if content is None:
                    source.unlink()
                else:
                    source.write_bytes(content)
                script = root / 'query.cmake'
                script.write_text('set(COVERAGE_SCENE_SOURCE "' + source.as_posix() + '")\n'
                    'set(CTRPORT_ROMFS_DIR "' + root.as_posix() + '")\n' + cmake[start:end])
                subprocess.run(['cmake', '-P', str(script)], check=True,
                               stdin=subprocess.DEVNULL, capture_output=True, timeout=20)
                self.assertEqual(output.read_bytes(), content or b'')

    @unittest.skipUnless(shutil.which('cmake'), 'CMake required')
    def test_archive_without_private_scenes_keeps_existing_input_bytes_or_empty_list(self):
        cmake = (gate.ROOT / 'platform/3ds/CMakeLists.txt').read_text()
        start = cmake.index('set(BENCH_SCENE_TEXT "")')
        end = cmake.index('file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/bench_scenes.txt.new"', start)
        block = cmake[start:end]
        with tempfile.TemporaryDirectory(dir=gate.ROOT / 'scratch') as directory:
            root = Path(directory)
            present = root / 'present.txt'
            present.write_text('synthetic scene\n')
            for sources, expected in (([present, root / 'absent.txt'], b'synthetic scene\n\n'),
                                      ([root / 'absent.txt'], b'')):
                script = root / 'query.cmake'
                output = root / 'out.txt'
                script.write_text('set(BENCH_SCENE_SOURCES ' + ' '.join('"'+p.as_posix()+'"' for p in sources)
                    + ')\n' + block + '\nfile(WRITE "'+output.as_posix()+'" "${BENCH_SCENE_TEXT}")\n')
                subprocess.run(['cmake', '-P', str(script)], check=True,
                               stdin=subprocess.DEVNULL, capture_output=True, timeout=20)
                self.assertEqual(output.read_text(), expected.decode())
                if present in sources:
                    # Compare with the old unconditional READ/APPEND on this host,
                    # including CMake's native output newline convention.
                    old_output = root / 'old.txt'
                    script.write_text('file(READ "'+present.as_posix()+'" text)\n'
                        'string(APPEND text "\n")\nfile(WRITE "'+old_output.as_posix()+'" "${text}")\n')
                    subprocess.run(['cmake', '-P', str(script)], check=True,
                                   stdin=subprocess.DEVNULL, capture_output=True, timeout=20)
                    self.assertEqual(output.read_bytes(), old_output.read_bytes())


if __name__ == '__main__':
    unittest.main()
