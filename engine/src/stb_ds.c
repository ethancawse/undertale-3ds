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
