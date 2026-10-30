"""Wizard page 7: put the personal build and the user's own game files where the 3DS can use them.

Copy to SD card (default), Save as files, or FTP-only Install over Wi-Fi.
FTP includes the programs and game files. The netloader is never contacted.
"""
from __future__ import annotations

import dataclasses
import ftplib
import os
import re
import shutil
import string
import subprocess
import sys

import products
import install_record
import sd_transaction
from processes import hidden_options
from saves import (  # noqa: F401
    IGNORED_SAVE_NAMES,
    IGNORED_SAVE_SUFFIXES,
    MAX_SAVE_BYTES,
    NO_3DS_FOLDER_MESSAGE,
    READ_ONLY_MESSAGE,
    DeliverError,
    ExistingSavesError,
    SaveCopyResult,
    SaveFile,
    SaveScan,
    check_sd_writable,
    copy_pc_saves,
    default_pc_save_dir,
    existing_sd_save_files,
    has_existing_sd_saves,
    inspect_save_file,
    scan_saves,
    scan_sd_saves,
    sd_save_dir,
    write_error,
)

FAT_SLACK = 2.0          # FAT keeps modification times to 2 s
DSP_MESSAGE = ("sd:/3ds/dspfirm.cdc is missing (needed for sound).\n"
               "Run DSP1 once in the Homebrew Launcher on your 3DS.")


@dataclasses.dataclass
class Card:
    root: str
    free: int
    label: str


def _drive_type(root):
    import ctypes
    return ctypes.windll.kernel32.GetDriveTypeW(root)


def _drive_label(root):
    import ctypes
    buf = ctypes.create_unicode_buffer(261)
    ok = ctypes.windll.kernel32.GetVolumeInformationW(root, buf, 261, None, None, None, None, 0)
    return buf.value if ok else ""


# A 3DS reads only FAT SD cards. Linux calls the filesystem vfat (or msdos), macOS msdos.
SD_FILESYSTEMS = ("vfat", "msdos")


def _unescape_mount(path):
    # /proc/self/mounts writes space, tab, newline and backslash as octal escapes.
    return re.sub(r"\\([0-7]{3})", lambda m: chr(int(m.group(1), 8)), path)


def mount_types(platform=sys.platform, read=None, run=None):
    """{mount point: filesystem type} for mounted volumes, or {} if it can't be read. Read-only."""
    out = {}
    try:
        if platform == "darwin":
            run = run or (lambda: subprocess.run(["/sbin/mount"], stdin=subprocess.DEVNULL, capture_output=True,
                                                 text=True, timeout=10).stdout)
            for line in run().splitlines():
                m = re.match(r"^.+? on (.+) \(([^,)]+)", line)
                if m:
                    out[m.group(1)] = m.group(2).strip().lower()
        else:
            if read is None:
                with open("/proc/self/mounts", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            else:
                text = read()
            for line in text.splitlines():
                parts = line.split()
                if len(parts) >= 3:
                    out[_unescape_mount(parts[1])] = parts[2].lower()
    except (OSError, subprocess.SubprocessError):
        return {}
    return out


def list_sd_cards(platform=sys.platform, drive_type=None, roots=None, mounts=None):
    """Removable drives that have a 3ds folder (a homebrew-ready SD card). On Linux and macOS only FAT mount points
    count, so a fixed disk, a network share or WSL's /mnt/c that happens to have a 3ds folder is never offered."""
    out = []
    if platform == "win32":
        drive_type = drive_type or _drive_type
        cands = roots if roots is not None else ["%s:\\" % c for c in string.ascii_uppercase
                                                  if os.path.exists("%s:\\" % c)]
        for r in cands:
            try:
                if drive_type(r) != 2:            # DRIVE_REMOVABLE
                    continue
            except OSError:
                continue
            if os.path.isdir(os.path.join(r, "3ds")):
                lab = ""
                try:
                    lab = _drive_label(r) if roots is None else ""
                except OSError:
                    pass
                out.append(Card(r, shutil.disk_usage(r).free, lab))
        return out
    if mounts is None and roots is None:
        mounts = mount_types(platform)
    user = os.environ.get("USER", "")
    bases = roots if roots is not None else (["/Volumes"] if platform == "darwin" else
                                           ["/media/" + user, "/run/media/" + user, "/media", "/mnt"])
    seen = set()
    for b in bases:
        try:
            names = sorted(os.listdir(b))
        except OSError:
            continue
        for n in names:
            r = os.path.join(b, n)
            key = os.path.realpath(r)
            try:
                if mounts is not None and mounts.get(key, mounts.get(r)) not in SD_FILESYSTEMS:
                    continue
                if key not in seen and os.path.isdir(os.path.join(r, "3ds")):
                    out.append(Card(r, shutil.disk_usage(r).free, n))
                    seen.add(key)
            except OSError:
                continue
    return out


@dataclasses.dataclass
class CopyItem:
    src: str
    dst: str           # relative to the SD root, forward slashes
    size: int
    skip: bool = False
    digest: str = None  # verified source identity for data.win, captured while planning


def plan_sd_copy(sd_root, product: products.Product, artifacts, game_folder, parts=None, cache_sets=()):
    """What goes where. .3dsx -> sd:/3ds/<app>/, .cia -> sd:/cia/, game files -> the runtime's data folder.
    data.win skips require fresh SHA-256 equality; other files use size and time."""
    if not os.path.isdir(os.path.join(sd_root, "3ds")):
        raise DeliverError(NO_3DS_FOLDER_MESSAGE)
    check_sd_writable(sd_root)
    items = []
    for a in unique_artifacts(artifacts):
        rel = ("cia/" if a.kind == "cia" else product.sd_data_dir + "/" if a.kind == 'sfxpack'
               else product.sd_app_dir() + "/") + os.path.basename(a.path)
        items.append(CopyItem(a.path, rel, a.size))
    for src, rel, size in products.sd_files(product, game_folder, parts):
        if "cache_exact" in rel.casefold().split("/"):
            continue  # Exact-colour caches are prepared only by the console.
        items.append(CopyItem(src, product.sd_data_dir + "/" + rel, size))
    for it in items:
        d = os.path.join(sd_root, *it.dst.split("/"))
        if os.path.basename(it.dst).casefold() in ("data.win", "adpcm-sfx.dtsp") or it.dst.endswith(('.3dsx', '.cia')):
            import texcache
            it.digest = texcache._digest(it.src)
            texcache.checked_digest(it.src, it.digest)
            try:
                it.skip = texcache.checked_digest(d) == it.digest
            except OSError:
                it.skip = False
            continue
        try:
            st, ss = os.stat(d), os.stat(it.src)
            it.skip = st.st_size == ss.st_size and abs(st.st_mtime - ss.st_mtime) <= FAT_SLACK
        except OSError:
            it.skip = False
    check_copy_space(sd_root, items, cache_sets, product=product)
    return items


def check_copy_space(sd_root, items, cache_sets=(), product=None):
    """Budget the complete staged set, allocation slack, caches, receipts and durable update metadata."""
    import texcache
    sizes, paths = [it.size for it in items], [it.dst for it in items]
    for cs in cache_sets:
        for name in [texcache.page_name(i) for i in texcache._local_pages(cs)] + [texcache.MARKER]:
            sizes.append(os.path.getsize(os.path.join(cs.local_dir, name)))
            paths.append(cs.sd_dir + '/' + name)
    # A version receipt per data.win plus the final install record; reserve metadata even if no texture tool works.
    record_size = 4096 + sum(256 + len(path.encode('utf-8')) for path in paths)
    previous = install_record.read(sd_root, product) if product is not None else None
    if previous:
        import json
        record_size += len(json.dumps(previous, indent=2).encode('utf-8'))
    sizes += [4096] * sum(it.dst.endswith('/data.win') for it in items) + [record_size]
    need = sd_transaction.space_required(sizes, paths)
    free = shutil.disk_usage(sd_root).free
    if need > free:
        need_mb = -(-need // 10**6)
        raise DeliverError("This card has %d MB free; %d MB needed.\n"
                           "Free up space on the SD card and try again." % (free // 10**6, need_mb))
    return need


def copy_items(sd_root, items, progress=None, cancelled=lambda: False):
    """Copies with a temporary name, then renames, so a pulled card never leaves a half-written file under the real
    name. Returns (copied, skipped, bytes)."""
    targets = [os.path.join(sd_root, *it.dst.split("/")) for it in items if not it.skip]
    check_sd_writable(sd_root, targets)
    total = sum(it.size for it in items if not it.skip)
    done = copied = skipped = 0
    for it in items:
        if cancelled():
            raise DeliverError("Copy cancelled.\nClick Copy to card to try again.")
        d = os.path.join(sd_root, *it.dst.split("/"))
        data_win = os.path.basename(it.dst).casefold() == "data.win"
        checked_file = data_win or os.path.basename(it.dst).casefold() == 'adpcm-sfx.dtsp' or it.dst.endswith(('.3dsx', '.cia'))
        if checked_file:
            import texcache
            it.digest = it.digest or texcache._digest(it.src)
            texcache.checked_digest(it.src, it.digest)
            # The card may have changed since planning (including same-size/date corruption).
            if it.skip:
                try:
                    it.skip = texcache.checked_digest(d) == it.digest
                except OSError:
                    it.skip = False
                if not it.skip:
                    total += it.size
        if it.skip:
            skipped += 1
            continue
        tmp = d + ".part"
        try:
            os.makedirs(os.path.dirname(d), exist_ok=True)
            if data_win:
                texcache.invalidate_version(d)
            if checked_file:
                source_before = texcache.file_identity(os.stat(it.src))
            shutil.copy2(it.src, tmp)
            sd_transaction.sync_file(tmp)
            if checked_file:
                if source_before != texcache.file_identity(os.stat(it.src)):
                    raise OSError("Game data changed while copying. Verify the files and try again.")
                texcache.checked_digest(tmp, it.digest)
            sd_transaction.replace(tmp, d)
            if checked_file:
                texcache.checked_digest(d, it.digest)
        except OSError as e:
            try:
                if os.path.exists(tmp):
                    os.remove(tmp)
            except OSError:
                pass
            raise write_error(sd_root, e) from e
        copied += 1
        done += it.size
        if progress:
            progress(done, total, it.dst)
    return copied, skipped, done


def _copy_payload(sd_root, items, product, folder, work, parts=None, progress=None, cancelled=lambda: False,
                  log=lambda s: None, textures=True, tool=None, data_version=True, prepared=None):
    """copy_items, then the texture cache folders prepared on the PC (texcache.py). Returns (copied, skipped, bytes,
    texcache.Result). Preparing is optional: if it can't run, the 3DS prepares the textures as before. A failed write
    to the card is an error like any other copy. Then each data.win's cache/data-version.txt (texcache.py), so the 3DS
    skips hashing the data.win on its first start; its message is appended to the Result's."""
    import texcache
    items = list(items)
    try:
        if cancelled():
            raise InterruptedError("cancelled")
        install_record.invalidate(sd_root, product)
        copied, skipped, nbytes = copy_items(sd_root, items, progress, cancelled)
        if textures:
            res = texcache.deliver_textures(sd_root, product, folder, work, parts, tool=tool, progress=progress,
                                            cancelled=cancelled, log=log, prepared=prepared)
        else:
            res = texcache.Result(False, "Textures: the 3DS will prepare them the first time.")
        if data_version:
            if cancelled():
                raise InterruptedError("cancelled")
            ver = texcache.deliver_data_versions(sd_root, product, folder, work, parts, log=log)
            res.message += " " + ver.message
            res.files += ver.files
        install_record.write(sd_root, product, [it.dst for it in items] + list(res.files), cancelled)
    except InterruptedError:
        raise DeliverError("Copy cancelled.\nClick Copy to card to try again.")
    except OSError as e:
        raise write_error(sd_root, e) from e
    return copied, skipped, nbytes, res


def copy_with_textures(sd_root, items, product, folder, work, parts=None, progress=None, cancelled=lambda: False,
                       log=lambda s: None, textures=True, tool=None, data_version=True):
    """Stage and verify a whole installation, then publish with rollback backups and a recovery journal.

    Nothing in the installed set changes on staging failure. Publication errors restore the old set. If the
    card disappears before rollback, retry this copy before launching the game: recovery runs first.
    """
    import texcache
    items = list(items)
    transaction = sd_transaction.Transaction(sd_root, product)
    try:
        if cancelled():
            raise InterruptedError('cancelled')
        transaction.recover()
        prepared = None
        if textures:
            tool = tool or texcache.find_tool(work, log=log)
            if tool.ok:
                prepared = texcache.prepare(tool, product, folder, work, parts, log=log)
        sets = prepared[0] if prepared else []
        staged = [dataclasses.replace(it, skip=False) for it in items]
        # Texture/receipt-only callers may keep an existing verified data.win in place.
        # Stage a private copy too, so receipt verification never trusts an absent/new destination.
        explicit = {it.dst for it in staged}
        for src, rel in texcache.data_wins(product, folder, parts):
            name = product.sd_data_dir + '/' + (rel + '/' if rel else '') + 'data.win'
            final = install_record.target(sd_root, name)
            if name not in explicit and final.is_file() and texcache.checked_digest(final) == texcache._digest(src):
                staged.append(CopyItem(str(final), name, final.stat().st_size))
        check_copy_space(sd_root, staged, sets, product=product)
        removals = [plan.cs.sd_dir + '/' + name for plan in texcache.plan(sd_root, sets)
                    for name in plan.remove if not name.endswith('.part')]
        transaction.begin()
        result = _copy_payload(str(transaction.new), staged, product, folder, work, parts, progress, cancelled,
                               log, textures, tool, data_version, prepared)
        copied = skipped = nbytes = 0
        for it in items:
            final = install_record.target(sd_root, it.dst)
            new = install_record.target(transaction.new, it.dst)
            if final.is_file() and texcache.checked_digest(final) == texcache.checked_digest(new):
                skipped += 1
            else:
                copied += 1
                nbytes += it.size
        transaction.publish(removals, cancelled)
        return copied, skipped, nbytes, result[3]
    except (OSError, ValueError, KeyError, TypeError, DeliverError) as error:
        try:
            transaction.recover()
        except (OSError, ValueError, KeyError, TypeError):
            # Keep the durable journal/backups for the next run when the card is available.
            pass
        if isinstance(error, DeliverError):
            raise
        if isinstance(error, InterruptedError) or cancelled():
            raise DeliverError("Copy cancelled.\nClick Copy to card to try again.") from error
        raise write_error(sd_root, error) from error


def dsp_missing(sd_root):
    return not os.path.isfile(os.path.join(sd_root, "3ds", "dspfirm.cdc"))


@dataclasses.dataclass
class SavedFiles:
    root: str
    files: int
    bytes: int
    textures: object
    folder: str = None  # Player-facing wrapper; root stays the internal SD layout for FTP.


FOLDER_README = '''READ THIS BEFORE COPYING

Use the builder's Copy to SD card option. It merges individual files and keeps
your saves, settings, other homebrew and other games.

SD-CARD-FILES contains the files for the card. Do not drag a whole 3ds folder
onto an existing 3ds folder and choose Replace in Finder. Replace can delete
the existing folder, including other homebrew and saves.

For manual copying on Mac: hold Option while dragging a folder to a folder
with the same name, then choose Merge. If Finder offers only Stop or Replace,
choose Stop and use Copy to SD card instead. Never choose Replace for a folder.
Replacing an individual build or game file is different; never replace saves.
'''


def sd_preview(sd_root, product, artifacts, folder, work, parts=None):
    """Read the card, prepare textures in work, and list each prospective card write."""
    import texcache
    items = plan_sd_copy(sd_root, product, artifacts, folder, parts)
    tool = texcache.find_tool(work, log=lambda text: None)
    changes = {item.dst: 'Keep' if item.skip else 'Replace' if os.path.exists(
        os.path.join(sd_root, *item.dst.split('/'))) else 'Add' for item in items}
    if tool.ok:
        sets, problems = texcache.prepare(tool, product, folder, work, parts, log=lambda text: None)
        check_copy_space(sd_root, items, sets, product=product)
        for plan in texcache.plan(sd_root, sets):
            if plan.keep:
                continue
            for page in plan.write_pages:
                rel = plan.cs.sd_dir + '/' + texcache.page_name(page)
                changes[rel] = 'Replace' if os.path.exists(os.path.join(sd_root, *rel.split('/'))) else 'Add'
            changes[plan.cs.sd_dir + '/complete'] = 'Replace'
            for name in plan.remove:
                changes[plan.cs.sd_dir + '/' + name] = 'Remove stale texture page'
    else:
        problems = [tool.message]
    for _src, rel in texcache.data_wins(product, folder, parts):
        changes[texcache.sd_cache_dir(product, rel) + '/data-version.txt'] = 'Write after checking data.win'
    changes[product.sd_data_dir + '/install.json'] = 'Write after checking files'
    text = ('Only these files will be written or replaced. Saves, settings, other homebrew and other games are kept.\n\n' +
            '\n'.join('%s: %s' % (action, path) for path, action in sorted(changes.items())))
    if problems:
        text += '\n\nTextures: the 3DS will prepare any missing pages the first time.\n' + '\n'.join(problems)
    return items, tool, text


def save_files(product, artifacts, folder, work, parts=None, progress=None, cancelled=lambda: False,
               log=lambda s: None, textures=True, tool=None, data_version=True):
    """A fresh SD layout on the PC, including the same rounded cache and version markers as SD delivery.

    Never reuse a previous selection's directory: an unselected chapter must not remain playable. A failed
    export keeps its .part directory for inspection, and is never offered as a finished output.
    """
    import tempfile
    parent = os.path.join(work, "out", "files")
    os.makedirs(parent, exist_ok=True)
    wrapper = tempfile.mkdtemp(prefix=product.key + "-3ds-install-", suffix=".part", dir=parent)
    staging = os.path.join(wrapper, 'SD-CARD-FILES')
    os.makedirs(staging)
    os.makedirs(os.path.join(staging, "3ds"))
    try:
        items = plan_sd_copy(staging, product, artifacts, folder, parts)
        _, _, _, tex = copy_with_textures(staging, items, product, folder, work, parts, progress, cancelled,
                                         log=log, textures=textures, tool=tool, data_version=data_version)
    except (DeliverError, OSError) as exc:
        log(str(exc))
        if cancelled() or isinstance(exc, InterruptedError):
            raise DeliverError("Save cancelled. Click Save files to try again.") from exc
        raise DeliverError("Could not save files. Check the output folder's permissions and free space.") from exc
    if cancelled():
        raise DeliverError("Save cancelled. Click Save files to try again.")
    count = size = 0
    for root, _, names in os.walk(staging):
        for name in names:
            size += os.path.getsize(os.path.join(root, name))
            count += 1
    with open(os.path.join(wrapper, 'READ-ME-FIRST.txt'), 'w', encoding='utf-8', newline='\n') as readme:
        readme.write(FOLDER_README)
    destination = wrapper[:-len(".part")]
    os.replace(wrapper, destination)
    return SavedFiles(os.path.join(destination, 'SD-CARD-FILES'), count, size, tex, destination)


def file_copy_steps(product, outputs):
    lines = []
    if '3dsx' in outputs:
        lines.append('Copy .3dsx to sd:/%s/.' % product.sd_app_dir())
    if 'cia' in outputs:
        lines.append('Copy .cia to sd:/cia/.')
    lines.append('Copy game files to sd:/%s/.' % product.sd_data_dir)
    return '\n'.join(lines)


def instructions(product: products.Product, targets, outputs, sd_root=None,
                 saves_result: SaveCopyResult | None = None, save_result: SaveCopyResult | None = None,
                 delivery=None):
    saves_result = saves_result or save_result
    where = (sd_root or "sd:").replace("\\", "/").rstrip("/")
    steps = []
    if sd_root:
        steps.append("Eject the SD card and put it in your 3DS.")
    elif delivery == 'files':
        steps.append('Read READ-ME-FIRST.txt in the saved folder. Use Copy to SD card; never replace a whole folder in Finder.')
        steps.append('Put the SD card in your 3DS.')
    elif delivery == 'wifi':
        steps.append('Files sent to the SD card. Close ftpd.')
    else:
        steps.extend(file_copy_steps(product, outputs).splitlines())
        steps.append('Put the SD card in your 3DS.')
    if "3dsx" in outputs:
        stems = " or ".join(products.package_stem(product, t) + ".3dsx" for t in targets)
        steps.append("Open the Homebrew Launcher and start %s." % stems)
    if "cia" in outputs:
        steps.append("Install the .cia from %s/cia with FBI, then start it from the HOME Menu." % where)
    steps.append("Press SELECT in-game for display and control options.")
    lines = ['What to do now:'] + ['%d. %s' % (i, step) for i, step in enumerate(steps, 1)]
    if saves_result and saves_result.copied:
        lines.append("Saves: %d file(s) copied to sd:/%s/." % (saves_result.copied, product.sd_save_dir()))
    else:
        lines.append("Saves: sd:/%s/." % product.sd_save_dir())
    lines.append("Personal build from your copy of the game. Don't share it.")
    return "\n".join(lines)


# ---------------------------------------------------------------------------------------------------------------------
# FTP-only Install over Wi-Fi

def threedslink_argv(threedslink, path, address=None):
    """Send-only: 3dslink broadcasts to find the Homebrew Launcher's netloader (press Y in HBL) unless the user typed
    an address. Never -s (server mode needs an inbound firewall rule) and never a separate probe."""
    argv = [threedslink]
    if address:
        argv += ["-a", address]
    return argv + [path]


def send_3dslink(threedslink, path, address=None, run=None, log=print):
    """Retained API guard for old callers: netloader delivery is no longer allowed."""
    raise DeliverError("Sending with the Homebrew Launcher's netloader is no longer supported. Use Install over Wi-Fi with ftpd.")


def wifi_items(product, artifacts, folder, parts=None):
    items = [CopyItem(src, product.sd_data_dir + '/' + rel, size)
             for src, rel, size in products.sd_files(product, folder, parts)]
    for artifact in unique_artifacts(artifacts):
        directory = ('cia' if artifact.kind == 'cia' else product.sd_data_dir
                     if artifact.kind == 'sfxpack' else product.sd_app_dir())
        items.append(CopyItem(artifact.path, directory + '/' + os.path.basename(artifact.path), artifact.size))
    return items


def unique_artifacts(artifacts):
    """Both model builds return the same shared pack; install it once."""
    seen = set()
    for artifact in artifacts:
        key = (artifact.kind, artifact.path)
        if key not in seen:
            seen.add(key)
            yield artifact


def send_ftp(host, items, port=5000, ftp_factory=None, progress=None, log=print):
    """Compatibility entry point; all callers use the verified FTP-only path."""
    import ftp_delivery
    kwargs = {}
    if ftp_factory is not None:
        kwargs['factory'] = ftp_factory
    if progress:
        kwargs['progress'] = lambda event: progress(event.done, event.total, event.path)
    ftp_delivery.send('%s:%d' % (host, port), items, **kwargs)
    log("Sent and checked %d files." % len(items))
