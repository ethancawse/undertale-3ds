"""Qualified SDK libraries, downloaded and verified in the private work folder."""
from __future__ import annotations

import hashlib
import errno
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

if __name__ == '__main__':
    sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain

PACKAGES = (
    dict(package='libctru', version='2.7.0-1', extension='zst', size=1751778,
         sha256='6bdf9734dbaea38f457d037793f9c39d8e778ac3b3c5904c583c8e2b96bada2d',
         signature='c3f95de89dc9437ab9a7966ce4c52ebe1101a4f9ce6e868e313c658815511ed2',
         library='libctru.a', library_sha256='5e72cd24512b57908545e928cf0a1c1affbb8535f97773d7007be8f10f0e02bc',
         prefix='libctru'),
    dict(package='citro3d', version='1.7.1-2', extension='zst', size=386681,
         sha256='8c9a7caa7c8dc6d641f33dfac5e2ac6aee671c0bd4f1beb5a208b66d27757354',
         signature='34ca9f502f6b1991d61ecb9484ba20d2ae4baae2eb63b28e0594a5ec780a6ff1',
         library='libcitro3d.a', library_sha256='3f34eff859c1f1e60ebe597b6df9cf5e5e74563d9e64ca01a7b3292fde63016a',
         prefix='libctru'),
    dict(package='3ds-libvorbisidec', version='1.2.1-3', extension='xz', size=72244,
         sha256='8d707f05826fdf7cd1abd9a6c04cea6a71f8d47dae204a947eb6ef0ab3d1dc28',
         signature='79558ea3c18eccd5811f98174d397b5fa1f909dcdec2710b7e0b86108f7f0207',
         library='libvorbisidec.a', library_sha256='8b40b8425702d85fe5bc4249204069184da6c5293817a950ffee8be55d6c7b23',
         prefix='portlibs/3ds'),
    dict(package='3ds-libogg', version='1.3.6-1', extension='zst', size=30401,
         sha256='c9694cb66a74145711ed643324c395f9587784467bd84b19790a63e55287825a',
         signature='baa0fed0d40fcfb05b33886b0372b0e1be22206c1b0eaeb0be667aa99bc7d289',
         library='libogg.a', library_sha256='c64e78af6e99ef9e2f69f07973ccab153a7b0bdf9bc13d4ec954eb4e3262b7a3',
         prefix='portlibs/3ds'),
)
SERVER = 'https://pkg.devkitpro.org/packages/'
RELEASE_PAGE = 'https://github.com/ethancawse/undertale-3ds/releases'
REFUSAL = ('The tested 3DS libraries are not ready, so the build has not started. '
           'Your devkitPro is unchanged.')


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1024*1024), b''):
            h.update(block)
    return h.hexdigest()


def filename(pin):
    return '%s-%s-any.pkg.tar.%s' % (pin['package'], pin['version'], pin['extension'])


def verify_package(directory, packages=PACKAGES):
    """Validate complete cached archives and their original detached signatures."""
    for pin in packages:
        for name, size, sha in ((filename(pin), pin['size'], pin['sha256']),
                                (filename(pin)+'.sig', 566, pin['signature'])):
            path = Path(directory)/name
            if path.is_symlink() or not path.is_file() or path.stat().st_size != size or digest(path) != sha:
                raise ValueError('The tested library input does not match its pin: ' + name)


def require_stack(tc, pins=None):
    """Refuse independent compiler-runtime/rules drift, including changed rule bytes."""
    if pins is None:
        pins = json.loads(Path(__file__).with_name('sdk-stack-pins.json').read_text(encoding='utf-8'))
        command = ([tc.bash, '--login', '-c', 'pacman -Q ' + ' '.join(pins['packages'])]
                   if tc.platform == 'win32' else [toolchain.pacman_exe(tc.root), '-Q', *pins['packages']])
        try:
            status, text = toolchain.run_capture(command, timeout=30)
            versions = dict(line.split() for line in text.splitlines() if len(line.split()) == 2)
            if status or not set(pins['packages']).issubset(versions):
                raise ValueError('unreadable package versions')
        except subprocess.TimeoutExpired:
            raise toolchain.Refused('Checking the compiler support packages timed out. The build has not started. '
                'Wait for the computer to finish starting, then click Check again on the compiler page and try again. '
                'Your devkitPro is unchanged.') from None
        except (OSError, ValueError, subprocess.SubprocessError):
            raise toolchain.Refused('Could not check the compiler support packages. The build has not started. '
                'Click Check again on the compiler page, then try again. Your devkitPro is unchanged.') from None
    else:
        versions = tc.package_versions
    for name, wanted in pins['packages'].items():
        found = versions.get(name)
        if found != wanted:
            raise toolchain.Refused('This builder needs tested %s %s; found %s. The build has not started. '
                'Your devkitPro is unchanged. Check the UNDERTALE 3DS release page for a newer builder.\n%s' %
                (name, wanted, found or 'an unreadable version', RELEASE_PAGE))
    for relative, wanted in dict(pins['files'], **pins['runtime_archives']).items():
        path = Path(tc.root) / relative
        if not path.is_file() or digest(path) != wanted:
            raise toolchain.Refused('The SDK build rules or compiler libraries do not match this builder: %s. '
                'The build has not started. Your devkitPro is unchanged. '
                'Check the UNDERTALE 3DS release page for a newer builder.\n%s' % (relative, RELEASE_PAGE))
    return dict(packages=pins['packages'], files=pins['files'], runtime_archives=pins['runtime_archives'])


def download_error(error, folder):
    """Distinguish local access/space failures from a failed network request."""
    code, windows = getattr(error, 'errno', None), getattr(error, 'winerror', None)
    if isinstance(error, urllib.error.HTTPError):
        message = ('The server did not provide the required 3DS files (HTTP %d). '
                   'Try again later.') % error.code
    elif code in (errno.ENOSPC, errno.EDQUOT) or windows in (39, 112):
        message = ('There is not enough free space in the work folder for the required 3DS files. '
                   'Free some space, then choose Try again.')
    elif code in (errno.EACCES, errno.EPERM, errno.EROFS, errno.ENOENT, errno.ENOTDIR) or windows in (5, 19):
        message = ('The builder could not save the required 3DS files in the work folder. '
                   'Check access to the work folder, then choose Try again.')
    elif isinstance(error, (urllib.error.URLError, ConnectionError, TimeoutError)):
        message = 'The required 3DS files could not be downloaded. Check your connection, then choose Try again.'
    else:
        message = ('The required 3DS files could not be downloaded or saved. Check your connection, free space '
                   'and access to the work folder, then choose Try again.')
    refused = toolchain.Refused(message + ' Your installed devkitPro was not changed.')
    refused.details = str(error)
    return refused


def _download_file(pin, url, dest, size, sha, opener):
    temporary = dest.with_name(dest.name + '.part')
    try:
        request = urllib.request.Request(url, headers={'User-Agent': 'ctrport-builder/' + toolchain.builder_version()})
        with opener(request, timeout=60) as response, temporary.open('wb') as stream:
            count, hashed = 0, hashlib.sha256()
            while True:
                block = response.read(min(65536, size + 1 - count))
                if not block:
                    break
                count += len(block)
                hashed.update(block)
                stream.write(block)
                if count > size:
                    break
        if count != size or hashed.hexdigest() != sha:
            raise toolchain.Refused('The download did not match the required file and was discarded. Choose Try again. '
                'If it still fails, check the release page for an updated builder. '
                'Your installed devkitPro was not changed.\n%s' % RELEASE_PAGE)
        os.replace(temporary, dest)
    except urllib.error.HTTPError as error:
        status = error.code
        error.close()
        if status == 404:
            raise toolchain.Refused("A required 3DS file could not be found on devkitPro's download server. "
                'Try again later. If it still fails, check the UNDERTALE 3DS release page for an updated builder. '
                'Your installed devkitPro was not changed.\n%s' % RELEASE_PAGE) from None
        raise download_error(error, dest.parent) from error
    except OSError as error:
        if toolchain.certificate_failure(error):
            raise toolchain.Refused('Could not verify the devkitPro download certificate. Check your computer '
                'date and time, then try again. If it still fails, check the UNDERTALE 3DS release page. '
                'Your devkitPro is unchanged.\n' + RELEASE_PAGE) from None
        raise download_error(error, dest.parent) from error
    finally:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass  # Preserve the original failure; cleanup may remove the private partial file later.


def download_package(pin, cache, opener):
    # Never overwrite unknown files. Damaged entries are ignored and kept for the
    # ownership-aware cleanup; every new transaction has its own private folder.
    downloads = cache / 'downloads'
    try:
        downloads.mkdir(exist_ok=True)
    except OSError as error:
        raise download_error(error, downloads) from error
    for directory in sorted(downloads.glob('pkg-*')):
        try:
            verify_package(directory, (pin,))
            return directory
        except (OSError, ValueError):
            continue
    try:
        directory = Path(tempfile.mkdtemp(prefix='pkg-', dir=downloads))
    except OSError as error:
        raise download_error(error, downloads) from error
    try:
        name = filename(pin)
        _download_file(pin, SERVER + name, directory/name, pin['size'], pin['sha256'], opener)
        _download_file(pin, SERVER + name + '.sig', directory/(name+'.sig'), 566, pin['signature'], opener)
        verify_package(directory, (pin,))
        return directory
    except BaseException:
        shutil.rmtree(directory, ignore_errors=True)
        raise


def posix_path(path, platform):
    value = str(path).replace('\\', '/')
    return '/' + value[0].lower() + value[2:] if platform == 'win32' and re.match(r'^[A-Za-z]:/', value) else value


def native_path(path, platform):
    if platform == 'win32' and re.match(r'^/[A-Za-z]/', path):
        return path[1].upper() + ':' + path[2:]
    return path.replace('\\', '/')


class ToolFailed(ValueError):
    """A tool that unpacks the libraries failed: the message says which tool and why; details go to the log."""
    def __init__(self, message, details=''):
        super().__init__(message)
        self.details = details


def command(argv, cwd=None, run=None):
    argv = list(map(str, argv))
    try:
        code, output = (run or toolchain.run_tool)(argv, timeout=90, cwd=cwd)
    except OSError as error:
        code, output = None, str(error)
    if code == 0:
        return output
    summary, details = toolchain.tool_failure(argv, code, output)
    if code is None or toolchain.exit_cause(code):
        # It never ran: the same fix as the compiler page's check of CMake and make.
        message = (summary + ' ' + toolchain.PARTLY_UPDATED +
                   ' Click Update devkitPro on the compiler page, then try the build again.')
    else:
        tail = (output or '').strip()[-1500:]
        message = 'The library archive could not be unpacked. ' + summary + ('\n' + tail if tail else '')
    raise ToolFailed(message, details)


def inventory(root):
    result = {}
    for path in root.rglob('*'):
        info = path.lstat()
        if path.is_symlink() or getattr(info, 'st_file_attributes', 0) & 0x400:
            raise ValueError('The library copy contains a linked path.')
        if path.is_file():
            result[path.relative_to(root).as_posix()] = digest(path)
    return result


def library_path(sdk, pin):
    return sdk/pin['prefix']/'lib'/pin['library']


def _unpack(pin, bundle, stage, cmake, run):
    archive = bundle/filename(pin)
    # Each use validates both the package and its original detached signature.
    verify_package(bundle, (pin,))
    members = run([cmake, '-E', 'tar', 'tf', archive]).splitlines()
    if not members or any(PurePosixPath(p).is_absolute() or '..' in PurePosixPath(p).parts or
                          '\\' in p or ':' in p for p in members):
        raise ValueError('The library archive contains an unsafe path.')
    run([cmake, '-E', 'tar', 'xf', archive], cwd=stage)
    payload = stage/'opt/devkitpro'
    inventory(payload)  # No links may enter the mutable work tree.
    if digest(library_path(payload, pin)) != pin['library_sha256']:
        raise ValueError('The packaged library does not match the qualified library.')
    return payload


def _cached(cache, identity):
    for receipt in sorted(cache.glob('sdk-*/receipt.json')):
        try:
            record = json.loads(receipt.read_text(encoding='utf-8'))
            if record.get('identity') == identity and inventory(receipt.parent/'sdk') == record['files']:
                return receipt.parent
        except (OSError, ValueError, KeyError):
            continue  # Preserve incomplete or changed copies; cleanup requires its own ownership proof.
    return None


def _quote_cmake(value):
    return str(value).replace('\\','/').replace('"','\\"').replace('$','\\$')


def _write_lf(path, text):
    with path.open('w', encoding='utf-8', newline='\n') as stream:
        stream.write(text)


def write_wrapper(folder, tc, sdk, cmake):
    folder.mkdir(parents=True, exist_ok=True)
    original = posix_path(Path(tc.root)/'cmake/3DS.cmake', tc.platform)
    ctr = posix_path(sdk/'libctru', tc.platform)
    ports = posix_path(sdk/'portlibs/3ds', tc.platform)
    wrapper = folder/'toolchain.cmake'
    override = folder/'rules.cmake'
    _write_lf(wrapper, 'set(ENV{DEVKITPRO} "%s")\nset(ENV{DEVKITARM} "%s/devkitARM")\n'
        'include("%s")\nset(CTR_ROOT "%s")\n'
        'list(PREPEND CMAKE_SYSTEM_PREFIX_PATH "%s" "%s")\n'
        'list(PREPEND CMAKE_PREFIX_PATH "%s")\n'
        'unset(TREMOR_INCLUDE_DIR CACHE)\nunset(TREMOR_LIBRARY CACHE)\nunset(OGG_LIBRARY CACHE)\n'
        'set(CMAKE_USER_MAKE_RULES_OVERRIDE "%s")\n' % tuple(map(_quote_cmake,
        (posix_path(tc.root,tc.platform), posix_path(tc.root,tc.platform),
         original, ctr, ctr, ports, ports, posix_path(override, tc.platform)))))
    # The official platform caches its include/link flags. Refresh just the
    # library prefixes on a retry or when returning to matching installed files.
    _write_lf(override, 'include("%s")\n'
        'foreach(lang C CXX ASM)\n'
        '  set(CMAKE_${lang}_STANDARD_INCLUDE_DIRECTORIES "%s/include" CACHE STRING "" FORCE)\n'
        'endforeach()\n'
        'if(DEFINED CMAKE_EXE_LINKER_FLAGS)\n  set(_builder_flags "${CMAKE_EXE_LINKER_FLAGS}")\n'
        'else()\n  set(_builder_flags "${CMAKE_EXE_LINKER_FLAGS_INIT}")\nendif()\n'
        'foreach(_root "${BUILDER_LIBRARY_CTR_ROOT}" "${DEVKITPRO}/libctru" "%s")\n'
        '  if(NOT _root STREQUAL "")\n'
        '    string(REPLACE "-L\\\"${_root}/lib\\\"" "" _builder_flags "${_builder_flags}")\n'
        '    string(REPLACE "-L${_root}/lib" "" _builder_flags "${_builder_flags}")\n'
        '  endif()\nendforeach()\n'
        'set(CMAKE_EXE_LINKER_FLAGS "-L\\\"%s/lib\\\" ${_builder_flags}" CACHE STRING "" FORCE)\n'
        'set(BUILDER_LIBRARY_CTR_ROOT "%s" CACHE INTERNAL "" FORCE)\n' % tuple(map(_quote_cmake,
        (posix_path(Path(tc.root)/'cmake/dkp-rule-overrides.cmake', tc.platform), ctr, ctr, ctr, ctr))))
    python = posix_path(toolchain.python_exe(), tc.platform)
    module = posix_path(Path(__file__).resolve(), tc.platform)
    if tc.platform == 'win32':
        executable = posix_path(Path(tc.bash).parent/'cmake.exe', tc.platform)
    else:
        # The forwarding shim needs an absolute host executable even when no
        # extraction is needed; calling bare "cmake" would invoke this shim again.
        executable = cmake or toolchain.host_tool('cmake', str(Path(folder).parents[2]/'tools/bin'))
        if not executable:
            raise toolchain.Refused('CMake was not found. Click Check again on the compiler page, then try again.')
    shim = folder/'cmake'
    # Reconfigure cached compiler/include paths whenever the effective SDK changes.
    identity = hashlib.sha256(json.dumps(dict(root=str(Path(tc.root).resolve()),
        sdk=str(sdk.resolve()), libraries={p['package']: digest(library_path(sdk,p))
            for p in PACKAGES if library_path(sdk,p).is_file()}),sort_keys=True).encode()).hexdigest()
    _write_lf(shim, '#!/bin/bash\nargs=()\nbuild=\nprevious=\nfor arg in "$@"; do\n'
        'if [ "$previous" = -B ]; then build="$arg"; fi\n'
        'case "$arg" in -B?*) build="${arg:2}" ;; -DCMAKE_TOOLCHAIN_FILE=*) arg=%s ;; esac\n'
        'previous="$arg"\nargs+=("$arg")\ndone\n'
        'if [ "${1:-}" = --build ]; then\n  %s -X utf8 -I -B %s --check-links "$2" %s || exit $?\nfi\n'
        'if [ -n "$build" ]; then\n'
        '  old=\n  if [ -f "$build/.builder-library-root" ]; then read -r old < "$build/.builder-library-root"; fi\n'
        '  if [ "$old" != %s ]; then\n'
        '    %s -E rm -rf -- "$build/CMakeCache.txt" "$build/CMakeFiles" || exit $?\n'
        '  fi\n'
        'fi\n'
        '%s "${args[@]}" || exit $?\n'
        'if [ -n "$build" ]; then printf "%%s\\n" %s > "$build/.builder-library-root"; fi\n' %
        (shlex.quote('-DCMAKE_TOOLCHAIN_FILE='+posix_path(wrapper,tc.platform)),
         shlex.quote(python),shlex.quote(module),shlex.quote(tc.platform),shlex.quote(identity),
         shlex.quote(executable),shlex.quote(executable),shlex.quote(identity)))
    shim.chmod(0o755)
    return str(folder)


def stable_wrapper(cache, tc, sdk, cmake):
    """The same toolchain, SDK and builder give the same wrapper folder, so the CMake toolchain file a build folder
    was configured with doesn't move between runs. A new folder each run made every rerun print CMake's "You have
    changed variables that require your cache to be deleted" (11-17 playtester). A folder is reused only when its
    receipt, written last, matches every file in it; a folder that fails that is never touched, and a new one is
    allocated instead."""
    with open(__file__, 'rb') as stream:
        source = hashlib.sha256(stream.read()).hexdigest()
    key = hashlib.sha256(json.dumps(dict(
        root=str(Path(tc.root).resolve()), platform=tc.platform, bash=str(tc.bash or ''), sdk=str(Path(sdk).resolve()),
        cmake=str(cmake or ''), python=str(toolchain.python_exe()), module=str(Path(__file__).resolve()),
        source=source, libraries={p['package']: digest(library_path(Path(sdk),p))
                                  for p in PACKAGES if library_path(Path(sdk),p).is_file()}),
        sort_keys=True).encode()).hexdigest()[:20]
    for n in range(100):
        folder = cache/('run-%s%s' % (key, '-%d' % n if n else ''))
        receipt = folder/'.receipt.json'
        if not folder.exists():
            break
        try:
            record = json.loads(receipt.read_text(encoding='utf-8'))
            files = {p.name: digest(p) for p in folder.iterdir() if p.name != receipt.name}
            if record == dict(key=key, files=files):
                return str(folder)
        except (OSError, ValueError):
            pass                                    # changed or unreadable: left as it is, never reused
    try:
        folder.mkdir()
    except OSError:
        folder = Path(tempfile.mkdtemp(prefix='run-', dir=cache))
        receipt = folder/'.receipt.json'
    write_wrapper(folder, tc, sdk, cmake)
    files = {p.name: digest(p) for p in folder.iterdir() if p.name != receipt.name}
    _write_lf(receipt, json.dumps(dict(key=key, files=files), sort_keys=True))
    return str(folder)


def select(tc, work, notify=lambda text: None, *, packages=PACKAGES, run=command, opener=None):
    """Fetch mismatches only; never write devkitPro or use an unqualified fallback."""
    if not tc.root or getattr(tc, 'can_build', True) is False:
        raise toolchain.Refused("The 3DS compiler wasn't found. Click Check again on the compiler page, "
                                "then install the missing 3DS tools.")
    stack = require_stack(tc)
    work, installed = Path(work).resolve(), Path(tc.root)
    opener = opener or toolchain.https_open
    rows, changed = [], []
    for pin in packages:
        path = library_path(installed, pin)
        found = tc.package_versions.get(pin['package'])
        matches = path.is_file() and digest(path) == pin['library_sha256']
        # Optional audio packages are not linked when absent. Required graphics
        # packages are checked again against the actual CMake link command.
        optional_absent = pin['prefix'] == 'portlibs/3ds' and not path.exists() and found is None
        mismatch = not optional_absent and (not matches or (found is not None and found != pin['version']))
        rows.append(dict(package=pin['package'], installed=found, effective=pin['version'] if matches or mismatch else None,
                         tested=pin['version'], source='private' if mismatch else 'installed',
                         linked_candidate=not optional_absent))
        if mismatch:
            changed.append(pin)
    # Detection refuses an incomplete SDK. Keep source-only preflights useful;
    # build.sh cannot configure a compile without the original toolchain file.
    if not (installed/'cmake/3DS.cmake').is_file():
        return dict(packages=rows, shims=None)
    cmake = None
    cache = work/'bin/libraries'
    try:
        cache.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        raise download_error(error, cache) from error
    sdk = installed
    if changed:
        # Windows installs CMake beside the selected MSYS bash, not on the
        # desktop process's PATH. Use the same executable as the build shim.
        cmake = (str(Path(tc.bash).parent/'cmake.exe') if tc.platform == 'win32'
                 else toolchain.host_tool('cmake', str(work/'tools/bin')))
        if not cmake:
            raise toolchain.Refused('A tool needed to prepare the 3DS files is missing. Go back to the compiler page, '
                'choose Check again, install CMake if offered, then try the build again.')
        archives = {pin['package']: download_package(pin, cache, opener) for pin in changed}
        # All directories are newly allocated. A stale or unowned directory is
        # never overwritten or deleted, even after an interrupted extraction.
        source_files = {}
        for relative in ('libctru','portlibs/3ds/include','portlibs/3ds/lib'):
            if (installed/relative).is_dir():
                source_files[relative] = inventory(installed/relative)
        identity = hashlib.sha256(json.dumps(dict(sdk=str(installed.resolve()), files=source_files,
            packages=[p['sha256'] for p in changed]),sort_keys=True).encode()).hexdigest()
        saved = _cached(cache, identity)
        if saved is None:
            saved = Path(tempfile.mkdtemp(prefix='sdk-',dir=cache))
            sdk = saved/'sdk'
            sdk.mkdir()
            try:
                for relative in source_files:
                    shutil.copytree(installed/relative,sdk/relative,dirs_exist_ok=True)
                for pin in changed:
                    with tempfile.TemporaryDirectory(prefix='.extract-',dir=saved) as temporary:
                        payload = _unpack(pin,archives[pin['package']],Path(temporary),cmake,run)
                        shutil.copytree(payload,sdk,dirs_exist_ok=True)
                # Refuse before a compile if a matching installed neighbour was
                # absent or changed during the overlay. Nothing in the SDK is written.
                for pin in packages:
                    path = library_path(sdk,pin)
                    if path.exists() and digest(path) != pin['library_sha256']:
                        raise ValueError('The selected library is not the tested version: '+pin['package'])
                record = dict(identity=identity, files=inventory(sdk))
                (saved/'receipt.json').write_text(json.dumps(record,sort_keys=True)+'\n',encoding='utf-8')
            except (OSError,ValueError,subprocess.SubprocessError) as error:
                refused = toolchain.Refused(REFUSAL+'\n'+str(error))
                refused.details = getattr(error, 'details', '')
                raise refused from error
        sdk = saved/'sdk'
        for pin in changed:
            notify('Using tested %s %s from the work folder. Your devkitPro is unchanged.' %
                   (pin['package'],pin['version']))
    return dict(packages=rows, stack=stack, shims=stable_wrapper(cache,tc,sdk,cmake), sdk=str(sdk))


def actual_linked(build, platform, packages=PACKAGES):
    """Resolve the four qualified libraries from the configured link command."""
    build = Path(native_path(str(build),platform))
    text = (build/'CMakeFiles/ctrport.dir/link.txt').read_text(encoding='utf-8')
    args = shlex.split(text)
    allowed = {pin['library'][3:-2] for pin in packages} | {'m', 'c', 'g', 'gcc', 'sysbase'}
    unknown = [arg for arg in args if arg.startswith('-l') and arg[2:] not in allowed]
    if unknown:
        raise ValueError('The link command names an unpinned library: ' + ', '.join(unknown))
    directories = [Path(native_path(arg[2:],platform)) for arg in args if arg.startswith('-L')]
    selected = []
    for pin in packages:
        flag = '-l'+pin['library'][3:-2]
        explicit = [Path(native_path(arg,platform)) for arg in args if arg.endswith('/'+pin['library'])]
        if flag not in args and not explicit:
            if pin['package'] in ('libctru','citro3d'):
                raise ValueError('The link command does not name '+pin['package'])
            continue
        choices = explicit or [directory/pin['library'] for directory in directories]
        path = next((path for path in choices if path.is_file()),None)
        if path is None or digest(path) != pin['library_sha256']:
            raise ValueError('The selected '+pin['package']+' is not the tested library.')
        selected.append(dict(package=pin['package'],version=pin['version'],path=str(path.resolve()),
                             sha256=digest(path),bytes=path.stat().st_size))
    return selected


def linked_files(map_path, platform):
    """Hash archives whose members GNU ld's map says contributed to the ELF."""
    path = Path(map_path)
    if not path.is_file():
        return []
    files = set()
    header = path.read_text(encoding='utf-8',errors='replace').split('Discarded input sections',1)[0]
    for line in header.splitlines():
        match = re.match(r'^([^\s].*?\.a)\([^()]+\)',line)
        if match:
            item = Path(native_path(match.group(1),platform))
            files.add((item if item.is_absolute() else path.parent/item).resolve())
    return [dict(path=str(item),sha256=digest(item),bytes=item.stat().st_size) for item in sorted(files)]


COMPILER_NAMES = ('arm-none-eabi-gcc', 'arm-none-eabi-gcc.exe', 'arm-none-eabi-g++', 'arm-none-eabi-g++.exe')


def link_compiler(args, platform):
    """The qualified 3DS compiler that a configured link command runs, or None.

    A plain link command starts with the compiler, as before. UNDERTALE's link also passes through a launcher
    (aacout's link check, platform/3ds/aacout.cmake's RULE_LAUNCH_LINK): its own command, then '--', then the
    compiler's command, which the launcher runs as given (tools/buildcache/aacout_compile.py runs exactly the argument
    after its separator). So the compiler is that one argument, right after the first '--': never a later operand
    (an output or input path can carry a compiler's name) and never anything past a second '--'. Each candidate is
    resolved first and then held to the exact names, as the plain case always was. Nothing else stands in for it:
    otherwise this returns None, and the caller allows no compiler runtime (it fails closed)."""
    def compiler(arg):
        path = Path(native_path(arg, platform)).resolve()
        return path if path.name in COMPILER_NAMES else None
    if not args:
        return None
    plain = compiler(args[0])
    if plain is not None:
        return plain
    if '--' not in args:
        return None
    slot = args.index('--') + 1
    return compiler(args[slot]) if slot < len(args) else None


def confirm_linked(build, platform, packages=PACKAGES):
    """The map must confirm the configured qualified files, including their location."""
    build=Path(build)
    configured=actual_linked(build,platform,packages)
    linked=linked_files(build/'release-link.map',platform)
    used={os.path.normcase(str(Path(row['path']).resolve())):row for row in linked}
    for row in configured:
        key=os.path.normcase(str(Path(row['path']).resolve()))
        if key not in used or used[key]['sha256'] != row['sha256']:
            raise ValueError('The linker did not use the checked '+row['package']+' library. '
                'Choose a new work folder and try again. Your installed devkitPro is unchanged.')
    expected = {row['sha256'] for row in configured}
    stack = json.loads(Path(__file__).with_name('sdk-stack-pins.json').read_text(encoding='utf-8'))
    expected.update(stack['runtime_archives'].values())
    local = {'libbenchromfs_mount.a', 'librelprep_romfs.a', 'libswkbd_late.a'}
    # libgcc belongs to the host-specific, version-gated compiler package. Its
    # debug/archive bytes can differ by host; never apply a Windows file hash to
    # Mac/Linux. Require the exact qualified compiler's private runtime location.
    args = shlex.split((build/'CMakeFiles/ctrport.dir/link.txt').read_text(encoding='utf-8'))
    compiler = link_compiler(args, platform)
    compiler_runtime = (compiler.parent.parent/'lib/gcc/arm-none-eabi/16.1.0/armv6k/fpu/libgcc.a').resolve() \
        if compiler is not None else None
    for row in linked:
        path = Path(row['path']).resolve()
        project = path.parent == build.resolve() and path.name in local
        compiler_file = compiler_runtime is not None and path == compiler_runtime
        if row['sha256'] not in expected and not project and not compiler_file:
            raise ValueError('An unpinned library contributed to the game: ' + str(path))
    return linked


if __name__ == '__main__':
    try:
        if len(sys.argv) != 4 or sys.argv[1] != '--check-links':
            raise ValueError('Missing build folder for the library check.')
        rows = actual_linked(sys.argv[2],sys.argv[3])
        print('Qualified link libraries: '+json.dumps(rows,sort_keys=True),flush=True)
    except (OSError,ValueError) as error:
        print('LIBRARY REFUSED: '+REFUSAL+'\n'+str(error),flush=True)
        sys.exit(2)
