#ifndef CTRPORT_CTR_EXIT_FILES_H
#define CTRPORT_CTR_EXIT_FILES_H

#include <3ds.h>

// A separate table keeps the original logger's BSS anchors and late slots.
typedef struct {
    Thread* thread;
    void (*report)(void* ctx, bool warning, const char* line);
    bool* socUp;
} CtrExitFilesLinkState;

extern const CtrExitFilesLinkState CtrExit_filesLinkState;

#endif
