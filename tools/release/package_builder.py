#!/usr/bin/env python3
"""Package the shared builder from an already curated per-game release export. Does not publish or install.

    py -3 -I tools/release/package_builder.py --source scratch/export --game undertale --platform linux --output scratch/dist

Windows also needs --runtime with a prepared python.org embeddable runtime and matching Tcl/Tk.
--dsp-library FILE adds the pinned Windows x64 sound encoder to UNDERTALE binary ZIPs only.
macOS: --mac-runtime-arm64, --mac-runtime-x86_64 and --mac-licences produce one .app ZIP
with pinned Python/Tk. The legacy script-only tar remains for prior rehearsals.
--texprep FILE adds the platform's texprep binary (texture preparation on the PC, builder/texcache.py) after checking
it against its pinned size and SHA-256; without it the package's builder leaves the textures to the console (or, on
Linux and macOS, builds texprep when a compiler is present).
Never point --source at a player's game folder. Run the release content gate on the export before packaging.
"""
import argparse
import io
import json
from pathlib import Path
import re
import sys
import tarfile
import zipfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent / "builder"))
import pipeline
import texcache
import sfx_encoder
import runtime_pins
import path_hygiene

EXTRA_FILES = ("tools/release/verify_game.py", "tools/release/release_contract.py",
               "tools/release/sfxpack-versions.json",
               "tools/release/music-versions.json", "tools/release/test_builder.py",
               "tools/release/test_builder_platforms.py", "tools/release/builder_platform_probe.py",
               "tools/release/package_builder.py", "tools/release/test_builder_native_data.py",
               "tools/release/test_texcache.py", "tools/release/check_kit_hygiene.py", "tools/release/export_scrub.py",
               "tools/release/export_pins.py", "tools/release/macos_bundle.py", "tools/release/runtime_pins.py",
               "tools/release/builder-runtime-pins.json",
               "tools/release/macos_runtime_check.py")
DIRECTORIES = pipeline.SOURCE_DIRS + ("tools/release/builder",)


def generated_cache(name):
    """Loose interpreter/test caches are host state, never packaging inputs.

    The vendor standard-library ZIP is retained byte for byte, including its
    source-less bytecode. Only loose generated files are excluded here.
    """
    return runtime_pins.generated_cache(name)


def validate_package_files(files):
    """Refuse private project paths/names in every final member and nested ZIP.

    Construct the search token so the guard itself remains safe to distribute.
    Explicit public-handle references are the only exception. Error messages
    identify a member/offset, never print the rejected content.
    """
    token = 'delta' + 'tale'
    patterns = []
    for encoding in ('ascii', 'utf-16le', 'utf-16be'):
        prefixes = b'|'.join(re.escape(s.encode(encoding))
                             for s in ('github.com/', 'x.com/', 'twitter.com/', '@'))
        boundary = b'|'.join(re.escape(c.encode(encoding)) for c in '/ \t\r\n"<>')
        needle = re.compile(re.escape(token.encode(encoding)), re.I)
        handle = re.compile(b'(?:' + prefixes + b')' + needle.pattern + b'(?=' + boundary + b'|$)', re.I)
        patterns.append((encoding, needle, handle))

    def inspect(label, data, depth=0):
        for encoding, needle, handle in patterns:
            # Byte regexes also find UTF-16 strings beginning at an odd offset.
            approved = [m.span() for m in handle.finditer(data)]
            for match in needle.finditer(data):
                if not any(start <= match.start() and match.end() <= end for start, end in approved):
                    raise ValueError('Private project name in package member %s at byte %d (%s)' %
                                     (label, match.start(), encoding))
        if zipfile.is_zipfile(io.BytesIO(data)):
            if depth >= 8:
                raise ValueError('Nested package ZIP exceeds scan depth: ' + label)
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                for member in archive.infolist():
                    if not member.is_dir():
                        child = label + '!' + member.filename
                        inspect(child + ' (name)', member.filename.encode('utf-8'), depth + 1)
                        inspect(child, archive.read(member), depth + 1)

    for rel, data in files.items():
        inspect(rel + ' (name)', rel.encode('utf-8'))
        inspect(rel, data)
    # The final extracted-kit scanner also guards all packaging inputs.
    # Public credits and generic system defaults follow its documented policy.
    import check_kit_hygiene
    hits = check_kit_hygiene.findings(files)
    if hits:
        hit = hits[0]
        raise ValueError('Kit hygiene: %s in %s at byte %d' %
                         (hit['rule'], hit['member'], hit['offset']))


def opening_instructions(game, platform=None):
    """Approved unsigned-beta wording (2026-10-26); name the actual launchers.

    Checksum sidecars are optional release metadata, never a player setup step.
    Wrapper packaging will substitute its EXE/app names when those exist.
    """
    lines = ["## Opening the builder"]
    if platform is None:
        lines.append('This beta builder is unsigned. Windows or macOS may warn you the first time you open it.')
    elif platform in ('windows', 'macos'):
        lines.append('This beta builder is unsigned. %s may warn you the first time you open it.' %
                     ('Windows' if platform == 'windows' else 'macOS'))
    if platform in (None, "windows"):
        lines.append('Windows: Extract all files, then open %s-3ds-builder.cmd. '
                     'If Windows shows "Windows protected your PC", choose "More info", then "Run anyway".' % game)
    if platform in (None, "macos"):
        lines.append('Mac: Unpack the download, then open %s-3ds-builder.command. '
                     'If macOS blocks it, open System Settings > Privacy & Security, '
                     'choose "Open Anyway", then "Open". '
                     'On macOS 11 or 12, use System Preferences > Security & Privacy > General, '
                     'then Open Anyway > Open.' % game)
    if platform in (None, "linux"):
        lines.append('Linux: Install Python 3.9 or newer with Tk, unpack the download, then run '
                     '`sh ./%s-3ds-builder.sh` in its folder.' % game)
        lines.append('On Debian or Ubuntu, install Python and Tk with '
                     '`sudo apt-get update && sudo apt-get install python3 python3-tk`.')
        lines.append('Use a distribution release that supplies Python 3.9 or newer.')
        lines.append("The builder's 3DS compiler page installs the remaining tools.")
    return "\n\n".join(lines) + "\n"


def launcher(game, platform):
    if platform == "windows":
        return ("@echo off\r\nsetlocal\r\nset \"ERRORLEVEL=\"\r\nset \"HERE=%~dp0tools\\release\\builder\"\r\n"
                'if not exist "%HERE%\\builder.py" goto unpack\r\n'
                'if not exist "%HERE%\\runtime\\pythonw.exe" goto unpack\r\n'
                'if not exist "%HERE%\\runtime\\python.exe" goto unpack\r\n'
                'if "%~1"=="" (\r\n  start "" "%HERE%\\runtime\\pythonw.exe" -X utf8 -I -B "%HERE%\\builder.py" --game ' + game +
                '\r\n  exit /b\r\n)\r\n' +
                '"%HERE%\\runtime\\python.exe" -X utf8 -I -B "%HERE%\\builder.py" --game ' + game + ' %*\r\n'
                'exit /b %errorlevel%\r\n:unpack\r\n'
                'echo Extract the whole builder ZIP first.\r\n'
                'echo Right-click the downloaded ZIP and choose Extract All.\r\n'
                'echo Then open ' + game + '-3ds-builder.cmd from the extracted folder.\r\n'
                'echo Keep all the extracted files together.\r\n'
                'if "%~1"=="" pause\r\nexit /b 2\r\n')
    python = 'python3'
    if platform == "macos":
        python = '"${BUILDER_PYTHON:-/Library/Frameworks/Python.framework/Versions/Current/bin/python3}"'
    check = ''
    if platform == "macos":
        check = ('if [ ! -x ' + python + ' ]; then\n'
                 '  echo "Install Python from python.org (macOS universal2, with Tk), then try again."\n'
                 '  exit 2\nfi\n')
    before = ''
    if platform == 'linux':
        before = ('if ! command -v python3 >/dev/null 2>&1; then\n'
                  '  printf "%s\\n" "The builder needs Python 3.9 or newer with Tk." '
                  '"On Debian or Ubuntu, install them with:" '
                  '"sudo apt-get update && sudo apt-get install python3 python3-tk" '
                  '"Use a distribution release that supplies Python 3.9 or newer, then open the builder again."\n'
                  '  exit 127\nfi\n')
    unpack = ('if [ ! -f "$HERE/tools/release/builder/builder.py" ]; then\n'
              '  printf "%s\\n" "Extract the whole builder download first." '
              '"Keep all the extracted files together, then open ' + game + '-3ds-builder' +
              ('.command' if platform == 'macos' else '.sh') + ' from the extracted folder." >&2\n'
              '  exit 2\nfi\n')
    return ('#!/bin/sh\nset -eu\n' + before + 'HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n' + unpack + check + 'exec ' + python +
            ' -X utf8 -I -B "$HERE/tools/release/builder/builder.py" --game ' + game + ' "$@"\n')


def inputs(root, game, runtime=None):
    runtime_files = runtime_pins.windows_files(runtime) if runtime is not None else {}
    result = {}
    for rel in pipeline.SOURCE_FILES + EXTRA_FILES:
        if generated_cache(rel) or not pipeline.source_wanted(rel, game):
            continue
        p = pipeline.source_input(root, rel, game)
        if not p.is_file():
            raise ValueError("Incomplete curated export: missing " + rel)
        result[rel] = p.read_bytes()
    for rel in pipeline.source_directories(game) + ("tools/release/builder",):
        directory = root / rel
        if not directory.is_dir():
            raise ValueError("Incomplete curated export: missing " + rel)
        for p in sorted(path_hygiene.iter_paths(directory)):
            if generated_cache(p.relative_to(root).as_posix()):
                continue
            if p.is_symlink():
                raise ValueError("Curated source contains a symlink: " + p.relative_to(root).as_posix())
            if p.is_file() and pipeline.source_wanted(p.relative_to(root).as_posix(), game):
                result[p.relative_to(root).as_posix()] = p.read_bytes()
    # The scaffolding is shared; only the selected game changes in each repo's package.
    key = "tools/release/builder/builder.py"
    text = result[key].decode("utf-8")
    text, changed = re.subn(r"(?m)^FIXED_GAME = .+$", 'FIXED_GAME = "%s"' % game, text)
    if changed != 1:
        raise ValueError("Builder game selection marker is missing or ambiguous")
    result[key] = text.encode("utf-8")
    import export_scrub
    import export_pins
    source_result = result
    result = export_scrub.scrub_files(result)
    # The kit builds from cleaned sources too: a content pin must list the cleaned bytes.
    export_pins.check(source_result, result)
    for rel, data in runtime_files.items():
        result["tools/release/builder/runtime/" + rel] = data
    return result


TEXPREP_KEYS = {"windows": ("win32", "x86_64"), "linux": ("linux", "x86_64"), "macos": ("darwin", "arm64")}
BIN = "tools/release/builder/bin/"


def texprep_entry(platform, data):
    """(published path, bytes) for a texprep binary that matches this platform's pin, else ValueError."""
    pin = texcache.PINNED.get(TEXPREP_KEYS[platform])
    if pin is None or pin[1] is None:
        raise ValueError("No pinned texprep for %s in texcache.PINNED" % platform)
    name, size, sha = pin
    if len(data) != size or pipeline.hashlib.sha256(data).hexdigest() != sha:
        raise ValueError("texprep binary does not match its pin (%s: %d bytes, SHA-256 %s)" % (name, size, sha))
    return BIN + name, data


def package(root, output, game, platform, runtime=None, texprep=None, release_descriptor=None,
            mac_runtimes=None, mac_licences=None, dsp_library=None):
    version = pipeline.release_version(str(root))
    if platform == "windows" and runtime is None:
        raise ValueError("Windows needs a prepared embeddable runtime: --runtime DIR")
    if platform != "windows" and runtime is not None:
        raise ValueError("Only the Windows archive embeds Python; Unix uses its installed Python with Tk")
    files = inputs(root, game, runtime)
    # Only this platform's pinned texprep goes in; a stray or stale binary from the export never does.
    for rel in [r for r in files if r.startswith(BIN)]:
        del files[rel]
    if platform == 'windows' and 'tools/release/builder/certs/LICENSE.certifi' in files:
        files[BIN+'certs/THIRD_PARTY_NOTICES.txt'] = (
            b'Mozilla CA certificates from certifi, MPL-2.0.\n'
            b'See tools/release/licenses/MPL-2.0 for the full licence.\n\n' +
            files['tools/release/builder/certs/LICENSE.certifi'])
    if texprep is not None:
        rel, data = texprep_entry(platform, texprep.read_bytes())
        files[rel] = data
    if dsp_library is not None:
        if game != 'undertale' or platform != 'windows':
            raise ValueError('The prebuilt sound encoder is for the UNDERTALE Windows kit only')
        files[sfx_encoder.RELATIVE] = sfx_encoder.validate(dsp_library, root)
    contract = pipeline.products.release_contract
    digest = contract.payload_digest({rel: files[rel] for rel in pipeline.source_payload(root, game)})
    descriptor = contract.source_descriptor(game, version, digest)
    if release_descriptor is None and (root / contract.DESCRIPTOR).is_file():
        release_descriptor = root / contract.DESCRIPTOR
    if release_descriptor is not None:
        descriptor = json.loads(Path(release_descriptor).read_text(encoding='utf-8'))
        contract.validate_source(descriptor, game, version, digest)
    files[contract.DESCRIPTOR] = (json.dumps(descriptor, indent=2) + '\n').encode('utf-8')
    if mac_runtimes is not None:
        if platform != 'macos' or not mac_licences:
            raise ValueError('Bundled Mac inputs require --platform macos and --mac-licences')
        import macos_bundle
        opening = opening_instructions(game, platform).replace(
            game + '-3ds-builder.command', game.upper() + ' 3DS Builder.app')
        return macos_bundle.write_app(files, output, game, version, mac_runtimes, opening, mac_licences)
    suffix = {"windows": ".cmd", "linux": ".sh", "macos": ".command"}[platform]
    start = game + "-3ds-builder" + suffix
    files[start] = launcher(game, platform).encode("utf-8")
    files["README.md"] = opening_instructions(game, platform).encode("utf-8")
    files['README.txt'] = (game.upper() + ' 3DS Builder\n\n' +
                          opening_instructions(game, platform).replace('## ', '').replace('`', '')).encode('utf-8')
    if platform == 'linux':
        files['README.txt'] = (game.upper() + ' 3DS Builder\n\n'
            'Before opening the builder, install Python 3.9 or newer with Tk.\n'
            'The builder window needs a graphical desktop.\n'
            'On Debian or Ubuntu, use:\n'
            'sudo apt-get update && sudo apt-get install python3 python3-tk\n\n'
            'Use a distribution release that supplies Python 3.9 or newer.\n\n'
            'Open a terminal in this folder and run:\n'
            'sh ./' + start + '\n\n'
            'The builder can guide you through setting up the remaining 3DS tools.\n').encode('utf-8')
    validate_package_files(files)
    stem = "%s-3ds-builder-%s-%s" % (game, version, platform)
    output.mkdir(parents=True, exist_ok=True)
    path = output / (stem + (".zip" if platform == "windows" else ".tar.gz"))
    if platform == "windows":
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
            for rel, data in sorted(files.items()):
                z.writestr(stem + "/" + rel, data)
    else:
        with tarfile.open(path, "w:gz") as t:
            for rel, data in sorted(files.items()):
                info = tarfile.TarInfo(stem + "/" + rel)
                info.size = len(data)
                info.mode = 0o755 if rel == start or rel.endswith(".sh") or rel.startswith(BIN) else 0o644
                t.addfile(info, io.BytesIO(data))
    sha = pipeline.sha256_file(path)
    path.with_name(path.name + ".sha256").write_bytes((sha + "  " + path.name + "\n").encode("ascii"))
    return path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--game", choices=("undertale", "deltarune"), required=True)
    p.add_argument("--platform", choices=("windows", "linux", "macos"), required=True)
    p.add_argument("--runtime", type=Path)
    p.add_argument("--texprep", type=Path, help="this platform's texprep binary (checked against its pin)")
    p.add_argument('--dsp-library', type=Path, help='pinned Windows x64 sound encoder DLL (binary kit only)')
    p.add_argument('--mac-runtime-arm64', type=Path)
    p.add_argument('--mac-runtime-x86_64', type=Path)
    p.add_argument('--mac-licences', type=Path)
    p.add_argument('--release-descriptor', type=Path,
                   help='Owner-approved exact public commit and source digest; unset kits cannot build')
    o = p.parse_args()
    try:
        mac = None
        if o.mac_runtime_arm64 or o.mac_runtime_x86_64 or o.mac_licences:
            if not (o.mac_runtime_arm64 and o.mac_runtime_x86_64 and o.mac_licences):
                raise ValueError('Provide both Mac runtime archives and their licensing directory')
            mac = dict(arm64=o.mac_runtime_arm64, x86_64=o.mac_runtime_x86_64)
        print(package(o.source.resolve(), o.output.resolve(), o.game, o.platform,
                      o.runtime.resolve() if o.runtime else None, o.texprep.resolve() if o.texprep else None,
                      o.release_descriptor, mac, o.mac_licences, dsp_library=o.dsp_library))
    except (OSError, ValueError, pipeline.BuildFailed) as e:
        p.error(str(e))


if __name__ == "__main__":
    main()
