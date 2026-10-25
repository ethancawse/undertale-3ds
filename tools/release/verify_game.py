#!/usr/bin/env python3
"""Checks that a game folder is an official, purchased copy before the builder uses it (docs/release/builder.md).

usage: verify_game.py undertale|deltarune [--game-dir DIR] [--steam-root DIR ...] [--gog-dir DIR ...] [--json]

Two checks, both offline, with no Valve or GOG SDK and no network:

1. Version: data.win (and each DELTARUNE chapter's data.win) must match an official release in ALLOW_LIST by size
   and SHA-256. UNDERTALE's macOS depot keeps its data file as UNDERTALE.app/Contents/Resources/game.ios; that file is
   checked against MACOS_ALLOW_LIST only, and a Windows data.win against ALLOW_LIST only.
2. Install record: the folder must be where a store installed it.
   - Steam: a library listed in <Steam>/steamapps/libraryfolders.vdf holds appmanifest_<appid>.acf, which says the
     game is fully installed in steamapps/common/<installdir> (this folder), and its LastOwner is an account in
     <Steam>/config/loginusers.vdf, an account that has signed in to Steam on this computer.
   - GOG (UNDERTALE only): goggame-<product id>.info in the folder names the product, and on Windows the GOG registry
     key or GOG Galaxy's database also records this folder.

DELTARUNE Chapters 1 and 2 are free (the DELTARUNE demo), so they need only the version check; Chapters 3 to 5 need
both checks. This is a deterrent, not a lock: the engine is AGPL-3.0, and a modified copy can leave the check out.

Nothing is written, uploaded or printed that identifies the Steam account; result messages say only whether the
install record names an account that has signed in here. Exit status: 0 when the copy is accepted (for DELTARUNE:
at least the free chapters), 1 when it is refused, 2 on a usage error.
"""
from __future__ import annotations

import dataclasses
import hashlib
import json
import os
import re
import sqlite3
import sys
from typing import Callable, Iterable

# ---------------------------------------------------------------------------------------------------------------------
# names only: official releases. Each row: game, part (the data.win's folder, "root" for the game folder itself), size in bytes,
# SHA-256, store, version label. Only identifiers, sizes and digests; no game bytes. Measured from
# purchased Steam copies on 2026-10-16 (the same digests as platform/3ds/data_versions.inc, revision 1).
# GOG UNDERTALE 1.08 (product 1456487183, Windows build 50921790503031850, 2018-02-02) is not listed yet: nobody on the
# project owns the GOG copy, and GOG publishes no per-file digests. Until it is measured, a GOG copy passes the install
# record check and fails the version check unless its data.win is byte-identical to the Steam one.
# DELTARUNE Chapter 6 (planned for 2027) is expected to re-patch older chapters: add the new rows, keep the old ones.
# One owner-overridable policy switch. Hash recognition itself always remains exact.
ALLOW_UNKNOWN_DELTARUNE = False
DELTARUNE_DEMO_APPID = "1690940"
# Measured Windows demo (1690940), combined Chapters 1+2, not the retail per-chapter product.
# The separate game key deliberately keeps these rows inactive until that variant is approved and implemented.
DELTARUNE_DEMO_ALLOW_LIST = [
    ("deltarune-demo", "root", 89903138, "4de4118ba4ad4243025e61337fe796532751032c0a04d0843d8b35f91ec2c220",
     "steam", "1.10 (Steam build 8272590)"),
]
DELTARUNE_DEMO_AUDIO = {
    "audiogroup1.dat": (15683508, "aa7593cf993cd19dc9fe30aa8d9eaf910a3072a481c6efa49d14a992c8b62d02"),
}

ALLOW_LIST = [
    ("undertale", "root", 62921978, "b718f8223a5bb31979ffeed10be6140c857b882fc0d0462b89d6287ae38c81c7",
     "steam", "1.08 (Steam build 4279148)"),
    ("deltarune", "root", 2953218, "82a7041a530407c5f17e439a97bdf53e4a24e30775a7ecbc2879369b9d2441d6",
     "steam", "v0.0.253 (Steam build 24484059)"),
    ("deltarune", "chapter1_windows", 13072876, "82c2bb61b8d78cd287120f6301588fecba34ec5a890bac711b7a8774c760ec70",
     "steam", "v0.0.253 (Steam build 24484059)"),
    ("deltarune", "chapter2_windows", 66949110, "047c5ab003e3e017a709c02757e119c81e0327760169512110fd276b19241e68",
     "steam", "v0.0.253 (Steam build 24484059)"),
    ("deltarune", "chapter3_windows", 126284402, "c1a0925343694ec9b9adcbf2f916a720b02fd1b999286cfe8fe6a52f3320f714",
     "steam", "v0.0.253 (Steam build 24484059)"),
    ("deltarune", "chapter4_windows", 132041918, "ed64789586238b52375e994e1c1cf13694dd2d0dab57d13e639b9c892e37d8f2",
     "steam", "v0.0.253 (Steam build 24484059)"),
    ("deltarune", "chapter5_windows", 165423612, "370dfd141d2955d5a1960122919b16e4092b52ffbb85fda541bc4680c6b3b85c",
     "steam", "v0.0.253 (Steam build 24484059)"),
]
# The macOS depot's files, in the same row format. Measured from a purchased Steam copy (Mac) on 2026-10-21:
# the same 1.08 game, a later (2019) build of its data file (platform/3ds/data_versions.inc, revision 2). The label is
# the key of the macOS record in music-versions.json.
MACOS_ALLOW_LIST = [
    ("undertale", "root", 62956178, "91a8acf1f40bd7c45d0bdb978de55187e7fb2cd76a1e2cda41c078dd44d3565e",
     "steam", "1.08 (Steam macOS)"),
]


@dataclasses.dataclass(frozen=True)
class Layout:
    """Where one store depot keeps the data file. Rows of one layout never match another layout's file."""
    key: str                   # "windows" or "macos": the music record's depot and the allow list used
    name: str                  # shown to the user
    resources: tuple           # folders from the install folder to the data file and music; () for the folder itself
    data_name: str             # the root data file's name there

    def data_dir(self, folder):
        """The folder holding the root data file and the music, or None when the bundle is not there. Bundle folder
        names are matched without regard to case, as macOS volumes usually do."""
        d = folder
        for name in self.resources:
            hit = os.path.join(d, name)
            if not os.path.isdir(hit):
                try:
                    hit = next((os.path.join(d, n) for n in sorted(os.listdir(d)) if n.casefold() == name.casefold()),
                               None)
                except OSError:
                    hit = None
                if hit is None or not os.path.isdir(hit):
                    return None
            d = hit
        return d

    def data_path(self, folder, part="root"):
        """The data file of `part`, or None when this layout has no place for it."""
        if self.key == "windows":
            return os.path.join(folder, "data.win") if part == "root" else os.path.join(folder, part, "data.win")
        d = self.data_dir(folder)
        return os.path.join(d, self.data_name) if d is not None and part == "root" else None


WINDOWS = Layout("windows", "Windows", (), "data.win")
MACOS_UNDERTALE = Layout("macos", "macOS", ("UNDERTALE.app", "Contents", "Resources"), "game.ios")
ALLOW_LISTS = {"windows": ALLOW_LIST, "macos": MACOS_ALLOW_LIST}


@dataclasses.dataclass(frozen=True)
class Game:
    key: str
    title: str
    steam_apps: tuple          # (appid, purchased?) pairs; the demo app is not a purchase
    install_dirs: tuple        # steamapps/common/<name> spellings, for the "found but no record" message
    gog_product: str | None
    store_links: tuple
    parts: tuple               # data.win folders, "root" first
    free_parts: frozenset      # parts that need no purchase
    layouts: tuple = (WINDOWS,)  # depots recognised, in the order tried


GAMES = {
    "undertale": Game("undertale", "UNDERTALE", (("391540", True),), ("Undertale", "UNDERTALE"), "1456487183",
                      ("https://store.steampowered.com/app/391540/Undertale/", "https://www.gog.com/game/undertale"),
                      ("root",), frozenset(), (WINDOWS, MACOS_UNDERTALE)),
    "deltarune": Game("deltarune", "DELTARUNE", (("1671210", True), ("1690940", False)), ("DELTARUNE", "Deltarune"),
                      None, ("https://store.steampowered.com/app/1671210/DELTARUNE/",),
                      ("root",) + tuple("chapter%d_windows" % i for i in range(1, 6)),
                      frozenset({"root", "chapter1_windows", "chapter2_windows"})),
}

# Steam's StateFlags: 4 = fully installed. Other bits (update required, updating, ...) mean the files may be in flux.
STATE_FULLY_INSTALLED = 4


# ---------------------------------------------------------------------------------------------------------------------
# Valve KeyValues text (.vdf / .acf)

_TOKEN = re.compile(r'\s*(?:(//[^\n]*)|(")|(\{)|(\})|([^\s{}"]+))', re.S)


def parse_vdf(text: str) -> dict:
    """Parses Valve's text KeyValues into nested dicts. Keys are kept as written; use vget() for case-insensitive
    lookups. Later duplicate keys win, as in Steam. Raises ValueError on malformed input."""
    pos, n = 0, len(text)
    if text.startswith("\ufeff"):
        pos = 1

    def token():
        nonlocal pos
        while True:
            m = _TOKEN.match(text, pos)
            if not m or m.end() == pos and pos >= n:
                return None
            if m.group(1) is not None:          # comment
                pos = m.end()
                continue
            if m.group(2):                      # quoted string with \\ \" \n \t escapes
                i = m.end()
                out = []
                while i < n and text[i] != '"':
                    c = text[i]
                    if c == "\\" and i + 1 < n:
                        nxt = text[i + 1]
                        out.append({"n": "\n", "t": "\t", "\\": "\\", '"': '"'}.get(nxt, "\\" + nxt))
                        i += 2
                        continue
                    out.append(c)
                    i += 1
                if i >= n:
                    raise ValueError("unterminated string")
                pos = i + 1
                return ("s", "".join(out))
            pos = m.end()
            if m.group(3):
                return ("{", None)
            if m.group(4):
                return ("}", None)
            if m.group(5):
                return ("s", m.group(5))
            return None

    def block(top):
        d = {}
        while True:
            t = token()
            if t is None:
                if top:
                    return d
                raise ValueError("unexpected end of file")
            if t[0] == "}":
                if top:
                    raise ValueError("unbalanced }")
                return d
            if t[0] != "s":
                raise ValueError("expected a key")
            v = token()
            if v is None:
                raise ValueError("key without value")
            if v[0] == "{":
                d[t[1]] = block(False)
            elif v[0] == "s":
                d[t[1]] = v[1]
            else:
                raise ValueError("unexpected }")
    return block(True)


def vget(d, *keys):
    """Case-insensitive nested lookup; None when any step is missing."""
    for k in keys:
        if not isinstance(d, dict):
            return None
        hit = None
        for kk, vv in d.items():
            if kk.lower() == k.lower():
                hit = vv
        d = hit
    return d


def read_vdf(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return parse_vdf(f.read())


# ---------------------------------------------------------------------------------------------------------------------
# Platform probes, injectable for tests

def _norm(p: str) -> str:
    return os.path.normcase(os.path.normpath(os.path.abspath(p)))


def _winreg_value(hive_name, key, value):
    try:
        import winreg
    except ImportError:
        return None
    hive = getattr(winreg, hive_name)
    for view in (winreg.KEY_WOW64_32KEY, winreg.KEY_WOW64_64KEY):
        try:
            with winreg.OpenKey(hive, key, 0, winreg.KEY_READ | view) as k:
                return str(winreg.QueryValueEx(k, value)[0])
        except OSError:
            continue
    return None


def default_steam_roots(registry=_winreg_value, home=None, platform=sys.platform):
    """Steam client folders: the registry's on Windows, then the usual install paths on each system."""
    roots = []
    if platform == "win32":
        for hive, key, value in (("HKEY_CURRENT_USER", r"Software\Valve\Steam", "SteamPath"),
                                 ("HKEY_LOCAL_MACHINE", r"SOFTWARE\Valve\Steam", "InstallPath")):
            v = registry(hive, key, value)
            if v:
                roots.append(v)
        roots += ["C:/Program Files (x86)/Steam", "C:/Program Files/Steam"]
    home = home or os.path.expanduser("~")
    if platform == "darwin":
        roots.append(os.path.join(home, "Library", "Application Support", "Steam"))
    elif platform != "win32":
        roots += [os.path.join(home, ".steam", "steam"), os.path.join(home, ".local", "share", "Steam"),
                  os.path.join(home, ".var", "app", "com.valvesoftware.Steam", ".local", "share", "Steam"),
                  os.path.join(home, "snap", "steam", "common", ".local", "share", "Steam")]
    out, seen = [], set()
    for r in roots:
        if os.path.isdir(os.path.join(r, "steamapps")) or os.path.isfile(os.path.join(r, "config", "loginusers.vdf")):
            key = _norm(os.path.realpath(r))
            if key not in seen:
                seen.add(key)
                out.append(r)
    return out


def default_gog_records(product, registry=_winreg_value, galaxy_db=None, platform=sys.platform):
    """Folders GOG says it installed `product` into: the GOG registry key (offline installer and Galaxy) and GOG
    Galaxy 2's database. Windows only; elsewhere the goggame/gameinfo file in the folder is the only record."""
    paths = []
    if platform == "win32":
        v = registry("HKEY_LOCAL_MACHINE", r"SOFTWARE\GOG.com\Games\%s" % product, "path")
        if v:
            paths.append(v)
        db = galaxy_db or os.path.join(os.environ.get("PROGRAMDATA", "C:/ProgramData"), "GOG.com", "Galaxy",
                                       "storage", "galaxy-2.0.db")
        paths += galaxy_install_paths(db, product)
    elif galaxy_db:
        paths += galaxy_install_paths(galaxy_db, product)
    return paths


def galaxy_install_paths(db, product):
    if not os.path.isfile(db):
        return []
    try:
        uri = "file:%s?mode=ro" % db.replace("\\", "/")
        con = sqlite3.connect(uri, uri=True, timeout=2)
        try:
            rows = con.execute("SELECT installationPath FROM InstalledBaseProducts WHERE productId = ?",
                               (int(product),)).fetchall()
        finally:
            con.close()
        return [r[0] for r in rows if r and r[0]]
    except (sqlite3.Error, ValueError):
        return []


def default_gog_dirs(game: Game, registry=_winreg_value, platform=sys.platform, home=None):
    """Candidate GOG folders to look at when the user did not pick one."""
    if not game.gog_product:
        return []
    out = list(default_gog_records(game.gog_product, registry=registry, platform=platform))
    home = home or os.path.expanduser("~")
    if platform == "win32":
        out += ["C:/GOG Games/Undertale", "C:/Program Files (x86)/GOG Galaxy/Games/Undertale"]
    elif platform == "darwin":
        out += ["/Applications/Undertale.app/Contents/Resources"]
    else:
        out += [os.path.join(home, "GOG Games", "Undertale", "game"), os.path.join(home, "Games", "undertale")]
    return [d for d in dict.fromkeys(out) if os.path.isdir(d)]


# ---------------------------------------------------------------------------------------------------------------------
# Results

OK, OK_FREE, NOT_FOUND, NO_RECORD, NOT_INSTALLED, ACCOUNT, UNKNOWN_VERSION, MISSING_FILES, UNREADABLE = (
    "ok", "ok_free_only", "not_found", "no_install_record", "not_fully_installed", "account_not_on_this_pc",
    "unknown_version", "missing_files", "unreadable")


@dataclasses.dataclass
class Result:
    status: str
    game: str
    folder: str | None = None
    store: str | None = None
    version: str | None = None
    accepted_parts: list = dataclasses.field(default_factory=list)
    refused_parts: list = dataclasses.field(default_factory=list)
    message: str = ""
    details: list = dataclasses.field(default_factory=list)
    layout: str | None = None      # "windows" or "macos": whose files were checked (Layout.key)
    data_dir: str | None = None    # the folder holding them and the music; the install folder for Windows

    @property
    def accepted(self):
        return self.status in (OK, OK_FREE)


def _buy(game: Game):
    return "Get it here: " + " or ".join(game.store_links)


def _chapter_name(part):
    return "the launcher" if part == "root" else "Chapter " + part[len("chapter")] if part.startswith("chapter") else part


# ---------------------------------------------------------------------------------------------------------------------
# Version check

def sha256_file(path, block=1 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(block), b""):
            h.update(chunk)
    return h.hexdigest()


_DIGESTS: dict = {}


def data_digest(path):
    """sha256_file, remembered per (path, size, mtime) for this process, so later steps (the builder's
    data-version.txt, builder/texcache.py) reuse the digest the version check computed."""
    st = os.stat(path)
    key = (os.path.abspath(path), st.st_size, st.st_mtime_ns)
    if key not in _DIGESTS:
        _DIGESTS[key] = sha256_file(path)
    return _DIGESTS[key]


def layout_for(game: Game, folder: str):
    """The depot layout of an install folder: the first of game.layouts whose root data file is there (Windows first,
    so Windows files copied next to the Mac bundle are checked as Windows files), or None."""
    for layout in game.layouts:
        path = layout.data_path(folder)
        if path and os.path.isfile(path):
            return layout
    return None


def check_versions(game: Game, folder: str, allow_list: Iterable = ALLOW_LIST, layout: Layout = WINDOWS):
    """Per part: ("ok", label) / ("unknown", None) / ("missing", None) / ("unreadable", error). Only `layout`'s files
    are read, and only against `allow_list`, which must be that layout's rows."""
    index = {}
    for g, part, size, digest, store, label in allow_list:
        if g == game.key:
            index.setdefault(part, {})[(size, digest)] = (store, label)
    out = {}
    for part in game.parts:
        path = layout.data_path(folder, part)
        if not path or not os.path.isfile(path):
            out[part] = ("missing", None)
            continue
        try:
            size = os.path.getsize(path)
            known = index.get(part, {})
            if not any(s == size for s, _ in known):    # cheap reject before hashing
                out[part] = ("unknown", None)
                continue
            digest = data_digest(path)
        except OSError as e:
            out[part] = ("unreadable", str(e))
            continue
        hit = known.get((size, digest))
        out[part] = ("ok", hit) if hit else ("unknown", None)
    return out


# ---------------------------------------------------------------------------------------------------------------------
# Steam install record

@dataclasses.dataclass
class SteamRecord:
    root: str
    library: str
    appid: str
    purchased: bool
    folder: str
    state_flags: int
    owner_known: bool
    buildid: str | None


def steam_libraries(root):
    """Libraries the Steam client at `root` knows: the root itself plus each path in libraryfolders.vdf (both the
    current format, "libraryfolders" { "0" { "path" ... } }, and the old one, "LibraryFolders" { "1" "D:\\..." })."""
    libs = [root]
    for vdf in (os.path.join(root, "steamapps", "libraryfolders.vdf"), os.path.join(root, "config", "libraryfolders.vdf")):
        if not os.path.isfile(vdf):
            continue
        try:
            data = read_vdf(vdf)
        except (OSError, ValueError):
            continue
        top = vget(data, "libraryfolders") or {}
        for k, v in top.items():
            if not k.isdigit():
                continue
            path = vget(v, "path") if isinstance(v, dict) else v
            if path:
                libs.append(path)
    out, seen = [], set()
    for lib in libs:
        key = _norm(lib)
        if key not in seen:
            seen.add(key)
            out.append(lib)
    return out


def signed_in_accounts(root):
    """SteamID64s in <root>/config/loginusers.vdf. Never printed."""
    path = os.path.join(root, "config", "loginusers.vdf")
    try:
        users = vget(read_vdf(path), "users") or {}
    except (OSError, ValueError):
        return set()
    return {k for k, v in users.items() if k.isdigit() and isinstance(v, dict)}


def steam_records(game: Game, roots):
    out = []
    for root in roots:
        accounts = None
        for lib in steam_libraries(root):
            for appid, purchased in game.steam_apps:
                acf = os.path.join(lib, "steamapps", "appmanifest_%s.acf" % appid)
                if not os.path.isfile(acf):
                    continue
                try:
                    st = vget(read_vdf(acf), "AppState") or {}
                except (OSError, ValueError):
                    continue
                if vget(st, "appid") != appid or not vget(st, "installdir"):
                    continue
                if accounts is None:
                    accounts = signed_in_accounts(root)
                try:
                    flags = int(vget(st, "StateFlags") or 0)
                except ValueError:
                    flags = 0
                owner = vget(st, "LastOwner") or ""
                out.append(SteamRecord(root, lib, appid, purchased,
                                       os.path.join(lib, "steamapps", "common", vget(st, "installdir")), flags,
                                       owner.isdigit() and owner != "0" and owner in accounts, vget(st, "buildid")))
    return out


# ---------------------------------------------------------------------------------------------------------------------
# GOG install record

def gog_record(game: Game, folder, records: Callable | None = None, platform=sys.platform):
    """(has_info_file, recorded_by_gog). The info file is goggame-<id>.info (Windows, macOS) or the offline Linux
    installer's gameinfo; on Windows GOG must also record the folder in its registry key or Galaxy's database."""
    if not game.gog_product:
        return False, False
    info = False
    p = os.path.join(folder, "goggame-%s.info" % game.gog_product)
    if os.path.isfile(p):
        try:
            with open(p, encoding="utf-8-sig") as f:
                j = json.load(f)
            info = str(j.get("gameId")) == game.gog_product or str(j.get("rootGameId")) == game.gog_product
        except (OSError, ValueError, AttributeError):
            info = False
    if not info and platform != "win32":
        gi = os.path.join(os.path.dirname(os.path.normpath(folder)), "gameinfo")
        try:
            with open(gi, encoding="utf-8", errors="replace") as f:
                info = f.readline().strip().lower() == game.title.lower()
        except OSError:
            pass
    if not info:
        return False, False
    if platform != "win32":
        return True, True
    recs = (records or (lambda prod: default_gog_records(prod)))(game.gog_product)
    return True, any(_norm(r) == _norm(folder) for r in recs)


# ---------------------------------------------------------------------------------------------------------------------
# The whole check

def _looks_like_game(game, folder):
    return layout_for(game, folder) is not None


def install_folder(game: Game, folder: str):
    """The install folder for a pick inside a Mac bundle (UNDERTALE.app, or its Contents or Resources folder): Steam's
    install record names the folder that holds the bundle. Any other folder comes back as it is."""
    parts = os.path.normpath(folder).split(os.sep)
    for layout in game.layouts:
        for depth in range(len(layout.resources), 0, -1):
            want = [n.casefold() for n in layout.resources[:depth]]
            if len(parts) > depth and [n.casefold() for n in parts[-depth:]] == want:
                up = os.sep.join(parts[:-depth]) or os.sep
                if layout_for(game, up) is layout:
                    return up
    return folder


def verify(game_key, game_dir=None, steam_roots=None, gog_dirs=None, gog_records=None, allow_list=ALLOW_LIST,
           platform=sys.platform, macos_allow_list=MACOS_ALLOW_LIST):
    game = GAMES[game_key]
    lists = {"windows": list(allow_list) + DELTARUNE_DEMO_ALLOW_LIST, "macos": macos_allow_list}
    if game_dir:
        game_dir = install_folder(game, game_dir)
    roots = default_steam_roots(platform=platform) if steam_roots is None else steam_roots
    records = steam_records(game, roots)
    # Purchased apps first, then the demo; within each, installed folders that exist.
    records.sort(key=lambda r: (not r.purchased, not os.path.isdir(r.folder)))

    candidates = []          # (folder, store, steam record or None)
    if game_dir:
        want = _norm(game_dir)
        rec = next((r for r in records if _norm(r.folder) == want), None)
        candidates.append((game_dir, "steam" if rec else None, rec))
    else:
        for r in records:
            if os.path.isdir(r.folder):
                candidates.append((r.folder, "steam", r))
        for d in (default_gog_dirs(game, platform=platform) if gog_dirs is None else gog_dirs):
            candidates.append((d, None, None))

    if not candidates:
        stray = [os.path.join(lib, "steamapps", "common", n) for root in roots for lib in steam_libraries(root)
                 for n in game.install_dirs]
        stray = [d for d in stray if _looks_like_game(game, d)]
        if stray:
            return Result(NO_RECORD, game.key, stray[0], message=(
                "%s is in %s, but Steam has no install record for it there. Install %s through Steam (or let Steam "
                "finish installing it), then try again. %s" % (game.title, stray[0], game.title, _buy(game))))
        return Result(NOT_FOUND, game.key, message=(
            "%s was not found in your Steam libraries%s. Install it with Steam%s, then try again. %s" % (
                game.title, " or GOG installs" if game.gog_product else "",
                " or GOG" if game.gog_product else "", _buy(game))))

    results = [_verify_folder(game, folder, store, rec, records, gog_records, lists, platform)
               for folder, store, rec in candidates]
    rank = {OK: 0, OK_FREE: 1, NOT_INSTALLED: 2, ACCOUNT: 3, UNKNOWN_VERSION: 4, MISSING_FILES: 5, UNREADABLE: 6,
            NO_RECORD: 7, NOT_FOUND: 8}
    return min(results, key=lambda r: rank[r.status])


def _verify_folder(game, folder, store, rec, records, gog_records, lists, platform):
    res = Result(NOT_FOUND, game.key, folder)
    layout = layout_for(game, folder) if os.path.isdir(folder) else None
    if layout is None:
        bundles = [lay.resources[0] for lay in game.layouts if lay.resources]
        res.status = MISSING_FILES
        res.message = ("%s has no data.win%s, so it is not a %s game folder. Pick the folder that holds %s's data.win%s, "
                       "or reinstall the game with %s." % (
                           folder, "".join(" or %s" % b for b in bundles), game.title, game.title,
                           "".join(" (on a Mac: the folder that holds %s)" % b for b in bundles),
                           "Steam or GOG" if game.gog_product else "Steam"))
        return res
    res.layout, res.data_dir = layout.key, layout.data_dir(folder)
    if layout is not WINDOWS:
        res.details.append("%s files in %s" % (layout.name, res.data_dir))

    # Install record.
    owner_ok = False
    if rec is not None:
        res.store = "steam"
        if rec.state_flags != STATE_FULLY_INSTALLED:
            res.status = NOT_INSTALLED
            res.message = ("Steam is still installing or updating %s (or an update is waiting). Let Steam finish, "
                           "then try again." % game.title)
            return res
        owner_ok = rec.owner_known and rec.purchased
        res.details.append("Steam app %s in library %s, buildid %s" % (rec.appid, rec.library, rec.buildid))
        res.details.append("install record names an account that has signed in on this PC: %s"
                           % ("yes" if rec.owner_known else "no"))
    else:
        has_info, recorded = gog_record(game, folder, gog_records, platform)
        if has_info and recorded:
            res.store = "gog"
            owner_ok = True
            res.details.append("GOG install record for product %s" % game.gog_product)
        elif has_info:
            res.details.append("goggame info present, but GOG has no record of this folder")
        else:
            res.details.append("no Steam or GOG install record for this folder")

    versions = check_versions(game, folder, lists.get(layout.key, ()), layout)
    unreadable = [p for p, (s, _) in versions.items() if s == "unreadable"]
    if unreadable:
        res.status = UNREADABLE
        res.message = ("Could not read %s's data.win in %s (%s). Close anything using the game files and try again."
                       % (game.title, folder, versions[unreadable[0]][1]))
        return res
    present = [p for p in game.parts if versions[p][0] != "missing"]
    if "root" not in present:
        res.status = MISSING_FILES
        res.message = "%s's data.win is missing in %s. Reinstall or repair the game, then try again." % (game.title, folder)
        return res
    labels = {versions[p][1][1] for p in present if versions[p][0] == "ok"}
    res.version = ", ".join(sorted(labels)) or None
    for part in present:
        good = versions[part][0] == "ok" or (game.key == "deltarune" and ALLOW_UNKNOWN_DELTARUNE
                                               and versions[part][0] == "unknown")
        free = part in game.free_parts
        if good and (owner_ok or free):
            res.accepted_parts.append(part)
        else:
            res.refused_parts.append(part)
    unknown = [p for p in present if versions[p][0] == "unknown"]
    paid_present = [p for p in present if p not in game.free_parts]
    repair = "use Steam's \"Verify integrity of game files\"" if res.store == "steam" else "reinstall it"

    if game.key == 'deltarune' and versions['root'][0] == 'unknown':
        path = layout.data_path(folder, 'root')
        if any(g == 'deltarune-demo' and part == 'root' and os.path.getsize(path) == size
               and data_digest(path) == digest
               for g, part, size, digest, _store, _label in DELTARUNE_DEMO_ALLOW_LIST):
            res.status = UNKNOWN_VERSION
            res.message = "The DELTARUNE demo isn't supported. Choose the full game."
            return res

    if unknown and (not game.free_parts or "root" in unknown) and not (game.key == "deltarune" and ALLOW_UNKNOWN_DELTARUNE):
        res.status = UNKNOWN_VERSION
        res.message = ("These %s files don't match an official release this builder knows. If you changed or "
                       "modded the game, %s to restore the original files. If the game was updated recently, a newer "
                       "builder may support it. %s" % (game.title, repair, _buy(game)))
        if game.key == "deltarune":
            res.message = "This version hasn't been checked yet. Update the builder."
        return res
    if not owner_ok and (not game.free_parts or not res.accepted_parts):
        if rec is not None and not rec.owner_known:
            res.status = ACCOUNT
            res.message = ("Steam's install record for %s names an account that has not signed in to Steam on this "
                           "computer. Sign in to Steam with the account that owns %s, then try again. %s"
                           % (game.title, game.title, _buy(game)))
        else:
            res.status = NO_RECORD
            res.message = ("%s isn't a Steam%s installation of %s. Install the game with Steam%s and pick the folder "
                           "the store installed. %s" % (folder, " or GOG" if game.gog_product else "", game.title,
                                                        " or GOG" if game.gog_product else "", _buy(game)))
        return res
    if game.key == "deltarune" and not any(p != "root" for p in res.accepted_parts):
        if unknown:
            res.status = UNKNOWN_VERSION
            res.message = ("Unchecked chapter files: %s. Verify the game files in Steam. "
                           "If they are unchanged, update the builder." %
                           ", ".join(_chapter_name(p) for p in unknown))
        else:
            res.status = MISSING_FILES
            res.message = "No chapter files were found. Verify the game files in Steam, then try again."
        return res
    if res.refused_parts:
        res.status = OK_FREE
        why = []
        if unknown:
            why.append("%s %s not match an official release (%s to restore them)" % (
                ", ".join(_chapter_name(p) for p in unknown), "does" if len(unknown) == 1 else "do", repair))
        locked = [p for p in res.refused_parts if p not in unknown]
        if locked:
            why.append("%s need%s a purchased copy (%s)" % (
                ", ".join(_chapter_name(p) for p in locked), "s" if len(locked) == 1 else "",
                "sign in to Steam with the account that owns DELTARUNE" if rec is not None and rec.purchased
                else "the free DELTARUNE demo includes only Chapters 1 and 2"))
        res.message = ("The free chapters can be built (%s). %s. %s" % (
            ", ".join(_chapter_name(p) for p in res.accepted_parts if p != "root") or "the launcher",
            "; ".join(why)[0].upper() + "; ".join(why)[1:], _buy(game)))
        return res
    res.status = OK
    res.message = "%s is an official %s copy, version %s." % (
        game.title, "Steam" if res.store == "steam" else "GOG" if res.store == "gog" else "free", res.version)
    if game.key == "deltarune" and not paid_present:
        res.message += " Only the free chapters are installed."
    if game.key == "deltarune" and unknown and ALLOW_UNKNOWN_DELTARUNE:
        res.message += " This version hasn't been checked yet."
    return res


# ---------------------------------------------------------------------------------------------------------------------

def main(argv):
    args = argv[1:]
    if not args or args[0] not in GAMES:
        print(__doc__)
        return 2
    game, rest = args[0], args[1:]
    game_dir, roots, gog, as_json = None, None, None, False
    i = 0
    while i < len(rest):
        a = rest[i]
        if a == "--json":
            as_json = True
            i += 1
            continue
        if i + 1 >= len(rest):
            print(__doc__)
            return 2
        v = rest[i + 1]
        if a == "--game-dir":
            game_dir = v
        elif a == "--steam-root":
            roots = (roots or []) + [v]
        elif a == "--gog-dir":
            gog = (gog or []) + [v]
        else:
            print(__doc__)
            return 2
        i += 2
    r = verify(game, game_dir=game_dir, steam_roots=roots, gog_dirs=gog)
    if as_json:
        print(json.dumps(dataclasses.asdict(r) | {"accepted": r.accepted}, indent=2))
    else:
        print(("OK: " if r.status == OK else "PARTLY OK: " if r.status == OK_FREE else "REFUSED: ") + r.message)
        if r.folder:
            print("  folder: %s" % r.folder)
        for d in r.details:
            print("  " + d)
        if r.accepted_parts or r.refused_parts:
            print("  accepted: %s" % (", ".join(r.accepted_parts) or "-"))
            print("  refused: %s" % (", ".join(r.refused_parts) or "-"))
    return 0 if r.accepted else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
