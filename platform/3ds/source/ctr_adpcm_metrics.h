// Test-only audio allocation accounting. No release code or global TLS slots.
#ifndef CTRPORT_ADPCM_METRICS_H
#define CTRPORT_ADPCM_METRICS_H
#include <stddef.h>
#include <stdint.h>
struct _reent;
#define AM_CODE __attribute__((noinline, noipa, section(".text.late.adpcm")))
#define AM_BSS __attribute__((section(".bss.late.adpcm")))
#define AM_DATA __attribute__((section(".data.late.adpcm")))
#define AM_RO __attribute__((section(".rodata.late.adpcm")))
typedef struct {
    uint32_t heapBytes, linearRequestedBytes, allocations, overflow;
} AmAllocationStats;
AM_CODE void amAllocationStats(AmAllocationStats* out);
AM_CODE void amMemoryUsage(const void* ptr, size_t requested, uint32_t* heap, uint32_t* linear);
AM_CODE void* amMalloc(size_t size, const void* caller);
AM_CODE void* amCalloc(size_t count, size_t size);
AM_CODE void* amRealloc(void* ptr, size_t size);
AM_CODE void amFree(void* ptr);
AM_CODE void amFreeR(struct _reent* reent, void* ptr);
#endif
