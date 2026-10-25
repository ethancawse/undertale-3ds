#include "ctr_exit.h"

#include <3ds.h>
#include <stdarg.h>
#include <string.h>

#include "log.h"
#include "ctr_lab.h"
#include "ctr_log.h"
#include "ctr_watchdog.h"

// Same entry used by logInfo; vLogInfo is otherwise dead-stripped, so calling
// it here would introduce a new function outside the late reserve.
extern void platformLog(logType type, const char* format, va_list args);

#define EXIT_TEXT __attribute__((noinline, section(".text.late.exithang")))
#define EXIT_RODATA __attribute__((section(".rodata.late.exithang")))
#define EXIT_BSS __attribute__((section(".bss.late.exithang")))

EXIT_BSS uint32_t CtrExit_started;
EXIT_BSS static Handle exitEvent;
EXIT_BSS static Handle exitThread;
EXIT_BSS static u32 exitStartedMs;
EXIT_BSS static u32 exitLimitMs;
EXIT_BSS static u32 exitStack[256] __attribute__((aligned(8)));
EXIT_RODATA static const char noticePhase[] = "game ended notice";

static EXIT_TEXT u32 exitNowMs(void) {
    return (u32) (svcGetSystemTick() / CPU_TICKS_PER_MSEC);
}

// This raw SVC thread deliberately never enters newlib or the allocator/thread
// trampolines. It needs no initialized newlib TLS, exception handler, service
// session, log lock or report callback. It signals the TLS recovery worker.
static EXIT_TEXT void exitGuard(void* arg) {
    (void) arg;
    if (svcWaitSynchronization(exitEvent, U64_MAX) != 0) CtrExitFiles_exitDeadline();
    u32 since = exitStartedMs;
    u32 lastProgress = CtrWatchdog_progress();
    for (;;) {
        u32 now = exitNowMs();
        u32 progress = CtrWatchdog_progress();
        // UNDERTALE deliberately waits for A after game_end. Keep that notice
        // open while its loop is responsive; a wedged notice still times out.
        if (progress != lastProgress && strcmp(CtrWatchdog_phase(), noticePhase) == 0) since = now;
        lastProgress = progress;
        if ((u32) (now - since) >= exitLimitMs) {
            CtrExitFiles_exitDeadline();
        }
        svcSleepThread(100000000LL);
    }
}

EXIT_TEXT void CtrExit_openLog(const char* path) {
    // No gameplay until a deadline trigger exists. Unlike threadCreate, creation
    // cannot silently strand the guard in threadTrampoline's initial free().
    Result result = svcCreateEvent(&exitEvent, RESET_STICKY);
    if (result == 0) result = svcCreateThread(&exitThread, exitGuard, 0,
                                            exitStack + sizeof(exitStack) / sizeof(exitStack[0]), 0x18, 0);
    if (result != 0) {
        CtrExitFiles_startFailed();
    }
    CtrLog_openFile(path);
}

EXIT_TEXT void CtrExit_begin(int seconds) {
    if (!__atomic_load_n(&CtrExit_started, __ATOMIC_ACQUIRE)) {
        exitStartedMs = exitNowMs();
        // Give the existing lab guard its 40 s report/chainload attempt and
        // 25 s fallback budget. The TLS recovery worker handles this trigger;
        // a file batch instead has its own idle and absolute limits.
        exitLimitMs = (u32) (seconds > 0 ? seconds : 40) * 1000u + (CtrLab_active() ? 25000u : 0u);
        __atomic_store_n(&CtrExit_started, true, __ATOMIC_RELEASE);
        svcSignalEvent(exitEvent);
    }
    CtrLab_exiting(seconds);
}

// Keep main's call order and slot: moving its exit call before logInfo shrinks
// UNDERTALE's main by 8 B. Arm here, before even formatting the first exit line.
EXIT_TEXT void CtrExit_logBegin(const char* format, ...) {
    CtrExit_begin(40);
    va_list args;
    va_start(args, format);
    platformLog(LOG_TYPE_NORMAL, format, args);
    va_end(args);
}
