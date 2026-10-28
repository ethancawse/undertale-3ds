"""Preview-first cleanup, confined to known work areas and recorded new tools.

No discovery by tool name and no recursive removal. A tool present before the
builder's installer ran can never become eligible in a later session.
"""
from __future__ import annotations
from dataclasses import dataclass
from pathlib import Path
import hashlib
import json
import os
import stat
import tempfile

JOURNAL = 'builder-installed.json'
CATEGORIES = ('build', 'downloads', 'tools')
WORK_AREAS = {'build': ('src', 'ccache', 'data', 'chapter-data', 'texcache', 'bin'),
              'downloads': (), 'tools': ()}


def linked(path):
    """Reject symbolic links AND Windows junction/reparse points, including ancestors."""
    for p in (path, *path.parents):
        try:
            info = p.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
            return True
    return False


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as source:
        while block := source.read(1024*1024):
            h.update(block)
    return h.hexdigest()


def read_journal(work):
    path = Path(work)/JOURNAL
    if not path.exists():
        return {'schema': 1, 'entries': []}
    value = json.loads(path.read_text(encoding='utf-8'))
    if value.get('schema') != 1 or not isinstance(value.get('entries'), list):
        raise ValueError("Can't read the list of tools this builder installed, so cleanup is off. Nothing was deleted.")
    return value


def write_journal(work, value):
    work = Path(work).absolute()
    if linked(work):
        raise ValueError('Cleanup is off because the work folder points to another folder. Nothing was deleted.')
    work.mkdir(parents=True, exist_ok=True)
    handle, temporary = tempfile.mkstemp(prefix='.installed-', dir=work)
    try:
        with os.fdopen(handle, 'w', encoding='utf-8') as out:
            json.dump(value, out, indent=2)
            out.flush()
            os.fsync(out.fileno())
        os.replace(temporary, work/JOURNAL)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def record_before(work, paths, category='tools'):
    """Persist BEFORE any installer action. Existing roots are permanently excluded."""
    if category not in ('tools', 'downloads'):
        raise ValueError(category)
    journal = read_journal(work)
    for path in paths:
        path = Path(path).absolute()
        if linked(path) or not path.is_absolute() or path == Path(path.anchor):
            raise ValueError("Can't use this folder for tools (it is a drive root or points to another folder): " + str(path))
        found = next((e for e in journal['entries'] if e['root'] == str(path)), None)
        if found is None:
            journal['entries'].append({'root': str(path), 'category': category, 'preexisting': path.exists(),
                                       'files': [], 'complete': False})
    write_journal(work, journal)


def files_under(root):
    if linked(root):
        return
    if root.is_file():
        yield root
    elif root.is_dir():
        for directory, folders, names in os.walk(root, followlinks=False):
            folders[:] = [n for n in folders if not linked(Path(directory)/n)]
            for name in names:
                p = Path(directory)/name
                if not linked(p) and p.is_file():
                    yield p


def record_after(work, paths):
    journal = read_journal(work)
    selected = {str(Path(p).absolute()) for p in paths}
    for entry in journal['entries']:
        if entry['root'] not in selected or entry['preexisting']:
            continue
        root = Path(entry['root'])
        previous = {f['path']: f for f in entry['files']}
        for path in files_under(root):
            key = str(path)
            if key in previous and previous[key]['sha256'] != digest(path):
                # Changed since our earlier install: a later user/other installer owns this file now.
                previous.pop(key)
                entry.setdefault('kept', []).append(key)
            elif key not in entry.get('kept', []):
                previous[key] = {'path': key, 'bytes': path.stat().st_size, 'sha256': digest(path)}
        entry['files'] = list(previous.values())
        entry['complete'] = True
    write_journal(work, journal)


@dataclass(frozen=True)
class File:
    path: str
    root: str
    category: str
    bytes: int
    mtime_ns: int
    inode: int
    sha256: str | None = None


@dataclass
class Plan:
    files: list
    kept: list

    def size(self, category):
        return sum(f.bytes for f in self.files if f.category == category)

    def preview(self, categories):
        selected = [f for f in self.files if f.category in categories]
        return '\n'.join(['Delete %d files (%.1f MB):' % (len(selected), sum(f.bytes for f in selected)/1e6)]
                         + [f.path for f in selected]
                         + (['', 'These are kept:'] + self.kept if self.kept else []))


def plan(work, *, protected=()):
    work = Path(work).absolute()
    if linked(work):
        raise ValueError('Cleanup is off because the work folder points to another folder. Nothing was deleted.')
    protected = [Path(p).absolute() for p in protected if p] + [work/'out']
    def safe(path):
        return not linked(path) and not any(path == p or p in path.parents for p in protected)
    files, kept = [], []
    def add(path, root, category, sha=None):
        if path.suffix.lower() in ('.3dsx', '.cia') or not safe(path):
            return
        st = path.stat()
        files.append(File(str(path), str(root), category, st.st_size, st.st_mtime_ns, st.st_ino, sha))
    journal = read_journal(work)
    # Installer roots are governed only by their ownership records, even if a
    # caller put a tool inside an ordinary work area such as bin or src.
    installer_roots = [Path(e['root']) for e in journal['entries']]
    for category, names in WORK_AREAS.items():
        for name in names:
            root = work/name
            if not safe(root):
                continue
            for path in files_under(root):
                if not any(path == p or p in path.parents for p in installer_roots):
                    add(path, root, category)
    for entry in journal['entries']:
        root = Path(entry['root'])
        if not root.is_absolute() or linked(root):
            kept.append('Kept (folder points elsewhere): ' + str(root))
            continue
        if entry['preexisting']:
            kept.append('Kept (was here before this builder): ' + str(root))
            continue
        if not entry.get('complete'):
            kept.append("Kept (install didn't finish): " + str(root))
            continue
        for recorded in entry['files']:
            path = Path(recorded['path'])
            if path != root and root not in path.parents:
                raise ValueError('The list of tools this builder installed looks wrong, so cleanup is off. Nothing was deleted.')
            if not safe(path) or not path.is_file():
                continue
            if path.stat().st_size != recorded['bytes'] or digest(path) != recorded['sha256']:
                kept.append('Kept (changed since it was installed): ' + str(path))
                continue
            add(path, root, entry['category'], recorded['sha256'])
    # A tool can be in a work area; never delete the same file twice.
    return Plan(list({f.path: f for f in files}.values()), kept)


def remove(preview, categories):
    """Consume an approved exact file list. Recheck every file immediately before unlink."""
    if not set(categories) <= set(CATEGORIES):
        raise ValueError('Invalid cleanup choice.')
    deleted, kept = [], list(preview.kept)
    for item in preview.files:
        if item.category not in categories:
            continue
        path, root = Path(item.path), Path(item.root)
        if (not path.is_absolute() or (path != root and root not in path.parents) or linked(path)
                or path.suffix.lower() in ('.3dsx', '.cia')):
            kept.append('Kept (moved or changed): ' + str(path))
            continue
        try:
            st = path.stat()
            if ((st.st_size, st.st_mtime_ns, st.st_ino) != (item.bytes, item.mtime_ns, item.inode)
                    or (item.sha256 and digest(path) != item.sha256)):
                kept.append('Kept (changed after the list was shown): ' + str(path))
                continue
            path.unlink()
            deleted.append(str(path))
        except OSError:
            kept.append("Kept (couldn't delete it): " + str(path))
    return deleted, kept
