"""UT registration call operands only; original compiler graph and bodies retained."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

MARKER = '--dlgflash-compiler'

def route(text):
    # Four direct source calls: create, persistent carry, room restore, rebuild.
    # Fail closed if the compiler inlines/clones one or changes its call form.
    pattern = r'(?m)^([ \t]+bl[ \t]+)Runner_addInstanceToObjectLists(\(PLT\))?([ \t]*(?:@[^\n]*)?)$'
    routed, count = re.subn(pattern, lambda m: m[1] + 'CtrDlgflash_registered' + (m[2] or '') + m[3], text)
    assert count == 4, ('dlgflash registration calls', count)
    return routed, count

def run(command):
    return subprocess.run(command, stdin=subprocess.DEVNULL, timeout=1800).returncode

def main(args):
    split = args.index(MARKER)
    delegate, command = [a for a in args[:split] if a], args[split + 1:]
    sources = [a for a in command if a.replace('\\', '/').endswith('/runner.c')]
    if not sources or '-DENABLE_WAD16' not in command:
        return run(delegate + command)
    assert len(sources) == 1 and '-c' in command and command.count('-o') == 1
    source = sources[0]
    output = Path(command[command.index('-o') + 1])
    # The existing GML-stub adapter owns runner compilation and emits its
    # final assembly, including retained bridges. Let it finish an ordinary
    # -c command, then route that exact result; sending it -S bypasses its
    # contract and would omit the approved background-dispatch operations.
    assert '--gmlstubs-compiler' in delegate, 'runner GML adapter missing'
    original = output.with_name(output.stem + '_gml1.s')
    receipt_file = output.with_name(output.stem + '_gmlstubs.json')
    patched = output.with_name('dlgflash1.s')
    original.unlink(missing_ok=True)
    receipt_file.unlink(missing_ok=True)
    rc = run(delegate + command)
    if rc: return rc
    assert original.is_file() and receipt_file.is_file(), 'GML delegate did not emit final runner assembly'
    receipt = json.loads(receipt_file.read_text(encoding='utf-8'))
    assert receipt['exit'] == 0 and receipt['source'] == source, 'unqualified GML delegate result'
    before = original.read_text(encoding='utf-8')
    after, count = route(before)
    with patched.open('w', encoding='utf-8', newline='\n') as handle: handle.write(after)
    assemble, index = [command[0]], 1
    while index < len(command):
        arg = command[index]
        if arg in ('-MF', '-MT', '-MQ', '-o'):
            index += 2
            continue
        if arg not in ('-MD', '-MMD', '-MP', '-c') and arg != source: assemble.append(arg)
        index += 1
    rc = run(assemble + ['-Wa,--fatal-warnings', '-x', 'assembler', '-c', str(patched), '-o', str(output)])
    output.with_name('dlgflash.json').write_text(json.dumps(dict(exit=rc, calls=count,
        original_sha256=hashlib.sha256(before.encode()).hexdigest(),
        routed_sha256=hashlib.sha256(after.encode()).hexdigest()), indent=2)+'\n')
    print('dlgflash: %d registration call operands routed' % count, flush=True)
    return rc

if __name__ == '__main__': sys.exit(main(sys.argv[1:]))
