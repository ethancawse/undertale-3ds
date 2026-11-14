// numcache (change loadclass, 11-08; both games): a thread's number-cache walk takes newlib's real list count, and on
// UNDERTALE Old 3DS the audio arena's calloc zeroes its whole block. Late code only, in a file of its own so that the
// two files that call it (ctr_memory.c, ctr_audio_system.c) change only their calls' targets.
//
// The fault (the loadclass proof rows on the o2ds, 11-08: a data abort on the audio loader as the app exited). newlib's
// number-conversion lists are _Kmax + 1 pointers (sys/reent.h; its Balloc callocs exactly that many), but
// CtrMemory_releaseNumberCaches, which every thread runs as it ends, walked as many lists as the block's usable size
// holds. The audio arena hands out a whole free block when the rest is too small to split, so an audio thread's lists
// could end in words of old arena bytes, which the walk followed as lists.
#include <malloc.h>
#include <reent.h>
#include <stddef.h>
#include <string.h>

#define NUMCACHE_CODE __attribute__((noinline, section(".text_numcache")))

size_t CtrMemory_numberListBytes(void* freelist);
#ifdef CTRPORT_PACER_ARENA
// As ctr_memory.c's own size calls on UNDERTALE Old 3DS: an arena block's size comes from the arena's header.
size_t CtrArena_usableSize(void* ptr);
#define NUMCACHE_USABLE CtrArena_usableSize
#else
#define NUMCACHE_USABLE malloc_usable_size
#endif

// The size CtrMemory_releaseNumberCaches walks: the lists' real size, never more than the block holds.
NUMCACHE_CODE size_t CtrMemory_numberListBytes(void* freelist) {
    size_t lists = (size_t) (_Kmax + 1) * sizeof(struct _Bigint*);
    size_t usable = NUMCACHE_USABLE(freelist);
    return usable < lists ? usable : lists;
}

#if defined(ENABLE_WAD16) && defined(CTRPORT_PACER_ARENA)
void* paCallocZero(void* p, int c, size_t n);
// The arena's calloc zeroes its whole block, as newlib's does its whole chunk, so every usable-size reader sees zeros
// past the bytes asked for. A block can be up to 16 bytes larger than asked (a free block whose rest is too small to
// split goes whole). Same signature as memset, so the arena's calloc changes only its call's target.
NUMCACHE_CODE void* paCallocZero(void* p, int c, size_t n) {
    size_t usable = CtrArena_usableSize(p);
    return memset(p, c, usable > n ? usable : n);
}
#endif
