"""Prove knsurf's compiler-table renames and same-size call redirection.

Read-only, test-only verifier. All changed pre-existing executable words must
be ARM calls from the existing Vine surface wrapper to knsurfSurfaceImpl; tables may only change
their compiler-assigned numeric suffix. Then run check_layout --full with
exactly the proven table names allowed. With --identity, all allocated bytes
must match except the fixed-width ctrportBuildId array.
"""
from pathlib import Path
import argparse
import hashlib
import re
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_layout


def elf_read(data, addr, size):
    phoff = struct.unpack_from('<I', data, 28)[0]
    phnum = struct.unpack_from('<H', data, 44)[0]
    for i in range(phnum):
        kind, offset, vaddr, _, filesz = struct.unpack_from('<IIIII', data, phoff + 32 * i)
        if kind == 1 and vaddr <= addr and addr + size <= vaddr + filesz:
            return data[offset + addr - vaddr:offset + addr - vaddr + size]
    raise AssertionError(('unbacked ELF address', hex(addr), size))


def branch(word, pc):
    # ARM B/BL with the same condition and link bit on both sides.
    assert word & 0x0e000000 == 0x0a000000, (hex(pc), hex(word))
    immediate = word & 0xffffff
    if immediate & 0x800000:
        immediate -= 1 << 24
    return pc + 8 + immediate * 4


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('base', type=Path)
    ap.add_argument('candidate', type=Path)
    ap.add_argument('--identity', action='store_true')
    args = ap.parse_args()
    nm = check_layout.tool('nm', None)
    readelf = check_layout.tool('readelf', None)
    old = check_layout.load(args.base, nm, readelf)
    new = check_layout.load(args.candidate, nm, readelf)
    old_data, new_data = args.base.read_bytes(), args.candidate.read_bytes()
    allow = []
    for key in sorted(old.symbols.keys() - new.symbols.keys()):
        name, kind = key
        if not re.fullmatch(r'CSWTCH\.\d+', name):
            continue  # check_layout must reject every other disappearance.
        assert len(old.symbols[key]) == 1
        addr, size = old.symbols[key][0]
        matches = [other for other in new.symbols.keys() - old.symbols.keys()
                   if other[1] == kind and re.fullmatch(r'CSWTCH\.\d+', other[0])
                   and new.symbols[other] == [(addr, size)]]
        assert len(matches) == 1, (key, matches)
        renamed = matches[0][0]
        before = elf_read(old_data, addr, size)
        assert before == elf_read(new_data, addr, size), (name, renamed, 'table contents changed')
        allow += [name, renamed]
        print('PROVEN table rename', name, '->', renamed, hex(addr), size,
              'B, SHA256', hashlib.sha256(before).hexdigest(), flush=True)

    helper = new.symbols.get(('knsurfSurfaceImpl', 't'))
    # knsurf is the last input inside .late.text (75zg, 10-31 18:5x; never an output section of its own, see
    # docs/3ds-hardware.md). ld puts its ARM-to-Thumb veneer after the last code input, so the helper takes the
    # veneer's old place and the veneer, unchanged, moves up by exactly the helper's size and still ends .late.text.
    veneer = ('____wrap_Crispart_drawTexture_from_arm', 't')
    if helper:
        assert len(helper) == 1
        addr, size = helper[0]
        assert len(old.symbols.get(veneer, [])) == 1 and len(new.symbols.get(veneer, [])) == 1, 'veneer'
        (old_v, v_size), (new_v, new_v_size) = old.symbols[veneer][0], new.symbols[veneer][0]
        assert v_size == new_v_size and old_v + v_size == old.plain['__late_text_end'], 'old veneer not last'
        assert addr == old_v and addr + size == new_v, "helper not in the veneer's old place"
        assert new_v + v_size == new.plain['__late_text_end'], 'new veneer not last'
        assert elf_read(old_data, old_v, v_size) == elf_read(new_data, new_v, v_size), 'veneer bytes changed'
        allow.append(veneer[0])  # its move is proven here; check_layout --full must accept only that
        print('PROVEN final helper', hex(addr), size, 'B; veneer', hex(old_v), '->', hex(new_v),
              '(same bytes); late reserve consumed',
              new.plain['__late_text_end'] - old.plain['__late_text_end'], 'B', flush=True)
    changes = 0
    for key in sorted(old.symbols.keys() & new.symbols.keys()):
        if key[1].lower() != 't' or (helper and key == veneer):
            continue
        for (addr, size), (new_addr, new_size) in zip(old.symbols[key], new.symbols[key]):
            assert (addr, size) == (new_addr, new_size), key
            before = elf_read(old_data, addr, size)
            after = elf_read(new_data, addr, size)
            if before == after:
                continue
            assert helper and not args.identity, (key, 'executable bytes changed')
            assert size % 4 == 0, key
            for offset in range(0, size, 4):
                a, b = struct.unpack_from('<I', before, offset)[0], struct.unpack_from('<I', after, offset)[0]
                if a == b:
                    continue
                assert a >> 24 == b >> 24
                if branch(a, addr + offset) == old_v and branch(b, addr + offset) == new_v:
                    # A call through the moved veneer (lead 18:5x: one cold caller, Crisp-art caDraw).
                    print('PROVEN veneer call follows the veneer', key[0], hex(addr + offset), flush=True)
                    changes += 1
                    continue
                # The stage compiler adapter redirects drawSurfaceImpl calls
                # through Vine's retained wrapper, including the helper's
                # fallback. Check the final ELF target, not the C spelling.
                assert branch(a, addr + offset) == old.symbols[('Vine_wrap_drawSurfaceImpl', 'T')][0][0], (key, hex(addr + offset))
                assert branch(b, addr + offset) == helper[0][0]
                print('PROVEN redirected branch', key[0], hex(addr + offset), flush=True)
                changes += 1

    # Read-only/data contents must be identical, including every pointer; the
    # build ID is the sole explicit exception. NOBITS is checked by check_layout.
    build_addr, build_size = old.symbols[('ctrportBuildId', 'R')][0]
    assert new.symbols[('ctrportBuildId', 'R')] == [(build_addr, build_size)]
    for section in old.sections:
        if section.kind == 'NOBITS':
            continue
        if 'X' in section.flags and not args.identity:
            continue
        before = bytearray(elf_read(old_data, section.addr, section.size))
        after = bytearray(elf_read(new_data, section.addr, section.size))
        if helper and not args.identity and section.name == '.late.text.reserve':
            # The old reserve's first bytes now hold the helper's tail and the moved veneer (proven above).
            start, end = max(section.addr, old.plain['__late_text_end']), new.plain['__late_text_end']
            assert section.addr <= start <= end <= section.end
            before[start - section.addr:end - section.addr] = after[start - section.addr:end - section.addr] = \
                b'\0' * (end - start)
        if section.addr <= build_addr and build_addr + build_size <= section.end:
            offset = build_addr - section.addr
            before[offset:offset + build_size] = after[offset:offset + build_size] = b'\0' * build_size
        assert before == after, (section.name, 'allocated bytes changed')
    print('PROVEN allocated', 'all sections' if args.identity else 'rodata/data',
          'identical except ctrportBuildId;', changes, 'redirected branches', flush=True)
    command = [sys.executable, '-I', str(Path(__file__).with_name('check_layout.py')),
               str(args.base), str(args.candidate), '--full']
    for name in allow:
        command += ['--allow', name]
    return subprocess.run(command, stdin=subprocess.DEVNULL, timeout=300).returncode


if __name__ == '__main__':
    raise SystemExit(main())
