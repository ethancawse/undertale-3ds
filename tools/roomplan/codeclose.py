"""UNDERTALE's in-play code closure for the room plan: the bytecode a scene can run in play that neither its record nor
the room change stages (Old 3DS; codeclose, ruling H1v 2026-10-28).

Read-only. Numbers only leave this module. On the Old 3DS the bytecode sits in an SD cache file in 4 KB groups
(engine data_win.c DataWin_moveBytecodeToFile: blobs in file order, a new group when a blob would end more than
CODE_GROUP_BYTES past the group's start), read on first use into an arena that never frees. The room change reads a
scene's planned code (z_roomload.c rlReadCode, then z_utstall.c usReadCode) and the placed objects' events; code first
needed later blocks the game thread in play (about 8.5 ms a read plus 0.145 ms a KB on the o3ds). Most of that is code
of objects created in play: the cast roll's monster bodies, the Ruins' faded music and Flowey's stalker.

Rule H1v, per scene: seeds = what the room change has today (the placed objects' events but Destroy, CleanUp and
PreCreate; room and instance creation code; the plan scene's code; all the record's code; a battle group: its plan
code and record only). Breadth first from the seeds in index order, an instance_create / instance_create_depth /
instance_change whose object is a constant, or a variable the same object (or a parent) only ever stores constants
into, adds every event of that object and its parents. Script calls are not followed, and nothing is followed out of
the hub scripts (HUBS: dispatchers over every battle, text and save). Each group the closure reaches that the seeds'
groups do not cover adds its lowest code index to the scene's code, until the scene's extras reach CAP_BYTES (a later
smaller group still fits). What chains of Create events reach comes first (a monster's body, drawn every frame), then
what other events create on some branch (attacks): the cap and the room load's deadline (z_utstall.c usReadCode reads
the list in order) cut the latter first. Rooms with no record scene get a code-only scene (no tiles) when the closure
adds any. (Rev 2, argclosereview F4/F5: stores read in the VM's pop order, only plain variables' stores count; the
Create-first order.)

Any data.win this module can't read (not GameMaker 1.x bytecode 15+, a chunk missing) gives no closure: the plan is
built as before.
"""
import os
import struct
import sys
from collections import deque

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'aot'))
import gmlaot as g  # noqa: E402

CODE_GROUP_BYTES = 4096    # data_win.c CODE_GROUP_BYTES
CAP_BYTES = 256 * 1024     # a scene's extras at most (ruling 2026-10-28: a guard, the largest H1v scene is 268 KB)
EVENT_CREATE, EVENT_DESTROY, EVENT_CLEANUP, EVENT_PRECREATE = 0, 1, 12, 14
SKIP_CHANGE = (EVENT_DESTROY, EVENT_CLEANUP, EVENT_PRECREATE)   # z_roomload does not stage these
OBJT_EVENT_TYPE_COUNT = 15
SPAWN_ARG = {'instance_create': 2, 'instance_change': 0, 'instance_create_depth': 3}
HUBS = ('scr_battlegroup', 'textdata_en', 'textdata_ja', 'SCR_TEXT', 'scr_gettext', 'scr_papcall', 'scr_torcall',
        'scr_itemuseb', 'SCR_GAMESTART', 'scr_load', 'scr_save', 'scr_saveprocess', 'scr_namingscreen')
VARTYPE_ARRAY, VARTYPE_STACKTOP, VARTYPE_NORMAL = 0x00, 0x80, 0xA0   # vm.h
WALK_VISITS = 8            # an address walked more often than this is left (loops converge well before)


class CodeClosure:
    def __init__(self, path):
        self.ok = False
        try:
            self._parse(path)
            self.ok = True
        except (SystemExit, ValueError, KeyError, IndexError, struct.error, UnicodeError, g.Reject):
            pass

    # --- data.win
    def _parse(self, path):
        dw = g.DataWin(path)
        dw.patch_operands()
        self.d = d = dw.data

        def plist(at):
            n = struct.unpack_from('<I', d, at)[0]
            return struct.unpack_from('<%dI' % n, d, at + 4)

        def chunk(name):
            return plist(dw.chunks[name][0])

        def i32(p):
            return struct.unpack_from('<i', d, p)[0]

        self.parent_by_obj, self.events_by_obj, self.event_type = [], [], {}
        for p in chunk('OBJT'):
            self.parent_by_obj.append(i32(p + 24))
            nverts = i32(p + 64)
            ev = {}
            for t, tp in enumerate(plist(p + 64 + 4 + 12 + max(nverts, 0) * 8)[:OBJT_EVENT_TYPE_COUNT]):
                for ep in plist(tp):
                    for ap in plist(ep + 4):
                        cid = i32(ap + 32)
                        if cid >= 0:
                            ev.setdefault(t, []).append(cid)
                            self.event_type.setdefault(cid, t)
            self.events_by_obj.append(ev)
        self.cc_by_room, self.insts_by_room = [], []
        for p in chunk('ROOM'):
            self.cc_by_room.append(i32(p + 32))
            self.insts_by_room.append([(i32(q + 8), i32(q + 16)) for q in plist(struct.unpack_from('<I', d, p + 48)[0])])
        scripts = {}
        for p in chunk('SCPT'):
            if p:
                scripts[dw.string(struct.unpack_from('<I', d, p)[0])] = i32(p + 4)
        self.hubs = {scripts[n] for n in HUBS if scripts.get(n, -1) >= 0}
        # The engine's groups (data_win.c): distinct blobs by offset (the longest length), in file order.
        code = dw.code
        size = {}
        for c in code:
            if c is not None and c['length']:
                size[c['abs']] = max(size.get(c['abs'], 0), c['length'])
        blobs = sorted(size.items())
        group, start, self.group_size, blob_group = 0, {}, {}, {}
        for i, (a, n) in enumerate(blobs):
            if a + n - blobs[group][0] > CODE_GROUP_BYTES:
                group = i
            blob_group[a] = group
            start.setdefault(group, a)
            self.group_size[group] = max(self.group_size.get(group, 0), a + n - start[group])
        self.code_group = [blob_group[c['abs']] if c is not None and c['length'] else None for c in code]
        self.group_rep = {}
        for c, grp in enumerate(self.code_group):
            if grp is not None and grp not in self.group_rep:
                self.group_rep[grp] = c
        # Each entry's spawn targets and constant stores; the object that owns it.
        funcs = [f['name'] for f in dw.funcs]
        vars_ = [v['name'] for v in dw.vars]
        self.spawns, self.var_spawns, self.stores = [], [], []
        for c in code:
            if c is None or not c['length']:
                r = {'spawns': set(), 'var_spawns': set(), 'stores': {}}
            else:
                r = walk(d, c['abs'], c['length'], funcs, vars_)
            self.spawns.append(r['spawns'])
            self.var_spawns.append(r['var_spawns'])
            self.stores.append(r['stores'])
        self.owner = {}
        for o in range(len(self.events_by_obj)):
            for cc in self.object_codes(o, parents=False):
                self.owner[cc] = o

    def object_codes(self, obj, skip=(), parents=True):
        """Every event code entry of obj and its parents (event types in skip aside), in data.win order."""
        out, seen = [], set()
        while 0 <= obj < len(self.events_by_obj) and obj not in seen and len(seen) < 32:
            seen.add(obj)
            for t, codes in self.events_by_obj[obj].items():
                if t not in skip:
                    out += codes
            if not parents:
                break
            obj = self.parent_by_obj[obj]
        return out

    # --- the closure
    def change_codes(self, room):
        """What the room change stages for a room whatever the plan says: its placed objects' events as z_roomload
        takes them, room and instance creation code."""
        out = set()
        if not 0 <= room < len(self.insts_by_room):
            return out
        for o in sorted({o for o, _cc in self.insts_by_room[room] if o >= 0}):
            out.update(self.object_codes(o, skip=SKIP_CHANGE))
        if self.cc_by_room[room] >= 0:
            out.add(self.cc_by_room[room])
        out.update(cc for _o, cc in self.insts_by_room[room] if cc >= 0)
        return out

    def _targets(self, c):
        targets = set(self.spawns[c])
        if self.var_spawns[c]:
            o = self.owner.get(c)
            family = self.object_codes(o) if o is not None else [c]
            for name in self.var_spawns[c]:
                for cc in family:
                    targets.update(x for x in self.stores[cc].get(name, ()) if 0 <= x < len(self.events_by_obj))
        return targets

    def ordered(self, seeds):
        """The code the seeds reach (seeds aside): first what chains of Create events reach (an object created in a
        Create is there whenever its creator is: a monster's body), then the rest (what Step, Alarm or other events
        create on some branch: attacks), each in breadth-first order."""
        every = self._reach(seeds, False)
        sure = set(self._reach(seeds, True))
        return [c for c in every if c in sure] + [c for c in every if c not in sure]

    def _reach(self, seeds, creates_only):
        seen, order, objs = set(seeds), [], set()
        q = deque(sorted(seeds))
        while q:
            c = q.popleft()
            if not 0 <= c < len(self.spawns) or c in self.hubs:
                continue
            if creates_only and c not in seeds and self.event_type.get(c, EVENT_CREATE) != EVENT_CREATE:
                continue
            for o in sorted(self._targets(c)):
                if o in objs or not 0 <= o < len(self.events_by_obj):
                    continue
                objs.add(o)
                for cc in self.object_codes(o):
                    if cc not in seen:
                        seen.add(cc)
                        order.append(cc)
                        q.append(cc)
        return order

    def groups(self, codes):
        cg = self.code_group
        return {cg[c] for c in codes if 0 <= c < len(cg) and cg[c] is not None}

    def extras(self, room, plan_code, record_code, battle=False, cap=None):
        """The code indices to add to a scene (one for each group, in the closure's order) and their bytes. plan_code: the scene's
        planned code (dframe >= 0); record_code: every code the record saw (the room change's too); battle: a battle
        group's scene (its seeds are its own code, not the battle room's placed objects)."""
        have = self.change_codes(room) | set(plan_code) | set(record_code)
        staged = self.groups(have)
        seeds = set(plan_code) | set(record_code) if battle else have
        added, total, cap = [], 0, CAP_BYTES if cap is None else cap
        for c in self.ordered(seeds):
            grp = self.code_group[c] if 0 <= c < len(self.code_group) else None
            if grp is None or grp in staged:
                continue
            staged.add(grp)
            if total + self.group_size[grp] > cap:
                continue
            added.append(grp)
            total += self.group_size[grp]
        return [self.group_rep[grp] for grp in added], total


def walk(d, base, length, funcs, vars_):
    """Abstract stack walk over one code entry: the objects its instance_create / instance_create_depth /
    instance_change calls name as constants (spawns), the variables they name instead (var_spawns), and the constants
    each variable is assigned (stores). A stack slot is ('c', frozenset of ints), ('v', name), ('a', k) or None."""
    blob = bytes(d[base:base + length])
    states = {0: ()}
    work = [0]
    out = {'spawns': set(), 'var_spawns': set(), 'stores': {}}
    visits = {}

    def pops(n, stack):
        for _ in range(n):
            if stack:
                stack.pop()

    while work:
        addr = work.pop()
        stack = list(states[addr])
        visits[addr] = visits.get(addr, 0) + 1
        if visits[addr] > WALK_VISITS:
            continue
        while addr + 4 <= length:
            try:
                ins = g.Instr(blob, addr)
            except g.Reject:
                break
            op, branch, stop = ins.op, None, False
            if op in (g.OP_NEG, g.OP_NOT):
                if stack:
                    top = stack.pop()
                    stack.append(('c', frozenset(-x for x in top[1])) if op == g.OP_NEG and top and top[0] == 'c'
                                 else None)
            elif op in (g.OP_MUL, g.OP_DIV, g.OP_REM, g.OP_MOD, g.OP_ADD, g.OP_SUB, g.OP_AND, g.OP_OR, g.OP_XOR,
                        g.OP_SHL, g.OP_SHR, g.OP_CMP):
                b = stack.pop() if stack else None
                a = stack.pop() if stack else None
                v = None
                if a and b and a[0] == 'c' and b[0] == 'c' and len(a[1]) * len(b[1]) <= 16 and op in (g.OP_ADD, g.OP_SUB):
                    v = ('c', frozenset((x + y) if op == g.OP_ADD else (x - y) for x in a[1] for y in b[1]))
                stack.append(v)
            elif op == g.OP_POP:
                # vm.c handlePop: a simple (pop.v.v) array or stacktop store has its value deepest, the index and
                # instance on top; a compound one has the value on top.
                vt = (ins.extra >> 24) & 0xF8 if ins.extra is not None else VARTYPE_NORMAL
                under = 2 if vt == VARTYPE_ARRAY else 1 if vt == VARTYPE_STACKTOP else 0
                if under and ins.t1 == g.T_VAR:
                    pops(under, stack)
                    val = stack.pop() if stack else None
                else:
                    val = stack.pop() if stack else None
                    pops(under, stack)
                # Only a plain variable's stores are its constants (not an array element's, not another instance's).
                if ins.extra is not None and vt == VARTYPE_NORMAL:
                    vi = ins.var_index()
                    name = vars_[vi] if 0 <= vi < len(vars_) else None
                    if name is not None and val and val[0] == 'c':
                        out['stores'].setdefault(name, set()).update(val[1])
            elif op == g.OP_PUSHI:
                stack.append(('c', frozenset([ins.inst])))
            elif op == g.OP_DUP:
                n = (ins.word & 0xFF) + 1
                stack.extend(stack[-n:] if len(stack) >= n else [None] * n)
            elif op == g.OP_RET:
                pops(1, stack)
                stop = True
            elif op == g.OP_EXIT:
                stop = True
            elif op == g.OP_POPZ:
                pops(1, stack)
            elif op == g.OP_B:
                branch, stop = ins.target(), True
            elif op in (g.OP_BT, g.OP_BF):
                pops(1, stack)
                branch = ins.target()
            elif op == g.OP_PUSHENV:
                pops(1, stack)   # its operand jumps to the popenv of an empty with: fall through
            elif op in (g.OP_PUSH, g.OP_PUSHLOC, g.OP_PUSHGLB, g.OP_PUSHBLTN):
                if ins.t1 == g.T_VAR:
                    vt = (ins.extra >> 24) & 0xF8
                    pops(2 if vt == 0x00 else 1 if vt == 0x80 else 0, stack)
                    vi = ins.var_index()
                    name = vars_[vi] if 0 <= vi < len(vars_) else None
                    if vt == 0x00:
                        stack.append(None)
                    elif name and name.startswith('argument') and name[8:].isdigit():
                        stack.append(('a', int(name[8:])))
                    else:
                        stack.append(('v', name))
                elif ins.t1 == g.T_INT16:
                    stack.append(('c', frozenset([ins.inst])))
                elif ins.t1 == g.T_INT32 and ins.extra is not None:
                    stack.append(('c', frozenset([ins.extra - (1 << 32) if ins.extra >= 1 << 31 else ins.extra])))
                elif ins.t1 == g.T_DOUBLE:
                    dv = struct.unpack_from('<d', blob, addr + 4)[0]
                    stack.append(('c', frozenset([int(dv)])) if dv == int(dv) and abs(dv) < 1e9 else None)
                else:
                    stack.append(None)
            elif op == g.OP_CALL:
                argc = ins.word & 0xFFFF
                fi = ins.extra
                name = funcs[fi] if fi is not None and 0 <= fi < len(funcs) else None
                args = [stack[-(k + 1)] if len(stack) > k else None for k in range(argc)]
                del stack[max(0, len(stack) - argc):]
                stack.append(None)
                k = SPAWN_ARG.get(name)
                if k is not None and argc > k and args[k]:
                    if args[k][0] == 'c':
                        out['spawns'].update(args[k][1])
                    elif args[k][0] == 'v':
                        out['var_spawns'].add(args[k][1])
            if branch is not None and 0 <= branch < length:
                st = tuple(stack)
                prev = states.get(branch)
                if prev is None:
                    states[branch] = st
                    work.append(branch)
                elif prev != st and len(prev) == len(st):
                    merged = tuple(p if p == s else None for p, s in zip(prev, st))
                    if merged != prev:
                        states[branch] = merged
                        work.append(branch)
            if stop:
                break
            addr = ins.next
            st = tuple(stack)
            prev = states.get(addr)
            if prev is not None:
                if prev == st or len(prev) != len(st):
                    break
                merged = tuple(p if p == s else None for p, s in zip(prev, st))
                if merged == prev:
                    break
                states[addr] = merged
                stack = list(merged)
            else:
                states[addr] = st
    return out
