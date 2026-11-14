"""Public-source regression gates for the player-only chosen-screen fixes."""
from pathlib import Path
import subprocess,sys,unittest

ROOT=Path(__file__).resolve().parents[2]

class DlgtopPlayerTests(unittest.TestCase):
    def test_actual_cold_helpers(self):
        subprocess.run([sys.executable,'-I',str(ROOT/'tools/analysis/test_dlgtop_carry.py')],
                       check=True,stdin=subprocess.DEVNULL,timeout=45)

    def test_player_call_sites(self):
        main=(ROOT/'platform/3ds/source/main.c').read_text()
        renderer=(ROOT/'platform/3ds/source/ctr_renderer.c').read_text()
        loader=(ROOT/'platform/3ds/source/z_loadscreen.c').read_text()
        # layout D: UNDERTALE's moveConsoles reaches the guarded restore through z_layout.c's CtrLayout_restoreMenu18,
        # which makes the same call unless the move up took a flight lend over (then the page is cleared).
        self.assertIn('if (screensMenu.frameBuffer != shown) CtrLayout_restoreMenu18(shown, screensMenu.frameBuffer, bytes);',main)
        layout=(ROOT/'platform/3ds/source/z_layout.c').read_text()
        self.assertIn('    return Loadgame_restoreMenu(dst, src, n);\n}',layout.split('void* CtrLayout_restoreMenu18(',1)[1])
        self.assertIn('#define SCREENS_CARRY_COPY Loadgame_carryCopy',renderer)
        self.assertIn('SCREENS_CARRY_COPY(',renderer.split('static u8* screensCarry(',1)[1].split('static void screensMoveGame(',1)[0])
        self.assertIn('Loadgame_delayFirst(force)',loader)
        self.assertNotIn('Loadgame_benchBorrow',main+renderer+loader)

    def test_runner_invokes_both_gates(self):
        runner=(ROOT/'tools/analysis/run_dlgtop_player_gates.py').read_text()
        self.assertIn('test_dlgtop_player.py',runner)
        self.assertIn('check_dlgtop_ut_layout.py',runner)
        verifier=(ROOT/'tools/analysis/check_dlgtop_ut_layout.py').read_text()
        self.assertNotIn('Loadgame_benchBorrow',verifier)

if __name__=='__main__':unittest.main()
