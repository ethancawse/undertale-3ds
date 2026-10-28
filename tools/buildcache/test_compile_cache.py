"""Tests for the compiler cache: py -3 -m pytest -q tools/buildcache/test_compile_cache.py

The unit tests need nothing else. The end-to-end tests run the real devkitARM compiler (skipped without it) through
compile_cache.py on small sources in two directory trees that stand in for two worktrees.
"""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import compile_cache as cc

SCRIPT = str(HERE / 'compile_cache.py')
ROOT = 'E:/work/tree-a' if os.name == 'nt' else '/work/tree-a'
CMAKE_LINE = ['-march=armv6k', '-mtune=mpcore', '-mfloat-abi=hard', '-D__3DS__', '-g', '-O2', '-DNDEBUG',
              '-std=gnu99', '-Wall', '-Wextra', '-Wno-format', '-ffast-math', '-fomit-frame-pointer',
              '-DCTRPORT_GAME_TITLE="DELTARUNE"', '-I' + ROOT + '/engine/src', '-isystem', ROOT + '/vendor',
              '-ffile-prefix-map=' + ROOT + '=.', '-MD', '-MT', 'CMakeFiles/x.dir/vm.o', '-MF',
              'CMakeFiles/x.dir/vm.o.d', '-o', 'CMakeFiles/x.dir/vm.o', '-c', ROOT + '/engine/src/vm.c']


def other_root(args, new_root):
    return [a.replace(ROOT, new_root) for a in args]


# ---------------------------------------------------------------------------------------------------------------------
# Command line

def test_parse_cmake_compile_line():
    p = cc.parse('gcc', CMAKE_LINE)
    assert p.lang == 'c'
    assert p.output == 'CMakeFiles/x.dir/vm.o'
    assert p.has_depfile
    assert p.prefix_map == (ROOT, '.')
    assert CMAKE_LINE[p.source].endswith('vm.c')


@pytest.mark.parametrize('change, reason', [
    (lambda a: [x for x in a if x != '-c'], 'no -c'),
    (lambda a: a + ['-E'], 'option -E'),
    (lambda a: a + ['-S'], 'option -S'),
    (lambda a: a + ['-M'], 'option -M'),
    (lambda a: a + ['-MP'], 'option -MP'),
    (lambda a: a + ['other.c'], '2 inputs'),
    (lambda a: a + ['@args.rsp'], 'response file'),
    (lambda a: a + ['-save-temps'], 'option -save-temps'),
    (lambda a: a + ['-flto'], 'option -flto'),
    (lambda a: a + ['-fprofile-generate'], 'option -fprofile-generate'),
    (lambda a: a + ['-gsplit-dwarf'], 'option -gsplit-dwarf'),
    (lambda a: a + ['-Wp,-MD,x.d'], 'option -Wp,-MD,x.d'),
    (lambda a: a + ['-Wa,-adhln=x.lst'], 'option -Wa,-adhln=x.lst'),
    (lambda a: a + ['-frecord-gcc-switches'], 'option -frecord-gcc-switches'),
    (lambda a: a + ['-fdebug-prefix-map=/a=b'], 'option -fdebug-prefix-map=/a=b'),
    (lambda a: a + ['-B', '/elsewhere'], 'option -B'),
    (lambda a: a + ['-specs', 'x.specs'], 'option -specs'),
    (lambda a: a + ['--frobnicate'], 'option --frobnicate'),
    (lambda a: a + ['-v'], 'option -v'),
    (lambda a: a + ['-MT', 'second'], '-MD needs exactly one -MF and one -MT'),
    (lambda a: [x for x in a if x not in ('-MF', 'CMakeFiles/x.dir/vm.o.d')], '-MD needs exactly one -MF and one -MT'),
    (lambda a: [x for x in a if x != '-MD'], '-MF/-MT without -MD'),
    (lambda a: a[:-1] + [a[-1][:-2] + '.S'], 'language None'),
    (lambda a: a[:-1] + ['-'], 'stdin input'),
    (lambda a: a + ['-x', 'assembler-with-cpp'], 'language assembler-with-cpp'),
])
def test_parse_falls_back(change, reason):
    with pytest.raises(cc.Unhandled) as e:
        cc.parse('gcc', change(list(CMAKE_LINE)))
    if reason != 'language None':
        assert str(e.value) == reason
    else:
        assert str(e.value).startswith('language')


def test_parse_no_output_falls_back():
    a = list(CMAKE_LINE)
    i = a.index('-o')
    del a[i:i + 2]
    with pytest.raises(cc.Unhandled, match='no -o'):
        cc.parse('gcc', a)


def test_no_map_or_two_maps_or_odd_root_means_no_normalization():
    a = [x for x in CMAKE_LINE if not x.startswith('-ffile-prefix-map')]
    assert cc.parse('gcc', a).prefix_map is None
    assert cc.parse('gcc', a + ['-ffile-prefix-map=/x=.', '-ffile-prefix-map=/y=.']).prefix_map is None
    assert cc.parse('gcc', a + ['-ffile-prefix-map=relative=.']).prefix_map is None
    assert cc.parse('gcc', a + ['-ffile-prefix-map=/with space=.']).prefix_map is None
    assert cc.parse('gcc', a + ['-ffile-prefix-map=/a$b=.']).prefix_map is None


# ---------------------------------------------------------------------------------------------------------------------
# Normalization

def test_strip_root_only_at_a_component_boundary():
    P = cc.PLACEHOLDER
    assert cc.strip_root('/r/x/engine/vm.c', '/r/x') == P + '/engine/vm.c'
    assert cc.strip_root('/r/x', '/r/x') == P
    assert cc.strip_root('/r/xy/engine/vm.c', '/r/x') == '/r/xy/engine/vm.c'
    assert cc.strip_root('/r/x/a', '/r/x/') == P + 'a'
    # The compiler does not map a path shorter than the prefix: neither may the key.
    assert cc.strip_root('/r/x', '/r/x/') == '/r/x'
    assert cc.strip_root('/other/r/x/a', '/r/x') == '/other/r/x/a'
    assert cc.strip_root('/R/X/a', '/r/x') == '/R/X/a'  # case differs: kept (the compiler may map it; safe)
    assert cc.strip_root('/r/x/a', None) == '/r/x/a'


@pytest.mark.skipif(os.name != 'nt', reason='Windows separators')
def test_strip_root_windows_separators():
    P = cc.PLACEHOLDER
    assert cc.strip_root('E:\\w\\a\\build\\x', 'E:/w/a') == P + '\\build\\x'
    assert cc.strip_root('E:/w/a\\b', 'E:/w/a') == P + '\\b'


def test_normalize_preprocessed_touches_linemarkers_only():
    src = ('# 0 "{r}/engine/src/vm.c"\n'
           '# 1 "{w}//"\n'
           '# 1 "<built-in>"\n'
           'const char* s = "{r}/engine/src/vm.c";\n'
           '# 12 "{r}/engine/src/vm.h" 1 3 4\n'
           'int x;\n'
           '# 3 "/usr/include/stdio.h" 2\n')
    w = ROOT.replace('/', '\\\\') + '\\\\build' if os.name == 'nt' else ROOT + '/build'
    data = src.format(r=ROOT, w=w).encode()
    out = cc.normalize_preprocessed(data, ROOT).decode()
    lines = out.split('\n')
    P = cc.PLACEHOLDER
    assert lines[0] == '# 0 "%s/engine/src/vm.c"' % P
    if os.name == 'nt':
        assert lines[1] == '# 1 "%s\\\\build//"' % P
    else:
        assert lines[1] == '# 1 "%s/build//"' % P
    assert lines[2] == '# 1 "<built-in>"'
    assert lines[3] == 'const char* s = "%s/engine/src/vm.c";' % ROOT  # content: as is
    assert lines[4] == '# 12 "%s/engine/src/vm.h" 1 3 4' % P
    assert lines[6] == '# 3 "/usr/include/stdio.h" 2'
    assert cc.normalize_preprocessed(data, None) == data


def test_key_args_drop_outputs_and_normalize_preprocessor_paths():
    p = cc.parse('gcc', CMAKE_LINE)
    k = cc.key_args(p)
    P = cc.PLACEHOLDER
    assert not any('vm.o' in x for x in k)
    assert '-I' + P + '/engine/src' in k
    assert P + '/vendor' in k
    assert '-ffile-prefix-map=' + P + '=.' in k
    assert '<src>' + P + '/engine/src/vm.c' in k
    assert '-O2' in k and '-g' in k
    q = cc.parse('gcc', other_root(CMAKE_LINE, ROOT + '-other'))
    assert cc.key_args(q) == k


def key_for(args, cwd, pre, ident='gcc 16.1'):
    return cc.compute_key(cc.parse('gcc', args), ident, cwd, pre)


def test_key_is_the_same_across_mapped_roots_and_differs_otherwise():
    root_b = ROOT + '-other'
    pre_a = ('# 0 "%s/engine/src/vm.c"\nint x;\n' % ROOT).encode()
    pre_b = ('# 0 "%s/engine/src/vm.c"\nint x;\n' % root_b).encode()
    a = key_for(CMAKE_LINE, ROOT + '/build/3ds', pre_a)
    b = key_for(other_root(CMAKE_LINE, root_b), root_b + '/build/3ds', pre_b)
    assert a == b
    # Different working directory below the root: different debug information.
    assert key_for(CMAKE_LINE, ROOT + '/build/3ds-o3ds', pre_a) != a
    # Code that holds the path itself (not a linemarker) keeps the roots apart.
    lit_a = pre_a + ('char* p = "%s";\n' % ROOT).encode()
    lit_b = pre_b + ('char* p = "%s";\n' % root_b).encode()
    assert key_for(CMAKE_LINE, ROOT + '/b', lit_a) != key_for(other_root(CMAKE_LINE, root_b), root_b + '/b', lit_b)
    # Without the prefix map nothing is normalized.
    nomap = [x for x in CMAKE_LINE if not x.startswith('-ffile-prefix-map')]
    assert key_for(nomap, ROOT + '/build/3ds', pre_a) != \
        key_for(other_root(nomap, root_b), root_b + '/build/3ds', pre_b)
    # Anything that changes code generation changes the key.
    assert key_for([x if x != '-O2' else '-O3' for x in CMAKE_LINE], ROOT + '/build/3ds', pre_a) != a
    assert key_for(CMAKE_LINE + ['-DEXTRA'], ROOT + '/build/3ds', pre_a) != a
    assert key_for(CMAKE_LINE, ROOT + '/build/3ds', pre_a + b' ') != a
    assert key_for(CMAKE_LINE, ROOT + '/build/3ds', pre_a, ident='gcc 16.2') != a
    # The output and dependency file names don't.
    renamed = [x.replace('vm.o', 'other.o') for x in CMAKE_LINE]
    assert key_for(renamed, ROOT + '/build/3ds', pre_a) == a


def test_key_follows_environment(monkeypatch):
    pre = b'int x;\n'
    monkeypatch.delenv('SOURCE_DATE_EPOCH', raising=False)
    a = key_for(CMAKE_LINE, ROOT, pre)
    monkeypatch.setenv('SOURCE_DATE_EPOCH', '1')
    assert key_for(CMAKE_LINE, ROOT, pre) != a


# ---------------------------------------------------------------------------------------------------------------------
# Storage

def test_entry_round_trip_and_corruption():
    e = cc.pack_entry(b'OBJ\x00\x01', b'out', b'warning: x\n', ROOT)
    assert cc.unpack_entry(e) == (b'OBJ\x00\x01', b'out', b'warning: x\n', ROOT)
    assert cc.unpack_entry(e[:-1]) is None
    assert cc.unpack_entry(e[:-1] + b'Y') is None
    assert cc.unpack_entry(b'junk') is None


def test_atomic_write_leaves_no_temp(tmp_path):
    p = tmp_path / 'e'
    assert cc.atomic_write(str(p), b'1')
    assert cc.atomic_write(str(p), b'2')
    assert p.read_bytes() == b'2'
    assert [x.name for x in tmp_path.iterdir()] == ['e']


def test_trim_removes_least_recently_used(tmp_path):
    now = time.time()
    for i in range(10):
        d = tmp_path / ('%02x' % i)
        d.mkdir()
        f = d / ('k' * 10)
        f.write_bytes(b'x' * 1000)
        os.utime(f, (now - 1000 + i, now - 1000 + i))
    stale = tmp_path / '00' / 'k.tmp1.2'
    stale.write_bytes(b'x')
    os.utime(stale, (now - 7200, now - 7200))
    (tmp_path / 'compilers').mkdir()
    removed = cc.trim(str(tmp_path), 5000, now=now)
    left = sorted(p.parent.name for p in tmp_path.glob('??/k*'))
    assert removed == 6
    assert left == ['06', '07', '08', '09']  # 4000 bytes <= 80% of 5000, newest kept
    assert not stale.exists()
    assert not (tmp_path / 'trim.lock').exists()
    assert cc.trim(str(tmp_path), 5000, now=now) == 0


def test_trim_backs_off_while_another_trims(tmp_path):
    (tmp_path / 'trim.lock').write_text('')
    d = tmp_path / 'ab'
    d.mkdir()
    (d / 'k').write_bytes(b'x' * 100)
    assert cc.trim(str(tmp_path), 10) == 0
    assert (d / 'k').exists()


def test_log_summary(tmp_path, monkeypatch):
    log = tmp_path / 'log'
    monkeypatch.setenv('CTRPORT_CCACHE_LOG', str(log))
    cc.log_event('hit', 'k', 'a.c')
    cc.log_event('hit', 'k', 'b.c')
    cc.log_event('miss', 'k', 'c.c')
    cc.log_event('uncached', 'no -c', 'd.c')
    text = cc.summary(str(log))
    assert text.startswith('compile cache: 4 compiles, 2 hits, 1 misses, 1 uncached (hit rate 50%); uncached: no -c x1')
    lines = log.read_text().splitlines()
    assert all(line.split('\t')[3].startswith('ms=') for line in lines)


def test_summary_totals_time(tmp_path):
    log = tmp_path / 'log'
    log.write_text('hit\tk\ta.c\tms=200 cpp=150 key=10\nhit\tk\tb.c\tms=400 cpp=300 key=10\n'
                   'miss\tk\tc.c\tms=3000 cpp=150 key=10 cc=2800\n')
    assert cc.summary(str(log)) == ('compile cache: 3 compiles, 2 hits, 1 misses, 0 uncached (hit rate 67%); '
                                    'hits 0.30 s each; misses 3.00 s each')


# ---------------------------------------------------------------------------------------------------------------------
# End to end with the real compiler

def find_gcc():
    for d in (os.environ.get('DEVKITARM', ''), 'C:/devkitPro/devkitARM', '/opt/devkitpro/devkitARM'):
        for name in ('arm-none-eabi-gcc.exe', 'arm-none-eabi-gcc'):
            p = os.path.join(d, 'bin', name)
            if d and os.path.isfile(p):
                return p.replace('\\', '/')
    return None


GCC = find_gcc()
needs_gcc = pytest.mark.skipif(GCC is None, reason='devkitARM not found')

HEADER = '#define GREETING "hi"\nint helper(int);\n'
SOURCE = ('#include "lib.h"\n#include <stdio.h>\n'
          'const char* where(void) { return __FILE__; }\n'
          'int f(int a) { return helper(a) + (int) sizeof(GREETING); }\n')
WARNING_SOURCE = SOURCE + 'int g(void) { int unused; return 0; }\n'


def make_tree(base, name, source=SOURCE):
    root = base / name
    (root / 'src').mkdir(parents=True)
    (root / 'build').mkdir()
    (root / 'src' / 'lib.h').write_text(HEADER)
    (root / 'src' / 'x.c').write_text(source)
    return root


def compile_line(root, extra=()):
    r = root.as_posix()
    return ['-march=armv6k', '-mfloat-abi=hard', '-g', '-O2', '-Wall', '-I' + r + '/src',
            '-ffile-prefix-map=' + r + '=.'] + list(extra) + \
           ['-MD', '-MT', 'CMakeFiles/x.o', '-MF', 'x.o.d', '-o', 'x.o', '-c', r + '/src/x.c']


def run_cached(root, cache, args, log=None, env=None):
    e = dict(os.environ)
    e.pop('CTRPORT_CCACHE', None)
    # Environment overrides --cache-dir in production; fixture caches must start empty even when pytest runs
    # inside a build campaign that supplies its own shared compiler cache.
    e.pop('CTRPORT_CCACHE_DIR', None)
    e.pop('CTRPORT_CCACHE_LOG', None)
    if log:
        e['CTRPORT_CCACHE_LOG'] = str(log)
    e.update(env or {})
    return subprocess.run([sys.executable, '-I', '-S', SCRIPT, '--cache-dir=' + str(cache), GCC] + args,
                          cwd=str(root / 'build'), capture_output=True, env=e)


def run_direct(root, args):
    return subprocess.run([GCC] + args, cwd=str(root / 'build'), capture_output=True)


def events(log):
    return [line.split('\t')[0] for line in log.read_text().splitlines()]


@needs_gcc
def test_hit_is_byte_identical_with_the_same_depfile_and_shares_across_roots(tmp_path):
    cache, log = tmp_path / 'cache', tmp_path / 'log'
    a = make_tree(tmp_path, 'tree-a')
    b = make_tree(tmp_path, 'tree-bbbbbb')
    # The reference: the plain compiler.
    assert run_direct(a, compile_line(a)).returncode == 0
    ref_obj, ref_dep = (a / 'build' / 'x.o').read_bytes(), (a / 'build' / 'x.o.d').read_bytes()
    assert run_direct(b, compile_line(b)).returncode == 0
    ref_obj_b, ref_dep_b = (b / 'build' / 'x.o').read_bytes(), (b / 'build' / 'x.o.d').read_bytes()
    assert ref_obj == ref_obj_b  # the prefix map at work
    for f in ('x.o', 'x.o.d'):
        (a / 'build' / f).unlink()
        (b / 'build' / f).unlink()

    r = run_cached(a, cache, compile_line(a), log)
    assert r.returncode == 0 and r.stderr == b''
    assert (a / 'build' / 'x.o').read_bytes() == ref_obj
    (a / 'build' / 'x.o').unlink()
    (a / 'build' / 'x.o.d').unlink()
    r = run_cached(a, cache, compile_line(a), log)
    assert r.returncode == 0 and r.stderr == b''
    assert (a / 'build' / 'x.o').read_bytes() == ref_obj
    assert (a / 'build' / 'x.o.d').read_bytes() == ref_dep

    r = run_cached(b, cache, compile_line(b), log)
    assert r.returncode == 0
    assert (b / 'build' / 'x.o').read_bytes() == ref_obj_b
    assert (b / 'build' / 'x.o.d').read_bytes() == ref_dep_b  # names tree-bbbbbb's files, not tree-a's
    assert b'tree-a' not in ref_dep_b
    assert events(log) == ['miss', 'hit', 'hit']


@needs_gcc
def test_header_change_misses(tmp_path):
    cache, log = tmp_path / 'cache', tmp_path / 'log'
    a = make_tree(tmp_path, 'tree-a')
    assert run_cached(a, cache, compile_line(a), log).returncode == 0
    first = (a / 'build' / 'x.o').read_bytes()
    (a / 'src' / 'lib.h').write_text(HEADER.replace('"hi"', '"hello"'))
    assert run_cached(a, cache, compile_line(a), log).returncode == 0
    second = (a / 'build' / 'x.o').read_bytes()
    assert first != second
    assert run_direct(a, compile_line(a)).returncode == 0
    assert (a / 'build' / 'x.o').read_bytes() == second
    assert events(log) == ['miss', 'miss']


@needs_gcc
def test_warnings_replay_exactly_and_only_in_their_own_tree(tmp_path):
    cache, log = tmp_path / 'cache', tmp_path / 'log'
    a = make_tree(tmp_path, 'tree-a', WARNING_SOURCE)
    b = make_tree(tmp_path, 'tree-bbbbbb', WARNING_SOURCE)
    direct = run_direct(a, compile_line(a))
    assert b'unused' in direct.stderr
    first = run_cached(a, cache, compile_line(a), log)
    again = run_cached(a, cache, compile_line(a), log)
    assert first.stderr == direct.stderr and again.stderr == direct.stderr
    other = run_cached(b, cache, compile_line(b), log)
    assert other.stderr == run_direct(b, compile_line(b)).stderr
    assert b'tree-a' not in other.stderr
    assert events(log) == ['miss', 'hit', 'miss']


@needs_gcc
def test_compile_error_is_passed_on_and_not_cached(tmp_path):
    cache, log = tmp_path / 'cache', tmp_path / 'log'
    a = make_tree(tmp_path, 'tree-a', SOURCE + 'int broken( {\n')
    for _ in range(2):
        r = run_cached(a, cache, compile_line(a), log)
        assert r.returncode != 0 and b'error' in r.stderr
    assert events(log) == ['uncached', 'uncached']
    assert not list(cache.glob('??/*'))


@needs_gcc
def test_incbin_is_not_cached(tmp_path):
    cache, log = tmp_path / 'cache', tmp_path / 'log'
    a = make_tree(tmp_path, 'tree-a', SOURCE + '__asm__(".incbin \\"data.bin\\"");\n')
    (a / 'build' / 'data.bin').write_bytes(b'1234')
    r = run_cached(a, cache, compile_line(a), log)
    assert r.returncode == 0
    assert events(log) == ['uncached']


@needs_gcc
def test_unknown_option_runs_the_real_compiler(tmp_path):
    cache, log = tmp_path / 'cache', tmp_path / 'log'
    a = make_tree(tmp_path, 'tree-a')
    r = run_cached(a, cache, compile_line(a, ['-save-temps']), log)
    assert r.returncode == 0
    assert (a / 'build' / 'x.o').exists() and (a / 'build' / 'x.i').exists()
    assert events(log) == ['uncached']


@needs_gcc
def test_opt_out_is_a_plain_pass_through(tmp_path):
    cache, log = tmp_path / 'cache', tmp_path / 'log'
    a = make_tree(tmp_path, 'tree-a')
    r = run_cached(a, cache, compile_line(a), log, env={'CTRPORT_CCACHE': '0'})
    assert r.returncode == 0 and (a / 'build' / 'x.o').exists()
    assert not log.exists()
    assert not cache.exists()


@needs_gcc
def test_without_the_map_trees_do_not_share(tmp_path):
    cache, log = tmp_path / 'cache', tmp_path / 'log'
    a = make_tree(tmp_path, 'tree-a')
    b = make_tree(tmp_path, 'tree-bbbbbb')

    def nomap(root):
        return [x for x in compile_line(root) if not x.startswith('-ffile-prefix-map')]
    assert run_cached(a, cache, nomap(a), log).returncode == 0
    assert run_cached(b, cache, nomap(b), log).returncode == 0
    cached = (b / 'build' / 'x.o').read_bytes()
    assert b'tree-bbbbbb' in cached
    assert run_direct(b, nomap(b)).returncode == 0
    assert (b / 'build' / 'x.o').read_bytes() == cached
    assert events(log) == ['miss', 'miss']
