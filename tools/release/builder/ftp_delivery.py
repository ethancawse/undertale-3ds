"""FTP-only console delivery. Importing this module never opens a socket.

Discovery is opt-in, read-only, bounded to connected local IPv4 subnets and port
5000. Uploads use verified resumable temporary files and publish receipts last.
No netloader, broadcast, or 3dslink is used.
"""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor, wait, FIRST_COMPLETED
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
import ftplib
import errno
import hashlib
import io
import ipaddress
import json
import os
import re
import subprocess
import sys
import threading
import time

from processes import hidden_options

BLOCK = 65536
SMALL = 1024 * 1024
FORBIDDEN_PORT = 17491


class TransferError(OSError):
    pass


class Cancelled(TransferError):
    pass


def delivery_error(exc, *, connecting=False):
    """Keep actionable validation errors; translate only known transport failures."""
    if isinstance(exc, TransferError):
        return exc
    if isinstance(exc, ftplib.Error) and str(exc)[:3] in ('452', '552'):
        return TransferError('The 3DS SD card is full. Free up space on it, then click Send.')
    network_errno = {errno.ECONNREFUSED, errno.ECONNRESET, errno.ECONNABORTED,
                     errno.ENETUNREACH, errno.EHOSTUNREACH, errno.ETIMEDOUT, errno.EPIPE}
    disconnected = (isinstance(exc, (TimeoutError, ConnectionError, EOFError))
                    or isinstance(exc, OSError) and (
                        exc.errno in network_errno or getattr(exc, 'winerror', None)
                        in (10051, 10053, 10054, 10060, 10061, 10065))
                    or isinstance(exc, ftplib.Error) and str(exc)[:3] in ('421', '425', '426'))
    if disconnected:
        if connecting:
            return TransferError("Can't reach a 3DS at this address. Check that ftpd is open and the 3DS is on the same network as this PC, then try again.")
        return TransferError('Lost the connection to the 3DS. Check that ftpd is still open, then click Send to pick up where it left off.')
    return exc


def error_text(exc, *, connecting=False):
    return str(delivery_error(exc, connecting=connecting))


@contextmanager
def friendly_errors(*, connecting=False):
    try:
        yield
    except (OSError, EOFError, ftplib.Error) as exc:
        translated = delivery_error(exc, connecting=connecting)
        if translated is exc:
            raise
        raise translated from exc


def endpoint(value, port=5000):
    """A literal IPv4 address, optionally followed by :port. Never resolve a host."""
    try:
        host, sep, typed = value.strip().partition(':')
        address = ipaddress.IPv4Address(host)
        port = int(typed) if sep else int(port)
        if not 1 <= port <= 65535 or port == FORBIDDEN_PORT or address.is_multicast or address.is_unspecified:
            raise ValueError()
        return str(address), port
    except (ValueError, TypeError):
        raise TransferError('Type the address ftpd shows on your 3DS, for example 192.168.1.25:5000.') from None


class ConsoleFTP(ftplib.FTP):
    def connect(self, host='', port=0, timeout=-999, source_address=None):
        host, port = endpoint(host, port or 5000)
        return super().connect(host, port, timeout, source_address)

    def makepasv(self):
        host, port = super().makepasv()
        if port == FORBIDDEN_PORT:
            raise TransferError("ftpd picked a port the builder can't use. Close and reopen ftpd, then try again.")
        return host, port


def check_cancel(cancelled):
    if cancelled():
        raise Cancelled('Stopped. Nothing is lost: a new send picks up where this one left off.')


def identify(ftp):
    # Console ftpd can greet with only "220 Hello!". Identify the SD layout.
    rows = list(ftp.mlsd('/', facts=['type']))
    folders = {name.casefold() for name, facts in rows if facts.get('type') == 'dir'}
    if not {'3ds', 'nintendo 3ds'} <= folders:
        raise TransferError("This ftpd isn't showing a 3DS SD card. Check that the address is your 3DS's and its SD card is in.")


def disconnect(ftp, *, graceful=True):
    """QUIT only an idle command stream; always close even if QUIT fails."""
    try:
        if graceful and hasattr(ftp, 'quit'):
            ftp.quit()
    except (OSError, EOFError, ftplib.Error):
        pass
    finally:
        ftp.close()


def connect(value, *, factory=ConsoleFTP, timeout=10):
    host, port = endpoint(value)
    ftp = factory()
    try:
        with friendly_errors(connecting=True):
            ftp.connect(host, port, timeout=timeout)
            ftp.login()
            identify(ftp)
            ftp.voidcmd('TYPE I')
        return ftp
    except BaseException:
        ftp.close()
        raise


def local_networks(platform=sys.platform, run=subprocess.run):
    """Read interface addresses locally; no UDP route probe or network discovery."""
    if platform == 'win32':
        argv = ['powershell.exe', '-NoProfile', '-NonInteractive', '-Command',
                'Get-NetIPAddress -AddressFamily IPv4 | Where-Object {$_.AddressState -eq "Preferred"} | '
                'Select-Object IPAddress,PrefixLength | ConvertTo-Json -Compress']
    elif platform == 'darwin':
        argv = ['/sbin/ifconfig']
    else:
        argv = ['ip', '-j', '-4', 'address', 'show', 'up']
    result = run(argv, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                 timeout=15, **hidden_options())
    if result.returncode:
        raise TransferError("Couldn't search your network. Type the address ftpd shows on your 3DS.")
    rows = []
    if platform == 'win32':
        data = json.loads(result.stdout or '[]')
        for item in ([data] if isinstance(data, dict) else data):
            rows.append((item['IPAddress'], item['PrefixLength']))
    elif platform == 'darwin':
        for address, mask in re.findall(r'\binet (\d+\.\d+\.\d+\.\d+) netmask (0x[0-9a-fA-F]+)', result.stdout):
            rows.append((address, ipaddress.IPv4Address(int(mask, 16)).__str__()))
    else:
        for interface in json.loads(result.stdout):
            rows.extend((a['local'], a['prefixlen']) for a in interface.get('addr_info', []) if a.get('family') == 'inet')
    networks = set()
    for host, mask in rows:
        address = ipaddress.IPv4Address(host)
        if address.is_loopback or address.is_link_local or not address.is_private:
            continue
        network = ipaddress.IPv4Network('%s/%s' % (host, mask), strict=False)
        if network.prefixlen < 20:
            continue  # Never silently scan a huge corporate/VPN network.
        networks.add(network)
    return sorted(networks, key=lambda n: (int(n.network_address), n.prefixlen))


def find_consoles(networks, *, allowed, cancelled=lambda: False, progress=lambda done, total: None,
                  factory=ConsoleFTP):
    """Only caller-selected local subnets, <=4094 hosts each, at most 16 probes."""
    allowed = {ipaddress.IPv4Network(n) for n in allowed}
    selected = {ipaddress.IPv4Network(n) for n in networks}
    if not selected <= allowed or any(n.prefixlen < 20 or n.is_loopback for n in selected):
        raise TransferError("Couldn't search your network. Type the address ftpd shows on your 3DS.")
    hosts = sorted({str(h) for n in selected for h in n.hosts()})
    active = set()
    lock = threading.Lock()
    def probe(host):
        if cancelled():
            return None
        ftp = factory()
        with lock:
            active.add(ftp)
        complete = False
        try:
            connect(host + ':5000', factory=lambda: ftp, timeout=0.6)
            check_cancel(cancelled)
            complete = True
            return host + ':5000'
        except (OSError, EOFError, ftplib.Error):
            return None  # Other devices, including AirPlay, are ordinary non-matches.
        finally:
            disconnect(ftp, graceful=complete and not cancelled())
            with lock:
                active.discard(ftp)
    found = []
    with ThreadPoolExecutor(max_workers=16) as workers:
        # Submit in small batches so cancellation never leaves a subnet queued.
        for offset in range(0, len(hosts), 16):
            check_cancel(cancelled)
            pending = {workers.submit(probe, host) for host in hosts[offset:offset+16]}
            while pending:
                if cancelled():
                    with lock:
                        for ftp in active:
                            ftp.close()
                    for future in pending:
                        future.cancel()
                    check_cancel(cancelled)
                done, pending = wait(pending, timeout=0.1, return_when=FIRST_COMPLETED)
                for future in done:
                    hit = future.result()
                    if hit:
                        found.append(hit)
            progress(min(offset+16, len(hosts)), len(hosts))
    check_cancel(cancelled)
    return sorted(found)


def remote_path(value):
    value = str(value).replace('\\', '/')
    if any(c in value for c in '\r\n\x00') or any(p in ('..', '.') for p in value.split('/')):
        raise TransferError('Invalid destination path.')
    path = PurePosixPath('/' + value.lstrip('/'))
    if len(path.parts) < 2 or ':' in value:
        raise TransferError('Invalid destination path.')
    return str(path)


def size(ftp, path):
    try:
        return ftp.size(path)
    except ftplib.error_perm as exc:
        if str(exc)[:3] not in ('550', '553'):
            raise
        return None


def delete_if_present(ftp, path):
    if size(ftp, path) is not None:
        try:
            ftp.delete(path)
        except ftplib.error_perm as exc:
            # The file may disappear after SIZE, including on a retry.
            if str(exc)[:3] not in ('550', '553'):
                raise


def fingerprint(ftp, path):
    parent, name = path.rsplit('/', 1)
    rows = [facts for entry, facts in ftp.mlsd(parent or '/', facts=['type', 'size', 'modify']) if entry == name]
    if len(rows) != 1 or rows[0].get('type') != 'file':
        raise TransferError("Couldn't check this file on the 3DS. Send again: " + path)
    stamp = rows[0].get('modify', '')
    if (not re.fullmatch(r'\d{14}(?:\.\d+)?', stamp) or stamp.startswith(('19700101', '00000000'))):
        raise TransferError('ftpd isn\'t sending file dates. In ftpd\'s settings, turn on "Get mtime", then send again.')
    count = size(ftp, path)
    if rows[0].get('size') != str(count):
        raise TransferError('A file on the 3DS changed during the check. Send again: ' + path)
    return count, stamp


@dataclass(frozen=True)
class Progress:
    phase: str
    path: str
    done: int
    total: int
    speed: float
    seconds_left: float | None


class Transfer:
    def __init__(self, ftp, *, cancelled=lambda: False, progress=lambda event: None, install=True):
        self.ftp, self.cancelled, self.progress = ftp, cancelled, progress
        self.install = install
        self.total = self.done = self.wire = 0
        self.started = time.monotonic()
        self.folders = set()
        self.verified_data = {}

    def tick(self, phase, path, count=0, wire=True):
        check_cancel(self.cancelled)
        self.done += count
        self.wire += count if wire else 0
        elapsed = time.monotonic() - self.started
        speed = self.wire / max(elapsed, 0.001)
        self.progress(Progress(phase, path, self.done, self.total, speed,
                               max(0, self.total-self.done)/speed if speed else None))

    def mkdirs(self, path):
        parts = PurePosixPath(path).parent.parts[1:]
        for i in range(1, len(parts)+1):
            folder = '/' + '/'.join(parts[:i])
            if folder not in self.folders:
                try:
                    self.ftp.mkd(folder)
                except ftplib.error_perm as exc:
                    if not str(exc).startswith('550'):
                        raise
                    # Confirm that an existing path is a directory, not an unrelated error.
                    previous = self.ftp.pwd()
                    self.ftp.cwd(folder)
                    self.ftp.cwd(previous)
                self.folders.add(folder)

    def hash_remote(self, path, limit=None, phase='Checking'):
        digest, count = hashlib.sha256(), 0
        def take(block):
            nonlocal count
            count += len(block)
            if limit is not None and count > limit:
                raise TransferError('A file on the 3DS changed during the check. Send again: ' + path)
            digest.update(block)
            self.tick(phase, path, len(block))
        self.ftp.retrbinary('RETR ' + path, take, blocksize=BLOCK)
        return digest.hexdigest(), count

    def upload(self, item):
        source, path = Path(item.src), remote_path(item.dst)
        initial = source.stat()
        if initial.st_size != item.size:
            raise TransferError('A build file on this PC changed. Build again, then send.')
        data_file = self.install and PurePosixPath(path).name.casefold() == 'data.win'
        receipt_for = None
        if self.install and PurePosixPath(path).name == 'data-version.txt':
            import texcache
            receipt_for = str(PurePosixPath(path).parent.parent / 'data.win')
            proof = self.verified_data.get(receipt_for)
            if proof is None:
                raise TransferError('The files were sent in the wrong order. Send again.')
            identity, expected_hash, expected_size = proof
            expected_text = texcache.data_version_text(texcache.data_versions_revision(digest=expected_hash),
                                                       expected_size, expected_hash, 'sdmc:' + receipt_for)
            if source.read_bytes() != expected_text or fingerprint(self.ftp, receipt_for) != identity:
                raise TransferError('The game file on the 3DS changed during sending. Send again.')
        critical = (data_file or item.size <= SMALL or PurePosixPath(path).name == 'install.json'
                    or PurePosixPath(path).suffix.casefold() in ('.3dsx', '.cia', '.dtsp'))
        local_hash = hashlib.sha256()
        if critical:
            with source.open('rb') as stream:
                while block := stream.read(BLOCK):
                    check_cancel(self.cancelled)
                    local_hash.update(block)
        digest = local_hash.hexdigest()
        self.mkdirs(path)
        if data_file:
            delete_if_present(self.ftp, str(PurePosixPath(path).parent / 'cache/data-version.txt'))
        existing = size(self.ftp, path)
        if existing == item.size:
            before = fingerprint(self.ftp, path) if data_file else None
            if not critical or self.hash_remote(path, item.size) == (digest, item.size):
                if data_file and fingerprint(self.ftp, path) != before:
                    raise TransferError('A file on the 3DS changed during the check. Send again: ' + path)
                if data_file:
                    self.verified_data[path] = before, digest, item.size
                after = source.stat()
                if (initial.st_size, initial.st_mtime_ns, initial.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino):
                    raise TransferError('A build file on this PC changed. Build again, then send.')
                if receipt_for and fingerprint(self.ftp, receipt_for) != self.verified_data[receipt_for][0]:
                    delete_if_present(self.ftp, path)
                    raise TransferError('A file on the 3DS changed at the last step. Send again.')
                self.tick('Already on the 3DS', path, item.size, wire=False)
                return
            self.total += item.size  # A failed same-size read-back precedes a new full copy.
        # A stale version receipt must never survive a data replacement or interrupted upload.
        if data_file:
            delete_if_present(self.ftp, str(PurePosixPath(path).parent / 'cache/data-version.txt'))
        temporary = path + '.builder-part'
        offset = size(self.ftp, temporary) or 0
        if offset > item.size:
            self.ftp.delete(temporary)
            offset = 0
        if offset:
            # Authenticate a previous partial file before asking FTP to resume it.
            prefix = hashlib.sha256()
            with source.open('rb') as stream:
                remaining = offset
                while remaining:
                    check_cancel(self.cancelled)
                    block = stream.read(min(BLOCK, remaining))
                    if not block:
                        raise TransferError('A build file on this PC changed. Build again, then send.')
                    prefix.update(block)
                    remaining -= len(block)
            self.total += offset  # Prefix read-back is extra real work.
            if self.hash_remote(temporary, offset) != (prefix.hexdigest(), offset):
                self.ftp.delete(temporary)
                offset = 0
        self.tick('Sending', path, offset, wire=False)
        with source.open('rb') as stream:
            stream.seek(offset)
            try:
                self.ftp.storbinary('STOR ' + temporary, stream, blocksize=BLOCK,
                                   callback=lambda b: self.tick('Sending', path, len(b)), rest=offset or None)
            except ftplib.error_perm as exc:
                if not offset or str(exc).startswith('552'):
                    raise
                # Servers without REST still support a clean full-file retry on the next send.
                self.ftp.delete(temporary)
                raise TransferError('ftpd could not resume this file. Send again to copy it from the start.') from None
        if size(self.ftp, temporary) != item.size:
            raise TransferError('A file arrived incomplete. Send again: ' + path)
        before = fingerprint(self.ftp, temporary) if data_file else None
        if critical and self.hash_remote(temporary, item.size) != (digest, item.size):
            self.ftp.delete(temporary)
            raise TransferError('A file arrived damaged. Send again: ' + path)
        if data_file and fingerprint(self.ftp, temporary) != before:
            raise TransferError('A file on the 3DS changed during the check. Send again: ' + path)
        after = source.stat()
        if (initial.st_size, initial.st_mtime_ns, initial.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino):
            raise TransferError('A build file on this PC changed. Build again, then send.')
        check_cancel(self.cancelled)
        # ftpd may reject RNTO over an existing file. The verified .part survives a power-off in this gap.
        delete_if_present(self.ftp, path)
        self.ftp.rename(temporary, path)
        if size(self.ftp, path) != item.size:
            raise TransferError('A file on the 3DS has the wrong size. Send again: ' + path)
        if data_file:
            identity = fingerprint(self.ftp, path)
            if identity != before:
                raise TransferError('A file on the 3DS changed at the last step. Send again: ' + path)
            self.verified_data[path] = identity, digest, item.size
        if receipt_for and fingerprint(self.ftp, receipt_for) != self.verified_data[receipt_for][0]:
            delete_if_present(self.ftp, path)
            raise TransferError('A file on the 3DS changed at the last step. Send again.')

    def send(self, items):
        check_cancel(self.cancelled)
        items = list(items)
        # Data -> pages -> complete -> version receipts -> install record.
        def order(it):
            name = PurePosixPath(it.dst).name
            return (4 if name == 'install.json' else 3 if name == 'data-version.txt' else
                    2 if name == 'complete' else 0 if name == 'data.win' else 1,
                    it.dst)
        items.sort(key=order)
        self.total = sum(it.size * (2 if it.size <= SMALL or PurePosixPath(it.dst).suffix.casefold() in ('.3dsx', '.cia', '.dtsp')
                                    or PurePosixPath(it.dst).name.casefold() in ('data.win', 'install.json')
                                    else 1) for it in items)
        for it in items:
            if self.install and PurePosixPath(it.dst).name in ('complete', 'install.json'):
                delete_if_present(self.ftp, remote_path(it.dst))
        for it in items:
            check_cancel(self.cancelled)
            self.upload(it)
        self.tick('Finished', '', max(0, self.total-self.done), wire=False)


def send(value, items, *, factory=ConsoleFTP, cancelled=lambda: False, progress=lambda event: None):
    ftp = connect(value, factory=factory)
    try:
        with friendly_errors():
            Transfer(ftp, cancelled=cancelled, progress=progress).send(items)
    finally:
        # close rather than QUIT: cancellation/failure can leave a pending data response.
        ftp.close()


def read_install_record(ftp, product, cancelled=lambda: False):
    """Read only bounded installer metadata from the already identified console."""
    import install_record
    path = remote_path(install_record.relative_path(product))
    length = size(ftp, path)
    if length is None or length > install_record.MAX_BYTES:
        return None
    chunks, count = [], 0
    def take(block):
        nonlocal count
        check_cancel(cancelled)
        count += len(block)
        if count > install_record.MAX_BYTES:
            raise TransferError('A file on the 3DS changed during the check. Send again: ' + path)
        chunks.append(block)
    try:
        ftp.retrbinary('RETR ' + path, take, blocksize=BLOCK)
    except ftplib.error_perm as exc:
        if str(exc)[:3] in ('550', '553'):
            return None
        raise
    return install_record.parse(b''.join(chunks), product)


def send_product(value, product, artifacts, folder, work, parts=None, *, cancelled=lambda: False,
                 progress=lambda event: None, log=lambda message: None):
    """Use the ordinary destination-verified PC layout, including textures and receipts."""
    import deliver
    check_cancel(cancelled)
    # Reject a mistyped address before preparing a potentially large local layout.
    probe = connect(value)
    probe.close()
    saved = deliver.save_files(product, artifacts, folder, work, parts, cancelled=cancelled, log=log)
    import install_record
    ftp = connect(value)
    try:
        with friendly_errors():
            previous = read_install_record(ftp, product, cancelled)
            current = install_record.read(saved.root, product)
            if current is None:
                raise ValueError('Invalid installer inventory path')
            names = [entry['path'] for entry in current['files']]
            # Transfer invalidates these receipts for every data.win it checks.
            removed = [str(PurePosixPath(name).parent / 'cache/data-version.txt')
                       for name in names if PurePosixPath(name).name.casefold() == 'data.win']
            def record_cancelled():
                check_cancel(cancelled)
                return False
            install_record.write(saved.root, product, names, record_cancelled, previous=previous,
                present=lambda name: size(ftp, remote_path(name)) is not None, removed=removed)
            items = [deliver.CopyItem(str(p), p.relative_to(saved.root).as_posix(), p.stat().st_size)
                     for p in Path(saved.root).rglob('*') if p.is_file()]
            Transfer(ftp, cancelled=cancelled, progress=progress).send(items)
    finally:
        ftp.close()
    return saved
