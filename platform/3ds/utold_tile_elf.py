"""Minimal ARM ELF object inspection for the tile-plan slot verifier.

Public compiler output only; no game data, Git, or analysis-tree dependency.
"""
import hashlib
import re
import struct

def functions(dis):
    dis = dis.replace('\r\n', '\n')
    return {m[1]: m[2] for m in re.finditer(
        r'^[0-9a-f]+ <([^>]+)>:\n(.*?)(?=^[0-9a-f]+ <|^Disassembly of section |\Z)',
        dis, re.M | re.S)}


def tables(raw):
    assert raw[:7] == b'\x7fELF\x01\x01\x01'
    assert struct.unpack_from('<HH', raw, 16) == (1, 40)
    shoff = struct.unpack_from('<I', raw, 32)[0]
    width, count = struct.unpack_from('<HH', raw, 46)
    assert width == 40
    sections = [list(struct.unpack_from('<10I', raw, shoff+40*i)) for i in range(count)]
    si, = [i for i, s in enumerate(sections) if s[1] == 2]
    sy = sections[si]
    strings = sections[sy[6]]
    names = raw[strings[4]:strings[4]+strings[5]]
    symbols = {}
    for i, at in enumerate(range(sy[4], sy[4]+sy[5], 16)):
        entry = struct.unpack_from('<IIIBBH', raw, at)
        name = names[entry[0]:].split(b'\0', 1)[0].decode()
        if name:
            symbols[name] = (i, entry)
    return sections, si, symbols


def relocation_inventory(raw):
    sections, si, _ = tables(raw)
    st = sections[struct.unpack_from('<H', raw, 50)[0]]
    sn = raw[st[4]:st[4]+st[5]]
    def name(offset, strings):
        return strings[offset:].split(b'\0', 1)[0].decode()
    section_names = [name(s[0], sn) for s in sections]
    sy = sections[si]
    strings = sections[sy[6]]
    names = raw[strings[4]:strings[4]+strings[5]]
    symbols = []
    for at in range(sy[4], sy[4]+sy[5], 16):
        symbol = struct.unpack_from('<IIIBBH', raw, at)
        label = name(symbol[0], names)
        if symbol[3] & 15 == 3:
            label = section_names[symbol[5]]
        symbols.append(label)
    result = {}
    for s in sections:
        if s[1] != 9:
            continue
        if not sections[s[7]][2] & 2:  # Runtime allocated sections only, not DWARF.
            continue
        assert s[6] == si and s[9] == 8
        for at in range(s[4], s[4]+s[5], 8):
            offset, info = struct.unpack_from('<II', raw, at)
            key = section_names[s[7]], offset
            assert key not in result, key
            result[key] = (info & 255, symbols[info >> 8])
    return result


def snapshot(path):
    raw=path.read_bytes()
    sections,_,symbols=tables(raw)
    strings=sections[struct.unpack_from('<H',raw,50)[0]]
    names=raw[strings[4]:strings[4]+strings[5]]
    def section_name(s): return names[s[0]:].split(b'\0',1)[0].decode()
    result={}
    for name,(_,s) in symbols.items():
        if s[3]&15 not in (1,2) or not 0<s[5]<len(sections): continue
        section=sections[s[5]]
        start=section[4]+s[1]
        result[name]={'size':s[2],'offset':s[1],'section':section_name(section),
            'bytes':hashlib.sha256(raw[start:start+s[2]]).hexdigest() if section[1]!=8 else None}
    allocated={section_name(s):{'size':s[5],'sha256':hashlib.sha256(raw[s[4]:s[4]+s[5]]).hexdigest()}
        for s in sections if s[2]&2 and s[1]!=8 and s[5]}
    return result,allocated
