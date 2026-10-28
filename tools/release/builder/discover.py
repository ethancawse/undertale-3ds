"""Find the user's copies of the game (wizard page 2) and verify one (page 3). Offline; built on verify_game.py.

Every probe is injectable (Steam roots, GOG folders, a registry reader, GOG Galaxy's database), so the tests run on
fixture VDFs and registry exports instead of this PC's installs.
"""
from __future__ import annotations

import dataclasses
import os
import re
import sys

import products
from products import verify_game as vg


@dataclasses.dataclass
class Copy:
    store: str          # "Steam", "Steam (demo)", "GOG", "folder"
    folder: str
    version: str        # what the store says, before verification
    note: str = ""
    platform: str = ""  # whose files the folder holds: "Windows", "macOS" (verify_game.Layout.name), "" when neither


@dataclasses.dataclass
class Probes:
    """Where discovery looks. None means "this PC" (verify_game's defaults)."""
    steam_roots: list | None = None
    gog_dirs: list | None = None
    registry: object = None             # callable(hive_name, key, value) -> str | None
    galaxy_db: str | None = None
    platform: str = sys.platform
    save_dir: str | None = None

    def pc_save_dir(self, game_key):
        if self.save_dir is not None:
            return self.save_dir
        import saves
        return saves.default_pc_save_dir(game_key, platform=self.platform)

    def roots(self):
        if self.steam_roots is not None:
            return list(self.steam_roots)
        return vg.default_steam_roots(registry=self.registry or vg._winreg_value, platform=self.platform)

    def gog_records(self, product_id):
        return vg.default_gog_records(product_id, registry=self.registry or vg._winreg_value,
                                      galaxy_db=self.galaxy_db, platform=self.platform)

    def gog_candidates(self, game):
        if self.gog_dirs is not None:
            return [d for d in self.gog_dirs if os.path.isdir(d)]
        if not game.gog_product:
            return []
        out = [p for p in self.gog_records(game.gog_product) if os.path.isdir(p)]
        out += vg.default_gog_dirs(game, registry=self.registry or vg._winreg_value, platform=self.platform)
        return list(dict.fromkeys(out))


def _version_label(game_key, appid, buildid):
    for g, _part, _size, _digest, store, label in vg.ALLOW_LIST:
        if g == game_key and store == "steam" and buildid and ("build %s)" % buildid) in label:
            return label
    return "Steam build %s" % (buildid or "?")


def find_copies(game_key, probes: Probes | None = None):
    """Copies to offer on page 2: every store install record whose folder exists, then GOG folders. Verification
    (page 3) decides; this only lists."""
    probes = probes or Probes()
    game = vg.GAMES[game_key]
    out, seen = [], set()
    records = vg.steam_records(game, probes.roots())
    records.sort(key=lambda r: (not r.purchased,))
    for r in records:
        key = vg._norm(r.folder)
        if key in seen or not os.path.isdir(r.folder):
            continue
        seen.add(key)
        note = "" if r.state_flags == vg.STATE_FULLY_INSTALLED else "Steam is still installing or updating it"
        layout = vg.layout_for(game, r.folder)
        version = _version_label(game_key, r.appid, r.buildid)
        if layout is not None and layout is not vg.WINDOWS:
            version = next((label for g, _p, _s, _d, store, label in vg.ALLOW_LISTS.get(layout.key, ())
                            if g == game_key and store == "steam"), version)
        out.append(Copy("Steam" if r.purchased else "Steam (demo)", r.folder, version, note,
                        layout.name if layout else ""))
    for d in probes.gog_candidates(game):
        key = vg._norm(d)
        if key in seen:
            continue
        seen.add(key)
        _info, recorded = vg.gog_record(game, d, probes.gog_records, probes.platform)
        layout = vg.layout_for(game, d)
        out.append(Copy("GOG" if recorded else "folder", d, "GOG" if recorded else "unknown",
                        platform=layout.name if layout else ""))
    return out


@dataclasses.dataclass
class Verdict:
    accepted: bool
    result: vg.Result
    music_ok: bool
    message: str
    store_links: tuple

    @property
    def parts(self):
        return list(self.result.accepted_parts)

    @property
    def platform(self):
        """Whose files were checked: "Windows" or "macOS" (the GUI's label for the copy), "" when none were found."""
        return {"windows": "Windows", "macos": "macOS"}.get(self.result.layout or "", "")


def verify_copy(game_key, folder, probes: Probes | None = None, allow_list=None, macos_allow_list=None) -> Verdict:
    """Check the data version, Steam install record and music hashes. A Mac copy (UNDERTALE.app) is checked against
    the macOS rows and music record; the Windows depot steps are only for native files the builder does not know."""
    probes = probes or Probes()
    product = products.PRODUCTS[game_key]
    game = product.game
    r = vg.verify(game_key, game_dir=folder, steam_roots=probes.roots(), gog_dirs=[],
                  gog_records=probes.gog_records, platform=probes.platform,
                  allow_list=vg.ALLOW_LIST if allow_list is None else allow_list,
                  macos_allow_list=vg.MACOS_ALLOW_LIST if macos_allow_list is None else macos_allow_list)
    if not r.accepted:
        message = r.message
        # Native files the builder doesn't know (Linux, DELTARUNE's Mac bundle, a Mac version not listed yet): the
        # Windows depot is the way. A known Mac copy that fails another check gets that check's own message.
        if ((r.layout is None or r.layout != "windows" and r.status == vg.UNKNOWN_VERSION)
                and native_data_files(folder)):
            info, recorded = vg.gog_record(game,folder,probes.gog_records,probes.platform)
            parent_info = os.path.isfile(os.path.join(os.path.dirname(os.path.normpath(folder)), 'gameinfo'))
            message = windows_depot_message(game_key, probes.platform,
                                           store='gog' if info or recorded or parent_info else 'steam')
        return Verdict(False, r, False, message, game.store_links)
    excluded = []
    if game_key == 'deltarune':
        import chapters
        checked, audio_failures = {}, []
        root_errors = chapters.audio_errors(folder, ['root'], checked)
        if root_errors:
            return Verdict(False, r, False, 'Missing or unreadable audio: %s. Verify the game files in Steam.' % root_errors[0], ())
        for part in list(r.accepted_parts):
            errors = chapters.audio_errors(folder, [part], checked) if part != 'root' else []
            if errors:
                audio_failures.extend(errors)
                r.accepted_parts.remove(part)
                r.refused_parts.append(part)
                excluded.append(part)
        if not any(p != 'root' for p in r.accepted_parts):
            return Verdict(False, r, False, 'Missing or unreadable audio: %s. Verify the game files in Steam.' % audio_failures[0], ())
    ok, note = products.music_check(product, r.data_dir or folder, r.version, platform=r.layout or "windows",
                                    parts=r.accepted_parts, store=r.store or "steam")
    if not ok:
        return Verdict(False, r, False, note, ())
    if excluded:
        playable = [p for p in r.accepted_parts if p != 'root']
        message = 'Ready to build: %s. Audio missing for %s; unavailable. Verify the game files in Steam.' % (
            ', '.join(vg._chapter_name(p) for p in playable),
            ', '.join(vg._chapter_name(p) for p in excluded))
        return Verdict(True, r, True, message + ' ' + note, ())
    return Verdict(True, r, True, r.message + " " + note, ())


def native_data_files(folder):
    """Probe the known Linux assets and macOS bundle locations, without following directory links."""
    bases = [folder, os.path.join(folder, "assets")]
    try:
        bundles = [os.path.join(folder, n, "Contents", "Resources") for n in os.listdir(folder)
                   if n.lower().endswith(".app")]
    except OSError:
        return []
    if folder.lower().endswith(".app"):
        bundles.append(os.path.join(folder, "Contents", "Resources"))
    bases += bundles
    # Chapter data may sit below a native bundle's Resources directory.
    for base in list(bases):
        for part in vg.GAMES["deltarune"].parts:
            if part != "root":
                bases.append(os.path.join(base, part))
    return [os.path.join(b, name) for b in bases for name in ("game.unx", "game.ios")
            if os.path.isfile(os.path.join(b, name))]


def windows_depot_message(game_key, platform=sys.platform, store="steam"):
    """Short numbered steps; the full walk-through is docs/release/builder-platforms.md."""
    title = products.PRODUCTS[game_key].title
    head = "This is the %s version of %s. The builder needs the Windows version's files.\n" % (
        "Linux" if platform == "linux" else "Mac", title)
    if store == 'gog':
        return ("This GOG copy's Mac or Linux files aren't supported yet.\n"
                "Choose the supported Windows GOG files, with their original music.\n"
                "Keep the original files together. Stay determined.")
    if platform == "linux":
        return head + ("1. In Steam, right-click %s > Properties > Compatibility.\n"
                       "2. Tick 'Force the use of a specific Steam Play compatibility tool', choose Proton "
                       "Experimental.\n"
                       "3. Let Steam download the Windows version.\n"
                       "4. Manage > Browse local files, then choose that folder here.\n"
                       "Keep Proton selected while you build." % title)
    if game_key == "undertale":
        return head + ("1. Open steam://open/console in your browser and let Steam open it.\n"
                       "2. In Steam's Console, enter: download_depot 391540 391541\n"
                       "3. Wait for 'Depot download complete'.\n"
                       "4. In Finder, press Command-Shift-G and open\n"
                       "   ~/Library/Application Support/Steam/steamapps/content/app_391540/depot_391541\n"
                       "5. Copy data.win, credits.txt and every .ogg file into UNDERTALE's installed folder "
                       "(Steam > Manage > Browse local files), next to the .app.\n"
                       "6. Choose the installed folder here, not the depot folder.")
    return head + ("1. Open steam://open/console and enter: app_info_print 1671210\n"
                   "2. Find the depot whose oslist is windows, then enter: download_depot 1671210 <depot ID>\n"
                   "3. Copy the downloaded files into DELTARUNE's installed folder "
                   "(Steam > Manage > Browse local files).\n"
                   "4. Choose the installed folder here.")


# ---------------------------------------------------------------------------------------------------------------------
# Registry exports (regedit's .reg text), for tests and for support: a user can export the Steam and GOG keys and the
# builder's --dry-run can read them instead of the live registry.

_HIVES = {"HKEY_LOCAL_MACHINE": "HKEY_LOCAL_MACHINE", "HKLM": "HKEY_LOCAL_MACHINE",
          "HKEY_CURRENT_USER": "HKEY_CURRENT_USER", "HKCU": "HKEY_CURRENT_USER"}


def _unescape_reg(s):
    return re.sub(r'\\(.)', r'\1', s)


def parse_reg_export(text):
    """{(hive, key lower-case): {value name lower-case: string}} from a .reg export. Only REG_SZ ("...") and dword
    values are kept, which is all the builder reads. A 64-bit export's WOW6432Node keys are also filed under the
    plain path, the way the 32-bit registry view shows them."""
    if text.startswith("﻿"):
        text = text[1:]
    out, cur = {}, None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("[") and line.endswith("]"):
            path = line[1:-1]
            hive, _, key = path.partition("\\")
            hive = _HIVES.get(hive.upper())
            cur = None
            if hive:
                cur = {}
                keys = [key]
                if "\\wow6432node\\" in key.lower():
                    keys.append(re.sub(r"(?i)\\wow6432node\\", r"\\", key, count=1))
                for k in keys:
                    out.setdefault((hive, k.lower()), {})
                cur = [out[(hive, k.lower())] for k in keys]
            continue
        if cur is None:
            continue
        m = re.match(r'^(@|"((?:[^"\\]|\\.)*)")=(.*)$', line)
        if not m:
            continue
        name = "" if m.group(1) == "@" else _unescape_reg(m.group(2))
        val = m.group(3)
        if val.startswith('"') and val.endswith('"'):
            v = _unescape_reg(val[1:-1])
        elif val.lower().startswith("dword:"):
            v = str(int(val[6:], 16))
        else:
            continue
        for d in cur:
            d[name.lower()] = v
    return out


def registry_from_export(text):
    """A registry reader with verify_game._winreg_value's signature, backed by a .reg export."""
    table = parse_reg_export(text)

    def read(hive_name, key, value):
        return table.get((hive_name, key.lower()), {}).get(value.lower())
    return read


def read_reg_file(path):
    with open(path, "rb") as f:
        data = f.read()
    for enc in ("utf-16", "utf-8-sig") if data[:2] in (b"\xff\xfe", b"\xfe\xff") else ("utf-8-sig", "cp1252"):
        try:
            return data.decode(enc)
        except UnicodeDecodeError:
            continue
    return data.decode("latin-1")
