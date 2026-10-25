#include "ctr_startup_clock.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

static uint64_t origin;
static uint64_t libctruUs;

uint64_t CtrStartup_elapsedUs(void) {
    uint64_t ticks = svcGetSystemTick() - origin;
    // Avoid overflowing after a long play session.
    return ticks / SYSCLOCK_ARM11 * 1000000u + ticks % SYSCLOCK_ARM11 * 1000000u / SYSCLOCK_ARM11;
}

uint64_t CtrStartup_libctruUs(void) { return libctruUs; }

void __real___libctru_init(uint32_t returnAddress);
void __wrap___libctru_init(uint32_t returnAddress) {
    // crt0 has cleared BSS and supplied an initial stack. Preserve r0: libctru
    // uses this return address to distinguish HBL from an installed application.
    origin = svcGetSystemTick();
    __real___libctru_init(returnAddress);
    libctruUs = CtrStartup_elapsedUs();
}

void CtrStartup_stamp(char* text, size_t capacity, uint64_t elapsedUs) {
    char stamp[64];
    int size = snprintf(stamp, sizeof(stamp), " [app +%llu.%03llu ms]",
                        (unsigned long long) (elapsedUs / 1000u), (unsigned long long) (elapsedUs % 1000u));
    if (size <= 0 || (size_t) size >= sizeof(stamp) || capacity <= (size_t) size) return;
    size_t marker = (size_t) size;
    size_t length = strnlen(text, capacity - 1);
    text[length] = '\0';
    // Choose the longest prefix that leaves room to stamp all its lines.
    size_t keep = 0, lines = 0;
    while (keep < length) {
        size_t nextLines = lines + (text[keep] == '\n');
        size_t needed = keep + 1 + marker * (nextLines + (text[keep] != '\n'));
        if (needed >= capacity) break;
        lines = nextLines;
        keep++;
    }
    size_t end = keep + marker * (lines + (keep == 0 || text[keep - 1] != '\n'));
    text[end] = '\0';
    if (keep == 0 || text[keep - 1] != '\n') {
        end -= marker;
        memcpy(text + end, stamp, marker);
    }
    // Expand backwards, so the source and destination can share a buffer.
    while (keep > 0) {
        char ch = text[--keep];
        text[--end] = ch;
        if (ch == '\n') {
            end -= marker;
            memcpy(text + end, stamp, marker);
        }
    }
}
