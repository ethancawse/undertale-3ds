// labguard (change labguard, 2026-11-08, for the lab defects found in change
// dimlight). UNDERTALE lab builds only: CMakeLists.txt compiles this file, and sets CTRPORT_LABGUARD for
// main.c and ctr_lab.c's renames, only for UNDERTALE builds without CTRPORT_RELEASE_SOURCE_URL. Public builds compile none of it
// and keep every byte. Late code only (late_reserve.ld: this object's catch-alls); existing code changes only call
// targets (CMakeLists.txt's renames for ctr_lab.c, main.c's lab config) and gains link wraps.
//
// 1. The startup guard. ctr_lab.c's hang check watches progress only while cfg->watching() is true, and the watchdog's
//    CtrWatchdog_watching is false until main.c hands it the runner (CtrWatchdog_watch, after Runner_create), while
//    CtrLab_booted ends the boot guard when the log opens. So a start that stalled between the log opening and the
//    runner (data.win, textures, the renderer, audio: both dimlight B consoles froze there, 11-08) went unreported
//    until the run's deadline, or never. main.c's lab config now names CtrLabguard_watching: true from the
//    watchdog's start (the log open, CtrWatchdog_start's wrap) until the runner is first watched, so the usual hang
//    check (hangSeconds without a CtrWatchdog_mark, --lab-hang, default 90 s) covers that stretch too. The longest
//    start in 908 console logs took 8.9 s; first-run texture preparation marks progress every pass. Once the runner
//    has been watched, the watchdog's answer passes through unchanged, so the guard is as before. The report's phase
//    (CtrLabguard_phase) then says where the start was: the watchdog's phase, the last boot trail step and how long
//    ago it was marked, and how long ago the log opened. HOME Menu and sleep pause it as they pause the watchdog
//    (c26rev2 LABGUARD-1): its own APT hook, registered at the log open in lab mode, keeps the same suspended flag
//    the watchdog's hook keeps (ctr_watchdog.c's onAptEvent), so a suspension before the runner, or with the runner
//    made but not yet seen watching, answers false, and ctr_lab.c's hang check restarts its count, as it does for
//    the watchdog's own false.
//
// 2. The bounded leave. ctr_lab.c's leave runs its steps in order, each with a stamp here: the hb:ldr target (FS and
//    hb:ldr IPC), the report file (FS), the boot trail (FS), the report to the PC (SOC; argcrash's crash screens),
//    APT's exit (the jump back to the loader), then the exit (saves, SOC close, libctru's exit). None had a time limit,
//    and the old fallback (25 s after the leave started) ran the same exit again, so one stalled owner stopped both:
//    the XL's dimlight B report (11-08) went out, then its loader never came back. Now fallbackMain's thread is a
//    monitor. A step past its bound (lgBoundMs) hands the rest of the leave to a recovery thread made at the lab's
//    start (the taker): the stalled thread stops for good if its call ever returns (the baton, lgRunner), and the taker
//    goes on from the next step, skipping the file steps after an FS stall and never repeating the stalled step. A
//    second stall, or a stalled exit, ends the process with the kernel's own exit (svcExitProcess), but only once SOC
//    is closed or was never opened (libctru's socExit zeroes SOCU_handle after its shutdown IPC): docs/3ds-hardware.md,
//    "Raw process exit during a moving transfer is unsafe under HBL; Azahar hides the loader failure", rules that the
//    process must not terminate while SOC remains live (the HOME Menu's "An error has occurred"). With SOC still open
//    the monitor parks for good instead (ruling B, 11-08), after a stamp on the boot trail and in the report
//    file that names the stalled step and says to power-cycle the console. Either way the loader may not come back;
//    the PC says so (tools/remote/labqueue.py, loader-lost). The report file's results (open, size, write, close) go to
//    the report sent to the PC and to the boot trail; they were dropped before.
//
//    Outside lab mode (z_argcrash.c's default guard for players) every wrapper here passes straight through and the
//    fallback thread does what it did (25 s after the leave event, the exit).
//
// --labguard-crash=<kind> (UNDERTALE lab builds without the exit trail, whose z_exittrail.c wraps CtrLab_crashTest;
// needs --lab) is filtered out ahead of CtrLaunchArgs_parse, whose --lab-crash list stays as it is, and becomes the
// run's --lab-crash kind (main.c logs it and calls CtrLab_crashTest after the log opened). queue_lint.py accepts the
// option because the build holds its name. "startstall" stops the main thread for good after the log opened, before the runner exists, with nothing else
// changed (the startup guard has to see it); "stall-hbldr", "stall-report", "stall-trail", "stall-emergency",
// "stall-apt" and "stall-exit" do the same and then stop the leave's thread inside that step, as a stalled owner would.
#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "ctr_boot_trail.h"
#include "ctr_hbldr.h"
#include "ctr_lab.h"
#include "ctr_lab_proto.h"
#include "ctr_wrap_chain.h" // CTR_HBLDR_AFTER_ARGCRASH
#ifdef LG_CRASHTEST
#include <stdarg.h>
#include <stdio.h>
#include "ctr_launch_args.h"
#endif

bool CtrWatchdog_watching(void);
const char* CtrWatchdog_phase(void);
void CtrLog_emergency(const char* text, int waitMs);
void __real_CtrLog_emergency(const char* text, int waitMs);
void CtrSaveguard_exit(void) __attribute__((noreturn));
void CtrSaveguard_labAppend(char* buffer, size_t size, const char* text);
void CtrArgcrash_labSleep(s64 ns);
void __real_CtrWatchdog_start(const char* reportPath);
void __real_CtrBootTrail_mark(const char* step);
bool __real_CtrLab_start(const CtrLabConfig* config);
// libctru's SOC session (soc_common.o): 0 before socInit and after socExit's shutdown. Weak, so this object never
// pulls soc_common.o out of libctru itself: a strong reference changed which member the link takes first and moved
// libctru's socket and syscall code (check_layout). Every lab build links SOC already (the lab's PC link).
extern Handle SOCU_handle __attribute__((weak));

// The report writer ctr_lab.c called before: datarefuse's (CMakeLists.txt passes it) or libctru's.
#ifndef LG_FS_WRITE_NEXT
#define LG_FS_WRITE_NEXT FSFILE_Write
#endif
Result LG_FS_WRITE_NEXT(Handle file, u32* written, u64 offset, const void* buffer, u32 size, u32 flags);

#ifndef LG_RAW_EXIT
#define LG_RAW_EXIT()                                       \
    do {                                                    \
        __asm__ volatile("svc 0x03" : : : "memory");        \
        __builtin_unreachable();                            \
    } while (0)
#endif

// Recovery threads: the monitor is ctr_lab.c's fallback thread (0x18, as the guard); the taker runs below it, so a
// taker that spins can't hold the monitor off. Both above the main thread (0x30).
#define LG_TAKER_PRIORITY 0x19
#define LG_TAKER_STACK (8 * 1024)
#define LG_POLL_NS 100000000LL
#define LG_EMERGENCY_MS 5000
#define LG_PARK_WAIT_NS 300000000LL
#define LG_OLD_FALLBACK_NS (25 * 1000000000LL)

enum { LG_NONE, LG_HBLDR, LG_REPORT, LG_TRAIL, LG_EMERGENCY, LG_APT, LG_EXIT, LG_IDLE, LG_STEPS };

static const char* const lgStepName[LG_STEPS] = {
    "nothing", "the hb:ldr target (FS, hb:ldr)", "the report file (FS)", "the boot trail (FS)",
    "the report to the PC (SOC, crash screens)", "APT's exit (the jump back to the loader)",
    "the exit (saves, SOC close, libctru's exit)", "the code between two steps",
};
// Each step's bound (ms): its usual time is milliseconds (the report to the PC: up to EMERGENCY_LOG_MS, 5 s; the exit:
// the save guard's 2 s, bounded joins and the files cancel's 5 s).
static const u32 lgBoundMs[LG_STEPS] = { 0, 5000, 5000, 3000, 8000, 10000, 30000, 20000 };
#ifdef LG_CRASHTEST // build.sh's labcrash (test only)
static const char* const lgInjectName[LG_STEPS] = {
    "startstall", "stall-hbldr", "stall-report", "stall-trail", "stall-emergency", "stall-apt", "stall-exit", NULL,
};
#endif

// ===[ State ]===
static volatile bool lgLab;            // main.c's lab config started the lab (CtrLab_start's wrap)
static volatile u64 lgBootAt;          // the watchdog's start (the log is open), ms
static volatile u64 lgTrailAt;         // the last boot trail mark, ms
static volatile bool lgRunnerSeen;     // the watchdog has watched the runner
static volatile bool lgSuspended;      // HOME Menu or sleep, as the watchdog's own flag (lgOnApt)
static aptHookCookie lgAptCookie;
static char lgPhase[224];

static const void* volatile lgRunner;  // the thread doing the leave's steps: the leaver, then the taker
static volatile u32 lgStep;            // the step under way (LG_IDLE between steps)
static volatile u64 lgStepAt;          // when it started, ms
static u32 lgStepMs[LG_STEPS];         // how long each finished step took, ms
static volatile u32 lgStalls;          // stalls seen by the monitor (1: the taker took over; 2: the end)
static u32 lgStalled[2];
static u32 lgStalledMs[2];
static volatile bool lgFsBad;          // an FS step stalled: no more FS
static volatile bool lgAptDone;
#ifdef LG_CRASHTEST
static volatile u32 lgInject;          // --lab-crash stall-<step>
#endif
static const void* volatile lgTakerTls;
static Thread lgTaker;
static LightEvent lgTakerEvent;
static volatile u32 lgResume;

static const char* lgHbldrProblem;
static bool lgHbldrDone;
static Result lgOpenRc, lgSizeRc, lgWriteRc, lgCloseRc;
static u32 lgWritten, lgWanted;
static u32 lgFsDid; // LG_DID_*
#define LG_DID_OPEN 1u
#define LG_DID_SIZE 2u
#define LG_DID_WRITE 4u
#define LG_DID_CLOSE 8u
static const char* lgReportPath;       // ctr_lab.c's reportPath (static), from the report file's open
static char* lgReport;                 // ctr_lab.c's report buffer, from its appends
static size_t lgReportSize;
static char lgNote[160];               // the report file's result, one line
static char lgStamp[400];              // the parked end's stamp

static u64 lgNow(void) {
    return svcGetSystemTick() / CPU_TICKS_PER_MSEC;
}

static bool lgMine(void) {
    return lgLab && lgRunner != NULL && lgRunner == getThreadLocalStorage();
}

static __attribute__((noreturn)) void lgParkForGood(void) {
    for (;;) CtrArgcrash_labSleep(1000000000LL);
}

// A thread whose step the taker took over must not go on with the leave: it stops here, if its call ever returns.
static void lgCheckBaton(void) {
    const void* runner = lgRunner;
    if (lgLab && runner != NULL && runner != getThreadLocalStorage()) lgParkForGood();
}

static void lgBegin(u32 step) {
    __atomic_store_n(&lgStepAt, lgNow(), __ATOMIC_RELEASE);
    __atomic_store_n(&lgStep, step, __ATOMIC_RELEASE);
}

static void lgEnd(u32 step) {
    u64 now = lgNow();
    lgStepMs[step] = (u32) (now - __atomic_load_n(&lgStepAt, __ATOMIC_ACQUIRE));
    __atomic_store_n(&lgStepAt, now, __ATOMIC_RELEASE);
    __atomic_store_n(&lgStep, LG_IDLE, __ATOMIC_RELEASE);
}

// --lab-crash stall-<step>: the leave's own thread stops inside that step, as if its owner never answered. Only in
// build.sh's labcrash builds (LG_CRASHTEST); otherwise nothing.
static void lgMaybeInject(u32 step) {
#ifdef LG_CRASHTEST
    if (lgInject == step && getThreadLocalStorage() != lgTakerTls) lgParkForGood();
#else
    (void) step;
#endif
}

static void lgAppend(char* out, size_t size, const char* text) {
    LabProto_append(out, size, text != NULL ? text : "(null)");
}

static void lgHex(char* out, size_t size, u32 value) {
    lgAppend(out, size, "0x");
    LabProto_appendHex(out, size, value, 8);
}

static void lgMs(char* out, size_t size, u32 ms) {
    LabProto_appendDec(out, size, (int32_t) ms);
    lgAppend(out, size, " ms");
}

static bool lgReportFailed(void) {
    if ((lgFsDid & LG_DID_OPEN) == 0) return false;
    if (R_FAILED(lgOpenRc)) return true;
    if ((lgFsDid & LG_DID_SIZE) && R_FAILED(lgSizeRc)) return true;
    if ((lgFsDid & LG_DID_WRITE) && (R_FAILED(lgWriteRc) || lgWritten != lgWanted)) return true;
    return (lgFsDid & LG_DID_CLOSE) && R_FAILED(lgCloseRc);
}

// The report file's result, one line (lgNote).
static void lgComposeNote(void) {
    lgNote[0] = '\0';
    lgAppend(lgNote, sizeof(lgNote), "report file: ");
    if ((lgFsDid & LG_DID_OPEN) == 0) {
        lgAppend(lgNote, sizeof(lgNote), "not written");
    } else if (R_FAILED(lgOpenRc)) {
        lgAppend(lgNote, sizeof(lgNote), "open failed ");
        lgHex(lgNote, sizeof(lgNote), (u32) lgOpenRc);
    } else {
        if ((lgFsDid & LG_DID_SIZE) && R_FAILED(lgSizeRc)) {
            lgAppend(lgNote, sizeof(lgNote), "set size failed ");
            lgHex(lgNote, sizeof(lgNote), (u32) lgSizeRc);
            lgAppend(lgNote, sizeof(lgNote), ", ");
        }
        if ((lgFsDid & LG_DID_WRITE) == 0) {
            lgAppend(lgNote, sizeof(lgNote), "no write");
        } else {
            if (R_FAILED(lgWriteRc)) {
                lgAppend(lgNote, sizeof(lgNote), "write failed ");
                lgHex(lgNote, sizeof(lgNote), (u32) lgWriteRc);
                lgAppend(lgNote, sizeof(lgNote), ", ");
            }
            lgAppend(lgNote, sizeof(lgNote), "wrote ");
            LabProto_appendDec(lgNote, sizeof(lgNote), (int32_t) lgWritten);
            lgAppend(lgNote, sizeof(lgNote), " of ");
            LabProto_appendDec(lgNote, sizeof(lgNote), (int32_t) lgWanted);
            lgAppend(lgNote, sizeof(lgNote), " B");
        }
        if ((lgFsDid & LG_DID_CLOSE) && R_FAILED(lgCloseRc)) {
            lgAppend(lgNote, sizeof(lgNote), ", close failed ");
            lgHex(lgNote, sizeof(lgNote), (u32) lgCloseRc);
        }
    }
    if (lgReportFailed()) lgAppend(lgNote, sizeof(lgNote), " (FAILED)");
}

static void lgPutStalls(char* out, size_t size) {
    u32 stalls = __atomic_load_n(&lgStalls, __ATOMIC_ACQUIRE);
    for (u32 i = 0; stalls > i && 2 > i; i++) {
        lgAppend(out, size, i == 0 ? "stalled: " : ", then ");
        lgAppend(out, size, lgStepName[lgStalled[i]]);
        lgAppend(out, size, " for ");
        lgMs(out, size, lgStalledMs[i]);
    }
}

// The recovery section at the report's end (the report sent to the PC; the file was written before it).
static void lgPutNotes(void) {
    char* out = lgReport;
    size_t size = lgReportSize;
    if (out == NULL) return;
    lgAppend(out, size, "--- recovery (labguard) ---\nhb:ldr target: ");
    if (!lgHbldrDone) {
        lgAppend(out, size, "not answered");
    } else {
        lgAppend(out, size, lgHbldrProblem == NULL ? "the lab host" : lgHbldrProblem);
        lgAppend(out, size, ", ");
        lgMs(out, size, lgStepMs[LG_HBLDR]);
    }
    lgAppend(out, size, "\n");
    lgComposeNote();
    lgAppend(out, size, lgNote);
    if (lgFsDid & LG_DID_OPEN) {
        lgAppend(out, size, ", ");
        lgMs(out, size, lgStepMs[LG_REPORT]);
    }
    lgAppend(out, size, "\n");
    if (__atomic_load_n(&lgStalls, __ATOMIC_ACQUIRE) > 0) {
        lgPutStalls(out, size);
        lgAppend(out, size, "; the recovery thread goes on from the next step");
        if (lgFsBad) lgAppend(out, size, " without FS");
        lgAppend(out, size, "\n");
    }
}

// ===[ 1. The startup guard ]===

// ctr_watchdog.c's onAptEvent, for the same hooks: suspended from ONSUSPEND/ONSLEEP until ONRESTORE/ONWAKEUP.
static void lgOnApt(APT_HookType hook, void* param) {
    (void) param;
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) {
        __atomic_store_n(&lgSuspended, true, __ATOMIC_RELEASE);
    } else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) {
        __atomic_store_n(&lgSuspended, false, __ATOMIC_RELEASE);
    }
}

bool CtrLabguard_watching(void) {
    if (CtrWatchdog_watching()) {
        lgRunnerSeen = true;
        return true;
    }
    // The watchdog's false means no runner yet, or suspended. Before the runner is first seen watching, the
    // startup guard answers instead, except while suspended (the runner may exist by then: the watchdog's false).
    if (lgRunnerSeen || __atomic_load_n(&lgBootAt, __ATOMIC_ACQUIRE) == 0) return false;
    return !__atomic_load_n(&lgSuspended, __ATOMIC_ACQUIRE);
}

const char* CtrLabguard_phase(void) {
    const char* phase = CtrWatchdog_phase();
    u64 boot = __atomic_load_n(&lgBootAt, __ATOMIC_ACQUIRE);
    if (lgRunnerSeen || boot == 0) return phase;
    u64 now = lgNow();
    u64 trail = __atomic_load_n(&lgTrailAt, __ATOMIC_ACQUIRE);
    const char* last = CtrBootTrail_last();
    lgPhase[0] = '\0';
    lgAppend(lgPhase, sizeof(lgPhase), "startup, before the runner: watchdog '");
    lgAppend(lgPhase, sizeof(lgPhase), phase);
    lgAppend(lgPhase, sizeof(lgPhase), "', last boot step '");
    lgAppend(lgPhase, sizeof(lgPhase), last != NULL ? last : "none");
    lgAppend(lgPhase, sizeof(lgPhase), "'");
    if (trail != 0) {
        lgAppend(lgPhase, sizeof(lgPhase), " ");
        LabProto_appendDec(lgPhase, sizeof(lgPhase), (int32_t) ((now - trail) / 1000u));
        lgAppend(lgPhase, sizeof(lgPhase), " s ago");
    }
    lgAppend(lgPhase, sizeof(lgPhase), ", log open ");
    LabProto_appendDec(lgPhase, sizeof(lgPhase), (int32_t) ((now - boot) / 1000u));
    lgAppend(lgPhase, sizeof(lgPhase), " s ago");
    return lgPhase;
}

void __wrap_CtrWatchdog_start(const char* reportPath) {
    __real_CtrWatchdog_start(reportPath);
    if (__atomic_load_n(&lgBootAt, __ATOMIC_ACQUIRE) != 0) return;
    if (lgLab) aptHook(&lgAptCookie, lgOnApt, NULL); // before the startup guard counts (main.c starts the lab first)
    __atomic_store_n(&lgBootAt, lgNow() | 1u, __ATOMIC_RELEASE);
}

void __wrap_CtrBootTrail_mark(const char* step) {
    __atomic_store_n(&lgTrailAt, lgNow() | 1u, __ATOMIC_RELEASE);
    __real_CtrBootTrail_mark(step);
}

bool __wrap_CtrLab_start(const CtrLabConfig* config) {
    // Before the start: its fallback thread asks at once.
    bool mine = !lgLab && config != NULL && config->trail == CtrLabguard_trail;
    if (mine) lgLab = true;
    bool started = __real_CtrLab_start(config);
    if (mine && !started) lgLab = false;
    return started;
}

#ifdef LG_CRASHTEST
// CtrLaunchArgs_parse's chain (ctr_wrap_chain.h): argcrash -> labguard -> gogut (UNDERTALE GOG builds) -> the parser.
#ifdef CTRPORT_GOG_COMPAT
bool CtrLaunchArgs_chainGogut(CtrLaunchArgs* args, int argc, char** argv);
#define LG_ARGS_NEXT CtrLaunchArgs_chainGogut
#else
bool __real_CtrLaunchArgs_parse(CtrLaunchArgs* args, int argc, char** argv);
#define LG_ARGS_NEXT __real_CtrLaunchArgs_parse
#endif
static const char lgCrashFlag[] __attribute__((used)) = "--labguard-crash"; // whole, so queue_lint.py finds the name in the build

static __attribute__((format(printf, 2, 3))) void lgArgsProblem(CtrLaunchArgs* args, const char* format, ...) {
    args->problems++; // as ctr_launch_args.c's problem(): "; " between messages
    size_t used = strlen(args->messages);
    if (used + 3 >= sizeof(args->messages)) return;
    if (used > 0) {
        memcpy(args->messages + used, "; ", 3);
        used += 2;
    }
    va_list va;
    va_start(va, format);
    vsnprintf(args->messages + used, sizeof(args->messages) - used, format, va);
    va_end(va);
}

bool CtrLaunchArgs_chainLabguard(CtrLaunchArgs* args, int argc, char** argv) {
    if (argc < 1 || argc > 64) return LG_ARGS_NEXT(args, argc, argv);
    char* kept[64];
    int count = 1;
    const char* kind = NULL;
    size_t flagLen = sizeof(lgCrashFlag) - 1;
    kept[0] = argv[0];
    for (int i = 1; argc > i; i++) {
        if (argv[i] != NULL && strncmp(argv[i], lgCrashFlag, flagLen) == 0 && argv[i][flagLen] == '=') {
            kind = argv[i] + flagLen + 1;
        } else {
            kept[count++] = argv[i];
        }
    }
    bool result = LG_ARGS_NEXT(args, count, kept);
    if (kind == NULL) return result;
    u32 step = 0;
    while (LG_IDLE > step && strcmp(kind, lgInjectName[step]) != 0) step++;
    if (step == LG_IDLE) {
        lgArgsProblem(args, "--labguard-crash wants startstall or stall-hbldr, -report, -trail, -emergency, -apt or "
                      "-exit, not '%s'", kind);
    } else if (!args->lab) {
        lgArgsProblem(args, "--labguard-crash does nothing without --lab");
    } else if (args->labCrash[0] != '\0') {
        lgArgsProblem(args, "--labguard-crash and --lab-crash: one at a time, kept --lab-crash %s", args->labCrash);
    } else {
        snprintf(args->labCrash, sizeof(args->labCrash), "%s", kind);
    }
    return true;
}

void __real_CtrLab_crashTest(const char* kind);
void __wrap_CtrLab_crashTest(const char* kind) {
    for (u32 step = 0; LG_IDLE > step; step++) {
        if (strcmp(kind, lgInjectName[step]) != 0) continue;
        lgInject = step; // 0 (startstall): no step
        // After the log opened, before the runner: only the startup guard can see this (no forceWatching).
        for (;;) svcSleepThread(1000000000LL);
    }
    __real_CtrLab_crashTest(kind);
}
#endif

// ===[ 2. The leave's steps (ctr_lab.c's calls, renamed) ]===

void CtrLabguard_append(char* buffer, size_t size, const char* text) {
    lgCheckBaton();
    if (lgLab && size >= 4096) { // ctr_lab.c's report (REPORT_SIZE); its other buffers are under 200 B
        lgReport = buffer;
        lgReportSize = size;
    }
    CtrSaveguard_labAppend(buffer, size, text);
}

void CtrLabguard_leaveSignal(LightEvent* event) {
    if (lgLab && lgRunner == NULL) {
        lgRunner = getThreadLocalStorage();
        lgBegin(LG_HBLDR);
    }
    LightEvent_Signal(event);
}

// Every hb:ldr target ctr_lab.c sets (its start's "died", a leave, an exit). argcrash's wrap of CtrHbldr_setNext answers
// "normal play" until its CtrMemory_markGameThread wrap reads CtrLab_active() (main.c, after the start), so in every
// UNDERTALE lab build a leave or exit before that went to the plain Homebrew Launcher instead of the lab host (o2ds,
// 11-08 10:44Z: "next: the Homebrew Launcher (... normal play)", stranded without a netloader). Lab mode is known here
// from CtrLab_start (lgLab: main.c's lab config), so in lab mode the call goes to argcrash's next link, as argcrash's
// own wrap does once it knows. Outside lab mode (argcrash's default guard for players) it goes through argcrash's wrap.
const char* CtrLabguard_setNext(const char* path, const char* const* args, int count) {
    lgCheckBaton();
    bool mine = lgMine() && __atomic_load_n(&lgStep, __ATOMIC_ACQUIRE) == LG_HBLDR;
    if (mine) lgMaybeInject(LG_HBLDR);
    const char* problem = lgLab ? CTR_HBLDR_AFTER_ARGCRASH(path, args, count) : CtrHbldr_setNext(path, args, count);
    lgCheckBaton();
    if (mine) {
        lgHbldrProblem = problem;
        lgHbldrDone = true;
        lgEnd(LG_HBLDR);
    }
    return problem;
}

Result CtrLabguard_fsOpen(Handle* out, FS_ArchiveID archive, FS_Path archivePath, FS_Path filePath, u32 flags, u32 attributes) {
    lgCheckBaton();
    bool mine = lgMine();
    if (mine) {
        lgBegin(LG_REPORT);
        lgReportPath = filePath.type == PATH_ASCII ? (const char*) filePath.data : NULL;
        lgMaybeInject(LG_REPORT);
    }
    Result rc = FSUSER_OpenFileDirectly(out, archive, archivePath, filePath, flags, attributes);
    lgCheckBaton();
    if (mine) {
        lgOpenRc = rc;
        lgFsDid |= LG_DID_OPEN;
        if (R_FAILED(rc)) lgEnd(LG_REPORT);
    }
    return rc;
}

Result CtrLabguard_fsSetSize(Handle file, u64 size) {
    lgCheckBaton();
    Result rc = FSFILE_SetSize(file, size);
    lgCheckBaton();
    if (lgMine()) {
        lgSizeRc = rc;
        lgFsDid |= LG_DID_SIZE;
    }
    return rc;
}

Result CtrLabguard_fsWrite(Handle file, u32* written, u64 offset, const void* buffer, u32 size, u32 flags) {
    lgCheckBaton();
    Result rc = LG_FS_WRITE_NEXT(file, written, offset, buffer, size, flags);
    lgCheckBaton();
    if (lgMine()) {
        lgWriteRc = rc;
        lgWritten = written != NULL ? *written : 0;
        lgWanted = size;
        lgFsDid |= LG_DID_WRITE;
    }
    return rc;
}

Result CtrLabguard_fsClose(Handle file) {
    lgCheckBaton();
    Result rc = FSFILE_Close(file);
    lgCheckBaton();
    if (lgMine() && (lgFsDid & LG_DID_OPEN)) {
        lgCloseRc = rc;
        lgFsDid |= LG_DID_CLOSE;
        lgEnd(LG_REPORT);
    }
    return rc;
}

void CtrLabguard_trail(const char* text) {
    lgCheckBaton();
    if (!lgMine()) {
        CtrBootTrail_mark(text);
        return;
    }
    lgBegin(LG_TRAIL);
    lgMaybeInject(LG_TRAIL);
    CtrBootTrail_mark(text);
    if (lgReportFailed()) {
        lgComposeNote();
        CtrBootTrail_mark(lgNote);
    }
    lgCheckBaton();
    lgEnd(LG_TRAIL);
}

void CtrLabguard_emergencyLog(const char* text, int waitMs) {
    lgCheckBaton();
    bool mine = lgMine();
    if (mine) {
        lgBegin(LG_EMERGENCY);
        if (text == lgReport) lgPutNotes();
        lgMaybeInject(LG_EMERGENCY);
    }
    CtrLog_emergency(text, waitMs);
    lgCheckBaton();
    if (mine) lgEnd(LG_EMERGENCY);
}

void CtrLabguard_aptExit(void) {
    lgCheckBaton();
    bool mine = lgMine();
    if (mine) {
        lgBegin(LG_APT);
        lgMaybeInject(LG_APT);
    }
    aptExit();
    lgCheckBaton();
    if (mine) {
        lgAptDone = true;
        lgEnd(LG_APT);
    }
}

void CtrLabguard_labExit(void) {
    lgCheckBaton();
    if (lgMine()) {
        lgBegin(LG_EXIT);
        lgMaybeInject(LG_EXIT);
    }
    CtrSaveguard_exit();
}

// ===[ Recovery: the taker and the monitor ]===

// The rest of the leave, from lgResume, on its own thread: the report to the PC (without the crash screens' painting,
// which argcrash's emergency wrap adds: the LCD may be what stalled), APT's exit, then the exit.
static void lgTakerMain(void* arg) {
    (void) arg;
    __atomic_store_n(&lgTakerTls, getThreadLocalStorage(), __ATOMIC_RELEASE);
    LightEvent_Wait(&lgTakerEvent);
    u32 from = __atomic_load_n(&lgResume, __ATOMIC_ACQUIRE);
    if (LG_EMERGENCY >= from && lgReport != NULL) {
        lgBegin(LG_EMERGENCY);
        lgPutNotes();
        __real_CtrLog_emergency(lgReport, LG_EMERGENCY_MS);
        lgCheckBaton();
        lgEnd(LG_EMERGENCY);
    }
    if (LG_APT >= from) {
        svcSleepThread(LG_PARK_WAIT_NS); // the main thread parks at its next CtrLab_poll (a leaver is set)
        CtrLabguard_aptExit();
    }
    CtrLabguard_labExit();
}

// The parked end's stamp: what stalled, and what a person has to do.
static void lgComposeStamp(void) {
    lgStamp[0] = '\0';
    lgAppend(lgStamp, sizeof(lgStamp), "=== labguard: the lab's leave is stuck. ");
    lgPutStalls(lgStamp, sizeof(lgStamp));
    lgAppend(lgStamp, sizeof(lgStamp), lgAptDone ? ". APT's exit had finished. " : ". APT's exit had not finished. ");
    lgAppend(lgStamp, sizeof(lgStamp),
             "SOC is still open, so the process is not ended (docs/3ds-hardware.md: no raw exit while SOC is live) and "
             "the console will not go back to the lab host. Power-cycle it: hold POWER until it turns off. ===\n");
}

static __attribute__((noreturn)) void lgFinal(void) {
    // Ruling B (11-08): the kernel's own exit only once SOC is closed or was never opened.
    // docs/3ds-hardware.md, "Raw process exit during a moving transfer is unsafe under HBL; Azahar hides the loader
    // failure": the process must not terminate while SOC remains live.
    if (&SOCU_handle == NULL || __atomic_load_n(&SOCU_handle, __ATOMIC_ACQUIRE) == 0) LG_RAW_EXIT();
    lgComposeStamp();
    if (!lgFsBad) {
        // FS answered every step so far; if it doesn't now, this thread parks here instead, as it would below.
        static const char trail[] = "labguard: leave stuck, SOC open; power-cycle (hold POWER)";
        CtrBootTrail_mark(trail);
        Handle file = 0;
        if (lgReportPath != NULL && (lgFsDid & LG_DID_OPEN) && R_SUCCEEDED(lgOpenRc) &&
            R_SUCCEEDED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                                                fsMakePath(PATH_ASCII, lgReportPath), FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
            // At the file's end (datarefuse's line may come before the report), straight to FS: no second line.
            u64 end = 0;
            u32 written = 0;
            if (R_SUCCEEDED(FSFILE_GetSize(file, &end)))
                FSFILE_Write(file, &written, end, lgStamp, (u32) strlen(lgStamp), FS_WRITE_FLUSH);
            FSFILE_Close(file);
        }
    }
    lgParkForGood();
}

static void lgStall(u32 step, u64 ms) {
    u32 n = __atomic_load_n(&lgStalls, __ATOMIC_ACQUIRE);
    if (n < 2) {
        lgStalled[n] = step;
        lgStalledMs[n] = (u32) ms;
    }
    __atomic_store_n(&lgStalls, n + 1, __ATOMIC_RELEASE);
    if (step == LG_HBLDR || step == LG_REPORT || step == LG_TRAIL) lgFsBad = true;
    const void* taker = __atomic_load_n(&lgTakerTls, __ATOMIC_ACQUIRE);
    if (n != 0 || step == LG_EXIT || taker == NULL) lgFinal();
    // The next step after the stalled one (never the stalled one again); between steps (no step under way: something
    // ctr_lab.c calls without a stamp), APT's exit, or the exit if APT's exit is done.
    u32 resume = step <= LG_TRAIL ? LG_EMERGENCY : step == LG_EMERGENCY ? LG_APT : step == LG_APT ? LG_EXIT :
                 lgAptDone ? LG_EXIT : LG_APT;
    __atomic_store_n(&lgResume, resume, __ATOMIC_RELEASE);
    __atomic_store_n(&lgStepAt, lgNow(), __ATOMIC_RELEASE);
    __atomic_store_n(&lgStep, LG_IDLE, __ATOMIC_RELEASE);
    __atomic_store_n(&lgRunner, taker, __ATOMIC_RELEASE);
    LightEvent_Signal(&lgTakerEvent);
}

// fallbackMain's wait (ctr_lab.c). Lab mode: the taker is made now, while the heap is sound, and this thread becomes
// the steps' monitor once the leave starts; it never returns. Otherwise: the wait as before, and fallbackMain goes on
// as before.
void CtrLabguard_fallback(LightEvent* event) {
    if (!lgLab) {
        LightEvent_Wait(event);
        return;
    }
    LightEvent_Init(&lgTakerEvent, RESET_STICKY);
    lgTaker = threadCreate(lgTakerMain, NULL, LG_TAKER_STACK, LG_TAKER_PRIORITY, -2, true);
    LightEvent_Wait(event);
    u64 base = lgNow();
    for (;;) {
        svcSleepThread(LG_POLL_NS);
        u32 step = __atomic_load_n(&lgStep, __ATOMIC_ACQUIRE);
        u64 at = __atomic_load_n(&lgStepAt, __ATOMIC_ACQUIRE);
        if (step == LG_NONE || step >= LG_STEPS) continue;
        u64 since = at > base ? at : base;
        u64 now = lgNow();
        if (now <= since || now - since <= lgBoundMs[step]) continue;
        lgStall(step, now - since);
    }
}

#ifdef ENABLE_WAD16
#line 675
// homesave: before the lab guard has seen the runner, its startup fallback uses
// lgSuspended rather than the watchdog's flag. Pause both before the long drain.
// lgOnApt already restores this flag on wake/restore; Close keeps it paused.
// Outside the lab, no lgOnApt clears it; the startup fallback is inactive there.
extern volatile bool homesaveLabSuspended __asm__("lgSuspended");
void CtrHomesave_pauseLab(void);
#if !defined(__clang__)
__attribute__((noclone))
#endif
__attribute__((noinline, section(".homesave_code")))
void CtrHomesave_pauseLab(void) {
    __atomic_store_n(&homesaveLabSuspended, true, __ATOMIC_RELEASE);
}
#endif
