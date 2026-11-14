"""Route only final port-menu memcpy relocations; no allocated object bytes change.

Keep all normal compiler/layout launchers, then append an undefined helper symbol
and retarget the qualified function's one branch relocation. Other memcpy callers,
including preview composition and diagnostic captures, keep their original edges.
"""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

MARKER = '--portmenu-compiler'
TARGET = 'CtrPortmenu_copy'
# Dev's Options path paints its thicker border before the final memcpy. Route
# that final copy, preserving both optCopyShown and the complete border painter.
CALLERS = {'z_menuborder.c': 'CtrMenuborder_copy', 'z_scanout.c': 'CtrScanout_noticeScan'}


def inventory(data):
    assert data[:7] == b'\x7fELF\x01\x01\x01', 'expected little-endian ELF32'
    header = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    assert header[1:3] == (1, 40), 'expected relocatable ARM object'
    sections = [struct.unpack_from('<10I', data, header[6] + header[11]*i) for i in range(header[12])]
    strings = sections[header[13]]
    names = data[strings[4]:strings[4]+strings[5]]
    def name(pool, offset):
        return pool[offset:].split(b'\0', 1)[0].decode()
    tables = [i for i, s in enumerate(sections) if s[1] == 2]
    assert len(tables) == 1, 'expected one symbol table'
    symindex = tables[0]
    symbols = sections[symindex]
    assert symbols[9] == 16
    strindex = symbols[6]
    strings = sections[strindex]
    pool = data[strings[4]:strings[4]+strings[5]]
    syms = [struct.unpack_from('<IIIBBH', data, off) for off in range(symbols[4], symbols[4]+symbols[5], 16)]
    allocated, edges = {}, []
    for s in sections:
        if s[2] & 2:
            key = name(names, s[0])
            assert key not in allocated
            allocated[key] = (s[1], s[2], s[3], s[5], s[8], data[s[4]:s[4]+s[5]] if s[1] != 8 else b'')
        if s[1] != 9:
            continue
        assert s[6] == symindex and s[9] == 8
        for off in range(s[4], s[4]+s[5], 8):
            address, info = struct.unpack_from('<II', data, off)
            symbol = syms[info >> 8]
            functions = [name(pool, f[0]) for f in syms if f[3] & 15 == 2 and f[5] == s[7]
                         and f[1] <= address < f[1]+f[2]]
            edges.append((off, s[7], address, info & 255, name(pool, symbol[0]), symbol[5], functions))
    return header, sections, symindex, strindex, syms, pool, allocated, edges


def rewrite(data, caller):
    h, sections, symindex, strindex, syms, pool, allocated, edges = inventory(data)
    selected = [e for e in edges if e[4] == 'memcpy' and any(n == caller or n.startswith(caller+'.') for n in e[6])]
    assert len(selected) == 1, ('unqualified menu copy inventory', caller, selected)
    edge, = selected
    assert edge[3] in (28, 29) and edge[5] == 0, ('unexpected menu copy relocation', edge)
    code = sections[edge[1]]
    instruction, = struct.unpack_from('<I', data, code[4]+edge[2])
    assert instruction & 0xfe000000 == 0xea000000, 'menu copy is not an unconditional ARM branch'
    assert TARGET.encode()+b'\0' not in pool, 'menu copy already routed'
    out = bytearray(data)
    # Append non-allocated tables rather than inserting into any existing section.
    newpool = pool + TARGET.encode()+b'\0'
    newpooloff = len(out)
    out.extend(newpool)
    out.extend(b'\0' * (-len(out) % 4))
    newsoff = len(out)
    symbols = sections[symindex]
    out.extend(data[symbols[4]:symbols[4]+symbols[5]])
    out.extend(struct.pack('<IIIBBH', len(pool), 0, 0, 0x10, 0, 0))
    struct.pack_into('<I', out, edge[0]+4, (len(syms) << 8) | edge[3])
    struct.pack_into('<I', out, h[6]+h[11]*strindex+16, newpooloff)
    struct.pack_into('<I', out, h[6]+h[11]*strindex+20, len(newpool))
    struct.pack_into('<I', out, h[6]+h[11]*symindex+16, newsoff)
    struct.pack_into('<I', out, h[6]+h[11]*symindex+20, symbols[5]+16)
    final = inventory(out)
    assert final[6] == allocated, 'allocated object bytes changed'
    expected = [(*e[:4], TARGET, 0, e[6]) if e == edge else e for e in edges]
    assert final[7] == expected, 'unqualified relocation changed'
    assert final[4][:-1] == syms, 'pre-existing symbols changed'
    return bytes(out), dict(caller=edge[6], old='memcpy', new=TARGET, relocation=edge[3],
                            allocated_bytes_changed=0, before_sha256=hashlib.sha256(data).hexdigest(),
                            after_sha256=hashlib.sha256(out).hexdigest())


def main(args):
    split = args.index(MARKER)
    delegate, command = [a for a in args[:split] if a], args[split+1:]
    status = subprocess.run(delegate+command, stdin=subprocess.DEVNULL, timeout=1800).returncode
    if status or '-c' not in command or '-DENABLE_WAD16' not in command or '-DENABLE_WAD17' in command:
        return status
    sources = [a for a in command if a.replace('\\', '/').rsplit('/', 1)[-1] in CALLERS]
    if not sources:
        return status
    assert len(sources) == 1 and command.count('-o') == 1
    output = Path(command[command.index('-o')+1])
    source = sources[0].replace('\\', '/').rsplit('/', 1)[-1]
    patched, receipt = rewrite(output.read_bytes(), CALLERS[source])
    output.write_bytes(patched)
    output.with_suffix('.portmenu.json').write_text(json.dumps(receipt, indent=2)+'\n', encoding='utf-8')
    print('Port menu copy: '+json.dumps(receipt), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
