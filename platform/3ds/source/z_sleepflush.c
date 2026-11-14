// sleepflush (change auditfixe, SCOURS2-005, both games): a save the game just made reaches the card before the
// console sleeps or the HOME Menu opens.
//
// The save queue (ctr_file_system.c) waits WRITE_DELAY_NS (0.5 s) after a write is queued before it starts it, so the
// room's music can start first, and only CtrFileSystem_flush (the exit) skips the wait. A save point's file0 and
// undertale.ini queued just before the lid closed were still waiting when the console went to sleep: if the battery
// then ran out, or was pulled, during sleep, the next boot had the save before (CtrSafeSave keeps that one whole).
// HOME is the same window: the app's threads keep running in the HOME Menu, but a lid closed there sleeps the console
// with the write still waiting. On PC GameMaker's ini_close / file_text_close have written the file when they return.
//
// So an APT hook flushes the save queue on APTHOOK_ONSLEEP and APTHOOK_ONSUSPEND. libctru's aptHandleSleep (called
// by aptMainLoop on the game's thread) runs every hook with ONSLEEP, then sends the sleep reply and waits for the
// wake-up, so the flush ends before the console sleeps; aptJumpToHomeMenu runs the ONSUSPEND hooks the same way before
// it hands the screens over. With nothing queued the flush returns at once (no change to an ordinary sleep or HOME);
// with a save queued, sleep or the HOME Menu waits for that write (23-118 ms a file on the lab consoles' cards).
//
// The wait has one deadline for the whole transition (r2, R178 F1): a card whose writes fail (the queue's 8 tries 250
// ms apart, per file: 3.5 s for file0 and undertale.ini) or hang would otherwise hold the sleep or the HOME Menu for
// that long, or for good. Neither libctru nor 3dbrew gives a deadline to meet: aptHandleSleep and aptJumpToHomeMenu
// call the hooks and wait with no timeout (libctru apt.c), and 3dbrew's APT:ReplySleepNotificationComplete and PTM
// pages name none. So SLEEPFLUSH_BUDGET_MS is ours: about 4x the slowest save point measured (two files, 2 x 118 ms),
// and longer than the 0.5 s write delay, so a save still queued at the deadline is not held back again. The other hooks
// of the transition add little to it: the APT trace's and the watchdog's log flushes, the pacer's GPU wait (at most a
// frame) and flags. When the deadline passes the hook logs how many saves are not yet on the card and returns: they
// stay queued and are written after it (before the console sleeps, in the HOME Menu, or after the wake), and until then
// a power loss keeps the previous save (CtrSafeSave), as before this hook.
//
// How: main.c's CtrFileSystem_create and CtrFileSystem_destroy are reached through the linker
// (-Wl,--wrap=CtrFileSystem_create,--wrap=CtrFileSystem_destroy, CMakeLists.txt): the first create adds the hook,
// each create names the file system it flushes, and destroy forgets it first (the hook then does nothing). The hook
// is added after the watchdog's (hooks run newest first, so the watchdog is told of the sleep after the flush) and
// before the renderer's, the pacer's and the APT trace's. Code, constants and statics are late (late_reserve.ld:
// .text_sleepflush, .rodata_sleepflush, .text_sleepflush_r2, .bss.sleepflush_late).
#include <stdbool.h>
#include <stddef.h>

#include <3ds.h>
#include "ctr_file_system.h"
#include "log.h"

#define SLEEPFLUSH_CODE __attribute__((noinline, section(".text_sleepflush")))
#define SLEEPFLUSH_R2_CODE __attribute__((noinline, section(".text_sleepflush_r2")))
#define SLEEPFLUSH_BSS __attribute__((section(".bss.sleepflush_late")))
#define SLEEPFLUSH_RO __attribute__((section(".rodata_sleepflush")))
#define SLEEPFLUSH_BUDGET_MS 1000
#ifdef ENABLE_WAD16
void CtrHomesave_onApt(APT_HookType hook, void* param);
#endif
#line 47

FileSystem* __real_CtrFileSystem_create(const char* bundlePath, const char* savePath);
void __real_CtrFileSystem_destroy(FileSystem* fs);
FileSystem* __wrap_CtrFileSystem_create(const char* bundlePath, const char* savePath);
void __wrap_CtrFileSystem_destroy(FileSystem* fs);

static FileSystem* sleepflushSaves SLEEPFLUSH_BSS;
static aptHookCookie sleepflushCookie SLEEPFLUSH_BSS;
static bool sleepflushHooked SLEEPFLUSH_BSS;
// Named arrays, not literals: a literal's mergeable string section would let ld fold an earlier late string into it
// and move what follows (docs/3ds-hardware.md, "Mergeable late strings can move original constants in earlier inputs").
static const char sleepflushLateText[] SLEEPFLUSH_RO =
    "Files: %d saves not on the card after %d ms; %s goes ahead and they are written after it\n";
static const char sleepflushSleep[] SLEEPFLUSH_RO = "sleep";
static const char sleepflushHome[] SLEEPFLUSH_RO = "the HOME Menu";

// r2's bounded flush. Its own section, after r1's code, and it takes only the hook, so r1's hook keeps its 32 bytes
// (only its branch target changes) and nothing of r1 moves.
SLEEPFLUSH_R2_CODE static void sleepflushBounded(APT_HookType hook) {
    int left = CtrFileSystem_flushBy(sleepflushSaves, svcGetSystemTick() + (uint64_t) SYSCLOCK_ARM11 * SLEEPFLUSH_BUDGET_MS / 1000);
    if (left > 0)
        logInfo(sleepflushLateText, left, SLEEPFLUSH_BUDGET_MS, hook == APTHOOK_ONSLEEP ? sleepflushSleep : sleepflushHome);
}

SLEEPFLUSH_CODE __attribute__((used)) static void sleepflushOnApt(APT_HookType hook, void* param) {
    (void) param;
    if ((hook == APTHOOK_ONSLEEP || hook == APTHOOK_ONSUSPEND) && sleepflushSaves != NULL) {
        sleepflushBounded(hook);
    }
}

SLEEPFLUSH_CODE FileSystem* __wrap_CtrFileSystem_create(const char* bundlePath, const char* savePath) {
    FileSystem* fs = __real_CtrFileSystem_create(bundlePath, savePath);
    sleepflushSaves = fs;
    if (!sleepflushHooked) {
        sleepflushHooked = true;
#ifdef ENABLE_WAD16
        aptHook(&sleepflushCookie, CtrHomesave_onApt, NULL);
#else
        aptHook(&sleepflushCookie, sleepflushOnApt, NULL);
#endif
#line 83
    }
    return fs;
}

SLEEPFLUSH_CODE void __wrap_CtrFileSystem_destroy(FileSystem* fs) {
    if (sleepflushSaves == fs) sleepflushSaves = NULL;
    __real_CtrFileSystem_destroy(fs);
}

#ifdef ENABLE_WAD16
// homesave B (review sonrev10 delta): try the whole queue for at most 10 s, then
// hand off even if the card is hung. Outstanding saves retain the staged rules.
// The old callback and constants retain their slots for layout and DELTARUNE.
#include "ctr_safe_save.h"
void CtrHomesave_pause(void);
void CtrHomesave_logPending(FileSystem* fs, const char* format);
#define HOMESAVE_BUDGET_MS 10000
#ifdef CTRPORT_LABGUARD
void CtrHomesave_pauseLab(void);
#endif
// An assembler alias keeps the compiler's existing static-anchor ordering intact.
extern FileSystem* homesaveSaves __asm__("sleepflushSaves");
// Outside .text.* / .rodata.*: this object's old catch-all would take them early.
static const char homesaveFailed[] __attribute__((section(".homesave_const"))) =
    "Files: save failed before suspend/close; progress may be lost\n";
static const char homesaveQueued[] __attribute__((section(".homesave_const"))) =
    "Files: still queued: %s\n";
static const char homesaveClose[] __attribute__((section(".homesave_const"))) = "Close";

__attribute__((noinline, noclone, optimize("Os"), section(".homesave_code")))
void CtrHomesave_onApt(APT_HookType hook, void* param) {
    (void) param;
    if (hook != APTHOOK_ONSUSPEND && hook != APTHOOK_ONSLEEP && hook != APTHOOK_ONEXIT) return;
    // Hooks run newest first: our drain precedes the watchdog's ordinary pause.
    // Its existing restore/wakeup handler increments progress and unpauses it.
    // Close stays paused through the normal exit flush and watchdog stop.
    CtrHomesave_pause();
#ifdef CTRPORT_LABGUARD
    CtrHomesave_pauseLab();
#endif
    FileSystem* fs = homesaveSaves;
    if (fs == NULL) return;
    int left = CtrFileSystem_flushBy(fs, svcGetSystemTick() + (uint64_t) SYSCLOCK_ARM11 * HOMESAVE_BUDGET_MS / 1000);
    if (left > 0) {
        logInfo(sleepflushLateText, left, HOMESAVE_BUDGET_MS,
                hook == APTHOOK_ONSLEEP ? sleepflushSleep : hook == APTHOOK_ONEXIT ? homesaveClose : sleepflushHome);
        CtrHomesave_logPending(fs, homesaveQueued);
    }
    // A failed job is removed after eight tries: queue empty is not success.
    if (__atomic_load_n(&CtrSave_failed, __ATOMIC_ACQUIRE)) logError(homesaveFailed);
}
#endif
