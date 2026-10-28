"""Typed AOT tier, steps T1 through T3 (docs/perf/typed-aot.md, sections 2, 4.6-4.8 and 6.1): what gmlaot.py --typed
<list> adds.

For the code entries the list names, each loop the tier can follow becomes a typed region: an out-of-line C function
in aot_typed.c, which platform builds compile without -ffast-math and without FP contraction (engine/src/aot/
vm_aot_typed.h says how it is called and what it returns). The generated function calls it just before the loop's
head; the code gmlaot.py emits today stays in the file and runs whenever the region doesn't.

What a region does:
- Guards first, before any side effect: every cached slot it uses is there, every builtin it calls is bound, self is an
  instance when it reads one's builtin fields, and every variable whose value on entry it may read holds a plain
  number (int32 or real) with no asset tag. A failed guard returns 0 and the generated
  loop runs as it always has.
- Variables (self, other, global and local) live in C RValues for the whole region and are written back when it is
  left, each only if the region stored to it, with the value and tags (type, gmlStackType, assetRefType) the generated
  code would have left there. Stack temporaries are C scalars where their type is proven, C RValues where it is not.
- Every operation is the interpreter's own, on the same operands, in bytecode order, each double operation rounded
  once: vmArithNumbers, vmDivNumbers, AOT_MOD's fmod, vmCmpOrder/vmCmpHolds, vmConvFast, the aotMath copies (out of
  line, compiled as the interpreter is, for those with more than one floating-point operation). Where a type is not
  proven the code tests it, and where the interpreter would take a slow path (handleAddSlow, handleMod's error,
  handleCmp, handleDup, a store of a string...) the region hands over: it writes the variables back, puts its operand
  stack in the VM's stack slots and lets the interpreter finish the call from that instruction (AOT_DEOPT_AT).
- Builtins are called through their bound C entry (ctx->funcCallCache) with argument blocks holding the tags the
  interpreter would have pushed. Only builtins of EFFECTS (each read in engine/src/vm_builtins.c) and the INLINE_MATH
  ones may appear, so nothing in a region runs GML, reads or writes a GML variable or moves a slot: the values kept in
  C can't be observed before they are written back. A pure builtin whose arguments don't change in the region (and
  whose state no call in it writes) is called at its first execution in each region entry and its result reused.
- Script calls (T2): a call of a script with no effect (cameray, screeny...) has the script's body compiled in place
  (Gen.inline, "Inlined script calls" below), so it too runs no GML and changes nothing but its result.
- Array/member reads (T3): regions with these reads keep variable slots in memory, reading fresh weak copies and
  storing immediately with storeStackValueIntoSlot. This preserves aliases and reference lifetimes. Cached arrays
  use aotArrayRead, other scopes/members use the existing VM bridges, including static-member fallback. Arithmetic
  guards run at the operation, allowing reference/type changes to hand over with the exact stack. Materializing
  multidimensional reads and array writes stay generated code. Audited builtin getters run at their original ip.

A region is a natural loop: a contiguous range, entered (by the call) either at its head, which the code above falls
into, or at the condition an unconditional branch just above the head jumps to (the usual while/for shape), at stack
depth 0, and left only to depth-0 code outside it. Other edges into it just run the generated loop. Anything else, or any instruction outside the set above, leaves that
loop to the generated code, and aot_report.txt says why.

Straight-line regions (T2): the code of a listed entry outside its loops is cut into spans the tier can follow, each
from one depth-0 statement start to another (straight_regions). A span's call sits just after its first label, so
every way into the span runs it; its Exit instructions are exits (the generated code runs them), and a statement the
tier can't follow ends one span and starts the next after it. A span shorter than STRAIGHT_MIN instructions isn't
worth its guards and stays generated code. In a script, argumentN reads (AOT_PUSH_ARG) are variables of the region
too: read only, guarded like the others, absent (nullptr, the guard fails) when the call passed fewer arguments.
"""
import re
import struct

G = None   # the gmlaot module (bind)

# Experimental, explicitly enabled by the utold object probe only. Normal
# generation (including --typed all and every WAD17 build) stays unchanged.
# No game body or resource constants live here: these are public VM operations.
NUMERIC_SPRITE_LOOPS = False


def bind(mod):
    global G
    G = mod


class Unsupported(Exception):
    addr = None   # the region instruction it is about, when known (straight_regions cuts the span there)


# Builtins a region may call besides the INLINE_MATH ones. Each was read in engine/src/vm_builtins.c: it runs no GML,
# reads and writes no GML variable, creates and destroys no instance or struct, raises no GML exception and leaves the
# operand stack alone (builtin_audit.py must agree too: a builtin it doesn't pass is refused).
#   (kind, state read, state written, result)
#   kind: "pure", the result depends only on the arguments and the state read, and the call changes nothing;
#         "effect", it changes only the state written.
#   result: "R" when it always returns RValue_makeReal (a real with no asset tag that owns nothing), else None.
EFFECTS = {
    # (int32) RValue_toReal(args[0]); the application surface's size or Renderer_getSurfaceWidth/Height
    "surface_get_width": ("pure", ("surface",), (), "R"),
    "surface_get_height": ("pure", ("surface",), (), "R"),
    # RValue_toReal of the three arguments, clamped (GMS2) or RValue_toInt32 & 0xFF (GMS1); RValue_makeReal
    "make_color_hsv": ("pure", (), (), "R"),
    # RValue_toReal/RValue_toColour of the arguments, then the renderer's drawSurface; RValue_makeUndefined
    "draw_surface_part": ("effect", ("surface", "draw"), ("target",), None),
    "draw_surface_part_ext": ("effect", ("surface", "draw"), ("target",), None),
    # (int32) RValue_toReal(args[0]), bounds-checked; the data.win's sprite size (never changes); RValue_makeReal
    "sprite_get_width": ("pure", (), (), "R"),
    "sprite_get_height": ("pure", (), (), "R"),
    # RValue_toInt32/RValue_toReal/RValue_toColour of the arguments (subimg < 0 reads the instance's image_index, which a
    # region never writes), then the renderer's drawSpritePartExt; RValue_makeUndefined
    "draw_sprite_part_ext": ("effect", ("draw", "instance"), ("target",), None),
    # The same for draw_sprite_ext (nine arguments, the renderer's drawSpriteExt)
    "draw_sprite_ext": ("effect", ("draw", "instance"), ("target",), None),
    # RValue_toInt32(args[0]), then Runner_getCameraById (a lookup); RValue_makeReal of the camera's viewX/viewY, or -1
    "camera_get_view_x": ("pure", ("camera",), (), "R"),
    "camera_get_view_y": ("pure", ("camera",), (), "R"),
    # argCount >= 2 (an array argument is never a region value): RValue_toBool(args[0]), RValue_toColour(args[1]),
    # then the renderer's gpuSetFog; RValue_makeUndefined
    "gpu_set_fog": ("effect", (), ("draw",), None),
    # RValue_toColour of args[0] and args[1], (float) RValue_toReal(args[2]), Color_lerp; RValue_makeReal
    "merge_color": ("pure", (), (), "R"),
}

# The arguments a builtin of EFFECTS reads without checking argCount.
EFFECTS_ARGS = {"make_color_hsv": 3, "draw_surface_part": 7, "draw_surface_part_ext": 11, "draw_sprite_part_ext": 12,
                "draw_sprite_ext": 9, "gpu_set_fog": 2, "merge_color": 3}

# Builtin array variables an inlined script may read with a constant index, on self or the builtin scope (what
# resolveVariableRead does for them: VMBuiltins_getVariable on the current instance, none of them instance-scoped, so
# a struct as self reads the same). Each read in engine/src/vm_builtins.c: (state read, result).
BUILTIN_ARRAY_READS = {
    # BUILTIN_VAR_CAMERA_VIEW: RValue_makeReal(runner->views[i].cameraId), or RValue_makeReal(-1.0) out of range
    "view_camera": (("view",), "R"),
}
# Reads needed by the array/member regions. Use the VM's getter at the original instruction, never hoist time.
# Dimensions return RValue_makeReal; member-scoped instance builtins can instead be arbitrary struct fields.
BUILTIN_READS = frozenset(("current_time", "sprite_height", "sprite_width", "room_width", "room_height", "object_index"))
STRAIGHT_MIN = 12    # instructions in a straight-line region, at least
INLINE_DEPTH = 3     # scripts inlined within one another, at most
INLINE_STEPS = 200   # instructions followed in one inlined body, at most
_callees = {}

# INLINE_MATH builtins a region computes inline: one library call or one operation, the same in any compile unit.
# The others go through aotTypedMath<name> (vm_aot_bridge.inc), compiled with the engine's flags.
MATH_INLINE = ("aotMathFloor", "aotMathCeil", "aotMathAbs", "aotMathSqrt", "aotMathSqr", "aotMathSin", "aotMathCos")

T_DOUBLE, T_FLOAT, T_INT32, T_INT64, T_BOOL, T_VAR, T_STRING, T_INT16 = 0, 1, 2, 3, 4, 5, 6, 0xF
INT_TYPES = (T_INT32, T_INT64, T_INT16)
ARITH = {0x0C: ("VM_ARITH_ADD", "+"), 0x0D: ("VM_ARITH_SUB", "-"), 0x08: ("VM_ARITH_MUL", "*")}
IDENT = re.compile(r"^[A-Za-z_][A-Za-z_0-9]*$")


class Val:
    """An operand stack value. k: "r" (a C GMLReal), "i" (an int32_t), "b" (an int32_t 0/1: a bool) or "v" (a C RValue);
    c: its C expression (for "v", a variable). g: the gmlStackType of a scalar (an RValue carries its own). t: what is
    known of the type: "R", "I", "B", "N" (a plain number: int32 or real) or None; a0: its assetRefType is known to be
    0; inv: the same value at every execution within one region entry; n: for a "v" plain number, a GMLReal C
    expression already holding AOT_NUM of it (computed once an entry), else None. A scalar is always an RValue of its
    type with gmlStackType g, no asset tag, nothing owned."""
    __slots__ = ("k", "c", "g", "t", "a0", "inv", "n", "kv")

    def __init__(self, k, c, g=None, t=None, a0=False, inv=False, n=None, kv=None):
        self.k, self.c, self.g, self.t, self.a0, self.inv, self.n, self.kv = k, c, g, t, a0, inv, n, kv
        if k in ("r", "i", "b"):
            self.t, self.a0 = {"r": "R", "i": "I", "b": "B"}[k], True

    def num(self):
        return self.t in ("R", "I", "N")


def join_t(a, b):
    if a == b:
        return a
    if a in ("R", "I", "N") and b in ("R", "I", "N"):
        return "N"
    return None


def as_num(v):
    """A GMLReal C expression for a value known to be a plain number (what AOT_NUM reads)."""
    if v.k == "r":
        return v.c
    if v.k == "i":
        return "(GMLReal) %s" % v.c
    if v.k == "v" and v.n is not None:
        return v.n
    if v.k == "v" and v.t == "R":
        return "%s.real" % v.c
    if v.k == "v" and v.t == "I":
        return "(GMLReal) %s.int32" % v.c
    if v.k == "v" and v.t == "N":
        return "AOT_NUM(&%s)" % v.c
    raise AssertionError("as_num of a %s/%s value" % (v.k, v.t))


def as_int(v):
    return v.c if v.k in ("i", "b") else "%s.int32" % v.c


def as_rv(v):
    """An RValue C expression for a value, with the tags the interpreter's stack slot would hold."""
    if v.k == "r":
        return "TYPED_KR(%s, %d)" % (v.c, v.g)
    if v.k == "i":
        return "TYPED_KI(%s, %d)" % (v.c, v.g)
    if v.k == "b":
        return "TYPED_KB(%s, %d)" % (v.c, v.g)
    return v.c


class State:
    __slots__ = ("stack", "vars")

    def __init__(self, stack, vars):
        self.stack = stack     # [Val]
        self.vars = vars       # var key -> (t, a0)

    def copy(self):
        return State(list(self.stack), dict(self.vars))


class Region:
    def __init__(self, em, lo, hi, straight=False):
        self.em, self.fn, self.lo, self.hi = em, em.fn, lo, hi
        fn = self.fn
        self.straight = straight   # a straight-line region: called after its head's label, its Exits are exits
        self.addrs = sorted(a for a in fn.instrs if lo <= a < hi and a in fn.depth
                            and not (straight and fn.instrs[a].op == G.OP_EXIT))
        self.inside = set(self.addrs)
        self.labels = sorted(set(a for a in self.addrs if a in fn.labels) | {lo})
        self.keys = []          # variable keys in order of first use: ("s", scope, varID) or ("l", varIdx)
        self.reads = {}         # addr -> key read there
        self.writes = {}        # addr -> key written there
        self.fields = {}        # addr -> Instance field read there (self builtins)
        self.accesses = {}      # addr -> (operand count, cached array key or None), reads through memory
        self.sprite_getters = {}  # experimental: audited scalar getters, not aliasing memory reads
        self.memory = False     # these regions read/write slots at each instruction (member aliases are visible)
        self.builtins = {}      # function index -> name
        self.scripts = {}       # function index -> (code index, name) of each script inlined (Gen.inline)
        self.need_self = False  # an inlined script reads a builtin field of self
        self.exits = []         # targets outside the region, in order
        self.entry_addr = lo    # where the region starts running: lo, or the condition the branch at site jumps to
        self.site = lo          # where the generated function calls it: before the head, or before that branch
        self.name = "aott_%d_%04X" % (em.k, lo)


# ---------------------------------------------------------------------------------------------------------------
# Decoding

def memory_read(em, ins):
    """A non-materializing read: (operands consumed, cached array key or None), else None.

    PushAF/PopAF are deliberately excluded: they can materialize arrays. Their original generated code keeps CoW,
    growth and ownership. Ordinary array reads do not change the array or any reference count.
    """
    if ins.op not in (G.OP_PUSH, G.OP_PUSHBLTN) or ins.t1 != T_VAR:
        return None
    v, vt, inst = em.dw.vars[ins.var_index()], ins.var_type(), ins.inst
    stack = em.fn.stacks[ins.addr]
    if vt == G.VT_ARRAY:
        if v["varID"] < 0 and not (v["varID"] == G.VARIABLE_BUILTIN and v["name"] in BUILTIN_ARRAY_READS):
            return None
        scope = stack[-2][0]
        count = 2 + int(em.dw.wad >= 17 and scope == G.STACKTOP)
        key = None
        if v["varID"] >= 0:
            if scope in G.SLOT_SCOPES:
                key = ("s", scope, v["varID"])
            elif scope == G.LOCAL:
                key = ("l", ins.var_index())
        return count, key
    if vt not in (G.VT_NORMAL, G.VT_STACKTOP):
        return None
    if v["varID"] < 0 and not (v["varID"] == G.VARIABLE_BUILTIN
                               and v["name"] in BUILTIN_READS | set(G.SELF_FIELDS)):
        return None
    if vt == G.VT_STACKTOP and em.dw.wad >= 17:
        return 1 + int(stack[-1][0] == G.STACKTOP), None
    if vt == G.VT_NORMAL:
        if inst == G.STACKTOP and em.dw.wad >= 17:
            return 1, None
        if inst >= 0 or inst == G.BUILTIN or (v["varID"] == G.VARIABLE_BUILTIN and v["name"] in BUILTIN_READS):
            return 0, None
    return None

def var_access(em, ins):
    """(read key or None, write key or None, self field or None) of an instruction; Unsupported for a variable access
    a region can't keep in C."""
    op, t1, inst = ins.op, ins.t1, ins.inst
    if op == G.OP_POP:
        v = em.dw.vars[ins.var_index()]
        if t1 != T_VAR or ins.var_type() != G.VT_NORMAL:
            raise Unsupported("Pop form at %04X" % ins.addr)
        if v["varID"] >= 0 and inst in G.SLOT_SCOPES and G.SLOTS:
            return None, ("s", inst, v["varID"]), None
        if v["varID"] >= 0 and inst == G.LOCAL:
            return None, ("l", ins.var_index()), None
        raise Unsupported("write of %s at %04X" % (v["name"], ins.addr))
    if (op == G.OP_PUSH and t1 == T_VAR) or op in (G.OP_PUSHLOC, G.OP_PUSHGLB, G.OP_PUSHBLTN):
        v = em.dw.vars[ins.var_index()]
        if ins.var_type() != G.VT_NORMAL:
            raise Unsupported("array or member access at %04X" % ins.addr)
        if op in (G.OP_PUSH, G.OP_PUSHBLTN) and em.arg_number(v) is not None and (
                (op == G.OP_PUSH and inst == G.ARG and em.dw.wad >= 17)
                or (inst == G.SELF and v["varID"] == G.VARIABLE_BUILTIN)):
            return ("a", em.arg_number(v)), None, None   # AOT_PUSH_ARG
        if op == G.OP_PUSHLOC:
            if v["varID"] >= 0:
                return ("l", ins.var_index()), None, None
            raise Unsupported("PushLoc of %s at %04X" % (v["name"], ins.addr))
        if op == G.OP_PUSHGLB:
            if v["varID"] >= 0 and G.SLOTS:
                return ("s", G.GLOBAL, v["varID"]), None, None
            raise Unsupported("PushGlb of %s at %04X" % (v["name"], ins.addr))
        if op == G.OP_PUSHBLTN or v["varID"] == G.VARIABLE_BUILTIN:
            if (inst == G.SELF and v["varID"] == G.VARIABLE_BUILTIN and v["name"] in G.SELF_FIELDS
                    and em.arg_number(v) is None):
                return None, None, G.SELF_FIELDS[v["name"]]
            raise Unsupported("builtin variable %s at %04X" % (v["name"], ins.addr))
        if v["varID"] >= 0 and inst in G.SLOT_SCOPES and G.SLOTS:
            return ("s", inst, v["varID"]), None, None
        if v["varID"] >= 0 and inst == G.LOCAL:
            return ("l", ins.var_index()), None, None
        raise Unsupported("read of %s (instance type %d) at %04X" % (v["name"], inst, ins.addr))
    return None, None, None


# ---------------------------------------------------------------------------------------------------------------
# Region selection

def select_regions(em, report, name):
    """The typed regions of em's function (see the module comment), outermost loops first. Called before em.gen()."""
    fn = em.fn
    if not G.SLOTS or not G.DIRECT:
        report.append("typed %s: needs cached slots and direct calls" % name)
        return []
    succs = {}
    for a, ins in fn.instrs.items():
        if a in fn.depth:
            s = list(fn.successors(ins))
            if a in fn.fused_targets:
                s.append(fn.fused_targets[a])
            succs[a] = s
    heads = {}
    for a, s in succs.items():
        for t in s:
            if t <= a and t in fn.depth:
                heads[t] = max(heads.get(t, a), a)
    cands = sorted(((h, fn.instrs[e].next) for h, e in heads.items()), key=lambda r: (-(r[1] - r[0]), r[0]))
    chosen = []
    for lo, hi in cands:
        if any(not (hi <= min(c.lo, c.site) or lo >= c.hi) for c in chosen):
            continue
        r = Region(em, lo, hi)
        try:
            check_region(r, succs)
            if any(not (hi <= min(c.lo, c.site) or min(lo, r.site) >= c.hi) for c in chosen):
                raise Unsupported("its call would go in another region")
            infer(r)
        except Unsupported as why:
            report.append("typed %s: loop %04X-%04X left to the generated code: %s" % (name, lo, hi, why))
            continue
        chosen.append(r)
        report.append("typed %s: region %04X-%04X (%s), %d instructions, %d variables (%d guarded), builtins %s, exits %s"
                      % (name, lo, hi, r.name, len(r.addrs), len(r.keys),
                         sum(1 for k in r.keys if r.entry[k][0] is not None),
                         " ".join(sorted(set(r.builtins.values()))) or "none", " ".join("%04X" % t for t in r.exits)))
    chosen += straight_regions(em, succs, sorted(heads.items()), chosen, report, name)
    return sorted(chosen, key=lambda r: r.lo)


def straight_regions(em, succs, loops, chosen, report, name):
    """The straight-line regions of em's function (module comment): spans of depth-0 statements outside every loop,
    cut where the tier can't follow a statement."""
    fn = em.fn
    order = sorted(a for a in fn.instrs if a in fn.depth)
    if not order:
        return []
    end = max(fn.instrs[a].next for a in order)
    ranges = [(h, fn.instrs[e].next) for h, e in loops] + [(min(c.lo, c.site), c.hi) for c in chosen]

    def in_range(a):
        return any(lo <= a < hi for lo, hi in ranges)

    starts = [a for a in order if fn.depth[a] == 0]
    spans, i = [], 0
    while i < len(order):   # maximal runs of instructions outside every loop and region
        if in_range(order[i]):
            i += 1
            continue
        j = i
        while j < len(order) and not in_range(order[j]):
            j += 1
        spans.append((order[i], order[j] if j < len(order) else end))
        i = j
    out = []
    while spans:
        lo, hi = spans.pop(0)
        lo = next((a for a in starts if lo <= a < hi and fn.instrs[a].op != G.OP_EXIT), None)
        if lo is None:
            continue
        n = sum(1 for a in order if lo <= a < hi and fn.instrs[a].op != G.OP_EXIT)
        if n < STRAIGHT_MIN:
            continue
        r = Region(em, lo, hi, straight=True)
        r.name = "aots_%d_%04X" % (em.k, lo)
        try:
            check_region(r, succs)
            infer(r)
        except Unsupported as why:
            x = why.addr
            if x is None or not lo <= x < hi:
                report.append("typed %s: straight %04X-%04X left to the generated code: %s" % (name, lo, hi, why))
                continue
            s = max(a for a in starts if lo <= a <= x)
            e = next((a for a in starts if a > x and a < hi), hi)
            report.append("typed %s: straight %04X-%04X cut at %04X-%04X: %s" % (name, lo, hi, s, e, why))
            spans[:0] = [p for p in ((lo, s), (e, hi)) if p[0] < p[1]]
            continue
        out.append(r)
        report.append("typed %s: straight region %04X-%04X (%s), %d instructions, %d variables (%d guarded), builtins %s, exits %s"
                      % (name, lo, hi, r.name, len(r.addrs), len(r.keys),
                         sum(1 for k in r.keys if r.entry[k][0] is not None),
                         " ".join(sorted(set(r.builtins.values()))) or "none", " ".join("%04X" % t for t in r.exits)))
    return out


def check_region(r, succs):
    fn, em = r.fn, r.em
    if fn.depth.get(r.lo) != 0:
        raise Unsupported("stack depth %s at the head" % fn.depth.get(r.lo))
    inside = set(r.addrs)
    prev = [a for a in fn.instrs if a in fn.depth and fn.instrs[a].next == r.lo]
    p = prev[0] if prev else None
    if r.straight:   # called after the label at its head: every way in runs it
        for a in r.addrs:
            if fn.instrs[a].op == G.OP_RET:
                why = Unsupported("Ret at %04X" % a)
                why.addr = a
                raise why
    elif p is None or p in fn.stubs or fn.instrs[p].op in (G.OP_RET, G.OP_EXIT):
        raise Unsupported("not entered from just above its head")
    elif fn.instrs[p].op == G.OP_B:
        if fn.instrs[p].target() not in inside or fn.depth.get(p) != 0:
            raise Unsupported("the branch above its head goes elsewhere")
        r.entry_addr, r.site = fn.instrs[p].target(), p
        if fn.depth.get(r.entry_addr) != 0:
            raise Unsupported("stack depth %s at its entry" % fn.depth.get(r.entry_addr))
        if r.entry_addr not in r.labels:
            r.labels = sorted(set(r.labels) | {r.entry_addr})
    for a, s in sorted(succs.items()):
        for t in s:
            if a in inside and t not in inside:
                if t not in fn.instrs or t not in fn.depth:
                    why = Unsupported("left for the end of the code at %04X" % a)
                    why.addr = a
                    raise why
                if fn.depth[t] != 0:
                    raise Unsupported("left at stack depth %d" % fn.depth[t])
                if t not in r.exits:
                    r.exits.append(t)
    if not r.exits:
        raise Unsupported("never left")
    fused_dests = set(fn.fused_targets.values())
    for a in r.addrs:
        ins = fn.instrs[a]
        try:
            if a in fn.stubs:
                raise Unsupported("instruction left to the interpreter at %04X (%s)" % (a, fn.stubs[a]))
            if a in fn.fused_targets or a in fused_dests:
                raise Unsupported("method call at %04X" % a)
            access = memory_read(em, ins)
            v = em.dw.vars[ins.var_index()] if access is not None else None
            if (NUMERIC_SPRITE_LOOPS and em.dw.wad == 16 and access == (0, None)
                    and ins.op in (G.OP_PUSH, G.OP_PUSHBLTN) and ins.var_type() == G.VT_NORMAL
                    and ins.inst == G.SELF and v['varID'] == G.VARIABLE_BUILTIN
                    and v['name'] in ('sprite_width', 'sprite_height')):
                # Both getters only read sprite dimensions and instance scale
                # (VMBuiltins_getVariable). They neither observe local/self
                # variable slots nor materialize an array or move a slot.
                r.sprite_getters[a] = ins.var_index()
                r.need_self = True
                rk, wk, field = None, None, None
            elif access is not None:
                r.accesses[a] = access
                r.memory = True
                rk, wk, field = access[1], None, None
            else:
                rk, wk, field = var_access(em, ins)
        except Unsupported as why:
            why.addr = a
            raise
        for k in (rk, wk):
            if k is not None and k not in r.keys:
                r.keys.append(k)
        if rk is not None:
            r.reads[a] = rk
        if wk is not None:
            r.writes[a] = wk
        if field is not None:
            r.fields[a] = field
    # A pure builtin's result is reused within an entry only when no call in the region writes state it reads.
    r.state_written = set()
    for a in r.addrs:
        ins = r.fn.instrs[a]
        if ins.op == G.OP_CALL:
            nm = em.dw.funcs[ins.extra]["name"]
            if nm in EFFECTS:
                r.state_written |= set(EFFECTS[nm][2])
            elif nm == 'draw_sprite_part' and NUMERIC_SPRITE_LOOPS and em.dw.wad == 16:
                r.state_written.add('target')
    liveness(r)


def liveness(r):
    """The variables a region may read before writing them (backward liveness to its head): those are guarded."""
    fn = r.fn
    inside = set(r.addrs)
    live = {a: frozenset() for a in r.addrs}
    changed = True
    while changed:
        changed = False
        for a in reversed(r.addrs):
            out = frozenset()
            for t in fn.successors(fn.instrs[a]):
                if t in inside:
                    out |= live[t]
            new = out
            if a in r.writes:
                new = new - {r.writes[a]}
            if a in r.reads:
                new = new | {r.reads[a]}
            if new != live[a]:
                live[a] = new
                changed = True
    # Numeric WAD16 copies may enter at the condition below the loop body.
    # Guard the values used on that actual edge, including ones overwritten
    # at the head. Keep ordinary generation unchanged for the other game.
    entry = r.entry_addr if NUMERIC_SPRITE_LOOPS and r.em.dw.wad == 16 else r.lo
    r.live_in = live[entry]
    r.written = set(r.writes.values())


# ---------------------------------------------------------------------------------------------------------------
# Inference and emission: one walk over the region (Gen), run until the label states settle, then once more to emit C.

class Gen:
    def __init__(self, r, label_states, emitting):
        self.r, self.fn, self.em = r, r.fn, r.em
        self.label_states = label_states   # label -> State at its start (joined over the edges into it)
        self.emitting = emitting
        self.lines = []
        self.ntmp = 0
        self.changed = False
        self.memos = 0
        # Values the same at every execution in an entry, computed once after the guards (function_text puts them
        # there): instance fields no call in the region writes, and the weak copies of variables it never writes.
        self.pre = []
        self.pre_names = set()
        self.inl = None   # while a script call's body is being inlined: (Call address, operand stack before it)
        self.inl_chain = []   # the scripts being inlined, outermost first

    def hoist(self, name, lines):
        if name not in self.pre_names:
            self.pre_names.add(name)
            self.pre.extend(lines)
        return name

    def tmp(self):
        self.ntmp += 1
        return "t%d" % self.ntmp

    def e(self, s):
        if self.emitting:
            self.lines.append("    " + s)

    def vi(self, k):
        return self.r.keys.index(k)

    # ---[ edges ]---

    @staticmethod
    def canonical(st):
        """A state as a label holds it: its stack values in the merge RValues ms_<n>."""
        return State([Val("v", "ms_%d" % i, None, v.t, v.a0, False) for i, v in enumerate(st.stack)], dict(st.vars))

    def join_into(self, t, st):
        c = self.canonical(st)
        old = self.label_states.get(t)
        if old is None:
            self.label_states[t] = c
            self.changed = True
            return
        if len(old.stack) != len(c.stack):
            raise Unsupported("stack depth differs at %04X" % t)
        stack = [Val("v", o.c, None, join_t(o.t, n.t), o.a0 and n.a0, False) for o, n in zip(old.stack, c.stack)]
        vars = {}
        for k in set(old.vars) | set(c.vars):
            a, b = old.vars.get(k, (None, False)), c.vars.get(k, (None, False))
            vars[k] = (join_t(a[0], b[0]), a[1] and b[1])
        if [(v.t, v.a0) for v in stack] != [(v.t, v.a0) for v in old.stack] or vars != old.vars:
            self.label_states[t] = State(stack, vars)
            self.changed = True

    def merge_assign(self, st):
        """C statements putting a state's stack values in the merge RValues (on an edge into a label)."""
        out, tmps = [], []
        for i, v in enumerate(st.stack):
            if v.k == "v" and v.c == "ms_%d" % i:
                tmps.append(None)
                continue
            n = self.tmp()
            out.append("const RValue %s = %s;" % (n, as_rv(v)))
            tmps.append(n)
        for i, n in enumerate(tmps):
            if n is not None:
                out.append("ms_%d = %s;" % (i, n))
        return out

    def edge(self, t, st, cond=None):
        """A jump to t (a label of the region or an exit), taken when cond (a C expression) holds."""
        if not self.emitting:
            if t in self.r.labels:
                self.join_into(t, st)
            return
        body = (self.merge_assign(st) + ["goto T_%04X;" % t]) if t in self.r.labels else ["goto X_%04X;" % t]
        if cond is None:
            for b in body:
                self.e(b)
        else:
            self.e("if (%s) { %s }" % (cond, " ".join(body)))

    # ---[ handing over ]---

    def writeback_lines(self):
        if self.r.memory:
            return []   # stores already happened, including reference strengthening and release
        return ["if (dz%d) TYPED_WB(p%d, v%d);" % (i, i, i) for i, k in enumerate(self.r.keys) if k in self.r.written]

    def deopt(self, addr, stack, cond):
        """Hands over to the interpreter at addr when cond holds, stack being the operand stack there. Inside an inlined
        script, at the outermost inlined Call instead, with the stack before it: the script has no effect (see inline)."""
        if self.inl is not None:
            addr, stack = self.inl
        body = self.writeback_lines()
        for i, v in enumerate(stack):
            body.append("S[%d] = %s;" % (i, as_rv(v)))
        body.append("ctx->stack.top = (int32_t) (S - ctx->stack.slots) + %d;" % len(stack))
        body.append("TYPED_HANDOVER(aottag_, 0x%Xu);" % addr)
        body.append("return 0x%Xu;" % (addr << 1))
        self.e("if (__builtin_expect(%s, 0)) { %s }" % (cond, " ".join(body)))

    def ensure_num(self, v, addr, before):
        """v as a value known to be a plain number: tested (handing over at addr, with the stack before) when that
        isn't proven."""
        if v.num():
            return v
        if v.k != "v" or v.t == "B":
            raise Unsupported("arithmetic on a bool at %04X" % addr)
        self.deopt(addr, before, "!vmIsPlainNumber(%s.type)" % v.c)
        return Val("v", v.c, None, "N", v.a0, v.inv)

    def memo(self, lines, expr):
        """A real computed from values that don't change in the region: at its first execution in each entry."""
        m = self.memos
        self.memos += 1
        self.e("if (__builtin_expect(!mz%d, 0)) { %s mr%d = %s; mz%d = true; }" % (m, " ".join(lines + [""]), m, expr, m))
        n = self.tmp()
        self.e("const GMLReal %s = mr%d;" % (n, m))
        return n

    def drop(self, v):
        """A value popped and never used."""
        if v.k == "v" and v.t is None:
            self.e("if (%s.ownsReference) RValue_free(&%s);" % (v.c, v.c))
        elif IDENT.match(v.c):
            self.e("(void) %s;" % v.c)

    # ---[ the walk ]---

    def run(self, entry):
        r, fn = self.r, self.fn
        st = None
        if r.entry_addr != r.lo:
            self.e("goto T_%04X;" % r.entry_addr)
        for a in r.addrs:
            ins = fn.instrs[a]
            if a in r.labels:
                if st is not None:   # falling into the label
                    if self.emitting:
                        for l in self.merge_assign(st):
                            self.e(l)
                    else:
                        self.join_into(a, st)
                if a == r.entry_addr and a not in self.label_states:
                    self.label_states[a] = entry
                if a not in self.label_states:
                    st = None
                    continue
                st = self.label_states[a].copy()
                if self.emitting:
                    self.lines.append("T_%04X: ;" % a)
                self.deopt(a, st.stack, "TYPED_STRESS_HIT()")
            if st is None:
                continue
            try:
                st, falls = self.step(ins, st)
            except Unsupported as why:
                if why.addr is None:
                    why.addr = a
                raise
            if not falls:
                st = None
            elif ins.next >= r.hi or ins.next not in r.inside:
                self.edge(ins.next, st)
                st = None

    def step(self, ins, st):
        """Emits one instruction; returns (state after it, whether it falls through)."""
        r = self.r
        op, t1, t2, a = ins.op, ins.t1, ins.t2, ins.addr
        before = list(st.stack)
        S = st.stack
        e = self.e
        if self.inl is not None:
            pass
        elif a in r.sprite_getters:
            idx = r.sprite_getters[a]
            expr = 'typedBuiltinArrayReal(ctx, %du, -1)' % idx
            block = ['ctx->ip = 0x%X;' % ins.next]
            if 'instance' not in r.state_written:
                n = self.memo(block, expr)
                S.append(Val('r', n, T_VAR, inv=True))
            else:
                n = self.tmp()
                e('ctx->ip = 0x%X; const GMLReal %s = %s;' % (ins.next, n, expr))
                S.append(Val('r', n, T_VAR))
            return st, True
        elif a in r.accesses:
            return self.read_memory(ins, st, before), True
        elif a in r.fields:
            f = r.fields[a]
            if "instance" not in r.state_written:   # no call in the region moves the instance: read it once
                n = self.hoist("sf_" + f, ["    const GMLReal sf_%s = (GMLReal) self_->%s;" % (f, f)])
            else:
                n = self.tmp()
                e("const GMLReal %s = (GMLReal) self_->%s;" % (n, f))
            S.append(Val("r", n, T_VAR, inv=True))
            return st, True
        elif a in r.reads:
            k = r.reads[a]
            if r.memory:
                if k[0] == "s":
                    return self.read_memory(ins, st, before, (0, k)), True
                n = self.tmp()
                e("RValue %s; vmWeakCopyInto(&%s, p%d);" % (n, n, self.vi(k)))
                S.append(Val("v", n))
                return st, True
            t, a0 = st.vars[k]
            i = self.vi(k)
            n = self.tmp()
            inv = k not in r.written
            if t == "R" and a0:
                e("const GMLReal %s = v%d.real;" % (n, i))
                S.append(Val("r", n, T_VAR, inv=inv))
            elif t in ("I", "B") and a0:
                e("const int32_t %s = v%d.int32;" % (n, i))
                S.append(Val("i" if t == "I" else "b", n, T_VAR, inv=inv))
            elif inv:   # the weak copy pushVar makes, the same each time: made once (no generated code changes a copy)
                w = self.hoist("w%d" % i, ["    RValue w%d = v%d; w%d.ownsReference = false; w%d.gmlStackType = %d;"
                                           % (i, i, i, i, T_VAR)])
                num = None
                if t == "N" and a0:
                    num = self.hoist("nw%d" % i, ["    const GMLReal nw%d = AOT_NUM(&w%d);" % (i, i)])
                S.append(Val("v", w, None, t, a0, inv, num))
            else:   # the weak copy pushVar makes
                e("RValue %s = v%d; %s.ownsReference = false; %s.gmlStackType = %d;" % (n, i, n, n, T_VAR))
                S.append(Val("v", n, None, t, a0, inv))
            return st, True
        elif a in r.writes:
            k = r.writes[a]
            v = S.pop()
            if t2 in INT_TYPES:   # coerceIntStoreToRealInPlace
                if v.k == "i" or (v.k == "v" and v.t == "I"):
                    n = self.tmp()
                    e("const GMLReal %s = (GMLReal) %s;" % (n, as_int(v)))
                    v = Val("r", n, T_DOUBLE, inv=v.inv)
                elif v.k == "v" and v.t != "R":
                    n = self.tmp()
                    e("RValue %s = %s; coerceIntStoreToRealInPlace(&%s, %d);" % (n, v.c, n, t2))
                    v = Val("v", n, None, "R" if v.t == "N" else v.t, v.a0, v.inv)
            if r.memory:
                # Keep members/array slots coherent with scalar aliases. Use the actual slot, at the original store,
                # so weak struct/array/string values gain a reference and the overwritten value is released now.
                if k[0] == "s":
                    self.deopt(a, before, "p%d == nullptr" % self.vi(k))
                n = self.tmp()
                e("{ RValue %s = %s; storeStackValueIntoSlot(p%d, &%s); }" % (n, as_rv(v), self.vi(k), n))
                st.vars[k] = (None, False)
                return st, True
            if v.k == "v" and v.t is None:
                # storeStackValueIntoSlot copies strings, arrays, structs and methods: the interpreter does that.
                self.deopt(a, before, "(AOT_REF_TYPES >> (%s.type & 31u)) & 1u" % v.c)
            i = self.vi(k)
            e("v%d = %s; dz%d = true;" % (i, as_rv(v), i))
            st.vars[k] = (v.t, v.a0)
            return st, True
        if op == G.OP_PUSHI or (op == G.OP_PUSH and t1 == T_INT16):
            S.append(Val("i", "((int32_t) %d)" % ins.inst, T_INT16, inv=True, kv=ins.inst))
            return st, True
        if op == G.OP_PUSH and t1 == T_INT32:
            k = struct.unpack("<i", struct.pack("<I", ins.extra))[0]
            S.append(Val("i", "((int32_t) %dL)" % k if k != -2147483648 else "((int32_t) (-2147483647L - 1))", T_INT32, inv=True, kv=k))
            return st, True
        if op == G.OP_PUSH and t1 == T_DOUBLE:
            bits = struct.unpack_from("<Q", self.fn.blob, a + 4)[0]
            S.append(Val("r", "aotBitsDouble(0x%016XULL)" % bits, T_DOUBLE, inv=True))
            return st, True
        if op == G.OP_POPZ:
            self.drop(S.pop())
            return st, True
        if op in ARITH or op == G.OP_DIV:
            return self.arith(ins, st, before), True
        if op == G.OP_MOD and G.INLINE:
            b, x = S.pop(), S.pop()
            x = self.ensure_num(x, a, before)
            b = self.ensure_num(b, a, before)
            if not (b.k == "i" and b.c.startswith("((int32_t)") and b.c != "((int32_t) 0)"):
                self.deopt(a, before, "%s == (GMLReal) 0" % as_num(b))   # AOT_MOD's slow path: handleMod's error
            expr = "aotFmodOut(%s, %s)" % (as_num(x), as_num(b))
            if x.inv and b.inv:   # an out-of-line call: once an entry
                S.append(Val("r", self.memo([], expr), t2, inv=True))
                return st, True
            n = self.tmp()
            e("const GMLReal %s = %s;" % (n, expr))
            S.append(Val("r", n, t2))
            return st, True
        if op == G.OP_NEG and G.INLINE:
            x = self.ensure_num(S.pop(), a, before)
            n = self.tmp()
            e("const GMLReal %s = -%s;" % (n, as_num(x)))
            S.append(Val("r", n, t1, inv=x.inv))
            return st, True
        if op == G.OP_NOT and G.INLINE:
            x = S.pop()
            if x.t not in ("I", "B"):
                raise Unsupported("Not of a %s value at %04X" % (x.t, a))
            n = self.tmp()
            if t1 == T_BOOL:
                e("const int32_t %s = %s == 0 ? 1 : 0;" % (n, as_int(x)))
                S.append(Val("b", n, t1, inv=x.inv))
            else:
                e("const int32_t %s = ~%s;" % (n, as_int(x)))
                S.append(Val("i", n, t1, inv=x.inv))
            return st, True
        if op == G.OP_CONV:
            return self.conv(ins, st, before), True
        if op == G.OP_CMP:
            kind = (ins.word >> 8) & 0xFF
            b, x = S.pop(), S.pop()
            x = self.ensure_num(x, a, before)   # anything else: aotCmpStrings or handleCmp
            b = self.ensure_num(b, a, before)
            n = self.tmp()
            e("const int32_t %s = vmCmpHolds(vmCmpOrder(%s - %s), %d);" % (n, as_num(x), as_num(b), kind))
            if x.a0:
                S.append(Val("b", n, T_BOOL, inv=x.inv and b.inv))
            else:   # vmSetCmpBoolInPlace keeps the left operand's other fields
                m = self.tmp()
                e("RValue %s = %s; %s.int32 = %s; %s.type = RVALUE_BOOL; %s.gmlStackType = %d;" % (m, x.c, m, n, m, m, T_BOOL))
                S.append(Val("v", m, None, "B", False, x.inv and b.inv))
            return st, True
        if op in (G.OP_BT, G.OP_BF):
            x = S.pop()
            if x.k in ("i", "b"):
                c = "%s != 0" % x.c
            elif x.k == "r":
                c = "typedTruthR(%s, %d)" % (x.c, x.g)
            else:
                c = "aotTruth(&%s)" % x.c
            self.edge(ins.target(), st, c if op == G.OP_BT else "!(%s)" % c)
            return st, True
        if op == G.OP_B:
            self.edge(ins.target(), st)
            return st, False
        if op == G.OP_DUP:
            return self.dup(ins, st, before), True
        if op == G.OP_CALL:
            return self.call(ins, st, before), True
        raise Unsupported("opcode %#x at %04X" % (op, a))

    def read_memory(self, ins, st, before, access=None):
        """Array reads use the existing exact AOT helper. Other reads use the original VM handler and its lookup,
        static fallback and builtin rules. Only its consumed operands are spilled; all other temporaries stay in C.
        """
        count, key = access if access is not None else self.r.accesses[ins.addr]
        S, a = st.stack, ins.addr
        operands = S[-count:] if count else []
        n = self.tmp()
        if key is not None:
            self.e("RValue %s;" % n)
            if key[0] == "s":
                self.e("if (__builtin_expect(p%d != nullptr, 1)) {" % self.vi(key))
            if count:
                ix = self.tmp()
                self.e("{ const RValue %s = %s; aotArrayRead(ctx, &%s, p%d, &%s); }"
                       % (ix, as_rv(operands[-1]), n, self.vi(key), ix))
            else:
                self.e("vmWeakCopyInto(&%s, p%d);" % (n, self.vi(key)))
            if key[0] == "s":
                self.e("} else {")
        if key is None or key[0] == "s":
            # A cache miss need not mean the read fails: resolveVariableRead also handles other-without-an-other,
            # static members and missing values. Use exactly the original handler; reads do not insert map slots.
            # The generic stack analysis only consumes the extra receiver for a statically known -9. Refuse an
            # unexpected runtime -9 before the handler can pop a different number of operands.
            vt = ins.var_type()
            if vt in (G.VT_ARRAY, G.VT_STACKTOP) and self.em.dw.wad >= 17:
                marker = operands[-2] if vt == G.VT_ARRAY else operands[-1]
                expected = count == (3 if vt == G.VT_ARRAY else 2)
                if marker.kv is None:
                    m = self.tmp()
                    self.e("const RValue %s = %s;" % (m, as_rv(marker)))
                    self.deopt(a, before, "(vmToInt32Fast(&%s) == %d) != %s" % (m, G.STACKTOP, "true" if expected else "false"))
            if count:
                # A dynamic LOCAL scope could make resolveLocalSlot grow ctx->localVars, invalidating the local
                # pointers passed to this region. Known local arrays use the pre-resolved slot above; hand over
                # before any other read that could grow the frame. Struct/instance receivers resolve to their id.
                receiver = operands[0]
                if receiver.kv is None:
                    m = self.tmp()
                    self.e("const RValue %s = %s;" % (m, as_rv(receiver)))
                    self.deopt(a, before, "vmToInt32Fast(&%s) == %d" % (m, G.LOCAL))
                elif receiver.kv == G.LOCAL:
                    raise Unsupported("dynamic local read at %04X" % a)
            base = len(S) - count
            for i, v in enumerate(operands):
                self.e("S[%d] = %s;" % (base + i, as_rv(v)))
            self.e("ctx->stack.top = (int32_t) (S - ctx->stack.slots) + %d; ctx->ip = 0x%X;" % (len(S), ins.next))
            member_count = count if ((vt == G.VT_STACKTOP and count == 2)
                                     or (vt == G.VT_NORMAL and ins.inst == G.STACKTOP)) else 0
            if member_count:
                # Match executeLoop's struct fast path before the general scope lookup, including builtin-named
                # members and static fallback. The bridge returns false without changing the stack on a miss.
                self.e("if (!aotx_tryPushStructMember(ctx, &ctx->dataWin->vari.variables[%d], %d))"
                       % (ins.var_index(), member_count))
            if ins.op == G.OP_PUSHGLB:
                self.e("aotPushGlobalSlow(ctx, 0x%08Xu);" % ins.extra)
            elif ins.op == G.OP_PUSHBLTN:
                self.e("aotx_handlePushBltn(ctx, 0x%08Xu, ctx->bytecodeBase + 0x%X);" % (ins.word, a + 4))
            else:
                self.e("aotx_handlePush(ctx, 0x%08Xu, ctx->bytecodeBase + 0x%X, %d);" % (ins.word, a + 4, T_VAR))
            self.e("%s%s = S[%d];" % ("RValue " if key is None else "", n, base))
            self.e("ctx->stack.top = (int32_t) (S - ctx->stack.slots);")
            if key is not None:
                self.e("}")
        if count:
            del S[-count:]
        result = Val("v", n)
        self.deopt(ins.next, S + [result], "TYPED_EXCEPTION_PENDING()")
        S.append(result)
        return st

    def arith(self, ins, st, before):
        op, t1, t2, a = ins.op, ins.t1, ins.t2, ins.addr
        S, e = st.stack, self.e
        b, x = S.pop(), S.pop()
        x = self.ensure_num(x, a, before)   # anything else: handleAddSlow and the rest
        b = self.ensure_num(b, a, before)
        n = self.tmp()
        inv = x.inv and b.inv
        if op == G.OP_DIV:
            if t1 in (T_INT32, T_INT64) and t2 in (T_INT32, T_INT64):
                self.deopt(a, before, "%s == (GMLReal) 0" % as_num(b))   # vmDivNumbers' divide by zero error
            expr = "%s / %s" % (as_num(x), as_num(b))
        else:
            sym = ARITH[op][1]
            if x.t == "I" and b.t == "I":   # two int32s: an int32, wrapping
                expr = "(int32_t) ((uint32_t) %s %s (uint32_t) %s)" % (as_int(x), sym, as_int(b))
                if x.k == "i":
                    e("const int32_t %s = %s;" % (n, expr))
                    S.append(Val("i", n, t2, inv=inv))
                else:
                    e("RValue %s = %s; %s.int32 = %s; %s.gmlStackType = %d;" % (n, x.c, n, expr, n, t2))
                    S.append(Val("v", n, None, "I", x.a0, inv))
                return st
            if x.t != "R" and b.t != "R":   # int32 or real only at run time: vmArithNumbers itself
                e("RValue %s = %s; { const RValue b_ = %s; (void) vmArithNumbers(&%s, &b_, %s, %d); }"
                  % (n, as_rv(x), as_rv(b), n, ARITH[op][0], t2))
                S.append(Val("v", n, None, "N", x.a0, inv))
                return st
            expr = "%s %s %s" % (as_num(x), sym, as_num(b))
        if x.k != "v":
            e("const GMLReal %s = %s;" % (n, expr))
            S.append(Val("r", n, t2, inv=inv))
        else:   # vmArithNumbers and vmDivNumbers write over the left operand: its asset tag stays
            e("RValue %s = %s; %s.real = %s; %s.type = RVALUE_REAL; %s.gmlStackType = %d;" % (n, x.c, n, expr, n, n, t2))
            S.append(Val("v", n, None, "R", x.a0, inv))
        return st

    def conv(self, ins, st, before):
        t1, t2, a = ins.t1, ins.t2, ins.addr
        S, e = st.stack, self.e
        x = S.pop()
        key = (t2 << 4) | t1
        if key == 0x54:   # handleConv's Bool -> Variable: the value as it is, retagged
            if x.k == "v":
                n = self.tmp()
                e("RValue %s = %s; %s.gmlStackType = %d;" % (n, x.c, n, t2))
                S.append(Val("v", n, None, x.t, x.a0, x.inv))
            else:
                S.append(Val(x.k, x.c, T_VAR, inv=x.inv))
            return st
        if key not in (0x52, 0x45, 0x25, 0x02):
            raise Unsupported("Conv %02X at %04X" % (key, a))
        n = self.tmp()
        if x.k == "v":   # vmConvFast on a copy, else handleConv
            e("RValue %s = %s;" % (n, x.c))
            self.deopt(a, before, "!vmConvFast(&%s, 0x%02X)" % (n, key))
            e("%s.gmlStackType = %d;" % (n, t2))
            t = {0x52: x.t, 0x45: "B", 0x25: "I", 0x02: "R"}[key]
            S.append(Val("v", n, None, t, x.a0, x.inv))
            return st
        if key == 0x52:   # a pure retag
            S.append(Val(x.k, x.c, T_VAR, inv=x.inv))
        elif key == 0x45:
            c = x.c if x.k == "b" else ("%s > 0 ? 1 : 0" % x.c if x.k == "i" else "%s > (GMLReal) 0.5 ? 1 : 0" % x.c)
            e("const int32_t %s = %s;" % (n, c))
            S.append(Val("b", n, T_BOOL, inv=x.inv))
        elif key == 0x25:
            e("const int32_t %s = %s;" % (n, x.c if x.k != "r" else "(int32_t) %s" % x.c))
            S.append(Val("i", n, T_INT32, inv=x.inv))
        else:
            if x.k == "r":
                raise Unsupported("Conv.i.d of a real at %04X" % a)
            e("const GMLReal %s = (GMLReal) %s;" % (n, x.c))
            S.append(Val("r", n, T_DOUBLE, inv=x.inv))
        return st

    def dup(self, ins, st, before):
        fn, a = self.fn, ins.addr
        S = st.stack
        operand = ins.word & 0xFFFF
        count = fn.dup_slots.get(a)
        if ((ins.word & 0xFFFFF800) == 0x86058800 and self.em.dw.wad >= 17) or (operand & 0x8000 and self.em.dw.wad >= 17):
            raise Unsupported("Dup swap at %04X" % a)
        if not isinstance(count, int) or (ins.word & 0xFF00) != 0 or count > 4 or count != (operand & 0xFF) + 1 or count > len(S):
            raise Unsupported("Dup at %04X" % a)
        size = G.TYPE_NATIVE_SIZE.get(ins.t1, 16)
        tests = []
        for v in S[len(S) - count:]:
            if v.k == "v":
                tests.append("%s.ownsReference || gmlTypeNativeSize(%s.gmlStackType) != %d" % (v.c, v.c, size))
            elif G.TYPE_NATIVE_SIZE.get(v.g, 16) != size:
                raise Unsupported("Dup of a value of another size at %04X" % a)
        tests.append("(int32_t) (S - ctx->stack.slots) + %d > VM_STACK_SIZE" % (len(S) + count))
        self.deopt(a, before, " || ".join("(%s)" % t for t in tests))   # handleDup
        S.extend(S[len(S) - count:])
        return st

    def call(self, ins, st, before):
        em, r, e = self.em, self.r, self.e
        a = ins.addr
        idx = ins.extra
        name = em.dw.funcs[idx]["name"]
        argc = ins.word & 0xFFFF
        S = st.stack
        if em.script_callee(idx):
            return self.inline(ins, st, before)
        if name.startswith("@@") or G.builtin_audit().get(name, "not a builtin") is not None:
            raise Unsupported("call of %s at %04X" % (name, a))
        if argc > len(S):
            raise Unsupported("call with %d arguments at %04X" % (argc, a))
        math = G.inline_math().get(name) if G.INLINE else None
        if math is not None and math[0] == argc:
            args = [S.pop() for _ in range(argc)]   # args[0] is the first argument (pushed last)
            args = [self.ensure_num(v, a, before) for v in args]   # anything else: aotCallBuiltin
            f = math[1] if math[1] in MATH_INLINE else "aotTypedMath" + math[1][len("aotMath"):]
            expr = "%s(%s)" % (f, ", ".join(as_num(v) for v in args))
            if all(v.inv for v in args) and argc:
                S.append(Val("r", self.memo([], expr), T_VAR, inv=True))
                return st
            n = self.tmp()
            e("const GMLReal %s = %s;" % (n, expr))
            S.append(Val("r", n, T_VAR))
            return st
        effect = EFFECTS.get(name)
        min_args = EFFECTS_ARGS.get(name, 0)
        if name == 'draw_sprite_part' and NUMERIC_SPRITE_LOOPS and em.dw.wad == 16:
            # Public builtin audited at vm_builtins.c: it only submits a
            # renderer draw; negative subimage reads self.imageIndex. Keep
            # the actual bound builtin, conversions, order and return value.
            effect = ('effect', ('draw', 'instance'), ('target',), None)
            min_args = 8
        if effect is None:
            raise Unsupported("builtin %s at %04X is not in the typed effect table" % (name, a))
        if argc < min_args:   # the builtin reads that many arguments, however many it is given
            raise Unsupported("builtin %s at %04X with %d arguments" % (name, a, argc))
        kind, reads, writes, result = effect
        if self.inl is not None and kind != "pure":
            raise Unsupported("builtin %s in an inlined script at %04X changes state" % (name, a))
        args = [S.pop() for _ in range(argc)]
        r.builtins[idx] = name
        fv = "fb%d" % idx
        if r.memory:
            self.deopt(a, before, "%s == nullptr" % fv)
        memo = kind == "pure" and result == "R" and all(v.inv for v in args) and not (set(reads) & r.state_written)
        block = []
        scalar_part = (NUMERIC_SPRITE_LOOPS and em.dw.wad == 16
                       and name == 'draw_sprite_part' and argc == 8
                       and all(v.num() for v in args))
        if argc and not scalar_part:
            block.append("RValue a_[%d] = { %s };" % (argc, ", ".join(as_rv(v) for v in args)))
        block.append("ctx->ip = 0x%X;" % ((self.inl[0] if self.inl is not None else a) + 8))
        call = "%s(ctx, %s, %d)" % (fv, "a_" if argc else "nullptr", argc)
        free = ["if (a_[%d].ownsReference) RValue_free(&a_[%d]);" % (i, i) for i, v in enumerate(args) if v.k == "v" and v.t is None]
        if scalar_part:
            call = "Utold_scalarPart(ctx, %s)" % ", ".join(as_num(v) for v in args)
            free = []
        n = self.tmp()
        if result == "R":
            # RValue_makeReal, pushed typed Variable (stackPushTyped): a real scalar
            res = Val("r", n, T_VAR, inv=memo)
            if memo:   # (a pure builtin's result is a real that owns nothing: its arguments are numbers here)
                res.c = self.memo(block, "%s.real" % call)
            else:
                e("GMLReal %s; { %s %s = %s.real; %s }" % (n, " ".join(block), n, call, " ".join(free)))
        else:
            res = Val("v", n, None, None, False, False)
            e("RValue %s; { %s %s = %s; %s %s.gmlStackType = %d; }" % (n, " ".join(block), n, call, " ".join(free), n, T_VAR))
        self.deopt(a + 8, S + [res], "TYPED_EXCEPTION_PENDING()")   # AOT_CALL_BUILTIN's AOT_CHECK
        S.append(res)
        return st


# ---------------------------------------------------------------------------------------------------------------
# Inlined script calls (T2). A call of a script whose body the walk below can follow is compiled into the region:
# the body's own operations, with argument_count a constant (its tests decided here), argument[i] the caller's values,
# its locals C values, and its result pushed as handleCall/aotCallScript push it (stackPushTyped, Variable). Such a
# body reads no GML variable but its locals, arguments, builtin fields of self and BUILTIN_ARRAY_READS, writes none
# but its locals, and calls only "pure" builtins and other such scripts: running it changes nothing a GML program or
# the interpreter can see apart from the result. So wherever its code would hand over, the region hands over at the
# outermost inlined Call instead, with the caller's stack before it, and the interpreter makes the call from there.
# At entry the region checks that the call goes where the generator looked (funcCallCache: no builtin of the name,
# the code entry inlined, no forward or native version), which is what aotCallScript's direct path checks.

def callee_function(dw, name):
    key = (id(dw), name)
    if key not in _callees:
        e = dw.code_by_name[name]
        _callees[key] = (e, G.Function(dw, e, bytes(dw.data[e["abs"]:e["abs"] + e["length"]])))
    return _callees[key]


def builtin_forward(em, entry, fn):
    """The numeric/argument subset of vm.c findScriptForward. Leave these calls generated: their forwarding cache
    deliberately fails the inliner's guard, so admitting one would hand the rest of the event to the interpreter.
    This only refuses regions; the complete runtime guard remains authoritative for every admitted script.
    """
    if em.dw.wad < 17:
        return False
    values = []
    a = entry["offset"]
    while a + 8 <= entry["length"]:
        ins = fn.instrs.get(a)
        if ins is None:
            return False
        op, t1 = ins.op, ins.t1
        if op == G.OP_CALL:
            name = em.dw.funcs[ins.extra]["name"]
            if ((ins.word & 0xFFFF) != len(values) or name.startswith("@@")
                    or G.builtin_audit().get(name, "unknown") is not None):
                return False
            tail = fn.instrs.get(ins.next)
            after = fn.instrs.get(tail.next) if tail is not None else None
            return tail is not None and (tail.op == G.OP_RET or (
                tail.op == G.OP_POPZ and after is not None and after.op == G.OP_EXIT))
        if op == G.OP_CONV:
            if not values or (ins.t2 << 4 | t1) != {"int": 0x52, "real": 0x50}.get(values[-1]):
                return False
        elif len(values) == 16:
            return False
        elif (op == G.OP_PUSHI and t1 == T_INT16) or (op == G.OP_PUSH and t1 == T_INT32):
            values.append("int")
        elif op == G.OP_PUSH and t1 == T_DOUBLE:
            values.append("real")
        elif op == G.OP_PUSH and t1 == T_VAR and ins.var_type() == G.VT_NORMAL:
            v = em.dw.vars[ins.var_index()]
            if em.arg_number(v) is None or not (ins.inst == G.ARG or (
                    ins.inst == G.SELF and v["varID"] == G.VARIABLE_BUILTIN)):
                return False
            values.append("arg")
        else:
            return False
        a = ins.next
    return False


def _inline(self, ins, st, before):
    em, r = self.em, self.r
    dw = em.dw
    a, idx = ins.addr, ins.extra
    name = dw.funcs[idx]["name"]
    argc = ins.word & 0xFFFF
    S = st.stack
    if name not in dw.code_by_name or G.builtin_audit().get(name, "not a builtin") is None:
        raise Unsupported("script call %s at %04X: not a code entry of that name" % (name, a))
    if name in self.inl_chain or len(self.inl_chain) >= INLINE_DEPTH:
        raise Unsupported("script call %s at %04X: recursive or too deep to inline" % (name, a))
    if argc > len(S):
        raise Unsupported("script call with %d arguments at %04X" % (argc, a))
    entry, cfn = callee_function(dw, name)
    if cfn.stubs:
        raise Unsupported("script %s at %04X has code left to the interpreter" % (name, a))
    if builtin_forward(em, entry, cfn):
        raise Unsupported("script %s at %04X forwards to a builtin" % (name, a))
    if r.memory:
        self.deopt(a, before, script_guard(idx, entry["index"]))
    args = [S.pop() for _ in range(argc)]   # args[0] is the first argument (pushed last)
    args = [self.ensure_num(v, a, before) for v in args]   # anything else: made independent, owned
    outer = (self.inl, self.fn)
    if self.inl is None:
        self.inl = (a, list(before))
    self.fn = cfn
    self.inl_chain.append(name)
    try:
        res = self.inline_body(name, entry, cfn, args, st)
    finally:
        self.inl, self.fn = outer
        self.inl_chain.pop()
    r.scripts[idx] = (entry["index"], name)
    S.append(res)
    return st


def _retag(self, v):
    """A value as pushVar's weak copy or stackPushTyped(..., Variable) leaves it: gmlStackType Variable."""
    if v.k in ("r", "i", "b"):
        return Val(v.k, v.c, T_VAR, inv=v.inv, kv=v.kv)
    n = self.tmp()
    self.e("RValue %s = %s; %s.ownsReference = false; %s.gmlStackType = %d;" % (n, v.c, n, n, T_VAR))
    return Val("v", n, None, v.t, v.a0, v.inv, v.n)


CMP_HOLDS = {1: lambda o: o < 0, 2: lambda o: o <= 0, 3: lambda o: o == 0, 4: lambda o: o != 0, 5: lambda o: o >= 0,
             6: lambda o: o > 0}


def _inline_body(self, name, entry, cfn, args, st):
    dw, r = self.em.dw, self.r
    argc = len(args)
    cs = State([], st.vars)   # the callee's operand stack (it reads and writes none of the region's variables)
    S = cs.stack
    locals_ = {}              # varID -> value of each local the body has stored
    a = entry["offset"]
    for _ in range(INLINE_STEPS):
        ins = cfn.instrs.get(a)
        if ins is None or a not in cfn.depth:
            raise Unsupported("inlined script %s: no instruction at %04X" % (name, a))
        op, t1, inst = ins.op, ins.t1, ins.inst
        if op == G.OP_RET:
            if len(S) != 1 or not S[0].num():
                raise Unsupported("inlined script %s returns a value not known to be a number" % name)
            return self.retag(S.pop())
        if op in (G.OP_BT, G.OP_BF):
            x = S.pop()
            if x.kv is None:
                raise Unsupported("inlined script %s branches on a run-time value at %04X" % (name, a))
            a = ins.target() if (x.kv != 0) == (op == G.OP_BT) else ins.next
            continue
        if op == G.OP_B:
            a = ins.target()
            continue
        if op == G.OP_CMP and len(S) >= 2 and S[-1].kv is not None and S[-2].kv is not None:
            b, x = S.pop(), S.pop()   # two constants: vmCmpHolds(vmCmpOrder(x - b)), integers
            d = x.kv - b.kv
            h = 1 if CMP_HOLDS.get((ins.word >> 8) & 0xFF, lambda o: False)(0 if d == 0 else (-1 if d < 0 else 1)) else 0
            S.append(Val("b", "((int32_t) %d)" % h, T_BOOL, inv=True, kv=h))
            a = ins.next
            continue
        if op == G.OP_POP or op in (G.OP_PUSHLOC, G.OP_PUSHGLB, G.OP_PUSHBLTN) or (op == G.OP_PUSH and t1 == T_VAR):
            v = dw.vars[ins.var_index()]
            vt = ins.var_type()
            if op == G.OP_POP:
                if t1 != T_VAR or ins.t2 != T_VAR or vt != G.VT_NORMAL or inst != G.LOCAL or v["varID"] < 0:
                    raise Unsupported("inlined script %s writes %s at %04X" % (name, v["name"], a))
                x = S.pop()
                if not x.num():
                    raise Unsupported("inlined script %s stores a value not known to be a number at %04X" % (name, a))
                locals_[v["varID"]] = x
            elif vt == G.VT_NORMAL and inst == G.LOCAL and op in (G.OP_PUSHLOC, G.OP_PUSH) and v["varID"] >= 0:
                if v["varID"] not in locals_:
                    raise Unsupported("inlined script %s reads local %s before storing it" % (name, v["name"]))
                S.append(self.retag(locals_[v["varID"]]))
            elif (vt == G.VT_NORMAL and op in (G.OP_PUSHBLTN, G.OP_PUSH) and v["varID"] == G.VARIABLE_BUILTIN
                  and v["name"] == "argument_count" and inst in (G.SELF, G.BUILTIN, G.ARG)):
                S.append(Val("r", "((GMLReal) %d)" % argc, T_VAR, inv=True, kv=argc))   # RValue_makeReal(scriptArgCount)
            elif (vt == G.VT_NORMAL and op in (G.OP_PUSHBLTN, G.OP_PUSH) and self.em.arg_number(v) is not None
                  and ((op == G.OP_PUSH and inst == G.ARG and dw.wad >= 17)
                       or (inst == G.SELF and v["varID"] == G.VARIABLE_BUILTIN))):
                k = self.em.arg_number(v)
                if k >= argc:
                    raise Unsupported("inlined script %s reads argument%d of %d" % (name, k, argc))
                S.append(self.retag(args[k]))   # same weak Variable push as argument[k]
            elif (vt == G.VT_NORMAL and op in (G.OP_PUSHBLTN, G.OP_PUSH) and inst == G.SELF
                  and v["varID"] == G.VARIABLE_BUILTIN and v["name"] in G.SELF_FIELDS and self.em.arg_number(v) is None):
                f = G.SELF_FIELDS[v["name"]]
                r.need_self = True
                if "instance" not in r.state_written:
                    n = self.hoist("sf_" + f, ["    const GMLReal sf_%s = (GMLReal) self_->%s;" % (f, f)])
                else:
                    n = self.tmp()
                    self.e("const GMLReal %s = (GMLReal) self_->%s;" % (n, f))
                S.append(Val("r", n, T_VAR, inv=True))
            elif vt == G.VT_ARRAY and op == G.OP_PUSH and len(S) >= 2 and S[-1].kv is not None and S[-2].kv is not None:
                k, scope = S.pop().kv, S.pop().kv
                if v["name"] == "argument" and v["varID"] == G.VARIABLE_BUILTIN and scope == G.ARG and dw.wad >= 17:
                    if not 0 <= k < argc:
                        raise Unsupported("inlined script %s reads argument[%d] of %d" % (name, k, argc))
                    S.append(self.retag(args[k]))   # scriptArgs[k], ownsReference false, pushed Variable
                elif (v["varID"] == G.VARIABLE_BUILTIN and v["name"] in BUILTIN_ARRAY_READS and scope in (G.SELF, G.BUILTIN)
                      and dw.wad >= 17):
                    reads, _ = BUILTIN_ARRAY_READS[v["name"]]
                    expr = "typedBuiltinArrayReal(ctx, %du, %d)" % (ins.var_index(), k)
                    if set(reads) & r.state_written:
                        n = self.tmp()
                        self.e("const GMLReal %s = %s;" % (n, expr))
                        S.append(Val("r", n, T_VAR))
                    else:
                        S.append(Val("r", self.memo([], expr), T_VAR, inv=True))
                else:
                    raise Unsupported("inlined script %s reads %s[] at %04X" % (name, v["name"], a))
            else:
                raise Unsupported("inlined script %s reads %s at %04X" % (name, v["name"], a))
            a = ins.next
            continue
        _, falls = self.step(ins, cs)
        if not falls:
            raise Unsupported("inlined script %s: %#x at %04X" % (name, op, a))
        a = ins.next
    raise Unsupported("inlined script %s is longer than %d instructions" % (name, INLINE_STEPS))


Gen.inline = _inline
Gen.retag = _retag
Gen.inline_body = _inline_body


def infer(r):
    """Runs the walk until the label states settle (Unsupported for anything the tier can't type) and keeps them."""
    r.entry = {}
    for k in r.keys:
        if k in r.live_in and not r.memory:
            # Preserve the entry's runtime tag, including for variables written by the region itself. Converting an
            # int32 to real here would change int/int arithmetic (wrapping) and the tags a later store observes.
            r.entry[k] = ("N", True)
        else:
            r.entry[k] = (None, False)
    entry = State([], dict(r.entry))
    states = {}
    for _ in range(50):
        g = Gen(r, states, False)
        g.run(entry)
        if not g.changed:
            break
    else:
        raise Unsupported("types do not settle")
    for lab in r.labels:
        if lab not in states:
            raise Unsupported("label %04X not reached" % lab)
    r.states = states
    r.max_depth = max(len(s.stack) for s in states.values())


# ---------------------------------------------------------------------------------------------------------------
# Output

def call_site(em, r):
    """The lines the generated function gets just before the region's head (Emitter.gen, with em.cur_in the pointers
    good at the head)."""
    have = set(em.cur_in)
    look = frozenset(k[1:] for k in r.keys if k[0] == "s" and k[1:] not in have)
    args = ["ctx", "S"] + [em.slot_name(k[1:]) if k[0] == "s" else
                           "(ctx->scriptArgs != nullptr && ctx->scriptArgCount > %d ? &ctx->scriptArgs[%d] : nullptr)" % (k[1], k[1])
                           if k[0] == "a" else "&" + em.local(k[1]) for k in r.keys]
    valid = have | set(look)
    if G.TYPED == ALL:
        # gmlaot.py --typed all: the region is a weak reference, null when the module was linked without it
        # (platform/3ds/aot/aot_module_link.cmake), and then the generated code below runs as if it had handed over at
        # once, which it may always do.
        out = ["    /* typed region %04X-%04X, from %04X: %s (aot_typed_<n>.c, null if left out) */"
               % (r.lo, r.hi, r.entry_addr, r.name), "    if (%s != nullptr) {" % r.name]
    else:
        out = ["    /* typed region %04X-%04X, from %04X: %s (aot_typed.c) */" % (r.lo, r.hi, r.entry_addr, r.name), "    {"]
    if look:
        out.append("        " + em.refind(look, cold=False))
    out.append("        const uint32_t ty_ = %s(%s);" % (r.name, ", ".join(args)))
    for t in r.exits:
        again = frozenset(k for k in em.slot_in.get(t, frozenset()) if k not in valid)
        out.append("        if (ty_ == 0x%Xu) { %sgoto aoty_%04X; }" % ((t << 1) | 1, (em.refind(again, cold=False) + " ") if again else "", t))
    out.append("        if (ty_ != 0) AOT_DEOPT_AT(ty_ >> 1);")
    out.append("    }")
    return out


def prototype(r):
    return "uint32_t %s(%s)" % (r.name, ", ".join(["VMContext* ctx", "RValue* S"] + ["RValue* p%d" % i for i in range(len(r.keys))]))


def script_guard(idx, code_index):
    return ("ctx->funcCallCache[%d].builtin != nullptr || ctx->funcCallCache[%d].scriptCodeIndex != %d"
            " || !ctx->funcCallCache[%d].forwardChecked || ctx->funcCallCache[%d].forward != nullptr"
            % (idx, idx, code_index, idx, idx))


def inline_bindings(em, regions):
    """Separate typed-only callee descriptors; the ordinary table and chapter-module ABI do not change.

    Gen.inline records nested calls in the same region's scripts map. Pin every referenced definition in every
    reachable callee instruction (including a constant-folded branch), and all code bytes from its entry onward.
    This conservatively validates more than the selected inline path. A changed nested body or reference is just
    as invalid as a changed direct callee. Binding validation runs once per caller and context epoch, outside the
    typed guards; a region containing inlined calls only loads and compares the cached result at its entry.
    """
    scripts = {code_index: name for r in regions for code_index, name in r.scripts.values()}
    if not scripts:
        return ""
    out = ["AotInlineCache aotic_%d;" % em.k]
    entries = []
    for code_index, name in sorted(scripts.items()):
        entry, fn = callee_function(em.dw, name)
        variables, functions = set(), set()
        for ins in fn.instrs.values():
            if ins.op in (G.OP_PUSHLOC, G.OP_PUSHGLB, G.OP_PUSHBLTN, G.OP_POP) or (ins.op == G.OP_PUSH and ins.t1 == T_VAR):
                variables.add(ins.var_index())
            if ins.op == G.OP_CALL:
                functions.add(ins.extra)
        prefix = "aotid_%d_%d" % (em.k, code_index)
        if variables:
            out.append("static const AotVarRef %s_v[] = {" % prefix)
            for idx in sorted(variables):
                v = em.dw.vars[idx]
                out.append("    { %d, %d, %d, %s }," % (idx, v["varID"], v["instanceType"], G.c_string(v["name"])))
            out.append("};")
        if functions:
            out.append("static const AotFuncRef %s_f[] = {" % prefix)
            for idx in sorted(functions):
                out.append("    { %d, %s }," % (idx, G.c_string(em.dw.funcs[idx]["name"])))
            out.append("};")
        digest = G.fnv1a64(fn.blob[entry["offset"]:entry["length"]])
        entries.append("    { %d, %d, { %s, %d, %d, %d, 0x%016XULL, nullptr, %s, %d, %s, %d } },"
                       % (code_index, em.dw.wad, G.c_string(name), entry["offset"], entry["length"], entry["length"], digest,
                          prefix + "_v" if variables else "nullptr", len(variables),
                          prefix + "_f" if functions else "nullptr", len(functions)))
    out += ["const AotInlineBinding aotid_%d[] = {" % em.k] + entries + ["};"]
    return "\n".join(out)


def function_text(r, code_name):
    g = Gen(r, dict(r.states), True)
    g.run(r.states[r.lo])
    body = g.lines
    # Labels are "%04X": an address past 0xFFFF has five or more digits.
    used = set(re.findall(r"goto ([TX]_[0-9A-F]{4,});", "\n".join(body)))
    body = [l for l in body if not (re.match(r"^T_[0-9A-F]{4,}: ;$", l) and l.split(":")[0] not in used)]
    tag = G.c_string("%s@%04X" % (code_name, r.lo))
    out = ["// %s, bytecode %04X-%04X" % (code_name.replace("*/", ""), r.lo, r.hi),
           prototype(r) + " {",
           "#define aottag_ %s" % tag,
           "    TYPED_PREAMBLE();"]
    if r.scripts:
        # Check before preloading any callee reference (especially a builtin cache entry). Changed definitions can
        # include smaller VARI/FUNC tables. Once the region starts, no audited operation can rebind the context.
        out.append("    if (!aotic_%d.valid) TYPED_FAIL();" % r.em.k)
    # Guards, before anything happens.
    # Memory regions only require a slot when an instruction uses it. A cold branch's missing slot or unresolved
    # binding must not reject a whole loop. A missing slot hands over before its read/store (never insert here:
    # insertion could grow the map and invalidate the region's and caller's other cached pointers).
    nulls = ["p%d == nullptr" % i for i, k in enumerate(r.keys)
             if k[0] == "a" or (k[0] == "s" and not r.memory)]
    if nulls:
        out.append("    if (%s) TYPED_FAIL();" % " || ".join(nulls))
    for i, k in enumerate(r.keys):
        for j, k2 in enumerate(r.keys[:i]):
            if not r.memory and k[0] == "s" and k2[0] == "s" and k[2] == k2[2] and (k in r.written or k2 in r.written):
                out.append("    if (p%d == p%d) TYPED_FAIL();   /* one variable in two scopes */" % (i, j))
    if r.fields or r.need_self:
        out.append("    Instance* const self_ = (Instance*) ctx->currentInstance;")
        out.append("    if (self_ == nullptr || self_->objectIndex == STRUCT_OBJECT_INDEX) TYPED_FAIL();")
    for idx in sorted(r.builtins):
        out.append("    const BuiltinFunc fb%d = (BuiltinFunc) ctx->funcCallCache[%d].builtin;   /* %s */" % (idx, idx, r.builtins[idx]))
        if not r.memory:
            out.append("    if (fb%d == nullptr) TYPED_FAIL();" % idx)
        if NUMERIC_SPRITE_LOOPS and r.em.dw.wad == 16 and r.builtins[idx] == 'draw_sprite_part':
            out.append("    if (!Utold_scalarPartIsBuiltin(fb%d)) TYPED_FAIL();" % idx)
    for idx in sorted(r.scripts):   # where aotCallScript would call the script inlined: see Gen.inline
        if not r.memory:
            out.append("    if (%s) TYPED_FAIL();   /* %s */"
                       % (script_guard(idx, r.scripts[idx][0]), r.scripts[idx][1].replace("*/", "")))
    if not r.memory:
        for i, k in enumerate(r.keys):
            out.append("    RValue v%d = *p%d;   /* %s */" % (i, i, key_name(r, k)))
    for i, k in enumerate(r.keys):
        t = r.entry[k][0]
        if t == "R":
            out.append("    if (v%d.type != RVALUE_REAL || v%d.assetRefType != 0) TYPED_FAIL();" % (i, i))
        elif t == "N":
            out.append("    if ((v%d.type != RVALUE_REAL && v%d.type != RVALUE_INT32) || v%d.assetRefType != 0) TYPED_FAIL();" % (i, i, i))
    # The hoisted values the code uses (a weak copy may be wanted only for its number, or the reverse).
    text, keep = "\n".join(body), []
    for line in reversed(g.pre):
        name = re.search(r" (sf_\w+|w\d+|nw\d+) = ", line).group(1)
        if re.search(r"\b%s\b" % name, text):
            keep.append(line)
            text += "\n" + line
    out.extend(reversed(keep))
    for i, k in enumerate(r.keys):
        if k in r.written and not r.memory:
            out.append("    bool dz%d = false;" % i)
    for i in range(r.max_depth):
        out.append("    RValue ms_%d = { 0 };" % i)
    for m in range(g.memos):
        out.append("    bool mz%d = false; GMLReal mr%d = 0;" % (m, m))
    out.append("    TYPED_EVENT(aottag_, 0);")
    out.extend(body)
    for t in r.exits:
        if "X_%04X" % t not in used:
            continue
        out.append("X_%04X:" % t)
        out.extend("    " + l for l in g.writeback_lines())
        out.append("    TYPED_EVENT(aottag_, 3);")
        out.append("    return 0x%Xu;" % ((t << 1) | 1))
    out += ["#undef aottag_", "}"]
    return "\n".join(out)


def key_name(r, k):
    dw = r.em.dw
    if k[0] == "l":
        return "local " + dw.vars[k[1]]["name"].replace("*/", "")
    if k[0] == "a":
        return "argument%d" % k[1]
    scope = {G.SELF: "self", G.OTHER: "other", G.GLOBAL: "global"}[k[1]]
    for v in dw.vars:
        if v["varID"] == k[2] and v["instanceType"] != G.LOCAL:
            return "%s.%s" % (scope, v["name"].replace("*/", ""))
    return "%s.var%d" % (scope, k[2])


ALL = "all"   # gmlaot.py --typed all: every code entry the run compiles is a candidate


def load_selection(path):
    """(folder, code entry) pairs of a typed selection list: "<data.win folder> <code entry>" per line, # comments;
    the folder "root" matches any data.win. The value "all" (ALL) names no file and selects every code entry the run
    compiles: the regions then come from the player's data alone, each kept only if the rules above accept it."""
    if path == ALL:
        return ALL
    out = set()
    for line in open(path, encoding="utf-8"):
        f = line.split("#")[0].split()
        if len(f) == 2:
            out.add((f[0], f[1]))
        elif len(f) == 1:
            out.add(("root", f[0]))
    return out


def selected(sel, folder, name):
    if sel == ALL:
        return True
    return (folder, name) in sel or ("root", name) in sel
