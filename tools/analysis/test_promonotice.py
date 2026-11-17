"""Actual late decision, frozen frame routing, and must-fail ownership mutants."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import promonotice_compile
import vine_extract

ROOT = Path(__file__).resolve().parents[2]


def fixture():
    return ('other:\n\tstrb r9, [r7, #540]\n\tbl configureScreens\n\t.size other, .-other\n'
            'CtrRenderer_beginFrame:\n\tadd r7, r5, #12288\n\tldrb r3, [r7, #540]\n' +
            ''.join(f'\t.loc 2 {line} 25\n\tstrb {reg}, [r7, #540]\n'
                    for line, reg in ((18228, 'r9'), (18232, 'r3'), (18228, 'r9'))) +
            '\tbl CtrGameover_configure\n\t.size CtrRenderer_beginFrame, .-CtrRenderer_beginFrame\n')


class NoticeTests(unittest.TestCase):
    def test_router_changes_only_three_stores_and_preserves_gameover_call(self):
        before = fixture()
        after, receipt = promonotice_compile.route(before)
        self.assertEqual(receipt['stores'], 3)
        self.assertEqual(receipt['warm_calls_changed'], 0)
        self.assertEqual(receipt['gameover_guard_calls'], 1)
        self.assertEqual(after.count('nop\t@ promonotice'), 3)
        self.assertEqual(after.split('CtrRenderer_beginFrame:')[0], before.split('CtrRenderer_beginFrame:')[0])
        self.assertEqual(len(after.splitlines()), len(before.splitlines()))
        self.assertEqual(after.count('bl configureScreens'), 1)
        self.assertEqual(after.count('bl CtrGameover_configure'), 1)
        for bad in (before+before, before.replace('18232', '19235'),
                    before.replace('strb r3', 'strb r0'), before.replace('add r7', 'add r6'),
                    before.replace('ldrb r3', 'ldrb r0'), after,
                    before.replace('\tbl CtrGameover_configure\n', '\tb CtrGameover_configure\n')):
            with self.assertRaises(AssertionError):
                promonotice_compile.route(bad)

    def test_guarded_terminal_delegation_is_the_only_call_change(self):
        before=('other:\n\tb configureScreens\n\t.size other, .-other\n'
                'CtrGameover_configure:\n\tldr r3, state\n\ttst r3, #1\n\tbxne lr\n'
                '\tb configureScreens\n\t.size CtrGameover_configure, .-CtrGameover_configure\n')
        after=promonotice_compile.route_configure(before)
        self.assertEqual(after.replace('CtrPromonotice_configure','configureScreens'),before)
        self.assertEqual(after.count('b CtrPromonotice_configure'),1)
        for bad in (before+before,after,before.replace('\tb configureScreens\n','\tb unknown\n')):
            with self.assertRaises(AssertionError):promonotice_compile.route_configure(bad)

    def test_helper_only_pass_has_no_state_or_original_body(self):
        asm = ('\t.text\noriginal:\n\tbx lr\n\t.section .text_promonotice,"ax",%progbits\n'
               'CtrPromonotice_configure:\n\tbx lr\n\t.section .bss\nstate:\n\t.space 4\n')
        result, sections = vine_extract.late_sections(asm, 'promonotice')
        self.assertEqual(sections, ['.text_promonotice'])
        self.assertNotIn('state:', result)
        self.assertNotIn('original:', result)

    def test_actual_decision_and_required_must_fail_mutants(self):
        gcc = shutil.which('gcc') or next((str(p) for p in (
            Path('C:/devkitPro/msys2/ucrt64/bin/gcc.exe'),
            Path('C:/devkitPro/msys2/mingw64/bin/gcc.exe')) if p.is_file()), None)
        self.assertIsNotNone(gcc, 'Host C compiler required for the notice decision')
        helper = (ROOT/'platform/3ds/source/ctr_promonotice.inc').read_text()
        helper = re.sub(r'__attribute__\(\(.*\)\)\n', '', helper)
        source = (ROOT/'platform/3ds/source/ctr_renderer.c').read_text()
        prefix = source.split('void CtrRenderer_beginFrame(Renderer* renderer) {', 1)[1].split('    dlgBeginFrame(r);', 1)[0]
        prefix = prefix.replace(' FW_T(fwB0);', '')
        self.assertEqual(prefix.count('r->displayDirty = true;'), 2)
        # Execute the frozen source's capture prefix with the adapter's proven
        # semantics: notice branches still arrive at the routed configure call,
        # while their stores do not replace a previously pending real flag.
        routed = prefix.replace('r->displayDirty = true;', 'notice = true;')
        routed = routed.replace('if (r->displayDirty) configureScreens(r);',
                                'if (r->displayDirty || notice) CtrPromonotice_configure(r);')
        self.assertIn('if (r->displayDirty || notice)', routed)
        driver = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ENABLE_WAD16 1
#define CTRPORT_PROMONOTICE_HELPER 1
#define nullptr NULL
typedef struct {
    char pad[12828]; bool displayDirty;
    bool screenshotRequested, screenshotCaptured, screenshotTimingAffected, screenshotPlayer;
    char screenshotSuffix[8]; uint64_t screenshotNoticeUntil;
    bool shown; int holdFrames, gapFrames; void *target;
    int wantedSettings, appliedSettings, borrower, appliedBorrower, role, appliedRole;
} CtrRenderer;
typedef CtrRenderer Renderer;
static bool CtrScreens_onBottom, CtrScreens_otherOff, CtrScreens_noticeShown;
static int configures, noticeCalls, topCalls, bottomCalls, savedOK=1;
static uint64_t now=100;
static void processDeferredFrees(CtrRenderer *r) {(void)r;}
static void parkIdleSurface(CtrRenderer *r) {(void)r;}
static uint64_t osGetTime(void) {return now;}
static bool saveScreenshot(CtrRenderer *r, void *p, bool all, bool player) {
    (void)r; (void)p; (void)all; (void)player; return savedOK;
}
void configureScreens(CtrRenderer *r) {
    configures++; r->displayDirty=false; r->shown=false;
    r->appliedSettings=r->wantedSettings; r->appliedBorrower=r->borrower; r->appliedRole=r->role;
}
void applyBottomBacklight(CtrRenderer *r) {(void)r;bottomCalls++;}
void screensTopBacklight(CtrRenderer *r, bool off) {(void)r;(void)off;topCalls++;}
static void CtrScreens_screenshotNotice(bool on) {noticeCalls++;CtrScreens_noticeShown=on;}
// HELPER
static void oldFrame(Renderer *renderer) { // OLD
}
static void fixedFrame(Renderer *renderer) {bool notice=false; // FIXED
}
static CtrRenderer ready(void) {
    CtrRenderer r={0};r.shown=true;r.holdFrames=19;r.gapFrames=7;r.target=(void*)1234;return r;
}
static int scenario(int which) {
    CtrRenderer r=ready(); configures=0; now=100;
    if(which==1) { // settings change together with a notice start/refresh
        r.displayDirty=true;r.wantedSettings=3;r.screenshotRequested=true;
    } else if(which==2) { // borrower changes while a notice is already active
        r.displayDirty=true;r.borrower=1;r.screenshotNoticeUntil=1000;
    } else { // role changes on the exact notice expiry frame
        r.displayDirty=true;r.role=1;r.screenshotNoticeUntil=99;
    }
    fixedFrame(&r);
    return configures!=1 || r.displayDirty || r.appliedSettings!=r.wantedSettings ||
           r.appliedBorrower!=r.borrower || r.appliedRole!=r.role;
}
int main(int argc,char **argv) {
    if(argc>1) return scenario(atoi(argv[1]));
    CtrRenderer old=ready(), fixed=ready(); int oldOwned=0,newOwned=0;
    for(int f=0;f<17;f++) {
        old.screenshotRequested=fixed.screenshotRequested=true;
        oldFrame(&old);fixedFrame(&fixed);
        oldOwned+=!old.shown;newOwned+=!fixed.shown;
        if(fixed.holdFrames!=19 || fixed.gapFrames!=7 || fixed.target!=(void*)1234) return 10;
        old.shown=fixed.shown=true;
    }
    if(oldOwned!=17 || newOwned!=0) return 11;
    printf("notice ownership decision: %d/17 -> %d/17 erroneous console claims\n",oldOwned,newOwned);
    for(int i=1;i<=3;i++) if(scenario(i)) return 20+i;
    // Idle frames do no work. Start, refresh, failed refresh and expiry update
    // the notice/backlights while retaining every renderer byte except capture metadata.
    for(int k=0;k<5;k++) {
        CtrRenderer r=ready(); configures=noticeCalls=topCalls=bottomCalls=0;
        CtrScreens_onBottom=true;CtrScreens_otherOff=true;CtrScreens_noticeShown=false;
        savedOK=k!=3;
        r.screenshotRequested=k==1 || k==2 || k==3;
        r.screenshotNoticeUntil=k==2 ? 1000 : k==4 ? 99 : 0;
        if(k==3 || k==4) CtrScreens_noticeShown=true;
        fixedFrame(&r);
        if(configures || !r.shown || r.holdFrames!=19 || r.gapFrames!=7 || r.target!=(void*)1234) return 30+k;
        if(bottomCalls!=(k!=0) || topCalls!=(k!=0)) return 40+k;
        if(k && noticeCalls!=1) return 50+k;
    }
    puts("settings / borrower / changed-role expiry: full configure; notice start / refresh / failure / expiry: ownership retained");
    return 0;
}
'''
        driver = driver.replace('// HELPER', helper).replace('// OLD', prefix).replace('// FIXED', routed)
        with tempfile.TemporaryDirectory(prefix='promonotice-') as temp:
            c=Path(temp)/'test.c'; exe=Path(temp)/'test.exe'
            def compile_run(code, case=None):
                c.write_text(code, encoding='utf-8')
                subprocess.run([gcc,'-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],
                               stdin=subprocess.DEVNULL, capture_output=True, timeout=30, check=True)
                return subprocess.run([str(exe)]+([] if case is None else [str(case)]),
                                      stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=10)
            result=compile_run(driver)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            print(result.stdout.strip())
            for case, condition, label in (
                (1, 'r->displayDirty && !r->screenshotCaptured', 'settings-change-during-notice'),
                (2, 'r->displayDirty && !r->screenshotNoticeUntil', 'borrower-change-during-notice'),
                (3, 'r->displayDirty && !r->screenshotTimingAffected', 'changed-role-on-expiry')):
                bad=helper.replace('if (r->displayDirty) {',f'if ({condition}) {{')
                self.assertNotEqual(bad,helper)
                mutant=driver.replace(helper,bad)
                result=compile_run(mutant,case)
                self.assertNotEqual(result.returncode,0,label+' mutant unexpectedly survived')
                print('MUST-FAIL '+label+': killed (rc '+str(result.returncode)+')')


if __name__ == '__main__':
    unittest.main()
