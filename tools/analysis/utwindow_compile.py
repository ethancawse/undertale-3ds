"""Route UNDERTALE's final picture calls after compilation, preserving the complete warm body.

The helper has the original blitter's ABI and memory effects; it never retains the rectangle pointer.
Scope and exact call count fail closed. The independent ELF verifier checks every other allocated byte.
"""
from pathlib import Path
import hashlib
import importlib.util
import json
import re
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
import vine_extract
import undlg_routes
import promonotice_compile


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


def route_gameover_yield(text):
    # F1: dlgflash owns the live wrapper call on dv8u20. Route its terminal
    # layout operand, preserving S4's preceding lease return and refresh.
    symbol = 'CtrDlgflash_yield'
    scopes = list(re.finditer(r'(?ms)^' + symbol + r':\r?\n.*?^\s*\.size\s+' + symbol + r',\s*\.-' + symbol + r'\s*$', text))
    assert len(scopes) == 1, ('game-over dlgflash yield scope', len(scopes))
    scope = scopes[0]
    routed, count = re.subn(r'(?m)^([ \t]+bl[ \t]+)CtrLayout_yield8(\(PLT\))?([ \t]*(?:@[^\n]*)?)$',
                            lambda m: m[1]+'CtrGameover_yield'+(m[2] or '')+m[3], scope[0])
    assert count == 1, ('game-over dlgflash yield calls', count)
    return text[:scope.start()]+routed+text[scope.end():]


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


def route_gameover_setup(text):
    """One call in real beginFrame; readback before it stays untouched."""
    name = 'CtrRenderer_beginFrame'
    scopes = list(re.finditer(r'(?ms)^'+name+r':\r?\n.*?^\s*\.size\s+'+name+
                             r',\s*\.-'+name+r'\s*$', text))
    assert len(scopes)==1, ('Game-over begin scope',len(scopes))
    scope = scopes[0]
    changed,count = re.subn(r'(?m)^([ \t]+bl[ \t]+)configureScreens(\(PLT\))?([ \t]*(?:@[^\n]*)?)$',
                           lambda m:m[1]+'CtrGameover_configure'+(m[2] or '')+m[3],scope[0])
    assert count==1, ('Game-over begin setup calls',count)
    return text[:scope.start()]+changed+text[scope.end():]


# asgsmart F1u: ctr_gameoverscreen.inc's CtrGameover_fuseSprite takes utkDrawSprite's place in the alpha stubs' target
# table (slot 25; the stub, its alpha adaptation and abqCore are unchanged). It branches to these four, reads these
# words, and nothing else. Each name must be defined exactly once, in its own section: the renderer's in the ordinary
# pass, go in the game-over unit's retained storage. A mismatch fails the build rather than picking another recipe.
# Operands are checked whole (c26rev6 F1UI-1): a branch names one entry or one of the fold's labels exactly (no
# addend), each pool word is exactly one claimed name, each once (no addend, compound or number), and every other
# instruction names no symbol except a load from one of the fold's own pools at a word offset inside it.
# First, though (c26rev6 r2 delta, 18:1x): the fold's whole section must be one of the reviewed recipes below,
# instruction for instruction (fuse_canonical: comments, whitespace runs, local label spelling, and local labels
# nothing in the section names, only). Any other assembly fails the build with "F1u NOT fused"; a new compiler output
# needs its recipe reviewed and added here. F1u r5 (N11 link, 11-15): a -g build's fold carries GCC's debug view labels
# (.LVL, .LFB, .LBB, ...), named only from the debug sections. They are dropped like comments, so the release, dev and
# New builds read as the reviewed recipes. The recipes are the r3 texts minus their one unreferenced label (the pool's
# skip label); every instruction, operand, referenced label and pool word is unchanged.
FUSE_RECIPES = {'57e67a9228c1812db18fab8b68f5a72a69040a847ee6c175e9d16f166ce0af0d': 'plain',  # release Old, dev Old, New
                '1bba55ae4119b7e5e926e567c9fb5e7baa6eb76c1d7a2c913ed4a339d2a941b7': 'probe'}  # the counter probe twin
FUSE = 'CtrGameover_fuseSprite'
FUSE_SLOT = 25
FUSE_BRANCHES = {'utkDrawSprite': ('.text.late.utkill',), 'utkDrawSpriteBody': ('.text.late.utkill',),
                 'ctrDrawSprite': ('.text.ctrDrawSprite', '.text'), 'aslFineSprite': ('.text_asgorelong',)}
FUSE_DATA = {'asgRun': '.bss.late.asgdraw', 'utaGateFrame': '.bss.late.utasgore',
             'utaGateCalls': '.bss.late.utasgore', 'utaGateOn': '.bss.late.utasgore'}
FUSE_PROBE = {'utaOff': '.bss.late.utasgore', 'utaGateProbe': '.bss.late.utasgore', 'utaWhy': '.bss.late.utasgore'}
FUSE_OWN = {'go': '.bss_gameoverscreen'}
FUSE_OPERANDS = re.compile(r'^(?:r\d+|sp|lr|pc|ip|fp|sl|sb|lsl|lsr|asr|ror|rrx)$')


def fuse_expected(command):
    """The builds that compile the fold (ctr_gameoverscreen.inc's own condition): not timing, not a forced split."""
    forced = [a for a in command if a.startswith('-DASG_FORCE_SPLIT')]
    assert all(a in ('-DASG_FORCE_SPLIT=0', '-DASG_FORCE_SPLIT=1', '-DASG_FORCE_SPLIT=2', '-DASG_FORCE_SPLIT=3')
               for a in forced), ('Unknown forced split define', forced)
    return '-DCTRPORT_RENDER_TIMING' not in command and all(a == '-DASG_FORCE_SPLIT=0' for a in forced)


def label_sections(text):
    """Every label's definition sections, in order (GCC's section directives, as vine_extract follows them)."""
    found, section, stack = {}, '.text', []
    for line in text.splitlines():
        directive = re.match(r'\s*\.(pushsection|section)\s+([^,\s]+)', line)
        if directive:
            if directive[1] == 'pushsection':
                stack.append(section)
            section = directive[2]
            continue
        if re.match(r'\s*\.popsection\b', line):
            section = stack.pop()
            continue
        plain = re.match(r'\s*\.(text|data|bss)\s*(?:@.*)?$', line)
        if plain:
            section = '.' + plain[1]
            continue
        label = re.match(r'([A-Za-z_][\w.]*):', line)
        if label:
            found.setdefault(label[1], []).append(section)
    return found


# F1u r4 (audit F3-1, 11-14): the fold's section is ctr_place.h's UT_COLD_TEXT(asgfuse), per model: the AAC text
# slot on UNDERTALE Old, an appended late section on New. FUSE_SECTION is the reviewed recipes' spelling; fuse_canonical
# reads the build's own section and writes it with that spelling, so one recipe covers both models.
FUSE_SECTION = '.text_asgfuse'
FUSE_SECTIONS = {'-DCTRPORT_TARGET_O3DS': '.aacout.text.asgfuse', '-DCTRPORT_TARGET_N3DS': FUSE_SECTION}


def fuse_section(command):
    """The fold's section for this build's model (exactly one model define)."""
    models = [a for a in command if a in FUSE_SECTIONS]
    assert len(models) == 1, ('F1u: one model define', models)
    return FUSE_SECTIONS[models[0]]


def fuse_canonical(go_extra, section=FUSE_SECTION):
    """The fold's section (its section lines and body) as one text: comments dropped, each whitespace run one space,
    a local label definition that no line of the helper names dropped, the other local labels renamed .LF0, .LF1,
    ... in order of first use, the section named FUSE_SECTION. Instructions, operands, pool order and alignment stay.
    "No line of the helper" is every retained section, not just the fold's (c26rev6 F1UR5-1): a .word in
    .rodata_gameoverscreen that names a fold label pins that label's place as surely as a branch in the fold does.
    The helper is late_sections' output: no .debug_* sections, no .loc lines, so GCC's -g view labels stay unnamed.
    The reference search reads the raw lines, comments and all (c26rev6 F1UR6-1: '.ascii "@"' then '.word .LX' on
    one line hides the .word from an '@' split). A label named only in a comment is kept: fails closed."""
    out, selected, names = [], False, {}
    for line in go_extra.splitlines():
        directive = re.match(r'\s*\.(pushsection|section)\s+([^,\s]+)', line)
        if directive:
            selected = directive[2] == section
            if selected:
                line = line[:directive.start(2)] + FUSE_SECTION + line[directive.end(2):]
        elif re.match(r'\s*\.(?:popsection|text|data|bss)\b', line):
            selected = False
        if selected:
            s = ' '.join(line.split('@', 1)[0].split())
            if s:
                out.append(s)
    label, local = r'\.L[\w.]+:', r'(?<![\w.])\.L[\w.]+'
    every = (line.strip() for line in go_extra.splitlines())
    named = {m for s in every if not re.fullmatch(label, s) for m in re.findall(local, s)}
    out = [s for s in out if not (re.fullmatch(label, s) and s[:-1] not in named)]
    out = [re.sub(local, lambda m: names.setdefault(m[0], '.LF%d' % len(names)), s) for s in out]
    return '\n'.join(out) + '\n'


def route_fuse(text, go_extra, expected, probe, section=FUSE_SECTION):
    """Check the fold's every operand and binding, then point abqTargets' slot 25 at it. Returns (text, receipt)."""
    defined = FUSE in label_sections(go_extra)
    named = [s for s in FUSE_SECTIONS.values() if re.search(r'(?<![\w.])' + re.escape(s) + r'(?![\w.])', go_extra)]
    if not expected:
        assert not defined and not named, 'Fold present where its build excludes it'
        return text, dict(fused=False)
    assert defined, 'Fold missing from the game-over pass'
    assert named == [section], ('F1u: the fold is in this model\'s section only', section, named)
    recipe = hashlib.sha256(fuse_canonical(go_extra, section).encode()).hexdigest()
    assert FUSE_RECIPES.get(recipe) == ('probe' if probe else 'plain'), \
        ('F1u NOT fused: the fold assembly is not a reviewed recipe; review it and add it to FUSE_RECIPES', recipe)
    # The fold's own section, as extracted.
    body, selected = [], False
    for line in go_extra.splitlines():
        directive = re.match(r'\s*\.(pushsection|section)\s+([^,\s]+)', line)
        if directive:
            selected = directive[2] == section
            continue
        if re.match(r'\s*\.(?:popsection|text|data|bss)\b', line):
            selected = False
            continue
        if selected:
            body.append(line.split('@', 1)[0].rstrip())
    labels = [m[1] for line in body for m in [re.match(r'([\w.]+):$', line.strip())] if m]
    assert [n for n in labels if not n.startswith('.L')] == [FUSE], ('Fold section labels', labels)
    local = set(n for n in labels if n.startswith('.L'))
    assert len(local) == len(labels) - 1, ('Fold label twice', labels)
    # Each literal pool: a fold label and the .word lines right after it (their count bounds a load's offset).
    pools, current = {}, None
    for line in body:
        s = line.strip()
        if re.fullmatch(r'\.L[\w.]+:', s):
            current = s[:-1]
            pools[current] = 0
        elif current is not None and re.match(r'\.(?:word|4byte|long)\s', s):
            pools[current] += 1
        elif s and not re.match(r'\.(?:p2)?align\s', s):
            current = None
    data = dict(FUSE_DATA, **(FUSE_PROBE if probe else {}), **FUSE_OWN)
    branches, words = [], []
    for line in body:
        s = line.strip()
        if not s or re.fullmatch(r'[\w.]+:', s):
            continue
        parts = s.split(None, 1)
        op, rest = parts[0], parts[1].strip() if len(parts) > 1 else ''
        if op.startswith('.'):
            if op in ('.word', '.4byte', '.long'):
                # The whole expression is one claimed name: no addend, no compound, no number, no other symbol.
                assert rest in data, ('Fold data word', s)
                words.append(rest)
            else:
                exact = {'.global': FUSE, '.type': FUSE + ', %function', '.size': FUSE + ', .-' + FUSE,
                         '.syntax': 'unified', '.arm': '', '.code': '32'}
                assert (op in exact and rest == exact[op]) or (op in ('.align', '.p2align') and re.fullmatch(r'\d', rest)) \
                    or (op == '.fpu' and re.fullmatch(r'[\w-]+', rest)), ('Fold directive', s)
            continue
        if re.fullmatch(r'bl?(?:eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?', op):
            # The whole operand: one of the fold's own labels, or one of the four entries by its exact name.
            assert rest in local or rest in FUSE_BRANCHES, ('Fold branch operand', s)
            if rest in FUSE_BRANCHES:
                branches.append(rest)
            continue
        # Any other instruction: registers, shifts and immediates, or a load from one of the fold's pools at a word
        # offset inside it. Nothing else may name a symbol.
        def pool_ref(m):
            offset = int(m[2] or '+0')
            assert pools.get(m[1], 0) > 0 and offset % 4 == 0 and 0 <= offset < 4 * pools[m[1]], ('Fold pool load', s)
            return ' '
        plain = re.sub(r'(?<![\w.])(\.L[\w.]+)(\+\d+)?(?![\w.+\-*/])', pool_ref, rest)
        names = [n for n in re.findall(r'(?<![\w#])\.?[A-Za-z_][\w.]*', plain) if not FUSE_OPERANDS.match(n)]
        assert not names and '=' not in plain, ('Fold instruction names a symbol', s)
    assert set(branches) == set(FUSE_BRANCHES), ('Fold branch targets', sorted(set(branches)))
    assert sorted(words) == sorted(data), ('Fold data words, each exactly once', sorted(words))
    ordinary, own = label_sections(text), label_sections(go_extra)
    for name, sections in FUSE_BRANCHES.items():
        assert len(ordinary.get(name, [])) == 1 and ordinary[name][0] in sections and name not in own, ('Fold branch', name)
    for name, section in data.items():
        where = (own if name in FUSE_OWN else ordinary).get(name, [])
        other = (ordinary if name in FUSE_OWN else own).get(name, [])
        assert where == [section] and not other, ('Fold data binding', name, where, other)
    # abqTargets' slot 25, the only utkDrawSprite word anywhere.
    assert ordinary.get('abqTargets') == ['.rodata.late.alphabyte'], ('Alpha stub targets', ordinary.get('abqTargets'))
    table = re.search(r'(?m)^abqTargets:\n((?:\t\.word\t[\w.]+\n)+)', text)
    assert table, 'Alpha stub target table shape'
    targets = table[1].splitlines()
    assert len(targets) > FUSE_SLOT and targets[FUSE_SLOT] == '\t.word\tutkDrawSprite', ('Slot 25', targets[FUSE_SLOT:FUSE_SLOT + 1])
    assert len(re.findall(r'(?m)^\t\.word\tutkDrawSprite$', text)) == 1, 'utkDrawSprite word outside slot 25'
    assert not re.search(r'(?<![\w.])' + FUSE + r'(?![\w.])', text), 'Fold already named in the ordinary pass'
    targets[FUSE_SLOT] = '\t.word\t' + FUSE
    text = text[:table.start(1)] + '\n'.join(targets) + '\n' + text[table.end(1):]
    return text, dict(fused=True, slot=FUSE_SLOT, recipe=FUSE_RECIPES[recipe] + ' ' + recipe, branches=sorted(set(branches)),
                      words=sorted(set(words)))


def route_lcd(text):
    """Apply dimlight only after this adapter's existing picture-call edits."""
    path = Path(__file__).resolve().parents[1] / 'buildcache/dimlight_compile.py'
    spec = importlib.util.spec_from_file_location('dimlight_compile', path)
    adapter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(adapter)
    return adapter.route(text, 'ctr_renderer.c')


# change smoothold (11-16): ctrDrawSprite's asgBegin/asgEnd, drawTextImpl's smartLinearBegin/smartLinearEnd and
# addHoldDraw's asgHoldDraw become calls of the wrappers at ctr_renderer.c's end (compiled only in the helper pass,
# CTRPORT_SMOOTHOLD_HELPER), so copy sprites and text join held runs. Only the five operands change: the C calls stay,
# since calling the wrappers in C changed GCC's choices across the file (smartLinearEnd split, drawTowerStrips past
# its oomport slot).
SMO_ROUTES = (('ctrDrawSprite', 'asgBegin', 'CtrRenderer_smoSpriteBegin'),
              ('ctrDrawSprite', 'asgEnd', 'CtrRenderer_smoSpriteEnd'),
              ('drawTextImpl', 'smartLinearBegin', 'CtrRenderer_smoTextBegin'),
              ('drawTextImpl', 'smartLinearEnd', 'CtrRenderer_smoTextEnd'),
              ('addHoldDraw', 'asgHoldDraw', 'CtrRenderer_smoHoldDraw'))
SMO_STATE = 'smoothold_state'
# The table's words, in order: every function the wrappers call. The first five are the routed callees (each must
# reach an external function, so its callers already assume a full AAPCS call); the wrappers only call through the
# words, so any function may follow.
SMO_WORDS = ('asgBegin', 'asgEnd', 'smartLinearBegin', 'smartLinearEnd', 'asgHoldDraw', 'prophecyBatches',
             'recordingSurface', 'flushBatch')
SMO_ROUTED = tuple(old for _, old, _ in SMO_ROUTES)
# Data the wrappers read by name (besides the table): defined once by the ordinary pass where named, or external.
SMO_DATA = {'bsGpu': '.data_battlesplit', 'asgRun': '.bss.late.asgdraw'}
SMO_EXTERN = ('__C3D_Context',)
SMO_HOMES = {'asgBegin': '.text.late.asgdraw', 'asgEnd': '.text.late.asgdraw', 'asgHoldDraw': '.text.late.asgdraw',
             'smartLinearBegin': '.text.smartLinearBegin', 'smartLinearEnd': '.text.smartLinearEnd',
             'prophecyBatches': '.text.prophecyBatches', 'recordingSurface': '.text.recordingSurface',
             'flushBatch': '.text.flushBatch', **SMO_DATA}


def smo_body(text, name):
    scopes = list(re.finditer(r'(?ms)^' + re.escape(name) + r':\r?\n.*?^\s*\.size\s+' + re.escape(name) +
                              r',\s*\.-' + re.escape(name) + r'\s*$', text))
    return scopes[0] if len(scopes) == 1 else None


def smo_reaches_external(text, root):
    """Whether root, through direct calls inside this assembly, reaches a function this assembly doesn't define. Then
    GCC's IPA facts about root (-fipa-ra's clobbers, modref) are the full AAPCS ones, as for any unknown function, so a
    caller compiled against root may call a plain AAPCS function in its place."""
    seen, todo = set(), [root]
    while todo:
        name = todo.pop()
        if name in seen:
            continue
        seen.add(name)
        body = smo_body(text, name)
        if body is None:
            return True
        todo += re.findall(r'(?m)^\s*bl?(?:eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?\s+([A-Za-z_][\w.]*)'
                           r'(?:\(PLT\))?\s*(?:@[^\n]*)?$', body[0])
    return False


def route_smoothold(text, extra):
    """Check the helper's wrappers (names, sections, every symbol they name) and the ordinary pass's callees and data,
    then retarget the five call operands. Returns (text, receipt); the caller appends extra."""
    ordinary, own = label_sections(text), label_sections(extra)
    globals_ = sorted(n for n in own if not n.startswith('.L'))
    wrappers = sorted({new for _, _, new in SMO_ROUTES})
    assert globals_ == sorted(wrappers + [SMO_STATE]), ('smoothold helper labels', globals_)
    assert all(own[n] == ['.text_smoothold'] for n in wrappers) and own[SMO_STATE] == ['.data_smoothold'], \
        ('smoothold helper sections', {n: own[n] for n in globals_})
    for name in globals_:
        assert name not in ordinary, ('smoothold name already in the ordinary pass', name)
    for name, home in SMO_HOMES.items():
        assert ordinary.get(name) == [home], ('smoothold callee or datum', name, ordinary.get(name))
        assert not re.search(r'(?m)^' + re.escape(name) + r'\.(?:constprop|isra|part)\.', text), ('Clone', name)
    for name in SMO_EXTERN:
        assert name not in ordinary and name not in own, ('smoothold external defined here', name)
    for name in SMO_ROUTED:
        assert smo_reaches_external(text, name), ('smoothold callee with IPA facts of its own', name)
    # The table: the words in order, then the two flag bytes (zero, and padding), nothing else.
    table = re.search(r'(?ms)^' + SMO_STATE + r':\n(.*?)(?=^\s*\.(?:size|section|text|data|bss|global|type|align)\b|\Z)',
                      extra)
    assert table, 'smoothold table'
    rows = [' '.join(l.split('@', 1)[0].split()) for l in table[1].splitlines()]
    rows = [r for r in rows if r]
    n = len(SMO_WORDS)
    assert rows[:n] == ['.word ' + w for w in SMO_WORDS] and rows[n:] in (['.byte 0', '.byte 0', '.space 2'], ['.space 4']), \
        ('smoothold table words', rows)
    # Every other line: directives of its own, local labels, and instructions naming no symbol but its own pool words
    # (the table, SMO_DATA, SMO_EXTERN) and its own local labels. No direct branch out.
    local = set(re.findall(r'(?m)^(\.L[\w.]+):', extra))
    names_ok = set(own) | set(SMO_DATA) | set(SMO_EXTERN)
    for line in extra.splitlines():
        s = ' '.join(line.split('@', 1)[0].split())
        if not s or re.fullmatch(r'[\w.]+:', s):
            continue
        op, _, rest = s.partition(' ')
        if op in ('.word', '.4byte', '.long'):
            assert rest in names_ok or rest in SMO_WORDS or re.fullmatch(r'-?[0-9]+', rest), ('smoothold word', s)
            continue
        if op.startswith('.'):
            continue
        if re.fullmatch(r'bl?(?:eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?', op):
            assert rest in local, ('smoothold direct branch', s)
            continue
        plain = re.sub(r'(?<![\w.])\.L[\w.]+(?:\+\d+)?', lambda m: ' ' if m[0].split('+')[0] in local else m[0], rest)
        names = [n for n in re.findall(r'(?<![\w#])\.?[A-Za-z_][\w.]*', plain) if not FUSE_OPERANDS.match(n)]
        assert not names and '=' not in plain, ('smoothold instruction names a symbol', s)
    edits, witnesses = [], []
    for caller, old, new in SMO_ROUTES:
        body = smo_body(text, caller)
        assert body is not None, ('smoothold caller', caller)
        sites = list(re.finditer(r'(?m)^[ \t]+bl?(?:eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?[ \t]+(?P<t>'
                                 + re.escape(old) + r')(?:\(PLT\))?[ \t]*(?:@[^\n]*)?$', body[0]))
        assert len(sites) == 1, ('smoothold call sites', caller, old, len(sites))
        edits.append((body.start() + sites[0].start('t'), body.start() + sites[0].end('t'), old, new))
        witnesses.append(dict(caller=caller, old=old, new=new))
    result = text
    for start, end, old, new in sorted(edits, reverse=True):
        assert result[start:end] == old
        result = result[:start] + new + result[end:]
    restored, shift = result, 0
    for start, end, old, new in sorted(edits):
        at = start + shift
        assert restored[at:at + len(new)] == new
        restored = restored[:at] + old + restored[at + len(new):]
    assert restored == text, 'smoothold: non-operand assembly change'
    return result, dict(routes=witnesses, words=list(SMO_WORDS))


def main(args):
    split = args.index('--')
    delegate, command = [a for a in args[:split] if a], args[split + 1:]
    sources = [a for a in command if a.replace('\\', '/').endswith('/ctr_renderer.c')]
    if not sources or '-DENABLE_WAD16' not in command:
        return undlg_routes.compile_extra(delegate, command)
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
    text = route_gameover_setup(text)
    text = route_dialogue(text, diagnostic="-DCTRPORT_DLGFLASH_LOG" in command)
    text, notice_receipt = promonotice_compile.route(text)
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
    helper += ['-DCTRPORT_DLGWHITE_HELPER', '-DCTRPORT_DLGFLASH_HELPER',
               '-DCTRPORT_PROMONOTICE_HELPER', '-DCTRPORT_SMOOTHOLD_HELPER', '-fno-section-anchors']
    rc = run(helper)
    if rc: return rc
    smo, _ = vine_extract.late_sections(helper_file.read_text(encoding='utf-8'), 'smoothold')
    text, smo_receipt = route_smoothold(text, smo)
    text += '\n' + smo
    extra, sections = vine_extract.late_sections(helper_file.read_text(encoding='utf-8'), 'dlgwhite')
    flash, flash_sections = vine_extract.late_sections(helper_file.read_text(encoding='utf-8'), 'dlgflash')
    top, top_sections = vine_extract.late_sections(helper_file.read_text(encoding='utf-8'), 'dlgtop4')
    notice, notice_sections = vine_extract.late_sections(helper_file.read_text(encoding='utf-8'), 'promonotice')
    flash = route_gameover_yield(flash)
    # Keep frozen S4 arbitration first; only its terminal layout operand
    # enters the game-over guard. Inactive guard delegates to layout as before.
    top, go_chain_count = re.subn(r'(?m)^([ \t]+b(?:l)?[ \t]+)CtrLayout_chain18(\(PLT\))?([ \t]*(?:@[^\n]*)?)$',
                                  lambda m: m[1]+'CtrGameover_beginFrame'+(m[2] or '')+m[3], top)
    assert go_chain_count == 1, ('S4 to game-over begin seam', go_chain_count)
    text += '\n'+extra+'\n'+flash+'\n'+top+'\n'+notice
    # The game-over unit is independent: ordinary renderer compilation sees
    # none of its C, preserving all existing instructions and static locals.
    go_file = output.with_name('gos2.s')
    go_helper = [a for a in helper if a not in ('-DCTRPORT_DLGWHITE_HELPER', '-DCTRPORT_DLGFLASH_HELPER',
                                              '-DCTRPORT_PROMONOTICE_HELPER', '-DCTRPORT_SMOOTHOLD_HELPER')]
    go_helper[go_helper.index('-o') + 1] = str(go_file)
    # Keep discarded surrounding bodies at the stock optimization too: -Os
    # exposes a -Wrestrict warning there. GO_CODE sizes only the retained unit.
    go_helper += ['-DCTRPORT_GAMEOVERSCREEN_HELPER']
    rc = run(go_helper)
    if rc: return rc
    go_extra, go_sections = vine_extract.late_sections(go_file.read_text(encoding='utf-8'), 'gameoverscreen')
    go_extra = promonotice_compile.route_configure(go_extra)
    go_extra = go_extra.replace('.LFlowery_', '.LGameover_').replace('FloweryLocal_', 'GameoverLocal_')
    go_pattern = r'(?m)^([ \t]+b(?:l)?[ \t]+)ctrDrawSprite(\(PLT\))?([ \t]*(?:@[^\n]*)?)$'
    go_callers = [re.findall(r'(?m)^([A-Za-z_][\w.]*):', text[:m.start()])[-1]
                  for m in re.finditer(go_pattern, text)]
    # A render-timing (profile) build compiles none of the utk/uta/asl fast paths (ctr_renderer.c's guards), so only
    # the replay and skip paths call ctrDrawSprite there, and nothing calls utkDrawSpriteBody.
    timing = '-DCTRPORT_RENDER_TIMING' in command
    assert sorted(go_callers) == (sorted(['skipReplayOps', 'skipDrawSprite']) if timing else
                                  sorted(['skipReplayOps', 'aslFineSprite', 'utkDrawSpriteBody', 'utkDrawSprite',
                                          'utkDrawSprite', 'skipDrawSprite', 'utaFineSprite'])), go_callers
    text = re.sub(go_pattern, lambda m: m[1]+'CtrGameover_drawSprite'+(m[2] or '')+m[3], text)
    coarse_pattern = r'(?m)^([ \t]+b(?:l)?[ \t]+)utkDrawSpriteBody(\(PLT\))?([ \t]*(?:@[^\n]*)?)$'
    coarse_callers = [re.findall(r'(?m)^([A-Za-z_][\w.]*):', text[:m.start()])[-1]
                      for m in re.finditer(coarse_pattern, text)]
    assert coarse_callers == ([] if timing else ['utkDrawSprite']), coarse_callers
    text = re.sub(coarse_pattern, lambda m: m[1]+'CtrGameover_coarseSprite'+(m[2] or '')+m[3], text)
    # After both reroutes, so the fold's own direct calls stay direct and both caller lists above are as they were.
    text, fuse_receipt = route_fuse(text, go_extra, fuse_expected(command), '-DCTRPORT_ASG_PROBE' in command,
                                    fuse_section(command))
    text += '\n'+go_extra
    text, speech_receipt = undlg_routes.route_renderer(text, diagnostic='-DCTRPORT_DLGFLASH_LOG' in command,
        model='o3ds' if '-DCTRPORT_TARGET_O3DS' in command else 'n3ds')
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
        lcd=lcd_receipt, fuse=fuse_receipt, speech=speech_receipt,
        notice=notice_receipt, smoothold=smo_receipt), indent=2) + '\n', encoding='utf-8')
    print(f'Window motion: {count} final picture call operands routed; warm instructions retained', flush=True)
    print('dimlight LCD call operands: ' + json.dumps(lcd_receipt), flush=True)
    # Every build says whether F1u is fused (an unreviewed fold never gets here: it fails the build, "F1u NOT fused").
    print('F1u slot 25: ' + json.dumps(fuse_receipt), flush=True)
    print('Notice-only configure: ' + json.dumps(notice_receipt), flush=True)
    print('smoothold call operands: ' + json.dumps(smo_receipt), flush=True)
    return rc
