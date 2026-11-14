"""testproc.run: every wait bounded, with a stuck grandchild injected (it only sleeps; no load).

AS A GATE: a run certifies testproc only if the witnesses RAN, not skipped: the tree-kill proof
(test_new_helper_returns_within_the_bound_and_kills_the_tree), the raw-output contract
(test_timeout_output_is_raw_bytes_even_mid_character) and the failed-kill report
(test_a_failed_tree_kill_is_reported_not_certified). Each skips as inconclusive when its child never started in
time. A run where they skipped is green and certifies nothing: read pytest's -rs line, never only the count."""
import os
import subprocess
import sys
import threading
import time
import warnings
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import testproc  # noqa: E402

PY = [sys.executable, '-X', 'utf8', '-I']
STUCK = 120  # seconds the grandchild sleeps holding the inherited stdout and stderr
TIMEOUT = 20  # long enough for the child to start its grandchild: a python start took up to 10 s on a loaded machine


# Windows: a process is named by (pid, creation time), never by PID alone: a busy machine hands a dead process's
# PID to an unrelated one within the second, and these tests kill what they name. The same source runs in the child
# (on the grandchild's handle) and here (on a handle opened by PID), so the two times are read the same way.
# POSIX has no such stable identity here (creation time is written as 0), so on POSIX these helpers never kill:
# kill_exact only reports a still-running PID for a human, and alive() is a liveness hint that a reused PID can fool.
CREATED_SRC = '''
def created(handle):
    import os
    if os.name != "nt":
        return 0
    import ctypes
    from ctypes import wintypes
    times = [wintypes.FILETIME() for _ in range(4)]
    k32 = ctypes.WinDLL("kernel32")
    k32.GetProcessTimes.argtypes = (wintypes.HANDLE,) + (ctypes.POINTER(wintypes.FILETIME),) * 4
    if not k32.GetProcessTimes(handle, *[ctypes.byref(t) for t in times]):
        return None
    return times[0].dwHighDateTime << 32 | times[0].dwLowDateTime
'''
exec(CREATED_SRC)  # defines created(handle)


def write_identity_src(pid_file):
    """Child source: write 'pid created' for its Popen p (the grandchild), atomically (the test may poll it)."""
    return (CREATED_SRC + 'import os\n'
            'open(%r, "w").write("%%d %%d" %% (p.pid, created(int(p._handle)) if os.name == "nt" else 0))\n'
            'os.replace(%r, %r)\n' % (str(pid_file) + '.tmp', str(pid_file) + '.tmp', str(pid_file)))


def parent_with_stuck_child(pid_file):
    """A child that starts a sleeping grandchild (inheriting its stdout and stderr), prints, then waits on it."""
    grandchild = 'import time; time.sleep(%d)' % STUCK
    body = ('import subprocess, sys\np = subprocess.Popen([sys.executable, "-I", "-c", %r])\n' % grandchild
            + write_identity_src(pid_file) + 'print("started", flush=True)\np.wait()\n')
    return PY + ['-c', body]


def _open(ident):
    """Windows: a handle to the process ident names, or None when it is gone (or the PID is someone else's)."""
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL('kernel32')
    k32.OpenProcess.restype = wintypes.HANDLE
    handle = k32.OpenProcess(0x00101001, False, ident[0])  # SYNCHRONIZE | QUERY_LIMITED_INFORMATION | TERMINATE
    if not handle:
        return None
    if created(handle) != ident[1]:
        k32.CloseHandle(wintypes.HANDLE(handle))
        return None
    return handle


def alive(ident):
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes
        handle = _open(ident)
        if handle is None:
            return False
        k32 = ctypes.WinDLL('kernel32')
        try:
            return k32.WaitForSingleObject(wintypes.HANDLE(handle), 0) == 0x102  # WAIT_TIMEOUT: still running
        finally:
            k32.CloseHandle(wintypes.HANDLE(handle))
    try:
        os.kill(ident[0], 0)
    except ProcessLookupError:
        return False
    return True


def kill_exact(ident):
    """Windows: kill the exact process ident names (same PID and creation time), through a handle, so no PID race.
    POSIX: never kills (no stable identity); a still-running PID is reported on stderr instead."""
    if ident is None:
        return
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes
        handle = _open(ident)
        if handle is not None:
            k32 = ctypes.WinDLL('kernel32')
            k32.TerminateProcess(wintypes.HANDLE(handle), 1)
            k32.CloseHandle(wintypes.HANDLE(handle))
    elif alive(ident):
        # No stable ownership on POSIX (see CREATED_SRC): never signal a bare PID. Report it instead.
        print('test_testproc: pid %d may be a leftover of this test; not killed (no stable identity on POSIX)'
              % ident[0], file=sys.stderr)


def read_pid(pid_file):
    """(pid, creation time) the child wrote, or None."""
    try:
        text = pid_file.read_text().split()
    except OSError:
        return None
    return (int(text[0]), int(text[1])) if len(text) == 2 else None


def wait_until_gone(ident, bound=10):
    end = time.monotonic() + bound
    while alive(ident) and time.monotonic() < end:
        time.sleep(0.1)
    return not alive(ident)


@pytest.mark.skipif(os.name != 'nt', reason='subprocess.run waits on the pipe readers after a kill only on Windows')
def test_old_pattern_hangs_on_a_stuck_grandchild(tmp_path):
    """The pattern being replaced: subprocess.run(capture_output, timeout) outlives its timeout by the grandchild.
    Readiness is the grandchild's PID file, not the clock: if the old call returns before its grandchild existed
    (process starts slower than TIMEOUT), the run proves nothing and is skipped, never passed or failed."""
    pid_file = tmp_path / 'grandchild.pid'
    outcome = {}

    def old():
        try:
            subprocess.run(parent_with_stuck_child(pid_file), capture_output=True, text=True, timeout=TIMEOUT,
                           stdin=subprocess.DEVNULL)
            outcome['result'] = 'returned'
        except subprocess.TimeoutExpired:
            outcome['result'] = 'timeout'
            outcome['at'] = time.monotonic()

    thread = threading.Thread(target=old, daemon=True)
    thread.start()
    while read_pid(pid_file) is None and thread.is_alive():
        time.sleep(0.1)
    pid = read_pid(pid_file)
    if pid is None:
        pytest.skip('inconclusive: the child had not started its grandchild when its %d s timeout ran out' % TIMEOUT)
    ready = time.monotonic()
    try:
        # From the moment the grandchild exists, the old call has at most TIMEOUT left before its kill; give it
        # that plus a KILL_WAIT and 10 s more. It is still waiting on the grandchild's pipe.
        thread.join(TIMEOUT + testproc.KILL_WAIT + 10)
        assert thread.is_alive(), 'old pattern returned %s %.1f s after readiness' % (outcome, time.monotonic() - ready)
    finally:
        kill_exact(pid)  # the exact grandchild this test started; the old call then returns
        thread.join(60)
    assert outcome.get('result') == 'timeout'


PROOF_TIMEOUT = 45  # long enough for a child and its grandchild to start on a loaded machine (10.3 s starts seen)


def test_new_helper_returns_within_the_bound_and_kills_the_tree(tmp_path):
    """Process creation is not covered by any timeout (subprocess's own rule), so the bound is checked where it
    applies: from the timeout's expiry to the raise. Readiness is the grandchild's PID file."""
    pid_file = tmp_path / 'grandchild.pid'
    with pytest.raises(subprocess.TimeoutExpired) as raised:
        testproc.run(parent_with_stuck_child(pid_file), capture_output=True, text=True, timeout=PROOF_TIMEOUT,
                     stdin=subprocess.DEVNULL)
    pid = read_pid(pid_file)
    if pid is None:
        pytest.skip('inconclusive: no grandchild within %d s of the child starting' % PROOF_TIMEOUT)
    try:
        error = raised.value
        assert error.tree_killed is True
        assert error.after_timeout_seconds < testproc.KILL_WAIT + 2, error.after_timeout_seconds
        assert error.start_seconds >= 0
        assert b'started' in error.stdout  # raw bytes: the timeout output contract (module doc)
        assert wait_until_gone(pid), 'grandchild survived the tree kill'
    finally:
        kill_exact(pid)


@pytest.mark.skipif(os.name != 'nt', reason='the delay is injected at the Windows resume step')
def test_a_slow_start_does_not_count_against_the_bound(monkeypatch):
    """Deterministic delayed-start control: 3 s injected before the contained process is resumed."""
    real_resume = testproc._resume

    def slow_resume(handle):
        time.sleep(3)
        real_resume(handle)

    monkeypatch.setattr(testproc, '_resume', slow_resume)
    began = time.monotonic()
    with pytest.raises(subprocess.TimeoutExpired) as raised:
        testproc.run(PY + ['-c', 'import time; time.sleep(%d)' % STUCK], capture_output=True, timeout=2,
                     stdin=subprocess.DEVNULL)
    took = time.monotonic() - began
    error = raised.value
    assert error.start_seconds >= 3 and took >= 3 + 2
    assert error.after_timeout_seconds < testproc.KILL_WAIT + 2
    assert error.tree_killed is True


def test_timeout_output_is_raw_bytes_even_mid_character(tmp_path):
    """TP-2: a child killed after the first byte of a two-byte UTF-8 character. text/encoding were asked for, but
    the timeout carries the raw bytes (the timeout is never replaced by a UnicodeDecodeError)."""
    ready = tmp_path / 'ready'
    body = ('import sys, time; sys.stdout.buffer.write(b"\\xc3"); sys.stdout.buffer.flush(); '
            'sys.stderr.buffer.write(b"e\\xc3"); sys.stderr.buffer.flush(); open(%r, "w").close(); time.sleep(%d)'
            % (str(ready), STUCK))
    with pytest.raises(subprocess.TimeoutExpired) as raised:
        testproc.run(PY + ['-c', body], capture_output=True, text=True, encoding='utf-8', timeout=PROOF_TIMEOUT,
                     stdin=subprocess.DEVNULL)
    if not ready.exists():
        pytest.skip('inconclusive: the child had not written within %d s of starting' % PROOF_TIMEOUT)
    assert raised.value.stdout == raised.value.output == b'\xc3'
    assert raised.value.stderr == b'e\xc3'


def test_timeout_without_capture_has_no_output():
    with pytest.raises(subprocess.TimeoutExpired) as raised:
        testproc.run(PY + ['-c', 'import time; time.sleep(%d)' % STUCK], timeout=2, stdin=subprocess.DEVNULL,
                     stdout=subprocess.DEVNULL)
    assert raised.value.stdout is None and raised.value.stderr is None


def marker_child(marker):
    """A child whose first act is to write a marker file: if it ever ran, the file exists."""
    return PY + ['-c', 'open(%r, "w").close(); import time; time.sleep(%d)' % (str(marker), STUCK)]


def reaped_procs(monkeypatch):
    """The Popen objects run() reaps. Liveness is read from them (their handle), never by PID: once the handle is
    closed a busy machine can give the PID to an unrelated process within the second."""
    procs = []
    real = testproc._reap

    def record(proc):
        procs.append(proc)
        real(proc)

    monkeypatch.setattr(testproc, '_reap', record)
    return procs


@pytest.mark.skipif(os.name != 'nt', reason='Windows job containment')
@pytest.mark.parametrize('step', ['_create_job', '_assign', '_resume'])
def test_containment_failure_never_runs_the_process(tmp_path, monkeypatch, step):
    """TP-1: job creation, assignment or resume fails -> the process is terminated before it runs, then reaped."""
    def fail(*args):
        raise OSError('injected failure at ' + step)

    monkeypatch.setattr(testproc, step, fail)
    procs = reaped_procs(monkeypatch)
    marker = tmp_path / 'ran'
    with pytest.raises(testproc.ContainmentError, match='injected failure'):
        testproc.run(marker_child(marker), capture_output=True, timeout=120, stdin=subprocess.DEVNULL)
    assert len(procs) == 1
    try:
        assert procs[0].poll() is not None, 'the uncontained process was left running'
        assert not marker.exists(), 'the process ran before containment'
    finally:
        if procs[0].poll() is None:
            procs[0].kill()  # through its own handle, never by PID
            procs[0].wait(30)


@pytest.mark.skipif(os.name != 'nt', reason='Windows job containment')
def test_a_failed_tree_kill_is_reported_not_certified(tmp_path, monkeypatch, capsys):
    """TerminateJobObject fails -> only the root is killed; the timeout says tree_killed False and a line says so."""
    monkeypatch.setattr(testproc, '_terminate_job', lambda job: False)
    pid_file = tmp_path / 'grandchild.pid'
    with pytest.raises(subprocess.TimeoutExpired) as raised:
        testproc.run(parent_with_stuck_child(pid_file), capture_output=True, timeout=PROOF_TIMEOUT,
                     stdin=subprocess.DEVNULL)
    pid = read_pid(pid_file)
    if pid is None:
        pytest.skip('inconclusive: no grandchild within %d s of the child starting' % PROOF_TIMEOUT)
    try:
        assert raised.value.tree_killed is False
        assert 'TREE KILL FAILED' in capsys.readouterr().err
    finally:
        if pid is not None:
            kill_exact(pid)  # the grandchild the failed tree kill left; exact PID


def test_a_failed_second_spool_closes_the_first(monkeypatch):
    """TP-3: every spool and the input are acquired inside one cleanup scope."""
    made = []
    real = testproc.tempfile.TemporaryFile

    def flaky(*args, **kwargs):
        if made:
            raise OSError(28, 'injected: no space left on device')
        handle = real(*args, **kwargs)
        made.append(handle)
        return handle

    monkeypatch.setattr(testproc.tempfile, 'TemporaryFile', flaky)
    with pytest.raises(OSError, match='injected'):
        testproc.run(PY + ['-c', 'pass'], capture_output=True, timeout=120)
    assert len(made) == 1 and made[0].closed
    made.clear()
    with pytest.raises(OSError, match='injected'):  # stdout spool made, input spool fails
        testproc.run(PY + ['-c', 'pass'], stdout=subprocess.PIPE, input=b'x', timeout=120)
    assert len(made) == 1 and made[0].closed
    made.clear()

    def record(*args, **kwargs):
        handle = real(*args, **kwargs)
        made.append(handle)
        return handle

    monkeypatch.setattr(testproc.tempfile, 'TemporaryFile', record)
    with pytest.raises(UnicodeEncodeError):  # input preparation fails after both output spools exist
        testproc.run(PY + ['-c', 'pass'], capture_output=True, input='\udcff', encoding='utf-8', timeout=120)
    assert len(made) == 2 and all(handle.closed for handle in made)


def test_new_helper_matches_subprocess_run_when_in_time(tmp_path):
    body = ('import sys; data = sys.stdin.read(); print("out", data.upper()); print("line2\\r\\nline3"); '
            'print("err", file=sys.stderr); sys.exit(3)')
    args = PY + ['-c', body]
    old = subprocess.run(args, input='abc', capture_output=True, text=True, timeout=120)
    new = testproc.run(args, input='abc', capture_output=True, text=True, timeout=120)
    assert (new.returncode, new.stdout, new.stderr) == (old.returncode, old.stdout, old.stderr) == (
        3, 'out ABC\nline2\n\nline3\n', 'err\n')
    raw = testproc.run(args, input=b'x', capture_output=True, timeout=120)
    old_raw = subprocess.run(args, input=b'x', capture_output=True, timeout=120)
    assert (raw.returncode, raw.stdout, raw.stderr) == (old_raw.returncode, old_raw.stdout, old_raw.stderr)
    with pytest.raises(subprocess.CalledProcessError) as raised:
        testproc.run(args, input='q', capture_output=True, text=True, timeout=120, check=True)
    assert raised.value.returncode == 3 and raised.value.stdout.startswith('out Q')
    assert testproc.check_output(PY + ['-c', 'print(7)'], text=True, timeout=120) == '7\n'
    plain = testproc.run(PY + ['-c', 'print(1)'], stdout=subprocess.DEVNULL, timeout=120)
    assert plain.returncode == 0 and plain.stdout is None and plain.stderr is None
    merged = testproc.run(PY + ['-c', 'import sys; print("a", flush=True); print("b", file=sys.stderr)'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120)
    assert merged.stdout == 'a\nb\n' and merged.stderr is None
    with pytest.raises(ValueError):
        testproc.run(PY + ['-c', 'pass'], timeout=None)


def test_a_slow_start_prints_a_soft_line_and_never_fails(monkeypatch, capsys):
    monkeypatch.setattr(testproc, 'SLOW_START', -1.0)  # every start counts as slow
    with pytest.warns(testproc.SlowStart, match='SLOW START'):
        result = testproc.run(PY + ['-c', 'print(5)'], capture_output=True, text=True, timeout=120)
    assert (result.returncode, result.stdout) == (0, '5\n')
    assert 'testproc: SLOW START' in capsys.readouterr().err
    with warnings.catch_warnings():
        warnings.simplefilter('error')  # as under -W error: still only a line
        assert testproc.run(PY + ['-c', 'pass'], timeout=120).returncode == 0
    monkeypatch.setattr(testproc, 'SLOW_START', 1e9)
    with warnings.catch_warnings():
        warnings.simplefilter('error')
        testproc.run(PY + ['-c', 'pass'], timeout=120)  # a normal start says nothing


def test_children_of_a_run_that_ended_in_time_live_on(tmp_path):
    """As with subprocess.run: only a timeout kills the tree."""
    pid_file = tmp_path / 'grandchild.pid'
    body = ('import subprocess, sys\np = subprocess.Popen([sys.executable, "-I", "-c", "import time; time.sleep(%d)"], '
            'stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)\n' % STUCK + write_identity_src(pid_file))
    result = testproc.run(PY + ['-c', body], capture_output=True, text=True, timeout=120)
    pid = read_pid(pid_file)  # written before the child exited, so present once run() returned
    try:
        assert pid is not None
        assert result.returncode == 0
        assert alive(pid)
    finally:
        kill_exact(pid)
