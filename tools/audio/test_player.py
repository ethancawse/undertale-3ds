"""Compile and exercise the production native player against a simulated NDSP."""
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_pcm

ROOT = Path(__file__).resolve().parents[2]


class PlayerTests(unittest.TestCase):
    def test_native_queue_lifecycle(self):
        compiler = shutil.which('gcc') or shutil.which('clang')
        if not compiler:
            compiler = next((str(p) for p in (Path('C:/msys64/ucrt64/bin/gcc.exe'),
                Path('C:/devkitPro/msys2/ucrt64/bin/gcc.exe')) if p.is_file()), None)
        self.assertIsNotNone(compiler, 'Host C compiler required')
        out = ROOT / 'scratch/adpcm/host'
        out.mkdir(parents=True, exist_ok=True)
        executable = out / 'test-player.exe'
        subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
            '-DCTR_ADPCM_HOST', '-DCTR_ADPCM_PLAYER_HOST', '-Iplatform/3ds/source',
            '-Itools/audio/test_support', '-Iengine/vendor/sha1', 'tools/audio/test_player.c',
            'platform/3ds/source/z_adpcm_player.c', 'platform/3ds/source/z_adpcm_format.c',
            'platform/3ds/source/z_adpcm_store.c', 'platform/3ds/source/z_adpcm_catalog.c',
            'engine/vendor/sha1/sha1.c',
            '-o', str(executable)], cwd=ROOT, stdin=subprocess.DEVNULL, timeout=25, check=True)
        subprocess.run([str(executable)], cwd=ROOT, stdin=subprocess.DEVNULL, timeout=20, check=True)
        fixture = test_pcm.PcmTests()
        fixture.encoder = test_pcm.dsp.native()
        for channels in (1, 2):
            data, expected = fixture.fixture(7 * 2044 + 17, channels)
            encoded, decoded = out / 'bridge-source.dtad', out / 'bridge-reference.pcm'
            encoded.write_bytes(data)
            decoded.write_bytes(expected)
            subprocess.run([str(executable), str(encoded), str(decoded)],
                cwd=ROOT, stdin=subprocess.DEVNULL, timeout=20, check=True)


if __name__ == '__main__':
    unittest.main()
