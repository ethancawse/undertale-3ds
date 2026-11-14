"""Append-only overworld continuations: three exact call routes.

Runs after the quiz adapter. GCC sees the original source and call graph for
every existing unit; objcopy changes only qualified symbol references/bindings.
"""
from pathlib import Path
import hashlib
import json
import re
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from touchquiz_compile import inventory

MARKER = '--touchoverworld-compiler'
ROUTES = {
    'z_battletouch.c': ('Battletouch_take2', '.text_battletouch2', 'CtrTouchQuiz_buttons', 'To_buttons'),
    'z_touchquiz.c': ('CtrTouchQuiz_claim', '.aacout.text.touchquiz', 'CtrTouchExt_sample', 'To_sample'),
    'z_touchmute.c': ('__wrap_VM_registerBuiltin', '.touchui_text', '__real_VM_registerBuiltin', 'To_register'),
}


def symbols(data):
    h = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    sections = [struct.unpack_from('<10I', data, h[6] + h[11] * i) for i in range(h[12])]
    names = sections[h[13]]
    shnames = data[names[4]:names[4] + names[5]]
    def name(pool, at):
        return pool[at:pool.index(b'\0', at)].decode()
    result = {}
    for section in sections:
        if section[1] != 2:
            continue
        strings = sections[section[6]]
        pool = data[strings[4]:strings[4] + strings[5]]
        for at in range(section[4], section[4] + section[5], section[9]):
            sn, value, size, info, other, index = struct.unpack_from('<IIIBBH', data, at)
            n = name(pool, sn)
            if not n or n.startswith('$') or info & 15 in (3, 4):
                continue
            assert n not in result, ('duplicate symbol', n)
            result[n] = (value, size, info, other, name(shnames, sections[index][0]) if index < len(sections) else index)
    return result


def qualify(source, allocated, edges, syms):
    caller, section, old, new = ROUTES[source]
    selected = [e for e in edges if e[3] in (old, new)]
    assert len(selected) == 1 and selected[0][3] == old, ('unqualified continuation edges', source, selected)
    lo, size, _, _, actual_section = syms[caller]
    sec, at, kind, _ = selected[0]
    assert actual_section == sec == section and lo <= at < lo + size, ('wrong continuation caller', source)
    word = struct.unpack_from('<I', allocated[sec][-1], at)[0]
    assert (kind == 28 and word >> 24 == 0xeb) or (kind == 29 and word >> 24 == 0xea), 'Not ARM BL/B'
    return selected


def run(args):
    return subprocess.run(args, stdin=subprocess.DEVNULL, timeout=1800).returncode


def main(args):
    split = args.index(MARKER)
    delegate, command = [a for a in args[:split] if a], args[split + 1:]
    source = next((Path(a.replace('\\', '/')).name for a in command
        if Path(a.replace('\\', '/')).name in ROUTES), None)
    status = run(delegate + command)
    if status or source is None or '-c' not in command or '-DENABLE_WAD16' not in command:
        return status
    assert command.count('-o') == 1
    output = Path(command[command.index('-o') + 1])
    before = output.read_bytes()
    allocated, edges, _ = inventory(before)
    old_syms = symbols(before)
    selected = qualify(source, allocated, edges, old_syms)
    compiler = Path(command[0])
    tool, count = re.subn(r'gcc(\.exe)?$', r'objcopy\1', compiler.name)
    assert count == 1, ('unqualified compiler', compiler)
    _, _, old, new = ROUTES[source]
    options = ['--redefine-sym', old + '=' + new]
    route = {old: new}
    status = run([str(compiler.with_name(tool)), *options, str(output)])
    if status:
        return status
    after = output.read_bytes()
    final_allocated, final_edges, _ = inventory(after)
    assert allocated == final_allocated, 'Existing allocated bytes/size/alignment changed'
    assert final_edges == sorted((*e[:3], route.get(e[3], e[3])) for e in edges), 'Unexpected relocation change'
    expected_symbols = {route.get(n, n): s for n, s in old_syms.items()}
    assert symbols(after) == expected_symbols, 'Unexpected symbol change'
    receipt = dict(source=source, before_sha256=hashlib.sha256(before).hexdigest(),
        after_sha256=hashlib.sha256(after).hexdigest(), routes=selected,
        allocated_bytes_changed=0)
    output.with_suffix('.touchoverworld.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print('Overworld object continuation: ' + json.dumps(receipt), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
