// Code lives in the late reserve; the TLS input follows ctr_memory.c so its
// two bytes fit before the next aligned TLS object. Gate the complete ELF.
// Keep the original lock call signatures in ctr_memory.c.
#include "ctr_memory_lock.h"

#define ALLOCATION_LATE(name) __attribute__((section(".text.late.titanthrash." #name)))

static __thread bool savedNoRelief;

ALLOCATION_LATE(acquired) static void acquired(void) {
    savedNoRelief = ctrMemoryNoRelief;
    ctrMemoryNoRelief = true;
}

ALLOCATION_LATE(lockAcquire) void CtrMemory_linearLockAcquire(LightLock* lock) {
    LightLock_Lock(lock);
    acquired();
}

ALLOCATION_LATE(lockTry) int CtrMemory_linearLockTry(LightLock* lock) {
    int result = LightLock_TryLock(lock);
    if (result == 0) acquired();
    return result;
}

ALLOCATION_LATE(lockRelease) void CtrMemory_linearLockRelease(LightLock* lock) {
    ctrMemoryNoRelief = savedNoRelief;
    LightLock_Unlock(lock);
}

static __thread bool inRetry;
void* __real_malloc(size_t size);
void __real_free(void* ptr);
void* __wrap_malloc(size_t size);
void __wrap_free(void* ptr);

ALLOCATION_LATE(deferredRelief) static bool deferredRelief(void) {
    if (inRetry) return false;
    void* probe = __real_malloc(24);
    if (probe != NULL) {
        __real_free(probe);
        return false;
    }
    inRetry = true;
    // Explicit wrapper calls keep the normal free(malloc(64)) relief from
    // being eliminated as a malloc/free pair. The caller policy is restored.
    __wrap_free(__wrap_malloc(64));
    return true;
}

#define ALLOC_ENTRY(name, parameters, arguments) \
    void* __wrap_##name parameters; \
    ALLOCATION_LATE(name) void* __wrap_##name parameters { \
        void* p = CtrMemory_##name##Once arguments; \
        if (p == NULL && deferredRelief()) { \
            p = CtrMemory_##name##Once arguments; \
            inRetry = false; \
        } \
        return p; \
    }

ALLOC_ENTRY(linearAlloc, (size_t size), (size))
ALLOC_ENTRY(linearMemAlign, (size_t size, size_t alignment), (size, alignment))
ALLOC_ENTRY(vramAlloc, (size_t size), (size))
ALLOC_ENTRY(vramAllocAt, (size_t size, vramAllocPos pos), (size, pos))
ALLOC_ENTRY(vramMemAlign, (size_t size, size_t alignment), (size, alignment))
ALLOC_ENTRY(vramMemAlignAt, (size_t size, size_t alignment, vramAllocPos pos), (size, alignment, pos))
