"""Run the dialogue-transition source/helper gate in the release test suite."""
from pathlib import Path
import subprocess
import sys
import unittest

class DlgflashGate(unittest.TestCase):
    def test_actual_helpers_and_router(self):
        root=Path(__file__).resolve().parents[2]
        subprocess.run([sys.executable,'-I',str(root/'tools/analysis/test_dlgflash.py')],
                       check=True,stdin=subprocess.DEVNULL,timeout=60)
        subprocess.run([sys.executable,'-I',str(root/'tools/analysis/test_utwindow_compile.py')],
                       check=True,stdin=subprocess.DEVNULL,timeout=60)

if __name__=='__main__': unittest.main()
