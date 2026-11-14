// heldslow (change scanout, logaudit G1 and G2, 2026-11-08; UNDERTALE only): prints the held-image and slow-frame
// records, every [perf] window (main.c's call of CtrPacer_logStats is pointed here by name, CMakeLists.txt), after the
// [pace] line.
//
// A record is a game-screen image that stayed up longer than its frames' slots planned (ctr_frame_pacer.c's appended
// heldslow code keeps them): one late frame, a room change's load, a stall. Planned counts the slots the pacer meant
// it to stay up for, so the half-rate and Auto frame-skip modes' unshown frames (60 fps to 30 and below, on purpose)
// never make a record. The frames apart (the replacing frame's number less the replaced one's) is the gap the load or
// stall left; the rooms say whether it was a room change. Records survive the counters' resets: each is kept until
// printed, up to CTR_HELD_RECORDS a window, the rest counted.
//
// G2: a slow frame is one whose main thread took over 33.4 ms, with its parts between main.c's marks; a frame whose
// parts were not measured (CTRPORT_SLOWLOG_ONETICK, or marks out of order) prints its total alone.
//
// Late code and constants (late_reserve.ld, after logfix); no data of its own.
#include <stdbool.h>
#include <stdint.h>
#include "log.h"
#include "z_heldslow.h"

#define HS_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text_heldslow_print")))
#define HS_RO __attribute__((section(".rodata.heldslow_in_text")))

void CtrPacer_logStats(void);
void CtrHeldslow_logStats(void);

static const char fmtHeld[] HS_RO =
    "[held] frame %lu stayed up %u vblanks (planned %u) from vblank %lu, %lu ms in, room %d; replaced by frame %lu, "
    "%lu frames later, room %d%s\n";
static const char fmtHeldMore[] HS_RO = "[held] %lu more this window, not kept\n";
static const char strRoomChange[] HS_RO = " (room change)";
static const char strEmpty[] HS_RO = "";
static const char fmtSlow[] HS_RO =
    "[slow] frame %lu took %u.%u ms on the main thread, %lu ms in, room %d: before the step %u.%u, step %u.%u, "
    "draw %u.%u, audio and room change %u.%u, present and frame end %u.%u\n";
static const char fmtSlowTotal[] HS_RO = "[slow] frame %lu took %u.%u ms on the main thread, %lu ms in, room %d\n";
static const char fmtSlowMore[] HS_RO = "[slow] %lu more this window, not kept\n";
static const char fmtBottom[] HS_RO = "[pace] %lu bottom transfers let through outside the blank (%lu forced, give-up %s)\n";
static const char strYes[] HS_RO = "yes";
static const char strNo[] HS_RO = "no";

HS_CODE void CtrHeldslow_logStats(void) {
    CtrPacer_logStats();
    CtrHeldRecord records[CTR_HELD_RECORDS];
    uint32_t more = 0;
    uint32_t count = CtrHeldslow_takeHeld(records, &more);
    for (uint32_t i = 0; count > i; i++) {
        const CtrHeldRecord* r = &records[i];
        logInfo(fmtHeld, (unsigned long) r->shownFrame, (unsigned) r->vblanks, (unsigned) r->planned,
                (unsigned long) r->fromVblank, (unsigned long) r->ms, (int) r->shownRoom, (unsigned long) r->frame,
                (unsigned long) (r->frame - r->shownFrame), (int) r->room,
                r->room != r->shownRoom ? strRoomChange : strEmpty);
    }
    if (more != 0) logInfo(fmtHeldMore, (unsigned long) more);
    CtrSlowRecord slow[CTR_SLOW_RECORDS];
    count = CtrHeldslow_takeSlow(slow, &more);
    for (uint32_t i = 0; count > i; i++) {
        const uint16_t* t = slow[i].tenths;
        bool parts = true;
        for (int k = 1; 6 > k; k++) parts = parts && t[k] != CTR_SLOW_UNKNOWN;
        if (parts) {
            logInfo(fmtSlow, (unsigned long) slow[i].frame, t[0] / 10u, t[0] % 10u, (unsigned long) slow[i].ms,
                    (int) slow[i].room, t[1] / 10u, t[1] % 10u, t[2] / 10u, t[2] % 10u, t[3] / 10u, t[3] % 10u,
                    t[4] / 10u, t[4] % 10u, t[5] / 10u, t[5] % 10u);
        } else {
            logInfo(fmtSlowTotal, (unsigned long) slow[i].frame, t[0] / 10u, t[0] % 10u, (unsigned long) slow[i].ms,
                    (int) slow[i].room);
        }
    }
    if (more != 0) logInfo(fmtSlowMore, (unsigned long) more);
    // dlgblank's misses: bottom transfers into the buffer on screen that went outside its blank (a tear risk).
    uint32_t outside = 0, forced = 0;
    bool gaveUp = false;
    CtrHeldslow_takeBottom(&outside, &forced, &gaveUp);
    if (outside != 0 || forced != 0) {
        logInfo(fmtBottom, (unsigned long) outside, (unsigned long) forced, gaveUp ? strYes : strNo);
    }
}
