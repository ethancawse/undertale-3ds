#pragma once

#include <stdbool.h>
#include <stddef.h>

// Only for unnamed application-surface shots. Scan at capture time, so boot
// and chapter changes cannot reuse a number. Explicit benchmark paths bypass it.
bool CtrScreenshot_nextPath(const char* directory, char* path, size_t capacity);
