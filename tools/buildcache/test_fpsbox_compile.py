"""Exercise the retained-slot assembler adapter and fail-closed cases."""
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fpsbox_compile

ROOT = Path(__file__).resolve().parents[2]
ARM = 'C:/devkitPro/devkitARM/bin/arm-none-eabi-'


def main():
    out = ROOT/'scratch/fpsbox-slot-test'
    out.mkdir(exist_ok=True)
    source = '.syntax unified\n.arm\n.section .text,"ax",%progbits\n'
    for name in fpsbox_compile.SLOTS:
        source += f'.global {name}\n.type {name},%function\n{name}:\n\tbx lr\n\t.size {name}, .-{name}\n'
    padded = fpsbox_compile.retain(source)
    path, obj, raw = out/'slots.s', out/'slots.o', out/'slots.bin'
    path.write_text(padded, encoding='utf-8')
    subprocess.run([ARM+'as.exe', str(path), '-o', str(obj)], check=True,
                   stdin=subprocess.DEVNULL, timeout=10)
    subprocess.run([ARM+'objcopy.exe', '-O', 'binary', str(obj), str(raw)], check=True,
                   stdin=subprocess.DEVNULL, timeout=10)
    assert raw.read_bytes() == bytes.fromhex('1eff2fe1')+bytes(668)+bytes.fromhex('1eff2fe1')+bytes(712)
    path.write_text(padded.replace('arguiPaintScreen:\n\tbx lr',
                                  'arguiPaintScreen:\n\t.space 676'), encoding='utf-8')
    failure = subprocess.run([ARM+'as.exe', '--fatal-warnings', str(path), '-o', str(obj)], capture_output=True,
                             stdin=subprocess.DEVNULL, text=True, timeout=10)
    assert failure.returncode != 0 and '.space' in failure.stderr and 'negative' in failure.stderr
    for bad in (source.replace('arguiDrawText, .-arguiDrawText', 'other, .-other'), source+source):
        try:
            fpsbox_compile.retain(bad)
        except AssertionError:
            pass
        else:
            raise AssertionError('Missing/duplicate function accepted')
    print('PASS: exact 672/716 B slots and zero tails; overflow/missing/duplicate rejected')


if __name__ == '__main__':
    main()
