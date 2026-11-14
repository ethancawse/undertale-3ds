// Rosalina's menu stayed dark after spamming its combo or SELECT (change rosafreeze3, bug 2; maintainer, o3DS XL, 11-04,
// other screen Off): Rosalina opened and answered, but the bottom screen stayed unlit.
//
// Rosalina's lock doesn't hold GSP (mask 3: rosalinaThreadLockPredicate returns inside its `mask & 1` branch,
// synchronization.c:102-113, v13.4), so a bottom power-off already sent when the menu opens is carried out after the
// menu is drawn. Two senders kept sending them while the buttons were spammed: ctr_rosalina.c's watcher, whose off
// follows every release of the combo, and the screen set-up after the options menu (SELECT) closes.
//
// So once the combo forms, no bottom power-off is sent while any of its buttons stays down, nor for ROSABL_GRACE_POLLS
// of the watcher's polls (each at least 5 ms: 150 ms, three of Rosalina's 50 ms polls) after the last one that saw
// it forming or a button of it down:
// - The watcher's buttons come through here. CMakeLists.txt renames, in ctr_rosalina.c only, its sleep to
//   CtrRosabl_sleep and the HID shared memory it reads to CtrRosabl_hid: after each sleep, this reads the newest pad
//   entry as the watcher did and hands it on with the combo's buttons added while the hold lasts. The watcher then
//   keeps its light through the hold and powers it off after it.
// - The app's power-offs come through CtrRosabl_powerOff: ctr_screens.c's wrap makes its real call here (renamed by
//   CMakeLists.txt, for that file only). While the hold lasts, a bottom power-off is held back and flagged; the wrap
//   marks the bottom dark after the call returns. The flag keeps the hold going until that mark shows, then that
//   request (and no newer one: the flag carries a count) is taken and starts the hold again, so the watcher (which
//   sees the bottom dark) clears and lights the screen, then powers it off once the hold is over. A bottom power-on
//   that works (ctr_screens.c's power-on calls, renamed to CtrRosabl_powerOn) cancels the flag: the app wants the
//   bottom lit after all.
// - The watcher's own power-off after the hold, and its power-on if the app lit the bottom meanwhile, come through
//   CtrRosabl_send (ctr_rosalina.c's svcSendSyncRequest, renamed there): they carry out the app's newest bottom
//   request instead, so they never undo one (R217c review: the power-on, decided on a dark read made before the app's
//   newer power-off, landed after it, and the bottom stayed lit). See CtrRosabl_send.
// What's left is a power-off sent before the watcher's first poll that sees the combo forming (5 ms or less after it
// starts to) and still in gsp::Lcd when Rosalina stops the app. One of its buttons alone doesn't start a hold: Down is
// a game button, and walking down must not light the bottom.
//
// ctr_rosalina.c and ctr_screens.c keep every instruction (only the five renamed call and load targets differ), since
// on UNDERTALE Old the freebytes H1 holes after their late sections pad 0 B: they can't grow. All of this is appended
// late code and statics (.text_rosabl, .bss.rosabl_late in late_reserve.ld). Without Luma the watcher never starts, the
// hold stays clear and the wrap's call goes straight on. Release builds log nothing from it.

#include <3ds.h>

#ifdef CTRPORT_ROSALINA_PROBE
#include "log.h"
#endif

#if defined(__3DS__) && defined(__GNUC__)
#pragma GCC optimize("Os")
#endif

#define ROSABL_CODE __attribute__((noinline, section(".text_rosabl")))
#define ROSABL_BSS __attribute__((section(".bss.rosabl_late")))
#ifndef ROSABL_GRACE_POLLS // UNDERTALE Old sets 36 (CMakeLists.txt): its watcher passes at HID's 234 a second
#define ROSABL_GRACE_POLLS 30 // 150 ms or more: a poll is a 5 ms sleep, and a pause (Rosalina's) stops the count
#endif
#define ROSABL_DEFAULT_COMBO (KEY_L | KEY_DDOWN | KEY_SELECT) // ctr_rosalina.c's ROSA_DEFAULT_COMBO

// The watcher's view of the HID shared memory: the newest entry's index (always 0) and that entry's held buttons, at
// the word offsets it reads (4 and 10).
typedef struct {
    u32 head[4];
    u32 index;
    u32 gap[5];
    u32 held;
} RosablPad;
static RosablPad rosablPad ROSABL_BSS;
ROSABL_BSS vu32* CtrRosabl_hid;
static u32 rosablCombo ROSABL_BSS, rosablAlone ROSABL_BSS;
static u8 rosablLeft ROSABL_BSS;   // the hold's polls left
static bool rosablHold ROSABL_BSS; // set by the watcher's thread at each poll, read by the wrap's
// A held-back power-off: bit 0 set while one is owed; bits 1-31 count every change the app's threads make (each new
// held-back power-off and each cancel), so the watcher takes exactly the request whose dark mark it saw (R217b).
static u32 rosablOwed ROSABL_BSS;
// The app's newest bottom power request: bit 0 set for a power-off, clear for a power-on; bits 1-31 count every one.
// Made before the request is sent (held back or not), so it is never older than what gsp::Lcd last did for the app.
static u32 rosablWant ROSABL_BSS;
// The watcher's own (only its thread sends through CtrRosabl_send): its sends of the app's request in this poll, and
// whether that request is still to be sent, at a later poll (R217d review).
static u8 rosablSends ROSABL_BSS;
static bool rosablRedo ROSABL_BSS;
#define ROSABL_POLL_SENDS 2

#ifdef CTRPORT_ROSALINA_PROBE
static const char rosablKeptText[] = "Rosalina: the app's bottom power-off was held back (combo held)\n";
#endif

// One poll's buttons through the hold: the combo is Luma's, read as rosaStart reads it, and "forming" is the
// watcher's own test (SELECT, or L or R alone for a two-button combo, or any two of its buttons). A flag left by a
// held-back power-off restarts the hold, so the watcher sees the combo until the grace is over: by then the wrap has
// marked the bottom dark, and the watcher has lit it and powers it off.
bool CtrScreens_bottomDark(void); // ctr_screens.c: the app has powered the bottom off (its wrap's mark)
static inline __attribute__((always_inline)) u32 rosablFilter(u32 held) {
    if (rosablCombo == 0) {
        s64 info; // read only after a call that succeeded
        u32 combo = ROSABL_DEFAULT_COMBO;
        if (R_SUCCEEDED(svcGetSystemInfo(&info, 0x10000, 0x101)) && (u32) info != 0) combo = (u32) info;
        u32 rest = combo & (combo - 1);
        rosablAlone = combo & (KEY_SELECT | ((rest & (rest - 1)) == 0 ? KEY_L | KEY_R : 0));
        rosablCombo = combo;
    }
    // A held-back power-off is taken only once the wrap has marked the bottom dark, which it does after
    // CtrRosabl_powerOff returns; until then the hold goes on (R217 F2: taken before that, the watcher never lit the
    // bottom, so it never powered it off). The mark comes after the flag, so a poll can see the flag without it.
    // Acquire pairs with the flag's release: a mark read after it is that request's or newer, never one cleared before
    // it. Taking it is a compare-exchange on the whole word, so if a power-on cancelled the request and a new one was
    // flagged between this load and the take (its mark not yet made), the take fails and the new one stays owed
    // (R217b: a plain clear here dropped it, and the bottom stayed lit).
    u32 owedWord = __atomic_load_n(&rosablOwed, __ATOMIC_ACQUIRE);
    bool owed = (owedWord & 1) != 0;
    if (owed && CtrScreens_bottomDark() &&
        __atomic_compare_exchange_n(&rosablOwed, &owedWord, owedWord & ~1u, false, __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED)) {
#ifdef CTRPORT_ROSALINA_PROBE
        logInfo(rosablKeptText);
#endif
    }
    u32 part = held & rosablCombo;
    u32 left = rosablLeft;
    // Forming, a held-back power-off, or any of the combo's buttons still down while the hold lasts (R217 F1: L alone
    // after the combo let the grace run out and the watcher power the bottom off with L down).
    if ((part & rosablAlone) != 0 || (part & (part - 1)) != 0 || owed || (left != 0 && part != 0))
        left = ROSABL_GRACE_POLLS + 1;
    if (left != 0) left--;
    rosablLeft = (u8) left;
    __atomic_store_n(&rosablHold, left != 0, __ATOMIC_RELAXED);
    return left != 0 ? held | rosablCombo : held;
}

// Sends the app's newest bottom request on the watcher's gsp::Lcd session, until that request is the same after the
// send as before it, but at most ROSABL_POLL_SENDS times in one poll: an app that keeps changing it would otherwise
// keep the watcher from its next poll (R217d review: 8,193 sends in one poll). What's left is sent at the next poll.
static ROSABL_CODE Result rosablReconcile(Handle session) {
    u32* cmd = getThreadCommandBuffer();
    Result res = 0;
    cmd[1] = 0; // rosaBacklight's result when nothing is sent
    rosablRedo = true;
    while (rosablSends < ROSABL_POLL_SENDS) {
        rosablSends++;
        u32 want = __atomic_load_n(&rosablWant, __ATOMIC_ACQUIRE);
        cmd[0] = IPC_MakeHeader(0x11 + (want & 1), 1, 0); // PowerOnBacklight, or PowerOffBacklight
        cmd[1] = GSPLCD_SCREEN_BOTTOM;
        res = svcSendSyncRequest(session);
        if (__atomic_load_n(&rosablWant, __ATOMIC_SEQ_CST) == want) {
            rosablRedo = false;
            break;
        }
    }
    return res;
}

static const char rosablService[] = "gsp::Lcd";

// The watcher's sleep (ctr_rosalina.c's CtrRosalina_guardSleep, renamed there), then its buttons for this poll, and
// the app's request if a poll before this one left it unsent. Not while the hold lasts: the watcher lights the bottom
// for Rosalina then, and its power-off after the hold sends the request (CtrRosabl_send). Until then it stays owed,
// also when the watcher no longer has the bottom lit and sends nothing of its own.
void CtrRosalina_guardSleep(s64 ns);
void CtrRosabl_sleep(s64 ns);
ROSABL_CODE void CtrRosabl_sleep(s64 ns) {
    CtrRosalina_guardSleep(ns);
    rosablSends = 0;
#ifndef CTRPORT_ROSALINA_SIM // the simulation's buttons come through CtrRosabl_filter in ctr_rosalina.c
    vu32* shared = hidSharedMem; // mapped from before the watcher starts until after it is joined
    u32 index = shared[4];
    rosablPad.held = rosablFilter(8 > index ? shared[10 + index * 4] : 0);
    CtrRosabl_hid = (vu32*) &rosablPad;
#endif
    Handle lcd = 0; // a session of its own for the send, as rosaBacklight's
    if (rosablRedo && !__atomic_load_n(&rosablHold, __ATOMIC_RELAXED) &&
        R_SUCCEEDED(srvGetServiceHandle(&lcd, rosablService))) {
        rosablReconcile(lcd);
        svcCloseHandle(lcd);
    }
}

// A new count for the held-back power-off word or the request word, with bit 0 set or not (the app's threads; the
// watcher only clears the held-back word's bit 0). Sequentially consistent: a new request is seen by the watcher
// before gsp::Lcd gets it (the barrier after the store comes before the request's svc).
static ROSABL_CODE void rosablCount(u32* at, u32 bit) {
    u32 word = __atomic_load_n(at, __ATOMIC_RELAXED);
    while (!__atomic_compare_exchange_n(at, &word, ((word | 1u) + 1u) | bit, true, __ATOMIC_SEQ_CST,
                                        __ATOMIC_RELAXED)) {}
}

// ctr_screens.c's power-off wrap's real call.
Result __real_GSPLCD_PowerOffBacklight(u32 screen);
Result CtrRosabl_powerOff(u32 screen);
ROSABL_CODE Result CtrRosabl_powerOff(u32 screen) {
    if ((screen & GSPLCD_SCREEN_BOTTOM) != 0) rosablCount(&rosablWant, 1);
    if (screen == GSPLCD_SCREEN_BOTTOM && __atomic_load_n(&rosablHold, __ATOMIC_RELAXED)) {
        rosablCount(&rosablOwed, 1);
        return 0;
    }
    return __real_GSPLCD_PowerOffBacklight(screen);
}

// ctr_screens.c's power-on calls (its wrap's and HOME, sleep and exit's): a bottom power-on that worked cancels a
// held-back power-off, so the hold doesn't wait for a dark mark that will not come.
Result __real_GSPLCD_PowerOnBacklight(u32 screen);
Result CtrRosabl_powerOn(u32 screen);
ROSABL_CODE Result CtrRosabl_powerOn(u32 screen) {
    if ((screen & GSPLCD_SCREEN_BOTTOM) != 0) rosablCount(&rosablWant, 0);
    Result res = __real_GSPLCD_PowerOnBacklight(screen);
    if (R_SUCCEEDED(res) && (screen & GSPLCD_SCREEN_BOTTOM) != 0) rosablCount(&rosablOwed, 0);
    return res;
}

// The watcher's gsp::Lcd request (ctr_rosalina.c's rosaBacklight, its only svcSendSyncRequest; renamed there). While
// the hold lasts it lights the bottom for Rosalina's menu and goes as it is. Otherwise it is the power-off after the
// hold or the power-on after that (the app lit the bottom meanwhile, by the dark mark), and each sends the app's newest
// request instead, again until that request is the same after the send as before it (rosablReconcile: at most two sends
// a poll, the rest at the next). The app's thread makes a request before sending it, so whichever of the two sends
// gsp::Lcd does last, the bottom ends as the app's newest request wants (R217c review: the power-on's dark read was
// older than the app's newer power-off, and the on came after it). A request the app makes later is sent by the app
// after this. The hold is the watcher thread's own flag; while it lasts the watcher sees the combo forming, and only
// then lights the bottom.
Result svcSendSyncRequest(Handle session);
Result CtrRosabl_send(Handle session);
ROSABL_CODE Result CtrRosabl_send(Handle session) {
    if (__atomic_load_n(&rosablHold, __ATOMIC_RELAXED)) return svcSendSyncRequest(session);
    return rosablReconcile(session);
}

#ifdef CTRPORT_ROSALINA_SIM
// The simulated buttons (ctr_rosalina.c's SIM block) take the same path.
u32 CtrRosabl_filter(u32 held);
ROSABL_CODE u32 CtrRosabl_filter(u32 held) { return rosablFilter(held); }
// Azahar only: the app's own bottom power-on and power-off through the wraps, back to back, as the options menu's open
// and the set-up after its close make them (ctr_rosalina.c calls it inside its second simulated hold, where the
// power-off is held back, and 2 s later, where it isn't).
static const char rosablSimText[] =
    "Rosalina sim: app power-on %08lX, power-off %08lX at %lu ms (hold %d); bottom dark %d\n";
void CtrRosabl_simMenu(u32 simMs);
ROSABL_CODE void CtrRosabl_simMenu(u32 simMs) {
    Result on = -1, off = -1;
    if (R_SUCCEEDED(gspLcdInit())) {
        on = GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
        off = GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM);
        gspLcdExit();
    }
    logInfo(rosablSimText, (unsigned long) on, (unsigned long) off, (unsigned long) simMs,
            (int) __atomic_load_n(&rosablHold, __ATOMIC_RELAXED), (int) CtrScreens_bottomDark());
}
#endif

// Dimlight observes the existing hold without duplicating combo or grace
// logic. New code belongs to its own appended section, not this change's slot.
__attribute__((noinline, section(".text.late.dimlight"))) bool CtrDimlight_rosalinaHeld(void) {
    return __atomic_load_n(&rosablHold, __ATOMIC_ACQUIRE);
}
