"""Preserve frozen bodies; route shared overworld speech operands after GCC.

The renderer adapter invokes route_renderer after its existing helper extraction.
The same installed renderer launcher handles two pure-policy callers, the
digest owner, and the new unit. No global --wrap order is replaced.
"""
from pathlib import Path
import hashlib,json,re,subprocess,sys
MARKER='--undlg-compiler'
SECTION='.text_undlg_speech'

def section(model):
    return '.aacout.text.undlg_speech' if model=='o3ds' else '.text_undlg_speech'

CONDITION=r'(?:eq|ne|cs|cc|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?'

def scoped(text,caller,old,new,expected):
    pattern=r'(?ms)^'+re.escape(caller)+r':\r?\n.*?^\s*\.size\s+'+re.escape(caller)+r',\s*\.-'+re.escape(caller)+r'\s*$'
    scopes=list(re.finditer(pattern,text));assert len(scopes)==1,('speech caller scope',caller,len(scopes))
    scope=scopes[0]
    branch=r'(?m)^([ \t]+b(?:l)?'+CONDITION+r'[ \t]+)'+re.escape(old)+r'(\(PLT\))?([ \t]*(?:@[^\n]*)?)$'
    body,count=re.subn(branch,lambda m:m[1]+new+(m[2] or '')+m[3],scope[0])
    assert count==expected,('speech caller operands',caller,old,count,expected)
    return text[:scope.start()]+body+text[scope.end():],dict(caller=caller,old=old,new=new,count=count)

def all_calls(text,old,new,expected):
    pattern=r'(?m)^([ \t]+b(?:l)?'+CONDITION+r'[ \t]+)'+re.escape(old)+r'(\(PLT\))?([ \t]*(?:@[^\n]*)?)$'
    routed,count=re.subn(pattern,lambda m:m[1]+new+(m[2] or '')+m[3],text)
    assert count==expected,('speech complete operand census',old,count,expected)
    return routed,dict(old=old,new=new,count=count)

def bridges(model):
    # Each bridge is the measured single ARM tail branch. They are appended
    # after routing, so each bridge's old-body edge stays original.
    out='\n.pushsection '+section(model)+',"ax",%progbits\n.arm\n.balign 4\n'
    for name,target in [('CtrUndlg_baseBoxObject','dlgflashBoxObject'),('CtrUndlg_baseBoxOpen','dlgtopBoxOpen')]:
        out+=f'.global {name}\n.type {name}, %function\n{name}:\n b {target}\n.size {name}, .-{name}\n'
    return out+'.popsection\n'

def route_renderer(text,diagnostic=False,model="o3ds"):
    rows=[]
    entries=[('CtrDlgflash_init','__real_CtrDialogue_init','CtrUndlg_init',1),
             ('CtrDlgtop_refresh','__real_CtrDialogue_allowed','CtrUndlg_allowed',1),
             ('dlgflashRefresh','__real_CtrDialogue_allowed','CtrUndlg_allowed',1),
             ('CtrDlgflash_registered','dlgflashBoxObject','CtrUndlg_boxObject',1)]
    if diagnostic:
        entries.extend([('CtrDlgflash_free','CtrDialogue_free','CtrUndlg_free',1),
                        ('CtrDlgflash_recordingSurface','dlgflashBoxObject','CtrUndlg_boxObject',1)])
    else:entries.append(('ctrDestroy','CtrDialogue_free','CtrUndlg_free',1))
    for args in entries:
        text,row=scoped(text,*args);rows.append(row)
    # N12's retained-loan wake/record/submit chain also queries logical box
    # ownership. All ten queries must share the standalone-speech predicate.
    for caller,count in [('CtrDlgtop_topBusy',1),('dlgtopTake',1),('dlgtopPrepare',1),
                         ('dlgtopDrawBirth',1),('CtrDlgtop_end',1),('dlgtopRecord',1),
                         ('dlgtopPresentTarget',1),('dlgtopWake',2),('CtrDlgtop_idleChain',1)]:
        text,row=scoped(text,caller,'dlgtopBoxOpen','CtrUndlg_boxOpen',count)
        rows.append(row)
    assert not re.search(r'(?m)^\s+b(?:l)?'+CONDITION+r'\s+dlgtopBoxOpen(?:\(PLT\))?\s*$',text), 'Unregistered speech box query'
    return text+bridges(model),rows

def digest_accessor(model):
    # z_roomplan.c's rp begins with char digest[65]. This is exactly the
    # owning-TU accessor compiled and disassembled in R198/R204 (12 B).
    return ('''
.pushsection SECTION,"ax",%progbits
.arm
.balign 4
.global CtrUndlg_bootDigest
.type CtrUndlg_bootDigest, %function
CtrUndlg_bootDigest:
 ldr r0, 1f
 bx lr
1: .word rp
.size CtrUndlg_bootDigest, .-CtrUndlg_bootDigest
.popsection
''').replace('SECTION',section(model))

def route_unit(text,model):
    pattern=r'(?m)^(\s*\.section\s+\.bss_undlg_speech,"aw")(?:,%progbits)?\s*$'
    text,count=re.subn(pattern,lambda m:m[1]+',%nobits',text)
    assert count==1,('speech zero-fill section',count)
    return text

def run(command):return subprocess.run(command,stdin=subprocess.DEVNULL,timeout=1800).returncode

def compile_extra(delegate,command):
    names=('ctr_dialogue.c','z_popbot2.c','z_touchui.c','z_roomplan.c','z_undlg_speech.c')
    sources=[x for x in command if x.replace('\\','/').rsplit('/',1)[-1] in names]
    if not sources or '-DENABLE_WAD16' not in command:return run(delegate+command)
    assert len(sources)==1 and '-c' in command and command.count('-o')==1
    model='o3ds' if '-DCTRPORT_TARGET_O3DS' in command else 'n3ds'
    source=sources[0];name=source.replace('\\','/').rsplit('/',1)[-1]
    # make compiles these translation units concurrently in the same directory.
    # Keep both intermediate assembly files tied to their own object filename.
    output=Path(command[command.index('-o')+1]);before=output.with_suffix('.undlg0.s');after=output.with_suffix('.undlg1.s')
    compile_args=list(command);compile_args[compile_args.index('-c')]='-S';compile_args[compile_args.index('-o')+1]=str(before)
    rc=run(delegate+compile_args)
    if rc:return rc
    text=before.read_text(encoding='utf8');original=text;rows=[]
    if name in ('z_popbot2.c','z_touchui.c'):
        caller='CtrPopbot2_allowed' if name=='z_popbot2.c' else 'tuView'
        text,row=scoped(text,caller,'__real_CtrDialogue_allowed','CtrUndlg_allowed',1);rows.append(row)
    elif name=='ctr_dialogue.c':
        for target in ('findAll','anyExists'):
            assert re.search(r'^'+target+':',text,re.M),('Missing reused policy helper',target)
        text+='\n.pushsection '+section(model)+',"ax",%progbits\n.arm\n.balign 4\n'
        for target in ('findAll','anyExists'):
            name='CtrUndlg_'+target
            text+=f'.global {name}\n.type {name}, %function\n{name}:\n b {target}\n.size {name}, .-{name}\n'
        text+='.popsection\n'
    elif name=='z_roomplan.c':
        body=Path(source).read_text(encoding='utf8')
        assert re.search(r'static struct\s*\{\s*char digest\[65\];',body),'digest owner layout changed'
        assert re.search(r'^rp:',text,re.M),'digest owner symbol missing'
        text+=digest_accessor(model)
    else:text=route_unit(text,model)
    with after.open('w',encoding='utf8',newline='\n') as out:out.write(text)
    assemble=[command[0]];i=1
    while i<len(command):
        arg=command[i]
        if arg in ('-MF','-MT','-MQ','-o'):i+=2;continue
        if arg not in ('-MD','-MMD','-MP','-c') and arg!=source:assemble.append(arg)
        i+=1
    rc=run(assemble+['-Wa,--fatal-warnings','-x','assembler','-c',str(after),'-o',str(output)])
    output.with_suffix('.undlg.json').write_text(json.dumps(dict(exit=rc,source=name,routes=rows,
        original_sha256=hashlib.sha256(original.encode()).hexdigest(),routed_sha256=hashlib.sha256(text.encode()).hexdigest()),indent=2)+'\n',encoding='utf8')
    print('Overworld speech: '+name+' '+json.dumps(rows),flush=True)
    return rc
