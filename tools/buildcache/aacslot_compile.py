"""Retain a routed AOT function's original input extent, in original order.

Installed only by UNDERTALE's AAC linker configuration. The generator gives
the listed function its AAC section; this adapter adds zero storage under
the original input name. It never edits an instruction or relocation.
"""
from pathlib import Path
import re
import subprocess
import sys

MARKER = '--aacslot-compiler'


def rewrite(text, records):
    chunks = re.split(r'(?=^[ \t]*\.section[ \t]+)', text, flags=re.M)
    seen = set()
    for index, chunk in enumerate(chunks):
        match = re.match(r'\s*\.section\s+\.aacout\.text\.(\w+),"ax",%progbits\s*\n', chunk)
        if not match:
            continue
        rid = match[1]
        if rid not in records or rid in seen:
            raise ValueError('unlisted or repeated AAC AOT assembly input: ' + rid)
        size = records[rid]
        names = re.findall(r'^\s*\.type\s+(aotf_\d+),\s*%function\s*$', chunk, re.M)
        if len(names) != 1:
            raise ValueError('expected one AOT definition in ' + rid)
        name = names[0]
        pattern = r'(?m)^([ \t]*\.size[ \t]+' + name + r',[ \t]*\.-' + name + r')[ \t]*$'
        if len(re.findall(pattern, chunk)) != 1:
            raise ValueError('missing AOT size directive: ' + name)
        # A backwards .org refuses a body that exceeds its recorded extent.
        # The independent ELF proof also refuses a smaller or changed body.
        chunk = re.sub(pattern, lambda m: f'\t.org {name}+{size}, 0\n' + m[1], chunk)
        original = (f'\t.section .text.{name},"axR",%progbits\n'
                    f'\t.balign 4\n\t.space {size},0\n')
        chunks[index] = original + chunk
        seen.add(rid)
    if seen != set(records):
        raise ValueError('generated AAC AOT annotation has no matching assembly input')
    return ''.join(chunks)


def run(command):
    return subprocess.run(command, stdin=subprocess.DEVNULL, timeout=1800).returncode


def main(args):
    split = args.index(MARKER)
    delegate, command = [a for a in args[:split] if a], args[split + 1:]
    sources = [a for a in command if re.search(r'(?:^|[/\\])aot_(?:gen|append)_\d+\.c$', a)]
    if len(sources) != 1:
        return run(delegate + command)
    source = sources[0]
    records = {rid: int(size) for rid, size in re.findall(r'/\* AACSLOT (\w+) (\d+) \*/',
                                                        Path(source).read_text(encoding='utf-8'))}
    if not records:
        return run(delegate + command)
    if '-DENABLE_WAD16' not in command:
        raise ValueError('AAC AOT placement requires an UNDERTALE compilation')
    output = Path(command[command.index('-o') + 1])
    original, patched = output.with_suffix('.aacslot0.s'), output.with_suffix('.aacslot1.s')
    emit = list(command)
    emit[emit.index('-c')] = '-S'
    emit[emit.index('-o') + 1] = str(original)
    status = run(delegate + emit)
    if status:
        return status
    patched.write_bytes(rewrite(original.read_text(encoding='utf-8'), records).encode('utf-8'))
    assemble = [command[0]]
    index = 1
    while index < len(command):
        arg = command[index]
        if arg in ('-MF', '-MT', '-MQ', '-o'):
            index += 2
            continue
        if arg not in ('-MD', '-MMD', '-MP', '-c') and arg != source:
            assemble.append(arg)
        index += 1
    return run(assemble + ['-Wa,--fatal-warnings', '-x', 'assembler', '-c', str(patched), '-o', str(output)])


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
