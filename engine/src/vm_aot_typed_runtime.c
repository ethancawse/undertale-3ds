// Typed-only helpers use the engine math flags, isolated from vm.c's inline budget.
#include "aot/vm_aot.h"

#ifdef CTRPORT_TYPED_AOT
// ---[ Typed regions (aot/vm_aot_typed.h, tools/aot/typed.py) ]---

// The math builtins typed regions call out of line, compiled here with the engine's flags as the interpreter's are.
AOT_TYPED_TEXT GMLReal aotTypedMathDsin(GMLReal x) { return aotMathDsin(x); }
AOT_TYPED_TEXT GMLReal aotTypedMathDcos(GMLReal x) { return aotMathDcos(x); }
AOT_TYPED_TEXT GMLReal aotTypedMathDegtorad(GMLReal x) { return aotMathDegtorad(x); }
AOT_TYPED_TEXT GMLReal aotTypedMathRadtodeg(GMLReal x) { return aotMathRadtodeg(x); }
AOT_TYPED_TEXT GMLReal aotTypedMathFrac(GMLReal x) { return aotMathFrac(x); }
AOT_TYPED_TEXT GMLReal aotTypedMathSign(GMLReal x) { return aotMathSign(x); }
AOT_TYPED_TEXT GMLReal aotTypedMathMax(GMLReal a, GMLReal b) { return aotMathMax(a, b); }
AOT_TYPED_TEXT GMLReal aotTypedMathMin(GMLReal a, GMLReal b) { return aotMathMin(a, b); }
AOT_TYPED_TEXT GMLReal aotTypedMathLengthdirX(GMLReal len, GMLReal dir) { return aotMathLengthdirX(len, dir); }
AOT_TYPED_TEXT GMLReal aotTypedMathLengthdirY(GMLReal len, GMLReal dir) { return aotMathLengthdirY(len, dir); }
AOT_TYPED_TEXT GMLReal aotTypedMathAngleDifference(GMLReal src, GMLReal dest) { return aotMathAngleDifference(src, dest); }
AOT_TYPED_TEXT GMLReal aotTypedMathClamp(GMLReal val, GMLReal lo, GMLReal hi) { return aotMathClamp(val, lo, hi); }
AOT_TYPED_TEXT GMLReal aotTypedMathLerp(GMLReal a, GMLReal b, GMLReal t) { return aotMathLerp(a, b, t); }
AOT_TYPED_TEXT GMLReal aotTypedMathPointDistance(GMLReal x1, GMLReal y1, GMLReal x2, GMLReal y2) { return aotMathPointDistance(x1, y1, x2, y2); }

#ifdef CTRPORT_TYPED_STRESS
AOT_TYPED_BSS uint32_t aotTypedStressEntries, aotTypedStressBlocks;
#endif

#ifdef CTRPORT_TYPED_STATS
static const char aotTypedLogFormat[] AOT_TYPED_CONST = "[typed] %s entered %lu guard-failed %lu deopted %lu exited %lu last-handover %04lX\n";
// failed a guard, handed over, left normally. aotTypedLog logs and clears them.
#define AOT_TYPED_SITES 64
static struct { char tag[96]; uint32_t n[4]; uint32_t lastHandover; } aotTypedSites[AOT_TYPED_SITES] AOT_TYPED_BSS;
static uint32_t aotTypedSiteCount AOT_TYPED_BSS;

AOT_TYPED_TEXT void aotTypedEvent(const char* tag, int what) {
    uint32_t i = 0;
    while (aotTypedSiteCount > i && strcmp(aotTypedSites[i].tag, tag) != 0) i++;
    if (i == aotTypedSiteCount) {
        if (i == AOT_TYPED_SITES) return;
        snprintf(aotTypedSites[i].tag, sizeof aotTypedSites[i].tag, "%s", tag);
        aotTypedSiteCount++;
    }
    aotTypedSites[i].n[what & 3]++;
}

AOT_TYPED_TEXT void aotTypedHandover(const char* tag, uint32_t ip) {
    aotTypedEvent(tag, 2);
    repeat(aotTypedSiteCount, i) {
        if (strcmp(aotTypedSites[i].tag, tag) == 0) {
            aotTypedSites[i].lastHandover = ip;
            break;
        }
    }
}

AOT_TYPED_TEXT void aotTypedLog(void) {
    repeat(aotTypedSiteCount, i) {
        logInfo(aotTypedLogFormat, aotTypedSites[i].tag,
                (unsigned long) aotTypedSites[i].n[0], (unsigned long) aotTypedSites[i].n[1],
                (unsigned long) aotTypedSites[i].n[2], (unsigned long) aotTypedSites[i].n[3],
                (unsigned long) aotTypedSites[i].lastHandover);
        memset(aotTypedSites[i].n, 0, sizeof aotTypedSites[i].n);
        aotTypedSites[i].lastHandover = 0;
    }
}
#endif
#endif

#ifdef CTRPORT_TYPED_CENSUS
AOT_TYPED_BSS uint32_t aotCensus[AOTC_COUNT];
#endif
