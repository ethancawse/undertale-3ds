"""Prove six public (eight test-log) call operands, late storage and build ID change."""
import argparse
from pathlib import Path
import struct
import subprocess
import sys

sys.path.insert(0,str(Path(__file__).resolve().parent))
import check_layout
from check_knsurf_layout import elf_read, branch

def one(layout,name):
    values={value for (symbol,kind),entries in layout.symbols.items() if symbol==name for value in entries}
    assert len(values)==1,(name,values)
    return next(iter(values))

def prove(base_path,candidate_path,base_map=None,candidate_map=None):
    nm,readelf=check_layout.tool('nm',None),check_layout.tool('readelf',None)
    base=check_layout.load(base_path,nm,readelf,base_map)
    cand=check_layout.load(candidate_path,nm,readelf,candidate_map)
    before,after=base_path.read_bytes(),candidate_path.read_bytes()
    diagnostic=any(name=='CtrDlgflash_recordingSurface' for name,kind in cand.symbols)
    pairs=[
        ('CtrDialogue_init','CtrDlgflash_init'),
        ('CtrLayout_yield8','CtrDlgflash_yield'),
        ('Runner_addInstanceToObjectLists','CtrDlgflash_registered')]
    if diagnostic:pairs += [
        ('recordingSurface','CtrDlgflash_recordingSurface'),
        ('CtrDialogue_free','CtrDlgflash_free')]
    routes={one(base,a)[0]:one(cand,b)[0] for a,b in pairs}
    counts={address:0 for address in routes}; patches={}
    for key in base.symbols.keys() & cand.symbols.keys():
        assert base.symbols[key]==cand.symbols[key],('moved/resized existing symbol',key)
        if key[1] not in 'tTwW': continue
        for address,size in base.symbols[key]:
            a,b=elf_read(before,address,size),elf_read(after,address,size)
            if a==b: continue
            assert size%4==0,('unaligned body',key)
            for offset in range(0,size,4):
                wa,wb=struct.unpack_from('<I',a,offset)[0],struct.unpack_from('<I',b,offset)[0]
                if wa==wb:continue
                pc=address+offset
                assert wa>>24==wb>>24==0xeb,('not unchanged ARM BL',key,hex(pc),hex(wa),hex(wb))
                old,new=branch(wa,pc),branch(wb,pc)
                assert old in routes and routes[old]==new,('unexpected callee',key,hex(pc),hex(old),hex(new))
                if pc not in patches: counts[old]+=1
                patches[pc]=4
    assert sorted(counts.values())==([1,1,1,1,4] if diagnostic else [1,1,4]),counts
    # The logger may observe only the white-quad query after the solid test,
    # not emitQuad's earlier textured branch or any sprite/glyph query.
    emit,size=one(base,'emitQuad')
    body=elf_read(before,emit,size)
    original_record=one(base,'recordingSurface')[0]
    original_solid=one(base,'CtrDlgwhite_solidDrawIsNoop')[0]
    records=[];solids=[]
    for offset in range(0,size,4):
        word=struct.unpack_from('<I',body,offset)[0];pc=emit+offset
        if word>>24!=0xeb:continue
        target=branch(word,pc)
        if target==original_record:records.append(pc)
        if target==original_solid:solids.append(pc)
    assert len(records)==2 and len(solids)==1 and records[0]<solids[0]<records[1],('white branch shape',records,solids)
    routed_records=[pc for pc in patches if struct.unpack('<I',elf_read(before,pc,4))[0]>>24==0xeb and
                    branch(struct.unpack('<I',elf_read(before,pc,4))[0],pc)==original_record]
    assert routed_records==([records[1]] if diagnostic else []),('wrong recording query',routed_records)
    assert cand.plain['__late_rodata_end']==base.plain['__late_rodata_end'],'unexpected late rodata'
    assert cand.plain['__late_data_end']-base.plain['__late_data_end']==(12 if diagnostic else 0),'unexpected data delta'
    for name in ('__bss_start__','__bss_end__','__end__'):
        assert base.plain[name]==cand.plain[name],('BSS delta',name)
    build_id,build_size=one(base,'ctrportBuildId');assert one(cand,'ctrportBuildId')==(build_id,build_size)
    masks=list(patches.items())+[(build_id,build_size)]
    for kind in ('text','rodata','data'):
        lo,hi=base.plain[f'__late_{kind}_end'],cand.plain[f'__late_{kind}_end']
        assert hi>=lo,(kind,'late shrink');masks.append((lo,hi-lo))
    for section in base.sections:
        if section.kind=='NOBITS' or not section.size:continue
        a,b=[bytearray(elf_read(data,section.addr,section.size)) for data in (before,after)]
        for address,size in masks:
            lo,hi=max(address,section.addr),min(address+size,section.end)
            if lo<hi:a[lo-section.addr:hi-section.addr]=b[lo-section.addr:hi-section.addr]=bytes(hi-lo)
        assert a==b,('unrelated allocated bytes',section.name)
    print('PASS:',len(patches),'decoded calls; all existing symbols/allocated bytes retained; 0 B BSS; append-only code/constants/data')
    print('Decoded call operands:',', '.join(hex(pc) for pc in sorted(patches)))
    print('Late text delta:',cand.plain['__late_text_end']-base.plain['__late_text_end'],'B')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('base',type=Path);p.add_argument('candidate',type=Path)
    p.add_argument('--base-map',type=Path);p.add_argument('--map',dest='candidate_map',type=Path)
    args=p.parse_args();prove(args.base,args.candidate,args.base_map,args.candidate_map)
    maps=[]
    if args.base_map:maps+=['--base-map',str(args.base_map)]
    if args.candidate_map:maps+=['--map',str(args.candidate_map)]
    raise SystemExit(subprocess.run([sys.executable,'-I',str(Path(__file__).with_name('check_layout.py')),
        str(args.base),str(args.candidate),'--full',*maps],stdin=subprocess.DEVNULL,timeout=120).returncode)
