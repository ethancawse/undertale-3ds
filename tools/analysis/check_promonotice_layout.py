"""Prove the complete N12 notice unit: three NOPs, one guarded B and appended code only."""
import argparse
from pathlib import Path
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_layout
from check_dlgwhite_layout import one
from check_knsurf_layout import elf_read, branch


def prove(base_path, candidate_path, base_map=None, candidate_map=None):
    nm, readelf = check_layout.tool('nm', None), check_layout.tool('readelf', None)
    base = check_layout.load(base_path, nm, readelf, base_map)
    cand = check_layout.load(candidate_path, nm, readelf, candidate_map)
    before, after = base_path.read_bytes(), candidate_path.read_bytes()
    helper, helper_size = one(cand, 'CtrPromonotice_configure')
    old_end = base.plain['__late_text_end']
    assert old_end <= helper < old_end+4, ('helper not last appended', hex(old_end),hex(helper))
    assert helper+helper_size == cand.plain['__late_text_end'], 'unexpected extra late text'
    for kind in ('rodata', 'data'):
        assert base.plain[f'__late_{kind}_end'] == cand.plain[f'__late_{kind}_end'], ('late storage',kind)
    for name in ('__bss_start__', '__bss_end__', '__end__'):
        assert base.plain[name] == cand.plain[name], ('BSS changed',name)
    for key in base.symbols.keys() & cand.symbols.keys():
        assert base.symbols[key] == cand.symbols[key], ('existing symbol moved/resized',key)
    start, size = one(base, 'CtrRenderer_beginFrame')
    a, b = elf_read(before,start,size), elf_read(after,start,size)
    patches, nops = {}, 0
    for offset in range(0,size,4):
        wa, wb = struct.unpack_from('<I',a,offset)[0], struct.unpack_from('<I',b,offset)[0]
        if wa == wb: continue
        pc = start+offset
        # Original three stores: STRB r9/r3, [r7, #540]. The loader's
        # complete field displacement is 12288+540, pinned by the helper.
        if wa in (0xe5c7921c, 0xe5c7321c) and wb in (0xe320f000, 0xe1a00000):
            nops += 1
        else:
            raise AssertionError(('unexpected frame instruction',hex(pc),hex(wa),hex(wb)))
        patches[pc] = 4
    assert nops == 3, ('notice patch counts',nops)
    start,size = one(base,'CtrGameover_configure')
    a,b = elf_read(before,start,size),elf_read(after,start,size)
    calls=0
    for offset in range(0,size,4):
        wa,wb=struct.unpack_from('<I',a,offset)[0],struct.unpack_from('<I',b,offset)[0]
        if wa==wb: continue
        pc=start+offset
        assert wa>>24 == wb>>24 == 0xea, ('unexpected guarded delegation',hex(pc),hex(wa),hex(wb))
        assert branch(wa,pc)==one(base,'configureScreens')[0]
        assert branch(wb,pc)==helper
        calls+=1;patches[pc]=4
    assert calls==1, ('guarded delegation count',calls)
    build_id, build_size = one(base,'ctrportBuildId')
    assert one(cand,'ctrportBuildId') == (build_id,build_size)
    masks = list(patches.items())+[(build_id,build_size),(old_end,cand.plain['__late_text_end']-old_end)]
    # Every pre-existing allocated byte, including cold and late bodies,
    # constants and initialized mutable storage, must match outside this list.
    for section in base.sections:
        if section.kind == 'NOBITS' or not section.size: continue
        aa,bb = [bytearray(elf_read(data,section.addr,section.size)) for data in (before,after)]
        for address,length in masks:
            lo,hi = max(address,section.addr),min(address+length,section.end)
            if lo < hi:
                aa[lo-section.addr:hi-section.addr] = bb[lo-section.addr:hi-section.addr] = bytes(hi-lo)
        assert aa == bb, ('unrelated allocated bytes',section.name)
    print('PASS: three decoded notice-store NOPs + one guarded configure B; all existing symbols/allocated bytes retained; 0 B new data/BSS')
    print('Release late text price:',cand.plain['__late_text_end']-old_end,'B')


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base',type=Path); parser.add_argument('candidate',type=Path)
    parser.add_argument('--base-map',type=Path); parser.add_argument('--map',dest='candidate_map',type=Path)
    args=parser.parse_args()
    prove(args.base,args.candidate,args.base_map,args.candidate_map)
    maps=[]
    if args.base_map: maps+=['--base-map',str(args.base_map)]
    if args.candidate_map: maps+=['--map',str(args.candidate_map)]
    raise SystemExit(subprocess.run([sys.executable,'-I',str(Path(__file__).with_name('check_layout.py')),
        str(args.base),str(args.candidate),'--full',*maps],stdin=subprocess.DEVNULL,timeout=120).returncode)
