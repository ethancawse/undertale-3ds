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
