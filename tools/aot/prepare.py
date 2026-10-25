#!/usr/bin/env python3
"""Generates a console's AOT code from the committed selection list and your game data (platform/3ds/build.sh aot).

usage: prepare.py --game undertale|deltarune --target o3ds|n3ds --out DIR [--data DIR] [--chapters] [--check-only] [--typed LIST|all]

The game folder is --data, else $CTRPORT_UNDERTALE_DATA / $CTRPORT_DELTARUNE_DATA, else the game's folder in a
Steam library (steamapps/common/UNDERTALE or DELTARUNE). The PC install and the copy for the SD card have the same
layout, so either works. Code is generated into DIR with gmlaot.py --selection tools/aot/selection/<game>.txt
--target <target>; when the list, the generator's files and arguments, the target, what gmlaot.py reads from the
engine sources and the data.win files are the same as last time (DIR/aot_inputs.txt), nothing is done. When tools/aot/selection/<game>-layout.txt is there (tools/aot/select.py layout), it says the order the
functions go in (gmlaot.py --layout).

--late-numeric ENTRY explicitly generates an experimental separate numeric entry for single-table UNDERTALE.
It is off by default; its generated caller and regions stay in the private output directory.

--chapters (DELTARUNE): one set of code for each chapter instead of one for the whole game, each from that chapter's
part of tools/aot/selection/deltarune-parts.txt and each with the whole budget, into DIR/<chapter folder>/.
DIR/aot_modules.txt lists them. The 3DS build links each into a module of its own and loads the chapter's module when
the chapter starts (platform/3ds/build.sh aot=chapters, docs/aot.md).

New-only lists, when present, are <game>-n3ds.txt or deltarune-parts-n3ds.txt.
The latter uses a 15 MB selection budget at 85 B/instruction; the runtime's
12 MiB module reservation is independent. Old targets retain their original lists.

Exit status: 0 when DIR holds the code, 3 when no game data was found (the build goes on without AOT), else an error.
"""
import contextlib
import hashlib
import io
import os
from pathlib import Path
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import corpus  # noqa: E402
import gmlaot  # noqa: E402
import data_versions  # noqa: E402

STEAM_FOLDERS = {"undertale": ("Undertale", "UNDERTALE"), "deltarune": ("DELTARUNE", "Deltarune")}
NO_DATA = 3
# Code size an instruction for a chapter's module (--chapters), in place of gmlaot.py's 85. A module's region is the
# budget whatever the module holds, so the Old 3DS modules are filled at what their code measures (3.33-3.38 MB for
# 47,058 instructions: 71-72 bytes), 55,555 instructions a chapter. New modules
# use 85 B/instruction and the separate selection budget below. The module linker
# still checks the unchanged reservation and can leave out files that do not fit.
CHAPTER_BYTES_PER_INSTR = {"o3ds": 72.0}


def selection_file(game, target, chapters=False):
    stem = game + ("-parts" if chapters else "")
    new = os.path.join(HERE, "selection", stem + "-n3ds.txt")
    if target == "n3ds" and os.path.isfile(new):
        return new
    return os.path.join(HERE, "selection", stem + ".txt")


def selection_budget(target, selection, chapters=False):
    if chapters and target == "n3ds" and os.path.basename(selection) == "deltarune-parts-n3ds.txt":
        return 15.0
    return gmlaot.TARGET_BUDGET_MB[target]


def steam_roots():
    roots = []
    if sys.platform == "win32":
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
                roots.append(winreg.QueryValueEx(k, "SteamPath")[0])
        except OSError:
            pass
        roots += ["C:/Program Files (x86)/Steam", "C:/Program Files/Steam"]
    home = os.path.expanduser("~")
    roots += [os.path.join(home, ".steam", "steam"), os.path.join(home, ".local", "share", "Steam"),
              os.path.join(home, ".var", "app", "com.valvesoftware.Steam", ".local", "share", "Steam")]
    libraries = []
    for r in roots:
        vdf = os.path.join(r, "steamapps", "libraryfolders.vdf")
        if os.path.isdir(os.path.join(r, "steamapps")):
            libraries.append(r)
        if os.path.isfile(vdf):
            for m in re.finditer(r'"path"\s+"([^"]+)"', Path(vdf).read_text(encoding="utf-8", errors="replace")):
                libraries.append(m.group(1).replace("\\\\", "/"))
    seen, out = set(), []
    for lib in libraries:
        key = os.path.normcase(os.path.normpath(lib))
        if key not in seen:
            seen.add(key)
            out.append(lib)
    return out


def find_data(game, explicit):
    if explicit:
        return explicit if corpus.parts(game, explicit) else None
    env = os.environ.get("CTRPORT_%s_DATA" % game.upper())
    if env:
        return env if corpus.parts(game, env) else None
    for lib in steam_roots():
        for name in STEAM_FOLDERS[game]:
            d = os.path.join(lib, "steamapps", "common", name)
            if corpus.parts(game, d):
                return os.path.normpath(d)
    return None


def layout_file(game):
    path = os.path.join(HERE, "selection", game + "-layout.txt")
    return path if os.path.exists(path) else None


# Arguments added to every gmlaot.py run (none in a normal build). A diagnostic build can add some here, for example
# "--no-native"; they are part of the fingerprint, so the code is generated again when they change.
GMLAOT_EXTRA = []
# The Python files that generate the code: gmlaot.py and what it imports, and this file, which picks its arguments.
GENERATOR_FILES = ("gmlaot.py", "builtin_audit.py", "corpus.py", "prepare.py", "btnprompt.py", "escape.py",
                   "late_numeric.py", "typed.py")


def gmlaot_runs(game, target, found, out, selection, chapters=False, typed=None, late_numeric=None):
    """[(folder, gmlaot.py arguments)]: one run into out, or with chapters one into out/<part> for each listed part
    present in found (its lines are written to out/<part>/selection.txt before its run). typed: gmlaot.py --typed
    (tools/aot/typed.py) on every run."""
    typed_args = ["--typed", typed] if typed else []
    if late_numeric:
        if game != 'undertale' or chapters:
            raise ValueError('--late-numeric requires single-table UNDERTALE')
        typed_args += ['--late-numeric', late_numeric]
    layout = ["--layout", layout_file(game)] if layout_file(game) else []
    if not chapters:
        return [(out, ["gmlaot.py", out, "--target", target, "--selection", selection, "--data", found] + layout
                 + typed_args + GMLAOT_EXTRA)]
    budget = selection_budget(target, selection, chapters=True)
    tag = "aot%gch" % budget
    runs = []
    present = {part for part, _path, _chapter in corpus.parts(game, found)}
    for part in selection_parts(selection):
        if part not in present:
            continue
        d = os.path.join(out, part)
        args = ["gmlaot.py", d, "--target", target, "--selection", os.path.join(d, "selection.txt"), "--data", found,
                "--tag", tag, "--budget", "%g" % budget] + layout
        if target in CHAPTER_BYTES_PER_INSTR:
            args += ["--bytes-per-instr", "%g" % CHAPTER_BYTES_PER_INSTR[target]]
        runs.append((d, args + typed_args + GMLAOT_EXTRA))
    return runs


def selection_parts(selection):
    """{part: [its lines]} of a --chapters list, in list order."""
    parts = {}
    for line in Path(selection).read_text(encoding="utf-8").splitlines(keepends=True):
        f = line.split("#")[0].split()
        if len(f) in (2, 3):
            parts.setdefault(f[0], []).append(line)
    return parts


def engine_inputs():
    """What gmlaot.py reads from this tree's engine sources (builtin_audit.py), as text: which builtins are pure
    (direct calls, cached slots kept across them) and the body hashes of the builtins it may inline (INLINE_MATH). An
    engine change that alters either makes different code, so it is part of the fingerprint; other engine changes
    are not."""
    import builtin_audit
    pure = sorted(name for name, why in builtin_audit.audit().items() if why is None)
    hashes = builtin_audit.builtin_body_hashes()
    return repr(pure) + repr(sorted((name, hashes.get(name)) for name in getattr(gmlaot, "INLINE_MATH", {})))


def fingerprint(game, target, data, selection, chapters=False, runs=(), typed=None):
    """A digest of everything the generated code depends on: the generator's files, the list, the layout, the target,
    the builtin audit, the data files, and each run's gmlaot.py arguments (runs, from
    gmlaot_runs; the output folder and the data folder are written as <out> and <data>, so moving the build tree
    alone does not count). typed: the typed list, hashed with typed.py."""
    h = hashlib.sha256()
    for path in [os.path.join(HERE, name) for name in GENERATOR_FILES] + [selection]:
        h.update(Path(path).read_bytes())
    if typed:   # gmlaot.py --typed (tools/aot/typed.py); "all" names no file
        for path in (os.path.join(HERE, "typed.py"),) + (() if typed == "all" else (typed,)):
            h.update(Path(path).read_bytes())
        if typed == "all":
            h.update(b"typed all")
    h.update(engine_inputs().encode())
    import btnprompt   # the rules gmlaot.py finds UNDERTALE's prompt sites by (also in GENERATOR_FILES)
    h.update(btnprompt.source_text().encode())
    # escape.py's CTRPORT_AOT_ESCAPE=0 (every elided site compiled to its generic code only) changes the code.
    h.update(("escape %s" % os.environ.get("CTRPORT_AOT_ESCAPE", "")).encode())
    for folder, args in runs:
        out = os.path.dirname(folder) if chapters else folder
        h.update(repr([a.replace(out, "<out>").replace(data, "<data>") if isinstance(a, str) else a
                       for a in args]).encode())
    layout = layout_file(game)
    h.update(Path(layout).read_bytes() if layout else b"no layout")
    h.update(target.encode())
    if chapters:
        h.update(b"chapters %g" % CHAPTER_BYTES_PER_INSTR.get(target, 0))
    for part, path, _ in corpus.parts(game, data):
        st = os.stat(path)
        h.update(("%s %s %d %d" % (part, os.path.abspath(path), st.st_size, int(st.st_mtime))).encode())
    return h.hexdigest()


def run_gmlaot(args):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = gmlaot.main(args)
    for line in buf.getvalue().splitlines():
        if line.startswith(("game ", "budget ", "skip ", "layout ")) or "functions" in line or "files of generated code" in line:
            print("AOT:   " + line)
    return rc


def prepare_chapters(game, target, found, out, selection, runs):
    """One gmlaot.py run for each part of the list, into out/<part>/ (runs, from gmlaot_runs)."""
    parts = selection_parts(selection)
    tag = "aot%gch" % selection_budget(target, selection, chapters=True)
    os.makedirs(out, exist_ok=True)
    done = []
    for d, args in runs:
        part = os.path.basename(os.path.normpath(d))
        lines = parts[part]
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "selection.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.writelines(lines)
        print("AOT: %s" % part)
        rc = run_gmlaot(args)
        if rc:
            return rc
        done.append(part)
    with open(os.path.join(out, "aot_modules.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# Made by tools/aot/prepare.py --chapters: the build tag, then one folder of generated code for each part\n")
        f.write("tag %s\n" % tag)
        for part in done:
            f.write("part %s\n" % part)
    return 0


def main(argv):
    game = target = out = data = typed = late_numeric = None
    chapters = "--chapters" in argv
    check_only = "--check-only" in argv
    args = [a for a in argv[1:] if a not in ("--chapters", "--check-only")]
    for i in range(0, len(args), 2):
        k, v = args[i], args[i + 1] if i + 1 < len(args) else None
        if k == "--game":
            game = v
        elif k == "--target":
            target = v
        elif k == "--out":
            out = v
        elif k == "--data":
            data = v
        elif k == "--typed" and v:
            typed = v
        elif k == "--late-numeric" and v:
            late_numeric = v
        else:
            print(__doc__)
            return 2
    if game not in STEAM_FOLDERS or target not in gmlaot.TARGET_BUDGET_MB or not out:
        print(__doc__)
        return 2
    found = find_data(game, data)
    if not found:
        where = ("in " + data) if data else ("in $CTRPORT_%s_DATA or a Steam library" % game.upper())
        print("AOT: no %s data found %s; pass data=<game folder> to build.sh" % (game.upper(), where))
        return NO_DATA
    data_versions.check_folder(game, found, out)
    if check_only:
        return 0
    selection = selection_file(game, target, chapters)
    if chapters and not os.path.exists(selection):
        print("AOT: --chapters needs %s (tools/aot/select.py rank --per-part)" % selection)
        return 2
    stamp = os.path.join(out, "aot_inputs.txt")
    runs = gmlaot_runs(game, target, found, out, selection, chapters, typed, late_numeric)
    if chapters and not runs:
        print("AOT: no installed chapters have entries in the selection list")
        return 2
    fp = fingerprint(game, target, found, selection, chapters, runs, typed)
    made = os.path.join(out, "aot_modules.txt" if chapters else "aot_table.c")
    if os.path.exists(stamp) and os.path.exists(made) and Path(stamp).read_text().strip() == fp:
        print("AOT: %s code for %s is up to date (%s)" % (game.upper(), target, found))
        return 0
    print("AOT: generating %s code for %s%s from %s" % (game.upper(), target, ", one set per chapter," if chapters else "", found))
    if os.path.exists(stamp):
        os.remove(stamp)
    if chapters:
        rc = prepare_chapters(game, target, found, out, selection, runs)
    else:
        rc = run_gmlaot(runs[0][1])
    if rc:
        return rc
    with open(stamp, "w") as f:
        f.write(fp + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
