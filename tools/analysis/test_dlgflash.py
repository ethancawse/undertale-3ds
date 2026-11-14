"""Exercise the shipping transition helpers and fail-closed registration router."""
from pathlib import Path
import importlib.util
import re
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import json

ROOT = Path(__file__).resolve().parents[2]

DRIVER = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#define nullptr NULL
#define ENABLE_WAD16
typedef struct { const char* name; int parentId; } GameObject;
typedef struct { struct { unsigned count; GameObject* objects; } objt; } DataWin;
typedef struct { int objectIndex; uint32_t instanceId; bool destroyed; } Instance;
typedef struct { int currentEventType; Instance* currentInstance; } VMContext;
#define EVENT_DRAW 8
typedef struct { int unused; } CtrSurface;
typedef struct Runner Runner;
typedef struct { Runner* runner; } Renderer;
typedef struct { int blockObjects[8], blockCount, gateObjects[4], gateCount, redirectCount;
    uint8_t* redirect; int32_t objectCount; } CtrDialoguePolicy;
typedef struct { Renderer base; struct { bool dialogueBottom; } display; void* bottomTarget; bool bottomYielded;
    int boundSurface; uint32_t frame;
    struct { Instance* lastInst; bool lastAnswer, armed; CtrDialoguePolicy policy; CtrSurface rec; } dlg; } CtrRenderer;
struct Runner { Renderer* renderer; DataWin* dataWin; bool gate, blocker;
    VMContext* vmContext; int applicationSurfaceId, currentRoomIndex; Instance*** instancesByObject; };
#define arrlen(list) ((list) == NULL ? 0 : ((list)[1] == NULL ? 1 : 2))
static int boxLogs, choiceLogs, originalFrees;
static char boxLine[300];
static char choiceLine[300];
static void logInfo(const char* format, ...) {
    va_list args; va_start(args,format);
    if (strncmp(format,"Dialogue route:",15)==0) { vsnprintf(choiceLine,sizeof(choiceLine),format,args); ++choiceLogs; }
    else { vsnprintf(boxLine,sizeof(boxLine),format,args); ++boxLogs; }
    va_end(args);
}
static CtrSurface* recordingSurface(CtrRenderer* r) { return r->dlg.armed ? &r->dlg.rec : NULL; }
static void CtrDialogue_free(CtrDialoguePolicy* p) { (void)p; ++originalFrees; }
static int registrations, policyChecks, realYields, popupChecks;
static bool __real_CtrDialogue_allowed(const CtrDialoguePolicy* p, Runner* r) { (void)p; ++policyChecks; return r->gate && !r->blocker; }
static bool CtrDialogue_allowed(const CtrDialoguePolicy* p, Runner* r) { ++popupChecks; return __real_CtrDialogue_allowed(p,r); }
static void __real_CtrDialogue_init(CtrDialoguePolicy* p, const DataWin* d) {
    static uint8_t redirects[8];
    memset(p,0,sizeof(*p)); p->blockObjects[0]=1; p->blockObjects[1]=2; p->blockObjects[2]=3; p->blockCount=3;
    memset(redirects,0,sizeof(redirects)); redirects[0]=redirects[7]=1;
    p->redirect=redirects; p->objectCount=(int32_t)d->objt.count; p->redirectCount=2;
}
static void Runner_addInstanceToObjectLists(Runner* r, Instance* i) {
    ++registrations; if(i->objectIndex==0 || i->objectIndex==4) r->gate=true;
}
static void CtrLayout_yield8(Renderer* base, bool yield) { ++realYields; ((CtrRenderer*)base)->bottomYielded=yield; }
typedef void C3D_RenderTarget;
static bool CtrScreens_onBottom;
// Isolate registration/refresh from the loan lifecycle, which is exercised
// with the complete shipping controller by test_dlgtop_lease.py.
enum { DLGTOP_FREE = 0 };
static struct { CtrRenderer* owner; bool battle, drawing; } dlgtopLease;
static bool dlgtopWants(const CtrRenderer* r) { return CtrScreens_onBottom && !r->bottomYielded; }
static C3D_RenderTarget* dlgtopBorrowed(CtrRenderer* r) { return dlgtopWants(r) ? r : NULL; }
static void dlgtopTake(CtrRenderer* r) { (void)r; }
static int dlgtopHolder(void) { return DLGTOP_FREE; }
void CtrDlgtop_yield(Renderer* r, bool yield) { (void)r; (void)yield; }
// HELPERS
int main(void) {
    // The included shipping helper owns the four lookup strings; its byte
    // gate pins them. This fixture supplies relationships and policy bits.
    GameObject objects[]={{"box",-1},{dlgflashMusic,-1},{"battle",-1},{dlgflashCosmetic,-1},{"child",0},
        {"unused1",-1},{"unused2",-1},{"unrelated",-1}};
    DataWin data={.objt={8,objects}};
    CtrRenderer renderer={0}; Runner runner={.renderer=&renderer.base,.dataWin=&data}; renderer.base.runner=&runner;
    CtrDlgflash_init(&renderer.dlg.policy,&data);
    assert(renderer.dlg.policy.blockCount==1 && renderer.dlg.policy.blockObjects[0]==2);
    assert(renderer.dlg.policy.redirectCount==2);
    for (int i=0;i<8;++i)
        assert(renderer.dlg.policy.redirect[i]==(i==0 || i==7));
    renderer.display.dialogueBottom=true; renderer.bottomTarget=&renderer;
    renderer.dlg.policy.gateCount=1; renderer.dlg.policy.gateObjects[0]=0; renderer.dlg.policy.redirectCount=3;
    Instance box={.objectIndex=0,.instanceId=1001}, other={.objectIndex=1,.instanceId=1002}, child={.objectIndex=4,.instanceId=1003};
    // Gate appears during Draw after the frame was disarmed.
    CtrDlgflash_registered(&runner,&box); assert(registrations==1 && renderer.dlg.armed && policyChecks==1);
    // Unrelated creation does not scan the policy; descendants do.
    CtrDlgflash_registered(&runner,&other); assert(policyChecks==1 && registrations==2);
    renderer.dlg.armed=false; CtrDlgflash_registered(&runner,&child); assert(renderer.dlg.armed && policyChecks==2);
    // Actual text-owning blocker wins, including on the same-pass creation.
    runner.blocker=true; CtrDlgflash_registered(&runner,&box); assert(!renderer.dlg.armed);
    runner.blocker=false;
    assert(popupChecks==0); // Never execute popup anchor/free side effects mid-Draw.
    // Menu-close ownership release refreshes the stale armed flag and cache.
    renderer.bottomYielded=true; renderer.dlg.lastInst=&box; renderer.dlg.lastAnswer=true;
    CtrDlgflash_yield(&renderer.base,false);
    assert(renderer.dlg.armed && !renderer.bottomYielded && renderer.dlg.lastInst==NULL && !renderer.dlg.lastAnswer);
    assert(popupChecks==1);
    int checks=policyChecks; CtrDlgflash_yield(&renderer.base,false); assert(policyChecks==checks && realYields==2);
    CtrDlgflash_yield(&renderer.base,true); renderer.dlg.armed=false;
    CtrDlgflash_registered(&runner,&box); assert(!renderer.dlg.armed);
    // Disabled/unavailable bottom never arms. Every original registration remains.
    renderer.bottomYielded=false; renderer.bottomTarget=NULL; CtrDlgflash_registered(&runner,&box); assert(!renderer.dlg.armed);
    renderer.bottomTarget=&renderer; renderer.display.dialogueBottom=false;
    checks=policyChecks; CtrDlgflash_registered(&runner,&box); assert(!renderer.dlg.armed && policyChecks==checks);
    assert(registrations==7);
    // A Draw-created box is routed in the mirrored direction too, with no
    // bottom-role target. The actual S4 refresh must still respect blockers.
    CtrScreens_onBottom=true; renderer.display.dialogueBottom=true; renderer.bottomTarget=NULL;
    CtrDlgflash_registered(&runner,&box); assert(renderer.dlg.armed);
    runner.blocker=true; CtrDlgflash_registered(&runner,&box); assert(!renderer.dlg.armed);
    runner.blocker=false; CtrScreens_onBottom=false; renderer.bottomTarget=&renderer;
    // Actual rectangle query: duplicate outer/inner calls count one frame.
    VMContext context={EVENT_DRAW,&box}; runner.vmContext=&context; runner.currentRoomIndex=213;
    Instance* live[]={&box,NULL}; Instance** lists[]={live,NULL,NULL,NULL,NULL}; runner.instancesByObject=lists;
    renderer.display.dialogueBottom=true; renderer.boundSurface=runner.applicationSurfaceId=7;
    renderer.frame=10; renderer.dlg.armed=false;
    assert(CtrDlgflash_recordingSurface(&renderer)==NULL);
    assert(choiceLogs==1 && strstr(choiceLine,"room=213 object=0 id=1001 frame=10 screen=top")!=NULL);
    assert(CtrDlgflash_recordingSurface(&renderer)==NULL);
    renderer.frame=11; CtrDlgflash_recordingSurface(&renderer);
    renderer.frame=12; renderer.dlg.armed=true;
    assert(CtrDlgflash_recordingSurface(&renderer)==&renderer.dlg.rec);
    CtrDlgflash_recordingSurface(&renderer); assert(boxLogs==0);
    // A later menu-close flash is retained in the same box's one summary.
    renderer.frame=13; renderer.dlg.armed=false; CtrDlgflash_recordingSurface(&renderer);
    renderer.frame=14; renderer.dlg.armed=true; CtrDlgflash_recordingSurface(&renderer);
    dlgflashLogSweep(&renderer); assert(boxLogs==0);
    box.destroyed=true; dlgflashLogSweep(&renderer);
    assert(boxLogs==1 && strstr(boxLine,"room=213 object=0 id=1001")!=NULL);
    assert(strstr(boxLine,"top_before_bottom=2 top_after_bottom=1 bottom_frames=2")!=NULL);
    dlgflashLogSweep(&renderer); assert(boxLogs==1);
    // Zero-top box, concurrent descendant and shutdown all produce one each.
    box.destroyed=false; renderer.frame=15; CtrDlgflash_recordingSurface(&renderer);
    context.currentInstance=&child; CtrDlgflash_recordingSurface(&renderer);
    CtrDlgflash_free(&renderer.dlg.policy);
    assert(boxLogs==3 && originalFrees==1 && dlgflashLogs.head==NULL);
    assert(choiceLogs==3 && strstr(choiceLine,"screen=bottom")!=NULL);
    assert(strstr(boxLine,"top_before_bottom=0 top_after_bottom=0 bottom_frames=1")!=NULL);
    // Non-box, wrong event/surface and disabled dialogue add no records.
    context.currentInstance=&other; CtrDlgflash_recordingSurface(&renderer);
    context.currentInstance=&box; context.currentEventType=0; CtrDlgflash_recordingSurface(&renderer);
    context.currentEventType=EVENT_DRAW; renderer.boundSurface=8; CtrDlgflash_recordingSurface(&renderer);
    renderer.boundSurface=7; renderer.display.dialogueBottom=false; CtrDlgflash_recordingSurface(&renderer);
    assert(dlgflashLogs.head==NULL && boxLogs==3);
    puts("PASS: cosmetic-only filter, Draw-created gate/child, menu-close cache, real blocker, ownership and disabled paths");
}
'''

class DlgflashTests(unittest.TestCase):
    def test_yield_refresh_preserves_the_current_layout_delegate(self):
        renderer=(ROOT/'platform/3ds/source/ctr_renderer.c').read_text()
        helper=(ROOT/'platform/3ds/source/ctr_dlgflash.inc').read_text()
        # The wrapper's operand names an assembler alias. Its UNDERTALE
        # target owns the layout's held-target/menu policy before refresh.
        self.assertIn('__asm__("CtrScreens_yieldHook")',renderer)
        self.assertIn('CtrScreens_yieldHook, CtrLayout_yield8',renderer)
        self.assertIn('originalYield(Renderer*, bool) __asm__("CtrLayout_yield8")',helper)
        self.assertNotIn('CtrGsfix_yieldAlias',helper)

    def test_init_filter_targets_the_current_popup_chain(self):
        cmake=(ROOT/'platform/3ds/CMakeLists.txt').read_text()
        owners=re.findall(r'set_property\(SOURCE "\$\{CMAKE_CURRENT_SOURCE_DIR\}/source/([^\"]+)" APPEND PROPERTY\s+'
                          r'COMPILE_DEFINITIONS "__real_CtrDialogue_init=CtrDlgflash_init"\)',cmake)
        self.assertEqual(owners,['z_popbot2.c'])
        first=(ROOT/'platform/3ds/source/z_popbot.c').read_text()
        second=(ROOT/'platform/3ds/source'/owners[0]).read_text()
        # Inspect function bodies: an unused declaration must not qualify the hook.
        self.assertRegex(first,r'void __wrap_CtrDialogue_init\([^{}]+\)\s*\{\s*CtrPopbot2_init\(policy, dataWin\);')
        self.assertRegex(second,r'void CtrPopbot2_init\([^{}]+\)\s*\{\s*__real_CtrDialogue_init\(policy, dataWin\);')

    def test_registration_adapter_composes_after_complete_gml_pass(self):
        spec=importlib.util.spec_from_file_location('dlgflash',ROOT/'tools/buildcache/dlgflash_compile.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        before='\tbl Runner_addInstanceToObjectLists\n'*4+'\tbl Gmlstubs_drawLegacy\nGmlstubs_initRoomOriginal:\n b initRoom\n'
        with tempfile.TemporaryDirectory() as temp:
            output=Path(temp)/'runner.o';commands=[]
            def delegate(command):
                commands.append(command)
                if len(commands)==1:
                    self.assertIn('-c',command);self.assertNotIn('-S',command)
                    output.with_name('runner_gml1.s').write_text(before,encoding='utf-8')
                    output.with_name('runner_gmlstubs.json').write_text(json.dumps({'exit':0,'source':'/engine/src/runner.c'}),encoding='utf-8')
                return 0
            args=['delegate','--gmlstubs-compiler',module.MARKER,'gcc','-DENABLE_WAD16','-c','/engine/src/runner.c','-o',str(output)]
            with patch.object(module,'run',side_effect=delegate):self.assertEqual(module.main(args),0)
            actual=output.with_name('dlgflash1.s').read_text(encoding='utf-8')
            self.assertEqual(actual.replace('CtrDlgflash_registered','Runner_addInstanceToObjectLists'),before)
            self.assertEqual(len(commands),2)
            # A stale side file cannot qualify a delegate which emitted none.
            with patch.object(module,'run',return_value=0),self.assertRaises(AssertionError):module.main(args)
            with patch.object(module,'run',return_value=0),self.assertRaises(AssertionError):
                module.main([a for a in args if a!='--gmlstubs-compiler'])

    def test_summary_is_safe_from_the_release_quiet_filter(self):
        source=(ROOT/'platform/3ds/source/z_argcrash.c').read_text()
        definitions=dict(re.findall(r'ARGCRASH_STR\((\w+),\s*("(?:[^"\\]|\\.)*")\);',source))
        import ast
        table=re.search(r'dropPrefixes\[DROP_PREFIX_COUNT\].*?=\s*\{(.*?)\};',source,re.S)[1]
        names=re.findall(r'\bstr\w+\b',table)
        self.assertEqual(len(names),int(re.search(r'#define DROP_PREFIX_COUNT (\d+)u',source)[1]))
        for name in names:
            self.assertFalse('Dialogue box: '.startswith(ast.literal_eval(definitions[name])),name)
            self.assertFalse('Dialogue route: '.startswith(ast.literal_eval(definitions[name])),name)
        wrapper=source[source.index('ARGCRASH_LATE void __wrap_platformLog('):]
        self.assertIn('type == LOG_TYPE_DEBUG || (format != nullptr && matchesAnyPrefix(format, dropPrefixes, DROP_PREFIX_COUNT))',wrapper)
        helper=(ROOT/'platform/3ds/source/ctr_dlgflash.inc').read_text()
        self.assertIn('logInfo(dlgflashBoxFormat,',helper)

    def test_actual_helpers(self):
        gcc = shutil.which('gcc')
        if not gcc:
            gcc = next((str(p) for p in (Path('C:/devkitPro/msys2/ucrt64/bin/gcc.exe'),
                       Path('C:/devkitPro/msys2/mingw64/bin/gcc.exe')) if p.is_file()), None)
        if not gcc: self.skipTest('host gcc unavailable')
        source = (ROOT/'platform/3ds/source/ctr_dlgflash.inc').read_text()
        source = re.sub(r'^#define DLGFLASH_(?:TEXT|RO) .*$', '', source, flags=re.M)
        source = re.sub(r'^#undef DLGFLASH_(?:TEXT|RO)$', '', source, flags=re.M)
        source = source.replace('DLGFLASH_TEXT','').replace('DLGFLASH_RO','')
        source = source.replace('void originalYield(Renderer*, bool) __asm__("CtrLayout_yield8");', '')
        source = source.replace('originalYield(renderer, yield);','CtrLayout_yield8(renderer, yield);')
        top = (ROOT/'platform/3ds/source/ctr_dlgtop.inc').read_text()
        for name in ('CtrDlgtop_refresh',):
            body = re.search(r'^DLGTOP_CODE [^\n]*\b'+name+r'\([^\n]*\) \{\n.*?^\}', top, re.M|re.S)
            self.assertIsNotNone(body, name)
            source += '\n'+body[0].replace('DLGTOP_CODE', '')+'\n'
        for old,new,decl in (
            ('originalRecording','recordingSurface','CtrSurface* originalRecording(CtrRenderer*) __asm__("recordingSurface");'),
            ('originalFree','CtrDialogue_free','void originalFree(CtrDialoguePolicy*) __asm__("CtrDialogue_free");'),
        ):
            source=source.replace(decl,'').replace(old+'(',new+'(')
        with tempfile.TemporaryDirectory(prefix='dlgflash-') as temp:
            folder=Path(temp); c=folder/'test.c'; exe=folder/'test.exe'
            c.write_text(DRIVER.replace('// HELPERS',source))
            subprocess.run([gcc,'-std=c11','-Wall','-Wextra','-Werror','-DCTRPORT_DLGFLASH_LOG',str(c),'-o',str(exe)],
                           stdin=subprocess.DEVNULL,check=True,timeout=30)
            subprocess.run([str(exe)],stdin=subprocess.DEVNULL,check=True,timeout=10)
            # Public builds run the same routing cases with no diagnostic functions/state.
            start=DRIVER.index('    // Actual rectangle query:')
            end=DRIVER.index('    puts(',start)
            public=DRIVER[:start]+DRIVER[end:]
            c.write_text(public.replace('// HELPERS',source))
            subprocess.run([gcc,'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',
                            '-Wno-unused-variable',str(c),'-o',str(exe)],
                           stdin=subprocess.DEVNULL,check=True,timeout=30)
            subprocess.run([str(exe)],stdin=subprocess.DEVNULL,check=True,timeout=10)
            nm=shutil.which('nm')
            if nm:
                symbols=subprocess.check_output([nm,str(exe)],stdin=subprocess.DEVNULL,text=True,timeout=10)
                for name in ('dlgflashLogs','CtrDlgflash_recordingSurface','CtrDlgflash_free'):
                    self.assertNotIn(name,symbols)

    def test_registration_router(self):
        spec=importlib.util.spec_from_file_location('dlgflash',ROOT/'tools/buildcache/dlgflash_compile.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        original='\tbl Runner_addInstanceToObjectLists\n'*4
        routed,count=module.route(original)
        self.assertEqual(count,4);self.assertEqual(routed.count('CtrDlgflash_registered'),4)
        for mutant in (original.replace('bl ','b ',1),original+original,original.replace('Lists','Lists.isra.0')):
            with self.assertRaises(AssertionError): module.route(mutant)

    def test_helper_pass_excludes_original_code_and_data(self):
        spec=importlib.util.spec_from_file_location('vine_extract',ROOT/'tools/analysis/vine_extract.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        original='''\t.text
original:
\tbx lr
\t.section .rodata
originalData:
\t.word 123
\t.section .text_dlgflash,"ax",%progbits
\t.global CtrDlgflash_test
CtrDlgflash_test:
.L123:
\tbx lr
\t.section .rodata.dlgflash_in_text,"a",%progbits
newData:
\t.word 456
\t.section .debug_info
debugData:
\t.word 789
'''
        result,sections=module.late_sections(original,'dlgflash')
        self.assertEqual(sections,['.rodata.dlgflash_in_text','.text_dlgflash'])
        for forbidden in ('original:', 'originalData:', 'debugData:'):self.assertNotIn(forbidden,result)
        self.assertIn('.LDlgflash_123:',result)
        with self.assertRaises(AssertionError):module.late_sections(original.replace('bx lr','b .Lexcluded',1).replace('\tbx lr\n\t.section .rodata.dlgflash','\tb .Lexcluded\n\t.section .rodata.dlgflash'),'dlgflash')

if __name__=='__main__': unittest.main()
