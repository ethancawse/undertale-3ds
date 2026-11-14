"""Exercise the actual UT wrapper against consoleInit's published swap contract."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'platform/3ds/source/z_bootline_console.c'
HEADER = '#include <stdbool.h>\ntypedef enum {GFX_TOP, GFX_BOTTOM} gfxScreen_t;\n'
HARNESS = r'''
#include <3ds.h>
#include <assert.h>
bool CtrScreens_onBottom;
static unsigned current[2], doubled[2], bar[2][2], flips[2];
void __wrap_gfxSwapBuffersGpu(void);
void __real_gfxScreenSwapBuffers(gfxScreen_t s, bool stereo) {
    (void)stereo;
    current[s] ^= doubled[s];
    flips[s]++;
}
static void original_global_swap(void) {
    __real_gfxScreenSwapBuffers(GFX_TOP, true);
    __real_gfxScreenSwapBuffers(GFX_BOTTOM, true);
}
int main(void) {
    // Both physical placements, before and after the first bar, cold or warm.
    // Model the SDK's console-only single-buffer selection then its global call.
    for (unsigned bottom=0; bottom<2; bottom++) {
        for (unsigned first=0; first<2; first++) {
            CtrScreens_onBottom=bottom;
            unsigned game=bottom ? GFX_BOTTOM:GFX_TOP, other=game^1u;
            for (unsigned control=0; control<2; control++) {
                current[0]=current[1]=0; doubled[0]=doubled[1]=1;
                flips[0]=flips[1]=0;
                bar[game][0]=0; bar[game][1]=1; current[game]=first;
                unsigned lost=0;
                for (unsigned console=0; console<3; console++) {
                    doubled[other]=0;
                    if (control) original_global_swap();
                    else __wrap_gfxSwapBuffersGpu();
                    if (first && !bar[game][current[game]]) lost++;
                }
                if (control) assert(flips[game]==3 && (!first || lost==2));
                else assert(flips[game]==0 && flips[other]==3 && lost==0 && current[game]==first);
            }
        }
    }
    return 0;
}
'''

class ConsoleSwap(unittest.TestCase):
    def test_actual_wrapper_keeps_game_picture_through_three_consoles(self):
        cc = shutil.which('gcc') or shutil.which('clang')
        if not cc and Path('C:/msys64/ucrt64/bin/gcc.exe').is_file():
            cc = 'C:/msys64/ucrt64/bin/gcc.exe'
        self.assertIsNotNone(cc, 'host C compiler required')
        out = ROOT / 'scratch/console-swap-host'
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=out) as tmp:
            tmp=Path(tmp)
            (tmp/'3ds.h').write_text(HEADER, encoding='utf8')
            (tmp/'harness.c').write_text(HARNESS, encoding='utf8')
            binary=tmp/'check.exe'
            subprocess.run([cc,'-std=c11','-Wall','-Wextra','-Werror','-I',str(tmp),
                            str(SOURCE),str(tmp/'harness.c'),'-o',str(binary)],
                           stdin=subprocess.DEVNULL,check=True,timeout=30)
            subprocess.run([str(binary)],stdin=subprocess.DEVNULL,check=True,timeout=10)

if __name__ == '__main__':
    unittest.main()
