"""Host gate for audio metrics bookkeeping, without game data or a 3DS."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import os

ROOT = Path(__file__).resolve().parents[2]


def main():
    compiler = shutil.which('gcc') or 'C:/msys64/ucrt64/bin/gcc.exe'
    out = ROOT / 'scratch/adpcm'
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out) as temporary:
        folder = Path(temporary)
        (folder / '3ds.h').write_text('''#pragma once
#include <stdint.h>
typedef uint64_t u64;
typedef uintptr_t u32; // address-sized host adapter for libctru's heap bounds
typedef int32_t s32;
typedef int LightLock;
void* getThreadLocalStorage(void);
void LightLock_Lock(LightLock*);
void LightLock_Unlock(LightLock*);
''')
        exe = folder / ('metrics.exe' if os.name == 'nt' else 'metrics')
        # Windows' CRT calls the usable-size query _msize.
        flags = ['-Dmalloc_usable_size=_msize'] if os.name == 'nt' else []
        subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        *flags, '-I', str(folder), str(ROOT / 'tools/audio/test_metrics.c'),
                        '-o', str(exe)], check=True, timeout=25, stdin=subprocess.DEVNULL)
        subprocess.run([str(exe)], check=True, timeout=20, stdin=subprocess.DEVNULL)


if __name__ == '__main__':
    main()
