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
from path_hygiene import is_junk  # noqa: E402

TARGETS = {
    "n3ds": "New 3DS / New 3DS XL / New 2DS XL",
    "o3ds": "Old 3DS / 3DS XL / 2DS",
}
# Package names and release labels come from the chosen source tree's contract.
TARGET_HINT = "A New 3DS has a C-stick nub above the face buttons and ZL/ZR shoulder buttons."


# Decimal MB: shared by Options and the pre-build free-space gate. Delivery is checked separately.
UNDERTALE_WORK_MB = 1000


def _comparison_path(path):
    path = os.path.normcase(path)
    if os.name == 'nt':
        if path.startswith('\\\\?\\unc\\'):
            path = '\\\\' + path[8:]
        elif path.startswith('\\\\?\\'):
            path = path[4:]
            if not re.match(r'^[a-z]:\\', path):
                raise ValueError('Cannot compare source and output roots; choose a plain local or UNC path.')
        elif path.startswith('\\\\.\\'):
            raise ValueError('Cannot compare source and output roots; choose a plain local or UNC path.')
    return os.path.normpath(path)


def require_output_outside_game(folder, output):
    """Refuse source-folder writes, including through a symlink or junction."""
    if folder is None:
        return
    # Resolve whole input files before taking their parents: data.win can itself
    # be a link outside the selected folder. Protect the visible bundle as well.
    folders = [folder] + [os.path.join(folder, 'chapter%d_windows' % n) for n in range(1, 6)
                          if os.path.isdir(os.path.join(folder, 'chapter%d_windows' % n))]
    sources = set()
    for candidate in folders:
        roots = [os.path.realpath(candidate)] + [os.path.dirname(os.path.realpath(os.path.join(candidate, name)))
                                               for name in ('data.win', 'game.ios')]
        for root in roots:
            if re.fullmatch(r'chapter[1-5]_windows', os.path.basename(root), re.IGNORECASE):
                root = os.path.dirname(root)
            sources.add(_comparison_path(root))
    target = _comparison_path(os.path.realpath(output))
    for source in sources:
        if os.name == 'nt':
            left, right = os.path.splitdrive(source)[0], os.path.splitdrive(target)[0]
            # Resolved, distinct local drive letters are provably disjoint.
            # UNC/device roots cannot be assumed disjoint from a local drive.
            if re.fullmatch(r'[a-z]:', left) and re.fullmatch(r'[a-z]:', right) and left != right:
                continue
        try:
            inside = os.path.commonpath((source, target)) == source
        except ValueError as exc:
            raise ValueError('Cannot compare source and output roots; refusing a possible source-folder write. '
                             'Copy or install the game on a local drive and choose a local work/output folder.') from exc
        if inside:
            raise ValueError('Choose a work/output folder outside the source game folder; game files are read-only.')


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

    def work_disk_mb(self, targets):
        """Planning allowance in decimal MB, excluding delivery copies and installed tools."""
        if self.key == 'undertale':
            # R268 Windows both-model work: 833,062,635 B including about 196 MB
            # exported delivery files, so about 637 MB source/build/cache. Round
            # up for working space; this is not a measured transient peak. Use
            # the same allowance for one model until separately measured.
            return UNDERTALE_WORK_MB
        return sum(self.disk_mb[target] for target in targets) + 30

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

# Release setting, off for now (maintainer, 2026-10-26): UNDERTALE's Old 3DS build with Omega Flowey's over-resolution art
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
        if (stray_name(name) or not re.fullmatch(r'[A-Za-z0-9_. -]+\.ogg', name) or name.casefold() in names
                or not isinstance(entry['size'], int) or entry['size'] <= 0
                or not re.fullmatch(r'[0-9a-f]{64}', entry['sha256'])):
            raise ValueError('Invalid music file record')
        names.add(name.casefold())
    return entries


def load_credits_manifest(version, *, store='steam', platform='windows'):
    record = read_music_manifest()['releases'][store][platform][version]['credits']
    if (not isinstance(record['size'], int) or record['size'] <= 0
            or not re.fullmatch(r'[0-9a-f]{64}', record['sha256'])):
        raise ValueError('Invalid credits file record')
    return dict(record, name='credits.txt')


def music_check(product: Product, folder: str, version: str | None, *, store='steam', platform='windows', parts=None,
                log=lambda message: None):
    """Verify every UNDERTALE music file by size and SHA-256; check DELTARUNE's music folder."""
    if product.key == "undertale":
        try:
            names = os.listdir(folder)
        except OSError:
            return False, "Could not open the game folder. Check folder access, then try again."
        try:
            entries = load_music_manifest(version, store=store, platform=platform)
            credits_entry = load_credits_manifest(version, store=store, platform=platform)
            if not entries:
                raise ValueError('Empty music manifest')
        except (OSError, ValueError, KeyError, TypeError):
            return False, "This builder could not check the game's music and credits. Download and unpack the complete builder again."
        expected = {entry['name'].casefold() for entry in entries}
        if any(n.lower().endswith('.ogg') and n.casefold() not in expected for n in names):
            log("Skipped extra music files. Only the game's original music is used.")
        credits = os.path.join(folder, "credits.txt")
        problems = []
        if not os.path.isfile(credits) or os.path.getsize(credits) == 0:
            problems.append("credits.txt is missing or empty (the true ending's credits need it)")
        missing = [entry['name'] for entry in entries if not os.path.isfile(os.path.join(folder, entry['name']))]
        if missing:
            problems.append('Music file %s is missing; only %d of the %d music files (.ogg) are there' % (
                missing[0], len(entries) - len(missing), len(entries)))
            if len(missing) > 1:
                problems.append('missing music: ' + ', '.join(missing[:3]))
            if len(missing) == len(entries):
                problems.append('the music files (.ogg) are missing')
        restore = 'Restore the original music and credits from a complete copy of the game, then try again.'
        if problems:
            return False, "%s. %s" % ("; ".join(problems), restore)
        for entry in [credits_entry, *entries]:
            name = entry['name']
            path = os.path.join(folder, name)
            kind = 'Game credits file' if name == 'credits.txt' else 'Music file'
            repair = restore if name == 'credits.txt' else 'Restore the original music files from a complete copy of the game, then try again.'
            try:
                if os.path.getsize(path) != entry['size']:
                    return False, '%s %s does not match the supported original. %s' % (kind, name, repair)
                sha = hashlib.sha256()
                with open(path, 'rb') as source:
                    for block in iter(lambda: source.read(1024 * 1024), b''):
                        sha.update(block)
                if sha.hexdigest() != entry['sha256']:
                    return False, '%s %s does not match the supported original. %s' % (kind, name, repair)
            except FileNotFoundError:
                return False, '%s %s is missing. %s' % (kind, name, repair)
            except OSError:
                return False, 'Could not read %s %s. Check folder access, then try again.' % (kind.lower(), name)
        return True, 'Game credits and %d music files checked. Ready to build.' % len(entries)
    if not os.path.isdir(os.path.join(folder, "mus")):
        return False, ('The shared music folder is missing. Restore the complete original game folder, '
                       'then try again.')
    import chapters
    selected = [p for p in (parts if parts is not None else chapters.PARTS) if p != 'root']
    errors = chapters.audio_errors(folder, ['root'] + selected)
    if errors:
        return False, 'Missing or unreadable audio: %s. Restore the complete original game folder, then try again.' % errors[0]
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


def source_music_names(platform):
    """Official source spellings only. Supported editions share the same music-name set (tested)."""
    return sorted({entry['name'] for releases in read_music_manifest()['releases'].values()
                   for record in releases.get(platform, {}).values() for entry in record['files']})


def log_ignored_music(product, folder, log):
    if product.key != 'undertale':
        return
    layout = verify_game.layout_for(product.game, folder)
    if layout is None:
        return
    try:
        expected = {name.casefold() for name in source_music_names(layout.key)}
        locations = [folder, layout.data_dir(folder)]
        if layout is not verify_game.WINDOWS:
            locations.append(os.path.dirname(layout.data_dir(folder)))
        if any(name.lower().endswith('.ogg') and name.casefold() not in expected
               for location in dict.fromkeys(locations) for name in os.listdir(location)):
            log("Skipped extra music files. Only the game's original music is used.")
    except OSError:
        pass  # The normal file checks report unreadable required files.


def stray_name(name):
    """Game entries have no dot names; source files use only the shared metadata rules."""
    return is_junk(name) or any(part.startswith('.')
                                for part in os.fspath(name).replace('\\', '/').split('/'))


def _same_bytes(a: str, b: str) -> bool:
    # Not filecmp.cmp: it caches by size and date, which is the case this check is for.
    with open(a, "rb") as fa, open(b, "rb") as fb:
        while True:
            x, y = fa.read(1 << 20), fb.read(1 << 20)
            if x != y:
                return False
            if not x:
                return True


UNDERTALE_AOT_FOLDER = 'Undertale'


class MissingGameData(ValueError):
    """The selected install no longer has the input needed for staging."""


def aot_data_dir(product: Product, folder: str, staging: str):
    """Stage UNDERTALE as <staging>/Undertale/data.win for a stable AOT label on every host and depot.

    The generated AOT table embeds the parent folder name. Keep the tested spelling independent of the player's
    chosen path. Copy into writable private files; delivery still reads the selected install.
    """
    if product.key != 'undertale':
        return folder
    layout = verify_game.layout_for(product.game, folder)
    if layout is None:
        raise MissingGameData("Could not find UNDERTALE's data file. Go back and choose the game folder again.")
    import cleanup
    require_output_outside_game(folder, staging)
    cleanup.require_plain_tree(staging)
    src = layout.data_path(folder)
    out = os.path.join(staging, UNDERTALE_AOT_FOLDER)
    dst = os.path.join(out, "data.win")
    os.makedirs(out, exist_ok=True)
    try:
        # Retire links left by an earlier builder. A copy is reusable only while
        # its bytes still match: a replaced data.win/game.ios can keep size/date.
        if not os.path.samefile(src, dst) and os.stat(src).st_size == os.stat(dst).st_size and _same_bytes(src, dst):
            return out
    except OSError:
        pass
    tmp = dst + ".part"
    for p in (tmp, dst):
        if os.path.lexists(p):
            os.remove(p)
    try:
        shutil.copyfile(src, tmp)
        stamp = os.stat(src)
        os.utime(tmp, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        os.replace(tmp, dst)
    finally:
        try:
            os.remove(tmp)
        except OSError:
            pass  # Preserve the staging error if the work folder also refuses removal.
    return out


def sd_files(product: Product, folder: str, parts=None):
    """The game files the 3DS app reads, as (source path, path relative to the SD data folder, size). A macOS copy's
    files come from its bundle, with game.ios as data.win and the music under the game's own spelling, so its card
    looks like one made from the Windows files."""
    out = []
    layout = verify_game.layout_for(product.game, folder) if product.key == "undertale" else None
    if product.key == 'undertale':
        layout = layout or verify_game.WINDOWS
        src_dir = layout.data_dir(folder)
        names = card_music_names()
        for name in [layout.data_name, 'credits.txt', *source_music_names(layout.key)]:
            p = os.path.join(src_dir, name)
            if not os.path.isfile(p):
                continue
            if name == layout.data_name:
                out.append((p, "data.win", os.path.getsize(p)))
            else:
                out.append((p, names.get(name.casefold(), name), os.path.getsize(p)))
        return sorted(out, key=lambda e: e[1])
    # DELTARUNE: the folder's contents, minus PC binaries and chapters not chosen.
    chapters = {p for p in product.game.parts if p != "root"}
    chosen = set(parts) if parts is not None else chapters
    for dirpath, dirnames, filenames in os.walk(folder):
        # Save import has its own explicit preview and confirmation. Rebuilding never imports saves or settings.
        dirnames[:] = [d for d in dirnames if d.casefold() != 'saves' and not stray_name(d)
                       and not d.lower().endswith('.ogg')]
        rel_dir = os.path.relpath(dirpath, folder)
        top = rel_dir.split(os.sep)[0]
        if rel_dir == ".":
            dirnames[:] = [d for d in dirnames if d not in chapters or d in chosen]
        for name in sorted(filenames):
            if stray_name(name):
                continue
            if name.casefold() in ('settings.ini', 'legacycfg.ini', 'delta' + 'tale.ini'):
                continue
            if name.lower().endswith(SKIP_SUFFIXES) or name.lower() in SKIP_NAMES:
                continue
            if top in chapters and top not in chosen:
                continue
            p = os.path.join(dirpath, name)
            if not os.path.isfile(p) or os.path.getsize(p) == 0:
                continue
            rel = name if rel_dir == "." else os.path.join(rel_dir, name).replace(os.sep, "/")
            out.append((p, rel, os.path.getsize(p)))
    return out
