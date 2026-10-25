#include "ctr_release_branding.h"

// Lab mode: unattended test runs on the consoles that end cleanly whatever happens, and hand the console back to
// the lab host, which waits for the next .3dsx, so overnight tests need nobody to press a button.
//
// Only the lab host (platform/3ds/labhost/labhost.c, sdmc:/3ds/ctrport-lab.3dsx) turns it on: it starts each .3dsx
// it receives with "--lab-host <its own path>" (ctr_launch_args.h). Started any other way, nothing here runs.
//
// How a run ends in lab mode:
//   - A normal exit (the test finished with --exit-after, --timeout, an error screen timing out, SELECT+START): an
//     atexit handler tells Luma3DS's hb:ldr (ctr_hbldr.h) to load the lab host next, with "--after exit" (or
//     "error"), and libctru's usual exit restarts the launcher title, which loads it.
//   - A CPU exception (data abort, prefetch abort, undefined instruction, VFP; svcBreak, which Luma3DS turns into a
//     prefetch abort; on any thread): Luma3DS passes exceptions to a userland handler when the thread, or failing
//     that the main thread, has one (libctru's threadOnException), instead of showing its crash screen and rebooting.
//     The handler starts on a small entry stack of the thread's own (CtrLab_runThread; the main thread's is static),
//     claims the crash (one thread at a time; any other thread that faults meanwhile just stops), and moves to a
//     static 16 KB stack. There it writes a report: the exception, registers, likely return addresses from the
//     stack, a stack excerpt, the build and thread, and the last log lines. The report goes to crash.txt in the game
//     folder (FS calls straight to the service: no heap, no stdio), to log.txt and to the PC watching the 3dslink log
//     (cfg->emergencyLog, CtrLog_emergency: locks only tried, every wait bounded). Then hb:ldr gets the lab host
//     with "--after crash --report <path>", the main thread is stopped (CtrLab_poll; its priority dropped so it
//     can't starve what follows), and aptExit() makes the jump: APT_PrepareToDoApplicationJump +
//     APT_DoApplicationJump to the same launcher title, as a normal exit does. Audio, GPU and threads are not shut
//     down (they may be what crashed): the process just exits, and the kernel and system services clean up after
//     it. The lab host resets the DSP, which a dead process can leave playing.
//   - abort() (and failed asserts): CtrLab_fail reports it the same way, with the caller in place of registers.
//   - A hang: a guard thread above the main thread's priority checks once a second that cfg->progress (the main
//     loop's watchdog marks) moves while cfg->watching (a game is running, not in the HOME Menu). After hangSeconds
//     without a move (--lab-hang, default 90) it reports (with the watchdog's hang report and the log tail) and
//     leaves as for a crash, with "--after hang". deadlineSeconds (--timeout plus a minute) ends any run that the
//     main loop's own --timeout didn't (stuck while loading, say). bootSeconds ends a start that never gets as far
//     as CtrLab_booted (the log open), saying how far it got (cfg->bootStep). CtrLab_exiting gives the app's own
//     exit a time limit too (NDSP's exit has been seen to hang).
//   - Death without any of that (the kernel ending the process, a fault inside the handler): CtrLab_start already
//     told hb:ldr to load the lab host next, with "--after died", for when the launcher title next starts. The boot
//     trail (cfg->trail, ctr_boot_trail.h) shows the last step the start reached, and how a run that left, left.
//   - If leaving itself gets stuck (aptExit waiting on a thread that is wedged), a fallback thread ends the process
//     25 s after leaving started: the console then goes to the HOME Menu, not the lab host.
//
// What it can't recover from: a GPU hang or a kernel/system-module crash takes the whole console down (Luma3DS's
// own crash screen, or a frozen picture); hold POWER. The guard thread can't help either if the kernel stops
// scheduling it. A crash before CtrLab_start, a nested fault inside the handler, or a corrupted heap that also
// breaks the FS service's IPC buffers end the process without a report. Closing the lid can drop Wi-Fi.
//
// Emulators: Azahar has no Luma3DS, so hb:ldr is missing and the run ends as it would without lab mode. Azahar
// (2126) doesn't pass CPU exceptions to userland handlers either: a bad write is logged and skipped, an undefined
// instruction stops the emulator. What can be tried there is the rest: the hang guard, abort(), the report file, the
// log tail and leaving (ctrport-labprobe.3dsx built with fake="--lab-crash hang", see platform/3ds/labhost/build.sh).
// The exception handler itself can only be tried on a console.
#include "ctr_lab.h"
#include "ctr_exit.h"
#line 52

#include <3ds.h>
#include <stdlib.h>
#include <string.h>

#include "ctr_hbldr.h"
#include "ctr_lab_proto.h"

#define REPORT_SIZE 8192
#define WORK_STACK_SIZE (16 * 1024)
#define MAIN_ENTRY_STACK_SIZE 512
#define THREAD_ENTRY_STACK_SIZE 256
#define GUARD_STACK_SIZE (16 * 1024)
#define FALLBACK_STACK_SIZE 4096
// Above the main thread (0x30), the APT thread (0x31) and the log threads, so a spinning main thread can't hold it off.
#define GUARD_PRIORITY 0x18
#define FALLBACK_SECONDS 25
#define EMERGENCY_LOG_MS 5000
#define PARK_WAIT_MS 300
#define STACK_WORDS 48
#define RETURN_ADDRESSES 16
#define STACK_SCAN_WORDS 512
#define LOG_TAIL_BYTES 2048
// What Luma3DS's hb:ldr loads when nobody said otherwise: the Homebrew Launcher.
#define HBMENU_PATH "/boot.3dsx"

static const CtrLabConfig* cfg;
static volatile bool active;
static char hostPath[LAB_PATH_MAX];       // "/3ds/ctrport-lab.3dsx"
static char hostArg0[LAB_PATH_MAX + 8];   // "sdmc:/3ds/ctrport-lab.3dsx"
static char reportPath[LAB_PATH_MAX];     // "/3ds/undertale-3ds/crash.txt"
static char reportArg[LAB_PATH_MAX + 8];  // "sdmc:..."
static const char* volatile exitNote = "exit";
static volatile u64 exitingAt;
static volatile int exitingSeconds;
static volatile u32 exitProgressCount; // CtrLab_exitProgress: counted here, timed by the guard thread
static volatile bool forceWatching;
static Handle mainThread;
static const void* mainTls;
static volatile bool mainParked;
// The thread (its TLS) that is leaving: set once, by whoever gets there first.
static const void* volatile leaver;
static LightEvent leaveEvent;
static Thread guardThread;
static Thread fallbackThread;

// What the crash left, copied off the entry stack.
static ERRF_ExceptionInfo crashInfo;
static CpuRegisters crashRegs;
static const void* crashTls;
static const char* failKind;
static const char* failDetail;
static u32 failCaller;
static u32 failSp;
// The report's headline, for the boot trail.
static const char* leaveWhat = "?";
// CtrLab_booted was called.
static volatile bool booted;

static char report[REPORT_SIZE];
static char logTail[LOG_TAIL_BYTES];
static u8 mainEntryStack[MAIN_ENTRY_STACK_SIZE] __attribute__((aligned(8)));
static u8 workStack[WORK_STACK_SIZE] __attribute__((aligned(8)));

static u64 nowMs(void) {
    return svcGetSystemTick() / CPU_TICKS_PER_MSEC;
}

// ===[ The report ]===

static void put(const char* text) {
    LabProto_append(report, sizeof(report), text != NULL ? text : "(null)");
}

static void putHex(u32 value) {
    LabProto_appendHex(report, sizeof(report), value, 8);
}

static void putDec(s32 value) {
    LabProto_appendDec(report, sizeof(report), value);
}

static void putHeader(const char* what) {
    report[0] = '\0';
    leaveWhat = what;
    put(ctrReleaseLabHeading);
    put(what);
    put(" ===\n");
    put(cfg->appName != NULL ? cfg->appName : "app");
    put(", build ");
    put(cfg->buildId != NULL ? cfg->buildId : "?");
    put(", ");
    putDec((s32) (nowMs() / 1000));
    put(" s after the console started\n");
}

static void putThread(const void* tls) {
    const char* name = tls == mainTls ? "main" : (cfg->threadName != NULL ? cfg->threadName(tls) : NULL);
    put("thread ");
    put(name != NULL ? name : "(unnamed)");
    put(" (TLS ");
    putHex((u32) tls);
    put("), main loop phase \"");
    put(cfg->phase != NULL ? cfg->phase() : "?");
    put("\"\n");
}

static bool readable(u32 address, u32* end) {
    MemInfo info;
    PageInfo page;
    if (R_FAILED(svcQueryMemory(&info, &page, address))) return false;
    if ((info.perm & MEMPERM_READ) == 0 || info.state == MEMSTATE_FREE) return false;
    *end = info.base_addr + info.size;
    return true;
}

// Words on the stack that point into the code are mostly return addresses: a backtrace of sorts, for
// arm-none-eabi-addr2line -e <the .elf>.
static void putStack(u32 sp) {
    sp &= ~3u;
    u32 stackEnd = 0;
    if (!readable(sp, &stackEnd)) {
        put("stack: sp ");
        putHex(sp);
        put(" is not readable\n");
        return;
    }
    u32 codeEnd = 0;
    u32 codeStart = 0x00100000; // where every .3dsx's code is linked and loaded
    if (!readable(codeStart, &codeEnd)) codeEnd = codeStart;
    put("code ");
    putHex(codeStart);
    put("-");
    putHex(codeEnd);
    put("; likely return addresses on the stack:");
    int found = 0;
    for (u32 i = 0; STACK_SCAN_WORDS > i && stackEnd > sp + 4 * i && RETURN_ADDRESSES > found; i++) {
        u32 word = ((const volatile u32*) sp)[i];
        if (word >= codeStart + 4 && codeEnd > word) {
            put(found % 8 == 0 ? "\n  " : " ");
            putHex(word & ~1u);
            found++;
        }
    }
    put(found == 0 ? " none\n" : "\n");
    put("stack from sp:");
    for (u32 i = 0; STACK_WORDS > i && stackEnd > sp + 4 * i; i++) {
        if (i % 4 == 0) {
            put("\n  ");
            putHex(sp + 4 * i);
            put(":");
        }
        put(" ");
        putHex(((const volatile u32*) sp)[i]);
    }
    put("\n");
}

static void putLogTail(void) {
    if (cfg->logTail == NULL) return;
    size_t n = cfg->logTail(logTail, sizeof(logTail));
    if (n == 0) return;
    put("--- the log before it ---\n");
    put(logTail);
    if (logTail[strlen(logTail) - 1] != '\n') put("\n");
}

static void writeReportFile(void) {
    Handle file = 0;
    if (R_FAILED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, reportPath),
                                         FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
        return;
    }
    FSFILE_SetSize(file, 0);
    u32 written = 0;
    FSFILE_Write(file, &written, 0, report, (u32) strlen(report), FS_WRITE_FLUSH);
    FSFILE_Close(file);
}

// ===[ Leaving ]===

static bool claim(void) {
    const void* expected = NULL;
    return __atomic_compare_exchange_n(&leaver, &expected, getThreadLocalStorage(), false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

// Another thread is leaving (or this one faulted while leaving, which only ending the process can get out of).
static __attribute__((noreturn)) void standAside(void) {
    if (leaver == getThreadLocalStorage()) CtrExitFiles_labExit();
    for (;;) svcSleepThread(1000000000LL);
}

static __attribute__((noreturn)) void leave(const char* why) {
    LightEvent_Signal(&leaveEvent); // the fallback's clock starts
    const char* args[] = { hostArg0, "--after", why, "--report", reportArg };
    const char* problem = CtrHbldr_setNext(hostPath, args, 5);
    if (!booted && cfg->bootStep != NULL) {
        put("during the start, which had got to: ");
        put(cfg->bootStep());
        put("\n");
    }
    put("=== next: ");
    if (problem == NULL) {
        put("the lab host, ");
        put(hostArg0);
    } else {
        put("the Homebrew Launcher (lab host ");
        put(hostArg0);
        put(": ");
        put(problem);
        put(")");
    }
    put(" ===\n");
    writeReportFile();
    if (cfg->trail != NULL) {
        static char line[96];
        line[0] = '\0';
        LabProto_append(line, sizeof(line), "leaving: ");
        LabProto_append(line, sizeof(line), leaveWhat);
        cfg->trail(line);
    }
    if (cfg->emergencyLog != NULL) cfg->emergencyLog(report, EMERGENCY_LOG_MS);
    else svcOutputDebugString(report, (s32) strlen(report));
    // The main thread mustn't use APT while it goes, nor spin above the threads aptExit waits for.
    if (getThreadLocalStorage() != mainTls) {
        if (mainThread != 0) svcSetThreadPriority(mainThread, 0x3F);
        for (int waited = 0; PARK_WAIT_MS > waited && !mainParked; waited += 10) svcSleepThread(10000000LL);
    }
    aptExit();
    CtrExitFiles_labExit();
}

static const char* exceptionName(ERRF_ExceptionType type) {
    switch (type) {
        case ERRF_EXCEPTION_PREFETCH_ABORT: return "prefetch abort";
        case ERRF_EXCEPTION_DATA_ABORT: return "data abort";
        case ERRF_EXCEPTION_UNDEFINED: return "undefined instruction";
        case ERRF_EXCEPTION_VFP: return "VFP exception";
        default: return "exception";
    }
}

static __attribute__((noreturn)) void crashMain(void) {
    char what[64] = "crash: ";
    LabProto_append(what, sizeof(what), exceptionName(crashInfo.type));
    putHeader(what);
    putThread(crashTls);
    put("pc ");
    putHex(crashRegs.pc);
    put(" lr ");
    putHex(crashRegs.lr);
    if (crashInfo.type == ERRF_EXCEPTION_DATA_ABORT || crashInfo.type == ERRF_EXCEPTION_PREFETCH_ABORT) {
        put(crashInfo.type == ERRF_EXCEPTION_DATA_ABORT ? ", data address " : ", instruction address ");
        putHex(crashInfo.far);
        put(" (status ");
        putHex(crashInfo.fsr);
        put(")");
    } else if (crashInfo.type == ERRF_EXCEPTION_VFP) {
        put(", fpexc ");
        putHex(crashInfo.fpexc);
        put(" fpinst ");
        putHex(crashInfo.fpinst);
        put(" fpinst2 ");
        putHex(crashInfo.fpinst2);
    }
    put("\n");
    for (int i = 0; 13 > i; i++) {
        put("r");
        putDec(i);
        put(i < 10 ? "  " : " ");
        putHex(crashRegs.r[i]);
        put(i % 4 == 3 ? "\n" : "  ");
    }
    put("sp  ");
    putHex(crashRegs.sp);
    put("  cpsr ");
    putHex(crashRegs.cpsr);
    put("\n");
    putStack(crashRegs.sp);
    putLogTail();
    leave("crash");
}

static __attribute__((noreturn)) void failMain(void) {
    char what[96] = "failure: ";
    LabProto_append(what, sizeof(what), failKind);
    putHeader(what);
    put(failDetail != NULL ? failDetail : "");
    put("\n");
    putThread(crashTls);
    put("called from ");
    putHex(failCaller);
    put("\n");
    putStack(failSp);
    putLogTail();
    leave("crash");
}

// Moves to the work stack and runs fn there, for good.
static __attribute__((naked, noreturn, target("arm"))) void switchStack(__attribute__((unused)) void* top,
                                                                        __attribute__((unused)) void (*fn)(void)) {
    __asm__ volatile("mov sp, r0\n\tbx r1\n\t");
}

// Every thread's exception handler. It runs on the thread's small entry stack, where the kernel put the exception
// data: claim the crash, copy that data, and move to the work stack.
static __attribute__((noreturn, target("arm"))) void entryHandler(ERRF_ExceptionInfo* info, CpuRegisters* regs) {
    if (!claim()) standAside();
    crashInfo = *info;
    crashRegs = *regs;
    crashTls = getThreadLocalStorage();
    switchStack(workStack + sizeof(workStack), crashMain);
}

void CtrLab_fail(const char* kind, const char* detail) {
    if (!active) abort();
    if (!claim()) standAside();
    failKind = kind;
    failDetail = detail;
    failCaller = (u32) __builtin_return_address(0);
    failSp = (u32) __builtin_frame_address(0);
    crashTls = getThreadLocalStorage();
    switchStack(workStack + sizeof(workStack), failMain);
}

static void hangLeave(const char* what, int seconds) {
    if (!claim()) return;
    putHeader(what);
    put("no progress for ");
    putDec(seconds);
    put(" s; main loop phase \"");
    put(cfg->phase != NULL ? cfg->phase() : "?");
    put("\"\n");
    const char* details = cfg->hangDetails != NULL ? cfg->hangDetails() : NULL;
    if (details != NULL && details[0] != '\0') {
        put("--- the watchdog's last report ---\n");
        put(details);
    }
    putLogTail();
    leave("hang");
}

// ===[ Threads ]===

static void guardMain(void* arg) {
    (void) arg;
    u32 last = cfg->progress != NULL ? cfg->progress() : 0;
    int still = 0;
    int seconds = 0;
    u32 exitMoved = 0; // exitProgressCount when last seen, and when it moved
    u64 exitMovedAt = 0;
    for (;;) {
        CtrLab_guardSleep(1000000000LL);
        seconds++;
        if (leaver != NULL) continue;
        u64 exiting = exitingAt;
        if (exiting != 0) {
            u32 moved = exitProgressCount;
            if (moved != exitMoved) {
                exitMoved = moved;
                exitMovedAt = nowMs();
            }
            u64 since = exitMovedAt > exiting ? exitMovedAt : exiting;
            if (nowMs() - since > (u64) exitingSeconds * 1000u) hangLeave("hang: the app's exit got stuck", exitingSeconds);
            continue;
        }
        bool watching = forceWatching || (cfg->progress != NULL && (cfg->watching == NULL || cfg->watching()));
        u32 progress = cfg->progress != NULL ? cfg->progress() : 0;
        if (!watching || progress != last) {
            last = progress;
            still = 0;
        } else {
            still++;
        }
        if (cfg->hangSeconds > 0 && still >= cfg->hangSeconds) hangLeave("hang: the main loop stopped", still);
        if (cfg->deadlineSeconds > 0 && seconds >= cfg->deadlineSeconds) hangLeave("hang: the run's deadline passed", seconds);
        if (!booted && cfg->bootSeconds > 0 && seconds >= cfg->bootSeconds) hangLeave("hang: the start stalled", seconds);
    }
}

static void fallbackMain(void* arg) {
    (void) arg;
    LightEvent_Wait(&leaveEvent);
    svcSleepThread(FALLBACK_SECONDS * 1000000000LL);
    CtrExitFiles_labExit();
}

static void onExit(void) {
    if (!active || leaver != NULL) return;
    // Closed from the HOME Menu (or the power button): the console goes there, and hb:ldr goes back to the Homebrew
    // Launcher (CtrLab_start pointed it at the lab host), so the launcher title starts that next time as usual.
    if (aptShouldClose()) {
        const char* args[] = { "sdmc:" HBMENU_PATH };
        CtrHbldr_setNext(HBMENU_PATH, args, 1);
        return;
    }
    const char* args[] = { hostArg0, "--after", exitNote };
    CtrHbldr_setNext(hostPath, args, 3);
    if (cfg->trail != NULL) {
        char line[64] = "leaving: exit (--after ";
        LabProto_append(line, sizeof(line), exitNote);
        LabProto_append(line, sizeof(line), ")");
        cfg->trail(line);
    }
}

// ===[ Interface ]===

bool CtrLab_start(const CtrLabConfig* config) {
    if (active || config == NULL) return false;
    cfg = config;
    if (config->hostPath == NULL || !LabProto_sdPath(config->hostPath, hostPath, sizeof(hostPath))) {
        LabProto_sdPath(LAB_HOST_DEFAULT, hostPath, sizeof(hostPath));
    }
    hostArg0[0] = '\0';
    LabProto_append(hostArg0, sizeof(hostArg0), "sdmc:");
    LabProto_append(hostArg0, sizeof(hostArg0), hostPath);
    const char* report = config->reportPath != NULL ? config->reportPath : "sdmc:/3ds/crash.txt";
    if (strncmp(report, "sdmc:", 5) == 0) report += 5;
    reportPath[0] = '\0';
    LabProto_append(reportPath, sizeof(reportPath), report);
    reportArg[0] = '\0';
    LabProto_append(reportArg, sizeof(reportArg), "sdmc:");
    LabProto_append(reportArg, sizeof(reportArg), reportPath);
    // A report from an earlier run would mislead; it is kept as "<report>.prev", so the next run can't wipe a report
    // nobody has read yet.
    FS_Archive sdmc;
    if (R_SUCCEEDED(FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        char prevPath[LAB_PATH_MAX + 8] = "";
        LabProto_append(prevPath, sizeof(prevPath), reportPath);
        LabProto_append(prevPath, sizeof(prevPath), ".prev");
        Handle old = 0;
        if (R_SUCCEEDED(FSUSER_OpenFile(&old, sdmc, fsMakePath(PATH_ASCII, reportPath), FS_OPEN_READ, 0))) {
            FSFILE_Close(old);
            FSUSER_DeleteFile(sdmc, fsMakePath(PATH_ASCII, prevPath));
            FSUSER_RenameFile(sdmc, fsMakePath(PATH_ASCII, reportPath), sdmc, fsMakePath(PATH_ASCII, prevPath));
            FSUSER_DeleteFile(sdmc, fsMakePath(PATH_ASCII, reportPath));
        }
        FSUSER_CloseArchive(sdmc);
    }
    // Should the process die without leaving, the launcher title still goes to the lab host next. Leaving and a
    // normal exit set it again, saying how the run ended.
    const char* diedArgs[] = { hostArg0, "--after", "died" };
    CtrHbldr_setNext(hostPath, diedArgs, 3);

    mainTls = getThreadLocalStorage();
    u32 id = 0;
    if (R_FAILED(svcGetThreadId(&id, CUR_THREAD_HANDLE)) || R_FAILED(svcOpenThread(&mainThread, CUR_PROCESS_HANDLE, id))) mainThread = 0;
    LightEvent_Init(&leaveEvent, RESET_STICKY);
    active = true;
    // The main thread's handler is also the one for threads without their own.
    threadOnException(entryHandler, mainEntryStack + sizeof(mainEntryStack), WRITE_DATA_TO_HANDLER_STACK);
    fallbackThread = threadCreate(fallbackMain, NULL, FALLBACK_STACK_SIZE, GUARD_PRIORITY, -2, true);
    guardThread = threadCreate(guardMain, NULL, GUARD_STACK_SIZE, GUARD_PRIORITY, -2, true);
    atexit(onExit);
    return true;
}

void CtrLab_booted(void) {
    booted = true;
}

bool CtrLab_active(void) {
    return active;
}

bool CtrLab_hostReady(void) {
    return active && CtrHbldr_fileLoadable(hostPath);
}

void CtrLab_runThread(void (*entry)(void*), void* arg) {
    if (!active) {
        entry(arg);
        return;
    }
    u8 area[THREAD_ENTRY_STACK_SIZE] __attribute__((aligned(8)));
    threadOnException(entryHandler, area + sizeof(area), WRITE_DATA_TO_HANDLER_STACK);
    entry(arg);
    // The area goes with this frame; the main thread's handler covers the rest of the thread's life.
    threadOnException(NULL, NULL, NULL);
    __asm__ volatile("" : : "r"(area) : "memory");
}

void CtrLab_setExitNote(const char* note) {
    exitNote = note;
}

void CtrLab_exiting(int seconds) {
    if (!active || exitingAt != 0) return;
    exitingSeconds = seconds;
    exitingAt = nowMs();
}

void CtrLab_exitProgress(void) {
    if (active) exitProgressCount++;
}

void CtrLab_poll(void) {
    if (leaver == NULL || leaver == getThreadLocalStorage()) return;
    mainParked = true;
    for (;;) svcSleepThread(1000000000LL);
}

// Nothing is mapped below 0x00100000.
static volatile u32* volatile badAddress = (volatile u32*) 8;

static void crashThreadMain(void* arg) {
    (void) arg;
    svcSleepThread(100000000LL);
    *badAddress = 0xDEAD;
}

void CtrLab_crashTest(const char* kind) {
    if (strcmp(kind, "data") == 0) {
        *badAddress = 0xDEAD;
    } else if (strcmp(kind, "prefetch") == 0) {
        void (*volatile nowhere)(void) = (void (*)(void)) 0x10;
        nowhere();
    } else if (strcmp(kind, "undef") == 0) {
        __builtin_trap();
    } else if (strcmp(kind, "thread") == 0) {
        threadCreate(crashThreadMain, NULL, 8192, 0x30, -2, true);
        for (;;) {
            CtrLab_poll();
            svcSleepThread(10000000LL);
        }
    } else if (strcmp(kind, "break") == 0) {
        svcBreak(USERBREAK_PANIC);
    } else if (strcmp(kind, "abort") == 0) {
        abort();
    } else if (strcmp(kind, "hang") == 0) {
        // A main thread spinning at its own priority: the guard has to get past it.
        forceWatching = true;
        for (;;) __asm__ volatile("" : : : "memory");
    }
}

// B10: the old literal is padding; the new heading lives in late rodata.
static const char* const relprepLabLegacy
    __attribute__((used, section(".rodata.late.relprep"))) = "\n=== ctrport lab: ";

// o2crash (2026-10-22): the guard's one-second sleep, and what it does on waking when the process is already being
// torn down. libctru's __libctru_exit unmaps the heap (where threadCreate put this thread's stack and
// CtrLab_runThread's handler area) before svcExitProcess. A guard that wakes in between faults on its first stack
// write, and Luma3DS, finding the thread's handler stack unmapped, shows its crash screen instead (o2DS dump
// 00000002: guardMain, data abort writing at sp 0x08012058, after a clean "Exit: done"). So after the sleep the
// guard asks the kernel about its own stack page, in registers only, and parks for good in the kernel if the page
// is gone; process exit ends it there. It touches no stack and keeps r4-r11 (r4/r5 via CtrLab_guardSave).
// docs/3ds-hardware.md, "A thread that wakes after libctru unmaps the heap crashes the console".
__attribute__((used, section(".bss.late.o2crash"))) u32 CtrLab_guardSave[2];
__asm__(
    "    .section .text.late.o2crash,\"ax\",%progbits\n"
    "    .arm\n"
    "    .align 2\n"
    "    .global CtrLab_guardSleep\n"
    "    .type CtrLab_guardSleep, %function\n"
    "CtrLab_guardSleep:\n"
    "    svc 0x0A\n"                  /* SleepThread(r0:r1 ns) */
    "    ldr r12, =CtrLab_guardSave\n"
    "    stm r12, {r4, r5}\n"
    "    mov r2, sp\n"
    "    svc 0x02\n"                  /* QueryMemory(r2): r0 result, r3 permission, r4 state, r5 page flags */
    "    ldr r12, =CtrLab_guardSave\n"
    "    ldm r12, {r4, r5}\n"
    "    cmp r0, #0\n"
    "    bxne lr\n"                   /* the query failed: carry on as before */
    "    and r3, r3, #3\n"
    "    cmp r3, #3\n"                /* MEMPERM_READ | MEMPERM_WRITE: the stack is still there */
    "    bxeq lr\n"
    "1:  mvn r0, #0\n"
    "    mvn r1, #0x80000000\n"       /* r0:r1 = INT64_MAX ns */
    "    svc 0x0A\n"
    "    b 1b\n"
    "    .ltorg\n"
    "    .size CtrLab_guardSleep, . - CtrLab_guardSleep\n"
    "    .previous\n");
