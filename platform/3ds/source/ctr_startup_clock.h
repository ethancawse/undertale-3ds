#ifndef CTRPORT_CTR_STARTUP_CLOCK_H
#define CTRPORT_CTR_STARTUP_CLOCK_H

#include <stddef.h>
#include <stdint.h>

// Real monotonic time, independent of the deterministic game clock. The origin
// is the first C runtime hook after crt0 clears BSS, before libctru initializes
// services/heaps. OS launch, executable loading and BSS clearing precede it.
uint64_t CtrStartup_elapsedUs(void);
uint64_t CtrStartup_libctruUs(void);
// Preserve the start of each line for existing log readers; add a producer-time
// suffix before each newline. Dedicated screen consoles do not use this.
void CtrStartup_stamp(char* text, size_t capacity, uint64_t elapsedUs);

#endif
