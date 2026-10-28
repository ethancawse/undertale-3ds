#pragma once

#include <stdbool.h>

typedef enum CtrDataVersionResult {
    CTR_DATA_KNOWN,
    CTR_DATA_UNKNOWN,
    CTR_DATA_UNREADABLE
} CtrDataVersionResult;

// Boot/chapter-load only. Receipt identity: revision, full path, size, stat time and bound archive time.
// A trusted builder receipt is not an independent per-boot content proof (see builder-version-receipts.md).
// Cache failure never prevents checking or running. progress keeps the watchdog alive.
CtrDataVersionResult CtrDataVersion_check(const char* path, const char* game, const char* part,
                                        const char* cachePath, void (*progress)(void), bool* cached, char digest[65]);
