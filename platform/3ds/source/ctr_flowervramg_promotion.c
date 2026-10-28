// Round 18, DR Old only. Packed copies are a load-speed cache: an unused
// copy may go even while its already-decoded tiles remain resident. Their
// texels do not point into the packed allocation. Never cancel a read or
// discard a queued/current-room source to admit another page.
#if defined(ENABLE_WAD17) && defined(CTRPORT_TARGET_O3DS)
#include <malloc.h>
#include "ctr_textures.h"
#include "log.h"

#define FVG_PROMO_CODE __attribute__((noinline, section(".text.late.flowervramg_promotion")))
#define FVG_PROMO_CONST __attribute__((section(".rodata.late.flowervramg_promotion")))

static const char fvgPromoReclaimLog[] FVG_PROMO_CONST =
    "Textures: DR reclaim idle packed page %u (%u KB), promotion %u\n";

static FVG_PROMO_CODE bool fvgPromoIdleCopy(const CtrTextureCache* cache, const CtrPage* page, const CtrPage* keep) {
    if (page == keep || page == cache->busyPage || page->packed == nullptr || page->read != nullptr ||
        page->promotionPending || cache->frame - page->lastUsedFrame <= 60) return false;
    for (int32_t i = 0; page->layoutKnown && i < page->tilesX * page->tilesY; i++) {
        const CtrTile* tile = &page->tiles[i];
        if (tile->reserved || tile->requested || tile->wanted ||
            (tile->valid && cache->frame - tile->lastUsedFrame <= 60)) return false;
    }
    return true;
}

static FVG_PROMO_CODE uint32_t fvgPromoFreeHeap(void) {
    struct mallinfo info = mallinfo();
    uint32_t heap = envGetHeapSize();
    return heap > (uint32_t) info.uordblks ? heap - (uint32_t) info.uordblks : 0;
}

// Run after the existing whole-read collector, before Step/Draw and the
// ordinary promotion admission. Keep its budget, reader reserve, most-recent
// request ordering and retry interval. Reclaim LRU copies before giving up,
// including a partial reclaim when too little idle memory exists to admit.
static FVG_PROMO_CODE void fvgPromoReclaimPending(CtrTextureCache* cache) {
    if (cache->reader == nullptr || cache->wholeRead != nullptr) return;
    if (cache->workerKeepsPacked && cache->worker != nullptr) {
        uint32_t done = 0, total = 0;
        CtrPackWorker_progress(cache->worker, &done, &total);
        if (total > done) return;
    }
    CtrPage* keep = nullptr;
    for (uint32_t i = 0; i < cache->pageCount; i++) {
        CtrPage* page = &cache->pages[i];
        if (!page->promotionPending || page->packed != nullptr || page->failed ||
            page->cacheFile != CTR_CACHE_FILE_VALID) continue;
        if (keep == nullptr || page->lastUsedFrame > keep->lastUsedFrame) keep = page;
    }
    if (keep == nullptr || keep->promotionBytes > cache->packedBudgetBytes) return;
    uint64_t heapNeeded = (uint64_t) keep->promotionBytes + CtrTileReader_heapReserve();
    while (heapNeeded > fvgPromoFreeHeap() ||
           (uint64_t) cache->packedBytes + cache->wholeBytes + keep->promotionBytes > cache->packedBudgetBytes) {
        CtrPage* victim = nullptr;
        for (uint32_t i = 0; i < cache->pageCount; i++) {
            CtrPage* page = &cache->pages[i];
            if (fvgPromoIdleCopy(cache, page, keep) &&
                (victim == nullptr || page->lastUsedFrame < victim->lastUsedFrame)) victim = page;
        }
        if (victim == nullptr) return; // original admission still checks both limits
        uint32_t bytes = victim->packedSize;
        CtrPacked_free(victim->packed);
        cache->packedBytes -= bytes;
        victim->packed = nullptr;
        victim->packedSize = 0;
        cache->packedReclaims++;
        cache->packedReclaimBytes += bytes;
        logInfo(fvgPromoReclaimLog, (unsigned) (victim - cache->pages), (unsigned) (bytes / 1024),
                (unsigned) (keep - cache->pages));
    }
}

// z_phaseplan.c's existing __real_RoomLoad_collect reference is redirected
// here only for DR Old. Our reference retains the linker's __real_ spelling,
// so it reaches the original collector without recursing through phaseplan.
void __real_RoomLoad_collect(CtrTextureCache* cache, bool drop);
void FvgPromotion_collect(CtrTextureCache* cache, bool drop);
FVG_PROMO_CODE void FvgPromotion_collect(CtrTextureCache* cache, bool drop) {
    __real_RoomLoad_collect(cache, drop);
#ifndef CTRPORT_DR_OLD_CIA80_TILES
    // Ruling, round 19: ordinary 80 MB admission changes palette timing.
    // Ordinary builds enable reclaim only at the accepted 64 MB HBL grant.
    // Query the same OS grant used by the console log, not heap/image size.
    // CIA80 retains the already-gated collector byte for byte.
    // Read the libctru accessor's field here: this build uses -fno-inline,
    // which otherwise emits a new osGetMemRegionSize body in pinned text.
    if (OS_KernelConfig->memregion_sz[MEMREGION_APPLICATION - 1] != 64u * 1024u * 1024u) return;
#endif
    if (!drop) fvgPromoReclaimPending(cache);
}
#endif
