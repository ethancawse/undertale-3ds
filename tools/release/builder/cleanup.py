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
WORK_AREAS = {'build': ('src', 'ccache', 'data', 'chapter-data', 'texcache', 'sfx-cache', 'bin'),
              'downloads': (), 'tools': ()}


def require_work_paths(work):
    work = Path(work).resolve()
    if work.resolve() in (Path(work.anchor), Path.home().resolve()):
        raise ValueError('Choose a separate work folder, not your home folder or a drive root.')
    require_plain_tree(work, descendants=False)
    for name in WORK_AREAS['build'] + ('logs', 'tools', 'out'):
        require_plain_tree(work/name)


def plain_root(path):
    """Resolve legitimate ancestors, while leaving the selected descendant observable."""
    path = Path(path).absolute()
    return path.parent.resolve()/path.name


def reparse(info):
    return stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400


def require_plain_tree(root, *, descendants=True):
    root = plain_root(root)
    pending = [root]
    try:
        while pending:
            path = pending.pop()
            try:
                info = path.lstat()
            except FileNotFoundError:
                continue
            if reparse(info):
                raise ValueError('The work folder contains a path that points to another folder. '
                                 'Choose a work folder with ordinary folders and files. Nothing was changed.')
            if descendants and stat.S_ISDIR(info.st_mode):
                with os.scandir(path) as entries:
                    for entry in entries:
                        info = entry.stat(follow_symlinks=False)
                        if reparse(info):
                            raise ValueError('The work folder contains a path that points to another folder. '
                                             'Choose a work folder with ordinary folders and files. Nothing was changed.')
                        if stat.S_ISDIR(info.st_mode):
                            pending.append(Path(entry.path))
    except OSError as exc:
        raise ValueError('The work folder could not all be checked. Choose another work folder. Nothing was changed.') from exc


def available(work):
    """Cheap first-screen entry check; the preview performs the ownership validation."""
    work = Path(work)
    return (work/JOURNAL).is_file() or any((work/name).exists()
        for names in WORK_AREAS.values() for name in names)


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
    work = Path(work).resolve()
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
    if category not in CATEGORIES:
        raise ValueError(category)
    journal = read_journal(work)
    for path in paths:
        path = plain_root(path)
        if linked(path) or not path.is_absolute() or path == Path(path.anchor):
            raise ValueError("Can't use this folder for tools (it is a drive root or points to another folder): " + str(path))
        found = next((e for e in journal['entries'] if e['root'] == str(path)), None)
        if found is None:
            journal['entries'].append({'root': str(path), 'category': category, 'preexisting': path.exists(),
                                       'files': [], 'complete': False})
        elif found.get('complete') and not found['preexisting'] and found['category'] != 'build':
            # Another visit to a root this builder made. What is there now without a record was added by the
            # player or another installer since, and stays theirs whatever this step goes on to write.
            recorded = {f['path'] for f in found['files']}
            added = {str(p) for p in files_under(path)} - recorded
            if added - set(found.get('kept', [])):
                found['kept'] = sorted(added | set(found.get('kept', [])))
    write_journal(work, journal)


def files_under(root):
    root = plain_root(root)
    if linked(root):
        return
    if root.is_file():
        yield root
    elif root.is_dir():
        pending = [root]
        while pending:
            with os.scandir(pending.pop()) as entries:
                for entry in entries:
                    info = entry.stat(follow_symlinks=False)
                    if reparse(info):
                        continue
                    if stat.S_ISDIR(info.st_mode):
                        pending.append(Path(entry.path))
                    elif stat.S_ISREG(info.st_mode):
                        yield Path(entry.path)


def record_after(work, paths):
    journal = read_journal(work)
    selected = {str(plain_root(p)) for p in paths}
    for entry in journal['entries']:
        if entry['root'] not in selected or entry['preexisting']:
            continue
        root = Path(entry['root'])
        if entry['category'] == 'build' and (entry.get('complete') or not root.exists()):
            continue  # Never adopt later player additions to an already recorded build area.
        previous = {f['path']: f for f in entry['files']}
        kept = set(entry.get('kept', []))  # a set: a player's own packages can be tens of thousands of files
        for path in files_under(root):
            key = str(path)
            if key in previous and previous[key]['sha256'] != digest(path):
                # Changed since our earlier install: a later user/other installer owns this file now.
                previous.pop(key)
                kept.add(key)
            elif key not in kept:
                previous[key] = {'path': key, 'bytes': path.stat().st_size, 'sha256': digest(path)}
        if kept:
            entry['kept'] = sorted(kept)
        entry['files'] = list(previous.values())
        entry['complete'] = True
    write_journal(work, journal)


def begin_build(work, paths):
    """Snapshot before writes; ownership covers new files, never pre-existing additions."""
    journal = read_journal(work)
    snapshots = {}
    for selected in paths:
        area = plain_root(selected)
        require_plain_tree(area)
        before = {str(path) for path in files_under(area)}
        entry = next((e for e in journal['entries'] if e['root'] == str(area) and
                      e['category'] == 'build' and not e.get('temporary')), None)
        if entry is None:
            entry = dict(root=str(area), category='build', preexisting=area.exists(),
                         files=[], complete=True)
            journal['entries'].append(entry)
        owned = set()
        for recorded in entry['files']:
            path = Path(recorded['path'])
            if (str(path) in before and path.stat().st_size == recorded['bytes'] and
                    digest(path) == recorded['sha256']):
                owned.add(str(path))
        # A player edit before this operation permanently loses build ownership.
        entry['files'] = [f for f in entry['files'] if f['path'] in owned]
        snapshots[str(area)] = (before, owned)
    write_journal(work, journal)
    return snapshots


def finish_build(work, snapshots):
    journal = read_journal(work)
    for entry in journal['entries']:
        if entry['root'] not in snapshots or entry.get('temporary'):
            continue
        before, owned = snapshots[entry['root']]
        records = {}
        for path in files_under(Path(entry['root'])):
            key = str(path)
            if key not in before or key in owned:
                records[key] = dict(path=key, bytes=path.stat().st_size, sha256=digest(path))
        entry['files'], entry['complete'] = list(records.values()), True
    write_journal(work, journal)


def record_temporary(work, path):
    """Called only for the private file just returned by mkstemp, before copying."""
    work, path = Path(work).resolve(), plain_root(path)
    if work/'out' not in path.parents or not path.name.startswith('.') or path.suffix != '.part' or linked(path):
        raise ValueError('Invalid private package copy path.')
    info = path.stat()
    journal = read_journal(work)
    journal['entries'] = [e for e in journal['entries']
                          if not e.get('temporary') or Path(e['root']).exists()]
    journal['entries'].append(dict(root=str(path), category='build', preexisting=False,
        complete=True, temporary=True, device=info.st_dev, inode=info.st_ino, files=[]))
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
                         + self.file_summary(selected)
                         + (['', 'These are kept:'] + self.kept if self.kept else []))


    @staticmethod
    def file_summary(selected):
        groups = {}
        for item in selected:
            groups.setdefault(item.root, []).append(item.path)
        return [line for root, paths in groups.items()
                for line in (paths if len(paths) <= 12 else
                             ['%s (%d files)' % (root, len(paths))])]


def plan(work, *, protected=()):
    work = Path(work).resolve()
    if linked(work):
        raise ValueError('Cleanup is off because the work folder points to another folder. Nothing was deleted.')
    protected = [Path(p).resolve() for p in protected if p] + [work/'out']
    checked = set()
    def linked_once(path):
        for part in (path, *path.parents):
            if part in checked:
                continue
            try:
                if reparse(part.lstat()):
                    return True
            except FileNotFoundError:
                pass
            checked.add(part)
        return False
    def safe(path):
        return not linked_once(path) and not any(path == p or p in path.parents for p in protected)
    files, kept, summaries = [], [], {}
    def keep_file(reason, root, path):
        key = (reason, str(root))
        count, first = summaries.get(key, (0, str(path)))
        summaries[key] = (count + 1, first)
    def add(path, root, category, sha=None):
        if path.suffix.lower() in ('.3dsx', '.cia') or not safe(path):
            return
        st = path.stat()
        files.append(File(str(path), str(root), category, st.st_size, st.st_mtime_ns, st.st_ino, sha))
    journal = read_journal(work)
    # Folder names never confer ownership. Older work areas and player additions
    # remain untouched unless their exact bytes have a builder creation record.
    for category, names in WORK_AREAS.items():
        for name in names:
            root = work/name
            if not safe(root):
                if root.exists() or root.is_symlink():
                    kept.append('Kept (folder is protected or points elsewhere): ' + str(root))
                continue
            recorded = {f['path'] for e in journal['entries'] if (not e['preexisting'] or e['category'] == 'build') and e.get('complete')
                        for f in e['files']}
            count = 0
            for path in files_under(root):
                count += 1
                if str(path) not in recorded:
                    keep_file('no record that this builder created it', root, path)
            if not count and root.exists() and not any(e['root'] == str(root) for e in journal['entries']):
                kept.append('Kept (no record that this builder created it): ' + str(root))
    for entry in journal['entries']:
        root = Path(entry['root'])
        if not root.is_absolute() or linked(root):
            kept.append('Kept (folder points elsewhere): ' + str(root))
            continue
        if entry['preexisting'] and entry['category'] != 'build':
            kept.append('Kept (was here before this builder): ' + str(root))
            continue
        if not entry.get('complete'):
            kept.append("Kept (install didn't finish): " + str(root))
            continue
        if entry.get('temporary'):
            if (work/'out' in root.parents and root.name.startswith('.') and root.suffix == '.part'
                    and not linked_once(root) and root.is_file()):
                info = root.stat()
                if info.st_ino and (info.st_dev, info.st_ino) == (entry['device'], entry['inode']):
                    files.append(File(str(root), str(root), 'build', info.st_size,
                                      info.st_mtime_ns, info.st_ino, digest(root)))
            continue
        for recorded in entry['files']:
            path = Path(recorded['path'])
            if entry['category'] == 'build' and any(
                    other['category'] != 'build' and (path == Path(other['root']) or Path(other['root']) in path.parents)
                    for other in journal['entries']):
                continue
            if path != root and root not in path.parents:
                raise ValueError('The list of tools this builder installed looks wrong, so cleanup is off. Nothing was deleted.')
            if not safe(path) or not path.is_file():
                continue
            if path.stat().st_size != recorded['bytes'] or digest(path) != recorded['sha256']:
                keep_file('changed since it was installed', root, path)
                continue
            add(path, root, entry['category'], recorded['sha256'])
    # A tool can be in a work area; never delete the same file twice.
    kept.extend('Kept (%s): %s (%d files%s)' % (reason, folder, count,
                '; ' + first if count == 1 else '')
                for (reason, folder), (count, first) in summaries.items())
    return Plan(list({f.path: f for f in files}.values()), kept)


def remove(preview, categories):
    """Consume an approved exact file list. Recheck every file immediately before unlink."""
    if not set(categories) <= set(CATEGORIES):
        raise ValueError('Invalid cleanup choice.')
    deleted, kept = [], list(preview.kept)
    failures = {}
    def keep(reason, item):
        key = (reason, item.root)
        count, first = failures.get(key, (0, item.path))
        failures[key] = (count + 1, first)
    for item in preview.files:
        if item.category not in categories:
            continue
        path, root = Path(item.path), Path(item.root)
        if (not path.is_absolute() or (path != root and root not in path.parents) or linked(path)
                or path.suffix.lower() in ('.3dsx', '.cia')):
            keep('moved or changed', item)
            continue
        try:
            st = path.stat()
            if ((st.st_size, st.st_mtime_ns, st.st_ino) != (item.bytes, item.mtime_ns, item.inode)
                    or (item.sha256 and digest(path) != item.sha256)):
                keep('changed after the list was shown', item)
                continue
            path.unlink()
            deleted.append(str(path))
        except OSError:
            keep("couldn't delete it", item)
    kept.extend('Kept (%s): %s' % (reason, first if count == 1 else '%s (%d files)' % (root, count))
                for (reason, root), (count, first) in failures.items())
    return deleted, kept
