"""S4 + generic first-frame repair: decoded edges, zero movers and exact loaded-byte proof.

S4 occupies zero padding inside AAC's explicitly retired Frame hole. No old
symbol is waived. Each added AAC symbol must fit that independently delimited
hole, whose original loaded bytes must be zero. All other changed loaded bytes
must be a named branch operand, the build ID, or the ordinary appended repair.
"""
import argparse
from pathlib import Path
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_layout
from check_dlgflash_layout import one
from check_knsurf_layout import elf_read, branch


def prove(base_path, candidate_path, base_map=None, candidate_map=None):
    nm, readelf = check_layout.tool('nm', None), check_layout.tool('readelf', None)
    base = check_layout.load(base_path, nm, readelf, base_map)
    cand = check_layout.load(candidate_path, nm, readelf, candidate_map)
    before, after = base_path.read_bytes(), candidate_path.read_bytes()
    pairs = [('CtrDialogue_init', 'CtrDlgflash_init', 1),
             ('CtrLayout_yield8', 'CtrDlgflash_yield', 1),
             ('Runner_addInstanceToObjectLists', 'CtrDlgflash_registered', 4),
             ('CtrRenderer_beginFrame', 'CtrDlgtop_begin', 1),
             # GCC clones the early-return delegate: Old BL/B/BL, New BL/B.
             ('CtrRenderer_endFrame', 'CtrDlgtop_end',
              3 if 'CtrRenderer_chainEndFrameBoxcache' in {n for n, k in base.symbols} else 2),
             ('presentBottom', 'CtrDlgtop_present', 2),
             ('__wrap_CtrDimlight_setDisplay', 'CtrDlgtop_display', 3)]
    routes = {one(base, old)[0]: (one(cand, new)[0], count, old, new) for old, new, count in pairs}
    counts = {address: 0 for address in routes}
    patches = {}
    for key in base.symbols.keys() & cand.symbols.keys():
        assert base.symbols[key] == cand.symbols[key], ('moved/resized existing symbol', key)
        if key[1] not in 'tTwW': continue
        for address, size in base.symbols[key]:
            a, b = elf_read(before, address, size), elf_read(after, address, size)
            if a == b: continue
            assert size % 4 == 0, ('unaligned changed body', key)
            for offset in range(0, size, 4):
                wa, wb = struct.unpack_from('<I', a, offset)[0], struct.unpack_from('<I', b, offset)[0]
                if wa == wb: continue
                pc = address + offset
                assert wa >> 24 == wb >> 24 and wa >> 24 in (0xea, 0xeb), ('not a preserved ARM B/BL', key, hex(pc))
                old, new = branch(wa, pc), branch(wb, pc)
                assert old in routes and routes[old][0] == new, ('unexplained changed word', key, hex(pc), hex(old), hex(new))
                if routes[old][2] == 'CtrRenderer_endFrame':
                    owner = ('CtrRenderer_chainEndFrameBoxcache' if routes[old][1] == 3
                             else 'CtrBottomPaint_endFrame')
                    assert key[0] == owner, ('unexpected final end-frame caller', key, hex(pc), owner)
                if pc not in patches: counts[old] += 1
                patches[pc] = 4
    for old, (_, expected, old_name, new_name) in routes.items():
        assert counts[old] == expected, ('edge census', old_name, counts[old], expected)
        print('EDGE', old_name, '->', new_name, counts[old])

    # --wrap binds the old external display call to paintchg's owner adapter.
    # Both S4 branches must delegate through it, including the inactive branch.
    start, size = one(cand, 'CtrDlgtop_display')
    wrapped = one(cand, '__wrap_CtrDimlight_setDisplay')[0]
    unwrapped = one(cand, 'CtrDimlight_setDisplay')[0]
    delegates = []
    for offset in range(0, size, 4):
        word = struct.unpack('<I', elf_read(after, start+offset, 4))[0]
        if word >> 24 not in (0xea, 0xeb): continue
        target = branch(word, start+offset)
        assert target != unwrapped, 'S4 bypasses the existing paintchg display wrapper'
        if target == wrapped: delegates.append(start+offset)
    assert len(delegates) == 2, ('S4 active/inactive display delegates', delegates)
    print('DISPLAY: both S4 branches retain the existing paintchg wrapper')

    masks = list(patches.items())
    build_id, build_size = one(base, 'ctrportBuildId')
    assert one(cand, 'ctrportBuildId') == (build_id, build_size)
    masks.append((build_id, build_size))
    for name in ('__bss_start__', '__bss_end__', '__end__'):
        assert base.plain[name] == cand.plain[name], ('BSS changed', name)
    for kind in ('text', 'rodata', 'data'):
        lo, hi = base.plain[f'__late_{kind}_end'], cand.plain[f'__late_{kind}_end']
        assert hi >= lo, ('late shrink', kind)
        if kind != 'text': assert hi == lo, ('new non-code repair storage', kind)
        masks.append((lo, hi - lo))

    holes = []
    total = 0
    for kind, capacity in (('text', 9308), ('rodata', 748)):
        first, last = [f'__aacout_{kind}_retired_frame_{part}' for part in ('start', 'end')]
        start, end = base.plain[first], base.plain[last]
        assert (cand.plain[first], cand.plain[last]) == (start, end) and end - start == capacity
        lo, hi = [cand.plain[f'__aacout_{kind}_dlgtop4_{part}'] for part in ('start', 'end')]
        assert lo == start and lo <= hi <= end, ('S4 outside retired hole', kind)
        assert elf_read(before, start, end-start) == bytes(end-start), ('base retired hole not empty', kind)
        assert elf_read(after, hi, end-hi) == bytes(end-hi), ('unexpected hole suffix bytes', kind)
        total += hi-lo
        holes.append((lo, hi))
        masks.append((lo, hi-lo))
        print('S4 AAC', kind, hi-lo, 'B')
    assert total <= 6300, ('S4 exceeds AAC budget', total)
    added_aac = set()
    for (name, kind), values in cand.symbols.items():
        if (name, kind) in base.symbols: continue
        if all(size > 0 and any(lo <= address and address+size <= hi for lo, hi in holes) for address, size in values):
            added_aac.add(name)
    assert added_aac and all(name not in {n for n, k in base.symbols} for name in added_aac)
    for section in base.sections:
        if section.kind == 'NOBITS' or not section.size: continue
        a, b = [bytearray(elf_read(data, section.addr, section.size)) for data in (before, after)]
        for address, size in masks:
            lo, hi = max(address, section.addr), min(address+size, section.end)
            if lo < hi: a[lo-section.addr:hi-section.addr] = b[lo-section.addr:hi-section.addr] = bytes(hi-lo)
        assert a == b, ('unexplained allocated bytes', section.name)
    # New AAC symbols were independently proven above; no existing symbol is
    # allowed to change by this placement gate.
    report = check_layout.compare(base, cand, frozenset(added_aac), False, False)
    for line in report.lines: print(line)
    assert report.failures == 0, 'strict layout gate'
    print('PASS: zero existing movers; 0 B BSS; all loaded changes explained; S4', total, 'B')
    print('Decoded operands:', ', '.join(hex(pc) for pc in sorted(patches)))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('base', type=Path); p.add_argument('candidate', type=Path)
    p.add_argument('--base-map', type=Path); p.add_argument('--map', dest='candidate_map', type=Path)
    args = p.parse_args()
    prove(args.base, args.candidate, args.base_map, args.candidate_map)
