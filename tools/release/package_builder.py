#!/usr/bin/env python3
"""Package the shared builder from an already curated per-game release export. Does not publish or install.

    py -3 -I tools/release/package_builder.py --source scratch/export --game undertale --platform linux --output scratch/dist

Windows also needs --runtime with a prepared python.org embeddable runtime and matching Tcl/Tk.
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

EXTRA_FILES = ("tools/release/verify_game.py", "tools/release/release_contract.py",
               "tools/release/music-versions.json", "tools/release/test_builder.py",
               "tools/release/test_builder_platforms.py", "tools/release/builder_platform_probe.py",
               "tools/release/package_builder.py", "tools/release/test_builder_native_data.py",
               "tools/release/test_texcache.py")
DIRECTORIES = pipeline.SOURCE_DIRS + ("tools/release/builder",)


def opening_instructions(game, platform=None):
    """Approved unsigned-beta wording (2026-10-26); name the actual launchers.

    Checksum sidecars are optional release metadata, never a player setup step.
    Wrapper packaging will substitute its EXE/app names when those exist.
    """
    lines = ["## Opening the builder",
             "This beta builder is unsigned. Windows or macOS may warn you the first time you open it."]
    if platform in (None, "windows"):
        lines.append('Windows: Extract all files, then open %s-3ds-builder.cmd. '
                     'If Windows shows "Windows protected your PC", choose "More info", then "Run anyway".' % game)
    if platform in (None, "macos"):
        lines.append('Mac: Unpack the download, then open %s-3ds-builder.command. '
                     'If macOS blocks it, open System Settings > Privacy & Security, '
                     'choose "Open Anyway", then "Open".' % game)
    return "\n\n".join(lines) + "\n"


def launcher(game, platform):
    if platform == "windows":
        return ("@echo off\r\nsetlocal\r\nset \"HERE=%~dp0tools\\release\\builder\"\r\n"
                'if "%~1"=="" (\r\n  start "" "%HERE%\\runtime\\pythonw.exe" -I "%HERE%\\builder.py" --game ' + game +
                '\r\n  exit /b\r\n)\r\n' +
                '"%HERE%\\runtime\\python.exe" -I "%HERE%\\builder.py" --game ' + game + ' %*\r\n')
    python = 'python3'
    if platform == "macos":
        python = '"${BUILDER_PYTHON:-/Library/Frameworks/Python.framework/Versions/Current/bin/python3}"'
    check = ''
    if platform == "macos":
        check = ('if [ ! -x ' + python + ' ]; then\n'
                 '  echo "Install Python from python.org (macOS universal2, with Tk), then try again."\n'
                 '  exit 2\nfi\n')
    return ('#!/bin/sh\nset -eu\nHERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n' + check + 'exec ' + python +
            ' -I "$HERE/tools/release/builder/builder.py" --game ' + game + ' "$@"\n')


def inputs(root, game, runtime=None):
    result = {}
    for rel in pipeline.SOURCE_FILES + EXTRA_FILES:
        p = root / rel
        if not p.is_file():
            raise ValueError("Incomplete curated export: missing " + rel)
        result[rel] = p.read_bytes()
    for rel in DIRECTORIES:
        directory = root / rel
        if not directory.is_dir():
            raise ValueError("Incomplete curated export: missing " + rel)
        for p in sorted(directory.rglob("*")):
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
    if runtime:
        required = ("python.exe", "pythonw.exe", "_tkinter.pyd", "_multiprocessing.pyd",
                    "tcl86t.dll", "tk86t.dll", "tkinter/__init__.py",
                    "LICENSE.txt", "tcl/tcl8.6/license.terms", "tcl/tk8.6/license.terms")
        for rel in required:
            if not (runtime / rel).is_file() or not (runtime / rel).stat().st_size:
                raise ValueError("Prepared Windows runtime is missing " + rel)
        if not (runtime / "tcl").is_dir():
            raise ValueError("Prepared Windows runtime is missing tcl/")
        for p in sorted(runtime.rglob("*")):
            if p.is_symlink():
                raise ValueError("Prepared Windows runtime contains a symlink")
            if p.is_file():
                result["tools/release/builder/runtime/" + p.relative_to(runtime).as_posix()] = p.read_bytes()
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


def package(root, output, game, platform, runtime=None, texprep=None, release_descriptor=None):
    version = pipeline.release_version(str(root))
    if platform == "windows" and runtime is None:
        raise ValueError("Windows needs a prepared embeddable runtime: --runtime DIR")
    if platform != "windows" and runtime is not None:
        raise ValueError("Only the Windows archive embeds Python; Unix uses its installed Python with Tk")
    files = inputs(root, game, runtime)
    # Only this platform's pinned texprep goes in; a stray or stale binary from the export never does.
    for rel in [r for r in files if r.startswith(BIN)]:
        del files[rel]
    if texprep is not None:
        rel, data = texprep_entry(platform, texprep.read_bytes())
        files[rel] = data
    contract = pipeline.products.release_contract
    digest = contract.payload_digest({rel: files[rel] for rel in pipeline.source_payload(root, game)})
    descriptor = contract.source_descriptor(game, version, digest)
    if release_descriptor is None and (root / contract.DESCRIPTOR).is_file():
        release_descriptor = root / contract.DESCRIPTOR
    if release_descriptor is not None:
        descriptor = json.loads(Path(release_descriptor).read_text(encoding='utf-8'))
        contract.validate_source(descriptor, game, version, digest)
    files[contract.DESCRIPTOR] = (json.dumps(descriptor, indent=2) + '\n').encode('utf-8')
    suffix = {"windows": ".cmd", "linux": ".sh", "macos": ".command"}[platform]
    start = game + "-3ds-builder" + suffix
    files[start] = launcher(game, platform).encode("utf-8")
    if platform in ("windows", "macos"):
        files["README.md"] = opening_instructions(game, platform).encode("utf-8")
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
    p.add_argument('--release-descriptor', type=Path,
                   help='Owner-approved exact public commit and source digest; unset kits cannot build')
    o = p.parse_args()
    try:
        print(package(o.source.resolve(), o.output.resolve(), o.game, o.platform,
                      o.runtime.resolve() if o.runtime else None, o.texprep.resolve() if o.texprep else None,
                      o.release_descriptor))
    except (OSError, ValueError, pipeline.BuildFailed) as e:
        p.error(str(e))


if __name__ == "__main__":
    main()
