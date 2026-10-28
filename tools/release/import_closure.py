"""Static release Python/input closure. Never imports or executes the audited tools.

Walk build drivers, builder entry points, local imports (including sys.path inserts), and fixed repository
inputs. Generated outputs and player files are not release inputs. Optional developer art packers are reported
separately: build.sh explicitly checks for them, and production kits use the player's runtime assets instead.
"""
import argparse
import ast
import json
from pathlib import Path
import posixpath as pp
import re
import subprocess
import sys

DRIVERS = ('platform/3ds/build.sh', 'platform/3ds/CMakeLists.txt', 'tools/texprep/build.sh')
ENTRY_POINTS = ('tools/release/builder/builder.py', 'tools/release/builder_platform_probe.py',
                'tools/release/package_builder.py', 'tools/release/check_release.py')
OPTIONAL_PACKERS = {'tools/analysis/crispart_pack.py', 'tools/analysis/smalltext_pack.py'}
GAME_ONLY = {'tools/aot/pixgrid.py': 'undertale', 'tools/aot/btnprompt_build.py': 'undertale',
             'tools/aot/loadscreen_art.py': 'deltarune', 'tools/aot/chapter_manifest.py': 'deltarune',
             'tools/roomplan/build_sfxplan.py': 'undertale', 'tools/aot/gog_smart.py': 'undertale',
             # UNDERTALE Old's half-resolution art (build.sh halfart, the builder's products.HALFART setting).
             'tools/analysis/halfart_pack.py': 'undertale',
             # stpack.py and stglyphs.py make both games' glyph packs (DELTARUNE New's Crisp text, sansfont round 3).
             'tools/roomplan/build_tracks.py': 'deltarune', 'tools/aot/stmenu.py': 'undertale',
             # flowfull's chapter-5 code plan (build.sh flowcode, DELTARUNE New only).
             'tools/roomplan/build_flowcode.py': 'deltarune',
             # offkill's early-destroy entries (build.sh runs it for UNDERTALE only; pipeline.py's source_wanted agrees).
             'tools/aot/offkill.py': 'undertale'}


class Repository:
    def __init__(self, root, ref=None):
        self.root, self.ref, self.cache = Path(root), ref, {}
        command = ['git', 'ls-tree', '-r', '--name-only', '-z', ref] if ref else ['git', 'ls-files', '-z']
        data = subprocess.check_output(command, cwd=self.root, stdin=subprocess.DEVNULL, timeout=30)
        self.files = set(data.decode('utf-8').strip('\0').split('\0'))

    def read(self, name):
        if name not in self.cache:
            if self.ref:
                data = subprocess.check_output(['git', 'show', self.ref + ':' + name], cwd=self.root,
                                               stdin=subprocess.DEVNULL, timeout=15)
                self.cache[name] = data.decode('utf-8-sig')
            else:
                self.cache[name] = (self.root / name).read_text(encoding='utf-8-sig')
        return self.cache[name]


def qualified(node):
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        return qualified(node.value) + '.' + node.attr
    return ''


def value(node, env):
    """The small path-expression language used by the build tools; unknown/player values stay unknown."""
    if isinstance(node, ast.Constant) and isinstance(node.value, (str, int)):
        return node.value
    if isinstance(node, ast.Name):
        return env.get(node.id)
    if isinstance(node, (ast.Tuple, ast.List)):
        items = [value(n, env) for n in node.elts]
        return items if all(n is not None for n in items) else None
    if isinstance(node, ast.Attribute) and node.attr == 'parent':
        parent = value(node.value, env)
        return pp.dirname(parent) if isinstance(parent, str) else None
    if isinstance(node, ast.Subscript) and isinstance(node.value, ast.Attribute) and node.value.attr == 'parents':
        parent, count = value(node.value.value, env), value(node.slice, env)
        if isinstance(parent, str) and isinstance(count, int):
            for _ in range(count + 1):
                parent = pp.dirname(parent)
            return parent
    if isinstance(node, ast.BinOp):
        left, right = value(node.left, env), value(node.right, env)
        if isinstance(left, str) and isinstance(right, str):
            if isinstance(node.op, ast.Div):
                return pp.normpath(pp.join(left, right))
            if isinstance(node.op, ast.Add):
                return left + right
    if isinstance(node, ast.JoinedStr):
        parts = [value(n.value, env) if isinstance(n, ast.FormattedValue) else value(n, env) for n in node.values]
        return ''.join(map(str, parts)) if all(n is not None for n in parts) else None
    if isinstance(node, ast.Call):
        name, args = qualified(node.func), [value(a, env) for a in node.args]
        if any(a is None for a in args):
            return None
        if name in ('Path', 'pathlib.Path', 'str') and args:
            return pp.join(*map(str, args))
        if name in ('os.path.dirname', 'pp.dirname') and len(args) == 1:
            return pp.dirname(args[0])
        if name in ('os.path.abspath', 'os.path.realpath', 'os.path.normpath') and len(args) == 1:
            return pp.normpath(args[0])
        if name == 'os.path.join':
            return pp.normpath(pp.join(*args))
        if isinstance(node.func, ast.Attribute):
            receiver = value(node.func.value, env)
            if isinstance(receiver, str):
                if node.func.attr in ('resolve', 'absolute', 'as_posix'):
                    return pp.normpath(receiver)
                if node.func.attr == 'with_name' and len(args) == 1:
                    return pp.join(pp.dirname(receiver), args[0])
                if node.func.attr == 'joinpath':
                    return pp.normpath(pp.join(receiver, *args))
    return None


def module_paths(name, searches, files):
    result = []
    for directory in searches:
        base = pp.normpath(pp.join(directory, name.replace('.', '/'))).lstrip('/')
        parents = [pp.join(directory, *name.split('.')[:i], '__init__.py').lstrip('/')
                   for i in range(1, len(name.split('.')))]
        for path in parents + [base + '.py', base + '/__init__.py']:
            if path in files and path not in result:
                result.append(path)
    return result


def relevant(path, game):
    # Shared fontverdict imports the GOG helper only for a verified UNDERTALE container.
    # Apply product scoping to transitive imports as well as build-driver invocations.
    if path in GAME_ONLY and GAME_ONLY[path] != game:
        return False
    # Records/selections are chosen per product; shared scaffold imports still belong to both packages.
    if path.startswith(('tools/roomplan/records/', 'tools/roomplan/phases/', 'tools/roomplan/tracks/',
                        'tools/aot/selection/')):
        other = 'deltarune' if game == 'undertale' else 'undertale'
        return not pp.basename(path).startswith((other + '-', other + '.')) and pp.basename(path) != 'typed-' + other + '.txt'
    return True


def read_argument(node):
    if not isinstance(node, ast.Call):
        return None
    name = qualified(node.func)
    if name in ('open', 'io.open') and node.args:
        mode = node.args[1] if len(node.args) > 1 else next((k.value for k in node.keywords if k.arg == 'mode'), None)
        if mode is None or (isinstance(mode, ast.Constant) and not any(c in str(mode.value) for c in 'wax')):
            return node.args[0]
    if isinstance(node.func, ast.Attribute) and node.func.attr in ('open', 'read_text', 'read_bytes'):
        if node.func.attr == 'open':
            mode = node.args[0] if node.args else next((k.value for k in node.keywords if k.arg == 'mode'), None)
            if mode is not None and (not isinstance(mode, ast.Constant) or any(c in str(mode.value) for c in 'wax')):
                return None
        return node.func.value
    return None


def scopes(tree, inherited):
    """Resolve constants within their own function; do not leak a helper's locals into its caller."""
    nodes, nested = [], []
    def collect(node):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            nested.append(node)
        else:
            nodes.append(node)
            for child in ast.iter_child_nodes(node):
                collect(child)
    for statement in tree.body:
        collect(statement)
    env = dict(inherited)
    for node in nodes:
        if isinstance(node, (ast.Assign, ast.AnnAssign)):
            result = value(node.value, env)
            targets = node.targets if isinstance(node, ast.Assign) else [node.target]
            for target in targets:
                if isinstance(target, ast.Name) and result is not None:
                    env[target.id] = result
    yield nodes, env
    for child in nested:
        yield from scopes(child, env)


def directory_prefix(node, env):
    """Keep the fixed directory in join(HERE, 'selection', a_dynamic_filename)."""
    if isinstance(node, ast.Call) and qualified(node.func) == 'os.path.join':
        known = []
        for argument in node.args:
            part = value(argument, env)
            if not isinstance(part, str):
                break
            known.append(part)
        return pp.normpath(pp.join(*known)) if known else None
    return value(node, env)


def check(repository, manifest, game, entry_points=None, drivers=None):
    """Return all missing inputs with their first importing/opening caller. Does not weaken the content gate."""
    files, required, visited, optional = repository.files, {}, set(), set()
    pending = []
    def need(path, caller):
        path = pp.normpath(path).lstrip('/')
        if not relevant(path, game):
            return
        required.setdefault(path, caller)
        if path.endswith('.py') and path in files and path not in visited:
            pending.append(path)
    for path in ENTRY_POINTS if entry_points is None else entry_points:
        need(path, 'builder entry point')
    for path in manifest - files:
        required.setdefault(path, 'manifest entry')
    for driver in DRIVERS if drivers is None else drivers:
        need(driver, 'build driver')
        if driver not in files:
            continue
        lines = '\n'.join(line for line in repository.read(driver).splitlines() if not line.lstrip().startswith('#'))
        for path in re.findall(r'(?:tools|platform)/[A-Za-z0-9_./-]+\.py', lines):
            if path in OPTIONAL_PACKERS:
                optional.add(path)
            elif path not in GAME_ONLY or GAME_ONLY[path] == game:
                need(path, driver)
    while pending:
        path = pending.pop()
        if path in visited:
            continue
        visited.add(path)
        tree = ast.parse(repository.read(path), filename=path)
        env = {'__file__': '/' + path, 'game': game}
        # Resolve module constants before processing functions, without executing any source.
        for node in tree.body:
            if isinstance(node, (ast.Assign, ast.AnnAssign)):
                result = value(node.value, env)
                targets = node.targets if isinstance(node, ast.Assign) else [node.target]
                for target in targets:
                    if isinstance(target, ast.Name) and result is not None:
                        env[target.id] = result
                        if isinstance(result, list) and target.id in ('SOURCE_FILES', 'EXTRA_FILES', 'GENERATOR_FILES'):
                            for item in result:
                                if isinstance(item, str):
                                    candidate = pp.join(pp.dirname(path), item) if target.id == 'GENERATOR_FILES' else item
                                    need(candidate, '%s:%d input list' % (path, node.lineno))
        searches = ['/' + pp.dirname(path)]
        # Inserts under conditional/platform/function scopes are all considered, so host OS cannot hide an import.
        scoped_nodes = [(node, local) for nodes, local in scopes(tree, env) for node in nodes]
        for node, local in scoped_nodes:
            if isinstance(node, ast.Call) and qualified(node.func) in ('sys.path.insert', 'sys.path.append') and node.args:
                directory = value(node.args[-1], local)
                if isinstance(directory, str):
                    searches.insert(0, directory)
        for node, local_env in scoped_nodes:
            if isinstance(node, ast.Import):
                names = [a.name for a in node.names]
            elif isinstance(node, ast.ImportFrom):
                prefix = node.module or ''
                local = searches
                if node.level:
                    base = '/' + pp.dirname(path)
                    for _ in range(node.level - 1):
                        base = pp.dirname(base)
                    local = [base]
                for name in [prefix] + [prefix + ('.' if prefix else '') + a.name for a in node.names]:
                    for imported in module_paths(name, local, files):
                        need(imported, '%s:%d import' % (path, node.lineno))
                continue
            elif isinstance(node, ast.Call) and qualified(node.func) in ('__import__', 'importlib.import_module') and node.args:
                name = value(node.args[0], local_env)
                names = [name] if isinstance(name, str) else []
            else:
                names = []
            for name in names:
                for imported in module_paths(name, searches, files):
                    need(imported, '%s:%d import' % (path, node.lineno))
            # Inspect reads, rather than path strings in assertions about deliberately excluded source files.
            argument = read_argument(node)
            result = value(argument, local_env) if argument is not None else None
            if isinstance(result, str) and result.startswith('/'):
                candidate = pp.normpath(result).lstrip('/')
                if candidate in files:
                    need(candidate, '%s:%d fixed path' % (path, getattr(node, 'lineno', 0)))
            directory = directory_prefix(node, local_env)
            if isinstance(directory, str) and directory.startswith('/'):
                candidate = pp.normpath(directory).lstrip('/')
                # Sound records live one level below records/. Do not lose them merely because the
                # final component is "sounds" (the generator selects its JSON by the player's hash).
                if any(part in ('records', 'fixtures', 'selection', 'tracks') for part in candidate.split('/')):
                    # An argparse default can name a fixed record rather than a directory.
                    if candidate in files and not candidate.endswith('.pyc'):
                        need(candidate, '%s:%d fixed record' % (path, getattr(node, 'lineno', 0)))
                    for child in files:
                        if child.startswith(candidate + '/') and not child.endswith('.pyc'):
                            need(child, '%s:%d fixed directory' % (path, getattr(node, 'lineno', 0)))
            if isinstance(node, ast.Constant) and isinstance(node.value, str) and '\n' not in node.value:
                # Fixed repo-relative subprocess paths and sibling generator fingerprints.
                literal = node.value
                if literal.endswith('.py') and '/' not in literal:
                    candidate = pp.join(pp.dirname(path), literal)
                    if candidate in files:
                        need(candidate, '%s:%d sibling script' % (path, node.lineno))
    missing = [{'path': p, 'required_by': required[p], 'absent_from_source': p not in files}
               for p in sorted(required) if p not in manifest or p not in files]
    return dict(game=game, missing=missing, python_modules=sorted(visited), required=sorted(required),
                optional_developer_packers=sorted(optional))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--ref', help='read an exact Git revision, without changing that checkout')
    parser.add_argument('--game', choices=['undertale', 'deltarune'], required=True)
    parser.add_argument('--json', type=Path)
    args = parser.parse_args()
    repo = Repository(args.root, args.ref)
    manifest = 'tools/release/manifest-' + ('ut' if args.game == 'undertale' else 'dr') + '.txt'
    if manifest not in repo.files:
        message = 'no %s at %s' % (manifest, args.ref or 'the source checkout')
        if args.json:
            args.json.write_text(json.dumps(dict(game=args.game, error=message), indent=2) + '\n', encoding='utf-8')
        print('ERROR: ' + message, file=sys.stderr)
        return 2
    names = {line.strip() for line in repo.read(manifest).splitlines() if line.strip() and not line.startswith('#')}
    report = check(repo, names, args.game)
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    for row in report['missing']:
        print('MISSING %s (%s)' % (row['path'], row['required_by']))
    print('%s: %d Python modules, %d required inputs, %d missing' %
          (args.game, len(report['python_modules']), len(report['required']), len(report['missing'])))
    return bool(report['missing'])


if __name__ == '__main__':
    sys.exit(main())
