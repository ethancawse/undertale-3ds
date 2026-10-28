// Crisp small text, DELTARUNE New 3DS (source/z_smalltext.c; sansfont round 3, approved 10-26).
//
// DELTARUNE New builds compile z_smalltext.c through this file, so that its object (z_smalltext_dr.o) and its sections
// (named smalltext_dr) have their own lines last in platform/3ds/late_reserve.ld: code merged before it keeps its
// place. UNDERTALE compiles z_smalltext.c itself, and this file adds nothing to it or to DELTARUNE Old builds.
#if defined(ENABLE_WAD17) && defined(CTRPORT_TARGET_N3DS)
#define ST_DR_UNIT 1
#include "z_smalltext.c"
#else
#include <stdint.h> // a translation unit of its own in other builds
#endif
