"""Fail closed on native AAC attributes that could become DELTARUNE orphans.

Use a simple explicit ENABLE_WAD16 guard even in UT-only native files. This
lexical gate deliberately does not infer CMake source membership or evaluate
arbitrary preprocessor expressions. Full target layout remains the backstop.
"""
from pathlib import Path
import re

import pytest

ROOT = Path(__file__).resolve().parents[2]
# Keep quoted strings intact and preserve comment newlines for diagnostics.
TOKENS = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
DIRECTIVE = re.compile(r'^\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)')


def guard_value(directive, condition):
    """Return positive/negative guarantees; unknown expressions grant neither."""
    condition = condition.strip()
    if directive in ('ifdef', 'ifndef'):
        return ((directive == 'ifdef'), (directive == 'ifndef')) if condition == 'ENABLE_WAD16' else (False, False)
    defined = r'defined(?:\s*\(\s*ENABLE_WAD16\s*\)|\s+ENABLE_WAD16)'
    return (bool(re.fullmatch(defined, condition)),
            bool(re.fullmatch(r'!\s*' + defined, condition)))


def unguarded_slots(source):
    source = TOKENS.sub(lambda match: '\n' * match[0].count('\n')
                        if match[0].startswith(('//', '/*')) else match[0], source)
    # Join preprocessor continuations without losing original line numbers.
    logical = []
    pending, first = '', 1
    for line_number, line in enumerate(source.splitlines(), 1):
        if not pending:
            first = line_number
        pending += line[:-1] if line.endswith('\\') else line
        if not line.endswith('\\'):
            logical.append((first, pending))
            pending = ''
    if pending:
        logical.append((first, pending))
    stack, failures = [], []
    for line_number, line in logical:
        directive = DIRECTIVE.match(line)
        if directive:
            kind, condition = directive.groups()
            if kind in ('if', 'ifdef', 'ifndef'):
                stack.append(guard_value(kind, condition))
            elif kind == 'elif':
                stack[-1] = guard_value(kind, condition)
            elif kind == 'else':
                stack[-1] = (stack[-1][1], False)
            else:
                stack.pop()
        # Also catches section-name macros. A macro defined only in UT cannot
        # silently supply a DR section attribute, even if used outside its guard.
        if '.aacout' in line and not any(positive for positive, _ in stack):
            failures.append(line_number)
    return failures


@pytest.mark.parametrize('before,after,allowed', [
    ('', '', False),
    ('#ifdef OTHER\n', '#endif\n', False),
    ('#ifdef ENABLE_WAD16\n', '#endif\n', True),
    ('#if defined ( ENABLE_WAD16 )\n', '#endif\n', True),
    ('#if defined ENABLE_WAD16\n', '#endif\n', True),
    ('#if definedENABLE_WAD16\n', '#endif\n', False),
    ('#if !definedENABLE_WAD16\n#else\n', '#endif\n', False),
    ('#if defined(ENABLE_WAD16) || defined(OTHER)\n', '#endif\n', False),
    ('#ifndef ENABLE_WAD16\n', '#endif\n', False),
    ('#ifndef ENABLE_WAD16\n#else\n', '#endif\n', True),
    ('#ifdef ENABLE_WAD16\n#else\n', '#endif\n', False),
    ('#ifdef ENABLE_WAD16\n#elif OTHER\n', '#endif\n', False),
    ('#if OTHER\n#elif defined(ENABLE_WAD16)\n', '#endif\n', True),
    ('#ifdef ENABLE_WAD16\n#if OTHER\n#else\n', '#endif\n#endif\n', True),
    ('#ifdef OTHER\n#ifdef ENABLE_WAD16\n#endif\n', '#endif\n', False),
    ('/*\n#ifdef ENABLE_WAD16\n*/\n', '', False),
    ('#if defined(\\\n ENABLE_WAD16)\n', '#endif\n', True),
])
@pytest.mark.parametrize('attribute', [
    '__attribute__((section(".aacout.text.fixture"))) void fixture(void) {}\n',
    '#define SLOT ".aacout.rodata.fixture"\n',
])
def test_shared_slot_guard_branches(before, after, allowed, attribute):
    assert bool(unguarded_slots(before + attribute + after)) != allowed


def test_comments_do_not_register_tenants():
    assert unguarded_slots('/* .aacout.text.example */\n// .aacout.rodata.example\n') == []


def test_native_sources_cannot_silently_add_dr_aac_orphans():
    failures = []
    for directory in ('engine/src', 'engine/include', 'platform/3ds/source'):
        for source in sorted((ROOT / directory).rglob('*')):
            if source.suffix in ('.c', '.h', '.inc'):
                failures.extend(f'{source.relative_to(ROOT)}:{line}: AAC section requires an explicit ENABLE_WAD16 guard'
                                for line in unguarded_slots(source.read_text(encoding='utf-8')))
    assert not failures, '\n'.join(failures)
