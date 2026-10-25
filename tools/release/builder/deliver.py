"""Wizard page 7: put the personal build and the user's own game files where the 3DS can use them.

Copy to SD card (default), Save as files, or Send over Wi-Fi. Wi-Fi sending is off unless the builder runs with
--enable-wifi-send: it uses the official 3dslink client in send-only mode (never -s, never a probe of the console;
a bare connect to the netloader's port crashes it) and FTP to the console's ftpd for the game files.
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


def plan_sd_copy(sd_root, product: products.Product, artifacts, game_folder, parts=None):
    """What goes where. .3dsx -> sd:/3ds/<app>/, .cia -> sd:/cia/, game files -> the runtime's data folder.
    Files already there with the same size and time are skipped."""
    if not os.path.isdir(os.path.join(sd_root, "3ds")):
        raise DeliverError(NO_3DS_FOLDER_MESSAGE)
    check_sd_writable(sd_root)
    items = []
    for a in artifacts:
        rel = ("cia/" if a.kind == "cia" else product.sd_app_dir() + "/") + os.path.basename(a.path)
        items.append(CopyItem(a.path, rel, a.size))
    for src, rel, size in products.sd_files(product, game_folder, parts):
        if "cache_exact" in rel.casefold().split("/"):
            continue  # Exact-colour caches are prepared only by the console.
        items.append(CopyItem(src, product.sd_data_dir + "/" + rel, size))
    for it in items:
        d = os.path.join(sd_root, *it.dst.split("/"))
        try:
            st, ss = os.stat(d), os.stat(it.src)
            it.skip = st.st_size == ss.st_size and abs(st.st_mtime - ss.st_mtime) <= FAT_SLACK
        except OSError:
            it.skip = False
    need = sum(it.size for it in items if not it.skip)
    free = shutil.disk_usage(sd_root).free
    if need > free:
        need_mb = -(-need // 10**6)
        raise DeliverError("This card has %d MB free; %d MB needed.\n"
                           "Free up space on the SD card and try again." % (free // 10**6, need_mb))
    return items


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
        if it.skip:
            skipped += 1
            continue
        d = os.path.join(sd_root, *it.dst.split("/"))
        tmp = d + ".part"
        try:
            os.makedirs(os.path.dirname(d), exist_ok=True)
            shutil.copy2(it.src, tmp)
            os.replace(tmp, d)
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


def copy_with_textures(sd_root, items, product, folder, work, parts=None, progress=None, cancelled=lambda: False,
                       log=lambda s: None, textures=True, tool=None, data_version=True):
    """copy_items, then the texture cache folders prepared on the PC (texcache.py). Returns (copied, skipped, bytes,
    texcache.Result). Preparing is optional: if it can't run, the 3DS prepares the textures as before. A failed write
    to the card is an error like any other copy. Then each data.win's cache/data-version.txt (texcache.py), so the 3DS
    skips hashing the data.win on its first start; its message is appended to the Result's."""
    import texcache
    copied, skipped, nbytes = copy_items(sd_root, items, progress, cancelled)
    try:
        if textures:
            res = texcache.deliver_textures(sd_root, product, folder, work, parts, tool=tool, progress=progress,
                                            cancelled=cancelled, log=log)
        else:
            res = texcache.Result(False, "Textures: the 3DS will prepare them the first time.")
        if data_version:
            if cancelled():
                raise InterruptedError("cancelled")
            ver = texcache.deliver_data_versions(sd_root, product, folder, work, parts, log=log)
            res.message += " " + ver.message
    except InterruptedError:
        raise DeliverError("Copy cancelled.\nClick Copy to card to try again.")
    except OSError as e:
        raise write_error(sd_root, e) from e
    return copied, skipped, nbytes, res


def dsp_missing(sd_root):
    return not os.path.isfile(os.path.join(sd_root, "3ds", "dspfirm.cdc"))


@dataclasses.dataclass
class SavedFiles:
    root: str
    files: int
    bytes: int
    textures: object


def save_files(product, artifacts, folder, work, parts=None, progress=None, cancelled=lambda: False,
               log=lambda s: None, textures=True, tool=None, data_version=True):
    """A fresh SD layout on the PC, including the same rounded cache and version markers as SD delivery.

    Never reuse a previous selection's directory: an unselected chapter must not remain playable. A failed
    export keeps its .part directory for inspection, and is never offered as a finished output.
    """
    import tempfile
    parent = os.path.join(work, "out", "files")
    os.makedirs(parent, exist_ok=True)
    staging = tempfile.mkdtemp(prefix=product.key + "-", suffix=".part", dir=parent)
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
    destination = staging[:-len(".part")]
    os.replace(staging, destination)
    return SavedFiles(destination, count, size, tex)


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
    where = sd_root or "sd:"
    steps = []
    if sd_root:
        steps.append("Eject the SD card and put it in your 3DS.")
    elif delivery == 'files':
        steps.append('Copy the contents of the saved output folder to your SD card.')
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
# Send over Wi-Fi (behind --enable-wifi-send)

def threedslink_argv(threedslink, path, address=None):
    """Send-only: 3dslink broadcasts to find the Homebrew Launcher's netloader (press Y in HBL) unless the user typed
    an address. Never -s (server mode needs an inbound firewall rule) and never a separate probe."""
    argv = [threedslink]
    if address:
        argv += ["-a", address]
    return argv + [path]


def send_3dslink(threedslink, path, address=None, run=None, log=print):
    def capture(argv):
        result = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, errors='replace', timeout=600, **hidden_options())
        if result.stdout:
            log(result.stdout)
        return result.returncode
    run = run or capture
    argv = threedslink_argv(threedslink, path, address)
    log("running " + " ".join(argv))
    rc = run(argv)
    if rc != 0:
        raise DeliverError("3dslink failed (exit %d).\n"
                           "Press Y in the Homebrew Launcher, then try again." % rc)


def wifi_items(product, artifacts, folder, parts=None):
    items = [CopyItem(src, product.sd_data_dir + '/' + rel, size)
             for src, rel, size in products.sd_files(product, folder, parts)]
    for artifact in artifacts:
        directory = 'cia' if artifact.kind == 'cia' else product.sd_app_dir()
        items.append(CopyItem(artifact.path, directory + '/' + os.path.basename(artifact.path), artifact.size))
    return items


def send_ftp(host, items, port=5000, ftp_factory=ftplib.FTP, progress=None, log=print):
    """Game files to a running ftpd on the console. Console Wi-Fi is slow: about 156 MB for UNDERTALE can take
    10-20 minutes."""
    ftp = ftp_factory()
    ftp.connect(host, port, timeout=30)
    try:
        ftp.login()
        made = set()
        total = sum(it.size for it in items)
        done = 0
        for it in items:
            parts = it.dst.split("/")
            for i in range(1, len(parts)):
                d = "/" + "/".join(parts[:i])
                if d in made:
                    continue
                try:
                    ftp.mkd(d)
                except ftplib.error_perm:
                    pass
                made.add(d)
            remote = "/" + it.dst
            try:
                if ftp.size(remote) == it.size:
                    done += it.size
                    continue
            except ftplib.all_errors:
                pass
            with open(it.src, "rb") as f:
                ftp.storbinary("STOR " + remote, f, blocksize=1 << 16)
            done += it.size
            if progress:
                progress(done, total, it.dst)
        log("sent %d files to %s:%d" % (len(items), host, port))
    finally:
        try:
            ftp.quit()
        except ftplib.all_errors:
            ftp.close()
