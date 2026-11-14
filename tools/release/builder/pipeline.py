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
import stat
import subprocess
import sys
import threading
import tempfile
import time

import products
import cleanup
import toolchain
import library_pins
import home_art
import sfx_encoder
import path_hygiene
from processes import hidden_options

CIA_NOT_MADE = "The .cia was not made.\nUntick .cia on Options or set up the .cia tools."


def available_outputs(artifacts):
    """Instructions describe the formats actually delivered, excluding optional data packs."""
    return [kind for kind in ('3dsx', 'cia') if any(a.kind == kind for a in artifacts)]


HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))     # repo root (or the export's root)
# What build.sh and CMake read, relative to the source root.
SOURCE_DIRS = ("engine/src", "engine/vendor", "engine/compat", "platform/3ds", "tools/aot", "tools/buildcache", "tools/deps/openh264",
               "tools/texprep", "tools/roomplan", "tools/audio", "tools/release/licenses", "tools/analysis")
SOURCE_FILES = ("engine/LICENSE", "VERSION", "LICENSE", "THIRD_PARTY_NOTICES",
                "docs/release/build-contract.json", "docs/release/builder-version-receipts.md", "tools/release/prepare_legal.py",
                # tools/aot/credits.py reads this port's credits for the options menu's Credits page (build.sh runs it).
                "docs/release/credits-port.txt", "tools/release/sfxpack-versions.json",
                # CMake's renderer launcher imports both helpers, including on builds that delegate unchanged.
                "tools/analysis/vine_old_observer_compile.py", "tools/analysis/vine_old_parent.py",
                "tools/analysis/vine_extract.py",
                # layout S3's scene table builder and its names-only spec (build.sh, UNDERTALE only: source_wanted).
                "tools/layout/build_scenes.py", "tools/layout/undertale-scenes.json")
SKIP_DIRS = {"__pycache__", ".git", "build", "scratch"}
SKIP_SUFFIXES = (".pyc", ".win", ".ogg", ".cia", ".3dsx", ".elf", ".o", ".a", ".dat")


WORK_BUSY = ("Another copy of the builder is already running with the work folder %s.\n"
             "Use that one, or close it and start the builder again.")
WORK_NO_LOCK = ("The work folder %s can't lock files, so another builder could change it during a build.\n"
                "Use a work folder on this PC's own drive (--work), then start the builder again.")
WORK_LOCK_ERROR = ("Can't use the work folder %s (%s).\n"
                   "Check that it can be written, or choose another with --work, then start the builder again.")
# The window's own wording. The window can't choose its work folder, and moving the folder changes nothing (the
# next launch works out the same path), so it names the folder and points to --work on a command line (R175b-text).
WINDOW_NO_LOCK = ("The builder keeps its work in %s, and that folder's drive can't lock files, so the builder "
                  "won't start there.\nThis window can't choose another work folder. To use one on this PC's own "
                  "drive, start the builder from a command line with --work and that folder.")
WINDOW_LOCK_ERROR = ("The builder can't use its work folder %s (%s).\nClose any other builder, check that the folder "
                     "isn't read-only and its drive isn't full, then open the builder again.")
WORK_LEASE = ".builder-session.lock"


class WorkLockError(RuntimeError):
    """The builder can't hold its work folder, so it must not start: Busy, no locks, or the lock can't be made
    (R175 F1/F3). There is no build without the lease. str() is the command line's text, window the window's."""

    def __init__(self, message, window=None):
        super().__init__(message)
        self.window = window or message


class WorkBusy(WorkLockError):
    pass


def require_launcher_paths(work, source_root=None, devkitpro=None):
    """CMake splits compiler-launcher paths at semicolons on every host."""
    if sys.platform == 'win32' and any(c in value for value in (os.path.abspath(work), os.path.realpath(work))
                                     for c in "'[],"):
        raise WorkLockError("The work folder path contains an apostrophe, bracket or comma, which the Windows "
                            "build tools can't use. Start the builder from a command line with --work and a "
                            "short, plain folder path, for example --work C:/u3ds.")
    for label, path in (("builder", SOURCE_ROOT if source_root is None else source_root),
                        ("Python", toolchain.python_exe()), ("work", work),
                        ("devkitPro", devkitpro or toolchain.candidate_roots()[0])):
        # Check both the spelling passed to the build and the interpreter's resolved
        # location: a harmless-looking link can point into a semicolon-named folder.
        if any(';' in value for value in (os.fspath(path), os.path.realpath(path))):
            if label == 'work':
                message = ("The work folder path contains a semicolon (;). Start the builder from a command line "
                           "with --work and a folder whose path has no semicolon.")
            elif label == 'devkitPro':
                message = ("The devkitPro folder path contains a semicolon (;). "
                           "Use devkitPro in a folder whose path has no semicolon, then open the builder again.")
            else:
                message = ("The builder's %s path contains a semicolon (;). " %
                           ('folder' if label == 'builder' else 'Python') +
                           "Move the builder to a folder whose path has no semicolon, then open it again.")
                if label == 'Python':
                    message += " If you use a separate Python, its folder path must have no semicolon either."
            raise WorkLockError(message)


def claim_work(work, devkitpro=None):
    """One builder process per work folder (AUD-15): builds mirror and delete src/build, and delivery and
    cleanup rewrite the rest. The OS lock goes when the process ends, so a crashed builder never blocks the next.
    Returns the held lease, or raises WorkLockError: WorkBusy for another builder, otherwise the folder or its lock
    file can't be used, and the builder stops rather than run unlocked (R175 F3)."""
    require_launcher_paths(work, devkitpro=devkitpro)
    try:
        cleanup.require_work_paths(work)
    except (ValueError, OSError) as exc:
        raise WorkLockError(str(exc)) from exc
    import sd_transaction
    try:
        os.makedirs(work, exist_ok=True)
        lease = sd_transaction.Lease(os.path.join(work, WORK_LEASE))
        lease.acquire()
    except sd_transaction.Busy:
        raise WorkBusy(WORK_BUSY % work) from None
    except sd_transaction.NoLock:
        raise WorkLockError(WORK_NO_LOCK % work, WINDOW_NO_LOCK % work) from None
    except OSError as error:
        reason = error.strerror or type(error).__name__
        raise WorkLockError(WORK_LOCK_ERROR % (work, reason), WINDOW_LOCK_ERROR % (work, reason)) from None
    return lease


def default_work_dir(env=None, platform=sys.platform):
    """A short folder without spaces (CMake object paths come close to Windows' 250-character limit)."""
    env = os.environ if env is None else env
    if platform == "win32":
        base = env.get("LOCALAPPDATA", "")
        cand = os.path.join(base, "u3ds", "w") if base else ""
        if cand and not any(c in cand for c in " %;'[],\t\r\n") and cand.isascii() and len(cand) <= 60:
            return cand
        return "C:/u3ds"
    candidate = (os.path.join(os.path.expanduser("~/Library/Caches"), "u3ds") if platform == 'darwin'
                 else os.path.join(env.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"), "u3ds"))
    if candidate.isascii() and not any(c in candidate for c in ' %;\t\r\n'):
        return candidate
    user = hashlib.sha256(os.path.expanduser('~').encode('utf-8')).hexdigest()[:12]
    return '/tmp/u3ds-' + user


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
    if relative.replace('\\', '/') in {
            'platform/3ds/source/' + name for name in (
                'z_bottomframe.c', 'z_bottomframe_core.c', 'z_bottomframe_assets.c',
                'z_bottomframe_settings.c', 'bottomframe_core.h', 'bottomframe_assets.h', 'ctr_bottomframe.h')}:
        return False  # Retired client: excluded from every target by CMake.
    if path_hygiene.is_junk(relative):
        return False
    if relative.replace("\\", "/") == "tools/buildcache/test_aacslot_compile.py":
        return False  # Host regression uses the private linked-ELF proof helper.
    if relative.replace("\\", "/") == "tools/buildcache/test_portmenu_compile.py":
        return False  # Host regression loads a private experiment helper; production adapter still ships.
    normalized = relative.replace("\\", "/")
    mexdlg_fixtures = 'tools/buildcache/fixtures/mexdlg'
    if (normalized == 'tools/buildcache/test_mexdlg_native_compile.py'
            or normalized == mexdlg_fixtures or normalized.startswith(mexdlg_fixtures + '/')):
        return False  # Checkout-only native-M regressions; the test needs Git history.
    if relative.replace("\\", "/") in (
            'tools/buildcache/mexdlg_compile.py', 'tools/buildcache/mexdlg_d12_compile.py',
            'tools/buildcache/test_mexdlg_d12_retired.py',
            'platform/3ds/mexdlg_d12_m.cmake'):
        return False  # Historical change measurement controls; no production build dependency.
    if relative.replace("\\", "/") == "tools/release/builder/undertale-3ds-builder.cmd":
        return False  # Packages have one game-specific launcher at their root.
    parts = relative.replace("\\", "/").split("/")
    game_only = {
        'platform/3ds/source/z_bootline_console.c': 'undertale',
        'platform/3ds/aacout.cmake': 'undertale',
        'tools/buildcache/aacout_compile.py': 'undertale',
        'tools/buildcache/test_aacout_compile.py': 'undertale',
        'tools/buildcache/aacslot_compile.py': 'undertale',
        'platform/3ds/source/ctr_aac_debug_bits.c': 'deltarune',
        'platform/3ds/source/ctr_audio_decoder_aac.inc': 'deltarune',
        # dlgtop's compiler adapter and retained slots are selected only by UT CMake.
        'tools/buildcache/loadgame_compile.py': 'undertale',
        'tools/buildcache/loadgame-order.json': 'undertale',
        'tools/buildcache/test_loadgame_compile.py': 'undertale',
        'tools/release/sfxpack-versions.json': 'undertale',
        'tools/release/music-versions.json': 'undertale',
        'tools/release/manifest-ut.txt': 'undertale',
        'tools/release/builder/chapters.py': 'deltarune',
        'tools/release/builder/audio-deltarune.json': 'deltarune',
        'engine/src/runner_tile_plan.inc': 'undertale',
    }
    if game is not None and '/'.join(parts) in game_only and game_only['/'.join(parts)] != game:
        return False
    if any(p in SKIP_DIRS or p == ".github" for p in parts) or not _wanted(parts[-1]):
        return False
    if parts[:2] == ["engine", "src"]:
        if len(parts) == 3:
            if parts[-1].startswith('utold_tile_') and game == 'deltarune':
                return False
            # simplefix's board-grid front (CMake compiles it for DELTARUNE New only; Policy E).
            if parts[-1] == "z_boardgrid.c":
                return game != "undertale"
            # depthmove's layer element deques (CMake compiles it for DELTARUNE only).
            if parts[-1] == "z_depthmove.c":
                return game != "undertale"
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
            # The other game's lists: <game>*, typed-<game>.txt, typed-<game>-<target>.txt and
            # typed-priority-<game>-<target>.txt.
            other = "deltarune" if game == "undertale" else "undertale"
            return game is None or not (parts[-1].startswith((other, "typed-priority-" + other + "-", "typed-" + other + "-")) or
                                        parts[-1] == "typed-" + other + ".txt")
        # loadscreen_art.py: the loading screen art (build.sh runs it for both games). escape.py: gmlaot.py's
        # escape elision (G9), imported for WAD 17 only but part of gmlaot.py's import closure. stpack.py and
        # stglyphs.py: the Crisp text glyph packs (build.sh makes them for UNDERTALE and DELTARUNE New by default);
        # stmenu.py: UNDERTALE's Smoothing menu data beside them (souls, sharp sprites, previews). symverdict.py: symmetric halving's table,
        # which fontverdict.py's build mode writes for UNDERTALE and DELTARUNE's New 3DS builds (DELTARUNE's builder
        # makes both models). itemverdict.py: Smart smoothing's item verdicts, which it writes for both games. credits.py:
        # the Credits page text (build.sh runs it for both games). typed_priority.py: the typed regions' saving order,
        # which gmlaot.py imports for a typed-priority list (part of its import closure for both games).
        return len(parts) == 3 and (parts[-1] in (
            "builtin_audit.py", "btnprompt.py", "corpus.py", "data_versions.py", "escape.py", "fontverdict.py",
            "gmlaot.py", "itemverdict.py", "inkart.py", "prepare.py", "padrefs.py", "aot_select.py", "typed.py", "typed_priority.py",
            "loadscreen_art.py", "late_numeric.py", "private_game.py", "aacslot.py", "aacslot.json",
            "symverdict.py", "stglyphs.py", "stpack.py", "stasrtext.py", "credits.py", "weirdshd.py")
            # dskip.py: UNDERTALE Old's covered-star key (build.sh runs it for UNDERTALE only).
            or (parts[-1] in ("pixgrid.py", "btnprompt_build.py", "stmenu.py", "dskip.py") and game != "deltarune")
            or (parts[-1] in ("gog_smart.py", "gog-smart-1.08.json") and game != "deltarune")
            # offkill.py: the early destroy's entries (build.sh runs it; UNDERTALE only).
            or (parts[-1] == "offkill.py" and game != "deltarune")
            # hitcheck_guard.py: reject UT data that calls the trimmed region builtins.
            or (parts[-1] == "hitcheck_guard.py" and game != "deltarune")
            # thinrule.py: Smart smoothing's whole-rows list, which fontverdict.py's build mode writes for UNDERTALE only.
            or (parts[-1] == "thinrule.py" and game != "deltarune")
            # textcase.py: the dialogue script's case table (build.sh runs it; UNDERTALE Old only).
            or (parts[-1] == "textcase.py" and game != "deltarune")
            or (parts[-1] == "chapter_manifest.py" and game != "undertale")
            # check_dspe_gate.py: knightgpu dspe's room gate check (build.sh runs it; DELTARUNE New only).
            or (parts[-1] == "check_dspe_gate.py" and game != "undertale"))
    if parts[:2] == ["tools", "texprep"] and parts[-1].startswith("test_"):
        return False  # Host-only regression tests use private pytest/compiler helpers.
    if parts[:2] == ["tools", "analysis"]:
        # The renderer launcher's helpers (SOURCE_FILES too), and UNDERTALE's half-resolution art packer with its list
        # of sprite name hashes (build.sh halfart, products.HALFART).
        # flowervramg_palette_retry.py: vine_old_parent.py imports it, and CMake makes ctr_renderer.c depend on it.
        if parts[2:] in (["vine_extract.py"], ["vine_old_observer_compile.py"], ["vine_old_parent.py"],
                         ["flowervramg_palette_retry.py"], ["utwindow_compile.py"],
                         ["cheaplevers_kick_compile.py"]):  # the GPU kick's launcher (DELTARUNE New CMake)
            return True
        return game != "deltarune" and parts[2:] in (["halfart_pack.py"], ["halfart", "undertale.txt"])
    if parts[:3] == ["tools", "deps", "openh264"]:
        # DELTARUNE's intro video decoder; build.sh never builds it for UNDERTALE.
        return game != "undertale"
    if parts[:2] == ['tools', 'audio']:
        # The stage's optional ADPCM staging driver is shared by both games.
        return len(parts) == 3 and (parts[-1] in (
            'stage.py', 'prepare.py', 'mono_sfx.py', 'convert.py', 'dspadpcm.py', 'dspadpcm.c', 'FORMAT.md')
            or parts[-1] == 'sfxpack.py' and game != 'deltarune')
    if parts[:2] == ["tools", "layout"]:
        # layout S3's scene table: build.sh resolves the names-only spec against the player's data.win (UNDERTALE only).
        return game != "deltarune" and parts[2:] in (["build_scenes.py"], ["undertale-scenes.json"])
    if parts[:2] == ["tools", "roomplan"]:
        if parts[2:] == ['build_sfxplan.py']:
            return game != 'deltarune'
        # voice_census.py: build_sfxplan.py imports it (the plan's text voices; change papvoice).
        if parts[2:] == ['voice_census.py']:
            return game != 'deltarune'
        # The room plan's build step (build.sh runs build_plan.py) and this game's committed records.
        # codeclose.py: build_plan.py imports it (UNDERTALE's in-play code closure; change codeclose).
        # records/sounds/: the sound plan records (build_sfxplan.py, UNDERTALE).
        if parts[2] == "records" and (len(parts) == 4 or (len(parts) == 5 and parts[3] == "sounds")):
            return game is None or parts[-1].startswith(game + "-")
        # phases/: the phase plan's records (build_phases.py, change utflowey).
        if parts[2] == "phases" and len(parts) == 4:
            return game is None or parts[-1].startswith(game + "-")
        # New DELTARUNE builds derive tracks.dtp from these reviewed records.
        if parts[2] == "tracks" and len(parts) == 4:
            return parts[-1] == "deltarune.json" and game != "undertale"
        if len(parts) == 3 and parts[-1] in ("build_tracks.py", "build_phases.py"):
            return game != "undertale"
        # build_flowcode.py: build.sh's flowcode step (flowfull's chapter-5 code plan, DELTARUNE New, default off).
        if len(parts) == 3 and parts[-1] == "build_flowcode.py":
            return game != "undertale"
        # sharebuf: build.sh's shared-surface list (change titancpu, DELTARUNE New) and its reviewed record.
        if len(parts) == 3 and parts[-1] == "build_sharebuf.py":
            return game != "undertale"
        if parts[2] == "sharebuf" and len(parts) == 4:
            return parts[-1] == "deltarune.json" and game != "undertale"
        return len(parts) == 3 and parts[-1] in ("build_phases.py", "build_plan.py", "build_sfxplan.py", "closure.py", "codeclose.py", "dwinfo.py",
                                                 "records.py",
                                                 "source_plan.py")
    if parts[:2] == ["platform", "3ds"]:
        if parts[2:4] == ['vendor', 'pvaac'] and game == 'undertale':
            return False
        if game == 'undertale' and (parts[2:] in (
                ['source', 'z_builderx_new.c'], ['source', 'z_builderx_chapters.c'], ['source', 'z_phaseplan.c'])
                or len(parts) == 4 and parts[2] == 'hotpin' and parts[-1].startswith('deltarune-')):
            return False
        if game == 'deltarune' and (parts[-1].startswith('utold_tile_') or parts[-1] == 'z_utold_tile_assets.c'):
            return False
        if parts == ["platform", "3ds", "source", "z_builderx_old.c"] and game == "undertale":
            return False
        if parts[2:] == ["source", "z_gogut.c"]:
            return game != "deltarune"
        # pacealign's bench window edges (CMake compiles it for UNDERTALE only).
        if parts[2:] == ["source", "z_pacealign.c"]:
            return game != "deltarune"
        # knightnew's afterimage history (build.sh knightnew-history, DELTARUNE New only; Policy E).
        if parts[2:] == ["source", "z_knightnew_history.c"]:
            return game != "undertale"
        # freebytes G3's collision rules (CMake builds it in place of collision_gm14.c for DELTARUNE only; Policy E).
        if parts[2:] == ["source", "collision_gm14_dr.c"]:
            return game != "undertale"
        # credfix's dodge list front (CMake compiles it for UNDERTALE Old only; Policy E).
        if parts[2:] == ["source", "z_dodgefront.c"]:
            return game != "deltarune"
        # simplefix's centred-text fronts (CMake compiles it for UNDERTALE Old only; Policy E).
        if parts[2:] == ["source", "z_textfront.c"]:
            return game != "deltarune"
        # freebies' dialogue writer front (CMake compiles it for UNDERTALE Old only; Policy E).
        if parts[2:] == ["source", "z_writerfront.c"]:
            return game != "deltarune"
        # asrfight's SAVE Asriel trail front (CMake compiles it for UNDERTALE Old writerfront builds only; Policy E).
        if parts[2:] == ["source", "z_trailfront.c"]:
            return game != "deltarune"
        # asrfight's item-name fronts (CMake compiles it for UNDERTALE Old writerfront builds only; Policy E).
        if parts[2:] == ["source", "z_itemfront.c"]:
            return game != "deltarune"
        # codeclose's dialogue case jump (CMake compiles it for UNDERTALE Old only; Policy E).
        if parts[2:] == ["source", "z_textcase.c"]:
            return game != "deltarune"
        # codeclose's first-Draw wrapper over writerfront (CMake compiles it for UNDERTALE Old writerfront builds only).
        if parts[2:] == ["source", "z_writerfirst.c"]:
            return game != "deltarune"
        # credfix's exit trail (build.sh exittrail): an o2DS panic diagnosis build, never a release.
        if parts == ["platform", "3ds", "source", "z_exittrail.c"]:
            return False
        # The ballast test build (build.sh ballast, dsplan B10.2): never a release, so never in a player's kit.
        if parts == ["platform", "3ds", "source", "z_ballast.c"]:
            return False
        # Half-resolution art (build.sh halfart, products.HALFART): UNDERTALE's Old 3DS build only. Its draw-scale
        # census (build.sh halfcensus) is a developer test.
        if parts == ["platform", "3ds", "source", "ctr_halfart.c"] and game == "deltarune":
            return False
        if parts == ["platform", "3ds", "source", "ctr_halfcensus.c"]:
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


def source_directories(game):
    """Whole source folders belonging to this game, including curated exports."""
    omitted = {'tools/deps/openh264'} if game == 'undertale' else set()
    return tuple(path for path in SOURCE_DIRS if path not in omitted)


def source_input(root, relative, game=None):
    """Select UT's scoped notices in the monorepo; curated kits already use that root file."""
    path = Path(root) / relative
    if relative == 'THIRD_PARTY_NOTICES' and game == 'undertale':
        scoped = Path(root) / 'tools/release/templates/THIRD_PARTY_NOTICES-undertale.txt'
        if scoped.is_file():
            path = scoped
    if path.is_symlink():
        raise ValueError('Source contains a symlink: ' + relative)
    return path


def mirror_source(src_root, dest_root, game=None, *, source_digest=None):
    """Mirror by contents, never archive timestamps. Changed inputs invalidate generated build state;
    an unchanged kit (including a second model) keeps its objects. Returns (copied, removed)."""
    cleanup.require_plain_tree(dest_root)
    copied = removed = 0
    os.makedirs(dest_root, exist_ok=True)
    invalidated = False
    # Build.prepare supplies the digest only after validating the complete kit.
    # HOME art is regenerated after every mirror, overwriting these five inputs
    # and adding its report. Those changes alone are not a kit update.
    identity = dict(schema=1, game=game, source_sha256=source_digest)
    stamp = os.path.join(dest_root, '.builder-source.json')
    unchanged = False
    if source_digest is not None:
        try:
            with open(stamp, encoding='utf-8') as stream:
                unchanged = json.loads(stream.read(4096)) == identity
        except (OSError, ValueError, TypeError, RecursionError):
            pass
    generated = {'platform/3ds/assets/banner.wav', 'platform/3ds/assets/home-art.json'}
    generated.update('platform/3ds/assets/%s-%s-%s.png' % (kind, game, target)
                     for kind in ('banner', 'icon') for target in ('n3ds', 'o3ds'))
    if game not in ('undertale', 'deltarune'):
        generated.clear()

    def invalidate():
        nonlocal invalidated
        if not invalidated:
            build = os.path.join(dest_root, 'build')
            if os.path.lexists(build):
                shutil.rmtree(build)
            invalidated = True

    def changed_input(path):
        relative = os.path.relpath(path, dest_root).replace('\\', '/')
        if not (unchanged and relative in generated):
            invalidate()

    # A changed kit (even with identical archive timestamps) or missing/corrupt
    # stamp always starts clean, before the first source mutation.
    if source_digest is not None and not unchanged:
        invalidate()

    def copy_changed(s, d):
        nonlocal copied
        try:
            if os.path.getsize(s) == os.path.getsize(d) and sha256_file(s) == sha256_file(d):
                return
        except FileNotFoundError:
            pass
        # Clear before the first mutation: interruption must not leave a mirrored new source
        # beside old objects. copy2 preserves tar's epoch timestamp, which cannot trigger make.
        changed_input(d)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copy2(s, d)
        copied += 1

    for rel in SOURCE_DIRS:
        src = os.path.join(src_root, rel)
        dst = os.path.join(dest_root, rel)
        want = set()
        for dirpath, dirnames, filenames in os.walk(src):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS and not path_hygiene.is_junk(d)]
            rd = os.path.relpath(dirpath, src)
            for n in filenames:
                r = os.path.normpath(os.path.join(rd, n))
                if not source_wanted(os.path.join(rel, r), game):
                    continue
                want.add(os.path.normcase(r))
                s, d = os.path.join(src, r), os.path.join(dst, r)
                copy_changed(s, d)
        if os.path.isdir(dst):
            for dirpath, dirnames, filenames in os.walk(dst):
                dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
                rd = os.path.relpath(dirpath, dst)
                for n in filenames:
                    r = os.path.normpath(os.path.join(rd, n))
                    if os.path.normcase(r) not in want and (_wanted(n) or path_hygiene.is_junk(r)):
                        changed_input(os.path.join(dst, r))
                        os.remove(os.path.join(dst, r))
                        removed += 1
    for rel in SOURCE_FILES:
        if not source_wanted(rel, game):
            continue
        s = source_input(src_root, rel, game)
        if os.path.isfile(s):
            d = os.path.join(dest_root, rel)
            copy_changed(s, d)
    if source_digest is not None:
        # Commit the identity only after every copy/removal succeeds.
        with open(stamp + '.part', 'w', encoding='utf-8', newline='\n') as stream:
            json.dump(identity, stream, sort_keys=True)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(stamp + '.part', stamp)
    return copied, removed


def source_payload(root, game):
    """The kit's build inputs, including its builder; excludes launchers, host binaries and the descriptor."""
    root = Path(root)
    paths = {rel for rel in SOURCE_FILES if source_wanted(rel, game)}
    for directory in SOURCE_DIRS + ('tools/release/builder',):
        base = root / directory
        for p in path_hygiene.iter_paths(base):
            if p.is_symlink():
                raise ValueError('Source contains a symlink: ' + p.relative_to(root).as_posix())
            rel = p.relative_to(root).as_posix()
            if p.is_file() and source_wanted(rel, game) and not rel.startswith(
                    ('tools/release/builder/runtime/', 'tools/release/builder/bin/')):
                paths.add(rel)
    # These modules participate in the build but live outside the mirrored source folders.
    paths.update(('tools/release/release_contract.py', 'tools/release/verify_game.py',
                  'tools/release/music-versions.json'))
    paths = {rel for rel in paths if source_wanted(rel, game)}
    for rel in paths:
        if (root / rel).is_symlink():
            raise ValueError('Source contains a symlink: ' + rel)
    return {rel: source_input(root, rel, game).read_bytes() for rel in sorted(paths)}


NO_SOURCE_LINK = ("This copy of the source has no release-descriptor.json and no Git history, so the builder can't "
                  "name the source it builds. Clone it with git (git clone %s), or use the release builder.")
# A source build never claims to match its commit (R198a3): files git ignores or is told not to check are built too,
# so "based on" is the only claim the builder can always stand behind. "with local changes" when git reports any.
SOURCE_BUILD = ("Source build: this checkout has no release-descriptor.json, so it builds as an unofficial copy based "
                "on commit %s%s. Its Legal page shows the release tag; the build log links to that base commit: %s.")
# Git history whose HEAD can't be read stops at once (R201): the release line builds only with
# {repository}/tree/{commit}, which keeps the release layout guards, and a reused work folder would keep the last
# commit's link in CMake's cache.
NO_SOURCE_COMMIT = ("This checkout has no release-descriptor.json and the builder could not read which commit it is "
                    "on, so it can't name the source it builds. Clone it again with git (git clone %s), or use the "
                    "release builder.")
SOURCE_CHANGED = ", with local changes"


def ref_name_ok(ref):
    """Git's own ref-name rules (git check-ref-format), so any branch git allows is read, and nothing that could
    leave the refs folder: no '..', empty or dot-led component, '.lock' ending, control character, space,
    ~^:?*[, backslash or '@{'. Non-ASCII names are allowed, as in git."""
    if not ref.startswith('refs/') or ref.endswith(('/', '.')) or '..' in ref or '@{' in ref:
        return False
    if any(ord(c) < 0x20 or c == '\x7f' or c in ' ~^:?*[\\' for c in ref):
        return False
    return all(part and not part.startswith('.') and not part.endswith('.lock') for part in ref.split('/'))


def _git_text(path):
    """A file git wrote, as UTF-8 with only git's own ASCII whitespace trimmed: a branch name may end in a non-ASCII
    space (U+00A0, U+0085, U+2003), which str.strip() would cut off (R198a3)."""
    return path.read_bytes().decode('utf-8').strip(' \t\r\n')


def checkout_commit(root):
    """HEAD's commit ID when `root` is the top of a Git checkout, read from .git without running git; else None
    (no history, or a HEAD this can't read: source_build still builds then, with the commit unknown)."""
    git = Path(root) / '.git'
    try:
        if git.is_file():       # A linked worktree or submodule: "gitdir: <path>".
            line = _git_text(git)
            if not line.startswith('gitdir:'):
                return None
            git = (Path(root) / line[7:].strip(' \t')).resolve()
        common = git
        if (git / 'commondir').is_file():
            common = (git / _git_text(git / 'commondir')).resolve()
        head = _git_text(git / 'HEAD')
        if head.startswith('ref:'):
            ref = head[4:].strip(' \t')
            if not ref_name_ok(ref):
                return None
            loose = common / ref
            try:
                is_loose = loose.is_file()
            except OSError:         # a name git allows but this file system can't hold: git packs it
                is_loose = False
            if is_loose:
                head = _git_text(loose)
            else:
                head = None
                packed = common / 'packed-refs'
                if packed.is_file():
                    # Rows end in LF only; U+0085, U+2028 and U+2029 can be inside a name (not str.splitlines()).
                    # Compared as bytes, so another row git wrote in a different encoding can't stop the lookup.
                    want = ref.encode('utf-8')
                    for row in packed.read_bytes().split(b'\n'):
                        parts = row.rstrip(b'\r').split(b' ')
                        if len(parts) == 2 and parts[1] == want:
                            head = parts[0].decode('ascii', 'replace')
                            break
    except (OSError, ValueError):       # UnicodeDecodeError is a ValueError
        return None
    if head and re.fullmatch(r'[0-9a-f]{40}', head) and head != '0' * 40:
        return head
    return None


def local_changes(root):
    """True or False when git can tell whether the checkout differs from HEAD; None when it can't (no git).
    Untracked files count: the build takes every file in its source folders, added ones too (R198a)."""
    git = shutil.which('git')
    if not git:
        return None
    try:
        result = subprocess.run([git, '-C', str(root), 'status', '--porcelain', '--untracked-files=all'],
                                stdin=subprocess.DEVNULL, capture_output=True, timeout=30, **hidden_options())
    except (OSError, subprocess.SubprocessError):
        return None
    return bool(result.stdout.strip()) if result.returncode == 0 else None


def source_build(root, game):
    """A source checkout has no release descriptor: describe it from the same release contract a kit's comes from,
    with the checkout's own commit, so it builds with the release profile and is labelled a source build."""
    contract = products.release_contract
    commit = checkout_commit(root)
    if commit is None:
        text = NO_SOURCE_COMMIT if os.path.lexists(os.path.join(str(root), '.git')) else NO_SOURCE_LINK
        raise BuildFailed(text % contract.source_descriptor(game, '', '')['repository'])
    try:
        version = release_version(root)
        digest = contract.payload_digest(source_payload(root, game))
        descriptor = contract.source_descriptor(game, version, digest)
        descriptor['public_commit'] = commit
        result = contract.validate_source(descriptor, game, version, digest)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise BuildFailed('Could not read this source checkout: ' + str(exc)) from exc
    # What git reported, not a claim that the build matches the commit: git leaves out ignored and assume-unchanged
    # files, which the build still takes.
    result.update(kind='source', git_reports_changes=local_changes(root))
    return result


def smdh_source(source):
    """What a source build's HOME Menu publisher line names (R204): the commit it is based on, "-dirty" when git
    reports changes, as platform/3ds/smdh.cmake reads CTRPORT_SMDH_SOURCE. The mirror has no .git, so without this
    the line would show the release version. None for a kit's release build."""
    if source.get('kind') != 'source':
        return None
    return source['public_commit'][:7] + ('-dirty' if source.get('git_reports_changes') else '')


def source_build_text(source):
    """The one line that tells a player a build is a source build (CLI, dry run, build log and record)."""
    changed = SOURCE_CHANGED if source.get('git_reports_changes') else ''
    return SOURCE_BUILD % (source['public_commit'][:12], changed, source['source_url'])


def release_source(root, game):
    """The release descriptor's verified source link. A kit carries the descriptor; a missing one means a source
    checkout, which builds as a labelled source build (source_build). A descriptor that is present but unset or
    doesn't match still stops the build."""
    contract = products.release_contract
    path = Path(root) / contract.DESCRIPTOR
    if not os.path.lexists(path):
        return source_build(root, game)
    try:
        descriptor = json.loads(path.read_text(encoding='utf-8'))
        result = contract.validate_source(descriptor, game, release_version(root),
                                          contract.payload_digest(source_payload(root, game)))
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise BuildFailed('Could not verify this builder\'s public source: ' + str(exc)) from exc
    result['kind'] = 'release'
    return result


def write_python_shims(bin_dir, python):
    """python3 and python for build.sh, pointing at the builder's interpreter (the embeddable package on a user's
    PC), so the build finds Python exactly as on a developer machine."""
    os.makedirs(bin_dir, exist_ok=True)
    py = python.replace("\\", "/")
    if re.match(r"^[A-Za-z]:/", py):
        py = "/" + py[0].lower() + py[2:]
    for name in ("python3", "python"):
        with open(os.path.join(bin_dir, name), "w", encoding="utf-8", newline="\n") as f:
            f.write('#!/bin/sh\nexec %s -X utf8 "$@"\n' % shlex.quote(py))
        os.chmod(os.path.join(bin_dir, name), 0o755)
    # build.sh uses nproc, which macOS does not ship. Keep this local to the builder.
    with open(os.path.join(bin_dir, "nproc"), "w", encoding="utf-8", newline="\n") as f:
        f.write('#!/bin/sh\nexec %s -X utf8 -I -c "import os; print(os.cpu_count() or 1)"\n' % shlex.quote(py))
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
if [ -n "${CTRPORT_LIBRARY_SHIMS:-}" ]; then export PATH="$CTRPORT_LIBRARY_SHIMS:$PATH"; fi
exec bash platform/3ds/build.sh "$@"
'''

UNIX_BUILD_SCRIPT = r'''
set -euo pipefail
export DEVKITPRO="$4"
export DEVKITARM="$DEVKITPRO/devkitARM"
export PATH="$1:$2:$2/cmake/cmake-3.31.8-macos-universal/CMake.app/Contents/bin:$DEVKITARM/bin:$DEVKITPRO/tools/bin:/Applications/CMake.app/Contents/bin:/opt/homebrew/bin:/usr/local/bin:$PATH"
cd "$3"
shift 4
if [ -n "${CTRPORT_LIBRARY_SHIMS:-}" ]; then export PATH="$CTRPORT_LIBRARY_SHIMS:$PATH"; fi
exec bash platform/3ds/build.sh "$@"
'''


def build_argv(tc: toolchain.Toolchain, work, game, target, data_dir, openh264=False, platform=sys.platform, halfart=None):
    """halfart: None follows products.HALFART (UNDERTALE o3ds only either way)."""
    args = [game, target, "aot", "data=" + (data_dir.replace("\\", "/") if platform == "win32" else data_dir)]
    if not openh264:
        args.append("openh264=off")
    if products.halfart(game, target, halfart):
        args.append("halfart")
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
        self.activity = STAGES[0]
        # build.sh makes the .cia whenever makerom and bannertool are there. Without --cia nobody asked for one, so
        # that step shows as packaging (betasmoke B8).
        self.cia_wanted = True

    def feed(self, line):
        t = self.clock()
        s = line.rstrip("\r\n")
        if s.startswith("AOT: generating"):
            self.aot_start = t
        elif "code for" in s and "is up to date" in s and s.startswith("AOT:"):
            self.aot_up_to_date = True
        elif "WITHOUT AOT" in s or (s.startswith("AOT:") and "failed" in s.lower()
                                    and not s[4:].lstrip().startswith("skip ")):
            # gmlaot's per-function "AOT:   skip <name>: ... failed before generation" leaves that one function to
            # the interpreter (DELTARUNE chapter 5 has two); the build still has AOT.
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
                self.percent = max(self.percent, min(100, int(m.group(1))))
                if self.percent >= 100:
                    self.stage = 3
            elif "Building CIA" in s or s.startswith("built "):
                self.stage = 3
                if s.startswith("built "):
                    self.outputs.append(s[6:])
        if ": warning:" in s:
            self.warnings += 1
        self.activity = (('Making .cia' if self.cia_wanted else STAGES[3]) if 'Building CIA' in s else
                         'Making .3dsx' if 'Creating 3DSX' in s else
                         self.activity if self.stage == 3 and not s.startswith('built ') else STAGES[self.stage])

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
            # Traceback locations need their basename, line and function for support, never directories.
            frame = re.match(r'^(\s*File ")([^"\r\n]+)(", line \d+(?:, in [^\r\n]*)?)$', line)
            if frame:
                line = frame[1] + re.split(r'[\\/]', frame[2])[-1] + frame[3]
            if line.startswith("AOT:   ") or "gml_" in line or re.match(r'^\s*SKIP\s+\d+\s', line):
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
            line = re.sub(r"(?:(?<![\w:/\\])|(?<=-[ILF]))(?:[A-Za-z]:[\\/]|\\\\|/(?=[^\s/]))[^\r\n]*", "<path>", line)
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
    def __init__(self, message, log_path=None, details=''):
        super().__init__(message)
        self.log_path = log_path
        self.details = details


class GameFilesRefused(BuildFailed):
    """The game files changed after verification; return to file selection, not build retry."""


def game_files_problem(title, kind):
    """Player guidance shared by the window and CLI (cli_text translates the final action)."""
    messages = {
        'unknown': ("These %s files don't match an official release this builder knows. "
                    'Restore the complete original game folder, or try a newer builder.'),
        'unreadable': ('Could not read the %s game files. Check that the game folder is readable '
                       'and close anything that is using the files.'),
        'changed': ('The %s game files changed while they were being checked. '
                    'Wait for any update or file copy to finish, then try again.'),
        'missing': ('Could not find the %s game files. The game folder may have moved or been removed.'),
        'art': ('Could not prepare HOME Menu art for %s. Click Copy log to report the error.'),
    }
    message = messages[kind] % title
    return message + ' Go back to choose the game files.'


def game_files_refusal(log_path, title):
    """Recognize build_input's diagnostics; paths and raw errors remain in the support log."""
    try:
        with open(log_path, encoding='utf-8', errors='replace') as log:
            for line in log:
                text = line.strip()
                if text == 'Could not find the game files.':
                    return game_files_problem(title, 'missing')
                if text.startswith('Refusing to build: could not read '):
                    kind = 'changed' if text.endswith('(file changed while checking).') else 'unreadable'
                    return game_files_problem(title, kind)
                if (text.startswith('Refusing to build: ') and
                        ' is not an unmodified Steam or GOG copy of %s (SHA-256 ' % title in text):
                    return game_files_problem(title, 'unknown')
    except OSError:
        pass
    return None


FAILED = "The build stopped.\nClick Try again, or click Copy log to report the error."
# A failing step's own words, so the command line can say what failed (B2). Warnings are never the cause.
_ERROR_LINE = re.compile(r"(?:^|\s)(?:fatal )?error:|: error\b|overfilled|undefined reference|^ld: |"
                         r"^make(?:\[\d+\])?: \*\*\*|^CMake Error|^Traceback ", re.I)
# A diagnostic's severity is its first "warning:" or "error:" marker ("ld: warning: ...", "a.c:3:1: warning: ...").
# Only a warning by that first marker is skipped: "a.c:1:19: error: warning: ..." and "a.S:1: Error: warning: ..."
# are errors (R198c2), and a line naming something called warning is no warning at all.
_SEVERITY = re.compile(r"(?:^|:\s*)(warning|(?:fatal )?error):", re.I)


def _is_warning(text):
    m = _SEVERITY.search(text)
    return bool(m) and m.group(1).lower() == 'warning'


def first_error(log_path, limit=300):
    """The first line in a build log that reads as the failure (a compiler, linker, make or Python error), or
    None. Warning diagnostics are skipped (devkitARM's ld prints harmless ones in every build); an error that
    only names something called warning is kept."""
    try:
        with open(log_path, encoding='utf-8', errors='replace') as log:
            for line in log:
                text = line.strip()
                if text and not _is_warning(text) and _ERROR_LINE.search(text):
                    return text[:limit]
    except (OSError, TypeError):
        pass
    return None


@dataclasses.dataclass
class Artifact:
    target: str
    kind: str          # "3dsx" or "cia"
    path: str
    size: int
    sha256: str


SFX_PACK_MISSING = ("The Old 3DS sound files could not be prepared. The game will use its original sounds, "
                    "which may cause pauses in busy scenes on a 3DS, 3DS XL or 2DS. "
                    "Try the build again, or copy the log for support.")
SFX_NO_ENCODER = ("this copy of the builder has no Windows sound encoder (%s comes only with the release builder)"
                  % sfx_encoder.RELATIVE)


def sfx_pack_problem(source_root=SOURCE_ROOT, platform=None):
    """Why this PC can't make UNDERTALE's Old 3DS sound pack before building, or None (betasmoke R2 finding 1:
    a Windows source checkout has no encoder DLL, and the build used to say so only in its log)."""
    if (platform or sys.platform) == 'win32' and not (Path(source_root) / sfx_encoder.RELATIVE).is_file():
        return SFX_NO_ENCODER
    return None


def prepare_sfx_pack(job, data_dir):
    """Run the exported packer against the exact data.win selected for this build.

    Publish only after its digest-specific release-table check succeeds. The
    source game is read-only; encodes and the generated pack stay in work/.
    """
    out = Path(job.work) / 'out/data/adpcm-sfx.dtsp'
    out.parent.mkdir(parents=True, exist_ok=True)
    temporary = out.with_name(out.name + '.builder-part')
    audio = Path(job.work) / 'src/tools/audio'
    # Encodes made before a compiler-policy/source fix must not survive in the
    # object cache (for example M1 FMA encodes from before -ffp-contract=off).
    identity = hashlib.sha256(b'\0'.join((audio / name).read_bytes() for name in
        ('dspadpcm.c', 'dspadpcm.py', 'mono_sfx.py', 'sfxpack.py'))).hexdigest()
    argv = [toolchain.python_exe(), '-X', 'utf8', '-I', '-B',
            str(Path(job.work) / 'src/tools/audio/sfxpack.py'), '--data-win',
            str(Path(data_dir) / 'data.win'), '--out', str(temporary),
            '--work', str(Path(job.work) / 'sfx-cache' / identity)]
    if sys.platform == 'win32':
        library = Path(job.source_root) / sfx_encoder.RELATIVE
        sfx_encoder.validate(library, Path(job.work) / 'src')
        argv += ['--dsp-library', str(library)]
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
    options = hidden_options(group=True)
    if sys.platform != 'win32':
        options['start_new_session'] = True
    try:
        with open(job.log_path, 'a', encoding='utf-8', newline='\n') as log:
            log.write('builder: preparing UNDERTALE sound pack\n')
            with job._lock:
                if job.cancelled:
                    raise BuildFailed('The build was cancelled.', job.log_path)
                job.proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace',
                    env=env, **options)
            for line in job.proc.stdout:
                log.write(line)
                job.on_line(line.rstrip('\r\n'))
            code = job.proc.wait()
            if code or job.cancelled or not temporary.is_file() or not temporary.stat().st_size:
                log.write('builder: sfxpack exit code %d\n' % code)
                raise BuildFailed(FAILED, job.log_path)
        os.replace(temporary, out)
    except OSError as error:
        job.on_line('builder: sfxpack: ' + str(error))
        raise BuildFailed(FAILED, job.log_path) from error
    finally:
        temporary.unlink(missing_ok=True)
    return Artifact('shared', 'sfxpack', str(out), out.stat().st_size, sha256_file(out))


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


# Both-model compiler cache was about 82 MB; old versions can accumulate without reuse.
WORK_CACHE_CREDIT_MB = 100


def existing_work_bytes(work):
    """Credit reusable work, with bounded cache credit; never exported installs or links."""
    total, cache_total, seen = 0, 0, set()
    pending = [(Path(work) / name, name == 'ccache')
               for name in ('src', 'ccache', 'out/o3ds', 'out/n3ds')]
    while pending:
        path, is_cache = pending.pop()
        try:
            info = path.lstat()
            # A junction can point outside the work volume, just like a symlink.
            attributes = getattr(info, 'st_file_attributes', 0)
            if stat.S_ISLNK(info.st_mode) or attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
                continue
            if stat.S_ISDIR(info.st_mode):
                with os.scandir(path) as entries:
                    pending.extend((Path(entry.path), is_cache) for entry in entries)
            elif stat.S_ISREG(info.st_mode):
                identity = (info.st_dev, info.st_ino)
                if info.st_ino and identity in seen:
                    continue
                seen.add(identity)
                # Sparse/compressed files must not earn more credit than their allocation.
                if attributes & (stat.FILE_ATTRIBUTE_SPARSE_FILE | stat.FILE_ATTRIBUTE_COMPRESSED):
                    continue
                size = info.st_size
                if hasattr(info, 'st_blocks'):
                    size = min(size, info.st_blocks * 512)
                if is_cache:
                    cache_total += size
                else:
                    total += size
        except OSError:
            pass  # Unreadable/missing work earns no credit; do not reduce the space requirement.
    return total + min(cache_total, WORK_CACHE_CREDIT_MB * 10**6)


class Build:
    """One target. run() blocks; cancel() may be called from another thread."""

    def __init__(self, product, tc, work, target, data_dir, outputs=("3dsx",), openh264=False, on_line=None,
                 on_progress=None, popen=subprocess.Popen, source_root=SOURCE_ROOT, clock=time.monotonic,
                 art_generator=home_art.generate, store="steam", parts=None, sfx_builder=prepare_sfx_pack):
        if not str(work).isascii() or any(c in str(work) for c in '%\t\r\n'):
            raise BuildFailed('Choose a work folder with plain letters, numbers and spaces. Leave out % and control characters.')
        try:
            require_launcher_paths(work, source_root, getattr(tc, 'root', None))
            cleanup.require_work_paths(work)
        except (WorkLockError, ValueError, OSError) as exc:
            raise BuildFailed(str(exc)) from exc
        self.product, self.tc, self.work, self.target = product, tc, work, target
        try:
            products.require_output_outside_game(data_dir, work)
        except ValueError as exc:
            raise BuildFailed(str(exc)) from exc
        self.data_dir, self.outputs, self.openh264 = data_dir, tuple(outputs), openh264
        self.on_line = on_line or (lambda s: None)
        self.on_progress = on_progress or (lambda p: None)
        self.popen = popen
        self.source_root = source_root
        self.art_generator = art_generator
        self.sfx_builder = sfx_builder
        self.store = store
        self.parts = parts
        self.libraries = {'packages': [], 'shims': None}
        self.notices = []
        self.progress = Progress(product, target, clock)
        self.progress.cia_wanted = "cia" in self.outputs
        self.proc = None
        self.cleanup_roots = [os.path.join(work, name) for name in cleanup.WORK_AREAS['build']]
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

    def refuse_game_files(self, error):
        if self.product.key == 'undertale':
            import errno
            log_path = self.log_path
            try:
                os.makedirs(os.path.dirname(log_path), exist_ok=True)
                with open(log_path, 'w', encoding='utf-8') as log:
                    log.write('UNDERTALE staging: ' + str(error) + '\n')
            except OSError:
                log_path = None
            missing = isinstance(error, products.MissingGameData)
            try:
                source_error = (isinstance(error, OSError) and error.errno != errno.ENOSPC and
                                error.filename2 is None and error.filename is not None and
                                Path(error.filename).resolve().is_relative_to(Path(self.data_dir).resolve()))
            except (OSError, ValueError, TypeError):
                source_error = False
            if missing or source_error:
                kind = 'missing' if missing or isinstance(error, FileNotFoundError) else 'unreadable'
                raise GameFilesRefused(game_files_problem(self.product.title, kind), log_path) from error
            if isinstance(error, OSError):
                if error.errno == errno.ENOSPC:
                    message = 'Not enough space to prepare UNDERTALE. Free up space in the work folder and try again.'
                else:
                    message = ('Could not prepare UNDERTALE. Check that the game folder is readable '
                               'and that files can be saved in the work folder. '
                               'Free up space if needed. Click Try again, '
                               'or click Copy log to report the error.')
            else:
                message = str(error)
            raise BuildFailed(message, log_path) from error
        import chapters
        import errno
        # Validation can fail before log creation, including when the work folder itself is unwritable.
        log_path = self.log_path
        try:
            os.makedirs(os.path.dirname(log_path), exist_ok=True)
            with open(log_path, 'w', encoding='utf-8') as log:
                log.write('chapter validation: ' + str(error) + '\n')
        except OSError:
            log_path = None
        if isinstance(error, chapters.UnknownVersion):
            raise GameFilesRefused(game_files_problem(self.product.title, 'unknown'), log_path) from error
        if isinstance(error, chapters.GameFilesError):
            raise GameFilesRefused(str(error) + ' Go back to choose the game files.', log_path) from error
        if isinstance(error, OSError):
            if error.errno == errno.ENOSPC:
                message = 'Not enough space for the selected chapters. Free up space and try again.'
            elif isinstance(error, chapters.StageCopyError) or error.filename2 is not None:
                message = ('Could not copy the selected chapters. Check that the game folder is readable '
                           'and that the work folder has free space and allows these files. '
                           'Click Try again, or click Copy log to report the error.')
            else:
                try:
                    source_error = (error.filename is not None and
                                    Path(error.filename).resolve().is_relative_to(Path(self.data_dir).resolve()))
                except (OSError, ValueError, TypeError):
                    source_error = False
                if source_error:
                    raise GameFilesRefused(game_files_problem(self.product.title, 'unreadable'), log_path) from error
                message = ('Could not prepare the selected chapters in the work folder. '
                           'Check free space and folder permissions. '
                           'Click Try again, or click Copy log to report the error.')
        else:
            message = str(error)
        raise BuildFailed(message, log_path) from error

    def prepare(self):
        try:
            cleanup.require_work_paths(self.work)
        except (ValueError, OSError) as exc:
            raise BuildFailed(str(exc)) from exc
        if self.product.key == 'deltarune':
            import chapters
            try:
                self.parts = chapters.runtime_selection(self.data_dir, self.parts, self.target)
            except (OSError, ValueError) as exc:
                self.refuse_game_files(exc)
        version = release_version(self.source_root)      # an incomplete archive stops before anything is written
        self.release_source = release_source(self.source_root, self.product.key)
        if self.release_source.get('kind') == 'source':
            self.notices.append(source_build_text(self.release_source))
            self.on_line(self.notices[-1])
        if self.product.key == 'undertale':
            # Options describes total work, including reusable files already on disk.
            # Recheck remaining space for each model/rerun; keep 30 MB of working room.
            # Delivery copies have their own actual destination-space check.
            need = max(30 * 10**6, self.product.work_disk_mb([self.target]) * 10**6
                       - existing_work_bytes(self.work))
        else:
            need = int(self.product.disk_mb[self.target] * 1.5 + 30) * 1024 * 1024
        if self.product.key == 'deltarune':
            need += chapters.stage_bytes(self.data_dir, self.parts)
        os.makedirs(self.work, exist_ok=True)
        free = shutil.disk_usage(self.work).free
        if free < need:
            raise BuildFailed("%s has %d MB free; %d MB needed.\n"
                              "Free up disk space and try again." % (
                                  self.work, free // 10**6, -(-need // 10**6)))
        self._cleanup_snapshot = cleanup.begin_build(self.work, self.cleanup_roots)
        try:
            copied, removed = mirror_source(self.source_root, os.path.join(self.work, "src"), self.product.key,
                                            source_digest=self.release_source['source_sha256'])
            write_python_shims(os.path.join(self.work, "bin"), toolchain.python_exe())
            os.makedirs(os.path.join(self.work, "tools", "bin"), exist_ok=True)
            try:
                self.libraries = library_pins.select(self.tc, self.work, self.on_line)
            except (toolchain.Refused, OSError, ValueError) as exc:
                details = getattr(exc, 'details', '')
                try:
                    with open(self.log_path, 'a', encoding='utf-8', newline='\n') as log:
                        log.write(str(exc) + '\n' + details + '\n')
                except OSError:
                    pass  # A blocked/full log must not hide the setup failure.
                raise BuildFailed(str(exc), self.log_path, details) from exc
            return version, copied, removed
        finally:
            if not getattr(self, '_running', False):
                self.finish_ownership()

    def finish_ownership(self):
        snapshot = self.__dict__.pop('_cleanup_snapshot', None)
        if snapshot is not None:
            try:
                cleanup.finish_build(self.work, snapshot)
            except (OSError, ValueError) as exc:
                # An incomplete ownership record keeps files; it never authorizes deletion.
                self.on_line('Some work files could not be recorded for cleanup: ' + str(exc))

    def environment(self):
        """build.sh's environment, after prepare(): the release profile's source link and, for a source build, the
        commit the HOME Menu publisher line names (a kit's build never carries one, even from a stale shell). Only a
        kit's verified build asks smdh.cmake for the release version and publisher, and a stale shell can't ask."""
        env = dict(os.environ)
        env["CTRPORT_CCACHE_DIR"] = os.path.join(self.work, "ccache")
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        env["CTRPORT_PYTHON"] = toolchain.python_exe()
        env = products.release_contract.build_environment(
            self.product.key, self.release_source['public_commit'], env)
        # Player builds never run optional lab qualification, including when a
        # reused mirror still has an old ON CMake cache entry.
        env["CTRPORT_PLAYER_BUILD"] = "1"
        env.pop("CTRPORT_SMDH_SOURCE", None)
        env.pop("CTRPORT_SMDH_RELEASE", None)
        stamp = smdh_source(self.release_source)
        if stamp:
            env["CTRPORT_SMDH_SOURCE"] = stamp
        else:
            env["CTRPORT_SMDH_RELEASE"] = "1"
        env.pop("CTRPORT_FILE_PREFIX_MAP", None)
        env.pop('CTRPORT_LIBRARY_SHIMS', None)
        shims = getattr(self, 'libraries', {}).get('shims')
        if shims:
            env['CTRPORT_LIBRARY_SHIMS'] = library_pins.posix_path(shims, self.tc.platform)
        return env

    def run(self):
        try:
            return self._run()
        finally:
            self.finish_ownership()
            self._running = False

    def _run(self):
        release_version(self.source_root)
        compiler = toolchain.compiler_details(self.tc)
        tested = products.release_contract.read(self.source_root)['tested_compiler']
        if self.product.key == 'undertale':
            try:
                import updates
                toolchain.require_release_compiler(compiler, tested, self.tc.platform,
                                                   updates.repository(self.product.key, self.source_root) + '/releases')
            except toolchain.Refused as exc:
                raise BuildFailed(str(exc)) from exc
        if self.product.key == 'deltarune':
            import chapters
        t0 = time.monotonic()
        self._running = True
        version, copied, removed = self.prepare()
        # UNDERTALE's data.win/game.ios is staged under its tested AOT label (products.aot_data_dir).
        if self.product.key == 'deltarune':
            try:
                data_dir, self.parts = chapters.stage(self.data_dir, self.work, self.parts)
            except (OSError, ValueError) as exc:
                self.refuse_game_files(exc)
        else:
            try:
                data_dir = products.aot_data_dir(self.product, self.data_dir, os.path.join(self.work, "data"))
            except (OSError, ValueError) as exc:
                self.refuse_game_files(exc)
        argv = build_argv(self.tc, self.work, self.product.key, self.target, data_dir, self.openh264,
                          platform=self.tc.platform)
        env = self.environment()
        compiler['tested'] = tested
        compiler['layout_pins'] = (compiler['version'] == tested['version'] and
                                   compiler['target'] == tested['target'])
        compiler['warning'] = ('' if compiler['layout_pins'] else
            'Tested with devkitARM GCC %s (%s); found %s (%s), so layout pins are off and performance may differ slightly.' %
            (tested['version'], tested['target'], compiler['version'] or 'not readable',
             compiler['target'] or 'not readable'))
        kw = hidden_options(group=True)
        if sys.platform != "win32":
            kw["start_new_session"] = True
        # Line-buffered, so the log on disk follows the build (support, and a crash leaves the lines so far).
        with open(self.log_path, "w", encoding="utf-8", newline="\n", buffering=1) as log:
            products.log_ignored_music(self.product, self.data_dir, lambda message: log.write(message + '\n'))
            log.write("builder: %s %s, source %s (%d files copied, %d removed), data %s\n" % (
                self.product.key, self.target, version, copied, removed, self.data_dir))
            if data_dir != self.data_dir:
                log.write("builder: private game inputs staged in %s\n" % data_dir)
            log.write("toolchain packages (pacman -Q at detection):\n")
            log.write('selected SDK libraries: ' + json.dumps(self.libraries, sort_keys=True) + '\n')
            log.write('public source: ' + self.release_source['source_url'] + '\n')
            if self.release_source.get('kind') == 'source':
                log.write(source_build_text(self.release_source) + '\n')
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
                # UNDERTALE's private AOT copy has no executable SOUL icon.
                art_data_dir = self.data_dir if self.product.key == 'undertale' else data_dir
                art = self.art_generator(self.product.key, art_data_dir,
                    os.path.join(self.work, "src", "platform", "3ds", "assets"),
                    os.path.join(self.tc.root or "/opt/devkitpro", "tools", "bin",
                                 "tex3ds.exe" if self.tc.platform == "win32" else "tex3ds"), store=self.store)
            except home_art.ArtError as exc:
                log.write("HOME Menu art: " + str(exc) + "\n")
                raise GameFilesRefused(game_files_problem(self.product.title, 'art'), self.log_path) from exc
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
        refusal = game_files_refusal(self.log_path, self.product.title) if rc != 0 or self.progress.no_aot else None
        if refusal:
            raise GameFilesRefused(refusal, self.log_path)
        if self.progress.no_aot:
            raise BuildFailed("The game code could not be prepared.\n"
                              "Click Try again, or click Copy log to report the error.", self.log_path)
        if rc != 0:
            lines = Path(self.log_path).read_text(encoding='utf-8', errors='replace').splitlines()
            for index, line in enumerate(lines):
                if line.startswith('LIBRARY REFUSED: '):
                    raise BuildFailed('The required 3DS files did not pass the final check, so the game was not built. '
                                      'Choose Try again, or Copy log for support.', self.log_path,
                                      '\n'.join(lines[index:]))
        if rc != 0:
            raise BuildFailed("The build stopped (exit code %d).\n"
                              "Click Try again, or click Copy log to report the error." % rc, self.log_path)
        try:
            bid = check_build_id(os.path.join(self.work, "src", "build", "3ds-%s-%s" % (self.product.key,
                                              self.target), "generated", "ctrport_build_id.h"), version, self.source_root)
        except BuildFailed as e:
            raise BuildFailed(str(e), self.log_path)
        linked = []
        if getattr(self, 'libraries', {}).get('sdk'):
            try:
                linked = library_pins.confirm_linked(Path(self.work)/'src/build'/
                    ('3ds-%s-%s' % (self.product.key, self.target)), self.tc.platform)
            except (OSError, ValueError) as error:
                raise BuildFailed('The built game did not pass the library check. '+str(error), self.log_path) from error
        arts = self.collect()
        # Only the Old model runtime reads the pack. Building both models invokes this once.
        if self.product.key == 'undertale' and self.target == 'o3ds':
            try:
                pack = self.sfx_builder(self, data_dir)
            except Exception as error:
                if self.cancelled:
                    raise BuildFailed('The build was cancelled.', self.log_path) from error
                reason = sfx_pack_problem(self.source_root) or 'its sound step failed (the build log says why)'
                message = SFX_PACK_MISSING
                with open(self.log_path, 'a', encoding='utf-8', newline='\n') as log:
                    log.write(message + '\n' + 'builder: sfxpack: ' + reason + ': ' + str(error) + '\n')
                self.on_line(message)
                self.notices.append(message)
                pack = None
            if pack is not None:
                arts.append(pack)
        with open(self.log_path, 'a', encoding='utf-8') as log:
            log.write('linked archive SHA-256: ' + json.dumps(linked, sort_keys=True) + '\n')
        record = {"game": self.product.key, "target": self.target, "version": version, "build_id": bid,
                  "chapters": getattr(self, 'chapter_record', None),
                  "home_menu_art": art, "notices": list(self.notices),
                  "toolchain_packages": dict(self.tc.package_versions),
                  "sdk_libraries": self.libraries, "linked_libraries": linked,
                  "compiler": compiler, "release_source": self.release_source,
                  "halfart": products.halfart(self.product.key, self.target),
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
                    raise BuildFailed("The finished build is missing prepared game code.\n" + FAILED, self.log_path)
        except OSError:
            raise BuildFailed("The finished build could not be checked.\n" + FAILED, self.log_path)
        out_dir = os.path.join(self.work, "out", self.target)
        os.makedirs(out_dir, exist_ok=True)
        arts = []
        outputs = self.outputs
        if 'cia' in outputs and not self.tc.can_cia:
            # build.sh always makes a 3dsx. Never collect a stale CIA from an earlier run.
            outputs = ('3dsx',)
            self.notices.append(CIA_NOT_MADE)
            with open(self.log_path, 'a', encoding='utf-8', newline='\n') as log:
                log.write(CIA_NOT_MADE + '\n')
            self.on_line(CIA_NOT_MADE)
        for kind in outputs:
            p = os.path.join(bdir, stem + "." + kind)
            if not os.path.isfile(p) or os.path.getsize(p) == 0:
                if kind == "cia":
                    raise BuildFailed(CIA_NOT_MADE, self.log_path)
                raise BuildFailed("The finished game file is missing: %s.\n" % os.path.basename(p) + FAILED, self.log_path)
            dst = os.path.join(out_dir, os.path.basename(p))
            # Keep the previous package until a private copy matches the compiler's output.
            temporary = None
            try:
                size, digest = os.path.getsize(p), sha256_file(p)
                descriptor, temporary = tempfile.mkstemp(prefix='.' + os.path.basename(p) + '.',
                                                         suffix='.part', dir=out_dir)
                os.close(descriptor)
                cleanup.record_temporary(self.work, temporary)
                shutil.copy2(p, temporary)
                with open(temporary, 'r+b') as copied:
                    os.fsync(copied.fileno())
                if (os.path.getsize(temporary) != size or sha256_file(temporary) != digest or
                        os.path.getsize(p) != size or sha256_file(p) != digest):
                    raise BuildFailed("The finished build could not be copied correctly. "
                                      "Some build files may already have been updated. Check free space and access "
                                      "to the work folder, then try again.", self.log_path)
                os.replace(temporary, dst)
                temporary = None
            except OSError as exc:
                raise BuildFailed("The finished build could not be saved. Some build files may already have been updated. "
                                  "Check free space and access to the work folder, then try again.", self.log_path) from exc
            finally:
                if temporary is not None:
                    try:
                        os.unlink(temporary)
                    except FileNotFoundError:
                        pass
            arts.append(Artifact(self.target, kind, dst, size, digest))
        return arts
