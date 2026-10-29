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

Half-resolution art (products.HALFART, UNDERTALE's Old 3DS build): that build keeps its pages in a folder of its own,
cache_lt_<n>, n from its patch (platform/3ds/source/ctr_halfart.c). build.sh leaves the patch and the patched pages
(tools/analysis/halfart_pack.py --pages) in the build folder; the pages go into a copy of the player's data.win in the
work folder, each in its own blob's space (same file size, same blob offsets and sizes, so the same page headers),
and texprep, unchanged, writes from that copy what the console would. Written as well as "cache", not instead.
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
    # MSYS2 UCRT64 gcc 16.1.0, static, UTF-8 paths (2026-10-27).
    # texprep-src-1:0250c5ae0f1337f8a2fe80a5f44def20d484537fcb3356cacaf41f0d2d944a10
    ("win32", "x86_64"): ("texprep-windows-x64.exe", 354294,
                          "d4dbe6ec7ba7f9a7d69506f479d305000eda4e48f59c1f3517a574e9cb9100d0"),
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


def _run(argv):
    return subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True, text=True, encoding='utf-8',
                          errors='replace', timeout=1800, **hidden_options())


def _run_texprep(tool, data_win, local, name, run, log, source=None):
    """texprep from data_win (or the copy at source) into a fresh local folder: (page count, bytes) or a problem
    string."""
    if os.path.isdir(local):
        shutil.rmtree(local)        # always from scratch, so the output depends only on data.win and the tool
    os.makedirs(local)
    try:
        pages = page_count(data_win)
        r = run([tool.path, source or data_win, local])
    except (OSError, ValueError, subprocess.SubprocessError) as e:
        return "%s: %s" % (name, e)
    out = (r.stdout or "").strip()
    if out:
        log(out.replace(source or data_win, name))
    if r.returncode != 0 or not os.path.isfile(os.path.join(local, MARKER)):
        return "%s: texprep exit %d" % (name, r.returncode)
    missing = [i for i in range(pages) if not os.path.isfile(os.path.join(local, page_name(i)))]
    # The console writes no file for an entry without an image; the marker counts every entry.
    size = sum(os.path.getsize(os.path.join(local, page_name(i))) for i in range(pages) if i not in missing)
    size += os.path.getsize(os.path.join(local, MARKER))
    return pages, size


def prepare(tool, product, folder, work, parts=None, run=None, log=print, halfart=None):
    """Runs texprep for every delivered data.win into <work>/texcache/<game>/... Returns (sets, problems): only
    complete folders are in sets; a data.win whose folder didn't complete is left to the console. halfart: None
    follows products.HALFART; on, UNDERTALE's half-resolution art folder is prepared too (halfart_set)."""
    import products
    run = run or _run
    sets, problems = [], []
    for data_win, rel in data_wins(product, folder, parts):
        local = os.path.join(work, "texcache", product.key, *(rel.split("/") if rel else []), CACHE_DIR)
        got = _run_texprep(tool, data_win, local, rel or "data.win", run, log)
        if isinstance(got, str):
            problems.append(got)
            continue
        sets.append(CacheSet(data_win, sd_cache_dir(product, rel), local, *got))
        if not rel and products.halfart(product.key, "o3ds", halfart):
            cs, problem = halfart_set(tool, product, data_win, work, run, log)
            if cs is not None:
                sets.append(cs)
            else:
                problems.append(problem)
    return sets, problems


# ---------------------------------------------------------------------------------------------------------------------
# Half-resolution art (products.HALFART)

# What build.sh writes for the release's "halfart" (box filter), relative to the build's source tree.
HALFART_PATCH = ("build", "halfart-undertale", "undertale-box")
HALFART_HEADER = struct.Struct("<4sIIIQIIII")    # ctr_halfart.c HaHeader
HALFART_ITEM = 28                                # its HaItem


def halfart_cache_run(patch):
    """n in the build's cache_lt_<n>: the patch's SHA-256, first 32 bits, top bit set (CMake's
    CTRPORT_HALFART_CACHE_RUN), XOR texture_cache_run, which is 0 outside tests."""
    return int(hashlib.sha256(patch).hexdigest()[:8], 16) | 0x80000000


def _fnv1a(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def halfart_pages(patch, pages_file):
    """The patch's pages ({page: first-4096-bytes hash}) and the build's patched pages [(page, blob offset, blob
    size, PNG)], each page of the patch once. Raises ValueError for files that don't fit together."""
    if len(patch) < HALFART_HEADER.size:
        raise ValueError("short patch")
    magic, version, _size, _game, _stamp, items, npages, _ops, _ = HALFART_HEADER.unpack_from(patch)
    if magic != b"DTHR" or version != 1:
        raise ValueError("not a version 1 patch")
    at = HALFART_HEADER.size + items * HALFART_ITEM
    if at + 16 * npages > len(patch):
        raise ValueError("short patch")
    hashes = {}
    for k in range(npages):
        page, h, _first, _count = struct.unpack_from("<4I", patch, at + 16 * k)
        hashes[page] = h
    if pages_file[:44] != struct.pack("<4sII", b"DTHP", 1, npages) + hashlib.sha256(patch).digest():
        raise ValueError("the pages file is not this patch's")
    out, at = [], 44
    for _k in range(npages):
        if at + 16 > len(pages_file):
            raise ValueError("short pages file")
        page, off, size, n = struct.unpack_from("<4I", pages_file, at)
        at += 16
        if page not in hashes or any(page == o[0] for o in out) or at + n > len(pages_file) or n > size:
            raise ValueError("the pages file is not this patch's")
        out.append((page, off, size, pages_file[at:at + n]))
        at += n
    if at != len(pages_file):
        raise ValueError("the pages file is not this patch's")
    return hashes, out


def halfart_set(tool, product, data_win, work, run=None, log=print):
    """(CacheSet, None) for the half-resolution art build's own folder, or (None, problem): the console prepares it
    when the build's patch is missing, isn't for this data.win, or texprep doesn't finish."""
    base = os.path.join(work, "src", *HALFART_PATCH)
    name = "data.win (half-resolution art)"
    try:
        with open(base + ".dthr", "rb") as f:
            patch = f.read()
        with open(base + ".pages", "rb") as f:
            pages_file = f.read()
        hashes, pages = halfart_pages(patch, pages_file)
        if HALFART_HEADER.unpack_from(patch)[2] != os.path.getsize(data_win):
            raise ValueError("the build's patch is for another data.win")
    except (OSError, ValueError) as e:
        return None, "%s: %s" % (name, e)
    n = halfart_cache_run(patch)
    copy = os.path.join(work, "texcache", "halfart", "data.win")
    os.makedirs(os.path.dirname(copy), exist_ok=True)
    try:
        shutil.copyfile(data_win, copy)
        with open(copy, "r+b") as f:
            for page, off, size, png in pages:
                f.seek(off)
                if _fnv1a(f.read(4096)) != hashes[page]:
                    raise ValueError("page %d is not the one the build's patch was made from" % page)
                f.seek(off)
                f.write(png + bytes(size - len(png)))
        got = _run_texprep(tool, data_win, os.path.join(work, "texcache", product.key, "cache_lt_%u" % n), name,
                           run or _run, log, source=copy)
    except (OSError, ValueError) as e:
        got = "%s: %s" % (name, e)
    finally:
        try:
            os.remove(copy)     # a copy of the player's game data: never kept
        except OSError:
            pass
    if isinstance(got, str):
        return None, got
    return CacheSet(data_win, "/".join((product.sd_data_dir, "cache_lt_%u" % n)),
                    os.path.join(work, "texcache", product.key, "cache_lt_%u" % n), *got), None


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
    files: tuple = ()      # only the explicit cache/receipt paths delivered by this operation


def deliver_textures(sd_root, product, folder, work, parts=None, tool=None, fs=None, progress=None,
                     cancelled=lambda: False, log=print, run=None, halfart=None, prepared=None):
    """Prepare on the PC and write to the card. Failures to prepare are not errors (the console prepares); a failed
    card write raises OSError like any other copy."""
    tool = tool or find_tool(work, log=log)
    if not tool.ok:
        return Result(False, "Textures: the 3DS will prepare them the first time (%s)." % tool.message)
    sets, problems = (prepared if prepared is not None else
                      prepare(tool, product, folder, work, parts, run=run, log=log, halfart=halfart))
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
    files = tuple(cs.sd_dir + '/' + name for cs in sets
                  for name in [page_name(i) for i in _local_pages(cs)] + [MARKER])
    return Result(True, msg, written, kept, nbytes, files)


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
        import products
        if not rel and products.halfart(product.key):
            lines.append("  %s, half-resolution art (the Old 3DS build) -> sd:/%s/cache_lt_<n>/ (n from the build's "
                         "patch), the same way, from the build's patched pages" % (rel or "data.win", product.sd_data_dir))
    return lines


def data_version_dry_run_lines(product, folder, parts=None, source_root=SOURCE_ROOT):
    """The dry run's data-version.txt lines (no hashing, nothing written)."""
    revision = data_versions_revision(source_root)
    lines = ["Version check on the PC: %s" % ("data_versions.inc revision %d" % revision if revision is not None
                                             else "off (no data_versions.inc revision); the 3DS hashes each data.win")]
    if revision is not None:
        lines[0] += "; runtime revision selected from the verified digest (GOG may differ)"
    if revision is None or folder is None or not os.path.isdir(folder):
        return lines
    for data_win, rel in data_wins(product, folder, parts):
        lines.append("  %s -> sd:/%s/%s: keep if revision, size, SHA-256 and path match; else write \"<runtime-revision> <size> %d "
                     "<SHA-256>\" + \"%s\" as .part then rename, after the texture folder"
                     % (rel or "data.win", sd_cache_dir(product, rel), DATA_VERSION, DATA_VERSION_MTIME,
                        console_data_win_path(product, rel)))
    return lines


# ---------------------------------------------------------------------------------------------------------------------
# data-version.txt: the console's data.win version cache, written on the PC
#
# On a game's first start the console hashes the whole data.win with SHA-256 to check it against data_versions.inc
# (platform/3ds/source/ctr_data_version.c, CtrDataVersion_check): 24-64 s per data.win on an Old 3DS. It caches the
# result in <data.win folder>/cache/data-version.txt as fprintf("%u %llu %lld %s\n%s\n", revision, st_size,
# st_mtime, digest, data.win path) and skips the hash while all five fields still match. The builder writes that
# base receipt after freshly hashing the final destination. mtime is 0: libctru stat reports 0 on SD (hardware and
# Azahar). The runtime adds a FAT1 archive timestamp on first trust, and rehashes if that timestamp changes.
# A receipt authorizes the warning and generated-data consumers; it is not independent per-boot content proof.

DATA_VERSION = "data-version.txt"
DATA_VERSION_MTIME = 0
FAT_SLACK = 2.0     # as deliver.FAT_SLACK: FAT keeps modification times to 2 s
_REVISION_RE = re.compile(r"^\s*#define\s+DATA_VERSIONS_REVISION\s+([0-9]+)\s*$", re.M)
_BASE_REVISION_RE = re.compile(
    r"^#if \(defined\(DATA_VERSIONS_BASE_ONLY\) \|\| defined\(DATA_VERSIONS_LATE_ONLY\)\)"
    r" && !defined\(CTRPORT_GOG_COMPAT\)\n#undef DATA_VERSIONS_REVISION\n"
    r"#define DATA_VERSIONS_REVISION ([0-9]+)\n#endif$", re.M)
_GOG_ROWS_RE = re.compile(
    r"^#if !defined\(DATA_VERSIONS_BASE_ONLY\) && !defined\(DATA_VERSIONS_LATE_ONLY\)\n"
    r"(.*?)^#endif$", re.M | re.S)
_DATA_VERSION_RE = re.compile(r"^([0-9]+) ([0-9]+) (-?[0-9]+) ([0-9a-f]{64})\n([^\r\n]*)\n(?:FAT1 [0-9]+\n)?$")


def data_versions_revision(source_root=SOURCE_ROOT, digest=None):
    """Runtime receipt revision, including the retained non-GOG cache ABI.

    The table reader's first define is not necessarily the console's define. Non-GOG
    builds retain revision 2; build.sh enables GOG compatibility only for its full
    container digest. Resolve that same table branch from the already verified digest.
    Unknown conditional formats omit receipts rather than guessing a runtime ABI.
    """
    try:
        with open(os.path.join(source_root, "platform", "3ds", "data_versions.inc"), encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return None
    revisions = _REVISION_RE.findall(text)
    if len(revisions) == 1:
        return int(revisions[0])
    base = _BASE_REVISION_RE.search(text)
    gog = _GOG_ROWS_RE.search(text)
    if len(revisions) != 2 or base is None or gog is None or revisions[1] != base.group(1):
        return None
    gog_digests = re.findall(r'DATA_VERSION\("undertale", "root", [0-9]+ULL, "([0-9a-f]{64})"\)', gog.group(1))
    return int(revisions[0] if digest in gog_digests else base.group(1))


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
    destination: str = None
    digest: str = None


def _digest(path):
    try:
        import verify_game
        return verify_game.data_digest(path)    # the digest the version check computed, when it ran
    except ImportError:
        return sha256_file(path)


def file_identity(st):
    """Detect replacement or mutation during a read; dates alone never establish content."""
    return (st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns, st.st_ctime_ns)


def checked_digest(path, expected=None):
    """Fresh SHA-256, with path and open-file pre/post checks. Never uses the digest memo."""
    before = os.stat(path)
    sha = hashlib.sha256()
    count = 0
    with open(path, "rb") as stream:
        opened = os.fstat(stream.fileno())
        # On Windows Python's stat/fstat disagree about ctime (creation vs change time).
        # Compare ctime only within the same API; device/inode/size/mtime still bind the open handle.
        if file_identity(opened)[:4] != file_identity(before)[:4]:
            raise OSError("File changed before verification: %s" % path)
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(block)
            count += len(block)
        after = os.fstat(stream.fileno())
    if (file_identity(opened) != file_identity(after) or count != before.st_size or
            file_identity(before) != file_identity(os.stat(path))):
        raise OSError("File changed during verification: %s" % path)
    result = sha.hexdigest()
    if expected is not None and result != expected:
        raise OSError("File content changed. Verify the game files and try again: %s" % path)
    return result


def invalidate_version(data_win):
    """Remove a receipt before replacing its data, or when verification fails."""
    try:
        os.remove(os.path.join(os.path.dirname(data_win), "cache", DATA_VERSION))
    except FileNotFoundError:
        pass


def plan_data_versions(sd_root, product, folder, parts=None, source_root=SOURCE_ROOT, digest=None):
    """One VersionPlan per delivered data.win. Keeps a file on the card that already holds this data.win's revision,
    size, digest and path. Freshly hash the destination against the verified source before keeping or publishing
    any receipt. A size/date match is never content verification. Otherwise skip=True and invalidate the receipt."""
    revision = data_versions_revision(source_root)
    if revision is None:
        return []
    digest = digest or _digest
    out = []
    for data_win, rel in data_wins(product, folder, parts):
        sd_file = sd_cache_dir(product, rel) + "/" + DATA_VERSION
        path = console_data_win_path(product, rel)
        size = os.path.getsize(data_win)
        card_dw = os.path.join(sd_root, *sd_cache_dir(product, rel).split("/")[:-1], 'data.win')
        dig = digest(data_win)
        checked_digest(data_win, dig)
        revision = data_versions_revision(source_root, dig)
        if revision is None:
            continue
        try:
            copied = os.path.getsize(card_dw) == size and checked_digest(card_dw) == dig
        except OSError:
            copied = False
        if not copied:
            invalidate_version(card_dw)
            out.append(VersionPlan(sd_file, b"", False, "the card's data.win is not this one", skip=True))
            continue
        data = data_version_text(revision, size, dig, path)
        try:
            with open(os.path.join(sd_root, *sd_file.split("/")), "rb") as f:
                have = parse_data_version(f.read(4096))
        except OSError:
            have = None
        if have is not None and have[:2] == (revision, size) and have[3:] == (dig, path):
            out.append(VersionPlan(sd_file, data, True, "already there", destination=card_dw, digest=dig))
        else:
            out.append(VersionPlan(sd_file, data, False, "missing" if have is None else "stale",
                                   destination=card_dw, digest=dig))
    return out


def write_data_versions(sd_root, plans, fs=None, work=None):
    """Writes each file as .part then renames it. Returns (written, kept). Raises OSError part way."""
    fs = fs or FileOps()
    written = kept = 0
    for p in plans:
        if p.skip:
            continue
        try:
            if p.destination:
                before = file_identity(os.stat(p.destination))
                checked_digest(p.destination, p.digest)
                if before != file_identity(os.stat(p.destination)):
                    raise OSError("Game data changed before writing its receipt. Try copying again.")
        except OSError:
            invalidate_version(p.destination)
            raise
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
            if p.destination and before != file_identity(os.stat(p.destination)):
                invalidate_version(p.destination)
                raise OSError("Game data changed while writing its receipt. Try copying again.")
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
    return Result(True, msg, written, kept, sum(len(p.data) for p in plans if not p.keep),
                  tuple(p.sd_file for p in plans if not p.skip))
