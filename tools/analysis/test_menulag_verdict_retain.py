"""Compile the product helper and real native wrapper; cover full-byte
restoration, no edits, table resets and allocation failure/recovery.
Invented data only; all temporary files stay in host TEMP.
"""
from pathlib import Path
import json
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT
CC = Path('C:/msys64/ucrt64/bin/gcc.exe')
if not CC.is_file():
    CC = Path(shutil.which('gcc') or 'C:/msys64/ucrt64/bin/gcc.exe')

HEADER = r'''
#ifndef ML_TEST_TYPES
#define ML_TEST_TYPES
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define nullptr NULL
typedef struct { const char* name; uint32_t textureCount; int32_t* tpagIndices; } Sprite;
typedef struct { int sourceWidth, sourceHeight; } TexturePageItem;
typedef struct DataWin {
 struct { uint32_t count; Sprite* sprites; } sprt;
 struct { uint32_t count; TexturePageItem* items; } tpag;
} DataWin;
typedef struct Renderer { DataWin* dataWin; } Renderer;
typedef struct {
 const DataWin* dataWin; uint32_t count; uint8_t on, applied;
 uint8_t group[96]; int32_t item[96];
} TestGroups;
#endif
'''

DRIVER = r'''
#include "renderer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
TestGroups CtrSmooth_sharp;
static DataWin dw[2];
static Renderer renderer;
static uint8_t values[6550], natural[6550], ink[4];
static int allocations, fail_alloc, native_calls, null_values;
void* MlTest_malloc(size_t n) {
 assert(n == 96); allocations++;
 return fail_alloc ? NULL : malloc(n);
}
uint8_t* Krsharp_verdicts(Renderer* r) { assert(r == &renderer); native_calls++; return null_values ? NULL : values; }
uint8_t* Krsharp_ink(Renderer* r, int32_t i) { (void)r; (void)i; return ink; }
void logInfo(const char* fmt, ...) { (void)fmt; }
void __real_CtrRenderer_setFrameSkipKeepApp(Renderer* r, bool on) { assert(r == &renderer); (void)on; }
void __wrap_CtrRenderer_setFrameSkipKeepApp(Renderer*, bool);
static void reset(unsigned generation) {
 renderer.dataWin=&dw[generation%2];
 memset(&CtrSmooth_sharp,0,sizeof(CtrSmooth_sharp));
 CtrSmooth_sharp.dataWin=renderer.dataWin;
 CtrSmooth_sharp.count=96;
 for(unsigned i=0;i<sizeof(values);i++) natural[i]=(uint8_t)(0x80u | ((i+generation)%64u));
 memcpy(values,natural,sizeof(values));
 for(unsigned i=0;i<96;i++) {
  CtrSmooth_sharp.group[i]=(uint8_t)(1+i%2);
  CtrSmooth_sharp.item[i]=(int32_t)(3+67*i);
 }
}
static void step(unsigned mask) {
 CtrSmooth_sharp.on=(uint8_t)mask;
 int before=native_calls; unsigned previous=CtrSmooth_sharp.applied;
 __wrap_CtrRenderer_setFrameSkipKeepApp(&renderer,false);
 assert(native_calls == before + (mask || previous ? 1 : 0));
 assert(CtrSmooth_sharp.applied == mask);
}
static void expected(unsigned mask) {
 uint8_t want[6550]; memcpy(want,natural,sizeof(want));
 for(unsigned i=0;i<96;i++) if(mask & (1u<<CtrSmooth_sharp.group[i])) want[CtrSmooth_sharp.item[i]]=0x80;
 assert(memcmp(values,want,sizeof(want))==0);
}
int main(int argc,char** argv) {
 assert(argc==2); const char* test=argv[1]; reset(0);
 if(!strcmp(test,"roundtrip")) {
  unsigned masks[]={0,2,2,6,4,0,6,0,4,2,0,6,6,0};
  for(unsigned i=0;i<sizeof(masks)/sizeof(*masks);i++){ step(masks[i]); expected(masks[i]); }
  assert(allocations==1);
 } else if(!strcmp(test,"noedit")) {
  for(int i=0;i<300;i++){ step(0); expected(0); }
  assert(allocations==0);
  step(6); expected(6); int count=allocations;
  for(int i=0;i<300;i++){ step(6); expected(6); }
  assert(allocations==count);
 } else if(!strcmp(test,"recreate")) {
  step(6); expected(6);
  for(unsigned i=1;i<10;i++) { reset(i); step(2); expected(2); step(0); expected(0); }
  assert(allocations==1);
 } else if(!strcmp(test,"oom")) {
  fail_alloc=1; step(2); assert(allocations==1); step(0);
  for(unsigned i=0;i<96;i++) if(CtrSmooth_sharp.group[i]==1) assert(values[CtrSmooth_sharp.item[i]]==0);
  /* Model the native table refill, then retry with a later allocation. */
  memcpy(values,natural,sizeof(values)); fail_alloc=0;
  step(2); expected(2); step(0); expected(0);
 } else if(!strcmp(test,"partial-oom")) {
  fail_alloc=1; step(2); fail_alloc=0; step(6); step(0);
  /* Group 1 had no saved original. It must remain invalid for native refill. */
  for(unsigned i=0;i<96;i++) assert(values[CtrSmooth_sharp.item[i]] == (CtrSmooth_sharp.group[i]==1 ? 0 : natural[CtrSmooth_sharp.item[i]]));
 } else if(!strcmp(test,"unknown")) {
  memset(values,0,sizeof(values)); memset(natural,0,sizeof(natural));
  step(6); expected(6); step(0); expected(0);
 } else if(!strcmp(test,"null")) {
  null_values=1; CtrSmooth_sharp.on=6;
  __wrap_CtrRenderer_setFrameSkipKeepApp(&renderer,false);
  assert(CtrSmooth_sharp.applied==0 && allocations==0);
  null_values=0; step(6); expected(6); step(0); expected(0);
 } else { assert(!"unknown case"); }
 puts(test); return 0;
}
'''


def run_cases(source=SOURCE):
    assert CC.is_file()
    env = dict(os.environ, PATH=str(CC.parent)+os.pathsep+os.environ['PATH'])
    cases = ('roundtrip', 'noedit', 'recreate', 'oom', 'partial-oom', 'unknown', 'null')
    results = []
    with tempfile.TemporaryDirectory(prefix='retain-host-') as tmp:
        task = Path(tmp)
        for name in ('common.h', 'data_win.h', 'renderer.h'):
            (task/name).write_text(HEADER)
        (task/'log.h').write_text('void logInfo(const char*, ...);\n')
        (task/'driver.c').write_text(DRIVER)
        for optimization in ('-O0', '-O2'):
            commands = [
                [CC, optimization, '-Wall', '-Wextra', '-Werror', '-I', task, '-c', source/'platform/3ds/source/ctr_krsharp.c', '-DKrsharp_verdicts=Menulag_verdicts', '-o', task/'native.o'],
                [CC, optimization, '-Wall', '-Wextra', '-Werror', '-I', task, '-c', source/'platform/3ds/source/z_menulag.c', '-Dmalloc=MlTest_malloc', '-o', task/'helper.o'],
                [CC, optimization, '-Wall', '-Wextra', '-Werror', '-I', task, task/'driver.c', task/'native.o', task/'helper.o', '-o', task/'test.exe'],
            ]
            for command in commands:
                r = subprocess.run(list(map(str, command)), capture_output=True, stdin=subprocess.DEVNULL,
                    env=env, timeout=20, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                assert r.returncode==0 and not r.stderr, (command,r.stdout,r.stderr)
            for case in cases:
                r = subprocess.run([str(task/'test.exe'),case], capture_output=True, stdin=subprocess.DEVNULL,
                    env=env, timeout=5, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                assert r.returncode==0, (optimization,case,r.stdout,r.stderr)
                results.append(dict(optimization=optimization,case=case,exit=r.returncode))
    return results



class VerdictRetention(unittest.TestCase):
    @unittest.skipUnless(CC.is_file(), 'requires host gcc')
    def test_native_group_wrapper_restores_all_verdict_bytes(self):
        self.assertEqual(len(run_cases()), 14)


if __name__ == '__main__':
    unittest.main()
