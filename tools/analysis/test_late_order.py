"""Single late-order contract and negative controls; no compiler or game data."""
import ast
import copy
import json
from pathlib import Path
import re
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
        # The last plain selector before repack_test (a per-model unit's own selector also fails its unit check).
        plain = [e for e in registry['text'][:registry['text'].index('*(.repack_test)')]
                 if not any(unit in e for unit in registry.get('per_model', []))]
        missing = plain[-1]
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


SOURCE = 'platform/3ds/source/probe.c'


def per_model(kinds=('text',), keep=False):
    """Live registry plus one per-model unit 'probe' with both names for each kind, and a source using it."""
    registry = copy.deepcopy(late_order.load())
    registry['per_model'] = ['probe']  # live units' sources are not in these trees
    registry['old_late_text'] = []
    body = ['#include "ctr_place.h"\n']
    for kind in kinds:
        aac, late = late_order.place_names(kind, 'probe')
        registry['aac_' + kind].append(aac)
        registry[kind].append(('KEEP(*(%s))' if keep else '*(%s)') % late)
        body.append('static int %s probe_%s(void) { return 1; }\n'
                    % ('UT_COLD_TEXT(probe)' if kind == 'text' else 'UT_COLD_RODATA( probe )', kind))
    return registry, {SOURCE: ''.join(body)}

# c26rev8 r2 F3R-4: a backslash-newline inside an escape (C splices before it reads the escape), and F3R-5: three
# short literal-valued macros. Both compile to .text_probe; the first is plain once spliced, the second is the map's.
F3R4_SOURCES = {
    'hex escape split': '__attribute__((section("\\x2\\\ne" "text_probe"))) int bad(void) { return 2; }\n',
    'octal escape split': '__attribute__((section("\\05\\\n6text_probe"))) int bad(void) { return 2; }\n',
    'escape backslash split': '__attribute__((section("\\\\\nx2etext_probe"))) int bad(void) { return 2; }\n',
}
F3R5_SOURCE = ('#define A ".te"\n#define B "xt_pr"\n#define C "obe"\n'
               '__attribute__((section(A B C))) int bad(void) { return 2; }\n')


class PerModelTests(unittest.TestCase):
    def check(self, registry, sources):
        return late_order.check_per_model(registry, sources)

    def assertOneError(self, errors, *parts):
        self.assertEqual(len(errors), 1, errors)
        for part in parts:
            self.assertIn(part, errors[0])

    def test_live_tree_passes_and_the_header_is_not_scanned(self):
        sources = late_order.read_sources()
        self.assertNotIn(late_order.PLACE_HEADER, sources)
        self.assertTrue((late_order.ROOT / late_order.PLACE_HEADER).is_file())
        self.assertGreater(sum(path.startswith('engine/src/') for path in sources), 100)
        self.assertGreater(sum(path.startswith('platform/3ds/source/') for path in sources), 100)
        self.assertEqual(self.check(late_order.load(), sources), [])

    def test_complete_pairs_pass_in_both_late_forms(self):
        for kinds in (('text',), ('rodata',), ('text', 'rodata')):
            for keep in (False, True):
                with self.subTest(kinds=kinds, keep=keep):
                    self.assertEqual(self.check(*per_model(kinds, keep)), [])

    def test_a_missing_name_fails_on_either_side(self):
        for kind in late_order.PLACE_KINDS:
            aac, late = late_order.place_names(kind, 'probe')
            registry, sources = per_model((kind,))
            registry['aac_' + kind].remove(aac)
            with self.subTest(kind=kind, missing='aac'):
                errors = self.check(registry, sources)
                self.assertIn('add ' + aac, errors[0])
                self.assertTrue(any('UT_COLD_%s(probe): unit probe has no registered' % kind.upper() in e
                                    for e in errors), errors)
            registry, sources = per_model((kind,))
            registry[kind].remove('*(%s)' % late)
            with self.subTest(kind=kind, missing='late'):
                self.assertOneError(self.check(registry, sources), 'add ' + late + ' in late ' + kind)

    def test_duplicate_misplaced_or_object_scoped_late_names_fail(self):
        late = '*(.text_probe)'
        mutants = {
            'duplicate late': lambda r: r['text'].append(late),
            'duplicate keep': lambda r: r['text'].append('KEEP(%s)' % late),
            'wrong list': lambda r: r['rodata'].append(late),
            'object scoped': lambda r: r['text'].append('*probe.o(.text_probe)'),
            'replaced by object scope': lambda r: r['text'].__setitem__(r['text'].index(late), '*p.o(.text_probe)'),
        }
        for name, mutate in mutants.items():
            registry, sources = per_model()
            mutate(registry)
            with self.subTest(name):
                self.assertOneError(self.check(registry, sources), 'must be one late text selector')
        registry, sources = per_model()
        registry['aac_text'].append('.aacout.text.probe')
        self.assertOneError(self.check(registry, sources), 'listed 2 times in aac_text')

    def test_similar_names_are_other_sections(self):
        registry, sources = per_model()
        registry['text'].append('*(.text_probe_late)')
        registry['aac_text'].append('.aacout.text.probe.late')
        sources['engine/src/other.c'] = 'static int __attribute__((section(".text_probe_late"))) f(void);\n'
        self.assertEqual(self.check(registry, sources), [])

    def test_a_unit_with_no_names_fails(self):
        registry = copy.deepcopy(late_order.load())
        registry['per_model'] = ['probe']
        self.assertOneError(self.check(registry, {}), 'per_model probe: registers neither')

    def test_registered_kind_without_a_use_fails(self):
        registry, sources = per_model(('text', 'rodata'))
        sources[SOURCE] = sources[SOURCE].replace('UT_COLD_RODATA( probe )', '')
        self.assertOneError(self.check(registry, sources), 'rodata is registered but no source uses '
                            'UT_COLD_RODATA(probe)')
        sources[SOURCE] = sources[SOURCE].replace('UT_COLD_TEXT(probe)', '/* UT_COLD_TEXT(probe) */')
        self.assertEqual(len(self.check(registry, sources)), 2)

    def test_uses_must_name_a_registered_unit_and_kind(self):
        mutants = {
            'unregistered unit': ('UT_COLD_TEXT(probe)', 'UT_COLD_TEXT(probe) UT_COLD_TEXT(other)',
                                  'unit other has no registered text'),
            'unregistered kind': ('UT_COLD_TEXT(probe)', 'UT_COLD_TEXT(probe) UT_COLD_RODATA(probe)',
                                  'unit probe has no registered rodata'),
            'two names': ('UT_COLD_TEXT(probe)', 'UT_COLD_TEXT(probe) UT_COLD_TEXT(probe x)', 'one plain unit name'),
            'no argument': ('UT_COLD_TEXT(probe)', 'UT_COLD_TEXT(probe) UT_COLD_TEXT', 'one plain unit name'),
            'nested': ('UT_COLD_TEXT(probe)', 'UT_COLD_TEXT(probe) UT_COLD_TEXT(N(probe))', 'one plain unit name'),
        }
        for name, (old, new, message) in mutants.items():
            registry, sources = per_model()
            sources[SOURCE] = sources[SOURCE].replace(old, new)
            with self.subTest(name):
                self.assertOneError(self.check(registry, sources), SOURCE + ':2: ', message)

    def test_hand_spelled_section_names_fail_outside_comments(self):
        for name in ('.text_probe', '.aacout.text.probe'):
            for where in ('engine/src/hand.c', 'platform/3ds/source/hand.h', 'engine/src/hand.inc'):
                registry, sources = per_model()
                sources[where] = '\nstatic int __attribute__((section("%s"))) g(void);\n' % name
                with self.subTest(name=name, where=where):
                    self.assertOneError(self.check(registry, sources), where + ':2: ' + name + ' is spelled by hand')
            registry, sources = per_model()
            sources['engine/src/note.c'] = '// %s\n/* "%s" */\nint x;\n' % (name, name)
            self.assertEqual(self.check(registry, sources), [])

    def test_joined_or_escaped_literals_are_read_as_c_reads_them(self):  # c26rev8 F3R-2
        spellings = {
            'adjacent': 'section(".text_" "probe")',
            'three pieces': 'section(".te" "xt_pr" "obe")',
            'comment between': 'section(".text_" /* x */ "probe")',
            'line comment between': 'section(".text_" // x\n "probe")',
            'continued': '\\\n section(".text_" \\\n "probe")',
            'hex escape': 'section(".text_\\x70robe")',
            'octal escape': 'section("\\056text_probe")',
            'universal escape': 'section(".text_\\u0070robe")',
            'spliced': 'section(".text_pr\\\nobe")',
            'spliced hex escape': 'section(".text_\\x7\\\n0robe")',
            'spliced octal escape': 'section(".text_\\16\\\n0robe")',
            'prefixed': 'section(u8".text_" "probe")',
            'aac name joined': 'section(".aacout.text." "probe")',
            'assembler': '__asm__(".pushsection .text_probe,\\"ax\\"")',
            'stringized tokens': 'section(STR(.text_probe))',
        }
        for name, attr in spellings.items():
            registry, sources = per_model()
            sources['engine/src/hand.c'] = '\nstatic int __attribute__((%s)) g(void);\n' % attr
            with self.subTest(name):
                errors = self.check(registry, sources)
                self.assertOneError(errors, 'engine/src/hand.c:', 'is spelled by hand')
                self.assertTrue(re.search(r'\.(text_|aacout\.text\.)probe is spelled', errors[0]), errors)
        others = {
            'other section joined': 'section(".text_" "probe_late")',
            'other escape': 'section(".text_\\x70robes")',
            'string then char': 'f(".text_", \'p\', "robe")',
            'call arguments': 'f(".text_", "probe")',
            'name in a log line': 'printf("probe %d", 1)',
        }
        for name, attr in others.items():
            registry, sources = per_model()
            sources['engine/src/other.c'] = '\nint g(void) { return %s; }\n' % attr
            with self.subTest(name):
                self.assertEqual(self.check(registry, sources), [])

    def test_pieces_a_macro_or_define_joins_warn(self):  # c26rev8 F3R-2; advisory since the r2 ruling
        pieces = {
            'stringizing macro': ('#define SEC(x) ".text_" #x\nint __attribute__((section(SEC(probe)))) g(void);\n',
                                  "'.text_' is a piece of .text_probe"),
            'prefix in a define': ('#define PRE ".text_"\n', "'.text_' is a piece of .text_probe"),
            'suffix after a macro': ('int __attribute__((section(PRE "probe"))) g(void);\n',
                                     "'probe' is a piece of .aacout.text.probe"),
            'suffix in a define': ('#define NAME "probe"\n', "'probe' is a piece of .aacout.text.probe"),
            'aac prefix by macro': ('int __attribute__((section(".aacout.text." UNIT))) g(void);\n',
                                    "'.aacout.text.' is a piece of .aacout.text.probe"),
            'continued define': ('#define SEC \\\n    ".text_pr"\n', "'.text_pr' is a piece of .text_probe"),
            'function-like define': ('#define PRE() ".text_"\n', "'.text_' is a piece of .text_probe"),
            'continued function-like define': ('#define PRE() \\\n    ".text_"\n',
                                               "'.text_' is a piece of .text_probe"),
        }
        for name, (source, message) in pieces.items():
            registry, sources = per_model()
            sources['engine/src/piece.c'] = source
            with self.subTest(name):
                warnings = []
                self.assertEqual(late_order.check_per_model(registry, sources, warnings), [])
                self.assertTrue(warnings and all('engine/src/piece.c:' in w and 'joined by a macro' in w and
                                                 'advisory' in w for w in warnings), warnings)
                self.assertTrue(any(message.replace(' is a ', ' may be a ') in w for w in warnings), warnings)
        for source in ('#define LOG "probe: %d"\n', '#define DOT "."\n', '#define T ".text"\n', '#define S "obe"\n',
                       'int g(void) { return f(".text_"); }\n', '#include "probe.h"\n'):
            registry, sources = per_model()
            sources['engine/src/fine.c'] = source
            with self.subTest(source=source):
                warnings = []
                self.assertEqual(late_order.check_per_model(registry, sources, warnings), [])
                self.assertEqual(warnings, [])

    def test_spliced_escapes_are_plain_and_short_macros_are_the_maps(self):  # c26rev8 r2 F3R-4, F3R-5
        for name, source in F3R4_SOURCES.items():
            registry, sources = per_model()
            sources['engine/src/hand.c'] = source
            with self.subTest(name):
                self.assertOneError(self.check(registry, sources), 'engine/src/hand.c:1: .text_probe is spelled')
        registry, sources = per_model()  # pieces no macro rule can see: late_floor_gate.py's Old map check
        sources['engine/src/hand.c'] = F3R5_SOURCE
        warnings = []
        self.assertEqual(late_order.check_per_model(registry, sources, warnings), [])

    def test_splice_keeps_source_lines(self):
        text, joins = late_order.splice('a\\\nb\nc\\\r\n\\\nd\ne')
        self.assertEqual(text, 'ab\ncd\ne')
        self.assertEqual([late_order.line_of(text, joins, text.index(c)) for c in 'abcde'], [1, 2, 3, 5, 6])

    def test_a_macro_name_in_a_string_or_char_is_no_use(self):  # c26rev8 F3R-3
        for body in ('const char *note = "UT_COLD_TEXT(probe)";\n', 'int c = \'UT_COLD_TEXT(probe)\';\n',
                     'const char *n = "a" "UT_COLD_TEXT(probe)";\n'):
            registry, sources = per_model()
            sources[SOURCE] = body
            with self.subTest(body=body):
                self.assertOneError(self.check(registry, sources), 'text is registered but no source uses '
                                    'UT_COLD_TEXT(probe)')
        registry, sources = per_model()  # a use in a string does not count as an unregistered use either
        sources['engine/src/note.c'] = 'const char *note = "UT_COLD_TEXT(other)";\n'
        self.assertEqual(self.check(registry, sources), [])

    def test_lexer_joins_only_string_sequences(self):
        code, runs = late_order.lex(late_order.splice('a = "x" /* c */ "y" \\\n "z"; f(\'q\', "w"); "u" + "v";'
                                                      '\n// "no"\n')[0])
        self.assertEqual([v for _, _, v in runs], ['xyz', 'w', 'u', 'v'])
        self.assertNotIn('"', code)
        self.assertEqual(code.count('\n'), 2)
        self.assertEqual([v for _, _, v in late_order.lex('"a" \'b\' "c"')[1]], ['a', 'c'])
        self.assertEqual(late_order.decode_literal(r'\x41\101A\n\q\\'), 'AAA\nq\\')

    def test_cli_on_the_review_sources(self):  # c26rev8 F3R-2/3 and r2 F3R-4/5, the full command line
        registry, _ = per_model()
        ld = (late_order.ROOT / 'platform/3ds/late_reserve.ld').read_text(encoding='utf-8')
        aac = (late_order.ROOT / 'platform/3ds/aacout.cmake').read_text(encoding='utf-8')
        ld = ld.replace('    *(.repack_test_ro)\n', '    *(.repack_test_ro)\n    *(.text_probe)\n', 1)
        aac = re.sub(r'(^set\(aacout_text_inputs[ \t]*\n.*?)(^\))', r'\1    .aacout.text.probe\n\2', aac, count=1,
                     flags=re.M | re.S)
        good = '#include "ctr_place.h"\nUT_COLD_TEXT(probe) int good(void) { return 1; }\n'
        cases = {
            'clean': (good, 0, []),
            'joined literal': (good + '__attribute__((section(".text_" "probe"))) int bad(void) { return 2; }\n', 1,
                               ['FAIL: platform/3ds/source/probe.c:3: .text_probe is spelled by hand; use the '
                                'ctr_place.h macro so both models place it']),
            'use only in a string': ('const char *note = "UT_COLD_TEXT(probe)";\n', 1,
                                     ['FAIL: per_model probe: text is registered but no source uses '
                                      'UT_COLD_TEXT(probe)']),
            'hex escape split': (good + F3R4_SOURCES['hex escape split'], 1,
                                 ['FAIL: platform/3ds/source/probe.c:3: .text_probe is spelled by hand; use the '
                                  'ctr_place.h macro so both models place it']),
            'macro piece': (good + '#define PRE ".text_"\n', 0,
                            ["WARN: platform/3ds/source/probe.c:3: '.text_' may be a piece of .text_probe joined by "
                             'a macro, # or #define this lint cannot read; use UT_COLD_TEXT(probe) (advisory: the '
                             'Old map check decides)']),
            'short macros (the map gate)': (good + F3R5_SOURCE, 0, []),
        }
        for name, (source, rc, lines) in cases.items():
            with tempfile.TemporaryDirectory() as folder, self.subTest(name):
                folder = Path(folder)
                for top in late_order.SOURCE_DIRS:
                    (folder / top).mkdir(parents=True)
                (folder / SOURCE).write_bytes(source.encode('utf-8'))
                (folder / 'unit.json').write_bytes(json.dumps(registry).encode('utf-8'))
                (folder / 'late.ld').write_bytes(ld.encode('utf-8'))
                (folder / 'aac.cmake').write_bytes(aac.encode('utf-8'))
                result = subprocess.run([sys.executable, '-I', str(Path(late_order.__file__)), '--registry',
                                         str(folder / 'unit.json'), '--ld', str(folder / 'late.ld'), '--aac',
                                         str(folder / 'aac.cmake'), '--root', str(folder)],
                                        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, rc, result.stdout + result.stderr)
                if lines:
                    self.assertEqual(result.stdout.splitlines()[:len(lines)], lines)
                if rc:
                    self.assertEqual(result.stdout.splitlines(), lines)
                else:
                    self.assertIn('and 1 per-model unit(s).', result.stdout)

    def test_comment_stripping_keeps_strings_and_lines(self):
        text = 'a // "x"\n"/* not */" /* b\nc */ \'"\' d\n'
        stripped = late_order.strip_comments(text)
        self.assertEqual(stripped.count('\n'), text.count('\n'))
        self.assertIn('"/* not */"', stripped)
        self.assertIn('\'"\' d', stripped)
        self.assertNotIn('"x"', stripped)
        self.assertNotIn('c */', stripped)

    def test_registry_rejects_bad_per_model_lists(self):
        mutants = {
            'missing key': lambda r: r.pop('per_model'),
            'not a list': lambda r: r.__setitem__('per_model', 'probe'),
            'duplicate': lambda r: r.__setitem__('per_model', ['probe', 'probe']),
            'dotted': lambda r: r.__setitem__('per_model', ['probe.late']),
            'empty name': lambda r: r.__setitem__('per_model', ['']),
            'number': lambda r: r.__setitem__('per_model', [7]),
        }
        for name, mutate in mutants.items():
            registry = copy.deepcopy(late_order.load())
            mutate(registry)
            with tempfile.TemporaryDirectory() as folder, self.subTest(name):
                path = Path(folder) / 'bad.json'
                path.write_text(json.dumps(registry), encoding='utf-8')
                with self.assertRaises(ValueError):
                    late_order.load(path)

    def test_registry_rejects_unknown_or_duplicate_old_text_overrides(self):
        for overrides in (['unknown'], ['bstall', 'bstall'], 'bstall', [7]):
            registry = copy.deepcopy(late_order.load())
            registry['old_late_text'] = overrides
            with tempfile.TemporaryDirectory() as folder, self.subTest(overrides=overrides):
                path = Path(folder) / 'bad.json'
                path.write_text(json.dumps(registry), encoding='utf-8')
                with self.assertRaisesRegex(ValueError, 'old_late_text'):
                    late_order.load(path)

    def test_cli_runs_the_per_model_check(self):
        registry = copy.deepcopy(late_order.load())
        registry['per_model'].append('probe')
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'unit.json'
            path.write_text(json.dumps(registry), encoding='utf-8')
            result = subprocess.run([sys.executable, '-I', str(Path(late_order.__file__)), '--registry', str(path)],
                                    stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(result.stdout.splitlines(), ['FAIL: per_model probe: registers neither text nor rodata names'])

    def test_cli_rejects_a_header_registry_override_mismatch(self):
        registry = copy.deepcopy(late_order.load())
        registry['old_late_text'] = []
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'unit.json'
            path.write_text(json.dumps(registry), encoding='utf-8')
            result = subprocess.run([sys.executable, '-I', str(Path(late_order.__file__)), '--registry', str(path)],
                                    stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('old_late_text: registry and ctr_place.h named overrides differ', result.stdout)


if __name__ == '__main__':
    unittest.main()
