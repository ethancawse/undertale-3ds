"""Build an ordinary .app ZIP with pinned, relocatable Python/Tk; never installs Python.

Runtime archives are private build inputs, not files in the curated source tree.
Upstream Mach-O bytes (including their loader signatures) are never rewritten.
Native Finder/Tk/build acceptance remains a separate gate on each architecture.
"""
from __future__ import annotations

import hashlib
import ast
import json
from pathlib import Path, PurePosixPath
import plistlib
import posixpath
import stat
import struct
import tarfile
import zipfile
import zlib

RELEASE = '20261026'
PYTHON = '3.14.10'
VERSION = '3.14'
PINS = {
    'arm64': ('aarch64', 26753457, '95eb8c87d16af37c54e9765e31d8c56c0d8ffa9968ea0f704f73a9b83f84e9f9'),
    'x86_64': ('x86_64', 26943401, '2fd8becd48cf0ff5f0a15df3c0db7cae7aa708dcaacf901bd30d6cdd6229370b'),
}


def runtime_url(arch):
    triple = PINS[arch][0]
    name = f'cpython-{PYTHON}+{RELEASE}-{triple}-apple-darwin-install_only_stripped.tar.gz'
    return 'https://github.com/astral-sh/python-build-standalone/releases/download/' + RELEASE + '/' + name.replace('+', '%2B')


def safe_name(name):
    p = PurePosixPath(name)
    if not name or '\\' in name or ':' in name or p.is_absolute() or '..' in p.parts or '\0' in name:
        raise ValueError('Unsafe runtime member: ' + name)
    if p.parts[0] != 'python':
        raise ValueError('Unexpected runtime root: ' + name)
    return p.as_posix()


def wanted(name):
    """Remove package installers/developer headers, retaining stdlib, Tk, native dependencies and licences."""
    rel = name.removeprefix('python/')
    if rel.startswith('bin/'):
        return rel in ('bin/python', 'bin/python3', 'bin/python3.14')
    return not (rel.startswith(('include/', 'share/man/', 'lib/pkgconfig/',
                               'lib/python3.14/site-packages/', 'lib/python3.14/ensurepip/')) or
                rel.endswith(('.pyc', '.a')) or '/__pycache__/' in rel)


def runtime_files(path, arch):
    """Verify the upstream archive, then read regular files and resolve internal links without extraction."""
    path = Path(path)
    pin = PINS[arch]
    with path.open('rb') as stream:
        if path.stat().st_size != pin[1] or hashlib.file_digest(stream, 'sha256').hexdigest() != pin[2]:
            raise ValueError('Mac runtime does not match its pin: ' + arch)
    result = {}
    with tarfile.open(path) as archive:
        members = {}
        for member in archive.getmembers():
            name = safe_name(member.name)
            if name in members:
                raise ValueError('Duplicate runtime member: ' + name)
            if not (member.isdir() or member.isfile() or member.issym() or member.islnk()):
                raise ValueError('Unsupported runtime member: ' + name)
            members[name] = member

        def resolve(name, seen=()):
            if name in seen or name not in members:
                raise ValueError('Broken runtime link: ' + name)
            member = members[name]
            if member.issym() or member.islnk():
                target = member.linkname
                if target.startswith('/') or '\\' in target:
                    raise ValueError('External runtime link: ' + name)
                target = (posixpath.join(posixpath.dirname(name), target) if member.issym() else target)
                target = safe_name(posixpath.normpath(target))
                return resolve(target, (*seen, name))
            if not member.isfile() or member.size > 100 * 1024 * 1024:
                raise ValueError('Invalid runtime file: ' + name)
            return archive.extractfile(member).read(), 0o755 if member.mode & 0o111 else 0o644

        for name, member in members.items():
            if member.issym() or member.islnk():
                resolve(name)
            if not member.isdir() and wanted(name):
                result[name.removeprefix('python/')] = resolve(name)
        # OpenSSL cannot assume python.org's installed certificate bundle exists.
        # Reuse the CA snapshot and its MPL text already inside this pinned input.
        vendor = 'python/lib/python3.14/site-packages/pip/_vendor/certifi/'
        result['certs/cacert.pem'] = resolve(vendor + 'cacert.pem')
        result['certs/LICENSE.certifi'] = resolve(vendor + 'LICENSE')
    required = ('bin/python3.14', 'lib/python3.14/encodings/__init__.py',
                'lib/python3.14/tkinter/__init__.py', 'lib/python3.14/multiprocessing/spawn.py',
                'lib/python3.14/lib-dynload/_tkinter.cpython-314-darwin.so',
                'lib/libtcl9.0.dylib', 'lib/libtcl9tk9.0.dylib',
                'lib/tcl9.0/init.tcl', 'lib/tk9.0/tk.tcl', 'lib/python3.14/LICENSE.txt')
    for name in required:
        if not result.get(name, (b'',))[0]:
            raise ValueError('Incomplete bundled runtime: ' + name)
    # ZIP/Finder extraction must not need symlink support. Tiny aliases avoid
    # duplicating the 19 MB interpreter for each upstream python/python3 link.
    alias = b'#!/bin/sh\nexec "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/python3.14" "$@"\n'
    for name in ('bin/python', 'bin/python3'):
        result[name] = (alias, 0o755)
    return result


def launcher(game):
    # Absolute OS utilities only. Do not invoke Apple's /usr/bin/python3 or a PATH Python.
    # Bundles may be read-only or translocated; logs/work/output always live outside them.
    return '''#!/bin/sh
set -eu
CONTENTS=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
case "$(/usr/bin/uname -m)" in
  arm64) ARCH=arm64 ;;
  x86_64) ARCH=x86_64 ;;
  *) exit 2 ;;
esac
RUNTIME="$CONTENTS/Resources/runtime/$ARCH"
export TCL_LIBRARY="$RUNTIME/lib/tcl9.0"
export TK_LIBRARY="$RUNTIME/lib/tk9.0"
export SSL_CERT_FILE="$RUNTIME/certs/cacert.pem"
unset PYTHONHOME PYTHONPATH PYTHONSTARTUP __PYVENV_LAUNCHER__
exec "$RUNTIME/bin/python3.14" -I -B "$CONTENTS/Resources/app_bootstrap.py" --game ''' + game + ''' "$@"
'''


BOOTSTRAP = '''"""Finder entry point. Keep the real interpreter usable by AOT children and multiprocessing."""
import os
from pathlib import Path
import runpy
import sys

if __name__ == '__main__':
    resources = Path(__file__).resolve().parent
    app = resources / 'builder/tools/release/builder/builder.py'
    sys.argv = [arg for arg in sys.argv if not arg.startswith('-psn_')]
    if '--check-runtime' in sys.argv:
        # This support check needs the unmodified isolated stdlib path.
        runpy.run_path(str(resources / 'runtime_check.py'), run_name='__main__')
        raise SystemExit(0)
    if not any(arg in sys.argv for arg in ('--cli', '--dry-run', '--help', '-h')):
        # Finder supplies no terminal; capture early Python/Tk errors outside the app.
        logs = Path.home() / 'Library/Logs/3DS Builder'
        logs.mkdir(parents=True, exist_ok=True)
        sys.stdout = sys.stderr = (logs / 'launcher.log').open('w', encoding='utf-8', buffering=1)
        print('Bundled Python:', sys.executable)
        print('Runtime:', sys.version)
    # Match direct Python script execution without relying on cwd, PATH or user site.
    sys.path.insert(0, str(app.parent))
    sys.argv[0] = str(app)
    runpy.run_path(str(app), run_name='__main__')
'''


def heart_icon(gui_source):
    """Reuse the builder's original heart, never extract art from a player's game."""
    tree = ast.parse(gui_source)
    rows, = [ast.literal_eval(node.value) for node in tree.body if isinstance(node, ast.Assign)
             and any(isinstance(name, ast.Name) and name.id == 'FALLBACK_SOUL_ROWS' for name in node.targets)]
    if len(rows) != 16 or any(len(row) != 16 or set(row) - {'.', '#'} for row in rows):
        raise ValueError('Invalid original builder heart')
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    icons = []
    for kind, size in ((b'ic07', 128), (b'ic08', 256), (b'ic09', 512)):
        scale = size // 32
        pad = (size - 16 * scale) // 2
        pixels = bytearray()
        for y in range(size):
            pixels.append(0)  # PNG filter None
            for x in range(size):
                heart = pad <= x < size-pad and pad <= y < size-pad and rows[(y-pad)//scale][(x-pad)//scale] == '#'
                pixels.extend((255 if heart else 0, 0, 0, 255))
        png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
               chunk(b'IDAT', zlib.compress(pixels, 9)) + chunk(b'IEND', b''))
        icons.append(kind + struct.pack('>I', len(png) + 8) + png)
    body = b''.join(icons)
    return b'icns' + struct.pack('>I', len(body) + 8) + body


def write_app(files, output, game, version, runtimes, opening, licences):
    """One ZIP containing one app, source, two native runtimes and dependency notices."""
    if set(runtimes) != set(PINS):
        raise ValueError('Mac app needs both arm64 and x86_64 runtime archives')
    title = game.upper() + ' 3DS Builder'
    prefix = title + '.app/Contents/'
    entries = {prefix + 'Resources/builder/' + rel: (data, 0o755 if rel.endswith('.sh') else 0o644)
               for rel, data in files.items()}
    info = dict(CFBundleExecutable='builder-launcher', CFBundleIdentifier='com.ethancawse.' + game + '-3ds-builder',
                CFBundleName=title, CFBundleDisplayName=title, CFBundlePackageType='APPL',
                CFBundleShortVersionString=version.split('-')[0], CFBundleVersion='1',
                CFBundleIconFile='builder.icns', LSArchitecturePriority=['arm64', 'x86_64'],
                NSLocalNetworkUsageDescription='Find your 3DS and copy your build to its SD card over Wi-Fi.',
                NSHighResolutionCapable=True, LSMinimumSystemVersion='11.0')
    entries[prefix + 'Info.plist'] = (plistlib.dumps(info, sort_keys=True), 0o644)
    entries[prefix + 'PkgInfo'] = (b'APPL????', 0o644)
    gui_source = files.get('tools/release/builder/gui.py') or (Path(__file__).parent / 'builder/gui.py').read_bytes()
    entries[prefix + 'Resources/builder.icns'] = (heart_icon(gui_source), 0o644)
    entries[prefix + 'MacOS/builder-launcher'] = (launcher(game).encode('utf-8'), 0o755)
    entries[prefix + 'Resources/app_bootstrap.py'] = (BOOTSTRAP.encode('utf-8'), 0o644)
    entries[prefix + 'Resources/runtime_check.py'] = (Path(__file__).with_name('macos_runtime_check.py').read_bytes(), 0o644)
    entries[prefix + 'Resources/README.md'] = (opening.encode('utf-8'), 0o644)
    receipt = dict(schema=1, python=PYTHON, release=RELEASE, runtimes={})
    for arch, path in runtimes.items():
        payload = runtime_files(path, arch)
        for rel, value in payload.items():
            entries[prefix + 'Resources/runtime/' + arch + '/' + rel] = value
        receipt['runtimes'][arch] = dict(url=runtime_url(arch), bytes=PINS[arch][1], sha256=PINS[arch][2],
            files={rel: dict(bytes=len(value[0]), sha256=hashlib.sha256(value[0]).hexdigest())
                   for rel, value in sorted(payload.items())})
    # Full upstream distribution licensing metadata/texts, supplied by the input preparation gate.
    licence_root = Path(licences)
    for p in sorted(licence_root.rglob('*')):
        if p.is_symlink():
            raise ValueError('Unexpected licence symlink')
        if p.is_file():
            entries[prefix + 'Resources/runtime-licences/' + p.relative_to(licence_root).as_posix()] = (p.read_bytes(), 0o644)
    for arch in PINS:
        root = licence_root / arch
        if not (root / 'PYTHON.json').is_file() or not list(root.rglob('LICENSE*')):
            raise ValueError('Missing upstream runtime licensing metadata/texts: ' + arch)
        metadata = json.loads((root / 'PYTHON.json').read_text(encoding='utf-8'))
        if metadata.get('target_triple') != PINS[arch][0] + '-apple-darwin' or metadata.get('python_version') != PYTHON:
            raise ValueError('Runtime licence metadata does not match its architecture/version')
        # Check every licence path referenced by the upstream build metadata, not only CPython.
        def check_licences(value):
            if isinstance(value, dict):
                for key, item in value.items():
                    if key in ('license_path', 'license_paths'):
                        for rel in [item] if isinstance(item, str) else item:
                            p = root / rel
                            if not p.resolve().is_relative_to(root.resolve()) or not p.is_file():
                                raise ValueError('Missing referenced runtime licence: ' + rel)
                    else:
                        check_licences(item)
            elif isinstance(value, list):
                for item in value:
                    check_licences(item)
        check_licences(metadata)
    entries[prefix + 'Resources/runtime-inventory.json'] = ((json.dumps(receipt, indent=2) + '\n').encode(), 0o644)
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    path = output / f'{game}-3ds-builder-{version}-macos.zip'
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, (data, mode) in sorted(entries.items()):
            member = zipfile.ZipInfo(name, (2026, 10, 28, 0, 0, 0))
            member.create_system = 3
            member.external_attr = (stat.S_IFREG | mode) << 16
            archive.writestr(member, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    path.with_name(path.name + '.sha256').write_bytes((digest + '  ' + path.name + '\n').encode('ascii'))
    return path
