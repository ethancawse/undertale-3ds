#ifndef CTRPORT_CTR_FPSBOX_H
#define CTRPORT_CTR_FPSBOX_H

#include <stdbool.h>
#include <stdint.h>

// Main-thread sampling, before the framebuffer painter reads the two digits.
void CtrFpsbox_update(char* digits);
// Restore the menu under the counter before its next transparent paint.
void CtrFpsbox_restoreMenu(uint16_t* shown, const uint16_t* page, bool top);

// Main-thread discontinuity: Off/re-enable or a physical game-screen move.
void CtrFpsbox_reset(void);
// Physical setter's raw old-screen clock read, with origin retirement.
uint32_t CtrFpsbox_moveOrigin(int screen);

// Development telemetry only. Expands away completely in release builds.
#if defined(CTRPORT_FPSGHOST_DEV) && defined(ENABLE_WAD16)
#define CTR_FPSGHOST_DIAG 1
void CtrFpsdiag_sample(const char* digits, bool valid, uint32_t flips);
void CtrFpsdiag_paint(uint32_t screen, void* buffer, const char* digits);
void CtrFpsdiag_event(uint32_t reason);
void CtrFpsdiag_reset(void);
void CtrFpsdiag_update(char* digits);
void CtrFpsdiag_draw(int32_t x, int32_t y, const char* text, uint32_t colour);
#define CTR_FPSBOX_UPDATE(d) CtrFpsdiag_update(d)
#define CTR_FPSDIAG_DRAW(x, y, d, c) CtrFpsdiag_draw(x, y, d, c)
#define CTR_FPSDIAG_OPEN(m, c, s, p) fpsghostOpen(m, c, s, p)
#define CTR_FPSDIAG_CLOSE(m) fpsghostClose(m)
#define CTR_FPSDIAG_SAMPLE(d, v, f) CtrFpsdiag_sample(d, v, f)
#define CTR_FPSDIAG_PAINT(s, b, d) CtrFpsdiag_paint(s, b, d)
#define CTR_FPSDIAG_EVENT(r) CtrFpsdiag_event(r)
#define CTR_FPSDIAG_RESET() CtrFpsdiag_reset()
#else
#define CTR_FPSBOX_UPDATE(d) CtrFpsbox_update(d)
#define CTR_FPSDIAG_DRAW(x, y, d, c) __wrap_arguiDrawText(x, y, d, c)
#define CTR_FPSDIAG_OPEN(m, c, s, p) CTR_MENUOPEN_AFTER_OPTMENU(m, c, s, p)
#define CTR_FPSDIAG_CLOSE(m) CTR_MENUCLOSE_AFTER_OPTMENU(m)
#define CTR_FPSDIAG_SAMPLE(d, v, f) ((void) 0)
#define CTR_FPSDIAG_PAINT(s, b, d) ((void) 0)
#define CTR_FPSDIAG_EVENT(r) ((void) 0)
#define CTR_FPSDIAG_RESET() ((void) 0)
#endif

#endif
