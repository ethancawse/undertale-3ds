"""Compile and run the production music adapter with synthetic staged audio."""
import os
import contextlib
import io
import struct
import wave
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_catalog
import receipts
from test_store import ROOT


class MusicTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        test_catalog.CatalogTests.setUpClass()

    def test_game_adapter_lifecycle(self):
        fixture = test_catalog.CatalogTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        stereo = fixture.source / 'stereo.ogg'
        with wave.open(str(stereo), 'wb') as sound:
            sound.setparams((2, 2, 44100, 0, 'NONE', 'not compressed'))
            sound.writeframes(struct.pack('<24538h', *(v for i in range(12269)
                for v in (i - 6000, 12000 - i))))
        mono = fixture.source / 'mono.ogg'
        with wave.open(str(mono), 'wb') as sound:
            sound.setparams((1, 2, 22050, 0, 'NONE', 'not compressed'))
            sound.writeframes(struct.pack('<2173h', *(i * 7 - 7000 for i in range(2173))))
        with contextlib.redirect_stdout(io.StringIO()):
            prepared = test_catalog.prepare.prepare(fixture.source, fixture.prepared, lib=fixture.encoder)
            test_catalog.stage.stage(fixture.source, fixture.prepared, fixture.out)
        row = next(r for r in prepared['files'] if r['path'] == 'stereo.ogg')
        pcm, rate, channels = test_catalog.dsp.read(fixture.prepared / row['object'])
        self.assertEqual((rate, channels, len(pcm)), (44100, 2, 12269 * 4))
        reference = fixture.root / 'stereo-reference.pcm'
        reference.write_bytes(pcm)
        extra = []
        for name, frames, hz, chans in (('copy.ogg', 2045, 32000, 2), ('mono.ogg', 2173, 22050, 1)):
            row = next(r for r in prepared['files'] if r['path'] == name)
            rendered, rate, channels = test_catalog.dsp.read(fixture.prepared / row['object'])
            self.assertEqual((rate, channels, len(rendered)), (hz, chans, frames * chans * 2))
            target = fixture.root / (name + '.pcm')
            target.write_bytes(rendered)
            extra.append(target.as_posix())
        shutil.copytree(fixture.out, fixture.source / 'audio-adpcm')
        receipts.publish(fixture.source, fixture.source / 'audio-adpcm',
            next((fixture.source / 'audio-adpcm/index').glob('*.dtai')))
        compiler = shutil.which('gcc') or shutil.which('clang')
        if not compiler:
            compiler = next((str(p) for p in (Path('C:/msys64/ucrt64/bin/gcc.exe'),
                Path('C:/devkitPro/msys2/ucrt64/bin/gcc.exe')) if p.is_file()), None)
        self.assertIsNotNone(compiler)
        executable = ROOT / 'scratch/adpcm/host/test-music.exe'
        executable.parent.mkdir(parents=True, exist_ok=True)
        audio = (ROOT / 'platform/3ds/source/ctr_audio_system.c').read_text()
        seam_type = audio[audio.index("// A stream's loop seam."):audio.index('// Whether a cached sound')]
        seam_code = audio[audio.index('UTA_LATE static UtaSeam* utaSeamFor('):audio.index('// ===[ utstall5:')]
        header = executable.with_suffix('.seam.h')
        header.write_text('#define UTA_LATE\n#define CTRPORT_ADPCM_MUSIC\n'
            '#define UTA_SEAM_RATIO 4\n#define UTA_SEAM_SECONDS_X1000 3\n'
            + seam_type + '\nstatic CtrAudioSystem* utaCa;\n'
            'static UtaSeam utaSeams[STREAM_CONTEXTS];\n'
            'static void (*utaPost)(CtrAudioSystem*, const char*, ...);\n'
            'static const char utaSeamFormat[]="seam", utaNone[]="none", '
            'utaSmoothed[]="smoothed", utaKept[]="kept";\n' + seam_code)
        subprocess.run([compiler, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
            '-DCTR_ADPCM_HOST', '-DCTR_ADPCM_PLAYER_HOST', '-Iplatform/3ds/source',
            '-DCTR_ADPCM_TEST_SEAM_HEADER="' + header.as_posix() + '"',
            '-Itools/audio/test_support', '-Iengine/vendor/sha1', 'tools/audio/test_music.c',
            *['platform/3ds/source/z_adpcm_' + n + '.c' for n in ('player', 'format', 'store', 'catalog')],
            'engine/vendor/sha1/sha1.c', '-o', str(executable), '-lm'], cwd=ROOT,
            stdin=subprocess.DEVNULL, timeout=25, check=True)
        env = dict(os.environ)
        env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
        subprocess.run([str(executable), (fixture.source / 'data.win').as_posix(), fixture.sha.hex(),
            (fixture.source / 'copy.ogg').as_posix(), stereo.as_posix(), reference.as_posix(),
            extra[0], mono.as_posix(), extra[1]], cwd=ROOT, stdin=subprocess.DEVNULL,
            timeout=20, check=True, env=env)


if __name__ == '__main__':
    unittest.main()
