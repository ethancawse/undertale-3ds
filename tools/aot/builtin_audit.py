#!/usr/bin/env python3
"""Which of the engine's builtin functions leave cached variable slots alone.

gmlaot.py keeps a pointer to each self/other/global variable it has looked up (see "Cached variable slots" in
engine/src/aot/vm_aot.h) and drops them all at anything that could insert a variable into an instance's map, change
which instance a scope refers to, or run GML (which can do either). A builtin call is one of those unless the builtin
can't reach any of that. This module works that out from the engine's sources, so the answer follows the engine:

  - every function defined in engine/src (and engine/src/physics) is read, with comments and string literals removed;
  - a function refers to every function whose name appears in its body other than as a struct field, called or not
    (so callbacks passed to qsort and the like count);
  - a builtin is pure when, following those references, it can't reach any of ROOTS: the functions that insert into a
    variable map, copy an instance's variables, create (with its events), copy or destroy an instance, change rooms,
    or run code or events. Making a struct isn't one: a new map moves no slot of an existing one.

That over-approximates (any mention counts, and same-named static functions are merged), which only ever makes a
builtin impure. A builtin whose body isn't found (one defined by a macro, or registered from elsewhere) is impure.
What it can't see is a call through a function pointer stored in a struct: those are the platform's renderer, audio,
file system and window callbacks, which run no GML and touch no instance.

Usage: builtin_audit.py [name ...]   prints, for each builtin (all registered ones by default), pure or why not.
"""
import glob
import os
import re
import sys

ENGINE_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "engine", "src")

ROOTS = frozenset((
    # inserting into a variable map
    "Instance_setSelfVar", "IntRValueHashMap_getOrInsertUndefined", "IntRValueHashMap_getOrInsertUndefinedFast",
    "IntRValueHashMap_clear", "vmScopeVarWriteSlot", "Instance_copyFields", "VM_structSet", "VM_structSetAndFreeVal",
    # creating, copying and destroying instances
    "Runner_createInstance", "Runner_createInstanceWithDepth", "Runner_createInstanceWithLayer",
    "Runner_copyInstance", "Runner_destroyInstance", "Runner_cleanupDestroyedInstances", "Runner_handlePendingRoomChange",
    # running code
    "VM_callCodeIndex", "VM_executeCode", "runScriptFrame", "runScriptFrameWith", "executeLoop",
    "Runner_executeEvent", "Runner_executeEventForAll", "Runner_executeEventFromObject",
))

_DEF = re.compile(r'^(?:static\s+)?(?:inline\s+)?(?:NOINLINE\s+)?(?:__attribute__\(\([^)]*\)\)\s+)?'
                  r'[A-Za-z_][A-Za-z_0-9\s\*]*?\b([A-Za-z_][A-Za-z_0-9]*)\s*\([^;]*$')
_STRIP = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)
_NAME = re.compile(r'(?<![.>\w])([A-Za-z_][A-Za-z_0-9]*)\b')   # not a struct field (a.x, a->x)
_REGISTER = re.compile(r'VM_registerBuiltin\(ctx,\s*"([^"]+)",\s*([A-Za-z_0-9]+)\)')
_KEYWORDS = frozenset(("if", "while", "for", "switch", "return", "sizeof"))


def _sources(src):
    files = glob.glob(os.path.join(src, "*.c")) + glob.glob(os.path.join(src, "*.h")) + \
        glob.glob(os.path.join(src, "physics", "*.c"))
    # Not the desktop drivers: their main loops (loop.c's loop, which runs whole frames) and bench script would make
    # every builtin with a local called "loop" reach everything.
    return [f for f in sorted(files) if os.path.basename(f) not in ("bench_script.c", "loop.c")]


def _functions(files):
    """{name: [body, ...]}: a body runs from the definition's line to the first line that is just "}"."""
    funcs = {}
    for path in files:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.read().split("\n")
        i = 0
        while i < len(lines):
            line = lines[i]
            m = _DEF.match(line)
            if m and not line[:1].isspace() and not line.startswith(("#", "//", "}")) and m.group(1) not in _KEYWORDS:
                j = i
                while j < len(lines) and "{" not in lines[j] and ";" not in lines[j]:
                    j += 1
                if j < len(lines) and "{" in lines[j]:
                    k = j
                    while k < len(lines) and lines[k].rstrip() != "}" and not (k == j and lines[k].rstrip().endswith("}")):
                        k += 1
                    funcs.setdefault(m.group(1), []).append("\n".join(lines[i:k + 1]))
                    i = k + 1
                    continue
            i += 1
    return funcs


def _registered(files):
    """{builtin name: the C function VM_registerBuiltin registers for it}."""
    registered = {}
    for path in files:
        with open(path, encoding="utf-8", errors="replace") as f:
            for m in _REGISTER.finditer(f.read()):
                registered[m.group(1)] = m.group(2)
    return registered


def builtin_body_hashes(src=ENGINE_SRC):
    """{builtin name: SHA-1 (12 hex digits) of its function's body, comments and string literals removed and
    whitespace collapsed}, for builtins whose function has exactly one body. gmlaot.py inlines a copy of a few math
    builtins (INLINE_MATH) only while the engine's body still hashes to what the copy was made from."""
    import hashlib
    files = _sources(src)
    funcs = _functions(files)
    result = {}
    for name, fn in _registered(files).items():
        bodies = funcs.get(fn, [])
        if len(bodies) == 1:
            text = " ".join(_STRIP.sub(" ", bodies[0]).split())
            result[name] = hashlib.sha1(text.encode("utf-8")).hexdigest()[:12]
    return result


def audit(src=ENGINE_SRC):
    """{builtin name: None when pure, else the chain of functions that reaches a root (or why it can't be read)}."""
    files = _sources(src)
    funcs = _functions(files)
    refs = {n: set(_NAME.findall(_STRIP.sub(" ", "\n".join(bodies)))) - {n} for n, bodies in funcs.items()}
    reach = set(ROOTS)
    changed = True
    while changed:
        changed = False
        for n, r in refs.items():
            if n not in reach and r & reach:
                reach.add(n)
                changed = True
    registered = _registered(files)
    result = {}
    for name, fn in registered.items():
        if fn not in funcs:
            result[name] = "%s: no body found" % fn
        elif fn in reach:
            chain, cur, seen = [fn], fn, {fn}
            while cur not in ROOTS:
                nxt = sorted(c for c in refs.get(cur, ()) if c in reach and c not in seen)
                if not nxt:
                    break
                cur = nxt[0]
                seen.add(cur)
                chain.append(cur)
            result[name] = " > ".join(chain)
        else:
            result[name] = None
    return result


def pure_builtins(src=ENGINE_SRC):
    return frozenset(n for n, why in audit(src).items() if why is None)


if __name__ == "__main__":
    res = audit()
    names = sys.argv[1:] or sorted(res)
    for name in names:
        why = res.get(name, "not registered")
        print("%-36s %s" % (name, "pure" if why is None else why))
    if not sys.argv[1:]:
        print("%d of %d builtins pure" % (sum(1 for w in res.values() if w is None), len(res)), file=sys.stderr)
