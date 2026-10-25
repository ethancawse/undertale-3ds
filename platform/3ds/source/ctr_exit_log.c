#include "ctr_exit.h"
#include "ctr_exit_log_state.h"
#include "ctr_log.h"

#include <string.h>

#define EXIT_TEXT __attribute__((noipa, section(".text.late.exithang")))
#define state CtrExit_logState

static EXIT_TEXT u64 exitLogNow(void) {
    return svcGetSystemTick() / CPU_TICKS_PER_MSEC;
}

static EXIT_TEXT void exitRingPut(const void* src, u32 length) {
    ByteRing* ring = state.ring;
    const char* from = src;
    while (length > 0) {
        u32 at = ring->head % ring->size;
        u32 run = ring->size - at;
        if (run > length) run = length;
        memcpy(ring->data + at, from, run);
        from += run;
        length -= run;
        ring->head += run;
    }
}

// Returning true means accepted-or-dropped, suppressing synchronous fallback
// even if the writer failed to start or stopped with a held lock.
EXIT_TEXT bool CtrExit_postRecord(u16 kind, const void* payload, u32 size, bool forConsole, bool droppable) {
    if (!__atomic_load_n(&CtrExit_started, __ATOMIC_ACQUIRE)) return state.post(kind, payload, size, forConsole, droppable);
    if (!*state.running || LightLock_TryLock(state.lock) != 0) return true;
    struct { u16 kind, size; } header = {kind, (u16) size};
    bool queued = *state.running && state.ring->size - (state.ring->head - state.ring->tail) >= sizeof(header) + size;
    if (queued) {
        exitRingPut(&header, sizeof(header));
        exitRingPut(payload, size);
        if (forConsole) __atomic_add_fetch(state.consoleQueued, 1, __ATOMIC_SEQ_CST);
    }
    LightLock_Unlock(state.lock);
    if (queued) LightEvent_Signal(state.event);
    return true;
}

EXIT_TEXT void CtrExit_flush(void) {
    if (!__atomic_load_n(&CtrExit_started, __ATOMIC_ACQUIRE)) {
        CtrLog_flush();
        return;
    }
    // One wall-clock budget includes queue-lock acquisition, queue space and
    // acknowledgement. No synchronous fallback, including without a writer.
    u64 deadline = exitLogNow() + 100u;
    u32 number = 0;
    bool posted = false;
    while (*state.running && exitLogNow() < deadline) {
        if (LightLock_TryLock(state.lock) == 0) {
            number = ++*state.asked;
            LightLock_Unlock(state.lock);
            posted = true;
            break;
        }
        svcSleepThread(1000000LL);
    }
    if (!posted || !state.postBefore(2 /* RECORD_FLUSH */, &number, sizeof(number), deadline)) return;
    while (*state.running && (s32) (*state.done - number) < 0 && exitLogNow() < deadline) svcSleepThread(1000000LL);
}

EXIT_TEXT void CtrExit_closeLog(void) {
    if (!__atomic_load_n(&CtrExit_started, __ATOMIC_ACQUIRE)) {
        CtrLog_closeFile();
        return;
    }
    CtrExit_flush();
    // Never wait for a peer, call socExit, or free a live sender's memory.
    *state.linkStop = true;
    *state.stop = true;
    if (*state.thread != NULL) {
        LightEvent_Signal(state.event);
        if (threadJoin(*state.thread, 250000000ULL) != 0) CtrExitFiles_closeFailed();
    }
    // The independent deadline also bounds stuck FS and C library/APT exit.
    if (*state.file != NULL) fclose(*state.file);
    *state.file = NULL;
}
