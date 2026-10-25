// One texture worker owns this publisher; callers serialize it with sdWriteLock.
// A flushed, closed temporary file is the only input publish accepts. Readers
// never open temporary files. The completion marker is removed before any page
// is rebuilt and is written again only after all pages have succeeded.
#ifndef CTRPORT_CTR_CACHE_PUBLISH_H
#define CTRPORT_CTR_CACHE_PUBLISH_H

#include <stdbool.h>

typedef struct {
    float deleteMs;
    float renameMs;
    bool direct;
} CtrCachePublishStats;

bool CtrCachePublish_begin(const char* marker, bool direct);
bool CtrCachePublish_run(const char* temporary, const char* destination, CtrCachePublishStats* stats);
void CtrCachePublish_close(void);

#endif
