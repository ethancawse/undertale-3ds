"""Synthetic ARM objects exercise the production sampler relocation adapter."""
import importlib.util
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('touchquiz_compile', Path(__file__).with_name('touchquiz_compile.py'))
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)
sys.path.insert(0, str(Path(__file__).parent))
from devkit_tools import arm_tools

SOURCE = '''extern int __real_CtrInput_take(int);
__attribute__((weak,noinline)) int CtrTouchExt_sample(int n) { return n + 7; }
__attribute__((noinline,section(".text_battletouch2")))
int Battletouch_take2(int n) { return __real_CtrInput_take(n) + CtrTouchExt_sample(n); }
'''


@pytest.fixture
def cc():
    return arm_tools('gcc')[0]


@pytest.mark.parametrize('enabled', [True, False])
def test_two_routes_preserve_bytes_and_all_other_relocations(tmp_path, cc, enabled):
    source = tmp_path / 'z_battletouch.c'
    source.write_text(SOURCE)
    before, after = tmp_path / 'before.o', tmp_path / 'after.o'
    command = [cc, '-O0', '-marm', *(['-DENABLE_WAD16'] if enabled else []), '-c', str(source), '-o']
    subprocess.run([*command, str(before)], check=True, stdin=subprocess.DEVNULL, timeout=30)
    assert adapter.main([adapter.MARKER, *command, str(after)]) == 0
    a, ae, ab = adapter.inventory(before.read_bytes())
    b, be, bb = adapter.inventory(after.read_bytes())
    assert a == b and ab == bb
    assert be == (sorted((*e[:3], adapter.ROUTES.get(e[3], e[3])) for e in ae) if enabled else ae)
    assert after.with_suffix('.touchquiz.json').exists() == enabled


@pytest.mark.parametrize('extra', [
    'int other(int n) { return CtrTouchExt_sample(n); }',
    'int other(int n) { return __real_CtrInput_take(n); }'])
def test_additional_reader_fails_closed(tmp_path, cc, extra):
    source = tmp_path / 'z_battletouch.c'
    source.write_text(SOURCE + extra)
    with pytest.raises(AssertionError, match='unqualified sampler edges'):
        adapter.main([adapter.MARKER, cc, '-O0', '-marm', '-DENABLE_WAD16', '-c', str(source),
                      '-o', str(tmp_path / 'out.o')])


def test_wrong_caller_fails_closed(tmp_path, cc):
    source = tmp_path / 'z_battletouch.c'
    source.write_text(SOURCE.replace('.text_battletouch2', '.text_wrong'))
    with pytest.raises(AssertionError, match='unqualified sampler'):
        adapter.main([adapter.MARKER, cc, '-O0', '-marm', '-DENABLE_WAD16', '-c', str(source),
                      '-o', str(tmp_path / 'out.o')])


def test_assembly_request_unchanged(tmp_path, cc):
    source = tmp_path / 'z_battletouch.c'
    source.write_text(SOURCE)
    before, after = tmp_path / 'before.s', tmp_path / 'after.s'
    command = [cc, '-O0', '-marm', '-DENABLE_WAD16', '-S', str(source), '-o']
    subprocess.run([*command, str(before)], check=True, stdin=subprocess.DEVNULL, timeout=30)
    assert adapter.main([adapter.MARKER, *command, str(after)]) == 0
    assert before.read_bytes() == after.read_bytes()


def test_launcher_precedes_last_public_adapter():
    text = (ROOT / 'platform/3ds/CMakeLists.txt').read_text()
    assert text.index('get_target_property(touchquiz_delegate') < text.index('get_target_property(p51_delegate')
