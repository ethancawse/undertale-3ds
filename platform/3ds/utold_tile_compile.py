"""Preserve UNDERTALE Release runner slots using the same source and flags.

CMake compiler launcher. Other translation units pass through the previous
launcher. The reference preprocessor branch contains today's live traversal;
it is compiled locally, without Git or a frozen external object. The placement
step preserves original allocated data and compiler-emitted instructions,
retains two smaller function extents, and checks same-ABI call redirects.
Whole-linked layout, retained-tail and identity gates remain independent.
"""
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0,str(Path(__file__).resolve().parent))
import utold_tile_place as place
from utold_tile_elf import snapshot,relocation_inventory


def run(command):
    result=subprocess.run(command,stdin=subprocess.DEVNULL,timeout=300,capture_output=True)
    output=result.stdout+result.stderr
    if output:
        sys.stdout.buffer.write(output)
        sys.stdout.flush()
    if result.returncode:
        raise RuntimeError('Tile slot compiler command failed, exit '+str(result.returncode))
    if re.search(rb'\bwarning:',output):
        raise RuntimeError('Tile slot compiler command emitted a warning')


def without_dependencies(command):
    result=[];index=0
    while index<len(command):
        arg=command[index]
        if arg in ('-MF','-MT','-MQ'):
            index+=2
            continue
        if arg not in ('-MD','-MMD','-MP'):
            result.append(arg)
        index+=1
    return result


def capture(command,folder,reference=False):
    folder.mkdir()
    obj=folder/'runner.o';assembly=folder/'runner.s'
    compile_command=without_dependencies(command)
    if reference:
        compile_command.append('-DUTOLD_TILE_PLAN_REFERENCE')
        compile_command[compile_command.index('-o')+1]=str(obj)
        run(compile_command)
    else:
        shutil.copy2(Path(command[command.index('-o')+1]),obj)
    compile_command[compile_command.index('-c')]='-S'
    compile_command[compile_command.index('-o')+1]=str(assembly)
    run(compile_command)
    roundtrip=folder/'runner-roundtrip.o'
    place.assemble(assembly,roundtrip)
    assert snapshot(obj)==snapshot(roundtrip),'Compiler -S changed allocated runner payloads'
    assert relocation_inventory(obj.read_bytes())==relocation_inventory(roundtrip.read_bytes()),'Compiler -S changed runner relocation targets'
    proof={'original_sha256':place.sha(obj),'assembly_sha256':place.sha(assembly),
           'roundtrip_sha256':place.sha(roundtrip),
           'allocated_symbols_data_and_relocation_targets_equal':True,
           'reference_preprocessor_branch':reference}
    place.write_text(folder/'runner-assembly-proof.json',json.dumps(proof,indent=2)+'\n')


def main():
    args=sys.argv[1:];split=args.index('--utp-compiler')
    prefix=args[:split]
    work_root=None
    if prefix and prefix[0]=='--utp-work-root':
        assert len(prefix)>=2
        work_root=Path(prefix[1]).resolve()
        prefix=prefix[2:]
    delegate=[arg for arg in prefix if arg and not arg.endswith('-NOTFOUND')]
    command=args[split+1:]
    sources=[arg for arg in command if arg.replace('\\','/').endswith('/runner.c')]
    if (not sources or '-DENABLE_WAD16' not in command or
            '-DCTRPORT_LAYERFIX_RELEASE_LAYOUT' not in command):
        return subprocess.run(delegate+command,stdin=subprocess.DEVNULL,timeout=300).returncode
    assert len(sources)==1 and command.count('-o')==1 and command.count('-c')==1
    output=Path(command[command.index('-o')+1]).resolve()
    compiler=shutil.which(command[0])
    assert compiler,('Compiler not found',command[0])
    place.ARM=Path(compiler).resolve().parent
    # Every attempt owns a fresh directory. Preserve failures for inspection;
    # remove only its unqualified output so make cannot reuse it on a retry.
    # CMake's hashed source-object path may already approach MAX_PATH. The
    # GNU assembler cannot open our longer absolute paths there. CMake passes
    # a short directory inside the target's build tree; standalone callers
    # retain their own output directory as the default.
    if work_root is None:
        work_root=output.parent
    work_root.mkdir(parents=True,exist_ok=True)
    work=Path(tempfile.mkdtemp(prefix='runner-tile-layout-',dir=work_root))
    try:
        run(delegate+command)
        capture(command,work/'candidate')
        capture(command,work/'base',reference=True)
        proof=place.place(work,work/'placed')
        installed=work/'placed/runner-placed.o'
        proof.update(reference_source_sha256=hashlib.sha256(Path(sources[0]).read_bytes()).hexdigest(),
                     command=command,work=str(work),installed_sha256=place.sha(installed),
                     no_git_or_external_reference=True)
        place.write_text(output.with_suffix(output.suffix+'.utp-layout.json'),json.dumps(proof,indent=2)+'\n')
        temporary=output.with_suffix(output.suffix+'.utp-install')
        shutil.copy2(installed,temporary)
        os.replace(temporary,output)
        print('Tile runner slots: local reference, original data exact, 0 warnings',flush=True)
    except BaseException:
        output.unlink(missing_ok=True)
        raise
    return 0


if __name__=='__main__':
    sys.exit(main())
