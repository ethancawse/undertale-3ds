"""What the builder knows about each game: names, SD layout, the files the 3DS app needs, and measured build costs.

Identifiers, counts and timings only; no game content. UNDERTALE ships first; DELTARUNE uses the same code paths.
"""
from __future__ import annotations

import dataclasses
import hashlib
import json
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RELEASE = os.path.dirname(HERE)
if RELEASE not in sys.path:
    sys.path.insert(0, RELEASE)
import verify_game  # noqa: E402
import release_contract  # noqa: E402

TARGETS = {
    "n3ds": "New 3DS / New 3DS XL / New 2DS XL",
    "o3ds": "Old 3DS / 3DS XL / 2DS",
}
# Package names and release labels come from the chosen source tree's contract.
TARGET_HINT = "A New 3DS has a C-stick nub above the face buttons and ZL/ZR shoulder buttons."


@dataclasses.dataclass(frozen=True)
class Product:
    key: str                 # verify_game.GAMES key and build.sh game argument
    title: str
    app_name: str            # public repo and SD folder of the .3dsx
    sd_data_dir: str         # where the runtime reads the game files (renamed if readiness D3 renames it)
    # Cold build on the measuring PC: target -> (wall s, AOT s).
    reference_seconds: dict
    # Work-folder disk use per target in MB (build folder + AOT folder + compile cache, same table).
    disk_mb: dict
    # Quick missing-file count; the music manifest supplies the exact file hashes.
    music_counts: dict
    # Reported smartcut table cost, added to the older cold-build references (seconds).
    graphics_seconds: float = 0.0

    @property
    def game(self) -> verify_game.Game:
        return verify_game.GAMES[self.key]

    def sd_app_dir(self) -> str:
        return "3ds/" + self.app_name

    def sd_save_dir(self) -> str:
        return self.sd_data_dir + "/saves"


PRODUCTS = {
    "undertale": Product(
        "undertale", "UNDERTALE", "undertale-3ds", "3ds/undertale-3ds",
        {"o3ds": (78.0, 2.8), "n3ds": (147.2, 7.5)},
        {"o3ds": 106, "n3ds": 196},
        {"1.08 (Steam build 4279148)": 212, "1.08 (Steam macOS)": 212, "1.08 (GOG)": 212}, 14.0),
    "deltarune": Product(
        "deltarune", "DELTARUNE", "deltarune-3ds", "3ds/deltarune-3ds",
        {"o3ds": (492.4, 27.0), "n3ds": (800.9, 62.0)},
        {"o3ds": 367, "n3ds": 792},
        {}, 230.0),
}

# Release setting, off for now (decided 2026-10-26): UNDERTALE's Old 3DS build with Omega Flowey's over-resolution art
# stored at half resolution (build.sh halfart, tools/analysis/halfart_pack.py, platform/3ds/source/ctr_halfart.c), the
# fallback if the full-resolution set doesn't fit by release. On: the UNDERTALE o3ds build gets "halfart"
# (pipeline.build_argv), and texture preparation on the PC also writes that build's own cache folder, cache_lt_<n>
# (texcache.py), so its first start doesn't prepare textures on the console. Every other build is unchanged.
HALFART = False


def halfart(game, target="o3ds", setting=None):
    """Whether this game and model build with half-resolution art (HALFART, or setting when given)."""
    return bool(HALFART if setting is None else setting) and game == "undertale" and target == "o3ds"


# Never copied to the SD card: the PC executable and its libraries.
SKIP_SUFFIXES = (".exe", ".dll")
SKIP_NAMES = ("steam_appid.txt",)


def package_stem(product: Product, target: str, source_root=release_contract.ROOT) -> str:
    return release_contract.read(source_root)['package_stem'].format(game=product.key, model=target)


def read_music_manifest():
    with open(os.path.join(RELEASE, 'music-versions.json'), encoding='utf-8') as source:
        manifest = json.load(source)
    if manifest.get('schema') != 2 or manifest.get('game') != 'undertale':
        raise ValueError('Invalid music manifest')
    return manifest


def load_music_manifest(version, *, store='steam', platform='windows'):
    manifest = read_music_manifest()
    record = manifest['releases'].get(store, {}).get(platform, {}).get(version)
    if record is None:
        raise ValueError('No verified music manifest for this version')
    entries = record['files']
    if record['store'] != store or record['depot'] != platform or len(entries) != record['count']:
        raise ValueError('Invalid music manifest record')
    names = set()
    for entry in entries:
        name = entry['name']
        if (not re.fullmatch(r'[A-Za-z0-9_. -]+\.ogg', name) or name.casefold() in names
                or not isinstance(entry['size'], int) or entry['size'] <= 0
                or not re.fullmatch(r'[0-9a-f]{64}', entry['sha256'])):
            raise ValueError('Invalid music file record')
        names.add(name.casefold())
    return entries


def music_check(product: Product, folder: str, version: str | None, *, store='steam', platform='windows', parts=None):
    """Verify every UNDERTALE music file by size and SHA-256; check DELTARUNE's music folder."""
    if product.key == "undertale":
        try:
            names = os.listdir(folder)
        except OSError as e:
            return False, "Could not list %s (%s)." % (folder, e)
        oggs = [n for n in names if n.lower().endswith(".ogg") and os.path.isfile(os.path.join(folder, n))]
        credits = os.path.join(folder, "credits.txt")
        want = product.music_counts.get(version or "")
        problems = []
        if not os.path.isfile(credits) or os.path.getsize(credits) == 0:
            problems.append("credits.txt is missing (the true ending's credits need it)")
        if want is not None and len(oggs) < want:
            problems.append("only %d of the %d music files (.ogg) are there" % (len(oggs), want))
            try:
                missing = [entry['name'] for entry in load_music_manifest(version, store=store, platform=platform)
                           if not os.path.isfile(os.path.join(folder, entry['name']))]
                if missing:
                    problems.append('missing music: ' + ', '.join(missing[:3]))
            except (OSError, ValueError, KeyError, TypeError):
                pass  # The count failure already stops the build.
        elif not oggs:
            problems.append("the music files (.ogg) are missing")
        restore = ('Use Steam\'s "Verify integrity of game files"' if store == 'steam'
                   else 'Use GOG Galaxy\'s Verify / Repair, or reinstall your GOG copy')
        if problems:
            return False, "%s: %s. %s to restore them, then try again." % (folder, "; ".join(problems), restore)
        repair = restore + ' to restore the music, then try again. Move custom music out of the game folder first.'
        try:
            entries = load_music_manifest(version, store=store, platform=platform)
        except (OSError, ValueError, KeyError, TypeError) as exc:
            return False, '%s. Download and unpack the current builder, then try again.' % exc
        expected = {entry['name'].casefold() for entry in entries}
        for name in oggs:
            if name.casefold() not in expected:
                return False, 'Unexpected music file %s. %s' % (name, repair)
        for entry in entries:
            name = entry['name']
            path = os.path.join(folder, name)
            try:
                if os.path.getsize(path) != entry['size']:
                    return False, 'Music file %s has the wrong size. %s' % (name, repair)
                sha = hashlib.sha256()
                with open(path, 'rb') as source:
                    for block in iter(lambda: source.read(1024 * 1024), b''):
                        sha.update(block)
                if sha.hexdigest() != entry['sha256']:
                    return False, 'Music file %s has the wrong SHA-256. %s' % (name, repair)
            except OSError:
                return False, 'Music file %s is missing or unreadable. %s' % (name, repair)
        return True, 'credits.txt is present; %d music files verified (size and SHA-256).' % len(entries)
    if not os.path.isdir(os.path.join(folder, "mus")):
        return False, ("%s has no mus folder (the shared music). Use Steam's \"Verify integrity of game files\", then "
                       "try again." % folder)
    import chapters
    selected = [p for p in (parts if parts is not None else chapters.PARTS) if p != 'root']
    errors = chapters.audio_errors(folder, ['root'] + selected)
    if errors:
        return False, 'Missing or unreadable audio: %s. Verify the game files in Steam.' % errors[0]
    return True, '%s %s: audio checked.' % ('Chapter' if len(selected) == 1 else 'Chapters', chapters.numbers(selected))


def card_music_names(store='steam'):
    """{case-folded name: the Windows depot's spelling}, the names the game asks for (its SOND file names). The macOS
    depot spells some differently (mus_bgflamea.ogg for mus_bgflameA.ogg); the card gets the game's spelling, so it
    never depends on the 3DS's FAT lookup ignoring case. Empty when the manifest can't be read (the music check, which
    runs first, then refuses the copy)."""
    try:
        windows = read_music_manifest()['releases'].get(store, {}).get('windows', {})
        return {e['name'].casefold(): e['name'] for record in windows.values() for e in record['files']}
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return {}


def aot_data_dir(product: Product, folder: str, staging: str):
    """The folder build.sh's data= gets. A Windows copy is used where it is. A macOS copy's game.ios is staged as
    <staging>/macos/data.win, a hard link (a copy where links don't work), since the AOT generator reads data.win."""
    layout = verify_game.layout_for(product.game, folder)
    if layout is None or layout is verify_game.WINDOWS:
        return folder
    src = layout.data_path(folder)
    out = os.path.join(staging, layout.key)
    dst = os.path.join(out, "data.win")
    os.makedirs(out, exist_ok=True)
    try:
        if os.path.samefile(src, dst):
            return out
        s, d = os.stat(src), os.stat(dst)
        if s.st_size == d.st_size and s.st_mtime == d.st_mtime:
            return out
    except OSError:
        pass
    tmp = dst + ".part"
    for p in (tmp, dst):
        if os.path.lexists(p):
            os.remove(p)
    try:
        os.link(src, tmp)
    except OSError:
        shutil.copy2(src, tmp)
    os.replace(tmp, dst)
    return out


def sd_files(product: Product, folder: str, parts=None):
    """The game files the 3DS app reads, as (source path, path relative to the SD data folder, size). A macOS copy's
    files come from its bundle, with game.ios as data.win and the music under the game's own spelling, so its card
    looks like one made from the Windows files."""
    out = []
    layout = verify_game.layout_for(product.game, folder) if product.key == "undertale" else None
    if layout is not None and layout is not verify_game.WINDOWS:
        src_dir = layout.data_dir(folder)
        names = card_music_names()
        for name in sorted(os.listdir(src_dir)):
            p = os.path.join(src_dir, name)
            if not os.path.isfile(p):
                continue
            if name == layout.data_name:
                out.append((p, "data.win", os.path.getsize(p)))
            elif name.lower() == "credits.txt" or name.lower().endswith(".ogg"):
                out.append((p, names.get(name.casefold(), name), os.path.getsize(p)))
        return sorted(out, key=lambda e: e[1])
    if product.key == "undertale":
        for name in sorted(os.listdir(folder)):
            low = name.lower()
            p = os.path.join(folder, name)
            if os.path.isfile(p) and (low in ("data.win", "credits.txt") or low.endswith(".ogg")):
                out.append((p, name, os.path.getsize(p)))
        return out
    # DELTARUNE: the folder's contents, minus PC binaries and chapters not chosen.
    chapters = {p for p in product.game.parts if p != "root"}
    chosen = set(parts) if parts is not None else chapters
    for dirpath, dirnames, filenames in os.walk(folder):
        rel_dir = os.path.relpath(dirpath, folder)
        top = rel_dir.split(os.sep)[0]
        if rel_dir == ".":
            dirnames[:] = [d for d in dirnames if d not in chapters or d in chosen]
        for name in sorted(filenames):
            if name.lower().endswith(SKIP_SUFFIXES) or name.lower() in SKIP_NAMES:
                continue
            if top in chapters and top not in chosen:
                continue
            p = os.path.join(dirpath, name)
            rel = name if rel_dir == "." else os.path.join(rel_dir, name).replace(os.sep, "/")
            out.append((p, rel, os.path.getsize(p)))
    return out
