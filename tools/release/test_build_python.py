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
