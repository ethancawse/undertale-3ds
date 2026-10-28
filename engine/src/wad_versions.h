#ifndef _BS_WAD_VERSIONS_H_
#define _BS_WAD_VERSIONS_H_

// To avoid unused code in production builds that are only used for a single game (example: PlayStation 2 target), we can enable/disable specific WAD versions
// to remove unused instructions on the hot bytecode interpreter loop.
//
// If only a single WAD version is enabled, the checks will be collapsed and removed by the compiler during build time, reducing code size and improving
// performance on low end hardware!

#if defined(ENABLE_WAD14)
#  define IS_WAD14_ENABLED 1
#else
#  define IS_WAD14_ENABLED 0
#endif

#if defined(ENABLE_WAD16)
#  define IS_WAD16_OR_HIGHER_ENABLED 1
#else
#  define IS_WAD16_OR_HIGHER_ENABLED 0
#endif

#if defined(ENABLE_WAD17)
#  define IS_WAD17_OR_HIGHER_ENABLED 1
#else
#  define IS_WAD17_OR_HIGHER_ENABLED 0
#endif

#if defined(ENABLE_WAD14) && (defined(ENABLE_WAD16) || defined(ENABLE_WAD17))
#  define IS_WAD14_OR_BELOW(ctx)   (14 >= ctx->dataWin->gen8.wadVersion)
#elif defined(ENABLE_WAD14)
#  define IS_WAD14_OR_BELOW(ctx)   1
#else
#  define IS_WAD14_OR_BELOW(ctx)   0
#endif

#if defined(ENABLE_WAD14) && (defined(ENABLE_WAD16) || defined(ENABLE_WAD17))
#  define IS_WAD15_OR_HIGHER(ctx)  (ctx->dataWin->gen8.wadVersion >= 15)
#elif defined(ENABLE_WAD14)
#  define IS_WAD15_OR_HIGHER(ctx)  0
#else
#  define IS_WAD15_OR_HIGHER(ctx)  1
#endif

#if defined(ENABLE_WAD16) && defined(ENABLE_WAD17)
#  define IS_WAD16_OR_BELOW(ctx)   (16 >= ctx->dataWin->gen8.wadVersion)
#  define IS_WAD16_OR_HIGHER(ctx)  (ctx->dataWin->gen8.wadVersion >= 16)
#  define IS_WAD17_OR_BELOW(ctx)   (17 >= ctx->dataWin->gen8.wadVersion)
#  define IS_WAD17_OR_HIGHER(ctx)  (ctx->dataWin->gen8.wadVersion >= 17)
#elif defined(ENABLE_WAD16)
#  define IS_WAD16_OR_BELOW(ctx)   1
#  define IS_WAD16_OR_HIGHER(ctx)  1
#  define IS_WAD17_OR_BELOW(ctx)   1
#  define IS_WAD17_OR_HIGHER(ctx)  0
#elif defined(ENABLE_WAD17)
#  define IS_WAD16_OR_BELOW(ctx)   0
#  define IS_WAD16_OR_HIGHER(ctx)  1
#  define IS_WAD17_OR_BELOW(ctx)   1
#  define IS_WAD17_OR_HIGHER(ctx)  1
#elif defined(ENABLE_WAD14)
#  define IS_WAD16_OR_BELOW(ctx)   1
#  define IS_WAD16_OR_HIGHER(ctx)  0
#  define IS_WAD17_OR_BELOW(ctx)   1
#  define IS_WAD17_OR_HIGHER(ctx)  0
#else
#  error "You need to build Butterscotch with at least one WAD version enabled!"
#endif

// UNDERTALE's late-text reclaim (argcheck): code and keep-alive references only builds with WAD 17 (DELTARUNE,
// and hosts) compile. One line each, so that no __LINE__ after them moves.
#if defined(ENABLE_WAD17)
#  define WAD17_ONLY(...) __VA_ARGS__
#  define WAD17_NONE(...)
#else
#  define WAD17_ONLY(...)
#  define WAD17_NONE(...) __VA_ARGS__
#endif

#endif /* _BS_WAD_VERSIONS_H_ */
