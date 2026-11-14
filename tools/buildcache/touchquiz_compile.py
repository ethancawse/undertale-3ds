"""Route two shared-sampler call relocations without changing its compiled bytes.

GCC sees the original battletouch source and call graph. Only objcopy symbol
names change afterward. The mutable claim boundary lives in touchquiz's tail;
the original callback keeps its read-only ABI. A new reader/caller fails closed.
"""
from pathlib import Path
import hashlib
import json
import re
import struct
import subprocess
import sys

MARKER = '--touchquiz-compiler'
ROUTES = {'__real_CtrInput_take': 'CtrTouchQuiz_buttons', 'CtrTouchExt_sample': 'CtrTouchQuiz_claim'}


def inventory(data):
    assert data[:7] == b'\x7fELF\x01\x01\x01', 'expected little-endian ELF32'
    h = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    assert h[1:3] == (1, 40), 'expected relocatable ARM object'
    sections = [struct.unpack_from('<10I', data, h[6] + h[11]*i) for i in range(h[12])]
    sh = sections[h[13]]
    names = data[sh[4]:sh[4]+sh[5]]
    def name(pool, offset):
        return pool[offset:].split(b'\0', 1)[0].decode()
    allocated, edges, bounds = {}, [], []
    for sec in sections:
        secname = name(names, sec[0])
        if sec[2] & 2:
            assert secname not in allocated
            allocated[secname] = (sec[1], sec[2], sec[5], sec[8],
                                  data[sec[4]:sec[4]+sec[5]] if sec[1] != 8 else b'')
        if sec[1] == 2:
            strings = sections[sec[6]]
            pool = data[strings[4]:strings[4]+strings[5]]
            for offset in range(sec[4], sec[4]+sec[5], sec[9]):
                s = struct.unpack_from('<IIIBBH', data, offset)
                if name(pool, s[0]) == 'Battletouch_take2':
                    bounds.append((name(names, sections[s[5]][0]), s[1], s[1]+s[2]))
        if sec[1] != 9:
            continue
        syms = sections[sec[6]]
        strings = sections[syms[6]]
        pool = data[strings[4]:strings[4]+strings[5]]
        caller = name(names, sections[sec[7]][0])
        for offset in range(sec[4], sec[4]+sec[5], sec[9]):
            address, info = struct.unpack_from('<II', data, offset)
            s = struct.unpack_from('<IIIBBH', data, syms[4]+(info >> 8)*syms[9])
            edges.append((caller, address, info & 255, name(pool, s[0])))
    return allocated, sorted(edges), bounds


def selected_edges(allocated, edges, bounds):
    selected = [e for e in edges if e[3] in ROUTES or e[3] in ROUTES.values()]
    assert len(bounds) == 1 and bounds[0][0] == '.text_battletouch2', ('unqualified sampler', bounds)
    section, start, end = bounds[0]
    assert len(selected) == 2 and {e[3] for e in selected} == set(ROUTES), ('unqualified sampler edges', selected)
    for caller, at, kind, symbol in selected:
        assert caller == section and start <= at < end and kind == 28, ('unqualified sampler edge', symbol, caller, at, kind)
        word = struct.unpack_from('<I', allocated[caller][-1], at)[0]
        assert word >> 24 == 0xeb, ('sampler edge is not unconditional ARM BL', symbol, hex(word))
    return selected


def run(args):
    return subprocess.run(args, stdin=subprocess.DEVNULL, timeout=1800).returncode


def main(args):
    split = args.index(MARKER)
    delegate, command = [a for a in args[:split] if a], args[split+1:]
    selected = ('-c' in command and '-DENABLE_WAD16' in command and
                any(a.replace('\\', '/').endswith('/z_battletouch.c') for a in command))
    status = run(delegate + command)
    if status or not selected:
        return status
    assert command.count('-o') == 1
    output = Path(command[command.index('-o')+1])
    before = output.read_bytes()
    allocated, edges, bounds = inventory(before)
    routed = selected_edges(allocated, edges, bounds)
    compiler = Path(command[0])
    tool, count = re.subn(r'gcc(\.exe)?$', r'objcopy\1', compiler.name)
    assert count == 1, ('unqualified compiler', compiler)
    options = [a for old, new in ROUTES.items() for a in ('--redefine-sym', old+'='+new)]
    status = run([str(compiler.with_name(tool)), *options, str(output)])
    if status:
        return status
    after = output.read_bytes()
    final_allocated, final_edges, final_bounds = inventory(after)
    assert final_allocated == allocated, 'allocated object bytes changed'
    expected = sorted((*e[:3], ROUTES.get(e[3], e[3])) for e in edges)
    assert final_edges == expected and final_bounds == bounds, 'unexpected relocation or sampler change'
    receipt = dict(before_sha256=hashlib.sha256(before).hexdigest(), after_sha256=hashlib.sha256(after).hexdigest(),
                   edges=routed, routed_edges=[(*e[:3], ROUTES[e[3]]) for e in routed],
                   allocated_bytes_changed=0)
    output.with_suffix('.touchquiz.json').write_text(json.dumps(receipt, indent=2)+'\n', encoding='utf-8')
    print('Touchquiz sampler routes: '+json.dumps(receipt), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
