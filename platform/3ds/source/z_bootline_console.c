// consoleInit's global swap must not publish the loading thread's other buffer.
// This UT link has exactly three consoleInit calls, all in main's boot setup.
// CtrScreens_onBottom is physical placement; the requested/bench setting can differ.
#include <3ds.h>

extern bool CtrScreens_onBottom;
void __real_gfxScreenSwapBuffers(gfxScreen_t screen, bool stereo);
void __wrap_gfxSwapBuffersGpu(void);

__attribute__((noinline, noipa, section(".text.bootline_console_late")))
void __wrap_gfxSwapBuffersGpu(void) {
    __real_gfxScreenSwapBuffers(CtrScreens_onBottom ? GFX_TOP : GFX_BOTTOM, true);
}
