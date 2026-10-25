#include <3ds.h>
#include <errno.h>
#include <sys/socket.h>

#include "ctr_exit.h"
#include "ctr_link_files.h"

#define MIDFILE_TEXT __attribute__((noipa, section(".text.late.exithang.midfile")))
#define MIDFILE_BSS __attribute__((section(".bss.late.exithang.midfile")))

// libctru gives its GSP callback thread 4 KiB. The hint/FPS callback can
// load a font tile and log it: platformLog + postLine alone need over 3 KiB,
// on top of the callback, texture loader and number formatter's frames.
// Scope the extra 12 KiB to gspInit's thread; other stacks stay unchanged.
MIDFILE_BSS static u32 gspCreating;

Result __real_gspInit(void);
Result __wrap_gspInit(void);
MIDFILE_TEXT Result __wrap_gspInit(void) {
    gspCreating = 1;
    Result result = __real_gspInit();
    gspCreating = 0;
    return result;
}

Thread __real_threadCreate(ThreadFunc entry, void* arg, size_t stackSize, int prio, int core, bool detached);
Thread CtrExitGsp_threadCreate(ThreadFunc entry, void* arg, size_t stackSize, int prio, int core, bool detached);
MIDFILE_TEXT Thread CtrExitGsp_threadCreate(ThreadFunc entry, void* arg, size_t stackSize, int prio, int core, bool detached) {
    if (gspCreating && stackSize < 16384) stackSize = 16384;
    return __real_threadCreate(entry, arg, stackSize, prio, core, detached);
}

// send() only hands bytes to SOC. A joined sender does not prove that the
// peer got the final file/end lines: immediate SOC teardown can discard its
// queued tail. Half-close first; the receiver reads through FIN and closes
// its side, which acknowledges draining the ordered stream. The socket is
// already nonblocking. A dead/old peer gets at most 5 s, including inbound
// junk or EINTR; the existing cancellation worker can still shut it down.
MIDFILE_TEXT int CtrExitFiles_shutdownDrained(int sock, int how) {
    if (__atomic_load_n(&CtrExit_started, __ATOMIC_ACQUIRE) &&
        CtrLinkFiles_enabled() && CtrLinkFiles_progress() > 0 && shutdown(sock, SHUT_WR) == 0) {
        u32 start = (u32) (svcGetSystemTick() / CPU_TICKS_PER_MSEC);
        char discard[64];
        while ((u32) ((u32) (svcGetSystemTick() / CPU_TICKS_PER_MSEC) - start) < 5000u) {
            int n = recv(sock, discard, sizeof(discard), 0);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
            svcSleepThread(10000000LL);
        }
    }
    return CtrExitFiles_shutdown(sock, how);
}
