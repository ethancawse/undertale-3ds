"""Use tiny synthetic ARM objects to verify the actual relocation-only compiler adapter."""
import importlib.util
from pathlib import Path
import os
import re
import subprocess
import sys
import unittest
from unittest.mock import patch

import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('p51_compile', Path(__file__).with_name('public_benchfile_compile.py'))
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)
sys.path.insert(0, str(Path(__file__).parent))
from devkit_tools import arm_tools, REQUIRE
import devkit_tools


@pytest.fixture
def cc():
    return arm_tools('gcc')[0]


@pytest.mark.parametrize('flags,expected', [(['-DENABLE_WAD16', '-DCTRPORT_PUBLIC_BENCH_FILE'], '__wrap_bootHookStat'),
                                           (['-DENABLE_WAD16'], 'stat'),
                                           (['-DENABLE_WAD17', '-DCTRPORT_PUBLIC_BENCH_FILE'], 'stat')])
def test_actual_object_retains_all_allocated_bytes(tmp_path, flags, expected, cc):
    source = tmp_path / 'main.c'
    source.write_text('extern int stat(void*, void*);\n'
                      '__attribute__((section(".text.late.screens"))) int probe(void) { return stat(0, 0); }\n')
    before, after = tmp_path / 'before.o', tmp_path / 'after.o'
    common = [cc, '-O0', '-marm', *flags, '-c', str(source), '-o']
    subprocess.run([*common, str(before)], check=True, stdin=subprocess.DEVNULL, timeout=30)
    assert adapter.main([adapter.MARKER, *common, str(after)]) == 0
    a, ae = adapter.inventory(before.read_bytes())
    b, be = adapter.inventory(after.read_bytes())
    assert a == b
    assert len(ae) == len(be) == 1 and ae[0][3] == 'stat' and be[0][3] == expected
    assert ae[0][:3] == be[0][:3]


def test_unqualified_second_stat_edge_is_refused(tmp_path, cc):
    source = tmp_path / 'main.c'
    source.write_text('extern int stat(void*, void*);\n'
                      '__attribute__((section(".text.late.screens"))) int probe(void) { return stat(0, 0); }\n'
                      'int extra(void) { return stat(0, 0); }\n')
    with pytest.raises(AssertionError, match='unqualified screen probe relocation'):
        adapter.main([adapter.MARKER, cc, '-O0', '-marm', '-DENABLE_WAD16', '-DCTRPORT_PUBLIC_BENCH_FILE',
                      '-c', str(source), '-o', str(tmp_path / 'out.o')])


def test_assembly_request_is_delegated_unchanged(tmp_path, cc):
    source = tmp_path / 'main.c'
    source.write_text('extern int stat(void*, void*);\nint probe(void) { return stat(0, 0); }\n')
    command = [cc, '-O0', '-marm', '-DENABLE_WAD16', '-DCTRPORT_PUBLIC_BENCH_FILE', '-S', str(source), '-o']
    before, after = tmp_path / 'before.s', tmp_path / 'after.s'
    subprocess.run([*command, str(before)], check=True, stdin=subprocess.DEVNULL, timeout=30)
    assert adapter.main([adapter.MARKER, *command, str(after)]) == 0
    assert before.read_bytes() == after.read_bytes()
    assert not after.with_suffix('.p51.json').exists()


def assert_reviewed_main_adapter_order(cmake):
    assert cmake.index('get_target_property(p51_delegate') > cmake.index('get_target_property(loadgame_delegate')
    block = re.search(r'if\(CTRPORT_GAME STREQUAL "undertale" AND CTRPORT_RELEASE_SOURCE_URL\)\s+'
                      r'# Preserve main.c.*?\nendif\(\)', cmake, re.S)[0]
    launcher = re.search(r'set_property\(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER\s+"[^"]+"\)', block)[0]
    assert 'public_benchfile_compile.py' in launcher
    assert ';${p51_delegate};--p51-compiler"' in block
    assert re.search(r'set_property\(SOURCE "\$\{CMAKE_CURRENT_SOURCE_DIR\}/source/main.c" '
                     r'APPEND PROPERTY OBJECT_DEPENDS\s+'
                     r'"\$\{CMAKE_CURRENT_SOURCE_DIR\}/../../tools/buildcache/public_benchfile_compile.py"\)', block)
    # P5-1 runs inside three explicitly reviewed launchers. dlgflash and
    # fpsghost_return pass main.c through; sfxlate changes one ARM call relocation
    # after them. Unknown launchers, changed delegates and reordering fail closed.
    suffix = cmake[cmake.index(block) + len(block):]
    later = list(re.finditer(r'set_property\(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER\s+"([^"]+)"\)', suffix))
    names = ('dlgflash', 'fpsghost_return', 'sfxlate')
    markers = ('dlgflash', 'fpsghost-return', 'sfxlate')
    assert len(later) == len(names)
    previous = 0
    for entry, name, marker in zip(later, names, markers):
        expected = ('${CTRPORT_PYTHON_EXECUTABLE};-X;utf8;-I;${CMAKE_CURRENT_SOURCE_DIR}'
                    '/../../tools/buildcache/' + name + '_compile.py;${' + name + '_delegate};--' + marker + '-compiler')
        assert entry[1] == expected, ('Unreviewed outer launcher or order', name, entry[1])
        assert re.search(r'get_target_property\(' + name + r'_delegate ctrport C_COMPILER_LAUNCHER\)\s*$',
                         suffix[previous:entry.start()])
        previous = entry.end()


def test_reviewed_main_adapter_order_in_the_cmake_launcher_chain(monkeypatch):
    cmake = (ROOT / 'platform/3ds/CMakeLists.txt').read_text()
    assert_reviewed_main_adapter_order(cmake)
    for mutant in (cmake.replace('${fpsghost_return_delegate};--fpsghost-return-compiler',
                                 '${wrong_delegate};--fpsghost-return-compiler'),
                   cmake.replace('/fpsghost_return_compile.py;', '/unknown_compile.py;'),
                   cmake.replace('get_target_property(p51_delegate', 'get_target_property(missing_delegate')):
        with pytest.raises((AssertionError, ValueError)):
            assert_reviewed_main_adapter_order(mutant)
    spec = importlib.util.spec_from_file_location('dlgflash_p51_chain', Path(__file__).with_name('dlgflash_compile.py'))
    outer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(outer)
    delegated = []
    monkeypatch.setattr(outer, 'run', lambda command: delegated.append(command) or 73)
    command = ['arm-none-eabi-gcc', '-DENABLE_WAD16', '-DCTRPORT_PUBLIC_BENCH_FILE',
               '-c', '/fixture/main.c', '-o', '/fixture/main.o']
    previous = ['python', 'public_benchfile_compile.py', '--p51-compiler']
    assert outer.main([*previous, outer.MARKER, *command]) == 73
    assert delegated == [previous + command]


def test_outer_teardown_adapter_keeps_main_requests_exact():
    spec = importlib.util.spec_from_file_location('p51_outer_teardown', Path(__file__).with_name('fpsghost_return_compile.py'))
    outer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(outer)
    delegate = ['python', 'public_benchfile_compile.py', '--p51-compiler']
    for source in ('fixture/main.c', r'fixture\main.c'):
        for mode in ('-c', '-S'):
            for flags in (['-DENABLE_WAD16', '-DCTRPORT_PUBLIC_BENCH_FILE'], ['-DENABLE_WAD17']):
                command = ['compiler', *flags, mode, source, '-o', 'main.o']
                for status in (0, 7):
                    with patch.object(outer, 'run', return_value=status) as run:
                        assert outer.main([*delegate, outer.MARKER, *command]) == status
                        run.assert_called_once_with(delegate+command)


def test_compiler_discovery_missing_required_and_relocated(tmp_path, monkeypatch):
    monkeypatch.delenv('DEVKITARM', raising=False)
    monkeypatch.delenv('DEVKITPRO', raising=False)
    monkeypatch.delenv(REQUIRE, raising=False)
    monkeypatch.setattr(devkit_tools.toolchain, 'candidate_roots', lambda: [])
    monkeypatch.setattr(devkit_tools.shutil, 'which', lambda p: None)
    with pytest.raises(unittest.SkipTest):
        arm_tools('gcc')
    monkeypatch.setenv(REQUIRE, '1')
    with pytest.raises(AssertionError, match='not available'):
        arm_tools('gcc')
    monkeypatch.delenv(REQUIRE)
    suffix = '.exe' if os.name == 'nt' else ''
    compiler = tmp_path / 'relocated tools/bin' / ('arm-none-eabi-gcc' + suffix)
    compiler.parent.mkdir(parents=True)
    compiler.touch()
    monkeypatch.setenv('DEVKITARM', str(compiler.parent.parent))
    assert arm_tools('gcc') == [str(compiler)]


def test_unknown_and_reordered_outer_launchers_are_rejected():
    cmake = (ROOT / 'platform/3ds/CMakeLists.txt').read_text()
    assert_reviewed_main_adapter_order(cmake)
    for name in ('dlgflash', 'fpsghost_return', 'sfxlate'):
        with pytest.raises(AssertionError):
            assert_reviewed_main_adapter_order(cmake.replace('/' + name + '_compile.py;', '/unknown_compile.py;'))
    import itertools
    launchers = re.findall(r'set_property\(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER\s+"([^"]+)"\)', cmake)[-3:]
    assert len(launchers) == 3
    for order in itertools.permutations(range(3)):
        if order == (0, 1, 2):
            continue
        changed = cmake
        for index, launcher in enumerate(launchers):
            changed = changed.replace(launcher, 'ORDERED_LAUNCHER_' + str(index))
        for index, replacement in enumerate(order):
            changed = changed.replace('ORDERED_LAUNCHER_' + str(index), launchers[replacement])
        with pytest.raises(AssertionError):
            assert_reviewed_main_adapter_order(changed)
    extra = cmake + '\nset_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER "unknown.py")\n'
    with pytest.raises(AssertionError):
        assert_reviewed_main_adapter_order(extra)


def test_p51_then_cooler_changes_only_the_two_reviewed_main_edges(tmp_path, cc):
    spec = importlib.util.spec_from_file_location('sfxlate_main_chain', Path(__file__).with_name('sfxlate_compile.py'))
    cooler = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cooler)
    source = tmp_path / 'main.c'
    source.write_text('extern int stat(void*, void*);\nextern void CtrRenderer_skipPresent(void);\n'
        '__attribute__((section(".text.late.screens"))) int probe(void) { return stat(0, 0); }\n'
        'int main(void) { CtrRenderer_skipPresent(); return probe(); }\n')
    before, after = tmp_path / 'before.o', tmp_path / 'after.o'
    command = [cc, '-O0', '-marm', '-DENABLE_WAD16', '-DCTRPORT_PUBLIC_BENCH_FILE',
               '-DCTRPORT_TARGET_O3DS', '-DCTRPORT_HAVE_TREMOR', '-c', str(source), '-o']
    subprocess.run([*command, str(before)], check=True, stdin=subprocess.DEVNULL, timeout=30)
    # The same nesting as CMake: cooler -> fpsghost_return -> dlgflash -> P5-1 -> compiler.
    delegate = [sys.executable, '-I', str(Path(__file__).with_name('fpsghost_return_compile.py')),
                sys.executable, '-I', str(Path(__file__).with_name('dlgflash_compile.py')),
                sys.executable, '-I', str(Path(__file__).with_name('public_benchfile_compile.py')),
                '--p51-compiler', '--dlgflash-compiler', '--fpsghost-return-compiler']
    assert cooler.main([*delegate, cooler.MARKER, *command, str(after)]) == 0
    full_a, full_b = cooler.inventory(before.read_bytes()), cooler.inventory(after.read_bytes())
    assert full_a[6] == full_b[6]
    edges_a, edges_b = full_a[7], full_b[7]
    # File offsets of non-allocated relocation tables may differ with the output
    # filename's string-table length; section-relative call locations may not.
    assert [(e[1:4], e[5:]) for e in edges_a] == [(e[1:4], e[5:]) for e in edges_b]
    mapped = {'stat': '__wrap_bootHookStat', 'CtrRenderer_skipPresent': 'sfcSkip'}
    assert [mapped.get(e[4], e[4]) for e in edges_a] == [e[4] for e in edges_b]
    assert set(mapped) <= {e[4] for e in edges_a}


def test_cooler_delegates_assembly_and_errors_without_edits():
    spec = importlib.util.spec_from_file_location('sfxlate_delegate_contract', Path(__file__).with_name('sfxlate_compile.py'))
    cooler = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cooler)
    from types import SimpleNamespace
    delegate = ['reviewed-previous-launchers']
    for mode, status in (('-S', 0), ('-c', 7)):
        command = ['compiler', mode, 'fixture/main.c', '-o', 'main.o']
        with patch.object(cooler.subprocess, 'run', return_value=SimpleNamespace(returncode=status)) as run:
            with patch.object(cooler, 'route_main', side_effect=AssertionError('must not edit')):
                assert cooler.main([*delegate, cooler.MARKER, *command]) == status
                run.assert_called_once_with(delegate + command, stdin=subprocess.DEVNULL, timeout=1800)


def test_cooler_target_contract_rejects_other_game_and_target():
    spec = importlib.util.spec_from_file_location('sfxlate_target_contract', Path(__file__).with_name('sfxlate_compile.py'))
    cooler = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cooler)
    from types import SimpleNamespace
    required = ['-DENABLE_WAD16', '-DCTRPORT_TARGET_O3DS', '-DCTRPORT_HAVE_TREMOR']
    for flags in (required + ['-DENABLE_WAD17'], required[1:], [required[0], required[2]], required[:2]):
        with patch.object(cooler.subprocess, 'run', return_value=SimpleNamespace(returncode=0)):
            with pytest.raises(AssertionError, match='CMake cooler target contract'):
                cooler.main([cooler.MARKER, 'compiler', *flags, '-c', 'fixture/main.c', '-o', 'main.o'])
