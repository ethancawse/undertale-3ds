// The split between the normal heap and linear memory (ctr_memory.c), and
// large temporary buffers that may come from either.
#ifndef CTRPORT_CTR_MEMORY_H
#define CTRPORT_CTR_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A buffer of a few MB for decoding: from the heap, or from linear memory if
// the heap has no free block that large. Free with CtrMemory_freeWork.
void* CtrMemory_allocWork(size_t size);
void CtrMemory_freeWork(void* p);
// Whether the calling thread's work buffers come from linear memory first
// (then the heap), as while textures are prepared before the game starts.
void CtrMemory_preferLinearWork(bool prefer);

// How often a thread had to wait for the linear memory lock (ctr_memory.c),
// and for how long in all, since the start.
void CtrMemory_linearLockStats(uint32_t* contended, uint64_t* waitNanos);

// A large buffer that is freed again soon, while loading: from linear memory
// if it has room, so the heap doesn't fragment. Free with CtrMemory_freeWork.
void* CtrMemory_allocTemporary(size_t size);

// When malloc, calloc or realloc can't be satisfied on the game thread, the
// reliever is asked to free heap memory that can be had back later (the
// texture cache's packed copies) and the allocation is tried again, for as
// long as it reports having freed something. It runs on the game thread, in
// the middle of whatever allocated. Pass nullptr to remove it.
typedef bool (*CtrHeapReliever)(void* context, size_t wanted);
void CtrMemory_setHeapReliever(CtrHeapReliever reliever, void* context);
// Called once, by the game thread.
void CtrMemory_markGameThread(void);
// Whether the calling thread's failed allocations ask the reliever and then
// fall back to linear memory (by default they do), for one that has a
// fallback of its own. Returns the previous setting.
bool CtrMemory_allowRelief(bool allow);
// The allocator behind malloc, calloc, realloc and free (the heap profiler's
// wrappers call these): the heap, then the reliever, then linear memory.
// `caller` is the malloc call's return address (see CtrMemory_allocNode), or
// nullptr.
void* CtrMemory_malloc(size_t size, const void* caller);
void* CtrMemory_calloc(size_t count, size_t size);
void* CtrMemory_realloc(void* ptr, size_t size);
// Public allocation attempts clear the calling thread's failure state before
// entering the unchanged allocator body. The implementation uses the bodies
// directly for nested work within the same attempt.
void* CtrMemory_checkedMalloc(size_t size, const void* caller);
void* CtrMemory_checkedCalloc(size_t count, size_t size);
void* CtrMemory_checkedRealloc(void* ptr, size_t size);
#ifndef CTRPORT_CTR_MEMORY_IMPLEMENTATION
#define CtrMemory_malloc CtrMemory_checkedMalloc
#define CtrMemory_calloc CtrMemory_checkedCalloc
#define CtrMemory_realloc CtrMemory_checkedRealloc
#endif
void CtrMemory_free(void* ptr);
// newlib's _free_r (see ctr_memory.c).
struct _reent;
void CtrMemory_freeR(struct _reent* reent, void* ptr);
// Whether p lies in the normal heap (not linear memory or the node pool).
bool CtrMemory_isHeap(const void* p);
// On the game thread, the size of the last allocation that neither the heap,
// the reliever nor linear memory could satisfy (one whose caller had turned
// relief off doesn't count); 0 if there was none, a new allocation has
// started since, or on other threads.
size_t CtrMemory_failedAllocation(void);

// Fatal diagnostics only, in bytes: heap free from the last mallinfo call
// (cached, includes untouched heap), linear/VRAM totals including GPU-held
// frees. No allocation, heap walk or blocking lock; -1 means unavailable.
void CtrMemory_emergencyFree(int32_t* heapCached, int32_t* linear, int32_t* vram);

// For the malloc wrappers: a node of libctru's linear memory or VRAM
// allocator from a static pool when `caller` (the malloc call's return
// address) is that allocator, else nullptr. freeNode returns such a node
// and says whether p was one.
void* CtrMemory_allocNode(size_t size, const void* caller);
bool CtrMemory_isNode(const void* p);
bool CtrMemory_freeNode(void* p);

// VRAM for what can as well do without (tiles' copies in VRAM): an
// allocation that never waits for the GPU to finish with frees held back,
// and the VRAM free now, without those (vramSpaceFree counts them).
void* CtrMemory_vramAllocNow(size_t size);
uint32_t CtrMemory_vramFreeNow(void);

// A live VRAM allocation.
typedef struct {
    uintptr_t addr;
    uint32_t size;
} CtrVramBlock;

// Copies the live VRAM allocations into `out`, sorted by address, and
// returns how many there are; -1 if more than `max`, or if the list stopped
// being complete (more than ctr_memory.c's VRAM_BLOCKS_MAX at once).
// Allocations freed while the GPU was busy aren't on it.
int32_t CtrMemory_vramBlocks(CtrVramBlock* out, int32_t max);

// Frees the calling thread's number conversion caches (newlib keeps them per
// thread, on the heap); they are made again when next needed.
void CtrMemory_releaseNumberCaches(void);
// Has newlib allocate room for `count` (at most 64) open files now, while the
// heap is empty, instead of later wherever it has a gap; it never frees them.
void CtrMemory_reserveFileSlots(int count);

// Logs how the heap is laid out: the bytes in use and free, the largest free
// block, and the first `maxListed` runs of blocks in use, by address.
void CtrMemory_logHeapLayout(const char* stage, int maxListed);

// Writes every chunk of the heap to `path`, with the call stack that allocated it when the heap profiler tracks it,
// for tools/analysis/heapmap.py. False if the file couldn't be written.
bool CtrMemory_writeHeapMap(const char* path, const char* label);

#endif
