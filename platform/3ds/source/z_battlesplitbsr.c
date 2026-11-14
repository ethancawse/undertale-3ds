// battlesplit-rework (change battlesplit, 11-10; UNDERTALE only): the bottom backlight through a split frame's
// screen set-up (item 3), and in development builds a log line for a renderer LCD open that fails (4(i)).
//
// Zero-shift (c26rev5 F1; 11-10, battlesplit-rework): no function that existed moves or changes size, and
// battlesplit's AAC tenant (.aacout.text.battlesplit14) keeps its start, end and size. This file's code is in its own
// late text section, appended last (.text_bsr, with ctr_renderer.c's CtrRenderer_bsLitBsr; late_reserve.ld), its
// 1 B of state in its own late .bss section, appended last (.bss.bsr_late); what only development builds carry (the
// LCD open's log) is in .text_bsrdev after it. These calls change target, each the same instruction at the same
// address:
//   the chain's CtrRenderer_bsPrepare                -> CtrBattlesplit_prepareBsr (z_battlesplit.c, same line);
//   the chain's CtrRenderer_bsBegun13                -> CtrBattlesplit_begunBsr (z_battlesplit.c, same line);
//   development builds only: ctr_renderer.c's gspLcdInit calls -> CtrBattlesplit_lcdInitBsr (CMake, the no-URL
//                                                       branch; applyBottomBacklight's and screensTopBacklight's:
//                                                       gsfShow's is DELTARUNE only).
// tools/analysis/check_bsr_swaps.py checks every one of them in the linked ELF.
//
// Item 3. On a frame that is to be split with the bottom lit, a screen set-up clears dlg.shown, which powers the
// bottom backlight off, and CtrRenderer_bsBegun powers it on again in the same frame: a blink (maintainer's 11-09 log,
// lines 1759-1762, each options-menu close). The set-up can run in CtrRenderer_bsPrepare (a pending display change) or
// in the real beginFrame (a screenshot notice that ends there, c26rev5 F2). From prepareBsr to begunBsr the renderer's
// cached flag says off, so neither set-up makes a call; begunBsr puts it back (lit) before bsBegun13 asks for the
// light, which is then already as asked. A frame that turns out not to be split gets the backlight its display asks
// for.
//
// Item 4(ii), a failed renderer LCD open tried again after each beginFrame, was removed on 11-10 (lead). It answered a
// misread report: in maintainer's room 308 run it was the TOP screen that went dark, by the Other screen Off setting with
// the game moved below, not the bottom, and no failed open has been seen.
// Development builds keep the open's log (CTRPORT_BSR_LOG, no release source URL): one line for each renderer LCD
// open that fails (z_dimlight.c's CtrDimlight_openFailedBsr), so a failure would show in a tester's log.
#if defined(ENABLE_WAD16) && defined(__3DS__)
#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>

#include "log.h"
#include "renderer.h"
#include "ctr_battlesplit.h"
#define BS_CODEBSR __attribute__((noinline, noipa, section(".text_bsr")))
#define BS_CODEBSRDEV __attribute__((noinline, noipa, section(".text_bsrdev")))
#define BS_STATEBSR __attribute__((section(".bss.bsr_late")))
// ctr_renderer.c's end: 0 apply the bottom backlight as the display asks, 1 hold (if lit, the cached flag says off:
// true), 2 the cached flag says lit again.
enum { BS_LITBSR_APPLY = 0, BS_LITBSR_HOLD = 1, BS_LITBSR_LIT = 2 };
bool CtrRenderer_bsLitBsr(Renderer* renderer, int32_t op);
bool CtrRenderer_bsBegun13(Renderer* renderer);
int32_t CtrBattlesplit_prepareBsr(Renderer* renderer, bool on);
bool CtrBattlesplit_begunBsr(Renderer* renderer);

// The release state: 1 B (lead 11-10).
static struct {
    uint8_t held; // prepareBsr set the cached flag off for this frame's set-up (until begunBsr)
} bsr BS_STATEBSR;
_Static_assert(sizeof(bsr) == 1, "battlesplit-rework's release state is 1 B (lead 11-10)");

// In place of the chain's CtrRenderer_bsPrepare (called only for a frame that is to be split).
BS_CODEBSR int32_t CtrBattlesplit_prepareBsr(Renderer* renderer, bool on) {
    bsr.held = CtrRenderer_bsLitBsr(renderer, BS_LITBSR_HOLD);
    int32_t split = CtrRenderer_bsPrepare(renderer, on);
    if (split <= 0 && bsr.held != 0) {
        // Not split: its own set-up may have been held, so the backlight as the display asks; beginFrame then as ever.
        bsr.held = 0;
        CtrRenderer_bsLitBsr(renderer, BS_LITBSR_LIT);
        CtrRenderer_bsLitBsr(renderer, BS_LITBSR_APPLY);
    }
    return split;
}

// In place of the chain's CtrRenderer_bsBegun13 (called after the real beginFrame, when prepare said split).
BS_CODEBSR bool CtrBattlesplit_begunBsr(Renderer* renderer) {
    bool held = bsr.held != 0;
    bsr.held = 0;
    if (held) CtrRenderer_bsLitBsr(renderer, BS_LITBSR_LIT);
    bool begun = CtrRenderer_bsBegun13(renderer);
    if (held && !begun) CtrRenderer_bsLitBsr(renderer, BS_LITBSR_APPLY);
    return begun;
}

#ifdef CTRPORT_BSR_LOG
void CtrDimlight_openFailedBsr(Result result, uintptr_t caller);
Result CtrBattlesplit_lcdInitBsr(void);

// Development builds only: ctr_renderer.c's LCD opens (applyBottomBacklight, screensTopBacklight, gsfShow). The call
// goes on to dimlight's wrapper (-Wl,--wrap=gspLcdInit); a failed one is logged, and nothing else changes.
BS_CODEBSRDEV Result CtrBattlesplit_lcdInitBsr(void) {
    Result result = gspLcdInit();
    if (R_FAILED(result)) CtrDimlight_openFailedBsr(result, (uintptr_t) __builtin_return_address(0));
    return result;
}
#endif
#endif
