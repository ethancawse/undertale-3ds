// Include the production implementation so this gate exercises its table,
// scope accounting and failure paths, using a tiny host platform adapter.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CTRPORT_AUDIO_METRICS
#include "../../platform/3ds/source/z_adpcm_metrics.c"

static unsigned failRealloc;
static uintptr_t fakeThread = 8;
static void* fakeLinear;
uintptr_t __ctru_heap = 0, __ctru_heap_size = UINTPTR_MAX;
uintptr_t __ctru_linear_heap = 0, __ctru_linear_heap_size = 0;
void* getThreadLocalStorage(void) { return (void*)fakeThread; }
void LightLock_Lock(LightLock* lock) { assert(*lock == 1); *lock = 0; }
void LightLock_Unlock(LightLock* lock) { assert(*lock == 0); *lock = 1; }
void* CtrMemory_checkedMalloc(size_t size, const void* caller) { (void)caller; return malloc(size); }
void* CtrMemory_checkedCalloc(size_t count, size_t size) { return calloc(count, size); }
void* CtrMemory_checkedRealloc(void* ptr, size_t size) {
    if (failRealloc) return NULL;
    return realloc(ptr, size);
}
void CtrMemory_free(void* ptr) { free(ptr); }
void CtrMemory_freeR(struct _reent* reent, void* ptr) { (void)reent; free(ptr); }
void* __real__malloc_r(struct _reent* r, size_t size) { (void)r; return malloc(size); }
void* __real__calloc_r(struct _reent* r, size_t count, size_t size) { (void)r; return calloc(count, size); }
void* __real__realloc_r(struct _reent* r, void* ptr, size_t size) { (void)r; return CtrMemory_checkedRealloc(ptr, size); }
bool __real_CtrDecoder_openFile(CtrDecoder* d, const char* p) { (void)d; (void)p; return false; }
bool __real_CtrDecoder_openMemory(CtrDecoder* d, const uint8_t* p, uint32_t n) { (void)d; (void)p; (void)n; return false; }
int32_t __real_CtrDecoder_read(CtrDecoder* d, int16_t* p, uint32_t n) { (void)d; (void)p; (void)n; return 0; }
bool __real_CtrDecoder_seek(CtrDecoder* d, uint64_t n) { (void)d; (void)n; return false; }
void __real_CtrDecoder_close(CtrDecoder* d) { (void)d; }

int main(void) {
    void* untracked = amMalloc(13, NULL);
    assert(amStats.allocations == 0);
    amEnter();
    void* a = amMalloc(31, NULL);
    void* b = amCalloc(3, 17);
    amEnter();
    assert(amScopes[amThread(false)].depth == 2);
    amLeave();
    amLeave();
    assert(amStats.allocations == 2 && amStats.heapBytes >= 82);
    // An inner libc hook and its outer malloc hook see one pointer twice.
    amEnter();
    void* libc = __wrap__malloc_r(NULL, 45);
    amInsert(libc, 45, true);
    assert(amStats.allocations == 3);
    amLeave();
    amFreeR(NULL, libc);
    failRealloc = 1;
    uint32_t before = amStats.heapBytes;
    assert(amRealloc(a, 100000) == NULL);
    assert(amStats.allocations == 2 && amStats.heapBytes == before);
    failRealloc = 0;
    a = amRealloc(a, 1000); // resize outside decoder scope must stay attributed
    assert(a && amStats.allocations == 2 && amStats.heapBytes >= 1051);
    fakeThread = 16; // frees on read-ahead/other threads must remove ownership
    amFree(a);
    amFreeR(NULL, b);
    amFree(untracked);
    assert(amStats.allocations == 0 && amStats.heapBytes == 0);

    fakeLinear = malloc(100);
    __ctru_linear_heap = (uintptr_t)fakeLinear; __ctru_linear_heap_size = 100;
    amEnter(); amInsert(fakeLinear, 100, false); amLeave();
    assert(amStats.linearRequestedBytes == 100 && amStats.heapBytes == 0);
    uint32_t heapUsage = 0, linearUsage = 0;
    amMemoryUsage(fakeLinear, 100, &heapUsage, &linearUsage);
    amMemoryUsage(NULL, 100, &heapUsage, &linearUsage);
    assert(heapUsage == 0 && linearUsage == 100);
    amFree(fakeLinear); fakeLinear = NULL;
    __ctru_linear_heap = __ctru_linear_heap_size = 0;
    assert(amStats.linearRequestedBytes == 0);

    // Force collisions across the table end, delete the middle, then lookup
    // both surviving keys. Synthetic pointer keys are never dereferenced.
    uintptr_t keys[3];
    unsigned found = 0;
    for (uintptr_t key = 8; found < 3; key += 8)
        if (amHash((void*)key) == AM_SLOTS - 1) keys[found++] = key;
    for (unsigned i = 0; i < 3; ++i) {
        unsigned at = (AM_SLOTS - 1 + i) & (AM_SLOTS - 1);
        amBlocks[at] = (AmBlock){(void*)keys[i], 10};
        ++amStats.allocations; amStats.heapBytes += 10;
    }
    assert(amRemove((void*)keys[1]) == 10);
    assert(amRemove((void*)keys[0]) == 10);
    assert(amRemove((void*)keys[2]) == 10);
    assert(amStats.allocations == 0 && amStats.heapBytes == 0);

    // Many allocation lifetimes must not leave tombstones or stale bytes.
    for (unsigned round = 0; round < 100; ++round) {
        void* live[128];
        amEnter();
        for (unsigned i = 0; i < 128; ++i) live[i] = amMalloc(i * 17 + 1, NULL);
        amLeave();
        for (unsigned i = 0; i < 128; ++i) amFree(live[(i * 73) & 127]);
    }
    assert(amStats.allocations == 0 && amStats.heapBytes == 0 && amStats.overflow == 0);
    for (unsigned i = 0; i < AM_SLOTS; ++i) assert(!amBlocks[i].ptr);
    // A genuinely full table must report overflow and support bounded delete.
    for (unsigned i = 0; i < AM_SLOTS; ++i) {
        amBlocks[i] = (AmBlock){(void*)(uintptr_t)((i + 1) * 8), 1};
    }
    amStats.allocations = amStats.heapBytes = AM_SLOTS;
    // Restore a valid open-addressed full table by filling at each key's hash.
    for (unsigned i = 0; i < AM_SLOTS; ++i) {
        void* key = (void*)(uintptr_t)((i + 1) * 8);
        amBlocks[amHash(key)] = (AmBlock){key, 1};
    }
    amEnter();
    void* excess = amMalloc(64, NULL);
    amLeave();
    assert(amStats.overflow == 1 && amStats.allocations == AM_SLOTS);
    amFree(excess);
    assert(amRemove((void*)8) == 1);
    assert(amStats.allocations == AM_SLOTS - 1);
    for (unsigned i = 1; i < AM_SLOTS; ++i)
        assert(amRemove((void*)(uintptr_t)((i + 1) * 8)) == 1);
    assert(amStats.allocations == 0 && amStats.heapBytes == 0);
    puts("audio allocation accounting: scope, realloc failure, cross-thread free, collisions, churn, full-table delete passed");
    return 0;
}
