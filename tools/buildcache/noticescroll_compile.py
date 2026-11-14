"""Preserve legalui's call graph and slots; route clear calls and reader file edges to late helpers.

UNDERTALE only. The original local clone takes PrintConsole* and preserves r1;
the replacement's bridge keeps that extra preservation. Unknown ABIs fail closed.
"""
from pathlib import Path
import hashlib
import json
import re
import struct
import subprocess
import sys

MARKER = '--noticescroll-compiler'
TARGET = 'CtrNoticescroll_clear'
OBJECT_ROUTES = {
    'ctr_audio_system.c': dict(caller='__wrap_CtrMenu_close', section='.text.late.menupause',
        old='__real_CtrMenu_close', new='CtrNoticescroll_menuClose', stem='menu', key='menu_close'),
    'ctr_benchmark.c': dict(caller='__wrap_CtrExit_begin', section='.text.late.pacer_rows',
        old='__real_CtrExit_begin', new='CtrNoticescroll_beginExit', stem='exit', key='exit_begin'),
}


def check_clone(body):
    """Accept only understood leaf instructions; r1 is read-only, r2/r3/ip are clobbered.

    Rejecting unknown instructions is deliberate: this is an ABI guard, not an ARM disassembler.
    In particular calls, multi-register loads/ranges and hidden writebacks must not evade the r1 check.
    """
    conditions = ('', 'eq', 'ne', 'cs', 'cc', 'hs', 'lo', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', 'al')
    destinations = {'mov', 'mvn', 'add', 'sub', 'rsb', 'adc', 'sbc', 'rsc', 'and', 'orr', 'eor', 'bic',
                    'clz', 'lsl', 'lsr', 'asr', 'ror', 'rrx', 'mul', 'mla', 'ldr', 'ldrb', 'ldrh', 'ldrsb', 'ldrsh'}
    readonly = {'cmp', 'cmn', 'tst', 'teq', 'str', 'strb', 'strh', 'push', 'pop', 'b', 'bx', 'nop'}
    forms = {base + suffix + cond: base for base in destinations | readonly
             for cond in conditions for suffix in (('', 's') if base in destinations - {'ldr', 'ldrb', 'ldrh', 'ldrsb', 'ldrsh'} else ('',))}
    aliases = {'a1': 'r0', 'a2': 'r1', 'a3': 'r2', 'a4': 'r3', 'ip': 'r12'}
    labels = set(re.findall(r'(?m)^(\.L[\w.]+):', body))
    written = set()
    for line in body.splitlines():
        line = line.split('@', 1)[0].strip()
        if not line or line.startswith('.') or line.endswith(':'):
            continue
        parts = line.split(None, 1)
        op, operands = parts[0], parts[1] if len(parts) > 1 else ''
        assert op in forms, ('unknown clear instruction or call', line)
        base = forms[op]
        operands = re.sub(r'\b(a[1-4]|ip)\b', lambda m: aliases[m[0]], operands)
        # Only compares/tests may mention r1. This conservative rule covers destination operands,
        # register lists, post/pre-index writeback, shifts, multiplies and aliases without guessing.
        assert not re.search(r'\br1\b', operands) or base in {'cmp', 'cmn', 'tst', 'teq'}, \
            ('original no longer preserves r1', line)
        if base in {'push', 'pop'}:
            assert re.fullmatch(r'\{(?:r(?:[0-9]|1[0-2])|lr|pc)(?:,\s*(?:r(?:[0-9]|1[0-2])|lr|pc))*\}', operands), \
                ('unknown register list', line)
        if base == 'b':
            assert operands in labels, ('clear branches outside its body', line)
        if base == 'bx':
            assert operands == 'lr', ('unknown clear return', line)
        if base in destinations:
            dest = operands.split(',', 1)[0]
            assert re.fullmatch(r'r(?:[0-9]|1[0-2])|lr|pc|sp', dest), ('unknown destination', line)
            assert dest != 'pc', ('unknown clear control transfer', line)
            written.add(dest)
    # The bridge promises r1 only. The frozen clone already writes all three other AAPCS scratch registers;
    # if a compiler starts preserving one, callers may rely on it and need a new bridge/caller review.
    assert {'r2', 'r3', 'r12'} <= written, ('clear scratch-register contract changed', sorted(written))


def rewrite(text, allow_missing=False):
    before = text
    names = re.findall(r'(?m)^(luiClear\.isra\.\d+):$', text)
    if not names and allow_missing:
        return text, dict(calls=0, reason='unoptimized build without the private clone')
    assert len(names) == 1, ('clear clone ABI changed', names)
    name = names[0]
    start = text.index(name + ':')
    end = text.index('\n\t.size\t' + name + ',', start)
    body = text[start:end]
    # GCC replaced CtrMenu* with menu->console (PrintConsole*): offset 8 is its framebuffer.
    assert re.search(r'(?m)^\s*ldr\s+r0,\s*\[r0,\s*#8\]', body), 'clear first argument changed'
    check_clone(body)
    pattern = re.compile(r'(?m)^(\s*bl\s+)' + re.escape(name) + r'(\(PLT\))?([ \t]*(?:@[^\n]*)?)$')
    matches = list(pattern.finditer(text))
    assert len(matches) == 3, ('clear call count', len(matches))
    callers = []
    for match in matches:
        labels = re.findall(r'(?m)^([A-Za-z_][\w.]*)\:$', text[:match.start()])
        assert labels, 'missing clear caller'
        callers.append(labels[-1])
    expected = ('luiDrawHub', 'luiDrawDoc', 'luiDrawCredits')
    assert sorted(re.sub(r'\.isra\.\d+$', '', n) for n in callers) == sorted(expected), callers
    text = pattern.sub(lambda m: m[1] + TARGET + (m[2] or '') + m[3], text)
    reverse = re.compile(r'(?m)^(\s*bl\s+)' + TARGET + r'(\(PLT\))?([ \t]*(?:@[^\n]*)?)$')
    assert reverse.sub(lambda m: m[1] + name + (m[2] or '') + m[3], text) == before, 'non-call assembly changed'
    return text, dict(calls=3, callers=callers, old=name, new=TARGET,
                      before_sha256=hashlib.sha256(before.encode()).hexdigest(),
                      after_sha256=hashlib.sha256(text.encode()).hexdigest())


def run(command):
    return subprocess.run(command, stdin=subprocess.DEVNULL, timeout=1800).returncode


def rewrite_cache(text):
    """Route only the reader's file edges and the existing held-update tail; every instruction stays in its slot."""
    before = text
    routes = {
        'luiDrawDoc': {'luiOpen': 'CtrNoticescroll_open', 'fread': 'CtrNoticescroll_read',
                       'fclose': 'CtrNoticescroll_close'},
        'luiRead': {'fseek': 'CtrNoticescroll_seek', 'fread': 'CtrNoticescroll_read'},
        '__wrap_CtrLegalui_update': {'CtrCredroll_update': 'CtrNoticescroll_update'},
    }
    # luiOpen calls libc fopen, so GCC cannot infer any extra volatile-register preservation from its body.
    original_open = re.search(r'(?ms)^luiOpen:\n(.*?)^\s*\.size\s+luiOpen,', text)
    assert original_open and re.search(r'(?m)^\s*(?:b|bl)\s+fopen(?:\(PLT\))?\s*$', original_open[1]), \
        'reader open no longer has the ordinary libc scratch-register contract'
    changes = []
    for caller, mapping in routes.items():
        blocks = list(re.finditer(r'(?ms)^(' + re.escape(caller) + r'(?:\.isra\.\d+)?):\n(.*?)^\s*\.size\s+\1,', text))
        assert len(blocks) == 1, ('reader function changed', caller, len(blocks))
        match = blocks[0]
        body = match[2]
        for old, new in mapping.items():
            pattern = re.compile(r'(?m)^(\s*(?:b|bl)\s+)' + re.escape(old) + r'(\(PLT\))?([ \t]*(?:@[^\n]*)?)$')
            assert len(pattern.findall(body)) == 1, ('reader edge changed', caller, old)
            changed = pattern.sub(lambda m: m[1] + new + (m[2] or '') + m[3], body)
            reverse = re.compile(r'(?m)^(\s*(?:b|bl)\s+)' + new + r'(\(PLT\))?([ \t]*(?:@[^\n]*)?)$')
            # No helper is shared by two old targets within one caller.
            assert reverse.sub(lambda m: m[1] + old + (m[2] or '') + m[3], changed) == body
            changes.append(dict(caller=match[1], old=old, new=new))
            body = changed
        text = text[:match.start(2)] + body + text[match.end(2):]
    assert len(changes) == 6
    # Account for every changed operand; all original function bodies, literal pools and call graph stay emitted.
    restored = text
    for change in changes:
        name = re.escape(change['caller'])
        pattern = re.compile(r'(?ms)^' + name + r':\n(.*?)^\s*\.size\s+' + name + ',')
        match = pattern.search(restored)
        body = re.sub(r'(?m)^(\s*(?:b|bl)\s+)' + change['new'] + r'(?=\(PLT\)|[ \t]*$)',
                      lambda m: m[1] + change['old'], match[1])
        restored = restored[:match.start(1)] + body + restored[match.end(1):]
    assert restored == before, 'non-reader operands changed'
    return text, changes


def wrapped_call_inventory(data, route):
    """Qualify one existing wrapper's sole real-call branch; snapshot every allocated byte."""
    assert data[:7] == b'\x7fELF\x01\x01\x01', 'expected little-endian ELF32'
    header = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    assert header[1:3] == (1, 40), 'expected relocatable ARM object'
    sections = [struct.unpack_from('<10I', data, header[6] + header[11] * i) for i in range(header[12])]
    names = sections[header[13]]
    names = data[names[4]:names[4] + names[5]]

    def name(pool, offset):
        return pool[offset:].split(b'\0', 1)[0].decode()

    allocated, callers, edges = {}, [], []
    for section in sections:
        section_name = name(names, section[0])
        if section[2] & 2:
            assert section_name not in allocated
            allocated[section_name] = (section[1], section[2], section[5], section[8],
                data[section[4]:section[4] + section[5]] if section[1] != 8 else b'')
        if section[1] == 2: # SHT_SYMTAB
            strings = sections[section[6]]
            pool = data[strings[4]:strings[4] + strings[5]]
            for offset in range(section[4], section[4] + section[5], section[9]):
                symbol = struct.unpack_from('<IIIBBH', data, offset)
                if name(pool, symbol[0]) == route['caller']:
                    assert symbol[3] & 15 == 2 and symbol[2] > 0, 'wrapper is not a function'
                    callers.append((symbol[5], symbol[1], symbol[2]))
        if section[1] != 9: # SHT_REL
            continue
        symbols = sections[section[6]]
        strings = sections[symbols[6]]
        pool = data[strings[4]:strings[4] + strings[5]]
        for offset in range(section[4], section[4] + section[5], section[9]):
            address, info = struct.unpack_from('<II', data, offset)
            symbol = struct.unpack_from('<IIIBBH', data, symbols[4] + (info >> 8) * symbols[9])
            target = name(pool, symbol[0])
            if target in (route['old'], route['new']):
                edges.append((section[7], address, info & 255, target, symbol[5]))
    assert len(callers) == 1 and len(edges) == 1, ('wrapper call inventory changed', route, callers, edges)
    section, start, size = callers[0]
    edge = edges[0]
    assert name(names, sections[section][0]) == route['section'], 'wrapper section changed'
    assert edge[0] == section and start <= edge[1] < start + size and edge[2] in (28, 29) and edge[4] == 0, \
        ('unqualified real-call relocation', edge)
    instruction = struct.unpack_from('<I', data, sections[section][4] + edge[1])[0]
    assert instruction & 0xfe000000 == 0xea000000, 'real-call relocation is not an ARM branch'
    return allocated, edges, callers[0]


def route_wrapped_call(delegate, command, output, route):
    # audpar and other layout launchers own audio's assembly. Let the entire normal chain finish first.
    status = run(delegate + command)
    if status:
        return status
    before = output.read_bytes()
    allocated, edges, caller = wrapped_call_inventory(before, route)
    assert edges[0][3] == route['old'], 'real-call edge already routed'
    original = output.with_name('noticescroll-' + route['stem'] + '-original.o')
    original.write_bytes(before)
    compiler = Path(command[0])
    tool, count = re.subn(r'gcc(\.exe)?$', r'objcopy\1', compiler.name)
    assert count == 1, ('unqualified compiler', compiler)
    status = run([str(compiler.with_name(tool)), '--redefine-sym',
                  route['old'] + '=' + route['new'], str(output)])
    if status:
        return status
    after = output.read_bytes()
    final_allocated, final_edges, final_caller = wrapped_call_inventory(after, route)
    assert final_allocated == allocated and final_caller == caller, 'allocated object bytes changed'
    assert final_edges == [(*edges[0][:3], route['new'], 0)], 'unexpected real-call rewrite'
    receipt = dict({route['key']: dict(caller=route['caller'], old=edges[0][3], new=final_edges[0][3])},
                   before_sha256=hashlib.sha256(before).hexdigest(), after_sha256=hashlib.sha256(after).hexdigest(),
                   edges=edges, routed_edges=final_edges, allocated_bytes_changed=0, exit=0)
    output.with_name('noticescroll-' + route['stem'] + '-routing.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('Noticescroll ' + route['key'] + ': ' + json.dumps(receipt), flush=True)
    return 0


def main(args):
    split = args.index(MARKER)
    delegate, command = [a for a in args[:split] if a], args[split + 1:]
    sources = [a for a in command if a.replace('\\', '/').endswith(('/ctr_legal.c', '/ctr_audio_system.c', '/ctr_benchmark.c'))]
    if not sources or '-c' not in command or '-DENABLE_WAD16' not in command or '-DENABLE_WAD17' in command:
        return run(delegate + command)
    assert len(sources) == 1 and '-c' in command and command.count('-o') == 1
    source = sources[0]
    output = Path(command[command.index('-o') + 1])
    name = source.replace('\\', '/').rsplit('/', 1)[-1]
    if name in OBJECT_ROUTES:
        return route_wrapped_call(delegate, command, output, OBJECT_ROUTES[name])
    prefix = 'noticescroll'
    original, patched = output.with_name(prefix + '0.s'), output.with_name(prefix + '1.s')
    emit = list(command)
    emit[emit.index('-c')] = '-S'
    emit[emit.index('-o') + 1] = str(original)
    result = run(delegate + emit)
    if result:
        return result
    text = original.read_text(encoding='utf-8')
    optimization = next((a for a in reversed(command) if re.fullmatch(r'-O(?:[0-3sgz]|fast)', a)), None)
    text, receipt = rewrite(text, allow_missing=optimization in (None, '-O0', '-O1', '-Og'))
    if receipt['calls'] == 0:
        # Preserve the normal compiler's debug/object emission when there is no private ABI to replace.
        return run(delegate + command)
    text, cache_routes = rewrite_cache(text)
    receipt['cache_calls'] = cache_routes
    receipt['clear_after_sha256'] = receipt['after_sha256']
    receipt['after_sha256'] = hashlib.sha256(text.encode()).hexdigest()
    with open(patched, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(text)
    assemble, index = [command[0]], 1
    while index < len(command):
        arg = command[index]
        if arg in ('-MF', '-MT', '-MQ', '-o'):
            index += 2
            continue
        if arg not in ('-MD', '-MMD', '-MP', '-c') and arg != source:
            assemble.append(arg)
        index += 1
    assemble += ['-x', 'assembler', '-c', str(patched), '-o', str(output)]
    result = run(assemble)
    receipt.update(exit=result, source=source)
    output.with_name(prefix + '-routing.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('Noticescroll clear calls: ' + json.dumps(receipt), flush=True)
    return result


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
