"""Actual ARM objects test the exact new continuation/export adapter."""
from pathlib import Path
import importlib.util
import subprocess
import sys
import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('touchoverworld_compile', Path(__file__).with_name('touchoverworld_compile.py'))
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)
sys.path.insert(0, str(Path(__file__).parent))
from devkit_tools import arm_tools


@pytest.mark.parametrize('source', list(adapter.ROUTES))
@pytest.mark.parametrize('enabled', [False, True])
def test_bytes_and_exact_symbol_changes(tmp_path, source, enabled):
    cc = arm_tools('gcc')[0]
    caller, section, old, new = adapter.ROUTES[source]
    code = f'extern int {old}(int);\n__attribute__((noinline,section("{section}"))) int {caller}(int n) {{ return {old}(n) + 2; }}\n'
    path = tmp_path / source; path.write_text(code)
    before, after = tmp_path / 'before.o', tmp_path / 'after.o'
    cmd = [cc, '-O0', '-marm', *(['-DENABLE_WAD16'] if enabled else []), '-c', str(path), '-o']
    subprocess.run([*cmd, str(before)], stdin=subprocess.DEVNULL, check=True, timeout=30)
    assert adapter.main([adapter.MARKER, *cmd, str(after)]) == 0
    aa, ae, _ = adapter.inventory(before.read_bytes()); ba, be, _ = adapter.inventory(after.read_bytes())
    assert aa == ba
    if source in adapter.ROUTES and enabled:
        assert be == sorted((*e[:3], new if e[3] == old else e[3]) for e in ae)
    else:
        assert be == ae
    assert after.with_suffix('.touchoverworld.json').exists() == enabled


@pytest.mark.parametrize('source', list(adapter.ROUTES))
def test_extra_reader_rejected(tmp_path, source):
    cc = arm_tools('gcc')[0]
    caller, section, old, _ = adapter.ROUTES[source]
    path = tmp_path / source
    path.write_text(f'extern int {old}(int);\n__attribute__((section("{section}"))) int {caller}(int x) {{ return {old}(x); }}\nint extra(int x) {{ return {old}(x); }}')
    with pytest.raises(AssertionError, match='unqualified continuation edges'):
        adapter.main([adapter.MARKER, cc, '-O0', '-marm', '-DENABLE_WAD16', '-c', str(path), '-o', str(tmp_path / 'out.o')])


def test_before_final_public_launcher():
    text = (ROOT / 'platform/3ds/CMakeLists.txt').read_text()
    assert text.index('get_target_property(touchoverworld_delegate') < text.index('get_target_property(p51_delegate')


@pytest.mark.parametrize('model', ['O3DS', 'N3DS'])
def test_aac_helper_thunks_preserve_local_symbols_and_old_bytes(tmp_path, model):
    gcc = arm_tools('gcc')[0]
    devkitpro = Path(gcc).parents[2]
    source = (ROOT / 'platform/3ds/source/z_touchmenus.c').read_text()
    split = source.index('// AAC continuations preserve')
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
    for name, text in (('before', source[:split]), ('after', source)):
        path = tmp_path / (name + '.c'); path.write_text(text)
        obj = path.with_suffix('.o')
        subprocess.run([*flags, '-c', str(path), '-o', str(obj)], check=True,
            stdin=subprocess.DEVNULL, timeout=60)
        objects.append((adapter.inventory(obj.read_bytes()), adapter.symbols(obj.read_bytes())))
    (aa, ae, _), a_syms = objects[0]
    (ba, be, _), b_syms = objects[1]
    new = '.aacout.text.touchoverworld'
    assert ba.pop(new)[-1] and aa == ba
    assert [e for e in be if e[0] != new] == ae
    assert sorted((e[1], e[2], e[3]) for e in be if e[0] == new) == [
        (0, 29, 'Tm_only'), (4, 29, 'Tm_exists'), (8, 29, 'Tm_view')]
    assert all(b_syms[n] == s for n, s in a_syms.items())
    for name, offset in (('only', 0), ('exists', 4), ('view', 8)):
        assert a_syms['Tm_' + name][2] == b_syms['Tm_' + name][2] == 2
        assert b_syms['To_' + name] == (offset, 4, 18, 0, new)
