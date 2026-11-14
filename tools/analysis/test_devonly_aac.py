"""Dev-only AAC inputs (devdiet decision 2, 11-11): routing, markers, budgets and negative controls.

Drives the real aacout.cmake emitter in a probe project (as test_aacslot.py does) and links assembled fixture
objects with devkitARM's ld. No game data, no full build.
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'buildcache'))
import aacout_compile
import late_order

ROOT = Path(__file__).resolve().parents[2]
BIN = Path('C:/devkitPro/devkitARM/bin')


def run(command, **kwargs):
    return subprocess.run([str(arg) for arg in command], stdin=subprocess.DEVNULL,
                          capture_output=True, text=True, timeout=60, **kwargs)


def configure(tmp_path, devonly=True, target='o3ds', extra='', aac_inputs=None):
    cmake = shutil.which('cmake')
    if not cmake or not (BIN / 'arm-none-eabi-ld.exe').is_file():
        pytest.skip('requires CMake and devkitARM')
    module = ROOT / 'platform/3ds/aacout.cmake'
    if aac_inputs is not None:
        module = tmp_path / 'platform/3ds/aacout.cmake'
        module.parent.mkdir(parents=True)
        text = (ROOT / 'platform/3ds/aacout.cmake').read_text(encoding='utf-8')
        text, count = re.subn(r'^set\(aacout_text_inputs[ \t]*\n.*?^\)',
                              'set(aacout_text_inputs\n    ' + '\n    '.join(aac_inputs) + '\n)',
                              text, flags=re.M | re.S)
        assert count == 1
        module.write_text(text, encoding='utf-8')
        for name in ('aacslot-native.json',):
            shutil.copy(ROOT / 'platform/3ds' / name, module.with_name(name))
        (tmp_path / 'tools/aot').mkdir(parents=True)
        shutil.copy(ROOT / 'tools/aot/aacslot.json', tmp_path / 'tools/aot/aacslot.json')
    (tmp_path / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.19)\nproject(devonly_probe NONE)\n'
        'add_executable(ctrport IMPORTED)\n'
        'set(CTRPORT_DKA_BIN "' + BIN.as_posix() + '")\n'
        'set(CTRPORT_SPECS_NEW "-T 3dsx.ld%s")\n'
        'set(CTRPORT_PYTHON_EXECUTABLE python)\n'
        'set(CTRPORT_TARGET ' + target + ')\n'
        'set(PLATFORM_SOURCES source/other.c source/z_labguard.c)\n'
        + ('set(CTRPORT_DEVONLY_AAC ON)\n' if devonly else '') + extra +
        'include("' + module.as_posix() + '")\n', encoding='utf-8')
    result = run([cmake, '-S', tmp_path, '-B', tmp_path / 'build'])
    assert result.returncode == 0, result.stdout + result.stderr
    return tmp_path / 'build/aacout.ld'


def assemble(tmp_path, name, text):
    folder = tmp_path / 'obj'
    folder.mkdir(exist_ok=True)
    source, obj = folder / (name + '.s'), folder / (name + '.o')
    source.write_text(text, encoding='utf-8')
    result = run([BIN / 'arm-none-eabi-as.exe', '-march=armv6k', source, '-o', obj])
    assert result.returncode == 0 and not result.stderr, result.stderr
    return obj


CRT = '.section .crt0,"ax",%progbits\n.global _start\n_start: bx lr\n'
LABGUARD = ('.section .text.helper,"ax",%progbits\n.global lg_helper\nlg_helper: .word 0x11111111\n'
            '.section .homesave_code,"ax",%progbits\n.global lg_pause\nlg_pause: .word 0x22222222\n'
            '.section .rodata,"a",%progbits\n.global lg_const\nlg_const: .word 0x33333333\n')
OTHER = ('.section .text.other,"ax",%progbits\n.global other_code\nother_code: .word 0x44444444\n'
         '.section .rodata,"a",%progbits\n.global other_const\nother_const: .word 0x55555555\n'
         '.section .text_battletouchpen,"ax",%progbits\n.global pen_code\npen_code: .word 0x66666666\n'
         '.section .rodata_battletouchpen,"a",%progbits\n.global pen_const\npen_const: .word 0x77777777\n')


def link(tmp_path, script, objects):
    output = tmp_path / 'payload.elf'
    # Forward slashes, as the real link's CMake object paths: */name.o never matches a backslash.
    result = run([BIN / 'arm-none-eabi-ld.exe', '-T', script, *(Path(o).as_posix() for o in objects), '-o', output])
    return result, output


def objects(tmp_path, labguard=LABGUARD, other=OTHER):
    return [assemble(tmp_path, 'crt', CRT), assemble(tmp_path, 'other', other),
            assemble(tmp_path, 'z_labguard', labguard)]


def symbols(path):
    result = run([BIN / 'arm-none-eabi-nm.exe', path])
    assert result.returncode == 0, result.stderr
    return {f[2]: int(f[0], 16) for f in (line.split() for line in result.stdout.splitlines()) if len(f) == 3}


def test_dev_build_routes_labguard_and_pen_inputs_after_every_tenant(tmp_path):
    script = configure(tmp_path)
    text = script.read_text(encoding='utf-8')
    assert '*/z_labguard.o' not in text.split('__aacout_text_start')[0]  # DD-1: not in the application prefix
    assert '/* aacout object */ */other.o(.text.*)' in text
    result, output = link(tmp_path, script, objects(tmp_path))
    assert result.returncode == 0 and not result.stderr, result.stderr
    s = symbols(output)
    for kind, names in (('text', ('lg_helper', 'lg_pause', 'pen_code')), ('rodata', ('lg_const', 'pen_const'))):
        start, end = s[f'__aacout_{kind}_devonly_start'], s[f'__aacout_{kind}_devonly_end']
        assert s[f'__aacout_{kind}_production_end'] <= start < end == s[f'__aacout_{kind}_used_end']
        for name in names:
            assert start <= s[name] < end, (kind, name)
    assert s['other_code'] < s['__aacout_text_start'] and s['other_const'] < s['__aacout_rodata_start']
    # list order: labguard's code, then the pen pair
    assert s['lg_helper'] < s['pen_code'] and s['lg_const'] < s['pen_const']


def test_release_script_has_no_devonly_lines_and_the_prefix_would_capture_labguard(tmp_path):
    script = configure(tmp_path, devonly=False)
    text = script.read_text(encoding='utf-8')
    for word in ('devonly', 'production_end', 'protected reserve'):
        assert word not in text
    result, output = link(tmp_path, script, objects(tmp_path))
    assert result.returncode == 0, result.stderr
    s = symbols(output)
    # Without the dev-only routing the earlier per-object selector takes labguard before the slot (DD-1's case).
    assert s['lg_helper'] < s['__aacout_text_start'] and s['lg_const'] < s['__aacout_rodata_start']


def test_release_emitter_output_is_unchanged_by_the_devonly_code(tmp_path):
    # The release script must be what the emitter wrote before the dev-only lists existed: no dev line, marker or
    # assert, and every object (z_labguard.o included, when it is a source) in the prefix.
    text = configure(tmp_path, devonly=False).read_text(encoding='utf-8')
    assert '/* aacout object */ */z_labguard.o(.text.*)' in text
    assert '/* aacout object */ */z_labguard.o(.rodata)' in text
    assert 'INPUT_SECTION_FLAGS (SHF_ALLOC & !SHF_WRITE) */' not in text


@pytest.mark.parametrize('extra,budget,label', [('', 8192, 'plain dev'),
                                                ('set(CTRPORT_LABCRASH ON)\n', 9216, 'labcrash test vehicle')])
def test_budget_padding_overflow_fails_the_link(tmp_path, extra, budget, label):
    script = configure(tmp_path, extra=extra)
    first = tmp_path / 'first'
    first.mkdir()
    result, output = link(first, script, objects(first))
    assert result.returncode == 0, result.stderr
    s = symbols(output)
    tail_start = s['__aacout_text_devonly_start']
    # Payload that fits the budget only without alignment: a 4 B input, then one aligned to 256 B whose padding
    # pushes the tail over.
    gap = (-(tail_start + 4)) % 256
    assert gap > 0
    # the pen fixture's 4 B follows: 4 + gap + big + 4 = budget + 4 > budget, while 4 + big + 4 <= budget
    big = budget - gap - 4
    assert 4 + big + 4 <= budget
    labguard = ('.section .text.small,"ax",%progbits\n.word 1\n'
                '.section .text.big,"ax",%progbits\n.p2align 8\n.space ' + str(big) + '\n')
    second = tmp_path / 'second'
    second.mkdir()
    result, _ = link(second, script, objects(second, labguard=labguard))
    assert result.returncode != 0
    assert f'AAC text: {label} dev-only tail over {budget} B' in result.stderr
    # The same bytes without the alignment fit.
    third = tmp_path / 'third'
    third.mkdir()
    labguard = labguard.replace('.p2align 8\n', '')
    result, _ = link(third, script, objects(third, labguard=labguard))
    assert result.returncode == 0, result.stderr


def test_old_production_lists_must_leave_the_protected_reserve(tmp_path):
    script = configure(tmp_path, aac_inputs=['.aacout.text.fill'], extra='set(CTRPORT_AACOUT_FIXTURE_NO_FRAME_HOLES TRUE)\n')
    filler = 80260 - 16384 + 4
    other = OTHER + '.section .aacout.text.fill,"ax",%progbits\n.space ' + str(filler) + '\n'
    result, _ = link(tmp_path, script, objects(tmp_path, other=other))
    assert result.returncode != 0
    assert 'Old production lists inside the 16,384 B protected reserve' in result.stderr


def test_old_plain_dev_free_floor(tmp_path):
    script = configure(tmp_path)
    # The tail cap and the reserve leave at least 8,192 B, so the floor fires only if a cap is changed: check it is
    # emitted for plain Old and absent for the vehicles and New.
    text = script.read_text(encoding='utf-8')
    assert 'AAC text: Old plain dev under 8,192 B free' in text
    for extra, target in (('set(CTRPORT_LABCRASH ON)\n', 'o3ds'), ('set(CTRPORT_BALLAST ON)\n', 'o3ds'), ('', 'n3ds')):
        sub = tmp_path / (target + str(len(extra)))
        sub.mkdir()
        text = configure(sub, target=target, extra=extra).read_text(encoding='utf-8')
        assert 'Old plain dev under 8,192 B free' not in text
        assert ('protected reserve' in text) == (target == 'o3ds')


SOURCE = ROOT / 'platform/3ds/source'
# z_ballast.c's compile as the real ballast build's (CMakeLists.txt: -Os and its section options, CTRPORT_BALLAST).
BALLAST_CC = ['-c', '-Os', '-std=gnu99', '-march=armv6k', '-mfloat-abi=hard', '-mtp=soft', '-mword-relocations',
              '-ffunction-sections', '-fno-merge-constants', '-fno-tree-switch-conversion', '-fno-jump-tables',
              '-D__3DS__', '-DPLATFORM_3DS', '-DENABLE_WAD16', '-DCTRPORT_BALLAST', '-DCTRPORT_TARGET_O3DS']
BALLAST_INCLUDES = [SOURCE, ROOT / 'engine/src', ROOT / 'engine/src/image', ROOT / 'engine/src/video',
                    ROOT / 'engine/vendor/stb/ds', ROOT / 'engine/vendor/stb/image', ROOT / 'engine/vendor/stb/vorbis',
                    ROOT / 'engine/vendor/md5', ROOT / 'engine/vendor/miniz', ROOT / 'platform/3ds/vendor/pvaac/src',
                    Path('C:/devkitPro/libctru/include'), Path('C:/devkitPro/portlibs/3ds/include')]
# A plain dev tail as large as the measured one (7,948 B with the pen fixture's 4 B and alignment).
DEV_TAIL = '.section .text.helper,"ax",%progbits\n.space 7940\n'


def ballast_link(tmp_path, ballast_object):
    script = configure(tmp_path, extra='set(CTRPORT_BALLAST ON)\n')
    objs = objects(tmp_path, labguard=DEV_TAIL) + [ballast_object]
    output = tmp_path / 'ballast.elf'
    result = run([BIN / 'arm-none-eabi-ld.exe', '--unresolved-symbols=ignore-all', '-T', script,
                  *(Path(o).as_posix() for o in objs), '-o', output])
    return result, output


@pytest.mark.parametrize('real', [False, True])
def test_ballast_is_not_charged_to_the_dev_tail_cap(tmp_path, real):
    """c26rev2 DDUNIT-1: a ballast build (no release URL, UNDERTALE Old) with a full-size dev tail must link. Ballast
    sits between production_end and devonly_start and is held to the slot only; the 8,192 B cap counts the tail."""
    if real:
        gcc = BIN / 'arm-none-eabi-gcc.exe'
        if not gcc.is_file() or not (BALLAST_INCLUDES[-2] / '3ds.h').is_file():
            pytest.skip('requires devkitARM gcc and libctru headers')
        if not (SOURCE / 'z_ballast.c').is_file():
            pytest.skip('z_ballast.c is a test build source and is left out of the source kits')
        obj = tmp_path / 'z_ballast.o'
        command = [gcc, *BALLAST_CC]
        for folder in BALLAST_INCLUDES:
            command += ['-I', folder]
        result = run(command + [SOURCE / 'z_ballast.c', '-o', obj])
        assert result.returncode == 0, result.stderr
    else:
        obj = assemble(tmp_path, 'z_ballast', '.section .aacout.text.ballast,"ax",%progbits\n.space 1456\n')
    result, output = ballast_link(tmp_path, obj)
    assert result.returncode == 0, result.stderr  # bae8129fb: "AAC text: plain dev dev-only tail over 8192 B"
    s = symbols(output)
    ballast = s['__aacout_text_devonly_start'] - s['__aacout_text_production_end']
    tail = s['__aacout_text_devonly_end'] - s['__aacout_text_devonly_start']
    # bae8129fb charged ballast + tail to the cap; here that sum is over it, the tail alone is not.
    assert 7944 <= tail <= 8192 < tail + ballast, (ballast, tail)


def test_ballast_build_still_fails_a_dev_tail_over_the_cap(tmp_path):
    obj = assemble(tmp_path, 'z_ballast', '.section .aacout.text.ballast,"ax",%progbits\n.space 1456\n')
    script = configure(tmp_path, extra='set(CTRPORT_BALLAST ON)\n')
    objs = objects(tmp_path, labguard='.section .text.helper,"ax",%progbits\n.space 8192\n') + [obj]
    result = run([BIN / 'arm-none-eabi-ld.exe', '-T', script, *(Path(o).as_posix() for o in objs),
                  '-o', tmp_path / 'over.elf'])
    assert result.returncode != 0 and 'plain dev dev-only tail over 8192 B' in result.stderr, result.stderr


# ===[ mode guards: CMakeLists.txt's labcrash refusals, run as written (cmake -P) ]===

def guard_blocks():
    text = (ROOT / 'platform/3ds/CMakeLists.txt').read_text(encoding='utf-8')
    blocks = []
    option = text.index('option(CTRPORT_LABCRASH ')
    labguard = text.find('\n', option) + 1  # labguard's guard is the line right after the labcrash option
    for head, start in (('if(CTRPORT_GAME STREQUAL "undertale" AND NOT CTRPORT_RELEASE_SOURCE_URL)\n', labguard),
                        ('if(CTRPORT_LABCRASH AND CTRPORT_EXITTRAIL)\n', text.find('if(CTRPORT_LABCRASH AND CTRPORT_EXITTRAIL)\n'))):
        assert text.startswith(head, start) and text.count(head) >= 1, head
        blocks.append(text[start:text.index('\nendif()\n', start) + len('\nendif()\n')])
    return ''.join(blocks)


@pytest.mark.parametrize('game,url,labcrash,exittrail,refusal', [
    ('undertale', '', 'ON', 'OFF', None),
    ('undertale', '', 'OFF', 'ON', None),
    ('undertale', '', 'ON', 'ON', 'labcrash: not with exittrail'),
    ('undertale', 'https://example.invalid/src', 'ON', 'OFF', 'labcrash: UNDERTALE test builds only'),
    ('deltarune', '', 'ON', 'OFF', 'labcrash: UNDERTALE test builds only'),
])
def test_cmake_refuses_labcrash_modes(tmp_path, game, url, labcrash, exittrail, refusal):
    cmake = shutil.which('cmake')
    if not cmake:
        pytest.skip('requires CMake')
    probe = tmp_path / 'modes.cmake'
    probe.write_text('set(CTRPORT_GAME %s)\nset(CTRPORT_RELEASE_SOURCE_URL "%s")\nset(CTRPORT_LABCRASH %s)\n'
                     'set(CTRPORT_EXITTRAIL %s)\nset(PLATFORM_SOURCES)\n%smessage(STATUS "accepted")\n'
                     % (game, url, labcrash, exittrail, guard_blocks()), encoding='utf-8')
    result = run([cmake, '-P', probe])
    if refusal is None:
        assert result.returncode == 0 and 'accepted' in result.stdout + result.stderr, result.stderr
    else:
        assert result.returncode != 0 and refusal in result.stderr, result.stdout + result.stderr


def test_writable_dev_section_is_not_taken(tmp_path):
    script = configure(tmp_path)
    other = OTHER.replace('.text_battletouchpen,"ax"', '.text_battletouchpen,"aw"')
    result, output = link(tmp_path, script, objects(tmp_path, other=other))
    assert result.returncode == 0, result.stderr
    s = symbols(output)
    assert not s['__aacout_text_devonly_start'] <= s['pen_code'] < s['__aacout_text_devonly_end']


# ===[ aacout_compile.py's link check ]===

def link_command(tmp_path, names):
    return ['gcc'] + [str(tmp_path / n) for n in names]


def test_link_check_counts_dev_selectors_and_rejects_duplicates(tmp_path):
    script = configure(tmp_path)
    good = link_command(tmp_path, ['a/other.o', 'a/z_labguard.o'])
    assert aacout_compile.check_selectors(script, good) == 2 + 8
    with pytest.raises(ValueError, match='dev-only AAC selector \\*/z_labguard.o.* matches 2'):
        aacout_compile.check_selectors(script, good + [str(tmp_path / 'b/z_labguard.o')])
    with pytest.raises(ValueError, match='matches 0'):
        aacout_compile.check_selectors(script, link_command(tmp_path, ['a/other.o']))


def test_link_check_rejects_prefix_capture_and_lines_outside_markers(tmp_path):
    script = configure(tmp_path)
    text = script.read_text(encoding='utf-8')
    command = link_command(tmp_path, ['a/other.o', 'a/z_labguard.o'])
    bad = tmp_path / 'prefix.ld'
    bad.write_text(text.replace('/* aacout object */ */other.o(.text.*)',
                                '/* aacout object */ */other.o(.text.*)\n/* aacout object */ */z_labguard.o(.text.*)'),
                   encoding='utf-8')
    with pytest.raises(ValueError, match='also an application-prefix selector'):
        aacout_compile.check_selectors(bad, command)
    bad.write_text(text + '\n/* aacout devonly */ INPUT_SECTION_FLAGS (SHF_ALLOC) *(.text_stray)\n', encoding='utf-8')
    with pytest.raises(ValueError, match='outside its markers'):
        aacout_compile.check_selectors(bad, command)
    bad.write_text(text.replace('INPUT_SECTION_FLAGS (SHF_ALLOC & !SHF_WRITE) */z_labguard.o',
                                'INPUT_SECTION_FLAGS (SHF_ALLOC & !SHF_WRITE) KEEP(*/z_labguard.o', 1), encoding='utf-8')
    with pytest.raises(ValueError, match='malformed dev-only'):
        aacout_compile.check_selectors(bad, command)


def test_release_script_link_check_is_unchanged(tmp_path):
    script = configure(tmp_path, devonly=False)
    command = link_command(tmp_path, ['a/other.o', 'a/z_labguard.o'])
    assert aacout_compile.check_selectors(script, command) == 4


# ===[ the registry's dev-list contract (late_order.py) ]===

def test_registry_devonly_contract_matches_cmake():
    registry = late_order.load()
    errors = late_order.check_devonly(late_order.AAC.read_text(encoding='utf-8'),
                                      late_order.CMAKE.read_text(encoding='utf-8'),
                                      late_order.LD.read_text(encoding='utf-8'), registry)
    assert errors == [], errors


@pytest.mark.parametrize('mutation', ['reorder', 'production_name', 'missing_late', 'unknown_late', 'pad'])
def test_registry_devonly_contract_rejects_mutations(mutation):
    import copy
    registry = copy.deepcopy(late_order.load())
    dev = registry['devonly']
    aac, cmake, ld = (p.read_text(encoding='utf-8') for p in (late_order.AAC, late_order.CMAKE, late_order.LD))
    if mutation == 'reorder':
        dev['aac_text'][0], dev['aac_text'][1] = dev['aac_text'][1], dev['aac_text'][0]
    elif mutation == 'production_name':
        dev['aac_text'].append('*(.aacout.text.touchquiz)')
        aac = aac.replace('    "*(.text_bsrdev)"\n', '    "*(.text_bsrdev)"\n    "*(.aacout.text.touchquiz)"\n')
    elif mutation == 'missing_late':
        dev['late_removed'].pop()
    elif mutation == 'unknown_late':
        dev['late_removed'][0] = '*(.text_not_a_late_input)'
        cmake = cmake.replace('"    *z_labguard.o(.text .text.* .rodata .rodata.*)\\n"',
                              '"    *(.text_not_a_late_input)\\n"')
    else:
        dev['p51_pad']['bytes'] = [0x44, 0x24]
    assert late_order.check_devonly(aac, cmake, ld, registry)
