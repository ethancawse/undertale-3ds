"""Restore two UT Release string groups without changing code or references.

Ruling 2026-10-29, option A. The complete canonical pool fingerprint is
from the pre-UT-039 public engine TU, devkitARM GCC 16.1, ordinary Release.
Unknown strings/order or missing/duplicate anchors fail before assembly. The
linked ELF still requires the independent strict byte/layout proof.
"""
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

MARKER = '--colexact-compiler'
EXPECTED_COUNT = 1990
EXPECTED_POOL = '5cb6871dffeb77be6637c080637726207a32ba30e539c06b7a7f92728f4bcb87'
KEYS = ('distance_to_point', 'distance_to_object')
ANCHOR = 'ini_section_exists'
SECTION = re.compile(r'^[ \t]*\.section[ \t]+(\.rodata\.str[^,\s]+)', re.M)
STRINGS = re.compile(r'^[ \t]*\.(?:ascii|asciz|string)[ \t]+(.*)$', re.M)


def is_pool(chunk):
    return SECTION.match(chunk) is not None


def pool_strings(chunks):
    return [s for chunk in chunks if is_pool(chunk) for s in STRINGS.findall(chunk)]


def fingerprint(strings):
    return hashlib.sha256(('\n'.join(strings)+'\n').encode('utf-8')).hexdigest()


def restore_pool(text, expected_hash=EXPECTED_POOL, expected_count=EXPECTED_COUNT):
    chunks = re.split(r'(?=^[ \t]*\.section[ \t]+)', text, flags=re.M)
    before = pool_strings(chunks)
    found = {}
    for key in (*KEYS, ANCHOR):
        literal = '"'+key+r'\000"'
        indexes = [i for i, chunk in enumerate(chunks) if is_pool(chunk) and literal in STRINGS.findall(chunk)]
        assert len(indexes) == 1, ('missing/duplicate colexact pool anchor', key, indexes)
        index = indexes[0]
        # Move one entire aligned labeled string, never code/debug directives.
        match = re.fullmatch(r'[ \t]*\.section[ \t]+\.rodata\.str1\.4\n'
            r'[ \t]*\.align[ \t]+2\n(\.LC\d+):\n'
            r'[ \t]*\.ascii[ \t]+'+re.escape(literal)+r'\n', chunks[index])
        assert match, ('unqualified colexact string group', key)
        found[key] = (index, chunks[index], match[1])
    removed = {found[key][0] for key in KEYS}
    result = []
    for index, chunk in enumerate(chunks):
        if index == found[ANCHOR][0]:
            result.extend(found[key][1] for key in KEYS)
        if index not in removed:
            result.append(chunk)
    after = pool_strings(result)
    assert len(after) == len(before) == expected_count, ('unexpected string count', len(before), len(after))
    assert Counter(chunks) == Counter(result), 'string movement changed a section chunk'
    assert [c for c in chunks if not is_pool(c)] == [c for c in result if not is_pool(c)], 'non-pool assembly changed'
    digest = fingerprint(after)
    assert digest == expected_hash, ('unexpected canonical string pool', digest, expected_hash)
    return ''.join(result), dict(strings=len(after), before_sha256=fingerprint(before),
        after_sha256=digest, labels={key: found[key][2] for key in (*KEYS, ANCHOR)},
        restored_before=ANCHOR, changed=before != after)


def run(command):
    return subprocess.run(command, stdin=subprocess.DEVNULL, timeout=1800).returncode


def selected(command):
    sources = [a for a in command if a.replace('\\', '/').endswith('/vm_builtins.c')]
    required = ('-DENABLE_WAD16', '-DCTRPORT_BBOX_ROUND', '-DCTRPORT_LAYERFIX_RELEASE_LAYOUT')
    return len(sources) == 1 and all(a in command for a in required) and '-DUSE_FLOAT_REALS' not in command


def main(args):
    split = args.index(MARKER)
    delegate, command = [a for a in args[:split] if a], args[split+1:]
    if not selected(command):
        return run(delegate+command)
    assert '-c' in command and command.count('-o') == 1
    source = next(a for a in command if a.replace('\\', '/').endswith('/vm_builtins.c'))
    output = Path(command[command.index('-o')+1])
    # Side files next to the object, with names no longer than vm_builtins.o: CMake keeps object paths within
    # Windows' path limit, and devkitARM's assembler cannot open a longer one (a 92-character builder work folder
    # gave a 265-character vm_builtins.colexact-restored.s: "can't open ... No such file", integrate21 stage73).
    original = output.with_name('colexact0.s')
    restored = output.with_name('colexact1.s')
    emit = list(command)
    emit[emit.index('-c')] = '-S'
    emit[emit.index('-o')+1] = str(original)
    status = run(delegate+emit)
    if status:
        return status
    text, receipt = restore_pool(original.read_text(encoding='utf-8'))
    with open(restored, 'w', encoding='utf-8', newline='\n') as handle:  # write_text(newline=) needs Python 3.10
        handle.write(text)
    assemble, index = [command[0]], 1
    while index < len(command):
        arg = command[index]
        if arg in ('-MF', '-MT', '-MQ', '-o'):
            index += 2
            continue
        if arg not in ('-MD', '-MMD', '-MP', '-c') and arg != source:
            assemble.append(arg)
        index += 1
    assemble += ['-x', 'assembler', '-c', str(restored), '-o', str(output)]
    status = run(assemble)
    receipt.update(exit=status, original=str(original), restored=str(restored), source=source)
    output.with_name('colexact.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print('Colexact canonical pool: '+json.dumps(receipt), flush=True)
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
