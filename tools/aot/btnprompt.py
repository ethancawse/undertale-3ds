"""UNDERTALE's console prompts (btnprompt; engine/src/z_btnprompt.c): the patch table, made at build time from the
player's own data.win.

The repository holds no game data for this feature: no digests, byte offsets, instruction words or strings. It holds
the rules below, which name code entries (and one text key) and state each site as a GameMaker bytecode shape. This
module finds the sites in the data.win the build is made from, writes what it found into the generated aot_table.c
(the build directory, never committed), and makes the same branch patches to the DataWin before code generation, so
the generated code is the code the engine runs and its digests match.

What it finds:
- Branches: in each BRANCH_ENTRIES entry, every test of global.osflavor against a constant (Push.v osflavor; Push.i K;
  Cmp; Bf/Bt). UNDERTALE runs here as its PC version (osflavor 1); each test is turned to the outcome the console
  versions take by changing K: "osflavor >= K" (K > 1) becomes ">= 1", "osflavor <= K" (K >= 1) becomes "<= -2" (not
  "<= 0": generated code compares with -2.0 as with 2.0, a VFP immediate, at the same size). An entry with any other
  test of osflavor, or none, is left alone.
- Letter strings: in LETTER_ENTRY, each string_replace_all(<local>, P, R) whose P and R are string constants, where P
  is a text button escape (backslash, asterisk, then the key's letter Z, X or C) and R is three characters long. R's
  Push.s goes into the slot of P's letter.
- Title strings: in each TITLE_ENTRIES entry, the value of the ds_map_add whose key is TITLE_KEY.
- The button sprite: SPRITE_ENTRY's length and digest, so that the engine replaces only that script.

The generated table also carries each found string as the data.win has it (build directory only), so that the engine
can check it and put it back. The engine writes our own text into the strings it found (the 3DS button letters and the
title from platform/3ds/source/z_btnprompt_ctr.c), never edited game text, and patches nothing whose bytes differ from
what this module saw.

usage: py -3 -I btnprompt.py <data.win>   (lists what it finds; prints names, offsets and counts only)"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

NEEDS_GLYPHS = 1   # the entry's console branch draws a button sprite (scr_getbuttonsprite): only with the glyphs
# The entries whose osflavor tests take the console branch, and why:
BRANCH_ENTRIES = (
    ("gml_Script_scr_namingscreen", 0),          # instruction screen: no F4 or Hold ESC lines, console layout
    ("gml_Object_obj_papdate_Step_0", 0),        # Papyrus's date asks for the controller's button, not the C key
    ("gml_Object_obj_piano_Draw_0", 0),          # the piano rooms' quit scribble, which consoles leave out
    ("gml_Object_obj_paino_Draw_0", 0),
    ("gml_Object_obj_pressZ_Draw_0", NEEDS_GLYPHS),   # the Tough Glove's prompt: the button's sprite
)
OSFLAVOR = "osflavor"
LETTER_ENTRY = "gml_Script_scr_replace_buttons_pc"
LETTERS = "ZXC"                                   # slots 0, 1, 2
TITLE_ENTRIES = (("gml_Script_textdata_en", 3), ("gml_Script_textdata_ja", 4))
TITLE_KEY = "title_press_button_pc"
SPRITE_ENTRY = "gml_Script_scr_getbuttonsprite"
SLOT_TEXT_MAX = 19   # BtnpromptString text room (z_btnprompt.h)
CODE_MAX = 8         # BTNPROMPT_CODE_MAX (z_btnprompt.h)

OP_CONV, OP_CMP, OP_PUSHI, OP_BT, OP_BF, OP_CALL = 0x07, 0x15, 0x84, 0xB7, 0xB8, 0xD9
OP_PUSH, OP_PUSHLOC, OP_PUSHGLB = 0xC0, 0xC1, 0xC2
T_VAR, T_STRING, T_INT16 = 5, 6, 0xF
CMP_LTE, CMP_GTE = 2, 5


def fnv1a32(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def _instrs(dw, e):
    import gmlaot
    blob = bytes(dw.data[e["abs"]:e["abs"] + e["length"]])
    out, a = [], 0
    while a < len(blob):
        x = gmlaot.Instr(blob, a)
        out.append(x)
        a = x.next
    return blob, out


def _entry(dw, name):
    e = dw.code_by_name.get(name)
    return e if e is not None and e["offset"] == 0 else None


def _strings(dw):
    start = dw.chunks["STRG"][0]
    count = dw.u32(start)
    return lambda i: dw.string(dw.u32(start + 4 + 4 * i) + 4) if 0 <= i < count else None


def _push_string(x):
    return x.op == OP_PUSH and x.t1 == T_STRING


def _patched(word, kind):
    k = struct.unpack("<h", struct.pack("<H", word & 0xFFFF))[0]
    if kind == CMP_GTE and k > 1:
        return (word & 0xFFFF0000) | 1
    if kind == CMP_LTE and k >= 1:
        return (word & 0xFFFF0000) | (-2 & 0xFFFF)
    return None


def generate(dw):
    """The table for dw (after dw.patch_operands()): {"code": [(entry, CODE index, length, digest, offset, word, patched,
    flags)], "strings": [(entry, CODE index, length, offset, slot, the string as the data.win has it)], "sprite": (entry,
    CODE index, length, digest) or None, "notes": [...]}. Rows of one entry are adjacent, as the engine expects. The entry
    names are for notes; the engine finds entries by index."""
    notes, code, strings = [], [], []
    osflavor = {i for i, v in enumerate(dw.vars) if v["name"] == OSFLAVOR}
    for name, flags in BRANCH_ENTRIES:
        e = _entry(dw, name)
        if e is None:
            notes.append("btnprompt: %s absent, its PC prompt stays" % name)
            continue
        blob, ins = _instrs(dw, e)
        rows, other = [], 0
        for k, x in enumerate(ins):
            if not (x.op in (OP_PUSH, OP_PUSHLOC, OP_PUSHGLB) and x.t1 == T_VAR and x.var_index() in osflavor):
                continue
            q, c, b = (ins[k + j] if k + j < len(ins) else None for j in (1, 2, 3))
            patched = None
            if q is not None and c is not None and b is not None and q.op == OP_PUSHI and q.t1 == T_INT16 and \
                    c.op == OP_CMP and b.op in (OP_BT, OP_BF):
                patched = _patched(q.word, (c.word >> 8) & 0xFF)
            if patched is None:
                other += 1
            else:
                rows.append((q.addr, q.word, patched))
        if other or not rows:
            notes.append("btnprompt: %s has %d osflavor tests of another shape and %d of ours, left alone"
                         % (name, other, len(rows)))
            continue
        digest = fnv1a32(blob)
        if len(code) + len(rows) > CODE_MAX:
            notes.append("btnprompt: %s would pass the table's %d rows, left alone" % (name, CODE_MAX))
            continue
        code.extend((name, e["index"], e["length"], digest, off, word, patched, flags) for off, word, patched in rows)
    string_at = _strings(dw)
    e = _entry(dw, LETTER_ENTRY)
    found = {}
    if e is not None:
        _, ins = _instrs(dw, e)
        for k in range(len(ins) - 5):
            r, c1, p, c2, s, call = ins[k:k + 6]
            if not (_push_string(r) and c1.op == OP_CONV and _push_string(p) and c2.op == OP_CONV and
                    s.op in (OP_PUSH, OP_PUSHLOC) and call.op == OP_CALL and call.inst == 3 and
                    dw.funcs[call.extra]["name"] == "string_replace_all"):
                continue
            pat, rep = string_at(p.extra), string_at(r.extra)
            if pat is None or rep is None or len(pat) != 3 or pat[:2] != "\\*" or pat[2] not in LETTERS or len(rep) != 3:
                continue
            slot = LETTERS.index(pat[2])
            if slot not in found:
                found[slot] = (LETTER_ENTRY, e["index"], e["length"], r.addr, slot, rep)
    for slot in range(len(LETTERS)):
        if slot in found:
            strings.append(found[slot])
        else:
            notes.append("btnprompt: no %s string in %s, that key keeps its PC label" % (LETTERS[slot], LETTER_ENTRY))
    for name, slot in TITLE_ENTRIES:
        e = _entry(dw, name)
        hit = None
        if e is not None:
            _, ins = _instrs(dw, e)
            for k in range(len(ins) - 5):
                v, c1, key, c2, m, call = ins[k:k + 6]
                if _push_string(v) and c1.op == OP_CONV and _push_string(key) and c2.op == OP_CONV and \
                        m.op in (OP_PUSH, OP_PUSHGLB) and call.op == OP_CALL and call.inst == 3 and \
                        dw.funcs[call.extra]["name"] == "ds_map_add" and string_at(key.extra) == TITLE_KEY:
                    text = string_at(v.extra)
                    if text is not None and 0 < len(text.encode("utf-8")) <= SLOT_TEXT_MAX and hit is None:
                        hit = (name, e["index"], e["length"], v.addr, slot, text)
        if hit is None:
            notes.append("btnprompt: no title prompt in %s, it stays" % name)
        else:
            strings.append(hit)
    e = _entry(dw, SPRITE_ENTRY)
    sprite = None
    if e is not None:
        blob, _ = _instrs(dw, e)
        sprite = (SPRITE_ENTRY, e["index"], e["length"], fnv1a32(blob))
    notes.append("btnprompt: %d branches in %d entries, %d strings, sprite script %s"
                 % (len(code), len({r[0] for r in code}), len(strings), "found" if sprite else "absent"))
    return {"code": code, "strings": strings, "sprite": sprite, "notes": notes}


def apply(dw, table):
    """Makes the table's branch patches to dw.data (after dw.patch_operands()), as the engine does at VM_create;
    returns one note per entry. The Tough Glove's entry is patched here too: without the glyphs the engine leaves it,
    its digest no longer matches the generated code, and it runs in the interpreter, as it should."""
    notes = list(table["notes"])
    by_entry = {}
    for r in table["code"]:
        by_entry.setdefault(r[0], []).append(r)
    for name, group in by_entry.items():
        base = dw.code_by_name[name]["abs"]
        for _, _, _, _, off, _, patched, _ in group:
            struct.pack_into("<I", dw.data, base + off, patched)
        notes.append("btnprompt: %s patched at %s" % (name, ", ".join("0x%X" % r[4] for r in group)))
    return notes


def c_source(table, c_string):
    """The table as C, for aot_table.c (z_btnprompt.h has the types; the table has a fixed size)."""
    out = ["#include \"z_btnprompt.h\"\n",
           "const BtnpromptTable btnpromptTable BTNPROMPT_TABLE_SECTION = {",
           "    %du, %du," % (len(table["code"]), len(table["strings"])), "    {"]
    for name, index, length, digest, off, word, patched, flags in table["code"]:
        out.append("        { %du, %du, 0x%08Xu, 0x%04Xu, 0x%08Xu, 0x%08Xu, %du },  // %s"
                   % (index, length, digest, off, word, patched, flags, name))
    out.append("    },")
    out.append("    {")
    for name, index, length, off, slot, text in table["strings"]:
        out.append("        { %du, %du, 0x%Xu, %du, %s },  // %s" % (index, length, off, slot, c_string(text), name))
    out.append("    },")
    sprite = table["sprite"] or ("", 0, 0, 0)
    out.append("    %du, %du, 0x%08Xu,  // %s" % (sprite[1], sprite[2], sprite[3], sprite[0] or "absent"))
    out.append("};")
    return "\n".join(out) + "\n"


def source_text():
    """The rules as text, for prepare.py's fingerprint."""
    return repr((BRANCH_ENTRIES, OSFLAVOR, LETTER_ENTRY, LETTERS, TITLE_ENTRIES, TITLE_KEY, SPRITE_ENTRY))


def main(argv):
    sys.path.insert(0, HERE)
    import gmlaot
    if len(argv) != 2:
        print(__doc__)
        return 2
    dw = gmlaot.DataWin(argv[1])
    dw.patch_operands()
    table = generate(dw)
    for note in table["notes"]:
        print(note)
    for r in table["code"]:
        print("branch %s +0x%X" % (r[0], r[4]))
    for r in table["strings"]:
        print("string %s +0x%X slot %d (%d bytes)" % (r[0], r[3], r[4], len(r[5].encode("utf-8"))))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
