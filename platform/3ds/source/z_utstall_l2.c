// freewins L2 (round 4, UNDERTALE): z_utstall.c's usWholeMakeRoom eviction, late code last in merge order.
// Tiles not drawn in the old room's last frame go, least recently used first, until the room's missing tiles fit below
// the texture budget less the headroom AND in linear memory above its floor with 512 KB to spare. The budget assumes
// the other linear users' share; where they hold more (Flowey's room: the sound-effect cache and the music's buffers),
// the room load's passes stopped at the linear floor with stale tiles still resident, and the rest of the set loaded
// in play (freewins, round 4).
// A file of its own so that usWholeMakeRoom's call is external, as its call to CtrTextures_evictLeastRecentIdle was:
// a call into its own file changed the order GCC emits z_utstall.c's functions in, and moved every later symbol.
// Textures only: which tiles stay resident, never their texels, the game's state or its timing.
// Also freewins L1 (UNDERTALE Old, below): the overflow allocator's linear piece, made by evicting idle tiles.
#include <stdbool.h>
#include <stdint.h>

#if defined(ENABLE_WAD16)
#include "ctr_textures.h"

// UTSTALL_WHOLE_HEADROOM (z_utstall.c; ROOMLOAD_SPRITE_HEADROOM_BYTES in ctr_textures.c), repeated.
#define L2_HEADROOM (4u * 1024u * 1024u)
// LINEAR_FLOOR_BYTES (ctr_textures.c), repeated, plus 512 KB to spare.
#define L2_LINEAR (3u * 1024u * 1024u + 512u * 1024u)

void usWholeEvict(CtrTextureCache* cache, uint64_t missing);

__attribute__((noinline, section(".text.freewins_r4_late"))) void usWholeEvict(CtrTextureCache* cache, uint64_t missing) {
    while ((uint64_t) cache->residentBytes + missing + L2_HEADROOM > cache->budgetBytes ||
           (uint64_t) linearSpaceFree() < missing + L2_LINEAR) {
        if (!CtrTextures_evictLeastRecentIdle(cache, 1)) break;
    }
}

#if defined(CTRPORT_PKLIN)
// freewins L1 (round 4c, UNDERTALE Old only): an allocation that neither the heap nor the heap reliever could satisfy
// goes to linear memory (ctr_memory.c's overflowAlloc). Linear can hold MBs free with no piece the size asked: at a
// VM reset after a full session (18/40 MB, round 4) the text maps' 768 KB realloc found 7.08 MB free and no piece
// over 0.64 MB, and the game stopped. On the game thread, tiles not drawn in the last two frames go, least recently
// used first, until the piece fits; they load again when drawn. Other threads, and a call made while this one runs
// (its log line), get linearAlloc alone, as before.
#include "ctr_memory.h"
#include "log.h"

static __attribute__((section(".bss.freewins_r4_late"))) struct {
    CtrTextureCache* cache; // the heap reliever's (CtrTextures_init's call); nullptr after CtrTextures_destroy's
    bool busy;
} l1;

void __real_CtrMemory_setHeapReliever(CtrHeapReliever reliever, void* context);
void __wrap_CtrMemory_setHeapReliever(CtrHeapReliever reliever, void* context);
void* CtrMemory_overflowLinear(size_t size);

// The log line's constants, in late text (late_reserve.ld), so that the file's plain .rodata stays empty.
#define L1_CONST __attribute__((aligned(4), section(".rodata.freewins_r4_in_text")))
// Short: late text had 10 B left before its page with the longer text (check_layout's reserve line).
static const char l1Log[] L1_CONST = "Memory (L1): %u KB in no piece; evicted %u KB of idle tiles: %s (linear %u KB free)\n";
// The log's outcome: "no piece", or 9 B on, "fits" (one string, so that the call is made in one place).
static const char l1Result[] L1_CONST = "no piece\0fits";

// CtrTextures_init and CtrTextures_destroy call it (ctr_textures.c); the context is the texture cache. Nothing else
// wraps it in an UNDERTALE build (z_roomplan.c's wrap is DELTARUNE's).
__attribute__((noinline, section(".text.freewins_r4b_late"))) void __wrap_CtrMemory_setHeapReliever(CtrHeapReliever reliever,
                                                                                                      void* context) {
    l1.cache = (CtrTextureCache*) context;
    __real_CtrMemory_setHeapReliever(reliever, context);
}

// libctru's ThreadVars at the start of each thread's TLS: its magic, then its Thread (NULL on the main thread, which is
// the game thread: main.c's CtrMemory_markGameThread). threadGetCurrent would svcBreak on a thread made with
// svcCreateThread (ctr_exit.c's guard), which has no ThreadVars; reading them first sends such a thread to linearAlloc.
#define L1_THREADVARS_MAGIC 0x21545624u

__attribute__((noinline, section(".text.freewins_r4b_late"))) void* CtrMemory_overflowLinear(size_t size) {
    const uint32_t* vars = (const uint32_t*) getThreadLocalStorage();
    CtrTextureCache* cache = l1.cache;
    bool mine = cache != nullptr && !l1.busy && ((vars[0] ^ L1_THREADVARS_MAGIC) | vars[1]) == 0;
    // Busy before the first linearAlloc too: its deferred relief can allocate, and that must not come back here.
    if (mine) l1.busy = true;
    void* p = linearAlloc(size);
    if (!mine) return p;
    if (p == nullptr) {
        uint32_t before = cache->residentBytes;
        while (p == nullptr && CtrTextures_evictLeastRecentIdle(cache, 1)) p = linearAlloc(size);
        __asm__("" : "+r"(p)); // one log call for both outcomes (GCC would copy its tail per outcome)
        logInfo(l1Log, (unsigned) (size / 1024), (unsigned) ((before - cache->residentBytes) / 1024),
                l1Result + 9 * (p != nullptr), (unsigned) (linearSpaceFree() / 1024));
    }
    l1.busy = false;
    return p;
}
#endif
#endif
