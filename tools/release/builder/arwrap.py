"""The builder's archive step: run the real ar (or ranlib) in a private folder, then move the archive into place.

build.sh passes CMake archive rules that call this script when the builder sets CTRPORT_ARCHIVE_WRAPPER:

    python -X utf8 -I arwrap.py create <ar> qc <target> <flags> <objects>
    python -X utf8 -I arwrap.py append <ar> q <target> <flags> <objects>
    python -X utf8 -I arwrap.py finish <ranlib> <target>

11-17 playtester (Windows): ar stopped with "could not create temporary file whilst writing archive: Permission
denied" for one archive in a reused build folder, on every try, while its siblings in the same folder packed. ar
writes its temporary file beside the archive, so anything holding or blocking that folder or a stale file there
stops it. Here ar works in a new folder of its own (CTRPORT_ARCHIVE_TMP/<pid>-<n>, every object path absolute),
and only the finished archive is moved beside the others: replaced, or deleted then renamed, with short retries.
ar stores member basenames only, so the archive's bytes don't depend on where it was written.
"""
import errno
import os
import shlex
import shutil
import stat
import subprocess
import sys
import time

TAG = 'archive step'
HINT = ('This is usually antivirus (look in its Protection history or quarantine), Controlled Folder Access '
        '(Windows Security, Ransomware protection) or OneDrive holding or blocking the folder. Exclude the builder\'s '
        'work folder and the devkitPro folder from them, then try again.')
FAILED = TAG + ': FAILED'
RETRIES = 6


def say(text):
    sys.stderr.write('%s: %s\n' % (TAG, text))
    sys.stderr.flush()


def private_dir(base):
    os.makedirs(base, exist_ok=True)
    for n in range(1000):
        path = os.path.join(base, '%d-%d' % (os.getpid(), n))
        try:
            os.mkdir(path)
            return path
        except FileExistsError:
            continue
    raise OSError(errno.EEXIST, 'no free private archive folder', base)


def absolute_arg(arg, private, n):
    """Object paths (and response files, whose entries CMake writes relative to the build folder) become
    absolute, so ar finds them from its private folder."""
    if arg.startswith('@') and os.path.isfile(arg[1:]):
        with open(arg[1:], encoding='utf-8', errors='surrogateescape') as f:
            entries = shlex.split(f.read(), posix=True)
        rsp = os.path.join(private, 'objects-%d.rsp' % n)
        with open(rsp, 'w', encoding='utf-8', errors='surrogateescape', newline='\n') as f:
            f.write('\n'.join('"%s"' % absolute(e).replace('\\', '/') for e in entries) + '\n')
        return '@' + rsp
    return absolute(arg)


def absolute(arg):
    return os.path.abspath(arg) if not arg.startswith('-') and os.path.exists(arg) else arg


def describe(path):
    try:
        st = os.stat(path)
    except FileNotFoundError:
        return 'no file with that name'
    except OSError as error:
        return 'exists, unreadable (%s)' % error
    attrs = getattr(st, 'st_file_attributes', None)
    readonly = not st.st_mode & stat.S_IWRITE
    kind = 'folder' if stat.S_ISDIR(st.st_mode) else 'file'
    return 'a %s exists, %d bytes, attributes %s, read-only %s' % (
        kind, st.st_size, '0x%x' % attrs if attrs is not None else 'n/a', 'yes' if readonly else 'no')


def probe(folder):
    name = os.path.join(folder, '.u3ds-write-probe-%d' % os.getpid())
    try:
        with open(name, 'wb') as f:
            f.write(b'probe')
        os.remove(name)
        return 'writable (a probe file was created and removed)'
    except OSError as error:
        return 'NOT writable: %s' % error_text(error)


def error_text(error):
    win = getattr(error, 'winerror', None)
    return 'errno %s (%s)%s' % (error.errno, os.strerror(error.errno) if error.errno else '?',
                                ', Windows error %d' % win if win else '')


def diagnose(target, private, error=None):
    say(FAILED + ' for ' + target)
    if error is not None:
        say('error: ' + error_text(error) + ' ' + str(error))
    say('target: ' + describe(target))
    say('target folder: ' + probe(os.path.dirname(target)))
    if private and os.path.isdir(private):
        say('private folder: ' + probe(private))
    say(HINT)


def make_writable(path):
    try:
        os.chmod(path, stat.S_IWRITE | stat.S_IREAD)
    except OSError:
        pass


def place(built, target):
    """Move the finished archive over the target: replace, else delete then rename; a few short retries."""
    last = None
    for attempt in range(RETRIES):
        try:
            if os.path.lexists(target):
                make_writable(target)
            try:
                os.replace(built, target)
            except OSError:
                if os.path.lexists(target):
                    os.remove(target)
                os.rename(built, target)
            return None
        except OSError as error:
            last = error
            time.sleep(0.1 * (attempt + 1))
    return last


def remove_tree(folder):
    """Best effort: a leftover private folder costs nothing and is never reused (each run makes a new one)."""
    for root, dirs, files in os.walk(folder, topdown=False):
        for name in files:
            path = os.path.join(root, name)
            make_writable(path)
            try:
                os.remove(path)
            except OSError:
                pass
        try:
            os.rmdir(root)
        except OSError:
            pass


def main(argv):
    mode, tool, args = argv[0], argv[1], list(argv[2:])
    assert mode in ('create', 'append', 'finish'), mode
    at = 1 if mode in ('create', 'append') else len(args) - 1     # ar <op> <target> ...; ranlib [flags] <target>
    target = os.path.abspath(args[at])
    base = os.environ.get('CTRPORT_ARCHIVE_TMP') or os.path.join(os.path.dirname(target), '.artmp')
    private = None
    try:
        private = private_dir(base)
        built = os.path.join(private, os.path.basename(target))
        # CMake deletes the archive before its create rule; a create always starts a new archive. Append and
        # finish work on the archive so far.
        if mode != 'create' and os.path.isfile(target):
            shutil.copyfile(target, built)
        rest = [absolute_arg(a, private, n) for n, a in enumerate(args[at + 1:])]
        cmd = [tool] + args[:at] + [os.path.basename(target)] + rest
        rc = subprocess.call(cmd, cwd=private, stdin=subprocess.DEVNULL)
        if rc != 0:
            diagnose(target, private)
            return rc
        if not os.path.isfile(built):
            diagnose(target, private, OSError(errno.ENOENT, 'the archive tool made no archive', built))
            return 1
        error = place(built, target)
        if error is not None:
            diagnose(target, private, error)
            return 1
        return 0
    except OSError as error:
        diagnose(target, private, error)
        return 1
    finally:
        if private:
            remove_tree(private)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
