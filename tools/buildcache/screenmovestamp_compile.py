"""Preserve slots; route both move entries and their distinct marker ABIs.

Refuse any unrecognised prologue, marker, epilogue or second application.
No engine source edit, instruction insertion, global timestamp or new state.
"""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys

MARKER = '--screenmovestamp-compiler'
REGISTERS = 'r4, r5, r6, r7, r8, lr'
TRAMPOLINE = '''
    .pushsection .text_screenmovestamp,"ax",%progbits
    .arm
    .balign 4
    .global CtrScreenMoveStamp_body
    .type CtrScreenMoveStamp_body,%function
CtrScreenMoveStamp_body:
    push {r4, r5, r6, r7, r8, lr}
    b screensMoveGame+4
    .size CtrScreenMoveStamp_body,.-CtrScreenMoveStamp_body
    .global CtrScreenMoveStamp_scanoutBody
    .type CtrScreenMoveStamp_scanoutBody,%function
CtrScreenMoveStamp_scanoutBody:
    push {r4, r5, r6, r7, r8, r9, r10, r11, lr}
    b scanoutMove+4
    .size CtrScreenMoveStamp_scanoutBody,.-CtrScreenMoveStamp_scanoutBody
    .popsection
'''


def rewrite_legacy(text):
    assert 'CtrScreenMoveStamp_' not in text, 'Already instrumented renderer'
    match = re.search(r'(?ms)^screensMoveGame:\n(.*?)^\s*\.size\s+screensMoveGame,', text)
    assert match, 'Missing move body'
    body = match[1]
    instructions = list(re.finditer(r'(?m)^[ \t]*(?![.@#])([a-z][a-z0-9]*)[ \t]+([^\n]+)$', body))
    assert instructions and instructions[0][1] == 'push'
    first = instructions[0]
    assert re.fullmatch(r'\{\s*r4,\s*r5,\s*r6,\s*r7,\s*r8,\s*lr\s*\}(?:\s*@.*)?', first[2])
    marker = list(re.finditer(r'(?m)^([ \t]*)b[ \t]+logInfo(\(PLT\))?([ \t]*(?:@[^\n]*)?)$', body))
    assert len(marker) == 1, 'Expected one unconditional marker tail branch'
    tail = marker[0]
    pops = list(re.finditer(r'(?m)^\s*pop\s+\{\s*r4,\s*r5,\s*r6,\s*r7,\s*r8,\s*lr\s*\}[^\n]*$', body[:tail.start()]))
    assert len(pops) == 1, 'Marker must follow the restored original stack'
    between = body[pops[0].end():tail.start()]
    assert not re.search(r'\b(?:sp|push|pop|bl|blx)\b', between), 'Stack/call between pop and marker'
    changes = [(first.start(), first.end(), '    b CtrScreenMoveStamp_enter', first[0]),
        (tail.start(), tail.end(), tail[1]+'b CtrScreenMoveStamp_marker'+(tail[2] or '')+tail[3], tail[0])]
    routed = body
    for begin, end, new, _ in reversed(changes):
        routed = routed[:begin]+new+routed[end:]
    restored = routed
    for _, _, new, old in changes:
        assert restored.count(new) == 1
        restored = restored.replace(new, old, 1)
    assert restored == body, 'Original instruction stream must reconstruct exactly'
    return text[:match.start(1)]+routed+text[match.end(1):]


def rewrite(text):
    legacy = rewrite_legacy(text)
    match = re.search(r'(?ms)^scanoutMove:\n(.*?)^\s*\.size\s+scanoutMove,', legacy)
    assert match, 'Missing active scanout move body'
    body = match[1]
    instructions = list(re.finditer(
        r'(?m)^[ \t]*(?![.@#])([a-z][a-z0-9]*)[ \t]+([^\n]+)$', body))
    assert instructions and instructions[0][1] == 'push'
    first = instructions[0]
    assert re.fullmatch(r'\{\s*r4,\s*r5,\s*r6,\s*r7,\s*r8,\s*r9,\s*'
        r'(?:r10|sl),\s*(?:r11|fp),\s*lr\s*\}(?:\s*@.*)?', first[2]), 'Scanout saved registers'
    # Exactly the frozen 36 B save + 20 B local frame. All failure exits
    # and both success branches share the single restoring epilogue.
    stack = [i for i in instructions if i[1] in ('push', 'pop', 'vpush', 'vpop', 'stmdb', 'ldmia')
             or re.match(r'sp\s*,', i[2])]
    assert len(stack) == 4 and [i[1] for i in stack] == ['push', 'sub', 'add', 'pop'], 'Scanout stack operations'
    assert re.fullmatch(r'sp,\s*sp,\s*#20(?:\s*@.*)?', stack[1][2])
    assert re.fullmatch(r'sp,\s*sp,\s*#20(?:\s*@.*)?', stack[2][2])
    assert re.fullmatch(r'\{\s*r4,\s*r5,\s*r6,\s*r7,\s*r8,\s*r9,\s*'
        r'(?:r10|sl),\s*(?:r11|fp),\s*pc\s*\}(?:\s*@.*)?', stack[3][2])
    markers = list(re.finditer(r'(?m)^([ \t]*)bl[ \t]+logInfo(\(PLT\))?([ \t]*(?:@[^\n]*)?)$', body))
    assert len(markers) == 1, 'Expected one active scanout marker BL'
    marker = markers[0]
    after = [i for i in instructions if i.start() > marker.start()]
    assert len(after) >= 2 and after[0][1] == 'mov' and re.fullmatch(r'r0,\s*#1(?:\s*@.*)?', after[0][2])
    assert after[1][1] == 'b' and re.fullmatch(r'\.L\w+(?:\s*@.*)?', after[1][2]), 'Scanout success must use shared exit'
    label = after[1][2].split()[0]
    exits = list(re.finditer(r'(?m)^'+re.escape(label)+r':\s*$', body))
    assert len(exits) == 1, 'Success must name one shared exit'
    # GCC emits .loc/.cfi directives and comments around the epilogue.
    # Ignore annotations, but require the next two actual instructions to
    # be the exact restoring pair already checked above, with no bypass.
    exit_instructions = [i for i in instructions if i.start() >= exits[0].end()]
    assert len(exit_instructions) >= 2 and [i.start() for i in exit_instructions[:2]] == [
        stack[2].start(), stack[3].start()], 'Success stack must restore 56 B'
    changes = [(first.start(), first.end(), '    b CtrScreenMoveStamp_scanoutEnter', first[0]),
        (marker.start(), marker.end(), marker[1]+'bl CtrScreenMoveStamp_scanoutMarker'+(marker[2] or '')+marker[3], marker[0])]
    routed = body
    for begin, end, new, _ in reversed(changes):
        routed = routed[:begin]+new+routed[end:]
    restored = routed
    for _, _, new, old in changes:
        assert restored.count(new) == 1
        restored = restored.replace(new, old, 1)
    assert restored == body, 'Active scanout instruction stream must reconstruct exactly'
    return legacy[:match.start(1)]+routed+legacy[match.end(1):]+TRAMPOLINE


def run(command):
    return subprocess.run(command, stdin=subprocess.DEVNULL, timeout=1800).returncode


def main(args):
    split = args.index(MARKER)
    delegate, command = [a for a in args[:split] if a], args[split+1:]
    sources = [a for a in command if not a.startswith('-') and a.replace('\\', '/').endswith('/ctr_renderer.c')]
    if not sources or '-DENABLE_WAD16' not in command:
        return run(delegate+command)
    assert len(sources) == command.count('-o') == 1 and ('-c' in command) != ('-S' in command)
    assert Path(sources[0]).parent.name == 'source'
    output = Path(command[command.index('-o')+1])
    assembly_entry = '-S' in command
    original_file = output if assembly_entry else output.with_name('smsrender0.s')
    base = list(command)
    if not assembly_entry:
        base[base.index('-c')] = '-S'
        base[base.index('-o')+1] = str(original_file)
    status = run(delegate+base)
    if status:
        return status
    original = original_file.read_text(encoding='utf-8')
    combined = rewrite(original)
    target = output if assembly_entry else output.with_name('smsrender1.s')
    target.write_bytes(combined.encode('utf-8'))
    if not assembly_entry:
        assemble, i = [command[0]], 1
        while i < len(command):
            arg = command[i]
            if arg in ('-MF', '-MT', '-MQ', '-o'):
                i += 2
                continue
            if arg not in ('-MD', '-MMD', '-MP', '-c') and arg not in sources:
                assemble.append(arg)
            i += 1
        status = run(assemble+['-x', 'assembler', '-c', str(target), '-o', str(output)])
    output.with_name('smsrender.json').write_bytes((json.dumps(dict(exit=status,
        original_sha256=hashlib.sha256(original.encode()).hexdigest(),
        routed_sha256=hashlib.sha256(combined.encode()).hexdigest(),
        original_reconstructs_exact=True, changed_old_words=4, trampoline_bytes=16,
        scanout_live_stack_bytes=56, independent_contexts=True), indent=2)+'\n').encode())
    print('screenmovestamp: four original words; exact displaced prologues; 16 B appended trampolines', flush=True)
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
