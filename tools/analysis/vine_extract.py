"""Keep main's renderer assembly and add only the retention late sections.

The ordinary pass sees no retention C code. A second pass supplies types and
inline context to ctr_flowery_late.inc; *none* of that pass's original bodies
or ordinary data sections is retained. Existing calls/pointers are routed to
late dispatchers after GCC, preserving all original instruction counts.
The full-layout and original-text byte gates independently verify the result.
"""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys

ENTRIES = ('drawSurfaceImpl', 'clearBoundTarget', 'flushLazy', 'deferSurfaceFree',
           'skipRecOp', 'skipSync', 'ctrSurfaceSetPixels', 'skipClearScreen', 'CtrTextures_destroy')
PREFIX = 'Flowery_dispatch_'
WRAPPER = 'Flowery_wrap_'
CONDITION = r'(?:eq|ne|cs|cc|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?'
LATE = re.compile(r'^\.(?:text|rodata|data|bss)\.late\.perfflowery$')
LATE_LANES = {'perfflowery': LATE,
              # DELTARUNE New's tower face retention (vine_new_retain.inc), its own late sections.
              'flowergpu': re.compile(r'^\.(?:text|rodata|data|bss)\.late\.flowergpu$')}
sha = lambda text: hashlib.sha256(text.encode()).hexdigest()


def late_sections(text, lane='perfflowery'):
    """Take only explicit late sections, without colliding local GCC labels."""
    output, selected, stack, kinds = [], False, [], set()
    for line in text.splitlines():
        section = re.match(r'\s*\.(pushsection|section)\s+([^,\s]+)', line)
        if section:
            if section[1] == 'pushsection':
                stack.append(selected)
            selected = bool(LATE_LANES[lane].fullmatch(section[2]))
            if selected:
                kinds.add(section[2])
        elif re.match(r'\s*\.popsection\b', line):
            assert stack, 'Unbalanced helper section stack'
            old = selected
            selected = stack.pop()
            if old:
                output.append(line)
            continue
        elif re.match(r'\s*\.(?:text|data|bss)\b', line):
            selected = False
        if selected and not re.match(r'\s*\.(?:file|loc|cfi_\w+)\b', line):
            output.append(line)
    assert not stack and '.text.late.'+lane in kinds
    result = '\n'.join(output)+'\n'
    # GCC can emit .global/.type immediately before switching to an object's
    # section. Preserve declarations for retained definitions wherever they
    # appeared, without importing declarations for discarded definitions.
    labels = set(re.findall(r'(?m)^([\w.]+):', result))
    declarations = []
    for line in text.splitlines():
        declared = re.match(r'\s*\.(?:global|weak|type)\s+([\w.]+)(?:\s|,|$)', line)
        if declared and declared[1] in labels:
            declarations.append(line)
    result = '\n'.join(declarations)+'\n'+result
    # A reference to an ordinary rodata/debug label is not silently retained.
    # Give it an explicit late declaration in C instead.
    definitions = set(re.findall(r'(?m)^(\.L[\w.]+):', result))
    # GCC section anchors use .set rather than label syntax. Accept only
    # definitions already retained inside a named late section; references
    # to anchors in ordinary sections must still fail below.
    definitions.update(re.findall(r'(?m)^\s*\.(?:set|equ)\s+(\.L[\w.]+)\s*,', result))
    references = set(re.findall(r'(?<![\w.])(\.L[\w.]+)\b', result))
    assert references <= definitions, ('Late code references excluded local labels', sorted(references-definitions))
    result = re.sub(r'(?<![\w.])\.L([\w.]+)', r'.LFlowery_\1', result)
    # GCC's function-scope objects (message.42, logged.43, ...) can have the
    # same assembly name as an unrelated object in the independent pass.
    objects = set(re.findall(r'(?m)^\s*\.type\s+([\w.]+),\s*%object', result))
    for name in objects:
        if '.' in name:
            result = re.sub(r'(?<![\w.])'+re.escape(name)+r'(?![\w.])', 'FloweryLocal_'+name, result)
    assert not re.search(r'(?m)^drawSurfaceImpl:', result), 'Original draw body must never be copied'
    return result, sorted(kinds)


def route(text):
    counts = {}
    for name in ENTRIES:
        if name != 'CtrTextures_destroy':
            assert len(re.findall(r'(?m)^'+name+r':$', text)) == 1, ('Missing entry', name)
            assert not re.search(r'(?m)^'+name+r'\.(?:constprop|isra|part)\.', text), ('Clone', name)
        target = PREFIX+name
        pattern = r'(?m)^([ \t]+b(?:l)?'+CONDITION+r'[ \t]+)'+name+r'(\(PLT\))?([ \t]*(?:@[^\n]*)?)$'
        text, branches = re.subn(pattern, lambda m: m[1]+target+(m[2] or '')+m[3], text)
        pattern = r'(?m)^([ \t]+\.word[ \t]+)'+name+r'([ \t]*)$'
        text, pointers = re.subn(pattern, lambda m: m[1]+target+m[2], text)
        assert branches+pointers, ('No entry uses', name)
        counts[name] = dict(branches=branches, pointers=pointers)
    return text, counts


def dispatchers():
    text = '\n.pushsection .text.late.perfflowery,"ax",%progbits\n.balign 4\n'
    for name in ENTRIES:
        target = PREFIX+name
        text += (f'.global {target}\n.type {target}, %function\n{target}:\n'
                 'ldr ip, 1f\nldr ip, [ip]\ncmp ip, #0\n'
                 f'beq {name}\nb {WRAPPER+name}\n1: .word floweryHookSwitch\n'
                 f'.size {target}, .-{target}\n')
    return text+'.popsection\n'


def run(command):
    return subprocess.run(command, stdin=subprocess.DEVNULL, timeout=900).returncode


def main():
    args = sys.argv[1:]
    split = args.index('--')
    delegate, command = [x for x in args[:split] if x], args[split+1:]
    sources = [a for a in command if a.replace('\\', '/').endswith('/ctr_renderer.c')]
    if not sources or '-DCTRPORT_TARGET_N3DS' not in command:
        return run(delegate+command)
    assert len(sources) == 1 and '-c' in command and command.count('-o') == 1
    assert not any('CTRPORT_FLOWERY_COMPILE_LATE' in a for a in command)
    output = Path(command[command.index('-o')+1])
    files = {kind: output.with_suffix('.flowery-'+kind+'.s') for kind in ('original', 'helper', 'linked')}
    assembly = list(command)
    assembly[assembly.index('-c')] = '-S'
    assembly[assembly.index('-o')+1] = str(files['original'])
    print('Flowery: original renderer assembly pass', flush=True)
    rc = run(assembly)
    if rc:
        return rc
    helper = list(assembly)
    helper[helper.index('-o')+1] = str(files['helper'])
    # Keep the original dependency file and -MT target from the first pass.
    for option in ('-MF', '-MT', '-MQ'):
        while option in helper:
            at = helper.index(option)
            del helper[at:at+2]
    helper = [a for a in helper if a not in ('-MD', '-MMD', '-MP')]
    helper += ['-DCTRPORT_FLOWERY_COMPILE_LATE']
    print('Flowery: late helper assembly pass', flush=True)
    rc = run(helper)
    if rc:
        return rc
    original = files['original'].read_text(encoding='utf-8')
    extra, sections = late_sections(files['helper'].read_text(encoding='utf-8'))
    routed, counts = route(original)
    linked = routed+'\n'+extra+dispatchers()
    with files['linked'].open('w', encoding='utf-8', newline='\n') as stream:
        stream.write(linked)
    assemble = [command[0]]
    i = 1
    while i < len(command):
        arg = command[i]
        if arg in ('-MF', '-MT', '-MQ', '-o'):
            i += 2
            continue
        if arg in ('-MD', '-MMD', '-MP', '-c') or arg in sources:
            i += 1
            continue
        assemble.append(arg)
        i += 1
    assemble += ['-x', 'assembler', '-c', str(files['linked']), '-o', str(output)]
    rc = run(assemble)
    output.with_suffix('.flowery-routing.json').write_text(json.dumps(dict(
        entries=counts, sections=sections, compiler_command=command, helper_command=helper,
        assembly_command=assemble, files={k: str(v) for k, v in files.items()},
        original_assembly_sha256=sha(original), retained_helper_assembly_sha256=sha(extra),
        linked_assembly_sha256=sha(linked), exit=rc), indent=2)+'\n', encoding='utf-8')
    print('Flowery late entry dispatch: '+json.dumps(counts), flush=True)
    return rc


if __name__ == '__main__':
    sys.exit(main())
