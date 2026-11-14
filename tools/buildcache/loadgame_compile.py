"""Retain the frozen loader's cold input order and shrinking slots.

Only whole compiler-produced function chunks are reordered within their original
input section. Instructions, local labels and literal pools are not rewritten.
Short bodies receive unreachable zero tails; GAS rejects slot growth on Old.
"""
from pathlib import Path
import json
import re
import subprocess
import sys

MARKER = '--loadgame-compiler'


def constants(text, sections):
    """Restore named arrays and convert GCC anchor literals to the same named targets.

    Anchor expressions are accepted only as complete .word/.4byte operands.
    No load/store immediate or other instruction is rewritten.
    """
    redirects = {}
    for section, entries in sections.items():
        pattern = r'(?m)^[ \t]*\.section\s+'+re.escape(section)+r'(?=[,\s])[^\n]*\n'
        matches = list(re.finditer(pattern, text))
        assert len(matches) == 1, (section, len(matches))
        start, head = matches[0].start(), matches[0].group()
        next_section = re.search(r'(?m)^[ \t]*\.section\s+', text[matches[0].end():])
        end = matches[0].end()+next_section.start() if next_section else len(text)
        block = text[start:end]
        types = list(re.finditer(r'(?m)^[ \t]*\.type\s+([\w.]+),\s*%object\s*$', block))
        chunks = {m[1]: block[m.start():types[i+1].start() if i+1<len(types) else len(block)] for i,m in enumerate(types)}
        expected = {e['name']: e for e in entries}
        assert set(chunks) == set(expected), (section, set(chunks)^set(expected))
        cursor, positions, anchors, sizes = 0, {}, {}, {}
        for line in block.splitlines():
            if match := re.fullmatch(r'\s*\.align\s+(\d+)\s*', line):
                alignment = 1 << int(match[1])
                cursor = (cursor+alignment-1)&-alignment
            elif match := re.fullmatch(r'\s*\.set\s+(\.LANCHOR\d+),\s*\.\s*\+\s*(\d+)\s*', line):
                anchors[match[1]] = cursor+int(match[2])
            elif match := re.fullmatch(r'\s*\.size\s+([\w.]+),\s*(\d+)\s*', line):
                sizes[match[1]] = int(match[2])
            elif line.strip().endswith(':') and line.strip()[:-1] in expected:
                name = line.strip()[:-1]
                assert sizes[name] == expected[name]['size']
                positions[name] = cursor
                cursor += sizes[name]
        assert set(positions) == set(expected)
        for anchor, offset in anchors.items():
            redirects[anchor] = (offset, positions, sizes)
        label = '.Lloadgame_const_start_'+section.replace('.', '_')
        rebuilt = head+'\t.balign 2\n'+label+':\n'
        for entry in entries:
            chunk = re.sub(r'(?m)^[ \t]*\.(?:align|set)\s+[^\n]*\n?', '', chunks[entry['name']])
            rebuilt += '\t.org '+label+' + '+str(entry['offset'])+', 0\n'+chunk
        text = text[:start]+rebuilt+text[end:]
    for anchor, (base, positions, sizes) in redirects.items():
        pattern = r'(?m)^([ \t]*\.(?:word|4byte)\s+)'+re.escape(anchor)+r'([+-]\d+)?[ \t]*$'
        def rewrite(match):
            offset = base+int(match[2] or '0')
            names = [name for name,pos in positions.items() if pos <= offset < pos+sizes[name]]
            assert len(names) == 1, (anchor, offset, names)
            name = names[0]
            inner = offset-positions[name]
            return match[1]+name+('+'+str(inner) if inner else '')
        text = re.sub(pattern, rewrite, text)
        assert not re.search(re.escape(anchor)+r'(?!\d)', text), ('Non-literal constant anchor reference', anchor)
    return text


def retain(text, sections, old, game=None):
    # The new CtrDisplayCaps argument is a compiler-owned two-byte constant.
    # Keep it in the appended change constants, away from frozen .rodata.
    if game == 'undertale':
        text, count = re.subn(r'(?m)^[ \t]*\.section[ \t]+\.rodata[ \t]*$',
                             '\t.section .rodata.late.dlgtop,"a",%progbits', text)
        assert count == 1, ('missing/duplicate UT caps constant section', count)
    text = constants(text, sections.get('__constants', {}))
    aliases = dict(re.findall(r'(?m)^[ \t]*\.set\s+([\w.]+),\s*([\w.]+)[ \t]*$', text))
    sizes = list(re.finditer(r'(?m)^\s*\.size\s+([\w.]+),\s*\.\-\1\s*$', text))
    assert sizes
    first = re.search(r'(?m)^\s*\.section\s+\.text[^\n]*\n', text)
    assert first and first.start() < sizes[0].start()
    # A moved chunk must not reach a .loc before the numbered .file GCC emitted with the first function using it.
    numbered = re.compile(r'(?m)^[ \t]*\.file[ \t]+\d+[ \t]+"[^\n]*\n')
    files = numbered.findall(text[first.start():])
    text = text[:first.start()]+''.join(files)+numbered.sub('', text[first.start():])
    sizes = list(re.finditer(r'(?m)^\s*\.size\s+([\w.]+),\s*\.\-\1\s*$', text))
    first = re.search(r'(?m)^\s*\.section\s+\.text[^\n]*\n', text)
    assert first and first.start() < sizes[0].start()
    prefix, suffix = text[:first.start()], text[sizes[-1].end():]
    chunks, position, section = [], first.start(), None
    for match in sizes:
        body = text[position:match.end()]
        names = re.findall(r'(?m)^\s*\.section\s+([^,\s]+)[^\n]*', body)
        if names:
            section = names[-1]
        assert section and re.search(r'(?m)^\s*\.type\s+'+re.escape(match[1])+r',\s*%function\s*$', body)
        chunks.append(dict(name=match[1], section=section, text=body))
        position = match.end()
    # UNDERTALE cannot populate ls.text/ls.frames: their only setters are in
    # DELTARUNE's lsPickText/lsReadArt. Retain each old entry as a real return,
    # and place whole compiler-produced cold helper chunks in its unused slot.
    # lsBegin also has 56 B of already retained, unreachable tail space.
    fills = {'lsDrawDog': ['Loadgame_packFallback'],
             'lsDrawText': ['Loadgame_layout', 'Loadgame_framebuffer'],
             'lsBegin': ['Loadgame_consoleBack'],
             'psSaveShot': ['Loadgame_prepare', 'Loadgame_format', 'Loadgame_present', 'Loadgame_saveShot', 'Loadgame_swap']} if game == 'undertale' else {}
    packed = {part['name']: part for part in chunks
              if any(part['name'] in names for names in fills.values())}
    # zq52 froze this helper at 100 B on both UT models. Its new predicate
    # lives at the append point; preserve this slot and every following helper.
    if game == 'undertale':
        name = 'Loadgame_present'
        pattern = r'(?m)^\s*\.size\s+'+name+r',\s*\.\-'+name+r'\s*$'
        text, count = re.subn(pattern, lambda _: '\n\t.global __dlgtopfix_present_end\n\t.set __dlgtopfix_present_end, .\n\t.type __dlgtopfix_present_end,%notype\n\t.size __dlgtopfix_present_end,0\n\t.org '+name+' + 100, 0\n\t.size '+name+', .-'+name, packed[name]['text'])
        assert count == 1
        packed[name]['text'] = text
    assert set(packed) == {name for names in fills.values() for name in names}
    chunks = [part for part in chunks if part['name'] not in packed]
    for section, entries in sections.items():
        if section.startswith('__'):
            continue
        # ELF STT_FUNC aliases share their target's bytes; they are not a
        # second compiler body to order. Keep the original .set unmodified.
        by_name = {entry['name']: entry for entry in entries}
        for entry in entries:
            if entry['name'] in aliases:
                target = by_name[aliases[entry['name']]]
                assert (entry['offset'],entry['size']) == (target['offset'],target['size'])
        entries = [entry for entry in entries if entry['name'] not in aliases]
        indices = [i for i, part in enumerate(chunks) if part['section'] == section]
        names = [item['name'] for item in entries]
        present = {chunks[i]['name']: chunks[i] for i in indices}
        assert set(present) == set(names), (section, sorted(set(names)-set(present)), sorted(set(present)-set(names)))
        ordered = []
        for entry in entries:
            part = present[entry['name']]
            # Every target retains every 75zq slot, including late bodies.
            # These zero-sized labels describe unreachable padding only.
            tails = tuple(item['name'] for item in entries)
            pad = True
            body = part['text']
            if pad:
                pattern = r'(?m)^\s*\.size\s+'+re.escape(entry['name'])+r',\s*\.\-'+re.escape(entry['name'])+r'\s*$'
                end = '__loadgame_body_end_'+entry['name']
                marker = ('\n\t.global '+end+'\n\t.set '+end+', . - 1\n\t.type '+end+',%notype\n\t.size '+end+',0') if entry['name'] in tails else ''
                helpers = ''
                for name in fills.get(entry['name'], []):
                    helper = packed[name]['text']
                    helper = re.sub(r'(?m)^\s*\.section\s+[^\n]*',
                                    '\n\t.section '+section+',"ax",%progbits', helper)
                    helpers += '\n\t.section '+section+',"ax",%progbits\n'+helper
                if helpers:
                    end_slot = '__loadgame_slot_end_'+entry['name']
                    helpers += '\n\t.global '+end_slot+'\n\t.set '+end_slot+', . - 1\n\t.type '+end_slot+',%notype\n\t.size '+end_slot+',0'
                body, count = re.subn(pattern, lambda _: marker+helpers+'\n\t.org '+entry['name']+' + '+str(entry['size'])+', 0\n\t.size '+entry['name']+', .-'+entry['name'], body)
                assert count == 1
            # Each moved chunk must select its own section even when GCC omitted
            # a redundant .section between two neighboring functions.
            body = '\n\t.section '+section+',"ax",%progbits\n'+body
            ordered.append(dict(part, text=body))
        for index, part in zip(indices, ordered):
            chunks[index] = part
    return prefix+''.join(part['text'] for part in chunks)+suffix+'\n'


def run(command):
    return subprocess.run(command, stdin=subprocess.DEVNULL, timeout=1800).returncode


def write_assembly(path, text):
    with path.open('w', encoding='utf-8', newline='\n') as stream:
        stream.write(text)


def main_slots(text, game, model):
    slots = {'CtrBootlog_hide': 248, 'CtrScreens_moveConsoles': 248 if game == 'undertale' and model == 'o3ds' else 344}
    # R270 appends guarded bodies and keeps both reviewed player entry slots.
    if game == 'undertale' and 'Loadgame_guardedCarry:' in text:
        slots.update(Loadgame_carryCopy=112, Loadgame_restoreMenu=36 if model == 'o3ds' else 44)
    for name, size in slots.items():
        pattern = r'(?m)^\s*\.size\s+'+name+r',\s*\.\-'+name+r'\s*$'
        end = '__loadgame_body_end_'+name
        text, count = re.subn(pattern, lambda _: '\n\t.global '+end+'\n\t.set '+end+', . - 1\n\t.type '+end+',%notype\n\t.size '+end+',0\n\t.org '+name+' + '+str(size)+', 0\n\t.size '+name+', .-'+name, text)
        assert count == 1
    # A new external cold callee can make GCC emit moveConsoles last in this
    # input section. Restore whole compiler chunks, never instructions, to
    # the existing five-function order; all other chunks keep their position.
    order = ('CtrScreens_moveConsoles', 'CtrScreens_flushConsoles',
             'CtrScreens_benchLaunched', 'CtrScreens_screenshotNotice',
             '__wrap_CtrLog_emergencyKeepLink')
    if game == 'undertale' and 'CtrScreens_flushConsoles:' in text:
        sizes = list(re.finditer(r'(?m)^\s*\.size\s+([\w.]+),\s*\.\-\1\s*$', text))
        first = re.search(r'(?m)^\s*\.section\s+[^\n]*\n', text)
        assert sizes and first and first.start() < sizes[0].start()
        prefix, suffix = text[:first.start()], text[sizes[-1].end():]
        chunks, position, section = [], first.start(), None
        for match in sizes:
            body = text[position:match.end()]
            names = re.findall(r'(?m)^\s*\.section\s+([^,\s]+)[^\n]*', body)
            if names: section = names[-1]
            chunks.append(dict(name=match[1],section=section,text=body))
            position=match.end()
        indices=[i for i,c in enumerate(chunks) if c['name'] in order]
        selected={chunks[i]['name']:chunks[i] for i in indices}
        assert set(selected)==set(order)
        assert all(c['section']=='.text.late.screens' for c in selected.values())
        for index,name in zip(indices,order):
            chunks[index]=dict(selected[name],text='\n\t.section .text.late.screens,"ax",%progbits\n'+selected[name]['text'])
        text=prefix+''.join(c['text'] for c in chunks)+suffix
    return text+'\n'


def main(args):
    split = args.index(MARKER)
    delegate, command = [a for a in args[:split] if a], args[split+1:]
    sources = [a for a in command if a.replace('\\', '/').endswith(('/z_loadscreen.c', '/main.c'))]
    if not sources or '-DNDEBUG' not in command or '-DENABLE_WAD16' not in command:
        return run(delegate+command)
    assert len(sources) == 1 and command.count('-o') == 1 and '-c' in command
    game = 'undertale' if '-DENABLE_WAD16' in command else 'deltarune'
    model = 'o3ds' if '-DCTRPORT_TARGET_O3DS' in command else 'n3ds'
    old = game == 'undertale' and model == 'o3ds'
    sections = json.loads(Path(__file__).with_name('loadgame-order.json').read_text(encoding='utf-8'))[game+'-'+model]
    source = sources[0]
    output = Path(command[command.index('-o')+1])
    original, padded = output.with_suffix('.loadgame0.s'), output.with_suffix('.loadgame1.s')
    is_main = source.replace('\\', '/').endswith('/main.c')
    if is_main and game == 'deltarune':
        # builderx (and its Old vine delegate) requires normal -c input. Keep
        # that entire pass and pad its already routed assembly afterwards.
        receipt = output.with_suffix('.builderx-routing.json')
        receipt.unlink(missing_ok=True)
        status = run(delegate+command)
        if status:
            return status
        metadata = json.loads(receipt.read_text(encoding='utf-8'))
        routed = output.with_suffix('.builderx-routed.s')
        assert metadata['exit'] == 0 and Path(metadata['routed']).resolve() == routed.resolve()
        assert Path(metadata['source']).resolve() == Path(source).resolve()
        assert len(metadata['calls']) == 3 and all(row['calls'] == 1 for row in metadata['calls'].values())
        write_assembly(original, routed.read_text(encoding='utf-8'))
    else:
        emit = list(command)
        emit[emit.index('-c')] = '-S'
        emit[emit.index('-o')+1] = str(original)
        status = run(delegate+emit)
    if status:
        return status
    text = original.read_text(encoding='utf-8')
    text = main_slots(text, game, model) if is_main else retain(text, sections, old, game)
    if not is_main and game == 'undertale' and '-DCTRPORT_NO_BOTTOMFRAME' not in command:
        text = route_startup_cover(text)
    write_assembly(padded, text)
    assemble, index = [command[0]], 1
    while index < len(command):
        arg = command[index]
        if arg in ('-MF', '-MT', '-MQ', '-o'):
            index += 2
            continue
        if arg not in ('-MD', '-MMD', '-MP', '-c') and arg != source:
            assemble.append(arg)
        index += 1
    status=run(assemble+['-Wa,--fatal-warnings', '-x', 'assembler', '-c', str(padded), '-o', str(output)])
    if status:
        print('ERROR: frozen 75zq slot assembly failed; tools/buildcache/loadgame-order.json is the size/order contract. Move grown work to appended late helpers; never increase an existing slot.',file=sys.stderr)
    return status


def route_startup_cover(text):
    """Only two decoded call operands in the retained loading-swap body change."""
    body = list(re.finditer(r'^Loadgame_swap:\n.*?^\s*\.size\s+Loadgame_swap,[^\n]*', text, re.M | re.S))
    assert len(body) == 1, 'startup cover caller inventory'
    original = body[0][0]
    routed = original
    for old, new in (('__real_gfxScreenSwapBuffers', 'CtrLoadscreen_coverTopSwap'),
                     ('CtrScreens_pacerShow', 'CtrLoadscreen_coverBottomSwap')):
        assert re.search(r'(?m)^' + new + ':$', text), 'missing startup cover adapter'
        pattern = r'(?m)^([ \t]+b(?:l)?(?:eq|ne|cs|cc|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?[ \t]+)' + old + r'((?:\(PLT\))?[ \t]*)$'
        routed, count = re.subn(pattern, lambda m: m[1] + new + m[2], routed)
        assert count == 1, ('startup cover call inventory', old, count)
    restored = routed.replace('CtrLoadscreen_coverTopSwap', '__real_gfxScreenSwapBuffers').replace(
        'CtrLoadscreen_coverBottomSwap', 'CtrScreens_pacerShow')
    assert restored == original, 'non-operand startup cover change'
    return text[:body[0].start()] + routed + text[body[0].end():]


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except AssertionError as failure:
        print('ERROR: frozen 75zq loader layout mismatch; inspect tools/buildcache/loadgame-order.json and append changed code instead of growing an existing slot:',failure,file=sys.stderr)
        sys.exit(2)
