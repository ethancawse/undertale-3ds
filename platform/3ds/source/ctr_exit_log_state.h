#ifndef CTRPORT_CTR_EXIT_LOG_STATE_H
#define CTRPORT_CTR_EXIT_LOG_STATE_H

#include <3ds.h>
#include <stdio.h>

// ctr_log.c exports only this late pointer table; its existing private objects
// keep their linkage, layout and compiler anchor grouping. Order matches asm.
typedef struct {
    char* data;
    u32 size;
    u32 head, tail;
} ByteRing;

typedef struct {
    ByteRing* ring;
    LightLock* lock;
    LightEvent* event;
    Thread* thread;
    volatile bool* stop;
    volatile bool* running;
    u32* asked;
    volatile u32* done;
    volatile u32* consoleQueued;
    volatile bool* linkStop;
    FILE** file;
    bool (*post)(u16 kind, const void* payload, u32 size, bool forConsole, bool droppable);
    bool (*postBefore)(u16 kind, const void* payload, u32 size, u64 deadline);
} CtrExitLogState;

extern const CtrExitLogState CtrExit_logState;
bool CtrExit_postRecord(u16 kind, const void* payload, u32 size, bool forConsole, bool droppable);

#endif
