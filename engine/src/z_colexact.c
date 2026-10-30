// GMS 1.4's unrotated bbox: round the starting edge before adding the scaled
// width, round the end, then swap for a negative scale. Right/bottom are exclusive
// here; vm_builtins.c subtracts one for the public right/bottom variables.
#include "wad_versions.h"

#if defined(ENABLE_WAD16)
#include "collision.h"

#if !defined(__3DS__) || defined(USE_FLOAT_REALS)
// Share the formerly inlined grid/overlay bbox without growing their warm slots.
__attribute__((noinline, flatten, section(".text.late.colexact")))
InstanceBBox Colexact_sharedBBox(Runner* runner, Instance* inst) {
    return Collision_computeBBox(runner, inst);
}

#endif

#if defined(__3DS__) && !defined(USE_FLOAT_REALS)
#undef Collision_bboxRound
__attribute__((const)) GMLReal Collision_bboxRound(GMLReal v);
#endif

#include "colexact_bbox.h"

// Reads the sprite/instance and changes no state. Preserve that information for
// callers of the old bbox body when this calculation moves out of line.
__attribute__((pure, noinline, section(".text.late.colexact")))
InstanceBBox Colexact_unrotatedBBox(Sprite* spr, Instance* inst) {
    return Colexact_inlineBBox(spr, inst);
}
#endif
