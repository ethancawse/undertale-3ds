"""Exercise exact production SFX/stop bodies with a small simulated DSP owner.

The source extraction avoids a second implementation of the adapter under test.
Only platform services and channel publication are stubbed; PCM is independently
rendered by the approved Python decoder. All fixtures are synthetic.
"""
from pathlib import Path
import re
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_pcm

ROOT = Path(__file__).resolve().parents[2]


def body(source, name):
    hit = re.search(r'^(?:AS_CODE|UTA_LATE) (?:static )?[^\n]*\b' + name + r'\([^;]*?\) \{', source, re.M)
    assert hit, name
    end = source.index('\n}', hit.end()) + 2
    return source[hit.start():end] + '\n'


class SfxTests(unittest.TestCase):
    def test_actual_piece_and_stop_paths(self):
        import shutil
        compiler = shutil.which('gcc') or shutil.which('clang')
        if not compiler:
            compiler = next((str(p) for p in (Path('C:/msys64/ucrt64/bin/gcc.exe'),
                Path('C:/devkitPro/msys2/ucrt64/bin/gcc.exe')) if p.is_file()), None)
        self.assertIsNotNone(compiler, 'Host C compiler required')
        audio = (ROOT / 'platform/3ds/source/ctr_audio_system.c').read_text()
        adapter = (ROOT / 'platform/3ds/source/ctr_adpcm_sfx.inc').read_text()
        adapter = adapter.replace('#include "ctr_adpcm_sfx_types.inc"',
            (ROOT / 'platform/3ds/source/ctr_adpcm_sfx_types.inc').read_text())
        adapter = adapter.replace('#include "ctr_adpcm_sfx_pieces.inc"',
            (ROOT / 'platform/3ds/source/ctr_adpcm_sfx_pieces.inc').read_text())
        types_start = audio.index('typedef struct {\n    const uint8_t* data;', audio.index('#define UTA_RING'))
        # Stop at the type/declaration boundary. Music's pinned declarations
        # now live behind a conditional include; copying that opening #ifdef
        # into the SFX fixture would leave an unterminated conditional.
        types_end = audio.index('\n#ifdef CTRPORT_ADPCM_MUSIC\n#include "ctr_adpcm_uta_state.inc"', types_start)
        types = audio[types_start:types_end]
        types += adapter[adapter.index('typedef struct AdSfxMeta'):
                         adapter.index('typedef struct {\n    int state;')]
        functions = ''.join(body(audio, name) for name in ('utaLive', 'utaUnplayed', 'utaPendingPieces', 'utaQueue'))
        for name in ('adMeta', 'adRead', 'adSfxForget', 'adSfxReserve', 'adSfxRate', 'adSfxReap', 'adSfxSegment', 'adSfxPiece',
                     'adSfxOffsets', 'adSfxSeamPcm'):
            functions += body(adapter, name)
        for name in ('utaSeamOffsets', 'utaSeamWeight', 'utaClamp16', 'utaBuildSeam', 'utaSeamAt', 'utaAdvance'):
            functions += body(audio, name)
        functions += body(adapter, 'adSfxCaptureTail') + body(adapter, 'adSfxTailSample')
        functions += body(audio, 'utaStop')
        functions += body(audio, 'utaFeed')
        source = (ROOT / 'tools/audio/test_sfx.c').read_text().replace('// PRODUCTION_TYPES', types)
        constants = '\n'.join(re.findall(r'^#define AS_(?:PIECE_FRAMES|POOL_BYTES|FRAME_BYTES) .*$', adapter, re.M))
        source = source.replace('// PRODUCTION_CONSTANTS', constants)
        view = adapter[adapter.index('typedef struct {\n    UtaChan* channels;'):
                       adapter.index('extern const AdSfxView')]
        view += ('static const AdSfxView CtrAdpcm_sfxView = '
                 '{utaChan, utaWb, &utaTailPcm, &utaSeamPcm, NULL, NULL};\n')
        source = source.replace('// PRODUCTION_VIEW', view)
        source = source.replace('// PRODUCTION_FUNCTIONS', functions)
        folder = ROOT / 'scratch/adpcm/host/sfx'
        folder.mkdir(parents=True, exist_ok=True)
        fixture = test_pcm.PcmTests()
        fixture.encoder = test_pcm.dsp.native()
        for mono in (False, True):
            generated = folder / ('mono.c' if mono else 'test.c')
            executable = generated.with_suffix('.exe')
            variant = source
            if mono:
                variant = '#define AS_MONO_ONLY\n' + variant.replace('#define AS_FRAME_BYTES 4u', '#define AS_FRAME_BYTES 2u')
            generated.write_text(variant)
            subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-Wno-unused-variable', '-DCTR_ADPCM_HOST', '-DCTRPORT_ADPCM_SFX',
                '-Iplatform/3ds/source', str(generated), 'platform/3ds/source/z_adpcm_pcm.c',
                'platform/3ds/source/z_adpcm_format.c', '-o', str(executable), '-lm'],
                cwd=ROOT, stdin=subprocess.DEVNULL, timeout=25, check=True)
            for channels in ((1,) if mono else (1, 2)):
                data, pcm = fixture.fixture(7 * 2044 + 17, channels)
                (folder / 'source.dtad').write_bytes(data)
                (folder / 'decoded.pcm').write_bytes(pcm)
                subprocess.run([str(executable), str(folder / 'source.dtad'), str(folder / 'decoded.pcm')],
                    cwd=ROOT, stdin=subprocess.DEVNULL, timeout=20, check=True)


if __name__ == '__main__':
    unittest.main()
