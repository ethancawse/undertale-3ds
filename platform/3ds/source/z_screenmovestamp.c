// chg-screenmove-stamp: stack-only observer; installed by screenmovestamp_compile.py.
#include "ctr_benchmark.h"
#include "ctr_startup_clock.h"
#include "z_utpolish_bench.h"
#include "log.h"

#define SMS_TEXT __attribute__((noinline, noipa, optimize("Os"), section(".text_screenmovestamp")))
static const char smsFormat[] __attribute__((section(".rodata_screenmovestamp"))) =
    "Renderer: screen move frame=%ld screen=%s start=%.3f end=%.3f ms=%.3f%s\n";
static const char smsFallback[] __attribute__((section(".rodata_screenmovestamp"))) = " fallback";

// Legacy tail-branches after restoring its stack; scanout BLs with 56 B
// live. Both gateways pass the independent observer's start. Legacy tags
// bit 63 (over 292,000 app years); remove it before the zero sentinel or
// any clock arithmetic. No extra clock/log or shared/global flag state.
SMS_TEXT void CtrScreenMoveStamp_finish(const char* marker, const char* screen, uint64_t start) {
    unsigned fallback = (unsigned) (start >> 63);
    start &= ~(UINT64_C(1) << 63);
    if (!start) {
        logInfo(marker, screen);
        return;
    }
    uint64_t end = CtrStartup_elapsedUs();
    logInfo(marker, screen);
    long frame = utSlowFrame.runner ? utSlowFrame.runner->frameCount - 1 : 0;
    logInfo(smsFormat, frame, screen, (double) start / 1000.0,
        (double) end / 1000.0, (double) (end - start) / 1000.0,
        fallback ? smsFallback : smsFallback + sizeof(smsFallback) - 1);
}

#ifdef __arm__
// The 8-byte context is private to this invocation and survives nested
// calls. AAPCS stack alignment and all callee-saved registers are preserved.
// The original renderer object supplies both displaced-prologue trampolines,
// allowing branches back to its local screensMoveGame/scanoutMove symbols.
__asm__(
    ".pushsection .text_screenmovestamp,\"ax\",%progbits\n"
    ".arm\n.balign 4\n.global CtrScreenMoveStamp_enter\n"
    ".type CtrScreenMoveStamp_enter,%function\n"
    "CtrScreenMoveStamp_enter:\n"
    "push {r4,r5,r6,lr}\n"
    "mov r4,r0\nmov r5,r1\n"
    "bl CtrBench_running\ncmp r0,#0\n"
    "moveq r0,#0\nmoveq r1,#0\n"
    "blne CtrStartup_elapsedUs\n"
    "push {r0,r1}\n"
    "mov r0,r4\nmov r1,r5\n"
    "bl CtrScreenMoveStamp_body\n"
    "add sp,sp,#8\npop {r4,r5,r6,pc}\n"
    ".size CtrScreenMoveStamp_enter,.-CtrScreenMoveStamp_enter\n"
    ".global CtrScreenMoveStamp_marker\n"
    ".type CtrScreenMoveStamp_marker,%function\n"
    "CtrScreenMoveStamp_marker:\n"
    "ldrd r2,r3,[sp]\norr r3,r3,#0x80000000\nb CtrScreenMoveStamp_finish\n"
    ".size CtrScreenMoveStamp_marker,.-CtrScreenMoveStamp_marker\n"
    ".popsection\n");
__asm__(
    ".pushsection .text_screenmovestamp,\"ax\",%progbits\n"
    ".arm\n.balign 4\n.global CtrScreenMoveStamp_scanoutEnter\n"
    ".type CtrScreenMoveStamp_scanoutEnter,%function\n"
    "CtrScreenMoveStamp_scanoutEnter:\n"
    "push {r4,r5,r6,lr}\n"
    "mov r4,r0\nmov r5,r1\n"
    "bl CtrBench_running\ncmp r0,#0\n"
    "moveq r0,#0\nmoveq r1,#0\n"
    "blne CtrStartup_elapsedUs\n"
    "push {r0,r1}\n"
    "mov r0,r4\nmov r1,r5\n"
    "bl CtrScreenMoveStamp_scanoutBody\n"
    "add sp,sp,#8\npop {r4,r5,r6,pc}\n"
    ".size CtrScreenMoveStamp_scanoutEnter,.-CtrScreenMoveStamp_scanoutEnter\n"
    ".global CtrScreenMoveStamp_scanoutMarker\n"
    ".type CtrScreenMoveStamp_scanoutMarker,%function\n"
    "CtrScreenMoveStamp_scanoutMarker:\n"
    "ldrd r2,r3,[sp,#56]\nb CtrScreenMoveStamp_finish\n"
    ".size CtrScreenMoveStamp_scanoutMarker,.-CtrScreenMoveStamp_scanoutMarker\n"
    ".popsection\n");
#endif
