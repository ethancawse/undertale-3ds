// Luma3DS's Rosalina menu with the other screen Off (change rosalina2, maintainer's o3DS report of 2026-10-25: "When I open
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
// frames (ndsp: about 205 a second) tell which part stopped if the game doesn't come back (change rosafreeze).
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
// while the GPU, the LCDs and the DSP run on; as near as Azahar gets (change rosafreeze), a thread at the highest
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
        // z_rosabl.c (bug 2): the simulated buttons go through the hold as the HID's do in other builds, and the app's
        // own bottom power-on and power-off are made inside the second hold and 2 s after it.
        u32 CtrRosabl_filter(u32 held);
        void CtrRosabl_simMenu(u32 simMs);
        static u8 simMenus ROSA_BSS;
        if (simMenus < 2 && simMs >= ROSA_SIM_END + (simMenus != 0 ? 6000 : 4100)) {
            simMenus++;
            CtrRosabl_simMenu(simMs);
        }
        u32 held = CtrRosabl_filter((simMs + 1000 >= ROSA_SIM_AT && ROSA_SIM_AT + 1500 > simMs) ||
                                            (simMs >= ROSA_SIM_END + 4000 && ROSA_SIM_END + 4300 > simMs)
                                        ? KEY_L | KEY_DDOWN
                                        : 0);
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
#ifdef ENABLE_WAD16
// R216b4..R216d (change auditfixe2; c26rev6 R240 F1, c26rev2 R216b4 findings 2 and 3, integrate21's zq65 finding).
// UNDERTALE only, as the whole crash-screen net (ctr_r216_ut.h): DELTARUNE's watcher is as before R216, byte for byte.
// The crash screen stops the watcher before its first paint. Its painter (z_argcrash.c's argcrashPaintGated) lights
// both screens on a gsp::Lcd session of its own, past ctr_screens.c's wraps, so the bottom's dark mark stays set; a
// watcher still running then took the lit error screen for a dark bottom: a combo cleared the bottom error and its
// release switched the bottom off. So the watcher is told to stop (the exit's flag, rosaQuitRef) and waited for up to
// 100 ms (its poll is 5 ms). That wait is not a finished stop: a pass the watcher had already begun, held past the
// wait, could still clear the bottom buffer or send a power-off after the crash screen painted, and rosaStart stores
// the watcher's handle with a plain store that the stop read from another thread. Four parts, all here or in
// CMakeLists.txt, none in an existing body:
//  - Every CtrScreens_bottomDark call of this file is CtrRosalina_dark (a rename in CMakeLists.txt; the same call,
//    another target): once the stop flag is set the bottom counts as lit. So a pass that resumes after the stop
//    neither clears (rosaClear's own check, under the consoles' lock), nor lights, nor chooses to switch off. A full
//    fence comes before the flag's load, so of the watcher's send-then-look and the stop's flag-then-paint one always
//    sees the other. What it had already chosen still goes out: a power-off, and rosaMain's re-light after it
//    (`if (!CtrScreens_bottomDark()) rosaBacklight(0x11)`), which with rosafreeze3's z_rosabl.c is no light: there
//    the watcher's sends carry out the app's newest bottom request (CtrRosabl_send), Off when the bottom was set Off,
//    and the crash screen's light is not a request it has seen. The last part below settles both.
//  - rosaStart's thread create is CtrRosalina_threadPub (CMakeLists.txt's threadCreate rename), which publishes the
//    watcher's handle with a release store into rosaWatcher, read only atomically; rosaBorn says a watcher is being
//    created or exists, set before the create and checked against the stop flag (a full fence each side), so the stop
//    either sees it or the create sees the flag and makes no thread. rosaThread (the plain store) is now read only by
//    rosaStop at the exit, as before R216b4.
//  - CtrRosalina_quiet answers whether the watcher is quiet: true with no watcher (none made, or the create refused,
//    or this is the watcher's own thread), or once its thread has ended (libctru's threadJoin answers 0 then;
//    0x09401BFE when the 100 ms ran out). False while it is being created or still running past the wait: a clear
//    already past its check can then still land after the paint. z_argcrash.c's CtrArgcrash_quiet asks the pending
//    screen's paint (the one-shot screen) again in that case, each frame until the watcher is quiet, so while frames
//    keep coming one paint follows its end; argcrashEmergency's loop repaints every 30 frames anyway. The departing
//    screen has no later frame: its painter repaints after further stops itself, for up to a second (R216c,
//    z_argcrash.c's CtrArgcrash_depart).
//  - The watcher's last act (R216d): its thread runs CtrRosalina_watchRun, which runs rosaMain and then, if a crash
//    screen stopped it (rosaCrashStop, set before the stop flag), powers the bottom backlight on, as the crash screen
//    wants, on a gsp::Lcd session of its own (rosaBacklight's way). Nothing the watcher sends comes after it, so a
//    pass held past the wait leaves the bottom lit whatever it sent (a clear that landed after the paint still blanks
//    the bottom's text until the next paint: argcrashEmergency's every 30 frames, or a pending screen's next frame).
//    It goes to gsp::Lcd as it is, not through z_rosabl.c (whose rename of this file's svcSendSyncRequest the #undef
//    below drops for it): rosabl sends the app's newest request, and this is that request, the crash screen's, which
//    is made past the wraps on purpose (R188g F2: not on libctru's gsp::Lcd session, shared with the game thread) and
//    which only the watcher's own thread can send after the watcher's last send. A normal exit (rosaStop) sets no
//    flag: the watcher ends there as before.
//  How it is linked, so that no existing function or static of this file moves (GCC orders a file's functions and
//  statics by their calls and first uses, so a new callee or a new user of a static defined here would reorder them;
//  the stop has no caller here): the bodies below are named CtrRosalina_darkBody and CtrRosalina_pubBody, which
//  nothing here calls, and the asm after them gives each its rename's name (a link-time alias GCC doesn't see), so
//  every existing call is to an outside function as before; CtrRosalina_watchRun calls rosaMain through the pointer
//  the create was given. They and the stop name this file's statics by their link
//  names (z_argcrash.c's way). The statics' section is named outside .bss.* (nobits, as z_leavekeep.c's), so
//  late_reserve.ld's catch-all for this file's .bss.* can't take it ahead of the late statics it would move.
#define ROSA_QUIET_BSS __attribute__((section(".bss_rosaquiet_late,\"aw\",%nobits @")))
#define ROSA_QUIET_CODE __attribute__((noinline, noipa, section(".text_rosaquiet")))
Thread rosaWatcher ROSA_QUIET_BSS; // the watcher's handle once created (release store, atomic loads)
u32 rosaBorn ROSA_QUIET_BSS;       // 1 from the watcher's create on; 0 again if the create made no thread
void (*rosaWatchEntry)(void*) ROSA_QUIET_BSS; // the watcher's entry (rosaMain), stored before its create
u32 rosaCrashStop ROSA_QUIET_BSS;  // 1 once a crash screen has stopped the watcher (CtrRosalina_quiet)
extern u32 rosaQuitRef __asm__("rosaQuit");
extern const char rosaServiceRef[] __asm__("rosaService"); // "gsp::Lcd"

Thread CtrRosalina_threadCreate(void (*entry)(void*), void* arg, size_t stackBytes, int prio, int core, bool detached);
#undef CtrScreens_bottomDark
bool CtrScreens_bottomDark(void);
#undef svcSendSyncRequest // z_rosabl.c's rename (CtrRosabl_send): the last light goes to gsp::Lcd as it is
Result svcSendSyncRequest(Handle session);

// The watcher's thread: rosaMain, then the crash screen's bottom light if a crash screen stopped it (the last part
// above). Its result is not looked at: there is nothing else to try, and the painter lit the bottom too.
ROSA_QUIET_CODE void CtrRosalina_watchRun(void* arg) {
    rosaWatchEntry(arg);
    if (__atomic_load_n(&rosaCrashStop, __ATOMIC_SEQ_CST) == 0) return;
    Handle lcd = 0;
    if (R_FAILED(srvGetServiceHandle(&lcd, rosaServiceRef))) return;
    u32* cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(0x11, 1, 0); // PowerOnBacklight
    cmd[1] = GSPLCD_SCREEN_BOTTOM;
    svcSendSyncRequest(lcd);
    svcCloseHandle(lcd);
}

ROSA_QUIET_CODE bool CtrRosalina_quiet(void) {
    __atomic_store_n(&rosaCrashStop, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rosaQuitRef, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&rosaBorn, __ATOMIC_SEQ_CST) == 0) return true;
    Thread watcher = __atomic_load_n(&rosaWatcher, __ATOMIC_ACQUIRE);
    if (watcher == nullptr) return false; // created, not yet published: it may be in a pass
    if (watcher == threadGetCurrent()) return true;
    return threadJoin(watcher, 100000000ULL) == 0;
}

ROSA_QUIET_CODE bool CtrRosalina_darkBody(void) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return __atomic_load_n(&rosaQuitRef, __ATOMIC_SEQ_CST) == 0 && CtrScreens_bottomDark();
}

// The watcher is this file's one joinable thread (CTRPORT_ROSALINA_SIM's spinners are detached).
ROSA_QUIET_CODE Thread CtrRosalina_pubBody(void (*entry)(void*), void* arg, size_t stackBytes, int prio, int core,
                                           bool detached) {
    if (detached) return CtrRosalina_threadCreate(entry, arg, stackBytes, prio, core, detached);
    __atomic_store_n(&rosaBorn, 1, __ATOMIC_SEQ_CST);
    Thread made = nullptr;
    rosaWatchEntry = entry;
    if (__atomic_load_n(&rosaQuitRef, __ATOMIC_SEQ_CST) == 0)
        made = CtrRosalina_threadCreate(CtrRosalina_watchRun, arg, stackBytes, prio, core, detached);
    if (made != nullptr) __atomic_store_n(&rosaWatcher, made, __ATOMIC_RELEASE);
    else __atomic_store_n(&rosaBorn, 0, __ATOMIC_SEQ_CST);
    return made;
}

__asm__(".global CtrRosalina_dark\n"
        ".type CtrRosalina_dark, %function\n"
        ".set CtrRosalina_dark, CtrRosalina_darkBody\n"
        ".global CtrRosalina_threadPub\n"
        ".type CtrRosalina_threadPub, %function\n"
        ".set CtrRosalina_threadPub, CtrRosalina_pubBody\n");
#endif
#ifdef CTRPORT_ROSALOW
// UT-001 on UNDERTALE Old (z_rosalow.c): the watcher from the session's start, whatever the bottom backlight does. Named
// like the code above (the statics by their link names, nothing here called), so no function or static of this file moves.
extern void rosaStartRef(void) __asm__("rosaStart");
extern bool rosaTriedRef __asm__("rosaTried");
__attribute__((noinline, noipa, section(".text_rosalow"))) void CtrRosalina_startAll(void) {
    if (!rosaTriedRef) rosaStartRef();
}
#endif

#ifdef ENABLE_WAD16
// Keep the existing watcher bodies and slots unchanged. A keep-lit dim does
// not mark the bottom powered off, so New also needs this explicit first-dim
// start, after the last LCD reference is released.
extern void dimlightRosaStart(void) __asm__("rosaStart");
extern bool dimlightRosaTried __asm__("rosaTried");
__attribute__((noinline, noipa, section(".text.late.dimlight"))) void CtrDimlight_startWatcher(void) {
    if (!dimlightRosaTried) dimlightRosaStart();
}
// The base body is return-value-elided by GCC in release builds. Keep its
// compiled slot; the retry path needs this separate Result-preserving client.
Result CtrRosabl_send(Handle session);
__attribute__((noinline, noipa, section(".text.late.dimlight")))
static Result dimlightRosaBacklight(u32 command) {
    Handle lcd = 0;
    Result res = srvGetServiceHandle(&lcd, rosaService);
    if (R_FAILED(res)) return res;
    u32* cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(command, 1, 0);
    cmdbuf[1] = GSPLCD_SCREEN_BOTTOM;
    // The crash-stop tail above deliberately undefines the original rename.
    // This watcher client must explicitly retain rosabl's request reconciliation.
    res = CtrRosabl_send(lcd);
    if (R_SUCCEEDED(res)) res = (Result) cmdbuf[1];
    svcCloseHandle(lcd);
    return res;
}
u32* CtrDimlight_watcherPending(void);
__attribute__((noinline, noipa, section(".text.late.dimlight"))) Result CtrDimlight_watcherPower(u32 command) {
    u32* pending = CtrDimlight_watcherPending();
    if (pending) __atomic_store_n(pending, command, __ATOMIC_RELEASE);
    Result result = dimlightRosaBacklight(command);
    if (pending && R_SUCCEEDED(result)) {
        u32 expected = command;
        __atomic_compare_exchange_n(pending, &expected, 0, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    return result;
}
__attribute__((noinline, noipa, section(".text.late.dimlight"))) void CtrDimlight_retryWatcher(void) {
    u32* pending = CtrDimlight_watcherPending();
    if (!pending) return;
    if (__atomic_load_n(&rosaQuitRef, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(pending, 0, __ATOMIC_RELEASE);
        return;
    }
    u32 command = __atomic_load_n(pending, __ATOMIC_ACQUIRE);
    // The existing rosabl send continuation reconciles against the latest app
    // intent when the combo is released. A failed one-shot remains pending even
    // though rosaMain's unchanged local lit flag already advanced.
    if (command) CtrDimlight_watcherPower(command);
}
#endif
