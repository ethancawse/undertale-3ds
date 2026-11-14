"""Python discovery must fail before any AOT fallback or build can run."""
import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import sys
import tempfile
import time
import unittest
sys.path.append(str(Path(__file__).resolve().parents[1]))  # tools/: testproc
import testproc  # noqa: E402  (bounded subprocess waits for tests)

ROOT = Path(__file__).resolve().parents[2]
BASH = 'C:/devkitPro/msys2/usr/bin/bash.exe' if os.name == 'nt' else shutil.which('bash')


@unittest.skipUnless(BASH and Path(BASH).is_file(), 'Bash required')
class BuildPythonTests(unittest.TestCase):
    def setUp(self):
        (ROOT / 'scratch').mkdir(exist_ok=True)

    # A bash probe starts bash, then python (twice for discovery): testproc.BASH_TIMEOUT is a hang guard sized
    # to the loaded PC (15 s failed 2 of 4 discovery runs there on load alone, 11-12), not a speed claim.
    def discover(self, path, override=None):
        env = dict(os.environ)
        env['PATH'] = str(path)
        env.pop('CTRPORT_PYTHON', None)
        if override is not None:
            env['CTRPORT_PYTHON'] = str(override)
        return testproc.run([BASH, '--noprofile', '--norc', '-c',
            'set -euo pipefail; source "$1"; release_find_python; "${python[@]}" -c "print(12345)"',
            'python-test', (ROOT / 'platform/3ds/python.sh').as_posix()],
            env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=testproc.BASH_TIMEOUT)

    def test_no_launcher_or_python_fails_loudly(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'scratch') as empty:
            result = self.discover(empty)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn('ERROR: Python 3.8+ is required', result.stderr)
        self.assertNotIn('12345', result.stdout)

    def test_explicit_builder_python_survives_empty_path(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'scratch') as empty:
            result = self.discover(empty, sys.executable)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), '12345')

    def test_bad_explicit_interpreter_fails_without_fallback(self):
        result = self.discover(os.environ['PATH'], ROOT / 'scratch/nonexistent-python.exe')
        self.assertEqual(result.returncode, 2)
        self.assertIn('ERROR: CTRPORT_PYTHON', result.stderr)

    def test_msys_cmake_can_execute_the_selected_interpreter(self):
        env = dict(os.environ, CTRPORT_PYTHON=sys.executable)
        with tempfile.TemporaryDirectory(dir=ROOT / 'scratch') as folder:
            source = Path(folder)
            (source / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.19)\nproject(python_path NONE)\n'
                'add_custom_target(probe ALL COMMAND "${HOST_PYTHON}" -c '
                '"import sys; print(12345)" VERBATIM)\n', encoding='utf-8')
            command = [BASH, '--login', '-c',
                'set -euo pipefail; source "$1"; release_find_python; '
                'exe=$(release_python_executable); '
                'src="$2"; if command -v cygpath >/dev/null 2>&1; then src=$(cygpath -u "$src"); fi; '
                'cmake -S "$src" -B "$src/build" -G "Unix Makefiles" -DHOST_PYTHON:FILEPATH="$exe"; '
                'cmake --build "$src/build"', 'python-cmake-test',
                (ROOT / 'platform/3ds/python.sh').as_posix(), source.as_posix()]
            try:
                # Even a profile-free probe took 61.885 s on the shared Windows PC.
                # Keep the real login/make path; this is not a build-speed assertion.
                result = testproc.run(command, env=env, stdin=subprocess.DEVNULL,
                                      capture_output=True, text=True, timeout=180)
            except subprocess.TimeoutExpired as exc:
                self.fail('MSYS Python/CMake probe timed out. stdout=%r stderr=%r' %
                          (exc.stdout, exc.stderr))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('12345', result.stdout)


@unittest.skipUnless(BASH and Path(BASH).is_file(), 'Bash required')
class BuildEnvironmentTests(unittest.TestCase):
    """A login shell without APPDATA must not quietly build a game without its derived art."""

    def setUp(self):
        (ROOT / 'scratch').mkdir(exist_ok=True)

    def shell(self, script, env):
        return testproc.run([BASH, '--noprofile', '--norc', '-c', 'source "$1"; ' + script, 'env-test',
                             (ROOT / 'platform/3ds/python.sh').as_posix()],
                            env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                            timeout=testproc.BASH_TIMEOUT)

    def profile_shell(self, script, env):
        # Exercise the actual Bash parser with invented registry output, never this PC's logon record.
        values = dict(APPDATA=r'C:\Fixture Profile\Roaming', LOCALAPPDATA=r'C:\Fixture Profile\Local',
                      USERPROFILE=r'C:\Fixture Profile')
        record = '\n'.join('    %s    REG_SZ    %s' % row for row in values.items())
        prefix = 'reg.exe() { printf "%s\\n" ' + shlex.quote(record) + '; }; '
        return self.shell(prefix + script, env), values

    def test_profile_variables_come_back_in_a_shell_without_them(self):
        env = {k: v for k, v in os.environ.items() if k.upper() not in ('APPDATA', 'LOCALAPPDATA', 'USERPROFILE')}
        result, values = self.profile_shell(
            'release_windows_profile_env; printf "%s\n" "$APPDATA" "$LOCALAPPDATA" "$USERPROFILE"', env)
        self.assertEqual(result.returncode, 0, result.stderr)
        got = result.stdout.splitlines()
        want = [values[k] for k in ('APPDATA', 'LOCALAPPDATA', 'USERPROFILE')]
        self.assertEqual([os.path.normcase(v) for v in got], [os.path.normcase(v) for v in want])

    def test_profile_variables_already_set_are_kept(self):
        env = dict(os.environ, APPDATA=r'C:\kept-appdata')
        env.pop('LOCALAPPDATA', None)
        result, _values = self.profile_shell('release_windows_profile_env; printf "%s\n" "$APPDATA"', env)
        self.assertEqual(result.stdout.strip(), r'C:\kept-appdata', result.stderr)

    def test_missing_derivation_stops_the_build(self):
        env = dict(os.environ)
        env.pop('CTRPORT_DERIVATION_OPTIONAL', None)
        result = self.shell('release_missing_derivation "Crisp art" "cannot import numpy"; echo "rc=$?"', env)
        self.assertIn('rc=3', result.stdout)
        self.assertIn('ERROR: Crisp art: cannot import numpy.', result.stderr)
        self.assertIn('CTRPORT_DERIVATION_OPTIONAL=1', result.stderr)

    def test_tools_only_build_may_go_on_without_it(self):
        env = dict(os.environ, CTRPORT_DERIVATION_OPTIONAL='1')
        result = self.shell('release_missing_derivation "Crisp art" "cannot import numpy"; echo "rc=$?"', env)
        self.assertIn('rc=0', result.stdout)
        self.assertNotIn('ERROR', result.stderr)
        self.assertIn('building without it', result.stderr)

    def test_build_restores_the_profile_and_stops_on_missing_art(self):
        text = (ROOT / 'platform/3ds/build.sh').read_text(encoding='utf-8')
        self.assertIn('release_windows_profile_env\nrelease_find_python\n', text)
        self.assertIn('release_missing_derivation "Crisp art" "$crispart_why" || exit 3', text)
        self.assertNotIn('optional derivation tools unavailable', text)

    def cache_folder(self, marker, git_body):
        """The compile cache's folder as build.sh picks it, in a tree with or without .git, with a stand-in git."""
        text = (ROOT / 'platform/3ds/build.sh').read_text(encoding='utf-8')
        start = text.index('            main_checkout=$repo\n')
        end = text.index('            cache_dir="$main_checkout/scratch/ccache"\n')
        block = text[start:end] + '            cache_dir="$main_checkout/scratch/ccache"\n'
        with tempfile.TemporaryDirectory(dir=ROOT / 'scratch') as folder:
            top = Path(folder)
            repo = top / 'repo'
            repo.mkdir()
            tools = top / 'tools'
            tools.mkdir()
            called = top / 'called'
            (tools / 'git').write_bytes(('#!/bin/sh\n: > "%s"\n%s\n' % (called.as_posix(), git_body)).encode())
            if marker == 'dir':
                (repo / '.git').mkdir()
            elif marker == 'file':
                (repo / '.git').write_bytes(b'gitdir: elsewhere\n')
            env = dict(os.environ)
            env['PATH'] = os.pathsep.join((str(tools), str(Path(BASH).parent)))
            result = testproc.run([BASH, '--noprofile', '--norc', '-c',
                'set -euo pipefail; repo="$1"; ' + block + 'printf %s "$cache_dir"', 'cache-test', repo.as_posix()],
                env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=testproc.BASH_TIMEOUT)
            self.assertEqual(result.returncode, 0, result.stderr)
            return result.stdout, repo.as_posix(), called.exists()

    def test_cache_folder_needs_no_git_outside_a_checkout(self):
        # A source zip has no .git: git must not be asked, or a parent checkout would be found, or the build stopped.
        for body in ('exit 128', 'exit 127', 'echo /parent/.git'):
            with self.subTest(git=body):
                folder, repo, called = self.cache_folder(None, body)
                self.assertEqual(folder, repo + '/scratch/ccache')
                self.assertFalse(called)

    def test_cache_folder_of_a_checkout_git_cannot_read(self):
        for marker in ('dir', 'file'):
            for body in ('exit 128', 'exit 127'):
                with self.subTest(marker=marker, git=body):
                    folder, repo, called = self.cache_folder(marker, body)
                    self.assertEqual(folder, repo + '/scratch/ccache')
                    self.assertTrue(called)

    def test_cache_folder_of_a_worktree_is_the_main_checkouts(self):
        folder, _, called = self.cache_folder('file', 'echo /main/.git')
        self.assertEqual(folder, '/main/scratch/ccache')
        self.assertTrue(called)

    @unittest.skipUnless((ROOT / 'tools/analysis/crispart_pack.py').is_file(), 'derivation scripts not in this tree')
    def test_tool_check_names_the_import_and_the_place_it_looked(self):
        probe = testproc.run([sys.executable, '-I', '-c', 'import numpy'], stdin=subprocess.DEVNULL,
                             capture_output=True, timeout=60)
        if probe.returncode == 0:
            self.skipTest('numpy is installed outside the user site-packages')
        with tempfile.TemporaryDirectory(dir=ROOT / 'scratch') as empty:
            env = dict(os.environ, APPDATA=empty)
            # APPDATA selects Python's user site on Windows; Unix uses its own base.
            # Query the same isolated interpreter/environment as the tool under test.
            user_site = testproc.run([sys.executable, '-I', '-c',
                                     'import site; print(site.getusersitepackages())'],
                                    env=env, stdin=subprocess.DEVNULL, capture_output=True,
                                    text=True, check=True, timeout=30).stdout.strip()
            result = testproc.run([sys.executable, '-I', str(ROOT / 'tools/analysis/crispart_pack.py'), '--check-tools'],
                                  env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('crispart_pack: cannot import numpy', result.stderr)
        self.assertIn('user site-packages looked in: ' + user_site, result.stderr)


# The hang shape (fix-hang-build-python-empty-path-1112). On Windows, subprocess.run(capture_output=True,
# timeout=N) kills the process it started, then waits for pipe EOF with no deadline; a child of that process still
# holding the pipe holds the wait (bash's python held it 2 h 56 min). HOLDER builds the shape with a finite child:
# it starts a grandchild that inherits its output, records itself in `ready`, holds the output for HOLD seconds,
# then leaves `marker` and exits. HOLD is longer than the probe timeout plus testproc.KILL_WAIT, so a marker only
# appears when nothing killed the grandchild.
HOLDER = '''import os, subprocess, sys, time
ready, marker, hold = sys.argv[1], sys.argv[2], float(sys.argv[3])
if len(sys.argv) > 4:
    with open(ready + '.tmp', 'w') as stream:
        stream.write(repr(time.time()))
    os.replace(ready + '.tmp', ready)
    time.sleep(hold)
    with open(marker, 'w') as stream:
        stream.write(repr(time.time()))
    sys.exit(0)
subprocess.Popen([sys.executable, '-I', '-S', sys.argv[0], ready, marker, str(hold), 'grandchild'],
                 stdin=subprocess.DEVNULL)
print('holder started', flush=True)
time.sleep(4 * hold)
'''
# The old pattern, run only as a child of a testproc supervisor (SUPERVISOR_TIMEOUT) so it cannot hang this run.
OLD_PATTERN = '''import json, os, subprocess, sys, time
holder, ready, marker, hold, timeout = sys.argv[1:6]
begun = time.monotonic()
try:
    subprocess.run([sys.executable, '-I', '-S', holder, ready, marker, hold], stdin=subprocess.DEVNULL,
                   capture_output=True, text=True, timeout=float(timeout))
    outcome = 'returned'
except subprocess.TimeoutExpired:
    outcome = 'timed out'
print(json.dumps({'outcome': outcome, 'seconds': time.monotonic() - begun, 'marker': os.path.exists(marker)}))
'''
PROBE_TIMEOUT = 15  # a python start measured 0.5-10.3 s on the loaded PC; a later start reports INCONCLUSIVE
HOLD = PROBE_TIMEOUT + testproc.KILL_WAIT + 5
SUPERVISOR_TIMEOUT = PROBE_TIMEOUT + HOLD + 60


class HungProbeTests(unittest.TestCase):
    """Every probe above goes through testproc.run; these build the hang shape with a finite child."""

    def fixture(self, folder):
        folder = Path(folder)
        holder = folder / 'holder.py'
        holder.write_text(HOLDER, encoding='utf-8')
        return holder, folder / 'ready', folder / 'marker'

    def inconclusive_unless_ready(self, ready):
        if not ready.exists():
            self.skipTest('INCONCLUSIVE: the grandchild had not started when the probe timed out (process creation '
                          'is slow on this machine); the hang shape was not built')

    def test_probe_timeout_does_not_wait_for_a_child_holding_the_output(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'scratch') as folder:
            holder, ready, marker = self.fixture(folder)
            begun = time.monotonic()
            try:
                testproc.run([sys.executable, '-I', '-S', str(holder), str(ready), str(marker), str(HOLD)],
                             stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=PROBE_TIMEOUT)
            except subprocess.TimeoutExpired:
                pass
            else:
                self.fail('the holder returned before its timeout; it should sleep until killed')
            seconds = time.monotonic() - begun
            self.inconclusive_unless_ready(ready)
            held_until = float(ready.read_text(encoding='utf-8')) + HOLD
            self.assertFalse(marker.exists(), 'the probe waited %.1f s, until the grandchild let go of its '
                             'output (the old subprocess.run hang)' % seconds)
            # Past the moment the grandchild would have left its marker: it must have been killed with the tree.
            time.sleep(max(0.0, held_until + 3 - time.time()))
            self.assertFalse(marker.exists(), 'the grandchild outlived the probe timeout (no tree kill)')

    def test_control_the_old_pattern_waits_for_the_holding_child(self):
        """The fixture really builds the hang: plain subprocess.run returns only once the grandchild lets go."""
        with tempfile.TemporaryDirectory(dir=ROOT / 'scratch') as folder:
            holder, ready, marker = self.fixture(folder)
            old = Path(folder) / 'old_pattern.py'
            old.write_text(OLD_PATTERN, encoding='utf-8')
            try:
                result = testproc.run([sys.executable, '-I', '-S', str(old), str(holder), str(ready), str(marker),
                                       str(HOLD), str(PROBE_TIMEOUT)], stdin=subprocess.DEVNULL,
                                      capture_output=True, text=True, check=True, timeout=SUPERVISOR_TIMEOUT)
            except subprocess.TimeoutExpired:
                self.fail('the old pattern outlived its %d s supervisor (the grandchild is finite)'
                          % SUPERVISOR_TIMEOUT)
            self.inconclusive_unless_ready(ready)
            seen = json.loads(result.stdout)
            self.assertEqual(seen['outcome'], 'timed out', seen)
            self.assertTrue(seen['marker'], 'subprocess.run returned before the grandchild let go: %r (the '
                            'regression test above no longer has a bug to catch on this Python)' % (seen,))
