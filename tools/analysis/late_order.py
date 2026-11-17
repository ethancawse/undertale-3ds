"""Shared late-input order for tooling/tests; never generates or changes linker inputs.

Append selectors to late-order.json and late_reserve.ld in the same merge order.
Run this file to check all four lists and the two reusable AAC slot input lists, the dev-only contract
("devonly": dev builds' AAC tail selectors, the late selectors their late script drops, and the P5-1 pad), and the
per-model units ("per_model": each unit placed through platform/3ds/source/ctr_place.h has both its AAC and late
names, and the sources use the macros, never the names by hand).
The source scan for names by hand is an early lint: it fails on what it reads plainly and warns on pieces a macro
may join. The gate for that rule is late_floor_gate.py's Old map check, which reads the section names the compiler
emitted after every macro, escape and paste (review decision on c26rev8 r2).
"""
import argparse
import bisect
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
REGISTRY = Path(__file__).with_name('late-order.json')
LD = ROOT / 'platform/3ds/late_reserve.ld'
AAC = ROOT / 'platform/3ds/aacout.cmake'
CMAKE = ROOT / 'platform/3ds/CMakeLists.txt'
KINDS = ('text', 'rodata', 'data', 'bss')
AAC_KINDS = ('aac_text', 'aac_rodata')
# Per-model placement (review decision 2026-11-14, audit F3-1): platform/3ds/source/ctr_place.h's macros put a unit's code
# in .aacout.text.<unit> on UNDERTALE Old and .text_<unit> elsewhere, and its constants in .aacout.rodata.<unit> on
# UNDERTALE and .rodata_<unit> in DELTARUNE. Each kind a unit uses needs both names registered: one is linked on
# each model, the other matches nothing there.
UNIT = re.compile(r'[A-Za-z0-9_]+')
PLACE_HEADER = 'platform/3ds/source/ctr_place.h'
PLACE_KINDS = ('text', 'rodata')
PLACE_MACRO = {'text': 'UT_COLD_TEXT', 'rodata': 'UT_COLD_RODATA'}
SOURCE_DIRS = ('platform/3ds/source', 'engine/src')
SOURCE_SUFFIXES = ('.c', '.h', '.inc', '.cpp')
C_LEXEME = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)
# UT_COLD_TEXT_CONST(unit) is the text kind too: constants in the unit's text name (ctr_place.h).
PLACE_USE = re.compile(r'\bUT_COLD_(TEXT_CONST|TEXT|RODATA)\b\s*(?:\(\s*([^()]*?)\s*\))?')


def linker_inputs(text, kind):
    """Read selectors from one output section, ignoring comments and reserve arithmetic."""
    if kind not in KINDS:
        raise ValueError('unknown late section kind: ' + kind)
    text = re.sub(r'/\*.*?\*/', lambda m: '\n' * m[0].count('\n'), text, flags=re.S)
    match = re.search(r'^  \.late\.' + kind + r' \. :\s*\{(.*?)^  \}', text, re.M | re.S)
    if not match:
        raise ValueError('missing .late.' + kind + ' output section')
    result, pending = [], []
    for line in match[1].splitlines():
        line = line.strip()
        if pending or line.startswith(('*', 'KEEP(')):
            pending.append(line)
            selector = ' '.join(pending)
            if selector.count('(') == selector.count(')'):
                result.append(selector)
                pending = []
    if pending:
        raise ValueError('unterminated late selector: ' + ' '.join(pending))
    return result


def load(path=REGISTRY):
    value = json.loads(Path(path).read_text(encoding='utf-8'))
    if value.get('schema') != 1 or set(value) - {'old_late_text'} != {'schema', *KINDS, *AAC_KINDS, 'per_model', 'devonly'}:
        raise ValueError('late-order.json: expected schema 1, four late lists, two AAC lists, per_model and devonly')
    units = value['per_model']
    if not isinstance(units, list) or any(not isinstance(u, str) or not UNIT.fullmatch(u) for u in units) or (
            len(set(units)) != len(units)):
        raise ValueError('late-order.json: per_model is a list of distinct unit names ([A-Za-z0-9_]+)')
    overrides = value.get('old_late_text', [])
    if not isinstance(overrides, list) or any(not isinstance(u, str) or u not in units for u in overrides) or len(set(overrides)) != len(overrides):
        raise ValueError('late-order.json: old_late_text is a list of distinct per_model unit names')
    for kind in KINDS:
        entries = value[kind]
        if not isinstance(entries, list) or not entries or any(
                not isinstance(entry, str) or not entry.startswith(('*', 'KEEP(')) for entry in entries):
            raise ValueError('late-order.json: invalid ' + kind + ' selectors')
    for kind in AAC_KINDS:
        entries = value[kind]
        if not isinstance(entries, list) or any(not isinstance(entry, str) or not re.fullmatch(
                r'\.aacout\.' + kind[4:] + r'\.[A-Za-z0-9_.]+', entry) for entry in entries):
            raise ValueError('late-order.json: invalid ' + kind + ' input names')
    dev = value['devonly']
    if not isinstance(dev, dict) or set(dev) != set(DEVONLY_KEYS) or any(
            not isinstance(dev[k], list) or not dev[k] or any(not isinstance(e, str) for e in dev[k])
            for k in ('aac_text', 'aac_rodata', 'late_removed')):
        raise ValueError('late-order.json: invalid devonly contract')
    for kind in AAC_KINDS:
        for entry in dev[kind]:
            # Dev-only inputs are never production tenants: no .aacout name, no KEEP, one object or every object.
            if not re.fullmatch(r'\*(?:/[A-Za-z0-9_]+\.o)?\((?:\.[A-Za-z0-9_.*]+ ?)+\)', entry) or '.aacout' in entry:
                raise ValueError('late-order.json: invalid devonly ' + kind + ' selector ' + entry)
    return value


DEVONLY_KEYS = ('aac_text', 'aac_rodata', 'late_removed', 'late_replaced', 'p51_pad')


def devonly_aac(text, kind):
    """aacout.cmake's dev-only selector list (quoted items), not aacout_<kind>_inputs."""
    text = re.sub(r'#[^\n]*', '', text)
    blocks = re.findall(r'^set\(aacout_devonly_' + kind + r'[ \t]*\n(.*?)^\)', text, re.M | re.S)
    if len(blocks) != 1:
        raise ValueError('expected one aacout_devonly_' + kind + ' list')
    return re.findall(r'"([^"]+)"', blocks[0])


def devonly_late(cmake):
    """CMakeLists.txt's dropped late lines (the foreach over devonly_line), as late selectors."""
    blocks = re.findall(r'foreach\(devonly_line\n(.*?)\)\n', cmake, re.S)
    if len(blocks) != 1:
        raise ValueError('expected one foreach(devonly_line ...) in CMakeLists.txt')
    lines = re.findall(r'"    ([^"]+)\\n"', blocks[0])
    if len(lines) != blocks[0].count('"') // 2:
        raise ValueError('CMakeLists.txt devonly_line: every item is "    <selector>\\n"')
    return lines


def check_devonly(aac, cmake, ld, registry):
    """The dev-only contract: aacout.cmake's lists, CMakeLists.txt's dropped/replaced late lines and the P5-1 pad
    against the registry; the dropped lines are real late selectors, each once."""
    dev, errors = registry['devonly'], []
    for kind in ('text', 'rodata'):
        actual = devonly_aac(aac, kind)
        if actual != dev['aac_' + kind]:
            errors.append('late-order.json devonly aac_%s: aacout.cmake has %r; registry expects %r'
                          % (kind, actual, dev['aac_' + kind]))
        production = aac_inputs(aac, kind)
        if any('.aacout' in entry or entry in production for entry in actual):
            errors.append('devonly aac_%s names a production AAC input' % kind)
    late = devonly_late(cmake)
    if late != dev['late_removed']:
        errors.append('late-order.json devonly late_removed: CMakeLists.txt has %r; registry expects %r'
                      % (late, dev['late_removed']))
    selectors = [s for kind in KINDS for s in registry[kind]]
    for line in dev['late_removed']:
        if selectors.count(line) != 1 or ld.count('    ' + line + '\n') != 1:
            errors.append('devonly late_removed %s: not exactly one late selector' % line)
    for old, new in dev['late_replaced'].items():
        if old not in dev['late_removed'] or ('"    ' + new + '\\n"') not in cmake:
            errors.append('devonly late_replaced %s: CMakeLists.txt does not put back %s' % (old, new))
    pad = dev['p51_pad']
    pair = ''.join('    ' + s + '\n' for s in pad['after'])
    if ld.count(pair) != 1:
        errors.append('devonly p51_pad: late_reserve.ld has no single %r' % pair)
    found = re.findall(r'__devonly_p51_pad_start = ABSOLUTE\(\.\);\\n    \. = ALIGN\((\d+)\);\\n    \. \+= (0x[0-9a-f]+);'
                       r'\\n    \. \+= (0x[0-9a-f]+);\\n    __devonly_p51_pad_end = ABSOLUTE\(\.\);', cmake)
    if [(int(a), [int(b, 16), int(c, 16)]) for a, b, c in found] != [(pad['align'], pad['bytes'])]:
        errors.append('devonly p51_pad: CMakeLists.txt pad %r; registry expects align %d bytes %r'
                      % (found, pad['align'], pad['bytes']))
    return errors


def inputs(kind='text', start=None, stop=None, *, after=False, through=False, view='all'):
    """Registry slice, optionally using the old tests' raw-star or section-only view.

    Slicing uses exact selectors before filtering; KEEP entries retain their positions.
    """
    entries = load()[kind]
    first = entries.index(start) + int(after) if start is not None else 0
    last = entries.index(stop) + int(through) if stop is not None else len(entries)
    if first > last:
        raise ValueError('reversed late-order slice: %s .. %s' % (start, stop))
    entries = entries[first:last]
    if view == 'all':
        return entries
    if view == 'stars':
        return [entry for entry in entries if entry.startswith('*')]
    if view == 'bare':
        return [entry for entry in entries if entry.startswith('*(')]
    if view == 'sections':
        return [section for entry in entries for section in re.findall(r'\*\(([^)]+)\)', entry)]
    raise ValueError('unknown late-order view: ' + view)


def check(text, registry):
    """One actionable diagnostic for the first mismatch in each output section."""
    errors = []
    for kind in KINDS:
        actual, expected = linker_inputs(text, kind), registry[kind]
        for index in range(max(len(actual), len(expected))):
            got = actual[index] if index < len(actual) else '<end>'
            want = expected[index] if index < len(expected) else '<end>'
            if got != want:
                errors.append('late-order.json %s input %d: linker has %s; registry expects %s. '
                              'Update the single registry and ld in merge order.' % (kind, index + 1, got, want))
                break
    return errors


def aac_inputs(text, kind):
    """Exact names in emitted order (hole, production, release-only reclaim lists), before linker selectors."""
    if kind not in ('text', 'rodata'):
        raise ValueError('unknown AAC slot kind: ' + kind)
    text = re.sub(r'#[^\n]*', '', text)
    blocks = re.findall(r'^set\(aacout_' + kind + r'_inputs[ \t]*\n(.*?)^\)', text, re.M | re.S)
    if len(blocks) != 1:
        raise ValueError('expected one aacout_' + kind + '_inputs list')
    holes = re.findall(r'^set\(aacout_' + kind + r'_hole_inputs[ \t]*\n(.*?)^\)', text, re.M | re.S)
    if len(holes) > 1:
        raise ValueError('duplicate AAC hole input list')
    # Release-only reclaim inputs (N8a's rodata pair) are emitted after every production input (lead 11-16, N13 A).
    reclaim = re.findall(r'^set\(aacout_' + kind + r'_reclaim_inputs[ \t]*\n(.*?)^\)', text, re.M | re.S)
    if len(reclaim) > 1:
        raise ValueError('duplicate AAC reclaim input list')
    return (holes[0].split() if holes else []) + blocks[0].split() + (reclaim[0].split() if reclaim else [])


def check_aac(text, registry):
    errors = []
    for kind in AAC_KINDS:
        actual, expected = aac_inputs(text, kind[4:]), registry[kind]
        if actual != expected:
            errors.append('late-order.json %s: aacout.cmake has %r; registry expects %r. '
                          'Update the single registry and CMake list in merge order.' % (kind, actual, expected))
    return errors


def place_names(kind, unit):
    """(AAC input name, late section name) for one per-model unit and kind."""
    return '.aacout.%s.%s' % (kind, unit), '.%s_%s' % (kind, unit)


def section_token(name):
    return re.compile(r'(?<![A-Za-z0-9_.])' + re.escape(name) + r'(?![A-Za-z0-9_.])')


def read_sources(root=ROOT):
    """Source files that can use ctr_place.h, as {posix relative path: text}; the header itself is left out."""
    result = {}
    for top in SOURCE_DIRS:
        for path in sorted((Path(root) / top).rglob('*')):
            if path.suffix in SOURCE_SUFFIXES and path.is_file():
                rel = path.relative_to(root).as_posix()
                if rel != PLACE_HEADER:
                    result[rel] = path.read_text(encoding='utf-8', errors='replace')
    return result


def strip_comments(text):
    """C comments become blanks (newlines kept); string and character literals stay."""
    return C_LEXEME.sub(lambda m: m[0] if m[0][0] in '"\'' else re.sub(r'[^\n]', ' ', m[0]), text)


def blank(text):
    return re.sub(r'[^\n]', ' ', text)


C_ESCAPE = re.compile(r'\\(?:([0-7]{1,3})|x([0-9A-Fa-f]+)|u([0-9A-Fa-f]{4})|U([0-9A-Fa-f]{8})|(.))', re.S)
C_SIMPLE_ESCAPE = {'a': '\a', 'b': '\b', 'f': '\f', 'n': '\n', 'r': '\r', 't': '\t', 'v': '\v'}


def splice(text):
    """C translation phase 2 (c26rev8 F3R-4): every backslash-newline removed before anything is lexed, as the
    compiler removes it, inside an escape too. Returns (spliced text, the spliced offset of each removal)."""
    parts = re.split(r'\\\r?\n', text)
    at, joins = 0, []
    for part in parts[:-1]:
        at += len(part)
        joins.append(at)
    return ''.join(parts), joins


def line_of(text, joins, offset):
    """The source line of a spliced offset."""
    return text.count('\n', 0, offset) + 1 + bisect.bisect_right(joins, offset)


def decode_literal(body):
    """A C string literal's characters (body between the quotes, already spliced): every escape decoded."""
    def one(m):
        if m[1]:
            return chr(int(m[1], 8))
        if m[2] or m[3] or m[4]:
            return chr(min(int(m[2] or m[3] or m[4], 16), 0x10FFFF))
        return C_SIMPLE_ESCAPE.get(m[5], m[5])
    return C_ESCAPE.sub(one, body)


# A comment, or a string or character literal with an optional encoding prefix (not the tail of an identifier).
C_TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|(?<![A-Za-z0-9_])(?:u8|[uUL])?("(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\')',
                     re.S)


def lex(text):
    """(code, runs) of spliced text: code is text with comments and literals blanked (newlines kept), so a macro name
    inside a string is no use; runs are the string-literal sequences C joins (literals with only blanks or comments
    between them), each (start, end, decoded characters)."""
    code, runs, last, joinable = [], [], 0, False
    for m in C_TOKEN.finditer(text):
        between = text[last:m.start()]
        code += [between, blank(m[0])]
        last = m.end()
        if between.strip():
            joinable = False  # some other token since the last literal
        if not m[1]:
            continue  # a comment: blank, joins nothing apart
        if m[1][0] != '"':
            joinable = False  # a character literal
            continue
        value = decode_literal(m[1][1:-1])
        if joinable:
            runs[-1] = (runs[-1][0], m.end(), runs[-1][2] + value)
        else:
            runs.append((m.start(), m.end(), value))
        joinable = True
    code.append(text[last:])
    return ''.join(code), runs


def unparsed(code, start, end):
    """A string-literal sequence (in spliced code) the preprocessor may still join to something: next to an
    identifier (a macro) or a # (stringizing), or in a #define."""
    before = code[max(0, start - 4096):start].rstrip()
    after = code[end:end + 4096].lstrip()
    if before[-1:] == '#' or re.search(r'[A-Za-z0-9_]$', before) or re.match(r'[A-Za-z_#]', after):
        return True
    line = code.rfind('\n', 0, start) + 1
    return code[line:start].lstrip().startswith('#')


def check_per_model(registry, sources, warnings=None):
    """Every per-model unit has, for each kind it uses, its AAC name and exactly one late selector; every
    UT_COLD_TEXT/UT_COLD_RODATA use names such a unit and kind; every registered kind is used; and no source
    spells a registered unit's section names by hand.
    Each source is spliced (backslash-newlines removed) first, as C does. Uses count in code only (not in strings,
    characters or comments). A name by hand is an error when it is in code or in a string-literal sequence as C
    joins and decodes it (adjacent literals, comments between them, every escape). A literal the preprocessor may
    still join (next to a macro name or #, or in a #define) that is a piece of a registered name (its head through
    ".text_"/".aacout.text." or the rodata forms, or a tail holding the whole unit name) is appended to warnings:
    a lint cannot follow the preprocessor, so this is advisory. late_floor_gate.py's Old map check is the gate."""
    errors, used = [], set()
    units = registry['per_model']
    for unit in units:
        kinds = 0
        for kind in PLACE_KINDS:
            aac, late = place_names(kind, unit)
            has_aac = registry['aac_' + kind].count(aac)
            exact = {'*(%s)' % late, 'KEEP(*(%s))' % late}
            token = section_token(late)
            selectors = [(k, s) for k in KINDS for s in registry[k] if token.search(s)]
            has_late = sum(s in exact for s in registry[kind])
            if has_aac > 1:
                errors.append('per_model %s: %s is listed %d times in aac_%s' % (unit, aac, has_aac, kind))
            if any(k != kind or s not in exact for k, s in selectors) or has_late > 1:
                errors.append('per_model %s: %s must be one late %s selector, *(%s) or KEEP(*(%s)); registry has %r'
                              % (unit, late, kind, late, late, selectors))
            elif bool(has_aac) != bool(has_late):
                missing = late + ' in late ' + kind if has_aac else aac + ' in aac_' + kind
                errors.append('per_model %s: %s has one name but not the other; add %s. Old links one name and '
                              'New/DELTARUNE the other, so a missing name drops the code on that model.'
                              % (unit, kind, missing))
            kinds += bool(has_aac or has_late)
        if not kinds:
            errors.append('per_model %s: registers neither text nor rodata names' % unit)
    registered = {(kind, unit) for unit in units for kind in PLACE_KINDS
                  if place_names(kind, unit)[0] in registry['aac_' + kind]}
    # Each registered name, with its unit and the part before the unit (".text_", ".aacout.text.", ...).
    names = {name: (unit, name[:-len(unit)]) for unit in units for kind in PLACE_KINDS
             for name in place_names(kind, unit)}
    tokens = {name: section_token(name) for name in names}
    for rel, raw in sorted(sources.items()):
        if not units and 'UT_COLD_' not in raw:
            continue  # no unit to spell by hand, and no use to check
        # c26rev8 F3R-2/F3R-3: literals and comments are lexed as C lexes them. A macro name counts only in code (not
        # in a string), and a section name is looked for in each joined, escape-decoded string-literal sequence.
        spliced, joins = splice(raw)
        text, runs = lex(spliced)
        for match in PLACE_USE.finditer(text):
            line = line_of(text, joins, match.start())
            kind, unit = match[1].lower().removesuffix('_const'), match[2]
            if unit is None or not UNIT.fullmatch(unit):
                errors.append('%s:%d: UT_COLD_%s needs one plain unit name' % (rel, line, match[1]))
            elif (kind, unit) not in registered:
                errors.append('%s:%d: UT_COLD_%s(%s): unit %s has no registered %s pair in late-order.json per_model'
                              % (rel, line, match[1], unit, unit, kind))
            else:
                used.add((kind, unit))
        hand = [(match.start(), name) for name, token in tokens.items() for match in token.finditer(text)]
        for start, end, value in runs:
            whole = [name for name, token in tokens.items() if token.search(value)]
            hand += [(start, name) for name in whole]
            if whole:
                continue
            for name, (unit, head) in sorted(names.items()):
                if value != name and (name.startswith(value) and len(value) >= len(head)
                                      or name.endswith(value) and len(value) >= len(unit)) \
                        and unparsed(text, start, end):
                    (warnings if warnings is not None else []).append(
                        '%s:%d: %s may be a piece of %s joined by a macro, # or #define this lint cannot read; use '
                        'UT_COLD_%s(%s) (advisory: the Old map check decides)'
                        % (rel, line_of(text, joins, start), ascii(value), name,
                           'TEXT' if '.text' in head else 'RODATA', unit))
        for start, name in sorted(hand):
            errors.append('%s:%d: %s is spelled by hand; use the ctr_place.h macro so both models place it'
                          % (rel, line_of(text, joins, start), name))
    for kind, unit in sorted(registered - used):
        errors.append('per_model %s: %s is registered but no source uses UT_COLD_%s(%s)'
                      % (unit, kind, PLACE_MACRO[kind][8:], unit))
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--registry', type=Path, default=REGISTRY)
    parser.add_argument('--ld', type=Path, default=LD)
    parser.add_argument('--aac', type=Path, default=AAC)
    parser.add_argument('--root', type=Path, default=ROOT, help='tree whose sources are checked for ctr_place.h use')
    args = parser.parse_args()
    registry = load(args.registry)
    errors = check(args.ld.read_text(encoding='utf-8'), registry)
    errors += check_aac(args.aac.read_text(encoding='utf-8'), registry)
    errors += check_devonly(args.aac.read_text(encoding='utf-8'), CMAKE.read_text(encoding='utf-8'),
                            args.ld.read_text(encoding='utf-8'), registry)
    warnings = []
    errors += check_per_model(registry, read_sources(args.root), warnings)
    header_path = args.root / PLACE_HEADER
    header = header_path.read_text(encoding='utf-8') if header_path.is_file() else ''
    overrides = re.findall(r'^#define CTR_PLACE_OLD_OVERRIDE_(\w+) ', header, re.M)
    if sorted(overrides) != sorted(registry.get('old_late_text', [])):
        errors.append('old_late_text: registry and ctr_place.h named overrides differ')
    for error in errors:
        print('FAIL:', error)
    for warning in warnings:
        print('WARN:', warning)
    if not errors:
        print('PASS: late-order.json matches all four linker input lists (%d selectors), '
              'both AAC slot lists (%d inputs) and %d per-model unit(s).' %
              (sum(len(registry[kind]) for kind in KINDS), sum(len(registry[kind]) for kind in AAC_KINDS),
               len(registry['per_model'])))
    return int(bool(errors))


if __name__ == '__main__':
    raise SystemExit(main())
