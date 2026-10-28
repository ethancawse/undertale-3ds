// The first-boot hint ("SELECT: Options") off the game's picture (hintpos, playtest 10-26: "the select: options
// tooltip that appears on the bottom left i believe overlays on top of the deltarune version number").
//
// argui (ctr_argui.c) shows the hint on the screen without the game when that screen is free (Other screen Off, or
// Blank on the original 2DS), and for its first ~180 painted frames on the game's screen when it isn't (Stats and
// log). There, one line at (2, 228) is 76 px wide, so it ran 38 px past Standard's 40-px side bar into the game's
// bottom-left corner, and with the game on the bottom screen it started at the game's edge: DELTARUNE's version
// number on the chapter select is under it either way.
//
// arguiPaintScreen now calls CtrHint_draw with the arguments it gave arguiDrawText, so argui's code keeps its size,
// order and bytes and only that call's target changes (the FPS marker's __wrap_arguiDrawText pattern; adding a C
// function to ctr_argui.c reorders its old code). This draws with argui's own painter, so the font, colour and fade
// are argui's:
// - the game on the top screen: two lines, "SELECT:" over "Options", each 7 glyphs = 36 px, inside the left side
//   bar's bottom corner (x 2..37, y 216..239), covering none of the game. The FPS counter keeps the bar's top corner
//   (at most 3 x 12 px high, plus the virtual-clock "VC" line). The menu offers no top mode but Standard since
//   optmenu (optClamp); Wide's 80-px bars (argui's sx = 2) would hold the two lines too.
// - the game on the bottom screen (320 px, no side bar): the one line in the bottom-right corner, 2 px in from the
//   edge (x 242..317): free in both games' first seconds (DELTARUNE's version number is bottom-left, UNDERTALE's
//   intro panels and text are centred) and away from the FPS counter's top-left corner.
// - the free other screen (argui's rgb565 pass): as before.
// All late text (platform/3ds/late_reserve.ld, "hintpos"), named outside .text.late.* so the catch-alls leave it at
// the end of the block.
#include <stdbool.h>
#include <stdint.h>

#if defined(__3DS__) && defined(__GNUC__)
#define HINTPOS_CODE __attribute__((noinline, optimize("Os"), section(".text.hintpos_late")))
#define HINTPOS_RO __attribute__((section(".rodata.hintpos_in_text")))
#else
#define HINTPOS_CODE
#define HINTPOS_RO
#endif

void CtrFps_drawText(int32_t x, int32_t y, const char* s, uint32_t col); // argui's painter (an exact alias)
extern const bool CtrHint_rgb565;                                        // argui's rgb565 flag (an exact alias)
extern uint8_t CtrScreens_game;                                          // 0: the game on top, 1: on the bottom
void CtrHint_draw(int32_t x, int32_t y, const char* s, uint32_t col);

static const char hintposLines[16] HINTPOS_RO = "SELECT:\0Options";

HINTPOS_CODE void CtrHint_draw(int32_t x, int32_t y, const char* s, uint32_t col) {
    if (CtrHint_rgb565) {
        CtrFps_drawText(x, y, s, col);
        return;
    }
    if (CtrScreens_game != 0u) {
        CtrFps_drawText(320 - 2 - 76, y, s, col); // 15 glyphs 5 px apart, the last 6 px wide: 76 px
        return;
    }
    CtrFps_drawText(x, y - 12, hintposLines, col);
    CtrFps_drawText(x, y, hintposLines + 8, col);
}
