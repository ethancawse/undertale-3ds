"""The scheduler export must preserve the complete allocated ARM object."""
from pathlib import Path
import re
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).parent))
from devkit_tools import arm_tools
from touchquiz_compile import inventory


@pytest.mark.parametrize('model', ['O3DS', 'N3DS'])
def test_assembler_export_preserves_existing_code_and_relocations(tmp_path, model):
    gcc, nm = arm_tools('gcc', 'nm')
    devkitpro = Path(gcc).parents[2]
    source = (ROOT / 'platform/3ds/source/z_touchui.c').read_text()
    pattern = r'#if defined\(ENABLE_WAD16\) && defined\(__3DS__\)\n__asm__\(.*?\);\n#endif\n'
    original, count = re.subn(pattern, '', source, flags=re.S)
    assert count == 1, 'Expected exactly one assembler-only export'
    flags = [gcc, '-march=armv6k', '-mtune=mpcore', '-mfloat-abi=hard', '-mtp=soft',
        '-mword-relocations', '-ffunction-sections', '-D__3DS__', '-DNDEBUG', '-std=gnu99',
        '-include', str(ROOT / 'platform/3ds/release_guard.h'), '-Wall', '-Wextra', '-Werror',
        '-Wno-format', '-ffast-math', '-fno-finite-math-only', '-fomit-frame-pointer',
        '-DENABLE_WAD16', '-DPLATFORM_3DS', '-DCTRPORT_AOT', '-DCTRPORT_TARGET_' + model,
        '-I' + str(ROOT / 'platform/3ds/source'), '-I' + str(ROOT / 'engine/src'),
        '-I' + str(ROOT / 'engine/vendor/stb/ds'), '-I' + str(devkitpro / 'libctru/include'),
        '-I' + str(devkitpro / 'portlibs/3ds/include'), '-Os', '-fno-merge-constants',
        '-fno-tree-switch-conversion', '-fno-jump-tables']
    objects = []
    for name, text in (('before', original), ('after', source)):
        unit = tmp_path / (name + '.c'); unit.write_text(text, encoding='utf-8')
        obj = tmp_path / (name + '.o')
        subprocess.run([*flags, '-c', str(unit), '-o', str(obj)], check=True,
            stdin=subprocess.DEVNULL, timeout=60)
        objects.append(inventory(obj.read_bytes()))
    assert objects[0][0] == objects[1][0], 'Allocated bytes, extents or alignment changed'
    assert objects[0][1] == objects[1][1], 'Relocations changed'
    symbols = subprocess.run([nm, '-S', '--defined-only', str(obj)], check=True,
        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30).stdout
    rows = {p[3]: (p[0], p[1]) for line in symbols.splitlines() if len(p := line.split()) == 4}
    assert rows['CtrTouchUi_step'] == rows['Tu_step']
