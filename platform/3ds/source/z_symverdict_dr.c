// Symmetric halving's first look from the build, DELTARUNE New 3DS (source/z_symverdict.c).
//
// DELTARUNE New builds compile z_symverdict.c through this file, so that its object (z_symverdict_dr.o) and its
// sections (named symverdict_dr, ctr_wrap_chain.h) have their own lines last in platform/3ds/late_reserve.ld: code
// merged before it keeps its place. UNDERTALE compiles z_symverdict.c itself, and this file adds nothing to it or to
// DELTARUNE Old builds.
#define SY_DR_UNIT 1
#include "z_symverdict.c"
