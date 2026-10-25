#!/usr/bin/env python3
"""Ahead-of-time compiler for GameMaker bytecode (prototype, see docs/aot.md).

Reads YOUR data.win and translates the code entries you name (normally the hottest ones from a
--profile-gml-scripts run) into C functions written against engine/src/aot/vm_aot.h. The output is derived from
the game, so it goes into your build directory and is never committed.

usage:
  gmlaot.py <out_dir> [--target o3ds|n3ds | --budget MB] [--scene-weight share|ops] [--corpus-weight NAME W] [--cover PCT] [--max N] [--chunk N]
            [--max-size N] [--no-native] [--no-direct] [--no-slots] [--no-sites] [--no-inline] [--no-edge-lookups]
            [--no-array-refs] [--no-array-writes] [--shared-helpers] --game <data.win> [--names FILE] [--profile LOG ...]
            [--corpus NAME GLOB ...] [--game ...]
  gmlaot.py <out_dir> --target o3ds|n3ds --selection FILE --data DIR [--layout FILE]    (what platform/3ds/build.sh aot does)

  --selection FILE a list from tools/aot/select.py ("<data.win folder> <code entry>" per line, best first, e.g.
                   tools/aot/selection/deltarune.txt), taken in order until the budget is full, skipping what doesn't
                   fit; replaces --game, --names and --profile. An entry with "extra" after it is taken on top of the
                   budget
  --layout FILE   with --selection: the order to write the chosen functions in ("<data.win folder> <code entry>" per
                   line, from tools/aot/select.py layout), so that code that runs together sits together in the
                   binary; functions it doesn't list follow in the selection's order. The last quarter of the files
                   (LAYOUT_TAIL) keep the selection's order, so a module that has to leave its last files out still
                   leaves out the lowest-ranked functions. Only where each function sits changes: the table, and so
                   what binds to what, stays the same
  --data DIR       with --selection: the game folder, holding data.win (UNDERTALE: the list's folder is "root") or
                   chapterN_windows/data.win (DELTARUNE)
  --game DATA.WIN  starts the options for one data.win (one per DELTARUNE chapter); entries with the same name in
                   several chapters each get their own function
  --names FILE     code entry names, one per line (# comments allowed)
  --profile LOG    a butterscotch log from --profile-gml-scripts, one per scene; what counts is the windows after
                   "Bench: measuring" (or all windows). Without a budget: the hottest entries of each log until
                   --cover percent of its instructions are covered (default 95), at most --max per log (default 40)
  --budget MB      instead, pick entries from all the profiles of all games together until the generated code
                   would take MB megabytes of ARM code (at --bytes-per-instr, default 85, measured on the 3DS
                   build). Entries are ranked by gain per instruction of code, an entry's gain being its share of
                   each profile's instructions, weighted and summed. --names entries come first, whatever their
                   rank
  --scene-weight W what one --profile weighs: share (the default), 1 each; or ops, in proportion to its instructions
                   a frame (mean 1), so heavy scenes count for more. Desktop profiles undercount some scenes that are
                   heavy on the 3DS (DELTARUNE ch2's fireworks draw no sparks headless), so ops is no default
  --target T       the budget for a console: o3ds (4 MB) or n3ds (12 MB)
  --corpus NAME GLOB  more profiles for this game, as one named corpus (a room or encounter sweep, say: many short
                   profiles); with a budget, all of a corpus's profiles, over every game, together weigh as much as
                   --corpus-weight NAME W says (default: as much as all the --profile logs together)
  --max-size N     never generate an entry of more than N instructions (default 20000): GCC needs gigabytes for one
                   C function that size, and the largest entries (UNDERTALE's two text tables, 115,188
                   instructions each) run once
  --chunk N        bytecode instructions per generated C file (default 4000): smaller files compile faster
                   and in parallel
  --tag T          the set's tag in the build ID (default aot<budget MB>, or aot<N>fn without a budget)
  --no-native      accepted for older scripts and does nothing (see --no-utflames)
  --no-utflames    WAD 16 only: leave out the calls to engine/src/z_utflames.c's native fire-bullet bodies
  --no-direct      script calls go through handleCall and argument reads through the interpreter's push, as before
                   direct calls (for comparison)
  --no-slots       every self, other and global variable access looks the variable up, as before cached slots (for
                   comparison)
  --no-sites       every slow path is spelled out where it is, as before the aotSite functions (for comparison)
  --no-inline      Mod, Not, Neg and the math builtins of INLINE_MATH go through their handlers and aotCallBuiltin, as
                   before they were inline (for comparison)
  --no-edge-lookups  no slot lookups on the edges into a merge point (see edge_lookups), as before (for comparison)
  --no-array-refs  Push.v [arraypushaf] and Break pushaf (a[i][j] reads) go through their aotSite functions, as before
                   their fast paths were inline (for comparison)
  --no-array-writes  Push.v [arraypopaf] and Break popaf (a[i][j] = v) go through aotSitePush and aotSiteBreak, as
                   before aotSitePopAF and aotSiteBreakPopAF took their common case (for comparison)
  --shared-helpers one copy of the fast paths' out-of-line helpers (AOT_HELPER in vm_aot.h, weak) instead of one in each
                   generated file: less code, but the calls no longer know which registers the helper keeps

Writes <out_dir>/aot_gen_<n>.c (the functions), <out_dir>/aot_table.c (what the engine binds them by) and
<out_dir>/aot_report.txt. The engine's CMake (AOT_DIR) and the 3DS CMake (CTRPORT_AOT_DIR) compile every
aot_gen_*.c and aot_table.c they find in that directory.
WAD 16 (GameMaker 1.4: UNDERTALE) and WAD 17 (GameMaker 2.3+: DELTARUNE) bytecode are supported; anything else is left to
the interpreter.
"""
import glob
import os
import re
import struct
import sys
import time

# ---------------------------------------------------------------------------------------------------------
# data.win reading

class DataWin:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = bytearray(f.read())
        d = self.data
        if d[0:4] != b"FORM":
            raise SystemExit("not a data.win: " + path)
        self.chunks = {}
        pos = 8
        end = 8 + struct.unpack_from("<I", d, 4)[0]
        while pos + 8 <= end:
            name = bytes(d[pos:pos + 4]).decode("latin-1")
            size = struct.unpack_from("<I", d, pos + 4)[0]
            self.chunks[name] = (pos + 8, size)
            pos += 8 + size
        gen8 = self.chunks["GEN8"][0]
        self.wad = d[gen8 + 1]
        self.parse_code()
        self.parse_vari()
        self.parse_func()

    def u32(self, p):
        return struct.unpack_from("<I", self.data, p)[0]

    def i32(self, p):
        return struct.unpack_from("<i", self.data, p)[0]

    def string(self, ptr):
        if ptr == 0:
            return None
        end = self.data.index(0, ptr)
        return bytes(self.data[ptr:end]).decode("utf-8", "replace")

    def parse_code(self):
        start, size = self.chunks["CODE"]
        self.code = []
        self.code_by_name = {}
        if size == 0:
            return
        count = self.u32(start)
        for i in range(count):
            p = self.u32(start + 4 + 4 * i)
            if p == 0:
                self.code.append(None)
                continue
            e = {"index": i, "name": self.string(self.u32(p)), "length": self.u32(p + 4)}
            if self.wad <= 14:
                e.update(locals=0, args=0, abs=p + 8, offset=0)
            else:
                e["locals"] = struct.unpack_from("<H", self.data, p + 8)[0]
                e["args"] = struct.unpack_from("<H", self.data, p + 10)[0]
                e["abs"] = p + 12 + self.i32(p + 12)
                e["offset"] = self.u32(p + 16)
            self.code.append(e)
            self.code_by_name[e["name"]] = e

    def parse_vari(self):
        start, size = self.chunks["VARI"]
        self.vars = []
        if self.wad <= 14:
            raise SystemExit("WAD %d bytecode is not supported" % self.wad)
        n = (size - 12) // 20
        for i in range(n):
            p = start + 12 + 20 * i
            self.vars.append({"name": self.string(self.u32(p)), "instanceType": self.i32(p + 4),
                              "varID": self.i32(p + 8), "occurrences": self.u32(p + 12),
                              "firstAddress": self.u32(p + 16)})

    def parse_func(self):
        start, size = self.chunks["FUNC"]
        self.funcs = []
        n = self.u32(start)
        for i in range(n):
            p = start + 4 + 12 * i
            raw = self.u32(p + 8)
            # GameMaker 2.3+ points firstAddress at the operand word; the VM moves it back to the instruction.
            if self.wad >= 17 and raw != 0xFFFFFFFF:
                raw -= 4
            self.funcs.append({"name": self.string(self.u32(p)), "occurrences": self.u32(p + 4), "firstAddress": raw})

    def strg(self, index):
        """String index's text (STRG: a count, then pointers to a u32 length and the bytes), or None."""
        start, size = self.chunks.get("STRG", (0, 0))
        if size == 0 or index < 0 or index >= self.u32(start):
            return None
        p = self.u32(start + 4 + 4 * index)
        return bytes(self.data[p + 4:p + 4 + self.u32(p)])

    def patch_operands(self):
        """What patchReferenceOperands does at VM_create: variable operands become (upper 5 bits | varIdx), function
        operands funcIdx (PushRef ones (ASSET_TYPE_SCRIPT << 24) | funcIdx)."""
        d = self.data
        for idx, v in enumerate(self.vars):
            addr = v["firstAddress"]
            for occ in range(v["occurrences"]):
                op = self.u32(addr + 4)
                struct.pack_into("<I", d, addr + 4, (op & 0xF8000000) | (idx & 0x07FFFFFF))
                if occ + 1 < v["occurrences"]:
                    addr += op & 0x07FFFFFF
        for idx, f in enumerate(self.funcs):
            addr = f["firstAddress"]
            for occ in range(f["occurrences"]):
                op = self.u32(addr + 4)
                instr = self.u32(addr)
                if (instr >> 24) == 0xFF and to_i16(instr) == -11:
                    delta = op & 0x00FFFFFF
                    struct.pack_into("<I", d, addr + 4, (5 << 24) | (idx & 0x00FFFFFF))
                else:
                    delta = op & 0x07FFFFFF
                    struct.pack_into("<I", d, addr + 4, idx)
                if occ + 1 < f["occurrences"]:
                    addr += delta


def to_i16(instr):
    v = instr & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


# ---------------------------------------------------------------------------------------------------------
# Bytecode

OP_CONV, OP_MUL, OP_DIV, OP_REM, OP_MOD, OP_ADD, OP_SUB = 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D
OP_AND, OP_OR, OP_XOR, OP_NEG, OP_NOT, OP_SHL, OP_SHR, OP_CMP = 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15
OP_POP, OP_PUSHI, OP_DUP, OP_CALLV, OP_RET, OP_EXIT, OP_POPZ = 0x45, 0x84, 0x86, 0x99, 0x9C, 0x9D, 0x9E
OP_B, OP_BT, OP_BF, OP_PUSHENV, OP_POPENV = 0xB6, 0xB7, 0xB8, 0xBA, 0xBB
OP_PUSH, OP_PUSHLOC, OP_PUSHGLB, OP_PUSHBLTN, OP_CALL, OP_BREAK = 0xC0, 0xC1, 0xC2, 0xC3, 0xD9, 0xFF

T_DOUBLE, T_FLOAT, T_INT32, T_INT64, T_BOOL, T_VAR, T_STRING, T_INT16 = 0, 1, 2, 3, 4, 5, 6, 0xF
EXTRA_SIZE = {T_DOUBLE: 8, T_FLOAT: 4, T_INT32: 4, T_INT64: 8, T_BOOL: 4, T_VAR: 4, T_STRING: 4}
WITH_EXTRA = (OP_PUSH, OP_PUSHLOC, OP_PUSHGLB, OP_PUSHBLTN, OP_POP, OP_CALL, OP_BREAK)

SELF, OTHER, ALL, NOONE, GLOBAL, BUILTIN, LOCAL, STACKTOP, ARG, STATIC = -1, -2, -3, -4, -5, -6, -7, -9, -15, -16
VT_ARRAY, VT_STACKTOP, VT_NORMAL, VT_INSTANCE, VT_PUSHAF, VT_POPAF = 0x00, 0x80, 0xA0, 0xE0, 0x10, 0x90
VARIABLE_BUILTIN = -6

DIRECT = True   # direct script calls and inline argument reads (--no-direct turns them off)
LAVA_PART_DIRECT = False  # parked experiment: Azahar saving did not carry to the Old hardware run
SLOTS = True    # cached self/other/global variable slots (--no-slots turns them off)
SITES = True    # slow paths through the aotSite functions (--no-sites turns them off)
INLINE = True   # Mod, Not, Neg and INLINE_MATH builtins inline (--no-inline turns them off)
EDGE_LOOKUPS = True   # slot lookups on the edges into merge points (--no-edge-lookups turns them off)
ARRAY_REFS = True   # Push.v [arraypushaf] and Break pushaf fast paths inline (--no-array-refs turns them off)
# Push.v [arraypopaf] and Break popaf through the site functions that take their common case (aotSitePopAF and
# aotSiteBreakPopAF; --no-array-writes turns them off)
ARRAY_WRITES = True
SHARED_HELPERS = False   # one copy of the AOT_HELPER functions, weak (--shared-helpers)
# --typed <list>: the (data.win folder, code entry) pairs whose loops get typed regions (typed.py), or typed.ALL
# ("--typed all": every code entry compiled), None when off. With it off, nothing here changes what is generated.
TYPED = None
# (code entry, aotf number, region function text, straight region, bytecode start) of this run
TYPED_TEXTS = []
# --typed all: (aotf number, inline bindings text) of this run, for aot_typed_bind.c
TYPED_BIND = []
# --typed all: about this much C text in each aot_typed_<n>.c, so that a module too big for its region can be linked
# without its lowest-priority regions a few at a time (platform/3ds/aot/aot_module_link.cmake)
TYPED_FILE_CHARS = 24000
typed_mod = None
ESCAPE = True      # escape elision of `new` (tools/aot/escape.py, G9); --no-escape turns it off
escape_mod = None
# WAD 16 (UNDERTALE) only (utflowey): code entries, by their bytecode's digest (the table's), that get a late copy
# of their generated function (aotl_<k>, in aot_late.c, which links into late text) whose builtin writes on self of
# the fields below are the setter's own work inline (engine/src/aot/aot_late_copy.h) instead of the site; aotf_<k>
# stays as it was. For small entries that run on hundreds of instances a frame. --no-late-copies turns it off.
LATE_COPIES = {
    0x18F12B40727B60B2: "Photoshop Flowey's spinning eyes, ~250 at once: image_angle, image_xscale, image_yscale",
    0x56261395C32C3F19: "Photoshop Flowey's huge spinning bullets, ~80 at once: image_angle",
}
LATE_COPY_SETTERS = {"x": ("AOT_LATE_SET_FLOAT_DIRTY", "x"), "y": ("AOT_LATE_SET_FLOAT_DIRTY", "y"),
                     "image_xscale": ("AOT_LATE_SET_FLOAT_DIRTY", "imageXscale"),
                     "image_yscale": ("AOT_LATE_SET_FLOAT_DIRTY", "imageYscale"),
                     "image_angle": ("AOT_LATE_SET_REAL_DIRTY", "imageAngle"),
                     "image_index": ("AOT_LATE_SET_FLOAT", "imageIndex"), "image_speed": ("AOT_LATE_SET_FLOAT", "imageSpeed"),
                     "image_alpha": ("AOT_LATE_SET_FLOAT", "imageAlpha")}
LATE_TEXTS = []    # the late copies' C text, for aot_late.c
LATE_COPY_SITE = re.compile(r"^(\s*)(AOT_SITE_POPVAR\(0x[0-9A-F]+, (\d+), 0, \d+, 0x([0-9A-F]{8})u\));  /\* (\w+) \*/$")

# WAD 16 (UNDERTALE) only: code entries engine/src/z_utflames.c has a native body for go to it (UTF_NATIVE);
# --no-utflames turns it off.
UTFLAMES = True
LAYOUT_TAIL = 25   # --layout leaves the selection's last 25% of the files of generated code where they are
# Math builtins whose calls on plain numbers are inline (AOT_MATH<n> in vm_aot.h): name -> (argument count, the
# aotMath function in vm_aot.h that copies the builtin's body, the hash of that body as builtin_audit.py's
# builtin_body_hashes() takes it). A builtin whose body has changed since its copy was made isn't inlined (a warning
# says so) until the copy and its hash here are brought up to date together.
INLINE_MATH = {
    "floor": (1, "aotMathFloor", "a074e33f38ad"),
    "ceil": (1, "aotMathCeil", "1e7e07d83754"),
    "abs": (1, "aotMathAbs", "5d0b1f9ca74b"),
    "sqrt": (1, "aotMathSqrt", "7f2dfcf7e8e9"),
    "sqr": (1, "aotMathSqr", "09c2e98e07b1"),
    "sin": (1, "aotMathSin", "3abec912e2e1"),
    "cos": (1, "aotMathCos", "c2b3340c4440"),
    "dsin": (1, "aotMathDsin", "0d35603b15c3"),
    "dcos": (1, "aotMathDcos", "81dba1fa244e"),
    "degtorad": (1, "aotMathDegtorad", "14d203320bec"),
    "radtodeg": (1, "aotMathRadtodeg", "2904edc0f67c"),
    "frac": (1, "aotMathFrac", "f06da205acce"),
    "sign": (1, "aotMathSign", "cfb3f60b698c"),
    "max": (2, "aotMathMax", "f034eaf5416a"),
    "min": (2, "aotMathMin", "81dbaa0e1510"),
    "lengthdir_x": (2, "aotMathLengthdirX", "7200f4ec0d4a"),
    "lengthdir_y": (2, "aotMathLengthdirY", "78c664a96a92"),
    "angle_difference": (2, "aotMathAngleDifference", "a734b1d368d5"),
    "clamp": (3, "aotMathClamp", "9ea3ad607d0c"),
    "lerp": (3, "aotMathLerp", "0d5a0f0726ee"),
    "point_distance": (4, "aotMathPointDistance", "2d7642aecf40"),
}
SLOT_SCOPES = (SELF, OTHER, GLOBAL)
# The builtin variables pushSelfBuiltinVar reads straight from the instance, and the Instance field each one is.
SELF_FIELDS = {"x": "x", "y": "y", "image_index": "imageIndex", "sprite_index": "spriteIndex", "image_xscale": "imageXscale",
               "image_yscale": "imageYscale", "image_angle": "imageAngle", "image_alpha": "imageAlpha", "image_blend": "imageBlend"}
PURE, CLOBBER = ("pure",), ("clobber",)
# The builtin variables whose writes can run code (VMBuiltins_setVariable: Runner_setLives and Runner_setHealth run
# the no-more-lives and no-more-health events).
BUILTIN_WRITES_RUN_CODE = ("lives", "health")
_builtin_audit = None
_inline_math = None


def inline_math():
    """{builtin name: (argument count, aotMath function)} of the INLINE_MATH builtins whose engine body is still the
    one their copy in vm_aot.h was made from."""
    global _inline_math
    if _inline_math is None:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from builtin_audit import builtin_body_hashes
        hashes = builtin_body_hashes()
        _inline_math = {}
        for name, (argc, fn, h) in sorted(INLINE_MATH.items()):
            if hashes.get(name) == h:
                _inline_math[name] = (argc, fn)
            else:
                print("gmlaot: warning: builtin %s changed (body hash %s, %s copied from %s): calls to it are not inline"
                      % (name, hashes.get(name), fn, h), file=sys.stderr)
    return _inline_math


def builtin_audit():
    """{registered builtin: None when a call to it leaves cached slots alone, else why not} (builtin_audit.py, run on
    this tree's engine sources)."""
    global _builtin_audit
    if _builtin_audit is None:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from builtin_audit import audit
        _builtin_audit = audit()
    return _builtin_audit


_pure_scripts = {}   # (data.win, code entry name) -> whether a call to it leaves cached slots alone


def script_pure(dw, entry_name):
    """Whether running a script's code leaves cached slots alone: every instruction the generator can follow, and each
    either a read or one the slot analysis lets pointers survive (a call to a pure builtin or script among them), apart
    from its returns. Writes to self, other or global variables may insert, so they don't qualify. A script that is
    being worked out (a recursive call) counts as impure, which only ever loses pointers."""
    key = (id(dw), entry_name)
    if key in _pure_scripts:
        return _pure_scripts[key]
    _pure_scripts[key] = False
    entry = dw.code_by_name[entry_name]
    pure = False
    try:
        fn = Function(dw, entry, bytes(dw.data[entry["abs"]:entry["abs"] + entry["length"]]))
        if not fn.stubs:
            em = Emitter(dw, fn, -1)
            pure = True
            for a, ins in fn.instrs.items():
                if a not in fn.depth or ins.op in (OP_RET, OP_EXIT):
                    continue
                kind = em.slot_kind(ins, fn.stacks[a])
                if not (kind is PURE or kind[0] in ("read", "aread")) or (ins.op == OP_BREAK and ins.inst == -2):
                    pure = False
                    break
    except (Reject, Stub):
        pure = False
    _pure_scripts[key] = pure
    return pure

BINARY = (OP_MUL, OP_DIV, OP_REM, OP_MOD, OP_ADD, OP_SUB, OP_AND, OP_OR, OP_XOR, OP_SHL, OP_SHR, OP_CMP)
HANDLER_NAMES = {OP_REM: "aotx_handleRem", OP_MOD: "aotx_handleMod", OP_AND: "aotx_handleAnd", OP_OR: "aotx_handleOr",
                 OP_XOR: "aotx_handleXor", OP_SHL: "aotx_handleShl", OP_SHR: "aotx_handleShr", OP_NEG: "aotx_handleNeg",
                 OP_NOT: "aotx_handleNot"}
# gmlTypeNativeSize in vm_fastpath.h
TYPE_NATIVE_SIZE = {T_DOUBLE: 8, T_FLOAT: 4, T_INT32: 4, T_INT64: 8, T_BOOL: 4, T_VAR: 16, T_STRING: 4, T_INT16: 4}
BREAK_EFFECT = {-1: (0, 0), -2: (2, 1), -3: (3, 0), -4: (2, 1), -5: (1, 0), -6: (0, 1), -7: (0, 0), -8: (0, 0),
                -9: (0, 0), -10: (0, 1), -11: (0, 1)}
# The gmlStackType of what a Break pushes (handleBreakPushAF/PushAC/PushRef push variables, IsStaticOk/IsNullish bools):
# a Dup counted in native bytes above one needs it.
BREAK_PUSH_TYPE = {-2: T_VAR, -4: T_VAR, -6: T_BOOL, -10: T_BOOL, -11: T_VAR}


class Reject(Exception):
    pass


class Instr:
    __slots__ = ("addr", "word", "op", "t1", "t2", "inst", "size", "extra", "next")

    def __init__(self, blob, addr):
        if addr + 4 > len(blob):
            raise Reject("runs off the end of the code at %#x" % addr)
        self.addr = addr
        self.word = struct.unpack_from("<I", blob, addr)[0]
        self.op = self.word >> 24
        self.t1 = (self.word >> 16) & 0xF
        self.t2 = (self.word >> 20) & 0xF
        self.inst = to_i16(self.word)
        size = 4
        if self.op in WITH_EXTRA:
            size += EXTRA_SIZE.get(self.t1, 0)
        if addr + size > len(blob):
            raise Reject("instruction at %#x runs off the end of the code" % addr)
        self.size = size
        self.extra = struct.unpack_from("<I", blob, addr + 4)[0] if size >= 8 else None
        self.next = addr + size

    def target(self):
        off = self.word & 0x7FFFFF
        if off & 0x400000:
            off -= 0x800000
        return self.addr + off * 4

    def var_type(self):
        return (self.extra >> 24) & 0xF8

    def var_index(self):
        return self.extra & 0x07FFFFFF


class Stub(Exception):
    """An instruction the generated code hands over to the interpreter (it deopts there)."""


NATIVE_SIZE = {T_DOUBLE: 8, T_FLOAT: 4, T_INT32: 4, T_INT64: 8, T_BOOL: 4, T_VAR: 16, T_STRING: 4, T_INT16: 4}
UNKNOWN = (None, None)


def meet(a, b):
    return (a[0] if a[0] == b[0] else None, a[1] if a[1] == b[1] else None)


def bytes_to_slots(stack, nbytes, pos):
    """bytesToSlotCount on the abstract stack: the number of slots below pos that make up nbytes of native stack,
    going by each slot's gmlStackType; None when a type is unknown or the bytes don't line up (the VM aborts then)."""
    slots = 0
    remaining = nbytes
    while remaining > 0:
        slots += 1
        if pos < slots:
            return None
        t = stack[pos - slots][1]
        if t is None:
            return None
        size = NATIVE_SIZE.get(t, 16)
        if t == T_VAR and 0 < remaining < size:
            remaining = 0
        else:
            remaining -= size
    return slots if remaining == 0 else None


class Function:
    """One code entry: reachable instructions, the abstract stack before each (depth, known int constants and
    gmlStackTypes), labels, and the instructions left to the interpreter."""

    def __init__(self, dw, entry, blob):
        self.dw = dw
        self.entry = entry
        self.blob = blob
        self.instrs = {}
        self.depth = {}
        self.stacks = {}
        self.after = {}             # stack depth after each instruction
        self.dup_slots = {}         # Dup address -> slots it copies (or moves, for a swap: (top, bottom))
        self.labels = set()
        self.stubs = {}             # address -> why the generated code deopts there
        self.fused_cmp = set()      # addresses of Cmp instructions fused with the branch after them
        self.fused_targets = {}     # Dup swap address -> address its fused accessor call skips to
        self.analyse()

    def decode(self, addr):
        ins = self.instrs.get(addr)
        if ins is None:
            ins = Instr(self.blob, addr)
            self.instrs[addr] = ins
        return ins

    def effect(self, ins, stack):
        """The abstract stack after an instruction, given the one before it. Raises Stub for whatever the generator
        leaves to the interpreter."""
        op, t1, t2, inst = ins.op, ins.t1, ins.t2, ins.inst
        n = len(stack)

        def const_at(k):
            return stack[-k][0] if n >= k else None

        def result(pops, *pushed):
            if pops > n:
                raise Stub("stack underflow")
            return stack[:n - pops] + list(pushed)

        # WAD16: GameMaker 1 bytecode has no -9 "instance below on the stack" form, so a stacktop instance type never
        # pops a second value (the VM's popArrayAccess, resolveVariableRead and handlePushEnv check for WAD 17).
        w17 = self.dw.wad >= 17

        def st(cond):
            return 1 if w17 and cond else 0

        V = (None, T_VAR)
        if op in (OP_PUSHLOC, OP_PUSHGLB, OP_PUSHBLTN, OP_POP) or (op == OP_PUSH and t1 == T_VAR):
            if ins.var_index() >= len(self.dw.vars):
                raise Stub("variable index out of range")
        if op == OP_PUSH:
            if t1 == T_VAR:
                vt = ins.var_type()
                if vt == VT_NORMAL:
                    return result(st(inst == STACKTOP), V)
                if vt == VT_ARRAY or vt == VT_PUSHAF or vt == VT_POPAF:
                    return result(2 + st(const_at(2) == STACKTOP), V)
                if vt == VT_STACKTOP:
                    return result(1 + st(const_at(1) == STACKTOP), V)
                if vt == VT_INSTANCE:
                    return result(0, V)
                raise Stub("Push with variable type %#x" % vt)
            if t1 == T_INT16:
                return result(0, (inst, T_INT16))
            if t1 == T_INT32:
                return result(0, (struct.unpack("<i", struct.pack("<I", ins.extra))[0], T_INT32))
            if t1 in (T_DOUBLE, T_FLOAT, T_INT64, T_BOOL, T_STRING):
                return result(0, (None, t1))
            raise Stub("Push of type %#x" % t1)
        if op == OP_PUSHLOC:
            if not w17 and ins.var_type() in (VT_PUSHAF, VT_POPAF):
                raise Stub("PushLoc array reference in WAD %d bytecode" % self.dw.wad)   # WAD16
            return result(0, V)
        if op == OP_PUSHGLB:
            vt = ins.var_type()
            if vt == VT_ARRAY:
                return result(2 + st(const_at(2) == STACKTOP), V)
            if vt == VT_STACKTOP:
                return result(1 + st(const_at(1) == STACKTOP), V)
            return result(0, V)
        if op == OP_PUSHBLTN:
            vt = ins.var_type()
            if vt in (VT_PUSHAF, VT_POPAF):
                return result(0, V)
            if vt == VT_ARRAY:
                return result(2 + st(const_at(2) == STACKTOP), V)
            if vt == VT_STACKTOP:
                return result(1 + st(const_at(1) == STACKTOP), V)
            return result(st(inst == STACKTOP), V)
        if op == OP_PUSHI:
            return result(0, (inst, T_INT16))
        if op == OP_POP:
            if t1 == T_INT16:
                raise Stub("Pop swap form")
            vt = ins.var_type()
            if vt == VT_ARRAY:
                if t1 == T_VAR:
                    return result(3 + st(const_at(2) == STACKTOP))
                return result(3 + st(const_at(3) == STACKTOP))
            if vt == VT_STACKTOP:
                if t1 == T_VAR:
                    return result(2 + st(const_at(1) == STACKTOP))
                return result(2 + st(const_at(2) == STACKTOP))
            if vt == VT_NORMAL:
                return result(1 + st(inst == STACKTOP))
            if vt == VT_INSTANCE:
                return result(1)
            raise Stub("Pop with variable type %#x" % vt)
        if op == OP_POPZ:
            return result(1)
        if op == OP_CMP:
            return result(2, (None, T_BOOL))
        if op in BINARY:
            return result(2, (None, t2))
        if op in (OP_NEG, OP_NOT):
            return result(1, (None, t1))
        if op == OP_CONV:
            return result(1, (None, t2))
        if op == OP_DUP:
            operand = ins.word & 0xFFFF
            size = NATIVE_SIZE.get(t1, 16)
            if not w17:
                # WAD16: no swap form, and only the low 8 bits count (handleDup).
                count = bytes_to_slots(stack, ((operand & 0xFF) + 1) * size, n)
                if count is None:
                    raise Stub("Dup the generator cannot follow")
                self.dup_slots[ins.addr] = count
                return stack + stack[n - count:]
            if operand & 0x8000:
                top_units, bottom_units = operand & 0x7FF, (operand >> 11) & 0xF
                if t1 == T_VAR:
                    top, bottom = top_units, bottom_units
                else:
                    top = bytes_to_slots(stack, top_units * size, n)
                    bottom = None if top is None else bytes_to_slots(stack, bottom_units * size, n - top)
                if top is None or bottom is None or top + bottom > n or top > 32:
                    raise Stub("Dup swap the generator cannot follow")
                self.dup_slots[ins.addr] = (top, bottom)
                return stack[:n - top - bottom] + stack[n - top:] + stack[n - top - bottom:n - top]
            count = bytes_to_slots(stack, ((operand & 0x7FFF) + 1) * size, n)
            if count is None:
                raise Stub("Dup the generator cannot follow")
            self.dup_slots[ins.addr] = count
            return stack + stack[n - count:]
        if op == OP_B:
            return result(0)
        if op in (OP_BT, OP_BF):
            return result(1)
        if op == OP_CALL:
            name = self.dw.funcs[ins.extra]["name"] if ins.extra < len(self.dw.funcs) else None
            if name is None:
                raise Stub("call to an unknown function")
            # @@try_hook@@, @@try_unhook@@, @@finish_catch@@ and @@finish_finally@@ are ordinary calls: try_hook records
            # the handler with ctx->callDepth and the synced stack top, which are the interpreter's, and an exception
            # (only ever raised by a call) deopts at the instruction after it, where executeLoop unwinds to the catch or
            # finally block. Those blocks are reached only that way, so the generated code never contains them.
            return result(ins.word & 0xFFFF, V)
        if op == OP_CALLV:
            if not w17:
                raise Stub("CallV in WAD %d bytecode" % self.dw.wad)   # WAD16
            return result((ins.word & 0xFFFF) + 2, V)
        if op == OP_RET:
            return result(1)
        if op == OP_EXIT:
            return result(0)
        if op == OP_PUSHENV:
            return result(1 + st(const_at(1) == STACKTOP))
        if op == OP_POPENV:
            return result(0)
        if op == OP_BREAK:
            if not w17:
                return result(0)   # WAD16: handleBreak does nothing
            if inst not in BREAK_EFFECT:
                raise Stub("Break %d" % inst)
            pops, pushes = BREAK_EFFECT[inst]
            return result(pops, *([(None, BREAK_PUSH_TYPE.get(inst))] * pushes))
        raise Stub("opcode %#x" % op)

    def successors(self, ins):
        op = ins.op
        if op in (OP_RET, OP_EXIT):
            return []
        if op == OP_B:
            return [ins.target()]
        if op in (OP_BT, OP_BF, OP_PUSHENV):
            return [ins.next, ins.target()]
        if op == OP_POPENV:
            if (ins.word & 0x00FFFFFF) == 0xF00000:
                return [ins.next]
            return [ins.next, ins.target()]
        return [ins.next]

    def analyse(self):
        start = self.entry["offset"]
        length = len(self.blob)
        work = [(start, [])]
        self.depth[start] = 0
        self.stacks[start] = []
        while work:
            addr, stack = work.pop()
            while True:
                if addr == length:
                    break   # falls off the end: returns like Exit
                if addr > length or addr < 0:
                    raise Reject("jump outside the code (%#x)" % addr)
                ins = self.decode(addr)
                try:
                    after = self.effect(ins, stack)
                except Stub as why:
                    self.stubs[addr] = str(why)
                    break
                self.after[addr] = len(after)
                succ = self.successors(ins)
                # Dup.v swap(N, 1) followed by a getter/setter call: tryFusedAccessorCall may skip the next four
                # instructions, landing after the CallV with the receiver and arguments replaced by the result.
                if ins.op == OP_DUP and (ins.word & 0xFFFFF800) == 0x86058800 and self.dw.wad >= 17:   # WAD16: never
                    n_args = ins.word & 0x7FF
                    if self.fused_pattern(ins, n_args) and n_args <= 8 and len(stack) > n_args:
                        tgt = ins.next + 16
                        self.fused_targets[addr] = tgt
                        self.labels.add(tgt)
                        self.merge(tgt, stack[:len(stack) - 1 - n_args] + [(None, T_VAR)], work)
                if ins.op == OP_RET or ins.op == OP_EXIT:
                    break
                for t in succ[1:]:
                    self.labels.add(t)
                    self.merge(t, after, work)
                if ins.op == OP_B:
                    self.labels.add(succ[0])
                    self.merge(succ[0], after, work)
                    break
                nxt = succ[0]
                if nxt in self.depth:
                    self.merge(nxt, after, work)
                    break
                self.depth[nxt] = len(after)
                self.stacks[nxt] = after
                addr, stack = nxt, after
        # Cmp directly followed by BT/BF that nothing else jumps to: fuse.
        for addr, ins in self.instrs.items():
            if ins.op == OP_CMP and addr not in self.stubs and ins.next in self.instrs and ins.next not in self.labels:
                if self.instrs[ins.next].op in (OP_BT, OP_BF) and ins.next not in self.stubs:
                    self.fused_cmp.add(addr)

    def fused_pattern(self, ins, n_args):
        b = self.blob
        p = ins.next
        if p + 16 > len(b):
            return False
        w = struct.unpack_from("<4I", b, p)
        return w[0] == 0x86050000 and w[1] == 0xC005FFF7 and w[3] == (0x99050000 | n_args)

    def merge(self, addr, stack, work):
        if addr in self.depth:
            if self.depth[addr] != len(stack):
                raise Reject("inconsistent stack depth at %#x (%d vs %d)" % (addr, self.depth[addr], len(stack)))
            old = self.stacks[addr]
            merged = [meet(a, b) for a, b in zip(old, stack)]
            if merged != old:
                self.stacks[addr] = merged
                work.append((addr, merged))
            return
        self.depth[addr] = len(stack)
        self.stacks[addr] = list(stack)
        work.append((addr, list(stack)))


# ---------------------------------------------------------------------------------------------------------
# C emission

def c_string(s):
    out = ['"']
    for ch in s.encode("utf-8"):
        if ch in (0x22, 0x5C):
            out.append("\\" + chr(ch))
        elif 0x20 <= ch < 0x7F:
            out.append(chr(ch))
        else:
            out.append("\\%03o" % ch)
    out.append('"')
    return "".join(out)


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


class Emitter:
    def __init__(self, dw, fn, k):
        self.dw = dw
        self.fn = fn
        self.k = k
        self.lines = []
        self.locals = {}      # varIdx -> local number
        self.vars = {}        # varIdx -> True
        self.funcs = {}       # funcIdx -> True
        self.cslots = {}      # (scope, varID) -> name of its cached slot pointer
        self.refinds = []     # the key sets of the AOT_REFIND_<n> placeholders in the code (finish_refinds)
        self.slot_in = {}     # address -> the (scope, varID) whose pointers are good before the instruction
        self.slot_ant = {}    # address -> the (scope, varID) that may be accessed after it before anything clobbers
        self.slot_pre = {}    # address -> the (scope, varID) looked up after it, ahead of a loop that uses them
        self.slot_fix = {}    # (address, successor) -> the (scope, varID) looked up on that edge into a merge point
        self.fix_stubs = []   # (label, keys, target) of the edges whose lookups go on a branch (a stub at the end)
        self.fix_done = set()   # (address, successor, "fall" or "jump") of the edge lookups emitted
        self.cur_in = frozenset()
        self.typed = {}           # --typed: address of a region's call (its head, or the branch above it) -> typed.Region
        self.typed_exits = set()  # --typed: the addresses its regions leave for (an aoty_<address> label each)

    def L(self, addr):
        return "L_%04X" % addr

    def local(self, idx):
        if idx not in self.locals:
            self.locals[idx] = len(self.locals)
        self.vars[idx] = True
        return "LV(%d)" % self.locals[idx]

    def emit(self, s):
        self.lines.append("    " + s)

    def var_ref(self, ins):
        idx = ins.var_index()
        self.vars[idx] = True
        return idx, self.dw.vars[idx]

    def arg_number(self, v):
        """n when the engine resolves variable v to argumentn (BUILTIN_VAR_ARGUMENT0 + n: VM_create resolves builtin
        ids by name for the -6 varID or instance type, and aotMatches checks the name, varID and instance type),
        else None."""
        if not DIRECT or (v["varID"] != VARIABLE_BUILTIN and v["instanceType"] != BUILTIN):
            return None
        m = re.match(r"argument([0-9]+)$", v["name"])
        if m is None or int(m.group(1)) > 15 or "argument%d" % int(m.group(1)) != v["name"]:
            return None
        return int(m.group(1))

    def script_callee(self, func_index):
        """Whether Call.i of this function may run a script: the engine resolves it to a code entry when no builtin
        has the name. aotCallScript checks that at run time and hands anything else to handleCall."""
        name = self.dw.funcs[func_index]["name"]
        if self.dw.wad < 17 and name == "script_execute":
            return DIRECT   # WAD16: a SCPT index, which aotCallScript calls the same way
        return DIRECT and (name in self.dw.code_by_name or ("gml_Script_" + name) in self.dw.code_by_name)

    def constant(self, ins):
        """(GMLReal expression, RValue initialiser, int value or None) for a push of a number constant, else None."""
        if ins.op == OP_PUSHI or (ins.op == OP_PUSH and ins.t1 == T_INT16):
            return "(GMLReal) %d" % ins.inst, "AOT_KI(%d, %d)" % (ins.inst, T_INT16), ins.inst
        if ins.op == OP_PUSH and ins.t1 == T_INT32:
            k = struct.unpack("<i", struct.pack("<I", ins.extra))[0]
            return "(GMLReal) %d" % k, "AOT_KI(%d, %d)" % (k, T_INT32), k
        if ins.op == OP_PUSH and ins.t1 == T_DOUBLE:
            bits = struct.unpack_from("<Q", self.fn.blob, ins.addr + 4)[0]
            kv = "(GMLReal) aotBitsDouble(0x%016XULL)" % bits
            return kv, "AOT_KR(%s)" % kv, None
        return None

    # ---[ Cached variable slots (see "Cached variable slots" in vm_aot.h) ]---

    def slot_kind(self, ins, stack):
        """What an instruction, as gen_instr emits it, does to the cached slot pointers: PURE (nothing can move a
        slot or change a scope's instance), CLOBBER (anything may have), or (kind, scope, varID) for an access that
        uses (and, if it isn't good yet, sets) the pointer of that variable: "read", "write" (which inserts the variable
        when it has no pointer yet), "aread" and "awrite" (array elements with a known scope) and "afread" (the first
        step of a[i][j], AOT_PUSHAF, whose slow path looks the pointers up again)."""
        op, t1, inst = ins.op, ins.t1, ins.inst
        n = len(stack)

        def const_at(k):
            return stack[-k][0] if n >= k else None

        if op == OP_PUSHI or (op == OP_PUSH and t1 != T_VAR):
            return PURE
        if op == OP_PUSH:
            v = self.dw.vars[ins.var_index()]
            vt = ins.var_type()
            if vt == VT_NORMAL and v["varID"] >= 0 and inst in SLOT_SCOPES and not (self.dw.wad < 17 and inst in (ARG, STACKTOP)):
                return ("read", inst, v["varID"]) if SLOTS else CLOBBER
            if vt == VT_ARRAY and SLOTS and v["varID"] >= 0 and const_at(2) in SLOT_SCOPES:
                return ("aread", const_at(2), v["varID"])
            if vt == VT_PUSHAF and self.array_ref_inline(v, const_at(2)):
                return ("afread", const_at(2), v["varID"])
            # Every other read (pushSelfBuiltinVar, tryPushArrayElement, tryPushStructMember, aotPushNamedArg and
            # handlePush's resolveVariableRead) only reads, except PushAF and PopAF, which make the array they
            # step into.
            return CLOBBER if vt in (VT_PUSHAF, VT_POPAF) else PURE
        if op == OP_PUSHLOC:
            return CLOBBER if ins.var_type() in (VT_PUSHAF, VT_POPAF) else PURE
        if op == OP_PUSHGLB:
            v = self.dw.vars[ins.var_index()]
            if SLOTS and ins.var_type() == VT_NORMAL and v["varID"] >= 0:
                return ("read", GLOBAL, v["varID"])
            return CLOBBER if ins.var_type() in (VT_PUSHAF, VT_POPAF) else PURE
        if op == OP_PUSHBLTN:
            return CLOBBER if ins.var_type() in (VT_PUSHAF, VT_POPAF) else PURE
        if op == OP_POP:
            v = self.dw.vars[ins.var_index()]
            vt = ins.var_type()
            if t1 == T_VAR and vt == VT_NORMAL:
                if v["varID"] >= 0 and inst in SLOT_SCOPES:
                    return ("write", inst, v["varID"]) if SLOTS else CLOBBER
                if v["varID"] >= 0 and inst == LOCAL:
                    return PURE
                if v["varID"] == VARIABLE_BUILTIN and inst == SELF and v["name"] not in BUILTIN_WRITES_RUN_CODE:
                    return PURE   # VMBuiltins_setVariable; on a struct (WAD 17) gen_pop looks the pointers up again
                return CLOBBER
            if vt == VT_ARRAY:
                scope = const_at(2) if t1 == T_VAR else const_at(3)
                if SLOTS and v["varID"] >= 0 and scope in SLOT_SCOPES:
                    return ("awrite", scope, v["varID"])
                if v["varID"] == VARIABLE_BUILTIN and scope == SELF and v["name"] not in BUILTIN_WRITES_RUN_CODE:
                    return PURE   # alarm[n] and the like, as above
            return CLOBBER
        if op == OP_POPZ or op in BINARY or op in (OP_NEG, OP_NOT, OP_CONV, OP_B, OP_BT, OP_BF):
            return PURE
        if op == OP_DUP:
            if (ins.word & 0xFFFFF800) == 0x86058800 and self.dw.wad >= 17 and ins.addr in self.fn.fused_targets:
                return CLOBBER   # may call a getter or setter
            return PURE
        if op == OP_BREAK:
            if self.dw.wad < 17 or inst in (-1, -5):   # WAD16: nothing; chkindex and setowner touch no variable
                return PURE
            if inst == -2 and ARRAY_REFS:
                return PURE   # pushaf only reads; AOT_BREAK_PUSHAF looks the pointers up again after its slow path
            return CLOBBER
        if op == OP_CALL and SLOTS:
            name = self.dw.funcs[ins.extra]["name"]
            entry = name if name in self.dw.code_by_name else ("gml_Script_" + name)
            audit = builtin_audit()
            if entry not in self.dw.code_by_name:
                # A builtin that can't insert a variable, create or destroy an instance or run code.
                if name in audit and audit[name] is None:
                    return PURE
            elif name not in audit and script_pure(self.dw, entry):
                return PURE   # a script (no builtin has its name) that only reads and makes pure calls
        return CLOBBER   # other calls, with, returns

    def array_ref_inline(self, v, scope):
        """Whether a Push.v [arraypushaf] of variable v in scope has its fast path inline (AOT_PUSHAF): a plain
        variable (handlePush reads builtins, argument[] among them, another way) in a self, other or global scope known
        here, with cached slots."""
        return (ARRAY_REFS and SLOTS and self.dw.wad >= 17 and v["varID"] >= 0 and v["instanceType"] != BUILTIN
                and scope in SLOT_SCOPES)

    def slot_succ(self, ins):
        fn = self.fn
        succ = [] if ins.op in (OP_RET, OP_EXIT) else fn.successors(ins)
        if ins.addr in fn.fused_targets:
            succ = succ + [fn.fused_targets[ins.addr]]
        return [t for t in succ if t in fn.depth and t in fn.instrs]   # not the end of the code

    def slot_cached(self, addr, kind, state):
        """Whether an access goes through its variable's pointer (looking it up first when it isn't good): when the
        pointer is good, when the variable may be accessed again before anything clobbers, and for a write when other
        pointers are good (an insert has to look them up again). Otherwise a scalar read or write is the one-call
        lookup it was before cached slots, which leaves the pointers as they were (a write's insert can only matter
        to pointers when none are good)."""
        key = (kind[1], kind[2])
        if kind[0] in ("aread", "awrite", "afread") or key in state or key in self.slot_ant.get(addr, ()):
            return True
        return kind[0] == "write" and bool(state)

    def analyse_slots(self):
        """Which slot pointers are good before each instruction: those set on every path to it since the last
        CLOBBER (a forward must-analysis over the code's flow graph), after working out which variables may be
        accessed after each instruction before a CLOBBER (a backward may-analysis), which decides which accesses set
        a pointer. A loop that clobbers nothing has the variables it accesses looked up once on the way in (after the
        instruction that falls into its head, when that is the only way in from before), so they are good all the way
        round it; a lookup only reads, and a pointer to a variable that isn't there is nullptr until it is inserted.
        Then edge_lookups puts lookups on the edges into merge points."""
        fn = self.fn
        start = fn.entry["offset"]
        if not SLOTS or start not in fn.instrs or start not in fn.depth:
            return
        kinds = {a: self.slot_kind(fn.instrs[a], fn.stacks[a]) for a in fn.instrs if a in fn.depth and a not in fn.stubs}
        ant_in, ANT = {}, {}
        changed = True
        while changed:
            changed = False
            for a in sorted(kinds, reverse=True):
                out = frozenset().union(*[ant_in.get(t, frozenset()) for t in self.slot_succ(fn.instrs[a])])
                kind = kinds[a]
                if kind is CLOBBER:
                    new_in = frozenset()
                elif kind is PURE:
                    new_in = out
                else:
                    new_in = out | {(kind[1], kind[2])}
                if ANT.get(a) != out or ant_in.get(a) != new_in:
                    ANT[a], ant_in[a] = out, new_in
                    changed = True
        self.slot_ant = ANT

        def forward(pre, want=None, edges=()):
            """want: merge point -> the pointers the edges into it in edges look up when they aren't good."""
            IN, OUT = {start: frozenset()}, {}
            work = [start]
            while work:
                a = work.pop()
                ins = fn.instrs[a]
                if a in fn.stubs:
                    continue
                kind = kinds[a]
                state = IN[a]
                if kind is CLOBBER:
                    out = frozenset()
                elif kind is PURE or not self.slot_cached(a, kind, state):
                    out = state
                else:
                    out = state | {(kind[1], kind[2])}   # a write that inserts looks the others up again
                out = out | pre.get(a, frozenset())
                OUT[a] = out
                for t in self.slot_succ(ins):
                    c = out | want[t] if want and t in want and (a, t) in edges else out
                    if t not in IN:
                        IN[t] = c
                        work.append(t)
                    else:
                        m = IN[t] & c
                        if m != IN[t]:
                            IN[t] = m
                            work.append(t)
            return IN, OUT

        IN, OUT = forward({})
        preds = {}
        for a in OUT:
            for t in self.slot_succ(fn.instrs[a]):
                preds.setdefault(t, []).append(a)
        pre = {}
        for t in sorted(preds):
            back = [p for p in preds[t] if p >= t]
            ahead = [p for p in preds[t] if p < t]
            if not back or len(ahead) != 1:
                continue
            p = ahead[0]
            pin = fn.instrs[p]
            if self.slot_succ(pin) != [t] or pin.next != t or p in fn.stubs:
                continue   # the way in is a jump or a branch: the lookups would have to go on that edge alone
            body = [a for a in kinds if t <= a <= max(back)]
            if any(kinds[a] is CLOBBER for a in body):
                continue
            used = frozenset((kinds[a][1], kinds[a][2]) for a in body if len(kinds[a]) == 3)
            want = used - OUT.get(p, frozenset())
            if want:
                pre[p] = pre.get(p, frozenset()) | want
        if pre:
            IN, OUT = forward(pre)
        self.slot_pre = pre
        if EDGE_LOOKUPS:
            IN = self.edge_lookups(kinds, preds, pre, forward, IN, OUT)
        self.slot_in = IN

    def edge_lookups(self, kinds, preds, pre, forward, IN, OUT):
        """Lookups on the edges into a merge point, so that a pointer good on some ways in is good on all of them.

        A pointer is good after a merge point only when it is good on every edge into it. In a loop that calls
        something on some iterations (a tile loop that draws the tiles it can see, say), the call clobbers every
        pointer, so the loop's head and every merge after the call look up again, on every iteration, the variables
        the loop uses: the iterations that call nothing pay for the ones that do. Looking them up on the edges where
        they aren't good instead (after the call, on the way out of the inner loop) makes them good at the merge, and
        the other ways in pay nothing.

        Which pointers an edge into merge point t looks up: those every way on from t uses before anything clobbers
        (a backward must-analysis, W), so that each lookup on an edge is one the way on would have made anyway, only
        earlier. Loops would make that circular, and a loop's exit would make the variables used after it count for
        each iteration, so W is worked out on the flow graph without back edges, twice: first with a back edge
        counting as no use and a loop exit as any use, which gives each loop head what every iteration uses (LW);
        then with a back edge counting as LW of its head and an exit as LW of the loop it leaves (as if the loop went
        on). A lookup is then wasted at most once each time a loop is left. A pointer that no edge into t has good
        without its lookup gains nothing there and isn't looked up on them.

        The lookups go after an instruction for its fall-through, or into a stub at the end of the function that a
        branch jumps to instead (fix_stubs). Returns the pointers good before each instruction."""
        fn = self.fn
        start = fn.entry["offset"]
        edges = set()
        for a in OUT:
            ins = fn.instrs[a]
            for t in self.slot_succ(ins):
                if t == ins.next and ins.op not in (OP_B, OP_RET, OP_EXIT):
                    edges.add((a, t))
                if ins.op in (OP_B, OP_BT, OP_BF) and t == ins.target():
                    edges.add((a, t))
        heads = {}
        for t, ps in preds.items():
            back = [p for p in ps if p >= t]
            if back:
                heads[t] = max(back)
        exit_of = {}   # (a, t) of a loop exit -> the head of the innermost loop it leaves
        if heads:
            for a in OUT:
                for t in self.slot_succ(fn.instrs[a]):
                    if t > a:
                        left = [h for h, e in heads.items() if h <= a <= e and not h <= t <= e]
                        if left:
                            exit_of[(a, t)] = min(left, key=lambda h: heads[h] - h)
        nodes = sorted(kinds, reverse=True)

        def meet(x, y):   # None: every pointer
            return y if x is None else x if y is None else x & y

        def must_use(lw):
            W = {}
            for a in nodes:   # every edge but a back edge goes to a later address
                kind = kinds[a]
                if kind is CLOBBER:
                    W[a] = frozenset()
                    continue
                succ = self.slot_succ(fn.instrs[a])
                out = None if succ else frozenset()
                for t in succ:
                    if t <= a:
                        c = frozenset() if lw is None else lw.get(t, frozenset())
                    elif (a, t) in exit_of:
                        c = None if lw is None else lw.get(exit_of[(a, t)], frozenset())
                    else:
                        c = W.get(t, frozenset())
                    out = meet(out, c)
                if kind is not PURE and out is not None:
                    out = out | {(kind[1], kind[2])}
                W[a] = out
            return W

        W1 = must_use(None)
        LW = {h: W1.get(h) or frozenset() for h in heads}
        W = must_use(LW)
        want = {t: W[t] for t, ps in preds.items() if len(ps) >= 2 and t != start and W.get(t)}
        while want:
            IN, OUT = forward(pre, want, edges)
            pruned = {}
            for t, keys in want.items():
                keep = keys & frozenset().union(*[OUT.get(p, frozenset()) for p in preds[t]])
                if keep:
                    pruned[t] = keep
            if pruned == want:
                break
            want = pruned
        if not want:
            return forward(pre)[0]
        for (a, t) in edges:
            if t in want:
                keys = IN.get(t, frozenset()) - OUT.get(a, frozenset())
                if keys:
                    self.slot_fix[(a, t)] = keys
        return IN

    def jump_label(self, a, t):
        """The label branch a jumps to for its target t: t's, or a stub's that looks up the pointers of the edge first."""
        keys = self.slot_fix.get((a, t))
        if not keys:
            return self.L(t)
        label = "LE_%04X_%04X" % (a, t)
        self.fix_stubs.append((label, keys, t))
        self.fix_done.add((a, t, "jump"))
        return label

    def check_edge_lookups(self):
        """Every edge lookup edge_lookups counted on was emitted."""
        fn = self.fn
        for (a, t) in self.slot_fix:
            ins = fn.instrs[a]
            if t == ins.next and ins.op not in (OP_B, OP_RET, OP_EXIT) and (a, t, "fall") not in self.fix_done:
                raise Reject("internal: lookups on the edge %#x -> %#x not emitted" % (a, t))
            if ins.op in (OP_B, OP_BT, OP_BF) and t == ins.target() and (a, t, "jump") not in self.fix_done:
                raise Reject("internal: lookups on the jump %#x -> %#x not emitted" % (a, t))

    def stub_lines(self):
        """The edge-lookup stubs, for the end of the function."""
        out = []
        for label, keys, t in self.fix_stubs:
            out += ["%s:" % label, "    %s goto %s;" % (self.refind(keys, cold=False), self.L(t))]
        return out

    def slot_name(self, key):
        if key not in self.cslots:
            self.cslots[key] = "aotc_%d" % len(self.cslots)
        return self.cslots[key]

    def slot_ptr(self, scope, var_id):
        """The pointer for a cached access, after emitting its lookup when it isn't good here."""
        key = (scope, var_id)
        p = self.slot_name(key)
        if key not in self.cur_in:
            self.emit("AOT_CFIND(%s, %d, %d);" % (p, scope, var_id))
        return p

    def refind(self, keys, cold=True):
        """Looks up again the pointers of keys, after something may have inserted a variable (a write of one that
        wasn't there, a struct member). A cold one (after an insert) is a placeholder until the function's pointers
        are all known (finish_refinds)."""
        if cold and keys:
            for k in sorted(keys):
                self.slot_name(k)
            self.refinds.append(frozenset(keys))
            return "AOT_REFIND_%d;" % (len(self.refinds) - 1)
        return " ".join("AOT_CFIND(%s, %d, %d);" % (self.slot_name(k), k[0], k[1]) for k in sorted(keys)) or "(void) 0"

    def slow(self, site, call, ins, d, dA, var=None):
        """A slow path: call (an interpreter handler) with the stack synced to depth d, then on in the interpreter from
        the next instruction unless the stack is at dA with no exception pending. A call of aotSite<site> with a site
        word (AOT_SITE in vm_aot.h), and the variable (var, AOT_SITE_V) for the sites whose fast path needs only that;
        or spelled out when sites are off or the word can't hold this one."""
        a, size = ins.addr, ins.size
        if SITES and a % 4 == 0 and a < (1 << 26) and size in (4, 8, 12, 16) and -8 <= dA - d <= 7:
            if var is not None:
                return "AOT_SITE_V(aotSite%s, 0x%X, %d, %d, %d, %s)" % (site, a, size, d, dA, var)
            return "AOT_SITE(aotSite%s, 0x%X, %d, %d, %d)" % (site, a, size, d, dA)
        return "AOT_SYNC(%d); %s; AOT_CHECK(%d, 0x%X)" % (d, call, dA, ins.next)

    @staticmethod
    def self_builtin(d, v, slow):
        """A read of a builtin variable on self: AOT_SELF_FIELD for those pushSelfBuiltinVar reads straight from the
        instance, with the slow path (slow) for a struct or no instance; slow itself for the rest."""
        field = SELF_FIELDS.get(v["name"])
        return "AOT_SELF_FIELD(%d, %s, %s)" % (d, field, slow) if field else slow

    def slow_popvar(self, call, ins, d, dA, inst, t2):
        """Pop.v.v's slow path (aotPopVarSlow): AOT_SITE_POPVAR with the operand, the instance type's code and type2 in
        the word, or as slow() spells it out."""
        a = ins.addr
        if SITES and a % 4 == 0 and a < (1 << 26) and ins.size == 8 and dA == d - 1 and 0 <= t2 < 16:
            return "AOT_SITE_POPVAR(0x%X, %d, %d, %d, 0x%08Xu)" % (a, d, {SELF: 0, OTHER: 1, GLOBAL: 2}.get(inst, 3), t2, ins.extra)
        return "AOT_SYNC(%d); %s; AOT_CHECK(%d, 0x%X)" % (d, call, dA, ins.next)

    def alias_refind(self, scope, var_id):
        """After an insert of (scope, var_id) that didn't grow the map: the good pointers of the same variable in the
        other scopes, which may be the same instance, so that one that was nullptr is looked up again."""
        keys = [k for k in self.cur_in if k[1] == var_id and k[0] != scope]
        return " ".join("AOT_CFIND(%s, %d, %d);" % (self.slot_name(k), k[0], k[1]) for k in sorted(keys)) or "(void) 0"

    def finish_refinds(self, body):
        """Fills in the cold refind placeholders of body (lines): each site's lookups inline, or, when that is
        smaller, a jump to one block that looks up every pointer again and jumps back. Returns (body, lines to add
        at the end of the function, after its return)."""
        sites = len(self.refinds)
        if not sites:
            return body, []
        # Roughly in ARM instructions: a lookup is 5; a jump there is 2 and a word in the switch's table.
        inline = 5 * sum(len(keys) for keys in self.refinds)
        shared = 3 * sites + 5 * len(self.cslots) + 6
        text = "\n".join(body)
        if inline <= shared:
            text = re.sub(r"AOT_REFIND_(\d+);", lambda m: self.refind(self.refinds[int(m.group(1))], cold=False), text)
            return text.split("\n"), []
        text = re.sub(r"AOT_REFIND_(\d+);", lambda m: "{ aotRs_ = %s; goto aot_refind_; aot_rs_%s: ; }" % (m.group(1), m.group(1)), text)
        keys = sorted(self.cslots, key=lambda key: int(self.cslots[key][5:]))
        tail = ["aot_refind_:", "    " + self.refind(keys, cold=False), "    switch (aotRs_) {"]
        tail += ["    case %d: goto aot_rs_%d;" % (i, i) for i in range(sites)]
        tail += ["    default: __builtin_unreachable();", "    }"]
        return ["    uint32_t aotRs_ = 0;"] + text.split("\n"), tail

    def fuse_constant(self, ins, d):
        """A number constant pushed straight into Add/Sub/Mul/Cmp: emits the fused operation and returns the
        addresses it covers, or None."""
        fn = self.fn
        k = self.constant(ins)
        n = ins.next
        if k is None or n not in fn.depth or n in fn.labels or n in fn.stubs or n not in fn.instrs:
            return None
        nxt = fn.instrs[n]
        kv, kslot, kint = k
        e = self.emit
        if nxt.op in (OP_ADD, OP_SUB, OP_MUL):
            name = {OP_ADD: "ADD", OP_SUB: "SUB", OP_MUL: "MUL"}[nxt.op]
            e("/* %04X %04X */" % (ins.addr, n))
            if kint is not None:
                e("AOT_%s_K(%d, %d, %d, %d);" % (name, d, kint, T_INT16 if ins.op == OP_PUSHI or ins.t1 == T_INT16 else T_INT32, nxt.t2))
            else:
                e("AOT_%s_R(%d, %s, %d);" % (name, d, kv, nxt.t2))
            return [ins.addr, n]
        if nxt.op == OP_CMP:
            W = "0x%08Xu" % nxt.word
            if n in fn.fused_cmp:
                br = fn.instrs[nxt.next]
                e("/* %04X %04X %04X */" % (ins.addr, n, br.addr))
                e("AOT_CMP_K_BRANCH(%d, %s, %d, %s, %s, %s, %s);" % (d, W, (nxt.word >> 8) & 0xFF, kv, kslot,
                                                                  "true" if br.op == OP_BT else "false",
                                                                  self.jump_label(br.addr, br.target())))
                return [ins.addr, n, br.addr]
            e("/* %04X %04X */" % (ins.addr, n))
            e("AOT_CMP_K(%d, %s, %d, %s, %s);" % (d, W, (nxt.word >> 8) & 0xFF, kv, kslot))
            return [ins.addr, n]
        return None

    def gen(self):
        fn = self.fn
        self.analyse_slots()
        covered = set()   # instructions emitted together with an earlier one
        for addr in sorted(fn.instrs):
            if addr not in fn.depth:
                continue
            ins = fn.instrs[addr]
            d = fn.depth[addr]
            self.cur_in = self.slot_in.get(addr, frozenset())
            typed_r = self.typed.get(addr) if self.typed else None
            if self.typed_exits and addr in self.typed_exits:
                self.lines.append("aoty_%04X: ;" % addr)
            if typed_r is not None and typed_r.site == typed_r.lo and not typed_r.straight:   # before the head, where the code above falls in
                self.lines.extend(typed_mod.call_site(self, typed_r))
            if addr in fn.labels:
                self.lines.append("%s:" % self.L(addr))
                self.emit("AOT_BLOCK(%d, 0x%X);" % (d, addr))
            if typed_r is not None and typed_r.straight and addr in covered:
                raise Reject("internal: the typed region at %#x is emitted with the instruction before it" % addr)
            if typed_r is not None and (typed_r.site != typed_r.lo or typed_r.straight):   # before the branch into the loop, or a straight region's first instruction
                self.lines.extend(typed_mod.call_site(self, typed_r))
            if addr in covered or (addr - 4 in fn.fused_cmp and addr - 4 not in covered
                                   and fn.instrs.get(addr - 4) is not None and fn.instrs[addr - 4].next == addr):
                pass   # emitted with the instruction before it
            elif addr in fn.stubs:
                self.emit("/* %04X */ AOT_DEOPT(%d, 0x%X);  /* %s */" % (addr, d, addr, fn.stubs[addr].replace("*/", "* /")))
                continue
            else:
                group = self.fuse_constant(ins, d)
                if group is not None:
                    covered.update(group[1:])
                else:
                    self.gen_instr(ins, d, fn.stacks[addr])
            if addr in self.slot_pre:
                # Ahead of the loop whose head comes next (with nothing in between: see analyse_slots).
                self.emit(self.refind(self.slot_pre[addr], cold=False))
            if ins.op not in (OP_B, OP_RET, OP_EXIT) and (addr, ins.next) in self.slot_fix:
                # On the fall-through into a merge point (edge_lookups).
                self.emit(self.refind(self.slot_fix[(addr, ins.next)], cold=False))
                self.fix_done.add((addr, ins.next, "fall"))
            # Fallthrough off the end of the code returns like Exit.
            if ins.next == len(fn.blob) and ins.op not in (OP_RET, OP_EXIT, OP_B):
                self.emit("AOT_EXIT(%d);" % fn.after[addr])
        end = len(fn.blob)
        if end in fn.labels:
            # A branch to the end of the code returns like Exit.
            self.lines.append("%s:" % self.L(end))
            self.emit("AOT_EXIT(%d);" % fn.depth[end])
        self.check_edge_lookups()
        return self.lines

    def gen_instr(self, ins, d, stack):
        fn = self.fn
        op, t1, t2, inst, a, n = ins.op, ins.t1, ins.t2, ins.inst, ins.addr, ins.next
        W = "0x%08Xu" % ins.word
        dA = fn.after[a]
        e = self.emit
        e("/* %04X */" % a)
        if op == OP_PUSH:
            if t1 == T_INT16:
                e("AOT_PUSH_INT(%d, %d, %d);" % (d, inst, T_INT16))
            elif t1 == T_INT32:
                e("AOT_PUSH_INT(%d, %d, %d);" % (d, struct.unpack("<i", struct.pack("<I", ins.extra))[0], T_INT32))
            elif t1 == T_DOUBLE:
                bits = struct.unpack_from("<Q", fn.blob, a + 4)[0]
                e("AOT_PUSH_REAL(%d, (GMLReal) aotBitsDouble(0x%016XULL), %d);" % (d, bits, T_DOUBLE))
            elif t1 == T_FLOAT:
                e("AOT_PUSH_REAL(%d, (GMLReal) aotBitsFloat(0x%08Xu), %d);" % (d, ins.extra, T_FLOAT))
            elif t1 == T_INT64:
                v = struct.unpack_from("<q", fn.blob, a + 4)[0]
                e("AOT_SYNC(%d); aotx_stackPushTyped(ctx, RValue_makeInt64((int64_t) %dLL), %d);" % (d, v, T_INT64))
            elif t1 == T_BOOL:
                e("AOT_SYNC(%d); aotx_stackPushTyped(ctx, RValue_makeBool(%s), %d);" % (d, "true" if ins.extra != 0 else "false", T_BOOL))
            elif t1 == T_STRING:
                e("AOT_PUSH_STRING(%d, %d);" % (d, ins.extra))
            else:
                self.gen_push_var(ins, d, W, dA)
            return
        if op == OP_PUSHLOC:
            idx, v = self.var_ref(ins)
            vt = ins.var_type()
            lv = self.local(idx)
            if vt in (VT_PUSHAF, VT_POPAF):
                e("AOT_SYNC(%d); aotx_pushTopLevelArrayRef(ctx, &%s, %s);" % (d, lv, "true" if vt == VT_POPAF else "false"))
            else:
                e("AOT_WEAK_COPY(%d, &%s);" % (d, lv))
            return
        if op == OP_PUSHGLB:
            idx, v = self.var_ref(ins)
            if SLOTS and ins.var_type() == VT_NORMAL and v["varID"] >= 0 and self.slot_cached(a, ("read", GLOBAL, v["varID"]), self.cur_in):
                p = self.slot_ptr(GLOBAL, v["varID"])
                e("AOT_CREAD(%d, %s, %d, %d, %s);  /* global.%s */"
                  % (d, p, GLOBAL, v["varID"], self.slow("PushGlb", "aotPushGlobalSlow(ctx, 0x%08Xu)" % ins.extra, ins, d, dA),
                     v["name"].replace("*/", "* /")))
            elif ins.var_type() == VT_NORMAL and v["varID"] >= 0:
                e("AOT_PUSH_VAR(%d, %d, %d, %s);" % (d, GLOBAL, idx, self.slow("PushGlb", "aotPushGlobalSlow(ctx, 0x%08Xu)" % ins.extra, ins, d, dA)))
            else:
                e("%s;" % self.slow("PushGlb", "aotPushGlobalSlow(ctx, 0x%08Xu)" % ins.extra, ins, d, dA))
            return
        if op == OP_PUSHBLTN:
            idx, v = self.var_ref(ins)
            if ins.var_type() == VT_NORMAL and inst == SELF and v["varID"] == VARIABLE_BUILTIN and self.arg_number(v) is not None:
                e("AOT_PUSH_ARG(%d, %d);  /* %s */" % (d, self.arg_number(v), v["name"]))
            elif ins.var_type() == VT_NORMAL and inst == SELF and v["varID"] == VARIABLE_BUILTIN:
                e("%s;  /* %s */" % (self.self_builtin(d, v, self.slow("SelfBltn", "if (!aotx_pushSelfBuiltinVar(ctx, &V[%d])) aotx_handlePushBltn(ctx, %s, BC + 0x%X)"
                                                                         % (idx, W, a + 4), ins, d, dA, "&V[%d]" % idx)), v["name"]))
            else:
                e("%s;" % self.slow("PushBltn", "aotx_handlePushBltn(ctx, %s, BC + 0x%X)" % (W, a + 4), ins, d, dA))
            return
        if op == OP_PUSHI:
            e("AOT_PUSH_INT(%d, %d, %d);" % (d, inst, T_INT16))
            return
        if op == OP_POP:
            self.gen_pop(ins, d, W, dA)
            return
        if op == OP_POPZ:
            e("AOT_POPZ(%d);" % (d - 1))
            return
        if op == OP_ADD:
            e("AOT_ADD(%d, %d);" % (d, t2))
            return
        if op == OP_SUB:
            e("AOT_SUB(%d, %d);" % (d, t2))
            return
        if op == OP_MUL:
            e("AOT_MUL(%d, %d);" % (d, t2))
            return
        if op == OP_DIV:
            int_div = t1 in (T_INT32, T_INT64) and t2 in (T_INT32, T_INT64)
            e("AOT_DIV(%d, %s, %d, %d);" % (d, W, t2, 1 if int_div else 0))
            return
        if INLINE and op == OP_MOD:
            e("AOT_MOD(%d, %s, %d);" % (d, W, t2))
            return
        if INLINE and op == OP_NOT:
            e("AOT_NOT(%d, %s, %d);" % (d, W, t1))
            return
        if INLINE and op == OP_NEG:
            e("AOT_NEG(%d, %s, %d);" % (d, W, t1))
            return
        if op in HANDLER_NAMES:
            e("AOT_HANDLER(%d, %s(ctx, %s), %d);" % (d, HANDLER_NAMES[op], W, dA))
            return
        if op == OP_CONV:
            e("AOT_CONV(%d, %d, %d);" % (d, t1, t2))
            return
        if op == OP_CMP:
            if a in fn.fused_cmp:
                br = fn.instrs[n]
                e("AOT_CMP_BRANCH(%d, %s, %d, %s, %s);" % (d, W, (ins.word >> 8) & 0xFF, "true" if br.op == OP_BT else "false",
                                                        self.jump_label(br.addr, br.target())))
            else:
                e("AOT_CMP(%d, %s, %d);" % (d, W, (ins.word >> 8) & 0xFF))
            return
        if op == OP_DUP:
            self.gen_dup(ins, d, W, dA)
            return
        if op == OP_B:
            e("goto %s;" % self.jump_label(a, ins.target()))
            return
        if op in (OP_BT, OP_BF):
            e("if (aotTruth(&S[%d]) == %s) goto %s;" % (d - 1, "true" if op == OP_BT else "false", self.jump_label(a, ins.target())))
            return
        if op == OP_CALL:
            self.funcs[ins.extra] = True
            name = self.dw.funcs[ins.extra]["name"].replace("*/", "* /")
            if self.script_callee(ins.extra):
                e("AOT_CALL_SCRIPT(%d, %s, 0x%X, %d, %d);  /* %s */" % (d, W, a, dA, ins.extra, name))
            elif DIRECT and builtin_audit().get(self.dw.funcs[ins.extra]["name"], 0) is None and not self.dw.funcs[ins.extra]["name"].startswith("@@"):
                # (not the @@ helpers: @@try_hook@@ records the stack depth, which has the arguments on it here)
                math = inline_math().get(self.dw.funcs[ins.extra]["name"]) if INLINE else None
                if math is not None and math[0] == (ins.word & 0xFFFF):
                    e("AOT_MATH%d(%d, %s, 0x%X, %d, %d, %s);  /* %s */" % (math[0], d, W, a, dA, ins.extra, math[1], name))
                else:
                    call = "AOT_CALL_BUILTIN"
                    # One effect only: preserve the ordinary call ABI and all
                    # surrounding generated instructions. The runtime checks
                    # the resolved builtin and numeric arguments before use.
                    if (LAVA_PART_DIRECT and INLINE and self.dw.wad == 16 and
                            fn.entry["name"] == "gml_Script_scr_true_lavawaver" and
                            name == "draw_sprite_part" and (ins.word & 0xFFFF) == 8):
                        from builtin_audit import builtin_body_hashes
                        if builtin_body_hashes().get(name) == "5ad4236a564f":
                            call = "AOT_CALL_LAVA_PART"
                    e("%s(%d, %s, 0x%X, %d, %d);  /* %s */" % (call, d, W, a, dA, ins.extra, name))
            else:
                e("AOT_CALL(%d, %s, 0x%X, %d);  /* %s */" % (d, W, a, dA, name))
            return
        if op == OP_CALLV:
            e("AOT_CALLV(%d, %s, 0x%X, %d);" % (d, W, a, d - (ins.word & 0xFFFF) - 1))
            return
        if op == OP_RET:
            e("AOT_RET(%d);" % d)
            return
        if op == OP_EXIT:
            e("AOT_EXIT(%d);" % d)
            return
        if op == OP_PUSHENV:
            t = ins.target()
            e("AOT_PUSHENV(%d, %s, 0x%X, %d, %s, 0x%X);" % (d, W, a, fn.depth[t], self.L(t), t))
            return
        if op == OP_POPENV:
            if (ins.word & 0x00FFFFFF) == 0xF00000:
                e("AOT_POPENV_EXIT(%d, %s, 0x%X);" % (d, W, a))
            else:
                t = ins.target()
                e("AOT_POPENV(%d, %s, 0x%X, %s, 0x%X);" % (d, W, a, self.L(t), t))
            return
        if op == OP_BREAK:
            if self.dw.wad < 17:
                e("/* break: nothing in WAD %d */" % self.dw.wad)   # WAD16
                return
            if inst == -8:
                # handleBreakSaveARef only changes this per-call-frame balance.
                # Keep it in VMContext so slow handlers and deopts see the same value.
                e("++ctx->savearefBalance;")
                return
            if inst == -11 and ins.extra is not None and (ins.extra >> 24) == 5:
                self.funcs[ins.extra & 0xFFFFFF] = True
            brk = self.slow("BreakPopAF" if inst == -3 and ARRAY_WRITES else "Break",
                            "aotx_handleBreak(ctx, %s, 0x%X, BC + 0x%X)" % (W, a, a + 4), ins, d, dA)
            if inst == -9:
                # Retain the original assertion (and its synced stack) on invalid
                # bytecode; a valid restore touches neither stack nor ownership.
                e("if (ctx->savearefBalance > 0) --ctx->savearefBalance; else { %s; }" % brk)
            elif inst == -5:
                e("if (S[%d].type == RVALUE_INT32) ctx->currentArrayOwner = (void*) (intptr_t) S[%d].int32; else { %s; }"
                  % (d - 1, d - 1, brk))
            elif inst == -2 and ARRAY_REFS:
                e("AOT_BREAK_PUSHAF(%d, %s, %s);" % (d, brk, self.refind(self.cur_in)))
            else:
                e("%s;" % brk)
            return
        raise Reject("cannot emit opcode %#x" % op)

    def gen_push_var(self, ins, d, W, dA):
        e = self.emit
        idx, v = self.var_ref(ins)
        vt = ins.var_type()
        inst, a, n = ins.inst, ins.addr, ins.next
        call = "aotx_handlePush(ctx, %s, BC + 0x%X, 5)" % (W, a + 4)
        push = self.slow("Push", call, ins, d, dA)
        if vt == VT_NORMAL:
            if self.dw.wad < 17 and inst in (ARG, STACKTOP):
                e("%s;" % push)   # WAD16: the WAD 17 forms don't apply
            elif inst == ARG and self.arg_number(v) is not None:
                e("AOT_PUSH_ARG(%d, %d);  /* %s */" % (d, self.arg_number(v), v["name"]))
            elif inst == ARG:
                e("%s;" % self.slow("NamedArg", "if (!aotPushNamedArg(ctx, &V[%d])) %s" % (idx, call), ins, d, dA, "&V[%d]" % idx))
            elif v["varID"] >= 0 and inst in (SELF, OTHER, GLOBAL) and SLOTS and self.slot_cached(a, ("read", inst, v["varID"]), self.cur_in):
                p = self.slot_ptr(inst, v["varID"])
                e("AOT_CREAD(%d, %s, %d, %d, %s);  /* %s */" % (d, p, inst, v["varID"], push, v["name"].replace("*/", "* /")))
            elif v["varID"] >= 0 and inst in (SELF, OTHER, GLOBAL):
                e("AOT_PUSH_VAR(%d, %d, %d, %s);  /* %s */" % (d, inst, idx, push, v["name"].replace("*/", "* /")))
            elif v["varID"] >= 0 and inst == LOCAL:
                e("AOT_WEAK_COPY(%d, &%s);" % (d, self.local(idx)))
            elif v["varID"] == VARIABLE_BUILTIN and inst == SELF and self.arg_number(v) is not None:
                e("AOT_PUSH_ARG(%d, %d);  /* %s */" % (d, self.arg_number(v), v["name"]))
            elif v["varID"] == VARIABLE_BUILTIN and inst == SELF:
                e("%s;  /* %s */" % (self.self_builtin(d, v, self.slow("SelfBltn", "if (!aotx_pushSelfBuiltinVar(ctx, &V[%d])) %s" % (idx, call), ins, d, dA,
                                                                         "&V[%d]" % idx)), v["name"]))
            elif inst == STACKTOP:
                e("%s;" % self.slow("Member1", "if (!aotx_tryPushStructMember(ctx, &V[%d], 1)) %s" % (idx, call), ins, d, dA, "&V[%d]" % idx))
            else:
                e("%s;" % push)
        elif vt == VT_ARRAY and self.slot_kind(ins, self.fn.stacks[a])[0] == "aread":
            scope = self.fn.stacks[a][-2][0]
            p = self.slot_ptr(scope, v["varID"])
            e("AOT_CAREAD(%d, %s, %d, %d, %s);  /* %s[] */"
              % (d, p, scope, v["varID"], self.slow("PushArray", "if (!aotx_tryPushArrayElement(ctx, &V[%d])) %s" % (idx, call), ins, d, dA, "&V[%d]" % idx),
                 v["name"].replace("*/", "* /")))
        elif vt == VT_PUSHAF and self.slot_kind(ins, self.fn.stacks[a])[0] == "afread":
            scope = self.fn.stacks[a][-2][0]
            p = self.slot_ptr(scope, v["varID"])
            e("AOT_PUSHAF(%d, %s, %d, %d, %s, %s);  /* %s[][] */"
              % (d, p, scope, v["varID"], push, self.refind(self.cur_in | {(scope, v["varID"])}), v["name"].replace("*/", "* /")))
        elif vt == VT_POPAF and ARRAY_WRITES and self.dw.wad >= 17:
            # The site function reads the variable from the bytecode, as aotSitePush would: passing it (AOT_SITE_V)
            # would make every such site bigger.
            e("%s;" % self.slow("PopAF", call, ins, d, dA))
        elif vt == VT_ARRAY:
            e("%s;" % self.slow("PushArray", "if (!aotx_tryPushArrayElement(ctx, &V[%d])) %s" % (idx, call), ins, d, dA, "&V[%d]" % idx))
        elif vt == VT_STACKTOP and self.dw.wad >= 17:   # WAD16: no struct members
            e("%s;" % self.slow("Member2", "if (!aotx_tryPushStructMember(ctx, &V[%d], 2)) %s" % (idx, call), ins, d, dA, "&V[%d]" % idx))
        else:
            e("%s;" % push)

    def gen_pop(self, ins, d, W, dA):
        e = self.emit
        idx, v = self.var_ref(ins)
        vt = ins.var_type()
        t1, t2, inst, a, n = ins.t1, ins.t2, ins.inst, ins.addr, ins.next
        if t1 == T_VAR and vt == VT_NORMAL:
            pop = self.slow_popvar("aotPopVarSlow(ctx, %d, 0x%08Xu, %d)" % (inst, ins.extra, t2), ins, d, dA, inst, t2)
            if v["varID"] >= 0 and inst in (SELF, OTHER, GLOBAL) and SLOTS and self.slot_cached(a, ("write", inst, v["varID"]), self.cur_in):
                p = self.slot_ptr(inst, v["varID"])
                e("AOT_CSTORE(%d, %s, %d, %d, %d, %s, %s, %s);  /* %s */"
                  % (d, p, inst, v["varID"], t2, pop, self.alias_refind(inst, v["varID"]),
                     self.refind(self.cur_in | {(inst, v["varID"])}), v["name"].replace("*/", "* /")))
            elif v["varID"] >= 0 and inst in (SELF, OTHER, GLOBAL):
                e("AOT_POP_VAR(%d, %d, %d, %d, %s);  /* %s */" % (d, inst, idx, t2, pop, v["name"].replace("*/", "* /")))
            elif v["varID"] >= 0 and inst == LOCAL:
                e("AOT_STORE(&%s, %d, %d);" % (self.local(idx), d, t2))
            else:
                e("%s;  /* %s */" % (pop, v["name"].replace("*/", "* /")))
                self.refind_after_builtin_write(ins, dA)
            return
        instance_type = inst + 100000 if vt == VT_INSTANCE else inst
        call = "aotx_handlePop(ctx, %d, %d, 0x%08Xu, 0x%02X, %d)" % (t1, t2, ins.extra, vt, instance_type)
        kind = self.slot_kind(ins, self.fn.stacks[a])
        if vt == VT_ARRAY and kind[0] == "awrite":
            scope = kind[1]
            p = self.slot_ptr(scope, v["varID"])
            ix, sx = (d - 1, d - 3) if t1 == T_VAR else (d - 2, d - 1)
            e("AOT_CAWRITE(%d, %s, %d, %d, %d, %d, %d, %s, %s, %s);  /* %s[] */"
              % (d, p, scope, v["varID"], ix, sx, t2, "false" if t1 == T_VAR else "true", self.slow("Pop", call, ins, d, dA),
                 self.refind(self.cur_in | {(scope, v["varID"])}), v["name"].replace("*/", "* /")))
        elif vt == VT_ARRAY:
            e("%s;" % self.slow("PopArray", "if (!aotx_tryPopArrayElement(ctx, &V[%d], %d, %s)) %s"
                                % (idx, t2, "false" if t1 == T_VAR else "true", call), ins, d, dA))
            self.refind_after_builtin_write(ins, dA)
        else:
            e("%s;" % self.slow("Pop", call, ins, d, dA))

    def refind_after_builtin_write(self, ins, dA):
        """After a builtin variable write the slot analysis lets pointers survive: a struct's builtin-named members are
        its own variables (WAD 17), so on a struct self it may have inserted one, and the good pointers are looked up
        again."""
        if self.dw.wad >= 17 and self.cur_in and self.slot_kind(ins, self.fn.stacks[ins.addr]) is PURE:
            self.emit("if (__builtin_expect(aotSelfIsStruct(ctx), 0)) { %s }" % self.refind(self.cur_in))

    def gen_dup(self, ins, d, W, dA):
        e = self.emit
        fn = self.fn
        a, n = ins.addr, ins.next
        operand = ins.word & 0xFFFF
        if (ins.word & 0xFFFFF800) == 0x86058800 and self.dw.wad >= 17:   # WAD16: a plain Dup
            moved = ins.word & 0x7FF
            if moved <= 8 and d > moved:
                if a in fn.fused_targets:
                    # Push.v stacktop.<method> is the second of the four instructions after the swap; its operand
                    # names the method. A plain variable with a varID needs no bytecode at run time.
                    ref = struct.unpack_from("<I", fn.blob, n + 8)[0]
                    mv = self.dw.vars[ref & 0x07FFFFFF]
                    if moved <= 1 and ((ref >> 24) & 0xF8) == VT_NORMAL and mv["varID"] >= 0:
                        e("AOT_DUP_SWAP_CALL_K(%d, %d, %d, %s, 0x%X);  /* .%s() */"
                          % (d, moved, mv["varID"], self.L(fn.fused_targets[a]), fn.fused_targets[a],
                             mv["name"].replace("*/", "* /")))
                    else:
                        e("AOT_DUP_SWAP_CALL(%d, %s, %d, 0x%X, %s);" % (d, W, moved, a, self.L(fn.fused_targets[a])))
                else:
                    # tryFusedAccessorCall cannot match here (the next instructions aren't a getter/setter call).
                    e("{ RValue below_ = S[%d]; for (int32_t i_ = 0; %d > i_; i_++) S[%d + i_] = S[%d + i_]; S[%d] = below_; }"
                      % (d - 1 - moved, moved, d - 1 - moved, d - moved, d - 1))
                return
        call = "aotx_handleDup(ctx, %s)" % W
        if operand & 0x8000 and self.dw.wad >= 17:
            e("%s;" % self.slow("Dup", call, ins, d, d))
            return
        count = fn.dup_slots[a]
        if (ins.word & 0xFF00) == 0 and count <= 4 and count == (operand & 0xFF) + 1:
            e("AOT_DUP_PLAIN(%d, %d, %d, %s);" % (d, TYPE_NATIVE_SIZE.get(ins.t1, 16), count, self.slow("Dup", call, ins, d, d + count)))
            return
        e("%s;" % self.slow("Dup", call, ins, d, d + count))


# ---------------------------------------------------------------------------------------------------------
# Selection from profiler logs

TARGET_BUDGET_MB = {"o3ds": 4.0, "n3ds": 12.0}


def profile_ops(path):
    """The GML instructions a frame of each code entry, averaged over the profiler windows after "Bench: measuring"
    (or over all of them). Builtins appear with 0 instructions and drop out."""
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    measuring = any(l.startswith("Bench: measuring") for l in lines)
    active = not measuring
    agg = {}
    windows = 0
    cur = None
    for l in lines:
        if l.startswith("Bench: measuring"):
            active = True
        if l.startswith("GML Profiler"):
            cur = {} if active else None
            windows += 1 if active else 0
            continue
        if cur is not None:
            m = re.match(r"([\d.]+)ms (\d+) ops \((\d+) ns/op\) (\S+)", l)
            if m:
                if int(m.group(2)):
                    agg[m.group(4)] = agg.get(m.group(4), 0) + int(m.group(2))
            elif l.startswith("total"):
                cur = None
    return {k: v / max(windows, 1) for k, v in agg.items()}


def entry_size(dw, name):
    """Static bytecode instructions the generated function for name would have, or None when it can't be generated."""
    entry = dw.code_by_name.get(name)
    if entry is None:
        return None
    blob = bytes(dw.data[entry["abs"]:entry["abs"] + entry["length"]])
    try:
        return len(Function(dw, entry, blob).depth)
    except Exception:
        return None


def rank_by_gain(profiles, sizes, weights=None):
    """profiles: [(game key, {name: ops a frame})]; sizes: {(game key, name): instructions}; weights: one per profile
    (default 1). Returns the (game key, name) pairs that can be generated, best gain per instruction of code first,
    the gain of an entry being its weighted share of each profile's instructions."""
    gain = {}
    for i, (g, ops) in enumerate(profiles):
        total = sum(ops.values()) or 1.0
        w = weights[i] if weights else 1.0
        for name, v in ops.items():
            key = (g, name)
            if sizes.get(key):
                gain[key] = gain.get(key, 0.0) + w * v / total
    return sorted(gain, key=lambda key: (-gain[key] / sizes[key], key))


def coverage(profiles, chosen):
    """Per profile, the share of its instructions that run in the chosen entries."""
    out = []
    for g, ops in profiles:
        total = sum(ops.values()) or 1.0
        out.append(sum(v for name, v in ops.items() if (g, name) in chosen) / total)
    return out


def names_from_profile(path, cover, max_n):
    agg = profile_ops(path)
    total = sum(agg.values()) or 1
    out = []
    cum = 0
    for name, ops in sorted(agg.items(), key=lambda kv: -kv[1]):
        if cum * 100.0 / total >= cover or len(out) >= max_n:
            break
        out.append(name)
        cum += ops
    return out, cum * 100.0 / total


# ---------------------------------------------------------------------------------------------------------

def parse_args(args):
    """<out_dir> then one group per game: --game <data.win> followed by its --names / --profile options."""
    out_dir = args[0]
    groups = []
    opts = {"cover": 95.0, "max": 40, "chunk": 4000, "budget": None, "bytes": 85.0, "corpus_weight": {}, "max_size": 20000, "no_native": False, "scene_weight": "share"}
    i = 1
    while i < len(args):
        a = args[i]
        if a == "--no-native":
            opts["no_native"] = True
            i += 1
            continue
        if a in ("--selection", "--data", "--layout"):
            if i + 1 >= len(args):
                raise SystemExit("%s takes a value" % a)
            opts[a[2:]] = args[i + 1]
            i += 2
            continue
        if a == "--no-direct":
            global DIRECT
            DIRECT = False
            i += 1
            continue
        if a == "--no-slots":
            global SLOTS
            SLOTS = False
            i += 1
            continue
        if a == "--no-sites":
            global SITES
            SITES = False
            i += 1
            continue
        if a == "--no-late-copies":
            LATE_COPIES.clear()
            i += 1
            continue
        if a == "--no-escape":
            global ESCAPE
            ESCAPE = False
            i += 1
            continue
        if a == "--no-utflames":
            global UTFLAMES
            UTFLAMES = False
            i += 1
            continue
        if a == "--no-inline":
            global INLINE
            INLINE = False
            i += 1
            continue
        if a == "--no-edge-lookups":
            global EDGE_LOOKUPS
            EDGE_LOOKUPS = False
            i += 1
            continue
        if a == "--no-array-refs":
            global ARRAY_REFS
            ARRAY_REFS = False
            i += 1
            continue
        if a == "--no-array-writes":
            global ARRAY_WRITES
            ARRAY_WRITES = False
            i += 1
            continue
        if a == "--typed":
            if i + 1 >= len(args):
                raise SystemExit("--typed takes a value")
            global TYPED, typed_mod
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            import typed as typed_mod
            typed_mod.bind(sys.modules[__name__])
            TYPED = typed_mod.load_selection(args[i + 1])
            i += 2
            continue
        if a == "--late-numeric":
            if i + 1 >= len(args) or not args[i + 1].startswith("gml_"):
                raise SystemExit("--late-numeric takes one code entry name (experimental, WAD16 only)")
            opts.setdefault("late_numeric", set()).add(args[i + 1])
            i += 2
            continue
        if a == "--shared-helpers":
            global SHARED_HELPERS
            SHARED_HELPERS = True
            i += 1
            continue
        if a in ("--corpus", "--corpus-weight"):
            if i + 2 >= len(args):
                raise SystemExit("%s takes a name and a value" % a)
            if a == "--corpus-weight":
                opts["corpus_weight"][args[i + 1]] = float(args[i + 2])
            elif not groups:
                raise SystemExit("--corpus before --game")
            else:
                found = sorted(glob.glob(args[i + 2]))
                if not found:
                    raise SystemExit("--corpus %s: no file matches %s" % (args[i + 1], args[i + 2]))
                groups[-1]["corpora"].extend((args[i + 1], f) for f in found)
            i += 3
            continue
        if a == "--game":
            groups.append({"data_win": args[i + 1], "names": [], "profiles": [], "corpora": []})
        elif a in ("--names", "--profile") and not groups:
            raise SystemExit("%s before --game" % a)
        elif a == "--names":
            for l in open(args[i + 1], encoding="utf-8"):
                l = l.split("#")[0].strip()
                if l:
                    groups[-1]["names"].append(l)
        elif a == "--profile":
            groups[-1]["profiles"].append(args[i + 1])
        elif a == "--cover":
            opts["cover"] = float(args[i + 1])
        elif a == "--max":
            opts["max"] = int(args[i + 1])
        elif a == "--chunk":
            opts["chunk"] = int(args[i + 1])
        elif a == "--max-size":
            opts["max_size"] = int(args[i + 1])
        elif a == "--budget":
            opts["budget"] = float(args[i + 1])
        elif a == "--target":
            if args[i + 1] not in TARGET_BUDGET_MB:
                raise SystemExit("--target is one of " + ", ".join(sorted(TARGET_BUDGET_MB)))
            opts["budget"] = TARGET_BUDGET_MB[args[i + 1]]
        elif a == "--scene-weight":
            if args[i + 1] not in ("ops", "share"):
                raise SystemExit("--scene-weight is ops or share")
            opts["scene_weight"] = args[i + 1]
        elif a == "--bytes-per-instr":
            opts["bytes"] = float(args[i + 1])
        elif a == "--tag":
            opts["tag"] = args[i + 1]
        else:
            raise SystemExit("unknown argument " + a)
        i += 2
    if opts.get("selection"):
        # One group per data.win the list names, the entries in the list's order across all of them.
        if groups or not opts.get("data"):
            raise SystemExit("--selection needs --data and no --game")
        if opts["budget"] is None:
            raise SystemExit("--selection needs --target or --budget")
        index = {}
        opts["sel_order"] = []
        opts["sel_extra"] = set()
        for line in open(opts["selection"], encoding="utf-8"):
            f = line.split("#")[0].split()
            is_extra = len(f) == 3 and f[2] == "extra"
            if is_extra:
                f = f[:2]
            if len(f) != 2:
                continue
            if f[0] not in index:
                index[f[0]] = len(groups)
                folder = opts["data"] if f[0] == "root" else os.path.join(opts["data"], f[0])
                groups.append({"data_win": os.path.join(folder, "data.win"), "names": [], "profiles": [], "corpora": []})
            opts["sel_order"].append((index[f[0]], f[1]))
            if is_extra:
                opts["sel_extra"].add((index[f[0]], f[1]))
        # DELTARUNE's launcher (data.win in the game folder) has no entries in the list, but the engine's start-up
        # report should still know it as one of this game's data.win files.
        if opts.get("layout"):
            opts["layout_order"] = []
            for line in open(opts["layout"], encoding="utf-8"):
                f = line.split("#")[0].split()
                if len(f) == 2 and f[0] in index:
                    opts["layout_order"].append((index[f[0]], f[1]))
        launcher = os.path.join(opts["data"], "data.win")
        if "root" not in index and os.path.isfile(launcher):
            groups.append({"data_win": launcher, "names": [], "profiles": [], "corpora": []})
    elif opts.get("data") or opts.get("layout"):
        raise SystemExit("--data and --layout go with --selection")
    return out_dir, groups, opts


def escape_plan(dw, fn, k, name, em, report):
    """tools/aot/escape.py (G9): an Emitter that keeps the function's provably local structs in C, or None (WAD 16 never
    gets here: UNDERTALE has no constructors)."""
    global escape_mod
    if not ESCAPE or dw.wad < 17:
        return None
    if escape_mod is None:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import escape as escape_mod
        escape_mod.bind(sys.modules[__name__])
    return escape_mod.plan(dw, fn, k, name, em, report)


def late_copy(dw, k, func_text):
    """aot_late.c's copy of aotf_<k> (LATE_COPIES) and its count of inline writes, or None when no write in it is one
    of LATE_COPY_SETTERS. Where self isn't an instance, the copy runs aotf_<k>, which also keeps aotf_<k> linked."""
    lines, n = [], 0
    for line in func_text.splitlines():
        m = LATE_COPY_SITE.match(line)
        if m is not None:
            v = dw.vars[int(m.group(4), 16) & 0xFFFFFF]
            setter = LATE_COPY_SETTERS.get(v["name"])
            if v["varID"] == VARIABLE_BUILTIN and v["name"] == m.group(5) and setter is not None:
                line = "%s%s(%s, %s, %s);  /* %s */" % (m.group(1), setter[0], m.group(3), setter[1], m.group(2), v["name"])
                n += 1
        lines.append(line)
    head = "RValue aotf_%d(VMContext* ctx) {" % k
    if n == 0 or lines.count(head) != 1:
        return None
    i = lines.index(head)
    lines[i:i + 1] = ["RValue aotl_%d(VMContext* ctx) {" % k,
                      "    if (__builtin_expect(!aotLateSelfIsInstance(ctx), 0)) return aotf_%d(ctx);" % k]
    return ["RValue aotf_%d(VMContext* ctx) AOT_FN_DECL;" % k, "RValue aotl_%d(VMContext* ctx);" % k] + lines, n

# WAD 16 (UNDERTALE) only (utflames): code entries that engine/src/z_utflames.c has a native body for. Each is
# matched by its shape, every instruction word in order with what its operand names: a builtin variable or function
# by its name (with '-' for '_'), any other variable by the order it first appears in (v0, v1, ...), and any other
# operand by all its bytes. A match binds the entry to a small late wrapper (aotu_<k>) that calls the native body with
# the entry's generated function (aotf_<k>, which the body runs instead whenever it can't) and the operands listed
# after the shape: var:vN (that variable's ID), func:name (its function index), ref:ADDR (the operand word of the
# instruction at ADDR) and i16:ADDR (that PushI's value). The bytecode must be exactly this, so the body can't drift
# from what the game asks for, except that a token 840F**** is any PushI.i16 (the object a bullet tests, which the body
# takes from its i16 operand, so other data with that object at another index still matches). Only self variables
# with a non-negative ID are vN (a builtin by ID or instance type is named, and a negative ID refuses the entry).
#
# The fire bullets' shared tail: if (collision_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, <object>, 0, 1))
# event_user(0).
UTF_RECT = ("840F0001 07520000 840F0000 07520000 840F**** 07520000 C005FFFF.A0.bbox-bottom C005FFFF.A0.bbox-right "
            "C005FFFF.A0.bbox-top C005FFFF.A0.bbox-left D9020007.collision-rectangle 07450000 B8000006 840F0000 "
            "07520000 D9020001.event-user 9E050000")
UTF_NATIVE = [
    # Step of a fire bullet that moves on a sine wave (v0 += 1; x += sin(v0 / v1) * v2), is destroyed below the room
    # and tests its own box against one object (event_user(0) on a hit). Operands: the variables' IDs (v0, v1, v2),
    # collision_rectangle's function index and the object index it tests (the PushI at 0x90).
    ("UtFlames_sineStep",
     "C005FFFF.A0.v0 840F0001 0C520000 4555FFFF.A0.v0 C005FFFF.A0.x C005FFFF.A0.v0 C005FFFF.A0.v1 09550000 "
     "D9020001.sin C005FFFF.A0.v2 08550000 0C550000 4555FFFF.A0.x C005FFFF.A0.y C305FFFF.A0.room-height 840F0064 "
     "0C520000 15550600 B8000004 D9020000.instance-destroy 9E050000 840F0001 07520000 840F0000 07520000 840F**** "
     "07520000 C005FFFF.A0.bbox-bottom C005FFFF.A0.bbox-right C005FFFF.A0.bbox-top C005FFFF.A0.bbox-left "
     "D9020007.collision-rectangle 07450000 B8000006 840F0000 07520000 D9020001.event-user 9E050000",
     ("var:v0", "var:v1", "var:v2", "func:collision-rectangle", "i16:0x90")),
    # Step of a fire bullet that tests its own box (as above) and then slows down (speed -= v0). Operands:
    # collision_rectangle's function index, the object index (0x10), speed's operand (0x70), v0's ID.
    ("UtFlames_genericStep",
     UTF_RECT + " C005FFFF.A0.speed C005FFFF.A0.v0 0D550000 4555FFFF.A0.speed",
     ("func:collision-rectangle", "i16:0x10", "ref:0x70", "var:v0")),
    # Step of a fire bullet circling a point: v0 -= v1; v2 += v3; destroyed when v0 <= 0.5; x = v4 + lengthdir_x(v0,
    # v2); y = v5 + lengthdir_y(v0, v2); then the box test. Operands: v0..v5's IDs, collision_rectangle's function
    # index, the object index (0xC8).
    ("UtFlames_circleStep",
     "C005FFFF.A0.v0 C005FFFF.A0.v1 0D550000 4555FFFF.A0.v0 C005FFFF.A0.v2 C005FFFF.A0.v3 0C550000 4555FFFF.A0.v2 "
     "C005FFFF.A0.v0 C0000000.000000000000E03F 15500200 B8000004 D9020000.instance-destroy 9E050000 C005FFFF.A0.v4 "
     "C005FFFF.A0.v2 C005FFFF.A0.v0 D9020002.lengthdir-x 0C550000 4555FFFF.A0.x C005FFFF.A0.v5 C005FFFF.A0.v2 "
     "C005FFFF.A0.v0 D9020002.lengthdir-y 0C550000 4555FFFF.A0.y " + UTF_RECT,
     ("var:v0", "var:v1", "var:v2", "var:v3", "var:v4", "var:v5", "func:collision-rectangle", "i16:0xC8")),
    # Step of a fire bullet crossing the box: destroyed past global.v0[3] going down or above global.v0[2] less its
    # sprite height going up, then the box test. Operands: collision_rectangle's function index, the object index
    # (0xAC), the operands of the array read (0x10), vspeed (0x20) and sprite_height (0x60).
    ("UtFlames_sidedStep",
     "C005FFFF.A0.y 840FFFFB 840F0003 C0050000.00.v0 15550600 B8000006 C005FFFF.A0.vspeed 840F0000 15520600 "
     "B6000002 C00F0000 B8000004 D9020000.instance-destroy 9E050000 C005FFFF.A0.y 840FFFFB 840F0002 "
     "C0050000.00.v0 C005FFFF.A0.sprite-height 0D550000 15550100 B8000006 C005FFFF.A0.vspeed 840F0000 15520100 "
     "B6000002 C00F0000 B8000004 D9020000.instance-destroy 9E050000 " + UTF_RECT,
     ("func:collision-rectangle", "i16:0xAC", "ref:0x10", "ref:0x20", "ref:0x60")),
    # Step of an ember floating behind the fight: v1 eases towards v0 by 0.03 (outside +-0.1), the scales follow
    # it, image_alpha fades by 0.01 (destroyed below 0.01), v2 += 1, x += sin(v2 / v3) * v4, y += cos(v2 / v3) * v5.
    # Operands: v0..v5's IDs.
    ("UtFlames_emberStep",
     "C005FFFF.A0.v0 C005FFFF.A0.v1 C0000000.9A9999999999B93F 0C500000 15550600 B8000009 C005FFFF.A0.v1 "
     "C0000000.B81E85EB51B89E3F 0C500000 4555FFFF.A0.v1 C005FFFF.A0.v0 C005FFFF.A0.v1 C0000000.9A9999999999B93F "
     "0D500000 15550100 B8000009 C005FFFF.A0.v1 C0000000.B81E85EB51B89E3F 0D500000 4555FFFF.A0.v1 C005FFFF.A0.v1 "
     "4555FFFF.A0.image-xscale C005FFFF.A0.v1 4555FFFF.A0.image-yscale C005FFFF.A0.image-alpha "
     "C0000000.7B14AE47E17A843F 0D500000 4555FFFF.A0.image-alpha C005FFFF.A0.image-alpha C0000000.7B14AE47E17A843F "
     "15500100 B8000004 D9020000.instance-destroy 9E050000 C005FFFF.A0.v2 840F0001 0C520000 4555FFFF.A0.v2 "
     "C005FFFF.A0.x C005FFFF.A0.v2 C005FFFF.A0.v3 09550000 D9020001.sin C005FFFF.A0.v4 08550000 0C550000 "
     "4555FFFF.A0.x C005FFFF.A0.y C005FFFF.A0.v2 C005FFFF.A0.v3 09550000 D9020001.cos C005FFFF.A0.v5 08550000 "
     "0C550000 4555FFFF.A0.y",
     ("var:v0", "var:v1", "var:v2", "var:v3", "var:v4", "var:v5")),
    # Draw of a falling-water foreground strip (Waterfall): v0 += 3; 20 rows of draw_sprite_ext(sprite_index, 0, x,
    # -210 + y + v1 * 30 + v0, 2, 2, 0, c_white, 0.2); v0 wraps past 180; v2 follows the view (view_xview[0] clamped to
    # <the room's width> - view_wview[0]); x = xstart - v2 * 0.5. Operands: v0..v2's IDs, draw_sprite_ext's function index and
    # the operands of view_xview (0x11C), the room's width (0x140), view_wview (0x150), xstart (0x1DC) and x (0x200). Its
    # wrapper goes in a section of its own (the fourth item), after the others' late code.
    ("UtFlames_wfallDraw",
     "C005FFFF.A0.v0 840F0003 0C520000 4555FFFF.A0.v0 840F0000 4525FFFF.A0.v1 C005FFFF.A0.v1 840F0014 15520100 "
     "B800002A C0000000.9A9999999999C93F 07500000 C0020000.FFFFFF00 07520000 840F0000 07520000 840F0002 07520000 "
     "840F0002 07520000 840FFF2E C005FFFF.A0.y 0C250000 C005FFFF.A0.v1 840F001E 08520000 0C550000 C005FFFF.A0.v0 "
     "0C550000 C005FFFF.A0.x 840F0000 07520000 C005FFFF.A0.sprite-index D9020009.draw-sprite-ext 9E050000 "
     "C005FFFF.A0.v1 840F0001 0C520000 4555FFFF.A0.v1 B67FFFD3 C005FFFF.A0.v0 840F00B4 15520600 B8000007 "
     "C005FFFF.A0.v0 840F00B4 0D520000 4555FFFF.A0.v0 840F0000 4525FFFF.A0.v2 840FFFFF 840F0000 "
     "C0050000.00.view-xview 840F0000 15520600 B800000E 840FFFFF 840F0000 C0050000.00.view-xview "
     "C305FFFF.A0.room-width 840FFFFF 840F0000 C0050000.00.view-wview 0D550000 15550100 B6000002 C00F0000 B8000007 "
     "840FFFFF 840F0000 C0050000.00.view-xview 4555FFFF.A0.v2 840FFFFF 840F0000 C0050000.00.view-xview "
     "C305FFFF.A0.room-width 840FFFFF 840F0000 C0050000.00.view-wview 0D550000 15550500 B800000A "
     "C305FFFF.A0.room-width 840FFFFF 840F0000 C0050000.00.view-wview 0D550000 4555FFFF.A0.v2 C005FFFF.A0.xstart "
     "C005FFFF.A0.v2 C0000000.000000000000E03F 08500000 0D550000 4555FFFF.A0.x",
     ("var:v0", "var:v1", "var:v2", "func:draw-sprite-ext", "ref:0x11C", "ref:0x140", "ref:0x150", "ref:0x1DC",
      "ref:0x200"),
     ".text.late.utflames.wfall"),
    # The monster dust (engine/src/z_utkill_vapor.c): a shape given as "fnv:" and the FNV-1a 64 of its full token string
    # (utflames_shape's full tokens: every 64-bit immediate whole and every Push.s by its text, not its index; long
    # shapes), with "ops" after the operands: the body gets every operand, in order, in one int32 array
    # (RValue fn(VMContext*, uint32_t k, AotFn generated, const int32_t* ops)). "str:<address>" is the string index of
    # the Push.s there; "obj:<address>" the value of the PushI.i16 there, an object index, which may be any (its token
    # is 840F****, so a data.win with the object elsewhere still matches); "wild:<address>" makes the PushI.i16 there
    # any without passing it.
    # The monster dust maker's Draw (230 instructions): its row reader and the 2x2 pixels it makes (0x3E4: the
    # pixel's object; 0x2DC: the same object in the wide case, which the generated function keeps).
    ("Utkill_vaporDraw", "fnv:EE588BE6A792128E",
     ("var:v0", "var:v1", "var:v2", "var:v3", "var:v4", "var:v5", "var:v6", "var:v7", "var:v8", "var:v9", "var:v10",
      "var:v11", "var:v12", "var:v13", "var:v14", "func:draw-sprite-part-ext", "func:string-char-at",
      "func:draw-set-color", "func:ord", "func:instance-create", "func:event-user", "func:instance-destroy",
      "obj:0x3E4", "str:0x128", "str:0x140", "str:0x158", "str:0x4AC", "wild:0x2DC"), "ops"),
    # The dust pixel's Create: action_set_gravity(90, random(0.5) + 0.2); action_set_hspeed(random(4) - 2).
    ("Utkill_pixelCreate", "fnv:BBDC10A71459CDED",
     ("func:random", "func:action-set-gravity", "func:action-set-hspeed", "i16:0x28", "i16:0x3C", "i16:0x4C"), "ops"),
]


def utflames_shape(dw, blob, entry):
    """The entry's shape for UTF_NATIVE (instruction tokens; None when a variable has a negative ID that is not a
    builtin's), its variables' indices in first-use order, its functions' indices by name, its instructions by address,
    and the monster dust's "fnv:" view: its full tokens by address (a 64-bit immediate whole, a Push.s by its text in hex,
    any other operand as "%08X" % extra, a variable a builtin only by varID) with that view's variable order. None
    when it can't be read straight through."""
    tokens, var_order, funcs, by_addr, full, fvar_order = [], [], {}, {}, [], []
    shape_ok = True
    addr = entry["offset"]
    while addr < len(blob):
        try:
            ins = Instr(blob, addr)
        except Reject:
            return None
        by_addr[addr - entry["offset"]] = ins
        tok = "%08X" % ins.word
        otok = tok
        if ins.extra is not None:
            if ins.op in (OP_PUSHLOC, OP_PUSHGLB, OP_PUSHBLTN, OP_POP) or (ins.op == OP_PUSH and ins.t1 == T_VAR):
                vi = ins.var_index()
                if vi >= len(dw.vars):
                    return None
                v = dw.vars[vi]
                if v["varID"] == VARIABLE_BUILTIN or v["instanceType"] == BUILTIN:
                    what = v["name"].replace("_", "-")
                elif v["varID"] < 0:
                    shape_ok = False   # not a slot a native body can look up (-1 is the hash map's empty key)
                    what = "?"
                else:
                    if vi not in var_order:
                        var_order.append(vi)
                    what = "v%d" % var_order.index(vi)
                if v["varID"] == VARIABLE_BUILTIN:
                    fwhat = v["name"].replace("_", "-")
                else:
                    if vi not in fvar_order:
                        fvar_order.append(vi)
                    fwhat = "v%d" % fvar_order.index(vi)
                tok += ".%02X.%s" % (ins.var_type(), what)
                otok += ".%02X.%s" % (ins.var_type(), fwhat)
            elif ins.op == OP_CALL:
                if ins.extra >= len(dw.funcs):
                    return None
                tok += "." + dw.funcs[ins.extra]["name"].replace("_", "-")
                otok = tok
                funcs[dw.funcs[ins.extra]["name"].replace("_", "-")] = ins.extra
            else:
                tok += "." + blob[addr + 4:ins.next].hex().upper()   # every operand byte (a double's 8)
                otok += ".%08X" % ins.extra
        tokens.append(tok)
        if ins.op == OP_PUSH and ins.t1 in (T_DOUBLE, T_INT64) and ins.size == 12:
            ftok = "%08X.%016X" % (ins.word, struct.unpack_from("<Q", blob, addr + 4)[0])
        elif ins.op == OP_PUSH and ins.t1 == T_STRING and ins.extra is not None:
            text = dw.strg(ins.extra)
            if text is None:
                return None
            ftok = "%08X.s%s" % (ins.word, text.hex().upper())
        else:
            ftok = otok
        full.append((addr - entry["offset"], ftok))
        addr = ins.next
    return (" ".join(tokens) if shape_ok else None), var_order, funcs, by_addr, full, fvar_order


def utflames_same(shape, want):
    """Whether an entry's shape is a UTF_NATIVE shape (840F**** in it is any PushI.i16)."""
    if shape is None:
        return False
    if "*" not in want:
        return shape == want
    got, wanted = shape.split(" "), want.split(" ")
    return len(got) == len(wanted) and all(
        g == w or (w == "840F****" and len(g) == 8 and g.startswith("840F")) for g, w in zip(got, wanted))


def utflames_native(dw, blob, entry, k):
    """(C text to add after aotf_<k>, native body's name) when the entry matches a UTF_NATIVE shape, else None."""
    got = utflames_shape(dw, blob, entry)
    if got is None:
        return None
    shape, var_order, funcs, by_addr, full, fvar_order = got
    for item in UTF_NATIVE:
        fn_name, want, operands = item[:3]
        ops = len(item) > 3 and item[3] == "ops"
        if want.startswith("fnv:"):
            wild = set(int(op.split(":", 1)[1], 16) for op in operands if op.split(":", 1)[0] in ("obj", "wild"))
            if any(a not in by_addr or by_addr[a].op != OP_PUSHI or by_addr[a].t1 != T_INT16 for a in wild):
                continue
            text = " ".join("840F****" if a in wild else t for a, t in full)
            if int(want[4:], 16) != fnv1a64(text.encode()):
                continue
            order = fvar_order
        elif not utflames_same(shape, want):
            continue
        else:
            order = var_order
        args = []
        for op in operands:
            kind, what = op.split(":", 1)
            if kind == "wild":
                continue
            if kind == "var":
                args.append("%d" % dw.vars[order[int(what[1:])]]["varID"])
            elif kind == "func":
                args.append(("%du" if ops else "%d") % funcs[what])
            elif kind == "ref":
                args.append("(int32_t) 0x%08Xu" % by_addr[int(what, 16)].extra)
            elif kind == "str":
                args.append("%d" % by_addr[int(what, 16)].extra)
            else:
                args.append("%d" % to_i16(by_addr[int(what, 16)].word))
        text = ["#ifndef AOT_UTF_LATE",
                "#if defined(__arm__) && defined(__3DS__)",
                "#define AOT_UTF_LATE __attribute__((section(\".text.late.utflames\")))",
                "#else",
                "#define AOT_UTF_LATE",
                "#endif",
                "#endif"]
        if ops:
            # The monster dust: its wrapper in the body's own late section, so that it moves no late code.
            text += ["#ifndef AOT_UTK_LATE",
                     "#if defined(__arm__) && defined(__3DS__)",
                     "#define AOT_UTK_LATE __attribute__((section(\".text.late.utkill.vapor\")))",
                     "#else",
                     "#define AOT_UTK_LATE",
                     "#endif",
                     "#endif",
                     "RValue %s(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* ops);" % fn_name,
                     "RValue aotu_%d(VMContext* ctx);" % k,
                     "AOT_UTK_LATE RValue aotu_%d(VMContext* ctx) {" % k,
                     "    int32_t ops[%d];" % len(args)]
            text += ["    ops[%d] = (int32_t) %s;" % (i, v) for i, v in enumerate(args)]
            text += ["    return %s(ctx, %d, aotf_%d, ops);" % (fn_name, k, k), "}"]
            return text, fn_name
        text += ["RValue %s(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a);" % fn_name,
                 "RValue aotu_%d(VMContext* ctx);" % k]
        if len(item) > 3:
            text += ["#if defined(__arm__) && defined(__3DS__)",
                     "__attribute__((section(\"%s\")))" % item[3],
                     "#endif",
                     "RValue aotu_%d(VMContext* ctx) {" % k]
        else:
            text += ["AOT_UTF_LATE RValue aotu_%d(VMContext* ctx) {" % k]
        text += ["    int32_t a[%d];" % len(args)]
        text += ["    a[%d] = %s;" % (i, v) for i, v in enumerate(args)]
        text += ["    return %s(ctx, %d, aotf_%d, a);" % (fn_name, k, k), "}"]
        return text, fn_name
    return None



# WAD 16 (UNDERTALE) only: Photoshop Flowey's three pipe Draws go to engine/src/z_utflowey.c's UtFlowey_pipeDraw, keyed
# by the entry's digest (its bytecode exactly), with the operands it reads by name: which pipe, the variables' IDs (-1
# for a name the pipe doesn't use), draw_sprite_ext's function index, image_blend's Pop.v.v operand at 0048 and the
# address after each of the four draw_sprite_ext calls. --no-utflames turns it off with UTF_NATIVE.
UTW_VARS = ("frozen", "siner", "flash", "blend2", "i", "offx", "offy", "offs", "offx2", "offy2", "offs2", "op",
            "soul_rescue", "debug", "offv", "offv2", "magicfactor", "magicfactor2", "magicfactor3")
UTW_NATIVE = {
    0x0E103B355B52B7F9: (0, "pipe 0", ("debug", "offv", "offv2", "magicfactor", "magicfactor2", "magicfactor3")),
    0x44094B596671D8EA: (1, "pipe 1", ("offv", "offv2", "magicfactor", "magicfactor2", "magicfactor3")),
    0x42C2AF9B7EE3C4D4: (2, "pipe 2", ()),
}


def utflowey_native(dw, blob, entry, k, digest):
    """(C text to add after aotf_<k>, native body's name) for one of UTW_NATIVE's entries, else None."""
    if digest not in UTW_NATIVE:
        return None
    variant, _, absent = UTW_NATIVE[digest]
    got = utflames_shape(dw, blob, entry)
    if got is None:
        return None
    shape, var_order, funcs, by_addr = got[:4]
    if shape is None:
        return None
    ids = {}
    for vi in var_order:
        v = dw.vars[vi]
        if v["name"] in UTW_VARS:
            if v["name"] in ids:
                return None   # two variables of one name (self and global): not the entry this was written for
            ids[v["name"]] = v["varID"]
    args = ["%d" % variant]
    for name in UTW_VARS:
        if (name in ids) == (name in absent):
            return None
        args.append("%d" % ids.get(name, -1))
    if "draw-sprite-ext" not in funcs or 0x48 not in by_addr or by_addr[0x48].op != OP_POP:
        return None
    args.append("%d" % funcs["draw-sprite-ext"])
    args.append("(int32_t) 0x%08Xu" % by_addr[0x48].extra)
    calls = sorted(addr + 8 for addr, ins in by_addr.items()
                   if ins.op == OP_CALL and ins.extra == funcs["draw-sprite-ext"])
    if len(calls) != 4:
        return None
    args += ["0x%X" % c for c in calls]
    text = ["#ifndef AOT_UTW_LATE",
            "#if defined(__arm__) && defined(__3DS__)",
            "#define AOT_UTW_LATE __attribute__((section(\".text.late.utflowey3\")))",
            "#define AOT_UTW_RODATA __attribute__((section(\".rodata.utflowey3_in_text\")))",
            "#else",
            "#define AOT_UTW_LATE",
            "#define AOT_UTW_RODATA",
            "#endif",
            "#endif",
            "RValue UtFlowey_pipeDraw(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a);",
            "RValue aotu_%d(VMContext* ctx);" % k,
            "AOT_UTW_LATE RValue aotu_%d(VMContext* ctx) {" % k,
            "    static const int32_t a[%d] AOT_UTW_RODATA = { %s };" % (len(args), ", ".join(args)),
            "    return UtFlowey_pipeDraw(ctx, %d, aotf_%d, a);" % (k, k),
            "}"]
    return text, "UtFlowey_pipeDraw"


def generate_function(dw, name, k, report):
    """Returns (function C text, table C text, aotEntries line, instruction count) or None when the entry is left to
    the interpreter."""
    entry = dw.code_by_name.get(name)
    if entry is None:
        report.append("skip %s: no such code entry" % name)
        return None
    blob = bytes(dw.data[entry["abs"]:entry["abs"] + entry["length"]])
    try:
        fn = Function(dw, entry, blob)
        em = Emitter(dw, fn, k)
        if TYPED is not None and typed_mod.selected(TYPED, getattr(dw, "folder", ""), name):
            regions = typed_mod.select_regions(em, report, name)
            em.typed = {r.site: r for r in regions}
            em.typed_exits = set(t for r in regions for t in r.exits)
        esc = escape_plan(dw, fn, k, name, em, report)
        if esc is not None:
            em = esc
        body = em.gen()
    except Reject as ex:
        report.append("skip %s: %s" % (name, ex))
        return None
    if not fn.instrs:   # WAD16: UNDERTALE has 120 empty code entries
        report.append("skip %s: no instructions" % name)
        return None
    end = max(ins.next for addr, ins in fn.instrs.items() if addr in fn.depth)
    digest = fnv1a64(blob[entry["offset"]:end])
    body, tail = em.finish_refinds(body)
    tail = em.stub_lines() + tail
    out = ["// %s (%d instructions)" % (name.replace("*/", ""), len(fn.depth)),
           "#define AOT_K %d" % k]
    if any("AOT_CALL_LAVA_PART(" in line for line in body):
        # Use the existing comment line: another physical line changes
        # __LINE__ literals in unrelated functions later in the same unit.
        out[0] = '#include "aot/vm_aot_lava.h" ' + out[0]
    if esc is not None:
        tail += esc.esc_tail()
        out += esc.prelude()
    typed_regions = sorted(em.typed.values(), key=lambda r: r.lo)
    inline_text = typed_mod.inline_bindings(em, typed_regions) if typed_regions else ""
    inline_count = len({code_index for r in typed_regions for code_index, _ in r.scripts.values()})
    if inline_text:
        out += ["extern AotInlineCache aotic_%d;" % k,
                "extern const AotInlineBinding aotid_%d[%d];" % (k, inline_count)]
    # --typed all: each region may be left out of a module (aot_module_link.cmake), so the call is through a weak
    # reference, null when it was (typed_mod.call_site), and the inline bindings stay with the function, in
    # aot_typed_bind.c. A typed list: the regions and bindings together in aot_typed.c, called directly.
    split = bool(typed_regions) and TYPED == typed_mod.ALL
    if split and inline_text:
        TYPED_BIND.append((k, inline_text))
    for r in typed_regions:
        if split:
            out.append(typed_mod.prototype(r) + " __attribute__((weak));")
            text = ("extern AotInlineCache aotic_%d;\n" % k if r.scripts else "") + typed_mod.function_text(r, name)
        else:
            out.append(typed_mod.prototype(r) + ";")
            text = ((inline_text + "\n\n" if r is typed_regions[0] and inline_text else "")
                    + typed_mod.function_text(r, name))
        TYPED_TEXTS.append((name, k, text, r.straight, r.lo))
    nloc = len(em.locals)
    if nloc:
        local_vars = [idx for idx, num in sorted(em.locals.items(), key=lambda kv: kv[1])]
        out += ["static const uint32_t aotlv_%d[] = { %s };" % (k, ", ".join(str(i) for i in local_vars)),
                "static uint32_t aotls_%d[%d];" % (k, nloc),
                "static AotLocalCache aotlc_%d;" % k,
                "#define AOT_LS aotls_%d" % k]
    out += ["RValue aotf_%d(VMContext* ctx) {" % k,
            "    AOT_ENTRY_CHECK();"]
    if inline_text:
        out += ["    if (__builtin_expect(aotic_%d.epoch != aotEpoch, 0))" % k,
                "        aotResolveInlineBindings(ctx, &aotic_%d, aotid_%d, %d);" % (k, k, inline_count),
                # Generic slot analysis can also rely on a script being pure. A changed dependency must not run
                # that caller's generated fallback with assumptions derived from the old script body.
                "    if (__builtin_expect(!aotic_%d.valid, 0)) return aotx_executeLoop(ctx);" % k]
    out.append("    AOT_PROLOGUE();")
    if nloc:
        out.append("    AOT_LOCALS(aotlv_%d, aotls_%d, &aotlc_%d, %d);" % (k, k, k, nloc))
    if em.cslots:
        names = [p for key, p in sorted(em.cslots.items(), key=lambda kv: int(kv[1][5:]))]
        for i in range(0, len(names), 8):
            out.append("    RValue %s;" % ", ".join("*%s = nullptr" % p for p in names[i:i + 8]))
    out.extend(body)
    out += ["    AOT_EPILOGUE();"] + tail + ["}"]
    variant = utflames_native(dw, blob, entry, k) if UTFLAMES and dw.wad == 16 else None
    if variant is not None:
        report.append("utflames %s: aotu_%d calls %s" % (name, k, variant[1]))
    elif UTFLAMES and dw.wad == 16 and digest in UTW_NATIVE:
        variant = utflowey_native(dw, blob, entry, k, digest)
        report.append("utflowey %s: %s" % (name, "aotu_%d calls UtFlowey_pipeDraw" % k if variant is not None
                                           else "digest matches but the operands don't: generated code"))
    out.append("#undef AOT_K")
    if nloc:
        out.append("#undef AOT_LS")
    vars_sorted = sorted(em.vars)
    func_text = "\n".join(out)
    copy = None
    if (variant is None and dw.wad == 16 and digest in LATE_COPIES and not nloc and esc is None and not typed_regions
            and not inline_text):
        copy = late_copy(dw, k, func_text)
        if copy is not None:
            report.append("late copy %s: aotl_%d, %d builtin writes inline" % (name, k, copy[1]))
    out = ["RValue aotf_%d(VMContext* ctx) AOT_FN_DECL;" % k]
    if variant is not None:
        # In aot_table.c, not beside aotf_<k>: a caller in its own file changes where GCC puts aotf_<k> there.
        out += variant[0]
    if copy is not None:
        LATE_TEXTS.append("\n".join(copy[0]))
        out.append("RValue aotl_%d(VMContext* ctx);" % k)
    out.append("static const AotVarRef aotv_%d[] = {" % k)
    for idx in vars_sorted:
        v = dw.vars[idx]
        out.append("    { %d, %d, %d, %s }," % (idx, v["varID"], v["instanceType"], c_string(v["name"])))
    if not vars_sorted:
        out.append("    { 0, 0, 0, \"\" },")
    out.append("};")
    funcs_sorted = sorted(em.funcs)
    out.append("static const AotFuncRef aotfn_%d[] = {" % k)
    for idx in funcs_sorted:
        out.append("    { %d, %s }," % (idx, c_string(dw.funcs[idx]["name"])))
    if not funcs_sorted:
        out.append("    { 0, \"\" },")
    out.append("};")
    line = ("    { %s, %d, %d, %d, 0x%016XULL, %s_%d, aotv_%d, %d, aotfn_%d, %d },"
            % (c_string(name), entry["offset"], entry["length"], end, digest,
               "aotu" if variant is not None else "aotl" if copy is not None else "aotf", k, k,
               len(vars_sorted), k, len(funcs_sorted)))
    report.append("ok   %s: %d instructions, %d labels, %d locals, %d left to the interpreter%s"
                  % (name, len(fn.depth), len(fn.labels), len(em.locals), len(fn.stubs),
                     "".join("\n       %04X %s" % (a, why) for a, why in sorted(fn.stubs.items()))))
    return func_text, "\n".join(out), line, len(fn.depth)


def main(argv):
    # Isolated Python (-I) omits the script directory from its import path.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    args = list(argv[1:])
    if len(args) < 3:
        print(__doc__)
        return 2
    t0 = time.time()
    global TYPED
    TYPED = None   # prepare.py runs main once per chapter in one process: --typed sets it again
    del TYPED_TEXTS[:]
    del TYPED_BIND[:]
    out_dir, groups, opts = parse_args(args)
    chunk = opts["chunk"]
    report = []
    funcs_out = []    # (function text, instruction count)
    func_keys = []    # (group index, code entry) of each
    tables_out = []
    entries = []
    entry_parts = []  # per entry: its game's index in parts_out
    parts_out = []    # (folder name, code entries, variables, functions) of each game, for the run-time report
    total_instrs = 0
    sources = []
    games = []        # (group index, DataWin) for the games that can be compiled
    btnprompt_table = None   # UNDERTALE's console prompts (btnprompt.py), from the first WAD 16 game
    late_entries = []  # explicit --late-numeric only; original warm bodies stay in funcs_out
    for gi, g in enumerate(groups):
        dw = DataWin(g["data_win"])
        sources.append(os.path.basename(os.path.dirname(g["data_win"])) + "/" + os.path.basename(g["data_win"]))
        if dw.wad < 16:
            report.append("skip %s: WAD %d bytecode (only WAD 16 and 17 are supported)" % (g["data_win"], dw.wad))
            continue
        dw.patch_operands()
        if dw.wad == 16 and btnprompt_table is None:
            # UNDERTALE's console prompts: the table btnprompt.py finds in this data.win goes into aot_table.c for
            # z_btnprompt.c, and its branches are patched here as Btnprompt_registerAll patches them at VM_create.
            import btnprompt
            btnprompt_table = btnprompt.generate(dw)
            report.extend(btnprompt.apply(dw, btnprompt_table))
        report.append("game %s: WAD %d, %d code entries" % (g["data_win"], dw.wad, len(dw.code)))
        dw.part = len(parts_out)
        if TYPED is not None:
            dw.folder = os.path.basename(os.path.dirname(os.path.abspath(g["data_win"])))
        parts_out.append((os.path.basename(os.path.dirname(os.path.abspath(g["data_win"]))) or "data.win", len(dw.code),
                          len(dw.vars), len(dw.funcs)))
        games.append((gi, dw))
    group_of = {id(dw): gi for gi, dw in games}

    def emit(dw, name):
        late = None
        if name in opts.get("late_numeric", ()):
            import late_numeric
            late = late_numeric.generate(sys.modules[__name__], dw, name, len(entries), report)
            r = late['original'], late['table'], late['entry'], late['instruction_count']
        else:
            r = generate_function(dw, name, len(entries), report)
        if r is not None and r[3] > opts["max_size"]:
            TYPED_TEXTS[:] = [t for t in TYPED_TEXTS if t[1] != len(entries)]
            TYPED_BIND[:] = [t for t in TYPED_BIND if t[0] != len(entries)]
            report.append("skip %s: %d instructions, over --max-size %d" % (name, r[3], opts["max_size"]))
            return None
        if r is not None:
            if late is not None:
                late_entries.append(late)
            funcs_out.append((r[0], r[3]))
            func_keys.append((group_of[id(dw)], name))
            tables_out.append(r[1])
            entries.append(r[2])
            entry_parts.append(dw.part)
        return r

    if opts["budget"] is None:
        for gi, dw in games:
            g = groups[gi]
            names = list(g["names"])
            for p in g["profiles"]:
                sel, pct = names_from_profile(p, opts["cover"], opts["max"])
                report.append("profile %s: %d entries covering %.1f%% of its instructions" % (os.path.basename(p), len(sel), pct))
                names.extend(sel)
            seen = set()
            for name in names:
                if name in seen:
                    continue
                seen.add(name)
                r = emit(dw, name)
                if r is not None:
                    total_instrs += r[3]
    else:
        # One budget over every game: the --names entries first, then the profiled entries by gain per instruction.
        limit = int(opts["budget"] * 1e6 / opts["bytes"])
        dws = dict(games)
        sources_p = [(gi, None, p) for gi, _ in games for p in groups[gi]["profiles"]]
        sources_p += [(gi, c, p) for gi, _ in games for c, p in groups[gi]["corpora"]]
        profiles = [(gi, profile_ops(p)) for gi, c, p in sources_p]
        n_scenes = sum(1 for _, c, _ in sources_p if c is None)
        n_corpus = {}
        for _, c, _ in sources_p:
            if c is not None:
                n_corpus[c] = n_corpus.get(c, 0) + 1
        corpus_total = {c: opts["corpus_weight"].get(c, float(n_scenes or 1)) for c in n_corpus}
        scene_ops = [sum(ops.values()) for (_, c, _), (_, ops) in zip(sources_p, profiles) if c is None]
        mean_ops = sum(scene_ops) / len(scene_ops) if scene_ops and sum(scene_ops) else 0.0
        weights = [(sum(ops.values()) / mean_ops if opts["scene_weight"] == "ops" and mean_ops else 1.0) if c is None
                   else corpus_total[c] / n_corpus[c] for (_, c, _), (_, ops) in zip(sources_p, profiles)]
        sizes = {}
        for gi, ops in profiles:
            for name in ops:
                if (gi, name) not in sizes:
                    sizes[(gi, name)] = entry_size(dws[gi], name)
        if "sel_order" in opts:
            order = [key for key in opts["sel_order"] if key[0] in dws]
        else:
            order = [(gi, name) for gi, _ in games for name in groups[gi]["names"]] + rank_by_gain(profiles, sizes, weights)
        # An "extra" entry of the list is taken on top of the budget.
        extra = opts.get("sel_extra", set())
        chosen = set()
        counted = extra_instrs = 0
        for key in order:
            if key in chosen:
                continue
            size = sizes.get(key) or entry_size(dws[key[0]], key[1])
            if size is None:
                if "sel_order" in opts:
                    report.append("skip %s: entry missing or instruction analysis failed before generation" % key[1])
                continue
            if size > opts["max_size"]:
                report.append("skip %s: %d instructions, over --max-size %d" % (key[1], size, opts["max_size"]))
                continue
            if key not in extra and counted + size > limit:
                if "sel_order" in opts:
                    report.append("skip %s: budget fill order; %d instructions need more than %d remaining (%d of %d used)"
                                  % (key[1], size, limit - counted, counted, limit))
                continue
            r = emit(dws[key[0]], key[1])
            if r is not None:
                chosen.add(key)
                total_instrs += r[3]
                if key in extra:
                    extra_instrs += r[3]
                else:
                    counted += r[3]
        cov = coverage(profiles, chosen)
        report.append("budget %.1f MB at %g bytes an instruction: %d instructions, scenes weighted by %s"
                      % (opts["budget"], opts["bytes"], limit, opts["scene_weight"]))
        if extra_instrs:
            report.append("budget: %d instructions of entries marked extra on top" % extra_instrs)
        for (gi, c, p), x in zip(sources_p, cov):
            if c is None:
                report.append("profile %s: %.1f%% of its instructions covered" % (os.path.basename(p), 100 * x))
        for name in [None] + sorted(n_corpus):
            xs = [x for (_, c, _), x in zip(sources_p, cov) if c == name]
            if xs:
                report.append("%s: %d profiles weighing %g, coverage mean %.1f%%, worst %.1f%%" % (
                    "profiles" if name is None else "corpus " + name, len(xs), len(xs) if name is None else corpus_total[name],
                    100 * sum(xs) / len(xs), 100 * min(xs)))

    missing_late = opts.get("late_numeric", set()) - {name for _, name in func_keys}
    if missing_late:
        raise ValueError("Requested late numeric entries were not emitted: " + ", ".join(sorted(missing_late)))
    os.makedirs(out_dir, exist_ok=True)
    for f in os.listdir(out_dir):
        if re.fullmatch(r"aot_gen_\d+\.c|aot_table\.c|aot_generated\.inc|aot_typed(_bind|_\d+)?\.c|aot_late\.c|aot_utold_(loop|regions)\.c", f):
            os.remove(os.path.join(out_dir, f))
    header = ("// Generated by tools/aot/gmlaot.py from %s.\n"
              "// Derived from the game's code: keep it in the build directory, never commit or share it.\n\n"
              "%s#include \"aot/vm_aot.h\"\n\n"
              "#if VM_HAS_AOT\n\n" % (", ".join(sources), "#define AOT_SHARED_HELPERS 1\n" if SHARED_HELPERS else ""))
    footer = "\n\n#endif\n"

    # The functions go into files of about --chunk bytecode instructions each, so that no compile unit gets large and
    # they build in parallel.
    def chunked(order):
        out = []   # [function indexes, instructions]
        for i in order:
            n = funcs_out[i][1]
            if not out or (out[-1][1] + n > chunk and out[-1][0]):
                out.append([[], 0])
            out[-1][0].append(i)
            out[-1][1] += n
        return out

    chunks = chunked(range(len(funcs_out)))
    if opts.get("layout_order"):
        # --layout: the functions in its order, the rest after them as they were; aotf_<n> and the table stay as made.
        # A module too big for its region is linked without its last files (platform/3ds/aot/aot_module_link.cmake),
        # so the selection's last files keep their functions and their place at the end, and the same lowest-ranked
        # functions are left out with a layout as without: LAYOUT_TAIL of the files, several times what a module has
        # had to leave out.
        keep = -(-len(chunks) * LAYOUT_TAIL // 100)
        head = [i for c in chunks[:len(chunks) - keep] for i in c[0]]
        rank = {}
        for key in opts["layout_order"]:
            rank.setdefault(key, len(rank))
        head.sort(key=lambda i: (rank.get(func_keys[i], len(rank)), i))
        report.append("layout %s: %d of %d functions placed by it; the last %d of %d files as the selection has them"
                      % (os.path.basename(opts["layout"]), sum(1 for i in head if func_keys[i] in rank), len(funcs_out),
                         keep, len(chunks)))
        chunks = chunked(head) + chunks[len(chunks) - keep:]
    for i, (order, n) in enumerate(chunks):
        with open(os.path.join(out_dir, "aot_gen_%d.c" % i), "w", encoding="utf-8", newline="\n") as f:
            f.write(header + "\n\n".join(funcs_out[j][0] for j in order) + footer)
    with open(os.path.join(out_dir, "aot_table.c"), "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
        f.write("\n\n".join(tables_out))
        f.write("\n\nstatic const AotEntry aotEntries[] = {\n")
        f.write("\n".join(entries) if entries else "    { \"\", 0, 0, 0, 0, nullptr, nullptr, 0, nullptr, 0 },")
        f.write("\n};\n")
        # What the engine logs at start-up: which game each function is for, and a tag for the build ID.
        f.write("static const AotPart aotParts[] = {\n%s\n};\n" % "\n".join(
            "    { %s, %d, %d, %d }," % (c_string(n), c, v, fn) for n, c, v, fn in parts_out)
            if parts_out else "static const AotPart aotParts[] = { { \"\", 0, 0, 0 } };\n")
        f.write("static const uint8_t aotEntryPart[] = { %s };\n" % (", ".join(str(p) for p in entry_parts) or "0"))
        tag = opts.get("tag") or (("aot%g" % opts["budget"]) if opts["budget"] is not None else ("aot%dfn" % len(entries)))
        f.write("const char aotBuildTag[] = %s;\n" % c_string(tag))
        f.write("const AotTable aotTable = { aotEntries, %d, aotParts, %d, aotEntryPart, aotBuildTag };"
                % (len(entries), len(parts_out)))
        if btnprompt_table is not None:
            import btnprompt
            f.write("\n\n" + btnprompt.c_source(btnprompt_table, c_string))
        f.write(footer)
    if LATE_TEXTS:
        # The late copies (LATE_COPIES), in a file of their own that platform/3ds/late_reserve.ld links into late text.
        with open(os.path.join(out_dir, "aot_late.c"), "w", encoding="utf-8", newline="\n") as f:
            f.write(header.replace('#include "aot/vm_aot.h"', '#include "aot/aot_late_copy.h"'))
            f.write("\n\n".join(LATE_TEXTS))
            f.write(footer)
        report.append("late copies: %d in aot_late.c" % len(LATE_TEXTS))
    if late_entries:
        for filename, key, include in (('aot_utold_loop.c', 'caller', 'vm_aot.h'),
                                       ('aot_utold_regions.c', 'regions', 'vm_aot_typed.h')):
            with open(os.path.join(out_dir, filename), "w", encoding="utf-8", newline="\n") as f:
                f.write(header.replace('aot/vm_aot.h', 'aot/' + include))
                f.write("\n\n".join(entry[key] for entry in late_entries))
                f.write(footer)
        report.append("experimental late numeric entries: %d; originals retained" % len(late_entries))
    if TYPED is not None and TYPED == typed_mod.ALL:
        # --typed all: the regions in files of about TYPED_FILE_CHARS in priority order, which the module linker leaves
        # out from the last when a module is over its region, before any generated function: the loops first, by their
        # function's rank (its aotf number, the selection's order), then the straight regions by rank. Each file is
        # compiled as aot_typed.c is (vm_aot_typed.h); aot_typed_bind.c, always linked, has the inline bindings.
        typed_header = header.replace('#include "aot/vm_aot.h"', '#include "aot/vm_aot_typed.h"')
        with open(os.path.join(out_dir, "aot_typed_bind.c"), "w", encoding="utf-8", newline="\n") as f:
            f.write(typed_header)
            f.write("\n\n".join(t[1] for t in TYPED_BIND))
            f.write(footer)
        files = []
        for t in sorted(TYPED_TEXTS, key=lambda t: (t[3], t[1], t[4])):
            if not files or (files[-1][1] + len(t[2]) > TYPED_FILE_CHARS and files[-1][0]):
                files.append([[], 0])
            files[-1][0].append(t[2])
            files[-1][1] += len(t[2])
        for n, (texts, _) in enumerate(files):
            with open(os.path.join(out_dir, "aot_typed_%d.c" % n), "w", encoding="utf-8", newline="\n") as f:
                f.write(typed_header)
                f.write("\n\n".join(texts))
                f.write(footer)
        report.append("typed: %d regions in %d functions (%d loops), %d files by priority" % (
            len(TYPED_TEXTS), len(set(t[1] for t in TYPED_TEXTS)), sum(1 for t in TYPED_TEXTS if not t[3]), len(files)))
    elif TYPED is not None:
        # The typed regions, in a file of their own that platform builds compile without fast math (vm_aot_typed.h).
        with open(os.path.join(out_dir, "aot_typed.c"), "w", encoding="utf-8", newline="\n") as f:
            f.write(header.replace('#include "aot/vm_aot.h"', '#include "aot/vm_aot_typed.h"'))
            f.write("\n\n".join(t[2] for t in TYPED_TEXTS))
            f.write(footer)
        report.append("typed: %d regions in %d functions" % (len(TYPED_TEXTS), len(set(t[1] for t in TYPED_TEXTS))))
    report.append("%d files of generated code" % len(chunks))
    report.append("%d functions, %d instructions, %.1f s" % (len(entries), total_instrs, time.time() - t0))
    with open(os.path.join(out_dir, "aot_report.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(report) + "\n")
    print("\n".join(report))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

