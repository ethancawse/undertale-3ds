"""subprocess.run for tests, with every wait bounded.

subprocess.run(..., capture_output=True, timeout=N) kills only the process it started. When that process started
children of its own (bash starting python, a launcher starting a compiler), a child still holding the pipe keeps
run()'s second wait (communicate after the kill) open for as long as the child lives. integrate21 found one such
child that never ran (created suspended on a loaded machine): the release suite then sat for hours
(test_build_python ...survives_empty_path, 11-12).

What run() does differently, to bound every wait:
- Output goes to temporary files, never pipes, so nothing a child inherits can hold the call open.
- The process starts inside its own kill group. On Windows it is created suspended, joined to a new job object,
  and only then resumed, so everything it starts is in the job; if the job cannot be made or joined (an enclosing
  job may forbid it), or the resume fails, the still-suspended process is terminated, reaped (bounded wait) and the
  error raised: it never runs uncontained. Elsewhere it starts in a new session (process group).
- On timeout the whole group is killed, the wait for the started process is bounded by KILL_WAIT, and
  subprocess.TimeoutExpired is raised. Its .tree_killed says whether the group kill succeeded (False: only the
  started process was killed, and one line on stderr says so).

The timeout output contract: TimeoutExpired.output / .stdout / .stderr hold the RAW BYTES captured so far (None
for a stream that was not captured), whatever text/encoding were asked for. That is subprocess's documented
contract and what POSIX CPython gives; Windows CPython instead decodes after its own second wait, which is the
wait this module exists to bound. Raw bytes also mean a child killed in the middle of an encoded character can
never turn the timeout into a UnicodeDecodeError. A run that completes (CompletedProcess, CalledProcessError)
decodes as subprocess does, universal newlines included.

Supported arguments (this is not a general subprocess replacement): args, timeout (required), input,
capture_output, stdin (None, DEVNULL or a file), stdout/stderr (None, PIPE, DEVNULL, STDOUT for stderr, or a
file), check, text/universal_newlines, encoding, errors, and Popen keywords that do not touch stdio or process
groups (cwd, env, creationflags, startupinfo, ...). Not supported: stdin=PIPE without input, a string input
without text/encoding, and anything that needs a real pipe (stream type, backpressure, waiting for EOF).

Known differences from pipe capture:
- When the started process exits in time while a child of it still holds the output, run() returns what was
  written so far (a snapshot); it does not wait for that child's EOF, and the child lives on, as with
  subprocess.run's own success path.
- Output is spooled on disk but read back into memory whole: it is not memory bounded.
- No kill-on-close: if the Python running the test is itself killed, the job does not take the tree with it.
- POSIX: the group kill does not reach a descendant that called setsid() or changed its process group.
"""
import contextlib
import locale
import os
import signal
import subprocess
import sys
import tempfile
import time
import warnings

KILL_WAIT = 10  # seconds the killed process gets to exit before run() gives up waiting and raises anyway

# Timeouts by class. A test timeout is a hang detector, not a speed gate: the tree kill above makes a generous one
# free when the test passes. Each class is one constant so it comes back down in one edit.
# BASH_TIMEOUT: tests that start bash, which starts python (and sometimes more). Sized 11-12 to a loaded machine
# (900-1,900 live python.exe) where one python CreateProcess took 0.5-10.3 s; the old 10-30 s limits failed there
# with nothing wrong. Once a python start is back under 1 s (toolfix launch_chain lands), set it to 30.
BASH_TIMEOUT = 120
# A process start slower than this prints one line to stderr (it never fails the test), so a generous timeout
# cannot hide the next slowdown: the line says what is slow while the tests still pass.
SLOW_START = 5.0

if os.name == 'nt':
    import ctypes
    from ctypes import wintypes

    _k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    _k32.CreateJobObjectW.restype = wintypes.HANDLE
    _k32.CreateJobObjectW.argtypes = (wintypes.LPVOID, wintypes.LPCWSTR)
    _k32.AssignProcessToJobObject.argtypes = (wintypes.HANDLE, wintypes.HANDLE)
    _k32.AssignProcessToJobObject.restype = wintypes.BOOL
    _k32.TerminateJobObject.argtypes = (wintypes.HANDLE, wintypes.UINT)
    _k32.TerminateJobObject.restype = wintypes.BOOL
    _k32.CloseHandle.argtypes = (wintypes.HANDLE,)
    _ntdll = ctypes.WinDLL('ntdll')
    _ntdll.NtResumeProcess.argtypes = (wintypes.HANDLE,)
    _ntdll.NtResumeProcess.restype = ctypes.c_long  # NTSTATUS
    CREATE_SUSPENDED = 0x00000004

    # The four job steps, one function each so tests can make each one fail.
    def _create_job():
        job = _k32.CreateJobObjectW(None, None)
        if not job:
            raise ctypes.WinError(ctypes.get_last_error())
        return job

    def _assign(job, handle):
        if not _k32.AssignProcessToJobObject(job, handle):
            raise ctypes.WinError(ctypes.get_last_error())

    def _resume(handle):
        status = _ntdll.NtResumeProcess(handle)
        if status < 0:  # NT_ERROR / NT_WARNING high bit: not resumed
            raise OSError('NtResumeProcess failed: NTSTATUS 0x%08X' % (status & 0xFFFFFFFF))

    def _terminate_job(job):
        return bool(_k32.TerminateJobObject(job, 1))


class ContainmentError(OSError):
    """The process could not be put in its kill group; it was terminated before it ran."""


class _Group:
    """The started process and everything it starts, killable as one."""

    def __init__(self, popen_kwargs):
        self.job = None
        self.ended = False  # killed and reaped (bounded) already: no second kill or wait
        if os.name == 'nt':
            popen_kwargs['creationflags'] = popen_kwargs.get('creationflags', 0) | CREATE_SUSPENDED
        else:
            popen_kwargs['start_new_session'] = True

    def started(self, proc):
        """Windows: contain, then resume. Any failure: terminate and reap the still-suspended process, raise."""
        if os.name != 'nt':
            return
        handle = int(proc._handle)
        step = 'create the job object'
        try:
            self.job = _create_job()
            step = 'assign the process to its job object'
            _assign(self.job, handle)
            step = 'resume the contained process'
            _resume(handle)
        except BaseException as error:
            self.kill(proc)  # the job holds it (resume failed) or nothing does (it is still suspended): both end it
            self.close()
            _reap(proc)
            self.ended = True
            if isinstance(error, Exception):
                raise ContainmentError('testproc: could not %s (%s); the process was terminated before it ran'
                                       % (step, error)) from error
            raise

    def kill(self, proc):
        """Kill the group; True when the whole tree was killed, False when only the started process was."""
        tree = False
        if os.name == 'nt':
            if self.job:
                try:
                    tree = _terminate_job(self.job)
                except OSError:
                    tree = False
            try:
                proc.kill()
            except OSError:
                pass
        else:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
                tree = True
            except (OSError, ProcessLookupError):
                try:
                    proc.kill()
                except OSError:
                    pass
        return tree

    def close(self):
        if self.job:
            _k32.CloseHandle(self.job)  # no KILL_ON_JOB_CLOSE: children of a run that ended in time live on
            self.job = None


def _reap(proc):
    try:
        proc.wait(timeout=KILL_WAIT)
    except subprocess.TimeoutExpired:
        pass


def _decode(data, text, encoding, errors):
    if data is None or not (text or encoding or errors):
        return data
    value = data.decode(encoding or locale.getpreferredencoding(False), errors or 'strict')
    return value.replace('\r\n', '\n').replace('\r', '\n')  # universal newlines, as text=True pipes give


def _raw(handle):
    if handle is None:
        return None
    handle.flush()
    handle.seek(0)
    return handle.read()


def _note_slow_start(args, seconds):
    """The soft line: process creation (CreateProcess / fork+exec, until the process may run) over SLOW_START."""
    if seconds > SLOW_START:
        program = args if isinstance(args, (str, bytes)) else (args[0] if args else '?')
        line = ('testproc: SLOW START %.1f s for %s (over %.0f s; not a failure: process creation on this machine '
                'is slow, see tools/testproc.py)' % (seconds, os.path.basename(os.fsdecode(program)), SLOW_START))
        print(line, file=sys.stderr, flush=True)  # the run's own log (unittest, or pytest -s)
        try:
            warnings.warn(SlowStart(line), stacklevel=3)  # pytest lists it in its warnings summary of a passing run
        except Exception:  # noqa: BLE001  -W error must not turn the soft line into a failure
            pass


class SlowStart(UserWarning):
    """A process start took over SLOW_START seconds (never an error; see _note_slow_start)."""


def run(args, *, timeout, input=None, capture_output=False, stdin=None, stdout=None, stderr=None, check=False,
        text=None, encoding=None, errors=None, universal_newlines=None, **popen_kwargs):
    """subprocess.run(args, ..., timeout=timeout) with output in files and a tree kill on timeout (module doc)."""
    if timeout is None:
        raise ValueError('testproc.run needs a timeout')
    text = text or universal_newlines
    if capture_output:
        if stdout is not None or stderr is not None:
            raise ValueError('stdout and stderr arguments may not be used with capture_output.')
        stdout = stderr = subprocess.PIPE
    if input is not None and stdin is not None:
        raise ValueError('stdin and input arguments may not both be used.')
    if stdin == subprocess.PIPE:
        raise ValueError('testproc.run: stdin=PIPE is not supported; pass input=')
    with contextlib.ExitStack() as cleanup:
        def spool(kind):
            return cleanup.enter_context(tempfile.TemporaryFile(prefix='testproc-' + kind + '-'))

        out_file = spool('out') if stdout == subprocess.PIPE else None
        err_file = spool('err') if stderr == subprocess.PIPE else None
        if input is not None:
            data = input
            if isinstance(data, str):
                data = data.encode(encoding or locale.getpreferredencoding(False), errors or 'strict')
            in_file = spool('in')
            in_file.write(data)
            in_file.seek(0)
            stdin = in_file
        group = _Group(popen_kwargs)
        cleanup.callback(group.close)
        begun = time.monotonic()
        proc = subprocess.Popen(args, stdin=stdin, stdout=out_file if out_file else stdout,
                                stderr=err_file if err_file else stderr, **popen_kwargs)
        try:
            group.started(proc)
            start_seconds = time.monotonic() - begun
            _note_slow_start(args, start_seconds)
            try:
                proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                expired = time.monotonic()
                tree = group.kill(proc)
                if not tree:
                    print('testproc: TREE KILL FAILED on timeout; only the started process was killed (%s)'
                          % (os.fsdecode(args if isinstance(args, (str, bytes)) else args[0]),),
                          file=sys.stderr, flush=True)
                _reap(proc)
                group.ended = True
                timed_out = subprocess.TimeoutExpired(args, timeout, output=_raw(out_file), stderr=_raw(err_file))
                timed_out.tree_killed = tree
                timed_out.start_seconds = start_seconds  # process creation, before the timeout clock started
                timed_out.after_timeout_seconds = time.monotonic() - expired  # kill + bounded reap
                raise timed_out from None
        except BaseException:
            if proc.returncode is None and not group.ended:  # e.g. KeyboardInterrupt during the wait
                group.kill(proc)
                _reap(proc)
            raise
        result = subprocess.CompletedProcess(args, proc.returncode, _decode(_raw(out_file), text, encoding, errors),
                                             _decode(_raw(err_file), text, encoding, errors))
    if check and result.returncode:
        raise subprocess.CalledProcessError(result.returncode, args, result.stdout, result.stderr)
    return result


def check_output(args, *, timeout, **kwargs):
    """subprocess.check_output with run()'s bounded waits."""
    kwargs.setdefault('stdout', subprocess.PIPE)
    return run(args, timeout=timeout, check=True, **kwargs).stdout
