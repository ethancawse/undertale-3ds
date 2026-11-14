"""Fail closed on decoder input drift and preserve assembly section boundaries."""
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

import pytest

spec = importlib.util.spec_from_file_location('aacout_compile', Path(__file__).with_name('aacout_compile.py'))
aacout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(aacout)


def source():
    return ''.join(f'\t.section .text.{name},"ax",%progbits\n{name}:\n'
                   f'\tbx lr\n\t.size {name}, .-{name}\n'
                   for name, _ in reversed(aacout.SLOTS) if name != 'aacDecodeNext')


def test_sections_remain_separate_and_in_base_order():
    result = aacout.rewrite(source())
    positions = [result.index(f'.section .text.{name},') for name, _ in aacout.SLOTS]
    assert positions == sorted(positions)
    assert '\t.size tremorClose, .-tremorClose\n\t.section' in result
    assert 'aacDecodeNext:\n\t.space 316, 0\n' in result
    assert result.count('\t.org ') == len(aacout.SLOTS) - 1


@pytest.mark.parametrize('change', [
    lambda text: text + '\t.section .text.unreviewed,"ax",%progbits\nunreviewed:\n',
    lambda text: text.replace('.text.srcRead,', '.data.srcRead,'),
    lambda text: text.replace('\t.size srcRead, .-srcRead\n', ''),
])
def test_unreviewed_input_fails(change):
    with pytest.raises(AssertionError):
        aacout.rewrite(change(source()))


def test_arm_assembler_preserves_sizes_and_rejects_overflow(tmp_path):
    compiler = Path('C:/devkitPro/devkitARM/bin/arm-none-eabi-gcc.exe')
    if not compiler.is_file():
        pytest.skip('devkitARM is required for the assembler integration check')
    asm, obj = tmp_path / 'decoder.s', tmp_path / 'decoder.o'
    asm.write_text('.syntax unified\n.arm\n' + aacout.rewrite(source()))
    command = [str(compiler), '-march=armv6k', '-c', str(asm), '-o', str(obj)]
    result = subprocess.run(command, capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=20)
    assert result.returncode == 0, result.stderr
    assert not result.stderr, result.stderr
    nm = compiler.with_name('arm-none-eabi-nm.exe')
    symbols = subprocess.run([str(nm), '-S', str(obj)], capture_output=True, text=True,
                             check=True, stdin=subprocess.DEVNULL, timeout=20).stdout
    actual = {p[3]: int(p[1], 16) for line in symbols.splitlines() if len(p := line.split()) == 4}
    assert actual == dict(aacout.SLOTS)
    asm.write_text('.syntax unified\n.arm\n' + aacout.rewrite(source().replace(
        'tremorClose:\n\tbx lr', 'tremorClose:\n\t.space 12\n\tbx lr')))
    result = subprocess.run(command, capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=20)
    assert result.returncode != 0
    assert 'backwards' in result.stderr


@pytest.mark.parametrize('decoder', ['source/ctr_audio_decoder.c', 'ctr_audio_decoder.c'])
@pytest.mark.parametrize('missing', [
    ('-DCTRPORT_NO_AAC',), ('-DNDEBUG',), ('-DCTRPORT_HAVE_TREMOR',),
    ('-DCTRPORT_NO_AAC', '-DNDEBUG', '-DCTRPORT_HAVE_TREMOR'),
])
def test_main_refuses_unpadded_decoder(monkeypatch, capsys, missing, decoder):
    def forbidden(command):
        pytest.fail('compiler invoked without mandatory padding flags: ' + repr(command))
    monkeypatch.setattr(aacout, 'run', forbidden)
    flags = ['-DCTRPORT_NO_AAC', '-DNDEBUG', '-DCTRPORT_HAVE_TREMOR']
    command = ['gcc', *[f for f in flags if f not in missing], '-c',
               decoder, '-o', 'decoder.o']
    assert aacout.main([aacout.PAD, aacout.MARKER, *command]) == 2
    error = capsys.readouterr().err
    assert all(flag in error for flag in missing)


def test_main_emits_padded_assembly_and_preserves_delegate(tmp_path, monkeypatch):
    calls = []
    def compiler(command):
        calls.append(command)
        if '-S' in command:
            Path(command[command.index('-o') + 1]).write_text(source(), encoding='utf-8')
        return 0
    monkeypatch.setattr(aacout, 'run', compiler)
    assert aacout.main([aacout.PAD, 'cache', aacout.MARKER, 'gcc', '-DCTRPORT_NO_AAC', '-DNDEBUG',
                        '-DCTRPORT_HAVE_TREMOR', '-c', 'source/ctr_audio_decoder.c',
                        '-o', str(tmp_path / 'decoder.o')]) == 0
    assert calls[0][:2] == ['cache', 'gcc']
    assert calls[1][0] == 'gcc' and '-x' in calls[1]
    assert '\t.org tremorClose+8, 0\n' in (tmp_path / 'aacout1.s').read_text(encoding='utf-8')


def test_other_translation_units_delegate_unchanged(monkeypatch):
    calls = []
    monkeypatch.setattr(aacout, 'run', lambda command: calls.append(command) or 0)
    command = ['gcc', '-c', 'source/other.c', '-o', 'other.o']
    assert aacout.main([aacout.PAD, 'cache', aacout.MARKER, *command]) == 0
    assert calls == [['cache', *command]]


@pytest.mark.parametrize('flags', [
    ['-g', '-Og', '-DDEBUG', '-DCTRPORT_HAVE_TREMOR', '-DCTRPORT_NO_AAC'],
    ['-Oz', '-DNDEBUG', '-DCTRPORT_HAVE_TREMOR', '-DCTRPORT_NO_AAC'],
    ['-O2', '-DNDEBUG', '-DCTRPORT_NO_AAC'],
])
def test_unselected_padding_delegates_decoder_unchanged(monkeypatch, flags):
    calls = []
    monkeypatch.setattr(aacout, 'run', lambda command: calls.append(command) or 0)
    command = ['gcc', *flags, '-c', 'source/ctr_audio_decoder.c', '-o', 'decoder.o']
    assert aacout.main(['cache', aacout.MARKER, *command]) == 0
    assert calls == [['cache', *command]]


@pytest.mark.parametrize('routed', [False, True])
@pytest.mark.parametrize('config,tremor,padding', [
    ('Release', 'found', True), ('RelWithDebInfo', 'found', True),
    ('release', 'found', True), ('RELEASE', 'found', True),
    ('relwithdebinfo', 'found', True),
    ('Debug', 'found', False), ('MinSizeRel', 'found', False),
    ('Release', 'disabled', False), ('Release', 'missing', False),
    ('RelWithDebInfo', 'missing', False),
    ('Profile', 'found', None), ('', 'disabled', None),
])
def test_cmake_selects_padding_after_decoder_discovery(tmp_path, config, tremor, padding, routed):
    cmake = shutil.which('cmake')
    if not cmake:
        pytest.skip('CMake is required for the decoder-mode configuration check')
    root = Path(__file__).resolve().parents[2]
    # Configure the real module with an empty or synthetic nonempty route list.
    module = tmp_path / 'platform/3ds/aacout.cmake'
    module.parent.mkdir(parents=True)
    shutil.copyfile(root / 'platform/3ds/aacout.cmake', module)
    shutil.copyfile(root / 'platform/3ds/aacslot-native.json', module.with_name('aacslot-native.json'))
    manifest = tmp_path / 'tools/aot/aacslot.json'
    manifest.parent.mkdir(parents=True)
    manifest.write_text(json.dumps(dict(version=1, functions=[
        dict(id='fixture', phase=dict(o3ds=0, n3ds=0))] if routed else [])), encoding='utf-8')
    sdk = tmp_path / 'sdk'
    (sdk / 'bin').mkdir(parents=True)
    (sdk / 'arm-none-eabi/lib').mkdir(parents=True)
    (sdk / 'arm-none-eabi/lib/3dsx.ld').write_text('*(.text.*)\n*(.rodata)\n', encoding='utf-8')
    (tmp_path / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.19)\nproject(mode_probe NONE)\n'
        'add_executable(ctrport IMPORTED)\n'
        'set_property(TARGET ctrport PROPERTY C_COMPILER_LAUNCHER cache)\n'
        'set(CTRPORT_DKA_BIN "' + (sdk / 'bin').as_posix() + '")\n'
        'set(CTRPORT_SPECS_NEW "-T 3dsx.ld%s")\n'
        'set(CTRPORT_PYTHON_EXECUTABLE python)\n'
        'set(CTRPORT_TARGET o3ds)\n'
        'set(CTRPORT_AOT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/aot_fixture.c")\n'
        'set(PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_audio_decoder.c")\n'
        'set(ENGINE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../../engine/src/overlay_file_system.c")\n'
        'include("' + module.as_posix() + '")\n'
        # Set discovery results only after inclusion, as in the actual project.
        'set(CTRPORT_USE_TREMOR ' + ('OFF' if tremor == 'disabled' else 'ON') + ')\n'
        'set(TREMOR_INCLUDE_DIR headers)\nset(TREMOR_LIBRARY tremor)\n'
        'set(OGG_LIBRARY ' + ('OGG_LIBRARY-NOTFOUND' if tremor == 'missing' else 'ogg') + ')\n'
        'aacout_configure_decoder()\n'
        'get_target_property(launcher ctrport C_COMPILER_LAUNCHER)\n'
        'get_source_file_property(deps "${CTRPORT_AOT_SOURCES}" OBJECT_DEPENDS)\n'
        'file(WRITE "${CMAKE_BINARY_DIR}/launcher.txt" "${launcher}")\n'
        'file(WRITE "${CMAKE_BINARY_DIR}/deps.txt" "${deps}")\n', encoding='utf-8')
    build = tmp_path / 'build'
    result = subprocess.run([cmake, '-S', str(tmp_path), '-B', str(build), '-DCMAKE_BUILD_TYPE=' + config],
                            stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=20)
    if padding is None:
        assert result.returncode != 0
        assert 'aacout: unsupported build type' in result.stdout + result.stderr
        assert not (build / 'launcher.txt').exists()
        return
    assert result.returncode == 0, result.stdout + result.stderr
    launcher = (build / 'launcher.txt').read_text(encoding='utf-8').split(';')
    assert ('--aacout-pad' in launcher) == padding
    tail = ['cache', '--aacslot-compiler', '--aacout-compiler'] if routed else ['cache', '--aacout-compiler']
    assert launcher[-len(tail):] == tail
    assert launcher.count('--aacslot-compiler') == int(routed)
    assert any(item.endswith('/aacslot_compile.py') for item in launcher) == routed
    assert ('aacslot_compile.py' in (build / 'deps.txt').read_text(encoding='utf-8')) == routed
    assert launcher.count('--aacout-compiler') == 1
    script = build / 'aacout.ld'
    text = script.read_text(encoding='utf-8')
    assert 'KEEP(*/ctr_audio_decoder.o(.text.*))' in text
    assert '*/overlay_file_system.o(.text.*)' in text
    # The exact generated selectors must accept both ordinary and CMake-hashed
    # paths. Source path length, checkout root and directory spelling are free.
    for directory in ('source', '537ccbebaff01234567890123456789ab'):
        objects = ['CMakeFiles/ctrport.dir/' + directory + '/' + name + '.o'
                   for name in ('ctr_audio_decoder', 'overlay_file_system')]
        assert aacout.check_selectors(script, objects) == 4


def selector_script(tmp_path):
    script = tmp_path / 'aacout.ld'
    script.write_text('/* aacout object */ KEEP(*/decoder.o(.text.*))\n'
                      '/* aacout object */ */decoder.o(.rodata)\n', encoding='utf-8')
    return script


def test_link_main_matches_quoted_response_inputs_and_delegates(tmp_path, monkeypatch):
    script = selector_script(tmp_path)
    response = tmp_path / 'objects.rsp'
    response.write_text('"E:\\a checkout\\CMakeFiles\\ctrport.dir\\source\\decoder.o"\n', encoding='utf-8')
    calls = []
    monkeypatch.setattr(aacout, 'run', lambda command: calls.append(command) or 0)
    command = ['gcc', '@' + str(response), '-o', 'test.elf']
    assert aacout.main(['--aacout-link', str(script), '--', *command]) == 0
    assert calls == [command]


@pytest.mark.parametrize('problem', ['missing', 'duplicate', 'basename_collision', 'malformed', 'recursive_response'])
def test_link_main_refuses_unmatched_or_unqualified_input(tmp_path, monkeypatch, capsys, problem):
    script = selector_script(tmp_path)
    command = ['gcc', 'CMakeFiles/ctrport.dir/source/decoder.o']
    if problem == 'missing':
        command[1] = 'CMakeFiles/ctrport.dir/shortened/other.o'
    elif problem == 'duplicate':
        command.append(command[1])
    elif problem == 'basename_collision':
        command.append('CMakeFiles/another.dir/537ccbeb/decoder.o')
    elif problem == 'malformed':
        script.write_text(script.read_text(encoding='utf-8').replace('(.rodata)', '(.bss)'), encoding='utf-8')
    else:
        response = tmp_path / 'recursive.rsp'
        response.write_text('@' + str(response), encoding='utf-8')
        command = ['gcc', '@' + str(response)]
    def forbidden(command):
        pytest.fail('link compiler invoked despite invalid selectors: ' + repr(command))
    monkeypatch.setattr(aacout, 'run', forbidden)
    assert aacout.main(['--aacout-link', str(script), '--', *command]) == 2
    error = capsys.readouterr().err
    assert 'aacout:' in error
    assert ('matches 0 linker objects' in error if problem == 'missing' else
            'matches 2 linker objects' in error if problem in ('duplicate', 'basename_collision') else
            'malformed' in error if problem == 'malformed' else 'recursive' in error)


@pytest.mark.requires_target_build
def test_real_link_selectors_reject_a_renamed_object(tmp_path, monkeypatch, capsys):
    script, response = os.environ.get('AACOUT_LD'), os.environ.get('AACOUT_LINK_RSP')
    if not script or not response:
        pytest.skip('set AACOUT_LD and AACOUT_LINK_RSP to the generated script and linker object response file')
    command = ['gcc', '@' + response]
    assert aacout.check_selectors(script, command) >= 400
    text = Path(script).read_text(encoding='utf-8')
    pattern = re.search(r'/\* aacout object \*/\s*(?:KEEP\()?([^\s()]+)', text)[1]
    bad = tmp_path / 'missing-selector.ld'
    bad.write_text(text.replace(pattern, pattern + '.missing', 1), encoding='utf-8')
    def forbidden(command):
        pytest.fail('link compiler invoked with a real unmatched selector: ' + repr(command))
    monkeypatch.setattr(aacout, 'run', forbidden)
    assert aacout.main(['--aacout-link', str(bad), '--', *command]) == 2
    assert 'matches 0 linker objects' in capsys.readouterr().err


@pytest.mark.requires_target_build
def test_build_sh_full_strategy_overrides_short_environment_at_link(tmp_path):
    root = Path(__file__).resolve().parents[2]
    driver = (root / 'platform/3ds/build.sh').read_text(encoding='utf-8')
    configure = driver.split('cmake -S "$here" -B "$build"', 1)[1].split('\n\n', 1)[0]
    setting, = re.findall(r'-DCMAKE_INTERMEDIATE_DIR_STRATEGY=\w+', configure)
    assert setting == '-DCMAKE_INTERMEDIATE_DIR_STRATEGY=FULL'
    cmake, ninja = shutil.which('cmake'), shutil.which('ninja')
    compiler = Path('C:/devkitPro/devkitARM/bin/arm-none-eabi-gcc.exe')
    if not cmake or not ninja or not compiler.is_file():
        pytest.skip('CMake, Ninja and devkitARM required for the real selector link')
    version = subprocess.run([cmake, '--version'], capture_output=True, text=True,
                             stdin=subprocess.DEVNULL, check=True, timeout=20).stdout
    if tuple(map(int, re.search(r'version (\d+)\.(\d+)', version).groups())) < (4, 2):
        pytest.skip('CMake 4.2+ required for the SHORT strategy regression')
    source = tmp_path / 'platform/3ds'
    (source / 'source').mkdir(parents=True)
    adapter = tmp_path / 'tools/buildcache'
    adapter.mkdir(parents=True)
    shutil.copyfile(root / 'tools/buildcache/aacout_compile.py', adapter / 'aacout_compile.py')
    (source / 'source/main.c').write_text('int decoder(void); int _start(void) { return decoder(); }\n')
    (source / 'source/ctr_audio_decoder.c').write_text('int decoder(void) { return 0; }\n')
    (source / 'rules.cmake').write_text(
        'set(CMAKE_C_OUTPUT_EXTENSION .o)\nset(CMAKE_C_OUTPUT_EXTENSION_REPLACE ON)\n')
    (source / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.19)\n'
        'set(CMAKE_SYSTEM_NAME Generic)\nset(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)\n'
        'set(CMAKE_USER_MAKE_RULES_OVERRIDE "${CMAKE_CURRENT_SOURCE_DIR}/rules.cmake")\n'
        'project(selector_probe C)\n'
        'set(PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/main.c" '
        '"${CMAKE_CURRENT_SOURCE_DIR}/source/ctr_audio_decoder.c")\n'
        'add_executable(ctrport ${PLATFORM_SOURCES})\n'
        'target_link_options(ctrport PRIVATE -nostdlib -Wl,-e,_start)\n'
        'set(CTRPORT_DKA_BIN "' + compiler.parent.as_posix() + '")\n'
        'set(CTRPORT_PYTHON_EXECUTABLE "' + Path(sys.executable).as_posix() + '")\n'
        'set(CTRPORT_TARGET o3ds)\n'
        'include("' + (root / 'platform/3ds/aacout.cmake').as_posix() + '")\n', encoding='utf-8')
    env = dict(os.environ, CMAKE_INTERMEDIATE_DIR_STRATEGY='SHORT')
    build = tmp_path / 'build'
    command = [cmake, '-S', str(source), '-B', str(build), '-G', 'Ninja',
               '-DCMAKE_MAKE_PROGRAM=' + ninja, '-DCMAKE_C_COMPILER=' + compiler.as_posix()]

    def run(argv):
        result = subprocess.run(argv, env=env, capture_output=True, text=True,
                                stdin=subprocess.DEVNULL, timeout=30)
        return result.returncode, result.stdout + result.stderr

    rc, output = run(command)
    assert rc == 0, output
    rc, output = run([cmake, '--build', str(build)])
    assert rc != 0 and 'matches 0 linker objects; expected 1' in output, output
    # Reconfigure the same SHORT cache using the actual build.sh argument.
    rc, output = run([*command, setting])
    assert rc == 0, output
    rc, output = run([cmake, '--build', str(build)])
    assert rc == 0, output
    assert 'aacout: 4 object selectors match linker inputs' in output
    assert (build / 'CMakeFiles/ctrport.dir/source/main.o').is_file()
