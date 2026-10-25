"""Qualify ordered-tile runner assembly placement against its frozen control.

Restore the original string pool, retain unused original dispatch bytes and
the two smaller function extents. Only drawRoom's live dispatch table moves
to late rodata. Compiler instructions are not rewritten. Original objects
and assembly are never overwritten. Final linked layout/pins/tails remain
mandatory; this object check grants no layout waiver or timing verdict.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys

HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE))
import utold_tile_elf as elf
from utold_tile_elf import snapshot
ARM=Path('C:/devkitPro/devkitARM/bin')

def tool(name):
    path=ARM/('arm-none-eabi-'+name)
    return str(path.with_suffix('.exe') if path.with_suffix('.exe').exists() else path)


def write_text(path,text):
    # The exported builder supports Python 3.8; Path.write_text(newline=...)
    # was added later. Keep generated assembly and receipts explicitly LF.
    with path.open('w',encoding='utf-8',newline='\n') as stream:
        stream.write(text)

SHRUNK=('drawRoom','Runner_handlePendingRoomChange')
CALLS={'initRoom':'utpCall_initRoom','cleanupState':'utpCall_cleanupState',
       'persistRoomState':'utpCall_persistRoomState','rebuildDrawableCache':'utpCall_rebuildDrawableCache'}
POOL='.rodata.str1.4'


def run(command):
    p=subprocess.run(command,stdin=subprocess.DEVNULL,timeout=30,
                     capture_output=True)
    log=p.stdout+p.stderr
    if p.returncode:
        raise RuntimeError('Tile slot tool failed ('+str(p.returncode)+'):\n'+log.decode(errors='replace'))
    assert not re.search(rb'\bwarning:',log),log.decode(errors='replace')
    return log


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def image(path):
    raw=path.read_bytes();sections,_,symbols=elf.tables(raw)
    strings=sections[struct.unpack_from('<H',raw,50)[0]]
    names=raw[strings[4]:strings[4]+strings[5]]
    labels=[names[s[0]:].split(b'\0',1)[0].decode() for s in sections]
    byname={name:(i,s) for i,(name,s) in enumerate(zip(labels,sections))}
    data={name:raw[s[4]:s[4]+s[5]] for name,(_,s) in byname.items() if s[1]!=8}
    return raw,sections,symbols,labels,byname,data


def chunks(text):
    return re.split(r'(?=^[ \t]*\.(?:section[ \t]+|(?:text|data|bss)(?:[ \t]*\n)))',text,flags=re.M)


def section(chunk):
    m=re.match(r'^[ \t]*\.section[ \t]+([^,\s]+)',chunk)
    if m: return m[1]
    m=re.match(r'^[ \t]*(\.(?:text|data|bss))[ \t]*\n',chunk)
    return m[1] if m else None


def pool_chunks(text): return [c for c in chunks(text) if section(c)==POOL]


def string_labels(text,path):
    _,_,symbols,labels,_,data=image(path)
    result={}
    for chunk in pool_chunks(text):
        for name in re.findall(r'^(\.LC\d+):',chunk,re.M):
            _,sym=symbols[name]
            assert labels[sym[5]]==POOL,name
            raw=data[POOL][sym[1]:]
            end=raw.index(0)+1
            result[name]=(raw[:end],sym[1])
    return result


def table_chunk(text,name):
    begin=text.index('\n'+name+':\n')
    end=re.search(r'^[ \t]*\.size[ \t]+'+re.escape(name)+r',',text[begin:],re.M)
    assert end,name
    body=text[begin:begin+end.start()]
    found=[c for c in chunks(body) if section(c)=='.rodata']
    assert len(found)==1,(name,len(found))
    chunk=found[0]
    label,=re.findall(r'^(\.L\d+):',chunk,re.M)
    count=len(re.findall(r'^[ \t]*\.byte[ \t]+',chunk,re.M))
    assert count==4 and not re.search(r'^[ \t]*\.(?:word|2byte|ascii)',chunk,re.M)
    return chunk,label,count


def assemble(assembly,path,locals=False):
    run([tool('as'),*(['-L'] if locals else []),
         '-o',str(path),str(assembly)])


def preserve_instructions(original,placed):
    """Prove constants and assembler-resolved local branch destinations."""
    old=image(original);new=image(placed)
    a=snapshot(original)[0];b=snapshot(placed)[0]
    ar=elf.relocation_inventory(old[0]);br=elf.relocation_inventory(new[0])
    checked=0;addends=[]
    for name,(_,sym) in old[2].items():
        if sym[3]&15!=2 or not 0<sym[5]<len(old[1]): continue
        assert name in b and all(a[name][k]==b[name][k] for k in ('size','section')),name
        sec=a[name]['section'];start=a[name]['offset'];size=a[name]['size']
        left=bytearray(old[5][sec][start:start+size]);right=bytearray(new[5][sec][b[name]['offset']:b[name]['offset']+size])
        for key,target in ar.items():
            if key[0]!=sec or not start<=key[1]<start+size: continue
            offset=key[1]-start
            placed_key=(sec,b[name]['offset']+offset)
            actual=br.get(placed_key)
            if target==actual and left[offset:offset+4]==right[offset:offset+4]: continue
            assert target[0]==2 and actual and actual[0]==2,('Instruction/relocation changed',name,key,target,actual)
            x=struct.unpack_from('<I',left,offset)[0];y=struct.unpack_from('<I',right,offset)[0]
            if target[1]==actual[1]==POOL:
                # Check the value of each actual reference, including suffix
                # references into a merged string, rather than trusting labels.
                before=old[5][POOL][x:].split(b'\0',1)[0]
                after=new[5][POOL][y:].split(b'\0',1)[0]
                assert x<len(old[5][POOL]) and y<len(new[5][POOL]) and before==after,(name,key)
                kind='same string value'
            else:
                assert name=='drawRoom' and target[1]=='.rodata' and actual[1]=='.rodata.late.utold',(name,key,target,actual)
                assert old[5]['.rodata'][x:x+4]==new[5]['.rodata.late.utold'][y:y+4]
                kind='same live dispatch bytes'
            addends.append({'function':name,'offset':key[1],'kind':kind,'old':x,'new':y})
            left[offset:offset+4]=right[offset:offset+4]=bytes(4)
        for offset in range(0,size,4):
            if left[offset:offset+4]==right[offset:offset+4]: continue
            x=struct.unpack_from('<I',left,offset)[0]
            y=struct.unpack_from('<I',right,offset)[0]
            # Moving whole functions within one input makes GAS re-encode
            # already-resolved ARM B/BL displacements. Prove opcode, condition,
            # destination function and intra-function offset, not just a mask.
            assert x>>24==y>>24 and (x>>25)&7==5,('Candidate instruction bytes changed',name,offset)
            def destination(word,start,symbols):
                delta=word&0xffffff
                if delta&0x800000: delta-=0x1000000
                address=start+offset+8+delta*4
                matches=[(n,address-v['offset']) for n,v in symbols.items()
                         if v['section']==sec and v['offset']<=address<v['offset']+v['size']]
                assert len(matches)==1,('Ambiguous branch destination',name,address,matches)
                return matches[0]
            before=destination(x,start,a);after=destination(y,b[name]['offset'],b)
            assert before==after,('Candidate branch destination changed',name,offset,before,after)
            addends.append({'function':name,'offset':offset,'kind':'assembler local branch to same function/offset',
                            'target':list(before)})
            left[offset:offset+4]=right[offset:offset+4]=bytes(4)
        assert left==right,('Candidate instruction bytes changed',name)
        for key in br:
            if key[0]==sec and b[name]['offset']<=key[1]<b[name]['offset']+size:
                original_key=(sec,start+key[1]-b[name]['offset'])
                assert original_key in ar,('Added runtime relocation',name,key)
        checked+=1
    return {'candidate_functions_checked':checked,
            'resolved_data_and_local_branch_changes':addends,
            'compiler_assembly_instructions_edited':False}


def qualify(base,candidate,output,report):
    a,adata=snapshot(base);b,bdata=snapshot(candidate)
    missing=[];renames={};checked=[]
    for name,old in a.items():
        new=b.get(name)
        if new is None and re.fullmatch(r'CSWTCH\.\d+',name):
            matches=[n for n,v in b.items() if re.fullmatch(r'CSWTCH\.\d+',n) and v==old]
            assert len(matches)==1,(name,matches)
            renames[name]=matches[0];new=b[matches[0]]
        if new is None: missing.append(name);continue
        if name in SHRUNK:
            assert new['section']==old['section'] and new['offset']==old['offset']
            assert new['size']<old['size'],(name,old,new)
        else: assert new==old,('Unexpected original symbol payload/offset change',name,old,new)
        checked.append(name)
    assert not missing,missing
    for name,old in adata.items():
        if name.startswith('.text'):
            assert bdata[name]['size']==old['size'],('Original code extent changed',name)
        else: assert bdata.get(name)==old,('Original allocated data changed',name)
    additions={n:v for n,v in b.items() if n not in a and n not in renames.values()}
    assert all('.late.utold' in v['section'] for v in additions.values()),additions
    ar=elf.relocation_inventory(base.read_bytes());br=elf.relocation_inventory(candidate.read_bytes())
    redirects=[]
    for key,target in ar.items():
        if key[0] in {'.text.'+n for n in SHRUNK}: continue
        actual=br.get(key)
        if actual!=target:
            assert target[0] in (28,29) and actual==(target[0],CALLS.get(target[1])),(key,target,actual)
            redirects.append({'section':key[0],'offset':key[1],'old':target,'new':actual})
    # No extra relocation may appear in an unchanged original runtime input.
    for key,target in br.items():
        if key[0] in adata and key[0] not in {'.text.'+n for n in SHRUNK}:
            assert key in ar,('Unexpected relocation in original section',key,target)
    dis=run([tool('objdump'),'-dr',str(candidate)]).decode()
    write_text(output/'runner-placed-disassembly.txt',dis)
    functions=elf.functions(dis)
    tails=[]
    for name in SHRUNK:
        begin,end=b[name]['size'],a[name]['size']
        # Symbol payload excludes padding. Direct code transfers in that
        # payload must stay out of its retained tail. Linked whole-ELF incoming
        # transfers and following-symbol addresses are a later gate.
        for m in re.finditer(r'^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(b\w*|bl\w*)\s+([0-9a-f]+)\s+<'+re.escape(name)+r'(?:\+[^>]+)?>',functions[name],re.M):
            assert not begin<=int(m[3],16)<end,('Direct branch into tail',name,m[0])
        tails.append({'name':name,'body_bytes':begin,'original_extent_bytes':end,
                      'unused_tail_bytes':end-begin,'final_link_proof_pending':True})
    report.update(original_symbols_checked=len(checked),numeric_renames=renames,
                  call_redirects=redirects,retained_tails=tails,
                  added_late_symbols=len(additions),original_allocated_data_exact=True,
                  zero_warnings=True,final_link_layout_identity_timing_pending=True)


def retain_late_order(reference,text):
    """Reorder whole existing function chunks, never their instructions."""
    section_name='.text.late.layerfix'
    def inventory(value):
        result=[]
        for index,chunk in enumerate(chunks(value)):
            if section(chunk)!=section_name: continue
            names=re.findall(r'^\s*\.type\s+([^,]+), %function$',chunk,re.M)
            if not names: continue
            # GCC may omit a redundant section directive between adjacent
            # functions. Keep that whole group intact and require the same
            # grouping in the reference; never split a compiler body.
            result.append((index,tuple(names),chunk))
        return result
    baseline=inventory(reference);candidate=inventory(text)
    assert {name for _,name,_ in baseline}=={name for _,name,_ in candidate}
    byname={name:chunk for _,name,chunk in candidate}
    result=chunks(text)
    for (index,_,_),(_,name,_) in zip(candidate,baseline):
        result[index]=byname[name]
    return ''.join(result)


def place(folder,output):
    folder=folder.resolve();output=output.resolve()
    output.mkdir(exist_ok=False)
    for arm in ('base','candidate'):
        proof=json.loads((folder/arm/'runner-assembly-proof.json').read_text())
        assert proof['allocated_symbols_data_and_relocation_targets_equal']
        assert sha(folder/arm/'runner.o')==proof['original_sha256']
        assert sha(folder/arm/'runner.s')==proof['assembly_sha256']
        assemble(folder/arm/'runner.s',output/(arm+'-locals.o'),locals=True)
    base_text=(folder/'base/runner.s').read_text()
    candidate_text=(folder/'candidate/runner.s').read_text()
    bs=string_labels(base_text,output/'base-locals.o')
    cs=string_labels(candidate_text,output/'candidate-locals.o')
    assert len(bs)==len(cs),(len(bs),len(cs))
    mapping={}
    for name,(value,_) in cs.items():
        matches=[n for n,(v,_) in bs.items() if v==value]
        assert len(matches)==1,('Ambiguous/missing original string',name,len(matches))
        mapping[name]=matches[0]
    assert len(set(mapping.values()))==len(bs),'String multiset changed'
    old_table,old_label,width=table_chunk(base_text,'drawRoom')
    new_table,new_label,new_width=table_chunk(candidate_text,'drawRoom')
    assert width==new_width
    _,_,symbols,labels,_,data=image(output/'base-locals.o')
    old_symbol=symbols[old_label][1]
    assert labels[old_symbol[5]]=='.rodata'
    payload=data['.rodata'][old_symbol[1]:old_symbol[1]+width]
    _,_,symbols,labels,_,_=image(output/'candidate-locals.o')
    new_symbol=symbols[new_label][1]
    assert labels[new_symbol[5]]=='.rodata' and old_symbol[1]==new_symbol[1]
    # Define the live candidate label in late rodata; retain unused original
    # bytes in its old position. No original data symbol is moved or aliased.
    replacement=new_table.replace('\t.section\t.rodata\n',
        '\t.section\t.rodata.late.utold,"a",%progbits\n\t.balign 4\n',1)
    assert replacement!=new_table
    replacement+='\t.section\t.rodata\n\t.byte '+','.join(str(n) for n in payload)+'\n'
    assert candidate_text.count(new_table)==1
    candidate_text=candidate_text.replace(new_table,replacement,1)
    restored=[];inserted=False
    for chunk in chunks(candidate_text):
        if section(chunk)==POOL:
            if not inserted: restored.extend(pool_chunks(base_text));inserted=True
        else:
            restored.append(re.sub(r'\.LC\d+\b',lambda m:mapping.get(m[0],m[0]),chunk))
    assert inserted
    candidate_text=retain_late_order(base_text,''.join(restored))
    base_syms=snapshot(folder/'base/runner.o')[0]
    candidate_syms=snapshot(folder/'candidate/runner.o')[0]
    for name in SHRUNK:
        space=base_syms[name]['size']-candidate_syms[name]['size']
        assert 0<space<4096,(name,space)
        pattern=r'(^[ \t]*\.size[ \t]+'+re.escape(name)+r',[^\n]*\n)'
        candidate_text,count=re.subn(pattern,lambda m:m[1]+
            '.LutpRetainedTail_'+name+':\n\t.space '+str(space)+'\n',candidate_text,flags=re.M)
        assert count==1,(name,count)
    assembly=output/'runner-placed.s';write_text(assembly,candidate_text)
    placed=output/'runner-placed.o';assemble(assembly,placed)
    report={'scope':'UT Old object placement only; no link/layout waiver',
        'input':str(folder),'base_sha256':sha(folder/'base/runner.o'),
        'candidate_sha256':sha(folder/'candidate/runner.o'),
        'assembly_sha256':sha(assembly),'placed_sha256':sha(placed),
        'string_labels_mapped':len(mapping),'live_table_late_bytes':width,
        'original_dispatch_bytes_retained':payload.hex(),'instruction_rewrites':False,
        'entry_hooks':False}
    report['instruction_preservation']=preserve_instructions(folder/'candidate/runner.o',placed)
    qualify(folder/'base/runner.o',placed,output,report)
    write_text(output/'placement-proof.json',json.dumps(report,indent=2)+'\n')
    print('PLACEMENT OBJECT PASS',report['original_symbols_checked'],'original symbols;',
          len(mapping),'strings;',report['added_late_symbols'],'late symbols; 0 warnings',flush=True)
    print('Final linked layout/tails/pins, identity, weighted cost and timing still pending.',flush=True)
    return report


def negative_checks(folder,placed_folder,output):
    """Reject changed original data, instructions, and call targets."""
    output.mkdir(exist_ok=False)
    source=placed_folder/'runner-placed.o';raw,sections,symbols,names,_,_=image(source)
    mutations={}
    data=bytearray(raw);data[next(s[4] for s,n in zip(sections,names) if n=='.rodata') ]^=1
    mutations['original-data']=data
    for name,label in (('Renderer_drawTile','original-instruction'),('drawRoom','candidate-instruction')):
        data=bytearray(raw);sym=symbols[name][1];data[sections[sym[5]][4]+sym[1]]^=1
        mutations[label]=data
    data=bytearray(raw)
    reloc,= [s for s,n in zip(sections,names) if s[1]==9 and n=='.rel.text.Runner_reset']
    target=symbols['Utp_invalidate'][0]
    # Select a real runtime call, leaving instruction payload unchanged.
    calls=[at for at in range(reloc[4],reloc[4]+reloc[5],8) if struct.unpack_from('<I',data,at+4)[0]&255 in (28,29)]
    assert calls
    at=calls[0]+4;kind=struct.unpack_from('<I',data,at)[0]&255
    struct.pack_into('<I',data,at,(target<<8)|kind)
    mutations['original-call-target']=data
    rejected={}
    for label,data in mutations.items():
        case=output/label;case.mkdir();path=case/'runner.o';path.write_bytes(data)
        try:
            if label=='candidate-instruction': preserve_instructions(folder/'candidate/runner.o',path)
            else: qualify(folder/'base/runner.o',path,case,{})
        except AssertionError as error: rejected[label]=str(error)[:400]
        else: raise AssertionError('Tampered object accepted: '+label)
    write_text(output/'negative-checks.json',json.dumps(rejected,indent=2)+'\n')
    print('NEGATIVE CHECK PASS',len(rejected),'tampered objects rejected',flush=True)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--input',type=Path,required=True);ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--negative-checks',action='store_true')
    args=ap.parse_args();place(args.input,args.out)
    if args.negative_checks: negative_checks(args.input,args.out,args.out/'negative-checks')


if __name__=='__main__': main()
