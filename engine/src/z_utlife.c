// Outside Room uses native inclusive right/bottom in the UT compatibility
// runtime. Keep that conversion local to this consumer, not collision geometry.
#include "z_utlife.h"

#if defined(ENABLE_WAD16)
__attribute__((noinline, section(".text.late.utlife")))
InstanceBBox Utlife_outsideRoomBBox(Runner* runner, Instance* inst) {
    // Outside Room reads the same PC box as collision and public bbox fields.
    // The native call stays out of line without a duplicate generic bbox body.
    InstanceBBox bbox = Collision_readBBox(runner, inst);
    if (IS_WAD16_OR_BELOW(runner) && runner->collisionCompatibilityMode && bbox.valid) {
        bbox.right -= 1.0;
        bbox.bottom -= 1.0;
    }
    return bbox;
}
#endif
