"""Recoverable SD update of explicit installer files; saves and unknown files never participate.

Stage the whole new set on the card before changing final names. A durable journal
and rename backups allow ordinary failures to roll back, and a later run to recover
after card removal/power loss. FAT does not offer a multi-file atomic rename: don't
start the game while the copy is running or before an interrupted copy is retried.
"""
import errno
import json
import os
from pathlib import Path
import shutil
import stat

import install_record

# Conservative FAT32 allocation, including cards formatted with 64 KiB clusters.
CLUSTER = 65536


def space_required(sizes, paths=()):
    files = sum(max(1, (size + CLUSTER - 1) // CLUSTER) * CLUSTER for size in sizes)
    # Inventory and the journal have bounded per-file records, plus rename/dir entries.
    names = list(paths)
    metadata = 1024 + sum(512 + len(name.encode('utf-8')) * 8 for name in names)
    return files + ((metadata + CLUSTER - 1) // CLUSTER + len(names) + 8) * CLUSTER


def sync_file(path):
    # Windows FlushFileBuffers requires a writable handle. These are our staged copies only.
    os.chmod(path, os.stat(path).st_mode | stat.S_IWUSR)
    with open(path, 'r+b') as stream:
        os.fsync(stream.fileno())


def sync_directory(path):
    # Windows does not expose directory fsync via os.open. File flushes are mandatory.
    if os.name != 'nt':
        fd = os.open(path, os.O_RDONLY)
        try:
            try:
                os.fsync(fd)
            except OSError as error:
                # Some removable filesystems on macOS do not support directory fsync.
                # Staged file/journal flushes above remain mandatory on every host.
                if error.errno not in (errno.EINVAL, errno.ENOTSUP):
                    raise
        finally:
            os.close(fd)


def replace(source, destination):
    os.replace(source, destination)
    sync_directory(Path(destination).parent)
    if Path(source).parent != Path(destination).parent:
        sync_directory(Path(source).parent)


class Transaction:
    def __init__(self, root, product):
        self.root, self.product = Path(root).resolve(), product
        self.base = install_record.target(self.root, '.' + product.key + '-3ds-builder-update')
        self.new, self.old = self.base / 'new', self.base / 'old'
        self.journal = self.base / 'journal.json'

    def valid_name(self, name):
        return name == install_record.relative_path(self.product) or install_record.managed_path(self.product, name)

    def target(self, root, name):
        if not self.valid_name(name):
            raise OSError('Invalid SD update path')
        return install_record.target(root, name)

    def recover(self):
        if not self.base.exists():
            return
        marker = self.base / 'owner.json'
        if self.base.is_symlink() or marker.is_symlink() or marker.read_bytes() != self.owner():
            raise OSError('Unrecognised SD update folder; keep it and choose another card')
        if self.journal.exists():
            if self.journal.is_symlink() or self.journal.stat().st_size > install_record.MAX_BYTES:
                raise OSError('Invalid SD update journal')
            data = json.loads(self.journal.read_bytes())
            entries = data['entries']
            if data['state'] not in ('publishing', 'committed') or not isinstance(entries, list):
                raise OSError('Invalid SD update journal')
            seen = set()
            for entry in entries:
                name = entry['path']
                if (not self.valid_name(name) or name.casefold() in seen
                        or type(entry['existed']) is not bool or type(entry['new']) is not bool):
                    raise OSError('Invalid SD update journal')
                seen.add(name.casefold())
                for root in (self.root, self.old, self.new):
                    self.target(root, name)  # Reject redirected parents before any recovery writes.
            if data['state'] == 'publishing':
                for entry in reversed(entries):
                    final = self.target(self.root, entry['path'])
                    backup = self.target(self.old, entry['path'])
                    staged = self.target(self.new, entry['path'])
                    if backup.exists():
                        final.parent.mkdir(parents=True, exist_ok=True)
                        replace(backup, final)
                    elif not entry['existed'] and entry['new'] and not staged.exists():
                        final.unlink(missing_ok=True)
                        if final.parent.exists():
                            sync_directory(final.parent)
        shutil.rmtree(self.base)
        sync_directory(self.root)

    def owner(self):
        return ('builder SD transaction v1: ' + self.product.key + '\n').encode('ascii')

    def begin(self):
        self.recover()
        self.base.mkdir()
        with (self.base / 'owner.json').open('xb') as stream:
            stream.write(self.owner())
            stream.flush()
            os.fsync(stream.fileno())
        sync_directory(self.base)
        sync_directory(self.root)
        (self.new / '3ds').mkdir(parents=True)
        self.old.mkdir()

    def write_journal(self, entries, state):
        path = self.journal.with_suffix('.part')
        with path.open('w', encoding='utf-8', newline='\n') as stream:
            json.dump(dict(state=state, entries=entries), stream)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        replace(path, self.journal)

    def publish(self, removals=(), cancelled=lambda: False):
        import texcache
        names = {path.relative_to(self.new).as_posix() for path in self.new.rglob('*') if path.is_file()}
        removals = set(removals)
        # If no PC texture/receipt could be made for changed data, the old validity markers must go too.
        for name in names:
            if name.endswith('/data.win'):
                final, staged = self.target(self.root, name), self.target(self.new, name)
                if not final.is_file() or texcache.checked_digest(final) != texcache.checked_digest(staged):
                    for marker in ('complete', 'data-version.txt'):
                        old_marker = name[:-len('data.win')] + 'cache/' + marker
                        if old_marker not in names and self.target(self.root, old_marker).exists():
                            removals.add(old_marker)
        marker = install_record.relative_path(self.product)
        if marker in names:
            current = install_record.read(self.new, self.product)
            if current is None:
                raise ValueError('Invalid installer inventory path')
            # Build the final inventory before publication, while both the old
            # card set and the complete staged set are available. Do not copy
            # retained model binaries or sound packs into the transaction.
            install_record.write(self.new, self.product, [entry['path'] for entry in current['files']],
                cancelled, previous=install_record.read(self.root, self.product),
                present=lambda name: self.target(self.root, name).is_file(), removed=removals)
        entries = []
        for name in names | removals:
            final, staged = self.target(self.root, name), self.target(self.new, name)
            if name in names:
                sync_file(staged)
                sync_directory(staged.parent)
                if (not name.endswith(('/complete', '/data-version.txt', '/install.json'))
                        and final.is_file() and texcache.checked_digest(final) == texcache.checked_digest(staged)):
                    continue
            entries.append(dict(path=name, existed=final.exists(), new=name in names))
        def rank(entry):
            name = entry['path']
            if name == install_record.relative_path(self.product):
                return 5, name
            if name.endswith('/data-version.txt'):
                return 3, name
            if name.endswith('/complete'):
                return 2, name
            if '/cache/' in name or name.endswith('.dtx'):
                return 1, name
            if name.endswith(('.3dsx', '.cia')):
                return 4, name
            return 0, name
        entries.sort(key=rank)
        self.write_journal(entries, 'publishing')
        # Remove old validity markers before changing data or pages. They are restored on rollback.
        for entry in entries:
            if entry['existed'] and (entry['path'].endswith(('/complete', '/data-version.txt', '/install.json'))):
                backup = self.target(self.old, entry['path'])
                backup.parent.mkdir(parents=True, exist_ok=True)
                replace(self.target(self.root, entry['path']), backup)
        for entry in entries:
            if cancelled():
                raise InterruptedError('cancelled')
            final, backup = self.target(self.root, entry['path']), self.target(self.old, entry['path'])
            if entry['existed'] and not backup.exists():
                backup.parent.mkdir(parents=True, exist_ok=True)
                replace(final, backup)
            if entry['new']:
                final.parent.mkdir(parents=True, exist_ok=True)
                replace(self.target(self.new, entry['path']), final)
        self.write_journal(entries, 'committed')
        self.recover()
