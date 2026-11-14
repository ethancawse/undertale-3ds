"""Load explicitly inventoried deferred Python edges in the produced source export. Host only."""
import argparse
import importlib.util
from pathlib import Path
import sys
import tempfile

# Collection does not execute these function bodies/branches. Keep the runtime
# inventory explicit even where the static layer can also resolve their paths.
LAZY_LOADS = (
    ('tools/analysis/utwindow_compile.py', 'tools/buildcache/dimlight_compile.py',
     'route_lcd loads its adapter only after the renderer assembly is emitted.'),
    ('tools/buildcache/test_dimlight_compile.py', 'tools/analysis/utwindow_compile.py',
     'The assembly-chain test loads its delegate inside the test body.'),
    ('tools/analysis/test_credclip.py', 'tools/aot/credits.py',
     'gap_is_undertale_only loads the formatter only when the synthetic roll test runs.'),
    ('tools/analysis/test_dlgflash.py', 'tools/buildcache/dlgflash_compile.py',
     'The compile tests load the delegate inside test bodies.'),
    ('tools/analysis/test_dlgflash.py', 'tools/analysis/vine_extract.py',
     'The assembler-chain test loads the extractor inside its test body.'),
    ('tools/buildcache/test_public_benchfile_compile.py', 'tools/buildcache/fpsghost_return_compile.py',
     'The nested launcher test loads this delegate inside its test body.'),
    ('tools/buildcache/test_public_benchfile_compile.py', 'tools/buildcache/sfxlate_compile.py',
     'The sound-delegate tests load this adapter inside test bodies.'),
    ('tools/release/test_texcache.py', 'tools/release/export_scrub.py',
     'The scrub loader is inside the native-tool test, which can skip without that tool.'),
    ('tools/release/test_builder.py', 'tools/aot/data_versions.py',
     'The version-table loader runs inside the test, not at collection.'),
)


def load(root, relative):
    path = (root / relative).resolve(strict=True)
    if not path.is_relative_to(root.resolve()) or path.suffix != '.py':
        raise ValueError('Deferred Python input is outside the export: ' + relative)
    spec = importlib.util.spec_from_file_location('export_probe_' + path.stem, path)
    module = importlib.util.module_from_spec(spec)
    # Match a direct tool launch's sibling imports; isolated Python excludes cwd.
    # Registration also preserves Python's normal dataclass/module lookup contract.
    previous_path = sys.path[:]
    previous_module = sys.modules.get(spec.name)
    sys.path.insert(0, str(path.parent))
    sys.modules[spec.name] = module
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path[:] = previous_path
        if previous_module is None:
            sys.modules.pop(spec.name, None)
        else:
            sys.modules[spec.name] = previous_module
    return module


def check(root, entries=LAZY_LOADS):
    for owner, target, reason in entries:
        if not reason.strip():
            raise ValueError('Deferred input needs a reason: ' + target)
        # Product-specific tests may be absent; never exempt an input of an owner that ships.
        if (root / owner).is_file():
            load(root, target)
            print('LOADED ' + target + ' (' + owner + ': ' + reason + ')')
    # Exercise the real deferred adapter call with invented assembly, without any compiler.
    adapter = load(root, 'tools/analysis/utwindow_compile.py')
    assembly = ''.join(name + ':\n' + '\tbl screensTopBacklight\n' * count +
                       '\t.size ' + name + ', .-' + name + '\n' for name, count in
                       (('screensConfigureKeepBottom', 2), ('screensDestroy', 1),
                        ('__wrap_CtrRenderer_leaveForExit', 1)))
    routed, receipt = adapter.route_lcd(assembly)
    assert receipt['changed_operands'] == 4 and 'screensTopBacklight' not in routed
    # Capture's decoder is loaded by filename; exercise its API on invented pixels.
    capture = load(root, 'tools/analysis/capture_png.py')
    with tempfile.TemporaryDirectory() as directory:
        receipt = capture.write_png(Path(directory) / 'probe.png', 1, 1, [b'\x01\x02\x03'])
        assert receipt['pixel_mismatches'] == 0
    # This optional extension is deliberately private. Importing the shipped module
    # must work without it; no font or game container is opened.
    if (root / 'tools/analysis/stglyphs_extra.py').exists():
        raise ValueError('Expected the release export, without optional analysis font tables')
    fonts = load(root, 'tools/aot/stglyphs.py')
    assert isinstance(fonts.font_params('invented-font'), dict)


def main():
    argparse.ArgumentParser(description=__doc__).parse_args()
    check(Path(__file__).resolve().parents[2])
    print('Deferred export loads passed; no compiler or game inputs used.')


if __name__ == '__main__':
    main()
