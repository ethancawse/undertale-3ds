"""Restore the UT pack callback's section order without changing assembly bytes.

Retargeting its conversion call removes a local call-graph dependency, so GCC
emits packRow before startThread. The meter wrapper instead emits it between
the late converter and wrapper. Move that complete section back immediately
after CtrPack_convertTile. The full linked layout gate remains mandatory.
"""
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

MARKER = '--redpix-compiler'


def retain_parser_slot(text):
    """Keep the cold parser's original 648-byte extent after epoch reuse.

    Reading rounded mode from the cached epoch removes a second global load;
    GCC also folds two boolean instructions. Only append zero bytes after its
    final literal pool. Check the emitted body before padding: GNU as cannot
    evaluate a size-dependent .if before relaxing this function's alignment.
    """
    if not re.search(r'(?m)^fightpixVersionDelta:', text):
        return text, dict(present=False)
    name = 'CtrPack_parseHeader.part.0'
    ends = list(re.finditer(r'(?m)^[ \t]*\.size[ \t]+' + re.escape(name) + r',[ \t]*\.-' + re.escape(name) + r'[ \t]*$', text))
    starts = list(re.finditer(r'(?m)^' + re.escape(name) + ':$', text))
    assert len(starts) == len(ends) == 1, 'missing/duplicate parser slot'
    start, end = starts[0].start(), ends[0].start()
    assert start < end and not re.search(r'(?m)^\s*\.section\b', text[start:end]), 'nested parser section'
    padding = '\t.space 648-(.-' + name + '), 0\n'
    return text[:end] + padding + text[end:], dict(present=True, body_bytes=640, slot_bytes=648, padding_bytes=8)


def verify_parser_object(path, padded=False):
    """Independently bound the actual ARM ELF section, before and after padding."""
    data = path.read_bytes()
    assert data[:7] == b'\x7fELF\x01\x01\x01', 'expected little-endian ELF32'
    assert struct.unpack_from('<H', data, 18)[0] == 40, 'expected ARM object'
    offset = struct.unpack_from('<I', data, 32)[0]
    stride, count, names = struct.unpack_from('<HHH', data, 46)
    assert stride == 40 and 0 < names < count and offset + stride * count <= len(data), 'invalid ELF sections'
    sections = [struct.unpack_from('<10I', data, offset + i * stride) for i in range(count)]
    table = sections[names]
    strings = data[table[4]:table[4] + table[5]]
    matches = [s for s in sections if strings[s[0]:].split(b'\0', 1)[0] == b'.text.CtrPack_parseHeader.part.0']
    assert len(matches) == 1, 'missing/duplicate parser object section'
    section = matches[0]
    assert section[1] == 1 and section[4] + section[5] <= len(data), 'invalid parser object section'
    assert section[5] == (648 if padded else 640), 'unexpected generated parser extent'
    if padded:
        assert data[section[4] + 640:section[4] + 648] == bytes(8), 'parser tail is not zero padding'


def restore(text):
    def block(name):
        starts = list(re.finditer(r'(?m)^[ \t]*\.section[ \t]+\.text\.' + name + r',.*\n', text))
        ends = list(re.finditer(r'(?m)^[ \t]*\.size[ \t]+' + name + r',[ \t]*\.-' + name + r'[ \t]*\n', text))
        assert len(starts) == len(ends) == 1, ('missing/duplicate callback-order anchor', name)
        start, end = starts[0].start(), ends[0].end()
        assert start < end
        chunk = text[start:end]
        assert len(re.findall(r'(?m)^[ \t]*\.section\b', chunk)) == 1, ('nested section', name)
        assert len(re.findall(r'(?m)^' + name + r':$', chunk)) == 1, ('missing function label', name)
        return start, end, chunk

    start, end, row = block('packRow')
    anchor_start, anchor_end, _ = block('CtrPack_convertTile')
    calls = re.findall(r'(?m)^[ \t]*bl[ \t]+CtrPack_convertTile(Redpix|Fightpix)(?:\(PLT\))?[ \t]*$', row)
    assert len(calls) == 1, 'unexpected conversion call'
    if start >= anchor_end:
        between = text[anchor_end:start]
        if not re.search(r'(?m)^[ \t]*\.section[ \t]+\.text\.', between):
            return text, dict(changed=False, callback_sha256=hashlib.sha256(row.encode()).hexdigest())
        # Only the known late meter converter/wrapper sandwich is a second
        # supported GCC order; keep rejecting unknown callback placements.
        previous = re.search(r'(?m)^[ \t]*\.size[ \t]+CtrPack_convertTileFightpix,[ \t]*\.-CtrPack_convertTileFightpix[ \t]*\n\s*\Z', text[:start])
        following = re.match(r'[ \t]*\.section[ \t]+\.text\.fightpix_late[^\n]*\n', text[end:])
        next_function = re.search(r'(?m)^[ \t]*\.type[ \t]+([^,\s]+),[ \t]*%function', text[end:])
        assert calls == ['Fightpix'] and previous and following and next_function and next_function[1] == 'fightpixPackRow', 'unexpected callback order'
        chunks = [text[:anchor_end], between, row, text[end:]]
        reordered = [chunks[0], chunks[2], chunks[1], chunks[3]]
    else:
        assert end <= anchor_start, 'overlapping callback-order anchors'
        chunks = [text[:start], row, text[end:anchor_end], text[anchor_end:]]
        reordered = [chunks[0], chunks[2], chunks[1], chunks[3]]
    # Reorder complete byte strings only: no instruction, literal, label or directive is rewritten.
    assert Counter(chunks) == Counter(reordered)
    result = ''.join(reordered)
    assert result.count(row) == text.count(row) == 1
    return result, dict(changed=True, callback_sha256=hashlib.sha256(row.encode()).hexdigest())


def run(command):
    return subprocess.run(command, stdin=subprocess.DEVNULL, timeout=1800).returncode


def main(args):
    split = args.index(MARKER)
    delegate, command = [a for a in args[:split] if a], args[split+1:]
    sources = [a for a in command if a.replace('\\', '/').endswith('/ctr_texture_pack.c')]
    if not sources or '-DENABLE_WAD16' not in command or '-DENABLE_WAD17' in command or '-DNDEBUG' not in command:
        return run(delegate + command)
    assert len(sources) == 1 and command.count('-o') == 1 and '-c' in command
    source = sources[0]
    output = Path(command[command.index('-o')+1])
    original, restored = output.with_name('redpix0.s'), output.with_name('redpix1.s')
    emit = list(command)
    emit[emit.index('-c')] = '-S'
    emit[emit.index('-o')+1] = str(original)
    status = run(delegate + emit)
    if status:
        return status
    text, receipt = restore(original.read_text(encoding='utf-8'))
    with restored.open('w', encoding='utf-8', newline='\n') as stream:
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
    def assemble_to(path):
        return run(assemble + ['-Wa,--fatal-warnings', '-x', 'assembler', '-c', str(restored), '-o', str(path)])
    padded, receipt['parser_slot'] = retain_parser_slot(text)
    if receipt['parser_slot']['present']:
        probe = output.with_name('redpix-probe.o')
        status = assemble_to(probe)
        if status:
            return status
        verify_parser_object(probe)
        with restored.open('w', encoding='utf-8', newline='\n') as stream:
            stream.write(padded)
        status = assemble_to(output)
        if not status:
            verify_parser_object(output, padded=True)
        probe.unlink()
    else:
        status = assemble_to(output)
    receipt.update(exit=status)
    output.with_name('redpix-order.json').write_text(json.dumps(receipt, indent=2)+'\n', encoding='utf-8')
    print('redpix callback order: '+json.dumps(receipt), flush=True)
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
