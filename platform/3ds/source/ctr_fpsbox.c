// Count accepted game-screen flips over elapsed display vblanks, not calls to
// the painter (which also runs for the other screen). No wall-clock timer.
#include "ctr_fpsbox.h"
#include "ctr_frame_pacer.h"
#include <citro3d.h>
#include <string.h>

#define FPSBOX_CODE __attribute__((noinline, optimize("Os"), section(".text.late.fpsbox")))

// The physical game-screen selection used by the pacer's flip totals. Read
// its own vblank clock: bottom presents latch on VBlank1, not VBlank0.
extern uint8_t CtrScreens_pacerGame;

static struct {
    uint32_t vblank, flips;
    bool ready;
    uint8_t screen;
} sample __attribute__((section(".bss.late.fpsbox")));

FPSBOX_CODE void CtrFpsbox_update(char* digits) {
    uint32_t screen = CtrScreens_pacerGame;
    uint32_t now = C3D_FrameCounter((int) screen);
    bool sameScreen = sample.ready && sample.screen == screen;
    uint32_t elapsed = now - sample.vblank;
    if (sameScreen && elapsed < 60u) return;
    CtrFlipStats total;
    CtrPacer_getFlipTotals(&total);
    uint32_t rate = 0;
    if (sameScreen) {
        // Physical display rate, 59.831 Hz; nearest whole fps.
        // Unsigned subtraction handles both counters' wraparound. Include time
        // spent holding the current picture, even if no flip completed at all.
        uint64_t denominator = (uint64_t) elapsed * 1000u;
        rate = (uint32_t) (((uint64_t) (total.flips - sample.flips) * 59831u + denominator / 2u) / denominator);
    }
    if (rate > 99u) rate = 99u;
    digits[0] = (char) ('0' + rate / 10u);
    digits[1] = (char) ('0' + rate % 10u);
    digits[2] = '\0';
    sample.vblank = now;
    sample.flips = total.flips;
    sample.ready = true;
    sample.screen = (uint8_t) screen;
}

FPSBOX_CODE void CtrFpsbox_restoreMenu(uint16_t* shown, const uint16_t* page, bool top) {
    // A top menu occupies the middle 320 columns. Its counter is wholly in
    // the black left border. A bottom menu has no border: restore its pixels
    // from the composed page, preserving the menu's own lines and lettering.
    if (top) shown -= 40u * 240u;
    for (uint32_t x = 2; x < 35u; x++) {
        uint32_t offset = x * 240u + 202u; // y=2..37, largest two-digit cell
        if (top) memset(shown + offset, 0, 36u * sizeof(*shown));
        else memcpy(shown + offset, page + offset, 36u * sizeof(*shown));
    }
}

// The old restore slot stays intact. Its caller now leaves live pixels alone;
// the digit painter composites the menu backdrop and glyph in a single pass.
__attribute__((noinline, noipa, optimize("Os"), section(".text.fpsbox75zm_late")))
void CtrFpsbox_menuNoErase(uint16_t* shown, const uint16_t* page, bool top) {
    (void) shown; (void) page; (void) top;
}

#if defined(__3DS__) && defined(ENABLE_WAD17) && defined(CTRPORT_TARGET_N3DS)
// Retain DR New's existing ARM-to-Thumb veneer before the appended counter.
// caDraw still branches to the exact old address. 3dsxtool also relocates the
// word at veneer +8 by name, even for an explicit stub: give that slot a real,
// unused ABS32 pointer, never an instruction or unrelocated glyph constant.
__asm__(".pushsection .text.fpsbox75zm_veneer,\"ax\",%progbits\n"
        ".arm\n.balign 8\n"
        ".type ____wrap_Crispart_drawTexture_from_arm,%function\n"
        "____wrap_Crispart_drawTexture_from_arm:\n"
        ".global CtrFpsbox_crispVeneer\n"
        ".type CtrFpsbox_crispVeneer,%function\n"
        "CtrFpsbox_crispVeneer:\n"
        "ldr pc,[pc,#-4]\n"
        ".word __wrap_Crispart_drawTexture\n"
        ".size ____wrap_Crispart_drawTexture_from_arm,8\n"
        ".size CtrFpsbox_crispVeneer,8\n"
        ".global CtrFpsbox_veneerReloc\n"
        ".type CtrFpsbox_veneerReloc,%object\n"
        "CtrFpsbox_veneerReloc:\n"
        ".word CtrFpsbox_crispVeneer\n"
        ".size CtrFpsbox_veneerReloc,4\n"
        ".popsection\n");
#endif

// Reset at the transition itself: an away/back move between samples must
// discard its origin too. The existing sampler object keeps its layout.
__attribute__((noinline, noipa, optimize("Os"), section(".text.late.fpsghost")))
void CtrFpsbox_reset(void) { sample.ready = false; CTR_FPSDIAG_RESET(); }

// Preserve the physical setter's existing body extent: redirect one of its
// raw clock reads instead of adding a call after its unlock. Main thread
// only; no sampler runs between this read and the selection's assignment.
extern uint32_t __real_C3D_FrameCounter(int id);
__attribute__((noinline, noipa, optimize("Os"), section(".text.late.fpsghost")))
uint32_t CtrFpsbox_moveOrigin(int screen) {
    CtrFpsbox_reset();
    return __real_C3D_FrameCounter(screen);
}

#ifdef CTR_FPSGHOST_DIAG
#include "log.h"
#include "runner.h"

#define FPSGHOST_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text.late.fpsghost")))
#define FPSGHOST_RO __attribute__((section(".rodata.late.fpsghost")))
typedef struct {
    uint32_t vb[2], recheck, flips;
    uint64_t ticks;
} FpsghostBoundary;
typedef struct { volatile uint32_t address, digits; } FpsghostPaint;
static struct {
    FpsghostBoundary start, end;
    FpsghostPaint paint[2];
    uint64_t episodeAt;
    uint32_t digits, serial;
    uint8_t screen;
    bool valid, below;
} fpsghostDiag __attribute__((section(".bss.late.fpsghost")));
_Static_assert(sizeof(fpsghostDiag) <= 96u, "FPS diagnostic static budget");
static const char fpsghostRecord[] FPSGHOST_RO =
    "[fpsghost] tag %lu reason %lu f %ld room %ld digits %c%c valid %u scr %u "
    "vb %lu,%lu..%lu,%lu re %lu..%lu flips %lu..%lu ticks %llu..%llu "
    "now %lu,%lu,%lu/%lu@%llu "
    "paint top %04lx@%08lx shown %08lx sel %lu bottom %04lx@%08lx shown %08lx sel %lu\n";
static const char fpsghostCost[] FPSGHOST_RO =
    "[fpsghost-cost] tag %lu f %ld record %lu us (excludes this cost line)\n";
Runner* CtrHeldslow_liveRunner(void);

// Sequential raw reads, with VBlank0 rechecked to expose a boundary crossing.
static FPSGHOST_CODE void fpsghostBoundary(FpsghostBoundary* out, uint32_t flips) {
    out->ticks = svcGetSystemTick();
    out->vb[0] = __real_C3D_FrameCounter(0);
    out->vb[1] = __real_C3D_FrameCounter(1);
    out->recheck = __real_C3D_FrameCounter(0);
    out->flips = flips;
}

FPSGHOST_CODE void CtrFpsdiag_reset(void) {
    fpsghostDiag.valid = fpsghostDiag.below = false;
}

// GSP and main-thread paint paths only store the last attempt. This is not
// proof of glyph residency or of which pixels were scanned out. Never log
// or read LCD registers from the GSP callback.
FPSGHOST_CODE void CtrFpsdiag_paint(uint32_t screen, void* buffer, const char* digits) {
    if (screen > 1u) return;
    fpsghostDiag.paint[screen].address = (uint32_t) (uintptr_t) buffer;
    fpsghostDiag.paint[screen].digits = (uint32_t) (uint8_t) digits[0] |
                                      ((uint32_t) (uint8_t) digits[1] << 8);
}

// Main thread only; reason 0=below-29 entry, 1=menu open, 2=menu close.
// The existing startup-clock logger appends application time to both lines.
FPSGHOST_CODE void CtrFpsdiag_event(uint32_t reason) {
    uint64_t began = svcGetSystemTick();
    FpsghostBoundary current;
    CtrFlipStats total;
    CtrPacer_getFlipTotals(&total);
    fpsghostBoundary(&current, total.flips);
    uint32_t hw[2][5] = {{0}}, address[2], paint[2];
    for (uint32_t s = 0; s < 2u; s++) {
        if (R_FAILED(GSPGPU_ReadHWRegs(0x400468u + s * 0x100u, hw[s], sizeof(hw[s]))))
            hw[s][4] = 2u; // explicitly unknown, never read an invalid buffer index
        else hw[s][4] &= 1u;
        address[s] = fpsghostDiag.paint[s].address;
        paint[s] = fpsghostDiag.paint[s].digits;
        if (address[s] != 0u) address[s] = osConvertVirtToPhys((void*) (uintptr_t) address[s]);
    }
    Runner* runner = CtrHeldslow_liveRunner();
    int32_t frame = runner != NULL ? runner->frameCount : -1;
    int32_t room = runner != NULL ? runner->currentRoomIndex : -1;
    uint32_t tag = ++fpsghostDiag.serial;
    uint32_t digits = fpsghostDiag.digits != 0u ? fpsghostDiag.digits : 0x3f3fu;
    logInfo(fpsghostRecord, (unsigned long) tag, (unsigned long) reason, (long) frame, (long) room,
        (int) (digits & 255u), (int) (digits >> 8), (unsigned) fpsghostDiag.valid, (unsigned) fpsghostDiag.screen,
        (unsigned long) fpsghostDiag.start.vb[0], (unsigned long) fpsghostDiag.start.vb[1],
        (unsigned long) fpsghostDiag.end.vb[0], (unsigned long) fpsghostDiag.end.vb[1],
        (unsigned long) fpsghostDiag.start.recheck, (unsigned long) fpsghostDiag.end.recheck,
        (unsigned long) fpsghostDiag.start.flips, (unsigned long) fpsghostDiag.end.flips,
        (unsigned long long) fpsghostDiag.start.ticks, (unsigned long long) fpsghostDiag.end.ticks,
        (unsigned long) current.vb[0], (unsigned long) current.vb[1], (unsigned long) current.recheck,
        (unsigned long) current.flips, (unsigned long long) current.ticks,
        (unsigned long) paint[0], (unsigned long) address[0],
        (unsigned long) (hw[0][4] < 2u ? hw[0][hw[0][4]] : 0u), (unsigned long) hw[0][4],
        (unsigned long) paint[1], (unsigned long) address[1],
        (unsigned long) (hw[1][4] < 2u ? hw[1][hw[1][4]] : 0u), (unsigned long) hw[1][4]);
    uint32_t us = (uint32_t) ((svcGetSystemTick() - began) * 1000000u / 268123480u);
    logInfo(fpsghostCost, (unsigned long) tag, (long) frame, (unsigned long) us);
}

FPSGHOST_CODE void CtrFpsdiag_sample(const char* digits, bool valid, uint32_t flips) {
    fpsghostDiag.start = fpsghostDiag.end;
    fpsghostBoundary(&fpsghostDiag.end, flips);
    if (!valid) fpsghostDiag.start = fpsghostDiag.end;
    fpsghostDiag.digits = (uint32_t) (uint8_t) digits[0] | ((uint32_t) (uint8_t) digits[1] << 8);
    fpsghostDiag.screen = CtrScreens_pacerGame;
    fpsghostDiag.valid = valid;
    bool below = valid && (digits[0] - '0') * 10 + digits[1] - '0' < 29;
    if (below && !fpsghostDiag.below && (fpsghostDiag.episodeAt == 0u ||
        fpsghostDiag.end.ticks - fpsghostDiag.episodeAt >= 5ull * 268123480u)) {
        fpsghostDiag.episodeAt = fpsghostDiag.end.ticks;
        CtrFpsdiag_event(0);
    }
    fpsghostDiag.below = below;
}

// Redirect the existing main-thread call without growing the old sampler.
// Its updated origin identifies a completed sample; early returns do not log.
FPSGHOST_CODE void CtrFpsdiag_update(char* digits) {
    bool valid = sample.ready && sample.screen == CtrScreens_pacerGame;
    uint32_t previous = sample.vblank;
    CtrFpsbox_update(digits);
    if (!valid || sample.vblank != previous) CtrFpsdiag_sample(digits, valid, sample.flips);
}
#endif
