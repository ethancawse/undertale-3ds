#ifndef CTRPORT_CTR_FPS_CLOCK_H
#define CTRPORT_CTR_FPS_CLOCK_H

#include <stdint.h>

// Same ABI as CtrLog_printTo; the adapter tail-calls it with every argument intact.
void __wrap_CtrLog_printTo(void* console, const char* format, ...)
    __attribute__((format(printf, 2, 3)));
void __wrap_arguiDrawText(int32_t x, int32_t y, const char* text, uint32_t colour);

#endif
