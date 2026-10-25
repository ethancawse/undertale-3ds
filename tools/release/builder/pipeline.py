"""Wizard page 6: build the user's personal copy with the real pipeline (AOT + platform/3ds/build.sh).

The source tree is mirrored into the builder's short work folder (build.sh writes build/ beside its sources), then
build.sh runs in devkitPro's MSYS2 bash with the builder's own Python first on PATH. Progress has four stages:
generating code, preparing graphics, compiling, packaging. A build without AOT is stopped, not delivered.
"""
from __future__ import annotations

import dataclasses
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import threading
import time

import products
import toolchain
import home_art
import chapters
from processes import hidden_options

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))     # repo root (or the export's root)
# What build.sh and CMake read, relative to the source root.
SOURCE_DIRS = ("engine/src", "engine/vendor", "engine/compat", "platform/3ds", "tools/aot", "tools/buildcache", "tools/deps/openh264",
               "tools/texprep", "tools/roomplan", "tools/release/licenses")
SOURCE_FILES = ("engine/LICENSE", "VERSION", "LICENSE", "THIRD_PARTY_NOTICES",
                "docs/release/build-contract.json", "tools/release/prepare_legal.py",
                # CMake's renderer launcher imports both helpers, including on builds that delegate unchanged.
                "tools/analysis/vine_old_observer_compile.py", "tools/analysis/vine_old_parent.py",
                "tools/analysis/vine_extract.py")
SKIP_DIRS = {"__pycache__", ".git", "build", "scratch"}
SKIP_SUFFIXES = (".pyc", ".win", ".ogg", ".cia", ".3dsx", ".elf", ".o", ".a", ".dat")


def default_work_dir(env=None, platform=sys.platform):
    """A short folder without spaces (CMake object paths come close to Windows' 250-character limit)."""
    env = os.environ if env is None else env
    if platform == "win32":
        base = env.get("LOCALAPPDATA", "")
        cand = os.path.join(base, "u3ds", "w") if base else ""
        if cand and " " not in cand and cand.isascii() and len(cand) <= 60:
            return cand
        return "C:/u3ds"
    if platform == "darwin":
        return os.path.join(os.path.expanduser("~/Library/Caches"), "u3ds")
    return os.path.join(env.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"), "u3ds")


# ---------------------------------------------------------------------------------------------------------------------
# Source tree

# The release export puts the full label in VERSION. platform/3ds/build_id.cmake (release
# readiness S8) stamps a build whose source is not its own git checkout "archive-<version>", plus the AOT tag. The
# builder only copies VERSION, never writes it, and checks the stamp after the build.
LONGEST_AOT_TAG = "+aot12ch"
INCOMPLETE = ("This source archive is incomplete (%s).\n"
              "Download the builder again and unpack the full zip.")


def release_version(root=SOURCE_ROOT):
    """Read the full release label through the source tree's shared contract."""
    try:
        return products.release_contract.read(root)['version']
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise BuildFailed(INCOMPLETE % str(exc)) from exc


def expected_build_id(version, root=SOURCE_ROOT):
    return products.release_contract.read(root)['archive_id_prefix'] + version


def check_build_id(header, version, root=SOURCE_ROOT):
    """Check the archive label and AOT tag against the selected source contract."""
    try:
        with open(header, encoding="utf-8", errors="replace") as f:
            m = re.search(r'#define CTRPORT_COMMIT_TEXT "([^"]*)"', f.read())
    except OSError:
        m = None
    if not m:
        raise BuildFailed("Missing build ID header (%s).\n" % header + FAILED)
    bid = m.group(1)
    contract = products.release_contract.read(root)
    if not re.fullmatch(re.escape(expected_build_id(version, root)) + r"\+aot[0-9a-z]+", bid) or len(bid) > contract['build_id_storage_bytes'] - 2:
        raise BuildFailed("Build ID is %r, not %s+aot...\n" % (bid, expected_build_id(version, root)) + FAILED)
    return bid


def _wanted(name):
    return not name.lower().endswith(SKIP_SUFFIXES)


def source_wanted(relative, game=None):
    """Inputs used by the 3DS build, excluding desktop backends and private test data."""
    if relative.replace("\\", "/") == "tools/release/builder/undertale-3ds-builder.cmd":
        return False  # Packages have one game-specific launcher at their root.
    parts = relative.replace("\\", "/").split("/")
    if any(p in SKIP_DIRS or p == ".github" for p in parts) or not _wanted(parts[-1]):
        return False
    if parts[:2] == ["engine", "src"]:
        if len(parts) == 3:
            return parts[-1] != "loop.c"
        sub = parts[2:]
        if sub[0] == "cli":
            return len(sub) == 2 and sub[1] in ("state_hash.c", "state_hash.h", "state_hash_reach.inc")
        if sub[0] == "physics":
            return (len(sub) == 2 and sub[1] in ("physics.c", "physics.h", "physics_engine.h")) or sub[1] == "disabled"
        return sub[0] in ("aot", "image") or sub == ["video", "video.h"]
    if parts[:2] == ["engine", "compat"]:
        return parts[2] in ("getopt", "stdint")
    if parts[:2] == ["engine", "vendor"]:
        return parts[2] in ("base64", "bzip2", "md5", "miniz", "sha1", "stb")
    if parts[:2] == ["tools", "aot"]:
        if parts[2] == "selection":
            return game is None or not (parts[-1].startswith("deltarune" if game == "undertale" else "undertale") or
                                        parts[-1] == "typed-" + ("deltarune" if game == "undertale" else "undertale") + ".txt")
        # loadscreen_art.py: the loading screen art (build.sh runs it for both games). escape.py: gmlaot.py's
        # escape elision (G9), imported for WAD 17 only but part of gmlaot.py's import closure. stpack.py and
        # stglyphs.py: UNDERTALE's Crisp text glyph pack (build.sh makes it for UNDERTALE by default); stmenu.py: the
        # Smoothing menu's data beside it (souls, sharp sprites, previews). symverdict.py: symmetric
        # halving's table, which fontverdict.py's build mode writes for UNDERTALE only.
        return len(parts) == 3 and (parts[-1] in (
            "builtin_audit.py", "btnprompt.py", "corpus.py", "data_versions.py", "escape.py", "fontverdict.py",
            "gmlaot.py", "prepare.py", "select.py", "typed.py", "loadscreen_art.py", "late_numeric.py")
            or (parts[-1] in ("pixgrid.py", "btnprompt_build.py", "stglyphs.py", "stpack.py", "stmenu.py", "symverdict.py") and game != "deltarune")
            or (parts[-1] == "chapter_manifest.py" and game != "undertale"))
    if parts[:3] == ["tools", "deps", "openh264"]:
        # DELTARUNE's intro video decoder; build.sh never builds it for UNDERTALE.
        return game != "undertale"
    if parts[:2] == ["tools", "roomplan"]:
        # The room plan's build step (build.sh runs build_plan.py) and this game's committed records.
        # records/sounds/: the sound plan records (build_sfxplan.py, UNDERTALE).
        if parts[2] == "records" and (len(parts) == 4 or (len(parts) == 5 and parts[3] == "sounds")):
            return game is None or parts[-1].startswith(game + "-")
        return len(parts) == 3 and parts[-1] in ("build_plan.py", "build_sfxplan.py", "closure.py", "dwinfo.py", "records.py", "source_plan.py")
    if parts[:2] == ["platform", "3ds"]:
        if parts == ["platform", "3ds", "source", "z_builderx_old.c"] and game == "undertale":
            return False
        if len(parts) > 3 and parts[2] == "bench":
            return parts[-1] in (["drawskip_" + game + ".txt"] if game else
                                ["drawskip_undertale.txt", "drawskip_deltarune.txt"])
        if len(parts) > 3 and parts[2] == "assets":
            return (parts[-1].startswith(("banner-" + (game + "-" if game else ""),
                                          "icon-" + (game + "-" if game else ""))) or
                    parts[-1] in ("banner.wav", "btnprompt_glyphs.txt", "make_art.py"))
        return len(parts) == 3 or (parts[2] in ("aot", "bench", "cold", "cia", "source", "vendor", "hotpin")
                                   and "profiles" not in parts)
    return True


def mirror_source(src_root, dest_root, game=None):
    """Copies the build's sources into dest_root, replacing changed files and removing stale ones, so a second build
    (the other model) reuses the compiled objects. Returns (copied, removed)."""
    copied = removed = 0
    os.makedirs(dest_root, exist_ok=True)
    for rel in SOURCE_DIRS:
        src = os.path.join(src_root, rel)
        dst = os.path.join(dest_root, rel)
        if not os.path.isdir(src):
            continue
        want = set()
        for dirpath, dirnames, filenames in os.walk(src):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            rd = os.path.relpath(dirpath, src)
            for n in filenames:
                r = os.path.normpath(os.path.join(rd, n))
                if not source_wanted(os.path.join(rel, r), game):
                    continue
                want.add(os.path.normcase(r))
                s, d = os.path.join(src, r), os.path.join(dst, r)
                ss = os.stat(s)
                try:
                    ds = os.stat(d)
                    if ds.st_size == ss.st_size and int(ds.st_mtime) == int(ss.st_mtime):
                        continue
                except OSError:
                    pass
                os.makedirs(os.path.dirname(d), exist_ok=True)
                shutil.copy2(s, d)
                copied += 1
        if os.path.isdir(dst):
            for dirpath, dirnames, filenames in os.walk(dst):
                dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
                rd = os.path.relpath(dirpath, dst)
                for n in filenames:
                    r = os.path.normpath(os.path.join(rd, n))
                    if os.path.normcase(r) not in want and _wanted(n):
                        os.remove(os.path.join(dst, r))
                        removed += 1
    for rel in SOURCE_FILES:
        s = os.path.join(src_root, rel)
        if os.path.isfile(s):
            d = os.path.join(dest_root, rel)
            os.makedirs(os.path.dirname(d), exist_ok=True)
            shutil.copy2(s, d)
    return copied, removed


def source_payload(root, game):
    """The kit's build inputs, including its builder; excludes launchers/runtime and the descriptor."""
    root = Path(root)
    paths = set(SOURCE_FILES)
    for directory in SOURCE_DIRS + ('tools/release/builder',):
        base = root / directory
        for p in base.rglob('*'):
            if p.is_symlink():
                raise ValueError('Source contains a symlink: ' + p.relative_to(root).as_posix())
            rel = p.relative_to(root).as_posix()
            if p.is_file() and source_wanted(rel, game) and not rel.startswith('tools/release/builder/runtime/'):
                paths.add(rel)
    # These modules participate in the build but live outside the mirrored source folders.
    paths.update(('tools/release/release_contract.py', 'tools/release/verify_game.py',
                  'tools/release/music-versions.json'))
    for rel in paths:
        if (root / rel).is_symlink():
            raise ValueError('Source contains a symlink: ' + rel)
    return {rel: (root / rel).read_bytes() for rel in sorted(paths)}


def release_source(root, game):
    contract = products.release_contract
    try:
        descriptor = json.loads((Path(root) / contract.DESCRIPTOR).read_text(encoding='utf-8'))
        return contract.validate_source(descriptor, game, release_version(root),
                                        contract.payload_digest(source_payload(root, game)))
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise BuildFailed('Could not verify this builder\'s public source: ' + str(exc)) from exc


def write_python_shims(bin_dir, python):
    """python3 and python for build.sh, pointing at the builder's interpreter (the embeddable package on a user's
    PC), so the build finds Python exactly as on a developer machine."""
    os.makedirs(bin_dir, exist_ok=True)
    py = python.replace("\\", "/")
    if re.match(r"^[A-Za-z]:/", py):
        py = "/" + py[0].lower() + py[2:]
    for name in ("python3", "python"):
        with open(os.path.join(bin_dir, name), "w", encoding="utf-8", newline="\n") as f:
            f.write('#!/bin/sh\nexec %s "$@"\n' % shlex.quote(py))
        os.chmod(os.path.join(bin_dir, name), 0o755)
    # build.sh uses nproc, which macOS does not ship. Keep this local to the builder.
    with open(os.path.join(bin_dir, "nproc"), "w", encoding="utf-8", newline="\n") as f:
        f.write('#!/bin/sh\nexec %s -I -c "import os; print(os.cpu_count() or 1)"\n' % shlex.quote(py))
    os.chmod(os.path.join(bin_dir, "nproc"), 0o755)


BUILD_SCRIPT = r'''
set -euo pipefail
export PATH="$(cygpath -u "$1"):$(cygpath -u "$2"):$PATH"
DEVKITPRO="$(cygpath -u "$4")"
export DEVKITPRO
export DEVKITARM="$DEVKITPRO/devkitARM"
export PATH="$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH"
cd "$(cygpath -u "$3")"
shift 4
exec bash platform/3ds/build.sh "$@"
'''

UNIX_BUILD_SCRIPT = r'''
set -euo pipefail
export DEVKITPRO="$4"
export DEVKITARM="$DEVKITPRO/devkitARM"
export PATH="$1:$2:$2/cmake/cmake-3.31.8-macos-universal/CMake.app/Contents/bin:$DEVKITARM/bin:$DEVKITPRO/tools/bin:/Applications/CMake.app/Contents/bin:/opt/homebrew/bin:/usr/local/bin:$PATH"
cd "$3"
shift 4
exec bash platform/3ds/build.sh "$@"
'''


def build_argv(tc: toolchain.Toolchain, work, game, target, data_dir, openh264=False, platform=sys.platform):
    args = [game, target, "aot", "data=" + (data_dir.replace("\\", "/") if platform == "win32" else data_dir)]
    if not openh264:
        args.append("openh264=off")
    if platform == "win32":
        return [tc.bash, "--login", "-c", BUILD_SCRIPT, "builder-build", os.path.join(work, "bin"),
                os.path.join(work, "tools", "bin"), os.path.join(work, "src"), tc.root or "C:/devkitPro"] + args
    return [tc.bash or "/bin/bash", "-c", UNIX_BUILD_SCRIPT, "builder-build", os.path.join(work, "bin"),
            os.path.join(work, "tools", "bin"), os.path.join(work, "src"), tc.root or "/opt/devkitpro"] + args


# ---------------------------------------------------------------------------------------------------------------------
# Progress

STAGES = ("Generating code", "Preparing graphics", "Compiling", "Packaging")
_PCT = re.compile(r"^\[\s*(\d+)%\]")


class Progress:
    """Turns build.sh output into (stage, percent, estimate). The estimate starts from the measured cold time,
    scaled by AOT time against the measuring PC and corrected by the compile percentages."""

    def __init__(self, product: products.Product, target, clock=time.monotonic):
        self.clock = clock
        self.ref_total, self.ref_aot = product.reference_seconds[target]
        self.ref_graphics = product.graphics_seconds
        self.start = clock()
        self.stage = 0
        self.percent = 0
        self.aot_start = None
        self.aot_seconds = None
        self.compile_start = None
        self.graphics_start = None
        self.graphics_seconds = None
        self.no_aot = False
        self.aot_up_to_date = False
        self.warnings = 0
        self.outputs = []

    def feed(self, line):
        t = self.clock()
        s = line.rstrip("\r\n")
        if s.startswith("AOT: generating"):
            self.aot_start = t
        elif "code for" in s and "is up to date" in s and s.startswith("AOT:"):
            self.aot_up_to_date = True
        elif "WITHOUT AOT" in s:
            self.no_aot = True
        elif s.startswith("-- Configuring done"):
            if self.aot_start is not None and self.aot_seconds is None:
                self.aot_seconds = t - self.aot_start
        elif s == "Builder phase: Preparing graphics":
            self.stage = max(self.stage, 1)
            self.graphics_start = t
        elif s == "Builder phase: Compiling":
            self.stage = max(self.stage, 2)
            self.compile_start = t
            if self.graphics_start is not None:
                self.graphics_seconds = t - self.graphics_start
        else:
            m = _PCT.match(s)
            if m:
                self.stage = max(self.stage, 2)
                if self.compile_start is None:
                    self.compile_start = t
                self.percent = int(m.group(1))
                if self.percent >= 100:
                    self.stage = 3
            elif "Building CIA" in s or s.startswith("built "):
                self.stage = 3
                if s.startswith("built "):
                    self.outputs.append(s[6:])
        if ": warning:" in s:
            self.warnings += 1

    def speed_factor(self):
        if self.aot_seconds and self.ref_aot and not self.aot_up_to_date:
            return max(0.5, self.aot_seconds / self.ref_aot)
        return None

    def completion(self):
        """One model's display progress; table preparation has its own bounded share."""
        share = min(65.0, 90.0 * self.ref_graphics / self.ref_total)
        if self.stage == 0:
            return 5.0
        if self.stage == 1:
            spent = self.clock() - self.graphics_start if self.graphics_start is not None else 0.0
            expected = max(1.0, self.ref_graphics * (self.speed_factor() or 1.0))
            return 5.0 + share * min(0.95, max(0.0, spent / expected))
        if self.stage == 2:
            return 5.0 + share + (90.0 - share) * self.percent / 100.0
        return 97.0

    def estimate(self):
        """(low, high) seconds left, or None before anything is known."""
        now = self.clock()
        elapsed = now - self.start
        if self.stage == 2 and self.compile_start is not None and 5 <= self.percent < 100:
            spent = now - self.compile_start
            left = (spent / self.percent * (100 - self.percent)
                    + 0.2 * (self.ref_total - self.ref_graphics) * (self.speed_factor() or 1))
            return (left * 0.8, left * 1.3)
        f = self.speed_factor()
        if f is None:
            # No fresh AOT sample: historical best case and a busy PC (12x, relsimple's n3ds row).
            reference = self.ref_total
            if self.graphics_seconds is not None:
                reference -= self.ref_graphics
                elapsed -= self.graphics_seconds
            return (max(0.0, reference - elapsed), max(0.0, reference * 12 - elapsed))
        total = self.ref_total * f
        if self.graphics_seconds is not None:
            total += self.graphics_seconds - self.ref_graphics * f
        return (max(0.0, total * 0.8 - elapsed), max(0.0, total * 1.6 - elapsed))


def format_eta(est):
    if est is None:
        return ""

    def m(s):
        return "%d min" % max(1, round(s / 60)) if s >= 90 else "%d s" % max(1, round(s))
    lo, hi = est
    return "~%s left" % m(lo) if hi - lo < 30 else "%s-%s left" % (m(lo), m(hi))


# ---------------------------------------------------------------------------------------------------------------------
# Support log

class SupportLogFilter:
    """State survives separate emit() calls, including argsaves' multi-line PC-save section."""

    def __init__(self, home=None):
        self.home = home or os.path.expanduser("~")
        self.in_saves = False

    def __call__(self, text):
        out = []
        for line in text.splitlines() or [""]:
            if line.lstrip().startswith(("PC saves (", "PC saves:")):
                self.in_saves = True
                out.append("[PC save details removed]")
                continue
            if self.in_saves:
                if line.strip():
                    continue
                self.in_saves = False
            if line.startswith("AOT:   ") or "gml_" in line:
                out.append("[line removed: game identifiers]")
                continue
            # Keep artifact hashes even when a CLI puts one after a private output path.
            hashes = re.findall(r"\bsha256\s+([0-9a-f]{64})\b", line, re.I)
            build_ids = re.findall(r"\barchive-[0-9][0-9A-Za-z.+-]*", line)
            for h in {self.home, self.home.replace("\\", "/")}:
                if h and len(h) > 3:
                    line = line.replace(h, "%USERPROFILE%")
            line = re.sub(r"%USERPROFILE%[^\r\n]*", "%USERPROFILE%", line)
            # Paths may be outside the current home: external Steam libraries, mounted Windows homes,
            # and named Mac volumes. Drop the path and its tail rather than leaking a basename with spaces.
            line = re.sub(r"(?<![\w:/\\])(?:[A-Za-z]:[\\/]|\\\\|/(?=[^\s/]))[^\r\n]*", "<path>", line)
            line = re.sub(r"\b\d{17}\b", "<account>", line)
            for digest in hashes:
                if digest not in line:
                    line += " sha256 " + digest
            for build_id in build_ids:
                if build_id not in line:
                    line += " build ID " + build_id
            out.append(line)
        return "\n".join(out) + "\n"


def filter_log(text, home=None):
    """Sanitize a complete support log; streaming callers keep one SupportLogFilter instance."""
    return SupportLogFilter(home)(text)


# ---------------------------------------------------------------------------------------------------------------------
# Running a build

class BuildFailed(Exception):
    def __init__(self, message, log_path=None):
        super().__init__(message)
        self.log_path = log_path


FAILED = "The build stopped.\nClick Try again, or click Copy log to report the error."


@dataclasses.dataclass
class Artifact:
    target: str
    kind: str          # "3dsx" or "cia"
    path: str
    size: int
    sha256: str


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def kill_tree(proc):
    """Stops the build: the PID the builder started and everything under it."""
    if proc.poll() is not None:
        return
    if sys.platform == "win32":
        subprocess.run(["taskkill", "/PID", str(proc.pid), "/T", "/F"], stdin=subprocess.DEVNULL,
                       capture_output=True, timeout=30, **hidden_options())
    else:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except OSError:
            pass


class Build:
    """One target. run() blocks; cancel() may be called from another thread."""

    def __init__(self, product, tc, work, target, data_dir, outputs=("3dsx",), openh264=False, on_line=None,
                 on_progress=None, popen=subprocess.Popen, source_root=SOURCE_ROOT, clock=time.monotonic,
                 art_generator=home_art.generate, parts=None):
        self.product, self.tc, self.work, self.target = product, tc, work, target
        self.data_dir, self.outputs, self.openh264 = data_dir, tuple(outputs), openh264
        self.on_line = on_line or (lambda s: None)
        self.on_progress = on_progress or (lambda p: None)
        self.popen = popen
        self.source_root = source_root
        self.art_generator = art_generator
        self.parts = parts
        self.progress = Progress(product, target, clock)
        self.proc = None
        self.cancelled = False
        self._lock = threading.Lock()
        os.makedirs(os.path.join(work, "logs"), exist_ok=True)
        self.log_path = os.path.join(work, "logs", "build-%s-%s-%s.log" % (product.key, target,
                                                                           time.strftime("%Y%m%d-%H%M%S")))

    def cancel(self):
        with self._lock:
            self.cancelled = True
            if self.proc is not None:
                kill_tree(self.proc)

    def prepare(self):
        if self.product.key == 'deltarune':
            try:
                self.parts = chapters.runtime_selection(self.data_dir, self.parts, self.target)
            except (OSError, ValueError) as exc:
                raise BuildFailed(str(exc), self.log_path) from exc
        version = release_version(self.source_root)      # an incomplete archive stops before anything is written
        self.release_source = release_source(self.source_root, self.product.key)
        need = int(self.product.disk_mb[self.target] * 1.5 + 30) * 1024 * 1024
        if self.product.key == 'deltarune':
            need += chapters.stage_bytes(self.data_dir, self.parts)
        os.makedirs(self.work, exist_ok=True)
        free = shutil.disk_usage(self.work).free
        if free < need:
            raise BuildFailed("%s has %d MB free; %d MB needed.\n"
                              "Free up disk space and try again." % (
                                  self.work, free // (1 << 20), need // (1 << 20)))
        copied, removed = mirror_source(self.source_root, os.path.join(self.work, "src"), self.product.key)
        write_python_shims(os.path.join(self.work, "bin"), toolchain.python_exe())
        # The builder's own .cia tools live here (on PATH for the build); devkitPro's tools/bin is CMake's HINTS.
        os.makedirs(os.path.join(self.work, "tools", "bin"), exist_ok=True)
        return version, copied, removed

    def run(self):
        t0 = time.monotonic()
        version, copied, removed = self.prepare()
        # A macOS copy's game.ios is staged as data.win (products.aot_data_dir); a Windows copy is used where it is.
        if self.product.key == 'deltarune':
            try:
                data_dir, self.parts = chapters.stage(self.data_dir, self.work, self.parts)
            except (OSError, ValueError) as exc:
                raise BuildFailed(str(exc), self.log_path) from exc
        else:
            data_dir = products.aot_data_dir(self.product, self.data_dir, os.path.join(self.work, "data"))
        argv = build_argv(self.tc, self.work, self.product.key, self.target, data_dir, self.openh264,
                          platform=self.tc.platform)
        env = dict(os.environ)
        env["CTRPORT_CCACHE_DIR"] = os.path.join(self.work, "ccache")
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        env["CTRPORT_PYTHON"] = toolchain.python_exe()
        env['CTRPORT_RELEASE_SOURCE_URL'] = self.release_source['source_url']
        compiler = toolchain.compiler_details(self.tc)
        tested = products.release_contract.read(self.source_root)['tested_compiler']
        compiler['tested'] = tested
        compiler['layout_pins'] = (compiler['version'] == tested['version'] and
                                   compiler['target'] == tested['target'])
        compiler['warning'] = ('' if compiler['layout_pins'] else
            'Tested with devkitARM GCC %s (%s); found %s (%s), so layout pins are off and performance may differ slightly.' %
            (tested['version'], tested['target'], compiler['version'], compiler['target']))
        env.pop("CTRPORT_FILE_PREFIX_MAP", None)
        kw = hidden_options(group=True)
        if sys.platform != "win32":
            kw["start_new_session"] = True
        # Line-buffered, so the log on disk follows the build (support, and a crash leaves the lines so far).
        with open(self.log_path, "w", encoding="utf-8", newline="\n", buffering=1) as log:
            log.write("builder: %s %s, source %s (%d files copied, %d removed), data %s\n" % (
                self.product.key, self.target, version, copied, removed, self.data_dir))
            if data_dir != self.data_dir:
                log.write("builder: private game inputs staged in %s\n" % data_dir)
            log.write("toolchain packages (pacman -Q at detection):\n")
            log.write('public source: ' + self.release_source['source_url'] + '\n')
            log.write('compiler: ' + json.dumps(compiler, sort_keys=True) + '\n')
            if self.product.key == 'deltarune':
                self.chapter_record = {'version': sorted({v[1][1] for v in chapters.inventory(data_dir).values()
                                                         if v[0] == 'ok'}),
                                       'selected': self.parts,
                                       'installed': [p for p, v in chapters.inventory(self.data_dir).items()
                                                     if p != 'root' and v[0] != 'missing']}
                log.write('chapters: ' + json.dumps(self.chapter_record, sort_keys=True) + '\n')
            if compiler['warning']:
                self.on_line(compiler['warning'])
            for name, package_version in sorted(self.tc.package_versions.items()):
                log.write("  %s %s\n" % (name, package_version))
            if not self.tc.package_versions:
                log.write("  unavailable\n")
            try:
                art = self.art_generator(self.product.key, data_dir,
                    os.path.join(self.work, "src", "platform", "3ds", "assets"),
                    os.path.join(self.tc.root or "/opt/devkitpro", "tools", "bin",
                                 "tex3ds.exe" if self.tc.platform == "win32" else "tex3ds"))
            except home_art.ArtError as exc:
                raise BuildFailed(str(exc), self.log_path) from exc
            except OSError as exc:
                raise BuildFailed("Could not write HOME Menu art. Check free space and folder permissions.",
                                  self.log_path) from exc
            log.write("HOME Menu art: " + json.dumps(art, sort_keys=True) + "\n")
            log.flush()
            with self._lock:
                if self.cancelled:
                    raise BuildFailed("The build was cancelled.", self.log_path)
                self.proc = self.popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, env=env, text=True, encoding="utf-8",
                                       errors="replace", bufsize=1, **kw)
            for line in self.proc.stdout:
                log.write(line)
                self.progress.feed(line)
                self.on_line(line.rstrip("\r\n"))
                self.on_progress(self.progress)
                if self.progress.no_aot and not self.cancelled:
                    # relsimple: the easy path promises a recognised AOT build, never a quiet interpreter build.
                    log.write("builder: stopping, the build would go on without AOT\n")
                    self.cancel()
            rc = self.proc.wait()
        if self.cancelled and not self.progress.no_aot:
            raise BuildFailed("The build was cancelled.\nClick Try again when ready.", self.log_path)
        if self.progress.no_aot:
            raise BuildFailed("Could not generate fast (AOT) code.\n" + FAILED, self.log_path)
        if rc != 0:
            raise BuildFailed("The build stopped (build.sh exit code %d).\n"
                              "Click Try again, or click Copy log to report the error." % rc, self.log_path)
        try:
            bid = check_build_id(os.path.join(self.work, "src", "build", "3ds-%s-%s" % (self.product.key,
                                              self.target), "generated", "ctrport_build_id.h"), version, self.source_root)
        except BuildFailed as e:
            raise BuildFailed(str(e), self.log_path)
        arts = self.collect()
        record = {"game": self.product.key, "target": self.target, "version": version, "build_id": bid,
                  "chapters": getattr(self, 'chapter_record', None),
                  "home_menu_art": art,
                  "toolchain_packages": dict(self.tc.package_versions),
                  "compiler": compiler, "release_source": self.release_source,
                  "seconds": round(time.monotonic() - t0, 1), "compiler_warnings": self.progress.warnings,
                  "aot_seconds": self.progress.aot_seconds, "log": self.log_path,
                  "graphics_seconds": self.progress.graphics_seconds,
                  "artifacts": [dataclasses.asdict(a) for a in arts]}
        with open(os.path.join(self.work, "out", "build-%s-%s.json" % (self.product.key, self.target)), "w",
                  encoding="utf-8") as f:
            json.dump(record, f, indent=2)
        return arts, record

    def collect(self):
        bdir = os.path.join(self.work, "src", "build", "3ds-%s-%s" % (self.product.key, self.target))
        stem = products.package_stem(self.product, self.target, self.source_root)
        cache = os.path.join(bdir, "CMakeCache.txt")
        try:
            with open(cache, encoding="utf-8", errors="replace") as f:
                if not re.search(r"^CTRPORT_AOT_DIR:PATH=.+$", f.read(), re.M):
                    raise BuildFailed("The build finished without AOT code.\n" + FAILED, self.log_path)
        except OSError:
            raise BuildFailed("Missing CMakeCache.txt.\n" + FAILED, self.log_path)
        out_dir = os.path.join(self.work, "out", self.target)
        os.makedirs(out_dir, exist_ok=True)
        arts = []
        for kind in self.outputs:
            p = os.path.join(bdir, stem + "." + kind)
            if not os.path.isfile(p) or os.path.getsize(p) == 0:
                if kind == "cia":
                    raise BuildFailed("The .cia was not made.\n"
                                      "Untick .cia on Options or set up the .cia tools.", self.log_path)
                raise BuildFailed("Missing output %s.\n" % os.path.basename(p) + FAILED, self.log_path)
            dst = os.path.join(out_dir, os.path.basename(p))
            shutil.copy2(p, dst)
            arts.append(Artifact(self.target, kind, dst, os.path.getsize(dst), sha256_file(dst)))
        return arts
