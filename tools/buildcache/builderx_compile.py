"""Keep original menu/chapter slots; route only named cold call operands to appended helpers.

No source, instruction, data or pool is replaced. The original local functions remain callable through
qualified aliases. Full layout and decoded-call audits verify the emitted result independently.
"""
from pathlib import Path
import json
import re
import subprocess
import sys

MARKER = '--builderx-compiler'
CONDITION = r'(?:eq|ne|cs|cc|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?'


def route(text, source, chapter_prefix='BxN'):
    aliases, counts = [], {}
    if source == 'ctr_optmenu.c':
        rows = re.findall(r'(?m)^(optRowMain(?:\.constprop\.\d+)?):$', text)
        assert len(rows) == 1, ('Unqualified options-row ABI', rows)
        assert not re.search(r'(?m)^optRowMain\.(?:isra|part)\.', text), 'Row ABI changed'
        entries = [(rows[0], 'Bx_menuRow', 'Bx_menuRowOriginal'),
                   ('optActivate', 'Bx_menuActivate', 'Bx_menuActivateOriginal'),
                   # battletouch (11-08): the main page's keys and draw go through its Battle layout and Battle
                   # stylus rows (z_battletouch.c), which call the originals; optRow and the menu's state by name only.
                   ('optKeys', 'CtrOptbattle_keys', 'CtrOptbattle_keysOriginal'),
                   ('optCompose', 'CtrOptbattle_compose', 'CtrOptbattle_composeOriginal'),
                   ('optRow', None, 'CtrOptbattle_row'),
                   ('opt', None, 'CtrOptbattle_state')]
    elif source == 'main.c':
        assert chapter_prefix in ('BxN', 'BxO')
        entries = [('CtrChapters_init', chapter_prefix + '_init', None),
                   ('Runner_create', chapter_prefix + '_create', None),
                   ('CtrChapters_takeGameChange', chapter_prefix + '_change', None)]
    else:
        assert source == 'z_builderx_chapters.c'
        entries = [('CtrDataVersion_check', 'Bx_preflightCheck', None)]
    for name, target, alias in entries:
        if alias:
            assert len(re.findall(r'(?m)^' + re.escape(name) + r':$', text)) == 1, name
            aliases.append('\t.global ' + alias + '\n\t.set ' + alias + ', ' + name)
        if target is None:
            continue
        pattern = r'(?m)^([ \t]+b(?:l)?' + CONDITION + r'[ \t]+)' + re.escape(name) + r'(\(PLT\))?([ \t]*(?:@[^\n]*)?)$'
        text, count = re.subn(pattern, lambda m: m[1] + target + (m[2] or '') + m[3], text)
        assert count == 1, ('Unexpected cold caller count', name, count)
        counts[name] = dict(target=target, calls=count)
    return text + '\n' + '\n'.join(aliases) + '\n', counts


def run(command):
    return subprocess.run(command, stdin=subprocess.DEVNULL, timeout=1800).returncode


def emit_assembly(delegate, command, output, old_guard):
    if old_guard and any(arg.replace('\\', '/').endswith('/vine_old_observer_compile.py') for arg in delegate):
        # The Old renderer launcher needs its normal -c input so it can generate and route assembly itself.
        # Compose our three redirects after that pass, preserving all of its settings/renderer operands.
        linked = output.with_suffix('.vine-linked.s')
        receipt = output.with_suffix('.vine-routing.json')
        linked.unlink(missing_ok=True)
        receipt.unlink(missing_ok=True)
        result = run(delegate + command)
        if result:
            return result, None
        metadata = json.loads(receipt.read_text(encoding='utf-8'))
        assert metadata['exit'] == 0 and not metadata['helper_command'], 'Expected Old main assembly pass'
        assert Path(metadata['files']['linked']).resolve() == linked.resolve(), 'Wrong delegate assembly'
        return 0, linked.read_text(encoding='utf-8')
    original_file = output.with_suffix('.builderx-original.s')
    original = list(command)
    original[original.index('-c')] = '-S'
    original[original.index('-o') + 1] = str(original_file)
    result = run(delegate + original)
    return result, None if result else original_file.read_text(encoding='utf-8')


def main(args):
    split = args.index(MARKER)
    delegate, command = [arg for arg in args[:split] if arg], args[split + 1:]
    candidates = [arg for arg in command if arg.replace('\\', '/').endswith(('/ctr_optmenu.c', '/z_builderx_chapters.c', '/main.c'))]
    new_guard = bool(candidates and candidates[0].replace('\\', '/').endswith('/main.c') and
                     '-DENABLE_WAD17' in command and '-DCTRPORT_TARGET_N3DS' in command)
    old_guard = bool(candidates and candidates[0].replace('\\', '/').endswith('/main.c') and
                     '-DENABLE_WAD17' in command and '-DCTRPORT_TARGET_O3DS' in command)
    if (not candidates or ('-DENABLE_WAD17' in command and not (new_guard or old_guard)) or
            (candidates[0].replace('\\', '/').endswith('/main.c') and not (new_guard or old_guard)) or
            ('-DENABLE_WAD16' in command and candidates[0].replace('\\', '/').endswith('/z_builderx_chapters.c'))):
        return run(delegate + command)
    assert len(candidates) == 1 and '-c' in command and command.count('-o') == 1
    output = Path(command[command.index('-o') + 1])
    original_file = output.with_suffix('.builderx-original.s')
    routed_file = output.with_suffix('.builderx-routed.s')
    result, original_text = emit_assembly(delegate, command, output, old_guard)
    if result:
        return result
    if old_guard:
        with original_file.open('w', encoding='utf-8', newline='\n') as stream:
            stream.write(original_text)
    text, counts = route(original_text, Path(candidates[0]).name,
                         'BxO' if old_guard else 'BxN')
    if ('-DENABLE_WAD16' in command and '-DCTRPORT_NO_BOTTOMFRAME' not in command
            and Path(candidates[0]).name == 'ctr_optmenu.c'):
        text, counts['bottomframe'] = route_bottomframe(text)
    with routed_file.open('w', encoding='utf-8', newline='\n') as stream:
        stream.write(text)
    assemble, i = [command[0]], 1
    while i < len(command):
        arg = command[i]
        if arg in ('-MF', '-MT', '-MQ', '-o'):
            i += 2
            continue
        if arg not in ('-MD', '-MMD', '-MP', '-c') and arg not in candidates:
            assemble.append(arg)
        i += 1
    assemble += ['-x', 'assembler', '-c', str(routed_file), '-o', str(output)]
    result = run(assemble)
    output.with_suffix('.builderx-routing.json').write_text(json.dumps(dict(source=candidates[0],
        calls=counts, exit=result, original=str(original_file), routed=str(routed_file)), indent=2) + '\n')
    print('Builder cold call operands: ' + json.dumps(counts), flush=True)
    return result


def route_bottomframe(text):
    """Route two menu entry operands, retaining GCC's old graph and every other byte.

    The appended adapters call the old bodies through opaque assembler names.
    Match the exact caller and original callee; never globally replace those
    callees, since doing so would turn the adapters' continuations into recursion.
    """
    edges = (('optKeysHeard', 'CtrOptbattle_keys', 'CtrBottomframe_menuKeys'),
             ('optComposeShown', 'CtrOptbattle_compose', 'CtrBottomframe_menuCompose'))
    assert all(re.search(r'(?m)^' + new + r':$', text) for _, _, new in edges), 'missing bottomframe menu adapters'
    before, edits = text, []
    for caller, old, new in edges:
        bodies = list(re.finditer(r'^' + caller + r':\n.*?^\s*\.size\s+' + caller + r',[^\n]*', text, re.M | re.S))
        assert len(bodies) == 1, ('bottomframe caller inventory', caller)
        body = bodies[0]
        calls = list(re.finditer(r'(?m)^([ \t]+b(?:l)?' + CONDITION + r'[ \t]+)(?P<target>' + old +
                                r')(?:\(PLT\))?[ \t]*$', body[0]))
        assert len(calls) == 1, ('bottomframe call inventory', caller, old, len(calls))
        call = calls[0]
        edits.append((body.start() + call.start('target'), body.start() + call.end('target'), old, new))
    for start, end, old, new in sorted(edits, reverse=True):
        text = text[:start] + new + text[end:]
    restored = text
    for start, end, old, new in sorted(edits):
        assert restored[start:start + len(new)] == new
        restored = restored[:start] + old + restored[start + len(new):]
    assert restored == before, 'non-operand bottomframe assembly change'
    return text, dict(calls=2, edges=[dict(caller=c, old=o, new=n) for c, o, n in edges])


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
