"""Keep the original FPS painter slots; append only unreachable zero tails.

The two extents are identical in all four stage74b Release maps. Unknown
compiler output or growth fails assembly. No instruction is rewritten here;
the independent linked-image gate must also prove every old address.
"""
import json
from pathlib import Path
import re
import subprocess
import sys

MARKER = '--fpsbox-compiler'
SLOTS = {'arguiPaintScreen': 672, 'arguiDrawText': 716}


def retain(text):
    for name, size in SLOTS.items():
        pattern = r'(?m)^([ \t]*\.size[ \t]+'+name+r',[ \t]*\.-'+name+r')[ \t]*$'
        replacement = (
            '\t.global __fpsbox_body_end_'+name+'\n'
            '__fpsbox_body_end_'+name+':\n'
            f'\t.space {size} - (. - {name}), 0\n'
            f'\t.size {name}, .-{name}')
        text, count = re.subn(pattern, lambda _: replacement, text)
        assert count == 1, ('Missing/duplicate FPS body', name, count)
    return text


def run(command):
    return subprocess.run(command, stdin=subprocess.DEVNULL, timeout=1800).returncode


def main(args):
    split = args.index(MARKER)
    delegate, command = [a for a in args[:split] if a], args[split+1:]
    sources = [a for a in command if a.replace('\\', '/').endswith('/ctr_argui.c')]
    if not sources or '-DNDEBUG' not in command:
        return run(delegate+command)
    assert len(sources) == 1 and command.count('-o') == 1 and '-c' in command
    source = sources[0]
    output = Path(command[command.index('-o')+1])
    original, padded = output.with_name('fpsbox0.s'), output.with_name('fpsbox1.s')
    emit = list(command)
    emit[emit.index('-c')] = '-S'
    emit[emit.index('-o')+1] = str(original)
    status = run(delegate+emit)
    if status:
        return status
    with padded.open('w', encoding='utf-8', newline='\n') as stream:
        stream.write(retain(original.read_text(encoding='utf-8')))
    assemble, index = [command[0]], 1
    while index < len(command):
        arg = command[index]
        if arg in ('-MF', '-MT', '-MQ', '-o'):
            index += 2
            continue
        if arg not in ('-MD', '-MMD', '-MP', '-c') and arg != source:
            assemble.append(arg)
        index += 1
    # GAS resolves .space after relaxing literal/align fragments; .if cannot
    # evaluate the same difference early in a real GCC debug assembly file.
    # Negative .space is normally just a warning: make it a hard overflow error.
    status = run(assemble+['-Wa,--fatal-warnings', '-x', 'assembler', '-c', str(padded), '-o', str(output)])
    print('fpsbox retained slots: '+json.dumps(dict(slots=SLOTS, exit=status)), flush=True)
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
