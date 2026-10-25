// Virtual-clock runs cannot report wall-clock speed. Mark both FPS readouts;
// preserve the original format pointer and drawing arguments in normal play.
#include "ctr_fps_clock.h"
#include "ctr_release_branding.h"
#include "gettime.h"
#include <stddef.h>
#include <string.h>

#define FPS_CODE __attribute__((noinline, section(".text.late.pacer_vc")))
static const char fpsClockLabel[] __attribute__((section(".rodata.pacer_vc_in_text"))) = "VC";
static char fpsClockFormat[384] __attribute__((section(".data.late.pacer_vc"))) = {0};

// Exact aliases of argui's existing painter and its fps_size byte. No UI state
// layout, function body or existing constant changes to expose those two seams.
void CtrFps_drawText(int32_t x, int32_t y, const char* text, uint32_t colour);
extern const unsigned char CtrFps_counterSize;

FPS_CODE void __wrap_arguiDrawText(int32_t x, int32_t y, const char* text, uint32_t colour) {
    CtrFps_drawText(x, y, text, colour);
    if (GetTime_virtualClockOn())
        CtrFps_drawText(x, y + 12 * ((int32_t) CtrFps_counterSize + 1), fpsClockLabel, colour);
}

FPS_CODE const char* CtrFps_statsFormat(const char* format) {
    if (format != ctrReleaseStatsFormat || !GetTime_virtualClockOn()) return format;
    // postLine immediately copies the format into its record. Cache this fixed
    // format once; this is no log/measurement buffering and never touches time.
    if (fpsClockFormat[0] == '\0') {
        size_t size = strlen(format) + 1;
        if (size > sizeof(fpsClockFormat)) return format;
        memcpy(fpsClockFormat, format, size);
        for (size_t i = 0; i + 4 < size; i++) {
            if (fpsClockFormat[i] == 'f' && fpsClockFormat[i+1] == 'p' &&
                fpsClockFormat[i+2] == 's' && fpsClockFormat[i+3] == ' ') {
                // Same width: no wrapping of the 40-column statistics display.
                fpsClockFormat[i] = 'V';
                fpsClockFormat[i+1] = 'C';
                fpsClockFormat[i+2] = ':';
                break;
            }
        }
    }
    return fpsClockFormat;
}

#ifdef __3DS__
// A variadic ARM tail adapter: preserve r0-r3, lr, stack arguments and 8-byte
// alignment while selecting only r1 (the format). No variadic reformatting,
// floating-point conversions, logger scheduling or game-clock changes.
__asm__(
    ".pushsection .text.late.pacer_vc,\"ax\",%progbits\n"
    ".align 2\n.global __wrap_CtrLog_printTo\n.type __wrap_CtrLog_printTo,%function\n"
    "__wrap_CtrLog_printTo:\n"
    "push {r0-r3,r12,lr}\nmov r0,r1\nbl CtrFps_statsFormat\n"
    "str r0,[sp,#4]\npop {r0-r3,r12,lr}\nb CtrLog_printTo\n"
    ".size __wrap_CtrLog_printTo,.-__wrap_CtrLog_printTo\n.popsection\n");
#endif
