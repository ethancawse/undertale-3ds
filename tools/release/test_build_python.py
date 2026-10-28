"""Python discovery must fail before any AOT fallback or build can run."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BASH = 'C:/devkitPro/msys2/usr/bin/bash.exe' if os.name == 'nt' else shutil.which('bash')


@unittest.skipUnless(BASH and Path(BASH).is_file(), 'Bash required')
class BuildPythonTests(unittest.TestCase):
    def discover(self, path, override=None):
        env = dict(os.environ)
        env['PATH'] = str(path)
        env.pop('CTRPORT_PYTHON', None)
        if override is not None:
            env['CTRPORT_PYTHON'] = str(override)
        return subprocess.run([BASH, '--noprofile', '--norc', '-c',
            'set -euo pipefail; source "$1"; release_find_python; "${python[@]}" -c "print(12345)"',
            'python-test', (ROOT / 'platform/3ds/python.sh').as_posix()],
            env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=15)

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
                result = subprocess.run(command, env=env, stdin=subprocess.DEVNULL,
                                        capture_output=True, text=True, timeout=180)
            except subprocess.TimeoutExpired as exc:
                self.fail('MSYS Python/CMake probe timed out. stdout=%r stderr=%r' %
                          (exc.stdout, exc.stderr))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('12345', result.stdout)


@unittest.skipUnless(BASH and Path(BASH).is_file(), 'Bash required')
class BuildEnvironmentTests(unittest.TestCase):
    """A login shell without APPDATA must not quietly build a game without its derived art."""

    def shell(self, script, env):
        return subprocess.run([BASH, '--noprofile', '--norc', '-c', 'source "$1"; ' + script, 'env-test',
                               (ROOT / 'platform/3ds/python.sh').as_posix()],
                              env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30)

    @unittest.skipUnless(os.name == 'nt' and shutil.which('reg.exe') and os.environ.get('APPDATA'),
                         'Windows with a logon session')
    def test_profile_variables_come_back_in_a_shell_without_them(self):
        env = {k: v for k, v in os.environ.items() if k.upper() not in ('APPDATA', 'LOCALAPPDATA', 'USERPROFILE')}
        result = self.shell('release_windows_profile_env; printf "%s\n" "$APPDATA" "$LOCALAPPDATA" "$USERPROFILE"', env)
        self.assertEqual(result.returncode, 0, result.stderr)
        got = result.stdout.splitlines()
        want = [os.environ.get(k, '') for k in ('APPDATA', 'LOCALAPPDATA', 'USERPROFILE')]
        self.assertEqual([os.path.normcase(v) for v in got], [os.path.normcase(v) for v in want])

    @unittest.skipUnless(os.name == 'nt' and shutil.which('reg.exe'), 'Windows')
    def test_profile_variables_already_set_are_kept(self):
        env = dict(os.environ, APPDATA=r'C:\kept-appdata')
        env.pop('LOCALAPPDATA', None)
        result = self.shell('release_windows_profile_env; printf "%s\n" "$APPDATA"', env)
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

    @unittest.skipUnless((ROOT / 'tools/analysis/crispart_pack.py').is_file(), 'derivation scripts not in this tree')
    def test_tool_check_names_the_import_and_the_place_it_looked(self):
        probe = subprocess.run([sys.executable, '-I', '-c', 'import numpy'], stdin=subprocess.DEVNULL,
                               capture_output=True, timeout=60)
        if probe.returncode == 0:
            self.skipTest('numpy is installed outside the user site-packages')
        with tempfile.TemporaryDirectory(dir=ROOT / 'scratch') as empty:
            env = dict(os.environ, APPDATA=empty)
            # APPDATA selects Python's user site on Windows; Unix uses its own base.
            # Query the same isolated interpreter/environment as the tool under test.
            user_site = subprocess.run([sys.executable, '-I', '-c',
                                       'import site; print(site.getusersitepackages())'],
                                      env=env, stdin=subprocess.DEVNULL, capture_output=True,
                                      text=True, check=True, timeout=30).stdout.strip()
            result = subprocess.run([sys.executable, '-I', str(ROOT / 'tools/analysis/crispart_pack.py'), '--check-tools'],
                                    env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('crispart_pack: cannot import numpy', result.stderr)
        self.assertIn('user site-packages looked in: ' + user_site, result.stderr)
