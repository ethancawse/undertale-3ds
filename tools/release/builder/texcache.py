"""Texture preparation on the PC: the cache folders the console's "Preparing textures" screen would write.

The console converts every texture page of a data.win into pageNNN.dtx files and a "complete" marker in a "cache"
folder beside it (platform/3ds/source/ctr_texture_pack.c) the first time a game starts: about 30 s for UNDERTALE on
an Old 3DS and several minutes for DELTARUNE. tools/texprep builds that same console code for the PC, so the builder
can write those folders onto the SD card itself, byte for byte what the console would write. The console's checks
stay as they are: it reads a page only when its header matches the data.win, and it prepares any page that is
missing or stale, so an absent or partial folder only means the console does that work as before.

The tool is used only when it was built from the same sources as the tree the builder builds from (texprep
--sources, tools/texprep/sources.py). Otherwise, or when anything here fails, nothing is written to the card's cache
folders and the console prepares the textures. Ways to get the tool, in order:

1. the binary shipped in the builder package (bin/texprep-<platform>), pinned by size and SHA-256 below;
2. on Linux and macOS, when a C compiler is present, built from this tree (tools/texprep/build.sh) in the work folder.

Writing a folder on the card: an existing folder that already matches is kept untouched.
Otherwise the "complete" marker is deleted first, each page is written as pageNNN.dtx.part and renamed into place,
page files beyond the page count are removed, and "complete" is written last. A pulled card or a failed write leaves
no marker, so the console checks each page. Other files in the folder (rooms.dtm, bytecode.bin, data-version.txt)
are left as they are.

Rounded tiles only (the cache folder). The exact-colours folder (cache_exact) is the console's.
"""
from __future__ import annotations

import dataclasses
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from processes import hidden_options

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
_TP = os.path.join(SOURCE_ROOT, "tools", "texprep")
if os.path.isdir(_TP) and _TP not in sys.path:
    sys.path.insert(0, _TP)
try:
    import sources as texprep_sources  # tools/texprep/sources.py
except ImportError:  # an archive without tools/texprep: never prepare on the PC
    texprep_sources = None

CACHE_DIR = "cache"
MARKER = "complete"
PAGE_RE = re.compile(r"^page([0-9]{3,})\.dtx$", re.I)
PART = ".part"
# Bytes 52-55 of a page file are the header's tail padding. Console builds before 2026-10-21 left them
# uninitialised (stack bytes), and nothing reads them, so a folder that differs only there still matches.
PAD_START, PAD_END = 52, 56

# Shipped binaries: the builder package carries them at bin/<name> beside this file.
# (sys.platform, machine) -> (file name, size in bytes, SHA-256). Each prints the digest of the sources it was built
# from; the builder compares that with its own tree. Built with tools/texprep/build.sh: see
# docs/release/builder-status.md, "Texture preparation on the PC".
# Rebuild and re-pin whenever any file in `texprep --sources` changes (a release step, builder-status.md).
PINNED = {
    # MSYS2 UCRT64 gcc 16.1.0, static, UTF-8 paths, sources texprep-src-1:0c7a8a5f9e7bb7dd... (2026-10-23)
    ("win32", "x86_64"): ("texprep-windows-x64.exe", 351742,
                          "e1eb3e393424a90210072833e55c26592f6fe2d4de1aabd323ff4ba29410c473"),
    # Linux builds locally with the system compiler. The former static glibc 2.39 binary is
    # retired: it lacked a corresponding-source/relink kit and did not fit older Debian hosts.
    # macOS (arm64 and x86_64): texprep-macos-universal, built on an M1 Mac (steps in builder-status.md).
    # Until it is pinned, a Mac builds texprep when a compiler is present, else the console prepares.
}
BIN_DIR = os.path.join(HERE, "bin")
# Building texprep from source when no shipped binary fits (Linux and macOS). Tests turn it off.
ALLOW_SOURCE_BUILD = True


def page_name(i):
    return "page%03u.dtx" % i


def machine(value=None):
    import platform
    value = (value or platform.machine()).lower()
    return {"amd64": "x86_64", "x64": "x86_64", "aarch64": "arm64"}.get(value, value)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------------------------------------------------------------
# data.win: the page count (what the console's marker records), read-only

def page_count(data_win):
    """TXTR's entry count, the console's pageCount (dw->txtr.count). Raises ValueError for a file that isn't one."""
    with open(data_win, "rb") as f:
        head = f.read(8)
        if len(head) < 8 or head[:4] != b"FORM":
            raise ValueError("not a GameMaker data file")
        size = os.fstat(f.fileno()).st_size
        end = min(size, 8 + struct.unpack("<I", head[4:])[0])
        at = 8
        while at + 8 <= end:
            f.seek(at)
            chunk = f.read(8)
            if len(chunk) < 8:
                break
            n = struct.unpack("<I", chunk[4:])[0]
            if chunk[:4] == b"TXTR":
                c = f.read(4)
                if len(c) < 4:
                    break
                return struct.unpack("<I", c)[0]
            at += 8 + n
    raise ValueError("no TXTR chunk")


# ---------------------------------------------------------------------------------------------------------------------
# The tool

@dataclasses.dataclass
class Tool:
    path: str | None
    how: str            # "pinned", "built", or "" when there is none
    message: str

    @property
    def ok(self):
        return self.path is not None


def _compiler(which=shutil.which):
    for name in ("cc", "gcc", "clang"):
        p = which(name)
        if p:
            return p
    return None


def pinned_tool(platform=sys.platform, arch=None, bin_dir=BIN_DIR, pins=None):
    """(path or None, message) for the shipped binary of this platform."""
    pins = PINNED if pins is None else pins
    pin = pins.get((platform, machine(arch)))
    if pin is None:
        return None, "no shipped texprep for %s %s" % (platform, machine(arch))
    name, size, sha = pin
    path = os.path.join(bin_dir, name)
    if not os.path.isfile(path):
        return None, "shipped texprep missing (%s)" % name
    if size is None or sha is None:
        return None, "texprep %s has no pinned hash in this builder" % name
    if os.path.getsize(path) != size or sha256_file(path) != sha:
        return None, "shipped texprep %s does not match its pinned SHA-256" % name
    return path, "shipped texprep %s (SHA-256 %s...)" % (name, sha[:12])


def build_tool(work, source_root=SOURCE_ROOT, platform=sys.platform, run=None, which=shutil.which, log=print):
    """Builds texprep from the tree into the work folder (Linux and macOS, when a compiler is there)."""
    if platform == "win32":
        return None, "no build from source on Windows"
    cc = _compiler(which)
    bash = which("bash")
    script = os.path.join(source_root, "tools", "texprep", "build.sh")
    if not cc or not bash or not os.path.isfile(script):
        return None, "no C compiler to build texprep" if not cc else "cannot build texprep here"
    out = os.path.join(work, "texcache", "tool", "texprep")
    env = dict(os.environ, CC=cc, PYTHON=sys.executable)
    if platform == "darwin":
        env.setdefault("TEXPREP_CFLAGS", "-mmacosx-version-min=11.0")
    run = run or (lambda argv, env: subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                                                   encoding='utf-8', errors='replace',
                                                   env=env, timeout=900, **hidden_options()))
    log("building texprep from source with %s" % cc)
    r = run([bash, script, out], env)
    if r.returncode != 0 or not os.path.isfile(out):
        tail = ((r.stdout or "") + (r.stderr or "")).strip().splitlines()[-3:]
        return None, "texprep build failed (exit %s): %s" % (r.returncode, " / ".join(tail))
    return out, "texprep built here with %s" % os.path.basename(cc)


def find_tool(work, source_root=SOURCE_ROOT, platform=sys.platform, arch=None, bin_dir=BIN_DIR, pins=None,
              check=None, build=None, log=print):
    """The texprep to use, or Tool(None, "", why) when the console should prepare the textures."""
    if texprep_sources is None and check is None:
        return Tool(None, "", "this archive has no tools/texprep")
    check = check or (lambda path: texprep_sources.check(source_root, path))
    reasons = []
    path, msg = pinned_tool(platform, arch, bin_dir, pins)
    if path:
        ok, why = check(path)
        if ok:
            return Tool(path, "pinned", msg + "; " + why)
        reasons.append("shipped texprep: " + why)
    else:
        reasons.append(msg)
    if platform != "win32" and ALLOW_SOURCE_BUILD:
        built = os.path.join(work, "texcache", "tool", "texprep")
        if os.path.isfile(built) and check(built)[0]:
            return Tool(built, "built", "texprep built here earlier; " + check(built)[1])
        build = build or (lambda: build_tool(work, source_root, platform, log=log))
        path, msg = build()
        if path:
            ok, why = check(path)
            if ok:
                return Tool(path, "built", msg + "; " + why)
            reasons.append("built texprep: " + why)
        else:
            reasons.append(msg)
    return Tool(None, "", "; ".join(reasons))


# ---------------------------------------------------------------------------------------------------------------------
# Preparing on the PC

@dataclasses.dataclass
class CacheSet:
    data_win: str        # the player's data.win (read-only)
    sd_dir: str          # the cache folder on the card, relative to the SD root, forward slashes
    local_dir: str       # what texprep wrote, in the work folder
    pages: int
    bytes: int = 0


def data_wins(product, folder, parts=None):
    """(data.win path, its folder relative to the SD data folder: "" or "chapter1_windows") for each one delivered."""
    import products
    out = []
    for src, rel, _size in products.sd_files(product, folder, parts):
        if os.path.basename(rel).lower() == "data.win":
            out.append((src, os.path.dirname(rel).replace("\\", "/")))
    return sorted(out, key=lambda x: x[1])


def sd_cache_dir(product, rel_dir):
    return "/".join(p for p in (product.sd_data_dir, rel_dir, CACHE_DIR) if p)


def prepare(tool, product, folder, work, parts=None, run=None, log=print):
    """Runs texprep for every delivered data.win into <work>/texcache/<game>/... Returns (sets, problems): only
    complete folders are in sets; a data.win whose folder didn't complete is left to the console."""
    run = run or (lambda argv: subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                                              encoding='utf-8', errors='replace',
                                              timeout=1800, **hidden_options()))
    sets, problems = [], []
    for data_win, rel in data_wins(product, folder, parts):
        local = os.path.join(work, "texcache", product.key, *(rel.split("/") if rel else []), CACHE_DIR)
        if os.path.isdir(local):
            shutil.rmtree(local)        # always from scratch, so the output depends only on data.win and the tool
        os.makedirs(local)
        name = rel or "data.win"
        try:
            pages = page_count(data_win)
            r = run([tool.path, data_win, local])
        except (OSError, ValueError, subprocess.SubprocessError) as e:
            problems.append("%s: %s" % (name, e))
            continue
        out = (r.stdout or "").strip()
        if out:
            log(out.replace(data_win, name))
        if r.returncode != 0 or not os.path.isfile(os.path.join(local, MARKER)):
            problems.append("%s: texprep exit %d" % (name, r.returncode))
            continue
        missing = [i for i in range(pages) if not os.path.isfile(os.path.join(local, page_name(i)))]
        # The console writes no file for an entry without an image; the marker counts every entry.
        size = sum(os.path.getsize(os.path.join(local, page_name(i))) for i in range(pages) if i not in missing)
        size += os.path.getsize(os.path.join(local, MARKER))
        sets.append(CacheSet(data_win, sd_cache_dir(product, rel), local, pages, size))
    return sets, problems


# ---------------------------------------------------------------------------------------------------------------------
# The card

def _same(a, b, mask_pad=False):
    """Whether files a and b hold the same bytes (mask_pad: except a page header's padding)."""
    try:
        if os.path.getsize(a) != os.path.getsize(b):
            return False
        with open(a, "rb") as fa, open(b, "rb") as fb:
            ha, hb = fa.read(PAD_END), fb.read(PAD_END)
            if mask_pad and len(ha) == PAD_END:
                ha, hb = ha[:PAD_START], hb[:PAD_START]
            if ha != hb:
                return False
            while True:
                x, y = fa.read(1 << 20), fb.read(1 << 20)
                if x != y:
                    return False
                if not x:
                    return True
    except OSError:
        return False


def _local_pages(cs):
    return [i for i in range(cs.pages) if os.path.isfile(os.path.join(cs.local_dir, page_name(i)))]


def _extra_files(sd_dir, pages):
    """Page files at or beyond the page count, and .part files left by an interrupted write."""
    try:
        names = os.listdir(sd_dir)
    except OSError:
        return []
    out = []
    for n in sorted(names):
        m = PAGE_RE.match(n)
        if (m and int(m.group(1)) >= pages) or (n.lower().endswith(PART) and n.lower().startswith(("page", MARKER))):
            out.append(n)
    return out


def matches(sd_root, cs):
    """An existing folder on the card that already holds this cache (the marker exactly, each page except the
    header padding), with no page files beyond it."""
    d = os.path.join(sd_root, *cs.sd_dir.split("/"))
    if not _same(os.path.join(d, MARKER), os.path.join(cs.local_dir, MARKER)):
        return False
    have = set(_local_pages(cs))
    for i in range(cs.pages):
        target = os.path.join(d, page_name(i))
        if i in have:
            if not _same(target, os.path.join(cs.local_dir, page_name(i)), mask_pad=True):
                return False
        elif os.path.exists(target):
            return False
    return not _extra_files(d, cs.pages)


@dataclasses.dataclass
class Plan:
    cs: CacheSet
    keep: bool
    write_pages: list       # page numbers to write
    same_pages: int         # pages already there byte for byte
    remove: list            # file names to remove (beyond the page count, leftover .part)
    bytes: int


def plan(sd_root, sets):
    plans = []
    for cs in sets:
        d = os.path.join(sd_root, *cs.sd_dir.split("/"))
        if matches(sd_root, cs):
            plans.append(Plan(cs, True, [], cs.pages, [], 0))
            continue
        write, same, nbytes = [], 0, 0
        for i in _local_pages(cs):
            src, dst = os.path.join(cs.local_dir, page_name(i)), os.path.join(d, page_name(i))
            if _same(dst, src):
                same += 1
            else:
                write.append(i)
                nbytes += os.path.getsize(src)
        # A page the PC didn't write (an entry without an image) must not be left from another data.win.
        remove = [page_name(i) for i in range(cs.pages) if i not in set(_local_pages(cs))
                  and os.path.exists(os.path.join(d, page_name(i)))]
        remove += _extra_files(d, cs.pages)
        nbytes += os.path.getsize(os.path.join(cs.local_dir, MARKER))
        plans.append(Plan(cs, False, write, same, remove, nbytes))
    return plans


def describe(plans):
    lines = []
    for p in plans:
        if p.keep:
            lines.append("  %s: already prepared (%d pages), kept" % (p.cs.sd_dir, p.cs.pages))
        else:
            lines.append("  %s: delete %s; write %d of %d pages as .part then rename (%d already there); remove %d "
                         "files beyond the page count; write %s last (%.1f MB)"
                         % (p.cs.sd_dir, MARKER, len(p.write_pages), p.cs.pages, p.same_pages, len(p.remove), MARKER,
                            p.bytes / 1e6))
    return lines


def _write_file(src, dst, fs):
    tmp = dst + PART
    try:
        fs.copy(src, tmp)
        fs.replace(tmp, dst)
    except OSError:
        try:
            if os.path.exists(tmp):
                fs.remove(tmp)
        except OSError:
            pass
        raise


class FileOps:
    """The card writes, in one place so tests can watch their order and fail one."""

    def copy(self, src, dst):
        with open(src, "rb") as fi, open(dst, "wb") as fo:
            shutil.copyfileobj(fi, fo, 1 << 20)
            fo.flush()
            os.fsync(fo.fileno())

    def replace(self, src, dst):
        os.replace(src, dst)

    def remove(self, path):
        os.remove(path)

    def makedirs(self, path):
        os.makedirs(path, exist_ok=True)


def write_plans(sd_root, plans, fs=None, progress=None, cancelled=lambda: False):
    """Writes each folder in the approved order. Returns (folders written, folders kept, bytes). Raises OSError
    (or the caller's cancel) part way; the card then has no marker for that folder and the console checks it."""
    fs = fs or FileOps()
    total = sum(p.bytes for p in plans)
    done = written = kept = 0
    for p in plans:
        if p.keep:
            kept += 1
            continue
        d = os.path.join(sd_root, *p.cs.sd_dir.split("/"))
        fs.makedirs(d)
        marker = os.path.join(d, MARKER)
        if os.path.lexists(marker):
            fs.remove(marker)
        for i in p.write_pages:
            if cancelled():
                raise InterruptedError("cancelled")
            src = os.path.join(p.cs.local_dir, page_name(i))
            _write_file(src, os.path.join(d, page_name(i)), fs)
            done += os.path.getsize(src)
            if progress:
                progress(done, total, p.cs.sd_dir + "/" + page_name(i))
        for n in p.remove:
            if os.path.lexists(os.path.join(d, n)):    # a leftover .part may have just been reused and renamed
                fs.remove(os.path.join(d, n))
        _write_file(os.path.join(p.cs.local_dir, MARKER), marker, fs)
        done += os.path.getsize(os.path.join(p.cs.local_dir, MARKER))
        written += 1
    return written, kept, done


@dataclasses.dataclass
class Result:
    used: bool
    message: str
    written: int = 0
    kept: int = 0
    bytes: int = 0


def deliver_textures(sd_root, product, folder, work, parts=None, tool=None, fs=None, progress=None,
                     cancelled=lambda: False, log=print, run=None):
    """Prepare on the PC and write to the card. Failures to prepare are not errors (the console prepares); a failed
    card write raises OSError like any other copy."""
    tool = tool or find_tool(work, log=log)
    if not tool.ok:
        return Result(False, "Textures: the 3DS will prepare them the first time (%s)." % tool.message)
    sets, problems = prepare(tool, product, folder, work, parts, run=run, log=log)
    for pr in problems:
        log("texprep: " + pr)
    if not sets:
        return Result(False, "Textures: the 3DS will prepare them the first time (%s)."
                      % ("; ".join(problems) or "nothing to prepare"))
    plans = plan(sd_root, sets)
    need = sum(p.bytes for p in plans)
    free = shutil.disk_usage(sd_root).free
    if need > free:
        return Result(False, "Textures: not enough space on the card (%d MB needed); the 3DS will prepare them."
                      % (-(-need // 10**6)))
    for line in describe(plans):
        log(line.strip())
    written, kept, nbytes = write_plans(sd_root, plans, fs, progress, cancelled)
    msg = "Textures prepared: %d folder(s) written, %d already up to date." % (written, kept)
    if problems:
        msg += " The 3DS prepares the rest."
    return Result(True, msg, written, kept, nbytes)


def dry_run_lines(product, folder, work, parts=None, tool=None):
    """The dry run's texture lines: the tool found, and what would be written per data.win. Writes nothing."""
    tool = tool or find_tool(work, build=lambda: (None, "would build texprep from source here"), log=lambda s: None)
    lines = ["Texture preparation on the PC: %s" % (tool.message if tool.ok else "off (%s); the 3DS prepares them"
                                                   % tool.message)]
    if folder is None or not os.path.isdir(folder):
        return lines
    for data_win, rel in data_wins(product, folder, parts):
        try:
            n = "%d pages" % page_count(data_win)
        except (OSError, ValueError) as e:
            n = "unreadable (%s)" % e
        lines.append("  %s (%s) -> sd:/%s/: keep if it matches; else delete %s, write pages as .part then rename, "
                     "remove pages beyond the count, write %s last; keep rooms.dtm, bytecode.bin, data-version.txt"
                     % (rel or "data.win", n, sd_cache_dir(product, rel), MARKER, MARKER))
    return lines


def data_version_dry_run_lines(product, folder, parts=None, source_root=SOURCE_ROOT):
    """The dry run's data-version.txt lines (no hashing, nothing written)."""
    revision = data_versions_revision(source_root)
    lines = ["Version check on the PC: %s" % ("data_versions.inc revision %d" % revision if revision is not None
                                             else "off (no data_versions.inc revision); the 3DS hashes each data.win")]
    if revision is None or folder is None or not os.path.isdir(folder):
        return lines
    for data_win, rel in data_wins(product, folder, parts):
        lines.append("  %s -> sd:/%s/%s: keep if revision, size, SHA-256 and path match; else write \"%d <size> %d "
                     "<SHA-256>\" + \"%s\" as .part then rename, after the texture folder"
                     % (rel or "data.win", sd_cache_dir(product, rel), DATA_VERSION, revision, DATA_VERSION_MTIME,
                        console_data_win_path(product, rel)))
    return lines


# ---------------------------------------------------------------------------------------------------------------------
# data-version.txt: the console's data.win version cache, written on the PC
#
# On a game's first start the console hashes the whole data.win with SHA-256 to check it against data_versions.inc
# (platform/3ds/source/ctr_data_version.c, CtrDataVersion_check): 24-64 s per data.win on an Old 3DS. It caches the
# result in <data.win folder>/cache/data-version.txt as fprintf("%u %llu %lld %s\n%s\n", revision, st_size,
# st_mtime, digest, data.win path) and skips the hash while all five fields still match. The builder writes that
# same file, byte for byte, after the texture folder. mtime is written as 0: libctru's stat gives st_mtime 0 for SD
# files in Azahar; on hardware this is unconfirmed. If a console reports another mtime, it sees a mismatch, hashes
# as before and rewrites the file itself, so the worst case is losing the gain. The console's check is unchanged.

DATA_VERSION = "data-version.txt"
DATA_VERSION_MTIME = 0
FAT_SLACK = 2.0     # as deliver.FAT_SLACK: FAT keeps modification times to 2 s
_REVISION_RE = re.compile(r"^\s*#define\s+DATA_VERSIONS_REVISION\s+([0-9]+)\s*$", re.M)
_DATA_VERSION_RE = re.compile(r"^([0-9]+) ([0-9]+) (-?[0-9]+) ([0-9a-f]{64})\n([^\r\n]*)\n$")


def data_versions_revision(source_root=SOURCE_ROOT):
    """DATA_VERSIONS_REVISION from the tree the console is built from, or None when it can't be read."""
    try:
        with open(os.path.join(source_root, "platform", "3ds", "data_versions.inc"), encoding="utf-8") as f:
            m = _REVISION_RE.search(f.read())
    except OSError:
        return None
    return int(m.group(1)) if m else None


def console_data_win_path(product, rel_dir):
    """The data.win path the console stats and records: sdmc:/<sd_data_dir>[/<chapter folder>]/data.win (main.c's
    GAME_DIR "/data.win" for the root, ctr_chapters.c's joinNormalized(bundleDir, "data.win") for a chapter)."""
    return "sdmc:/" + "/".join(p for p in (product.sd_data_dir, rel_dir, "data.win") if p)


def data_version_text(revision, size, digest, path, mtime=DATA_VERSION_MTIME):
    """The bytes writeCache writes (newlib's text mode adds no carriage returns)."""
    if "\r" in path or "\n" in path:
        raise ValueError("data.win path has a line break")
    return ("%u %u %d %s\n%s\n" % (revision, size, mtime, digest, path)).encode("ascii")


def parse_data_version(data):
    """(revision, size, mtime, digest, path) of a data-version.txt the console would accept, or None."""
    try:
        m = _DATA_VERSION_RE.match(data.decode("ascii"))
    except UnicodeDecodeError:
        return None
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4), m.group(5)) if m else None


@dataclasses.dataclass
class VersionPlan:
    sd_file: str        # relative to the SD root, forward slashes
    data: bytes
    keep: bool
    why: str
    skip: bool = False  # the card's data.win isn't the copy of this one: write nothing, the console hashes


def _digest(path):
    try:
        import verify_game
        return verify_game.data_digest(path)    # the digest the version check computed, when it ran
    except ImportError:
        return sha256_file(path)


def plan_data_versions(sd_root, product, folder, parts=None, source_root=SOURCE_ROOT, digest=None):
    """One VersionPlan per delivered data.win. Keeps a file on the card that already holds this data.win's revision,
    size, digest and path (whatever its mtime: a console that records a real mtime keeps its own file). The digest is
    the PC file's, so it is written only when the card's data.win is the builder's copy of it: the same size and
    modification time (within FAT's 2 s), the test the copy step uses to skip a file. Otherwise skip=True."""
    revision = data_versions_revision(source_root)
    if revision is None:
        return []
    digest = digest or _digest
    out = []
    for data_win, rel in data_wins(product, folder, parts):
        sd_file = sd_cache_dir(product, rel) + "/" + DATA_VERSION
        path = console_data_win_path(product, rel)
        size = os.path.getsize(data_win)
        card_dw = os.path.join(sd_root, *sd_cache_dir(product, rel).split("/")[:-1], os.path.basename(data_win))
        try:
            st, ss = os.stat(card_dw), os.stat(data_win)
            copied = st.st_size == ss.st_size and abs(st.st_mtime - ss.st_mtime) <= FAT_SLACK
        except OSError:
            copied = False
        if not copied:
            out.append(VersionPlan(sd_file, b"", False, "the card's data.win is not this one", skip=True))
            continue
        dig = digest(data_win)
        data = data_version_text(revision, size, dig, path)
        try:
            with open(os.path.join(sd_root, *sd_file.split("/")), "rb") as f:
                have = parse_data_version(f.read(4096))
        except OSError:
            have = None
        if have is not None and have[:2] == (revision, size) and have[3:] == (dig, path):
            out.append(VersionPlan(sd_file, data, True, "already there"))
        else:
            out.append(VersionPlan(sd_file, data, False, "missing" if have is None else "stale"))
    return out


def write_data_versions(sd_root, plans, fs=None, work=None):
    """Writes each file as .part then renames it. Returns (written, kept). Raises OSError part way."""
    fs = fs or FileOps()
    written = kept = 0
    for p in plans:
        if p.skip:
            continue
        if p.keep:
            kept += 1
            continue
        dst = os.path.join(sd_root, *p.sd_file.split("/"))
        fs.makedirs(os.path.dirname(dst))
        src = os.path.join(work or tempfile.gettempdir(), "ctrport-" + DATA_VERSION + ".src")
        with open(src, "wb") as f:
            f.write(p.data)
        try:
            _write_file(src, dst, fs)
        finally:
            if os.path.exists(src):
                os.remove(src)
        written += 1
    return written, kept


def deliver_data_versions(sd_root, product, folder, work, parts=None, fs=None, log=print, source_root=SOURCE_ROOT,
                          digest=None):
    """data-version.txt for every delivered data.win. Not being able to plan is not an error (the console hashes);
    a failed card write raises OSError like any other copy."""
    try:
        plans = plan_data_versions(sd_root, product, folder, parts, source_root, digest)
    except (OSError, ValueError) as e:
        return Result(False, "Version check: the 3DS will do it the first time (%s)." % e)
    if not plans:
        return Result(False, "Version check: the 3DS will do it the first time.")
    for p in plans:
        log("%s: %s" % (p.sd_file, "%s; the 3DS checks it" % p.why if p.skip else "already there, kept" if p.keep
                        else "%s, write as .part then rename" % p.why))
    if all(p.skip for p in plans):
        return Result(False, "Version check: the 3DS will do it the first time (the card's data.win is not the "
                      "copied one).")
    os.makedirs(os.path.join(work, "texcache"), exist_ok=True)
    written, kept = write_data_versions(sd_root, plans, fs, os.path.join(work, "texcache"))
    msg = "Version check prepared: %d file(s) written, %d already up to date." % (written, kept)
    if any(p.skip for p in plans):
        msg += " The 3DS checks the rest."
    return Result(True, msg, written, kept, sum(len(p.data) for p in plans if not p.keep))
