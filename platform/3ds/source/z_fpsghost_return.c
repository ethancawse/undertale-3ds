// UNDERTALE: the borrowed RGB565 LCD returns to retained text once, at teardown.
#include <3ds.h>
#include <string.h>

extern uint8_t CtrScreens_game;
void CtrLog_beginConsole(void);
void CtrLog_endConsole(void);
u8* CtrBlank_settledShown(gfxScreen_t, gfx3dSide_t, u16*, u16*);
void CtrBlank_consolesSingle(gfxScreen_t, bool);

__attribute__((noinline, noipa, optimize("Os"), section(".text_fpsghost_return")))
void CtrFpsghost_reclaimOtherOff(void) {
    // The game LCD's RGB8 framebuffer must keep its ordinary alternating buffers.
    if (CtrScreens_game != GFX_TOP ||
        gfxGetScreenFormat(GFX_BOTTOM) != GSP_RGB565_OES) return;
    CtrLog_beginConsole();
    u16 w = 0, h = 0;
    u8* shown = CtrBlank_settledShown(GFX_BOTTOM, GFX_LEFT, &w, &h);
    CtrBlank_consolesSingle(GFX_BOTTOM, false);
    if (shown != NULL) {
        u32 bytes = (u32)w * h * 2u;
        memset(shown, 0, bytes);
        GSPGPU_FlushDataCache(shown, bytes);
    }
    CtrLog_endConsole();
}
