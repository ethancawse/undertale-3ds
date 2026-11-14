"""Actual ARM objects test the exact new continuation/export adapter."""
from pathlib import Path
import importlib.util
import subprocess
import sys
import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('touchname_compile', Path(__file__).with_name('touchname_compile.py'))
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
    assert after.with_suffix('.touchname.json').exists() == enabled


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
    assert text.index('get_target_property(touchname_delegate') < text.index('get_target_property(p51_delegate')
