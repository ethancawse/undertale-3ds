// Opt-in attribution of allocations made inside decoder entry points. The
// table follows frees on every thread, including after decoder scope exit.
// It deliberately does not replace the normal heap profiler (which changes
// the Old 64 MiB linear-memory budget by several MiB).
#ifdef CTRPORT_AUDIO_METRICS
#include <3ds.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include "ctr_adpcm_metrics.h"
#include "ctr_audio_decoder.h"
#include "ctr_memory.h"

// 4,096 live allocations: 32 KiB on ARM. No allocation or logging in hooks.
// Overflow is sticky: any nonzero overflow makes the measurement invalid.
#define AM_SLOTS 4096u
#define AM_THREADS 16u
#define AM_LINEAR 0x80000000u
typedef struct { void* ptr; uint32_t bytes; } AmBlock;
typedef struct { void* tls; uint32_t depth; } AmScope;
static AmBlock amBlocks[AM_SLOTS] AM_BSS;
static AmScope amScopes[AM_THREADS] AM_BSS;
static LightLock amLock AM_DATA = 1; // installed libctru LightLock_Init stores 1
static AmAllocationStats amStats AM_BSS;
extern u32 __ctru_heap, __ctru_heap_size;
extern u32 __ctru_linear_heap, __ctru_linear_heap_size;

// Buffers published on the game thread stay live while it takes a snapshot.
// Static/mapped pointers are excluded; linear fallback is requested bytes.
AM_CODE void amMemoryUsage(const void* ptr, size_t requested, uint32_t* heap, uint32_t* linear) {
    if (!ptr) return;
    uintptr_t at = (uintptr_t)ptr;
    if (at >= __ctru_linear_heap && at - __ctru_linear_heap < __ctru_linear_heap_size)
        *linear += (uint32_t)requested;
    else if (at >= __ctru_heap && at - __ctru_heap < __ctru_heap_size)
        *heap += (uint32_t)malloc_usable_size((void*)ptr);
}

AM_CODE static uint32_t amHash(const void* ptr) {
    return (((uintptr_t)ptr >> 3) * 2654435761u) & (AM_SLOTS - 1u);
}

// Caller holds amLock; depth is bounded and scopes are removed when idle.
AM_CODE static int amThread(bool create) {
    void* tls = getThreadLocalStorage();
    int empty = -1;
    for (uint32_t i = 0; i < AM_THREADS; ++i) {
        if (amScopes[i].tls == tls) return (int)i;
        if (!amScopes[i].tls) empty = (int)i;
    }
    if (create && empty >= 0) {
        amScopes[empty].tls = tls;
        return empty;
    }
    if (create) ++amStats.overflow;
    return -1;
}

AM_CODE static void amEnter(void) {
    LightLock_Lock(&amLock);
    int slot = amThread(true);
    if (slot >= 0) ++amScopes[slot].depth;
    LightLock_Unlock(&amLock);
}

AM_CODE static void amLeave(void) {
    LightLock_Lock(&amLock);
    int slot = amThread(false);
    if (slot >= 0 && --amScopes[slot].depth == 0) amScopes[slot].tls = NULL;
    LightLock_Unlock(&amLock);
}

// Backshift the probe chain on deletion: untracked frees stay cheap even
// after a long run has allocated and released many distinct pointers.
AM_CODE static uint32_t amRemove(void* ptr) {
    if (!ptr) return 0;
    uint32_t at = amHash(ptr);
    for (uint32_t i = 0; i < AM_SLOTS; ++i, at = (at + 1u) & (AM_SLOTS - 1u)) {
        AmBlock* b = &amBlocks[at];
        if (!b->ptr) break;
        if (b->ptr != ptr) continue;
        uint32_t bytes = b->bytes;
        if (bytes & AM_LINEAR) amStats.linearRequestedBytes -= bytes & ~AM_LINEAR;
        else amStats.heapBytes -= bytes;
        --amStats.allocations;
        b->ptr = NULL;
        b->bytes = 0;
        uint32_t hole = at;
        uint32_t next = (at + 1u) & (AM_SLOTS - 1u);
        for (uint32_t scan = 0; scan < AM_SLOTS - 1u && amBlocks[next].ptr;
             ++scan, next = (next + 1u) & (AM_SLOTS - 1u)) {
            uint32_t base = amHash(amBlocks[next].ptr);
            if (((next - base) & (AM_SLOTS - 1u)) < ((next - hole) & (AM_SLOTS - 1u))) continue;
            amBlocks[hole] = amBlocks[next];
            amBlocks[next].ptr = NULL;
            amBlocks[next].bytes = 0;
            hole = next;
        }
        return bytes;
    }
    return 0;
}

AM_CODE static void amInsert(void* ptr, size_t requested, bool force) {
    if (!ptr) return;
    int thread = amThread(false);
    if (!force && (thread < 0 || !amScopes[thread].depth)) return;
    uintptr_t atPtr = (uintptr_t)ptr;
    uint32_t bytes;
    if (atPtr >= __ctru_linear_heap && atPtr - __ctru_linear_heap < __ctru_linear_heap_size)
        bytes = (uint32_t)requested | AM_LINEAR;
    else if (atPtr >= __ctru_heap && atPtr - __ctru_heap < __ctru_heap_size)
        bytes = (uint32_t)malloc_usable_size(ptr);
    else return; // static allocator-node pool, not a heap allocation
    if (!bytes || requested >= AM_LINEAR) { ++amStats.overflow; return; }
    uint32_t at = amHash(ptr);
    for (uint32_t i = 0; i < AM_SLOTS; ++i, at = (at + 1u) & (AM_SLOTS - 1u)) {
        AmBlock* b = &amBlocks[at];
        if (b->ptr == ptr) return; // libc's _malloc_r and outer malloc hook
        if (b->ptr) continue;
        b->ptr = ptr;
        b->bytes = bytes;
        if (bytes & AM_LINEAR) amStats.linearRequestedBytes += bytes & ~AM_LINEAR;
        else amStats.heapBytes += bytes;
        ++amStats.allocations;
        return;
    }
    ++amStats.overflow;
}

AM_CODE void amAllocationStats(AmAllocationStats* out) {
    LightLock_Lock(&amLock);
    *out = amStats;
    LightLock_Unlock(&amLock);
}

AM_CODE void* amMalloc(size_t size, const void* caller) {
    void* ptr = CtrMemory_checkedMalloc(size, caller);
    LightLock_Lock(&amLock);
    amInsert(ptr, size, false);
    LightLock_Unlock(&amLock);
    return ptr;
}
AM_CODE void* amCalloc(size_t count, size_t size) {
    void* ptr = CtrMemory_checkedCalloc(count, size);
    LightLock_Lock(&amLock);
    amInsert(ptr, count * size, false);
    LightLock_Unlock(&amLock);
    return ptr;
}
AM_CODE void* amRealloc(void* old, size_t size) {
    // No table lock is held across an allocator: the allocator can itself
    // free a tracked buffer. Realloc's original pointer is only a lookup key.
    LightLock_Lock(&amLock);
    uint32_t tracked = amRemove(old);
    LightLock_Unlock(&amLock);
    void* ptr = CtrMemory_checkedRealloc(old, size);
    LightLock_Lock(&amLock);
    if (ptr) amInsert(ptr, size, tracked != 0);
    else if (size && tracked) amInsert(old, tracked & ~AM_LINEAR, true);
    LightLock_Unlock(&amLock);
    return ptr;
}
AM_CODE void amFree(void* ptr) {
    LightLock_Lock(&amLock);
    amRemove(ptr);
    LightLock_Unlock(&amLock);
    CtrMemory_free(ptr);
}
AM_CODE void amFreeR(struct _reent* reent, void* ptr) {
    LightLock_Lock(&amLock);
    amRemove(ptr);
    LightLock_Unlock(&amLock);
    CtrMemory_freeR(reent, ptr);
}

// libc allocates FILE state through the reentrant entries directly, while
// Tremor/ogg generally use malloc/calloc. Count both, once per live pointer.
void* __real__malloc_r(struct _reent*, size_t);
void* __real__calloc_r(struct _reent*, size_t, size_t);
void* __real__realloc_r(struct _reent*, void*, size_t);
AM_CODE void* __wrap__malloc_r(struct _reent* r, size_t size) {
    void* ptr = __real__malloc_r(r, size);
    LightLock_Lock(&amLock);
    amInsert(ptr, size, false);
    LightLock_Unlock(&amLock);
    return ptr;
}
AM_CODE void* __wrap__calloc_r(struct _reent* r, size_t count, size_t size) {
    void* ptr = __real__calloc_r(r, count, size);
    LightLock_Lock(&amLock);
    amInsert(ptr, count * size, false);
    LightLock_Unlock(&amLock);
    return ptr;
}
AM_CODE void* __wrap__realloc_r(struct _reent* r, void* old, size_t size) {
    LightLock_Lock(&amLock);
    uint32_t tracked = amRemove(old);
    LightLock_Unlock(&amLock);
    void* ptr = __real__realloc_r(r, old, size);
    LightLock_Lock(&amLock);
    if (ptr) amInsert(ptr, size, tracked != 0);
    else if (size && tracked) amInsert(old, tracked & ~AM_LINEAR, true);
    LightLock_Unlock(&amLock);
    return ptr;
}

bool __real_CtrDecoder_openFile(CtrDecoder*, const char*);
bool __real_CtrDecoder_openMemory(CtrDecoder*, const uint8_t*, uint32_t);
int32_t __real_CtrDecoder_read(CtrDecoder*, int16_t*, uint32_t);
bool __real_CtrDecoder_seek(CtrDecoder*, uint64_t);
void __real_CtrDecoder_close(CtrDecoder*);
AM_CODE bool __wrap_CtrDecoder_openFile(CtrDecoder* dec, const char* path) {
    amEnter();
    bool result = __real_CtrDecoder_openFile(dec, path);
    amLeave();
    return result;
}
AM_CODE bool __wrap_CtrDecoder_openMemory(CtrDecoder* dec, const uint8_t* data, uint32_t size) {
    amEnter();
    bool result = __real_CtrDecoder_openMemory(dec, data, size);
    amLeave();
    return result;
}
AM_CODE int32_t __wrap_CtrDecoder_read(CtrDecoder* dec, int16_t* out, uint32_t count) {
    amEnter();
    int32_t result = __real_CtrDecoder_read(dec, out, count);
    amLeave();
    return result;
}
AM_CODE bool __wrap_CtrDecoder_seek(CtrDecoder* dec, uint64_t frame) {
    amEnter();
    bool result = __real_CtrDecoder_seek(dec, frame);
    amLeave();
    return result;
}
AM_CODE void __wrap_CtrDecoder_close(CtrDecoder* dec) {
    amEnter();
    __real_CtrDecoder_close(dec);
    amLeave();
}
#endif
