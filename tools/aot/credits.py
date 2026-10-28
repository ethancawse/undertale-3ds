"""Write romfs/credits.txt for the options menu's Credits page (platform/3ds/source/ctr_legal.c, legalui).

The game's own credits come from the player's data.win at build time, as the game itself words them; nothing of them
is stored in this repository (only the names of the game's text keys and scripts that hold them). This port's
credits follow, from docs/release/credits-port.txt.
- UNDERTALE: the short credits' entries (the credits_short_* keys of the English text table, in the game's order;
  the console versions' variants left out), headings yellow and names white, as the game draws them.
- DELTARUNE: the last chapter's credits list (its credit script: each entry's heading lines and names), headings grey
  and names white.
Without game data (or if the data has no such credits) the file holds this port's credits only.

Output lines: a colour letter (Y yellow, G grey, W white) and the text; an empty line is a gap.

       py -3 -I tools/aot/credits.py --game <undertale|deltarune> --romfs <build>/romfs [--data <game folder>]
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.dirname(os.path.dirname(HERE))
PORT = os.path.join(ROOT, 'docs', 'release', 'credits-port.txt')

OP_POP, OP_CONV, OP_POPZ, OP_DUP, OP_PUSH, OP_PUSHLOC, OP_PUSHGLB, OP_PUSHBLTN, OP_PUSHI, OP_CALL = (
    0x45, 0x07, 0x9E, 0x86, 0xC0, 0xC1, 0xC2, 0xC3, 0x84, 0xD9)


def calls(dw, entry):
    """Yield (function name, [args, arg0 first]) for each call in a code entry, simulating the stack of string pushes
    (a string push is ('s', text); any other value is None; a call to stringset/stringsetloc returns its first
    argument, @@NewGMLArray@@ a list of its arguments)."""
    import gmlaot
    stack = []
    a, end = entry['abs'], entry['abs'] + entry['length']
    while a < end:
        ins = gmlaot.Instr(dw.data, a)
        a = ins.next
        if ins.op == OP_PUSH and ins.t1 == 6:
            text = dw.strg(ins.extra)
            stack.append(('s', text.decode('utf-8')) if text is not None else None)
        elif ins.op in (OP_PUSH, OP_PUSHLOC, OP_PUSHGLB, OP_PUSHBLTN, OP_PUSHI):
            stack.append(None)
        elif ins.op in (OP_POP, OP_POPZ):
            if stack:
                stack.pop()
        elif ins.op == OP_CALL:
            argc = ins.word & 0xFFFF
            args = [stack.pop() if stack else None for _ in range(argc)]
            name = dw.funcs[ins.extra]['name']
            yield name, args
            if name.endswith(('stringset', 'stringsetloc')) and args and args[0] and args[0][0] == 's':
                stack.append(args[0])
            elif name == '@@NewGMLArray@@':
                stack.append(('a', args))
            else:
                stack.append(None)


def texts(value):
    if value is None:
        return []
    if value[0] == 's':
        return [value[1]]
    if value[0] == 'a':
        return [t for v in value[1] for t in texts(v)]
    return []


def undertale(path):
    import gmlaot
    dw = gmlaot.DataWin(path)
    dw.patch_operands()
    entry = dw.code_by_name.get('gml_Script_textdata_en')
    if entry is None:
        return []
    pairs = []
    for name, args in calls(dw, entry):
        if name == 'ds_map_add' and len(args) == 3 and args[1] and args[2] and args[1][0] == args[2][0] == 's':
            key = args[1][1]
            if key.startswith('credits_short_') and not key.endswith('_console'):
                pairs.append((key[len('credits_short_'):], args[2][1]))
    out = []
    numbered = 0
    for key, value in pairs:
        lines = value.split('#')
        if key.isdigit():
            if numbered == 0:  # the first: "By Toby Fox", under the title
                out += ['WUNDERTALE'] + ['W' + t for t in lines]
            else:
                heading = numbered % 2 == 1
                if heading:
                    out.append('')
                out += [('Y' if heading else 'W') + t for t in lines]
            numbered += 1
        elif re.fullmatch(r'(loc\d+_\d+|test_0|thanks_0)', key):
            out += [''] + ['Y' + t for t in lines]
        else:
            out += ['W' + t for t in lines]
    return out


def deltarune(chapters):
    import gmlaot
    for path in reversed(chapters):
        dw = gmlaot.DataWin(path)
        entry = dw.code_by_name.get('gml_GlobalScript_scr_credit')
        if entry is None:
            continue
        dw.patch_operands()
        out = ['WDELTARUNE', 'WBy Toby Fox']
        for name, args in calls(dw, entry):
            if name == '@@NewGMLObject@@' and len(args) >= 3:
                heading, names = texts(args[1]), texts(args[2])
                if heading or names:
                    out += [''] + ['G' + t for t in heading] + ['W' + t for t in names]
        return out if len(out) > 2 else []
    return []


def port(heading):
    out = []
    with open(PORT, encoding='utf-8') as f:
        for line in f.read().splitlines():
            if line.startswith('#'):
                continue
            if not line:
                out.append('')
            elif line[0] in 'HN':
                out.append((heading if line[0] == 'H' else 'W') + line[1:])
            else:
                raise SystemExit('credits: bad line in %s: %r' % (PORT, line))
    return out


def credits(game, data):
    import corpus
    import prepare
    found = prepare.find_data(game, data)
    game_lines = []
    if found:
        parts = [p for _, p, _ in corpus.parts(game, found)]
        if game == 'undertale':
            game_lines = undertale(parts[0]) if parts else []
        else:
            game_lines = deltarune([p for p in parts if 'chapter' in os.path.basename(os.path.dirname(p))])
    if not game_lines:
        print('credits: no %s credits found in the game data; the page has this port\'s only' % game.upper())
    lines = game_lines + ([''] * 3 if game_lines else []) + port('Y' if game == 'undertale' else 'G')
    while lines and not lines[0]:
        lines.pop(0)
    return '\n'.join(lines) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', choices=('undertale', 'deltarune'), required=True)
    ap.add_argument('--romfs', required=True)
    ap.add_argument('--data')
    ap.add_argument('--print', action='store_true', help='print the text instead of writing it')
    a = ap.parse_args()
    text = credits(a.game, a.data)
    if a.print:
        sys.stdout.reconfigure(encoding='utf-8')
        sys.stdout.write(text)
        return 0
    path = os.path.join(a.romfs, 'credits.txt')
    data = text.encode('utf-8')
    try:
        with open(path, 'rb') as f:
            same = f.read() == data
    except OSError:
        same = False
    if not same:
        os.makedirs(a.romfs, exist_ok=True)
        with open(path, 'wb') as f:
            f.write(data)
    print('credits: %s (%d lines)%s' % (path, text.count('\n'), '' if not same else ', unchanged'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
