"""Exercise actual shop admission and frame wrappers with synthetic host state.

No game data or GPU emulation. Checks settings/S3 rejection, failed shared
preparation cleanup and one presentation per armed frame at -O0 and -O2.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'platform/3ds/source'


def body(text, signature):
    start = text.index(signature)
    start = text.rfind('\n', 0, start) + 1
    return text[start:text.index('\n}', start) + 2]


PRE = r'''
#include <stdio.h>
#include <stdlib.h>
#define nullptr NULL
#define SS_CODE
#include "ctr_layout.h"
#include "ctr_shopsplit.h"
typedef struct DataWin { int unused; } DataWin;
typedef struct Runner { DataWin* dataWin; int currentRoomIndex; } Runner;
typedef struct Renderer { Runner* runner; } Renderer;
typedef struct { Renderer base; CtrDisplaySettings display; } CtrRenderer;
typedef struct { int width, height; } CtrSurface;
enum { SS_OWNED = 1, SS_ARMED = 2 };
static uint32_t ctrShopsplitState;
static CtrLayoutPlaces places = {0, 1, 0, 0};
static CtrLayoutScene scene = {.band=120, .mode=1, .flags=1};
static CtrSurface surface = {640, 480};
static bool table = true, allowed = true, begun = true;
static int prepare_result = 1, leaves, begins, ends, presents, queried;
static DataWin data;
void CtrLayout_places(const CtrDisplaySettings* s, CtrLayoutPlaces* out) {
    if (!s) abort();
    *out = places;
}
const CtrLayoutScene* CtrLayout_scene(DataWin* dw, int32_t room, int32_t group) {
    if (dw != &data || room != 42 || group != -1) abort();
    queried++;
    return table ? &scene : NULL;
}
static bool CtrRenderer_bsAllowed(Renderer* r) { (void)r; return allowed; }
static int CtrRenderer_bsPrepare(Renderer* r, bool on) { (void)r; if (!on) abort(); return prepare_result; }
static void CtrRenderer_bsLeave8(Renderer* r, bool on) { (void)r; if (on) abort(); leaves++; }
static bool CtrRenderer_bsBegun(Renderer* r) { (void)r; return begun; }
static CtrSurface* bsApp(CtrRenderer* r) { (void)r; return &surface; }
static void ssPresent(Renderer* r) { (void)r; presents++; }
static void __real_CtrRenderer_chainBeginFrameBattlesplit(Renderer* r) { (void)r; begins++; }
static void __real_CtrRenderer_chainEndFrameBattlesplit(Renderer* r) { (void)r; ends++; }
'''

CASES = r'''
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main(void) {
    Runner runner = {&data, 42};
    CtrRenderer r = {{&runner}, {0}};
    Renderer* base = &r.base;
    CHECK(!CtrShopsplit_admitted(NULL, &r.display));
    CHECK(!CtrShopsplit_admitted(base, NULL));
    base->runner = NULL;
    CHECK(!CtrShopsplit_admitted(base, &r.display));
    base->runner = &runner;
    CHECK(queried == 0);
    places.game = 1; CHECK(!CtrShopsplit_admitted(base, &r.display)); places.game = 0;
    places.text = 0; CHECK(!CtrShopsplit_admitted(base, &r.display)); places.text = 1;
    CHECK(queried == 0);
    table = false; CHECK(!CtrShopsplit_admitted(base, &r.display)); table = true;
    CHECK(CtrShopsplit_admitted(base, &r.display));
    scene.mode = 0; CHECK(!CtrShopsplit_admitted(base, &r.display)); scene.mode = 1;
    scene.flags = 0; CHECK(!CtrShopsplit_admitted(base, &r.display));
    scene.flags = 3; CHECK(!CtrShopsplit_admitted(base, &r.display)); scene.flags = 1;
    scene.band = 119; CHECK(!CtrShopsplit_admitted(base, &r.display)); scene.band = 120;
    scene.lift = 1; CHECK(!CtrShopsplit_admitted(base, &r.display)); scene.lift = 0;
    scene.count = 1; CHECK(!CtrShopsplit_admitted(base, &r.display)); scene.count = 0;
    prepare_result = 0;
    __wrap_CtrRenderer_chainBeginFrameBattlesplit(base);
    CHECK(ctrShopsplitState == SS_OWNED && begins == 1);
    __wrap_CtrRenderer_chainEndFrameBattlesplit(base);
    CHECK(presents == 0 && ends == 1);
    table = false;
    __wrap_CtrRenderer_chainBeginFrameBattlesplit(base);
    CHECK(ctrShopsplitState == 0 && leaves == 1 && begins == 2);
    table = true; prepare_result = 1;
    __wrap_CtrRenderer_chainBeginFrameBattlesplit(base);
    CHECK(ctrShopsplitState == (SS_OWNED | SS_ARMED));
    __wrap_CtrRenderer_chainEndFrameBattlesplit(base);
    CHECK(ctrShopsplitState == SS_OWNED && presents == 1 && ends == 2);
    __wrap_CtrRenderer_chainEndFrameBattlesplit(base);
    CHECK(presents == 1 && ends == 3);
    begun = false;
    __wrap_CtrRenderer_chainBeginFrameBattlesplit(base);
    CHECK(ctrShopsplitState == SS_OWNED);
    allowed = false;
    __wrap_CtrRenderer_chainBeginFrameBattlesplit(base);
    CHECK(ctrShopsplitState == 0 && leaves == 2);
    puts("PASS shop settings, S3 rejection, partial cleanup and frame ownership");
    return 0;
}
'''


class Shopsplit(unittest.TestCase):
    def test_actual_admission_and_wrappers(self):
        cc = shutil.which('clang') or shutil.which('gcc')
        self.assertIsNotNone(cc)
        renderer = (SOURCE / 'ctr_renderer.c').read_text()
        admission = (SOURCE / 'z_shopsplit.c').read_text()
        fixture = '\n'.join([PRE, body(admission, 'bool CtrShopsplit_admitted('),
            body(renderer, 'SS_CODE void __wrap_CtrRenderer_chainBeginFrameBattlesplit('),
            body(renderer, 'SS_CODE void __wrap_CtrRenderer_chainEndFrameBattlesplit('), CASES])
        out = ROOT / 'scratch/shopsplit-host'
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=out) as tmp:
            src = Path(tmp) / 'shop.c'
            src.write_text(fixture)
            for opt in ('-O0', '-O2'):
                exe = Path(tmp) / f'shop{opt}.exe'
                build = subprocess.run([cc, '-std=c11', opt, '-Wall', '-Wextra', '-Werror',
                    '-I', str(SOURCE), str(src), '-o', str(exe)], capture_output=True,
                    text=True, stdin=subprocess.DEVNULL, timeout=60)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                run = subprocess.run([str(exe)], capture_output=True, text=True,
                                     stdin=subprocess.DEVNULL, timeout=10)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
