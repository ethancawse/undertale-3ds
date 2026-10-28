// Old-model palette retry only. Included after the original translation
// unit so existing __LINE__ values and compiler-local labels stay intact.
#ifdef CTRPORT_TARGET_O3DS
extern bool __real_CtrTextures_lendBudget(CtrTextureCache* cache, uint32_t bytes, uint32_t idleFrames);
extern void __real_CtrTextures_beginFrame(CtrTextureCache* cache, uint32_t frame);

static struct {
    CtrTextureCache* cache;
    uint32_t frame;
    uint32_t retryFrom;
    uint32_t cursor;
} paletteRetryFailed __attribute__((section(".bss.late.thrashport")));

__attribute__((noipa, section(".text.late.thrashport")))
bool __wrap_CtrTextures_lendBudget(CtrTextureCache* cache, uint32_t bytes, uint32_t idleFrames) {
    bool ok = __real_CtrTextures_lendBudget(cache, bytes, idleFrames);
    if (!ok) {
        if (paletteRetryFailed.cache != cache || cache->frame < paletteRetryFailed.frame) {
            paletteRetryFailed.retryFrom = cache->frame + 1u;
            paletteRetryFailed.cursor = 0;
        }
        paletteRetryFailed.cache = cache;
        paletteRetryFailed.frame = cache->frame;
    }
    return ok;
}

__attribute__((noipa, section(".text.late.thrashport")))
void __wrap_CtrTextures_beginFrame(CtrTextureCache* cache, uint32_t frame) {
#ifdef CTRPORT_DR_OLD_CIA80_TILES
    // DR palette admission now evicts decoded tiles directly. It can also
    // fail at the palette cap or at TexInit, neither of which reaches the
    // old lendBudget failure hook. Inspect empty backoff records after frame
    // maintenance instead, at most one reopening every four renderer frames.
    __asm__ volatile("@ flowervramg-early-retry");
    if (paletteRetryFailed.cache != cache || frame <= cache->frame) {
        paletteRetryFailed.cache = cache;
        paletteRetryFailed.retryFrom = frame;
        paletteRetryFailed.cursor = 0;
    }
    paletteRetryFailed.frame = frame;
    __real_CtrTextures_beginFrame(cache, frame);
    if (frame < paletteRetryFailed.retryFrom) return;
#else
    bool retry = paletteRetryFailed.cache == cache && paletteRetryFailed.frame <= cache->frame &&
        cache->frame - paletteRetryFailed.frame < PAL_RETRY_FRAMES && frame > cache->frame;
    // A renderer recreated at a chapter change starts its cache at frame 0;
    // a loan failure from its predecessor must not reopen the new records.
    if (!retry) paletteRetryFailed.cache = nullptr;
    __real_CtrTextures_beginFrame(cache, frame);
    if (!retry || cache->roomThrashed || frame < paletteRetryFailed.retryFrom) return;
#endif
    CtrRenderer* r = (CtrRenderer*) ((char*) cache - __builtin_offsetof(CtrRenderer, textures));
    // Frame-start lending retry rule: cache maintenance and deferred frees
    // can make yesterday's refused loan possible before the 30-frame delay.
    // Bounded early-retry rule: reopen at most one empty record per four
    // frames, rotating through records so failed copies cannot retry en masse
    // each frame. Ordinary 30-frame retries retain their original schedule.
    // Only a draw that needs the reopened copy allocates it.
    // Reopening alone neither allocates nor evicts. Ordinary targets keep
    // their lender/thrashing rules; CIA80 applies palette priority
    // while protecting tiles referenced by this frame's GPU work.
    uint32_t count = (uint32_t) arrlen(r->palCopies);
    for (uint32_t n = 0; count > n; n++) {
        uint32_t i = (paletteRetryFailed.cursor + n) % count;
        CtrPalCopy* copy = r->palCopies[i];
        if (0 <= copy->pageId && copy->bytes == 0 && copy->lastUsedFrame > frame &&
            copy->lastUsedFrame - frame <= PAL_RETRY_FRAMES) {
            copy->lastUsedFrame = frame;
            paletteRetryFailed.cursor = (i + 1u) % count;
            paletteRetryFailed.retryFrom = frame + 4u;
            break;
        }
    }
}
#endif
