/* GCC emits local BSS in function expansion order. The fallible-copy calls
 * change that order in DELTARUNE; keep this existing eight-byte run in main's
 * order, just as oldmemfix groups its existing parked-surface sentinels. */
#if defined(__3DS__) && defined(NDEBUG) && defined(ENABLE_WAD17) && \
    !defined(ENABLE_VM_GML_PROFILER) && !defined(CTRPORT_HEAP_PROFILE) && \
    !defined(CTRPORT_VINE_DIAGNOSTIC_HELPER) && !defined(CTRPORT_FLOWERY_COMPILE_LATE)
static struct {
    bool dialogue, rainbow, flicker, endFrame, primitiveSurface, primitiveTiles, copy, sprite;
} oomportLogSlots __asm__(".LoomportLogSlots") __attribute__((aligned(1)));
_Static_assert(sizeof(oomportLogSlots) == 8 && offsetof(__typeof__(oomportLogSlots), dialogue) == 0 &&
               offsetof(__typeof__(oomportLogSlots), rainbow) == 1 && offsetof(__typeof__(oomportLogSlots), flicker) == 2 &&
               offsetof(__typeof__(oomportLogSlots), endFrame) == 3 && offsetof(__typeof__(oomportLogSlots), primitiveSurface) == 4 &&
               offsetof(__typeof__(oomportLogSlots), primitiveTiles) == 5 && offsetof(__typeof__(oomportLogSlots), copy) == 6 &&
               offsetof(__typeof__(oomportLogSlots), sprite) == 7, "existing log guard layout");
__asm__(".local logged_.100027\n.type logged_.100027,%object\n.set logged_.100027,.LoomportLogSlots\n.size logged_.100027,1\n"
        ".local logged_.100082\n.type logged_.100082,%object\n.set logged_.100082,.LoomportLogSlots+1\n.size logged_.100082,1\n"
        ".local logged_.100026\n.type logged_.100026,%object\n.set logged_.100026,.LoomportLogSlots+2\n.size logged_.100026,1\n"
        ".local logged_.100028\n.type logged_.100028,%object\n.set logged_.100028,.LoomportLogSlots+3\n.size logged_.100028,1\n"
        ".local logged_.100070\n.type logged_.100070,%object\n.set logged_.100070,.LoomportLogSlots+4\n.size logged_.100070,1\n"
        ".local logged_.100069\n.type logged_.100069,%object\n.set logged_.100069,.LoomportLogSlots+5\n.size logged_.100069,1\n"
        ".local logged_.100055\n.type logged_.100055,%object\n.set logged_.100055,.LoomportLogSlots+6\n.size logged_.100055,1\n"
        ".local logged_.100032\n.type logged_.100032,%object\n.set logged_.100032,.LoomportLogSlots+7\n.size logged_.100032,1\n");
#define OOMPORT_LOG_ONCE(field, ...) do { \
    if (!oomportLogSlots.field) { oomportLogSlots.field = true; logWarn(__VA_ARGS__); } \
} while (0)
#else
#define OOMPORT_LOG_ONCE(field, ...) LOG_ONCE(__VA_ARGS__)
#endif
