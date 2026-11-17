// Screenshot notices do not change display settings or screen ownership. The
// compiler adapter removes only their three dirty stores from the frozen frame
// body. The existing game-over guard delegates here when inactive. A dirty flag
// from any real change (settings, borrower, screen role, HOME or wake) stays set.
#if defined(ENABLE_WAD16) && defined(CTRPORT_PROMONOTICE_HELPER)
_Static_assert(__builtin_offsetof(CtrRenderer, displayDirty) == 12828,
               "promonotice's frozen frame dirty offset");

__attribute__((section(".text_promonotice"), noinline, noclone, used, optimize("Os")))
void CtrPromonotice_configure(CtrRenderer* r) {
    void originalConfigure(CtrRenderer*) __asm__("configureScreens");
    void noticeBottomBacklight(CtrRenderer*) __asm__("applyBottomBacklight");
    void noticeTopBacklight(CtrRenderer*, bool) __asm__("screensTopBacklight");
    if (r->displayDirty) {
        originalConfigure(r);
        return;
    }
    // This is the same notice/backlight update as the full configure path,
    // without resetting dialogue, clearing buffers or touching render targets.
    bool onBottom = CtrScreens_onBottom;
    bool notice = onBottom && r->screenshotNoticeUntil != 0;
    if (notice || CtrScreens_noticeShown) CtrScreens_screenshotNotice(notice);
    bool topOff = onBottom && CtrScreens_otherOff && !notice;
    if (!topOff) noticeTopBacklight(r, false);
    noticeBottomBacklight(r);
    if (topOff) noticeTopBacklight(r, true);
}
#endif
