"""The digest of the sources a texprep binary was built from.

    py -3 -I tools/texprep/sources.py header <repo root> <out.h> <dependency file>...   (build.sh)
    py -3 -I tools/texprep/sources.py check <repo root> <texprep binary>              (prints match or mismatch)

build.sh has the compiler list every file each object reads (-M), keeps the ones inside the repository, and adds
itself (the compiler flags). The digest covers those files' relative paths and contents. It is compiled into the
binary, which prints it with the list (`texprep --sources`), so the builder can hash the same files in its own
tree: a different digest means the binary would not write what this tree's console code writes, and the builder
then leaves the textures to the console.

Contents are hashed with CRLF turned into LF (the compiler reads both alike), so a checkout's line endings don't
matter. Stdlib only: the builder imports this module.
"""
from __future__ import annotations

import hashlib
import os
import re
import subprocess
import sys

SCHEME = "texprep-src-1"
BUILD_SCRIPT = "tools/texprep/build.sh"


def file_digest(path):
    with open(path, "rb") as f:
        data = f.read()
    return hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest()


def digest(root, rels):
    """SCHEME:<sha256> over the sorted relative paths and their contents. Raises OSError for a missing file."""
    h = hashlib.sha256()
    for rel in sorted(set(rels)):
        h.update(rel.encode("utf-8") + b"\0" + file_digest(os.path.join(root, *rel.split("/"))).encode() + b"\n")
    return SCHEME + ":" + h.hexdigest()


def _native(path):
    # MSYS2 hands a native Windows Python "/e/dir" paths only when it converts them; a depfile can hold either form.
    m = re.match(r"^/([A-Za-z])/(.*)$", path)
    if os.name == "nt" and m:
        return m.group(1) + ":/" + m.group(2)
    return path


def parse_depfile(text):
    """The prerequisites of a make-style dependency file written with -MT x."""
    text = text.replace("\\\r\n", " ").replace("\\\n", " ")
    _, sep, rest = text.partition("x:")
    if not sep:
        raise ValueError("not a dependency file for target x")
    out = []
    for m in re.finditer(r"(?:\\ |[^\s])+", rest):
        out.append(m.group(0).replace("\\ ", " "))
    return out


def relative_inputs(root, depfiles):
    root = os.path.normcase(os.path.realpath(_native(root)))
    rels = {BUILD_SCRIPT}
    for dep in depfiles:
        with open(_native(dep), encoding="utf-8", errors="replace") as f:
            for p in parse_depfile(f.read()):
                full = os.path.normcase(os.path.realpath(_native(p)))
                if full.startswith(root + os.sep):
                    rels.add(os.path.relpath(full, root).replace(os.sep, "/"))
    return sorted(rels)


def write_header(root, out, depfiles):
    rels = relative_inputs(root, depfiles)
    d = digest(_native(root), rels)
    lines = ["// Written by tools/texprep/sources.py: the sources this binary was built from.",
             '#define TEXPREP_SOURCES_DIGEST "%s"' % d,
             "#define TEXPREP_SOURCES_LIST \\"]
    lines += ['    "%s\\n" \\' % r for r in rels]
    lines.append('    ""')
    with open(_native(out), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    return d


def read_binary_sources(binary, run=None, timeout=60):
    """(digest, [relative paths]) from `texprep --sources`, or raises ValueError."""
    run = run or (lambda argv: subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                                              timeout=timeout,
                                              creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0))
    r = run([os.path.abspath(binary), "--sources"])
    lines = [s.strip() for s in (r.stdout or "").splitlines() if s.strip()]
    if r.returncode != 0 or not lines or not lines[0].startswith(SCHEME + ":"):
        raise ValueError("texprep --sources gave no digest (exit %s)" % r.returncode)
    rels = lines[1:]
    for rel in rels:
        if rel.startswith("/") or ".." in rel.split("/") or "\\" in rel or ":" in rel:
            raise ValueError("texprep --sources listed an unexpected path: %r" % rel)
    if not rels:
        raise ValueError("texprep --sources listed no files")
    return lines[0], rels


def check(root, binary, run=None):
    """(ok, message): whether the binary was built from the same sources as the tree at root."""
    try:
        want, rels = read_binary_sources(binary, run)
    except (OSError, ValueError, subprocess.SubprocessError) as e:
        return False, str(e)
    try:
        have = digest(root, rels)
    except OSError as e:
        return False, "source file missing in this tree (%s)" % e
    if have != want:
        return False, "built from other sources (%s, this tree %s)" % (want, have)
    return True, "built from this tree's sources (%s, %d files)" % (want, len(rels))


def main(argv):
    if len(argv) >= 4 and argv[1] == "header":
        print(write_header(argv[2], argv[3], argv[4:]))
        return 0
    if len(argv) == 4 and argv[1] == "check":
        ok, msg = check(argv[2], argv[3])
        print(("match: " if ok else "mismatch: ") + msg)
        return 0 if ok else 1
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
