"""Retain the dialogue gate's instructions; change two symbol operands only.

Its Other-Off test already passed before the shared-backlight test. Reading
Other-Off again in that second slot admits original 3DS models as well.
Retarget the clear to the guarded teardown helper. No wait predicate, branch,
instruction count, original function extent or warm main.c byte is changed.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

MARKER = '--fpsghost-return-compiler'
CALLER = 'CtrBlank_dialogueShown'


def rewrite(text):
    scopes = list(re.finditer(r'(?ms)^'+CALLER+r':\n.*?^\s*\.size\s+'+CALLER+r',\s*\.-'+CALLER+r'\s*$', text))
    assert len(scopes) == 1, 'Missing/duplicate dialogue teardown scope'
    scope = scopes[0]
    before = scope[0]
    pointer = re.compile(r'(?m)^([ \t]*\.word[ \t]+)CtrScreens_sharedBacklight([ \t]*(?:@[^\n]*)?)$')
    call = re.compile(r'(?m)^([ \t]+bl[ \t]+)blankClearShown(\(PLT\))?([ \t]*(?:@[^\n]*)?)$')
    assert len(pointer.findall(before)) == len(call.findall(before)) == 1, 'Unknown teardown operands'
    assert re.search(r'(?m)^\s*\.word\s+CtrScreens_otherOff\s*$', before), 'Missing preceding Off test'
    after = pointer.sub(lambda m:m[1]+'CtrScreens_otherOff'+m[2], before)
    after = call.sub(lambda m:m[1]+'CtrFpsghost_reclaimOtherOff'+(m[2] or '')+m[3], after)
    # Reverse the two exact occurrences, proving all surrounding assembly exact.
    other_lines = list(re.finditer(r'(?m)^\s*\.word\s+CtrScreens_otherOff\s*$', after))
    assert len(other_lines) == 2
    # The replacement position is derived from the original operand, not order.
    original_pointer = pointer.search(before)
    changed_prefix = pointer.sub(lambda m:m[1]+'CtrScreens_otherOff'+m[2], before[:original_pointer.end()])
    pointer_at = changed_prefix.rfind('CtrScreens_otherOff')
    restored = after.replace('CtrFpsghost_reclaimOtherOff', 'blankClearShown')
    restored = restored[:pointer_at]+'CtrScreens_sharedBacklight'+restored[pointer_at+len('CtrScreens_otherOff'):]
    assert restored == before
    return text[:scope.start()]+after+text[scope.end():]


def run(command):
    return subprocess.run(command, stdin=subprocess.DEVNULL, timeout=1800).returncode


def main(args):
    split = args.index(MARKER)
    delegate, command = [a for a in args[:split] if a], args[split+1:]
    sources = [a for a in command if a.replace('\\','/').endswith('/ctr_gpu_queue.c')]
    if not sources or '-DENABLE_WAD16' not in command:
        return run(delegate+command)
    assert len(sources) == command.count('-o') == 1 and ('-c' in command) != ('-S' in command)
    source = sources[0]
    output = Path(command[command.index('-o')+1])
    assembly_entry = '-S' in command
    original = output if assembly_entry else output.with_name('fpsghost-return0.s')
    emit = list(command)
    if not assembly_entry:
        emit[emit.index('-c')] = '-S'
        emit[emit.index('-o')+1] = str(original)
    status = run(delegate+emit)
    if status:
        return status
    before = original.read_text(encoding='utf-8')
    after = rewrite(before)
    target = output if assembly_entry else output.with_name('fpsghost-return1.s')
    target.write_bytes(after.encode('utf-8'))
    if not assembly_entry:
        assemble, i = [command[0]], 1
        while i < len(command):
            arg = command[i]
            if arg in ('-MF','-MT','-MQ','-o'):
                i += 2
                continue
            if arg not in ('-MD','-MMD','-MP','-c') and arg != source:
                assemble.append(arg)
            i += 1
        status = run(assemble+['-Wa,--fatal-warnings','-x','assembler','-c',str(target),'-o',str(output)])
    output.with_name('fpsghost-return.json').write_bytes((json.dumps(dict(
        original_sha256=hashlib.sha256(before.encode()).hexdigest(),
        rewritten_sha256=hashlib.sha256(after.encode()).hexdigest(),
        caller=CALLER, changed_symbol_operands=2, restored_original_exact=True,
        instruction_count_changed=False, exit=status),indent=2)+'\n').encode('utf-8'))
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
