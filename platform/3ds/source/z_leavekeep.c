// leavekeep (credfix, charter rev 1 item 1, 2026-10-27): a lab leave ends the process without unmapping the heap.
//
// Why: the lab's crash, hang and fallback leaves (ctr_lab.c leave/standAside/fallbackMain) leave audio, the GPU and
// every other thread running on purpose, then end through CtrExitFiles_labExit > _exit > __ctru_exit > __appExit >
// __libctru_exit. libctru's __libctru_exit frees the linear heap and the application heap (svcControlMemory
// MEMOP_FREE) before svcExitProcess, and threadCreate put every thread's stack on that heap. The NDSP thread wakes on
// the DSP's next interrupt, its first stack write faults, and Luma3DS shows its crash screen instead of the lab host
// coming back (n3DS dump 17, 2026-10-27 05:26: ndspiReadChnState's push, FAR = sp - 40; docs/3ds-hardware.md, "A lab
// crash report ends on Luma's crash screen when NDSP is running"). ctr_exit_files.c's filesRecover is the same.
//
// What it does: those two callers end through CtrLeavekeep_exit, which marks the exit and calls _exit as before, so
// every _exit wrap, the inputlate stop, __appExit's service exits (FS, HID, APT, srv), __ctru_exit's stack switch,
// envDestroyHandles, __sync_fini and svcExitProcess all run as they did. Only the heap frees change: once the exit is
// marked, and when no loader return address was given (Luma3DS's hb:ldr starts each .3dsx as its own process:
// __system_retAddr is NULL), svcControlMemory's MEMOP_FREE calls are skipped. The kernel frees the process's memory
// when it ends the process, threads included. Every other exit, and every other memory call, is untouched.
//
// The lab crash test after NDSP starts (exittrail test builds only, CTRPORT_EXITTRAIL; not in release since 10-27, for
// UNDERTALE Old's late room): --lab-crash KIND --ini labcrash_after=SECONDS (1-600) fires the crash test
// SECONDS after the start, on a thread of its own, while the game runs with audio up, instead of before NDSP starts
// (main.c's --lab-crash call comes before audio). KIND is data, prefetch, undef, break or abort; thread and hang keep
// their own timing. labcrash_after=-SECONDS does the same but leaves with the heap freed, as before this fix (the
// control arm of the console test). The log shows main.c's "Lab: crashing on purpose" line and the arguments only.
//
// exitonce (2026-10-27): one thread runs the exit. ctr_exit_files.c's filesGuard ends the process itself when the
// exit's file batch stalls (filesRecover > _exit), and the main thread can still come unstuck and return from main
// into newlib's exit > _exit. On the o2DS both then ran __appExit at once (credfix-p2-et2-o2-dodge1/2, exit trail
// 2026-10-27 11:20: fsExit, hidExit and aptExit twice, from the guard and from main): the lab host never started and
// the console fell back to the Homebrew Launcher. Now the first thread into _exit owns it; any other thread that
// reaches _exit sleeps there until the owner's svcExitProcess ends it, and the heap stays mapped to the end (its
// stack is on it). If the owner has not ended the process 15 s later, the waiting thread ends it with
// svcExitProcess (the loader hand-back is lost then, as it was before).
// A filesRecover exit also takes the lab's leave and points hb:ldr at the lab host itself (ctr_lab.c's
// CtrLab_aimHost(true)): it skips newlib's atexit handlers, where the lab's onExit normally does that. And on the main
// loop's first CtrLab_poll it points hb:ldr at the lab host with "--after died" (CtrLab_aimHost(false)), which
// CtrLab_start meant to do but z_argcrash.c's wrap dropped, so a process that ends any other way still goes back to
// the lab host. (The o2DS et3 row, 2026-10-27 11:48, fell back to the Homebrew Launcher after a filesRecover exit: the
// exit's file batch stat()ed 139 files in a 1,189-file game folder for 34 s. The lab host now keeps game folders
// small instead: platform/3ds/labhost/labhost.c, "Game folder upkeep".)
#include <3ds.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ctr_exit_files.h"
#include "ctr_lab.h"
#include "ctr_launch_args.h"
#include "ctr_wrap_chain.h"

#define LK_CODE __attribute__((noinline, section(".text.leavekeep_late")))
#define LK_RO __attribute__((section(".rodata.leavekeep_in_text")))
#define LK_BSS __attribute__((section(".bss.leavekeep_late")))
#define LK_WAIT_SECONDS 15

extern void (*__system_retAddr)(void);
Result __real_svcControlMemory(u32* addr_out, u32 addr0, u32 addr1, u32 size, MemOp op, MemPerm perm);
Result __wrap_svcControlMemory(u32* addr_out, u32 addr0, u32 addr1, u32 size, MemOp op, MemPerm perm);
void __real__exit(int status) __attribute__((noreturn));
void CTR_EXIT_ONCE(int status) __attribute__((noreturn));

static u32 lkKeepHeap LK_BSS; // thread id + 1 of the lab leave or filesRecover caller
static u32 lkOwner LK_BSS;    // thread id + 1 of the thread running the exit
static u32 lkEntered LK_BSS;  // threads that reached _exit; above 1, a waiting thread's stack is on the heap
static u32 lkAimed LK_BSS;    // the "died" target set (CtrLab_aimHost(false))
#ifdef CTRPORT_EXITTRAIL
static u32 lkOldExit LK_BSS; // labcrash_after=-SECONDS: the control arm, heap freed as before
#define LK_OLD_EXIT lkOldExit
#else
#define LK_OLD_EXIT 0
#endif

static LK_CODE u32 lkSelf(void) {
    u32 id = 0;
    svcGetThreadId(&id, CUR_THREAD_HANDLE);
    return id + 1;
}

LK_CODE __attribute__((noreturn)) void CtrLeavekeep_exit(int status) {
    CtrLab_aimHost(true);
    if (!LK_OLD_EXIT) __atomic_store_n(&lkKeepHeap, lkSelf(), __ATOMIC_RELEASE);
    _exit(status);
}

LK_CODE void CTR_EXIT_ONCE(int status) {
    u32 self = lkSelf(), owner = 0;
    // Counted before the claim, so the owner sees a second thread before it frees the heap.
    if (__atomic_load_n(&lkOwner, __ATOMIC_ACQUIRE) != self) __atomic_add_fetch(&lkEntered, 1, __ATOMIC_ACQ_REL);
    if (!__atomic_compare_exchange_n(&lkOwner, &owner, self, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && owner != self) {
        svcSleepThread(LK_WAIT_SECONDS * 1000000000LL);
        svcExitProcess();
    }
    __real__exit(status);
}

// The heap stays mapped when a lab leave owns the exit, or when another thread waits in _exit.
#ifdef CTRPORT_EXITTRAIL
static u32 lkFrees[2] LK_BSS; // exit trail builds: MEMOP_FREE calls skipped, passed on
// usstop's provoke test: [keep, owner, entered, retAddr, skipped frees, passed frees]
LK_CODE void CtrLeavekeep_state(u32* out) {
    out[0] = lkKeepHeap;
    out[1] = lkOwner;
    out[2] = lkEntered;
    out[3] = (u32) (uintptr_t) __system_retAddr;
    out[4] = lkFrees[0];
    out[5] = lkFrees[1];
}
#define LK_COUNT_FREE(i) if ((op & MEMOP_OP_MASK) == MEMOP_FREE) lkFrees[i]++
// usstop's provoke test 6: rev 2's mark (542eaccb4), the control arm for c26rev r10 F1.
LK_CODE void CtrLeavekeep_rev2Keep(void) {
    __atomic_store_n(&lkKeepHeap, lkSelf(), __ATOMIC_RELEASE);
}
#else
#define LK_COUNT_FREE(i)
#endif
#ifdef ENABLE_WAD16
// usstop (UNDERTALE; ctr_audio_system.c's end, rev 3): called only from the exit's own aptExit and svcExitProcess.
// On the thread that owns the exit (lkOwner), with want (a sound copy still under way when shutdownAudio's wait ran
// out) or when the exit is already a keep exit (lkKeepHeap = this thread: filesRecover, a lab leave), the exit keeps
// the heap: the owner counts itself into lkEntered, so __wrap_svcControlMemory's "a second thread is in the exit"
// test skips libctru's MEMOP_FREE calls (with no return address, as for every keep). lkEntered only grows, so no
// other thread can take the keep back (rev 2 overwrote lkKeepHeap: c26rev r10 F1). Returns whether the exit keeps
// the heap; on any other thread, false and nothing changes.
// Its section is named outside .text.*: late_reserve.ld takes z_leavekeep.o's .text.* in leavekeep's place.
__attribute__((noinline, section(".usstop_keep_text"))) bool CtrLeavekeep_ownerKeep(bool want) {
    u32 self = lkSelf();
    if (__atomic_load_n(&lkOwner, __ATOMIC_ACQUIRE) != self) return false;
    if (!want && __atomic_load_n(&lkKeepHeap, __ATOMIC_ACQUIRE) != self) return false;
    __atomic_add_fetch(&lkEntered, 1, __ATOMIC_ACQ_REL);
    return true;
}
#endif
LK_CODE Result __wrap_svcControlMemory(u32* addr_out, u32 addr0, u32 addr1, u32 size, MemOp op, MemPerm perm) {
    u32 keep = __atomic_load_n(&lkKeepHeap, __ATOMIC_ACQUIRE);
    if ((op & MEMOP_OP_MASK) == MEMOP_FREE && __system_retAddr == NULL &&
        ((keep != 0 && keep == __atomic_load_n(&lkOwner, __ATOMIC_ACQUIRE)) || __atomic_load_n(&lkEntered, __ATOMIC_ACQUIRE) > 1)) {
        LK_COUNT_FREE(0);
        return 0;
    }
    LK_COUNT_FREE(1);
    return __real_svcControlMemory(addr_out, addr0, addr1, size, op, perm);
}

void __real_CtrLab_poll(void);
LK_CODE void CtrLabPoll_chainLeavekeep(void) {
    if (!lkAimed) {
        lkAimed = 1;
        CtrLab_aimHost(false);
    }
    __real_CtrLab_poll();
}

#ifdef CTRPORT_EXITTRAIL
CtrLaunchArgs* CtrBenchIo_launch(void);
void __real_CtrLab_crashTest(const char* kind);
void __wrap_CtrLab_crashTest(const char* kind);
static const char* lkLateKind LK_BSS;
static u32 lkLateSeconds LK_BSS;
static const char lkKey[] LK_RO = "labcrash_after=";

static LK_CODE void lkLateMain(void* arg) {
    (void) arg;
    svcSleepThread((s64) lkLateSeconds * 1000000000LL);
    __real_CtrLab_crashTest(lkLateKind);
}

LK_CODE void __wrap_CtrLab_crashTest(const char* kind) {
    CtrLaunchArgs* args = CtrBenchIo_launch();
    int seconds = 0;
    for (int i = 0; args != NULL && args->iniCount > i; i++) {
        if (strncmp(args->ini[i], lkKey, sizeof(lkKey) - 1) == 0) seconds = atoi(args->ini[i] + sizeof(lkKey) - 1);
    }
    lkOldExit = seconds < 0;
    if (seconds < 0) seconds = -seconds;
    bool delayable = strcmp(kind, "thread") != 0 && strcmp(kind, "hang") != 0;
    if (seconds == 0 || seconds > 600 || !delayable) {
        __real_CtrLab_crashTest(kind);
        return;
    }
    lkLateKind = kind;
    lkLateSeconds = (u32) seconds;
    if (threadCreate(lkLateMain, NULL, 4096, 0x30, -2, true) == NULL) __real_CtrLab_crashTest(kind);
}
#endif
