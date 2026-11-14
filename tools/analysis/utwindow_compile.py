"""Route UNDERTALE's final picture calls after compilation, preserving the complete warm body.

The helper has the original blitter's ABI and memory effects; it never retains the rectangle pointer.
Scope and exact call count fail closed. The independent ELF verifier checks every other allocated byte.
"""
from pathlib import Path
import importlib.util
import json
import re
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
import vine_extract


def route(text, development=False, size_optimized=False):
    name = 'CtrRenderer_endFrame.part.0'
    if development and not re.search(r'^' + re.escape(name) + ':$', text, re.M):
        # Freewins and unoptimized builds retain the public frame body.
        name = 'CtrRenderer_endFrame'
    if development and re.search(r'^present:$', text, re.M):
        # At -O0 present is not inlined. Its five source calls are the
        # application picture; presentBottom's magnifier remains separate.
        name = 'present'
    scopes = list(re.finditer(r'(?ms)^' + re.escape(name) + r':\r?\n.*?^\s*\.size\s+' +
                             re.escape(name) + r',\s*\.-' + re.escape(name) + r'\s*$', text))
    assert len(scopes) == 1, ('Unknown renderer frame body', len(scopes))
    scope = scopes[0]
    pattern = r'(?m)^([ \t]+bl[ \t]+)blitApp(\(PLT\))?([ \t]*(?:@[^\n]*)?)$'
    body, count = re.subn(pattern, lambda m: m[1] + 'CtrUtWindow_blitApp' + (m[2] or '') + m[3], scope[0])
    expected = 5 if name == 'present' else 4 if size_optimized else 6
    assert count == expected, ('Unknown final picture caller count', count, expected)
    return text[:scope.start()] + body + text[scope.end():], count


def route_solid(text):
    scopes = list(re.finditer(r'(?ms)^emitQuad:\r?\n.*?^\s*\.size\s+emitQuad,\s*\.-emitQuad\s*$', text))
    assert len(scopes) == 1, ('Unknown emitQuad body', len(scopes))
    scope = scopes[0]
    body, count = re.subn(r'(?m)^([ \t]+bl[ \t]+)solidDrawIsNoop(\(PLT\))?([ \t]*(?:@[^\n]*)?)$',
                         lambda m: m[1]+'CtrDlgwhite_solidDrawIsNoop'+(m[2] or '')+m[3], scope[0])
    assert count == 1, ('Unknown emitQuad no-op calls', count)
    return text[:scope.start()]+body+text[scope.end():]


def route_dialogue(text, diagnostic=False):
    # dlgflash: the sole screens wrapper call; leave the helper's original call alone.
    symbol = '__wrap_CtrRenderer_yieldBottomScreen'
    scopes = list(re.finditer(r'(?ms)^' + symbol + r':\r?\n.*?^\s*\.size\s+' + symbol + r',\s*\.-' + symbol + r'\s*$', text))
    assert len(scopes) == 1, ('dlgflash yield scope', len(scopes))
    scope = scopes[0]
    routed, calls = re.subn(r'(?m)^([ \t]+bl[ \t]+)CtrScreens_yieldHook(\(PLT\))?([ \t]*(?:@[^\n]*)?)$',
                           lambda m: m[1] + 'CtrDlgflash_yield' + (m[2] or '') + m[3], scope[0])
    assert calls == 1, ('dlgflash yield calls', calls)
    text = text[:scope.start()] + routed + text[scope.end():]
    if not diagnostic:
        return text
    # Optional test per-box diagnostic. Each wrapper delegates unchanged to the
    # original; scope/count checks forbid intercepting glyphs or helper calls.
    for symbol, old, new in (
        ('emitQuad', 'recordingSurface', 'CtrDlgflash_recordingSurface'),
        ('ctrDestroy', 'CtrDialogue_free', 'CtrDlgflash_free'),
    ):
        scopes = list(re.finditer(r'(?ms)^' + re.escape(symbol) + r':\r?\n.*?^\s*\.size\s+' +
                                 re.escape(symbol) + r',\s*\.-' + re.escape(symbol) + r'\s*$', text))
        assert len(scopes) == 1, ('dlgflash log scope', symbol, len(scopes))
        scope = scopes[0]
        call_pattern=r'(?m)^([ \t]+bl[ \t]+)' + re.escape(old) + r'(\(PLT\))?([ \t]*(?:@[^\n]*)?)$'
        calls=list(re.finditer(call_pattern,scope[0]))
        if symbol == 'emitQuad':
            # GCC splits textured and white quads. Only the white branch,
            # after its no-op test, observes a box rectangle actually issued.
            solids=list(re.finditer(r'(?m)^\s+bl\s+CtrDlgwhite_solidDrawIsNoop(?:\(PLT\))?\s*$',scope[0]))
            assert len(calls)==2 and len(solids)==1,('dlgflash white branch',len(calls),len(solids))
            assert calls[0].start()<solids[0].start()<calls[1].start(), 'dlgflash white query order'
            call=calls[1]
        else:
            assert len(calls)==1,('dlgflash log call',symbol,len(calls))
            call=calls[0]
        routed=scope[0][:call.start()]+call[1]+new+(call[2] or '')+call[3]+scope[0][call.end():]
        text = text[:scope.start()] + routed + text[scope.end():]
    return text


def route_top_dialogue(text, development=False):
    name = 'ctrDestroy'
    scopes = list(re.finditer(r'(?ms)^' + name + r':\r?\n.*?^\s*\.size\s+' + name + r',\s*\.-' + name + r'\s*$', text))
    assert len(scopes) == 1, ('S4 teardown scope', len(scopes))
    scope = scopes[0]
    body, calls = re.subn(r'(?m)^([ \t]+bl[ \t]+)CtrRenderer_releaseVideo(\(PLT\))?([ \t]*(?:@[^\n]*)?)$',
                         lambda m: m[1] + 'CtrDlgtop_destroy' + (m[2] or '') + m[3], scope[0])
    assert calls == 1, ('S4 teardown calls', calls)
    text = text[:scope.start()] + body + text[scope.end():]
    # A pre-Draw seam, after Step and before any game Draw, retains the
    # original viewport arguments/body and changes only this one call.
    name = 'ctrBeginFrame'
    scopes = list(re.finditer(r'(?ms)^' + name + r':\r?\n.*?^\s*\.size\s+' + name + r',\s*\.-' + name + r'\s*$', text))
    assert len(scopes) == 1, ('S4 pre-Draw scope', len(scopes))
    scope = scopes[0]
    body, calls = re.subn(r'(?m)^([ \t]+bl[ \t]+)setViewport(\(PLT\))?([ \t]*(?:@[^\n]*)?)$',
                         lambda m: m[1] + 'CtrDlgtop_viewport' + (m[2] or '') + m[3], scope[0])
    assert calls == 1, ('S4 pre-Draw viewport calls', calls)
    text = text[:scope.start()] + body + text[scope.end():]
    # Only final presentation calls, never presentBottom's own body or replay.
    name = 'present' if re.search(r'^present:$', text, re.M) else 'CtrRenderer_endFrame.part.0'
    if not re.search(r'^' + re.escape(name) + ':$', text, re.M): name = 'CtrRenderer_endFrame'
    scopes = list(re.finditer(r'(?ms)^' + re.escape(name) + r':\r?\n.*?^\s*\.size\s+' +
                             re.escape(name) + r',\s*\.-' + re.escape(name) + r'\s*$', text))
    assert len(scopes) == 1, ('S4 final frame scope', name, len(scopes))
    scope = scopes[0]
    body, calls = re.subn(r'(?m)^([ \t]+b(?:l)?[ \t]+)presentBottom(\(PLT\))?([ \t]*(?:@[^\n]*)?)$',
                         lambda m: m[1] + 'CtrDlgtop_present' + (m[2] or '') + m[3], scope[0])
    assert calls in (1, 2), ('S4 presentation calls', calls)
    text = text[:scope.start()] + body + text[scope.end():]
    name = '__wrap_CtrRenderer_setDisplay'
    scopes = list(re.finditer(r'(?ms)^' + name + r':\r?\n.*?^\s*\.size\s+' + name + r',\s*\.-' + name + r'\s*$', text))
    assert len(scopes) == 1, ('S4 display scope', len(scopes))
    scope = scopes[0]
    body, calls = re.subn(r'(?m)^([ \t]+bl[ \t]+)CtrDimlight_setDisplay(\(PLT\))?([ \t]*(?:@[^\n]*)?)$',
                         lambda m: m[1] + 'CtrDlgtop_display' + (m[2] or '') + m[3], scope[0])
    # Release GCC duplicates this one source call across the below-game,
    # shared-backlight and ordinary-screen paths. Development/size builds
    # retain one common call. Every clone must enter the same adapter.
    expected = 1 if development else 3
    assert calls == expected, ('S4 display calls', calls, expected)
    return text[:scope.start()] + body + text[scope.end():]


def run(command):
    return subprocess.run(command, stdin=subprocess.DEVNULL, timeout=1800).returncode


def route_lcd(text):
    """Apply dimlight only after this adapter's existing picture-call edits."""
    path = Path(__file__).resolve().parents[1] / 'buildcache/dimlight_compile.py'
    spec = importlib.util.spec_from_file_location('dimlight_compile', path)
    adapter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(adapter)
    return adapter.route(text, 'ctr_renderer.c')


def main(args):
    split = args.index('--')
    delegate, command = [a for a in args[:split] if a], args[split + 1:]
    sources = [a for a in command if a.replace('\\', '/').endswith('/ctr_renderer.c')]
    if not sources or '-DENABLE_WAD16' not in command:
        return run(delegate + command)
    assert len(sources) == 1 and '-c' in command and command.count('-o') == 1
    output = Path(command[command.index('-o') + 1])
    # Side files next to the object, with names no longer than ctr_renderer.c.o: CMake keeps object paths within
    # Windows' path limit and devkitARM's assembler cannot open a longer one (colexact_compile's lesson, integrate21
    # stage73: a 92-character builder work folder).
    original_file = output.with_name('utwindow0.s')
    routed_file = output.with_name('utwindow1.s')
    original = list(command)
    original[original.index('-c')] = '-S'
    original[original.index('-o') + 1] = str(original_file)
    rc = run(delegate + original)
    if rc:
        return rc
    development = '-DCTRPORT_FREEWINS' in command or not any(a in ('-O2', '-O3') for a in command)
    text, count = route(original_file.read_text(encoding='utf-8'), development=development,
                        size_optimized=any(flag in command for flag in ('-Os', '-Oz')))
    text = route_top_dialogue(text, development=development)
    text = route_solid(text)
    text = route_dialogue(text, diagnostic="-DCTRPORT_DLGFLASH_LOG" in command)
    # Helper-only pass retains no ordinary body or data, preserving GCC's
    # pre-existing compiler-local tables and all original slot identities.
    helper_file = output.with_name('dlgwhite2.s')
    helper = list(original)
    helper[helper.index('-o') + 1] = str(helper_file)
    for option in ('-MF', '-MT', '-MQ'):
        while option in helper:
            at = helper.index(option)
            del helper[at:at+2]
    helper = [a for a in helper if a not in ('-MD', '-MMD', '-MP')]
    helper += ['-DCTRPORT_DLGWHITE_HELPER', '-DCTRPORT_DLGFLASH_HELPER', '-fno-section-anchors']
    rc = run(helper)
    if rc: return rc
    extra, sections = vine_extract.late_sections(helper_file.read_text(encoding='utf-8'), 'dlgwhite')
    flash, flash_sections = vine_extract.late_sections(helper_file.read_text(encoding='utf-8'), 'dlgflash')
    top, top_sections = vine_extract.late_sections(helper_file.read_text(encoding='utf-8'), 'dlgtop4')
    text += '\n'+extra+'\n'+flash+'\n'+top
    text, lcd_receipt = route_lcd(text)
    with open(routed_file, 'w', encoding='utf-8', newline='\n') as handle:  # write_text(newline=) needs Python 3.10
        handle.write(text)
    assemble, i = [command[0]], 1
    while i < len(command):
        arg = command[i]
        if arg in ('-MF', '-MT', '-MQ', '-o'):
            i += 2
            continue
        if arg not in ('-MD', '-MMD', '-MP', '-c') and arg not in sources:
            assemble.append(arg)
        i += 1
    rc = run(assemble + ['-x', 'assembler', '-c', str(routed_file), '-o', str(output)])
    output.with_name('utwindow.json').write_text(json.dumps(dict(
        caller='CtrRenderer_endFrame.part.0', old='blitApp', new='CtrUtWindow_blitApp',
        count=count, exit=rc, original=str(original_file), routed=str(routed_file),
        lcd=lcd_receipt), indent=2) + '\n', encoding='utf-8')
    print(f'Window motion: {count} final picture call operands routed; warm instructions retained', flush=True)
    print('dimlight LCD call operands: ' + json.dumps(lcd_receipt), flush=True)
    return rc
