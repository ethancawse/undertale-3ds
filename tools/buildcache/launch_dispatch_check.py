"""Proof that the launcher dispatcher (launch_dispatch.py) builds the same bytes as the chain of processes.

    launch_dispatch_check.py identity --label <name> --out <dir> [--bash <bash>] [--env K=V ...] -- <build.sh words>
        Builds the same tree twice in the same build directory, from clean: A with CTRPORT_LAUNCH_DISPATCH=OFF (the
        chain of processes), then B with it ON, each with a fresh empty compiler cache so that every object really
        compiles through each side. Records wall time, the compile phase and python.exe counts, hashes every file of
        each build directory (<label>-A.json, <label>-B.json), keeps both ELFs and the files compare unpacks or
        diffs (<label>-{A,B}-tree), then runs `compare` (build-system diffs: <label>-buildsystem.diff) and `elfdiff`.
        build.sh runs in a login shell of --bash (default devkitPro's msys2 bash). With --slots N the first build
        of the process waits until fewer than N cmake.exe run; main() may be called again in the same process for
        the next config, whose builds then start at once in the slot this process already holds.
    launch_dispatch_check.py snapshot <build dir> <out.json>
    launch_dispatch_check.py compare <A.json> <B.json> [--a-dir <A tree> --b-dir <B tree>] [--diff-out <file>]
        The verdict is about outputs: every file must be byte-identical, on both sides, except
          - build-system inputs (CMakeCache.txt, each target's build.make, Makefile2, CMakeConfigureLog.yaml,
            configure's ABI probe) and the two timing logs: not in the verdict, shown as a plain diff;
          - static libraries (.a): read strictly as regular ar archives, headers at the same offsets, then
            byte-identical but for each member header's 12-byte date field; thin archives are refused, equal hashes
            or not;
          - .cia files: every content (the NCCH) byte-identical as ctrtool unpacks it, every byte outside the
            ticket and TMD identical, and every ticket and TMD field ctrtool prints identical except those makerom
            draws at random (signatures, title key, ticket ID, content ID).
        An unpacked file needs both kept copies, each matching its own snapshot's SHA-256. Exit 1 otherwise.
    launch_dispatch_check.py elfdiff <A.elf> <B.elf>
        Every allocated section: same address, size, flags and bytes (secdiff; the ELF's own sha is not compared).
    launch_dispatch_check.py rules <build dir> [--jobs N] [--only <launcher>...] [--expect-active <launcher>...]
        For each launcher of the chain, every source its rule names (the .c names in its code) is compiled again with
        that launcher removed from the line; the object and its side files must differ from the build's. Each
        compile line is build.make's recipe with make's $(VAR)s expanded from the target's flags.make, run by the
        shell make runs recipes with (devkitPro's msys2 sh for an MSYS make path), with the compiler cache off
        (CTRPORT_CCACHE=0). First, per object, a control: the full, unmodified line must exit 0 and write the
        build's object and side files byte for byte; otherwise that object's checks are ERROR, never RED, since a
        line that fails or differs anyway says nothing about one launcher. Each probe deletes the object first, so
        a command that writes nothing cannot pass. A launcher whose removal changes nothing in this build is listed
        as inactive here; one with any errored check as not probed. Each control and probe runs in its own kill
        group made at launch (a job object it joins before it runs, tools/testproc.py): on a timeout the whole group
        is killed, a process left alive after the command exits is killed and makes the check ERROR, and the group
        must be seen empty before the object is hashed; if it is not, the check is ERROR ("writers may be alive").
        Launchers also write work files of their own beside the objects, under other names (utwindow0.s,
        noticescroll-exit-routing.json, ...), and a run could leave anything in the tree, so nothing is ever put back
        in place. Before anything runs, the WHOLE tree is walked without following anything (os.scandir + os.lstat)
        and recorded: every directory and file, mtime_ns, attributes or mode, size and sha256. A tree that holds a
        reparse point (junction, symlink), a file with more than one hard link, a named stream or any unreadable
        entry is refused (exit 2): it cannot be copied exactly. rules also refuses (exit 1) beside any
        .rules-copy-* or .rules-pristine-* directory, any tree's, any case: an earlier run did not end. Then the tree
        is moved aside whole, by a rename, to .rules-pristine-<tree>-<time>-<pid>/tree (with a README.txt); nothing
        ever runs in it, and a rename that fails leaves the tree where it was (exit 1). For every control and probe,
        one at a time: the pristine tree is copied to the build path (bytes, then mtimes and attributes, directories
        included) and the copy's manifest must equal the first; the run goes there; once its group is empty, what it
        left besides its own object's files is listed (TOUCHED OUTSIDE), and the copy is deleted without following
        anything (a junction or link is removed as a link, never its contents; a file with more than one hard link
        is unlinked as it is). Only a directory the copy's own exclusive mkdir made is ever deleted: its identity
        (volume and file ID, os.lstat st_dev + st_ino) is read right after the mkdir, and the build path is read
        again before every delete attempt, each busy retry included; if the mkdir fails (something else is there)
        or the path no longer has that identity (gone, or renamed away and replaced), nothing is deleted. A run
        whose group was not seen empty ("writers may be alive") keeps its copy: it is not deleted, and the pristine
        tree is not moved back. The worktree outside build/ and scratch/ must also be unchanged: every tracked
        path's bytes as listed at the start (read directly, whatever the index flags say), every untracked or
        ignored file's bytes, HEAD, the index file's hash and the .git pointer. A copy that cannot be made, checked
        or deleted, a build path that no longer holds the copy, or a changed worktree, is RESTORE FAILED; it and
        WRITERS MAY BE ALIVE stop rules (no later run starts) and name the path. At the end, and on any stop, the
        pristine tree is renamed back and its manifest must equal the first (TREE CHANGED fails). If writers may be
        alive, the build path no longer held the copy, anything is still at the build path, or the rename back
        fails, the pristine tree stays aside and both paths are printed (PRISTINE TREE LEFT AT, RECOVER BY HAND,
        exit 1); its README.txt says how to put it back. A check with any problem counts as errored, RED or not.
        Exit 1 when a copy, a control or a probe failed, the tree changed, or a launcher named by --expect-active is
        not ACTIVE: a zero exit without it proves only that every run had a fresh copy of the tree and every control
        reproduced the build.
        Limits: there is no filesystem sandbox. A run (or any process outside the job) can deliberately write
        outside the checked scope (the build tree's copy and the worktree outside build/ and scratch/), and such
        writes are not seen. The identity check before each delete narrows, but cannot close, the window between
        reading the build path and deleting it, or between the mkdir and reading its identity: a process outside
        the job that swaps the path in that instant is not caught. Treat the sentence that rules deletes only its
        own directory as scoped by those limits, not as an absolute security property (c26rev4 round 12): it holds
        for trusted launchers on an idle tree, with nothing else writing there during the run.
"""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

# What compare checks. The identity proof is about outputs: every file the build writes must be byte-identical on
# both sides, except
#   - the two timing logs (diagnostics, compared by nobody): shown as information;
#   - the build-system inputs BUILD_SYSTEM names: CMake's cache, each target's build.make, Makefile2, the configure
#     log and configure's ABI probe. They differ by construction (the setting under test, each side's own compiler
#     cache directory, CMake's own randomness) and act on this build only through the commands they run, whose
#     results are the outputs compared here. Each is shown as a plain diff of its kept copies, never in the verdict;
#   - the outputs UNPACKED names, whose containers stamp per-build values: a static library is compared byte for
#     byte but for its members' dates, a .cia as ctrtool unpacks it. Both kept copies must be there and match their
#     snapshot hashes.
# A thin archive ("!<thin>\n") is refused wherever it is seen, equal hashes or not: its members are files outside it,
# which no snapshot binds (c26rev4 BF5-F2).
TIMING_LOGS = {
    'compile_cache.log': 'per-compile wall times and the cache directory (diagnostic)',
    'launch_dispatch.log': 'per-compile wall times (B only; diagnostic)',
}

BUILD_SYSTEM = (
    (re.compile(r'^CMakeCache\.txt$'), 'CMake cache (holds CTRPORT_LAUNCH_DISPATCH and the compiler cache directory)'),
    (re.compile(r'^CMakeFiles/[^/]+\.dir/build\.make$'), "a target's make rules (compile lines name the per-side "
     'cache directory; ctrport\'s start with the dispatcher on B)'),
    (re.compile(r'^CMakeFiles/Makefile2$'), 'the inter-target make rules (CMake orders some prerequisites anew '
     'each configure)'),
    (re.compile(r'^CMakeFiles/CMakeConfigureLog\.yaml$'), "the configure log (CMake's random try_compile names)"),
    (re.compile(r'^CMakeFiles/[^/]+/CMakeDetermineCompilerABI_C\.bin$'), "configure's ABI probe (its debug info "
     "names CMake's random try_compile directory)"),
)


def build_system(name):
    return next((why for r, why in BUILD_SYSTEM if r.search(name)), None)


# ------------------------------------------------------------------------------------------- unpacked outputs

def devkitpro():
    for d in (os.environ.get('DEVKITPRO'), 'C:/devkitPro', '/opt/devkitpro'):
        if d and Path(native(d)).is_dir():
            return Path(native(d))
    return None


def tool(rel):
    """A devkitPro program (rel: 'tools/bin/ctrtool'); ValueError when it is not installed."""
    base = devkitpro()
    p = base / (rel + ('.exe' if os.name == 'nt' else '')) if base else None
    if p is None or not p.is_file():
        raise ValueError('%s not found under devkitPro' % rel)
    return p


def _run(argv):
    p = subprocess.run([str(a) for a in argv], stdin=subprocess.DEVNULL, capture_output=True, timeout=600)
    if p.returncode:
        said = (p.stderr + b'\n' + p.stdout).decode('utf-8', 'replace').splitlines()
        said = [l.strip() for l in said if 'ERROR' in l] or [l.strip() for l in said if l.strip()][-3:]
        raise ValueError('%s exited %d: %s' % (Path(argv[0]).name, p.returncode, ' / '.join(said)[:300]))
    return p.stdout


AR_MAGIC, AR_THIN = b'!<arch>\n', b'!<thin>\n'
AR_HEADER = 60
AR_FIELDS = (('name', 0, 16), ('date', 16, 12), ('uid', 28, 6), ('gid', 34, 6), ('mode', 40, 8), ('size', 48, 10),
             ('terminator', 58, 2))
AR_DATE = AR_FIELDS[1][1:]


def ar_headers(data):
    """The offsets of a regular GNU ar archive's member headers, walked in order from the magic, plus the file's
    length. Strict: "!<arch>\\n" magic only (a thin archive's members live outside the file and no snapshot binds
    them); each 60-byte header ends in "`\\n" and has a decimal (or blank) date and a decimal size; each member's
    data is followed by a
    "\\n" pad when its size is odd; the last member ends exactly at the end of the file. ValueError otherwise."""
    if data[:8] == AR_THIN:
        raise ValueError('a thin archive (its members are files outside it, which no snapshot binds)')
    if data[:8] != AR_MAGIC:
        raise ValueError('not a regular ar archive (magic %r)' % data[:8])
    offsets, at = [], 8
    while at < len(data):
        h = data[at:at + AR_HEADER]
        if len(h) < AR_HEADER:
            raise ValueError('truncated member header at %d' % at)
        if h[58:60] != b'`\n':
            raise ValueError('member header at %d does not end in "`\\n"' % at)
        # GNU ar leaves the long-name table's date blank; every size is a number.
        for field, form in (('date', rb'[0-9]* *'), ('size', rb'[0-9]+ *')):
            _f, o, n = next(f for f in AR_FIELDS if f[0] == field)
            if not re.fullmatch(form, h[o:o + n]):
                raise ValueError('member header at %d: %s field %r is not decimal' % (at, field, h[o:o + n]))
        size = int(h[48:58])
        end = at + AR_HEADER + size
        if end > len(data):
            raise ValueError('member at %d runs past the end of the file (%d bytes, %d left)' % (
                at, size, len(data) - at - AR_HEADER))
        if size % 2:
            if data[end:end + 1] != b'\n':
                raise ValueError('member at %d: odd size %d without its "\\n" pad' % (at, size))
            end += 1
        offsets.append(at)
        at = end
    return offsets, len(data)


def check_archive(a, b, _work=None):
    """Both libraries read strictly as regular ar archives (ar_headers), with their member headers at the same
    offsets, and then byte-identical but for the 12-byte date field of each of those headers (ar stamps each member
    with its object's modification time): the symbol index, the long-name table, every member's name, owner, group,
    mode, size and data, and the pads. Anything that does not read is refused."""
    da, db = Path(a).read_bytes(), Path(b).read_bytes()
    layout = []
    for side, d in (('A', da), ('B', db)):
        try:
            layout.append(ar_headers(d))
        except ValueError as e:
            return 'not compared, %s: %s' % (side, e)
    if layout[0] != layout[1]:
        (oa, la), (ob, lb) = layout
        first = next((i for i, (x, y) in enumerate(zip(oa, ob)) if x != y), min(len(oa), len(ob)))
        return 'members laid out differently: %d members and %d bytes on A, %d and %d on B; first header apart: %s' % (
            len(oa), la, len(ob), lb, 'A at %s, B at %s' % (oa[first] if first < len(oa) else '-',
                                                             ob[first] if first < len(ob) else '-'))
    ma, mb = bytearray(da), bytearray(db)
    o, n = AR_DATE
    for at in layout[0][0]:
        ma[at + o:at + o + n] = mb[at + o:at + o + n] = b' ' * n
    if ma == mb:
        return None
    first = next(i for i, (x, y) in enumerate(zip(ma, mb)) if x != y)
    differ = sum(1 for x, y in zip(ma, mb) if x != y)
    at = max((h for h in layout[0][0] if h <= first), default=None)
    if at is None:
        where = 'the magic'
    elif first < at + AR_HEADER:
        where = "the %s field of the member header at %d" % (
            next(f for f, fo, fn in AR_FIELDS if fo <= first - at < fo + fn), at)
    else:
        name = da[at:at + 16].rstrip()
        where = 'the data of member %r (header at %d)' % (name.decode('utf-8', 'backslashreplace'), at)
    return '%d bytes differ outside the members\' dates; the first at byte %d, in %s' % (differ, first, where)


CIA_BLOCKS = ('CiaHeader', 'Certificate Chain', 'Ticket', 'TitleMetaData')
CIA_RANDOM = re.compile(r'^[|\\ -]*(Signature|TitleKey|TicketId|ContentId):')  # makerom draws these per .cia


def cia_view(path, work):
    """A .cia as ctrtool unpacks it (-p: stored bytes, nothing decrypted): its printout; the certificate chain,
    ticket and TMD files; the contents by index; and which byte ranges of the file the ticket and TMD are.

    Every part is checked against the file: the certificates, ticket, TMD and each content must be the file's bytes
    where the CIA header's sizes (as ctrtool prints them) put them, the TMD must list exactly the contents written,
    each unencrypted, and each content's SHA-256 must be its hash in the TMD. ctrtool itself refuses a CIA without
    a ticket or TMD, and a TMD whose record hashes do not match."""
    ctr = tool('tools/bin/ctrtool')
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    out = _run([ctr, '-p', '--contents=%s' % (work / 'content'), '--certs=%s' % (work / 'certs'),
                '--tik=%s' % (work / 'tik'), '--tmd=%s' % (work / 'tmd'), path])
    text = out.decode('utf-8', 'surrogateescape').replace('\r\n', '\n')
    if 'ERROR' in text or 'WARNING' in text:
        raise ValueError('ctrtool: %s' % next(l for l in text.splitlines() if 'ERROR' in l or 'WARNING' in l))
    size = {}
    for key in ('HeaderSize', 'CertificateSize', 'TicketSize', 'TitleMetaSize', 'FooterSize', 'ContentSize'):
        m = re.search(r'^\|- %s:\s+0x([0-9a-f]+)$' % key, text, re.M)
        if not m:
            raise ValueError('ctrtool printed no CIA %s' % key)
        size[key] = int(m.group(1), 16)
    if not (size['TicketSize'] and size['TitleMetaSize']):
        raise ValueError('no ticket or no TMD (ticket %d bytes, TMD %d bytes)' % (size['TicketSize'],
                                                                                   size['TitleMetaSize']))
    hashes, crypt, idx, block = {}, {}, None, None
    for line in text.splitlines():
        if line and line[0] not in ' |\\':
            block = line.rstrip(':')
        elif block == 'TitleMetaData':
            m = re.match(r'^[|\\ -]*0x([0-9a-f]{4}):$', line)
            idx = int(m.group(1), 16) if m else idx
            m = re.match(r'^[|\\ -]*Hash:\s+([0-9A-F]{64})$', line)
            if m:
                hashes[idx] = m.group(1).lower()
            m = re.match(r'^[|\\ -]*Encrypted:\s+(\S+)$', line)
            if m:
                crypt[idx] = m.group(1)
    # An encrypted content's bytes stand for different contents under different title keys, and makerom draws the
    # title key at random: only unencrypted contents (what this build makes) can be compared byte for byte.
    if sorted(crypt) != sorted(hashes) or any(v != 'NO' for v in crypt.values()):
        raise ValueError('content encrypted, or its encryption not shown (%s)' % crypt)
    contents = {}
    for p in work.glob('content.*'):
        m = re.fullmatch(r'content\.([0-9a-f]{4})\.[0-9a-f]{8}', p.name)
        if not m:
            raise ValueError('ctrtool wrote an unexpected content file %s' % p.name)
        contents[int(m.group(1), 16)] = p.read_bytes()
    if not contents or sorted(contents) != sorted(hashes):
        raise ValueError('contents written %s, TMD lists %s' % (sorted(contents), sorted(hashes)))
    for i, body in contents.items():
        if hashlib.sha256(body).hexdigest() != hashes[i]:
            raise ValueError('content %d does not match its hash in the TMD' % i)
    data = Path(path).read_bytes()

    def al(x):
        return (x + 63) // 64 * 64
    off, spans = al(size['HeaderSize']), {}
    for name, key in (('certs', 'CertificateSize'), ('tik', 'TicketSize'), ('tmd', 'TitleMetaSize')):
        blob = (work / name).read_bytes()
        if len(blob) != size[key] or data[off:off + len(blob)] != blob:
            raise ValueError('ctrtool\'s %s is not the file\'s bytes where the header puts it' % name)
        spans[name] = (off, len(blob))
        off = al(off + len(blob))
    start = off
    for i in sorted(contents):
        if data[off:off + len(contents[i])] != contents[i]:
            raise ValueError('content %d is not the file\'s bytes where the header puts it' % i)
        off += len(contents[i])
    if off - start != size['ContentSize'] or al(off) + size['FooterSize'] > len(data):
        raise ValueError('contents and footer do not fit the header\'s sizes')
    return text, contents, spans, data


def check_cia(a, b, work):
    """The same contents (NCCH: header, exheader, ExeFS, RomFS) byte for byte, and every byte of the file outside the
    ticket and TMD (header, certificates, contents, footer/meta, padding); of the ticket and TMD, every field
    ctrtool prints except those makerom draws at random (both signatures, the title key, the ticket ID, the content
    ID). Each content's hash in the TMD is checked against it on each side."""
    try:
        ta, ca, sa, da = cia_view(a, work / 'A')
        tb, cb, sb, db = cia_view(b, work / 'B')
    except (ValueError, OSError, subprocess.TimeoutExpired) as e:
        return str(e)
    if sorted(ca) != sorted(cb):
        return 'content indexes differ: A %s, B %s' % (sorted(ca), sorted(cb))
    for i in sorted(ca):
        if ca[i] != cb[i]:
            return 'content %d differs' % i
    if sa != sb:
        return 'ticket or TMD of another size or place'

    def outside(d, s):
        (t0, tn), (m0, mn) = s['tik'], s['tmd']
        return d[:t0] + d[t0 + tn:m0] + d[m0 + mn:]
    if outside(da, sa) != outside(db, sb):
        return 'differs outside the ticket and TMD'

    def fields(text):
        out, block = [], None
        for line in text.splitlines():
            if line and line[0] not in ' |\\':
                block = line.rstrip(':')
            if block in CIA_BLOCKS and not (block in ('Ticket', 'TitleMetaData') and CIA_RANDOM.match(line)):
                out.append(line)
        return out
    fa, fb = fields(ta), fields(tb)
    if fa != fb:
        first = next((i for i, (x, y) in enumerate(zip(fa, fb)) if x != y), min(len(fa), len(fb)))
        return 'ctrtool shows other ticket/TMD fields: A %r, B %r' % (fa[first] if first < len(fa) else '-',
                                                                     fb[first] if first < len(fb) else '-')
    return None


UNPACKED = (
    (re.compile(r'\.a$'), "regular ar archives, byte-identical but for each member header's date field (ar stamps "
     "each member with its object's modification time)", check_archive),
    (re.compile(r'\.cia$'), 'contents (NCCH) and every byte outside the ticket and TMD identical as ctrtool unpacks '
     'it; ticket and TMD fields identical but for those makerom draws at random (signatures, title key, ticket ID, '
     'content ID)', check_cia),
)


def unpacked(name):
    return next(((what, check) for r, what, check in UNPACKED if r.search(name)), None)


def kept_names(files):
    """The files of a build an identity run keeps for compare."""
    return sorted(n for n in files if build_system(n) or unpacked(n))


def plain_diff(a, b):
    """A plain diff of two kept copies: unified, line by line, raw bytes shown as \\x escapes; for a binary file,
    its sizes and how many bytes differ."""
    da, db = Path(a).read_bytes(), Path(b).read_bytes()
    if b'\0' in da or b'\0' in db:
        n = sum(1 for x, y in zip(da, db) if x != y) + abs(len(da) - len(db))
        first = next((i for i, (x, y) in enumerate(zip(da, db)) if x != y), min(len(da), len(db)))
        return ['binary: %d bytes on A, %d on B, %d differ, first at 0x%x' % (len(da), len(db), n, first)]
    la = da.decode('utf-8', 'backslashreplace').splitlines()
    lb = db.decode('utf-8', 'backslashreplace').splitlines()
    return list(difflib.unified_diff(la, lb, 'A', 'B', lineterm='', n=0))


DEVKITPRO_BASH = 'C:/devkitPro/msys2/usr/bin/bash.exe'


def native(token):
    """An MSYS path in a build.make token (/e/x, --opt=/e/x) as the native path MSYS hands a Windows program."""
    if os.name != 'nt':
        return token
    return re.sub(r'(^|=)/([a-zA-Z])/', lambda m: m.group(1) + m.group(2).upper() + ':/', token)


class Chain:
    def __init__(self, tokens):
        self.tokens = tokens                       # the launcher part of a compile line, as build.make has it
        self.launchers = [native(t) for t in tokens]
        self.levels = [t for t in self.launchers if t.endswith('.py') and not t.endswith('launch_dispatch.py')]


def compile_lines(build):
    """(object, source, recipe line) for each C compile in the ctrport target's build.make."""
    make = Path(build) / 'CMakeFiles/ctrport.dir/build.make'
    lines = make.read_text(encoding='utf-8', errors='replace').splitlines()
    out, deps = [], {}
    for i, line in enumerate(lines):
        m = re.match(r'^(CMakeFiles/ctrport\.dir/\S+\.o): (\S+\.c)$', line)
        if m:
            deps[m.group(1)] = m.group(2)
        if line.startswith('\t') and ('compile_cache.py' in line or '_compile.py' in line) and ' -c ' in line:
            m = re.search(r' -o (\S+) -c (\S+)', line)
            if m:
                out.append((m.group(1), m.group(2), line[1:]))
    return out


def chain_of(build, rebase=False):
    """The launcher chain of a configured build tree: every token before the compiler (rebase: with the tree's
    source checkout replaced by this one, so that this checkout's launchers run)."""
    for _obj, _src, line in compile_lines(build):
        toks = shlex.split(line)
        cut = next(i for i, t in enumerate(toks) if re.search(r'(^|/)arm-none-eabi-gcc(\.exe)?$', t))
        toks = toks[:cut]
        if any(t.endswith('launch_dispatch.py') for t in toks):
            toks = toks[toks.index(next(t for t in toks if t.endswith('launch_dispatch.py'))) + 1:]
        if rebase:
            cache = (Path(build) / 'CMakeCache.txt').read_text(encoding='utf-8', errors='replace')
            home = re.search(r'^CMAKE_HOME_DIRECTORY:\w+=(.+)$', cache, re.M).group(1).strip()
            checkout = home.rstrip('/').rsplit('/platform/3ds', 1)[0]
            mine = ROOT.as_posix()
            toks = [t.replace(checkout + '/', mine + '/') if checkout != mine else t for t in toks]
            msys = re.sub(r'^([A-Za-z]):/', lambda m: '/' + m.group(1).lower() + '/', checkout)
            toks = [t.replace(msys + '/', mine + '/') for t in toks]
        return Chain(toks)
    raise SystemExit('no ctrport compile line in %s' % build)


# ------------------------------------------------------------------------------------------- snapshot and compare

def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def is_thin(path):
    with open(path, 'rb') as f:
        return f.read(len(AR_THIN)) == AR_THIN


def snapshot(build, out):
    """SHA-256 of every file of a build directory, and the names of any thin archives in it (compare refuses
    those whatever their hashes)."""
    build = Path(build)
    files, thin = {}, []
    for dirpath, _dirs, names in os.walk(build):
        for name in names:
            p = Path(dirpath) / name
            rel = p.relative_to(build).as_posix()
            if p.is_symlink():
                files[rel] = 'link:' + os.readlink(p)
            else:
                files[rel] = sha256(p)
                if is_thin(p):
                    thin.append(rel)
    Path(out).write_text(json.dumps({'build': str(build), 'files': files, 'thin': sorted(thin)}, indent=0,
                                    sort_keys=True), encoding='utf-8')
    if thin:
        print('SNAPSHOT %s: thin archives, refused by compare: %s' % (build, ', '.join(sorted(thin))), flush=True)
    return files


def kept_copy_problem(name, a_dir, b_dir, ha, hb):
    """Why the kept copies of `name` cannot stand for the snapshot (missing, or not the snapshot's bytes); None
    when both are there and each one's SHA-256 is its own side's snapshot hash."""
    if not (a_dir and b_dir):
        return 'no kept copies to check'
    for side, d, h in (('A', a_dir, ha), ('B', b_dir, hb)):
        p = Path(d) / name
        if not p.is_file():
            return 'no kept copy on %s' % side
        if sha256(p) != h:
            return 'the kept copy on %s is not the snapshot (sha256 %s, snapshot %s)' % (side, sha256(p)[:16],
                                                                                      str(h)[:16])
    return None


def library_refusal(name, sides, a_dir, b_dir):
    """Why `name` is refused before its hashes are looked at (None: it is not): a thin archive, by either snapshot
    or a kept copy (c26rev4 BF5-F2: a thin archive's hash does not cover its members); a static library on both
    sides (.a) whose kept copies are not bound to the snapshots or do not read strictly as regular archives. Every
    library is read from its kept copies, equal hashes or not."""
    for side, snap, d in zip('AB', sides, (a_dir, b_dir)):
        if name in snap.get('thin', ()):
            return 'a thin archive on %s (snapshot): its members are files outside it, which no snapshot binds' % side
        if d and (Path(d) / name).is_file() and is_thin(Path(d) / name):
            return 'a thin archive on %s (kept copy): its members are files outside it, which no snapshot binds' % side
    fa, fb = sides[0]['files'], sides[1]['files']
    if not (name.endswith('.a') and name in fa and name in fb):
        return None
    problem = kept_copy_problem(name, a_dir, b_dir, fa[name], fb[name])
    if problem:
        return '%s; a library is read from its kept copies, equal hashes or not' % problem
    for side, d in (('A', a_dir), ('B', b_dir)):
        try:
            ar_headers((Path(d) / name).read_bytes())
        except ValueError as e:
            return 'not read, %s: %s' % (side, e)
    return None


def compare(a_json, b_json, a_dir=None, b_dir=None, diff_out=None):
    """The identity verdict over two snapshots (and the kept copies of each side's build directory): (number of
    unexpected differences, report lines). Build-system diffs go to diff_out when given, else into the report."""
    a = json.loads(Path(a_json).read_text(encoding='utf-8'))
    b = json.loads(Path(b_json).read_text(encoding='utf-8'))
    fa, fb = a['files'], b['files']
    report, info, diffs, bad = [], [], [], 0
    objects = sum(1 for n in fa if n.endswith(('.o', '.obj')))
    work = Path(tempfile.mkdtemp(prefix='launch_dispatch_check-'))
    try:
        for name in sorted(set(fa) | set(fb)):
            refusal = library_refusal(name, (a, b), a_dir, b_dir)
            if refusal:
                bad += 1
                report.append('UNEXPECTED %s: %s' % (name, refusal))
                continue
            if fa.get(name) == fb.get(name):
                continue
            where = '' if name in fa and name in fb else ' only on %s' % ('A' if name in fa else 'B')
            if name in TIMING_LOGS:
                info.append('info      %s%s: %s' % (name, where, TIMING_LOGS[name]))
                continue
            why = build_system(name)
            if why:
                problem = ('nothing to diff' if where else
                           kept_copy_problem(name, a_dir, b_dir, fa[name], fb[name]))
                lines = ['(no diff: %s)' % problem] if problem else plain_diff(Path(a_dir) / name, Path(b_dir) / name)
                info.append('info      %s%s: build-system input, not in the verdict: %s; %d diff lines%s' % (
                    name, where, why, 0 if problem else len(lines), ' in %s' % Path(diff_out).name if diff_out else ''))
                diffs += ['=== %s' % name] + lines
                continue
            if where:
                bad += 1
                report.append('UNEXPECTED %s%s' % (name, where))
                continue
            u = unpacked(name)
            if u is None:
                bad += 1
                report.append('UNEXPECTED %s differs' % name)
                continue
            what, check = u
            problem = kept_copy_problem(name, a_dir, b_dir, fa[name], fb[name])
            if problem:
                bad += 1
                report.append('UNEXPECTED %s differs (%s; unpacking needs both)' % (name, problem))
                continue
            problem = check(Path(a_dir) / name, Path(b_dir) / name, work / str(len(report)))
            if problem:
                bad += 1
                report.append('UNEXPECTED %s: %s' % (name, problem))
            else:
                report.append('same      %s unpacked: %s' % (name, what))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if diff_out:
        Path(diff_out).write_text('\n'.join(diffs) + '\n', encoding='utf-8', errors='backslashreplace')
    same = sum(1 for n in fa if fa[n] == fb.get(n))
    head = ('IDENTITY %s: %d files on A, %d on B, %d identical (%d objects on A, all %s), %d unexpected; '
            '%d build-system or timing files differ (information, not in the verdict)' % (
                'PASS' if not bad else 'FAIL', len(fa), len(fb), same, objects,
                'identical' if not any(n.endswith(('.o', '.obj')) and fa[n] != fb.get(n) for n in fa)
                else 'NOT identical', bad, len(info)))
    return bad, [head] + report + info + ([] if diff_out else diffs)


# ------------------------------------------------------------------------------------------- ELF sections

def sections(path):
    data = Path(path).read_bytes()
    assert data[:4] == b'\x7fELF' and data[4] == 1 and data[5] == 1, 'ELF32 little-endian expected'
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2E)
    heads = [struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize) for i in range(shnum)]
    strtab = heads[shstrndx]
    names = data[strtab[4]:strtab[4] + strtab[5]]
    out = {}
    for h in heads:
        name = names[h[0]:names.index(b'\0', h[0])].decode('ascii', 'replace')
        sh_type, flags, addr, offset, size = h[1], h[2], h[3], h[4], h[5]
        if not flags & 2:  # SHF_ALLOC
            continue
        body = b'' if sh_type == 8 else data[offset:offset + size]  # SHT_NOBITS has no bytes
        out[name] = (sh_type, flags, addr, size, hashlib.sha256(body).hexdigest())
    return out


def elfdiff(a, b):
    sa, sb = sections(a), sections(b)
    lines = []
    for name in sorted(set(sa) | set(sb)):
        if sa.get(name) != sb.get(name):
            lines.append('DIFF %s A=%s B=%s' % (name, sa.get(name), sb.get(name)))
    total = sum(v[3] for v in sa.values())
    lines.insert(0, 'ELFDIFF %s: %d allocated sections on A (%d bytes), %d on B, %d differ' % (
        'PASS' if not lines else 'FAIL', len(sa), total, len(sb), len(lines)))
    return len(lines) - 1 if len(lines) > 1 else 0, lines


# ------------------------------------------------------------------------------------------- rules

def launcher_marker(script):
    text = Path(script).read_text(encoding='utf-8')
    m = re.search(r"^MARKER = '([^']+)'", text, re.M)
    if m:
        return m.group(1)
    if Path(script).name == 'vine_old_observer_compile.py':
        return '--'
    return None


def rule_sources(script):
    """The .c names a launcher's code (and the modules it routes through) names."""
    script = Path(script)
    texts = [script.read_text(encoding='utf-8')]
    if script.name == 'vine_old_observer_compile.py':
        for extra in ('vine_old_parent.py', 'utwindow_compile.py'):
            if (script.parent / extra).exists():
                texts.append((script.parent / extra).read_text(encoding='utf-8'))
    names = set()
    for t in texts:
        names.update(re.findall(r"([A-Za-z0-9_]+\.c)\b(?![a-z])", t))
    return names


def without(tokens, script, marker):
    """The launcher tokens with one launcher (its interpreter, flags, script, options and marker) removed."""
    toks = list(tokens)
    i = next(k for k, t in enumerate(toks) if native(t).replace('\\', '/').endswith('/' + Path(script).name))
    start = i
    while start > 0 and (toks[start - 1].startswith('-') or toks[start - 1] == 'utf8'):
        start -= 1
    start -= 1  # the interpreter
    end = i + 1
    while end < len(toks) and toks[end].startswith('--') and toks[end].endswith('-pad'):
        end += 1
    # the marker is the last one of its kind before the compiler: markers accumulate innermost first
    m = max(k for k, t in enumerate(toks) if t == marker and k > end)
    return toks[:start] + toks[end:m] + toks[m + 1:]


def side_files(obj):
    obj = Path(obj)
    stem = obj.name[:-2] if obj.name.endswith('.o') else obj.stem
    return sorted(p for p in obj.parent.iterdir() if p.is_file() and (p.name == obj.name or
                                                                      p.name.startswith(stem + '.')))


def shell_for(build):
    cache = (Path(build) / 'CMakeCache.txt').read_text(encoding='utf-8', errors='replace')
    m = re.search(r'^CMAKE_MAKE_PROGRAM:\w+=(.+)$', cache, re.M)
    make = m.group(1).strip() if m else None
    if make and os.name == 'nt':
        make = native(make)
        if re.match(r'^/(?![a-zA-Z]/)', make):  # an MSYS path such as /usr/bin/make.exe: devkitPro's msys2 root
            base = devkitpro()
            make = str(base / 'msys2' / make.lstrip('/')) if base else None
        if make and (Path(make).parent / 'sh.exe').exists():
            return str(Path(make).parent / 'sh.exe')
        # Another sh (Git's) mounts other paths (/opt/devkitpro): its probes would fail for reasons of their own.
        raise ValueError('no sh.exe beside the make program %s (CMAKE_MAKE_PROGRAM %s)' % (
            make, m.group(1).strip() if m else 'not set'))
    if make and (Path(make).parent / 'sh').exists():
        return str(Path(make).parent / 'sh')
    return shutil.which('sh') or 'sh'


def make_variables(build):
    """The `NAME = value` variables of the ctrport target's flags.make (C_DEFINES, C_INCLUDES, C_FLAGS ...)."""
    path = Path(build) / 'CMakeFiles/ctrport.dir/flags.make'
    if not path.is_file():
        return {}
    out = {}
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*) = (.*)$', line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def expand_make(line, variables):
    """A build.make recipe line as make hands it to the shell: $(NAME) from flags.make, $$ as $. Anything else
    make would expand ($@, ${X}, an unknown or nested variable) is refused (ValueError), never left empty."""
    def one(m):
        if m.group(0) == '$$':
            return '$'
        name = m.group(1)
        if name is None or name not in variables:
            raise ValueError('make syntax %r not understood (flags.make defines %s)' % (
                m.group(0), ', '.join(sorted(variables)) or 'nothing'))
        if '$' in variables[name]:
            raise ValueError('make variable %s refers to others: %r' % (name, variables[name][:120]))
        return variables[name]
    return re.sub(r'\$\(([A-Za-z_][A-Za-z0-9_]*)\)|\$\$|\$.?', one, line)


def probe_groups(checks):
    """The checks in groups that share no file: two checks of one object, or of objects whose side files can overlap
    (same directory, one stem the other's or its prefix before a '.'), are in one group and run one after the other.
    Each check saves, recompiles, hashes and restores its object's files; run at once, one check's recompile would
    show in another's hash and its restore would undo the other's result (c26rev4 BF-F2)."""
    keys = sorted({obj for _n, _m, _s, obj, _l in checks})
    parent = {k: k for k in keys}

    def root(k):
        while parent[k] != k:
            parent[k] = parent[parent[k]]
            k = parent[k]
        return k

    def stem(obj):
        name = Path(obj).name
        return name[:-2] if name.endswith('.o') else Path(name).stem

    by_dir = {}
    for k in keys:
        by_dir.setdefault(Path(k).parent.as_posix(), []).append(k)
    for objs in by_dir.values():
        for i, a in enumerate(objs):
            for b in objs[i + 1:]:
                sa, sb = stem(a), stem(b)
                if sa == sb or sa.startswith(sb + '.') or sb.startswith(sa + '.'):
                    parent[root(a)] = root(b)
    groups = {}
    for check in checks:
        groups.setdefault(root(check[3]), []).append(check)
    return [groups[k] for k in sorted(groups)]


def file_attributes(st):
    """What the manifest keeps of a path's attributes: Windows file attributes, else the mode bits."""
    return getattr(st, 'st_file_attributes', None) if os.name == 'nt' else stat.S_IMODE(st.st_mode)


REPARSE_POINT = 0x400  # FILE_ATTRIBUTE_REPARSE_POINT: a junction, a symlink or any other reparse point
if os.name == 'nt':
    import ctypes
    from ctypes import wintypes

    class _StreamData(ctypes.Structure):
        _fields_ = [('StreamSize', ctypes.c_longlong), ('cStreamName', ctypes.c_wchar * 296)]

    _sk32 = ctypes.WinDLL('kernel32', use_last_error=True)
    _sk32.FindFirstStreamW.argtypes = (wintypes.LPCWSTR, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD)
    _sk32.FindFirstStreamW.restype = ctypes.c_void_p
    _sk32.FindNextStreamW.argtypes = (ctypes.c_void_p, ctypes.c_void_p)
    _sk32.FindNextStreamW.restype = wintypes.BOOL
    _sk32.FindClose.argtypes = (ctypes.c_void_p,)
    _sk32.FindClose.restype = wintypes.BOOL


def named_streams(path):
    """The named data streams of a file or directory (not the unnamed '::$DATA'). Windows only; OSError when they
    cannot be listed."""
    if os.name != 'nt':
        return []
    data = _StreamData()
    h = _sk32.FindFirstStreamW(str(path), 0, ctypes.byref(data), 0)
    if h is None or h == ctypes.c_void_p(-1).value:
        err = ctypes.get_last_error()
        if err == 38:  # ERROR_HANDLE_EOF: no stream at all (a directory without any)
            return []
        raise ctypes.WinError(err)
    try:
        names = [data.cStreamName]
        while _sk32.FindNextStreamW(h, ctypes.byref(data)):
            names.append(data.cStreamName)
        err = ctypes.get_last_error()
        if err != 38:
            raise ctypes.WinError(err)
    finally:
        _sk32.FindClose(h)
    return [n for n in names if n != '::$DATA']


def unsupported(path, st):
    """Why rules cannot copy or check this entry exactly, or None: a reparse point (junction, symlink, other), a file
    with more than one hard link, a named stream, anything but a plain file or directory."""
    if getattr(st, 'st_file_attributes', 0) & REPARSE_POINT or stat.S_ISLNK(st.st_mode):
        return 'a reparse point (junction or link)'
    if not (stat.S_ISDIR(st.st_mode) or stat.S_ISREG(st.st_mode)):
        return 'not a plain file or directory'
    if stat.S_ISREG(st.st_mode) and st.st_nlink > 1:
        return 'a file with %d hard links' % st.st_nlink
    streams = named_streams(path)
    if streams:
        return 'named streams %s' % ', '.join(streams)
    return None


def walk_entries(root, strict=True):
    """(relative path, path, lstat) for root ('.') and everything under it, a directory before its contents, never
    following anything: os.scandir + os.lstat, no reparse point is entered. strict: an unsupported entry is a
    ValueError; otherwise it is yielded (with its reason as a fourth item) and not entered. Any listing or stat
    error raises: the walk fails closed."""
    root = Path(root)
    pending = [(root, '.')]
    while pending:
        path, rel = pending.pop()
        st = os.lstat(path)
        why = unsupported(path, st)
        if why and strict:
            raise ValueError('%s: %s' % (why, path))
        yield rel, path, st, why
        if stat.S_ISDIR(st.st_mode) and not why:
            with os.scandir(path) as it:
                names = sorted(e.name for e in it)
            for name in reversed(names):
                pending.append((path / name, name if rel == '.' else rel + '/' + name))


def tree_manifest(root):
    """Every directory and file under root (root itself as '.'): relative path -> ('d', mtime_ns, attributes) or
    ('f', size, mtime_ns, attributes, sha256). Every file is hashed, every time; no size or time shortcut. An
    unsupported entry anywhere (see unsupported()) is a ValueError, before anything under it is read."""
    out = {}
    for rel, path, st, _why in walk_entries(root):
        out[rel] = (('d', st.st_mtime_ns, file_attributes(st)) if stat.S_ISDIR(st.st_mode) else
                    ('f', st.st_size, st.st_mtime_ns, file_attributes(st), sha256(path)))
    return out


def tree_changes(root, base):
    """What a run left in its copy, against the manifest it started from: manifest_differences() lines, with an
    unsupported entry (a junction, a hard link, a named stream) listed by its reason and not entered. For the report
    only: the copy is deleted whatever it holds."""
    now, odd = {}, []
    for rel, path, st, why in walk_entries(root, strict=False):
        if why:
            odd.append('%s: %s' % (rel, why))
            continue
        now[rel] = (('d', st.st_mtime_ns, file_attributes(st)) if stat.S_ISDIR(st.st_mode) else
                    ('f', st.st_size, st.st_mtime_ns, file_attributes(st), sha256(path)))
    odd_paths = {line.rsplit(': ', 1)[0] for line in odd}
    lines = [l for l in manifest_differences(base, now) if l.split(': ', 1)[0] not in odd_paths]
    return sorted(lines + odd, key=lambda l: l.split(': ', 1)[0])  # by path, as manifest_differences()


def manifest_differences(base, now):
    """Sorted '<path>: <what>' lines, one per path whose entry differs."""
    lines = []
    for rel in sorted(set(base) | set(now)):
        a, b = base.get(rel), now.get(rel)
        if a == b:
            continue
        if a is None:
            what = 'new %s' % ('directory' if b[0] == 'd' else 'file')
        elif b is None:
            what = 'missing %s' % ('directory' if a[0] == 'd' else 'file')
        elif a[0] != b[0]:
            what = 'type changed'
        else:
            fields = ('mtime', 'attributes') if a[0] == 'd' else ('size', 'mtime', 'attributes', 'bytes')
            what = ', '.join(f for f, x, y in zip(fields, a[1:], b[1:]) if x != y) + ' changed'
        lines.append('%s: %s' % (rel, what))
    return lines


_SETTABLE = 0x1 | 0x2 | 0x4 | 0x20 | 0x100 | 0x1000 | 0x2000  # READONLY HIDDEN SYSTEM ARCHIVE TEMPORARY OFFLINE NOT_INDEXED


def set_attributes(path, attributes):
    """Give path the manifest's attributes (Windows: the settable file attributes; else the mode bits)."""
    if os.name == 'nt':
        value = (attributes & _SETTABLE) or 0x80  # FILE_ATTRIBUTE_NORMAL stands alone
        if not _jk32.SetFileAttributesW(str(path), value):
            raise ctypes.WinError(ctypes.get_last_error())
    else:
        os.chmod(path, attributes)


def path_identity(path):
    """(volume, file ID) of the entry at path itself, not following a link: os.lstat's st_dev and st_ino. Measured
    here (Python 3.14, Windows 11, NTFS on C: and E:): nonzero, the same on every read, kept across a rename, and
    different for a new directory made at the same path."""
    st = os.lstat(path)
    return (st.st_dev, st.st_ino)


class CopyNotMade(Exception):
    """copy_tree failed. made: the identity of the directory copy_tree itself made at dest (its exclusive mkdir
    succeeded), or None when it made nothing there: then whatever is at dest is not the copy's and is never deleted."""

    def __init__(self, message, made=None):
        super().__init__(message)
        self.made = made


def copy_tree(source, dest, base):
    """Make dest a copy of the pristine tree source as its manifest base records it: every directory, every file's
    bytes, then every mtime and attribute (files first, then directories deepest first, since making entries changes
    a directory's mtime). dest is made with an exclusive mkdir, which fails if anything is there: only a directory
    this call made is the copy's. Its identity (path_identity) is read right after the mkdir and returned; nothing
    may delete the build path unless it still has that identity. The caller checks tree_manifest(dest) == base.
    Raises CopyNotMade, with the identity when the mkdir succeeded."""
    source, dest = Path(source), Path(dest)
    try:
        os.mkdir(dest)
    except OSError as e:
        raise CopyNotMade('%s cannot be made: %s: %s' % (dest, type(e).__name__, str(e)[:200])) from e
    try:
        made = path_identity(dest)
    except OSError as e:
        raise CopyNotMade('%s was made but cannot be read back: %s' % (dest, e)) from e
    try:
        dirs = sorted((r for r in base if base[r][0] == 'd' and r != '.'), key=lambda r: len(r.split('/')))
        for rel in dirs:
            os.mkdir(dest / rel)
        for rel, entry in base.items():
            if entry[0] == 'f':
                shutil.copyfile(source / rel, dest / rel)
        for rel, entry in sorted(base.items(), key=lambda kv: (kv[1][0] == 'd', -len(kv[0].split('/'))
                                                                if kv[0] != '.' else 1)):
            p = dest if rel == '.' else dest / rel
            mtime, attributes = (entry[1], entry[2]) if entry[0] == 'd' else (entry[2], entry[3])
            os.utime(p, ns=(mtime, mtime))
            set_attributes(p, attributes)
    except (OSError, ValueError) as e:
        raise CopyNotMade('%s: %s' % (type(e).__name__, str(e)[:200]), made) from e
    return made


class NotRemoved(Exception):
    """A path remove_tree could not classify or remove; it is left where it is. busy: Windows said it is in use
    (a sharing violation, or a directory not yet empty), which may pass by itself. foreign: the path no longer names
    the directory the caller made (remove_copy), so nothing was deleted."""

    def __init__(self, message, busy=False, foreign=False):
        super().__init__(message)
        self.busy = busy
        self.foreign = foreign


def remove_tree(root):
    """Delete root and everything under it WITHOUT following anything. A reparse point (junction, directory or file
    symlink) is removed as an entry (rmdir or unlink of the link itself), never its contents. A read-only plain file
    or directory is made writable first, but never a file with more than one hard link (that would change the other
    name's file too): it is unlinked as it is. Anything that cannot be classified or removed raises NotRemoved with
    its path, and stays."""
    root = Path(root)
    try:
        st = os.lstat(root)
    except OSError as e:
        raise NotRemoved('%s: cannot be read: %s' % (root, e))
    attrs = getattr(st, 'st_file_attributes', 0)
    try:
        if attrs & REPARSE_POINT or stat.S_ISLNK(st.st_mode):
            if attrs & 0x10:  # a directory junction or directory symlink: the link entry only
                os.rmdir(root)
            else:
                os.unlink(root)
        elif stat.S_ISDIR(st.st_mode):
            with os.scandir(root) as it:
                names = [e.name for e in it]
            for name in names:
                remove_tree(root / name)
            if os.name == 'nt' and attrs & 0x1:
                set_attributes(root, attrs & ~0x1)
            elif os.name != 'nt' and not st.st_mode & stat.S_IWUSR:
                os.chmod(root, stat.S_IMODE(st.st_mode) | stat.S_IWUSR)
            os.rmdir(root)
        elif stat.S_ISREG(st.st_mode):
            if os.name == 'nt' and attrs & 0x1 and st.st_nlink == 1:
                set_attributes(root, attrs & ~0x1)
            os.unlink(root)
        else:
            raise NotRemoved('%s: not a file, directory or link: left in place' % root)
    except NotRemoved:
        raise
    except OSError as e:
        raise NotRemoved('%s: %s: left in place' % (root, e), busy=getattr(e, 'winerror', None) in (32, 145))


REMOVE_GRACE = 10  # seconds a copy that is in use gets before it counts as not deleted


def remove_copy(root, identity):
    """remove_tree(root) for the copy whose directory has `identity` (what copy_tree returned), tried again every
    0.2 s for up to REMOVE_GRACE seconds while Windows says something is in use. Measured here (Python 3.14,
    Windows 11): after a timeout kill, the job's active count reaches 0 before the killed processes' handles are all
    closed; rmdir of the build directory, their working directory, then fails with WinError 32 for a moment.
    Before EVERY attempt, the first and each retry, root is read again without following links: if it is gone or
    names anything but that directory (renamed away and replaced, say), nothing is deleted and NotRemoved(foreign)
    is raised. A path still in use after the grace, or one that fails for any other reason, raises NotRemoved.
    The check and the delete are separate calls: something outside the job could still swap the path between them
    (see 'no sandbox' in the module notes)."""
    deadline = time.monotonic() + REMOVE_GRACE
    while True:
        try:
            now = path_identity(root)
        except OSError as e:
            now = 'nothing readable (%s)' % type(e).__name__
        if now != identity:
            raise NotRemoved('%s no longer holds this run\'s copy (it was volume %x file %x; the path now holds %s): '
                             'nothing deleted; where the copy went is not known' % (
                                 root, identity[0], identity[1],
                                 now if isinstance(now, str) else 'volume %x file %x' % now), foreign=True)
        try:
            return remove_tree(root)
        except NotRemoved as e:
            if not e.busy or time.monotonic() >= deadline:
                raise
        time.sleep(0.2)


def source_files(root, tracked=True):
    """The worktree's files outside build/ and scratch/ as git lists them: tracked (git ls-files, whatever the index
    flags say; None when not asked for) and untracked or ignored (--others). Raises OSError when git cannot tell."""
    def ls(*extra):
        r = subprocess.run(['git', '-C', str(root), 'ls-files', '-z', '--full-name'] + list(extra) +
                           ['--', '.', ':(exclude)build', ':(exclude)scratch'], capture_output=True, timeout=120,
                           stdin=subprocess.DEVNULL)
        if r.returncode:
            raise OSError('git ls-files: status %d: %s' % (r.returncode, r.stderr.decode('utf-8', 'replace')[:200]))
        return sorted(set(n for n in r.stdout.decode('utf-8', 'surrogateescape').split('\0') if n))
    return (ls() if tracked else None), ls('--others')


def git_identity(root):
    """HEAD, the index file's sha256 and the .git pointer's bytes: what git reads the tracked list against."""
    def git(*args):
        r = subprocess.run(['git', '-C', str(root)] + list(args), capture_output=True, timeout=120,
                           stdin=subprocess.DEVNULL)
        if r.returncode:
            raise OSError('git %s: status %d' % (args[0], r.returncode))
        return r.stdout.decode('utf-8', 'surrogateescape').strip()
    index = Path(git('rev-parse', '--path-format=absolute', '--git-path', 'index'))
    pointer = Path(root) / '.git'
    return (git('rev-parse', 'HEAD'), sha256(index) if index.exists() else 'no index',
            pointer.read_bytes() if pointer.is_file() else 'a directory' if pointer.is_dir() else 'missing')


def source_state(root, tracked=None):
    """The worktree outside build/ and scratch/, read directly: every tracked path's bytes (the list frozen at the
    start: tracked), every untracked or ignored file's bytes (listed again each time, so a new one shows), and the
    git identity. Equal states mean no source changed, whatever the index flags (skip-worktree,
    assume-unchanged) say. Raises OSError when git cannot tell."""
    listed, others = source_files(root, tracked is None)
    tracked = listed if tracked is None else tracked
    root = Path(root)

    def digest(rel):
        # lstat, never following: a tracked file swapped for a link to the same bytes still differs.
        p = root / rel
        try:
            st = os.lstat(p)
            if getattr(st, 'st_file_attributes', 0) & REPARSE_POINT or stat.S_ISLNK(st.st_mode):
                return 'a link'
            return sha256(p) if stat.S_ISREG(st.st_mode) else 'a directory' if stat.S_ISDIR(st.st_mode) else 'other'
        except FileNotFoundError:
            return 'missing'
        except OSError as e:
            return 'unreadable: %s' % e.strerror
    return dict(tracked={r: digest(r) for r in tracked}, others={r: digest(r) for r in others},
                identity=git_identity(root))


def freeze_sources(root):
    """At the start of rules: refresh the index's stat data once (git status), then freeze the tracked path list and
    take the first source_state(). (tracked list, state)."""
    subprocess.run(['git', '-C', str(root), 'status', '--porcelain'], capture_output=True, timeout=120,
                   stdin=subprocess.DEVNULL)
    tracked = source_files(root)[0]
    return tracked, source_state(root, tracked)


def state_differences(base, now):
    """Lines naming what differs between two source_state()s."""
    lines = ['git %s changed' % n for n, a, b in zip(('HEAD', 'index', '.git pointer'), base['identity'],
                                                   now['identity']) if a != b]
    for kind in ('tracked', 'others'):
        a, b = base[kind], now[kind]
        lines += ['%s %s: %s' % (kind, r, 'new' if r not in a else 'gone' if r not in b else 'bytes changed')
                  for r in sorted(set(a) | set(b)) if a.get(r) != b.get(r)]
    return lines


RULES_SIBLINGS = ('.rules-copy-', '.rules-pristine-')


def rules_leftovers(build):
    """Siblings of the build directory a rules run made and did not remove (any tree's, any case)."""
    return sorted(str(p) for p in Path(build).parent.iterdir() if p.name.lower().startswith(RULES_SIBLINGS))


# Lines rules prints only when something went wrong, matched where rules prints them (the start of a line). A marker
# found anywhere else is not one: rules' own TOUCHED OUTSIDE summary names "NOT CLEAN" in passing (gate iii, r12).
RULES_FAILURE_STARTS = ('NOT CLEAN after ', 'PRISTINE TREE LEFT AT ', 'RECOVER BY HAND: ', 'EXPECTED ACTIVE ',
                        'TREE CHANGED ', 'LEFT: ')
# Every line one rules run prints, each in its full format (rules() below): what rules_receipt_problems reads.
_RP_CHECK = re.compile(r'(?P<name>\S+\.py) +(?P<source>\S+) +(?P<obj>\S+) +(?P<verdict>RED \(.+\)|same without it|'
                       r'ERROR \(.+\)) (?P<seconds>\d+) s(?: (?P<problem>RESTORE FAILED|WRITERS MAY BE ALIVE|NOT RUN))?')
# The only two RED verdicts rules prints (one() in rules): a nonzero status, or the sorted, distinct names of the files
# that differ. Any other text in RED (...) earns no credit (c26rev4 BF-RP2-F2).
_RP_RED = re.compile(r'RED \((?:fails without it: status (?P<status>-?[1-9][0-9]*)|'
                     r'differs without it: (?P<files>[^,()\s](?:[^,()]*[^,()\s])?(?:, [^,()\s](?:[^,()]*[^,()\s])?)*))\)')


def _rp_red(verdict):
    """True for a RED verdict in one of rules' two exact forms (a differing-files list sorted and without repeats)."""
    m = _RP_RED.fullmatch(verdict)
    if not m:
        return False
    files = m.group('files')
    return files is None or files.split(', ') == sorted(set(files.split(', ')))


_RP_TOUCHED = re.compile(r'TOUCHED OUTSIDE (?P<label>control of (?P<cobj>\S+)|(?P<pobj>\S+) without '
                         r'(?P<pname>\S+\.py)): (?P<count>\d+) paths, (?P<fate>gone with the copy|in the copy, which '
                         r'was NOT deleted): (?P<paths>.+)')
_RP_RULES = re.compile(r'RULES (?P<tree>\S+): (?P<launchers>\d+) launchers in the chain, (?P<checks>\d+) checks in '
                       r'(?P<groups>\d+) groups that share no file')
_RP_LAUNCHER = re.compile(r'LAUNCHER (?P<name>\S+\.py) +(?P<state>\S.*)')
_RP_STATE = re.compile(r'ACTIVE \((?P<red>\d+) of (?P<n>\d+) named sources change without it\)|'
                       r'inactive in this build \((?P<none>\d+) named sources unchanged without it\)|'
                       r'(?P<nosource>no named source in this build)|(?P<notprobed>not probed \(.+\))')
_RP_TOUCHED_SUMMARY = re.compile(r"TOUCHED OUTSIDE: (?P<count>\d+) runs left paths besides their own object's in their "
                                 r"copy \(deleted with it unless named NOT CLEAN\)")
_RP_TREE = re.compile(r'TREE (?P<tree>\S+): all (?P<entries>\d+) directories and files equal to the tree rules found '
                      r'\(bytes, sizes, mtimes, attributes\)')
_RP_KINDS = (('check', _RP_CHECK), ('touched', _RP_TOUCHED), ('rules', _RP_RULES), ('launcher', _RP_LAUNCHER),
             ('touched-summary', _RP_TOUCHED_SUMMARY), ('tree', _RP_TREE))


def rules_receipt_problems(text, expected, tree=None):
    """What a receipt reviewer must reject in the output of ONE rules run: [] only when the text is exactly one clean,
    complete, self-consistent run in which every launcher in `expected` (unique names, with or without .py) is ACTIVE.

    Input: the undecorated stdout of one direct rules call, as the gate (iii) driver captures it in process. A job
    transcript with other lines in it, a line with a prefix or an indent, or two runs joined are rejected, not read
    around. It does not see rules' return code: the caller checks rc == 0 itself. `tree`: the build directory's name,
    when the caller knows it.

    Every line must be one of a clean run's own lines, in full and in rules' order: per-run lines (TOUCHED OUTSIDE
    lines of copies that were deleted, each followed by its check's line), then exactly once each the RULES summary,
    one LAUNCHER line per launcher in the chain, the TOUCHED OUTSIDE summary and the TREE equality line, and nothing
    after. Then everything must reconcile:
    - check lines: exactly as many as the RULES summary says; each RED in one of rules' two exact forms (a nonzero
      status, or the sorted distinct files that differ) or same, none ERROR, no problem suffix; one line per launcher
      and object, one source per object; in probe_groups' groups, as many as the summary says, printed group by group
      in its order, each group's checks in chain (LAUNCHER line) order, then by source;
    - LAUNCHER lines: exactly as many as the summary says, one per name; each state matches that launcher's check
      lines (ACTIVE a of n: n lines, a RED, a >= 1; inactive n: n lines, none RED; no named source: no line); no
      launcher "not probed"; every check line's launcher has a LAUNCHER line;
    - per-run TOUCHED OUTSIDE lines: exactly as many as the summary says, each listing as many paths as it counts;
    - the TREE line names the RULES summary's tree, and both name `tree` when it is given."""
    lines = text.splitlines()
    problems = []
    for start in RULES_FAILURE_STARTS:
        hits = [l for l in lines if l.startswith(start)]
        if hits:
            problems.append('%d lines start with %r, first: %s' % (len(hits), start, hits[0][:300]))
    hits = [l for l in lines if re.match(r'RULES \S+: not run:', l)]
    if hits:
        problems.append('not run: %s' % hits[0][:300])
    wanted = [w if w.endswith('.py') else w + '.py' for w in expected]
    if not wanted or len(set(wanted)) != len(wanted):
        problems.append('the expected names are empty or repeat a name: %s' % ' '.join(wanted))
    checks, touched, envelope, unread = [], [], [], []
    pending = []  # per-run TOUCHED OUTSIDE lines not yet followed by their check's line
    for i, line in enumerate(lines, 1):
        found = [(kind, m) for kind, rx in _RP_KINDS for m in [rx.fullmatch(line)] if m]
        if len(found) != 1:
            unread.append('line %d: %s' % (i, line[:300]))
            continue
        kind, m = found[0]
        if kind in ('check', 'touched') and envelope:
            unread.append("line %d, a run's line after the summary began: %s" % (i, line[:300]))
            continue
        if kind == 'touched':
            touched.append(m)
            pending.append((i, m))
        elif kind == 'check':
            checks.append(m)
            for j, t in pending:
                if (t.group('cobj') or t.group('pobj')) != m.group('obj') or \
                        t.group('pname') not in (None, m.group('name')):
                    unread.append('line %d: TOUCHED OUTSIDE %s, but the check line after it is %s on %s' % (
                        j, t.group('label')[:200], m.group('name'), m.group('obj')))
            pending = []
        else:
            if pending:
                unread.append('line %d: TOUCHED OUTSIDE %s with no check line after it' % (
                    pending[0][0], pending[0][1].group('label')[:200]))
                pending = []
            envelope.append((kind, m, i))
    if unread:
        problems.append("%d lines are not one clean rules run's own lines in its order, first: %s" % (
            len(unread), unread[0]))
    # What each line says on its own, reported even when the run's summary is broken (a failed run's often is).
    bad = [m.group(0) for m in checks if m.group('problem') or m.group('verdict').startswith('ERROR')]
    if bad:
        problems.append('%d of %d checks errored or did not end clean, first: %s' % (len(bad), len(checks), bad[0][:300]))
    malformed = [m.group(0) for m in checks if m.group('verdict').startswith('RED (') and not _rp_red(m.group('verdict'))]
    if malformed:
        problems.append('%d check lines have a RED verdict rules never prints, first: %s' % (
            len(malformed), malformed[0][:300]))
    # One check per launcher and object, one source per object (c26rev4 BF-RP2-F1): a repeated line is not evidence
    # that another source ran.
    seen, source_of = {}, {}
    for m in checks:
        key = (m.group('name'), m.group('obj'))
        seen[key] = seen.get(key, 0) + 1
        source_of.setdefault(m.group('obj'), set()).add(m.group('source'))
    repeated = sorted('%s on %s (%d lines)' % (n, o, c) for (n, o), c in seen.items() if c > 1)
    if repeated:
        problems.append('%d launcher/object checks appear more than once, first: %s' % (len(repeated), repeated[0]))
    mixed = sorted('%s from %s' % (o, ', '.join(sorted(s))) for o, s in source_of.items() if len(s) > 1)
    if mixed:
        problems.append('%d objects are named with more than one source, first: %s' % (len(mixed), mixed[0][:300]))
    states = {}
    for _kind, m, i in (e for e in envelope if e[0] == 'launcher'):
        if m.group('name') in states:
            problems.append('two LAUNCHER lines for %s (line %d)' % (m.group('name'), i))
        states[m.group('name')] = m.group('state')
    for want in wanted:
        if not states.get(want, '').startswith('ACTIVE ('):
            problems.append('%s is not ACTIVE: %s' % (want, states.get(want, 'no LAUNCHER line')))
    for m in touched:
        if m.group('fate') != 'gone with the copy':
            problems.append('a copy was not deleted: %s' % m.group(0)[:300])
        listed = len(m.group('paths').split('; '))
        if listed != int(m.group('count')):
            problems.append('TOUCHED OUTSIDE %s: counts %s paths, lists %d' % (
                m.group('label')[:200], m.group('count'), listed))
    # The run as a whole: one complete summary, and every count in it equal to the lines counted.
    kinds = [kind for kind, _m, _i in envelope]
    if len(kinds) < 3 or kinds[0] != 'rules' or kinds[-2:] != ['touched-summary', 'tree'] or \
            any(kind != 'launcher' for kind in kinds[1:-2]):
        problems.append('the summary is not RULES, the LAUNCHER lines, TOUCHED OUTSIDE and TREE, once each in that '
                        'order: found %s' % (' '.join(kinds) or 'no summary line'))
        return problems
    rules_m, summary_m, tree_m = envelope[0][1], envelope[-2][1], envelope[-1][1]
    ran_on = rules_m.group('tree')
    if tree_m.group('tree') != ran_on:
        problems.append('the TREE line names %s, the RULES line %s' % (tree_m.group('tree'), ran_on))
    if tree is not None and ran_on != tree:
        problems.append('the run is of %s, not %s' % (ran_on, tree))
    if len(checks) != int(rules_m.group('checks')):
        problems.append('%d check lines, the RULES line says %s checks' % (len(checks), rules_m.group('checks')))
    if len(envelope) - 3 != int(rules_m.group('launchers')):
        problems.append('%d LAUNCHER lines, the RULES line says %s launchers' % (
            len(envelope) - 3, rules_m.group('launchers')))
    # Groups and order (c26rev4 BF-RP2-N1): rules runs probe_groups(checks) in its order, each group's checks in
    # chain order (the LAUNCHER lines' order), then by source. The same function, given the printed checks, must
    # find the summary's group count and give back the printed order: no group split, reordered or reopened.
    groups = probe_groups([(m.group('name'), None, m.group('source'), m.group('obj'), None) for m in checks])
    if len(groups) != int(rules_m.group('groups')):
        problems.append('the check lines form %d groups, the RULES line says %s' % (len(groups), rules_m.group('groups')))
    printed = [(m.group('name'), m.group('obj')) for m in checks]
    regrouped = [(c[0], c[3]) for g in groups for c in g]
    if printed != regrouped:
        i = next(i for i, (a, b) in enumerate(zip(printed, regrouped)) if a != b)
        problems.append('the check lines are not in rules\' group order: line for %s on %s where %s on %s belongs' % (
            printed[i] + regrouped[i]))
    chain = {name: i for i, name in enumerate(states)}
    for g in groups:
        keys = [(chain.get(c[0], len(chain)), c[2]) for c in g]
        if keys != sorted(keys):
            problems.append('the checks of %s are not in chain order: %s' % (g[0][3], ', '.join(c[0] for c in g)))
    red = {}
    for m in checks:
        red.setdefault(m.group('name'), []).append(_rp_red(m.group('verdict')))
    for name in sorted(set(red) - set(states)):
        problems.append('%d check lines name %s, which has no LAUNCHER line' % (len(red[name]), name))
    for name, state in states.items():
        s = _RP_STATE.fullmatch(state)
        flags = red.get(name, [])
        if not s:
            problems.append('LAUNCHER %s: state not understood: %s' % (name, state[:200]))
        elif s.group('notprobed'):
            problems.append('LAUNCHER %s %s' % (name, state[:200]))
        elif s.group('nosource'):
            if flags:
                problems.append('LAUNCHER %s says no named source, but %d check lines name it' % (name, len(flags)))
        else:
            n, a = int(s.group('n') or s.group('none')), int(s.group('red') or 0)
            if len(flags) != n or sum(flags) != a or (s.group('red') is not None and a < 1):
                problems.append('LAUNCHER %s says %s, but its check lines are %d, %d of them RED' % (
                    name, state[:120], len(flags), sum(flags)))
    if len(touched) != int(summary_m.group('count')):
        problems.append('%d TOUCHED OUTSIDE lines, the summary says %s' % (len(touched), summary_m.group('count')))
    return problems


PROBE_TIMEOUT = 3600  # seconds one control or probe may run before its whole process group is killed
EXIT_GRACE = 2  # seconds the group of a command that exited gets to empty before what is left counts as outliving it
SETTLE = 30  # seconds a killed group gets to empty; past that, writers of the object may be alive
_TESTPROC = []


def testproc():
    """tools/testproc.py, whose kill group (_Group) the probes run in: on Windows a job object the command joins
    while still suspended, before it runs; elsewhere a new session."""
    if not _TESTPROC:
        import importlib.util
        spec = importlib.util.spec_from_file_location('ctrport_testproc', ROOT / 'tools' / 'testproc.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _TESTPROC.append(module)
    return _TESTPROC[0]


if os.name == 'nt':
    import ctypes
    from ctypes import wintypes

    class _JobAccounting(ctypes.Structure):  # JOBOBJECT_BASIC_ACCOUNTING_INFORMATION
        _fields_ = [('TotalUserTime', ctypes.c_int64), ('TotalKernelTime', ctypes.c_int64),
                    ('ThisPeriodTotalUserTime', ctypes.c_int64), ('ThisPeriodTotalKernelTime', ctypes.c_int64),
                    ('TotalPageFaultCount', wintypes.DWORD), ('TotalProcesses', wintypes.DWORD),
                    ('ActiveProcesses', wintypes.DWORD), ('TotalTerminatedProcesses', wintypes.DWORD)]

    _jk32 = ctypes.WinDLL('kernel32', use_last_error=True)
    _jk32.QueryInformationJobObject.argtypes = (wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD,
                                                ctypes.POINTER(wintypes.DWORD))
    _jk32.QueryInformationJobObject.restype = wintypes.BOOL
    _jk32.SetFileAttributesW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD)
    _jk32.SetFileAttributesW.restype = wintypes.BOOL


def group_alive(group, proc):
    """Processes still alive in the kill group: the job's active count on Windows; elsewhere 1 or 0, whether any
    process is left in the process group. Raises OSError when that cannot be read."""
    if os.name == 'nt':
        if not group.job:
            raise OSError('no job object')
        info = _JobAccounting()
        if not _jk32.QueryInformationJobObject(group.job, 1, ctypes.byref(info), ctypes.sizeof(info), None):
            raise ctypes.WinError(ctypes.get_last_error())
        return info.ActiveProcesses
    try:
        os.killpg(proc.pid, 0)
    except ProcessLookupError:
        return 0
    except PermissionError:
        return 1
    return 1


def emptied(group, proc, seconds):
    """True once the started process has exited and its group holds no process, within `seconds`."""
    deadline = time.monotonic() + seconds
    while True:
        try:
            if proc.poll() is not None and group_alive(group, proc) == 0:
                return True
        except OSError:
            return False
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.05)


def contained(argv, cwd, env, timeout):
    """Run argv in a kill group made at launch (testproc's _Group: on Windows created suspended, joined to a new job
    object, then resumed, so everything it starts is in the job). Output goes nowhere. The call returns only once
    the group is seen empty, so no process it started can write after it:
      - on a timeout the whole group is killed;
      - a command that exits while a process it started lives on (no launcher leaves one) is an error, and the group
        is killed too;
      - a killed group must then empty within SETTLE seconds.
    (status or None, error or None, empty): empty False means the group was not seen empty, so a writer of the
    command's files may still be alive; error says so then."""
    tp = testproc()
    kwargs = {'cwd': cwd, 'env': env}
    if os.name == 'nt':
        kwargs['creationflags'] = 0x08000000  # CREATE_NO_WINDOW
    group = tp._Group(kwargs)
    proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            **kwargs)
    status = error = None
    try:
        try:
            group.started(proc)
        except tp.ContainmentError as e:  # terminated while still suspended, before it ran
            return None, 'ContainmentError: %s' % str(e)[:200], proc.returncode is not None
        try:
            status = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            error = 'TimeoutExpired: no exit in %s s; its process group was killed' % timeout
            group.kill(proc)
        else:
            if not emptied(group, proc, EXIT_GRACE):
                error = 'a process the command started outlived it; its process group was killed'
                group.kill(proc)
        if not emptied(group, proc, SETTLE):
            return status, 'writers may be alive: the process group was not seen empty %d s after the kill' % (
                SETTLE), False
        return status, error, True
    finally:
        if proc.returncode is None and not group.ended:
            group.kill(proc)
            tp._reap(proc)
        group.close()


def rules(build, jobs=4, only=None, expect_active=()):
    given = Path(build).absolute()
    try:
        why = unsupported(given, os.lstat(given))  # before resolve(), which would follow a junction at the root
    except OSError as e:
        print('RULES %s: not run: the tree cannot be read: %s' % (given.name, e), flush=True)
        return 2
    if why:
        print('RULES %s: not run: the tree holds what rules cannot copy exactly: %s: %s' % (given.name, why, given),
              flush=True)
        return 2
    build = given.resolve()
    lines = compile_lines(build)
    chain = chain_of(build)
    try:
        sh = shell_for(build)
    except ValueError as e:
        print('RULES %s: not run: %s' % (build.name, e), flush=True)
        return 1
    variables = make_variables(build)
    by_name = {}
    for obj, src, line in lines:
        by_name.setdefault(Path(native(src)).name, []).append((obj, src, line))
    checks = []
    for script in chain.levels:
        name = Path(script).name
        if only and name not in only and Path(name).stem not in only:
            continue
        marker = launcher_marker(script)
        if marker is None:
            continue
        for source in sorted(rule_sources(script)):
            for obj, src, line in by_name.get(source, []):
                checks.append((name, marker, source, obj, line))
    lock = threading.Lock()
    results = []
    touched_outside = []  # (run, paths): every run that left anything in its copy besides its own object's files
    # The compiler cache is off: every probe, control included, really compiles.
    env = dict(os.environ, CTRPORT_LAUNCH_DISPATCH_LOG='', CTRPORT_CCACHE='0')
    # Launchers also write work files of their own beside the objects, under names outside each object's stem
    # (utwindow0.s, noticescroll-exit-routing.json, ...), and a run could leave anything in the tree: bytes, times,
    # attributes, links, streams. So nothing is ever restored in place. The tree rules found is moved aside whole
    # (the pristine tree, where nothing runs); every control and probe gets a fresh copy of it at the build path,
    # checked against the first manifest, and that copy is deleted after the run without following anything.
    kept = {}  # 'base': the manifest; 'pristine': the tree moved aside; 'tracked', 'sources': the worktree outside

    def own_path(objpath, rel):
        """Whether a changed path is the run's own: its object, a side file of it, or the object's directory (whose
        mtime changes because the object is deleted and written again)."""
        path = build / rel
        if path == objpath.parent:
            return True
        stem = objpath.name[:-2] if objpath.name.endswith('.o') else objpath.stem
        return path.parent == objpath.parent and (path.name == objpath.name or path.name.startswith(stem + '.'))

    def probe(objpath, toks, label):
        """Copy the pristine tree to the build path and check the copy; run one compile line there, in its own kill
        group, with the object deleted first; once the group is seen empty, read what the run left (for the report)
        and delete the copy without following anything; then check the worktree outside build/ and scratch/.
        Only a directory copy_tree's own exclusive mkdir made is ever deleted, and only while the build path still
        has that directory's identity (remove_copy). A run whose group was not seen empty keeps its copy: writers
        may still be in it, so neither it nor the pristine tree is touched again (kept['held']).
        (status or None, error or None, side-file hashes after, hashes before, problem or None, seconds). problem:
        'WRITERS MAY BE ALIVE' (the group was not seen empty) or 'RESTORE FAILED' (the copy could not be made or
        deleted, the build path no longer held it, or the worktree changed)."""
        base, pristine = kept['base'], kept['pristine']
        began = time.perf_counter()
        status = after = error = None
        before = {}
        empty = True
        left, changes = [], []
        owned = None  # the identity of the directory this run's copy_tree made at the build path
        deleted = False
        try:
            owned = copy_tree(pristine, build, base)
            made = tree_manifest(build)
            if made != base:
                left.append('the copy differs from the tree rules found: %s' % '; '.join(
                    manifest_differences(base, made)[:10]))
        except CopyNotMade as e:
            owned = e.made
            if owned is None and os.path.lexists(build):
                left.append('the build path is not free before the copy: something made %s after the last copy was '
                            'deleted; left in place (%s)' % (build, e))
            else:
                left.append('the copy could not be made: %s' % e)
        except (OSError, ValueError) as e:
            left.append('the copy could not be read back: %s: %s' % (type(e).__name__, str(e)[:200]))
        if left:
            error = left[0]
        else:
            before = {p.name: sha256(p) for p in side_files(objpath)}
            try:
                if objpath.exists():
                    objpath.unlink()
                empty = False  # until contained says so: if it raises, whether the group is empty is not known
                status, error, empty = contained([sh, '-c', shlex.join(toks)], build, env, PROBE_TIMEOUT)
                if empty:
                    after = {p.name: sha256(p) for p in side_files(objpath)}
            except Exception as e:  # a failure to run: the check counts as failed
                error = '%s: %s%s' % (type(e).__name__, str(e)[:200], '' if empty else
                                      ' (writers may be alive: the run\'s group was not seen empty)')
            try:
                changes = tree_changes(build, base)
            except (OSError, ValueError) as e:
                changes = ['(what the run left could not be read: %s: %s)' % (type(e).__name__, str(e)[:200])]
        spent = time.perf_counter() - began
        if owned is not None and not empty:
            # BF11-F2: the group was not seen empty, so something may still write in the copy. It is not deleted,
            # and the pristine tree is not moved back: both stay for recovery by hand.
            kept['held'].append('writers may be alive in the run\'s copy at %s (the run\'s group was not seen empty '
                                'after %s): it was not deleted' % (build, label))
            left.append('the copy was not deleted: writers may be alive in it')
        elif owned is not None:
            try:
                remove_copy(build, owned)
                deleted = True
            except NotRemoved as e:
                left.append('the copy could not be deleted: %s' % e)
                if e.foreign:
                    kept['held'].append(str(e))
        try:
            diffs = state_differences(kept['sources'], source_state(ROOT, kept['tracked']))
            if diffs:
                left.append('the worktree outside build/ and scratch/ changed: %s' % '; '.join(diffs[:10]))
        except (OSError, subprocess.SubprocessError) as e:
            left.append('the worktree outside build/ and scratch/ could not be read: %s' % e)
        outside = [c for c in changes if not own_path(objpath, c.split(': ', 1)[0])]
        if outside:
            touched_outside.append((label, outside))
            with lock:
                print('TOUCHED OUTSIDE %s: %d paths, %s: %s' % (
                    label, len(outside), 'gone with the copy' if deleted else 'in the copy, which was NOT deleted',
                    '; '.join(outside)), flush=True)
        if left:
            with lock:
                print('NOT CLEAN after %s: %s' % (label, '; '.join(left[:20])), flush=True)
        problem = 'WRITERS MAY BE ALIVE' if not empty else 'RESTORE FAILED' if left else None
        return status, error, after, before, problem, spent

    def differing(after, before):
        return sorted(n for n in set(after) | set(before) if after.get(n) != before.get(n))

    def control(obj, head, tail):
        """None when the full, unmodified line rebuilds the build's object and side files byte for byte; else why
        not. Second value: the probe's problem (None when the run ended cleanly and its copy was deleted)."""
        status, error, after, before, problem, _spent = probe(build / obj, head + tail, 'control of %s' % obj)
        if error is not None:
            return 'control: %s' % error, problem
        if problem:
            return 'control: %s' % problem.lower(), problem
        if status:
            return 'control: the full line fails, status %d' % status, problem
        if (build / obj).name not in after:
            return 'control: the full line wrote no object', problem
        if after != before:
            return 'control: the full line builds different bytes: %s' % ', '.join(differing(after, before)), problem
        return None, problem

    def one(check, controls, spoiled):
        """One check. `spoiled` (shared by every check: launchers share work files across a target's objects): once
        any run did not end cleanly, or its copy was not made or deleted cleanly, no later check runs."""
        name, marker, source, obj, line = check
        began = time.perf_counter()
        problem, verdict = None, None
        if spoiled:
            problem = 'NOT RUN'
            verdict = 'ERROR (not run: an earlier run on these files did not end cleanly: %s)' % spoiled[0]
        if verdict is None:
            try:
                toks = shlex.split(expand_make(line, variables))
                cut = next(i for i, t in enumerate(toks) if re.search(r'(^|/)arm-none-eabi-gcc(\.exe)?$', t))
            except (ValueError, StopIteration) as e:
                verdict = 'ERROR (compile line: %s)' % (str(e)[:200] or 'no arm-none-eabi-gcc')
        if verdict is None:
            head, tail = toks[:cut], toks[cut:]
            try:
                cut_head = without(head, name, marker)
            except (StopIteration, ValueError):
                verdict = 'ERROR (launcher not in the line)'
        if verdict is None:
            if obj not in controls:
                controls[obj] = control(obj, head, tail)
            why, problem = controls[obj]
            if why:
                verdict = 'ERROR (%s)' % why
        if verdict is None:
            status, error, after, before, problem, _spent = probe(build / obj, cut_head + tail,
                                                                  '%s without %s' % (obj, name))
            if error is not None:
                verdict = 'ERROR (%s)' % error
            elif status:
                verdict = 'RED (fails without it: status %d)' % status
            elif (build / obj).name not in after:
                verdict = 'ERROR (no object written without it)'
            elif after != before:
                verdict = 'RED (differs without it: %s)' % ', '.join(differing(after, before))
            else:
                verdict = 'same without it'
        if problem and problem != 'NOT RUN' and not spoiled:
            spoiled.append('%s after %s on %s' % (problem, name, obj))
        spent = time.perf_counter() - began
        with lock:
            print('%-34s %-28s %-60s %s %.0f s%s' % (name, source, obj, verdict, spent,
                                                     ' ' + problem if problem else ''), flush=True)
        return (name, source, verdict, problem or ('ERROR' if verdict.startswith('ERROR') else ''))

    # Every run is one at a time, in group order: a target's compiles share its launchers' work files, and every run
    # uses the one build path. (--jobs is kept for old command lines and no longer runs anything at once.)
    groups = probe_groups(checks)
    controls, spoiled = {}, []  # one control per object
    # A pristine tree or copy beside the build directory (any tree's, any case) means a rules run that did not end:
    # the build path may hold a run's copy, not the tree. Nothing starts until it is sorted out (its README.txt).
    leftovers = rules_leftovers(build)
    if leftovers:
        print('RULES %s: not run: an earlier rules run left %s. It did not end: the build path may hold one of its '
              'copies. See the README.txt inside: move the pristine tree back (after deleting what is at the build '
              'path, without following links), then delete the directory.' % (build.name, ', '.join(leftovers)),
              flush=True)
        return 1
    try:
        kept['base'] = tree_manifest(build)
    except ValueError as e:
        print('RULES %s: not run: the tree holds what rules cannot copy exactly: %s' % (build.name, e), flush=True)
        return 2
    except OSError as e:
        print('RULES %s: not run: the tree cannot be read: %s' % (build.name, e), flush=True)
        return 2
    try:
        kept['tracked'], kept['sources'] = freeze_sources(ROOT)
    except (OSError, subprocess.SubprocessError) as e:
        print('RULES %s: not run: the worktree outside build/ and scratch/ cannot be read: %s' % (build.name, e),
              flush=True)
        return 1
    aside = build.parent / ('.rules-pristine-%s-%s-%d' % (build.name, time.strftime('%Y%m%d-%H%M%S'), os.getpid()))
    readme = aside / 'README.txt'
    try:
        os.mkdir(aside)
        readme.write_text(
            'launch_dispatch_check.py rules (pid %d) moved the build tree %s here (tree/) at %s, as it found it. '
            'Nothing runs in it: each control and probe gets a fresh copy at %s, deleted after the run.\n'
            'If this is still here, the run was killed or stopped on purpose (its output says why: a copy that '
            'writers may still be in, or a build path that no longer held its copy). Recovery, once no rules run '
            'and no process it started is alive: look at what is at %s; if it is a rules copy, delete it WITHOUT '
            'following links (it may hold junctions); move tree/ back to %s, then delete this directory.\n'
            % (os.getpid(), build, time.strftime('%Y-%m-%d %H:%M:%S'), build, build, build), encoding='utf-8')
    except OSError as e:
        print('RULES %s: not run: %s cannot be made: %s (delete it if it is there)' % (build.name, aside, e),
              flush=True)
        return 1
    pristine = aside / 'tree'
    try:
        os.rename(build, pristine)
    except OSError as e:
        readme.unlink()
        aside.rmdir()
        print('RULES %s: not run: the tree cannot be moved aside (open files?): %s; it stays at %s' % (
            build.name, e, build), flush=True)
        return 1
    kept['pristine'] = pristine
    kept['held'] = []  # why neither the build path nor the pristine tree may be touched again
    back = []  # what went wrong putting the pristine tree back
    try:
        results = [one(c, controls, spoiled) for g in groups for c in g]
    finally:
        if kept['held']:
            # A copy that writers may still be in, or a build path that no longer holds this run's copy: nothing is
            # deleted or moved; both paths are printed for recovery by hand.
            back.extend(kept['held'])
        elif os.path.lexists(build):
            # A copy that could not be deleted, or something else that was there (named above).
            back.append('the build path still holds something rules did not delete (%s)' % build)
        else:
            try:
                os.rename(pristine, build)
            except OSError as e:
                back.append('it could not be moved back: %s' % e)
        if back:
            print('PRISTINE TREE LEFT AT %s: %s. The tree rules found is there, untouched; see README.txt there.' % (
                pristine, '; '.join(back)), flush=True)
            print('RECOVER BY HAND: build path %s, pristine tree %s. Once no process of the run is alive, look at '
                  'what is at the build path; if it is a rules copy, delete it without following links; then move '
                  'the pristine tree back to the build path.' % (build, pristine), flush=True)
        else:
            try:
                remove_tree(readme)  # made read-only by a run, it still goes
                aside.rmdir()  # anything else a run put in it stays, and is named
            except (OSError, NotRemoved) as e:
                back.append('%s not removed: %s' % (aside, e))
                print('LEFT: %s (the tree is back at %s; this directory could not be removed: look inside, then '
                      'delete it by hand): %s' % (aside, build, e), flush=True)
    try:
        tree_left = manifest_differences(kept['base'], tree_manifest(build)) if not os.path.lexists(pristine) else [
            'the tree is still aside at %s' % pristine]
    except (OSError, ValueError) as e:
        tree_left = ['the tree cannot be read: %s: %s' % (type(e).__name__, str(e)[:200])]
    active, errored = {}, {}
    for name, source, verdict, problem in results:
        # Only a check that ran cleanly, in a clean copy, counts: any problem (a copy not clean, writers
        # that may be alive, a check not run) makes it errored, RED or not, and its launcher not probed.
        clean = not problem and (verdict.startswith('RED') or verdict.startswith('same'))
        active.setdefault(name, []).append(clean and verdict.startswith('RED'))
        if not clean:
            errored.setdefault(name, []).append(source)
    print('RULES %s: %d launchers in the chain, %d checks in %d groups that share no file' % (
        build.name, len(chain.levels), len(checks), len(groups)))
    states = {}
    for script in chain.levels:
        name = Path(script).name
        flags = active.get(name)
        state = ('no named source in this build' if flags is None else
                 'not probed (%d of %d checks errored: %s)' % (len(errored[name]), len(flags),
                                                               ', '.join(errored[name])) if name in errored else
                 'ACTIVE (%d of %d named sources change without it)' % (sum(flags), len(flags)) if any(flags) else
                 'inactive in this build (%d named sources unchanged without it)' % len(flags))
        states[name] = state
        print('LAUNCHER %-34s %s' % (name, state))
    failed = [r for r in results if r[3]]
    missing = []
    for want in expect_active:
        name = next((n for n in states if n == want or Path(n).stem == want), want)
        if not states.get(name, '').startswith('ACTIVE'):
            missing.append(name)
            print('EXPECTED ACTIVE %s: %s' % (name, states.get(name, 'not in the chain')))
    print('TOUCHED OUTSIDE: %d runs left paths besides their own object\'s in their copy (deleted with it unless named NOT CLEAN)' %
          len(touched_outside))
    base = kept['base']
    if tree_left:
        print('TREE CHANGED %s: %d paths differ from the tree rules found (%d directories and files)' % (
            build.name, len(tree_left), len(base)))
        for line in tree_left:
            print('  %s' % line)
    else:
        print('TREE %s: all %d directories and files equal to the tree rules found (bytes, sizes, mtimes, '
              'attributes)' % (build.name, len(base)))
    return 1 if failed or missing or tree_left or back else 0


# ------------------------------------------------------------------------------------------- identity driver

def image_count(image):
    if os.name != 'nt':
        return -1
    try:
        r = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq ' + image, '/NH', '/FO', 'CSV'], capture_output=True,
                           text=True, timeout=60, stdin=subprocess.DEVNULL)
        return sum(1 for l in r.stdout.splitlines() if l.lower().startswith('"%s"' % image))
    except (OSError, subprocess.SubprocessError):
        return -1


def python_count():
    return image_count('python.exe')


_SLOT_HELD = False  # set once this process' first build has its slot


def wait_for_slot(limit):
    """Wait until fewer than `limit` cmake.exe run on this machine (other builds), checking each minute.

    Only this process' first build waits. Its later builds start the moment the one before exits, since each one
    replaces this process' own finished build and the machine's build count stays the same (no race for the slot)."""
    global _SLOT_HELD
    if not limit or _SLOT_HELD:
        return
    _SLOT_HELD = True
    while True:
        n = image_count('cmake.exe')
        if n < limit:
            return
        print('waiting for a build slot: %d cmake.exe running, limit %d (%s)' % (n, limit, time.strftime('%H:%M')),
              flush=True)
        time.sleep(60)


def build_dir_for(words):
    game = next((w for w in words if w in ('undertale', 'deltarune')), 'undertale')
    target = next((w for w in words if w in ('n3ds', 'o3ds')), 'n3ds')
    return ROOT / 'build' / ('3ds-%s-%s' % (game, target))


def run_build(label, side, words, env, out, bash, slots=0):
    wait_for_slot(slots)
    build = build_dir_for(words)
    if build.exists():
        shutil.rmtree(build)
    cache = out / ('ccache-%s-%s' % (label, side))
    if cache.exists():
        shutil.rmtree(cache)
    cache.mkdir(parents=True)
    e = dict(os.environ, **env)
    e['CTRPORT_LAUNCH_DISPATCH'] = 'OFF' if side == 'A' else 'ON'
    e['CTRPORT_CCACHE_DIR'] = str(cache)
    log = out / ('%s-%s.log' % (label, side))
    counts, stop = [], threading.Event()

    def sample():
        while not stop.is_set():
            counts.append(python_count())
            stop.wait(30)

    sampler = threading.Thread(target=sample, daemon=True)
    sampler.start()
    began = time.time()
    first = last = None
    with open(log, 'w', encoding='utf-8') as f:
        # A login shell, as every build tool here runs build.sh: its profile puts devkitPro's msys2 tools first.
        script = 'cd %s && exec platform/3ds/build.sh %s' % (shlex.quote(ROOT.as_posix()),
                                                              ' '.join(shlex.quote(w) for w in words))
        p = subprocess.Popen([bash, '--login', '-c', script], cwd=ROOT, env=e,
                             stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for raw in p.stdout:
            line = raw.decode('utf-8', 'replace').rstrip('\r\n')
            t = time.time() - began
            if 'Building C object' in line:
                first = t if first is None else first
                last = t
            f.write('%8.1f %s\n' % (t, line))
        status = p.wait()
    stop.set()
    sampler.join()
    wall = time.time() - began
    warnings = sum(1 for l in log.read_text(encoding='utf-8', errors='replace').splitlines()
                   if 'warning:' in l.lower())
    files = snapshot(build, out / ('%s-%s.json' % (label, side)))
    for name in ('undertale-3ds.elf', 'deltarune-3ds.elf'):
        if (build / name).exists():
            shutil.copy2(build / name, out / ('%s-%s.elf' % (label, side)))
    keep = out / ('%s-%s-tree' % (label, side))
    for name in kept_names(files) + list(TIMING_LOGS):
        if (build / name).exists():
            (keep / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(build / name, keep / name)
    counts = sorted(c for c in counts if c >= 0)
    summary = dict(label=label, side=side, status=status, wall_s=round(wall, 1), warnings=warnings,
                   compile_phase_s=round((last or 0) - (first or 0), 1), objects=sum(n.endswith('.o') for n in files),
                   python_exe=dict(min=counts[0], median=counts[len(counts) // 2], max=counts[-1]) if counts else None)
    (out / ('%s-%s.summary.json' % (label, side))).write_text(json.dumps(summary, indent=1), encoding='utf-8')
    print('BUILD', json.dumps(summary), flush=True)
    return summary


def identity(args):
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(kv.split('=', 1) for kv in args.env)
    words = args.words
    summaries = []
    for side in ('A', 'B'):
        summaries.append(run_build(args.label, side, words, env, out, args.bash, args.slots))
        if summaries[-1]['status']:
            print('IDENTITY STOP: build %s failed with status %d' % (side, summaries[-1]['status']), flush=True)
            return 1
    bad, report = compare(out / ('%s-A.json' % args.label), out / ('%s-B.json' % args.label),
                          out / ('%s-A-tree' % args.label), out / ('%s-B-tree' % args.label),
                          out / ('%s-buildsystem.diff' % args.label))
    ebad, elines = elfdiff(out / ('%s-A.elf' % args.label), out / ('%s-B.elf' % args.label))
    text = '\n'.join(report + elines + ['TIME A %.0f s (compile phase %.0f s), B %.0f s (compile phase %.0f s); '
                                        'python.exe A %s, B %s' % (
                                            summaries[0]['wall_s'], summaries[0]['compile_phase_s'],
                                            summaries[1]['wall_s'], summaries[1]['compile_phase_s'],
                                            summaries[0]['python_exe'], summaries[1]['python_exe'])])
    (out / ('%s-identity.txt' % args.label)).write_text(text + '\n', encoding='utf-8')
    print(text)
    return 1 if bad or ebad else 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('identity')
    p.add_argument('--label', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--bash', default=DEVKITPRO_BASH if Path(DEVKITPRO_BASH).exists() else 'bash')
    p.add_argument('--env', action='append', default=[])
    p.add_argument('--slots', type=int, default=0, help='start each build only while fewer cmake.exe run')
    p.add_argument('words', nargs='+')
    p = sub.add_parser('snapshot')
    p.add_argument('build')
    p.add_argument('out')
    p = sub.add_parser('compare')
    p.add_argument('a')
    p.add_argument('b')
    p.add_argument('--a-dir')
    p.add_argument('--b-dir')
    p.add_argument('--diff-out', help='write the build-system diffs here instead of into the report')
    p = sub.add_parser('elfdiff')
    p.add_argument('a')
    p.add_argument('b')
    p = sub.add_parser('rules')
    p.add_argument('build')
    p.add_argument('--jobs', type=int, default=1, help='ignored: kept for old command lines; runs are serial')
    p.add_argument('--only', nargs='*')
    p.add_argument('--expect-active', nargs='*', default=[], help='launchers that must come out ACTIVE')
    a = ap.parse_args(argv)
    if a.cmd == 'identity':
        return identity(a)
    if a.cmd == 'snapshot':
        files = snapshot(a.build, a.out)
        print('%d files' % len(files))
        return 0
    if a.cmd == 'compare':
        bad, report = compare(a.a, a.b, a.a_dir, a.b_dir, a.diff_out)
        print('\n'.join(report))
        return 1 if bad else 0
    if a.cmd == 'elfdiff':
        bad, lines = elfdiff(a.a, a.b)
        print('\n'.join(lines))
        return 1 if bad else 0
    if a.cmd == 'rules':
        return rules(a.build, a.jobs, a.only, a.expect_active)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
