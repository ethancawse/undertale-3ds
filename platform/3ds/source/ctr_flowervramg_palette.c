// Palette results are rendering inputs; decoded source tiles are a reloadable
// cache. The shipping DR Old CIA80 layout must retain recolouring under pressure.
#include "ctr_textures.h"

#if defined(ENABLE_WAD17) && defined(CTRPORT_TARGET_O3DS) && defined(CTRPORT_DR_OLD_CIA80_TILES)
#define FVG_PAL_TEXT __attribute__((noipa, section(".text.late.flowervramg_palette")))
#ifndef CTRPORT_FVG_PALETTE_POLICY
#define CTRPORT_FVG_PALETTE_POLICY 2
#endif
#if CTRPORT_FVG_PALETTE_POLICY < 0 || CTRPORT_FVG_PALETTE_POLICY > 2
#error "palette policy: 0 original diagnostic control, 1 retry, 2 retain"
#endif

extern bool __real_CtrTextures_roomThrashing(const CtrTextureCache* cache);
extern bool __real_CtrTextures_lendBudgetInRoom(CtrTextureCache* cache, uint32_t bytes, uint32_t idleFrames);
extern void FvgPalette_evictTile(CtrTextureCache* cache, CtrPage* page, CtrTile* tile);

#if CTRPORT_FVG_PALETTE_POLICY == 1
static struct {
    const CtrTextureCache* cache;
    uint32_t room, frame, retryAt;
    bool waiting;
} fvgPaletteRetry __attribute__((section(".bss.late.flowervramg_palette")));
#endif

FVG_PAL_TEXT bool __wrap_CtrTextures_roomThrashing(const CtrTextureCache* cache) {
#if CTRPORT_FVG_PALETTE_POLICY == 0
    return __real_CtrTextures_roomThrashing(cache);
#elif CTRPORT_FVG_PALETTE_POLICY == 1
    // A repeated thrash starts another bounded wait; it never consumes a
    // room-wide, one-shot forgiveness. No wall clock enters the decision.
    if (fvgPaletteRetry.cache != cache || fvgPaletteRetry.room != cache->roomStartFrame ||
        cache->frame < fvgPaletteRetry.frame) {
        fvgPaletteRetry.cache = cache;
        fvgPaletteRetry.room = cache->roomStartFrame;
        fvgPaletteRetry.waiting = false;
    }
    fvgPaletteRetry.frame = cache->frame;
    if (!cache->roomThrashed) return false;
    if (!fvgPaletteRetry.waiting) {
        fvgPaletteRetry.waiting = true;
        fvgPaletteRetry.retryAt = cache->frame + 4u;
    }
    if ((int32_t) (cache->frame - fvgPaletteRetry.retryAt) < 0) return true;
    fvgPaletteRetry.waiting = false;
    CtrTextures_forgiveThrashing((CtrTextureCache*) cache);
    return false;
#else
    // Keep existing, correct copies. Their demand-driven loans reclaim tiles
    // below; the texture cache still records its own thrash/eviction statistics.
    (void) cache;
    return false;
#endif
}

#if CTRPORT_FVG_PALETTE_POLICY != 0
FVG_PAL_TEXT static bool fvgPaletteMakeRoom(CtrTextureCache* cache, uint32_t bytes) {
    if (bytes > cache->budgetBytes) return false;
    while (cache->residentBytes > cache->budgetBytes - bytes) {
        CtrPage* victimPage = nullptr;
        CtrTile* victim = nullptr;
        bool backed = false;
        for (uint32_t p = 0; p < cache->pageCount; p++) {
            CtrPage* page = &cache->pages[p];
            if (page->residentTiles == 0 || page == cache->busyPage) continue;
            for (int32_t i = 0; i < page->tilesX * page->tilesY; i++) {
                CtrTile* tile = &page->tiles[i];
                // Current-frame draws may still reference the source. A pending
                // reader owns reserved/requested storage too. Never steal it.
                if (!tile->valid || tile->lastUsedFrame >= cache->frame || tile->reserved || tile->requested) continue;
                bool inRam = page->packed != nullptr;
                if (victim == nullptr || (inRam && !backed) ||
                    (inRam == backed && tile->lastUsedFrame < victim->lastUsedFrame)) {
                    victimPage = page;
                    victim = tile;
                    backed = inRam;
                }
            }
        }
        if (victim == nullptr) return false;
        // Use the existing mirror/deferred-free and accounting path. A palette
        // loan's own eviction must not restart the diagnostic retry variant.
        bool thrashed = cache->roomThrashed;
        FvgPalette_evictTile(cache, victimPage, victim);
        cache->roomThrashed = thrashed;
    }
    cache->budgetBytes -= bytes;
    return true;
}
#endif

FVG_PAL_TEXT bool __wrap_CtrTextures_lendBudgetInRoom(CtrTextureCache* cache, uint32_t bytes, uint32_t idleFrames) {
#if CTRPORT_FVG_PALETTE_POLICY == 0
    return __real_CtrTextures_lendBudgetInRoom(cache, bytes, idleFrames);
#else
    (void) idleFrames;
    if (__wrap_CtrTextures_roomThrashing(cache)) return false;
    return fvgPaletteMakeRoom(cache, bytes);
#endif
}
#endif
