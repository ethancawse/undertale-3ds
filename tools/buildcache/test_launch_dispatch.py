"""launch_dispatch.py runs a launcher chain in one process with the same observable results as the chain of real
processes: same compiler calls, same files, same return codes, same stdout and stderr text.

Every test runs a command twice, as real processes and through the dispatcher, from the same directory, and compares
what each left behind. The real launchers are covered here on the pass-through path (a source no rule selects, through
each launcher alone and through the whole UT chain as CMake wires it); launch_dispatch_check.py covers each launcher's
rule on the real sources of a configured build tree, where a rule's inputs exist.
"""
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import subprocess
import sys
import textwrap
import time

import pytest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DISPATCH = HERE / 'launch_dispatch.py'
PY = [sys.executable, '-X', 'utf8', '-I']

# A launcher in the shape of the real ones: split at its marker, change the command, run the rest with
# subprocess.run(stdin=DEVNULL, timeout=1800), post-process the output and write a receipt.
LAUNCHER = '''
"""toy launcher"""
import builtins, json, os, subprocess, sys, warnings
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
fresh = 'toyhelper' not in sys.modules
import toyhelper
toyhelper.COUNT += 1
name = sys.argv[1]
args = sys.argv[2:]
split = args.index('--%s-x' % name)
delegate, command = args[:split], args[split + 1:]
out = command[command.index('-o') + 1]
seen = dict(name=name, argv=sys.argv, file=__file__, main=__name__, doc=__doc__, spec=repr(__spec__),
            package=__package__, cached=__cached__, loader=type(__loader__).__name__, path=sys.path,
            site_builtins=sorted(n for n in ('exit', 'quit', 'copyright', 'credits', 'license', 'help')
                                 if hasattr(builtins, n)),
            leaked=hasattr(builtins, 'toy_leak'), fresh=fresh, count=toyhelper.COUNT,
            filters=[f[0] for f in warnings.filters], cwd=os.getcwd(), env=os.environ.get('TOY_ENV'),
            dwb=sys.dont_write_bytecode, flags=[sys.flags.isolated, sys.flags.no_site, sys.flags.utf8_mode],
            hook=sys.excepthook is sys.__excepthook__, stdout=sys.stdout is sys.__stdout__,
            meta=[type(m).__name__ for m in sys.meta_path], stdin=sys.stdin.read() if name == 'reads' else None)
builtins.toy_leak = 1
sys.excepthook = lambda *a: None
warnings.simplefilter('ignore')
sys.path.append('nowhere')
os.environ['TOY_ENV'] = name
sys.dont_write_bytecode = not sys.dont_write_bytecode
if name == 'cd':
    os.makedirs('sub', exist_ok=True)
    os.chdir('sub')
    out = os.path.join('..', out)
    command = [c if c != command[command.index('-o') + 1] else out for c in command]
command = command + ['-D' + name]
mode = os.environ.get('TOY_CALL_' + name, os.environ.get('TOY_CALL', 'run'))
if mode == 'run':
    rc = subprocess.run(delegate + command, stdin=subprocess.DEVNULL, timeout=1800).returncode
elif mode == 'call':
    rc = subprocess.call(delegate + command, stdin=subprocess.DEVNULL)
elif mode == 'inherit':
    rc = subprocess.run(delegate + command).returncode
elif mode == 'check':
    try:
        rc = subprocess.run(delegate + command, check=True).returncode
    except subprocess.CalledProcessError as e:
        rc = 'raised %s %r' % (e.returncode, type(e).__name__)
elif mode == 'capture':
    r = subprocess.run(delegate + command, stdin=subprocess.DEVNULL, capture_output=True, text=True)
    rc = r.returncode
    seen['captured'] = r.stdout + '|' + r.stderr
elif mode == 'env':
    rc = subprocess.run(delegate + command, stdin=subprocess.DEVNULL, env=dict(os.environ, TOY_ENV='given')).returncode
elif mode == 'check_call':
    rc = subprocess.check_call(delegate + command, stdin=subprocess.DEVNULL)
seen['rc'] = rc
if os.path.exists(out):
    with open(out, 'a', encoding='utf-8') as f:
        f.write('post ' + name + '\\n')
with open(out + '.' + name + '.json', 'w', encoding='utf-8') as f:
    json.dump(seen, f, indent=1, sort_keys=True)
print('toy', name, 'rc', rc)
print('toy err', name, file=sys.stderr)
sys.exit(rc if isinstance(rc, int) else 0)
'''

# The compiler: started with -B, which the dispatcher does not reproduce, so it is always a real process.
COMPILER = '''
import json, os, sys
args = sys.argv[1:]
out = args[args.index('-o') + 1]
code = int(os.environ.get('TOY_CC_STATUS', '0'))
with open(out, 'w', encoding='utf-8') as f:
    f.write(json.dumps(dict(args=args, stdin=sys.stdin.read(), env=os.environ.get('TOY_ENV'), cwd=os.getcwd())) + '\\n')
print('cc out')
print('cc err', file=sys.stderr)
sys.exit(code)
'''


@pytest.fixture
def toy(tmp_path):
    (tmp_path / 'toy_compile.py').write_text(LAUNCHER, encoding='utf-8')
    (tmp_path / 'toyhelper.py').write_text('COUNT = 0\n', encoding='utf-8')
    (tmp_path / 'cc.py').write_text(COMPILER, encoding='utf-8')
    (tmp_path / 'work').mkdir()
    return tmp_path


def toy_chain(root, names, no_site=()):
    head, markers = [], []
    for name in names:
        head += PY + (['-S'] if name in no_site else []) + [str(root / 'toy_compile.py'), name]
        markers.insert(0, '--%s-x' % name)
    return head + markers + [sys.executable, '-X', 'utf8', '-I', '-B', str(root / 'cc.py'),
                             '-DX', '-c', 'tiny.c', '-o', 'out.o']


def snapshot(work):
    files = {}
    for p in sorted(work.rglob('*')):
        if p.is_file() and '__pycache__' not in p.parts and p.name != 'dispatch.log':
            files[p.relative_to(work).as_posix()] = p.read_bytes()
    return files


def run_both(work, command, env=None, stdin=b'', log=None):
    """[(status, stdout, stderr, files)] for the real chain and for the dispatcher, each from a clean directory."""
    results = []
    for dispatched in (False, True):
        for p in list(work.iterdir()):
            shutil.rmtree(p) if p.is_dir() else p.unlink()
        e = dict(os.environ, **(env or {}))
        if log:
            e['CTRPORT_LAUNCH_DISPATCH_LOG'] = str(log)
        else:
            e.pop('CTRPORT_LAUNCH_DISPATCH_LOG', None)
        cmd = (PY + ['-S', str(DISPATCH)] + command) if dispatched else command
        r = subprocess.run(cmd, cwd=work, env=e, input=stdin, capture_output=True, timeout=600)
        results.append((r.returncode, r.stdout.replace(b'\r\n', b'\n'), r.stderr.replace(b'\r\n', b'\n'),
                        snapshot(work)))
    return results


def assert_same(real, dispatched):
    assert dispatched[0] == real[0]
    assert dispatched[1] == real[1]
    assert dispatched[2] == real[2]
    assert sorted(dispatched[3]) == sorted(real[3])
    for name in real[3]:
        assert dispatched[3][name] == real[3][name], name


def receipt(result, name):
    return json.loads(result[3]['out.o.%s.json' % name])


def last_log(log):
    return log.read_text(encoding='utf-8').splitlines()[-1].split('\t')


# ---------------------------------------------------------------------------------------------- process semantics

def test_nested_levels_match_real_processes(toy):
    """sys.argv, __file__, sys.path, site builtins, fresh modules, warnings, hooks, env and cwd per level."""
    log = toy / 'dispatch.log'
    names = ['a', 'b', 'cd', 'c', 'd']
    real, dispatched = run_both(toy / 'work', toy_chain(toy, names, no_site={'b', 'd'}), log=log)
    assert_same(real, dispatched)
    for name in names:
        seen = receipt(real, name)
        assert seen['fresh'] and seen['count'] == 1 and not seen['leaked'] and seen['hook'] and seen['stdout']
        assert seen['site_builtins'] == ([] if name in ('b', 'd') else
                                         ['copyright', 'credits', 'exit', 'help', 'license', 'quit'])
    assert receipt(real, 'b')['env'] == 'a' and receipt(real, 'b')['cwd'] == str(toy / 'work')
    assert receipt(real, 'c')['cwd'] == str(toy / 'work' / 'sub')
    fields = last_log(log)
    assert fields[1] == 'tiny.c' and 'levels=5 spawned=1' in fields[2] and fields[2].startswith('status=0 ')
    assert [x.split(':')[0] for x in fields[3].split(',')] == ['toy_compile'] * 5


@pytest.mark.parametrize('mode', ['call', 'inherit', 'check_call'])
def test_call_forms(toy, mode):
    log = toy / 'dispatch.log'
    real, dispatched = run_both(toy / 'work', toy_chain(toy, ['a', 'b', 'c']), env={'TOY_CALL': mode}, log=log)
    assert_same(real, dispatched)
    assert 'levels=3 spawned=1' in last_log(log)[2]


@pytest.mark.parametrize('mode', ['capture', 'env'])
def test_calls_it_cannot_reproduce_run_as_processes(toy, mode):
    """A call that captures output or sets env starts a real process, with the same result."""
    log = toy / 'dispatch.log'
    real, dispatched = run_both(toy / 'work', toy_chain(toy, ['a', 'b', 'c']), env={'TOY_CALL': mode}, log=log)
    assert_same(real, dispatched)
    assert 'levels=1 spawned=1' in last_log(log)[2]


@pytest.mark.parametrize('status', [0, 1, 3, 255])
def test_compiler_status_propagates(toy, status):
    real, dispatched = run_both(toy / 'work', toy_chain(toy, ['a', 'b']), env={'TOY_CC_STATUS': str(status)})
    assert real[0] == status
    assert_same(real, dispatched)


def test_check_true_raises_like_a_process(toy):
    real, dispatched = run_both(toy / 'work', toy_chain(toy, ['a', 'b']),
                                env={'TOY_CALL': 'check', 'TOY_CC_STATUS': '2'})
    assert receipt(real, 'b')['rc'] == "raised 2 'CalledProcessError'"
    assert_same(real, dispatched)


def test_standard_input_closed_where_the_chain_closes_it(toy):
    """The first level reads the build's stdin; levels and the compiler behind stdin=DEVNULL read nothing."""
    names = ['reads', 'b']
    real, dispatched = run_both(toy / 'work', toy_chain(toy, names), stdin=b'secret\n')
    assert receipt(real, 'reads')['stdin'] == 'secret\n'
    assert json.loads(real[3]['out.o'].splitlines()[0])['stdin'] == ''
    assert_same(real, dispatched)
    # a level that passes no stdin at all: its children inherit the null device it was given, levels and compiler
    real, dispatched = run_both(toy / 'work', toy_chain(toy, ['a', 'b', 'reads']), stdin=b'secret\n',
                                env={'TOY_CALL_b': 'inherit', 'TOY_CALL_reads': 'inherit'})
    assert receipt(real, 'reads')['stdin'] == ''
    assert json.loads(real[3]['out.o'].splitlines()[0])['stdin'] == ''
    assert_same(real, dispatched)
    # and the first level's children inherit the build's own stdin
    real, dispatched = run_both(toy / 'work', toy_chain(toy, ['a', 'reads']), stdin=b'secret\n',
                                env={'TOY_CALL_a': 'inherit'})
    assert receipt(real, 'reads')['stdin'] == 'secret\n'
    assert_same(real, dispatched)


EXITS = {
    'none': 'sys.exit()', 'zero': 'sys.exit(0)', 'three': 'sys.exit(3)', 'big': 'sys.exit(256)',
    'negative': 'sys.exit(-1)', 'true': 'sys.exit(True)', 'text': "sys.exit('boom')", 'fall': 'x = 1',
    'raise': "raise ValueError('bad ' + str(len(sys.argv)))", 'nested': 'def f():\n    1 / 0\nf()',
    'keyerror': "{}['k']", 'exitobj': 'raise SystemExit([1, 2])',
}


@pytest.mark.parametrize('kind', sorted(EXITS))
def test_exit_statuses_and_tracebacks(tmp_path, kind):
    script = tmp_path / ('exit_%s.py' % kind)
    script.write_text('import sys\nprint("before")\n' + EXITS[kind] + '\n', encoding='utf-8')
    (tmp_path / 'work').mkdir()
    for flags in ([], ['-S']):
        real, dispatched = run_both(tmp_path / 'work', PY + flags + [str(script), 'arg'])
        assert_same(real, dispatched)


def test_status_through_a_launcher(toy, tmp_path):
    """A launcher returns a failing level's status as its own, as with a real child."""
    bad = toy / 'bad.py'
    bad.write_text('import sys\nsys.exit(7)\n', encoding='utf-8')
    command = PY + [str(toy / 'toy_compile.py'), 'a', '--a-x'] + PY + [str(bad), '-o', 'out.o']
    real, dispatched = run_both(toy / 'work', command)
    assert receipt(real, 'a')['rc'] == 7
    assert_same(real, dispatched)


def test_other_interpreter_flags_run_as_processes(toy):
    """-B, -E without -I, or a missing script are not reproduced: those levels start real processes."""
    log = toy / 'dispatch.log'
    chain = toy_chain(toy, ['a', 'b'])
    chain[chain.index('-I', 4)] = '-E'  # level b: -X utf8 -E instead of -I
    real, dispatched = run_both(toy / 'work', chain, log=log)
    assert_same(real, dispatched)
    assert 'levels=1 spawned=1' in last_log(log)[2]


def test_plan_accepts_only_this_interpreter_in_isolated_mode():
    """plan() answers for the process it runs in, so ask a -X utf8 -I -S one, as CMake starts the dispatcher."""
    code = textwrap.dedent('''
        import sys
        sys.path.insert(0, %r)
        import launch_dispatch as d
        d._site_ready(); d.install()
        exe, s = sys.executable, %r
        cases = [[exe, '-X', 'utf8', '-I', s], [exe, '-X', 'utf8', '-I', '-S', s], [exe, '-Xutf8', '-I', s],
                 [exe, '-I', s], [exe, '-X', 'utf8', s], [exe, '-X', 'utf8', '-I', '-B', s],
                 [exe, '-X', 'utf8', '-I', s + '.missing'], [exe + 'x', '-X', 'utf8', '-I', s],
                 [exe, '-X', 'utf8', '-I', '-c', 'pass'], ['gcc', '-c', 'a.c'], 'string', [],
                 [exe.replace('\\\\', '/'), '-X', 'utf8', '-I', s]]
        print([d.plan(c) for c in cases])
    ''' % (str(HERE), str(DISPATCH)))
    out = subprocess.run(PY + ['-S', '-c', code], capture_output=True, text=True, timeout=60)
    assert out.returncode == 0, out.stderr
    assert out.stdout.strip() == ('[(4, False), (5, True), (3, False), None, None, None, None, None, None, None, '
                                  'None, None, (4, False)]')
    # a dispatcher started without -S cannot know a -S level's start state, so it reproduces no level
    out = subprocess.run(PY + ['-c', code], capture_output=True, text=True, timeout=60)
    assert out.stdout.strip() == '[None, None, None, None, None, None, None, None, None, None, None, None, None]'


def test_site_state_matches_a_real_process(tmp_path):
    """The start state of -I and -I -S levels: sys.path, builtins, import hooks, flags, main module attributes."""
    probe = tmp_path / 'probe.py'
    probe.write_text(textwrap.dedent('''
        """doc"""
        import builtins, json, sys
        print(json.dumps(dict(path=sys.path, argv=sys.argv, builtins=sorted(vars(builtins)),
                              meta=[type(m).__name__ for m in sys.meta_path], hooks=len(sys.path_hooks),
                              flags=str(sys.flags), file=__file__, doc=__doc__, name=__name__,
                              package=__package__, spec=repr(__spec__), cached=__cached__,
                              loader=type(__loader__).__name__, dwb=sys.dont_write_bytecode,
                              co=sys._getframe().f_code.co_filename,
                              interactivehook=hasattr(sys, '__interactivehook__'),
                              excepthook=sys.excepthook is sys.__excepthook__), sort_keys=True))
    '''), encoding='utf-8')
    (tmp_path / 'work').mkdir()
    for flags in ([], ['-S']):
        real, dispatched = run_both(tmp_path / 'work', PY + flags + [str(probe), 'x'])
        assert_same(real, dispatched)


def test_relative_script_path(toy):
    """A relative script path keeps sys.argv[0] as given and __file__ absolute, as the interpreter does."""
    shutil.copy(toy / 'toy_compile.py', toy / 'work' / 'toy_compile.py')
    shutil.copy(toy / 'toyhelper.py', toy / 'work' / 'toyhelper.py')
    command = PY + ['toy_compile.py', 'a', '--a-x', sys.executable, '-X', 'utf8', '-I', '-B', str(toy / 'cc.py'),
                    '-o', 'out.o']
    for p in ('toy_compile.py', 'toyhelper.py'):
        (toy / p).rename(toy / (p + '.away'))
    work = toy / 'work'
    results = []
    for dispatched in (False, True):
        for p in work.glob('out.o*'):
            p.unlink()
        cmd = (PY + ['-S', str(DISPATCH)] + command) if dispatched else command
        r = subprocess.run(cmd, cwd=work, capture_output=True, timeout=120)
        results.append((r.returncode, r.stdout, r.stderr, (work / 'out.o.a.json').read_bytes()))
    assert results[0] == results[1]
    seen = json.loads(results[0][3])
    assert seen['argv'][0] == 'toy_compile.py' and os.path.isabs(seen['file'])


def test_kill_switch_runs_the_chain_as_processes(toy):
    log = toy / 'dispatch.log'
    real, dispatched = run_both(toy / 'work', toy_chain(toy, ['a', 'b']), env={'CTRPORT_LAUNCH_DISPATCH': '0'},
                                log=log)
    assert_same(real, dispatched)
    assert not log.exists()


def test_summary(tmp_path):
    log = tmp_path / 'dispatch.log'
    log.write_text('compile\ta.c\tstatus=0 total_ms=200 levels=24 spawned=1 spawn_ms=100\tx:1\n'
                   'compile\tb.c\tstatus=0 total_ms=400 levels=24 spawned=3 spawn_ms=300\tx:1\n', encoding='utf-8')
    out = subprocess.run(PY + ['-S', str(DISPATCH), '--summary', str(log)], capture_output=True, text=True, timeout=60)
    assert out.stdout.strip() == ('launch dispatch: 2 compiles, 48 launcher levels run in-process, 4 subprocesses; '
                                  '0.30 s per compile, 0.20 s of it in subprocesses')


# ------------------------------------------------------------------------------------------ the project's launchers

def launcher_files():
    """Every launcher script CMake can put in a compile line, with the marker it splits on."""
    found = [(ROOT / 'tools/analysis/vine_old_observer_compile.py', '--')]
    for path in sorted(list((ROOT / 'tools/buildcache').glob('*_compile.py')) +
                       list((ROOT / 'tools/analysis').glob('*_compile.py'))):
        if path.name.startswith('test_'):
            continue
        text = path.read_text(encoding='utf-8')
        m = re.search(r"^MARKER = '([^']+)'", text, re.M)
        if m:
            found.append((path, m.group(1)))
    return found


def fake_compiler(root):
    cc = root / 'cc.py'
    cc.write_text(COMPILER, encoding='utf-8')
    return [sys.executable, '-X', 'utf8', '-I', '-B', str(cc)]


PASS_FLAGS = ['-DENABLE_WAD16', '-DCTRPORT_TARGET_N3DS', '-DNDEBUG', '-O2', '-c']
# The flags a launcher's own target contract asserts (CMake only wires it there).
CONTRACT_FLAGS = {'sfxlate_compile': ['-DENABLE_WAD16', '-DCTRPORT_TARGET_O3DS', '-DCTRPORT_HAVE_TREMOR', '-DNDEBUG',
                                      '-O2', '-c']}
RETIRED = {'mexdlg_d12_compile'}  # retired: refuses every compile with a message, by design


@pytest.mark.parametrize('path,marker', launcher_files(), ids=lambda x: getattr(x, 'stem', None) or '')
def test_each_launcher_passes_through_the_same(tmp_path, path, marker):
    """Each launcher alone, on a source no rule selects, makes the same compiler call in-process."""
    (tmp_path / 'work').mkdir()
    (tmp_path / 'work' / 'source').mkdir()
    (tmp_path / 'work' / 'source' / 'tiny_unselected.c').write_text('int tiny;\n', encoding='utf-8')
    log = tmp_path / 'dispatch.log'
    command = PY + [str(path), marker] + fake_compiler(tmp_path) + CONTRACT_FLAGS.get(path.stem, PASS_FLAGS) + [
        'source/tiny_unselected.c', '-o', 'out.o']
    real, dispatched = run_both(tmp_path / 'work', command, log=log)
    assert (real[0] != 0) == (path.stem in RETIRED), real[2]
    assert_same(real, dispatched)
    assert ('levels=1 spawned=%d' % (path.stem not in RETIRED)) in last_log(log)[2]


def configured_chain():
    """The ctrport launcher chain of a configured build tree named by CTRPORT_DISPATCH_TEST_BUILD (or None)."""
    build = os.environ.get('CTRPORT_DISPATCH_TEST_BUILD')
    if not build:
        return None
    sys.path.insert(0, str(HERE))
    import launch_dispatch_check
    return launch_dispatch_check.chain_of(Path(build), rebase=True)


def test_whole_configured_chain_passes_through_the_same(tmp_path):
    """The whole UT chain as CMake wrote it (CTRPORT_DISPATCH_TEST_BUILD=<build dir>), no rule selected."""
    chain = configured_chain()
    if chain is None:
        pytest.skip('set CTRPORT_DISPATCH_TEST_BUILD to a configured build directory')
    (tmp_path / 'work').mkdir()
    (tmp_path / 'work' / 'source').mkdir()
    (tmp_path / 'work' / 'source' / 'tiny_unselected.c').write_text('int tiny;\n', encoding='utf-8')
    log = tmp_path / 'dispatch.log'
    old = any(level.endswith('/sfxlate_compile.py') for level in chain.levels)
    flags = CONTRACT_FLAGS['sfxlate_compile'] if old else PASS_FLAGS
    command = chain.launchers + fake_compiler(tmp_path) + flags + ['source/tiny_unselected.c', '-o', 'out.o']
    real, dispatched = run_both(tmp_path / 'work', command, log=log, env={'CTRPORT_CCACHE': '0'})
    assert real[0] == 0, real[2]
    assert_same(real, dispatched)
    assert 'levels=%d spawned=1' % len(chain.levels) in last_log(log)[2]


# ------------------------------------------------------------------------------------------- level call timeouts
# The chain of processes bounds each level call by its timeout: subprocess.run(..., timeout=1800) kills the child and
# raises TimeoutExpired in the caller. The dispatcher must bound it too, however a level hangs inside it (c26rev4
# BF-F1). Each case runs as processes and dispatched; the old dispatcher fails every one but the last two.

GUARDED = '''import subprocess, sys
try:
    r = subprocess.run([sys.executable, '-X', 'utf8', '-I', '-S'] + sys.argv[2:], stdin=subprocess.DEVNULL,
                       timeout=float(sys.argv[1]))
except subprocess.TimeoutExpired as e:
    print('TIMEOUT', e.timeout, flush=True)
    sys.exit(7)
print('RETURNED', r.returncode, flush=True)
sys.exit(r.returncode)
'''


def deadline_case(tmp_path, inner_text, timeout, extra=(), env=None):
    """outer.py (a level call with `timeout`) -> [extra levels] -> inner.py, run as processes and dispatched:
    [(status, stdout, stderr, seconds)] for each."""
    (tmp_path / 'outer.py').write_text(GUARDED)
    (tmp_path / 'inner.py').write_text(textwrap.dedent(inner_text))
    base = PY + ['-S', str(tmp_path / 'outer.py'), str(timeout)] + list(extra) + [str(tmp_path / 'inner.py')]
    e = dict(os.environ, **(env or {}))
    out = []
    for cmd in (base, PY + ['-S', str(DISPATCH)] + base):
        began = time.monotonic()
        r = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, timeout=120, env=e, cwd=tmp_path)
        out.append((r.returncode, r.stdout.decode(), r.stderr.decode(), time.monotonic() - began))
    return out


def test_level_timeout_review_control(tmp_path):
    # c26rev4's control: a 0.2 s level call around a 1.5 s sleep. The old dispatcher returned 0 and SLOW COMPLETED.
    real, disp = deadline_case(tmp_path, '''
        import time
        time.sleep(1.5)
        print("SLOW COMPLETED", flush=True)
        ''', 0.2)
    for status, out, _err, _s in (real, disp):
        assert status == 7 and 'TIMEOUT 0.2' in out and 'SLOW COMPLETED' not in out, (real, disp)


@pytest.mark.parametrize('kind', ['sleep', 'busy'])
def test_level_timeout_stops_python_that_hangs(tmp_path, kind):
    body = 'import time\ntime.sleep(60)\n' if kind == 'sleep' else 'while True:\n    pass\n'
    real, disp = deadline_case(tmp_path, body + 'print("NOT STOPPED", flush=True)\n', 0.5)
    for status, out, _err, spent in (real, disp):
        assert status == 7 and 'TIMEOUT 0.5' in out and 'NOT STOPPED' not in out and spent < 30, (real, disp)


def test_level_timeout_kills_a_stuck_real_subprocess(tmp_path):
    # compile_cache.py's compiler calls have no timeout of their own: the level call's time bounds them.
    real, disp = deadline_case(tmp_path, '''
        import subprocess, sys
        side = 'disp' if subprocess.run.__name__ == 'dispatch_run' else 'real'
        code = ('import pathlib, time; pathlib.Path("started-%s.txt").write_text("x"); time.sleep(30); '
                'pathlib.Path("late-%s.txt").write_text("x")' % (side, side))
        subprocess.run([sys.executable, '-c', code], stdin=subprocess.DEVNULL)
        print("NOT STOPPED", flush=True)
        ''', 2)
    for status, out, _err, _spent in (real, disp):
        assert status == 7 and 'TIMEOUT 2' in out and 'NOT STOPPED' not in out, (real, disp)
    # The dispatcher kills the child the timed-out level was waiting on (the chain kills only the inner launcher, and
    # its child runs on). The child holds the captured output open, so the dispatched run ending well before the
    # child's 30 s sleep shows it did not wait (loaded runs took 7.4 s and 12.5 s); and once those 30 s have passed
    # since the child started, its late file never appearing shows the child died.
    started = tmp_path / 'started-disp.txt'
    assert started.exists() and not (tmp_path / 'late-disp.txt').exists()
    assert disp[3] < 25, disp
    time.sleep(max(0.0, started.stat().st_mtime + 33 - time.time()))
    assert not (tmp_path / 'late-disp.txt').exists()


def test_inner_timeout_is_the_inner_callers(tmp_path):
    # outer (30 s) -> middle (0.3 s) -> sleeper: the middle call times out and handles it; outer sees its status.
    (tmp_path / 'middle.py').write_text(textwrap.dedent('''
        import subprocess, sys
        try:
            subprocess.run([sys.executable, '-X', 'utf8', '-I', '-S', sys.argv[1]], stdin=subprocess.DEVNULL,
                           timeout=0.3)
        except subprocess.TimeoutExpired:
            print('MIDDLE TIMEOUT', flush=True)
            sys.exit(5)
        '''))
    real, disp = deadline_case(tmp_path, 'import time\ntime.sleep(60)\n', 30, extra=[str(tmp_path / 'middle.py')])
    for status, out, _err, spent in (real, disp):
        assert status == 5 and 'MIDDLE TIMEOUT' in out and 'RETURNED 5' in out and spent < 25, (real, disp)


def test_outer_timeout_unwinds_inner_calls(tmp_path):
    # outer (0.5 s) -> middle (30 s) -> sleeper: the outer call's time runs out first; the middle never sees it.
    (tmp_path / 'middle.py').write_text(textwrap.dedent('''
        import subprocess, sys
        try:
            r = subprocess.run([sys.executable, '-X', 'utf8', '-I', '-S', sys.argv[1]], stdin=subprocess.DEVNULL,
                               timeout=30)
        except subprocess.TimeoutExpired:
            print('MIDDLE TIMEOUT', flush=True)
            sys.exit(5)
        print('MIDDLE RETURNED', flush=True)
        '''))
    real, disp = deadline_case(tmp_path, 'import time\ntime.sleep(20)\n', 0.5, extra=[str(tmp_path / 'middle.py')])
    for status, out, _err, _spent in (real, disp):
        assert status == 7 and 'TIMEOUT 0.5' in out and 'MIDDLE' not in out, (real, disp)
    assert disp[3] < 15, disp  # the chain's sleeper, once started, outlives the killed middle and holds the output


def test_own_shorter_timeout_stays_the_levels(tmp_path):
    real, disp = deadline_case(tmp_path, '''
        import subprocess, sys
        try:
            subprocess.run([sys.executable, '-c', 'import time; time.sleep(30)'], stdin=subprocess.DEVNULL, timeout=0.3)
        except subprocess.TimeoutExpired as e:
            print('OWN TIMEOUT', e.timeout, flush=True)
            sys.exit(4)
        ''', 30)
    for status, out, _err, spent in (real, disp):
        assert status == 4 and 'OWN TIMEOUT 0.3' in out and 'RETURNED 4' in out and spent < 25, (real, disp)


def test_finished_call_leaves_no_deadline_behind(tmp_path):
    # The call's time ends with the call: its caller may run on well past it. The call has 12 s to start Python
    # and print (c26rev4 saw a 4 s budget run out under load); the caller then runs on past those 12 s.
    (tmp_path / 'outer.py').write_text(GUARDED)
    (tmp_path / 'after.py').write_text(textwrap.dedent('''
        import subprocess, sys, time
        r = subprocess.run([sys.executable, '-X', 'utf8', '-I', '-S', sys.argv[1], '12', sys.argv[2]],
                           stdin=subprocess.DEVNULL)
        time.sleep(13)
        print('AFTER', r.returncode, flush=True)
        '''))
    (tmp_path / 'quick.py').write_text('print("QUICK", flush=True)\n')
    base = PY + ['-S', str(tmp_path / 'after.py'), str(tmp_path / 'outer.py'), str(tmp_path / 'quick.py')]
    for cmd in (base, PY + ['-S', str(DISPATCH)] + base):
        r = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, timeout=90, cwd=tmp_path)
        assert r.returncode == 0 and r.stdout.decode().split() == ['QUICK', 'RETURNED', '0', 'AFTER', '0'], r


def test_level_that_will_not_stop_ends_the_process(tmp_path):
    # A level that swallows the deadline: the chain kills it (7); the dispatcher ends GRACE later with status 1.
    real, disp = deadline_case(tmp_path, '''
        import time
        while True:
            try:
                time.sleep(100)
            except BaseException:
                pass
        ''', 0.3, env={'CTRPORT_LAUNCH_DISPATCH_GRACE': '1'})
    assert real[0] == 7, real
    status, _out, err, spent = disp
    assert status == 1 and 'inner.py still running 1 s after its 0.3 s timeout ran out' in err and spent < 20, disp


CATCHERS = {
    # c26rev4 BF-R1's control: catch the deadline, finish and return normally.
    'returns': 'print("FALSE SUCCESS", flush=True)\n',
    'exits-0': 'import sys\nsys.exit(0)\n',
    'calls-on': ('import subprocess, sys\nsubprocess.run([sys.executable, "-c", "pass"], stdin=subprocess.DEVNULL)\n'
                 'print("FALSE SUCCESS", flush=True)\n'),
}


@pytest.mark.parametrize('after', sorted(CATCHERS))
def test_level_that_catches_the_deadline_still_times_out(tmp_path, after):
    # A level that catches the deadline and goes on (c26rev4 BF-R1): the chain kills it, so its caller gets
    # TimeoutExpired (7). The dispatched call must too; it can never become a success.
    body = 'import time\ntry:\n    time.sleep(3)\nexcept BaseException:\n    print("CAUGHT", flush=True)\n'
    real, disp = deadline_case(tmp_path, body + CATCHERS[after], 0.3)
    for status, out, _err, _spent in (real, disp):
        assert status == 7 and 'TIMEOUT 0.3' in out and 'RETURNED' not in out, (real, disp)


def test_outer_deadline_caught_by_an_inner_call_still_unwinds(tmp_path):
    # outer (0.3 s) -> middle, whose own call (30 s) the deadline passes through and which catches it and returns
    # 0: the outer call still times out.
    (tmp_path / 'middle.py').write_text(textwrap.dedent('''
        import subprocess, sys
        try:
            subprocess.run([sys.executable, '-X', 'utf8', '-I', '-S', sys.argv[1]], stdin=subprocess.DEVNULL,
                           timeout=30)
        except BaseException:
            pass
        print('MIDDLE RETURNED', flush=True)
        '''))
    real, disp = deadline_case(tmp_path, 'import time\ntime.sleep(20)\n', 0.3, extra=[str(tmp_path / 'middle.py')])
    for status, out, _err, _spent in (real, disp):
        assert status == 7 and 'TIMEOUT 0.3' in out and 'RETURNED 0' not in out, (real, disp)
    assert disp[3] < 15, disp  # the chain's sleeper, once started, outlives the killed middle and holds the output


def test_sigint_not_from_the_watcher_goes_to_the_old_handler():
    import importlib.util
    import signal
    spec = importlib.util.spec_from_file_location('launch_dispatch_sigint', DISPATCH)
    ld = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ld)
    before = signal.getsignal(signal.SIGINT)
    try:
        ld._hook_sigint()
        assert signal.getsignal(signal.SIGINT) is ld._on_sigint and ld.STATE.prev_sigint is before
        if before is signal.default_int_handler:
            with pytest.raises(KeyboardInterrupt):
                ld._on_sigint(signal.SIGINT, None)
        ld.STATE.sent += 1  # the watcher's own: taken, and nothing raised outside level code
        assert ld._on_sigint(signal.SIGINT, None) is None and ld.STATE.seen == ld.STATE.sent
    finally:
        signal.signal(signal.SIGINT, before)


# ------------------------------------------------------------------------------------------- rules probes
# launch_dispatch_check.py rules recompiles an object without one launcher and hashes it, each run in a fresh copy of
# the tree at the build path (round 11), one run at a time (c26rev4 BF-F2: two probes of one object at once would see
# each other's writes).

def load_check():
    import importlib.util
    spec = importlib.util.spec_from_file_location('launch_dispatch_check_under_test', HERE / 'launch_dispatch_check.py')
    check = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(check)
    return check


def fake_rules_tree(tmp_path, monkeypatch, check, run, control=None, flags='', extra=''):
    """Two launchers (inert, needed) that both name shared.c, one object; `run(name)` stands in for the compile
    without a launcher, after the fake compiler has written the build's own bytes. The control (the full line,
    starting with `python`) writes the build's bytes, or calls `control(target, argv)` when given. `flags`: the
    flags.make text; `extra`: added to each line before the compiler's -c."""
    import shlex
    import threading
    built = {'shared.c.o': b'BASE', 'other.c.o': b'OTHER'}
    for n, b in built.items():
        (tmp_path / n).write_bytes(b)
    obj = tmp_path / 'shared.c.o'
    if flags:
        (tmp_path / 'CMakeFiles/ctrport.dir').mkdir(parents=True)
        (tmp_path / 'CMakeFiles/ctrport.dir/flags.make').write_text(flags)
    scripts = [str(tmp_path / n) for n in ('inert_compile.py', 'needed_compile.py')]
    line = ('python inert_compile.py --inert python needed_compile.py --needed arm-none-eabi-gcc %s-c shared.c '
            '-o shared.c.o' % extra)
    monkeypatch.setattr(check, 'compile_lines', lambda b: [('shared.c.o', 'shared.c', line),
                                                           ('other.c.o', 'other.c', line.replace('shared', 'other'))])
    monkeypatch.setattr(check, 'chain_of', lambda b: type('C', (), {'levels': scripts})())
    monkeypatch.setattr(check, 'shell_for', lambda b: 'UNUSED')
    monkeypatch.setattr(check, 'launcher_marker', lambda s: '--' + Path(s).stem.split('_')[0])
    monkeypatch.setattr(check, 'rule_sources', lambda s: {'shared.c', 'other.c'})
    monkeypatch.setattr(check, 'without', lambda tokens, name, marker: [name])
    lock = threading.Lock()
    live = {}
    seen = {'max': 0, 'commands': []}

    def fake_run(argv, cwd):
        cmd = shlex.split(argv[2])
        name, target = cmd[0], cwd / cmd[cmd.index('-o') + 1]
        with lock:
            live[target] = live.get(target, 0) + 1
            seen['max'] = max(seen['max'], live[target])
            seen['commands'].append(argv[2])
        try:
            if name == 'python':
                if control:
                    return control(target, argv)
                target.write_bytes(built[target.name])
                return subprocess.CompletedProcess(argv, 0, b'', b'')
            target.write_bytes(built[target.name])
            return run(name, target, argv)
        finally:
            with lock:
                live[target] -= 1

    def fake_contained(argv, cwd, env, timeout):
        # A fake may return contained()'s own (status, error, empty), or a CompletedProcess for a run that ended
        # with its group empty.
        r = fake_run(argv, cwd)
        return r if isinstance(r, tuple) else (r.returncode, None, True)

    monkeypatch.setattr(check, 'contained', fake_contained)
    # The worktree outside build/ and scratch/ is not these tests' business (some tests check it in a repo of their
    # own): the real worktree may change while the tests run. (raising=False: the 041be616b witness has no
    # freeze_sources.)
    sources = dict(tracked={}, others={}, identity=('HEAD', 'index', 'pointer'))
    monkeypatch.setattr(check, 'freeze_sources', lambda root: ([], sources), raising=False)
    monkeypatch.setattr(check, 'source_state', lambda root, tracked=None: sources)
    return obj, seen


def test_rules_probes_of_one_object_never_overlap(tmp_path, monkeypatch, capsys):
    import threading
    check = load_check()
    inert_started, needed_wrote = threading.Event(), threading.Event()

    def run(name, target, argv):
        # Run at once (the old tool), the needed probe's write lands inside the inert probe's hash window: inert
        # comes out ACTIVE and needed inactive. One after the other, each sees only its own compile.
        if name == 'inert_compile.py':
            inert_started.set()
            needed_wrote.wait(3)
        else:
            inert_started.wait(3)
            if target.name == 'shared.c.o':
                target.write_bytes(b'CHANGED BY NEEDED PROBE')
            needed_wrote.set()
            time.sleep(0.5)
        return subprocess.CompletedProcess(argv, 0, b'', b'')

    obj, seen = fake_rules_tree(tmp_path, monkeypatch, check, run)
    status = check.rules(tmp_path, jobs=4)
    text = capsys.readouterr().out
    assert seen['max'] == 1, text
    assert obj.read_bytes() == b'BASE' and status == 0, text
    assert re.search(r'LAUNCHER inert_compile\.py\s+inactive in this build', text), text
    assert re.search(r'LAUNCHER needed_compile\.py\s+ACTIVE \(1 of 2', text), text
    # --expect-active turns a launcher that does not come out ACTIVE into a failure.
    assert check.rules(tmp_path, jobs=4, expect_active=['needed_compile.py']) == 0
    capsys.readouterr()
    assert check.rules(tmp_path, jobs=4, expect_active=['inert_compile']) == 1
    assert 'EXPECTED ACTIVE inert_compile.py: inactive' in capsys.readouterr().out


def test_rules_tree_unchanged_after_a_failed_probe(tmp_path, monkeypatch, capsys):
    check = load_check()

    def run(name, target, argv):
        target.write_bytes(b'HALF WRITTEN')
        (target.parent / (target.name[:-2] + '.extra')).write_bytes(b'NEW SIDE FILE')
        # What contained() returns for a timeout once the killed group is seen empty (it never raises it: round 12
        # treats an exception from contained() as "writers may be alive" and keeps the copy).
        return None, 'TimeoutExpired: no exit in 3600 s; its process group was killed', True

    obj, _seen = fake_rules_tree(tmp_path, monkeypatch, check, run)
    status = check.rules(tmp_path, jobs=2)
    text = capsys.readouterr().out
    assert status == 1 and 'ERROR (TimeoutExpired' in text, text
    assert obj.read_bytes() == b'BASE' and (tmp_path / 'other.c.o').read_bytes() == b'OTHER', text
    assert not (tmp_path / 'shared.c.extra').exists() and not (tmp_path / 'other.c.extra').exists(), text


def needed_changes_shared(name, target, argv):
    if name == 'needed_compile.py' and target.name == 'shared.c.o':
        target.write_bytes(b'CHANGED WITHOUT NEEDED')
    return subprocess.CompletedProcess(argv, 0, b'', b'')


def control_fails(target, argv):
    return subprocess.CompletedProcess(argv, 1, b'', b'fatal error: 3ds.h: No such file or directory')


def control_differs(target, argv):
    target.write_bytes(b'NOT THE BUILD')
    return subprocess.CompletedProcess(argv, 0, b'', b'')


def control_writes_nothing(target, argv):
    return subprocess.CompletedProcess(argv, 0, b'', b'')


def every_compile_fails(name, target, argv):
    return subprocess.CompletedProcess(argv, 1, b'', b'fatal error: 3ds.h: No such file or directory')


@pytest.mark.parametrize('control, run, why', [
    (control_fails, every_compile_fails, 'control: the full line fails, status 1'),  # the round-6 real tree
    (control_fails, needed_changes_shared, 'control: the full line fails, status 1'),
    (control_differs, needed_changes_shared, 'control: the full line builds different bytes: '),
    (control_writes_nothing, needed_changes_shared, 'control: the full line wrote no object')])
def test_rules_failed_control_is_error_never_active(tmp_path, monkeypatch, capsys, control, run, why):
    # Round 6 real tree: every compile failed (make's $(C_INCLUDES) left empty), with and without each launcher, and
    # the tool counted "fails without it" as ACTIVE: 22 of 22. A control that does not rebuild the build's object
    # makes that object's checks ERROR: never RED, never ACTIVE, and the launcher is not probed.
    check = load_check()
    obj, seen = fake_rules_tree(tmp_path, monkeypatch, check, run, control=control)
    status = check.rules(tmp_path, jobs=2, expect_active=['needed_compile', 'inert_compile'])
    text = capsys.readouterr().out
    assert status == 1 and 'RED' not in text and 'ACTIVE (' not in text, text
    assert text.count('ERROR (%s' % why) == 4, text
    assert re.search(r'LAUNCHER needed_compile\.py\s+not probed \(2 of 2 checks errored', text), text
    assert 'EXPECTED ACTIVE needed_compile.py: not probed' in text, text
    assert all(shlex.split(c)[0] == 'python' for c in seen['commands']), seen  # no probe ran without a launcher
    assert len(seen['commands']) == 2  # one control per object
    assert obj.read_bytes() == b'BASE' and (tmp_path / 'other.c.o').read_bytes() == b'OTHER', text


def test_rules_passing_control_then_verdicts(tmp_path, monkeypatch, capsys):
    check = load_check()
    obj, seen = fake_rules_tree(tmp_path, monkeypatch, check, needed_changes_shared)
    status = check.rules(tmp_path, jobs=2, expect_active=['needed_compile'])
    text = capsys.readouterr().out
    assert status == 0 and 'ERROR' not in text, text
    assert re.search(r'LAUNCHER needed_compile\.py\s+ACTIVE \(1 of 2', text), text
    assert re.search(r'LAUNCHER inert_compile\.py\s+inactive in this build', text), text
    assert sum(1 for c in seen['commands'] if shlex.split(c)[0] == 'python') == 2, seen  # one control per object


def test_rules_probe_that_writes_no_object_is_error(tmp_path, monkeypatch, capsys):
    # Each probe deletes the object first: a command that exits 0 and writes nothing cannot pass as "same".
    check = load_check()

    def run(name, target, argv):
        target.unlink()
        return subprocess.CompletedProcess(argv, 0, b'', b'')

    obj, _seen = fake_rules_tree(tmp_path, monkeypatch, check, run)
    assert check.rules(tmp_path, jobs=2) == 1
    text = capsys.readouterr().out
    assert text.count('ERROR (no object written without it)') == 4, text
    assert obj.read_bytes() == b'BASE', text


def pristine_dirs(build):
    """The directories a rules run moves its build tree into (.rules-pristine-<tree>-*), beside the tree."""
    return sorted(p for p in Path(build).parent.iterdir() if p.name.lower().startswith('.rules-pristine-'))


def pristine_tree(build):
    """The pristine tree of the one rules run going on beside build (during a run)."""
    dirs = pristine_dirs(build)
    assert len(dirs) == 1, dirs
    return dirs[0] / 'tree'


def put_back(check, build):
    """What a rules run's README.txt says to do when it was killed: delete what is at the build path without following
    links, move tree/ back, delete the directory. Here for tests that leave the tree aside on purpose, since a
    leftover would make every later rules run in the same pytest directory refuse to start."""
    for aside in pristine_dirs(build):
        if os.path.lexists(build):
            check.remove_tree(build)
        os.rename(aside / 'tree', build)
        (aside / 'README.txt').unlink()
        aside.rmdir()


def failing_sources(check, monkeypatch, when):
    """Make the worktree check after each run report a changed tracked source once when() is true."""
    real = check.state_differences
    monkeypatch.setattr(check, 'state_differences',
                        lambda base, now: ['tracked source.c: bytes changed'] if when() else real(base, now))


def test_c26rev4_restore_failed_red_is_never_active(tmp_path, monkeypatch, capsys):
    # c26rev4 BF7-F2: a probe that came out RED but did not end clean still made its launcher ACTIVE. Any problem
    # makes the check errored, RED or not, and no later probe runs. Here inert's probe of shared.c.o changes it and
    # the worktree outside build/ comes out changed after it.
    check = load_check()
    flag = []

    def run(name, target, argv):
        if name == 'inert_compile.py' and target.name == 'shared.c.o':
            target.write_bytes(b'CHANGED WITHOUT INERT')
            flag.append(1)
        return subprocess.CompletedProcess(argv, 0, b'', b'')

    obj, seen = fake_rules_tree(tmp_path, monkeypatch, check, run)
    failing_sources(check, monkeypatch, lambda: bool(flag))
    status = check.rules(tmp_path, jobs=2, expect_active=['inert_compile'])
    text = capsys.readouterr().out
    assert status == 1 and 'ACTIVE (' not in text, text
    assert re.search(r'inert_compile\.py .*shared\.c\.o +RED \(differs without it: shared\.c\.o\) .*RESTORE FAILED',
                     text), text
    assert re.search(r'NOT CLEAN after shared\.c\.o without inert_compile\.py: the worktree outside build/ and '
                     r'scratch/ changed: tracked source\.c: bytes changed', text), text
    assert re.search(r'needed_compile\.py .*shared\.c\.o +ERROR \(not run: an earlier run on these files did not end '
                     r'cleanly: RESTORE FAILED after inert_compile\.py on shared\.c\.o\).* NOT RUN', text), text
    assert re.search(r'LAUNCHER inert_compile\.py\s+not probed \(1 of 2 checks errored: shared\.c\)', text), text
    assert re.search(r'LAUNCHER needed_compile\.py\s+not probed \(1 of 2', text), text
    assert 'EXPECTED ACTIVE inert_compile.py: not probed' in text, text
    assert re.search(r'TREE \S+: all \d+ directories and files equal', text), text  # the pristine tree came back
    assert obj.read_bytes() == b'BASE' and not pristine_dirs(tmp_path), text
    shared_probes = [c for c in seen['commands'] if 'shared.c.o' in c and shlex.split(c)[0] != 'python']
    assert len(shared_probes) == 1, seen  # needed's probe of shared.c.o never ran


def test_rules_writers_may_be_alive_stops_the_group(tmp_path, monkeypatch, capsys):
    # BF7-F1: a run whose process group is not seen empty after the kill may still have writers. The check is ERROR
    # and nothing else runs. BF11-F2 (round 12): its copy is not deleted and the pristine tree is not moved back over
    # it; both paths are printed for recovery by hand, exit 1.
    check = load_check()
    removed = []
    real = check.remove_copy
    monkeypatch.setattr(check, 'remove_copy', lambda root, identity: removed.append(root) or real(root, identity))

    def run(name, target, argv):
        if name == 'inert_compile.py' and target.name == 'shared.c.o':
            target.write_bytes(b'CHANGED WITHOUT INERT')
            return None, 'writers may be alive: the process group was not seen empty 30 s after the kill', False
        return subprocess.CompletedProcess(argv, 0, b'', b'')

    obj, seen = fake_rules_tree(tmp_path, monkeypatch, check, run)
    try:
        status = check.rules(tmp_path, jobs=2)
        text = capsys.readouterr().out
        assert status == 1 and 'ACTIVE (' not in text, text
        assert re.search(r'inert_compile\.py .*shared\.c\.o +ERROR \(writers may be alive: .*WRITERS MAY BE ALIVE',
                         text), text
        assert re.search(r'needed_compile\.py .*shared\.c\.o +ERROR \(not run: .*WRITERS MAY BE ALIVE after '
                         r'inert_compile\.py on shared\.c\.o', text), text
        assert re.search(r'LAUNCHER inert_compile\.py\s+not probed', text), text
        assert re.search(r'LAUNCHER needed_compile\.py\s+not probed', text), text
        assert len([c for c in seen['commands'] if 'shared.c.o' in c and shlex.split(c)[0] != 'python']) == 1, seen
        # The copy the uncertain run used is still at the build path, as the run left it; the pristine tree is aside.
        assert obj.read_bytes() == b'CHANGED WITHOUT INERT', text
        assert (pristine_tree(tmp_path) / 'shared.c.o').read_bytes() == b'BASE', text
        # Every run before the uncertain one had its copy deleted; the uncertain one did not.
        assert len(removed) == len(seen['commands']) - 1, (removed, seen)
        assert re.search(r'PRISTINE TREE LEFT AT \S+tree: writers may be alive in the run\'s copy at %s ' %
                         re.escape(str(tmp_path)), text), text
        assert 'RECOVER BY HAND: build path %s, pristine tree %s' % (tmp_path, pristine_tree(tmp_path)) in text, text
    finally:
        monkeypatch.undo()
        put_back(check, tmp_path)
    assert obj.read_bytes() == b'BASE' and not pristine_dirs(tmp_path)


def entries(root):
    """What the checker's manifest compares, read here independently: every path's kind, size, mtime, attributes or
    mode, and bytes."""
    out = {}
    for p in sorted(Path(root).rglob('*')):
        st = os.stat(p)
        attrs = getattr(st, 'st_file_attributes', None) if os.name == 'nt' else stat.S_IMODE(st.st_mode)
        out[p.relative_to(root).as_posix()] = (p.is_dir(), st.st_mtime_ns, attrs) + (
            () if p.is_dir() else (p.read_bytes(),))
    st = os.stat(root)
    out['.'] = (True, st.st_mtime_ns)
    return out


def test_rules_object_left_read_only_is_deleted_with_its_copy(tmp_path, monkeypatch, capsys):
    check = load_check()

    def run(name, target, argv):
        if name == 'inert_compile.py' and target.name == 'shared.c.o':
            target.write_bytes(b'CHANGED WITHOUT INERT')
            os.chmod(target, stat.S_IREAD)
        return subprocess.CompletedProcess(argv, 0, b'', b'')

    obj, _seen = fake_rules_tree(tmp_path, monkeypatch, check, run)
    (tmp_path / 'flags.h').write_bytes(b'READ-ONLY IN THE BUILD')
    os.chmod(tmp_path / 'flags.h', stat.S_IREAD)  # copied read-only, and deleted with each copy
    before = entries(tmp_path)
    try:
        status = check.rules(tmp_path, jobs=2)
        after = entries(tmp_path)
    finally:
        os.chmod(tmp_path / 'flags.h', stat.S_IREAD | stat.S_IWRITE)
    text = capsys.readouterr().out
    assert status == 0 and after == before and not pristine_dirs(tmp_path), text
    assert re.search(r'TREE \S+: all \d+ directories and files equal', text), text


def sibling_writer(name, target, argv):
    """The real tree's round-8 finding: a launcher rewrites a work file of its own beside the objects, outside the
    object's stem (utwindow0.s), and creates another (smsrender0.s)."""
    (target.parent / 'utwindow0.s').write_bytes(b'WORK FILE OF THIS PROBE ' + name.encode())
    (target.parent / 'smsrender0.s').write_bytes(b'NEW WORK FILE')
    return needed_changes_shared(name, target, argv)


def test_c26rev4_work_files_outside_the_stem_never_reach_the_next_run(tmp_path, monkeypatch, capsys):
    # Round 8 real tree: 22/22 ACTIVE, no ERROR, and 18 launcher work files changed and 2 new afterwards: the restore
    # covered only each object and its stem's side files. Every run now has a fresh copy of the whole tree, and every
    # run that left anything besides its own object's files is reported.
    check = load_check()
    (tmp_path / 'utwindow0.s').write_bytes(b'WORK FILE OF THE BUILD')
    seen_work = []

    def run(name, target, argv):
        seen_work.append(((target.parent / 'utwindow0.s').read_bytes(), (target.parent / 'smsrender0.s').exists()))
        return sibling_writer(name, target, argv)

    obj, _seen = fake_rules_tree(tmp_path, monkeypatch, check, run)
    before = entries(tmp_path)
    status = check.rules(tmp_path, jobs=1, expect_active=['needed_compile'])
    text = capsys.readouterr().out
    assert entries(tmp_path) == before, text
    assert status == 0, text
    assert seen_work == [(b'WORK FILE OF THE BUILD', False)] * 4, seen_work
    assert re.search(r'LAUNCHER needed_compile\.py\s+ACTIVE \(1 of 2', text), text
    assert re.search(r'TOUCHED OUTSIDE shared\.c\.o without needed_compile\.py: 2 paths, gone with the copy: '
                     r'smsrender0\.s: new file; utwindow0\.s: size, mtime, bytes changed', text), text
    assert 'TOUCHED OUTSIDE: 4 runs left paths' in text, text  # the 4 probes; the controls write no work file


def test_c26rev4_pristine_tree_changed_during_a_run_is_restore_failed(tmp_path, monkeypatch, capsys):
    # Nothing runs in the pristine tree, but it is ordinary files beside the build path: a launcher that goes looking
    # for it can change it. The next copy is checked against the first manifest, so the next run does not start,
    # and the final check names the file.
    check = load_check()
    (tmp_path / 'utwindow0.s').write_bytes(b'WORK FILE OF THE BUILD')

    def run(name, target, argv):
        (pristine_tree(tmp_path) / 'utwindow0.s').write_bytes(b'CHANGED PRISTINE')
        return sibling_writer(name, target, argv)

    obj, seen = fake_rules_tree(tmp_path, monkeypatch, check, run)
    status = check.rules(tmp_path, jobs=1, expect_active=['needed_compile'])
    text = capsys.readouterr().out
    assert status == 1 and 'ACTIVE (' not in text, text
    assert re.search(r'NOT CLEAN after \S+ without needed_compile\.py: the copy differs from the tree rules found: '
                     r'utwindow0\.s: (size, )?bytes changed', text), text
    assert text.count(' NOT RUN') == 2, text  # every later check, in every group
    assert re.search(r'TREE CHANGED \S+: 1 paths differ', text) and '  utwindow0.s: ' in text, text
    assert len([c for c in seen['commands'] if shlex.split(c)[0] != 'python']) == 1, seen
    assert not pristine_dirs(tmp_path), text


def test_rules_writes_outside_the_object_directory_never_reach_the_next_run(tmp_path, monkeypatch, capsys):
    check = load_check()
    (tmp_path / 'objects').mkdir()

    def run(name, target, argv):
        assert not (tmp_path / 'objects' / 'install.log').exists()
        (tmp_path / 'objects' / 'install.log').write_text('written by a probe\n')
        return needed_changes_shared(name, target, argv)

    fake_rules_tree(tmp_path, monkeypatch, check, run)
    before = entries(tmp_path)
    status = check.rules(tmp_path, jobs=1, expect_active=['needed_compile'])
    text = capsys.readouterr().out
    assert status == 0 and entries(tmp_path) == before, text
    assert re.search(r'TOUCHED OUTSIDE \S+ without \S+: 2 paths, gone with the copy: objects: mtime changed; '
                     r'objects/install\.log: new file', text), text


def source_repo(path):
    """A git repository with one tracked source.c and an ignored ignored.h."""
    path.mkdir()
    git = ['git', '-C', str(path), '-c', 'user.name=t', '-c', 'user.email=t@example.invalid']
    (path / 'source.c').write_bytes(b'int x;\n')
    (path / '.gitignore').write_bytes(b'ignored.h\n')
    for args in (['init', '-q'], ['add', '.'], ['commit', '-q', '-m', 'base']):
        subprocess.run(git + args, check=True, capture_output=True, timeout=120, stdin=subprocess.DEVNULL)
    return path, git


def real_sources(check, monkeypatch, repo):
    """Undo fake_rules_tree's worktree stubs: rules reads repo as its worktree."""
    fresh = load_check()
    monkeypatch.setattr(check, 'ROOT', repo)
    monkeypatch.setattr(check, 'freeze_sources', fresh.freeze_sources)
    monkeypatch.setattr(check, 'source_state', fresh.source_state)
    # fresh's functions read their own module's globals; they need no ROOT.


def test_rules_changed_worktree_sources_are_restore_failed(tmp_path, monkeypatch, capsys):
    # The worktree outside build/ and scratch/ must not change during a run: rules cannot put it back.
    check = load_check()
    repo, _git = source_repo(tmp_path / 'repo')
    build = tmp_path / 'tree'
    build.mkdir()

    def run(name, target, argv):
        if name == 'inert_compile.py':
            (repo / 'source.c').write_text('int x = 1;\n')
        return needed_changes_shared(name, target, argv)

    fake_rules_tree(build, monkeypatch, check, run)
    real_sources(check, monkeypatch, repo)
    status = check.rules(build, jobs=1, expect_active=['needed_compile'])
    text = capsys.readouterr().out
    assert status == 1 and 'ACTIVE (' not in text, text
    assert re.search(r'NOT CLEAN after \S+ without inert_compile\.py: the worktree outside build/ and scratch/ '
                     r'changed: tracked source\.c: bytes changed', text), text


def test_c26rev4_r10_index_skip_worktree_cannot_hide_a_tracked_change(tmp_path, monkeypatch, capsys):
    # BF10-F3: `git update-index --skip-worktree` hid a tracked edit from git status/diff. Tracked bytes are read
    # directly now, and the index file's hash is part of the state.
    check = load_check()
    repo, git = source_repo(tmp_path / 'repo')
    build = repo / 'build' / 'tree'
    build.mkdir(parents=True)

    def run(name, target, argv):
        if name == 'inert_compile.py':
            subprocess.run(git + ['update-index', '--skip-worktree', 'source.c'], check=True, capture_output=True,
                           timeout=120, stdin=subprocess.DEVNULL)
            (repo / 'source.c').write_bytes(b'CHANGED BY PROBE\n')
        return needed_changes_shared(name, target, argv)

    fake_rules_tree(build, monkeypatch, check, run)
    real_sources(check, monkeypatch, repo)
    status = check.rules(build, jobs=1, expect_active=['needed_compile'])
    text = capsys.readouterr().out
    assert status == 1 and 'ACTIVE (' not in text, text
    assert re.search(r'NOT CLEAN after \S+ without inert_compile\.py: the worktree outside build/ and scratch/ '
                     r'changed: git index changed; tracked source\.c: bytes changed', text), text
    status_text = subprocess.run(git + ['status', '--porcelain'], capture_output=True, timeout=120,
                                 stdin=subprocess.DEVNULL).stdout
    assert b'source.c' not in status_text  # what git status shows: nothing


def test_c26rev4_r10_ignored_and_untracked_sources_are_visible(tmp_path):
    check = load_check()
    repo, _git = source_repo(tmp_path / 'repo')
    base = check.source_state(repo)
    (repo / 'ignored.h').write_bytes(b'NEW INPUT')
    assert check.state_differences(base, check.source_state(repo)) == ['others ignored.h: new']
    base = check.source_state(repo)
    (repo / 'ignored.h').write_bytes(b'CHANGED INPUT')
    (repo / 'new-header.h').write_bytes(b'NEW')
    assert check.state_differences(base, check.source_state(repo)) == ['others ignored.h: bytes changed',
                                                                       'others new-header.h: new']
    (repo / 'build').mkdir()
    (repo / 'build' / 'out.o').write_bytes(b'NOT A SOURCE')
    assert 'build/out.o' not in check.source_state(repo)['others']


@pytest.mark.parametrize('name', ['.RULES-COPY-tree-abandoned', '.Rules-Pristine-tree-20261113-000000-1',
                                  '.rules-pristine-other-20261113-000000-1'])
def test_c26rev4_r10_refuses_to_start_beside_a_leftover_any_case(tmp_path, monkeypatch, capsys, name):
    # BF10-F4: the refusal was case-sensitive. Any tree's copy or pristine directory, any case, stops rules.
    check = load_check()
    build = tmp_path / 'tree'
    build.mkdir()
    (tmp_path / name).mkdir()
    (tmp_path / name / 'README.txt').write_text('prior interrupted run')
    obj, seen = fake_rules_tree(build, monkeypatch, check, needed_changes_shared)
    assert check.rules(build, jobs=1, expect_active=['needed_compile']) == 1
    text = capsys.readouterr().out
    assert 'not run: an earlier rules run left' in text and name in text, text
    assert not seen['commands'] and obj.read_bytes() == b'BASE'


def make_junction(link, target):
    import _winapi
    _winapi.CreateJunction(str(target), str(link))


@pytest.mark.skipif(os.name != 'nt', reason='NTFS junctions, hard links and named streams')
@pytest.mark.parametrize('kind', ['junction', 'hard link', 'named stream', 'junction root'])
def test_rules_refuses_a_tree_it_cannot_copy_exactly(tmp_path, monkeypatch, capsys, kind):
    # Round 11 point 1: a tree that holds a reparse point, a hard-linked file or a named stream is refused, exit 2,
    # before anything is moved or run.
    check = load_check()
    real = tmp_path / 'real'
    real.mkdir()
    build = tmp_path / ('link' if kind == 'junction root' else 'tree')
    foreign = tmp_path / 'foreign'
    foreign.mkdir()
    (foreign / 'must-survive').write_bytes(b'FOREIGN')
    if kind == 'junction root':
        make_junction(build, real)
    else:
        build.mkdir()
    obj, seen = fake_rules_tree(build, monkeypatch, check, needed_changes_shared)
    if kind == 'junction':
        make_junction(build / 'sub', foreign)
    elif kind == 'hard link':
        os.link(foreign / 'must-survive', build / 'alias.h')
    elif kind == 'named stream':
        with open(str(build / 'shared.c.o') + ':review-data', 'wb') as f:
            f.write(b'STREAM')
    try:
        status = check.rules(build, jobs=1)
        text = capsys.readouterr().out
        assert status == 2 and not seen['commands'], text
        assert 'not run: the tree holds what rules cannot copy exactly: %s' % {
            'junction': 'a reparse point', 'junction root': 'a reparse point', 'hard link': 'a file with 2 hard links',
            'named stream': 'named streams :review-data:$DATA'}[kind] in text, text
        assert (foreign / 'must-survive').read_bytes() == b'FOREIGN' and not pristine_dirs(build)
        assert obj.read_bytes() == b'BASE'
    finally:
        if kind == 'junction':
            os.rmdir(build / 'sub')
        elif kind == 'junction root':
            os.rmdir(build)


@pytest.mark.skipif(os.name != 'nt', reason='NTFS junctions')
def test_c26rev4_r10_junction_made_during_a_probe_never_deletes_outside(tmp_path, monkeypatch, capsys):
    # BF10-F1 (HIGH): a junction a probe made in the tree was walked by the restore, which deleted the file it
    # reached outside the tree. Each copy is deleted without following anything now: the junction goes, its target
    # stays, and the next run's copy has no junction.
    check = load_check()
    build = tmp_path / 'tree'
    build.mkdir()
    foreign = tmp_path / 'foreign'
    foreign.mkdir()
    (foreign / 'must-survive').write_bytes(b'FOREIGN')
    (foreign / 'sub').mkdir()
    (foreign / 'sub' / 'deep').write_bytes(b'DEEP')
    os.chmod(foreign / 'sub' / 'deep', stat.S_IREAD)  # read-only: a delete that follows would clear it first
    found = []

    def run(name, target, argv):
        link = target.parent / 'new-junction'
        found.append(os.path.lexists(link))
        make_junction(link, foreign)
        return needed_changes_shared(name, target, argv)

    obj, seen = fake_rules_tree(build, monkeypatch, check, run)
    before = entries(foreign)
    try:
        status = check.rules(build, jobs=1, expect_active=['needed_compile'])
        after = entries(foreign)
    finally:
        os.chmod(foreign / 'sub' / 'deep', stat.S_IREAD | stat.S_IWRITE)
    text = capsys.readouterr().out
    assert after == before, text  # everything outside, attributes included, as it was
    assert found == [False] * 4, found  # each probe started from a fresh copy
    assert status == 0 and not os.path.lexists(build / 'new-junction'), text
    assert re.search(r'TOUCHED OUTSIDE \S+ without inert_compile\.py: 1 paths, gone with the copy: new-junction: a '
                     r'reparse point', text), text
    assert re.search(r'TREE tree: all \d+ directories and files equal', text), text


@pytest.mark.parametrize('outside_read_only', [False, True])
def test_c26rev4_r10_hard_link_made_during_a_probe_leaves_the_outside_file(tmp_path, monkeypatch, capsys,
                                                                          outside_read_only):
    # BF10-F2: a hard link to an outside input, made in the tree by a probe, passed with rc 0 (its bytes were equal).
    # The copy is deleted after the run: the link goes and the outside file is untouched. A read-only outside file
    # cannot be unlinked through its link without clearing read-only on the outside file too, so that copy is not
    # deleted: RESTORE FAILED, everything stops, the outside file's attributes stay, and the tree stays aside.
    check = load_check()
    build = tmp_path / 'tree'
    build.mkdir()
    (build / 'work.s').write_bytes(b'GOOD')
    outside = tmp_path / 'outside-input.h'
    outside.write_bytes(b'GOOD')
    if outside_read_only:
        os.chmod(outside, stat.S_IREAD)
    found = []

    def run(name, target, argv):
        work = target.parent / 'work.s'
        found.append(os.stat(work).st_nlink)
        work.unlink()
        os.link(outside, work)
        return needed_changes_shared(name, target, argv)

    obj, seen = fake_rules_tree(build, monkeypatch, check, run)
    before = os.stat(outside)
    try:
        status = check.rules(build, jobs=1, expect_active=['needed_compile'])
        text = capsys.readouterr().out
        after = os.stat(outside)
        assert outside.read_bytes() == b'GOOD', text
        assert (after.st_mode, getattr(after, 'st_file_attributes', None)) == (
            before.st_mode, getattr(before, 'st_file_attributes', None)), text
        if not outside_read_only or os.name != 'nt':
            assert status == 0 and found == [1] * 4 and after.st_nlink == 1, (text, found)
            assert re.search(r'TOUCHED OUTSIDE \S+ without inert_compile\.py: 1 paths, gone with the copy: work\.s: a '
                             r'file with 2 hard links', text), text
            assert (build / 'work.s').read_bytes() == b'GOOD' and not os.path.samefile(build / 'work.s', outside)
        else:
            assert status == 1 and found == [1] and 'ACTIVE (' not in text, (text, found)
            assert re.search(r'NOT CLEAN after other\.c\.o without inert_compile\.py: the copy could not be deleted: '
                             r'\S+work\.s: \[WinError 5\]', text), text
            assert text.count(' NOT RUN') == 3, text
            assert re.search(r'PRISTINE TREE LEFT AT \S+tree: the build path still holds something rules did not delete',
                         text), text
            assert pristine_dirs(build) and os.path.samefile(build / 'work.s', outside)
    finally:
        os.chmod(outside, stat.S_IREAD | stat.S_IWRITE)
        put_back(check, build)
    assert (build / 'work.s').read_bytes() == b'GOOD' and os.stat(outside).st_nlink == 1


@pytest.mark.skipif(os.name != 'nt', reason='NTFS named streams')
def test_c26rev4_r10_named_stream_made_during_a_probe_is_gone_at_the_next(tmp_path, monkeypatch, capsys):
    # BF10-F2: a named stream a probe left on a file of the tree passed with rc 0. The copy carrying it is deleted:
    # the next probe's copy has no stream, and neither has the tree at the end.
    check = load_check()
    build = tmp_path / 'tree'
    build.mkdir()
    (build / 'work.s').write_bytes(b'GOOD')
    found = []

    def run(name, target, argv):
        stream = str(target.parent / 'work.s') + ':review-data'
        found.append(os.path.exists(stream))
        with open(stream, 'wb') as f:
            f.write(b'LEFT BY PROBE')
        return needed_changes_shared(name, target, argv)

    fake_rules_tree(build, monkeypatch, check, run)
    status = check.rules(build, jobs=1, expect_active=['needed_compile'])
    text = capsys.readouterr().out
    assert status == 0 and found == [False] * 4, (text, found)
    assert not os.path.exists(str(build / 'work.s') + ':review-data') and check.named_streams(build / 'work.s') == []
    assert re.search(r'TOUCHED OUTSIDE \S+ without inert_compile\.py: 1 paths, gone with the copy: work\.s: named '
                     r'streams :review-data:\$DATA', text), text


@pytest.mark.skipif(os.name != 'nt', reason='Windows sharing lock')
def test_c26rev4_r10_copy_that_cannot_be_deleted_stops_everything(tmp_path, monkeypatch, capsys):
    # c26rev4's lock control: a probe leaves a file of its copy open without delete sharing. The copy cannot be
    # deleted: RESTORE FAILED, no later run, and the pristine tree stays aside with its path printed.
    import ctypes
    from ctypes import wintypes as W
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.CreateFileW.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, ctypes.c_void_p, W.DWORD, W.DWORD, W.HANDLE]
    k.CreateFileW.restype = W.HANDLE
    k.CloseHandle.argtypes = [W.HANDLE]
    k.CloseHandle.restype = W.BOOL
    check = load_check()
    work = tmp_path / 'work.s'
    work.write_bytes(b'GOOD')
    handles = []

    def run(name, target, argv):
        work.write_bytes(b'BAD')
        h = k.CreateFileW(str(work), 0x80000000, 1, None, 3, 0, None)
        assert h not in (None, ctypes.c_void_p(-1).value), ctypes.get_last_error()
        handles.append(h)
        return needed_changes_shared(name, target, argv)

    obj, seen = fake_rules_tree(tmp_path, monkeypatch, check, run)
    monkeypatch.setattr(check, 'REMOVE_GRACE', 1)  # in use for good: tried again for 1 s, then RESTORE FAILED
    try:
        status = check.rules(tmp_path, expect_active=['needed_compile'])
        text = capsys.readouterr().out
    finally:
        for h in handles:
            k.CloseHandle(h)
    try:
        assert status == 1 and len(seen['commands']) == 2 and text.count(' NOT RUN') == 3, text
        assert re.search(r'NOT CLEAN after other\.c\.o without inert_compile\.py: the copy could not be deleted: '
                         r'\S+work\.s: \[WinError 32\]', text), text
        assert re.search(r'PRISTINE TREE LEFT AT \S+tree: the build path still holds something rules did not delete',
                         text), text
        assert work.read_bytes() == b'BAD'  # the run's copy, left where it is
        assert (pristine_tree(tmp_path) / 'work.s').read_bytes() == b'GOOD'
    finally:
        put_back(check, tmp_path)
    assert work.read_bytes() == b'GOOD' and not pristine_dirs(tmp_path)


@pytest.mark.skipif(os.name != 'nt', reason='Windows sharing lock')
def test_rules_tree_that_cannot_be_moved_aside_stays(tmp_path, monkeypatch, capsys):
    # Round 11 point 2: a rename that fails (an open file in the tree) stops with exit 1, the tree where it was.
    check = load_check()
    obj, seen = fake_rules_tree(tmp_path, monkeypatch, check, needed_changes_shared)
    before = entries(tmp_path)
    with open(obj, 'rb'):
        status = check.rules(tmp_path, jobs=1)
    text = capsys.readouterr().out
    assert status == 1 and 'not run: the tree cannot be moved aside' in text and not seen['commands'], text
    assert entries(tmp_path) == before and not pristine_dirs(tmp_path), text


# Review round 9 rejection controls, ported. Each passed on
# 041be616b with rc 0 while leaving the tree or a later probe's input changed. In the two outside controls, objects/
# stands for the real tree's CMakeFiles/ctrport.dir and the write lands beside it; 041be616b backed up only the object
# directory, so c26rev4 pointed its object_root at objects/ there. Every run has a fresh copy now: no injection.

def test_c26rev4_r9_file_attribute_change_cannot_pass(tmp_path, monkeypatch, capsys):
    check = load_check()
    work = tmp_path / 'flags.keep'
    work.write_bytes(b'UNCHANGED BYTES')
    before = work.stat()

    def run(name, target, argv):
        os.chmod(work, stat.S_IREAD)
        return needed_changes_shared(name, target, argv)

    fake_rules_tree(tmp_path, monkeypatch, check, run)
    try:
        status = check.rules(tmp_path, jobs=16, expect_active=['needed_compile'])
        after = work.stat()
    finally:
        os.chmod(work, stat.S_IREAD | stat.S_IWRITE)
    text = capsys.readouterr().out
    assert status != 0 or (before.st_mode, getattr(before, 'st_file_attributes', None)) == (
        after.st_mode, getattr(after, 'st_file_attributes', None)), text
    assert status == 0, text  # each run's change goes with its copy


@pytest.mark.parametrize('change', ['new', 'removed'])
def test_c26rev4_r9_directory_change_cannot_pass(tmp_path, monkeypatch, capsys, change):
    check = load_check()
    d = tmp_path / 'empty-at-start'
    if change == 'removed':
        d.mkdir()

    def run(name, target, argv):
        if change == 'new':
            d.mkdir(exist_ok=True)
        elif d.exists():
            d.rmdir()
        return needed_changes_shared(name, target, argv)

    fake_rules_tree(tmp_path, monkeypatch, check, run)
    status = check.rules(tmp_path, jobs=16, expect_active=['needed_compile'])
    text = capsys.readouterr().out
    assert status != 0 or d.exists() == (change == 'removed'), text
    assert status == 0 and d.exists() == (change == 'removed'), text


def test_c26rev4_r9_same_size_mtime_work_file_does_not_contaminate_next_probe(tmp_path, monkeypatch, capsys):
    check = load_check()
    work = tmp_path / 'shared-work.s'
    work.write_bytes(b'GOOD')
    st = work.stat()
    dirty = []

    def write(b):
        work.write_bytes(b)
        os.utime(work, ns=(st.st_atime_ns, st.st_mtime_ns))

    def run(name, target, argv):
        if name == 'inert_compile.py':
            write(b'EVIL')
        else:
            if work.read_bytes() != b'GOOD':
                dirty.append(target.name)
                target.write_bytes(b'DIFF CAUSED BY PREVIOUS PROBE')
            write(b'GOOD')
        return subprocess.CompletedProcess(argv, 0, b'', b'')

    fake_rules_tree(tmp_path, monkeypatch, check, run)
    status = check.rules(tmp_path, jobs=16, expect_active=['needed_compile'])
    text = capsys.readouterr().out
    assert status != 0 or not dirty, ('ACTIVE was certified from a prior probe work-file mutation', text)
    assert not dirty, text  # each probe started from the tree rules found


def test_c26rev4_r9_outside_bytes_do_not_contaminate_next_probe(tmp_path, monkeypatch, capsys):
    check = load_check()
    (tmp_path / 'objects').mkdir()
    work = tmp_path / 'outside-generated.h'
    work.write_bytes(b'GOOD')
    dirty = []

    def run(name, target, argv):
        if name == 'inert_compile.py':
            work.write_bytes(b'CHANGED OUTSIDE OBJECT DIRECTORY')
        else:
            if work.read_bytes() != b'GOOD':
                dirty.append(target.name)
                target.write_bytes(b'DIFF CAUSED BY PREVIOUS PROBE')
            work.write_bytes(b'GOOD')
        return subprocess.CompletedProcess(argv, 0, b'', b'')

    fake_rules_tree(tmp_path, monkeypatch, check, run)
    status = check.rules(tmp_path, jobs=16, expect_active=['needed_compile'])
    text = capsys.readouterr().out
    assert status != 0 or not dirty, ('final equality hid cross-probe contamination', text)
    assert not dirty, text


def test_c26rev4_r9_outside_mtime_change_cannot_pass(tmp_path, monkeypatch, capsys):
    check = load_check()
    (tmp_path / 'objects').mkdir()
    work = tmp_path / 'generated-header.h'
    work.write_bytes(b'HEADER')
    st = work.stat()

    def run(name, target, argv):
        os.utime(work, ns=(st.st_atime_ns, st.st_mtime_ns + 10_000_000_000))
        return needed_changes_shared(name, target, argv)

    fake_rules_tree(tmp_path, monkeypatch, check, run)
    status = check.rules(tmp_path, jobs=1, expect_active=['needed_compile'])
    after = work.stat()
    text = capsys.readouterr().out
    assert status != 0 or st.st_mtime_ns == after.st_mtime_ns, text
    assert status == 0 and st.st_mtime_ns == after.st_mtime_ns, text


def test_c26rev4_r9_serial_runs_and_timestamp_putback(tmp_path, monkeypatch, capsys):
    check = load_check()
    work = tmp_path / 'work.s'
    work.write_bytes(b'ORIGINAL')
    before = work.stat()

    def run(name, target, argv):
        work.write_bytes(b'REWRITE BY PROBE')
        return needed_changes_shared(name, target, argv)

    obj, seen = fake_rules_tree(tmp_path, monkeypatch, check, run)
    assert check.rules(tmp_path, jobs=16, expect_active=['needed_compile']) == 0
    text = capsys.readouterr().out
    assert seen['max'] == 1 and work.read_bytes() == b'ORIGINAL' and work.stat().st_mtime_ns == before.st_mtime_ns
    assert 'TREE CHANGED' not in text


def test_c26rev4_r9_readonly_store_cleanup_is_not_silently_successful(tmp_path, monkeypatch, capsys):
    # c26rev4's control as written (beside the tree, round 11 keeps a pristine tree instead of a store): every run
    # makes every file it finds in rules' directory read-only. A pass must not leave that directory behind. Round 11:
    # copies take their attributes from the first manifest, not the pristine files, so the runs are clean; the
    # pristine tree comes back read-only, which the final check names (exit 1); its directory is removed all the same.
    check = load_check()
    stores = set()

    def run(name, target, argv):
        for store in tmp_path.parent.iterdir():
            if store.name.lower().startswith(('.rules-copy-', '.rules-pristine-')):
                stores.add(store)
                for p in store.rglob('*'):
                    if p.is_file():
                        os.chmod(p, stat.S_IREAD)
        return needed_changes_shared(name, target, argv)

    fake_rules_tree(tmp_path, monkeypatch, check, run)
    try:
        status = check.rules(tmp_path, jobs=1, expect_active=['needed_compile'])
        text = capsys.readouterr().out
        remaining = [p for p in stores if p.exists()]
        assert stores and (status != 0 or not remaining), (text, remaining)
        assert not remaining and status == 1 and 'RESTORE FAILED' not in text, (text, remaining)
        assert re.search(r'LAUNCHER needed_compile\.py\s+ACTIVE \(1 of 2', text), text
        assert re.search(r'TREE CHANGED \S+: 2 paths differ', text), text
        assert '  other.c.o: attributes changed' in text and '  shared.c.o: attributes changed' in text, text
    finally:
        for root in [tmp_path] + [s for s in stores if s.exists()]:
            for p in root.rglob('*'):
                if p.is_file():
                    os.chmod(p, stat.S_IREAD | stat.S_IWRITE)
        for root in stores:
            if root.exists():
                shutil.rmtree(root)


def test_rules_names_a_copy_it_cannot_delete_and_leaves_the_tree_aside(tmp_path, monkeypatch, capsys):
    # BF9-F4, round 11: a copy that cannot be deleted is named, nothing else runs, and the pristine tree stays aside
    # (its path printed) rather than be put back over a copy.
    check = load_check()
    obj, seen = fake_rules_tree(tmp_path, monkeypatch, check, needed_changes_shared)

    def refuse(root):
        raise check.NotRemoved('%s: held open: left in place' % root)

    monkeypatch.setattr(check, 'remove_tree', refuse)
    try:
        assert check.rules(tmp_path, jobs=1, expect_active=['needed_compile']) == 1
        text = capsys.readouterr().out
        assert re.search(r'NOT CLEAN after control of other\.c\.o: the copy could not be deleted: \S+: held open', text), \
            text
        assert len(seen['commands']) == 1 and text.count(' NOT RUN') == 3, (text, seen)
        assert re.search(r'PRISTINE TREE LEFT AT \S*\.rules-pristine-%s-\S+tree: ' % re.escape(tmp_path.name), text), \
            text
        assert re.search(r'TREE CHANGED \S+: 1 paths differ', text), text
        assert (pristine_tree(tmp_path) / 'README.txt').exists() is False
        readme = (pristine_tree(tmp_path).parent / 'README.txt').read_text()
        assert 'WITHOUT following links' in readme and str(tmp_path) in readme
    finally:
        monkeypatch.undo()
        put_back(check, tmp_path)
    assert obj.read_bytes() == b'BASE' and not pristine_dirs(tmp_path)


def test_rules_never_deletes_a_build_path_it_did_not_copy(tmp_path, monkeypatch, capsys):
    # Something makes the build path again after a copy was deleted (a late writer outside the job, another tool):
    # the next run does not copy over it or delete it. RESTORE FAILED, nothing else runs, it is left in place, and
    # the pristine tree stays aside.
    check = load_check()
    obj, seen = fake_rules_tree(tmp_path, monkeypatch, check, needed_changes_shared)
    real = check.remove_copy

    def then_a_late_writer(root, identity):
        real(root, identity)
        if not os.path.lexists(root):
            os.mkdir(root)
            (Path(root) / 'late.txt').write_bytes(b'NOT RULES')

    monkeypatch.setattr(check, 'remove_copy', then_a_late_writer)
    try:
        assert check.rules(tmp_path, jobs=1) == 1
        text = capsys.readouterr().out
        assert re.search(r'NOT CLEAN after other\.c\.o without inert_compile\.py: the build path is not free before the '
                         r'copy: something made \S+ after the last copy was deleted; left in place', text), text
        assert len(seen['commands']) == 1 and text.count(' NOT RUN') == 3, (text, seen)
        assert re.search(r'PRISTINE TREE LEFT AT \S+tree: the build path still holds something rules did not delete',
                         text), text
        assert 'RECOVER BY HAND: build path %s, pristine tree ' % tmp_path in text, text
        assert sorted(p.name for p in tmp_path.iterdir()) == ['late.txt'], text  # untouched
    finally:
        monkeypatch.undo()
        put_back(check, tmp_path)
    assert obj.read_bytes() == b'BASE' and not pristine_dirs(tmp_path)


def test_failed_copy_never_deletes_the_path_it_did_not_create(tmp_path, monkeypatch, capsys):
    # c26rev4 BF11-F1(a), their test as written: something else makes the build path after rules saw it free and
    # before copy_tree's mkdir. Only a directory rules' own exclusive mkdir made is ever deleted: rules made none, so
    # the foreign directory stays, no command runs, and the pristine tree stays aside.
    check = load_check()
    build = tmp_path / 'tree'
    build.mkdir()
    obj, seen = fake_rules_tree(build, monkeypatch, check, needed_changes_shared)
    real = check.copy_tree
    keep = build / 'foreign.txt'
    called = []

    def racing_copy(source, dest, base):
        if not called:
            called.append(True)
            os.mkdir(dest)
            keep.write_bytes(b'FOREIGN PATH WON THE MKDIR RACE')
        return real(source, dest, base)

    monkeypatch.setattr(check, 'copy_tree', racing_copy)
    try:
        rc = check.rules(build, expect_active=['needed_compile'])
        text = capsys.readouterr().out
        assert rc == 1 and not seen['commands'], (text, seen)
        assert keep.read_bytes() == b'FOREIGN PATH WON THE MKDIR RACE', text
        assert sorted(p.name for p in build.iterdir()) == ['foreign.txt'], text
        assert re.search(r'NOT CLEAN after control of \S+: the build path is not free before the copy', text), text
        assert re.search(r'PRISTINE TREE LEFT AT \S+tree: the build path still holds something', text), text
        assert 'RECOVER BY HAND: build path %s, pristine tree %s' % (build, pristine_tree(build)) in text, text
    finally:
        monkeypatch.undo()
        put_back(check, build)
    assert obj.read_bytes() == b'BASE' and not pristine_dirs(build)


def test_busy_retry_does_not_adopt_a_replacement_tree(tmp_path, monkeypatch, capsys):
    # c26rev4 BF11-F1(b), their test as written: the first delete attempt is busy, and before the retry the copy is
    # renamed away and another directory made at the build path. The retry reads the path's identity again, finds a
    # different directory and deletes nothing: RESTORE FAILED, both paths printed, the pristine tree stays aside.
    check = load_check()
    build = tmp_path / 'tree'
    build.mkdir()
    obj, seen = fake_rules_tree(build, monkeypatch, check, needed_changes_shared)
    real = check.remove_tree
    parked = tmp_path / 'held-own-copy'
    keep = build / 'foreign.txt'
    swapped = []

    def race(path):
        if Path(path) == build and not swapped:
            swapped.append(True)
            os.rename(build, parked)
            os.mkdir(build)
            keep.write_bytes(b'FOREIGN REPLACEMENT DURING BUSY GRACE')
            raise check.NotRemoved('review sharing-violation interleaving', busy=True)
        return real(path)

    monkeypatch.setattr(check, 'remove_tree', race)
    try:
        rc = check.rules(build, expect_active=['needed_compile'])
        text = capsys.readouterr().out
        assert rc == 1, text
        assert keep.read_bytes() == b'FOREIGN REPLACEMENT DURING BUSY GRACE', text
        assert sorted(p.name for p in build.iterdir()) == ['foreign.txt'], text
        assert (parked / 'shared.c.o').exists(), text  # the run's own copy, wherever it went, is not deleted either
        assert len(seen['commands']) == 1 and text.count(' NOT RUN') == 3, (text, seen)
        assert re.search(r'NOT CLEAN after control of \S+: the copy could not be deleted: %s no longer holds this '
                         r'run\'s copy \(it was volume \w+ file \w+; the path now holds volume \w+ file \w+\): nothing '
                         r'deleted' % re.escape(str(build)), text), text
        assert re.search(r'PRISTINE TREE LEFT AT \S+tree: %s no longer holds this run\'s copy' % re.escape(str(build)),
                         text), text
        assert 'RECOVER BY HAND: build path %s, pristine tree %s' % (build, pristine_tree(build)) in text, text
    finally:
        monkeypatch.undo()
        put_back(check, build)
        if parked.exists():
            check.remove_tree(parked)
    assert obj.read_bytes() == b'BASE' and not pristine_dirs(build)


@pytest.mark.parametrize('how', ['replaced', 'removed'])
def test_copy_replaced_before_the_first_delete_is_not_deleted(tmp_path, monkeypatch, capsys, how):
    # BF11-F1, an added case: after the copy was made and before the first delete attempt, the run renames
    # its copy away and puts another directory at the build path (or leaves nothing there). The first attempt reads
    # the identity, finds it changed, and deletes nothing; RESTORE FAILED, and the pristine tree stays aside even
    # when the build path is free, since where the copy went is not known.
    check = load_check()
    build = tmp_path / 'tree'
    build.mkdir()
    parked = tmp_path / 'moved-copy'
    tried = []
    real = check.remove_tree
    monkeypatch.setattr(check, 'remove_tree', lambda root: tried.append(Path(root)) or real(root))

    def run(name, target, argv):
        if not parked.exists():
            tried.clear()  # the deletes of earlier runs' copies
            os.rename(build, parked)
            if how == 'replaced':
                os.mkdir(build)
                (build / 'foreign.txt').write_bytes(b'FOREIGN')
        return subprocess.CompletedProcess(argv, 0, b'', b'')

    obj, seen = fake_rules_tree(build, monkeypatch, check, run)
    try:
        rc = check.rules(build)
        text = capsys.readouterr().out
        assert rc == 1, text
        # The first control (the fake's run() is not called for controls), then the probe that swaps the copy.
        assert len(seen['commands']) == 2 and text.count(' NOT RUN') == 3, (text, seen)
        assert build not in tried, tried  # not one delete attempt at the build path after the swap
        if how == 'replaced':
            assert sorted(p.name for p in build.iterdir()) == ['foreign.txt'], text
            assert re.search(r'the path now holds volume \w+ file \w+\): nothing deleted', text), text
        else:
            assert not os.path.lexists(build), text
            assert 'the path now holds nothing readable (FileNotFoundError)): nothing deleted' in text, text
        assert (parked / 'shared.c.o').exists(), text
        assert re.search(r'PRISTINE TREE LEFT AT \S+tree: %s no longer holds this run\'s copy' % re.escape(str(build)),
                         text), text
        assert 'RECOVER BY HAND: build path %s, pristine tree %s' % (build, pristine_tree(build)) in text, text
    finally:
        monkeypatch.undo()
        put_back(check, build)
        check.remove_tree(parked)
    assert obj.read_bytes() == b'BASE' and not pristine_dirs(build)


@pytest.mark.parametrize('how', ['unknown', 'raises'])
def test_copy_is_not_deleted_when_job_emptiness_is_unknown(tmp_path, monkeypatch, capsys, how):
    # c26rev4 BF11-F2, their test as written ('unknown'), plus contained() itself raising: whether the job is empty
    # is not known, so the copy is not deleted and the pristine tree is not moved back. Both are printed, exit 1.
    check = load_check()
    build = tmp_path / 'tree'
    build.mkdir()
    obj, _seen = fake_rules_tree(build, monkeypatch, check, needed_changes_shared)

    def uncertain(argv, cwd, env, timeout):
        if how == 'raises':
            raise OSError('review: the job could not be queried')
        return (0, 'writers may be alive: review query failure', False)

    monkeypatch.setattr(check, 'contained', uncertain)
    removed = []
    real = check.remove_copy

    def remove(path, identity):
        removed.append(str(path))
        return real(path, identity)

    monkeypatch.setattr(check, 'remove_copy', remove)
    try:
        rc = check.rules(build, expect_active=['needed_compile'])
        text = capsys.readouterr().out
        assert rc == 1 and 'not probed' in text, text
        assert not removed, 'copy deletion ran before the job was known empty'
        assert re.search(r'ERROR \(control: (writers may be alive: review query failure|OSError: review: the job '
                         r'could not be queried \(writers may be alive)', text), text
        assert 'WRITERS MAY BE ALIVE' in text and text.count(' NOT RUN') == 3, text
        assert (build / 'shared.c.o').exists() and (pristine_tree(build) / 'shared.c.o').exists(), text
        assert re.search(r'PRISTINE TREE LEFT AT \S+tree: writers may be alive in the run\'s copy at %s ' %
                         re.escape(str(build)), text), text
        assert 'RECOVER BY HAND: build path %s, pristine tree %s' % (build, pristine_tree(build)) in text, text
    finally:
        monkeypatch.undo()
        put_back(check, build)
    assert obj.read_bytes() == b'BASE' and not pristine_dirs(build)


def test_rules_receipt_reads_markers_where_rules_prints_them(tmp_path, monkeypatch, capsys):
    # Gate iii (r12): the receipt driver matched "NOT CLEAN" anywhere and failed a clean run on rules' own summary
    # line. rules_receipt_problems reads each marker where rules prints it: a clean run's real output has none, a real
    # "NOT CLEAN after" line still counts, and so does every other failure rules can print.
    check = load_check()
    build = tmp_path / 'tree'
    build.mkdir()

    def touches(name, target, argv):
        (target.parent / 'work.s').write_bytes(b'LAUNCHER WORK FILE')  # a TOUCHED OUTSIDE line and its summary
        return needed_changes_shared(name, target, argv)

    fake_rules_tree(build, monkeypatch, check, touches)
    assert check.rules(build, jobs=1, expect_active=['needed_compile']) == 0
    clean = capsys.readouterr().out
    summary = [l for l in clean.splitlines() if l.startswith('TOUCHED OUTSIDE: ')]
    assert summary and 'NOT CLEAN' in summary[0], clean  # the line that tripped the gate iii driver
    assert check.rules_receipt_problems(clean, ['needed_compile']) == [], clean
    assert check.rules_receipt_problems(clean, ['needed_compile'], tree='tree') == [], clean
    assert check.rules_receipt_problems(clean, ['needed_compile', 'inert_compile']) == [
        'inert_compile.py is not ACTIVE: inactive in this build (2 named sources unchanged without it)']
    real = 'NOT CLEAN after control of shared.c.o: the copy could not be deleted: held open'
    found = check.rules_receipt_problems(clean + real + '\n', ['needed_compile'])
    assert found[0].startswith("1 lines start with 'NOT CLEAN after '"), found
    for line, why in [('PRISTINE TREE LEFT AT C:\\x\\tree: writers may be alive', 'PRISTINE TREE LEFT AT '),
                      ('RECOVER BY HAND: build path C:\\x, pristine tree C:\\y\\tree.', 'RECOVER BY HAND: '),
                      ('EXPECTED ACTIVE needed_compile.py: not probed', 'EXPECTED ACTIVE '),
                      ('TREE CHANGED tree: 1 paths differ', 'TREE CHANGED '),
                      ('LEFT: C:\\x (the tree is back)', 'LEFT: '),
                      ('RULES tree: not run: an earlier rules run left x', 'not run:')]:
        assert any(why in p for p in check.rules_receipt_problems(clean + line + '\n', ['needed_compile'])), line
    # And from a real failing run: writers may be alive, the pristine tree left aside.
    check2 = load_check()
    build2 = tmp_path / 'tree2'
    build2.mkdir()

    def uncertain(name, target, argv):
        return None, 'writers may be alive: the process group was not seen empty 30 s after the kill', False

    fake_rules_tree(build2, monkeypatch, check2, uncertain)
    try:
        assert check2.rules(build2, jobs=1, expect_active=['needed_compile']) == 1
        failed = capsys.readouterr().out
        found = check2.rules_receipt_problems(failed, ['needed_compile'])
        for why in ('PRISTINE TREE LEFT AT ', 'RECOVER BY HAND: ', 'EXPECTED ACTIVE ', 'TREE CHANGED ',
                    'checks errored or did not end clean', 'needed_compile.py is not ACTIVE'):
            assert any(why in p for p in found), (why, found)
    finally:
        monkeypatch.undo()
        put_back(check2, build2)


def test_rules_receipt_needs_one_complete_reconciled_run(tmp_path, monkeypatch, capsys):
    # c26rev4 NO-GO on 340489297 (BF-RP-F1/F2): the receipt passed with expected check lines missing, ACTIVE counts
    # no check line supports, summaries missing, malformed or repeated, and decorated or interleaved lines. Each edit
    # of a real clean run below must be rejected; the run itself, LF or CRLF, passes.
    check = load_check()
    build = tmp_path / 'tree'
    build.mkdir()

    def touches(name, target, argv):
        (target.parent / 'work.s').write_bytes(b'LAUNCHER WORK FILE')
        return needed_changes_shared(name, target, argv)

    fake_rules_tree(build, monkeypatch, check, touches)
    try:
        assert check.rules(build, jobs=1, expect_active=['needed_compile']) == 0
    finally:
        monkeypatch.undo()
    clean = capsys.readouterr().out
    lines = clean.splitlines()
    want = ['needed_compile']
    assert check.rules_receipt_problems(clean, want, tree='tree') == [], clean
    assert check.rules_receipt_problems(clean.replace('\n', '\r\n'), want, tree='tree') == [], clean
    checks = [i for i, l in enumerate(lines) if re.fullmatch(r'\S+\.py +\S+ +\S+ +(RED \(.+\)|same without it) \d+ s', l)]
    red = [i for i in checks if 'RED (' in lines[i]]
    touched = [i for i, l in enumerate(lines) if l.startswith('TOUCHED OUTSIDE ') and ' paths, gone with the copy: ' in l]
    at = {k: next(i for i, l in enumerate(lines) if l.startswith(k)) for k in ('RULES ', 'TOUCHED OUTSIDE: ', 'TREE ')}
    launcher = [i for i, l in enumerate(lines) if l.startswith('LAUNCHER ')]
    assert len(checks) == 4 and len(red) == 1 and 'needed_compile.py' in lines[red[0]], clean
    assert touched and len(launcher) == 2 and at['TREE '] == len(lines) - 1, clean

    def edit(i, new=None, before=None):
        out = list(lines)
        if before is not None:
            out.insert(i, before)
        elif new is None:
            del out[i]
        else:
            out[i] = new
        return '\n'.join(out) + '\n'

    def moved(i, j):  # line i moved to just before line j
        out = list(lines)
        out.insert(j, out[i])
        del out[i + (1 if j <= i else 0)]
        return '\n'.join(out) + '\n'

    needed_same = [i for i in checks if 'needed_compile.py' in lines[i] and i not in red][0]
    tree_line = lines[at['TREE ']]
    cases = {
        # BF-RP-F1: an expected ACTIVE with its check lines missing or not RED.
        'the RED check line missing': edit(red[0]),
        'the other check line of the ACTIVE launcher missing': edit(needed_same),
        'RED made same': edit(red[0], re.sub(r'RED \(.+\)', 'same without it', lines[red[0]])),
        'ACTIVE count raised': clean.replace('ACTIVE (1 of 2', 'ACTIVE (2 of 2'),
        'ACTIVE out of more sources': clean.replace('ACTIVE (1 of 2', 'ACTIVE (1 of 3'),
        'ACTIVE (0 of 2)': clean.replace('ACTIVE (1 of 2', 'ACTIVE (0 of 2'),
        'all check lines missing': '\n'.join(l for i, l in enumerate(lines) if i not in checks and i not in touched)
        + '\n',
        # BF-RP-F2: one complete envelope.
        'RULES summary missing': edit(at['RULES ']),
        'RULES summary says one more check': edit(at['RULES '], lines[at['RULES ']].replace(' 4 checks', ' 5 checks')),
        'RULES summary says one more launcher': edit(at['RULES '],
                                                     lines[at['RULES ']].replace(' 2 launchers', ' 3 launchers')),
        'RULES summary of another tree': edit(at['RULES '], lines[at['RULES ']].replace('RULES tree:', 'RULES t2:')),
        'TREE line missing': edit(at['TREE ']),
        'TREE line of another tree': edit(at['TREE '], tree_line.replace('TREE tree:', 'TREE t2:')),
        'TREE line cut short': edit(at['TREE '], tree_line.split(' to the tree')[0]),
        'TREE line with a suffix': edit(at['TREE '], tree_line + ' (but see below)'),
        'a line after TREE': clean + 'GATE3 PASS\n',
        'TOUCHED OUTSIDE summary missing': edit(at['TOUCHED OUTSIDE: ']),
        'TOUCHED OUTSIDE summary count off': edit(at['TOUCHED OUTSIDE: '], re.sub(
            r'TOUCHED OUTSIDE: \d+ runs', 'TOUCHED OUTSIDE: 99 runs', lines[at['TOUCHED OUTSIDE: ']])),
        'a TOUCHED OUTSIDE line missing': edit(touched[0]),
        'a TOUCHED OUTSIDE line counting a path it does not list': edit(touched[0], re.sub(
            r': (\d+) paths, ', lambda m: ': %d paths, ' % (int(m.group(1)) + 1), lines[touched[0]], count=1)),
        'a launcher line missing': edit(launcher[0]),
        'a launcher line twice': edit(launcher[0], before=lines[launcher[0]]),
        'a launcher line not probed': edit(launcher[0], re.sub(r'(LAUNCHER \S+ +).*', r'\1not probed (1 of 2 checks '
                                                               r'errored: shared.c)', lines[launcher[0]])),
        'a check line of a launcher not in the chain': edit(checks[0], lines[checks[0]].replace(
            lines[checks[0]].split()[0], 'other_compile.py', 1), before=None),
        'two runs joined': clean + clean,
        'two summaries, one run': clean + '\n'.join(lines[at['RULES ']:]) + '\n',
        'a check line after the summary': clean.replace(lines[red[0]] + '\n', '') + lines[red[0]] + '\n',
        'a TOUCHED OUTSIDE line with no check line after it': moved(touched[-1], at['RULES ']),
        'a TOUCHED OUTSIDE line before another object\'s check': moved(touched[0], checks[-1]),
        # Item 3: decorated, indented or interleaved lines are rejected, not read around.
        'a job prefix on a check line': edit(red[0], '[job-1] ' + lines[red[0]]),
        'a job prefix on the TREE line': edit(at['TREE '], '[job-1] ' + tree_line),
        'an indented LAUNCHER line': edit(launcher[0], '  ' + lines[launcher[0]]),
        'an empty line': edit(at['TREE '], before=''),
        'a driver line in the stream': edit(0, before='MANIFEST BEFORE 1041 entries (2 s), root identity 1/2'),
        'a failure joined to a progress line': edit(checks[0], before='waiting... NOT CLEAN after control of '
                                                                      'shared.c.o: held open'),
        'a check line with a stray suffix': edit(red[0], lines[red[0]] + ' (retried)'),
    }
    for why, text in cases.items():
        assert text != clean, why
        assert check.rules_receipt_problems(text, want, tree='tree'), (why, text)
    assert check.rules_receipt_problems(clean, want, tree='t2') == ['the run is of tree, not t2']
    assert check.rules_receipt_problems(clean, ['needed_compile', 'needed_compile.py'])  # a name twice
    assert check.rules_receipt_problems(clean, [])  # nothing expected is not a receipt
    # Item 4, the twins: a launcher NAMED like a marker is read by the line's format, not by its words.
    for twin in ('NOT_CLEAN_compile.py', 'CLEAN_compile.py'):
        renamed = clean.replace('needed_compile.py', twin)
        assert check.rules_receipt_problems(renamed, [twin], tree='tree') == [], renamed
        twin_lines = renamed.splitlines()
        errored = list(twin_lines)
        errored[red[0]] = re.sub(r'RED \(.+\)', 'ERROR (control: the full line fails, status 1)', errored[red[0]])
        found = check.rules_receipt_problems('\n'.join(errored) + '\n', [twin], tree='tree')
        assert any('checks errored or did not end clean' in p for p in found), found
        unclean = list(twin_lines)
        unclean[red[0]] += ' RESTORE FAILED'
        found = check.rules_receipt_problems('\n'.join(unclean) + '\n', [twin], tree='tree')
        assert any('checks errored or did not end clean' in p for p in found), found


D = 'CMakeFiles/ctrport.dir/source/'
# A clean receipt in rules' own formats, written by hand (not by the code under test), shaped like gate (iii)'s: five
# launchers in chain order, eight objects in eight groups printed in sorted order, two launchers on one object, a
# launcher with several sources, a sole RED, both RED forms (a negative status too) and two TOUCHED OUTSIDE lines.
RP_CHECKS = [('alpha_compile.py', 'a.c', 'a.o', 'RED (differs without it: a.o)'),
             ('gamma_compile.py', 'a.c', 'a.o', 'same without it'),
             ('alpha_compile.py', 'b.c', 'b.o', 'RED (fails without it: status 1)'),
             ('alpha_compile.py', 'c.c', 'c.o', 'same without it'),
             ('beta_compile.py', 'd.c', 'd.o', 'RED (differs without it: d.json, d.o)'),
             ('delta_compile.py', 'e.c', 'e.o', 'RED (fails without it: status -1073741819)'),
             ('delta_compile.py', 'f.c', 'f.o', 'RED (differs without it: f.o)'),
             ('gamma_compile.py', 'g.c', 'g.o', 'RED (differs without it: g.o)'),
             ('gamma_compile.py', 'h.c', 'h.o', 'same without it')]
RP_STATES = [('alpha_compile.py', 'ACTIVE (2 of 3 named sources change without it)'),
             ('beta_compile.py', 'ACTIVE (1 of 1 named sources change without it)'),
             ('gamma_compile.py', 'ACTIVE (1 of 3 named sources change without it)'),
             ('delta_compile.py', 'ACTIVE (2 of 2 named sources change without it)'),
             ('compile_cache.py', 'no named source in this build')]
RP_EXPECTED = ['alpha_compile', 'beta_compile', 'gamma_compile', 'delta_compile']


def rp_check_line(name, source, obj, verdict, seconds=12):
    return '%-34s %-28s %-60s %s %.0f s' % (name, source, D + obj, verdict, seconds)


def rp_receipt(checks=RP_CHECKS, states=RP_STATES, groups=8):
    out, done = [], set()
    for name, source, obj, verdict in checks:
        first = (name, obj) not in done  # a repeated line repeats only the check line (the case under test)
        done.add((name, obj))
        if (name, obj) == ('alpha_compile.py', 'a.o') and first:
            out.append('TOUCHED OUTSIDE control of %sa.o: 1 paths, gone with the copy: %sa.json: mtime changed' % (D, D))
        if (name, obj) == ('gamma_compile.py', 'g.o') and first:
            out.append('TOUCHED OUTSIDE %sg.o without gamma_compile.py: 2 paths, gone with the copy: %sg0.s: mtime '
                       'changed; %sg.json: size, mtime, bytes changed' % (D, D, D))
        out.append(rp_check_line(name, source, obj, verdict))
    out.append('RULES tree: %d launchers in the chain, %d checks in %d groups that share no file' % (
        len(states), len(checks), groups))
    out += ['LAUNCHER %-34s %s' % s for s in states]
    out.append("TOUCHED OUTSIDE: 2 runs left paths besides their own object's in their copy (deleted with it unless "
               "named NOT CLEAN)")
    out.append('TREE tree: all 40 directories and files equal to the tree rules found (bytes, sizes, mtimes, '
               'attributes)')
    return '\n'.join(out) + '\n'


def rp_state(name, state):
    return [(n, state if n == name else s) for n, s in RP_STATES]


def test_rules_receipt_rejects_repeated_evidence_malformed_red_and_group_disorder():
    # c26rev4 NO-GO on 365af239a: a repeated check stood in for a missing source (BF-RP2-F1), any text inside
    # RED (...) earned ACTIVE credit (BF-RP2-F2), and the group count and order were never checked (BF-RP2-N1). Each
    # case below returned [] there; each must be rejected, and the clean twins must pass.
    check = load_check()

    def problems(checks=RP_CHECKS, states=RP_STATES, groups=8):
        return check.rules_receipt_problems(rp_receipt(checks, states, groups), RP_EXPECTED, tree='tree')

    assert problems() == [], rp_receipt()
    assert check.rules_receipt_problems(rp_receipt().replace('\n', '\r\n'), RP_EXPECTED, tree='tree') == []

    def swap(i, new):
        out = list(RP_CHECKS)
        out[i] = new
        return out

    def has(found, words):
        assert any(words in p for p in found), (words, found)

    # BF-RP2-F1: alpha's b.o RED replaced by a second copy of its a.o RED (counts and 2 of 3 unchanged).
    has(problems(swap(2, RP_CHECKS[0])), 'appear more than once')
    # ... and gamma's two "same" replaced by copies of its sole RED, its line raised to 3 of 3.
    gamma3 = [RP_CHECKS[7] if c[0] == 'gamma_compile.py' and c[3] == 'same without it' else c for c in RP_CHECKS]
    has(problems(gamma3, rp_state('gamma_compile.py', 'ACTIVE (3 of 3 named sources change without it)')),
        'appear more than once')
    # One object named with two sources.
    has(problems(swap(1, ('gamma_compile.py', 'z.c', 'a.o', 'same without it'))), 'more than one source')
    # BF-RP2-F2: beta's sole RED (1 of 1) with a body rules never prints.
    for body in ('RED (unrecognized verdict)', 'RED (same without it)', 'RED (fails without it: status 0)',
                 'RED (fails without it: status -0)', 'RED (fails without it: status 01)',
                 'RED (ERROR (control: full line failed))', 'RED (differs without it: )',
                 'RED (differs without it: d.o, d.json)', 'RED (differs without it: d.o, d.o)',
                 'RED (differs without it: d.o,d.json)', 'RED (differs without it:  d.o)'):
        has(problems(swap(4, ('beta_compile.py', 'd.c', 'd.o', body))), 'RED verdict rules never prints')
    # BF-RP2-N1: the group count off, two complete groups swapped, a closed group reopened, a group out of chain order.
    for groups in (0, 7, 9, 999):
        has(problems(groups=groups), 'the check lines form 8 groups, the RULES line says %d' % groups)
    e_f = list(RP_CHECKS)
    e_f[5], e_f[6] = RP_CHECKS[6], RP_CHECKS[5]
    has(problems(e_f), "not in rules' group order")
    reopened = RP_CHECKS[:1] + RP_CHECKS[2:3] + RP_CHECKS[1:2] + RP_CHECKS[3:]
    has(problems(reopened), "not in rules' group order")
    chain = [RP_CHECKS[1], RP_CHECKS[0]] + RP_CHECKS[2:]
    has(problems(chain), 'not in chain order')
    # Twins that must stay accepted: a negative status, several differing files, two launchers on one object.
    assert problems(swap(4, ('beta_compile.py', 'd.c', 'd.o', 'RED (fails without it: status -1)'))) == []
    assert problems(swap(4, ('beta_compile.py', 'd.c', 'd.o', 'RED (differs without it: d 0.s, d.json, d.o)'))) == []


def test_remove_copy_reads_the_identity_before_every_attempt(tmp_path, monkeypatch):
    # remove_copy itself: a path with another identity is never deleted, on the first attempt or a busy retry; the
    # directory it was given is deleted after a busy attempt.
    check = load_check()
    a, b = tmp_path / 'a', tmp_path / 'b'
    a.mkdir()
    (a / 'f').write_bytes(b'A')
    b.mkdir()
    with pytest.raises(check.NotRemoved) as e:
        check.remove_copy(a, check.path_identity(b))
    assert e.value.foreign and (a / 'f').read_bytes() == b'A'
    real, calls = check.remove_tree, []

    def busy_once(root):
        if Path(root) == a:  # remove_tree's own calls for the entries under it pass straight through
            calls.append(check.path_identity(root))
            if len(calls) == 1:
                raise check.NotRemoved('busy', busy=True)
        return real(root)

    monkeypatch.setattr(check, 'remove_tree', busy_once)
    identity = check.path_identity(a)
    check.remove_copy(a, identity)
    assert calls == [identity, identity] and not os.path.lexists(a) and b.is_dir()


@pytest.mark.skipif(os.name != 'nt', reason='NTFS junctions')
def test_remove_tree_takes_links_as_links(tmp_path):
    # Python 3.14 on this machine, not the docs: os.lstat flags a junction (0x400), and os.rmdir of the junction
    # removes the link only. remove_tree deletes the junction, a directory symlink would go the same way, and a
    # hard-linked file is unlinked as it is.
    check = load_check()
    foreign = tmp_path / 'foreign'
    foreign.mkdir()
    (foreign / 'keep').write_bytes(b'KEEP')
    tree = tmp_path / 'tree'
    (tree / 'd').mkdir(parents=True)
    make_junction(tree / 'd' / 'j', foreign)
    os.link(foreign / 'keep', tree / 'alias')
    (tree / 'ro').write_bytes(b'RO')
    os.chmod(tree / 'ro', stat.S_IREAD)
    assert os.lstat(tree / 'd' / 'j').st_file_attributes & check.REPARSE_POINT
    assert [w for _r, _p, _s, w in check.walk_entries(tree, strict=False) if w] == [
        'a file with 2 hard links', 'a reparse point (junction or link)']
    with pytest.raises(ValueError, match='hard links'):
        check.tree_manifest(tree)
    check.remove_tree(tree)
    assert not os.path.lexists(tree) and (foreign / 'keep').read_bytes() == b'KEEP'
    assert os.stat(foreign / 'keep').st_nlink == 1


def test_copy_tree_matches_the_manifest(tmp_path):
    check = load_check()
    src = tmp_path / 'src'
    (src / 'a' / 'b').mkdir(parents=True)
    (src / 'a' / 'b' / 'f').write_bytes(b'F')
    (src / 'top').write_bytes(b'T')
    os.chmod(src / 'top', stat.S_IREAD)
    os.utime(src / 'a', ns=(1, 1_000_000_000_000_000_000))
    base = check.tree_manifest(src)
    try:
        made = check.copy_tree(src, tmp_path / 'dst', base)
        assert check.tree_manifest(tmp_path / 'dst') == base
        assert made == check.path_identity(tmp_path / 'dst') and all(made), made
        with pytest.raises(check.CopyNotMade) as e:  # an exclusive mkdir: never into something already there
            check.copy_tree(src, tmp_path / 'dst', base)
        assert e.value.made is None
    finally:
        for root in (src, tmp_path / 'dst'):
            if (root / 'top').exists():
                os.chmod(root / 'top', stat.S_IREAD | stat.S_IWRITE)


def test_rules_long_paths_are_copied_or_refused(tmp_path, monkeypatch, capsys):
    # c26rev4's long-path control: a deep tree is either copied and run exactly, or refused before anything runs.
    check = load_check()
    deep = tmp_path
    for i in range(6):
        deep = deep / ('long-component-%d%s' % (i, 'x' * 32))
    deep.mkdir(parents=True)
    (deep / 'work.s').write_bytes(b'GOOD')

    def run(name, target, argv):
        (deep / 'work.s').write_bytes(b'BAD')
        return needed_changes_shared(name, target, argv)

    obj, seen = fake_rules_tree(tmp_path, monkeypatch, check, run)
    before = entries(tmp_path)
    status = check.rules(tmp_path, jobs=1, expect_active=['needed_compile'])
    text = capsys.readouterr().out
    print('LONG PATH', len(str(deep / 'work.s')), status)
    assert status in (0, 2) and entries(tmp_path) == before and not pristine_dirs(tmp_path), text
    assert status == 0 or not seen['commands'], text


def test_tree_manifest_and_differences(tmp_path):
    check = load_check()
    (tmp_path / 'd').mkdir()
    (tmp_path / 'd' / 'f').write_bytes(b'x')
    base = check.tree_manifest(tmp_path)
    assert set(base) == {'.', 'd', 'd/f'} and base['d'][0] == 'd' and base['d/f'][0] == 'f'
    (tmp_path / 'd' / 'f').write_bytes(b'y')
    st = os.stat(tmp_path / 'd' / 'f')
    os.utime(tmp_path / 'd' / 'f', ns=(st.st_atime_ns, base['d/f'][2]))
    assert check.manifest_differences(base, check.tree_manifest(tmp_path)) == ['d/f: bytes changed']


# The real witness (c26rev4 BF7-F1, reproduced there with devkitPro's sh): the launcher starts a child with its
# output handles closed and exits (or hangs past the timeout). The child outlives the shell and keeps writing the
# object. The checker must kill it with the shell's job before it restores, so the object stays the build's.
WITNESS_LAUNCHER = '''
import os, subprocess, sys, time
mode, started, target = sys.argv[1], sys.argv[2], sys.argv[sys.argv.index('-o') + 1]
subprocess.Popen([sys.executable, '-I', '../late_writer.py', started, target], stdin=subprocess.DEVNULL,
                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True)
deadline = time.monotonic() + 60
while not os.path.exists(started) and time.monotonic() < deadline:
    time.sleep(0.05)
with open(target, 'wb') as f:
    f.write(b'PROBE')
if mode == 'hang':
    time.sleep(120)
'''
LATE_WRITER = '''
import sys, time
open(sys.argv[1], 'w').close()
sys.argv.pop(1)
end = time.monotonic() + 45
while time.monotonic() < end:
    time.sleep(0.2)
    try:
        with open(sys.argv[1], 'wb') as f:
            f.write(b'LATE AFTER RESTORE')
    except OSError:
        pass
'''


def witness_rules_tree(tmp_path, monkeypatch, check, mode):
    """The build tree is tmp_path/tree; the scripts and the writer's start marker live beside it, outside the tree."""
    sh = devkit('msys2/usr/bin/sh') if os.name == 'nt' else Path(shutil.which('sh') or '/bin/sh')
    py = Path(sys.executable).as_posix()
    build = tmp_path / 'tree'
    build.mkdir()
    (build / 'shared.c.o').write_bytes(b'BASE')
    (tmp_path / 'control.py').write_text("import sys\nopen(sys.argv[sys.argv.index('-o') + 1], 'wb').write(b'BASE')\n")
    (tmp_path / 'witness.py').write_text(WITNESS_LAUNCHER)
    (tmp_path / 'late_writer.py').write_text(LATE_WRITER)
    started = (tmp_path / 'writer.started').as_posix()
    line = '%s ../control.py arm-none-eabi-gcc -c shared.c -o shared.c.o' % shlex.quote(py)
    monkeypatch.setattr(check, 'compile_lines', lambda b: [('shared.c.o', 'shared.c', line)])
    monkeypatch.setattr(check, 'chain_of', lambda b: type('C', (), {'levels': [str(tmp_path / 'witness.py')]})())
    monkeypatch.setattr(check, 'shell_for', lambda b: str(sh))
    monkeypatch.setattr(check, 'launcher_marker', lambda s: '--witness')
    monkeypatch.setattr(check, 'rule_sources', lambda s: {'shared.c'})
    monkeypatch.setattr(check, 'without', lambda tokens, name, marker: [py, '-I', '../witness.py', mode, started])
    return build


@pytest.mark.parametrize('mode, why', [('exit', 'a process the command started outlived it'),
                                       ('hang', 'TimeoutExpired: no exit in 20 s')])
def test_c26rev4_late_writer_cannot_outlive_the_probe(tmp_path, monkeypatch, capsys, mode, why):
    check = load_check()
    build = witness_rules_tree(tmp_path, monkeypatch, check, mode)
    obj = build / 'shared.c.o'
    monkeypatch.setattr(check, 'PROBE_TIMEOUT', 20)
    status = check.rules(build, jobs=1, expect_active=['witness'])
    text = capsys.readouterr().out
    assert (tmp_path / 'writer.started').exists(), text  # the late writer really ran, inside the probe
    time.sleep(1.5)  # a live writer rewrites the object every 0.2 s
    assert obj.read_bytes() == b'BASE', text
    assert status == 1 and 'ERROR (%s' % why in text and 'ACTIVE (' not in text, text
    assert re.search(r'LAUNCHER witness\.py\s+not probed', text), text
    assert 'RESTORE FAILED' not in text and 'WRITERS MAY BE ALIVE' not in text, text  # killed, then restored
    assert re.search(r'TREE tree: all 2 directories and files equal', text), text


def test_rules_expand_make_variables_from_flags_make(tmp_path, monkeypatch, capsys):
    # The empty-macro case: build.make's compile lines carry $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS), which make fills
    # from flags.make. The fake compiler fails unless they arrive.
    check = load_check()

    def needs_flags(target, argv):
        if '-DREAL -Iinc -O2' not in argv[2]:
            return subprocess.CompletedProcess(argv, 1, b'', b'fatal error: 3ds.h: No such file or directory')
        target.write_bytes({'shared.c.o': b'BASE', 'other.c.o': b'OTHER'}[target.name])
        return subprocess.CompletedProcess(argv, 0, b'', b'')

    flags = '# CMAKE generated file\nC_DEFINES = -DREAL\n\nC_INCLUDES = -Iinc\n\nC_FLAGS = -O2\n'
    fake_rules_tree(tmp_path, monkeypatch, check, needed_changes_shared, control=needs_flags, flags=flags,
                    extra='$(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) ')
    assert check.rules(tmp_path, jobs=2, expect_active=['needed_compile']) == 0
    text = capsys.readouterr().out
    assert 'ERROR' not in text and re.search(r'LAUNCHER needed_compile\.py\s+ACTIVE', text), text
    # Without flags.make (or with a variable it does not define) the line is refused, not run with it empty.
    (tmp_path / 'CMakeFiles/ctrport.dir/flags.make').unlink()
    assert check.rules(tmp_path, jobs=2, expect_active=['needed_compile']) == 1
    text = capsys.readouterr().out
    assert text.count("ERROR (compile line: make syntax '$(C_DEFINES)' not understood") == 4, text
    assert 'EXPECTED ACTIVE needed_compile.py: not probed' in text, text


def test_expand_make():
    check = load_check()
    v = {'C_DEFINES': '-DX=\\"a b\\"', 'C_FLAGS': '-O2', 'NESTED': '$(C_FLAGS)'}
    assert check.expand_make('gcc $(C_DEFINES) $(C_FLAGS) -c a.c', v) == 'gcc -DX=\\"a b\\" -O2 -c a.c'
    assert check.expand_make('echo $$HOME', v) == 'echo $HOME'
    for bad in ('gcc $(C_INCLUDES) -c a.c', 'gcc -o $@', 'gcc ${C_FLAGS}', 'gcc $(NESTED)', 'gcc $'):
        with pytest.raises(ValueError):
            check.expand_make(bad, v)


def test_shell_for_an_msys_make_path(tmp_path):
    # Round 6 real tree: CMAKE_MAKE_PROGRAM:FILEPATH=/usr/bin/make.exe, an MSYS path. It is devkitPro's msys2 make,
    # and its sh runs the recipes; another sh (Git's) mounts other paths, so none is taken in its place.
    check = load_check()
    cache = tmp_path / 'CMakeCache.txt'
    if os.name != 'nt':
        cache.write_text('CMAKE_MAKE_PROGRAM:FILEPATH=/usr/bin/make\n')
        assert check.shell_for(tmp_path) in ('/usr/bin/sh', shutil.which('sh') or 'sh')
        return
    sh = devkit('msys2/usr/bin/sh')
    cache.write_text('CMAKE_MAKE_PROGRAM:FILEPATH=/usr/bin/make.exe\nCMAKE_MAKE_PROGRAM-ADVANCED:INTERNAL=1\n')
    assert Path(check.shell_for(tmp_path)).resolve() == sh.resolve()
    cache.write_text('CMAKE_MAKE_PROGRAM:FILEPATH=/c/devkitPro/msys2/usr/bin/make.exe\n')
    assert Path(check.shell_for(tmp_path)).resolve() == sh.resolve()
    cache.write_text('CMAKE_MAKE_PROGRAM:FILEPATH=/no/such/make.exe\n')
    with pytest.raises(ValueError, match='no sh.exe beside the make program'):
        check.shell_for(tmp_path)


def test_rules_not_run_without_the_recipe_shell(tmp_path, monkeypatch, capsys):
    check = load_check()
    fake_rules_tree(tmp_path, monkeypatch, check, needed_changes_shared)

    def no_shell(build):
        raise ValueError('no sh.exe beside the make program X')
    monkeypatch.setattr(check, 'shell_for', no_shell)
    assert check.rules(tmp_path, jobs=2, expect_active=['needed_compile']) == 1
    assert 'not run: no sh.exe beside the make program' in capsys.readouterr().out


def test_probe_groups_join_objects_whose_files_overlap():
    check = load_check()
    checks = [(n, '--m', 's', o, 'line') for n, o in (
        ('a', 'd/x.c.o'), ('b', 'd/x.c.o'), ('a', 'd/x.c.extra.c.o'), ('a', 'd/y.c.o'), ('a', 'e/x.c.o'))]
    groups = sorted(sorted((c[0], c[3]) for c in g) for g in check.probe_groups(checks))
    assert groups == [[('a', 'd/x.c.extra.c.o'), ('a', 'd/x.c.o'), ('b', 'd/x.c.o')], [('a', 'd/y.c.o')],
                      [('a', 'e/x.c.o')]]


# ------------------------------------------------------------------------------------------- identity compare
# The verdict is about outputs (review round 3): every output byte-identical; static libraries and
# .cia files compared as ar/nm and ctrtool unpack them; build-system inputs (CMakeCache.txt, build.make, Makefile2,
# the configure log, the ABI probe) and the timing logs shown as plain diffs, never in the verdict. Tests marked
# c26rev4 are the reviewer's controls; each passed the 7648b11f1 checker's verdict, or was refused there by a check
# on a build-system file that is now information.

DEVKITPRO = next((Path(d) for d in ('C:/devkitPro', '/opt/devkitpro') if Path(d).is_dir()), None)
EXE = '.exe' if os.name == 'nt' else ''


def devkit(rel):
    p = DEVKITPRO / (rel + EXE) if DEVKITPRO else None
    if p is None or not p.is_file():
        pytest.skip('%s is not installed' % rel)
    return p


def pair(tmp_path, name, x, y):
    out = []
    for side, data in (('a', x), ('b', y)):
        p = tmp_path / side / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        out.append(p)
    return out


def snapshots(tmp_path, hashes):
    """A.json / B.json naming each file with the given (A hash, B hash); a hash None: the kept copy's own."""
    import hashlib
    for i, side in enumerate(('a', 'b')):
        files = {}
        for name, hs in hashes.items():
            h = hs[i]
            files[name] = h if h is not None else hashlib.sha256((tmp_path / side / name).read_bytes()).hexdigest()
        (tmp_path / (side + '.json')).write_text(json.dumps({'files': files}))
    return tmp_path / 'a.json', tmp_path / 'b.json'


def compare_pair(tmp_path, name, x, y, hashes=(None, None)):
    pa, pb = pair(tmp_path, name, x, y)
    return load_check().compare(*snapshots(tmp_path, {name: hashes}), pa.parents[name.count('/')],
                                pb.parents[name.count('/')])


def compare_files(tmp_path, files):
    """compare() over {name: (A bytes or None, B bytes or None)} (None: not on that side), kept copies bound."""
    import hashlib
    tmp_path.mkdir(parents=True, exist_ok=True)
    for i, side in enumerate(('a', 'b')):
        listed = {}
        for name, both in files.items():
            if both[i] is not None:
                p = tmp_path / side / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(both[i])
                listed[name] = hashlib.sha256(both[i]).hexdigest()
        (tmp_path / (side + '.json')).write_text(json.dumps({'files': listed}))
    return load_check().compare(tmp_path / 'a.json', tmp_path / 'b.json', tmp_path / 'a', tmp_path / 'b')


def verdict(report):
    return [l for l in report if not l.startswith(('info ', '  ', '=== ', '-', '+', '@@', '(no diff', 'binary:'))]


# ---- outputs: every other file byte-identical

@pytest.mark.parametrize('name', ['x.c.o', 'CMakeFiles/ctrport.dir/a/x.c.o.d', 'undertale-3ds.elf',
                                  'undertale-3ds-n3ds.3dsx', 'ctrport.smdh', 'canonical.ivfc',
                                  'CMakeFiles/ctrport.dir/flags.make', 'CMakeFiles/ctrport.dir/link.txt',
                                  'CMakeFiles/4.4.4/CMakeCCompiler.cmake', 'other/launch_dispatch.log',
                                  'sub/CMakeCache.txt', 'CMakeFiles/x.dir/sub/build.make'])
def test_every_other_file_must_be_identical(tmp_path, name):
    bad, report = compare_files(tmp_path, {name: (b'A\n', b'B\n')})
    assert bad == 1 and ('UNEXPECTED %s differs' % name) in report, report
    bad, report = compare_files(tmp_path / 'one', {name: (b'A\n', None)})
    assert bad == 1 and ('UNEXPECTED %s only on A' % name) in report, report


def test_only_exact_timing_logs_are_information(tmp_path):
    # c26rev4 round 3: only the two exact timing logs are exempt.
    a, b = tmp_path / 'a.json', tmp_path / 'b.json'
    check = load_check()
    for name, expected in [('compile_cache.log', 0), ('launch_dispatch.log', 0), ('other/launch_dispatch.log', 1)]:
        a.write_text(json.dumps({'files': {name: 'aaa'}}))
        b.write_text(json.dumps({'files': {name: 'bbb'}}))
        assert check.compare(a, b)[0] == expected, name


# ---- static libraries, read strictly and compared byte for byte but for the dates

def objects(tmp_path, bodies):
    """ARM ELF objects made by objcopy from each body (name: bytes)."""
    objcopy = devkit('devkitARM/bin/arm-none-eabi-objcopy')
    out = {}
    for name, body in bodies.items():
        src, obj = tmp_path / (name + '.bin'), tmp_path / (name + '.o')
        src.write_bytes(body)
        subprocess.run([str(objcopy), '-I', 'binary', '-O', 'elf32-littlearm', '-B', 'arm', src.name, obj.name],
                       cwd=tmp_path, stdin=subprocess.DEVNULL, check=True, capture_output=True, timeout=60)
        out[name] = obj
    return out


def archive(tmp_path, bodies, order=None, mtime=1794536913):
    """A static library made by GNU ar (real dates: U) from objects of `bodies`, in `order`."""
    tmp_path.mkdir(parents=True, exist_ok=True)
    objs = objects(tmp_path, bodies)
    for o in objs.values():
        os.utime(o, (mtime, mtime))
    lib = tmp_path / 'lib.a'
    subprocess.run([str(devkit('devkitARM/bin/arm-none-eabi-ar')), 'rcsU', lib.name] +
                   [objs[n].name for n in (order or sorted(bodies))],
                   cwd=tmp_path, stdin=subprocess.DEVNULL, check=True, capture_output=True, timeout=60)
    return lib.read_bytes()


def test_archive_dates_alone_pass(tmp_path):
    bodies = {'x': b'X' * 40, 'y': b'Y' * 41}
    a, b = archive(tmp_path / 'ma', bodies), archive(tmp_path / 'mb', bodies, mtime=1794538685)
    assert a != b
    bad, report = compare_pair(tmp_path, 'libx.a', a, b)
    assert bad == 0 and any(l.startswith('same      libx.a unpacked') for l in report), report


def test_archive_member_bytes_order_and_index_are_compared(tmp_path):
    bodies = {'x': b'X' * 40, 'y': b'Y' * 41}
    a = archive(tmp_path / 'ma', bodies)
    for i, b in enumerate([archive(tmp_path / 'mb', {'x': b'X' * 39 + b'Z', 'y': b'Y' * 41}),
                           archive(tmp_path / 'mc', bodies, order=['y', 'x']),
                           archive(tmp_path / 'md', {'x': b'X' * 40})]):
        bad, report = compare_pair(tmp_path / str(i), 'libx.a', a, b)
        assert bad == 1 and 'UNEXPECTED libx.a' in report[1], (i, report)


def test_c26rev4_archive_member_owner(tmp_path):
    # c26rev4 round 3: a member's uid changed.
    a = archive(tmp_path / 'ma', {'x': b'X' * 40})
    first = a.index(b'x.o/') if b'x.o/' in a else a.index(b'x.o')
    b = bytearray(a)
    b[first + 28] = ord('7')  # the uid field of x.o's member header
    bad, report = compare_pair(tmp_path, 'libfresh.a', a, bytes(b))
    assert bad == 1 and 'in the uid field of the member header at %d' % first in report[1], report


def hand_archive(*members, magic=b'!<arch>\n'):
    """An archive written by hand: each member (name, date, uid, data[, terminator[, pad]])."""
    out = magic
    for m in members:
        name, date, uid, data = m[:4]
        end = m[4] if len(m) > 4 else b'`\n'
        pad = m[5] if len(m) > 5 else (b'\n' if len(data) % 2 else b'')
        out += (name.ljust(16) + date.ljust(12) + uid.ljust(6) + b'0'.ljust(6) + b'644'.ljust(8) +
                str(len(data)).encode().ljust(10) + end + data + pad)
    return out


def test_c26rev4_archive_controls_that_ar_cannot_read(tmp_path):
    # c26rev4's hand-made archives (no symbol index, a member that is no object): a changed uid, a broken terminator.
    for i, (a, b) in enumerate([(hand_archive((b'x.o/', b'123', b'0', b'OBJ')),
                                 hand_archive((b'x.o/', b'123', b'1', b'OBJ'))),
                                (hand_archive((b'x.o/', b'123', b'0', b'OBJ')),
                                 hand_archive((b'x.o/', b'123', b'0', b'OBJ', b'XX')))]):
        bad, report = compare_pair(tmp_path / str(i), 'libfresh.a', a, b)
        assert bad == 1 and 'UNEXPECTED libfresh.a' in report[1], report


def test_archive_read_strictly_or_refused(tmp_path):
    # Round 5: the library is walked header by header; anything that does not read is refused, on either side, even
    # where the other side is the same bytes (equal hashes).
    good = hand_archive((b'x.o/', b'123', b'0', b'OBJ'), (b'y.o/', b'123', b'0', b'DATA'))
    assert compare_pair(tmp_path / 'good', 'libfresh.a', good,
                        hand_archive((b'x.o/', b'456', b'0', b'OBJ'), (b'y.o/', b'789', b'0', b'DATA')))[0] == 0
    size_at = 8 + 48
    broken = {
        'magic': b'!<arcx>\n' + good[8:],
        'thin': b'!<thin>\n' + good[8:],
        'terminator': hand_archive((b'x.o/', b'123', b'0', b'OBJ', b'`X')),
        'size not decimal': good[:size_at] + b'3x'.ljust(10) + good[size_at + 10:],
        'size blank': good[:size_at] + b' ' * 10 + good[size_at + 10:],
        'date not decimal': good[:8 + 16] + b'12x'.ljust(12) + good[8 + 28:],
        'odd member without its pad': hand_archive((b'x.o/', b'123', b'0', b'OBJ', b'`\n', b'')),
        'pad that is not a newline': hand_archive((b'x.o/', b'123', b'0', b'OBJ', b'`\n', b' ')),
        'trailing byte': good + b'\n',
        'truncated header': good + b'z.o/'.ljust(16),
        'truncated data': good[:-1],
    }
    for label, data in broken.items():
        for side, (x, y) in (('A', (data, good)), ('B', (good, data)), ('both', (data, data))):
            bad, report = compare_pair(tmp_path / ('%s-%s' % (label, side)).replace(' ', '-'), 'libfresh.a', x, y)
            assert bad == 1 and report[1].startswith('UNEXPECTED libfresh.a: '), (label, side, report)
            assert 'thin archive' in report[1] if label == 'thin' else 'not ' in report[1], (label, side, report)


def test_archive_mask_covers_only_each_header_date(tmp_path):
    # Round 5 mask boundary: the 12 date bytes of each walked header are the only bytes not compared. A byte changed
    # just before (the last name byte) or just after (the first uid byte) a date field fails, in every header, and
    # so does a "date" inside a member's data.
    check = load_check()
    a = archive(tmp_path / 'ma', {'x': b'X' * 40, 'y': b'Y' * 41})
    headers, _ = check.ar_headers(a)
    assert len(headers) == 3  # the symbol index, x.o, y.o
    for k, at in enumerate(headers):
        for pos, field in ((at + 15, 'name'), (at + 28, 'uid')):
            b = bytearray(a)
            b[pos] = ord('7') if b[pos] != ord('7') else ord('8')
            bad, report = compare_pair(tmp_path / ('%d-%s' % (k, field)), 'libx.a', a, bytes(b))
            assert bad == 1 and 'byte %d, in the %s field of the member header at %d' % (pos, field, at) in report[1], \
                report
        b = bytearray(a)
        b[at + 16:at + 28] = b'9'.ljust(12)  # the date itself: not compared
        assert compare_pair(tmp_path / ('%d-date' % k), 'libx.a', a, bytes(b))[0] == 0
    data = headers[1] + 60 + 4
    b = bytearray(a)
    b[data] ^= 1
    bad, report = compare_pair(tmp_path / 'data', 'libx.a', a, bytes(b))
    assert bad == 1 and 'in the data of member' in report[1], report


def duplicate_member_archive(tmp_path, mtime=1794536913):
    """Two different objects both stored as dup.o (ar q), with the same symbol names and a symbol index."""
    tmp_path.mkdir(parents=True, exist_ok=True)
    objcopy = devkit('devkitARM/bin/arm-none-eabi-objcopy')
    for d, body in (('d1', b'P' * 40), ('d2', b'Q' * 40)):
        (tmp_path / d).mkdir(exist_ok=True)
        (tmp_path / d / 'payload.bin').write_bytes(body)
        subprocess.run([str(objcopy), '-I', 'binary', '-O', 'elf32-littlearm', '-B', 'arm', 'payload.bin', 'dup.o'],
                       cwd=tmp_path / d, stdin=subprocess.DEVNULL, check=True, capture_output=True, timeout=60)
        os.utime(tmp_path / d / 'dup.o', (mtime, mtime))
    subprocess.run([str(devkit('devkitARM/bin/arm-none-eabi-ar')), 'qcsU', 'lib.a', 'd1/dup.o', 'd2/dup.o'],
                   cwd=tmp_path, stdin=subprocess.DEVNULL, check=True, capture_output=True, timeout=60)
    return (tmp_path / 'lib.a').read_bytes()


def retarget_first_index_entry(data, target):
    """The archive with its symbol index's first entry pointed at byte `target` (GNU index: "/" first, a big-endian
    count at +60, then the offsets)."""
    import struct
    assert data[8:24].rstrip() == b'/'
    out = bytearray(data)
    struct.pack_into('>I', out, 8 + 64, target)
    return bytes(out)


def test_c26rev4_archive_index_retargeted_between_same_named_members(tmp_path):
    # c26rev4 BF4-F1: nm prints an index entry's member by name. With two members named dup.o, pointing the first
    # index entry at the other dup.o leaves ar tv, ar p and nm --print-armap all unchanged, yet binds the symbol to
    # different bytes. The index's own bytes are compared now, so the retarget is refused.
    import struct
    a = duplicate_member_archive(tmp_path / 'ma')
    count = struct.unpack_from('>I', a, 68)[0]
    offsets = struct.unpack_from('>%dI' % count, a, 72)
    other = next(o for o in offsets if o != offsets[0])
    bad, report = compare_pair(tmp_path / 'retarget', 'libdup.a', a, retarget_first_index_entry(a, other))
    assert bad == 1 and "in the data of member '/'" in report[1], report
    # Repeated names are no longer refused as such: a dates-only twin is the same library.
    bad, report = compare_pair(tmp_path / 'dates', 'libdup.a', a, duplicate_member_archive(tmp_path / 'mb', 1794538685))
    assert bad == 0, report


def test_c26rev4_archive_index_retargeted_to_a_header_inside_a_member(tmp_path):
    # c26rev4 BF5-F1: carrier.o's data holds a 60-byte member-header lookalike named outer.o/ followed by another
    # object. Pointing the first index entry at that embedded header makes nm print outer.o again, so ar tv, ar p and
    # nm all match; only file bytes 74-75 change. They are index bytes, not a date field, so the library differs.
    objcopy = devkit('devkitARM/bin/arm-none-eabi-objcopy')
    for n in ('one', 'two'):
        (tmp_path / n).mkdir()
    first, hidden = (objects(tmp_path / n, {'dup': body})['dup'].read_bytes()
                     for n, body in (('one', b'P' * 40), ('two', b'Q' * 40)))
    embedded = (b'outer.o/'.ljust(16) + b'0'.ljust(12) + b'0'.ljust(6) + b'0'.ljust(6) + b'100644'.ljust(8) +
                str(len(hidden)).encode().ljust(10) + b'`\n' + hidden)
    work = tmp_path / 'lib'
    work.mkdir()
    (work / 'outer.o').write_bytes(first)
    (work / 'carrier.bin').write_bytes(embedded)
    subprocess.run([str(objcopy), '-I', 'binary', '-O', 'elf32-littlearm', '-B', 'arm', 'carrier.bin', 'carrier.o'],
                   cwd=work, stdin=subprocess.DEVNULL, check=True, capture_output=True, timeout=60)
    subprocess.run([str(devkit('devkitARM/bin/arm-none-eabi-ar')), 'qcsU', 'lib.a', 'outer.o', 'carrier.o'],
                   cwd=work, stdin=subprocess.DEVNULL, check=True, capture_output=True, timeout=60)
    a = (work / 'lib.a').read_bytes()
    b = retarget_first_index_entry(a, a.index(embedded))
    assert [i for i, (x, y) in enumerate(zip(a, b)) if x != y] in ([74, 75], [75], [74])
    nm = devkit('devkitARM/bin/arm-none-eabi-nm')
    printed = []
    for side, data in (('a', a), ('b', b)):
        (work / side).mkdir()
        (work / side / 'lib.a').write_bytes(data)
        printed.append(subprocess.run([str(nm), '--print-armap', 'lib.a'], cwd=work / side, stdin=subprocess.DEVNULL,
                                      check=True, capture_output=True, timeout=60).stdout)
    assert printed[0] == printed[1]  # the reviewer's point: nm cannot tell them apart
    bad, report = compare_pair(tmp_path / 'cmp', 'libfresh.a', a, b)
    assert bad == 1 and any("byte %d, in the data of member '/'" % i in report[1] for i in (74, 75)), report


def thin_build(tmp_path, body):
    """A build directory holding libthin.a, a thin archive (ar T) whose one member is ext/x.o outside it."""
    ext, build = tmp_path / 'ext', tmp_path / 'build'
    for d in (ext, build, tmp_path / 'objs'):
        d.mkdir(parents=True, exist_ok=True)
    (ext / 'x.o').write_bytes(objects(tmp_path / 'objs', {'x': body})['x'].read_bytes())
    lib = build / 'libthin.a'
    if lib.exists():
        lib.unlink()
    subprocess.run([str(devkit('devkitARM/bin/arm-none-eabi-ar')), 'qcsTU', 'build/libthin.a', 'ext/x.o'],
                   cwd=tmp_path, stdin=subprocess.DEVNULL, check=True, capture_output=True, timeout=60)
    return build


def test_c26rev4_thin_archive_refused_though_its_hash_is_equal(tmp_path):
    # c26rev4 BF5-F2: a thin archive's member is a file outside it. Its external object changed between A and B while
    # the archive's own bytes, and so its snapshot hash, stayed the same. Refused at snapshot and on compare's
    # equal-hash path, with kept copies or without.
    import shutil
    check = load_check()
    build = thin_build(tmp_path, b'A' * 40)
    thin = (build / 'libthin.a').read_bytes()
    assert thin[:8] == b'!<thin>\n'
    check.snapshot(build, tmp_path / 'A.json')
    shutil.copytree(build, tmp_path / 'keptA')
    thin_build(tmp_path, b'B' * 40)
    assert (build / 'libthin.a').read_bytes() == thin  # same container bytes, different member
    check.snapshot(build, tmp_path / 'B.json')
    sa, sb = (json.loads((tmp_path / n).read_text()) for n in ('A.json', 'B.json'))
    assert sa['files'] == sb['files'] and sa['thin'] == sb['thin'] == ['libthin.a']
    for dirs in ((), (tmp_path / 'keptA', build)):
        bad, report = check.compare(tmp_path / 'A.json', tmp_path / 'B.json', *dirs)
        assert bad == 1 and report[1].startswith('UNEXPECTED libthin.a: a thin archive on A (snapshot)'), report
    # Snapshots that never looked (no 'thin' list, as the reviewer's): the kept copies show it.
    for side in ('A', 'B'):
        snap = json.loads((tmp_path / ('%s.json' % side)).read_text())
        del snap['thin']
        (tmp_path / ('%s-old.json' % side)).write_text(json.dumps(snap))
    bad, report = check.compare(tmp_path / 'A-old.json', tmp_path / 'B-old.json', tmp_path / 'keptA', build)
    assert bad == 1 and report[1].startswith('UNEXPECTED libthin.a: a thin archive on A (kept copy)'), report
    # ... and without kept copies, an equal-hash library is not taken on its hash.
    bad, report = check.compare(tmp_path / 'A-old.json', tmp_path / 'B-old.json')
    assert bad == 1 and 'no kept copies' in report[1], report
    # A thin archive under another name is refused too (the snapshot looks at every file).
    (build / 'other.bin').write_bytes(thin)
    check.snapshot(build, tmp_path / 'C.json')
    assert json.loads((tmp_path / 'C.json').read_text())['thin'] == ['libthin.a', 'other.bin']
    bad, report = check.compare(tmp_path / 'C.json', tmp_path / 'C.json')
    assert bad == 2 and report[2].startswith('UNEXPECTED other.bin: a thin archive on A (snapshot)'), report


# ---- .cia files, as ctrtool unpacks them

RSF = '''BasicInfo:
  Title: "bftest"
  ProductCode: "CTR-P-BFTS"
  ContentType: Manual
  Logo: None
TitleInfo:
  UniqueId: 0xff3f1
  Category: Application
Option:
  UseOnSD: true
RomFs:
  RootPath: root
'''


def makerom_cia(tmp_path, text=b'hello romfs\n', content_id=0):
    """A real .cia: makerom packs a data archive (CFA) of a one-file RomFS holding `text` as content 0."""
    makerom = devkit('tools/bin/makerom')
    devkit('tools/bin/ctrtool')
    d = tmp_path / ('mk%d' % len(list(tmp_path.glob('mk*'))))
    (d / 'root').mkdir(parents=True, exist_ok=True)
    (d / 'root' / 'a.txt').write_bytes(text)
    (d / 't.rsf').write_text(RSF)
    for argv in (['-f', 'cfa', '-o', 't.cfa', '-rsf', 't.rsf'],
                 ['-f', 'cia', '-o', 't.cia', '-content', 't.cfa:0:0x%x' % content_id]):
        subprocess.run([str(makerom)] + argv, cwd=d, stdin=subprocess.DEVNULL, check=True, capture_output=True,
                       timeout=120)
    return (d / 't.cia').read_bytes()


def cia_layout(data):
    """(ticket offset, TMD offset, content offset, content size) of a .cia, for making fixtures only."""
    import struct
    al = lambda x: (x + 63) // 64 * 64
    hsize, _t, _v, cert, tik, tmd, _meta, size = struct.unpack_from('<IHHIIIIQ', data)
    t0 = al(hsize) + al(cert)
    m0 = t0 + al(tik)
    return t0, m0, m0 + al(tmd), size


def redraw(data, seed):
    """`data` with what makerom draws per .cia drawn anew: both signatures, the title key and the ticket ID."""
    import random
    rnd = random.Random(seed)
    t0, m0, _c0, _size = cia_layout(data)
    d = bytearray(data)
    for off, n in ((t0 + 4, 0x100), (t0 + 0x1BF, 16), (t0 + 0x1D0, 8), (m0 + 4, 0x100)):
        d[off:off + n] = bytes(rnd.randrange(256) for _ in range(n))
    return bytes(d)


def test_cia_makerom_random_fields_alone_pass(tmp_path):
    a = redraw(makerom_cia(tmp_path), 1)
    b = redraw(makerom_cia(tmp_path, content_id=0x277b6907), 2)
    assert a != b
    bad, report = compare_pair(tmp_path, 'x.cia', a, b)
    assert bad == 0 and any(l.startswith('same      x.cia unpacked') for l in report), report


def cia_variants(tmp_path):
    import hashlib
    base = makerom_cia(tmp_path)
    t0, m0, c0, size = cia_layout(base)
    out = {'other content': makerom_cia(tmp_path, b'hello romfs!\n')}
    d = bytearray(base)
    d[c0 + size - 0x10] ^= 1
    out['content changed, TMD hash not'] = bytes(d)
    d[m0 + 0xB04 + 0x10:m0 + 0xB04 + 0x30] = hashlib.sha256(d[c0:c0 + size]).digest()
    out['content and TMD hash changed'] = bytes(d)
    d = bytearray(base)
    d[0x2040 + 0x300] ^= 1
    out['certificate byte'] = bytes(d)
    out['byte appended'] = base + b'\0'
    d = bytearray(base)
    d[m0 + 0x18C + 7] ^= 1  # c26rev4 round 3: the TMD's title ID
    out['TMD title ID'] = bytes(d)
    d = bytearray(base)
    d[t0 + 0x1DC + 7] ^= 1
    out['ticket title ID'] = bytes(d)
    return base, out


def test_cia_contents_outside_bytes_and_fields_are_compared(tmp_path):
    base, variants = cia_variants(tmp_path)
    for i, (what, b) in enumerate(variants.items()):
        bad, report = compare_pair(tmp_path / str(i), 'x.cia', redraw(base, 1), redraw(b, 2))
        assert bad == 1 and 'UNEXPECTED x.cia' in report[1], (what, report)


def short_ticket_cia(data, mark):
    """c26rev4 round 3: no ticket at all (header ticket size 0), and a TMD byte that a ticket exemption covered."""
    import struct
    t0, m0, c0, size = cia_layout(data)
    out = bytearray(data[:t0] + data[m0:c0 + size])
    struct.pack_into('<I', out, 12, 0)
    out[t0 + 0x1c0] = mark
    return bytes(out)


def test_c26rev4_cia_missing_ticket(tmp_path):
    base = makerom_cia(tmp_path)
    bad, report = compare_pair(tmp_path, 'short.cia', short_ticket_cia(base, 0), short_ticket_cia(base, 1))
    assert bad == 1 and 'UNEXPECTED short.cia' in report[1], report


def test_c26rev4_cia_encrypted_content(tmp_path):
    # c26rev4 round 1: the same stored bytes under different title keys decrypt to different contents.
    base = makerom_cia(tmp_path)
    _t0, m0, _c0, _size = cia_layout(base)
    d = bytearray(base)
    d[m0 + 0xB04 + 7] |= 1  # content 0's type: encrypted; then the TMD's two record hashes, which ctrtool checks
    import hashlib
    d[m0 + 0x208:m0 + 0x228] = hashlib.sha256(d[m0 + 0xB04:m0 + 0xB34]).digest()
    d[m0 + 0x1E4:m0 + 0x204] = hashlib.sha256(d[m0 + 0x204:m0 + 0xB04]).digest()
    bad, report = compare_pair(tmp_path, 'x.cia', redraw(bytes(d), 1), redraw(bytes(d), 2))
    assert bad == 1 and 'encrypted' in report[1], report


# ---- kept copies

@pytest.mark.parametrize('name', ['libfresh.a', 'fresh.cia', 'sub/x.cia'])
def test_unpacked_outputs_need_bound_kept_copies(tmp_path, name):
    # c26rev4 round 2/3: kept copies must be there and be the snapshot's bytes.
    a, b = pair(tmp_path, name, b'A', b'B')
    sa, sb = snapshots(tmp_path, {name: (None, None)})
    check = load_check()
    bad, report = check.compare(sa, sb)
    assert bad == 1 and 'no kept copies' in report[1], report
    b.write_bytes(b'CHANGED')
    bad, report = check.compare(sa, sb, tmp_path / 'a', tmp_path / 'b')
    assert bad == 1 and 'not the snapshot' in report[1], report


def test_kept_names():
    assert load_check().kept_names(['x.cia', 'y.o', 'CMakeFiles/Makefile2', 'libz.a', 'CMakeCache.txt',
                                    'CMakeFiles/z.dir/build.make', 'compile_cache.log', 'x.elf',
                                    'CMakeFiles/CMakeConfigureLog.yaml',
                                    'CMakeFiles/4.4.4/CMakeDetermineCompilerABI_C.bin']) == [
        'CMakeCache.txt', 'CMakeFiles/4.4.4/CMakeDetermineCompilerABI_C.bin', 'CMakeFiles/CMakeConfigureLog.yaml',
        'CMakeFiles/Makefile2', 'CMakeFiles/z.dir/build.make', 'libz.a', 'x.cia']


# ---- build-system inputs: a plain diff, never the verdict

def minimal_elf(entry, debug=b'DBG1'):
    """c26rev4's ARM ELF (one .text), plus a non-allocated .debug_str holding `debug`."""
    import struct
    d = bytearray(0x200)
    d[:16] = b'\x7fELF\x01\x01\x01' + bytes(9)
    struct.pack_into('<HHIIIIIHHHHHH', d, 16, 2, 40, 1, entry, 0, 0xC0, 0, 52, 32, 0, 40, 4, 2)
    d[0x60:0x64] = b'CODE'
    names = b'\0.text\0.shstrtab\0.debug_str\0'
    d[0x70:0x70 + len(names)] = names
    d[0x1A0:0x1A0 + len(debug)] = debug
    struct.pack_into('<IIIIIIIIII', d, 0xC0 + 40, 1, 1, 6, 0x1000, 0x60, 4, 0, 0, 4, 0)
    struct.pack_into('<IIIIIIIIII', d, 0xC0 + 80, 7, 3, 0, 0, 0x70, len(names), 0, 0, 1, 0)
    struct.pack_into('<IIIIIIIIII', d, 0xC0 + 120, 17, 1, 0x30, 0, 0x1A0, len(debug), 0, 0, 1, 1)
    return bytes(d)


def debug_load_elf(word):
    """c26rev4 round 3: debug bytes inside a PT_LOAD segment (the loader follows PT_LOAD, not SHF_ALLOC)."""
    import struct
    d = bytearray(minimal_elf(0x11a0, struct.pack('<I', word)))
    struct.pack_into('<I', d, 0x1c, 52)
    struct.pack_into('<H', d, 0x2c, 1)
    struct.pack_into('<IIIIIIII', d, 52, 1, 0, 0x1000, 0x1000, len(d), len(d), 5, 1)
    return bytes(d)


CACHE = b'CMAKE_C_COMPILER_LAUNCHER:STRING=/py;first.py;/py;second.py;--cache-dir=E:/c/ccache-x-A\n'
QUOTE = b"\t/py compile_cache.py --cache-dir=E:/c/ccache-x-A gcc -DVALUE='$$BF_REVIEW_VALUE' -c x.c\n"
OTHER_TREE = b'\t/py -X utf8 -I /tree/tools/buildcache/a_compile.py gcc -c x.c\n'
EDGES = b'CMakeFiles/x.dir/all: CMakeFiles/a.dir/all\n\tprepare-input\nCMakeFiles/x.dir/all: CMakeFiles/b.dir/all\n'

# c26rev4's build-system controls: (name, A, B, a line the diff must show). Rounds 1 and 2 first, then round 3.
BUILD_SYSTEM_CONTROLS = {
    'cache-launcher-flags': ('CMakeCache.txt',
                             b'CMAKE_C_COMPILER_LAUNCHER:UNINITIALIZED=python;compile_cache.py;'
                             b'--cache-dir=E:/o/ccache-x-A;gcc;-O2\n',
                             b'CMAKE_C_COMPILER_LAUNCHER:UNINITIALIZED=python;compile_cache.py;'
                             b'--cache-dir=E:/o/ccache-x-B;gcc;-O0\n', '+CMAKE_C_COMPILER_LAUNCHER'),
    'makefile2-recipe-order': ('CMakeFiles/Makefile2', b'target:\n\tprepare-input\n\tcompile-input\n',
                               b'target:\n\tcompile-input\n\tprepare-input\n', '+\tcompile-input'),
    'abi-entry-point': ('CMakeFiles/4.4.4/CMakeDetermineCompilerABI_C.bin', minimal_elf(0x1000),
                        minimal_elf(0x1004), 'binary: 512 bytes on A, 512 on B, 1 differ'),
    'cache-launcher-reorder': ('CMakeCache.txt', CACHE,
                               CACHE.replace(b'first.py;/py;second.py', b'second.py;/py;first.py')
                               .replace(b'-A', b'-B'), '+CMAKE_C_COMPILER_LAUNCHER:STRING=/py;second.py'),
    'ctrport-shell-quote': ('CMakeFiles/ctrport.dir/build.make', QUOTE,
                            QUOTE.replace(b'-A', b'-B').replace(b"'$$BF_REVIEW_VALUE'", b'"$$BF_REVIEW_VALUE"'),
                            '+\t/py compile_cache.py --cache-dir=E:/c/ccache-x-B gcc -DVALUE="$$BF_REVIEW_VALUE"'),
    'benchromfs-shell-quote': ('CMakeFiles/benchromfs_mount.dir/build.make', QUOTE,
                               QUOTE.replace(b'-A', b'-B').replace(b"'$$BF_REVIEW_VALUE'", b'"$$BF_REVIEW_VALUE"'),
                               '+\t/py compile_cache.py --cache-dir=E:/c/ccache-x-B gcc -DVALUE="$$BF_REVIEW_VALUE"'),
    'dispatcher-other-tree': ('CMakeFiles/ctrport.dir/build.make', OTHER_TREE,
                              b'\t/py -X utf8 -I -S /unrelated/tools/buildcache/launch_dispatch.py ' + OTHER_TREE[1:],
                              '+\t/py -X utf8 -I -S /unrelated/tools/buildcache/launch_dispatch.py'),
    'makefile2-edge-across-recipe': ('CMakeFiles/Makefile2', EDGES,
                                     b'CMakeFiles/x.dir/all: CMakeFiles/b.dir/all\n\tprepare-input\n'
                                     b'CMakeFiles/x.dir/all: CMakeFiles/a.dir/all\n',
                                     '+CMakeFiles/x.dir/all: CMakeFiles/b.dir/all'),
    'abi-debug-in-load-segment': ('CMakeFiles/4.4.4/CMakeDetermineCompilerABI_C.bin', debug_load_elf(0xe3a00000),
                                  debug_load_elf(0xe3a00001), 'binary: 512 bytes on A, 512 on B, 1 differ'),
    'configure-log-raw-bytes': ('CMakeFiles/CMakeConfigureLog.yaml', b'note: \xff\n', b'note: \xfe\n',
                                '+note: \\xfe'),
}


@pytest.mark.parametrize('control', sorted(BUILD_SYSTEM_CONTROLS))
def test_c26rev4_build_system_control_is_shown_not_judged(tmp_path, control):
    name, a, b, shown = BUILD_SYSTEM_CONTROLS[control]
    bad, report = compare_pair(tmp_path, name, a, b)
    assert bad == 0 and any(l.startswith('info      %s: build-system input' % name) for l in report), report
    assert any(l.startswith(shown) for l in report), (shown, report)


def test_shell_quote_change_really_changes_the_argument():
    # c26rev4 round 3: Make turns $$ into $; the shell expands only the double-quoted form. Such a change can act on
    # the build only through the object it compiles, which the verdict compares (next test).
    sh = DEVKITPRO / 'msys2/usr/bin/bash.exe' if DEVKITPRO and os.name == 'nt' else Path('/bin/bash')
    if not sh.is_file():
        pytest.skip('no bash')
    results = []
    for arg in ("'$BF_REVIEW_VALUE'", '"$BF_REVIEW_VALUE"'):
        p = subprocess.run([str(sh), '-c', "printf '%s\\n' " + arg], env=dict(os.environ, BF_REVIEW_VALUE='EXPANDED'),
                           stdin=subprocess.DEVNULL, capture_output=True, timeout=20, check=True)
        results.append(p.stdout.strip())
    assert results == [b'$BF_REVIEW_VALUE', b'EXPANDED']


def test_the_outputs_decide(tmp_path):
    # A build-system difference with outputs it changed: the outputs fail the verdict; alone, it is information.
    name, a, b, _shown = BUILD_SYSTEM_CONTROLS['ctrport-shell-quote']
    bad, report = compare_files(tmp_path / 'with', {name: (a, b), 'CMakeFiles/ctrport.dir/x.c.o': (b'$BF', b'EXP')})
    assert bad == 1 and verdict(report)[1:] == ['UNEXPECTED CMakeFiles/ctrport.dir/x.c.o differs'], report
    bad, report = compare_files(tmp_path / 'alone', {name: (a, b), 'CMakeFiles/ctrport.dir/x.c.o': (b'$BF', b'$BF')})
    assert bad == 0 and verdict(report)[1:] == [], report


def test_build_system_diffs_without_bound_copies_and_on_one_side(tmp_path):
    check = load_check()
    name = 'CMakeFiles/Makefile2'
    a, b = pair(tmp_path, name, b'x: y\n', b'x: z\n')
    sa, sb = snapshots(tmp_path, {name: (None, None)})
    bad, report = check.compare(sa, sb)
    assert bad == 0 and '(no diff: no kept copies to check)' in report, report
    b.write_bytes(b'CHANGED')
    bad, report = check.compare(sa, sb, tmp_path / 'a', tmp_path / 'b')
    assert bad == 0 and any(l.startswith('(no diff: the kept copy on B is not the snapshot') for l in report), report
    bad, report = compare_files(tmp_path / 'one', {'CMakeFiles/z.dir/build.make': (None, b'z\n')})
    assert bad == 0 and any(l.startswith('info      CMakeFiles/z.dir/build.make only on B') for l in report), report


def test_build_system_diffs_can_go_to_a_file(tmp_path):
    check = load_check()
    pair(tmp_path, 'CMakeCache.txt', b'X:BOOL=OFF\n', b'X:BOOL=ON\n')
    out = tmp_path / 'diff.txt'
    bad, report = check.compare(*snapshots(tmp_path, {'CMakeCache.txt': (None, None)}), tmp_path / 'a',
                                tmp_path / 'b', out)
    assert bad == 0 and not any(l.startswith('+X') for l in report), report
    assert out.read_text().splitlines()[0] == '=== CMakeCache.txt' and '+X:BOOL=ON' in out.read_text()
