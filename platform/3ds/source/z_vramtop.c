// gpuburst F1 (UNDERTALE on the Old models): CtrMemory_vramAllocTop by VRAM bank. The link sends its callers (the tiles'
// copies, ctr_renderer.c's "Tiles' copies in VRAM on the Old models", and the seam copies, z_seamcopy.c) here
// (--wrap, CMakeLists.txt); ctr_memory.c's version stays where it was, unused. Late code only, in its own appended
// section (platform/3ds/late_reserve.ld), so nothing that existed before moves.
//
// As ctr_memory.c's: at the top of the highest free gap that holds `size`, every free gap below the target filled for a
// moment (blocks the GPU never sees, freed at once) so that libctru's first fit gives the target, and nullptr for
// anything else (an address not the one planned, more blocks than these lists hold): no copy, never a copy elsewhere.
// Surfaces go first fit from the bottom; copies at the top stay above them, so giving a copy back leaves no hole
// between surfaces (a 640x480 surface found 713 KB free, but in pieces).
//
// What differs: libctru keeps VRAM as two pools, bank A [lo, mid) and bank B [mid, hi). vramAlloc asks the pool with
// more free space first, vramAllocAt the one named, and no block crosses mid. ctr_memory.c's fillers were asked of
// either pool, so once bank A had more free space than bank B a filler meant for B went to A, the copy missed its
// place and was given back, and every tile and seam copy failed, every frame, for as long as the banks stayed so:
// room 112 drew in 105 draws instead of 3 (gpuburst round 3; docs/3ds-hardware-2.md, "VRAM is two banks"). Here a
// gap across mid is two gaps, and the fillers and the copy are asked of the target's bank (as knightgpu FC's
// CtrMemory_vramAllocFit does for DELTARUNE New). Copies so go to the top of bank B, then of bank A: above each bank's
// surfaces, as before.
//
// ctr_memory.c's statics are reached by its assembler aliases (CtrMemory_vt*, at that file's end); the lock is taken as
// its lockLinear takes it, and the held frees the GPU is done with are given back through CtrMemory_vramFreeNow, as
// knightgpu A does. The lists (3 KB, for VRAM_TOP_BLOCKS_MAX blocks, as before) are on the stack: callers run on the
// main thread, whose 32 KB peaks at 12.8 KB, and buffers there stay under 8 KB (docs/3ds-hardware.md, "The main stack
// is libctru's 32 KB and has room"); this file adds no static.
// A request that fails is asked again by its caller no more than once a frame (mirrorHotTiles stops at a null; the
// seam copies wait SEAM_RETRY_FRAMES), at the cost of one look over the blocks.
#if !defined(ENABLE_WAD17) && defined(CTRPORT_TARGET_O3DS) && defined(__3DS__)
#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>

#include "common.h"
#include "ctr_memory.h"
#include "ctr_memory_lock.h"

#define VT_LATE __attribute__((noinline, section(".text.late.gpuburst")))
#define VRAM_TOP_BLOCKS_MAX 256 // ctr_memory.c's
#define VRAM_BLOCKS_MAX 1024    // ctr_memory.c's

// ctr_memory.c's HeldFree, field for field.
typedef struct {
    void* mem;
    uint32_t bytes;
    uint32_t due;
    bool vram;
} VtHeldFree;

extern LightLock CtrMemory_linearLockStore;
extern CtrVramBlock CtrMemory_vtBlocks[VRAM_BLOCKS_MAX];
extern uint32_t CtrMemory_vtBlockCount;
extern bool CtrMemory_vtBlocksLost;
extern VtHeldFree CtrMemory_vtHeld[];
extern uint32_t CtrMemory_vtHeldCount;
extern uint32_t CtrMemory_vtContended;
extern uint64_t CtrMemory_vtWaitTicks;
void* __real_vramAllocAt(size_t size, vramAllocPos pos);
void __real_vramFree(void* mem);

void* __wrap_CtrMemory_vramAllocTop(size_t size) VT_LATE;
void* __wrap_CtrMemory_vramAllocTop(size_t size) {
    const uintptr_t lo = (uintptr_t) OS_VRAM_VADDR, hi = lo + (uintptr_t) OS_VRAM_SIZE;
    const uintptr_t mid = lo + (uintptr_t) OS_VRAM_SIZE / 2;
    size = (size + 0x7Fu) & ~(size_t) 0x7Fu;
    CtrMemory_vramFreeNow(); // the held frees the GPU is done with, given back
    // ctr_memory.c's lockLinear.
    if (CtrMemory_linearLockTry(&CtrMemory_linearLockStore) != 0) {
        uint64_t start = svcGetSystemTick();
        CtrMemory_linearLockAcquire(&CtrMemory_linearLockStore);
        CtrMemory_vtContended++;
        CtrMemory_vtWaitTicks += svcGetSystemTick() - start;
    }
    // The blocks taken: those tracked, and the frees held back until the GPU is done.
    uint32_t count = CtrMemory_vtBlockCount;
    for (uint32_t i = 0; CtrMemory_vtHeldCount > i; i++) count += CtrMemory_vtHeld[i].vram ? 1u : 0u;
    bool ok = !CtrMemory_vtBlocksLost && VRAM_TOP_BLOCKS_MAX >= count;
    const uint32_t n = ok ? count : 0;
    CtrVramBlock used[VRAM_TOP_BLOCKS_MAX];
    void* fillers[VRAM_TOP_BLOCKS_MAX + 1];
    uint32_t m = 0;
    for (uint32_t i = 0; ok && CtrMemory_vtBlockCount > i; i++) used[m++] = CtrMemory_vtBlocks[i];
    for (uint32_t i = 0; ok && CtrMemory_vtHeldCount > i; i++) {
        if (CtrMemory_vtHeld[i].vram) used[m++] = (CtrVramBlock) { (uintptr_t) CtrMemory_vtHeld[i].mem, CtrMemory_vtHeld[i].bytes };
    }
    for (uint32_t i = 1; ok && n > i; i++) {
        CtrVramBlock b = used[i];
        uint32_t j = i;
        for (; j > 0 && used[j - 1].addr > b.addr; j--) used[j] = used[j - 1];
        used[j] = b;
    }
    // The highest part of a gap, inside one bank, that fits: the gap from the end of block k-1 (or lo) to block k (or
    // hi), its part above mid first.
    uintptr_t gapLo = 0, target = 0;
    for (int32_t k = (int32_t) n; ok && k >= 0 && target == 0; k--) {
        uintptr_t start = k > 0 ? used[k - 1].addr + used[k - 1].size : lo;
        uintptr_t end = k < (int32_t) n ? used[k].addr : hi;
        start = (start + 0x7Fu) & ~(uintptr_t) 0x7Fu;
        for (uint32_t h = 0; 2u > h; h++) {
            uintptr_t s = h == 0 ? (mid > start ? mid : start) : start;
            uintptr_t e = h == 0 ? end : (mid > end ? end : mid);
            if (e > s && e - s >= size) {
                gapLo = s;
                target = (e - size) & ~(uintptr_t) 0x7Fu;
                break;
            }
        }
    }
    void* p = nullptr;
    uint32_t f = 0;
    ok = ok && target >= gapLo && target != 0;
    const uintptr_t bankLo = target >= mid ? mid : lo;
    const vramAllocPos bank = target >= mid ? VRAM_ALLOC_B : VRAM_ALLOC_A;
    // Fill every gap of the target's bank below the target, in address order, and the target gap's bottom (libctru's
    // vramAllocAt takes that pool's first gap that fits).
    for (uint32_t k = 0; ok && n >= k; k++) {
        uintptr_t start = k > 0 ? used[k - 1].addr + used[k - 1].size : lo;
        start = (start + 0x7Fu) & ~(uintptr_t) 0x7Fu;
        if (bankLo > start) start = bankLo;
        if (start >= target) break;
        uintptr_t end = k < n ? used[k].addr : hi;
        if (end > target) end = target;
        if (end <= start) continue;
        void* q = __real_vramAllocAt((size_t) (end - start), bank);
        if (q != nullptr) fillers[f++] = q;
        ok = q == (void*) start;
    }
    if (ok) {
        p = __real_vramAllocAt(size, bank);
        if (p != (void*) target) {
            if (p != nullptr) __real_vramFree(p);
            p = nullptr;
        }
    }
    for (uint32_t i = 0; f > i; i++) __real_vramFree(fillers[i]);
    // As ctr_memory.c's trackVram.
    if (p != nullptr && VRAM_BLOCKS_MAX > CtrMemory_vtBlockCount) {
        CtrMemory_vtBlocks[CtrMemory_vtBlockCount++] = (CtrVramBlock) { (uintptr_t) p, (uint32_t) vramGetSize(p) };
    } else if (p != nullptr) {
        __real_vramFree(p);
        p = nullptr;
    }
    // Not LightLock_Unlock: the release also gives back the caller's heap relief (ctr_memory_lock.c).
    CtrMemory_linearLockRelease(&CtrMemory_linearLockStore);
    return p;
}
#endif
