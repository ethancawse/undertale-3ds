"""Retarget cold LCD calls after GCC preserves the base's local call graph.

Changing these calls in C changes GCC's global emission order, constant pool
and optimized helper signatures. Keep the source calls; replace only named
branch operands in the emitted assembly. Full linked layout remains mandatory.
"""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys

MARKER = '--dimlight-compiler'
ROUTES = {
    'ctr_renderer.c': ('screensTopBacklight', 'CtrDimlight_topBacklight', {
        'screensConfigureKeepBottom': 2, 'screensDestroy': 1,
        '__wrap_CtrRenderer_leaveForExit': 1,
    }),
    'ctr_rosalina.c': ('rosaBacklight', 'CtrDimlight_watcherPower', {'rosaMain': 3}),
}
BACKLIGHT_ORDER = ('backlightsOn', 'backlightAptEvent', '__wrap_GSPLCD_PowerOnBacklight',
                   '__wrap_GSPLCD_PowerOffBacklight', '__wrap_gfxExit',
                   '__wrap_CtrRenderer_requestPlayerScreenshot')


def order_backlights(text):
    """Restore complete compiler chunks in the existing menupause input slot."""
    sizes = list(re.finditer(r'(?m)^\s*\.size\s+([\w.]+),\s*\.\-\1\s*$', text))
    first = re.search(r'(?m)^\s*\.section\s+[^\n]*\n', text)
    assert sizes and first and first.start() < sizes[0].start(), 'missing function sections'
    prefix, suffix = text[:first.start()], text[sizes[-1].end():]
    chunks, position, section = [], first.start(), None
    for match in sizes:
        part = text[position:match.end()]
        names = re.findall(r'(?m)^\s*\.section\s+([^,\s]+)[^\n]*', part)
        if names:
            section = names[-1]
        chunks.append(dict(name=match[1], section=section, text=part))
        position = match.end()
    indices = [i for i, chunk in enumerate(chunks) if chunk['section'] == '.text.late.menupause']
    chosen = [chunks[i] for i in indices]
    assert sorted(c['name'] for c in chosen) == sorted(BACKLIGHT_ORDER), 'menupause inventory'
    selected = {chunk['name']: chunk for chunk in chosen}
    assert all(len(re.findall(r'(?m)^'+re.escape(c['name'])+r':$', c['text'])) == 1
               for c in chosen), 'missing/ambiguous menupause label'
    for index, name in zip(indices, BACKLIGHT_ORDER):
        # A moved chunk may have inherited its input section. Reassert it;
        # every instruction, local label, literal pool and size remains intact.
        chunks[index] = dict(selected[name], text='\n\t.section .text.late.menupause,"ax",%progbits\n'
                            + selected[name]['text'])
    return prefix + ''.join(c['text'] for c in chunks) + suffix, dict(
        source='ctr_screens.c', original_order=[c['name'] for c in chosen],
        restored_order=list(BACKLIGHT_ORDER), complete_chunks=True)


def route(text, source):
    old, new, callers = ROUTES[source]
    edits, witnesses = [], []
    for caller, count in callers.items():
        bodies = list(re.finditer(r'^' + re.escape(caller) + r':\n.*?^\s*\.size\s+'
                                 + re.escape(caller) + r',[^\n]*\n', text, re.M | re.S))
        assert len(bodies) == 1, ('missing/ambiguous LCD caller', caller)
        body = bodies[0]
        branches = list(re.finditer(r'(?m)^[ \t]*b[a-z]{0,3}[ \t]+(?P<target>'
                                   + re.escape(old) + r'(?:\.isra\.\d+)?)(?:\(PLT\))?[ \t]*$', body[0]))
        assert len(branches) == count, ('LCD branch inventory', caller, len(branches), count)
        for branch in branches:
            start = body.start() + branch.start('target')
            end = body.start() + branch.end('target')
            edits.append((start, end, branch['target']))
        witnesses.append(dict(caller=caller, branches=count, target=new))
    result = text
    for start, end, original in sorted(edits, reverse=True):
        assert result[start:end] == original
        result = result[:start] + new + result[end:]
    # Exact inverse: no instruction, directive, label, ordering or pool change.
    restored = result
    for start, end, original in sorted(edits):
        at = start
        assert restored[at:at+len(new)] == new
        restored = restored[:at] + original + restored[at+len(new):]
        # Earlier inverse edits have restored the original offsets already.
    assert restored == text, 'non-operand assembly change'
    return result, dict(source=source, edges=witnesses, changed_operands=len(edits),
                        original_sha256=hashlib.sha256(text.encode()).hexdigest(),
                        routed_sha256=hashlib.sha256(result.encode()).hexdigest())


def run(command):
    return subprocess.run(command, stdin=subprocess.DEVNULL, timeout=1800).returncode


def main(args):
    split = args.index(MARKER)
    delegate, command = [a for a in args[:split] if a], args[split+1:]
    # The renderer already has an assembly-emitting utwindow/vine adapter.
    # It calls route() itself after its existing edits. Passing -S to that
    # adapter would bypass its required -c entry and lose those edits.
    sources = [a for a in command if Path(a.replace('\\', '/')).name in ('ctr_rosalina.c', 'ctr_screens.c')]
    if not sources or '-DENABLE_WAD16' not in command or '-c' not in command:
        return run(delegate + command)
    if Path(sources[0].replace('\\', '/')).name == 'ctr_screens.c' and (
            '-DNDEBUG' not in command or '-DCTRPORT_PLAYER_SHOTS' in command):
        return run(delegate + command)
    assert len(sources) == 1 and command.count('-o') == 1
    source = sources[0]
    output = Path(command[command.index('-o')+1])
    original = output.with_suffix('.dimlight0.s')
    patched = output.with_suffix('.dimlight1.s')
    emit = list(command)
    emit[emit.index('-c')] = '-S'
    emit[emit.index('-o')+1] = str(original)
    status = run(delegate + emit)
    if status:
        return status
    name = Path(source.replace('\\', '/')).name
    text = original.read_text(encoding='utf-8')
    text, receipt = order_backlights(text) if name == 'ctr_screens.c' else route(text, name)
    # Path.write_text(newline=) needs Python 3.10; release builders support 3.9.
    with patched.open('w', encoding='utf-8', newline='\n') as stream:
        stream.write(text)
    assemble, index = [command[0]], 1
    while index < len(command):
        arg = command[index]
        if arg in ('-MF', '-MT', '-MQ', '-o'):
            index += 2
            continue
        if arg not in ('-MD', '-MMD', '-MP', '-c') and arg != source:
            assemble.append(arg)
        index += 1
    status = run(assemble + ['-Wa,--fatal-warnings', '-x', 'assembler', '-c', str(patched), '-o', str(output)])
    receipt['exit'] = status
    output.with_suffix('.dimlight.json').write_text(json.dumps(receipt, indent=2)+'\n', encoding='utf-8')
    print('dimlight LCD call operands: ' + json.dumps(receipt), flush=True)
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
