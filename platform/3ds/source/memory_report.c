// The heap figures CtrMemory_logHeapLayout leaves out, the out-of-memory snapshot, and the log writer's number caches
// warmed at its start (heapfrag).
//
// The link wraps CtrMemory_logHeapLayout, CtrMemory_emergencyFree and CtrLab_runThread (platform/3ds/CMakeLists.txt),
// so ctr_memory.c and its callers keep main's code. The heap report is in the late .heapfrag section, outside the warm
// text; the number-cache warm-up came after the late reserve (platform/3ds/late_reserve.ld), so it is in heapfrag's
// .text.late.heapfrag, .rodata.late.heapfrag and .bss.late.heapfrag instead, and .heapfrag stays frozen. The
// linker creates that orphan output section where it first meets it in link order, and the platform sources link in
// name order: this file is named to sort after main.c, the first object with .vidfix, so .heapfrag lands after .vidfix
// and no other function moves. The text segment ends
// a page before .rodata, and on the New 3DS only 380 B of that page is left with this file: keep it small.
//
// newlib's malloc is dlmalloc 2.6: chunks lie end to end from where sbrk started, each headed by its predecessor's size
// and its own, whose low bit says whether the chunk before it is in use. The last, the top chunk, is free. When no
// free chunk fits, malloc_extend_top asks sbrk for the whole request (installed libc.a, _malloc_r+0x258..0x2c0), never
// for the shortfall, so the top chunk and the heap sbrk never reached don't combine into one allocation. The layout
// line's "largest free" adds them, so it overstates whenever the top chunk is the largest piece
// (docs/3ds-hardware.md, "A logged 'largest free' overstates...").

#include <3ds.h>
#include <malloc.h>
#include <reent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "log.h"

#include "ctr_memory.h"

// The log formats go in their own read-only orphan section too, which the linker places after .rodata, so no rodata
// symbol moves either (string literals would join .rodata in link order).
#if defined(__3DS__) && defined(__GNUC__)
#define LATE __attribute__((noinline, section(".heapfrag")))
#define LATE_TEXT __attribute__((section(".heapfrag_rodata")))
// Added after the late reserve: heapfrag's sections in late_reserve.ld (.heapfrag and .heapfrag_rodata are frozen).
#define HEAPFRAG_LATE __attribute__((noinline, section(".text.late.heapfrag")))
#define HEAPFRAG_LATE_RODATA __attribute__((section(".rodata.late.heapfrag")))
#define HEAPFRAG_LATE_BSS __attribute__((section(".bss.late.heapfrag")))
#else
#define LATE
#define LATE_TEXT
#define HEAPFRAG_LATE
#define HEAPFRAG_LATE_RODATA
#define HEAPFRAG_LATE_BSS
#endif

// ctr_memory.c's statics, under the global names its aliases give them.
extern LightLock CtrMemory_linearLockStore;
extern uint32_t CtrMemory_heldLinearStore;

extern u32 __ctru_heap;
extern u32 __ctru_heap_size;
extern char* __malloc_sbrk_base;
extern struct mallinfo __malloc_current_mallinfo;
// dlmalloc's bins; the third is the top chunk.
extern void* __malloc_av_[];
void __malloc_lock(struct _reent* reent);
void __malloc_unlock(struct _reent* reent);
void* __real_linearMemAlign(size_t size, size_t alignment);
void __real_linearFree(void* mem);
u32 __real_linearSpaceFree(void);

void __real_CtrMemory_logHeapLayout(const char* stage, int maxListed);
void __real_CtrMemory_emergencyFree(int32_t* heapCached, int32_t* linear, int32_t* vram);
void __wrap_CtrMemory_logHeapLayout(const char* stage, int maxListed);
#ifdef CTRPORT_HEAP_PROFILE
static void logWarmCheck(const char* stage);
#endif
void __wrap_CtrMemory_emergencyFree(int32_t* heapCached, int32_t* linear, int32_t* vram);

#define MIN_CHUNK 16u // dlmalloc's MINSIZE
#define PAGE 4096u
#define LIMITS 5
#define MB(bytes) ((double) (bytes) / (1024.0 * 1024.0))

// The heap when an allocation on the game thread failed, read as the out-of-memory screen starts: before it sends its
// summary, stops the watchdog (up to a second) or walks the heap. It lives in the first bytes of ctr_memory.c's run
// list, so this file adds no data and no data symbol moves. The real report writes that list only after the wrapper has
// read and cleared the snapshot, and the list's first run always starts at offset 0, so a stale list never reads as one.
typedef struct {
    uint32_t magic; // SNAPSHOT while one is held
    uint32_t tick;  // the system tick's low word
    uint32_t wanted;
    uint32_t top;    // the top chunk
    uint32_t beyond; // the heap sbrk never reached
} Snapshot;
#define SNAPSHOT 0x214d4f4fu // "OOM!"
extern Snapshot CtrMemory_heapRunStore;
#define failure CtrMemory_heapRunStore

// The heap past the end sbrk has reached.
LATE static uint32_t neverUsed(uintptr_t base, uintptr_t end) {
    uint32_t heapBytes = __ctru_heap_size - (uint32_t) (base - __ctru_heap);
    return heapBytes > (uint32_t) (end - base) ? heapBytes - (uint32_t) (end - base) : 0;
}

// The largest chunk malloc can hand out: the largest free chunk inside the heap, the top chunk less MINSIZE, or what
// sbrk can still give (whole pages) less MINSIZE. tools/analysis/heapfrag_censusmap.py's allocatable() is the same.
LATE static uint32_t mallocable(uint32_t interior, uint32_t top, uint32_t beyond) {
    uint32_t best = interior;
    if (top > MIN_CHUNK && top - MIN_CHUNK > best) best = top - MIN_CHUNK;
    uint32_t pages = beyond & ~(PAGE - 1);
    if (pages > MIN_CHUNK && pages - MIN_CHUNK > best) best = pages - MIN_CHUNK;
    return best;
}

// Still no allocation, heap walk or blocking lock (ctr_memory.h). After an allocation failure on the game thread it
// first records the top chunk and the heap sbrk never reached: unlocked word reads, which another thread's malloc
// may make a moment stale.
LATE void __wrap_CtrMemory_emergencyFree(int32_t* heapCached, int32_t* linear, int32_t* vram) {
    size_t wanted = CtrMemory_failedAllocation();
    if (wanted != 0 && failure.magic != SNAPSHOT && (uintptr_t) __malloc_sbrk_base != UINTPTR_MAX) {
        uintptr_t base = ((uintptr_t) __malloc_sbrk_base + 7) & ~(uintptr_t) 7;
        uintptr_t end = (uintptr_t) __malloc_sbrk_base + __malloc_current_mallinfo.arena;
        uintptr_t top = (uintptr_t) __malloc_av_[2];
        failure.tick = (uint32_t) svcGetSystemTick();
        failure.wanted = (uint32_t) wanted;
        failure.top = top >= base && end >= top + 8 ? ((const uint32_t*) top)[1] & ~3u : 0;
        failure.beyond = neverUsed(base, end);
        failure.magic = SNAPSHOT;
    }
    __real_CtrMemory_emergencyFree(heapCached, linear, vram);
}

// The largest piece of linear memory libctru can hand out now (0x80-aligned, as linearAlloc asks), by halving: an
// allocation and a free per step under the linear lock, each freed at once and never seen by the GPU. Frees held back
// for the GPU don't count. libctru's tree nodes come from ctr_memory.c's node pool (or the heap: with none left there,
// a step fails and the result is low). Probes nothing and returns false if the lock stays taken for 50 ms: the
// out-of-memory screen may come from a thread that holds it.
LATE static bool largestLinear(uint32_t* largest, uint32_t* total) {
    for (int tries = 0; LightLock_TryLock(&CtrMemory_linearLockStore) != 0; tries++) {
        if (tries == 50) return false;
        svcSleepThread(1000000);
    }
    // A failed map-node malloc inside libctru must return NULL, not try heap relief or linear overflow: overflow takes
    // the linear lock again.
    bool allowed = CtrMemory_allowRelief(false);
    *total = __real_linearSpaceFree();
    uint32_t fits = 0, fails = (*total & ~0x7fu) + 0x80;
    while (fails - fits > 0x80) {
        uint32_t size = ((fits + fails) / 2) & ~0x7fu;
        if (size <= fits) size = fits + 0x80;
        void* p = __real_linearMemAlign(size, 0x80);
        if (p != nullptr) {
            __real_linearFree(p);
            fits = size;
        } else {
            fails = size;
        }
    }
    CtrMemory_allowRelief(allowed);
    LightLock_Unlock(&CtrMemory_linearLockStore);
    *largest = fits;
    return true;
}

LATE_TEXT static const char mallocableLine[] =
    "[mem] %s: malloc-able: largest %.2f MB (interior %.2f, top chunk %.2f); without the blocks up to 64 B %.2f, "
    "256 B %.2f, 1024 B %.2f, 4096 B %.2f, 16384 B %.2f MB\n";
LATE_TEXT static const char linearLine[] =
    "[mem] %s: linear memory: largest piece %.2f MB of %.2f MB free (+%.2f MB held for the GPU)\n";
LATE_TEXT static const char linearBusyLine[] = "[mem] %s: linear memory: lock busy for 50 ms, not probed\n";
LATE_TEXT static const char failureLine[] =
    "[mem] %s: at the failure, %lu ms earlier: %lu B wanted; top chunk %lu B, %lu KB never used: at most %lu B "
    "malloc-able beside the interior (unlocked reads)\n";

// Before ctr_memory.c's layout lines: the largest block malloc can hand out, and the same for its what-ifs (the blocks
// up to 64, 256, 1024, 4096 and 16384 B somewhere else; the run that ends the heap is then the top chunk). Then the
// largest linear piece and, after an allocation failure, the snapshot.
LATE void __wrap_CtrMemory_logHeapLayout(const char* stage, int maxListed) {
    uint32_t interior[LIMITS + 1] = {0}, run[LIMITS + 1] = {0};
    // Copied with malloc's lock held, logged after: logging allocates.
    __malloc_lock(_REENT);
    uintptr_t base = ((uintptr_t) __malloc_sbrk_base + 7) & ~(uintptr_t) 7;
    uintptr_t end = (uintptr_t) __malloc_sbrk_base + __malloc_current_mallinfo.arena;
    bool broken = (uintptr_t) __malloc_sbrk_base == UINTPTR_MAX;
    // A top chunk is at least MINSIZE; ctr_memory.c's walk stops short of one that small.
    for (uintptr_t p = base; !broken && end >= p + MIN_CHUNK;) {
        uint32_t size = ((const uint32_t*) p)[1] & ~3u;
        if (16 > size || p + size > end) {
            broken = true;
            break;
        }
        // The top chunk (the last) is free; any other is in use when the next says so.
        bool used = end > p + size + 8 && (((const uint32_t*) (p + size))[1] & 1u) != 0;
        // k = 0: the heap as it is; then without the blocks up to 64 << 2(k - 1) B.
        for (uint32_t k = 0; LIMITS >= k; k++) {
            if (used && (k == 0 || size > 64u << (2 * (k - 1)))) {
                if (run[k] > interior[k]) interior[k] = run[k];
                run[k] = 0;
            } else {
                run[k] += size;
            }
        }
        p += size;
    }
    __malloc_unlock(_REENT);
    if (!broken) {
        uint32_t beyond = neverUsed(base, end);
        double m[LIMITS + 1];
        for (uint32_t k = 0; LIMITS >= k; k++) m[k] = MB(mallocable(interior[k], run[k], beyond));
        logInfo(mallocableLine, stage, m[0], MB(interior[0]), MB(run[0]), m[1], m[2], m[3], m[4], m[5]);
    }
    uint32_t linearLargest, linearTotal;
    if (largestLinear(&linearLargest, &linearTotal)) {
        logInfo(linearLine, stage, MB(linearLargest), MB(linearTotal), MB(CtrMemory_heldLinearStore));
    } else {
        logInfo(linearBusyLine, stage);
    }
    if (failure.magic == SNAPSHOT) {
        failure.magic = 0;
        uint32_t ms = ((uint32_t) svcGetSystemTick() - failure.tick) / (SYSCLOCK_ARM11 / 1000); // wraps after 16 s: fine
        logInfo(failureLine, stage, (unsigned long) ms, (unsigned long) failure.wanted,
                (unsigned long) failure.top, (unsigned long) (failure.beyond / 1024), (unsigned long) mallocable(0, failure.top, failure.beyond));
    }
#ifdef CTRPORT_HEAP_PROFILE
    logWarmCheck(stage);
#endif
    __real_CtrMemory_logHeapLayout(stage, maxListed);
}

// ===[ The log writer's number caches ]===
// newlib's number conversion (printf's %f and %g: _dtoa_r) takes its Bigints from the calling thread's struct _reent
// and keeps them there for the thread's life: a freelist by size, the cached powers of 5, the last result. The log
// writer formats every line's floats, and lives as long as the game, so the first line that needed a larger Bigint
// left it wherever malloc found room then. In the census, two 40 B ones born in the middle of chapter 3 split the heap
// freed at the chapter's unload. Formatting a few
// demanding numbers on the writer before it takes its first line allocates them while the heap is nearly empty, low
// down, and later lines reuse them. The text is thrown away. A cache never changes a formatted digit: _Balloc hands out
// a cached Bigint with the same k, maxwds, sign = 0 and wds = 0 as a new one, and the arithmetic reads only the words
// it wrote; the cached powers of 5 are exact.
extern void (*const CtrLog_writerEntry)(void*);
void __real_CtrLab_runThread(void (*entry)(void*), void* arg);
void __wrap_CtrLab_runThread(void (*entry)(void*), void* arg);

// Magnitudes from 1e-35 to 1e24 at 17 digits: Bigints up to k = 3, and the powers of 5 up to 5^32.
HEAPFRAG_LATE_RODATA static const char warmFormat[] = "%.17g %.17g %.17g %.17g";
#define WARM_ARGUMENTS 987654321.98765432, 1.2345678901234567e-6, 6.02214076e23, 1.6e-35
#ifdef CTRPORT_HEAP_PROFILE
// Heap builds format the same numbers again with the caches warm and say at the first layout report whether the text
// matched (1) or not (2).
HEAPFRAG_LATE_BSS static int warmChecked;
HEAPFRAG_LATE_BSS static char warmText[112];
HEAPFRAG_LATE_RODATA static const char warmLine[] =
    "[mem] %s: log writer's number caches warmed at its start; the same text cold and warm: %s (%s)\n";
#endif

// Every thread starts here (ctr_memory.c's threadTrampoline); only the log writer warms.
HEAPFRAG_LATE void __wrap_CtrLab_runThread(void (*entry)(void*), void* arg) {
    if (entry == CtrLog_writerEntry) {
        char cold[112];
        snprintf(cold, sizeof(cold), warmFormat, WARM_ARGUMENTS);
#ifdef CTRPORT_HEAP_PROFILE
        char warm[112];
        snprintf(warm, sizeof(warm), warmFormat, WARM_ARGUMENTS);
        warmChecked = strcmp(cold, warm) == 0 ? 1 : 2;
        memcpy(warmText, cold, sizeof(warmText));
#endif
    }
    __real_CtrLab_runThread(entry, arg);
}

#ifdef CTRPORT_HEAP_PROFILE
HEAPFRAG_LATE static void logWarmCheck(const char* stage) {
    if (warmChecked == 0) return;
    logInfo(warmLine, stage, warmChecked == 1 ? "equal" : "DIFFERENT", warmText);
    warmChecked = 0;
}
#endif
