#include "ctr_watchdog.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ctr_frame_pacer.h"
#include "ctr_gpu_queue.h"
#include "ctr_log.h"
#include "data_win.h"
#include "vm.h"

#define HANG_SECONDS 12
#define REPEAT_SECONDS 30
#define SAMPLES 4
#define MAX_FRAMES_PRINTED 12
#define MAX_THREADS 16
#define WATCHDOG_STACK_SIZE (16 * 1024)
// Above the main thread (0x30) so it runs even while the main thread spins.
#define WATCHDOG_PRIORITY 0x18

extern u32 __ctru_heap;
extern u32 __ctru_heap_size;

static Thread watchdogThread = nullptr;
static volatile bool stopRequested = false;
static volatile uint32_t markCount = 0;
static const char* volatile currentPhase = "startup";
// Set while the HOME Menu or sleep mode has the main thread parked in
// aptMainLoop(), which isn't a hang.
static volatile bool suspended = false;
static aptHookCookie aptCookie;
static Runner* volatile watchedRunner = nullptr;

// The report is written with FS calls into a static buffer: a main thread
// stuck while holding the heap lock or a stdio lock (a corrupted heap, say)
// would block fopen and fprintf, which allocate, and the report would never
// appear. fsReportPath is the report's path inside the SD card archive.
static FS_Archive sdmcArchive;
static bool sdmcOpen = false;
static char fsReportPath[256];
static char report[8192];
static size_t reportLength;

// Threads that named themselves, and what each last noted.
typedef struct {
    const char* name;
    const void* tls;
    const char* volatile state;
    volatile uint32_t notes;
    uint32_t lastNotes;   // watchdog-owned
    int32_t stillSeconds; // watchdog-owned: seconds without a new note
} ThreadSlot;
static ThreadSlot threadSlots[MAX_THREADS];
static volatile int32_t threadSlotCount = 0;
static __thread int32_t mySlot = -1;

// The newlib heap lock's holder, from the __malloc_lock wrappers below: the
// thread's TLS pointer and how long it has held the lock.
static const void* volatile mallocOwner = nullptr;
static int32_t mallocDepth = 0; // only touched while holding the lock
static volatile uint32_t mallocAcquisitions = 0;

struct _reent;
void __real___malloc_lock(struct _reent* r);
void __real___malloc_unlock(struct _reent* r);
void __wrap___malloc_lock(struct _reent* r);
void __wrap___malloc_unlock(struct _reent* r);

void __wrap___malloc_lock(struct _reent* r) {
    __real___malloc_lock(r);
    if (mallocDepth++ == 0) {
        mallocOwner = getThreadLocalStorage();
        mallocAcquisitions++;
    }
}

void __wrap___malloc_unlock(struct _reent* r) {
    if (--mallocDepth == 0) mallocOwner = nullptr;
    __real___malloc_unlock(r);
}

static void put(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void put(const char* format, ...) {
    if (reportLength >= sizeof(report) - 1) return;
    va_list va;
    va_start(va, format);
    int n = vsnprintf(report + reportLength, sizeof(report) - reportLength, format, va);
    va_end(va);
    if (n > 0) reportLength += (size_t) n;
    if (reportLength > sizeof(report) - 1) reportLength = sizeof(report) - 1;
}

static void appendReportToFile(void) {
    if (!sdmcOpen || reportLength == 0) return;
    Handle file;
    if (R_FAILED(FSUSER_OpenFile(&file, sdmcArchive, fsMakePath(PATH_ASCII, fsReportPath), FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) return;
    u64 size = 0;
    FSFILE_GetSize(file, &size);
    u32 written = 0;
    FSFILE_Write(file, &written, size, report, (u32) reportLength, FS_WRITE_FLUSH);
    FSFILE_Close(file);
}

// The main thread may free or reuse anything while we look, so only print
// strings that still point into the malloc heap.
static const char* safeName(const char* s) {
    uintptr_t p = (uintptr_t) s;
    if (s == nullptr || __ctru_heap > p || p >= __ctru_heap + __ctru_heap_size) return "?";
    return s;
}

static const char* threadName(const void* tls) {
    int32_t count = threadSlotCount;
    for (int32_t i = 0; count > i; i++) {
        if (threadSlots[i].tls == tls) return threadSlots[i].name;
    }
    return "an unnamed thread";
}

static void putThreads(void) {
    int32_t count = threadSlotCount;
    for (int32_t i = 0; count > i; i++) {
        ThreadSlot* t = &threadSlots[i];
        const char* name = t->name;
        const char* state = t->state;
        if (name == nullptr || state == nullptr) continue; // still being named
        put("thread %s: %s (unchanged for %ld s)\n", name, state, (long) t->stillSeconds);
    }
    const void* owner = mallocOwner;
    if (owner != nullptr) put("heap lock held by %s\n", threadName(owner));
}

static void writeReport(int32_t stuckSeconds) {
    reportLength = 0;
    Runner* runner = watchedRunner;
    put("=== main loop stuck for %ld s in phase \"%s\" ===\n", (long) stuckSeconds, currentPhase);
    putThreads();
    // Twice, a vblank and more apart, to show whether the vblank callbacks still run.
    for (int32_t i = 0; 2 > i; i++) {
        char line[512];
        CtrGpu_describeStall(line, sizeof(line));
        put("%s", line);
        CtrPacer_describeStall(line, sizeof(line));
        put("%s", line);
        if (i == 0) svcSleepThread(50 * 1000 * 1000LL);
    }
    static char events[4096];
    CtrGpu_describeEvents(events, sizeof(events));
    put("last gate events (queue entries/sent/done):\n%s", events);
    if (runner != nullptr) {
        Room* room = runner->currentRoom;
        put("frame %d, room %.64s\n", runner->frameCount, room != nullptr ? safeName(room->name) : "?");
        VMContext* vm = runner->vmContext;
        for (int32_t sample = 0; vm != nullptr && SAMPLES > sample; sample++) {
            const char* objectName = "?";
            Instance* inst = vm->currentInstance;
            DataWin* dw = vm->dataWin;
            uintptr_t ip = (uintptr_t) inst;
            if (inst != nullptr && ip >= __ctru_heap && __ctru_heap + __ctru_heap_size > ip && dw != nullptr) {
                int32_t objectIndex = inst->objectIndex;
                if (objectIndex >= 0 && (int32_t) dw->objt.count > objectIndex) objectName = safeName(dw->objt.objects[objectIndex].name);
            }
            put("sample %ld: code %.96s ip 0x%lX depth %ld event %ld/%ld object %.64s\n", (long) sample, safeName(vm->currentCodeName),
                (unsigned long) vm->ip, (long) vm->callDepth, (long) vm->currentEventType, (long) vm->currentEventSubtype, objectName);
            CallFrame* frame = vm->callStack;
            for (int32_t i = 0; frame != nullptr && MAX_FRAMES_PRINTED > i; i++) {
                uintptr_t fp = (uintptr_t) frame;
                if (__ctru_heap > fp || fp >= __ctru_heap + __ctru_heap_size) break;
                put("    called from %.96s ip 0x%lX\n", safeName(frame->savedCodeName), (unsigned long) frame->savedIP);
                frame = frame->parent;
            }
            svcSleepThread(100 * 1000 * 1000LL);
        }
    }
    appendReportToFile();
    // Last, as it blocks if the main thread is stuck inside a log write.
    CtrLog_flush();
}

static void watchdogMain(void* arg) {
    (void) arg;
    uint32_t lastMark = markCount;
    int32_t stuckSeconds = 0;
    int32_t nextReportAt = HANG_SECONDS;
    while (!stopRequested) {
        svcSleepThread(1000 * 1000 * 1000LL);
        int32_t count = threadSlotCount;
        for (int32_t i = 0; count > i; i++) {
            ThreadSlot* t = &threadSlots[i];
            uint32_t notes = t->notes;
            if (notes != t->lastNotes) {
                t->lastNotes = notes;
                t->stillSeconds = 0;
            } else {
                t->stillSeconds++;
            }
        }
        uint32_t mark = markCount;
        if (mark != lastMark || watchedRunner == nullptr || suspended) {
            lastMark = mark;
            stuckSeconds = 0;
            nextReportAt = HANG_SECONDS;
            continue;
        }
        stuckSeconds++;
        if (stuckSeconds >= nextReportAt) {
            writeReport(stuckSeconds);
            nextReportAt = stuckSeconds + REPEAT_SECONDS;
        }
    }
}

static void onAptEvent(APT_HookType hook, void* param) {
    (void) param;
    switch (hook) {
        case APTHOOK_ONSUSPEND:
        case APTHOOK_ONSLEEP:
            suspended = true;
            logInfo("App %s\n", hook == APTHOOK_ONSLEEP ? "going to sleep" : "suspended (HOME Menu)");
            CtrLog_flush();
            break;
        case APTHOOK_ONRESTORE:
        case APTHOOK_ONWAKEUP:
            markCount++;
            suspended = false;
            logInfo("App %s\n", hook == APTHOOK_ONWAKEUP ? "woke up" : "resumed");
            break;
        case APTHOOK_ONEXIT:
            logInfo("App closing\n");
            CtrLog_flush();
            break;
        default:
            break;
    }
}

void CtrWatchdog_start(const char* reportPath) {
    if (watchdogThread != nullptr) return;
    aptHook(&aptCookie, onAptEvent, nullptr);
    const char* path = strncmp(reportPath, "sdmc:", 5) == 0 ? reportPath + 5 : reportPath;
    snprintf(fsReportPath, sizeof(fsReportPath), "%s", path);
    sdmcOpen = R_SUCCEEDED(FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")));
    if (sdmcOpen) FSUSER_DeleteFile(sdmcArchive, fsMakePath(PATH_ASCII, fsReportPath));
    CtrWatchdog_nameThread("main");
    CtrWatchdog_note("starting");
    stopRequested = false;
    watchdogThread = threadCreate(watchdogMain, nullptr, WATCHDOG_STACK_SIZE, WATCHDOG_PRIORITY, -2, false);
}

void CtrWatchdog_stop(void) {
    if (watchdogThread == nullptr) return;
    stopRequested = true;
    threadJoin(watchdogThread, U64_MAX);
    threadFree(watchdogThread);
    watchdogThread = nullptr;
    aptUnhook(&aptCookie);
    if (sdmcOpen) FSUSER_CloseArchive(sdmcArchive);
    sdmcOpen = false;
}

void CtrWatchdog_watch(Runner* runner) {
    watchedRunner = runner;
    markCount++;
}

void CtrWatchdog_mark(const char* phase) {
    currentPhase = phase;
    markCount++;
    CtrWatchdog_note(phase);
}

void CtrWatchdog_nameThread(const char* name) {
    // A thread started again (each game's audio threads, say) takes over the
    // slot of the one before it with the same name.
    int32_t slot = -1;
    int32_t count = threadSlotCount;
    for (int32_t i = 0; count > i && slot < 0; i++) {
        if (threadSlots[i].name != nullptr && strcmp(threadSlots[i].name, name) == 0) slot = i;
    }
    if (slot < 0) {
        slot = count;
        for (;;) {
            if (slot >= MAX_THREADS) return;
            if (__atomic_compare_exchange_n(&threadSlotCount, &slot, slot + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) break;
        }
    }
    ThreadSlot* t = &threadSlots[slot];
    t->state = "named";
    t->tls = getThreadLocalStorage();
    t->name = name;
    t->notes++;
    mySlot = slot;
}
uint32_t CtrWatchdog_progress(void) {
    return markCount;
}

bool CtrWatchdog_watching(void) {
    return watchedRunner != nullptr && !suspended;
}

const char* CtrWatchdog_phase(void) {
    return currentPhase;
}

const char* CtrWatchdog_threadNameOf(const void* tls) {
    int32_t count = threadSlotCount;
    for (int32_t i = 0; count > i; i++) {
        if (threadSlots[i].tls == tls) return threadSlots[i].name;
    }
    return nullptr;
}

const char* CtrWatchdog_lastReport(void) {
    return reportLength > 0 ? report : nullptr;
}

void CtrWatchdog_note(const char* state) {
    int32_t slot = mySlot;
    if (slot < 0) return;
    threadSlots[slot].state = state;
    threadSlots[slot].notes++;
}
