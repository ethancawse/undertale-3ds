#!/usr/bin/env python3
"""Content pins must survive the export.

Some build tools pin a source file by its sha256 (tools/buildcache/mexdlg_native_compile.py pins
platform/3ds/source/z_writerfront.c). The export's comment cleanup must never leave such a file with bytes its pin
does not list: the build then stops for everyone building from the published source. A pin is a 64-hex literal
equal to a file's source digest, found in another exported file; that file's exported copy must list the exported
digest too. check_release runs this on every export; the command line checks a finished tree against its commit:

    py -3 -I tools/release/export_pins.py TREE --source-commit COMMIT [--repo REPO] [--allow-extra PATH ...]

The command line is the release receipt for a finished tree. It fails (exit 2) on a missing or empty TREE, on a
COMMIT that does not resolve to a commit, and when any TREE file is missing from COMMIT (a wrong or polluted tree)
unless that exact path is named with --allow-extra; a named path that is not an extra file in TREE fails too, so
the list cannot go stale. Allowed extras are printed on the receipt. So a partial overlap never passes: every TREE
file is either compared with COMMIT's file of the same path or named as an allowed extra. A TREE file whose path is
a directory or submodule in COMMIT fails (type mismatch), and TREE must hold only regular files and directories: any
symlink, junction or other reparse point under it (dangling or not) fails before a file is read. No --allow-extra
exempts either. Release receipts pass an explicit --repo and the full 40-hex COMMIT (a branch can shadow a short
name), and keep the printed resolved commit and both file counts.

What it does not cover. It is an export-change alarm for whole-file pins written as one 64-hex token (any case,
in any text file). It does not see a digest split across literals or computed at run time, a pin over a
normalised form (CRLF folding, comment stripping) or over several files at once (an aggregate or directory digest,
such as the texprep source record), or text with NUL bytes (UTF-16). It checks that the exported digest appears in
the pinning file, not that the code there accepts it, and files the export drops are outside the comparison. Those
pins need their own checks (test_texprep_pin.py, the mexdlg walk digests) and the real builds.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys

HEX = re.compile(rb'(?<![0-9A-Fa-f])[0-9A-Fa-f]{64}(?![0-9A-Fa-f])')
# Identifier approvals are rebound to the cleaned copy's digest by export_scrub; they are not build pins.
EXEMPT = frozenset({'tools/release/identifier-allowlist.json'})


def literals(files):
    found = {}
    for name, data in files.items():
        if name in EXEMPT or b'\0' in data:
            continue
        for match in set(HEX.findall(data)):
            found.setdefault(match.decode().lower(), set()).add(name)
    return found


def broken(source, exported):
    """[(name, source digest, exported digest, pinning files)] for every pin the export breaks."""
    pinned, kept = literals(source), literals(exported)
    result = []
    for name in sorted(set(source) & set(exported)):
        before = hashlib.sha256(source[name]).hexdigest()
        pinners = pinned.get(before, set()) & set(exported)
        if not pinners:
            continue
        after = hashlib.sha256(exported[name]).hexdigest()
        missing = sorted(p for p in pinners if p not in kept.get(after, set()))
        if missing:
            result.append((name, before, after, missing))
    return result


def check(source, exported):
    problems = broken(source, exported)
    if problems:
        raise ValueError('export breaks content pins: ' + '; '.join(
            '%s (source %s, exported %s, pinned in %s)' % (name, a[:16], b[:16], ', '.join(p))
            for name, a, b, p in problems))


REPARSE_POINT = 0x400  # FILE_ATTRIBUTE_REPARSE_POINT


def linked(entry):
    """True for a symlink, junction or any other reparse point (dangling ones too); never follows it."""
    if entry.is_symlink() or (hasattr(entry, 'is_junction') and entry.is_junction()):
        return True
    return bool(getattr(entry.stat(follow_symlinks=False), 'st_file_attributes', 0) & REPARSE_POINT)


def tree_names(tree):
    """{relative posix name: path} of every regular file under TREE, walked without following any link.

    A descendant symlink, junction or reparse point (dangling or not), or an entry that is neither a regular file
    nor a directory, is a ValueError before any file is read; .git (file or directory) is skipped after that check.
    Walk errors are not swallowed."""
    found, pending = {}, [(tree, '')]
    while pending:
        path, prefix = pending.pop()
        with os.scandir(path) as entries:
            for entry in entries:
                name = prefix + entry.name
                if linked(entry):
                    raise ValueError('export tree holds a symlink, junction or reparse point: %s' % name)
                if entry.name == '.git':
                    continue
                if entry.is_dir(follow_symlinks=False):
                    pending.append((entry.path, name + '/'))
                elif entry.is_file(follow_symlinks=False):
                    found[name] = Path(entry.path)
                else:
                    raise ValueError('export tree holds an entry that is not a regular file: %s' % name)
    return found


def read_tree(tree):
    """Every file under an existing, nonempty export directory (.git excluded); links are refused (tree_names)."""
    if not tree.is_dir():
        raise ValueError('export tree is not a directory: %s' % tree)
    try:
        names = tree_names(tree)
        files = {name: names[name].read_bytes() for name in sorted(names)}
    except OSError as error:
        raise ValueError('cannot read export tree %s: %s' % (tree, error))
    if not files:
        raise ValueError('export tree has no files: %s' % tree)
    return files


def resolve_commit(repo, ref):
    """The full name of the commit ref names, or ValueError."""
    run = subprocess.run(['git', '-C', str(repo), 'rev-parse', '--verify', '--quiet', ref + '^{commit}'],
                         capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=120)
    name = run.stdout.strip()
    if run.returncode or not re.fullmatch(r'[0-9a-f]{40}|[0-9a-f]{64}', name):
        raise ValueError('source commit does not resolve in %s: %s' % (repo, ref))
    return name


def read_commit(repo, commit, names):
    """The commit's bytes of each name it has, from one git cat-file --batch (commit: a resolved commit name).

    A name the commit holds as anything but a blob (a tree or a submodule commit) is a ValueError: an exported file
    can only be compared with a file."""
    request = ''.join('%s:%s\n' % (commit, name) for name in names).encode('utf-8')
    out = subprocess.run(['git', '-C', str(repo), 'cat-file', '--batch'], input=request, capture_output=True,
                         check=True, timeout=600).stdout
    result, mismatched, pos = {}, [], 0
    for name in names:
        end = out.index(b'\n', pos)
        line = out[pos:end]
        header = line.split()
        pos = end + 1
        if header[-1] == b'missing':
            continue
        if len(header) != 3:
            raise ValueError('unexpected git cat-file reply for %s: %r' % (name, line))
        size = int(header[2])
        if header[1] != b'blob':
            mismatched.append('%s (%s)' % (name, header[1].decode('ascii', 'replace')))
        else:
            result[name] = out[pos:pos + size]
        pos += size + 1
    if mismatched:
        raise ValueError('type mismatch: %d exported files are not files in %s: %s%s' % (
            len(mismatched), commit[:12], ', '.join(mismatched[:20]), ', ...' if len(mismatched) > 20 else ''))
    return result


def allowed_extras(paths):
    """The --allow-extra set: exact relative tree paths only (no wildcards, no allow-all)."""
    result = set()
    for path in paths:
        if (not path or any(c in path for c in '*?[]\\') or path.startswith('/') or ':' in path
                or any(part in ('', '.', '..') for part in path.split('/'))):
            raise ValueError('--allow-extra takes one exact relative tree path, not %r' % path)
        result.add(path)
    return result


def check_extras(exported, source, allowed, commit):
    """Sorted allowed extras; ValueError for an unexpected extra or a stale allowance."""
    extras = sorted(set(exported) - set(source))
    unexpected = [name for name in extras if name not in allowed]
    if unexpected:
        raise ValueError('%d tree files are not in %s (a wrong or polluted tree; name an expected one with '
                         '--allow-extra): %s%s' % (len(unexpected), commit[:12], ', '.join(unexpected[:20]),
                                                  ', ...' if len(unexpected) > 20 else ''))
    stale = sorted(name for name in allowed if name not in exported or name in source)
    if stale:
        raise ValueError('--allow-extra names paths that are not extra files in this tree: ' + ', '.join(stale))
    return extras


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('tree', type=Path)
    parser.add_argument('--source-commit', required=True)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--allow-extra', action='append', default=[], metavar='PATH',
                        help='an exact tree path expected to be absent from COMMIT (repeatable; no wildcards)')
    args = parser.parse_args(argv)
    try:
        allowed = allowed_extras(args.allow_extra)
        exported = read_tree(args.tree)
        commit = resolve_commit(args.repo, args.source_commit)
        source = read_commit(args.repo, commit, sorted(exported))
        if not source:
            raise ValueError('none of the %d exported files exist in %s' % (len(exported), commit))
        extras = check_extras(exported, source, allowed, commit)
    except ValueError as error:
        print('export pins: ERROR %s' % error)
        return 2
    for name in extras:
        print('EXTRA (allowed) %s' % name)
    problems = broken(source, exported)
    for name, a, b, pinners in problems:
        print('BROKEN %s source %s exported %s pinned in %s' % (name, a, b, ', '.join(pinners)))
    print('export pins: %d files, %d from commit %s, %d allowed extra, %d broken' % (
        len(exported), len(source), commit[:12], len(extras), len(problems)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
