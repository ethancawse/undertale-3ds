"""An exact compiler cache for the 3DS builds (docs/building-cia.md, "Compiler cache").

CMake runs every C compile through it (CMAKE_C_COMPILER_LAUNCHER, set by platform/3ds/build.sh):

    python compile_cache.py [--cache-dir=DIR] [--max-gb=N] <compiler> <compiler arguments...>

Preprocessor mode, as ccache's: the key is a SHA-256 of
  - the compiler's identity: its path, size and mtime, the same for cc1 and as, and its --version output;
  - every argument except the output file and the dependency-file options (-MD -MMD -MF -MT);
  - the working directory, whose path ends up in the debug information;
  - the preprocessed source (gcc -E), which carries every header's content and every file path.
A hit writes the cached object and replays the compile's stdout and stderr byte for byte; the dependency file comes
from the preprocessor run itself (-E keeps -MD -MF -MT), so it is exactly the file the compile would have written.

Worktrees: the absolute worktree path is in every file path, so the key is only made path-independent when the
compiler itself is told to remove it: exactly one -ffile-prefix-map=OLD=NEW. Then OLD is replaced by a placeholder in
the linemarkers of the preprocessed source, in the preprocessor options' paths (-I, -isystem, -D, ...), in the source
path and in the working directory, and nowhere else. Text such as a string literal that holds the path stays in the
key as it is. A cached compile that printed anything is only replayed into the worktree that made it.

Anything it does not understand runs the real compiler and caches nothing: an unknown option, no -c, more than one
input, a language other than C or C++, an .incbin or .include in the source, a failed compile.

CTRPORT_CCACHE=0 turns it into a plain pass-through. CTRPORT_CCACHE_LOG=<file> appends one line per compile
(hit, miss or uncached and why); `compile_cache.py --summary <file>` totals it.
"""
from __future__ import annotations

import hashlib
import json
import os
import random
import re
import subprocess
import sys
import time

START = time.perf_counter()
PHASES = {}  # name -> seconds, for the log

VERSION = 1  # bump to invalidate every entry when the key or the entry format changes
MAGIC = b'DTCC1\n'
PLACEHOLDER = '\x00ROOT\x00'
DEFAULT_MAX_GB = 20.0
IS_WINDOWS = os.name == 'nt'
SEPS = '/\\' if IS_WINDOWS else '/'

# Environment variables that change what the compiler does or prints.
ENV_IN_KEY = ('GCC_EXEC_PREFIX', 'COMPILER_PATH', 'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH',
              'OBJC_INCLUDE_PATH', 'SOURCE_DATE_EPOCH', 'LANG', 'LC_ALL', 'LC_MESSAGES', 'LC_CTYPE', 'GCC_COLORS',
              'GCC_URLS', 'TERM_URLS', 'COLUMNS')
# Set, these make gcc write a dependency file of its own: not handled.
ENV_UNHANDLED = ('DEPENDENCIES_OUTPUT', 'SUNPRO_DEPENDENCIES')

# Options taking a separate value.
SEPARATE_VALUE = {
    '-o', '-MF', '-MT', '-MQ', '-I', '-D', '-U', '-isystem', '-include', '-imacros', '-iquote', '-idirafter',
    '-iprefix', '-iwithprefix', '-iwithprefixbefore', '-isysroot', '-imultilib', '-x', '-Xpreprocessor',
    '-Xassembler', '-Xlinker', '-aux-info', '--param', '-A', '-L', '-l', '-T', '-u', '-z', '-e', '-wrapper',
    '-dumpbase', '-dumpdir', '-dumpbase-ext', '-B', '-specs', '--sysroot',
}
# Preprocessor options: everything they do shows in the preprocessed source, so paths in them may be normalized.
PREPROC_SEPARATE = {'-I', '-D', '-U', '-isystem', '-include', '-imacros', '-iquote', '-idirafter'}
PREPROC_JOINED = ('-I', '-D', '-U', '-isystem', '-include', '-imacros', '-iquote', '-idirafter')

# -f options that write files of their own, change what gets printed, read files the key can't see, or make the
# key's normalization unsound.
F_UNHANDLED = ('-fprofile', '-fauto-profile', '-fbranch-probabilities', '-ftest-coverage', '-fdump', '-flto',
               '-fplugin', '-fcanon-prefix-map', '-fstack-usage', '-fcallgraph-info', '-ftime-report',
               '-fmem-report', '-fsave-optimization-record', '-fopt-info', '-fpch', '-fdeps', '-fmodule',
               '-fdiagnostics-format', '-fdiagnostics-add-output', '-fdiagnostics-set-output', '-fcreate-profile',
               '-frecord-gcc-switches', '-fdebug-prefix-map', '-fmacro-prefix-map', '-fprofile-prefix-map',
               '-fworking-directory', '-fno-working-directory', '-fdirectives-only', '-fpreprocessed',
               '-fsyntax-only', '-fcompare-debug', '-fsanitize')
# Plain options that only steer code generation or warnings.
PLAIN_OK = {'-c', '-w', '-pipe', '-ansi', '-pedantic', '-pedantic-errors', '-nostdinc', '-nostdinc++', '-MD',
            '-MMD', '-trigraphs', '-pthread'}
LANG_BY_EXT = {'.c': 'c', '.cc': 'c++', '.cpp': 'c++', '.cxx': 'c++', '.c++': 'c++', '.C': 'c++'}


class Unhandled(Exception):
    """The compile runs uncached; the message says why."""


# ---------------------------------------------------------------------------------------------------------------------
# Command line

class Parsed:
    def __init__(self):
        self.compiler = ''
        self.args = []          # everything after the compiler
        self.source = None      # index into args
        self.output = None      # -o value
        self.lang = None
        self.prefix_map = None  # (old, new) from -ffile-prefix-map
        self.has_depfile = False


def parse(compiler, args):
    """Classify a compile command. Raises Unhandled for anything not provably cacheable."""
    p = Parsed()
    p.compiler = compiler
    p.args = list(args)
    inputs = []
    explicit_lang = None
    maps = []
    mt = mf = 0
    md = False
    has_c = False
    i = 0
    while i < len(args):
        a = args[i]
        if a.startswith('@'):
            raise Unhandled('response file')
        if not a.startswith('-') or a == '-':
            if a == '-':
                raise Unhandled('stdin input')
            inputs.append(i)
            i += 1
            continue
        if a in SEPARATE_VALUE:
            if i + 1 >= len(args):
                raise Unhandled('missing value for ' + a)
            v = args[i + 1]
            if a == '-o':
                if p.output is not None:
                    raise Unhandled('two -o')
                p.output = v
            elif a == '-x':
                explicit_lang = v
            elif a == '-MT':
                mt += 1
            elif a == '-MF':
                mf += 1
            elif a not in PREPROC_SEPARATE:
                raise Unhandled('option ' + a)
            i += 2
            continue
        if a == '-c':
            has_c = True
        elif a in ('-MD', '-MMD'):
            md = True
        elif a in PLAIN_OK:
            pass
        elif a.startswith('-ffile-prefix-map='):
            body = a[len('-ffile-prefix-map='):]
            if '=' not in body:
                raise Unhandled('bad -ffile-prefix-map')
            old, new = body.split('=', 1)
            maps.append((old, new))
        elif a.startswith('-f'):
            if a.startswith(F_UNHANDLED):
                raise Unhandled('option ' + a)
        elif a.startswith('-m') or a.startswith('-O') or a.startswith('-std=') or a.startswith('--param='):
            pass
        elif a.startswith('-g'):
            if 'split-dwarf' in a:
                raise Unhandled('option ' + a)
        elif a.startswith('-W'):
            if a.startswith(('-Wp,', '-Wa,')):
                raise Unhandled('option ' + a)
        elif a.startswith(PREPROC_JOINED):
            pass
        elif a.startswith('-x'):
            explicit_lang = a[2:]
        elif a.startswith('-MF') or a.startswith('-MT'):
            raise Unhandled('joined ' + a[:3])
        else:
            raise Unhandled('option ' + a)
        i += 1
    if not has_c:
        raise Unhandled('no -c')
    if len(inputs) != 1:
        raise Unhandled('%d inputs' % len(inputs))
    if p.output is None or p.output == '-':
        raise Unhandled('no -o file')
    if md:
        if mf != 1 or mt != 1:
            raise Unhandled('-MD needs exactly one -MF and one -MT')
        p.has_depfile = True
    elif mf or mt:
        raise Unhandled('-MF/-MT without -MD')
    p.source = inputs[0]
    src = args[p.source]
    lang = explicit_lang or LANG_BY_EXT.get(os.path.splitext(src)[1])
    if lang not in ('c', 'c++'):
        raise Unhandled('language ' + str(lang or os.path.splitext(src)[1]))
    p.lang = lang
    if len(maps) == 1 and safe_root(maps[0][0]):
        p.prefix_map = maps[0]
    return p


def safe_root(root):
    """Only plain absolute paths are normalized: no characters with a meaning in paths, depfiles or regexes."""
    if len(root) < 3 or not re.fullmatch(r'[A-Za-z0-9_.:/\\+~-]+', root):
        return False
    return root.startswith('/') or bool(re.match(r'[A-Za-z]:[/\\]', root))


# ---------------------------------------------------------------------------------------------------------------------
# Normalization

def _sep_equal(a, b):
    if IS_WINDOWS:
        return a.replace('\\', '/') == b.replace('\\', '/')
    return a == b


def strip_root(path, root):
    """PLACEHOLDER + the rest if path starts with root at a path-component boundary, else path unchanged.

    The comparison is exact except that on Windows / and \\ match each other, as they do in the compiler's own
    prefix-map matching there (libiberty's filename_ncmp). That also ignores case on Windows, which this does not, so
    this only ever strips a prefix the compiler also replaces, and the rest stays exactly as the compiler keeps it.
    """
    if root is None:
        return path
    n = len(root)
    if len(path) < n or not _sep_equal(path[:n], root):
        return path
    rest = path[n:]
    if rest and root[-1] not in SEPS and rest[0] not in SEPS:
        return path  # /a/root vs /a/rootother: the compiler maps it, but the rest would not be the same
    return PLACEHOLDER + rest


_LINEMARKER = re.compile(rb'^(#(?:line)? \d+ ")((?:[^"\\]|\\.)*)(".*)$')


def _unescape(b):
    return re.sub(rb'\\(.)', lambda m: m.group(1), b)


def _escape(b):
    return b.replace(b'\\', b'\\\\').replace(b'"', b'\\"')


def normalize_preprocessed(data, root):
    """The preprocessed source with root removed from linemarker file names only."""
    if root is None:
        return data
    root_b = root.encode('utf-8')
    # Quick reject: no line that could hold it.
    alt = root_b.replace(b'/', b'\\\\') if IS_WINDOWS else root_b
    if root_b not in data and alt not in data and root_b.replace(b'/', b'\\') not in data:
        return data
    out = []
    for line in data.split(b'\n'):
        if line.startswith(b'#'):
            m = _LINEMARKER.match(line)
            if m:
                name = _unescape(m.group(2)).decode('utf-8', 'surrogateescape')
                stripped = strip_root(name, root)
                if stripped != name:
                    line = m.group(1) + _escape(stripped.encode('utf-8', 'surrogateescape')) + m.group(3)
        out.append(line)
    return b'\n'.join(out)


def key_args(p):
    """The arguments that go into the key: output and dependency-file options dropped, preprocessor paths and the
    prefix map normalized."""
    root = p.prefix_map[0] if p.prefix_map else None
    out = []
    a = p.args
    i = 0
    while i < len(a):
        x = a[i]
        if x in ('-o', '-MF', '-MT'):
            i += 2
            continue
        if x in ('-MD', '-MMD'):
            out.append('-MD?')  # kept: -MMD vs -MD doesn't matter to the object, but costs nothing
            i += 1
            continue
        if i == p.source:
            out.append('<src>' + strip_root(x, root))
        elif x in PREPROC_SEPARATE and i + 1 < len(a):
            out.append(x)
            out.append(_norm_anywhere(a[i + 1], root))
            i += 1
        elif x.startswith(PREPROC_JOINED) and not x.startswith('-x'):
            out.append(_norm_anywhere(x, root))
        elif x.startswith('-ffile-prefix-map=') and root is not None:
            out.append('-ffile-prefix-map=' + PLACEHOLDER + '=' + p.prefix_map[1])
        else:
            out.append(x)
        i += 1
    return out


def _norm_anywhere(s, root):
    # Only for preprocessor options, whose whole effect is in the preprocessed source (hashed as is).
    if root is None:
        return s
    s2 = s.replace(root, PLACEHOLDER)
    if IS_WINDOWS:
        s2 = s2.replace(root.replace('/', '\\'), PLACEHOLDER)
    return s2


# ---------------------------------------------------------------------------------------------------------------------
# Compiler identity

def _stat_sig(path):
    try:
        st = os.stat(path)
        return '%s|%d|%d' % (path, st.st_size, st.st_mtime_ns)
    except OSError:
        return path + '|missing'


def compiler_identity(cache_dir, compiler, lang):
    """The compiler's path, size and mtime, its --version and -dumpmachine output, and the same signature for the
    cc1/cc1plus and as it runs. The slow part (running it) is cached per compiler signature."""
    sig = _stat_sig(compiler)
    if sig.endswith('|missing'):
        exe = compiler + '.exe'
        if IS_WINDOWS and os.path.exists(exe):
            sig = _stat_sig(exe)
        else:
            raise Unhandled('compiler not found')
    info_dir = os.path.join(cache_dir, 'compilers')
    info_path = os.path.join(info_dir, hashlib.sha256(sig.encode()).hexdigest()[:32] + '.json')
    info = None
    try:
        with open(info_path, 'r', encoding='utf-8') as f:
            info = json.load(f)
    except (OSError, ValueError):
        pass
    if not info or info.get('sig') != sig:
        def run(*a):
            r = subprocess.run([compiler] + list(a), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            return r.stdout.decode('utf-8', 'replace').strip()
        info = {
            'sig': sig,
            'version': run('--version'),
            'machine': run('-dumpmachine'),
            'cc1': run('-print-prog-name=cc1'),
            'cc1plus': run('-print-prog-name=cc1plus'),
            'as': run('-print-prog-name=as'),
        }
        os.makedirs(info_dir, exist_ok=True)
        atomic_write(info_path, json.dumps(info).encode())
    cc1 = info['cc1plus'] if lang == 'c++' else info['cc1']
    return '\n'.join([sig, info['version'], info['machine'], _stat_sig(cc1), _stat_sig(info['as'])])


# ---------------------------------------------------------------------------------------------------------------------
# Cache storage

def atomic_write(path, data):
    tmp = '%s.tmp%d.%d' % (path, os.getpid(), random.getrandbits(32))
    with open(tmp, 'wb') as f:
        f.write(data)
    try:
        os.replace(tmp, path)
    except OSError:
        # Windows: the target is open in another process. Whoever wrote it wrote the same thing.
        try:
            os.remove(tmp)
        except OSError:
            pass
        return False
    return True


def entry_path(cache_dir, key):
    return os.path.join(cache_dir, key[:2], key[2:])


def pack_entry(obj, stdout, stderr, root):
    payload = obj + stdout + stderr
    header = {'obj': len(obj), 'stdout': len(stdout), 'stderr': len(stderr), 'root': root,
              'sha': hashlib.sha256(payload).hexdigest()}
    return MAGIC + json.dumps(header).encode() + b'\n' + payload


def unpack_entry(data):
    if not data.startswith(MAGIC):
        return None
    nl = data.find(b'\n', len(MAGIC))
    if nl < 0:
        return None
    try:
        h = json.loads(data[len(MAGIC):nl])
    except ValueError:
        return None
    payload = data[nl + 1:]
    if len(payload) != h['obj'] + h['stdout'] + h['stderr'] or hashlib.sha256(payload).hexdigest() != h['sha']:
        return None
    o, so = h['obj'], h['stdout']
    return payload[:o], payload[o:o + so], payload[o + so:], h.get('root')


def trim(cache_dir, max_bytes, now=None):
    """Delete the least recently used entries until the cache is at most 80% of max_bytes. One trimmer at a time."""
    lock = os.path.join(cache_dir, 'trim.lock')
    now = time.time() if now is None else now
    try:
        fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        try:
            if now - os.stat(lock).st_mtime > 600:
                os.remove(lock)  # a trimmer that died
        except OSError:
            pass
        return 0
    removed = 0
    try:
        os.close(fd)
        entries = []
        total = 0
        for d in os.scandir(cache_dir):
            if not d.is_dir() or len(d.name) != 2:
                continue
            for e in os.scandir(d.path):
                try:
                    st = e.stat()
                except OSError:
                    continue
                if '.tmp' in e.name:
                    if now - st.st_mtime > 3600:  # left by a killed process
                        _rm(e.path)
                    continue
                entries.append((st.st_mtime, st.st_size, e.path))
                total += st.st_size
        if total > max_bytes:
            entries.sort()
            goal = max_bytes * 0.8
            for _, size, path in entries:
                if total <= goal:
                    break
                if _rm(path):
                    total -= size
                    removed += 1
    finally:
        _rm(lock)
    return removed


def _rm(path):
    try:
        os.remove(path)
        return True
    except OSError:
        return False


# ---------------------------------------------------------------------------------------------------------------------
# Log

def log_event(kind, detail, src):
    path = os.environ.get('CTRPORT_CCACHE_LOG')
    if not path:
        return
    times = ' '.join('%s=%d' % (k, v * 1000) for k, v in PHASES.items())
    total = (time.perf_counter() - START) * 1000
    line = ('%s\t%s\t%s\tms=%d %s\n' % (kind, detail, src, total, times)).encode('utf-8', 'replace')
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND | getattr(os, 'O_BINARY', 0))
    except OSError:
        return
    try:
        _lock(fd)
        os.lseek(fd, 0, os.SEEK_END)
        os.write(fd, line)
    except OSError:
        pass
    finally:
        _unlock(fd)
        os.close(fd)


def _lock(fd):
    if IS_WINDOWS:
        import msvcrt
        os.lseek(fd, 1 << 30, os.SEEK_SET)
        for _ in range(2000):
            try:
                msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
                return
            except OSError:
                time.sleep(0.005)
    else:
        import fcntl
        fcntl.flock(fd, fcntl.LOCK_EX)


def _unlock(fd):
    try:
        if IS_WINDOWS:
            import msvcrt
            os.lseek(fd, 1 << 30, os.SEEK_SET)
            msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(fd, fcntl.LOCK_UN)
    except OSError:
        pass


def summary(path):
    counts = {}
    reasons = {}
    ms = {}  # kind -> phase -> total ms
    try:
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            for line in f:
                parts = line.rstrip('\n').split('\t')
                for field in (parts[3].split() if len(parts) > 3 else ()):
                    name, _, value = field.partition('=')
                    if value.isdigit():
                        ms.setdefault(parts[0], {}).setdefault(name, 0)
                        ms[parts[0]][name] += int(value)
                if not parts or not parts[0]:
                    continue
                counts[parts[0]] = counts.get(parts[0], 0) + 1
                if parts[0] == 'uncached' and len(parts) > 1:
                    reasons[parts[1]] = reasons.get(parts[1], 0) + 1
    except OSError:
        return 'compile cache: no log'
    hit, miss, unc = counts.get('hit', 0), counts.get('miss', 0), counts.get('uncached', 0)
    total = hit + miss + unc
    text = 'compile cache: %d compiles, %d hits, %d misses, %d uncached' % (total, hit, miss, unc)
    if total:
        text += ' (hit rate %.0f%%)' % (100.0 * hit / total)
    if reasons:
        text += '; uncached: ' + ', '.join('%s x%d' % kv for kv in sorted(reasons.items()))
    for kind, label in (('hit', 'hits'), ('miss', 'misses')):
        if counts.get(kind) and ms.get(kind, {}).get('ms'):
            text += '; %s %.2f s each' % (label, ms[kind]['ms'] / 1000.0 / counts[kind])
    return text


# ---------------------------------------------------------------------------------------------------------------------
# Main

def run_real(compiler, args):
    return subprocess.call([compiler] + args)


def emit(stdout, stderr):
    if stdout:
        sys.stdout.buffer.write(stdout)
        sys.stdout.buffer.flush()
    if stderr:
        sys.stderr.buffer.write(stderr)
        sys.stderr.buffer.flush()


def preprocess_args(p):
    """The compile's arguments as a preprocessor run: without -c and -o, with -E. -MD -MF -MT stay, so this run
    writes the dependency file."""
    out = []
    a = p.args
    i = 0
    while i < len(a):
        if a[i] == '-o':
            i += 2
            continue
        if a[i] != '-c':
            out.append(a[i])
        i += 1
    return out + ['-E']


def compute_key(p, ident, cwd, preprocessed):
    root = p.prefix_map[0] if p.prefix_map else None
    h = hashlib.sha256()

    def put(tag, value):
        if isinstance(value, str):
            value = value.encode('utf-8', 'surrogateescape')
        h.update(tag.encode() + b'\x00' + str(len(value)).encode() + b'\x00' + value)

    put('version', str(VERSION))
    put('compiler', ident)
    put('lang', p.lang)
    for x in key_args(p):
        put('arg', x)
    put('cwd', strip_root(cwd, root))
    for name in ENV_IN_KEY:
        put('env', name + '=' + os.environ.get(name, '\x01unset'))
    put('cpp', normalize_preprocessed(preprocessed, root))
    return h.hexdigest()


def cached_compile(cache_dir, max_bytes, compiler, args):
    try:
        p = parse(compiler, args)
        for name in ENV_UNHANDLED:
            if os.environ.get(name):
                raise Unhandled('environment ' + name)
    except Unhandled as e:
        log_event('uncached', str(e), _src_guess(args))
        return run_real(compiler, args)
    src = args[p.source]
    try:
        ident = compiler_identity(cache_dir, compiler, p.lang)
    except (Unhandled, OSError) as e:
        log_event('uncached', 'compiler identity: %s' % e, src)
        return run_real(compiler, args)

    t = time.perf_counter()
    pre = subprocess.run([compiler] + preprocess_args(p), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    PHASES['cpp'] = time.perf_counter() - t
    if pre.returncode != 0:
        log_event('uncached', 'preprocessor failed', src)
        return run_real(compiler, args)
    data = pre.stdout
    if b'.incbin' in data or b'.include' in data or re.search(rb'(?m)^\s*#\s*embed', data):
        log_event('uncached', 'reads files the preprocessor does not see', src)
        return run_real(compiler, args)

    root = p.prefix_map[0] if p.prefix_map else None
    t = time.perf_counter()
    key = compute_key(p, ident, os.getcwd(), data)
    PHASES['key'] = time.perf_counter() - t
    path = entry_path(cache_dir, key)
    try:
        with open(path, 'rb') as f:
            entry = unpack_entry(f.read())
    except OSError:
        entry = None
    if entry is not None:
        obj, so, se, eroot = entry
        if (so or se) and eroot != root:
            entry = None  # its messages name another worktree's files: compile here instead
        else:
            atomic_write(p.output, obj) or _write_plain(p.output, obj)
            try:
                os.utime(path)
            except OSError:
                pass
            emit(so, se)
            log_event('hit', key[:16], src)
            return 0
    elif os.path.exists(path):
        _rm(path)  # corrupt

    t = time.perf_counter()
    r = subprocess.run([compiler] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    PHASES['cc'] = time.perf_counter() - t
    emit(r.stdout, r.stderr)
    if r.returncode != 0:
        log_event('uncached', 'compile failed', src)
        return r.returncode
    try:
        with open(p.output, 'rb') as f:
            obj = f.read()
        os.makedirs(os.path.dirname(path), exist_ok=True)
        atomic_write(path, pack_entry(obj, r.stdout, r.stderr, root))
    except OSError as e:
        log_event('miss', 'not stored: %s' % e, src)
        return 0
    log_event('miss', key[:16], src)
    if random.random() < 1.0 / 64:
        try:
            trim(cache_dir, max_bytes)
        except OSError:
            pass
    return 0


def _write_plain(path, data):
    with open(path, 'wb') as f:
        f.write(data)
    return True


def _src_guess(args):
    for a in args:
        if not a.startswith('-') and os.path.splitext(a)[1] in LANG_BY_EXT:
            return a
    return '?'


def default_cache_dir():
    return os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                        'scratch', 'ccache')


def main(argv):
    if len(argv) >= 2 and argv[0] == '--summary':
        print(summary(argv[1]))
        return 0
    cache_dir = os.environ.get('CTRPORT_CCACHE_DIR') or None
    max_gb = float(os.environ.get('CTRPORT_CCACHE_MAX_GB') or DEFAULT_MAX_GB)
    while argv and argv[0].startswith('--'):
        opt = argv.pop(0)
        if opt.startswith('--cache-dir='):
            cache_dir = cache_dir or opt.split('=', 1)[1]  # the environment wins
        elif opt.startswith('--max-gb='):
            if not os.environ.get('CTRPORT_CCACHE_MAX_GB'):
                max_gb = float(opt.split('=', 1)[1])
        elif opt == '--':
            break
        else:
            sys.stderr.write('compile_cache.py: unknown option %s\n' % opt)
            return 2
    if not argv:
        sys.stderr.write('usage: compile_cache.py [--cache-dir=DIR] [--max-gb=N] <compiler> <args...>\n')
        return 2
    compiler, args = argv[0], argv[1:]
    if os.environ.get('CTRPORT_CCACHE', '1') == '0':
        return run_real(compiler, args)
    cache_dir = cache_dir or default_cache_dir()
    try:
        os.makedirs(cache_dir, exist_ok=True)
    except OSError:
        return run_real(compiler, args)
    return cached_compile(cache_dir, int(max_gb * (1 << 30)), compiler, args)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
