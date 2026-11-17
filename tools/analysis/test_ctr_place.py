"""ctr_place.h (review decision 11-14, audit F3-1 T1): one probe compiled for each game and model gets the per-model
section names, the switch reads only the game and model defines (never the release source URL), a broken define set
stops the compile, and an Old unit links into the AAC text slot. devkitARM only; no game data."""
from pathlib import Path
import json
import re
import subprocess
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_devonly_aac as dv

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / 'platform/3ds/source/ctr_place.h'
GCC = dv.BIN / 'arm-none-eabi-gcc.exe'
READELF = dv.BIN / 'arm-none-eabi-readelf.exe'
PROBE = ('#include "ctr_place.h"\n'
         'UT_COLD_TEXT(probe) __attribute__((noinline)) int probe_fn(int x) { return x * 3 + 1; }\n'
         'UT_COLD_RODATA(probe) const int probe_table[4] = {1, 2, 3, 4};\n')
UT, DR, OLD, NEW = 'ENABLE_WAD16', 'ENABLE_WAD17', 'CTRPORT_TARGET_O3DS', 'CTRPORT_TARGET_N3DS'
CASES = {
    'UT Old': ([UT, OLD], '.aacout.text.probe', '.aacout.rodata.probe'),
    'UT Old release': ([UT, OLD, 'CTRPORT_RELEASE_SOURCE_URL="https://example.invalid/tree/0"', 'NDEBUG'],
                       '.aacout.text.probe', '.aacout.rodata.probe'),
    'UT New': ([UT, NEW], '.text_probe', '.aacout.rodata.probe'),
    'DR Old': ([DR, OLD], '.text_probe', '.rodata_probe'),
    'DR New': ([DR, NEW], '.text_probe', '.rodata_probe'),
}


def compile_probe(tmp_path, defines, source=PROBE, platform=True):
    if not GCC.is_file():
        pytest.skip('requires devkitARM')
    src, obj = tmp_path / 'probe.c', tmp_path / 'probe.o'
    src.write_text(source, encoding='utf-8')
    flags = ['-D' + d for d in (['PLATFORM_3DS'] if platform else []) + defines]
    return dv.run([GCC, '-c', '-Os', '-march=armv6k', '-mfloat-abi=hard', '-mword-relocations', '-ffunction-sections',
                   '-fdata-sections', *flags, '-I', HEADER.parent, src, '-o', obj]), obj


def section_of(obj):
    """{symbol: section name} for the probe's two symbols."""
    result = dv.run([READELF, '-W', '-S', '-s', obj])
    assert result.returncode == 0, result.stderr
    names = dict(re.findall(r'^\s*\[\s*(\d+)\]\s+(\S+)', result.stdout, re.M))
    out = {}
    for line in result.stdout.splitlines():
        f = line.split()
        if len(f) == 8 and f[7] in ('probe_fn', 'probe_table'):
            out[f[7]] = names[f[6]]
    return out


@pytest.mark.parametrize('name', CASES)
def test_each_game_and_model_gets_its_sections(tmp_path, name):
    defines, text, rodata = CASES[name]
    result, obj = compile_probe(tmp_path, defines)
    assert result.returncode == 0, result.stderr
    assert section_of(obj) == {'probe_fn': text, 'probe_table': rodata}


CONST_PROBE = ('#include "ctr_place.h"\n'
               'UT_COLD_TEXT(probe) __attribute__((noinline)) int probe_fn(int x) { return x * 3 + 1; }\n'
               'UT_COLD_TEXT_CONST(probe) const int probe_table[4] = {1, 2, 3, 4};\n')


@pytest.mark.parametrize('name', CASES)
def test_text_const_shares_the_text_name(tmp_path, name):
    """UT_COLD_TEXT_CONST (framelead, stepdrift 11-15): constants in the unit's text name, beside its code, with no
    section type conflict; no rodata input of the unit at all."""
    defines, text, _ = CASES[name]
    result, obj = compile_probe(tmp_path, defines, source=CONST_PROBE)
    assert result.returncode == 0, result.stderr
    assert section_of(obj) == {'probe_fn': text, 'probe_table': text}
    sections = dv.run([READELF, '-W', '-S', obj]).stdout
    assert 'rodata.probe' not in sections and 'rodata_probe' not in sections
    assert re.search(r'\s%s\s+PROGBITS\s+\S+\s+\S+\s+\S+\s+\S+\s+AX\s' % re.escape(text), sections), sections


def test_host_build_gets_no_section_attribute(tmp_path):
    result, obj = compile_probe(tmp_path, [UT, DR], platform=False)  # the desktop engine defines every game
    assert result.returncode == 0, result.stderr
    assert section_of(obj) == {'probe_fn': '.text.probe_fn', 'probe_table': '.rodata.probe_table'}


@pytest.mark.parametrize('name', CASES)
def test_named_bstall_text_override_leaves_constants_and_other_units_alone(tmp_path, name):
    defines, _, rodata = CASES[name]
    result, obj = compile_probe(tmp_path, defines, source=PROBE.replace('(probe)', '(bstall)'))
    assert result.returncode == 0, result.stderr
    assert section_of(obj) == {'probe_fn': '.text_bstall', 'probe_table': rodata.replace('probe', 'bstall')}
    # The probe tests above use an unlisted unit: it still follows the default on all models.


def test_named_old_overrides_match_registry():
    header = HEADER.read_text(encoding='utf-8')
    overrides = re.findall(r'^#define CTR_PLACE_OLD_OVERRIDE_(\w+) ', header, re.M)
    registry = json.loads((ROOT / 'tools/analysis/late-order.json').read_text())
    assert overrides == registry['old_late_text'] == ['bstall']


@pytest.mark.parametrize('defines,platform,message', [
    ([UT, DR, OLD], True, 'a build has one game'),
    ([OLD], True, 'a 3DS build names its game'),
    ([UT, OLD, NEW], True, 'a build has one model'),
    ([UT], True, 'a 3DS build names its model'),
    ([DR], True, 'a 3DS build names its model'),
    (['__3DS__', UT, OLD], False, 'this 3DS object is built outside ctrport'),
    (['__3DS__'], False, 'this 3DS object is built outside ctrport'),
])
def test_a_broken_define_set_stops_the_compile(tmp_path, defines, platform, message):
    result, _ = compile_probe(tmp_path, defines, platform=platform)
    assert result.returncode != 0
    assert 'ctr_place.h: ' + message in result.stderr, result.stderr


def test_the_switch_reads_only_game_and_model_defines():
    text = re.sub(r'//[^\n]*', '', HEADER.read_text(encoding='utf-8'))
    tested = set(re.findall(r'defined\((\w+)\)', text))
    assert tested == {'PLATFORM_3DS', UT, DR, OLD, NEW, '__3DS__'}, tested
    for line in text.splitlines():
        if line.startswith(('#if ', '#elif ')):
            assert re.fullmatch(r'#(?:el)?if (?:!?defined\(\w+\)(?: && | \|\| |$))+', line), line


def test_an_old_unit_links_into_the_aac_text_slot(tmp_path):
    """T1 + T3: the Old probe and an aacout.cmake list naming it put the code inside the slot
    with no emitter change; listing it is enough."""
    script = dv.configure(tmp_path / 'cm', devonly=False, aac_inputs=['.aacout.text.probe'],
                          extra='set(CTRPORT_AACOUT_FIXTURE_NO_FRAME_HOLES TRUE)\n')
    probe_dir = tmp_path / 'probe'
    probe_dir.mkdir()
    result, probe = compile_probe(probe_dir, [UT, OLD], source=PROBE.split('UT_COLD_RODATA')[0])
    assert result.returncode == 0, result.stderr
    result, output = dv.link(tmp_path / 'cm', script, dv.objects(tmp_path / 'cm') + [probe])
    assert result.returncode == 0, result.stderr
    s = dv.symbols(output)
    assert s['__aacout_text_start'] <= s['probe_fn'] < s['__aacout_text_used_end'] <= s['__aacout_text_end']
    # Unlisted, the same object fails the link instead of landing anywhere else.
    other = tmp_path / 'unlisted'
    script = dv.configure(other, devonly=False, aac_inputs=['.aacout.text.fixture'],
                          extra='set(CTRPORT_AACOUT_FIXTURE_NO_FRAME_HOLES TRUE)\n')
    result, _ = dv.link(other, script, dv.objects(other) + [probe])
    assert result.returncode != 0
    assert 'Unlisted or invalid AAC text input' in result.stderr
