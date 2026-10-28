"""Confirmed FTP save import; verified remote backups precede any replacement."""
from dataclasses import dataclass
from pathlib import Path
import hashlib
import tempfile
import time
import uuid

import ftp_delivery as network
import saves


class SavesChanged(network.TransferError):
    def __init__(self):
        super().__init__('The 3DS saves changed after they were checked. Nothing was replaced. Check the saves again, then retry.')


def error_text(exc, *, connecting=False):
    """The wizard has Back/Next; the CLI keeps the approved retry instruction."""
    if isinstance(exc, SavesChanged):
        return 'The 3DS saves changed after they were checked. Nothing was replaced. Click Back, then Next, to check them again.'
    return network.error_text(exc, connecting=connecting)


@dataclass
class Snapshot:
    files: dict
    player_summary: str


def snapshot(ftp, product, *, cancelled=lambda: False):
    folder = network.remote_path(product.sd_save_dir())
    try:
        rows = list(ftp.mlsd(folder, facts=['type', 'size']))
    except __import__('ftplib').error_perm as exc:
        if not str(exc).startswith('550'):
            raise
        return Snapshot({}, '')
    files = {}
    with tempfile.TemporaryDirectory(prefix='builder-saves-') as temporary:
        for name, facts in rows:
            # Match SD import: back up existing ordinary save-folder files, including unknown files.
            if facts.get('type') != 'file' or name.startswith('.') or name.endswith(('.tmp', '.part', '.save-part', '.builder-part')):
                continue
            if '/' in name or '\\' in name or name in ('..', '.'):
                raise network.TransferError('A file in the 3DS saves folder has an unusual name. Copy the saves by SD card instead.')
            path = folder + '/' + name
            size = network.size(ftp, path)
            if size is None or size > saves.MAX_SAVE_BYTES:
                raise network.TransferError('A file in the 3DS saves folder is too large to back up over Wi-Fi. Nothing was replaced. Copy the saves by SD card instead.')
            buffer = bytearray()
            def take(block):
                network.check_cancel(cancelled)
                buffer.extend(block)
                if len(buffer) > saves.MAX_SAVE_BYTES:
                    raise SavesChanged()
            ftp.retrbinary('RETR ' + path, take)
            if len(buffer) != size or network.size(ftp, path) != size:
                raise SavesChanged()
            files[name] = bytes(buffer)
            (Path(temporary)/name).write_bytes(buffer)
        summary = saves.scan_saves(temporary, product.key).player_summary
    return Snapshot(files, summary)


def inspect(value, product, *, factory=network.ConsoleFTP, cancelled=lambda: False):
    ftp = network.connect(value, factory=factory)
    try:
        with network.friendly_errors():
            return snapshot(ftp, product, cancelled=cancelled)
    finally:
        ftp.close()


def copy(value, product, scan, expected, *, overwrite=False, factory=network.ConsoleFTP,
         cancelled=lambda: False, progress=lambda event: None):
    if scan.has_corrupt or not scan.valid_files:
        raise network.TransferError('No readable PC saves to copy, so nothing was copied. Choose another folder or skip.')
    if expected.files and not overwrite:
        raise network.TransferError('Existing 3DS saves were found. Nothing was copied. Add --overwrite-saves to back them up and replace them.')
    ftp = network.connect(value, factory=factory)
    folder = network.remote_path(product.sd_save_dir())
    transfer = network.Transfer(ftp, cancelled=cancelled, progress=progress, install=False)
    try:
        current = snapshot(ftp, product, cancelled=cancelled)
        if current.files != expected.files:
            raise SavesChanged()
        from deliver import CopyItem
        backup = folder + '/backup-' + time.strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8]
        with tempfile.TemporaryDirectory(prefix='builder-save-copy-') as temporary:
            items = []
            for name, data in expected.files.items():
                source = Path(temporary)/name
                source.write_bytes(data)
                items.append(CopyItem(str(source), backup+'/'+name, len(data)))
            transfer.send(items)  # All old bytes read back and checked in the backup folder first.
            staged = []
            for source in scan.valid_files:
                network.check_cancel(cancelled)
                path = folder+'/'+source.name
                part = path+'.save-part'
                transfer.total += 2*source.size
                transfer.upload(CopyItem(source.path, part, source.size))
                staged.append((path, part))
            # Ensure the save we confirmed is still the save being replaced.
            if snapshot(ftp, product, cancelled=cancelled).files != expected.files:
                raise SavesChanged()
            network.check_cancel(cancelled)
            for path, part in staged:
                network.delete_if_present(ftp, path)
                ftp.rename(part, path)
            incoming = {s.name for s in scan.valid_files}
            for name in expected.files:
                if name not in incoming:
                    ftp.delete(folder+'/'+name)
            return saves.SaveCopyResult(len(staged), sum(s.size for s in scan.valid_files),
                                         backup if expected.files else None,
                                         [s.name for s in scan.valid_files], len(expected.files))
    except (OSError, EOFError, __import__('ftplib').Error) as exc:
        translated = network.delivery_error(exc)
        if translated is exc:
            raise
        raise translated from exc
    finally:
        ftp.close()
