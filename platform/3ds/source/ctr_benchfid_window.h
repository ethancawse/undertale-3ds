#ifndef CTRPORT_CTR_BENCHFID_WINDOW_H
#define CTRPORT_CTR_BENCHFID_WINDOW_H

#include <stdbool.h>

// The original benchmark owns these fields and opens its normal counters.
// The isolated option only asks whether this frame is the ready boundary.
typedef struct {
    bool ready;
    void (*open)(void);
} BenchfidWindowHook;

BenchfidWindowHook benchfidWindowHook(void);
#endif
