"""Private two-pass renderer launcher, retaining original bodies byte for byte."""
from pathlib import Path
import json
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vine_extract as extract

INTERNAL = ('flushBatch', 'applyTexEnv', 'queueTextureCopy', 'clearBoundTarget',
            'drawSurfaceImpl', 'CtrRenderer_beginFrame', 'ctrDrawSprite',
            'ctrDrawSpritePartColor', 'ctrDrawSpriteTiled', 'ctrDrawSpritePos')
EXTERNAL = ('C3D_SetViewport', 'C3D_SetScissor', 'C3D_FrameDrawOn', 'C3D_FrameBufClear')
ENTRIES = INTERNAL + EXTERNAL
# Calls routed inside one function only: (function, callee, target, calls expected there).
SCOPED = ()


def route(text):
    import flowervramg_palette_retry
    text = flowervramg_palette_retry.retain(text)
    counts = {}
    for name in ENTRIES:
        if name in INTERNAL:
            assert len(re.findall(r'(?m)^'+name+r':$', text)) == 1, name
            assert not re.search(r'(?m)^'+name+r'\.(?:constprop|isra|part)\.', text), name
        target = 'Vine_wrap_'+name
        pattern = r'(?m)^([ \t]+b(?:l)?'+extract.CONDITION+r'[ \t]+)'+name+r'(\(PLT\))?([ \t]*(?:@[^\n]*)?)$'
        text, branches = re.subn(pattern, lambda m: m[1]+target+(m[2] or '')+m[3], text)
        pattern = r'(?m)^([ \t]+\.word[ \t]+)'+name+r'([ \t]*)$'
        text, pointers = re.subn(pattern, lambda m: m[1]+target+m[2], text)
        assert branches+pointers, ('No uses', name)
        counts[name] = dict(branches=branches, pointers=pointers)
    for scope, name, target, expected in SCOPED:
        found = list(re.finditer(r'(?ms)^'+re.escape(scope)+r':\r?\n.*?^\s*\.size\s+'+re.escape(scope)+r',\s*\.-'+re.escape(scope)+r'\s*$', text))
        assert len(found) == 1, ('Scope', scope)
        body = found[0][0]
        pattern = r'(?m)^([ \t]+bl'+extract.CONDITION+r'[ \t]+)'+name+r'(\(PLT\))?([ \t]*(?:@[^\n]*)?)$'
        routed, branches = re.subn(pattern, lambda m: m[1]+target+(m[2] or '')+m[3], body)
        assert branches == expected, ('Scoped calls', scope, name, branches)
        text = text[:found[0].start()]+routed+text[found[0].end():]
        counts[scope+':'+name] = dict(branches=branches, pointers=0)
    return text, counts


def route_settings(text):
    # applyIniOverrides is inlined twice in main: once for early settings,
    # once for renderer-capability settings. Qualify the enclosing function
    # as well as the exact count, rather than assuming one source call means
    # one emitted call or accepting an arbitrary number of substitutions.
    scopes = {}
    for name in ('main','setupNotices.part.0'):
        scopes[name] = re.search(r'(?ms)^'+re.escape(name)+r':\r?\n.*?^\s*\.size\s+'+re.escape(name)+r',\s*\.-'+re.escape(name)+r'\s*$',text)
        assert scopes[name], ('Missing qualified main translation-unit function',name)
    counts = {}
    for name,target,expected_scopes in (
        ('CtrArgui_override','Vine_override',{'main':2}),
        ('CtrArgui_load','Vine_load',{'main':2}),
        ('CtrRenderer_beginFrame','Vine_wrap_CtrRenderer_beginFrame',{'main':1,'setupNotices.part.0':1})):
        pattern = r'(?m)^([ \t]+bl'+extract.CONDITION+r'[ \t]+)'+name+r'(\(PLT\))?([ \t]*(?:@[^\n]*)?)$'
        assert len(re.findall(pattern,text)) == sum(expected_scopes.values()), ('Unexpected caller count',name)
        for scope,expected in expected_scopes.items():
            assert len(re.findall(pattern,scopes[scope][0])) == expected, ('Unexpected qualified caller',name,scope)
        text,count = re.subn(pattern,lambda m:m[1]+target+(m[2] or '')+m[3],text)
        counts[name] = dict(branches=count,pointers=0)
    return text,counts


def run(command):
    return subprocess.run(command, stdin=subprocess.DEVNULL, timeout=1800).returncode


def main(retain_extra=None):
    args = sys.argv[1:]
    split = args.index('--')
    delegate, command = [x for x in args[:split] if x], args[split+1:]
    sources = [a for a in command if a.replace('\\', '/').endswith(('/ctr_renderer.c','/main.c'))]
    # DELTARUNE Old: the renderer and main.c. DELTARUNE New (flowergpu): the renderer only, for tower face
    # retention (vine_new_retain.inc); its main.c compiles as it always did.
    new = '-DCTRPORT_TARGET_N3DS' in command
    if new:
        sources = [a for a in sources if a.replace('\\', '/').endswith('/ctr_renderer.c')]
    if not sources or '-DENABLE_WAD17' not in command or ('-DCTRPORT_TARGET_O3DS' not in command and not new):
        return run(delegate+command)
    assert len(sources) == 1 and '-c' in command and command.count('-o') == 1
    output = Path(command[command.index('-o')+1])
    files = {key: output.with_suffix('.vine-'+key+'.s') for key in ('original', 'helper', 'linked')}
    original = list(command)
    original[original.index('-c')] = '-S'
    original[original.index('-o')+1] = str(files['original'])
    rc = run(original)
    if rc:
        return rc
    helper = []
    if sources[0].replace('\\','/').endswith('/ctr_renderer.c'):
        helper = list(original)
        helper[helper.index('-o')+1] = str(files['helper'])
        for option in ('-MF', '-MT', '-MQ'):
            while option in helper:
                at = helper.index(option)
                del helper[at:at+2]
        helper = [a for a in helper if a not in ('-MD', '-MMD', '-MP')]
        helper += ['-DCTRPORT_VINE_DIAGNOSTIC_HELPER', '-fno-section-anchors']
        rc = run(helper)
        if rc:
            return rc
        extra, sections = extract.late_sections(files['helper'].read_text(encoding='utf-8'), 'flowergpu' if new else 'perfflowery')
        if retain_extra is not None:
            extra = retain_extra(extra)
        routed, counts = route(files['original'].read_text(encoding='utf-8'))
    else:
        # Route file/launch settings through late parsers that chain main's
        # screens wrappers, and the real main-loop/notice beginFrame callers.
        # No new linker wrapper, body resize or instruction insertion.
        extra, sections = '', []
        routed, counts = route_settings(files['original'].read_text(encoding='utf-8'))
    with files['linked'].open('w', encoding='utf-8', newline='\n') as stream:
        stream.write(routed+'\n'+extra)
    assemble = [command[0]]
    i = 1
    while i < len(command):
        arg = command[i]
        if arg in ('-MF', '-MT', '-MQ', '-o'):
            i += 2
            continue
        if arg not in ('-MD', '-MMD', '-MP', '-c') and arg not in sources:
            assemble.append(arg)
        i += 1
    assemble += ['-x', 'assembler', '-c', str(files['linked']), '-o', str(output)]
    rc = run(assemble)
    output.with_suffix('.vine-routing.json').write_text(json.dumps(dict(entries=counts, sections=sections, exit=rc,
        files={k: str(v) for k,v in files.items()}, original_command=original, helper_command=helper), indent=2)+'\n')
    print('Vine diagnostic entry operands: '+json.dumps(counts), flush=True)
    return rc


if __name__ == '__main__':
    sys.exit(main())
