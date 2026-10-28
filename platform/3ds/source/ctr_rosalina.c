// Luma3DS's Rosalina menu with the other screen Off (rosalina2, an o3DS playtest report of 2026-10-25: "When I open
// the Rosalina menu with the bottom display off, it freezes and doesn't appear").
//
// Rosalina draws its menu on the bottom screen only, by pointing the bottom screen's framebuffer registers at its own
// buffer, and it never touches a backlight. While the menu is up it has locked every thread of this app and of the
// GSP and DSP sysmodules (svcKernelSetState 0x10000, mask 2|1), so nothing here can run until it closes. With Off and
// the game on the top screen, the bottom backlight is powered off (ctr_renderer.c's applyBottomBacklight), and the
// menu was drawn but unlit. docs/3ds-hardware.md, "Rosalina's menu needs the bottom backlight lit before it opens".
//
// So a watcher thread lights the bottom backlight while the player's held buttons are forming Rosalina's combo, before
// Rosalina's own poll (every 50 ms) can see the whole combo, and powers it off again once they aren't (straight after
// the menu closes). Lit means a black screen: the shown bottom buffer is cleared first. The combo is Luma's own
// (svcGetSystemInfo 0x10000/0x101, L+Down+SELECT when unset); without Luma (stock kernel, Azahar) nothing starts.
// "Forming" is two or more of the combo's buttons held, or SELECT alone when it is part of the combo (SELECT is never a
// game button). The watcher reads the HID shared memory every 5 ms on a system core, so Rosalina wins only when the
// combo's last two buttons go down within about one poll of each other.
//
// It acts only while this app has the bottom backlight off (ctr_screens.c's wraps keep that mask; HOME, sleep and the
// exit light it and clear the mask first), and it starts at the first gspLcdExit after the bottom backlight went off:
// it never runs for a player who keeps the other screen lit, nor in any test run (they keep the log console on the
// bottom screen).
//
// Kept small for the late reserve (DELTARUNE Old's is the tightest): release builds log nothing. Test builds only:
// CTRPORT_ROSALINA_PROBE adds a log line per pause the watcher sees (a gap in its 5 ms polling: Rosalina, or sleep),
// with how long the app was paused, whether the bottom was lit in time, the vblanks counted meanwhile and how soon the
// game showed a new picture after the resume, for "the menu is quite slow to close". CTRPORT_ROSALINA_SIM (Azahar,
// which has no Rosalina) also fakes Luma, the held buttons and a pause, and implies the probe.
//
// All late code, constants in late text and late statics (late_reserve.ld), so no existing symbol moves.

#include <3ds.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "ctr_log.h"

#if defined(CTRPORT_ROSALINA_SIM) && !defined(CTRPORT_ROSALINA_PROBE)
#define CTRPORT_ROSALINA_PROBE 1
#endif
#ifdef CTRPORT_ROSALINA_PROBE
#include "ctr_frame_pacer.h"
#include "log.h"
#endif

#if defined(__3DS__) && defined(__GNUC__)
#pragma GCC optimize("Os")
#endif

#define ROSA_CODE __attribute__((noinline, section(".text.rosalina2_late")))
#define ROSA_RO __attribute__((section(".rodata.rosalina2_in_text")))
#define ROSA_BSS __attribute__((section(".bss.rosalina2_late")))

#define ROSA_POLL_NS (5 * 1000 * 1000LL)
#define ROSA_DEFAULT_COMBO (KEY_L | KEY_DDOWN | KEY_SELECT)
#ifdef CTRPORT_ROSALINA_PROBE
#define ROSA_STACK_BYTES 8192 // the log's formatting
#else
#define ROSA_STACK_BYTES 4096
#endif

bool CtrScreens_bottomDark(void); // ctr_screens.c: whether this app has powered the bottom backlight off
// The start of argui's state (ctr_argui.c, a global; ctr_optmenu.c reads it the same way): whether the options menu is
// open, set before its first page is drawn.
typedef struct {
    uint8_t cfg[3];
    bool menuIsOpen;
} RosaArguiHead;
extern RosaArguiHead CtrArgui_state;
void __real_gspLcdExit(void);
void __wrap_gspLcdExit(void);

static Thread rosaThread ROSA_BSS;
static bool rosaTried ROSA_BSS;
static u32 rosaCombo ROSA_BSS;
static u32 rosaAlone ROSA_BSS; // the combo's buttons that count as forming it when held alone
static u32 rosaQuit ROSA_BSS;
__attribute__((used, section(".bss.rosalina2_late"))) u32 CtrRosalina_sleepSave[2];

static const char rosaService[] ROSA_RO = "gsp::Lcd";

// svcSleepThread, then a check that this thread's stack is still mapped read-write: an exceptional exit can free the
// heap it lives on while the thread sleeps (ctr_input.c's CtrInput_guardSleep, which UNDERTALE alone links).
void CtrRosalina_guardSleep(s64 ns);
__asm__(
    ".section .text.rosalina2_late,\"ax\",%progbits\n.arm\n.align 2\n"
    ".global CtrRosalina_guardSleep\n.type CtrRosalina_guardSleep, %function\n"
    "CtrRosalina_guardSleep:\n"
    "svc 0x0a\nldr r12, =CtrRosalina_sleepSave\nstm r12, {r4,r5}\n"
    "mov r2, sp\nsvc 0x02\nldr r12, =CtrRosalina_sleepSave\nldm r12, {r4,r5}\n"
    "cmp r0, #0\nbxne lr\nand r3, r3, #3\ncmp r3, #3\nbxeq lr\n"
    "1: mvn r0, #0\nmvn r1, #0x80000000\nsvc 0x0a\nb 1b\n.ltorg\n"
    ".size CtrRosalina_guardSleep, . - CtrRosalina_guardSleep\n.previous\n");

// gsp::Lcd PowerOnBacklight (0x11) or PowerOffBacklight (0x12) for the bottom screen, on a session of this thread's
// own, opened for the call only: libctru's gspLcdInit/gspLcdExit share one handle with the game thread, and a gsp::Lcd
// session held across HOME froze the console (docs/3ds-hardware.md).
static ROSA_CODE Result rosaBacklight(u32 command) {
    Handle lcd = 0;
    Result res = srvGetServiceHandle(&lcd, rosaService);
    if (R_FAILED(res)) return res;
    u32* cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(command, 1, 0);
    cmdbuf[1] = GSPLCD_SCREEN_BOTTOM;
    res = svcSendSyncRequest(lcd);
    if (R_SUCCEEDED(res)) res = (Result) cmdbuf[1];
    svcCloseHandle(lcd);
    return res;
}

// Blacks out the buffer the bottom screen shows (its registers say which), so lighting it shows nothing old: under Off
// it keeps whatever the consoles or the options menu last left there. Under the consoles' lock, and only while the
// bottom is still dark and the options menu closed: the menu (opened by SELECT, part of the default combo) sets its
// flag before it draws its first page under the same lock, and draws a page only when it changes. The linear heap and
// VRAM each map at one fixed offset, so the shown buffer is libctru's bottom buffer moved by the distance between their
// physical addresses, and is cleared only if that address converts back.
static ROSA_CODE bool rosaClear(void) {
    bool cleared = false;
    u32 regs[5]; // 0x568: buffer A, 0x56C: buffer B, 0x570: format, 0x574, 0x578: which one shows (bit 0)
    u16 w, h; // gfxGetFramebuffer sets both
    CtrLog_beginConsole();
    if (CtrScreens_bottomDark() && !__atomic_load_n(&CtrArgui_state.menuIsOpen, __ATOMIC_ACQUIRE) &&
        R_SUCCEEDED(GSPGPU_ReadHWRegs(0x400568, regs, sizeof(regs)))) {
        u32 shown = (regs[4] & 1) != 0 ? regs[1] : regs[0];
        uintptr_t known = (uintptr_t) gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
        u8* fb = (u8*) (known + shown - osConvertVirtToPhys((const void*) known));
        if (shown != 0 && osConvertVirtToPhys(fb) == shown) {
            u32 format = regs[2] & 7;
            u32 bytes = (u32) w * h * (format == 1 ? 3u : 2u) /* BGR8 or a 16-bit format */;
            memset(fb, 0, bytes);
            GSPGPU_FlushDataCache(fb, bytes);
            cleared = true;
        }
    }
    CtrLog_endConsole();
    return cleared;
}

#ifdef CTRPORT_ROSALINA_PROBE
#define ROSA_TICKS_PER_MS ((u64) SYSCLOCK_ARM11 / 1000)
u32 C3D_FrameCounter(int id); // citro3d: the vblanks its vblank callback has counted
// After the resume, the second's vblanks (citro3d's callback: GSP relays them), the game's pictures and the DSP's audio
// frames (ndsp: about 205 a second) tell which part stopped if the game doesn't come back (rosafreeze).
static const char rosaPauseText[] ROSA_RO =
    "Rosalina: app paused %lu ms (bottom %s); %lu vblanks counted meanwhile; first new picture %ld ms after the "
    "resume, %lu pictures, %lu vblanks and %lu DSP frames in the next second; lit for the combo %lu times so far "
    "(%lu on a cleared screen)\n";
// Logged and flushed as soon as the watcher runs again, before it touches the backlight: if the game then hangs, this
// line says Rosalina let the app go, and a missing "off" line after it says the gsp::Lcd call hung.
static const char rosaResumeText[] ROSA_RO = "Rosalina: app running again after %lu ms (bottom %s)\n";
static const char rosaStartText[] ROSA_RO = "Rosalina: watching for menu combo %08lX\n";
static const char rosaSwitchText[] ROSA_RO = "Rosalina: %s, cleared %d, %08lX\n";
static const char rosaLitWatcher[] ROSA_RO = "lit for the combo";
static const char rosaLitApp[] ROSA_RO = "lit by the app";
static const char rosaDarkText[] ROSA_RO = "dark";
static const char rosaOnText[] ROSA_RO = "lit";
static const char rosaOffText[] ROSA_RO = "off";
static u32 rosaLitCount ROSA_BSS, rosaClearCount ROSA_BSS;

static ROSA_CODE u32 rosaFlips(void) {
    CtrFlipStats flips;
    CtrPacer_getFlipTotals(&flips);
    return flips.flips;
}

// Once per poll: a poll over 250 ms late means the app was paused (it otherwise wakes every 5 ms); the flips are then
// watched for a second and the pause logged.
static ROSA_CODE void rosaProbe(bool lit, bool dark) {
    static u64 lastTick ROSA_BSS, trackStart ROSA_BSS, pauseTicks ROSA_BSS;
    static u32 lastVblank ROSA_BSS, trackFlips ROSA_BSS, pauseVblanks ROSA_BSS, firstMs ROSA_BSS;
    static u32 trackVblank ROSA_BSS, trackDsp ROSA_BSS;
    static const char* bottomAtPause ROSA_BSS;
    u64 now = svcGetSystemTick();
    u32 vblank = C3D_FrameCounter(0);
    if (lastTick != 0 && now - lastTick > 250 * ROSA_TICKS_PER_MS && trackStart == 0) {
        trackStart = now;
        pauseTicks = now - lastTick;
        pauseVblanks = vblank - lastVblank;
        trackVblank = vblank;
        trackDsp = ndspGetFrameCount();
        trackFlips = rosaFlips();
        firstMs = ~0u;
        bottomAtPause = lit ? rosaLitWatcher : dark ? rosaDarkText : rosaLitApp;
        logInfo(rosaResumeText, (unsigned long) (pauseTicks / ROSA_TICKS_PER_MS), bottomAtPause);
        CtrLog_flush();
    }
    lastTick = now;
    lastVblank = vblank;
    if (trackStart == 0) return;
    u32 flips = rosaFlips();
    if (firstMs == ~0u && flips != trackFlips) firstMs = (u32) ((now - trackStart) / ROSA_TICKS_PER_MS);
    if (now - trackStart < 1000 * ROSA_TICKS_PER_MS) return;
    logInfo(rosaPauseText, (unsigned long) (pauseTicks / ROSA_TICKS_PER_MS), bottomAtPause,
            (unsigned long) pauseVblanks, firstMs == ~0u ? -1L : (long) firstMs, (unsigned long) (flips - trackFlips),
            (unsigned long) (vblank - trackVblank), (unsigned long) (ndspGetFrameCount() - trackDsp),
            (unsigned long) rosaLitCount, (unsigned long) rosaClearCount);
    trackStart = 0;
}
#endif

#ifdef CTRPORT_ROSALINA_SIM
// Azahar only. The pause starts ROSA_SIM_AT_MS after the watcher starts and lasts ROSA_SIM_PAUSE_MS. L+Down is held
// from a second before it until 1.5 s into it (so the watcher lights the bottom before the pause and powers it off as
// soon as it ends, as after a real close), and again for 0.3 s 4 s after it. Rosalina stops every thread of the app
// while the GPU, the LCDs and the DSP run on; as near as Azahar gets (rosafreeze), a thread at the highest
// application priority spins on each of the app's cores for the whole pause, so that no other thread of the app runs
// there, citro3d's vblank callbacks and libctru's GSP and ndsp threads included. Azahar's GSP and DSP don't stop.
#ifndef CTRPORT_ROSALINA_SIM_AT_MS
#define CTRPORT_ROSALINA_SIM_AT_MS 3000
#endif
#ifndef CTRPORT_ROSALINA_SIM_PAUSE_MS
#define CTRPORT_ROSALINA_SIM_PAUSE_MS 12000
#endif
#define ROSA_SIM_AT ((u32) CTRPORT_ROSALINA_SIM_AT_MS)
#define ROSA_SIM_END ((u32) CTRPORT_ROSALINA_SIM_AT_MS + (u32) CTRPORT_ROSALINA_SIM_PAUSE_MS)
static u64 rosaSimStart ROSA_BSS, rosaSimSpinEnd ROSA_BSS;
static const char rosaSimText[] ROSA_RO = "Rosalina sim: every app thread stopped for %lu ms (spinners on cores 0-%d: %d made)\n";
static ROSA_CODE u32 rosaSimMs(void) { return (u32) ((svcGetSystemTick() - rosaSimStart) / ROSA_TICKS_PER_MS); }
static ROSA_CODE void rosaSimSpin(void* arg) {
    (void) arg;
    while (__atomic_load_n(&rosaSimSpinEnd, __ATOMIC_ACQUIRE) > svcGetSystemTick()) {
    }
}
// Core 0 first: the spinner made for this core stops this thread at once.
static ROSA_CODE void rosaSimPause(void) {
    bool isNew3ds = false;
    APT_CheckNew3DS(&isNew3ds);
    int last = isNew3ds ? 2 : 1;
    logInfo(rosaSimText, (unsigned long) CTRPORT_ROSALINA_SIM_PAUSE_MS, last, last + 1);
    CtrLog_flush();
    __atomic_store_n(&rosaSimSpinEnd, svcGetSystemTick() + (u64) CTRPORT_ROSALINA_SIM_PAUSE_MS * ROSA_TICKS_PER_MS,
                     __ATOMIC_RELEASE);
    int made = 0;
    for (int core = 0; last >= core; core++) made += threadCreate(rosaSimSpin, nullptr, 4096, 0x18, core, true) != nullptr;
    if (made != last + 1) logInfo(rosaSimText, (unsigned long) CTRPORT_ROSALINA_SIM_PAUSE_MS, last, made);
}
#endif

// Every 5 ms: the buttons held now, from the HID shared memory's newest pad entry (as hidScanInput reads it, without
// touching libctru's scan state, which belongs to the game thread), and the switch if the combo started or stopped
// forming. When the app lights the bottom itself (dialogue, the options menu), the watcher lets go of it; if the app
// darkens it again while the combo is still held, the next poll lights it again.
static ROSA_CODE void rosaMain(void* arg) {
    (void) arg;
    bool lit = false;
    while (__atomic_load_n(&rosaQuit, __ATOMIC_ACQUIRE) == 0) {
        CtrRosalina_guardSleep(ROSA_POLL_NS);
        bool dark = CtrScreens_bottomDark();
#ifdef CTRPORT_ROSALINA_SIM
        static bool simPaused ROSA_BSS;
        if (!simPaused && rosaSimMs() >= ROSA_SIM_AT) {
            simPaused = true;
            rosaSimPause();
        }
        u32 simMs = rosaSimMs();
        u32 held = (simMs + 1000 >= ROSA_SIM_AT && ROSA_SIM_AT + 1500 > simMs) ||
                           (simMs >= ROSA_SIM_END + 4000 && ROSA_SIM_END + 4300 > simMs)
                       ? KEY_L | KEY_DDOWN
                       : 0;
#else
        vu32* shared = hidSharedMem; // mapped from before the watcher starts until after it is joined
        u32 index = shared[4];
        u32 held = 8 > index ? shared[10 + index * 4] : 0;
#endif
#ifdef CTRPORT_ROSALINA_PROBE
        rosaProbe(lit, dark);
#endif
        u32 part = held & rosaCombo;
        bool forming = (part & rosaAlone) != 0 || (part & (part - 1)) != 0; // one of those, or two or more buttons
        if (forming && dark) {
            if (!lit) {
                bool cleared = rosaClear();
                Result res = rosaBacklight(0x11);
                lit = true;
#ifdef CTRPORT_ROSALINA_PROBE
                rosaLitCount++;
                rosaClearCount += cleared;
                logInfo(rosaSwitchText, rosaOnText, (int) cleared, (unsigned long) res);
#else
                (void) cleared;
                (void) res;
#endif
            }
        } else if (lit) {
            lit = false;
            if (dark) {
                Result res = rosaBacklight(0x12);
#ifdef CTRPORT_ROSALINA_PROBE
                logInfo(rosaSwitchText, rosaOffText, 0, (unsigned long) res);
#else
                (void) res;
#endif
                if (!CtrScreens_bottomDark()) rosaBacklight(0x11); // the app lit it meanwhile: leave it lit
            }
        }
    }
}

// At exit (before libctru unmaps the HID shared memory and closes its services): the watcher stopped.
static ROSA_CODE void rosaStop(void) {
    __atomic_store_n(&rosaQuit, 1, __ATOMIC_RELEASE);
    threadJoin(rosaThread, 100000000ULL);
}

// The watcher, on Luma only, with Luma's configured combo (versions without the 0x101 query use the default). On a
// system core where it can be made (core 2 on the New models, else core 1, whose time limit the audio thread has
// raised), else the game's core, where each poll preempts the game for a few microseconds; above the game thread's
// priority (0x30, which nothing changes while it runs), like the input sampler. SELECT alone counts as forming the
// combo (it is never a game button), and so does L or R alone when the combo has only two buttons: otherwise the
// second button would light the screen only within a poll of Rosalina's own.
static ROSA_CODE void rosaStart(void) {
    rosaTried = true;
    s64 info; // read only after a call that succeeded
#ifdef CTRPORT_ROSALINA_SIM
    rosaSimStart = svcGetSystemTick();
#else
    if (R_FAILED(svcGetSystemInfo(&info, 0x10000, 0))) return; // not Luma3DS: no Rosalina
#endif
    u32 combo = ROSA_DEFAULT_COMBO;
    if (R_SUCCEEDED(svcGetSystemInfo(&info, 0x10000, 0x101)) && (u32) info != 0) combo = (u32) info;
    rosaCombo = combo;
    u32 rest = combo & (combo - 1);
    rosaAlone = combo & (KEY_SELECT | ((rest & (rest - 1)) == 0 ? KEY_L | KEY_R : 0));
    // Asked for core 2 only on a New model: Azahar exits on a thread for core 2 on an Old one.
    bool isNew3ds = false;
    APT_CheckNew3DS(&isNew3ds);
    for (int core = isNew3ds ? 2 : 1;; core = -2) {
        rosaThread = threadCreate(rosaMain, nullptr, ROSA_STACK_BYTES, 0x2D, core, false); // the game thread: 0x30
        if (rosaThread != nullptr || core == -2) break;
    }
    if (rosaThread != nullptr) atexit(rosaStop);
#ifdef CTRPORT_ROSALINA_PROBE
    logInfo(rosaStartText, (unsigned long) (rosaThread != nullptr ? rosaCombo : 0));
#endif
}

// Every backlight change closes gsp::Lcd through here (ctr_renderer.c, ctr_screens.c, ctr_optmenu.c); the first one
// that leaves the bottom backlight off starts the watcher.
ROSA_CODE void __wrap_gspLcdExit(void) {
    __real_gspLcdExit();
    if (!rosaTried && CtrScreens_bottomDark()) rosaStart();
}
