// An empty asm-goto retains this cold block after the normal returns, but
// emits no edge to it. Each ARM word is UDF #0 (0xe7f000f0), a trap if reached.
// Explicit instruction lines let GCC position literal pools accurately.
#define KNFUSE_TRAP ".inst 0xe7f000f0\n"
#define KNFUSE_TRAP4 KNFUSE_TRAP KNFUSE_TRAP KNFUSE_TRAP KNFUSE_TRAP
#define KNFUSE_TRAP16 KNFUSE_TRAP4 KNFUSE_TRAP4 KNFUSE_TRAP4 KNFUSE_TRAP4
    __asm__ volatile (
        KNFUSE_TRAP16 KNFUSE_TRAP16 KNFUSE_TRAP16 KNFUSE_TRAP16
        KNFUSE_TRAP16 KNFUSE_TRAP16 KNFUSE_TRAP16 KNFUSE_TRAP16
        KNFUSE_TRAP16 KNFUSE_TRAP16 KNFUSE_TRAP4
#ifdef CTRPORT_RENDER_TIMING
        KNFUSE_TRAP4 KNFUSE_TRAP4 KNFUSE_TRAP4
        KNFUSE_TRAP16 KNFUSE_TRAP16
        KNFUSE_TRAP4 KNFUSE_TRAP4 KNFUSE_TRAP4
        KNFUSE_TRAP KNFUSE_TRAP KNFUSE_TRAP
#if defined(ENABLE_WAD17) && defined(NDEBUG)
        // The fountain dispatcher also removes 76 B in renderer-timed builds.
        // Match Release's retained compensation and the 2960 B profile guard.
        KNFUSE_TRAP16 KNFUSE_TRAP KNFUSE_TRAP KNFUSE_TRAP
#endif
#else
        KNFUSE_TRAP4 KNFUSE_TRAP KNFUSE_TRAP
#if defined(ENABLE_WAD17) && defined(NDEBUG)
        // The exact fountain's late direct-region dispatcher removes 76 B
        // of inlined code. Keep the existing function-size guard intact by
        // adding 19 unreachable ARM words to this already retained tail.
        KNFUSE_TRAP16 KNFUSE_TRAP KNFUSE_TRAP KNFUSE_TRAP
#endif
#endif
    );
#undef KNFUSE_TRAP16
#undef KNFUSE_TRAP4
#undef KNFUSE_TRAP
