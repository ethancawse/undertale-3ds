"""Append a bounded audio unit while retaining every original allocated object byte.

The ordinary object first passes the complete compiler/layout adapter chain.
Only the named appended sections from a second compile enter the merged object.
Original static dependencies retain their binding, slot and STT_FILE identity.
Final executable layout, loaded words and ARM/Thumb stub gates remain mandatory.

Derived from the byte-qualified sfxlate TEST object merger, with no trace router
or runtime observer payload. This module ships with the corresponding source.
"""
from pathlib import Path
import hashlib
import json
import re
import struct
import subprocess

ARM = Path('C:/devkitPro/devkitARM/bin')
ALLOWED = {'.text_sfxlate', '.rodata_sfxlate'}


def run(command):
    return subprocess.run([str(x) for x in command], stdin=subprocess.DEVNULL,
                          check=True, timeout=900)


class Object:
    def __init__(self, path):
        self.data = bytearray(Path(path).read_bytes())
        h = struct.unpack_from('<16sHHIIIIIHHHHHH', self.data)
        assert h[0][:6] == b'\x7fELF\x01\x01' and h[1] == 1 and h[2] == 40
        self.sections = [list(struct.unpack_from('<10I', self.data, h[6] + i * h[11])) for i in range(h[12])]
        string = self.sections[h[13]]
        names = self.data[string[4]:string[4] + string[5]]
        self.names = [self.string(names, s[0]) for s in self.sections]
        self.symindex = next(i for i, s in enumerate(self.sections) if s[1] == 2)
        table = self.sections[self.symindex]
        strings = self.sections[table[6]]
        blob = self.data[strings[4]:strings[4] + strings[5]]
        self.symbols = []
        file_origin = ''
        for pos in range(table[4], table[4] + table[5], table[9]):
            name, value, size, info, other, section = struct.unpack_from('<IIIBBH', self.data, pos)
            name = self.string(blob, name)
            if info & 15 == 4:
                file_origin = name
            self.symbols.append(dict(name=name, value=value, size=size, file_origin=file_origin,
                                     info=info, section=section, position=pos))

    @staticmethod
    def string(blob, offset):
        return bytes(blob[offset:blob.index(0, offset)]).decode('utf-8')

    def contents(self, index):
        s = self.sections[index]
        return bytes(self.data[s[4]:s[4]+s[5]])

    def owner(self, section, offset):
        return sorted(s['name'] for s in self.symbols if s['section'] == section and
                      s['info'] & 15 == 2 and s['value'] <= offset < s['value'] + s['size'])


def selected_assembly(raw, allowed=ALLOWED):
    kept = []
    for chunk in re.split(r'(?=^[ \t]*\.section[ \t]+)', raw, flags=re.M):
        found = re.match(r'\s*\.section\s+([^,\n]+)', chunk)
        if found and found[1] in allowed:
            kept.append(chunk)
    assert {re.match(r'\s*\.section\s+([^,\n]+)', c)[1] for c in kept} == allowed
    prefix = raw.split('\t.type\t', 1)[0]
    attributes = [line for line in prefix.splitlines() if
                  re.match(r'^\s*\.(?:arch|cpu|fpu|eabi_attribute|syntax|arm)\b', line)]
    assert any('eabi_attribute 28' in line.replace('\t', ' ') for line in attributes), 'missing hard-float ABI attribute'
    text = '\n'.join(attributes) + '\n\t.syntax unified\n\t.arm\n' + ''.join(kept)
    assert not re.search(r'^\s*\.(?:loc|file)\b', text, re.M), 'appended assembly must be -g0'
    return text


def resolver_abi(original):
    """Fail closed on the reviewed GCC clone ABI, not merely its spelling.

    r1 is the filename (copied to r3); r0 is CtrAudioSystem (saved in r5).
    The filesystem is loaded from ca + 0x3c. Pin the complete instruction
    body and its relocations so intervening register/dataflow changes also
    require review. Literal-pool addends may change without changing this ABI.
    This is a host gate; it changes no native instructions.
    """
    name = 'resolveExternalPath.isra.0'
    found = [s for s in original.symbols if s['name'] == name and s['section']]
    assert len(found) == 1, ('resolver ABI: unique clone required', found)
    symbol = found[0]
    index = symbol['section']
    assert index < len(original.sections), 'resolver ABI: invalid section'
    section = original.sections[index]
    assert original.names[index] == '.text.' + name and section[2] & 6 == 6
    assert symbol['info'] & 15 == 2 and symbol['value'] == 0 and symbol['size'] == section[5] == 0x84, (
        'resolver ABI: ARM function extent changed', symbol)
    reviewed = (
        0xe2513000, 0x0a00001a, 0xe5d32000, 0xe3520000,
        0x0a000015, 0xe92d4030, 0xe3a0102e, 0xe24ddf83,
        0xe1a05000, 0xe1a00003, 0xe58d3004, 0xebfffffe,
        0xe28d4008, 0xe3500000, 0xe59d3004, 0x159f2038,
        0x059f2038, 0xe3a01c02, 0xe1a00004, 0xebfffffe,
        0xe595003c, 0xe1a01004, 0xe5903000, 0xe5933000,
        0xe12fff33, 0xe28ddf83, 0xe8bd8030, 0xe1a00002,
        0xe12fff1e, 0xe1a00003, 0xe12fff1e,
    )
    body = original.contents(index)[:0x7c]
    assert body == struct.pack('<31I', *reviewed), (
        'resolver ABI: instruction/dataflow changed; re-review r0=ca (+0x3c), r1=file')
    relocations = []
    for rel in original.sections:
        if rel[1] not in (4, 9) or rel[7] != index:
            continue
        assert rel[1] == 9 and rel[6] == original.symindex and rel[9] == 8, 'resolver ABI: unexpected relocation table'
        for position in range(rel[4], rel[4] + rel[5], rel[9]):
            offset, info = struct.unpack_from('<II', original.data, position)
            target = original.symbols[info >> 8]
            if target['info'] & 15 == 3:
                assert target['section'] < len(original.sections)
                target_name = original.names[target['section']]
                assert original.sections[target['section']][2] & 3 == 2, 'resolver ABI: writable literal target'
            else:
                target_name = target['name']
            relocations.append((offset, info & 255, target_name))
    assert sorted(relocations) == [(0x2c, 28, 'strchr'), (0x4c, 28, 'snprintf'),
                                   (0x7c, 2, '.rodata.str1.4'), (0x80, 2, '.rodata.str1.4')], (
        'resolver ABI: relocation targets changed', relocations)
    return dict(symbol=name, arguments=['r0=CtrAudioSystem*', 'r1=const char* file'],
                filesystem_offset_bytes=60, checked_instruction_bytes=len(body),
                instruction_sha256=hashlib.sha256(body).hexdigest(), relocations=sorted(relocations))


def restore_file_blocks(path, original, restored):
    """Put relocalized dependencies back under their original STT_FILE marker.

    objcopy places newly localized globals after the appended unit's FILE marker.
    Reorder only symbol-table entries, remapping every linked relocation; no
    allocated section or relocation target changes here.
    """
    current = Object(path)
    origins = {s['name']: s['file_origin'] for s in original.symbols if s['name'] in restored}
    moved = {i for i, s in enumerate(current.symbols) if s['name'] in origins and s['info'] >> 4 == 0}
    assert len(moved) == len(restored)
    order = []
    inserted = set()
    for i, symbol in enumerate(current.symbols):
        if i in moved:
            continue
        order.append(i)
        if symbol['info'] & 15 == 4:
            batch = sorted(j for j in moved if origins[current.symbols[j]['name']] == symbol['name'])
            assert not inserted.intersection(batch), 'ambiguous original FILE markers'
            order.extend(batch)
            inserted.update(batch)
    assert inserted == moved and sorted(order) == list(range(len(current.symbols)))
    remap = {old: new for new, old in enumerate(order)}
    table = current.sections[current.symindex]
    entries = [bytes(current.data[s['position']:s['position'] + table[9]]) for s in current.symbols]
    current.data[table[4]:table[4] + table[5]] = b''.join(entries[i] for i in order)
    assert all(current.symbols[i]['info'] >> 4 == 0 for i in order[:table[7]])
    assert all(current.symbols[i]['info'] >> 4 != 0 for i in order[table[7]:])
    for section in current.sections:
        assert section[1] not in (5, 17, 0x6ffffff6), 'unexpected symbol-indexed section'
        if section[1] not in (4, 9) or section[6] != current.symindex:
            continue
        for pos in range(section[4], section[4] + section[5], section[9]):
            info = struct.unpack_from('<I', current.data, pos + 4)[0]
            struct.pack_into('<I', current.data, pos + 4, (remap[info >> 8] << 8) | (info & 255))
    Path(path).write_bytes(current.data)
    checked = Object(path)
    for old, new in remap.items():
        before, after = current.symbols[old], checked.symbols[new]
        assert all(before[key] == after[key] for key in ('name', 'value', 'size', 'info', 'section'))
    for section in current.sections:
        if section[1] not in (4, 9) or section[6] != current.symindex:
            continue
        for pos in range(section[4], section[4] + section[5], section[9]):
            expected = struct.unpack_from('<II', current.data, pos)
            assert struct.unpack_from('<II', checked.data, pos) == expected


def merge(baseline, unit, output, folder, *, allowed=ALLOWED, prefix='sfxlate'):
    original = Object(baseline)
    observed = Object(unit)
    defined = {s['name'] for s in original.symbols if s['section'] and s['name']}
    required = {s['name'] for s in observed.symbols if s['section'] == 0 and s['name']}
    exports = sorted(required & defined)
    assert 'resolveExternalPath.isra.0' in exports
    clone_proof = resolver_abi(original)
    assert 'resolveExternalPath' not in required
    assert not any(name.startswith(('.L', '$')) for name in required), required
    allocated = {observed.names[i] for i, s in enumerate(observed.sections) if s[2] & 2 and s[5]}
    assert allocated == allowed, allocated
    objectcopy = folder / (prefix + '-baseline-globals.o')
    command = [ARM / 'arm-none-eabi-objcopy.exe']
    command += ['--globalize-symbol=' + name for name in exports]
    run(command + [baseline, objectcopy])
    merged = folder / (prefix + '-merged.o')
    run([ARM / 'arm-none-eabi-ld.exe', '-r', '--unique', objectcopy, unit, '-o', merged])
    # The appended unit and its original dependencies now share one relocatable
    # object. Restore original local bindings before the executable link, so
    # nm kinds and STT_FILE identities remain those of the frozen object.
    restored = sorted(s['name'] for s in original.symbols
                      if s['name'] in exports and s['info'] >> 4 == 0)
    localized = folder / (prefix + '-localized.o')
    run([ARM / 'arm-none-eabi-objcopy.exe'] +
        ['--localize-symbol=' + name for name in restored] + [merged, localized])
    merged = localized
    restore_file_blocks(merged, original, restored)
    final = Object(merged)
    symbol_checks = 0
    for symbol in original.symbols:
        if not symbol['name'] or symbol['section'] in (0, 0xfff1) or symbol['info'] & 15 not in (1, 2):
            continue
        section_name = original.names[symbol['section']]
        matches = [s for s in final.symbols if s['name'] == symbol['name'] and
                   s['section'] < len(final.sections) and final.names[s['section']] == section_name and
                   (s['value'], s['size'], s['info']) == (symbol['value'], symbol['size'], symbol['info'])]
        assert len(matches) == 1, ('original symbol binding/slot', symbol['name'], matches)
        if symbol['info'] >> 4 == 0:
            assert matches[0]['file_origin'] == symbol['file_origin'], (
                'original local FILE identity', symbol['name'], symbol['file_origin'], matches[0]['file_origin'])
        symbol_checks += 1
    # Relocatable linking must not merge/reorder/grow any original input slot.
    # Existing bytes must remain identical here. The unit's original static
    # dependencies are resolved in place, with no rewritten audio call sites.
    for i, section in enumerate(original.sections):
        if not section[2] & 2 or not section[5]:
            continue
        matches = [j for j, name in enumerate(final.names) if name == original.names[i] and final.sections[j][5]]
        assert len(matches) == 1, ('original slot inventory', original.names[i], matches)
        j = matches[0]
        assert section[5] == final.sections[j][5] and section[8] == final.sections[j][8], original.names[i]
        if section[1] != 8:
            assert original.contents(i) == final.contents(j), ('original machine/data bytes changed before final link', original.names[i])
    output.write_bytes(merged.read_bytes())
    receipt = dict(test_only=False, baseline_sha256=hashlib.sha256(Path(baseline).read_bytes()).hexdigest(),
                   unit_sha256=hashlib.sha256(Path(unit).read_bytes()).hexdigest(),
                   exports=exports, restored_local_bindings=restored,
                   resolver_abi=clone_proof,
                   original_symbol_binding_checks=symbol_checks,
                   original_local_file_identities_identical=True,
                   edits=[], original_allocated_bytes_identical=True, final_link_gate_required=True)
    with (folder / (prefix + '-relocations.json')).open('w', encoding='utf-8', newline='\n') as stream:
        stream.write(json.dumps(receipt, indent=2) + '\n')
    print('SFXLATE product object: original allocated bytes identical; 0 rewritten audio relocations;',
          len(exports), 'symbol exports; final link/layout/loaded-word gates remain', flush=True)
