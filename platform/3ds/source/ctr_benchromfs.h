#pragma once

#include <3ds.h>

// Game-thread users share one cached mount attempt for the lifetime of the app; neither owns an unmount.
Result CtrRomfs_init(const char* name);
// Caller owns the NUL-terminated text. Failure is logged and returns NULL (no embedded fallback).
char* CtrBench_readRomfs(void);
