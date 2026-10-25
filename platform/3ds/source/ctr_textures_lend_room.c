// Palette copies' texture budget at a room's start (kawkaw). Late code
// only: this file adds nothing to .text, so no existing code moves.
#include "ctr_textures.h"

// CtrTextures_lendBudget lends only from tiles idle idleFrames, which right
// after a room change still counts the previous room's tiles as busy. So
// whether a palette copy made in a room's first second fitted depended on
// what the rooms before had left resident: on the Old 3DS budget DELTARUNE
// Chapter 5's kawkaw dash and ending drew different shading alone and after
// the DR44 scene run, with identical game state.
//
// Here a tile no one has used since the room began counts as idle too. The
// room marks the tiles it wants used when it starts (markRemembered,
// markInstances), so its own tiles keep their protection, and tiles drawn
// in the current frame are never evicted. The earlier rooms' tiles are older
// than any the room has used, so lendBudget's oldest-first eviction takes
// them first.
__attribute__((noinline, section(".text.late.kawkaw")))
bool CtrTextures_lendBudgetInRoom(CtrTextureCache* cache, uint32_t bytes, uint32_t idleFrames) {
    if (cache->frame >= cache->roomStartFrame && idleFrames > cache->frame - cache->roomStartFrame) idleFrames = cache->frame - cache->roomStartFrame;
    return CtrTextures_lendBudget(cache, bytes, idleFrames);
}
