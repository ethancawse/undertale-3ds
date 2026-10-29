"""Private pack metadata for one tightly stacked UNDERTALE dialogue draw.

Resolve the draw's English localization from the owner's bytecode, without storing
game text or bytecode in this repository. Only its lines after the first use the
PC-height row fit; the shared font and the game's pens/line pitch stay unchanged.
The optional ATX1 trailer is consumed by z_asrtext.c. Older packs omit it.
"""
import struct

DRAW = 'gml_Object_obj_bringitinguys_Draw_0'
TEXT = 'gml_Script_textdata_en'


def source(path, dw):
    import gmlaot as ga
    win = ga.DataWin(path)
    if DRAW not in win.code_by_name or TEXT not in win.code_by_name:
        return None
    win.patch_operands()

    def instructions(name):
        e = win.code_by_name[name]
        blob = bytes(win.data[e['abs']:e['abs'] + e['length']])
        addr = e['offset']
        while addr < len(blob):
            i = ga.Instr(blob, addr)
            yield i
            addr = i.next

    def call(i, name):
        return i.op == ga.OP_CALL and win.funcs[i.extra]['name'] == name

    # Six localized draws in this event; the fifth is the affected three-line
    # draw. Its ordinal identifies code structure, never stored game content.
    strings, keys = [], []
    for i in instructions(DRAW):
        if i.op == ga.OP_PUSH and i.t1 == ga.T_STRING:
            strings.append(win.strg(i.extra))
        if call(i, 'scr_gettext'):
            keys.append(strings[-1] if strings else None)
    if len(keys) != 6 or keys[4] is None:
        return None
    key = keys[4]
    strings, matches = [], []
    for i in instructions(TEXT):
        if i.op == ga.OP_PUSH and i.t1 == ga.T_STRING:
            strings.append(win.strg(i.extra))
        if call(i, 'ds_map_add'):
            if len(strings) == 2 and strings[1] == key:
                matches.append(strings[0])
            strings = []
    if len(matches) != 1:
        return None
    text = matches[0]
    if not text or text.count(b'#') != 2 or b'\\' in text or any(c < 32 or c >= 127 for c in text):
        return None
    fonts = [dw.string(dw.u32(p)) if p else None for p in dw.plist('FONT')]
    if 'fnt_plain' not in fonts:
        return None
    return fonts.index('fnt_plain'), text.replace(b'#', b'\n')


def variants(dw, glyphs, rows, adv, text):
    import stglyphs as sg
    needed = set(text.split(b'\n', 1)[1]) - {10, 32}
    remake = sg.derive(dw, 'fnt_plain', adv / 2, rows, None, rowfit='pc')['glyphs']
    base = {ch: ext for ch, v, _, _, ext in glyphs if v == 0}
    if not needed <= remake.keys() or not needed <= base.keys():
        raise ValueError('smalltext: stacked draw has unsupported glyphs')
    extra = [(ch, 4, remake[ch][0], remake[ch][1], base[ch]) for ch in sorted(needed)]
    lookup = bytearray(128)
    for i, (ch, _, _, _, _) in enumerate(extra, len(glyphs)):
        lookup[ch] = i + 1
    # Compute row phase on the whole PC-height font, as for every regular font.
    all_pc = [(ch, 0, g, left, base[ch]) for ch, (g, left) in remake.items() if ch in base]
    shift = sg.row_shifts(all_pc)
    return extra, bytes(lookup) + struct.pack('<bbBB', *shift, 1, 0)


def motion(path, dw):
    """Resolve the original shaking writer's localization at its verified call site.

    A different bytecode layout fails closed. The game-derived key and text stay
    in the private pack, rather than in a compiled string or hash allowlist.
    """
    import gmlaot as ga
    win = ga.DataWin(path)
    event = 'gml_Object_obj_friendscene_Step_0'
    if event not in win.code_by_name or TEXT not in win.code_by_name:
        return None
    win.patch_operands()

    def instructions(name):
        e = win.code_by_name[name]
        blob = bytes(win.data[e['abs']:e['abs'] + e['length']])
        addr = e['offset']
        while addr < len(blob):
            i = ga.Instr(blob, addr)
            yield addr, i
            addr = i.next

    at = dict(instructions(event))
    key_push, call = at.get(0x715C), at.get(0x7168)
    if (key_push is None or key_push.op != ga.OP_PUSH or key_push.t1 != ga.T_STRING or
            call is None or call.op != ga.OP_CALL or win.funcs[call.extra]['name'] != 'scr_gettext'):
        return None
    key = win.strg(key_push.extra)
    strings, matches = [], []
    for _, i in instructions(TEXT):
        if i.op == ga.OP_PUSH and i.t1 == ga.T_STRING:
            strings.append(win.strg(i.extra))
        if i.op == ga.OP_CALL and win.funcs[i.extra]['name'] == 'ds_map_add':
            if len(strings) == 2 and strings[1] == key:
                matches.append(strings[0])
            strings = []
    if len(matches) != 1:
        return None
    text = matches[0]
    if not text or text.count(b'&') != 1 or b'\\' in text or any(c < 32 or c >= 127 for c in text):
        return None
    fonts = [dw.string(dw.u32(p)) if p else None for p in dw.plist('FONT')]
    return (fonts.index('fnt_plain'), text) if 'fnt_plain' in fonts else None


def trailer(font, text, rule, moving=None):
    out = b'ATX1' + struct.pack('<II', font, len(text)) + rule + text
    if moving is not None:
        font, text = moving
        out += b'ASY1' + struct.pack('<II', font, len(text)) + text
    return out
