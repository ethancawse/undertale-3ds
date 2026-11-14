// Loan return is outside the GPU frame. Layout first unlinks the target and
// waits for its transfers; then retire both LCD buffers without writing the
// one currently scanned out. The temporary black flip is on the other screen,
// never a game frame, and does not change the game's pacer or recorder holds.
DLGTOP_COLD static void dlgtopRetireBuffers(void) {
    void CtrScreens_pacerShow(gfxScreen_t);
    void CtrScreens_pacerWaitShown(gfxScreen_t, bool);
    CtrGpu_waitIdle();
    CtrLog_beginConsole();
    while (!CtrPacer_screenReady(GFX_TOP)) CtrScreens_pacerWaitShown(GFX_TOP, true);
    gfxSetDoubleBuffering(GFX_TOP, true);
    // gfxInitDefault allocated RGB8 capacity, also when scanout is RGB565.
    const u32 bytes = TOP_SCREEN_W * TOP_SCREEN_H * 3u;
    u8* back = CtrPacer_backFramebuffer(GFX_TOP, GFX_LEFT);
    if (back != nullptr) {
        memset(back, 0, bytes);
        GSPGPU_FlushDataCache(back, bytes);
        CtrScreens_pacerShow(GFX_TOP);
        while (!CtrPacer_screenReady(GFX_TOP)) CtrScreens_pacerWaitShown(GFX_TOP, true);
        back = CtrPacer_backFramebuffer(GFX_TOP, GFX_LEFT);
        if (back != nullptr) {
            memset(back, 0, bytes);
            GSPGPU_FlushDataCache(back, bytes);
        }
    }
    gfxSetDoubleBuffering(GFX_TOP, false); // the console's original mode
    CtrLog_endConsole();
}
