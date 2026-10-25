#ifndef CTR_READBACK_H
#define CTR_READBACK_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(__3DS__) && defined(__GNUC__)
#define TPAGGROW_READBACK_COLD __attribute__((noinline, section(".text.late.tpaggrow")))
#define TPAGGROW_READBACK_RODATA __attribute__((section(".rodata.late.tpaggrow")))
#else
#define TPAGGROW_READBACK_COLD
#define TPAGGROW_READBACK_RODATA
#endif

// Capacity travels with the CPU-only RGBA allocation, preserving the renderer
// state layout. Allocation/free calls are replaced at their existing sites.
TPAGGROW_READBACK_COLD void* CtrReadback_reserve(void* pixels, size_t bytes);
TPAGGROW_READBACK_COLD void CtrReadback_free(void* pixels);

#ifdef __3DS__
extern const char CtrReadback_slotLimitMessage[];
TPAGGROW_READBACK_COLD void CtrReadback_logPixels(int32_t surfaceId, int width, int height,
                                               const void* data, uint64_t start, bool nativeScale,
                                               const uint8_t* rgba, size_t bytes);
#endif

#endif
