"""Keep the benchmark TU slots; append ready-clear code and change four calls.

No global console/renderer wrapper. Only beginMeasuring and benchPreTick
change call operands; all normal-play instructions and slots are retained.
"""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys

MARKER = '--asrhold-benchready-compiler'
SECTION = '.text_asrholdbenchready'
ROUTES = {
    'beginMeasuring': {'consoleSelect': ('CtrBench_readySkipSelect', 1),
                       'consoleClear': ('CtrBench_readySkipClear', 1)},
    'benchPreTick': {'BenchScript_tick': ('CtrBench_readyScriptTick', 2)},
}


def late_section(text):
    selected, stack, output = False, [], []
    for line in text.splitlines():
        push = re.match(r'\s*\.pushsection\s+([^,\s]+)', line)
        if push:
            assert not selected and push[1] != SECTION
            stack.append(False)
            continue
        if re.match(r'\s*\.popsection\b', line):
            assert stack
            selected = stack.pop()
            continue
        assert not re.match(r'\s*\.previous\b', line)
        match = re.match(r'\s*\.section\s+([^,\s]+)', line)
        if match:
            assert not stack or match[1] != SECTION
            selected = match[1] == SECTION
        elif re.match(r'\s*\.(?:text|data|bss)\b', line):
            selected = False
        if selected and not re.match(r'\s*\.(?:file|loc|cfi_\w+)\b', line):
            output.append(line)
    assert not stack and output, 'Missing appended benchmark unit'
    body = '\n'.join(output)+'\n'
    labels = set(re.findall(r'(?m)^([\w.]+):', body))
    assert {'CtrBench_readySuppressStatus', 'CtrBench_readySkipSelect',
            'CtrBench_readySkipClear', 'CtrBench_readyScriptTick'} <= labels
    declarations = [line for line in text.splitlines()
        if (m := re.match(r'\s*\.(?:global|weak|type)\s+([\w.]+)(?:\s|,|$)', line)) and m[1] in labels]
    body = '\n'.join(declarations)+'\n'+body
    definitions = set(re.findall(r'(?m)^(\.L[\w.]+):', body))
    references = set(re.findall(r'(?<![\w.])(\.L[\w.]+)\b', body))
    assert references <= definitions, ('Discarded helper-local dependency', sorted(references-definitions))
    return re.sub(r'(?<![\w.])\.L([\w.]+)', r'.LBenchReady_\1', body)


def rewrite(text):
    original, changes = text, []
    for caller, targets in ROUTES.items():
        match = re.search(r'(?ms)^'+caller+r':\n(.*?)^\s*\.size\s+'+caller+r',', text)
        assert match, ('Missing benchmark caller', caller)
        body = match[1]
        for old, (new, count) in targets.items():
            pattern = re.compile(r'(?m)^(\s*(?:bl|b)\s+)'+old+r'(\(PLT\))?([ \t]*(?:@[^\n]*)?)$')
            assert len(pattern.findall(body)) == count, (caller, old, count)
            body = pattern.sub(lambda m: m[1]+new+(m[2] or '')+m[3], body)
            changes.append(dict(caller=caller, old=old, new=new, count=count))
        text = text[:match.start(1)]+body+text[match.end(1):]
    restored = text
    for row in changes:
        match = re.search(r'(?ms)^'+row['caller']+r':\n(.*?)^\s*\.size\s+'+row['caller']+',', restored)
        body = re.sub(r'(?m)^(\s*(?:bl|b)\s+)'+row['new']+r'(?=\(PLT\)|[ \t]*$)',
                      lambda m: m[1]+row['old'], match[1])
        restored = restored[:match.start(1)]+body+restored[match.end(1):]
    assert restored == original and sum(row['count'] for row in changes) == 4
    return text, changes


def run(command):
    return subprocess.run(command, stdin=subprocess.DEVNULL, timeout=1800).returncode


def main(args):
    split = args.index(MARKER)
    delegate, command = [a for a in args[:split] if a], args[split+1:]
    sources = [a for a in command if not a.startswith('-') and a.replace('\\', '/').endswith('/ctr_benchmark.c')]
    if not sources or '-DENABLE_WAD16' not in command:
        return run(delegate+command)
    assert len(sources) == command.count('-o') == 1 and ('-c' in command) != ('-S' in command)
    source = Path(sources[0])
    assert source.parent.name == 'source'
    output = Path(command[command.index('-o')+1])
    assembly_entry = '-S' in command
    base_file = output if assembly_entry else output.with_name('brbench0.s')
    base = list(command)
    if not assembly_entry:
        base[base.index('-c')] = '-S'
        base[base.index('-o')+1] = str(base_file)
    status = run(delegate+base)
    if status:
        return status
    helper_file = output.with_name('brbench1.s')
    helper = list(base)
    helper[helper.index(sources[0])] = str(source.parent/'asrholdbenchready/ctr_benchmark.c')
    helper[helper.index('-o')+1] = str(helper_file)
    for flag in ('-MF', '-MT', '-MQ'):
        while flag in helper:
            at = helper.index(flag)
            del helper[at:at+2]
    helper = [a for a in helper if a not in ('-MD', '-MMD', '-MP')]
    status = run(helper+['-fno-section-anchors'])
    if status:
        return status
    original = base_file.read_text(encoding='utf-8')
    routed, changes = rewrite(original)
    combined = routed+'\n'+late_section(helper_file.read_text(encoding='utf-8'))
    target = output if assembly_entry else output.with_name('brbench2.s')
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
    output.with_name('brbench.json').write_bytes((json.dumps(dict(
        original_sha256=hashlib.sha256(original.encode()).hexdigest(),
        restored_original_exact=True, changes=changes, exit=status), indent=2)+'\n').encode('utf-8'))
    print('benchready: 4 fixed-slot call changes; original assembly otherwise exact; new section appended', flush=True)
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
