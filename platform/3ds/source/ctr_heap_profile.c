// Heap profiler: attributes live heap bytes to call stacks.
//
// Only compiled in with -DCTRPORT_HEAP_PROFILE=ON, which links malloc, calloc, realloc and free through the
// __wrap_ functions below and builds with unwind tables so each allocation's call stack can be walked. The tables
// live in linear memory so the heap being measured keeps its usual layout. CtrHeapProfile_dump logs the call
// stacks holding the most bytes; turn the addresses into names with arm-none-eabi-addr2line -f -e <the .elf>.

#include "ctr_heap_profile.h"

#ifdef CTRPORT_HEAP_PROFILE

#include <3ds.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unwind.h>

#include "common.h"
#include "ctr_memory.h"
#include "log.h"

extern u32 __ctru_heap;
extern u32 __ctru_heap_size;

// Frames kept per call stack, after the profiler's own.
#define SITE_DEPTH 9
// Frames skipped at the top of every stack (the symbolizer drops the profiler's own).
#define SKIP_FRAMES 0
#define SITE_CAPACITY CTR_HEAP_PROFILE_SITES
#define BLOCK_CAPACITY CTR_HEAP_PROFILE_BLOCKS

typedef struct {
    uint32_t pc[SITE_DEPTH];
    uint32_t liveBytes;
    uint32_t liveCount;
    uint32_t peakBytes;
} Site;
_Static_assert(sizeof(Site) == CTR_HEAP_PROFILE_SITE_BYTES, "CTR_HEAP_PROFILE_TABLE_BYTES counts on this size");

typedef struct {
    uintptr_t ptr; // 0 = empty
    uint32_t site; // low 16 bits: the site; high 16: the room serial it was allocated in (census)
} Block;
_Static_assert(CTR_HEAP_PROFILE_SITES <= 65536u, "sites must fit the low 16 bits of Block.site");
#define BLOCK_SITE(b) ((b).site & 0xFFFFu)
#define BLOCK_BIRTH(b) ((b).site >> 16)

// ===[ Fragmentation census ]===

// Rooms entered since boot (wraps at 65536), and the size classes and ages (in rooms) the census counts in.
static uint32_t roomSerial;
static const uint32_t censusSizes[] = {16, 32, 64, 128, 256, 512, 1024, 4096, 16384, 65536, 262144, UINT32_MAX};
#define CENSUS_SIZES (sizeof(censusSizes) / sizeof(censusSizes[0]))
// Ages: same room, 1 room, 2-4, 5-19, 20 or more.
#define CENSUS_AGES 5
static uint32_t freedCount[CENSUS_SIZES][CENSUS_AGES], freedBytes[CENSUS_SIZES][CENSUS_AGES];

static int censusSize(uint32_t bytes) {
    int b = 0;
    while (bytes > censusSizes[b]) b++;
    return b;
}

static int censusAge(uint32_t birth) {
    uint32_t age = (roomSerial - birth) & 0xFFFFu;
    return age == 0 ? 0 : age == 1 ? 1 : 5 > age ? 2 : 20 > age ? 3 : 4;
}

static Site* sites;
static uint32_t* siteIndex; // hash of stack -> site + 1, 0 = empty
static uint32_t siteCount;
static Block* blocks;
static uint32_t blockCount;
static uint32_t droppedCount;
// Records found still live when their address was allocated again (see recordAlloc).
static uint32_t staleCount;
static bool ready;
static LightLock lock;

typedef struct {
    uint32_t pc[SITE_DEPTH + SKIP_FRAMES];
    int count;
} Stack;

static _Unwind_Reason_Code onFrame(struct _Unwind_Context* context, void* arg) {
    Stack* stack = (Stack*) arg;
    if (stack->count >= SITE_DEPTH + SKIP_FRAMES) return _URC_END_OF_STACK;
    stack->pc[stack->count++] = (uint32_t) _Unwind_GetIP(context);
    return _URC_NO_REASON;
}

static uint32_t hashWord(uint32_t h, uint32_t v) {
    h ^= v;
    h *= 0x9E3779B1u;
    return h ^ (h >> 15);
}

static __attribute__((noinline)) void captureStack(Stack* stack) {
    stack->count = 0;
    _Unwind_Backtrace(onFrame, stack);
}

// With the lock held.
static uint32_t findSite(const Stack* stack) {
    uint32_t key[SITE_DEPTH] = {0};
    for (int i = SKIP_FRAMES; stack->count > i; i++) key[i - SKIP_FRAMES] = stack->pc[i];
    uint32_t h = 0x12345678u;
    for (int i = 0; SITE_DEPTH > i; i++) h = hashWord(h, key[i]);
    uint32_t slot = h & (SITE_CAPACITY * 2 - 1);
    while (siteIndex[slot] != 0) {
        Site* s = &sites[siteIndex[slot] - 1];
        if (memcmp(s->pc, key, sizeof(key)) == 0) return siteIndex[slot] - 1;
        slot = (slot + 1) & (SITE_CAPACITY * 2 - 1);
    }
    if (siteCount >= SITE_CAPACITY) return UINT32_MAX;
    Site* s = &sites[siteCount];
    memset(s, 0, sizeof(*s));
    memcpy(s->pc, key, sizeof(key));
    siteIndex[slot] = ++siteCount;
    return siteCount - 1;
}

static uint32_t blockSlot(uintptr_t ptr) {
    return (uint32_t) ((ptr >> 3) * 0x9E3779B1u) & (BLOCK_CAPACITY - 1);
}

// With the lock held: the slot holding `ptr`'s record, or UINT32_MAX.
static uint32_t findBlock(uintptr_t ptr) {
    uint32_t slot = blockSlot(ptr);
    while (blocks[slot].ptr != 0 && blocks[slot].ptr != ptr) slot = (slot + 1) & (BLOCK_CAPACITY - 1);
    return blocks[slot].ptr != 0 ? slot : UINT32_MAX;
}

// With the lock held: takes the record in `slot` off the table, `bytes` being its block's usable size. A block freed
// counts in the census's freed table; a stale record (its block was freed out of sight) doesn't.
static void dropBlock(uint32_t slot, uint32_t bytes, bool freed) {
    Site* s = &sites[BLOCK_SITE(blocks[slot])];
    s->liveBytes -= bytes;
    s->liveCount--;
    blockCount--;
    if (freed) {
        int sc = censusSize(bytes), ag = censusAge(BLOCK_BIRTH(blocks[slot]));
        freedCount[sc][ag]++;
        freedBytes[sc][ag] += bytes;
    }
    // Backward-shift deletion keeps the linear probe chains intact without tombstones.
    uint32_t hole = slot;
    uint32_t next = (hole + 1) & (BLOCK_CAPACITY - 1);
    while (blocks[next].ptr != 0) {
        uint32_t home = blockSlot(blocks[next].ptr);
        bool movable = (next > hole) ? (home <= hole || home > next) : (home <= hole && home > next);
        if (movable) {
            blocks[hole] = blocks[next];
            hole = next;
        }
        next = (next + 1) & (BLOCK_CAPACITY - 1);
    }
    blocks[hole].ptr = 0;
}

// `caller` is the allocating call's return address. The unwinder stops at
// code built without unwind tables (newlib, libctru, citro3d), so it takes
// the place of the first frame, which is always this function. One live record
// per block: an older record at the same address belongs to a block freed out
// of sight, and goes (counted in staleCount). A nonzero `tag` goes after the
// call stack: the _r wrappers pass the struct _reent* newlib called them with,
// which names the thread whose caches (number conversion, stdio) the block
// belongs to. libctru keeps a thread's struct _reent inside its threadCreate
// block; the main thread's is newlib's static impure_data.
static __attribute__((noinline)) void recordAllocTagged(void* ptr, void* caller, uint32_t tag) {
    if (!ready || !CtrMemory_isHeap(ptr)) return;
    Stack stack;
    captureStack(&stack);
    if (stack.count > 0) stack.pc[0] = (uint32_t) (uintptr_t) caller;
    const int depth = (int) (sizeof(stack.pc) / sizeof(stack.pc[0]));
    if (tag != 0) stack.pc[depth > stack.count ? stack.count++ : depth - 1] = tag;
    uint32_t bytes = (uint32_t) malloc_usable_size(ptr);
    LightLock_Lock(&lock);
    uint32_t old = findBlock((uintptr_t) ptr);
    if (old != UINT32_MAX) {
        dropBlock(old, bytes, false);
        staleCount++;
    }
    uint32_t site = findSite(&stack);
    if (site == UINT32_MAX || blockCount >= BLOCK_CAPACITY - BLOCK_CAPACITY / 8) {
        droppedCount++;
    } else {
        uint32_t slot = blockSlot((uintptr_t) ptr);
        while (blocks[slot].ptr != 0) slot = (slot + 1) & (BLOCK_CAPACITY - 1);
        blocks[slot].ptr = (uintptr_t) ptr;
        blocks[slot].site = site | ((roomSerial & 0xFFFFu) << 16);
        blockCount++;
        Site* s = &sites[site];
        s->liveBytes += bytes;
        s->liveCount++;
        if (s->liveBytes > s->peakBytes) s->peakBytes = s->liveBytes;
    }
    LightLock_Unlock(&lock);
}

static void recordAlloc(void* ptr, void* caller) {
    recordAllocTagged(ptr, caller, 0);
}

// Before the block is freed, while its usable size can still be read.
static void recordFree(void* ptr) {
    if (!ready || !CtrMemory_isHeap(ptr)) return;
    uint32_t bytes = (uint32_t) malloc_usable_size(ptr);
    LightLock_Lock(&lock);
    uint32_t slot = findBlock((uintptr_t) ptr);
    if (slot != UINT32_MAX) dropBlock(slot, bytes, true);
    LightLock_Unlock(&lock);
}

// The usable size of a heap block about to be reallocated (0 for others), read before the call frees or resizes it.
static uint32_t heapBytes(void* ptr) {
    return ready && CtrMemory_isHeap(ptr) ? (uint32_t) malloc_usable_size(ptr) : 0;
}

// After realloc(old, size) returned `p`; `oldBytes` from heapBytes before the call. A failed realloc leaves the old
// record as it was. One that kept its address keeps its record, birth room and call stack included, with its new
// size. One that moved frees the old block and records the new one, born now under this call stack.
static void recordRealloc(void* old, uint32_t oldBytes, void* p, size_t size, void* caller) {
    if (!ready) return;
    if (p == nullptr) {
        if (size != 0 || old == nullptr || !CtrMemory_isHeap(old)) return;
        // realloc(old, 0) freed it.
        LightLock_Lock(&lock);
        uint32_t slot = findBlock((uintptr_t) old);
        if (slot != UINT32_MAX) dropBlock(slot, oldBytes, true);
        LightLock_Unlock(&lock);
        return;
    }
    if (p == old) {
        if (!CtrMemory_isHeap(p)) return;
        uint32_t bytes = (uint32_t) malloc_usable_size(p);
        LightLock_Lock(&lock);
        uint32_t slot = findBlock((uintptr_t) p);
        if (slot != UINT32_MAX) {
            Site* s = &sites[BLOCK_SITE(blocks[slot])];
            s->liveBytes = s->liveBytes - oldBytes + bytes;
            if (s->liveBytes > s->peakBytes) s->peakBytes = s->liveBytes;
        }
        LightLock_Unlock(&lock);
        return;
    }
    if (old != nullptr && CtrMemory_isHeap(old)) {
        LightLock_Lock(&lock);
        uint32_t slot = findBlock((uintptr_t) old);
        if (slot != UINT32_MAX) dropBlock(slot, oldBytes, true);
        LightLock_Unlock(&lock);
    }
    recordAlloc(p, caller);
}

// The allocations themselves go through ctr_memory.c (the reliever, linear
// memory when the heap has no room, libctru's allocator nodes); only blocks in
// the heap are recorded.
//
// newlib's own functions call each other: calloc's _calloc_r calls _malloc_r, _realloc_r calls _malloc_r (realloc
// of NULL, a move) and _free_r, and free calls _free_r. The link wraps the _r functions too (CMakeLists.txt), so
// newlib's direct uses (number conversion caches, stdio buffers) are recorded. inWrapper is set while any wrapper
// below runs; a _r wrapper reached inside another wrapper is part of that call and records nothing. Public malloc,
// calloc, realloc and free nested inside a wrapper (libctru's allocator nodes, malloc'd while linear memory is
// allocated) are separate blocks, and record as usual.
static __thread int inWrapper;

void* __wrap_malloc(size_t size) {
    inWrapper++;
    void* p = CtrMemory_malloc(size, __builtin_return_address(0));
    inWrapper--;
    recordAlloc(p, __builtin_return_address(0));
    return p;
}

void* __wrap_calloc(size_t count, size_t size) {
    inWrapper++;
    void* p = CtrMemory_calloc(count, size);
    inWrapper--;
    recordAlloc(p, __builtin_return_address(0));
    return p;
}

void* __wrap_realloc(void* ptr, size_t size) {
    uint32_t oldBytes = heapBytes(ptr);
    inWrapper++;
    void* p = CtrMemory_realloc(ptr, size);
    inWrapper--;
    recordRealloc(ptr, oldBytes, p, size, __builtin_return_address(0));
    return p;
}

void __wrap_free(void* ptr) {
    recordFree(ptr);
    inWrapper++;
    CtrMemory_free(ptr);
    inWrapper--;
}

char* __real_strdup(const char* s);
char* __real_strndup(const char* s, size_t n);
char* __wrap_strdup(const char* s);
char* __wrap_strndup(const char* s, size_t n);

void __wrap__free_r(struct _reent* reent, void* ptr);
void __wrap__free_r(struct _reent* reent, void* ptr) {
    if (inWrapper != 0) {
        CtrMemory_freeR(reent, ptr);
        return;
    }
    recordFree(ptr);
    inWrapper++;
    CtrMemory_freeR(reent, ptr);
    inWrapper--;
}

void* __real__malloc_r(struct _reent* reent, size_t size);
void* __real__calloc_r(struct _reent* reent, size_t count, size_t size);
void* __real__realloc_r(struct _reent* reent, void* ptr, size_t size);
void* __wrap__malloc_r(struct _reent* reent, size_t size);
void* __wrap__calloc_r(struct _reent* reent, size_t count, size_t size);
void* __wrap__realloc_r(struct _reent* reent, void* ptr, size_t size);

void* __wrap__malloc_r(struct _reent* reent, size_t size) {
    if (inWrapper != 0) return __real__malloc_r(reent, size);
    inWrapper++;
    void* p = __real__malloc_r(reent, size);
    inWrapper--;
    recordAllocTagged(p, __builtin_return_address(0), (uint32_t) (uintptr_t) reent);
    return p;
}

void* __wrap__calloc_r(struct _reent* reent, size_t count, size_t size) {
    if (inWrapper != 0) return __real__calloc_r(reent, count, size);
    inWrapper++;
    void* p = __real__calloc_r(reent, count, size);
    inWrapper--;
    recordAllocTagged(p, __builtin_return_address(0), (uint32_t) (uintptr_t) reent);
    return p;
}

void* __wrap__realloc_r(struct _reent* reent, void* ptr, size_t size) {
    if (inWrapper != 0) return __real__realloc_r(reent, ptr, size);
    uint32_t oldBytes = heapBytes(ptr);
    inWrapper++;
    void* p = __real__realloc_r(reent, ptr, size);
    inWrapper--;
    recordRealloc(ptr, oldBytes, p, size, __builtin_return_address(0));
    return p;
}

char* __wrap_strdup(const char* s) {
    inWrapper++;
    char* p = __real_strdup(s);
    inWrapper--;
    recordAlloc(p, __builtin_return_address(0));
    return p;
}

char* __wrap_strndup(const char* s, size_t n) {
    inWrapper++;
    char* p = __real_strndup(s, n);
    inWrapper--;
    recordAlloc(p, __builtin_return_address(0));
    return p;
}

void CtrHeapProfile_init(void) {
    LightLock_Init(&lock);
    sites = (Site*) linearAlloc(SITE_CAPACITY * sizeof(Site));
    siteIndex = (uint32_t*) linearAlloc(SITE_CAPACITY * 2 * sizeof(uint32_t));
    blocks = (Block*) linearAlloc(BLOCK_CAPACITY * sizeof(Block));
    if (sites == nullptr || siteIndex == nullptr || blocks == nullptr) {
        logInfo("[heap] profiler: no linear memory for its tables\n");
        return;
    }
    memset(siteIndex, 0, SITE_CAPACITY * 2 * sizeof(uint32_t));
    memset(blocks, 0, BLOCK_CAPACITY * sizeof(Block));
    ready = true;
    logInfo("[heap] profiler on: %u KB of linear memory for its tables\n",
            (unsigned) ((SITE_CAPACITY * (sizeof(Site) + 8) + BLOCK_CAPACITY * sizeof(Block)) / 1024));
}

// Blocks this close together count as one island.
#define ISLAND_GAP (64u * 1024u)
#define LAYOUT_MAX_BLOCKS 8192u

static int compareAddresses(const void* a, const void* b) {
    uintptr_t x = ((const Block*) a)->ptr, y = ((const Block*) b)->ptr;
    return x < y ? -1 : x > y ? 1 : 0;
}

// Where the live blocks sit, when there are few of them (between games): the
// islands of blocks that split the free space, and the largest piece of it,
// which a big allocation (a data.win chunk) needs whole.
static void dumpLayout(const char* label) {
    static Block sorted[LAYOUT_MAX_BLOCKS];
    LightLock_Lock(&lock);
    uint32_t n = 0;
    uintptr_t heapStart = __ctru_heap, heapEnd = __ctru_heap + __ctru_heap_size;
    if (LAYOUT_MAX_BLOCKS >= blockCount) {
        // Only the heap's: work buffers may have come from linear memory.
        for (uint32_t i = 0; BLOCK_CAPACITY > i; i++) {
            if (blocks[i].ptr >= heapStart && heapEnd > blocks[i].ptr) sorted[n++] = blocks[i];
        }
    }
    LightLock_Unlock(&lock);
    if (n == 0) return;
    qsort(sorted, n, sizeof(Block), compareAddresses);
    uintptr_t largest = sorted[0].ptr - heapStart, largestAt = heapStart;
    uint32_t islands = 0;
    uint32_t first = 0;
    for (uint32_t i = 0; n >= i; i++) {
        uintptr_t end = i > 0 ? sorted[i - 1].ptr + malloc_usable_size((void*) sorted[i - 1].ptr) : heapStart;
        uintptr_t next = n > i ? sorted[i].ptr : heapEnd;
        if (end > next) end = next;
        if (i > 0 && next - end > largest) {
            largest = next - end;
            largestAt = end;
        }
        if (i > 0 && (n == i || next - end >= ISLAND_GAP)) {
            // The island [first, i) ends here.
            uintptr_t bytes = 0;
            for (uint32_t j = first; i > j; j++) bytes += malloc_usable_size((void*) sorted[j].ptr);
            const Site* s = &sites[BLOCK_SITE(sorted[first])];
            logInfo("[heap] island at +%.2f MB: %u blk, %u KB, gap after %.2f MB, first from %08lx %08lx %08lx %08lx %08lx %08lx\n",
                    (double) (sorted[first].ptr - heapStart) / (1024.0 * 1024.0), (unsigned) (i - first), (unsigned) (bytes / 1024),
                    (double) (next - end) / (1024.0 * 1024.0), (unsigned long) s->pc[0], (unsigned long) s->pc[1], (unsigned long) s->pc[2],
                    (unsigned long) s->pc[3], (unsigned long) s->pc[4], (unsigned long) s->pc[5]);
            islands++;
            first = i;
        }
    }
    logInfo("[heap] %s: %u live blocks in %u islands, largest free piece %.2f MB at +%.2f MB\n", label, (unsigned) n, (unsigned) islands,
            (double) largest / (1024.0 * 1024.0), (double) (largestAt - heapStart) / (1024.0 * 1024.0));
}

// How many live blocks there are of each size, and their bytes.
static void dumpSizes(const char* label) {
    static const uint32_t limits[] = {16, 32, 64, 128, 256, 512, 1024, 4096, 16384, 65536, 262144, UINT32_MAX};
    enum { BUCKETS = sizeof(limits) / sizeof(limits[0]) };
    uint32_t counts[BUCKETS] = {0}, bytes[BUCKETS] = {0};
    LightLock_Lock(&lock);
    for (uint32_t i = 0; BLOCK_CAPACITY > i; i++) {
        if (blocks[i].ptr == 0) continue;
        uint32_t size = (uint32_t) malloc_usable_size((void*) blocks[i].ptr);
        int b = 0;
        while (size > limits[b]) b++;
        counts[b]++;
        bytes[b] += size;
    }
    LightLock_Unlock(&lock);
    char text[BUCKETS * 32];
    int len = 0;
    for (int b = 0; BUCKETS > b; b++) {
        if (limits[b] == UINT32_MAX) {
            len += snprintf(text + len, sizeof(text) - (size_t) len, " more %lu/%lu", (unsigned long) counts[b], (unsigned long) (bytes[b] / 1024));
        } else if (limits[b] >= 1024) {
            len += snprintf(text + len, sizeof(text) - (size_t) len, " %luK %lu/%lu", (unsigned long) (limits[b] / 1024), (unsigned long) counts[b],
                            (unsigned long) (bytes[b] / 1024));
        } else {
            len += snprintf(text + len, sizeof(text) - (size_t) len, " %lu %lu/%lu", (unsigned long) limits[b], (unsigned long) counts[b],
                            (unsigned long) (bytes[b] / 1024));
        }
    }
    logInfo("[heap] %s sizes (up to: blocks/KB):%s\n", label, text);
}

static void writeCensusMap(const char* label);

void CtrHeapProfile_dump(const char* label, int top) {
    if (!ready) return;
    static uint32_t order[SITE_CAPACITY];
    LightLock_Lock(&lock);
    uint32_t n = siteCount;
    uint64_t total = 0;
    for (uint32_t i = 0; n > i; i++) {
        order[i] = i;
        total += sites[i].liveBytes;
    }
    uint32_t live = blockCount, dropped = droppedCount, stale = staleCount;
    LightLock_Unlock(&lock);
    if ((uint32_t) top > n) top = (int) n;
    // First by live bytes, then by live blocks (the small ones that split the heap).
    for (int byBlocks = 0; 2 > byBlocks; byBlocks++) {
        // Partial selection sort: only the top entries are needed.
        for (int i = 0; top > i; i++) {
            uint32_t best = (uint32_t) i;
            for (uint32_t j = (uint32_t) i + 1; n > j; j++) {
                const Site* a = &sites[order[j]];
                const Site* b = &sites[order[best]];
                if (byBlocks ? a->liveCount > b->liveCount : a->liveBytes > b->liveBytes) best = j;
            }
            uint32_t t = order[i];
            order[i] = order[best];
            order[best] = t;
        }
        logInfo("[heap] %s%s: %.1f MB in %u tracked blocks, %u call stacks, %u untracked, %u stale records replaced\n", label,
                byBlocks ? " by blocks" : "", (double) total / (1024.0 * 1024.0), (unsigned) live, (unsigned) n, (unsigned) dropped,
                (unsigned) stale);
        for (int i = 0; top > i; i++) {
            Site* s = &sites[order[i]];
            char pcs[SITE_DEPTH * 9 + 1];
            int len = 0;
            for (int j = 0; SITE_DEPTH > j; j++) len += snprintf(pcs + len, sizeof(pcs) - (size_t) len, " %08lx", (unsigned long) s->pc[j]);
            logInfo("[heap] %7u KB %6u blk (peak %7u KB)%s\n", (unsigned) (s->liveBytes / 1024), (unsigned) s->liveCount,
                    (unsigned) (s->peakBytes / 1024), pcs);
        }
    }
    dumpSizes(label);
    dumpLayout(label);
    CtrMemory_logHeapLayout(label, 0);
    // Every chunk, at each stage (the census writes room maps itself; running dumps write none).
    if (strcmp(label, "running") != 0) writeCensusMap(label);
}

// ===[ Census heap maps ]===

// Census maps: "u <offset> <size> b<birth room serial> <9 call-stack PCs>" per chunk in use ("b-" and no PCs when
// untracked), "f <offset> <size>" per free run. Hard caps on their number and total bytes.
#define CENSUS_MAP_MAX 120u
#define CENSUS_MAP_BYTES (320u * 1024u * 1024u)
// A room map every this many rooms.
#define CENSUS_MAP_EVERY 4u
static uint32_t censusMaps, censusMapBytes;

extern char* __malloc_sbrk_base;
extern struct mallinfo __malloc_current_mallinfo;
void __malloc_lock(struct _reent* reent);
void __malloc_unlock(struct _reent* reent);

static void writeCensusMap(const char* label) {
    if (censusMaps >= CENSUS_MAP_MAX || censusMapBytes >= CENSUS_MAP_BYTES) return;
    typedef struct {
        uint32_t offset;
        uint32_t size; // low bit: in use
    } MapChunk;
    const uint32_t capacity = 128u * 1024u;
    MapChunk* chunks = (MapChunk*) linearAlloc(capacity * sizeof(MapChunk));
    if (chunks == nullptr) return;
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
    char path[96];
    snprintf(path, sizeof(path), CTRPORT_GAME_DIR "/censusmap_%03u.txt", (unsigned) censusMaps);
    FILE* f = broken ? nullptr : fopen(path, "w");
    if (f == nullptr) {
        linearFree(chunks);
        return;
    }
    uint32_t heapBytes = __ctru_heap_size - (uint32_t) (base - __ctru_heap);
    long written = fprintf(f, "censusmap %s: room serial %lu, heap %lu B from %08lx, sbrk top at %lu, %lu chunks\n", label,
                           (unsigned long) roomSerial, (unsigned long) heapBytes, (unsigned long) base, (unsigned long) (end - base),
                           (unsigned long) count);
    for (uint32_t i = 0; count > i && CENSUS_MAP_BYTES > censusMapBytes + (uint32_t) written; i++) {
        uint32_t size = chunks[i].size & ~1u;
        if ((chunks[i].size & 1u) == 0) {
            written += fprintf(f, "f %lu %lu\n", (unsigned long) chunks[i].offset, (unsigned long) size);
            continue;
        }
        uintptr_t ptr = base + chunks[i].offset + 8;
        bool found = false;
        Block b = {0, 0};
        LightLock_Lock(&lock);
        uint32_t slot = blockSlot(ptr);
        while (blocks[slot].ptr != 0 && blocks[slot].ptr != ptr) slot = (slot + 1) & (BLOCK_CAPACITY - 1);
        if (blocks[slot].ptr != 0) {
            b = blocks[slot];
            found = true;
        }
        LightLock_Unlock(&lock);
        if (!found) {
            written += fprintf(f, "u %lu %lu b-\n", (unsigned long) chunks[i].offset, (unsigned long) size);
            continue;
        }
        const uint32_t* pc = sites[BLOCK_SITE(b)].pc;
        written += fprintf(f, "u %lu %lu b%lu %lx %lx %lx %lx %lx %lx %lx %lx %lx\n", (unsigned long) chunks[i].offset, (unsigned long) size,
                           (unsigned long) BLOCK_BIRTH(b), (unsigned long) pc[0], (unsigned long) pc[1], (unsigned long) pc[2],
                           (unsigned long) pc[3], (unsigned long) pc[4], (unsigned long) pc[5], (unsigned long) pc[6], (unsigned long) pc[7],
                           (unsigned long) pc[8]);
    }
    fclose(f);
    linearFree(chunks);
    censusMaps++;
    censusMapBytes += (uint32_t) written;
    logInfo("[census] %s: heap map %s (%lu KB; %lu KB of maps so far)\n", label, path, (unsigned long) (written / 1024),
            (unsigned long) (censusMapBytes / 1024));
}

static void logCensusTable(const char* what, uint32_t count[CENSUS_SIZES][CENSUS_AGES], uint32_t bytes[CENSUS_SIZES][CENSUS_AGES]) {
    // One line per size class with any blocks: count/KB at each age.
    for (uint32_t s = 0; CENSUS_SIZES > s; s++) {
        uint32_t any = 0;
        for (int a = 0; CENSUS_AGES > a; a++) any += count[s][a];
        if (any == 0) continue;
        char text[CENSUS_AGES * 24];
        int len = 0;
        for (int a = 0; CENSUS_AGES > a; a++) {
            len += snprintf(text + len, sizeof(text) - (size_t) len, " %lu/%lu", (unsigned long) count[s][a],
                            (unsigned long) ((bytes[s][a] + 512u) / 1024u));
        }
        if (censusSizes[s] == UINT32_MAX) {
            logInfo("[census] %s more: (n/KB by age 0,1,2-4,5-19,20+)%s\n", what, text);
        } else {
            logInfo("[census] %s <=%lu: (n/KB by age 0,1,2-4,5-19,20+)%s\n", what, (unsigned long) censusSizes[s], text);
        }
    }
}

void CtrHeapProfile_roomChanged(int32_t roomIndex) {
    if (!ready) return;
    static uint32_t liveCount[CENSUS_SIZES][CENSUS_AGES], liveBytes[CENSUS_SIZES][CENSUS_AGES];
    static uint32_t fc[CENSUS_SIZES][CENSUS_AGES], fb[CENSUS_SIZES][CENSUS_AGES];
    // Blocks freed in the room just left, by the age they reached; then the live ones, by age, before the serial moves.
    LightLock_Lock(&lock);
    memcpy(fc, freedCount, sizeof(fc));
    memcpy(fb, freedBytes, sizeof(fb));
    memset(freedCount, 0, sizeof(freedCount));
    memset(freedBytes, 0, sizeof(freedBytes));
    memset(liveCount, 0, sizeof(liveCount));
    memset(liveBytes, 0, sizeof(liveBytes));
    for (uint32_t i = 0; BLOCK_CAPACITY > i; i++) {
        if (blocks[i].ptr == 0) continue;
        uint32_t size = (uint32_t) malloc_usable_size((void*) blocks[i].ptr);
        int s = censusSize(size), a = censusAge(BLOCK_BIRTH(blocks[i]));
        liveCount[s][a]++;
        liveBytes[s][a] += size;
    }
    uint32_t serial = roomSerial;
    LightLock_Unlock(&lock);
    char label[48];
    snprintf(label, sizeof(label), "room %ld serial %lu", (long) roomIndex, (unsigned long) serial);
    logInfo("[census] %s: entering\n", label);
    CtrMemory_logHeapLayout(label, 0);
    logCensusTable("freed in last room", fc, fb);
    logCensusTable("live", liveCount, liveBytes);
    if (serial % CENSUS_MAP_EVERY == 0) writeCensusMap(label);
    LightLock_Lock(&lock);
    roomSerial++;
    LightLock_Unlock(&lock);
}

bool CtrHeapProfile_siteOf(const void* ptr, uint32_t* pcs, int count) {
    if (!ready || ptr == nullptr) return false;
    if (count > SITE_DEPTH) count = SITE_DEPTH;
    bool found = false;
    LightLock_Lock(&lock);
    uint32_t slot = blockSlot((uintptr_t) ptr);
    while (blocks[slot].ptr != 0 && blocks[slot].ptr != (uintptr_t) ptr) slot = (slot + 1) & (BLOCK_CAPACITY - 1);
    if (blocks[slot].ptr != 0) {
        memcpy(pcs, sites[BLOCK_SITE(blocks[slot])].pc, (size_t) count * sizeof(uint32_t));
        found = true;
    }
    LightLock_Unlock(&lock);
    return found;
}

#endif
