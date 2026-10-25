// Heap profiler (CTRPORT_HEAP_PROFILE builds only; the calls compile to nothing otherwise).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef CTRPORT_HEAP_PROFILE
// The call stacks and blocks the profiler can track, and the linear memory its tables take (rounded up to pages).
#define CTR_HEAP_PROFILE_SITES 32768u
#define CTR_HEAP_PROFILE_BLOCKS (1u << 20)
#define CTR_HEAP_PROFILE_SITE_BYTES 48u
#define CTR_HEAP_PROFILE_TABLE_BYTES \
    ((CTR_HEAP_PROFILE_SITES * (CTR_HEAP_PROFILE_SITE_BYTES + 8u) + CTR_HEAP_PROFILE_BLOCKS * 8u + 0x10FFFu) & ~0xFFFu)
// Starts tracking allocations. Call once, early; earlier allocations aren't attributed.
void CtrHeapProfile_init(void);
// Logs the "top" call stacks holding the most live heap bytes, then those holding the most blocks, the live blocks'
// sizes, and the heap's layout (CtrMemory_logHeapLayout).
void CtrHeapProfile_dump(const char* label, int top);
// The first `count` frames of the call stack that allocated the block at `ptr`, if it is tracked.
bool CtrHeapProfile_siteOf(const void* ptr, uint32_t* pcs, int count);
// Fragmentation census (heapfrag): the game entered another room. Logs the heap's free bytes, pieces and largest
// free piece, the live blocks by size class and age in rooms, the blocks freed since the last room by size class and
// age, and every few rooms writes a heap map with each block's birth room (capped in bytes).
void CtrHeapProfile_roomChanged(int32_t roomIndex);
#else
static inline void CtrHeapProfile_roomChanged(int32_t roomIndex) {
    (void) roomIndex;
}
static inline void CtrHeapProfile_init(void) {}
static inline bool CtrHeapProfile_siteOf(const void* ptr, uint32_t* pcs, int count) {
    (void) ptr;
    (void) pcs;
    (void) count;
    return false;
}
static inline void CtrHeapProfile_dump(const char* label, int top) {
    (void) label;
    (void) top;
}
#endif
