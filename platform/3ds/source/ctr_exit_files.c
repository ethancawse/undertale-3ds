#include "ctr_exit.h"
#include "ctr_exit_files.h"
#include "ctr_exit_log_state.h"
#include "ctr_link_files.h"
#include "ctr_lab.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define FILES_TEXT __attribute__((noipa, section(".text.late.exithang.files")))
#define FILES_BSS __attribute__((section(".bss.late.exithang.files")))
// Old 2DS: 288,054 B / 2,113 ms; a default 16 MiB batch needs about
// 123 s before SD/CRC/log costs. Progress renews idle time, never this cap.
#define FILES_IDLE_MS 25000u
#define FILES_LIMIT_MS 300000u
#define FILES_CANCEL_MS 5000u

FILES_BSS static u32 filesPeer;
FILES_BSS static int filesSocket;
FILES_BSS static u32 filesStartedMs;
FILES_BSS static u32 filesActive, filesFinished, filesCancel, filesExitExpired, filesReady, filesSocClosing;
FILES_BSS static Thread filesGuardThread;
FILES_BSS static Handle filesMainThread;
FILES_BSS static LightEvent filesEvent;

static FILES_TEXT u32 filesNowMs(void) {
    return (u32) (svcGetSystemTick() / CPU_TICKS_PER_MSEC);
}

static FILES_TEXT void filesCancelSocket(void) {
    *CtrExit_logState.linkStop = true;
    // Only the sender owns close(): no descriptor reuse/free race.
    if (__atomic_load_n(&filesPeer, __ATOMIC_ACQUIRE)) shutdown(filesSocket, SHUT_RDWR);
}

static FILES_TEXT bool filesCloseSoc(void) {
    if (*CtrExit_filesLinkState.socUp) {
        u32 expected = 0;
        if (!__atomic_compare_exchange_n(&filesSocClosing, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return false;
        if (socExit() != 0) return false;
        *CtrExit_filesLinkState.socUp = false;
    }
    return true;
}

// A hard failure must still take libctru's FS/HID/APT loader path. _exit
// bypasses stdio/atexit locks that a wedged writer could own. In lab mode
// CtrLab_start already selected the lab host (--after died) before gameplay.
// SOC must acknowledge closure first. Never kill a process with live SOC.
static FILES_TEXT __attribute__((noreturn)) void filesRecover(void) {
    filesCancelSocket();
    *CtrExit_logState.stop = true;
    LightEvent_Signal(CtrExit_logState.event);
    if (*CtrExit_filesLinkState.thread != NULL) threadJoin(*CtrExit_filesLinkState.thread, 2000000000ULL);
    if (*CtrExit_logState.thread != NULL) threadJoin(*CtrExit_logState.thread, 250000000ULL);
    if (filesCloseSoc()) {
        if (filesMainThread != 0) svcSetThreadPriority(filesMainThread, 0x3F);
        _exit(1);
    }
    // SOC rejected closure: forced death would repeat the HOME error.
    for (;;) svcSleepThread(1000000000LL);
}

// Created before gameplay with normal TLS, so shutdown/_exit may use errno
// and the runtime. The raw guard never calls newlib or a service itself.
static FILES_TEXT void filesGuard(void* arg) {
    (void) arg;
    __atomic_store_n(&filesReady, true, __ATOMIC_RELEASE);
    if (!__atomic_load_n(&filesExitExpired, __ATOMIC_ACQUIRE) && !__atomic_load_n(&filesCancel, __ATOMIC_ACQUIRE))
        LightEvent_Wait(&filesEvent);
    u32 moved = CtrLinkFiles_progress(), movedAt = filesNowMs(), cancelledAt = 0;
    bool cancelled = false;
    while (!__atomic_load_n(&filesFinished, __ATOMIC_ACQUIRE)) {
        u32 now = filesNowMs(), progress = CtrLinkFiles_progress();
        bool batch = __atomic_load_n(&filesActive, __ATOMIC_ACQUIRE);
        if (progress != moved) { moved = progress; movedAt = now; }
        // The batch has its own limit. Lab's 40 s and the raw guard's 65 s
        // must not truncate a progressing Old transfer.
        if (batch && !cancelled) CtrLab_exitProgress();
        bool expired = batch ? (u32) (now - movedAt) >= FILES_IDLE_MS ||
                               (u32) (now - filesStartedMs) >= FILES_LIMIT_MS :
                               __atomic_load_n(&filesExitExpired, __ATOMIC_ACQUIRE);
        if (!cancelled && (expired || __atomic_load_n(&filesCancel, __ATOMIC_ACQUIRE))) {
            cancelled = true;
            cancelledAt = now;
            __atomic_store_n(&filesCancel, true, __ATOMIC_RELEASE);
            filesCancelSocket();
        }
        if (cancelled && (u32) (now - cancelledAt) >= FILES_CANCEL_MS) filesRecover();
        svcSleepThread(10000000LL);
    }
    // Keep recovery alive through main's return and newlib/loader cleanup.
    // A normal process exit destroys this idle watcher, like the old raw
    // guard. It must not return into threadTrampoline's heap cleanup here.
    svcSleepThread(FILES_CANCEL_MS * 1000000LL);
    filesRecover();
}

FILES_TEXT void CtrExitFiles_openLog(const char* path) {
    CtrExit_openLog(path);
    LightEvent_Init(&filesEvent, RESET_STICKY);
    svcDuplicateHandle(&filesMainThread, CUR_THREAD_HANDLE);
    filesGuardThread = threadCreate(filesGuard, NULL, 8192, 0x18, 0, false);
    if (filesGuardThread == NULL) CtrExitFiles_startFailed();
    u32 started = filesNowMs();
    while (!__atomic_load_n(&filesReady, __ATOMIC_ACQUIRE)) {
        if ((u32) (filesNowMs() - started) >= 500u) CtrExitFiles_startFailed();
        svcSleepThread(1000000LL);
    }
}

FILES_TEXT __attribute__((noreturn)) void CtrExitFiles_startFailed(void) {
    _exit(1); // before CtrLog_startLink/SOC initialization
}

FILES_TEXT __attribute__((noreturn)) void CtrExitFiles_exitDeadline(void) {
    __atomic_store_n(&filesExitExpired, true, __ATOMIC_RELEASE);
    LightEvent_Signal(&filesEvent);
    svcExitThread(); // this is the original raw guard, never main
}

FILES_TEXT __attribute__((noreturn)) void CtrExitFiles_closeFailed(void) {
    __atomic_store_n(&filesCancel, true, __ATOMIC_RELEASE);
    LightEvent_Signal(&filesEvent);
    // Bounded joins failed. Park main while the recovery worker owns cleanup.
    for (;;) svcSleepThread(1000000000LL);
}

// Lab exceptional leave/fallback must not abandon the socket service or
// bypass the normal loader cleanup (APT may still be in flight).
FILES_TEXT __attribute__((noreturn)) void CtrExitFiles_labExit(void) {
    filesCancelSocket();
    if (filesCloseSoc()) _exit(1);
    for (;;) svcSleepThread(1000000000LL);
}

FILES_TEXT int CtrExitFiles_connected(int sock, int command, int flags) {
    int result = fcntl(sock, command, flags);
    filesSocket = sock;
    __atomic_store_n(&filesPeer, result >= 0, __ATOMIC_RELEASE);
    return result;
}

FILES_TEXT int CtrExitFiles_shutdown(int sock, int how) {
    __atomic_store_n(&filesPeer, 0, __ATOMIC_RELEASE);
    return shutdown(sock, how);
}

FILES_TEXT void CtrExit_closeFilesLog(void) {
    if (!__atomic_load_n(&CtrExit_started, __ATOMIC_ACQUIRE) ||
        !CtrLinkFiles_enabled() || !__atomic_load_n(&filesPeer, __ATOMIC_ACQUIRE) ||
        *CtrExit_filesLinkState.thread == NULL) {
        // No connected peer: exactly the existing close path, with no extra
        // wait, filesystem work or guard creation. Never wait for a connect.
        CtrExit_closeLog();
        __atomic_store_n(&filesFinished, true, __ATOMIC_RELEASE);
        LightEvent_Signal(&filesEvent);
        return;
    }
    filesStartedMs = filesNowMs();
    __atomic_store_n(&filesActive, true, __ATOMIC_RELEASE);
    LightEvent_Signal(&filesEvent);
    CtrLinkFiles_prepare(CtrExit_filesLinkState.report, NULL);
    CtrExit_flush();
    // Finish the writer before allowing the sender to see an empty log ring.
    // Its last log line must precede the files header on the same connection.
    *CtrExit_logState.stop = true;
    if (*CtrExit_logState.thread != NULL) {
        LightEvent_Signal(CtrExit_logState.event);
        if (threadJoin(*CtrExit_logState.thread, 250000000ULL) != 0) CtrExitFiles_closeFailed();
    }
    *CtrExit_logState.linkStop = true;
    u32 waitAt = filesNowMs();
    while (threadJoin(*CtrExit_filesLinkState.thread, 100000000ULL) != 0) {
        if ((u32) (filesNowMs() - waitAt) >= FILES_LIMIT_MS + FILES_CANCEL_MS) CtrExitFiles_closeFailed();
    }
    // Joined sender: all prepared files and the end marker have been sent,
    // or the connection failed/cancelled. Close SOC before normal return.
    if (!filesCloseSoc()) CtrExitFiles_closeFailed();
    if (*CtrExit_logState.file != NULL) fclose(*CtrExit_logState.file);
    *CtrExit_logState.file = NULL;
    __atomic_store_n(&filesFinished, true, __ATOMIC_RELEASE);
}
