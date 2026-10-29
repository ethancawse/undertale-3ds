#include "ctr_screenshot_path.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>

// The scan prefix lives in late constants (.rodata.snapname_late, late_reserve.ld, last): "shot_" stays merged with
// ctr_link_files.c's copy, and a new "snap_" literal in the string pool would move every later .rodata address by 8 B.
#if defined(__3DS__)
__attribute__((section(".rodata.snapname_late")))
#endif
static const char snapPrefix[] = "snap_";

bool CtrScreenshot_nextPath(const char* directory, char* path, size_t capacity) {
    DIR* dir = opendir(directory);
    if (dir == NULL) return false;
    unsigned long next = 1;
    bool ok = true;
    struct dirent* entry;
    while (true) {
        errno = 0;
        entry = readdir(dir);
        if (entry == NULL) { if (errno != 0) ok = false; break; }
        if (strncasecmp(entry->d_name, snapPrefix, 5) != 0) continue;
        const char* digits = entry->d_name + 5;
        if (*digits < '0' || *digits > '9') continue;
        char* end;
        unsigned long number = strtoul(digits, &end, 10);
        if (strcasecmp(end, ".bmp") != 0) continue;
        if (errno == ERANGE || number >= UINT32_MAX) { ok = false; break; }
        if (number >= next) next = number + 1;
    }
    if (closedir(dir) != 0) ok = false;
    if (!ok) return false;
    int length = snprintf(path, capacity, "%s/snap_%05lu.bmp", directory, next);
    if (length < 0 || (size_t) length >= capacity) return false;
    struct stat st;
    // A directory or any other existing entry also reserves its number.
    return stat(path, &st) != 0 && errno == ENOENT;
}
