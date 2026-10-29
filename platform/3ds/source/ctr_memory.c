// Splits the application's memory between the normal heap and linear memory.
//
// libctru calls __system_allocateHeaps before main. Its default gives linear
// memory (textures, render targets, audio buffers, GPU commands) at most
// 32 MB however much the system grants, so with the New 3DS's 124 MB the
// normal heap got 90 MB, of which Undertale uses about 30, while texture
// pages were evicted and reloaded for want of linear memory. With that much
// memory, split it in half instead. Smaller layouts (Old 3DS) keep libctru's
// split, as the engine needs the normal heap more there.
//
// A game whose heap needs are known can fix the normal heap's size instead
// (FIXED_HEAP_SIZE), and linear memory gets the rest. DELTARUNE on New 3DS
// does: a room can draw from five 2048x2048 pages at once, so textures want
// all the linear memory they can get. With 44 MB the heap had 23-25 MB free
// in Chapters 3-4's battles, while their tiles were evicted and reloaded
// (Chapter 4's Titan fight drew from 61 MB of tiles within a second against
// a 59 MB budget). With 36 MB the budget is 8 MB larger: in Azahar that
// fight went from 24.4 to 27.5 fps and from 252 tile loads to 80 in 20 s.
// Over a 29-scene run that reloads a chapter for each scene (Chapter 1
// through 5) at most 24.2 MB of a 34 MB heap was allocated, packed pages
// included; on hardware audio adds 2-4 MB. 30 MB failed in Chapter 5 after
// 26 chapter loads, a 1.1 MB realloc finding no piece that size, so 36
// leaves room for the heap to split between chapters.
//
// DELTARUNE on Old 3DS aims for a heap of O3DS_HEAP_TARGET_MB instead, when
// that leaves linear memory more than libctru's 32 MB: in the .cia's 80 MB
// mode (78 MB granted) a 42 MB heap and 36.4 MB of linear memory, against
// 46.4 and 32, so that a room drawing from many pages (Chapter 5's castle,
// Flowery) evicts less. Smaller heaps cost more than they gain there: the
// packed page copies get what the game leaves free (see ctr_textures.c),
// and with 38 MB Flowery's 16 pages no longer fit, while some heap must stay
// free in one piece for the next chapter's data.win. In a .3dsx's 64 MB mode
// (62.4 MB granted) the target changes nothing, as the heap is what runs out
// there; it keeps libctru's 30.4 and 32.
//
// A build that loads its generated code one chapter at a time (CTRPORT_AOT_MODULES, ctr_aot_module.c) takes the
// region the modules load into off what the system grants, before the split, and allocates it at the start of the
// heap's address range, where the modules are linked to run; the heap follows it. The main program is smaller by
// about the same amount (its generated code went into the modules), so the heap and linear memory get what they got
// with the code linked in.
#include <3ds.h>
#include <3ds/allocator/mappable.h>
#include <limits.h>
#include <malloc.h>
#include <reent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "log.h"

#include "ctr_aot_module.h"
#include "ctr_gpu_queue.h"
#include "ctr_heap_profile.h"
#include "ctr_lab.h"
#define CTRPORT_CTR_MEMORY_IMPLEMENTATION
#include "ctr_memory.h"
#include "ctr_memory_lock.h"
#define noRelief ctrMemoryNoRelief

// newlib's malloc hands the top of the heap back to sbrk once 128 KB of it is
// free. On the 3DS that gains nothing: the heap is a fixed region already
// committed above, and sbrk only moves a pointer inside it. Never trimming
// keeps mallinfo().arena at the heap's high-water mark, which the [mem] and
// [perf] logs report as the peak.
__attribute__((constructor)) static void keepHeapTop(void) {
    mallopt(M_TRIM_THRESHOLD, INT_MAX);
}

#define MB (1024u * 1024u)
// What is compared is "remaining" in __system_allocateHeaps: the memory the system grants (124 MB on a New 3DS,
// 64 MB for an Old 3DS .3dsx, 78 MB in the .cia's 80 MB mode) minus what the program already committed (its ELF load
// image: code, constants, data and bss, each rounded up to a page) and minus the AOT module region where there is one.
// 96 MB sits between the layouts: an Old 3DS never has more than 78 MB, and a New 3DS with a 13 MB image still has
// 111 MB, so the program can grow by about 15 MB before New comes near it. It was 112 MB until integrate16
// (2026-10-25), when UNDERTALE New's image passed 12 MB (0xC08000), left 112 MB minus 32 KB, and fell back to
// libctru's split: heap 80 / linear 32 MB instead of 56 / 56, and half the tile budget. check_memory_split.cmake
// fails the build if a target's split changes by more than the image would within 4 MB of where it is.
#define LARGE_MEMORY (96u * MB)

#if defined(ENABLE_WAD17) && defined(CTRPORT_TARGET_N3DS)
#define FIXED_HEAP_SIZE (36u * MB)
#endif
#if defined(ENABLE_WAD17) && defined(CTRPORT_TARGET_O3DS)
#ifndef O3DS_HEAP_TARGET_MB
#define O3DS_HEAP_TARGET_MB 42u
#endif
#define TARGET_HEAP_SIZE (O3DS_HEAP_TARGET_MB * MB)
#endif
// libctru's default split below the LARGE_MEMORY threshold: the normal heap gets at least DEFAULT_HEAP_MIN_MB and
// linear memory at most DEFAULT_LINEAR_MAX_MB. UNDERTALE on an Old 3DS (utflowey, rulings 2026-10-25 "1a"
// and 2026-10-26 "option 1") moves 6 MB from the heap to linear memory: heap 20.1 / linear 38.0 MB of the .3dsx's
// 64 MB mode instead of 26.1 / 32.0, so the Omega Flowey fight's whole set (132 tiles, 24.5 MB) stays resident beside
// the sound effects and the 3 MB linear floor (at 36 the floor evicted set tiles and 7 came back from the SD card in
// play). The game's own heap need in play is at most 17.8 MB (results/utflowey.md, census and the boss suite at 20.1).
// check_memory_split.cmake reads both numbers. The constants keep __system_allocateHeaps's code size (#line below).
#if !defined(ENABLE_WAD17) && defined(CTRPORT_TARGET_O3DS)
#define DEFAULT_HEAP_MIN_MB 20u
#define DEFAULT_LINEAR_MAX_MB 38u
#else
#define DEFAULT_HEAP_MIN_MB 24u
#define DEFAULT_LINEAR_MAX_MB 32u
#endif
#line 92

extern char* fake_heap_start;
extern char* fake_heap_end;
extern u32 __ctru_heap;
extern u32 __ctru_linear_heap;
extern u32 __ctru_heap_size;
extern u32 __ctru_linear_heap_size;

// ===[ Linear memory lock ]===

// libctru's linear allocator has no lock, and several threads use it: the
// game thread (textures, citro3d), the audio decoder and the texture packing
// threads (work buffers that don't fit in the heap). The link wraps every
// call to it (--wrap in CMakeLists.txt), libctru's and citro3d's included, so
// they all take this lock. It is held only while the allocator runs, which
// takes no other lock but newlib's malloc lock (its tree nodes).
static LightLock linearLock;
// How often a thread found the lock taken, and how long such threads waited
// in all (system ticks). Updated by the thread that then holds the lock.
static uint32_t linearLockContended;
static uint64_t linearLockWaitTicks;

static void lockLinear(void) {
    if (CtrMemory_linearLockTry(&linearLock) == 0) return;
    uint64_t start = svcGetSystemTick();
    CtrMemory_linearLockAcquire(&linearLock);
    linearLockContended++;
    linearLockWaitTicks += svcGetSystemTick() - start;
}

// memory_report.c's names for the lock and the GPU-held linear bytes (its probe of the largest linear piece), and for
// the heap layout's run list (its out-of-memory snapshot). Assembler aliases, so this file's code stays as main's.
__asm__(".globl CtrMemory_linearLockStore\n.set CtrMemory_linearLockStore, linearLock\n"
        ".globl CtrMemory_heldLinearStore\n.set CtrMemory_heldLinearStore, heldLinearBytes\n"
        ".globl CtrMemory_heapRunStore\n.set CtrMemory_heapRunStore, heapRuns\n");

void CtrMemory_linearLockStats(uint32_t* contended, uint64_t* waitNanos) {
    CtrMemory_linearLockAcquire(&linearLock);
    *contended = linearLockContended;
    *waitNanos = linearLockWaitTicks * 1000000000ull / SYSCLOCK_ARM11;
    CtrMemory_linearLockRelease(&linearLock);
}

// ===[ Frees the GPU may still need ]===

// The GPU runs a frame while the CPU records the next (ctr_gpu_queue.h), and
// the next frame may free a texture the one on the GPU samples: Chapter 4's
// page tiles are evicted to make room for others. Memory freed while the GPU
// is busy is only given back once it has finished what it was running then,
// so nothing allocated meanwhile can be written over what it reads. An
// allocation that fails while frees are held back waits for the GPU and tries
// again, so it fails only when the memory really isn't there.
typedef struct {
    void* mem;
    uint32_t bytes;
    // Given back once the GPU's completion count has reached this.
    uint32_t due;
    bool vram;
} HeldFree;

#define HELD_FREE_CAPACITY 1024
static HeldFree heldFrees[HELD_FREE_CAPACITY];
static uint32_t heldCount;
static uint32_t heldLinearBytes, heldVramBytes;

void __real_linearFree(void* mem);
size_t __real_linearGetSize(void* mem);
void __real_vramFree(void* mem);

// With the linear lock held. All of them if the GPU is idle.
static void releaseHeldFrees(void) {
    if (heldCount == 0) return;
    bool idle = !CtrGpu_busy();
    uint32_t done = CtrGpu_completions();
    uint32_t kept = 0;
    for (uint32_t i = 0; heldCount > i; i++) {
        HeldFree* h = &heldFrees[i];
        if (!idle && 0 > (int32_t) (done - h->due)) {
            heldFrees[kept++] = *h;
            continue;
        }
        if (h->vram) {
            heldVramBytes -= h->bytes;
            __real_vramFree(h->mem);
        } else {
            heldLinearBytes -= h->bytes;
            __real_linearFree(h->mem);
        }
    }
    heldCount = kept;
}

// Waits for the GPU from any thread (libctru's own wait only suits one).
static void waitForGpu(void) {
    while (CtrGpu_busy()) svcSleepThread(100 * 1000);
}

static void freeWhenGpuDone(void* mem, bool vram) {
    if (mem == nullptr) return;
    lockLinear();
    releaseHeldFrees();
    if (CtrGpu_busy() && heldCount == HELD_FREE_CAPACITY) {
        CtrMemory_linearLockRelease(&linearLock);
        waitForGpu();
        lockLinear();
        releaseHeldFrees();
    }
    if (CtrGpu_busy()) {
        uint32_t bytes = (uint32_t) (vram ? vramGetSize(mem) : __real_linearGetSize(mem));
        heldFrees[heldCount++] = (HeldFree) { mem, bytes, CtrGpu_completionDue(), vram };
        if (vram) heldVramBytes += bytes;
        else heldLinearBytes += bytes;
    } else if (vram) {
        __real_vramFree(mem);
    } else {
        __real_linearFree(mem);
    }
    CtrMemory_linearLockRelease(&linearLock);
}

// With the linear lock held: whether an allocation that just failed is worth
// trying again, after waiting for the GPU and giving back what it held.
static bool retryAfterGpu(void) {
    if (heldCount == 0) return false;
    CtrMemory_linearLockRelease(&linearLock);
    waitForGpu();
    lockLinear();
    releaseHeldFrees();
    return true;
}

// ===[ Linear memory ]===

void* __real_linearAlloc(size_t size);
void* __real_linearMemAlign(size_t size, size_t alignment);
void* __real_linearRealloc(void* mem, size_t size);
u32 __real_linearSpaceFree(void);
void* CtrMemory_linearAllocOnce(size_t size);
void* CtrMemory_linearMemAlignOnce(size_t size, size_t alignment);
void* __wrap_linearRealloc(void* mem, size_t size);
size_t __wrap_linearGetSize(void* mem);
void __wrap_linearFree(void* mem);
u32 __wrap_linearSpaceFree(void);

void* CtrMemory_linearAllocOnce(size_t size) {
    lockLinear();
    releaseHeldFrees();
    void* p = __real_linearAlloc(size);
    if (p == nullptr && retryAfterGpu()) p = __real_linearAlloc(size);
    CtrMemory_linearLockRelease(&linearLock);
    return p;
}

void* CtrMemory_linearMemAlignOnce(size_t size, size_t alignment) {
    lockLinear();
    releaseHeldFrees();
    void* p = __real_linearMemAlign(size, alignment);
    if (p == nullptr && retryAfterGpu()) p = __real_linearMemAlign(size, alignment);
    CtrMemory_linearLockRelease(&linearLock);
    return p;
}

// Nothing reallocates memory the GPU reads.
void* __wrap_linearRealloc(void* mem, size_t size) {
    lockLinear();
    void* p = __real_linearRealloc(mem, size);
    CtrMemory_linearLockRelease(&linearLock);
    return p;
}

size_t __wrap_linearGetSize(void* mem) {
    lockLinear();
    size_t size = __real_linearGetSize(mem);
    CtrMemory_linearLockRelease(&linearLock);
    return size;
}

void __wrap_linearFree(void* mem) { HEAPPROF_FREE_OTHER(mem);
    freeWhenGpuDone(mem, false);
}

// Counting what is held back, which an allocation waits for if it must.
u32 __wrap_linearSpaceFree(void) {
    lockLinear();
    releaseHeldFrees();
    u32 space = __real_linearSpaceFree() + heldLinearBytes;
    CtrMemory_linearLockRelease(&linearLock);
    return space;
}

// ===[ VRAM ]===

// Only the main thread uses VRAM, but its frees are held back as linear
// memory's are, under the same lock.
//
// Every live allocation is kept in vramBlocks (CtrMemory_vramBlocks), so the
// renderer can see where VRAM is split up (see "Moving surfaces in VRAM" in
// ctr_renderer.c). A free comes off the list at once, though it may be held
// back: it is free once the GPU is done.
#define VRAM_BLOCKS_MAX 1024
static CtrVramBlock vramBlocks[VRAM_BLOCKS_MAX];
static uint32_t vramBlockCount;
static bool vramBlocksLost; // one didn't fit, so the list is incomplete from then on

// With the linear lock held.
static void trackVram(void* p) {
    if (p == nullptr) return;
    if (vramBlockCount == VRAM_BLOCKS_MAX) {
        vramBlocksLost = true;
        return;
    }
    vramBlocks[vramBlockCount++] = (CtrVramBlock) { (uintptr_t) p, (uint32_t) vramGetSize(p) };
}

static void untrackVram(void* p) {
    for (uint32_t i = 0; vramBlockCount > i; i++) {
        if (vramBlocks[i].addr != (uintptr_t) p) continue;
        vramBlocks[i] = vramBlocks[--vramBlockCount];
        return;
    }
}

int32_t CtrMemory_vramBlocks(CtrVramBlock* out, int32_t max) {
    lockLinear();
    int32_t n = vramBlocksLost || (uint32_t) max < vramBlockCount ? -1 : (int32_t) vramBlockCount;
    for (int32_t i = 0; n > i; i++) out[i] = vramBlocks[i];
    CtrMemory_linearLockRelease(&linearLock);
    for (int32_t i = 1; n > i; i++) {
        CtrVramBlock b = out[i];
        int32_t j = i;
        for (; j > 0 && out[j - 1].addr > b.addr; j--) out[j] = out[j - 1];
        out[j] = b;
    }
    return n;
}

void* __real_vramAlloc(size_t size);
void* __real_vramAllocAt(size_t size, vramAllocPos pos);
void* __real_vramMemAlign(size_t size, size_t alignment);
void* __real_vramMemAlignAt(size_t size, size_t alignment, vramAllocPos pos);
u32 __real_vramSpaceFree(void);
void* CtrMemory_vramAllocOnce(size_t size);
void* CtrMemory_vramAllocAtOnce(size_t size, vramAllocPos pos);
void* CtrMemory_vramMemAlignOnce(size_t size, size_t alignment);
void* CtrMemory_vramMemAlignAtOnce(size_t size, size_t alignment, vramAllocPos pos);
void __wrap_vramFree(void* mem);
u32 __wrap_vramSpaceFree(void);

void* CtrMemory_vramAllocOnce(size_t size) {
    lockLinear();
    releaseHeldFrees();
    void* p = __real_vramAlloc(size);
    if (p == nullptr && retryAfterGpu()) p = __real_vramAlloc(size);
    trackVram(p);
    CtrMemory_linearLockRelease(&linearLock);
    return p;
}

void* CtrMemory_vramAllocAtOnce(size_t size, vramAllocPos pos) {
    lockLinear();
    releaseHeldFrees();
    void* p = __real_vramAllocAt(size, pos);
    if (p == nullptr && retryAfterGpu()) p = __real_vramAllocAt(size, pos);
    trackVram(p);
    CtrMemory_linearLockRelease(&linearLock);
    return p;
}

void* CtrMemory_vramMemAlignOnce(size_t size, size_t alignment) {
    lockLinear();
    releaseHeldFrees();
    void* p = __real_vramMemAlign(size, alignment);
    if (p == nullptr && retryAfterGpu()) p = __real_vramMemAlign(size, alignment);
    trackVram(p);
    CtrMemory_linearLockRelease(&linearLock);
    return p;
}

void* CtrMemory_vramMemAlignAtOnce(size_t size, size_t alignment, vramAllocPos pos) {
    lockLinear();
    releaseHeldFrees();
    void* p = __real_vramMemAlignAt(size, alignment, pos);
    if (p == nullptr && retryAfterGpu()) p = __real_vramMemAlignAt(size, alignment, pos);
    trackVram(p);
    CtrMemory_linearLockRelease(&linearLock);
    return p;
}

void __wrap_vramFree(void* mem) { HEAPPROF_FREE_OTHER(mem);
    lockLinear();
    untrackVram(mem);
    CtrMemory_linearLockRelease(&linearLock);
    freeWhenGpuDone(mem, true);
}

u32 __wrap_vramSpaceFree(void) {
    lockLinear();
    releaseHeldFrees();
    u32 space = __real_vramSpaceFree() + heldVramBytes;
    CtrMemory_linearLockRelease(&linearLock);
    return space;
}

void* CtrMemory_vramAllocNow(size_t size) {
    lockLinear();
    releaseHeldFrees();
    void* p = __real_vramAlloc(size);
    trackVram(p);
    CtrMemory_linearLockRelease(&linearLock);
    return p;
}

uint32_t CtrMemory_vramFreeNow(void) {
    lockLinear();
    releaseHeldFrees();
    u32 space = __real_vramSpaceFree();
    CtrMemory_linearLockRelease(&linearLock);
    return space;
}
// ===[ Heaps ]===

#ifdef CTRPORT_AOT_MODULES
static u32 aotRegionSize;   // reserved at CTR_AOT_MODULE_BASE
#endif

// Mirrors libctru's default (libctru/source/system/allocateHeaps.c) apart
// from the sizes.
void __system_allocateHeaps(void) {
#include "ctr_allocate_heaps.inc"
}
#line 491

#ifdef CTRPORT_AOT_MODULES
bool CtrAotModule_regionReserved(void) {
    return aotRegionSize == CTR_AOT_MODULE_REGION_SIZE;
}

// libctru frees the heap on the way out (__libctru_exit); the region goes too.
__attribute__((destructor)) static void freeAotRegion(void) {
    if (aotRegionSize == 0) return;
    u32 at;
    svcControlMemory(&at, CTR_AOT_MODULE_BASE, 0, aotRegionSize, MEMOP_FREE, 0);
    aotRegionSize = 0;
}
#endif

// ===[ Running out of heap ]===

// The game's heap use grows as it plays (a room's arrays, Chapter 4's titan
// fight 6 MB over its first room), and the texture cache keeps packed pages
// in what it reckons the game leaves free. When an allocation fails anyway,
// those copies go before the game would have to: the link wraps malloc,
// calloc and realloc (--wrap in CMakeLists.txt) to ask the reliever. Only on
// the game thread, which owns the copies; other threads go straight to
// linear memory (see "Heap overflow").
static CtrHeapReliever heapReliever;
static void* heapRelieverContext;
static __thread bool isGameThread;
static __thread bool relieving;   // the reliever is running (its own frees don't allocate, but its log may)
__thread bool noRelief;    // the caller has somewhere else to go (CtrMemory_allowRelief)

void CtrMemory_setHeapReliever(CtrHeapReliever reliever, void* context) {
    heapReliever = reliever;
    heapRelieverContext = context;
}

void CtrMemory_markGameThread(void) {
    isGameThread = true;
}

bool CtrMemory_allowRelief(bool allow) {
    bool was = !noRelief;
    noRelief = !allow;
    return was;
}

static bool relieveHeap(size_t wanted) {
    if (!isGameThread || relieving || noRelief || heapReliever == nullptr) return false;
    relieving = true;
    bool freed = heapReliever(heapRelieverContext, wanted);
    relieving = false;
    return freed;
}

// malloc, calloc, realloc and free themselves are under "Heap overflow"
// below: the heap, then the reliever, then linear memory.

// ===[ Work buffers ]===

// The normal heap fragments over a long run (pages loading and unloading, the
// game's own churn) until a few MB in one piece can't be had even with half
// of it free. Linear memory holds mostly textures, in blocks of 256 KB at
// most, so a large block can usually be found there instead.
//
// A thread can prefer linear memory instead (CtrMemory_preferLinearWork):
// before the game's textures load, linear memory is nearly all free, while
// the heap holds the game's data and should stay in one piece for it.
static __thread bool preferLinear;

void CtrMemory_preferLinearWork(bool prefer) {
    preferLinear = prefer;
}

void* CtrMemory_allocWork(size_t size) {
    if (preferLinear) {
        void* p = linearAlloc(size);
        return p != nullptr ? p : malloc(size);
    }
    // Linear memory before the packed copies.
    bool allowed = CtrMemory_allowRelief(false);
    void* p = malloc(size);
    CtrMemory_allowRelief(allowed);
    if (p == nullptr) p = linearAlloc(size);
    return p != nullptr ? p : malloc(size);
}

void CtrMemory_freeWork(void* p) {
    uintptr_t at = (uintptr_t) p;
    if (__ctru_linear_heap <= at && __ctru_linear_heap + __ctru_linear_heap_size > at) {
        linearFree(p);
    } else {
        free(p);
    }
}

// ===[ Allocator nodes ]===

// libctru's linear memory and VRAM allocators (MemPool) keep a 16-byte node
// on the heap for every free region, made with malloc whenever an allocation
// or a free splits one. Textures and surfaces come and go all through a
// chapter, so these nodes were the last blocks splitting the heap after an
// unload, one in the middle of it after every chapter. They come from a
// static pool instead, picked out by the caller's address: MemPool::Allocate
// and MemPool::Deallocate, which lie between CoalesceRight and GetFreeSpace
// in libctru. If that ever isn't so, or the pool is full, they go to the heap
// as before.
extern char _ZN7MemPool13CoalesceRightEP8MemBlock[];
extern char _ZN7MemPool12GetFreeSpaceEv[];

#define NODE_BYTES 16
#define NODE_COUNT 2048

typedef union PoolNode {
    union PoolNode* next;
    uint8_t bytes[NODE_BYTES];
} PoolNode;

static PoolNode nodes[NODE_COUNT];
static PoolNode* freeNodes;
static uint32_t nodesMade;
static LightLock nodeLock = 1; // unlocked (LightLock_Init stores 1)

bool CtrMemory_isNode(const void* p) {
    return (const void*) nodes <= p && (const void*) (nodes + NODE_COUNT) > p;
}

void* CtrMemory_allocNode(size_t size, const void* caller) {
    uintptr_t at = (uintptr_t) caller;
    uintptr_t from = (uintptr_t) _ZN7MemPool13CoalesceRightEP8MemBlock, to = (uintptr_t) _ZN7MemPool12GetFreeSpaceEv;
    if (size > NODE_BYTES || from > at || at >= to || to - from > 4096) return nullptr;
    LightLock_Lock(&nodeLock);
    PoolNode* node = freeNodes;
    if (node != nullptr) {
        freeNodes = node->next;
    } else if (NODE_COUNT > nodesMade) {
        node = &nodes[nodesMade++];
    }
    LightLock_Unlock(&nodeLock);
    return node;
}

bool CtrMemory_freeNode(void* p) {
    if (!CtrMemory_isNode(p)) return false;
    PoolNode* node = (PoolNode*) p;
    LightLock_Lock(&nodeLock);
    node->next = freeNodes;
    freeNodes = node;
    LightLock_Unlock(&nodeLock);
    return true;
}

// ===[ Heap overflow ]===

// DELTARUNE switches chapters in one process. Small blocks that outlive a
// chapter stay where they were allocated, spread over the heap, and the next
// chapter's blocks of several MB must fit between them: on an Old 3DS the
// 4.1 MB string chunk of Chapter 5 found no room with 0.2 MB of the heap in
// use. Rather than fail, an allocation the heap can't satisfy, even after the
// reliever, comes from linear memory, which free() and realloc() recognise by
// its address. Not when the caller has turned relief off: it has a fallback
// of its own (a work buffer, a packed page read tile by tile), and linear
// memory is better left to textures. The heap profiler's wrappers
// (ctr_heap_profile.c) call these too.
void* __real_malloc(size_t size);
void* __real_calloc(size_t count, size_t size);
void* __real_realloc(void* ptr, size_t size);
void __real_free(void* ptr);

static uint32_t overflowCount;
static uint64_t overflowBytes;

static inline bool isLinear(const void* p) {
    uintptr_t at = (uintptr_t) p;
    return __ctru_linear_heap <= at && __ctru_linear_heap + __ctru_linear_heap_size > at;
}

// The size of the calling thread's last allocation that nothing could satisfy.
static __thread size_t failedSize;

#if defined(CTRPORT_PKLIN)
// freewins L1 (UNDERTALE Old): linearAlloc, and on the game thread, idle tiles evicted until the piece fits
// (z_utstall_l2.c, late code). Only the call's target changes here; the #line below puts every later __LINE__ back.
void* CtrMemory_overflowLinear(size_t size);
#define overflowLinearAlloc CtrMemory_overflowLinear
#else
#define overflowLinearAlloc linearAlloc
#endif
#line 616
static void* overflowAlloc(size_t size) {
    if (noRelief) return nullptr;
    void* p = overflowLinearAlloc(size);
    if (p != nullptr) {
        __atomic_fetch_add(&overflowCount, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&overflowBytes, (uint64_t) size, __ATOMIC_RELAXED);
    } else {
        failedSize = size;
    }
    return p;
}

size_t CtrMemory_failedAllocation(void) {
    return isGameThread ? failedSize : 0;
}

void* CtrMemory_malloc(size_t size, const void* caller) {
    void* p = CtrMemory_allocNode(size, caller);
    if (p != nullptr) return p;
    p = __real_malloc(size);
    while (p == nullptr && size != 0 && relieveHeap(size)) p = __real_malloc(size);
    return p != nullptr || size == 0 ? p : overflowAlloc(size);
}

void* CtrMemory_calloc(size_t count, size_t size) {
    void* p = __real_calloc(count, size);
    if (p != nullptr || count == 0 || size == 0 || count > SIZE_MAX / size) return p;
    while (p == nullptr && relieveHeap(count * size)) p = __real_calloc(count, size);
    if (p != nullptr) return p;
    p = overflowAlloc(count * size);
    if (p != nullptr) memset(p, 0, count * size);
    return p;
}

void* CtrMemory_realloc(void* ptr, size_t size) {
    if (CtrMemory_isNode(ptr) || (ptr != nullptr && isLinear(ptr))) {
        if (size == 0) {
            CtrMemory_free(ptr);
            return nullptr;
        }
        size_t old = CtrMemory_isNode(ptr) ? NODE_BYTES : linearGetSize(ptr);
        if (isLinear(ptr) && old >= size) return ptr;
        void* p = CtrMemory_malloc(size, nullptr);
        if (p == nullptr) return nullptr;
        memcpy(p, ptr, old < size ? old : size);
        CtrMemory_free(ptr);
        return p;
    }
    void* p = __real_realloc(ptr, size);
    if (p != nullptr || size == 0) return p;
    // A failed realloc leaves the block where it was.
    while (p == nullptr && relieveHeap(size)) p = __real_realloc(ptr, size);
    if (p != nullptr) return p;
    p = overflowAlloc(size);
    if (p != nullptr && ptr != nullptr) {
        size_t old = malloc_usable_size(ptr);
        memcpy(p, ptr, old < size ? old : size);
        __real_free(ptr);
    }
    return p;
}

void CtrMemory_free(void* ptr) {
    if (CtrMemory_freeNode(ptr)) return;
    if (ptr != nullptr && isLinear(ptr)) {
        linearFree(ptr);
    } else {
        __real_free(ptr);
    }
}

// newlib frees a few blocks it got from malloc with _free_r, out of sight of
// the free wrapper: fclose frees setvbuf's buffer (128 KB for a data.win)
// that way. One from linear memory or the node pool would have gone to the
// heap's free lists. free() reaches here too, through newlib's own free().
void __real__free_r(struct _reent* reent, void* ptr);

#ifdef CTRPORT_AUDIO_METRICS
// The ordinary build inlines this into __wrap__free_r and discards its body.
// Diagnostic accounting needs an out-of-line call: keep that newly live body
// late rather than introducing it ahead of every later warm function.
__attribute__((noinline, noipa, section(".text.late.adpcm")))
#endif
#line 745
void CtrMemory_freeR(struct _reent* reent, void* ptr) {
    if (CtrMemory_freeNode(ptr)) return;
    if (ptr != nullptr && isLinear(ptr)) {
        linearFree(ptr);
    } else {
        __real__free_r(reent, ptr);
    }
}

bool CtrMemory_isHeap(const void* p) {
    uintptr_t at = (uintptr_t) p;
    return __ctru_heap <= at && __ctru_heap + __ctru_heap_size > at;
}

// Clear failure state at the public entry, outside the existing allocator
// bodies. The assembler name prevents GCC from cloning the malloc body here.
// Internal calls made while satisfying one realloc stay in the same attempt.
extern void* mallocBody(size_t size, const void* caller) __asm__("CtrMemory_malloc");
// Retain GCC's original partial-inline decision for the allocator body even
// though the public wrapper now enters through the TLS-reset helper.
__attribute__((used, noipa, section(".fatalflush")))
static void* mallocInlineShape(size_t size, const void* caller) {
    return CtrMemory_malloc(size, caller);
}
__attribute__((noipa, optimize("no-inline"), section(".fatalflush")))
void* CtrMemory_checkedMalloc(size_t size, const void* caller) {
    failedSize = 0;
    return mallocBody(size, caller);
}

__attribute__((noipa, optimize("no-inline"), section(".fatalflush")))
void* CtrMemory_checkedCalloc(size_t count, size_t size) {
    failedSize = 0;
    return CtrMemory_calloc(count, size);
}

__attribute__((noipa, optimize("no-inline"), section(".fatalflush")))
void* CtrMemory_checkedRealloc(void* ptr, size_t size) {
    failedSize = 0;
    return CtrMemory_realloc(ptr, size);
}

#ifdef CTRPORT_AUDIO_METRICS
#include "ctr_adpcm_metrics.h"
#define CtrMemory_checkedMalloc amMalloc
#define CtrMemory_checkedCalloc amCalloc
#define CtrMemory_checkedRealloc amRealloc
#define CtrMemory_free amFree
#define CtrMemory_freeR amFreeR
#define AM_MALLOC_BRANCH "b amMalloc\n"
#else
#define AM_MALLOC_BRANCH "b CtrMemory_checkedMalloc\n"
#endif
#line 788
#ifndef CTRPORT_HEAP_PROFILE
void* __wrap_malloc(size_t size);
void* __wrap_calloc(size_t count, size_t size);
void* __wrap_realloc(void* ptr, size_t size);
void __wrap_free(void* ptr);

#if defined(ENABLE_WAD17) && defined(__3DS__) && defined(NDEBUG)
// Retain the old duplicate malloc entry's 220 B slot, without executing its
// unused bytes or adding a prologue merely to forward the caller's address.
__attribute__((naked))
void* __wrap_malloc(size_t size __attribute__((unused))) {
    __asm__ volatile("mov r1, lr\n" AM_MALLOC_BRANCH ".space 212");
}
#else
void* __wrap_malloc(size_t size) {
    return CtrMemory_checkedMalloc(size, __builtin_return_address(0));
}
#endif

void* __wrap_calloc(size_t count, size_t size) {
    return CtrMemory_checkedCalloc(count, size);
}

void* __wrap_realloc(void* ptr, size_t size) {
    return CtrMemory_checkedRealloc(ptr, size);
}

#ifdef CTRPORT_AUDIO_METRICS
// The diagnostic forwards through late accounting, retaining the ARM Release
// allocator wrappers' original extents (including their inlined free bodies).
__attribute__((naked))
void __wrap_free(void* ptr __attribute__((unused))) {
    __asm__ volatile("b amFree\n.space 148");
}
#else
void __wrap_free(void* ptr) {
    CtrMemory_free(ptr);
}
#endif
#line 818

void __wrap__free_r(struct _reent* reent, void* ptr);
#ifdef CTRPORT_AUDIO_METRICS
__attribute__((naked))
void __wrap__free_r(struct _reent* reent __attribute__((unused)), void* ptr __attribute__((unused))) {
    __asm__ volatile("b amFreeR\n.space 152");
}
#else
void __wrap__free_r(struct _reent* reent, void* ptr) {
    CtrMemory_freeR(reent, ptr);
}
#endif
#line 823
#endif

// ===[ Blocks newlib keeps ]===
#ifdef CTRPORT_AUDIO_METRICS
#undef CtrMemory_checkedMalloc
#undef CtrMemory_checkedCalloc
#undef CtrMemory_checkedRealloc
#undef CtrMemory_free
#undef CtrMemory_freeR
#endif
#line 826

// Each thread's struct _reent caches number conversion buffers (printf's %f,
// strtod) on the heap, and libctru frees a thread without them. Threads
// that come and go with each chapter left them all over the heap, a few
// every chapter.
void CtrMemory_releaseNumberCaches(void) {
    struct _reent* r = _REENT;
    struct _Bigint** freelist = _REENT_MP_FREELIST(r);
    if (freelist != nullptr) {
        size_t lists = malloc_usable_size(freelist) / sizeof(*freelist);
        for (size_t k = 0; lists > k; k++) {
            for (struct _Bigint* b = freelist[k]; b != nullptr;) {
                struct _Bigint* next = b->_next;
                _free_r(r, b);
                b = next;
            }
        }
        _free_r(r, freelist);
        _REENT_MP_FREELIST(r) = nullptr;
    }
    for (struct _Bigint* b = _REENT_MP_P5S(r); b != nullptr;) {
        struct _Bigint* next = b->_next;
        _free_r(r, b);
        b = next;
    }
    _REENT_MP_P5S(r) = nullptr;
    if (_REENT_MP_RESULT(r) != nullptr) {
        _free_r(r, _REENT_MP_RESULT(r));
        _REENT_MP_RESULT(r) = nullptr;
    }
    if (_REENT_CVTBUF(r) != nullptr) {
        _free_r(r, _REENT_CVTBUF(r));
        _REENT_CVTBUF(r) = nullptr;
        _REENT_CVTLEN(r) = 0;
    }
}

// Every thread, libctru's included (the link wraps threadCreate), starts
// through this so its number caches go when its function returns, and, in lab
// mode, with an exception handler entry of its own (ctr_lab.h).
Thread CtrExitGsp_threadCreate(ThreadFunc entry, void* arg, size_t stackSize, int prio, int core, bool detached);
Thread __wrap_threadCreate(ThreadFunc entry, void* arg, size_t stackSize, int prio, int core, bool detached);

typedef struct {
    ThreadFunc entry;
    void* arg;
} ThreadStart;

static void threadTrampoline(void* p) {
    ThreadStart start = *(ThreadStart*) p;
    free(p);
    CtrLab_runThread(start.entry, start.arg);
    CtrMemory_releaseNumberCaches();
}

Thread __wrap_threadCreate(ThreadFunc entry, void* arg, size_t stackSize, int prio, int core, bool detached) {
    ThreadStart* start = malloc(sizeof(ThreadStart));
    if (start == nullptr) return nullptr;
    *start = (ThreadStart) { entry, arg };
    Thread t = CtrExitGsp_threadCreate(threadTrampoline, start, stackSize, prio, core, detached);
    if (t == nullptr) free(start);
    return t;
}

// newlib allocates FILE structures four at a time, the first time more
// files are open at once than ever before, and never frees them. Taking
// enough at startup keeps them at the bottom of the heap instead of wherever
// a chapter happened to open its most files.
void CtrMemory_reserveFileSlots(int count) {
    FILE* files[64];
    if (count > 64) count = 64;
    static char dummy[1];
    int opened = 0;
    while (count > opened) {
        files[opened] = fmemopen(dummy, sizeof(dummy), "r");
        if (files[opened] == nullptr) break;
        opened++;
    }
    while (opened > 0) fclose(files[--opened]);
}

// ===[ Heap layout ]===

// newlib's malloc (dlmalloc 2.6): chunks lie end to end from where sbrk
// started, each headed by its predecessor's size and its own, whose low bit
// says whether the chunk before it is in use.
extern char* __malloc_sbrk_base;
extern struct mallinfo __malloc_current_mallinfo;
void __malloc_lock(struct _reent* reent);
void __malloc_unlock(struct _reent* reent);

__attribute__((noinline, section(".fatalflush")))
void CtrMemory_emergencyFree(int32_t* heapCached, int32_t* linear, int32_t* vram) {
    // mallinfo walks the heap's free lists under __malloc_lock. In a fatal
    // path that lock or those lists may already be broken: use its last
    // published total instead. This includes the heap sbrk has not reached.
    uint32_t used = __malloc_current_mallinfo.uordblks;
    *heapCached = used <= __ctru_heap_size ? (int32_t) (__ctru_heap_size - used) : -1;
    *linear = *vram = -1;
    if (CtrMemory_linearLockTry(&linearLock) != 0) return;
    // The public SpaceFree wrappers release GPU-held frees, which can call
    // malloc/free. Query the totals directly without releasing anything.
    *linear = (int32_t) (__real_linearSpaceFree() + heldLinearBytes);
    *vram = (int32_t) (__real_vramSpaceFree() + heldVramBytes);
    CtrMemory_linearLockRelease(&linearLock);
}

typedef struct {
    uint32_t offset; // from the start of the heap
    uint32_t size;
    uint32_t blocks;
    uintptr_t first; // the first block's data
    char head[9];    // its first bytes, printable ones only
    bool used;
} HeapRun;

#define HEAP_RUNS 1024
static HeapRun heapRuns[HEAP_RUNS];

// Free runs at least this large on both sides of a block make it a splitter.
#define SPLIT_FREE_BYTES (512u * 1024u)

// A block handed to a system service stays in the heap but the app may no
// longer touch it: socInit's buffer (the 3dslink log) is one. Reading it is a
// data abort on the console; emulators don't enforce it.
static bool isReadable(uintptr_t at, uint32_t size) {
    MemInfo info;
    PageInfo page;
    if (R_FAILED(svcQueryMemory(&info, &page, (u32) at))) return false;
    return (info.perm & MEMPERM_READ) != 0 && info.base_addr + info.size >= at + size;
}

static void logRun(const HeapRun* run, bool splits) {
    const char* mark = splits ? " splits" : "";
    uint32_t pcs[6] = {0};
    if (CtrHeapProfile_siteOf((const void*) run->first, pcs, 6)) {
        logInfo("[mem]   at %6.2f MB: %6lu B in %3lu blocks \"%s\"%s from %08lx %08lx %08lx %08lx %08lx %08lx\n", (double) run->offset / (1024.0 * 1024.0),
                (unsigned long) run->size, (unsigned long) run->blocks, run->head, mark, (unsigned long) pcs[0], (unsigned long) pcs[1],
                (unsigned long) pcs[2], (unsigned long) pcs[3], (unsigned long) pcs[4], (unsigned long) pcs[5]);
    } else {
        logInfo("[mem]   at %6.2f MB: %6lu B in %3lu blocks \"%s\"%s\n", (double) run->offset / (1024.0 * 1024.0), (unsigned long) run->size,
                (unsigned long) run->blocks, run->head, mark);
    }
}

// The block sizes CtrMemory_logHeapLayout asks about: how large the largest free piece would be if the blocks in use
// up to that size were somewhere else (a pool of their own).
static const uint32_t smallLimits[] = {64, 256, 1024, 4096, 16384};
#define SMALL_LIMITS (sizeof(smallLimits) / sizeof(smallLimits[0]))

void CtrMemory_logHeapLayout(const char* stage, int maxListed) {
    // Copied with malloc's lock held, logged after: logging allocates. The totals count the whole heap; the runs
    // are kept for the listing while there is room.
    __malloc_lock(_REENT);
    uintptr_t base = ((uintptr_t) __malloc_sbrk_base + 7) & ~(uintptr_t) 7;
    uintptr_t end = (uintptr_t) __malloc_sbrk_base + __malloc_current_mallinfo.arena;
    uint32_t runCount = 0;
    bool truncated = false;
    bool broken = (uintptr_t) __malloc_sbrk_base == UINTPTR_MAX;
    uint32_t usedBytes = 0, usedBlocks = 0, freeBytes = 0, freePieces = 0, largestFree = 0;
    uint32_t freeRun = 0;   // the free run being walked
    uint32_t smallBlocks[SMALL_LIMITS] = {0}, smallBytes[SMALL_LIMITS] = {0};
    uint32_t smallRun[SMALL_LIMITS] = {0}, smallLargest[SMALL_LIMITS] = {0};
    bool lastUsed = true;
    uintptr_t p = base;
    while (!broken && end > p + 16) {
        uint32_t size = ((const uint32_t*) p)[1] & ~3u;
        if (16 > size || p + size > end) {
            broken = true;
            break;
        }
        // The top chunk (the last) is free; any other is in use when the next says so.
        bool used = end > p + size + 8 && (((const uint32_t*) (p + size))[1] & 1u) != 0;
        if (used) {
            usedBytes += size;
            usedBlocks++;
            if (freeRun > largestFree) largestFree = freeRun;
            freeRun = 0;
        } else {
            freeBytes += size;
            if (lastUsed) freePieces++;
            freeRun += size;
        }
        lastUsed = used;
        for (uint32_t k = 0; SMALL_LIMITS > k; k++) {
            if (used && size > smallLimits[k]) {
                if (smallRun[k] > smallLargest[k]) smallLargest[k] = smallRun[k];
                smallRun[k] = 0;
            } else {
                smallRun[k] += size;
                if (used) {
                    smallBlocks[k]++;
                    smallBytes[k] += size;
                }
            }
        }
        HeapRun* run = runCount > 0 ? &heapRuns[runCount - 1] : nullptr;
        if (run != nullptr && run->used == used) {
            run->size += size;
            run->blocks++;
        } else if (HEAP_RUNS > runCount) {
            run = &heapRuns[runCount++];
            *run = (HeapRun) { (uint32_t) (p - base), size, 1, p + 8, {0}, used };
            if (used && isReadable(p + 8, 8)) {
                const char* data = (const char*) (p + 8);
                for (int i = 0; 8 > i && size > (uint32_t) i + 8; i++) run->head[i] = data[i] >= 32 && 127 > data[i] ? data[i] : '.';
            }
        } else {
            truncated = true;
        }
        p += size;
    }
    __malloc_unlock(_REENT);

    if (broken) {
        logWarn("[mem] %s: heap layout unreadable at offset %lu\n", stage, (unsigned long) (p - base));
        return;
    }
    // Past the top chunk, the heap sbrk never reached.
    uint32_t heapBytes = __ctru_heap_size - (uint32_t) (base - __ctru_heap);
    uint32_t beyond = heapBytes > (uint32_t) (end - base) ? heapBytes - (uint32_t) (end - base) : 0;
    if (freeRun + beyond > largestFree) largestFree = freeRun + beyond;
    logInfo("[mem] %s: heap layout: %lu KB in use in %lu blocks, %lu KB free in %lu pieces (+%lu KB never used), largest free %.2f MB%s\n",
            stage, (unsigned long) (usedBytes / 1024), (unsigned long) usedBlocks, (unsigned long) (freeBytes / 1024), (unsigned long) freePieces,
            (unsigned long) (beyond / 1024), (double) largestFree / (1024.0 * 1024.0), truncated ? " (list truncated)" : "");
    char text[SMALL_LIMITS * 48];
    int len = 0;
    for (uint32_t k = 0; SMALL_LIMITS > k; k++) {
        if (smallRun[k] + beyond > smallLargest[k]) smallLargest[k] = smallRun[k] + beyond;
        len += snprintf(text + len, sizeof(text) - (size_t) len, "%s %lu B: %lu blocks, %lu KB -> %.2f MB", k > 0 ? ";" : "",
                        (unsigned long) smallLimits[k], (unsigned long) smallBlocks[k], (unsigned long) (smallBytes[k] / 1024),
                        (double) smallLargest[k] / (1024.0 * 1024.0));
    }
    logInfo("[mem] %s: largest free without the blocks up to%s\n", stage, text);
    if (overflowCount > 0) {
        logInfo("[mem] %s: %lu allocations (%.1f MB in all) went to linear memory for want of heap\n", stage, (unsigned long) overflowCount,
                (double) overflowBytes / (1024.0 * 1024.0));
    }
    // Blocks with a large free run on both sides are what keeps the largest
    // free block small, wherever they are: they come first, marked "splits".
    // Then the rest, from the bottom of the heap.
    int listed = 0;
    for (int pass = 0; 2 > pass; pass++) {
        for (uint32_t i = 0; runCount > i && maxListed > listed; i++) {
            HeapRun* run = &heapRuns[i];
            if (!run->used) continue;
            uint32_t below = i > 0 ? heapRuns[i - 1].size : 0;
            uint32_t above = runCount > i + 1 ? heapRuns[i + 1].size + (runCount == i + 2 ? beyond : 0) : beyond;
            bool splits = below >= SPLIT_FREE_BYTES && above >= SPLIT_FREE_BYTES;
            if (splits != (pass == 0)) continue;
            logRun(run, splits);
            listed++;
        }
    }
}

// Every chunk of the heap, for tools/analysis/heapmap.py: one line per chunk in use ("u <offset> <size> <the
// call stack that allocated it, when the heap profiler tracks it>"), one per free run ("f <offset> <size>"). The
// chunks are copied with malloc's lock held into linear memory, and written after.
bool CtrMemory_writeHeapMap(const char* path, const char* label) {
    typedef struct {
        uint32_t offset;
        uint32_t size; // low bit: in use
    } MapChunk;
    const uint32_t capacity = 128u * 1024u;
    MapChunk* chunks = (MapChunk*) linearAlloc(capacity * sizeof(MapChunk));
    if (chunks == nullptr) return false;
    uint32_t count = 0;
    __malloc_lock(_REENT);
    uintptr_t base = ((uintptr_t) __malloc_sbrk_base + 7) & ~(uintptr_t) 7;
    uintptr_t end = (uintptr_t) __malloc_sbrk_base + __malloc_current_mallinfo.arena;
    bool broken = (uintptr_t) __malloc_sbrk_base == UINTPTR_MAX;
    for (uintptr_t p = base; !broken && end > p + 16 && capacity > count;) {
        uint32_t size = ((const uint32_t*) p)[1] & ~3u;
        if (16 > size || p + size > end) {
            broken = true;
            break;
        }
        bool used = end > p + size + 8 && (((const uint32_t*) (p + size))[1] & 1u) != 0;
        if (!used && count > 0 && (chunks[count - 1].size & 1u) == 0) {
            chunks[count - 1].size += size;
        } else {
            chunks[count++] = (MapChunk) { (uint32_t) (p - base), size | (used ? 1u : 0u) };
        }
        p += size;
    }
    __malloc_unlock(_REENT);
    FILE* f = broken ? nullptr : fopen(path, "w");
    if (f == nullptr) {
        linearFree(chunks);
        return false;
    }
    uint32_t heapBytes = __ctru_heap_size - (uint32_t) (base - __ctru_heap);
    fprintf(f, "heapmap %s: heap %lu B from %08lx, sbrk top at %lu, %lu chunks\n", label, (unsigned long) heapBytes, (unsigned long) base,
            (unsigned long) (end - base), (unsigned long) count);
    for (uint32_t i = 0; count > i; i++) {
        uint32_t size = chunks[i].size & ~1u;
        if ((chunks[i].size & 1u) == 0) {
            fprintf(f, "f %lu %lu\n", (unsigned long) chunks[i].offset, (unsigned long) size);
            continue;
        }
        uint32_t pcs[9] = {0};
        if (CtrHeapProfile_siteOf((const void*) (base + chunks[i].offset + 8), pcs, 9)) {
            fprintf(f, "u %lu %lu %lx %lx %lx %lx %lx %lx %lx %lx %lx\n", (unsigned long) chunks[i].offset, (unsigned long) size,
                    (unsigned long) pcs[0], (unsigned long) pcs[1], (unsigned long) pcs[2], (unsigned long) pcs[3], (unsigned long) pcs[4],
                    (unsigned long) pcs[5], (unsigned long) pcs[6], (unsigned long) pcs[7], (unsigned long) pcs[8]);
        } else {
            fprintf(f, "u %lu %lu\n", (unsigned long) chunks[i].offset, (unsigned long) size);
        }
    }
    fclose(f);
    linearFree(chunks);
    return true;
}

// Loading allocates and frees buffers of several MB (a data.win chunk at a
// time) before the textures want linear memory. From the heap they would leave
// holes that the game's small allocations only partly fill.
void* CtrMemory_allocTemporary(size_t size) {
    void* p = linearAlloc(size);
    return p != nullptr ? p : malloc(size);
}

// As CtrMemory_vramAllocNow, but at the top of the highest free gap that
// holds `size`: tiles' copies in VRAM on the Old models (ctr_renderer.c's
// "Tiles' copies in VRAM on the Old models"). libctru's allocator is first
// fit from the bottom, where surfaces go; copies put there leave holes
// between surfaces when they are given back (a 640x480 surface found 713 KB
// free, but in pieces). Here every free gap below the target is filled for a
// moment (blocks the GPU never sees, freed at once), so the copy lands where
// planned. Anything else (an address not the one planned, more blocks than
// these lists hold) gives nullptr: no copy, never a copy elsewhere. Late
// code, declared where it is used.
#define VRAM_TOP_BLOCKS_MAX 256
void* CtrMemory_vramAllocTop(size_t size) __attribute__((noinline, section(".text.late.contention")));
void* CtrMemory_vramAllocTop(size_t size) {
    static CtrVramBlock used[VRAM_TOP_BLOCKS_MAX] __attribute__((section(".bss.late.contention")));
    static void* fillers[VRAM_TOP_BLOCKS_MAX + 1] __attribute__((section(".bss.late.contention")));
    const uintptr_t lo = (uintptr_t) OS_VRAM_VADDR, hi = lo + (uintptr_t) OS_VRAM_SIZE;
    size = (size + 0x7Fu) & ~(size_t) 0x7Fu;
    lockLinear();
    releaseHeldFrees();
    // The blocks taken: those tracked, and the frees held back until the GPU is done.
    uint32_t n = 0;
    bool ok = !vramBlocksLost;
    for (uint32_t i = 0; vramBlockCount > i && ok; i++) {
        ok = VRAM_TOP_BLOCKS_MAX > n;
        if (ok) used[n++] = vramBlocks[i];
    }
    for (uint32_t i = 0; heldCount > i && ok; i++) {
        if (!heldFrees[i].vram) continue;
        ok = VRAM_TOP_BLOCKS_MAX > n;
        if (ok) used[n++] = (CtrVramBlock) { (uintptr_t) heldFrees[i].mem, heldFrees[i].bytes };
    }
    for (uint32_t i = 1; ok && n > i; i++) {
        CtrVramBlock b = used[i];
        uint32_t j = i;
        for (; j > 0 && used[j - 1].addr > b.addr; j--) used[j] = used[j - 1];
        used[j] = b;
    }
    // The highest gap that fits: from the end of block k-1 (or lo) to block k (or hi).
    uintptr_t gapLo = 0, target = 0;
    for (int32_t k = (int32_t) n; ok && k >= 0; k--) {
        uintptr_t start = k > 0 ? used[k - 1].addr + used[k - 1].size : lo;
        uintptr_t end = k < (int32_t) n ? used[k].addr : hi;
        start = (start + 0x7Fu) & ~(uintptr_t) 0x7Fu;
        if (end > start && end - start >= size) {
            gapLo = start;
            target = (end - size) & ~(uintptr_t) 0x7Fu;
            break;
        }
    }
    void* p = nullptr;
    uint32_t f = 0;
    ok = ok && target >= gapLo && target != 0;
    // Fill every gap below the target, in address order, and the target gap's bottom.
    for (uint32_t k = 0; ok && n >= k; k++) {
        uintptr_t start = k > 0 ? used[k - 1].addr + used[k - 1].size : lo;
        start = (start + 0x7Fu) & ~(uintptr_t) 0x7Fu;
        if (start >= target) break;
        uintptr_t end = k < n ? used[k].addr : hi;
        if (start == gapLo) end = target;
        if (end <= start) continue;
        void* q = __real_vramAlloc((size_t) (end - start));
        if (q != nullptr) fillers[f++] = q;
        ok = q == (void*) start;
    }
    if (ok) {
        p = __real_vramAlloc(size);
        if (p != (void*) target) {
            if (p != nullptr) __real_vramFree(p);
            p = nullptr;
        }
    }
    for (uint32_t i = 0; f > i; i++) __real_vramFree(fillers[i]);
    // As trackVram, which stays as it was.
    if (p != nullptr && VRAM_BLOCKS_MAX > vramBlockCount) {
        vramBlocks[vramBlockCount++] = (CtrVramBlock) { (uintptr_t) p, (uint32_t) vramGetSize(p) };
    } else if (p != nullptr) {
        __real_vramFree(p);
        p = nullptr;
    }
    // Not LightLock_Unlock: the release also gives back the caller's heap relief, which taking the lock turned off
    // (ctr_memory_lock.c). A bare unlock left relief and linear overflow off on the game thread for good (oldoom).
    CtrMemory_linearLockRelease(&linearLock);
    return p;
}

// CIA80 packaging has its own ELF: do not redirect the ordinary four targets.
// libctru initSystem calls this once, before main. --wrap changes that branch;
// --undefined retains the original allocator's pinned slot and body.
#ifdef CTRPORT_DR_OLD_CIA80_TILES
#if !defined(ENABLE_WAD17) || !defined(CTRPORT_TARGET_O3DS)
#error "CIA80 tile split is only for DELTARUNE Old"
#endif
#define CTRPORT_ALLOCATE_CIA80
void __attribute__((noipa, section(".text.late.flowervramg_cia80"))) __wrap___system_allocateHeaps(void) {
#include "ctr_allocate_heaps.inc"
}
#undef CTRPORT_ALLOCATE_CIA80
#endif
