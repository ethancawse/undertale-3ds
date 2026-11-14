"""Shared late-input order for tooling/tests; never generates or changes linker inputs.

Append selectors to late-order.json and late_reserve.ld in the same merge order.
Run this file to check all four lists and the two reusable AAC slot input lists, and the dev-only contract
("devonly": dev builds' AAC tail selectors, the late selectors their late script drops, and the P5-1 pad).
"""
import argparse
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
    if value.get('schema') != 1 or set(value) != {'schema', *KINDS, *AAC_KINDS, 'devonly'}:
        raise ValueError('late-order.json: expected schema 1, four late lists, two AAC lists and devonly')
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
    """Exact names in the ordered CMake list, before generated linker selectors."""
    if kind not in ('text', 'rodata'):
        raise ValueError('unknown AAC slot kind: ' + kind)
    text = re.sub(r'#[^\n]*', '', text)
    blocks = re.findall(r'^set\(aacout_' + kind + r'_inputs[ \t]*\n(.*?)^\)', text, re.M | re.S)
    if len(blocks) != 1:
        raise ValueError('expected one aacout_' + kind + '_inputs list')
    holes = re.findall(r'^set\(aacout_' + kind + r'_hole_inputs[ \t]*\n(.*?)^\)', text, re.M | re.S)
    if len(holes) > 1:
        raise ValueError('duplicate AAC hole input list')
    return (holes[0].split() if holes else []) + blocks[0].split()


def check_aac(text, registry):
    errors = []
    for kind in AAC_KINDS:
        actual, expected = aac_inputs(text, kind[4:]), registry[kind]
        if actual != expected:
            errors.append('late-order.json %s: aacout.cmake has %r; registry expects %r. '
                          'Update the single registry and CMake list in merge order.' % (kind, actual, expected))
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--registry', type=Path, default=REGISTRY)
    parser.add_argument('--ld', type=Path, default=LD)
    parser.add_argument('--aac', type=Path, default=AAC)
    args = parser.parse_args()
    registry = load(args.registry)
    errors = check(args.ld.read_text(encoding='utf-8'), registry)
    errors += check_aac(args.aac.read_text(encoding='utf-8'), registry)
    errors += check_devonly(args.aac.read_text(encoding='utf-8'), CMAKE.read_text(encoding='utf-8'),
                            args.ld.read_text(encoding='utf-8'), registry)
    for error in errors:
        print('FAIL:', error)
    if not errors:
        print('PASS: late-order.json matches all four linker input lists (%d selectors) '
              'and both AAC slot lists (%d inputs).' %
              (sum(len(registry[kind]) for kind in KINDS), sum(len(registry[kind]) for kind in AAC_KINDS)))
    return int(bool(errors))


if __name__ == '__main__':
    raise SystemExit(main())
