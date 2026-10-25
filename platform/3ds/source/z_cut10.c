// Release 1.0 display policy. The old display modes stay
// compiled for the pinned layout; only these settings reach the live renderer.
// No per-frame hook: defaults and launch overrides alone pass through here.
#include "ctr_display.h"

#if defined(__3DS__) && defined(__GNUC__)
#define CUT10_CODE __attribute__((noinline, section(".text.late.cut10")))
#else
#define CUT10_CODE
#endif

void __real_CtrDisplay_defaults(CtrDisplaySettings* settings);
void __wrap_CtrDisplay_defaults(CtrDisplaySettings* settings);
bool __real_CtrDisplay_override(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps);
bool CtrCut10_override(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps);

// Keep ctr_display.c's retained body, including its inlined copy in saveEdits,
// intact. Replacing its initializers shrinks each by 4 B on ARM at Release -O2.
CUT10_CODE void __wrap_CtrDisplay_defaults(CtrDisplaySettings* settings) {
    __real_CtrDisplay_defaults(settings);
    settings->topMode = TOP_MODE_STANDARD;
    settings->stereoMode = STEREO_NORMAL;
    settings->bottomMode = BOTTOM_MODE_OFF;
}

// ctr_screens.c's call to the retained parser lands here. Accept old numbers
// and keys (including stereo_hd) without an unknown-setting warning, but never
// expose their modes, even through --ini. Other launch settings keep working.
CUT10_CODE bool CtrCut10_override(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps) {
    bool known = __real_CtrDisplay_override(settings, line, caps);
    settings->topMode = TOP_MODE_STANDARD;
    settings->stereoMode = STEREO_NORMAL;
    if (settings->bottomMode == BOTTOM_MODE_MAGNIFIER) settings->bottomMode = BOTTOM_MODE_OFF;
    return known;
}
