#pragma once

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

// Used only for linearLock. Other allocator locks retain their usual policy.
void CtrMemory_linearLockAcquire(LightLock* lock);
int CtrMemory_linearLockTry(LightLock* lock);
void CtrMemory_linearLockRelease(LightLock* lock);

// The allocation wrappers' existing per-thread relief/overflow policy.
#ifdef __3DS__
extern __thread bool ctrMemoryNoRelief __attribute__((tls_model("local-exec")));
#else
extern __thread bool ctrMemoryNoRelief;
#endif

// Original allocating bodies; entry points retry only after heap relief.
void* CtrMemory_linearAllocOnce(size_t size);
void* CtrMemory_linearMemAlignOnce(size_t size, size_t alignment);
void* CtrMemory_vramAllocOnce(size_t size);
void* CtrMemory_vramAllocAtOnce(size_t size, vramAllocPos pos);
void* CtrMemory_vramMemAlignOnce(size_t size, size_t alignment);
void* CtrMemory_vramMemAlignAtOnce(size_t size, size_t alignment, vramAllocPos pos);

// Heap profiler builds (ctr_heap_profile.c): linear memory and VRAM blocks are attributed to call stacks too.
#ifdef CTRPORT_HEAP_PROFILE
#include <stdbool.h>
#include <stdint.h>
void CtrHeapProfile_noteOther(void* ptr, uint32_t bytes, bool vram, void* caller);
#define HEAPPROF_FIRST2(a, ...) (a)
#define HEAPPROF_FIRST(...) HEAPPROF_FIRST2(__VA_ARGS__, 0)
#define HEAPPROF_NOTE_OTHER(name, p, args) \
    CtrHeapProfile_noteOther((p), (uint32_t) HEAPPROF_FIRST args, (name)[0] == 'v', __builtin_return_address(0))
#else
#define HEAPPROF_NOTE_OTHER(name, p, args) ((void) 0)
#endif
