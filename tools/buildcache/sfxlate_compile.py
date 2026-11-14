"""Prepare Old's cooler fan in its existing hidden room entry.

Run the complete normal compiler chain first. Keep its original audio object;
compile the appended unit separately, then merge only its two named sections.
In main.o change one relocation operand, never source, instruction bytes or slots.
"""
from pathlib import Path
import hashlib
import json
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import sfxlate_object as frozen
from portmenu_compile import inventory

MARKER = '--sfxlate-compiler'


def route_main(data):
    h, sections, si, strings, symbols, pool, allocated, edges = inventory(data)
    selected = [e for e in edges if e[4] == 'CtrRenderer_skipPresent']
    assert len(selected) == 1, ('cooler hidden-entry call inventory', selected)
    edge, = selected
    assert edge[3] == 28 and edge[5] == 0 and edge[6] == ['main'], edge
    word = struct.unpack_from('<I', data, sections[edge[1]][4] + edge[2])[0]
    assert word == 0xebfffffe, ('expected original unconditional ARM call', hex(word))
    target = 'sfcSkip'
    assert target.encode() + b'\0' not in pool, 'cooler call already routed'
    # Append non-allocated tables; every original allocated byte and symbol stays.
    out = bytearray(data)
    newpool = pool + target.encode() + b'\0'
    pooloff = len(out)
    out.extend(newpool)
    out.extend(b'\0' * (-len(out) % 4))
    symoff = len(out)
    table = sections[si]
    out.extend(data[table[4]:table[4] + table[5]])
    out.extend(struct.pack('<IIIBBH', len(pool), 0, 0, 0x10, 0, 0))
    struct.pack_into('<I', out, edge[0] + 4, (len(symbols) << 8) | edge[3])
    struct.pack_into('<II', out, h[6] + h[11] * strings + 16, pooloff, len(newpool))
    struct.pack_into('<II', out, h[6] + h[11] * si + 16, symoff, table[5] + 16)
    final = inventory(out)
    assert final[6] == allocated, 'cooler main allocated bytes changed'
    assert final[4][:-1] == symbols, 'cooler main original symbols changed'
    assert final[7] == [(*e[:4], target, 0, e[6]) if e == edge else e for e in edges], 'other relocation changed'
    return bytes(out), dict(caller='main', before='CtrRenderer_skipPresent', after=target,
        relocation=28, section_index=edge[1], offset=edge[2], original_allocated_bytes_identical=True,
        before_sha256=hashlib.sha256(data).hexdigest(), after_sha256=hashlib.sha256(out).hexdigest())


def append_unit(command, output):
    folder = output.parent
    baseline = folder / 'sfc-original-audio.o'
    baseline.write_bytes(output.read_bytes())
    emit = list(command)
    emit[emit.index('-c')] = '-S'
    raw = folder / 'sfc-product-raw.s'
    emit[emit.index('-o') + 1] = str(raw)
    emit += ['-DCTRPORT_SFXLATE_UNIT', '-g0', '-fno-section-anchors',
             '-fno-jump-tables', '-fno-tree-switch-conversion']
    frozen.run(emit)
    assembly = folder / 'sfc-product.s'
    with assembly.open('w', encoding='utf-8', newline='\n') as stream:
        stream.write(frozen.selected_assembly(raw.read_text(encoding='utf-8')))
    unit = folder / 'sfc-product.o'
    frozen.run([frozen.ARM / 'arm-none-eabi-gcc.exe', '-march=armv6k', '-mfloat-abi=hard', '-mfpu=vfp',
                '-Wa,--fatal-warnings', '-c', '-x', 'assembler', assembly, '-o', unit])
    obj = frozen.Object(unit)
    sizes = {name: sum(s[5] for n, s in zip(obj.names, obj.sections) if n == name) for name in frozen.ALLOWED}
    bss = sum(s[5] for s in obj.sections if s[2] & 2 and s[1] == 8)
    receipt = dict(test_only=False, text_bytes=sizes['.text_sfxlate'],
                   rodata_bytes=sizes['.rodata_sfxlate'], bss_bytes=bss, linked=False)
    (folder / 'sfc-product-price.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print('COOLER PRODUCT PRELINK PRICE', json.dumps(receipt), flush=True)
    assert 0 < receipt['text_bytes'] <= 2048 and 0 < receipt['rodata_bytes'] <= 128 and bss == 0, 'cooler price cap'
    frozen.merge(baseline, unit, output, folder, prefix='sfc-product')


def main(args):
    split = args.index(MARKER)
    delegate, command = [a for a in args[:split] if a], args[split + 1:]
    status = subprocess.run(delegate + command, stdin=subprocess.DEVNULL, timeout=1800).returncode
    if status or '-c' not in command:
        return status
    required = {'-DENABLE_WAD16', '-DCTRPORT_TARGET_O3DS', '-DCTRPORT_HAVE_TREMOR'}
    assert required <= set(command) and '-DENABLE_WAD17' not in command, 'CMake cooler target contract'
    sources = [a.replace('\\', '/').rsplit('/', 1)[-1] for a in command
               if a.replace('\\', '/').rsplit('/', 1)[-1] in {'ctr_audio_system.c', 'main.c'}]
    if not sources:
        return status
    assert len(sources) == 1 and command.count('-o') == 1
    output = Path(command[command.index('-o') + 1]).resolve()
    if sources[0] == 'ctr_audio_system.c':
        append_unit(command, output)
    else:
        patched, receipt = route_main(output.read_bytes())
        output.write_bytes(patched)
        output.with_suffix('.sfxlate.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
        print('COOLER PRODUCT MAIN', json.dumps(receipt), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
