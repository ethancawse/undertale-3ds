"""No file the product build runs names a fixed devkitPro folder (gate 1 of the 11-17 devkitPro-location fix).

11-17 playtester (N14 kit): devkitPro under the user folder, chosen in the builder; the build stopped at 41% because
tools/buildcache/sfxlate_object.py ran C:/devkitPro/devkitARM/bin/arm-none-eabi-gcc.exe. Build-time scripts take
the toolchain from the build (the compiler command CMake runs), never a literal install path.

Scope: every file the builder mirrors into its work folder (pipeline.source_wanted, both games), which is what
build.sh, CMake and their scripts can reach. Tests and comment lines are exempt. The builder's own discovery code
(tools/release/builder) chooses the location and is not mirrored; the runtime proof covers it (no process or file
under C:/devkitPro while another location is selected)."""
import os
from pathlib import Path
import re
import sys
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE / 'builder'))
import pipeline  # noqa: E402

FIXED = re.compile(r'(?i)(?:\b[a-z]:[\\/]+devkitpro\b|/opt/devkitpro\b|(?<![\w.])/[a-z]/devkitpro\b)')
COMMENT = ('#', '//', '/*', '*', ';', 'REM ', 'rem ')
TEXT = {'.py', '.sh', '.cmake', '.txt', '.c', '.h', '.inc', '.s', '.S', '.ld', '.specs', '.json', '.cfg', '.ini', ''}


def build_files():
    seen = set()
    for game in ('undertale', 'deltarune'):
        for rel in pipeline.SOURCE_DIRS:
            for dirpath, dirnames, filenames in os.walk(ROOT / rel):
                dirnames[:] = [d for d in dirnames if d not in pipeline.SKIP_DIRS]
                for name in filenames:
                    relative = os.path.relpath(os.path.join(dirpath, name), ROOT).replace('\\', '/')
                    if pipeline.source_wanted(relative, game):
                        seen.add(relative)
        seen.update(rel for rel in pipeline.SOURCE_FILES if pipeline.source_wanted(rel, game))
    return sorted(seen)


def fixed_paths(relative):
    path = ROOT / relative
    if path.suffix not in TEXT or path.name.startswith('test_') or not path.is_file():
        return []
    try:
        text = path.read_text(encoding='utf-8')
    except (UnicodeDecodeError, OSError):
        return []
    return ['%s:%d: %s' % (relative, number, line.strip()) for number, line in enumerate(text.splitlines(), 1)
            if FIXED.search(line) and not line.strip().startswith(COMMENT)]


class NoFixedDevkitProTests(unittest.TestCase):
    def test_the_scan_sees_the_build_scripts(self):
        files = build_files()
        for expected in ('platform/3ds/build.sh', 'platform/3ds/CMakeLists.txt', 'tools/buildcache/sfxlate_object.py',
                         'platform/3ds/utold_tile_place.py', 'tools/analysis/flowervramg_palette_retry.py'):
            self.assertIn(expected, files)
        self.assertTrue(FIXED.search("ARM = Path('C:/devkitPro/devkitARM/bin')"))
        self.assertTrue(FIXED.search('BIN = "/opt/devkitpro/devkitARM/bin"'))
        self.assertTrue(FIXED.search('/c/devkitPro/msys2'))
        self.assertFalse(FIXED.search('"$DEVKITPRO/cmake/3DS.cmake"'))

    def test_no_build_file_names_a_fixed_devkitpro_folder(self):
        hits = [hit for relative in build_files() for hit in fixed_paths(relative)]
        self.assertEqual(hits, [], 'build-time files must take the toolchain from the build, not a fixed folder')


if __name__ == '__main__':
    unittest.main()
