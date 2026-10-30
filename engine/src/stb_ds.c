#if defined(PLATFORM_3DS) && defined(__GNUC__)
#include <stdlib.h>
extern void* __wrap_malloc(size_t size);
extern void* __wrap_realloc(void* ptr, size_t size);
// The checked helpers must call the real wrapped allocators, rather than the
// builtin names redirected below for stb alone.
#define malloc __wrap_malloc
#define realloc __wrap_realloc
#endif
#include "utils.h"
#if defined(PLATFORM_3DS) && defined(__GNUC__)
#undef malloc
#undef realloc
#endif

// stb_ds assumes growth succeeds and writes through the returned pointer.
// Keep the checked allocator's requested size and call site on failure.
#if defined(PLATFORM_3DS) && defined(__GNUC__)
__attribute__((used, noipa, alloc_size(2), section(".fatalflush")))
#endif
static void* stbds_safe_realloc(void* ptr, size_t size) {
    return safeRealloc(ptr, size);
}
#if defined(PLATFORM_3DS) && defined(__GNUC__)
__attribute__((used, noipa, alloc_size(1), section(".fatalflush")))
static void* stbds_safe_malloc(size_t size) {
    return safeMalloc(size);
}
// Keep GCC's realloc builtin and its NULL -> malloc expansion intact, including
// after inlining. Bind this object's assembler references only, after GCC has
// optimized them. A pragma rename misses malloc introduced by the optimizer.
__asm__(".set realloc, stbds_safe_realloc\n.set malloc, stbds_safe_malloc");
#define STBDS_REALLOC(c, p, s) realloc((p), (s))
#else
#define STBDS_REALLOC(c, p, s) stbds_safe_realloc((p), (s))
#endif
#define STBDS_FREE(c, p) free(p)
#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"
#if defined(PLATFORM_3DS) && defined(__GNUC__)
// freewins_maps.h mirrors these two layouts to clear a parked map's hash index.
_Static_assert(sizeof(stbds_hash_bucket) == 64 && offsetof(stbds_hash_bucket, index) == 32, "freewins_maps.h bucket");
_Static_assert(offsetof(stbds_hash_index, slot_count) == 4 && offsetof(stbds_hash_index, used_count) == 8 &&
               offsetof(stbds_hash_index, tombstone_count) == 20 && offsetof(stbds_hash_index, storage) == 48,
               "freewins_maps.h hash index");
#endif

#if defined(PLATFORM_3DS) && defined(__GNUC__)
// creddodge lever 2 (both games): a direct-mapped cache in front of the instance-by-id lookup. reach_gate.h's
// RUNNER_INSTANCE_BY_ID calls cdHmgetKey with hmget's arguments, so each site's code is base's with one call target
// changed. A slot keeps the entry index the last lookup of an id with those low bits found. It is used only when the
// map's entry there still holds that id: keys are unique, and stb_ds keeps every entry below the length in the hash
// index (put appends, delete moves the last entry into the hole, a rebuild or a realloc keeps the entries), so that
// entry is the one stbds_hm_find_slot would find. A hit sets the header's temp as stbds_hmget_key does and returns
// the same pointer; anything else is stbds_hmget_key itself.
static uint32_t cdIdSlots[16] __attribute__((section(".bss.cdid_late")));
extern void* cdIdFullGet(void* a, size_t elemsize, void* key, size_t keysize, int mode) __asm__("stbds_hmget_key");
__attribute__((noinline, section(".text.cdid_late"))) void* cdHmgetKey(void* a, size_t elemsize, void* key,
                                                                       size_t keysize, int mode) {
    if (a != NULL && elemsize == 8 && keysize == 4 && mode == STBDS_HM_BINARY) {
        int32_t id = *(const int32_t*) key;
        uint32_t* slot = &cdIdSlots[(uint32_t) id & 15];
        stbds_array_header* h = stbds_header(STBDS_HASH_TO_ARR(a, 8));
        uint32_t i = *slot;
        if (i < h->length - 1 && ((const int32_t*) a)[i * 2] == id) {
            h->temp = (ptrdiff_t) i;
            return a;
        }
        a = cdIdFullGet(a, elemsize, key, keysize, mode);
        ptrdiff_t found = stbds_temp(STBDS_HASH_TO_ARR(a, 8));
        if (found >= 0) *slot = (uint32_t) found;
        return a;
    }
    return cdIdFullGet(a, elemsize, key, keysize, mode);
}
#endif
