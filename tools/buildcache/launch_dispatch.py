"""Run a compile's whole launcher chain in one Python process (docs/building-cia.md, "Launcher dispatcher").

CMake builds every ctrport compile as a chain of launchers (platform/3ds/CMakeLists.txt, C_COMPILER_LAUNCHER):

    python -X utf8 -I a_compile.py  python -X utf8 -I b_compile.py ... python -X utf8 -I -S compile_cache.py ...
        --b-compiler --a-compiler  arm-none-eabi-gcc <arguments>

Each launcher finds its marker, runs the rest of the line with subprocess.run and waits for it: about 25 nested
python.exe per compile, each started and torn down for one source file. platform/3ds/launch_dispatch.cmake puts this
file in front of that line (RULE_LAUNCH_COMPILE):

    python -X utf8 -I -S launch_dispatch.py <the chain, unchanged>

and it runs the chain itself. A command that starts this same interpreter on a .py file with flags this process can
reproduce (-X utf8 and -I, with or without -S) runs here as a "level": the script runs as __main__ in a fresh module,
with the sys.argv, sys.path, import hooks, site builtins, modules, sys.dont_write_bytecode, warnings filters,
environment, working directory and standard input a new process would start with, and its exit status (SystemExit,
an uncaught exception printed as the interpreter prints it, or 0) is returned to the caller as that process's return
code. Each level's own subprocess.run / subprocess.call of the next level is caught the same way. Everything else (the
compiler, the assembler, objcopy, a Python started with other flags, a call that captures output or changes env or
cwd) runs as a real subprocess, with standard input closed the way the launcher's own stdin=DEVNULL would have left
it. So the same launcher code makes the same calls in the same order with the same arguments; only the process
boundaries between Python levels are gone.

A level-to-level call's timeout (the launchers pass 1800 s) still bounds everything inside it. While it runs, every
real subprocess under it gets at most the time left (its own timeout when that is shorter); when the time runs out,
the levels inside the call are unwound (a _Deadline exception, raised in the level's code: when a real subprocess
times out, or on SIGINT from a watchdog thread when the level is running Python) and the call raises
subprocess.TimeoutExpired to its caller, as subprocess.run / call do after killing the child. The expiry holds until
that call ends: a level that catches the _Deadline meets it again at its next subprocess call and when it ends, so
the call raises TimeoutExpired whatever the level does (never a success). A level still running
GRACE seconds after that (stuck in one system call, or catching the exception) ends the whole process with status 1,
the status an uncaught TimeoutExpired gives the caller's process. CTRPORT_LAUNCH_DISPATCH_GRACE=<seconds> changes it.

Not reproduced: the relative order of a level's unflushed output against a later real subprocess' output (this
flushes before every real call); after a timeout, the inner levels' own exception handlers and finally blocks run as
the exception passes (a killed process runs none), and their real subprocesses are killed (a killed launcher's
children kept running).
Standard modules every launcher imports are loaded once and shared (PRELOAD). A Python whose site would run a
sitecustomize or usercustomize module runs its non -S levels as real processes, since that code runs at every start.

CTRPORT_LAUNCH_DISPATCH_LOG=<file> appends one line per compile: the total, each level's own time (without the levels
and subprocesses it waited on) and the time in real subprocesses. `launch_dispatch.py --summary <file>` totals it.
CTRPORT_LAUNCH_DISPATCH=0 in the environment runs the line as one plain subprocess (the old chain, one process more).
"""
import builtins
import os
import signal
import subprocess
import sys
import threading
import time
import types
import warnings
from importlib.machinery import SourceFileLoader

START = time.perf_counter()

# Standard modules the launchers import: loaded once, shared by all levels. They hold no state a launcher reads (re
# and fnmatch keep pattern caches). Anything else a level imports, including every project module, is imported afresh
# in that level and dropped when it ends, as in a new process.
PRELOAD = ('re', 'json', 'hashlib', 'struct', 'pathlib', 'shlex', 'fnmatch', 'collections', 'importlib.util',
           'traceback')
for _name in PRELOAD:
    try:
        __import__(_name)
    except ImportError:
        pass
import traceback  # noqa: E402

IS_WINDOWS = os.name == 'nt'
REAL_RUN = subprocess.run
REAL_CALL = subprocess.call
FLAGS = sys.flags  # this process' own; a level may see a lookalike
DEVNULL = subprocess.DEVNULL
EMULATED_KEYS = {'stdin', 'timeout', 'check'}  # subprocess.run / call keywords a level call may carry
SYS_HOOKS = ('stdout', 'stderr', 'excepthook', 'displayhook', 'unraisablehook', 'breakpointhook', 'flags')
_MISSING = object()
GRACE = 30.0  # seconds a level may run on after its call's timeout ran out before the process ends (see above)


class _Deadline(BaseException):
    """A level call's timeout ran out: unwinds the levels inside that call, which then raises TimeoutExpired."""


class _Guard:
    """One running level call that has a timeout."""
    __slots__ = ('at', 'timeout', 'script', 'fired')

    def __init__(self, timeout, script):
        self.at = time.monotonic() + timeout
        self.timeout = timeout
        self.script = script
        self.fired = False                       # its time ran out and its _Deadline was sent


class _Flags(tuple):
    """sys.flags as a process started with other -S setting has it: same fields, values and repr."""
    __slots__ = ()

    def __getattr__(self, name):
        try:
            return self._extra[name]
        except KeyError:
            raise AttributeError(name) from None

    def __repr__(self):
        return 'sys.flags(%s)' % ', '.join('%s=%r' % (n, getattr(self, n)) for n in self._fields)


def _flags_with(no_site):
    """A sys.flags lookalike differing from this process' only in no_site, or None if one can't be made exactly."""
    real = FLAGS
    try:
        fields = tuple(real.__match_args__)
        names = [n for n in dir(real) if not n.startswith('_') and
                 n not in ('count', 'index', 'n_fields', 'n_sequence_fields', 'n_unnamed_fields')]
        values = {n: getattr(real, n) for n in names}
        cls = type('flags', (_Flags,), {'_fields': fields, '_extra': None, '__slots__': ()})
        cls._extra = values
        same = cls(tuple(real))
        if repr(same) != repr(real) or tuple(same) != tuple(real) or any(getattr(same, n) != values[n] for n in names):
            return None
        values = dict(values, no_site=int(bool(no_site)))
        cls = type('flags', (_Flags,), {'_fields': fields, '_extra': values, '__slots__': ()})
        return cls(values[n] for n in fields)
    except Exception:
        return None


class _Start:
    """The interpreter state a new process of one kind (-S or not) starts its script with, or a level's state to
    restore when it ends."""

    def __init__(self):
        self.path = list(sys.path)
        self.modules = dict(sys.modules)
        self.meta_path = list(sys.meta_path)
        self.path_hooks = list(sys.path_hooks)
        self.builtins = dict(builtins.__dict__)
        self.hooks = {n: getattr(sys, n, _MISSING) for n in SYS_HOOKS}
        self.dont_write_bytecode = sys.dont_write_bytecode
        self.filters = warnings.filters[:]

    def apply(self):
        sys.path[:] = self.path
        sys.meta_path[:] = self.meta_path
        sys.path_hooks[:] = self.path_hooks
        sys.modules.clear()
        sys.modules.update(self.modules)
        d = builtins.__dict__
        for k in [k for k in d if k not in self.builtins]:
            del d[k]
        for k, v in self.builtins.items():
            if d.get(k, _MISSING) is not v:
                d[k] = v
        for n, v in self.hooks.items():
            if v is not _MISSING:
                setattr(sys, n, v)
        sys.dont_write_bytecode = self.dont_write_bytecode
        warnings.filters[:] = self.filters
        mutated = getattr(warnings, '_filters_mutated', None) or getattr(warnings, '_filters_mutated_lock_held', None)
        if mutated is not None:
            try:
                mutated()
            except Exception:
                pass


class _State:
    def __init__(self):
        self.base = None                         # a -I -S process (this one); set by install()
        self.site = None                         # a -I process, after site; None when it cannot be reproduced
        self.devnull = False                     # the running level's standard input is the null device
        self.stdin = sys.stdin                   # this process' standard input, which a level inherits
        self.code = {}                           # compiled scripts, by absolute path
        self.levels = []                         # (script, own seconds) per finished level, for the log
        self.stack = []                          # [seconds in nested levels and subprocesses] per running level
        self.spawned = 0
        self.spawn_seconds = 0.0
        # Timeouts of level calls. cond guards guards (the main thread alone adds and removes them), their fired flags
        # and the watcher. A fired guard stays on the stack until its own call ends, with TimeoutExpired.
        self.cond = threading.Condition()
        self.guards = []                         # _Guard per running level call with a timeout, outermost first
        self.in_code = False                     # the main thread is running a level's own code
        self.sent = self.seen = 0                # SIGINTs the watcher sent / the handler took as its own
        self.prev_sigint = None                  # the SIGINT handler before ours; None: ours is not installed
        self.watcher = None
        self.main_ident = threading.get_ident()
        self.grace = GRACE


STATE = _State()


def _find(name):
    import importlib.util
    try:
        return importlib.util.find_spec(name) is not None
    except (ImportError, ValueError):
        return True  # cannot tell: treat it as present


def _site_ready():
    """Run site once, as a process without -S does at startup, record the state it leaves, and undo it here.

    Returns whether non -S levels can run here (no sitecustomize / usercustomize would run in them)."""
    if STATE.site is not None:
        return True
    if not FLAGS.no_site or not FLAGS.isolated:
        return False  # this is not a -I -S process: a level's start state is not known here
    base = _Start()
    import site
    run_custom = (site.execsitecustomize, site.execusercustomize)
    site.execsitecustomize = site.execusercustomize = lambda: None
    try:
        site.main()
    finally:
        site.execsitecustomize, site.execusercustomize = run_custom
    custom = _find('sitecustomize') or (site.ENABLE_USER_SITE and _find('usercustomize'))
    start = _Start()
    base.apply()
    sys.path_importer_cache.clear()
    flags = _flags_with(no_site=False)
    if custom or flags is None:
        return False
    start.hooks['flags'] = flags
    STATE.site = start
    return True


def _same_interpreter(exe):
    try:
        return os.path.normcase(os.path.abspath(exe)) == os.path.normcase(os.path.abspath(sys.executable))
    except (TypeError, ValueError):
        return False


def plan(argv):
    """(script index, no_site) if argv starts this interpreter in a way this process can run, else None."""
    if not isinstance(argv, (list, tuple)) or not argv:
        return None
    try:
        argv = [os.fspath(a) for a in argv]
    except TypeError:
        return None
    if not all(isinstance(a, str) for a in argv) or not _same_interpreter(argv[0]):
        return None
    utf8 = isolated = no_site = False
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == '-X' and i + 1 < len(argv) and argv[i + 1] == 'utf8':
            utf8 = True
            i += 2
        elif a == '-Xutf8':
            utf8 = True
            i += 1
        elif a == '-I':
            isolated = True
            i += 1
        elif a == '-S':
            no_site = True
            i += 1
        else:
            break
    if i >= len(argv) or argv[i].startswith('-') or not argv[i].endswith('.py') or not os.path.isfile(argv[i]):
        return None
    if utf8 != bool(FLAGS.utf8_mode) or not isolated or not FLAGS.isolated or not FLAGS.no_site:
        return None
    if STATE.base is None or (not no_site and STATE.site is None):
        return None
    return i, no_site


def _exit_status(code):
    if code is None:
        return 0
    if isinstance(code, int):
        return code
    try:
        sys.stderr.write(str(code) + '\n')
    except Exception:
        pass
    return 1


def _process_status(status):
    """The return code the parent of a process exiting with this status would see."""
    return status & 0xFFFFFFFF if IS_WINDOWS else status & 0xFF


def _flush():
    for stream in (sys.stdout, sys.stderr):
        try:
            if stream is not None:
                stream.flush()
        except Exception:
            pass


# ------------------------------------------------------------------------------------------- level call timeouts

def _owner():
    """The running guard whose time runs out first (the outermost of equal ones): the call that the chain of
    processes would have timed out first, killing the levels inside it."""
    best = None
    for g in STATE.guards:
        if best is None or g.at < best.at:
            best = g
    return best


def _checkpoint():
    """Raise _Deadline while a running level call's time has run out. It is raised again at every boundary between
    dispatcher and level code until that call ends: a level that catches it cannot return as if in time."""
    if any(g.fired for g in STATE.guards):
        raise _Deadline()


def _code(on):
    """The main thread now runs a level's own code (on) or the dispatcher's; an owed _Deadline is raised here."""
    STATE.in_code = on
    _checkpoint()


def _on_sigint(signum, frame):
    if STATE.sent != STATE.seen:  # the watcher's: deliver the deadline if level code is running, else at _code()
        STATE.seen = STATE.sent
        try:
            signal.signal(signal.SIGINT, _on_sigint)  # a C runtime may have reset it before calling
        except (OSError, ValueError):
            pass
        if STATE.in_code:
            _checkpoint()
        return None
    prev = STATE.prev_sigint
    if callable(prev):
        return prev(signum, frame)
    if prev == signal.SIG_IGN:
        return None
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    signal.raise_signal(signal.SIGINT)
    return None


def _hook_sigint():
    if STATE.prev_sigint is not None:
        return
    try:
        prev = signal.getsignal(signal.SIGINT)
        if prev is None:  # installed outside Python: leave it; timed-out Python code then ends by the backstop
            return
        signal.signal(signal.SIGINT, _on_sigint)
        STATE.prev_sigint = prev
    except (OSError, ValueError, AttributeError):
        pass


def _interrupt_main():
    """Wake the main thread (also out of time.sleep and other interruptible waits) to take its _Deadline."""
    if STATE.prev_sigint is None:
        return
    STATE.sent += 1
    try:
        if IS_WINDOWS:
            signal.raise_signal(signal.SIGINT)  # runs the C handler here; it wakes the main thread's waits
        else:
            signal.pthread_kill(STATE.main_ident, signal.SIGINT)
    except (OSError, ValueError, AttributeError):
        pass


def _backstop(g):
    msg = ('launch_dispatch: %s still running %.0f s after its %g s timeout ran out; exiting with status 1\n' %
           (_script_name(g.script), STATE.grace, g.timeout))
    try:
        os.write(2, msg.encode('utf-8', 'replace'))
    except OSError:
        pass
    os._exit(1)


def _script_name(argv):
    p = plan(argv) if isinstance(argv, (list, tuple)) else None
    return os.path.basename(os.fspath(argv[p[0]])) if p else repr(argv)[:200]


def _watch():
    cond = STATE.cond
    with cond:
        while True:
            g = _owner()
            now = time.monotonic()
            if g is None:
                cond.wait()
            elif not g.fired:
                if now < g.at:
                    cond.wait(g.at - now)
                else:
                    g.fired = True
                    _interrupt_main()
            elif now < g.at + STATE.grace:
                cond.wait(g.at + STATE.grace - now)
            else:
                _backstop(g)


def _push(timeout, args):
    g = _Guard(timeout, args)
    _hook_sigint()
    with STATE.cond:
        STATE.guards.append(g)
        if STATE.watcher is None:
            STATE.watcher = threading.Thread(target=_watch, name='launch_dispatch deadline', daemon=True)
            STATE.watcher.start()
        STATE.cond.notify()
    return g


def _pop(g):
    """Remove the guard of an ending call; True when its time ran out (it can no longer fire after this)."""
    with STATE.cond:
        STATE.guards.remove(g)
        STATE.cond.notify()
        return g.fired


def _left():
    """(guard, seconds) of the first deadline under which a real subprocess now starts; (None, None) without one.
    Raises _Deadline when that time has already run out."""
    with STATE.cond:
        g = _owner()
        if g is None:
            return None, None
        left = g.at - time.monotonic()
        if left <= 0:
            g.fired = True
            STATE.cond.notify()
    if left <= 0:
        raise _Deadline()
    return g, left


def run_level(argv, index, no_site, devnull):
    """Run argv[index] (a .py file) as a new process would, here; return its exit status."""
    script = argv[index]
    path = os.path.abspath(script)
    code = STATE.code.get(path)
    if code is None:
        with open(path, 'rb') as f:
            code = compile(f.read(), path, 'exec', dont_inherit=True)
        STATE.code[path] = code
    saved = _Start()
    saved_argv = sys.argv
    saved_stdin = sys.stdin
    saved_env = dict(os.environ)
    saved_cwd = os.getcwd()
    saved_devnull = STATE.devnull
    null_in = None
    began = time.perf_counter()
    STATE.stack.append(0.0)
    try:
        sys.argv = list(argv[index:])
        (STATE.base if no_site else STATE.site).apply()
        module = types.ModuleType('__main__')
        module.__file__ = path
        module.__loader__ = SourceFileLoader('__main__', path)
        module.__builtins__ = builtins
        module.__cached__ = None
        sys.modules['__main__'] = module
        STATE.devnull = devnull
        if devnull:
            null_in = open(os.devnull, 'r', encoding=getattr(STATE.stdin, 'encoding', None) or 'utf-8')
            sys.stdin = null_in
        else:
            sys.stdin = STATE.stdin
        try:
            _code(True)
            exec(code, module.__dict__)
            status = 0
        except _Deadline:
            STATE.in_code = False
            raise
        except SystemExit as e:
            STATE.in_code = False
            status = _exit_status(e.code)
        except KeyboardInterrupt:
            STATE.in_code = False
            raise
        except BaseException as e:  # what the interpreter does with an uncaught exception
            STATE.in_code = False
            tb = e.__traceback__.tb_next if e.__traceback__ is not None else None
            e.__traceback__ = tb  # drop this frame: the traceback starts in the script, as in its own process
            try:
                sys.excepthook(type(e), e, tb)
            except Exception:
                traceback.print_exception(type(e), e, tb)
            status = 1
        _code(False)  # a deadline that ran out as the level ended still unwinds its call
        _flush()
        return _process_status(status)
    finally:
        STATE.in_code = False
        spent = time.perf_counter() - began
        STATE.levels.append((os.path.basename(script), spent - STATE.stack.pop()))
        if STATE.stack:
            STATE.stack[-1] += spent
        saved.apply()
        sys.argv = saved_argv
        sys.stdin = saved_stdin
        if null_in is not None:
            null_in.close()
        if dict(os.environ) != saved_env:
            os.environ.clear()
            os.environ.update(saved_env)
        if os.getcwd() != saved_cwd:
            os.chdir(saved_cwd)
        STATE.devnull = saved_devnull


def _child_devnull(kwargs):
    stdin = kwargs.get('stdin')
    return stdin is DEVNULL or (stdin is None and STATE.devnull)


def _level_call(args, kwargs):
    """(index, no_site) when this subprocess call can run as a level here, else None."""
    if not set(kwargs) <= EMULATED_KEYS:
        return None
    if kwargs.get('stdin') not in (None, DEVNULL):
        return None
    timeout = kwargs.get('timeout')
    if timeout is not None and (isinstance(timeout, bool) or not isinstance(timeout, (int, float))):
        return None
    return plan(args)


def _spawn(real, popenargs, kwargs):
    if STATE.devnull and kwargs.get('stdin') is None and kwargs.get('input') is None:
        kwargs = dict(kwargs, stdin=DEVNULL)
    guard, left = _left()
    if guard is not None:
        own = kwargs.get('timeout')
        if own is None or own > left:
            kwargs = dict(kwargs, timeout=left)  # the time its level call has left bounds it
        else:
            guard = None  # its own timeout comes first: TimeoutExpired is the caller's, as before
    _flush()
    began = time.perf_counter()
    try:
        return real(*popenargs, **kwargs)
    except subprocess.TimeoutExpired:
        if guard is None:
            raise
        with STATE.cond:
            guard.fired = True
            STATE.cond.notify()
        raise _Deadline() from None
    finally:
        spent = time.perf_counter() - began
        STATE.spawned += 1
        STATE.spawn_seconds += spent
        if STATE.stack:
            STATE.stack[-1] += spent


def _call_level(args, level, rest):
    """Run a level call; when its timeout runs out, raise TimeoutExpired as the real call would."""
    timeout = rest.get('timeout')
    argv = list(map(os.fspath, args))
    if timeout is None:
        return run_level(argv, level[0], level[1], _child_devnull(rest))
    guard = _push(timeout, args)
    try:
        status = run_level(argv, level[0], level[1], _child_devnull(rest))
    except _Deadline:
        if not _pop(guard):
            raise  # an outer call's time ran out first
        raise subprocess.TimeoutExpired(args, timeout) from None
    except BaseException:
        _pop(guard)
        raise
    if _pop(guard):  # its time ran out, whatever the level did with the _Deadline: never a success
        raise subprocess.TimeoutExpired(args, timeout)
    return status


def dispatch_run(*popenargs, **kwargs):
    if threading.get_ident() != STATE.main_ident:  # levels run on the main thread only
        return REAL_RUN(*popenargs, **kwargs)
    _code(False)
    try:
        args = popenargs[0] if popenargs else kwargs.get('args')
        rest = {k: v for k, v in kwargs.items() if k != 'args'}
        level = _level_call(args, rest) if len(popenargs) <= 1 else None
        if level is None:
            return _spawn(REAL_RUN, popenargs, kwargs)
        status = _call_level(args, level, rest)
        if rest.get('check') and status:
            raise subprocess.CalledProcessError(status, args)
        return subprocess.CompletedProcess(args, status)
    finally:
        _code(True)


def dispatch_call(*popenargs, **kwargs):
    if threading.get_ident() != STATE.main_ident:
        return REAL_CALL(*popenargs, **kwargs)
    _code(False)
    try:
        args = popenargs[0] if popenargs else kwargs.get('args')
        rest = {k: v for k, v in kwargs.items() if k != 'args'}
        level = _level_call(args, rest) if len(popenargs) <= 1 and 'check' not in rest else None
        if level is None:
            return _spawn(REAL_CALL, popenargs, kwargs)
        return _call_level(args, level, rest)
    finally:
        _code(True)


def install():
    """Take the -I -S start state (after _site_ready) and catch subprocess.run / call."""
    STATE.base = _Start()
    STATE.stdin = sys.stdin
    STATE.main_ident = threading.get_ident()
    subprocess.run = dispatch_run
    subprocess.call = dispatch_call


def _source_of(argv):
    for i, a in enumerate(argv):
        if a == '-c' and i + 1 < len(argv):
            return argv[i + 1]
    return '?'


def log(argv, status):
    path = os.environ.get('CTRPORT_LAUNCH_DISPATCH_LOG')
    if not path:
        return
    total = time.perf_counter() - START
    levels = ','.join('%s:%d' % (name[:-3] if name.endswith('.py') else name, sec * 1000)
                      for name, sec in STATE.levels)
    line = 'compile\t%s\tstatus=%d total_ms=%d levels=%d spawned=%d spawn_ms=%d\t%s\n' % (
        _source_of(argv), status, total * 1000, len(STATE.levels), STATE.spawned, STATE.spawn_seconds * 1000, levels)
    try:
        with open(path, 'a', encoding='utf-8') as f:
            f.write(line)
    except OSError:
        pass


def summary(path):
    n = levels = spawned = 0
    total = spawn = 0
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                parts = line.rstrip('\n').split('\t')
                if len(parts) < 3 or parts[0] != 'compile':
                    continue
                fields = dict(x.split('=', 1) for x in parts[2].split() if '=' in x)
                n += 1
                total += int(fields.get('total_ms', 0))
                spawn += int(fields.get('spawn_ms', 0))
                levels += int(fields.get('levels', 0))
                spawned += int(fields.get('spawned', 0))
    except OSError:
        return 'launch dispatch: no log'
    if not n:
        return 'launch dispatch: 0 compiles'
    return ('launch dispatch: %d compiles, %d launcher levels run in-process, %d subprocesses; %.2f s per compile, '
            '%.2f s of it in subprocesses' % (n, levels, spawned, total / 1000.0 / n, spawn / 1000.0 / n))


def main(argv):
    if len(argv) >= 2 and argv[0] == '--summary':
        print(summary(argv[1]))
        return 0
    if not argv:
        sys.stderr.write('usage: launch_dispatch.py <launcher chain and compiler command...>\n')
        return 2
    if os.environ.get('CTRPORT_LAUNCH_DISPATCH', '1') == '0':
        return REAL_CALL(argv)
    try:
        STATE.grace = max(0.0, float(os.environ.get('CTRPORT_LAUNCH_DISPATCH_GRACE', GRACE)))
    except ValueError:
        pass
    _site_ready()
    install()
    level = plan(argv)
    try:
        if level is None:
            status = _spawn(REAL_CALL, (argv,), {})
        else:
            status = run_level(list(argv), level[0], level[1], False)
    except _Deadline:  # not expected: every _Deadline belongs to a level call inside
        sys.stderr.write('launch_dispatch: a level call timed out\n')
        status = 1
    log(argv, status)
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
