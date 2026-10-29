"""Actual first-admission bodies plus real receipts, store and resident decoder."""
import contextlib
import io
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_sfx
import test_mono_pipeline

ROOT = Path(__file__).resolve().parents[2]


class MonoRuntimeTests(unittest.TestCase):
    def test_first_admission_and_fallback(self):
        test_mono_pipeline.PipelineTests.setUpClass()
        fixture = test_mono_pipeline.PipelineTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        fixture.publish()
        test_sfx.SfxTests().test_actual_piece_and_stop_paths()
        folder = ROOT / 'scratch/adpcm/host/sfx'
        source = (folder / 'mono.c').read_text()
        source = source.replace('int main(', 'int pieces_main(')
        source = source.replace('typedef struct { uint8_t* pcm; uint32_t bytes; } CachedSfx;',
            'typedef struct { uint8_t* pcm; uint32_t bytes, frames, sampleRate; '
            'uint16_t format; uint8_t reload, sourceChannels; uint32_t lastUse; uint64_t lastPlayTick; } CachedSfx;')
        source = source.replace('uint32_t cores;', 'uint32_t cores, gameFrames, installed; uint64_t lastLog;')
        adapter = (ROOT / 'platform/3ds/source/ctr_adpcm_mono.inc').read_text()
        constants = '\n'.join(re.findall(r'^static const char ms\w+\[\].*$', adapter, re.M))
        functions = ''
        for name in ('mmSfxPlanned', 'msCleanup', 'mmSfxAdmit', 'msPlay', 'msUpdate'):
            hit = re.search(r'^MM_CODE static [^\n]*\b' + name + r'\([^;]*?\) \{', adapter, re.M)
            self.assertIsNotNone(hit, name)
            end = adapter.index('\n}', hit.end()) + 2
            functions += adapter[hit.start():end] + '\n'
        source += '\n' + (ROOT / 'tools/audio/test_mono_runtime.c').read_text().replace(
            '// MONO_PRODUCTION', constants + '\n' + functions)
        generated, executable = folder / 'admission.c', folder / 'admission.exe'
        generated.write_text(source)
        audio = (ROOT / 'platform/3ds/source/ctr_audio_system.c').read_text()
        queue = audio[audio.index('int __wrap_CtrAudioSystem_queueSfx(int32_t soundIndex, uint32_t kib,\n'):]
        self.assertIn('int got = mmSfxPlanned(soundIndex) ? -1 : __real_CtrAudioSystem_queueSfx', queue)
        compiler = shutil.which('gcc') or shutil.which('clang') or 'C:/msys64/ucrt64/bin/gcc.exe'
        subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-function', '-Wno-unused-variable', '-DCTR_ADPCM_HOST', '-DCTRPORT_ADPCM_SFX',
            '-Iplatform/3ds/source', '-Itools/audio/test_support', '-Iengine/vendor/sha1', str(generated),
            *['platform/3ds/source/z_adpcm_' + n + '.c' for n in ('pcm', 'format', 'store', 'catalog')],
            'engine/vendor/sha1/sha1.c', '-o', str(executable), '-lm'], cwd=ROOT,
            stdin=subprocess.DEVNULL, timeout=25, check=True)
        row = fixture.manifest['sounds'][0]
        pcm, _, _ = test_sfx.test_pcm.dsp.read(fixture.fixture.prepared / row['object'])
        reference = folder / 'admission.pcm'
        reference.write_bytes(pcm)
        env = dict(os.environ)
        env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
        subprocess.run([str(executable), (fixture.root / 'data.win').as_posix(), fixture.fixture.sha.hex(),
            reference.as_posix()], cwd=ROOT, stdin=subprocess.DEVNULL, timeout=20, check=True, env=env)


if __name__ == '__main__':
    unittest.main()
