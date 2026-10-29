// Exact-dual-mono WAVs only. Admission precedes every original WAV load/copy.
// The original cache owns the compressed linear allocation and its eviction;
// this layer owns ordinary-heap metadata and bounded DSP-visible PCM pieces.
#include "ctr_adpcm_pcm.h"
#define AS_MONO_ONLY
#define AS_FRAME_BYTES 2u
#define AS_PIECE_FRAMES 256u
#define AS_POOL_BYTES (VOICE_COUNT * UTA_RING * AS_PIECE_FRAMES * AS_FRAME_BYTES)
#include "ctr_adpcm_sfx_types.inc"

static struct {
    uint8_t* pool;
    AdSfxRing* rings;
    AdSfxMeta* metas;
    AdSfxPhysical physical[VOICE_COUNT];
    uint64_t ticks, calls, decodedFrames, maxTicks, lastLog;
    uint32_t cores, gameFrames, installed, ringBytes, ringPeak, ringFailures;
    bool coreUnavailable;
} adSfx MM_BSS;
static const char msInstalled[] MM_RO = "ADPCM MONO sound %ld: %lu -> %lu B cache; %lu frames, %lu Hz, source ch 2; %.3f ms game-thread admission; no WAV load/copy\n";
static const char msFallback[] MM_RO = "ADPCM MONO fallback sound %ld: status %d; original PCM path\n";
static const char msCost[] MM_RO = "[adpcm-mono] pieces %llu, decoded %llu frames, %.6f ms decode, %.6f ms max, core mask 0x%lX, unknown %u; %lu updates; base %lu B, extra %lu B, peak %lu B, failures %lu; installed %lu\n";

#include "ctr_adpcm_sfx_pieces.inc"

// Queueing must stay nonblocking. A projected sound that could not fit the
// synchronous preload budget is admitted on its first play instead of sending
// the original WAV to the PCM worker. This lookup performs no file I/O.
MM_CODE static bool mmSfxPlanned(int32_t soundIndex) {
    CtrAdpcmAsset asset;
    return mm.ca && mm.streams && mm.attempted && !mm.storeResult && soundIndex >= 0 &&
        CtrAdpcmCatalog_sound(&mm.store.catalog, (uint32_t)soundIndex, &asset) &&
        (asset.flags & CTR_ADPCM_ASSET_PROJECTED_MONO);
}

MM_CODE static void msCleanup(void) {
    // Original shutdown has joined workers and disabled/reset the DSP first.
    while (adSfx.metas) {
        AdSfxMeta* next = adSfx.metas->next;
        free(adSfx.metas); adSfx.metas = next;
    }
    while (adSfx.rings) {
        AdSfxRing* next = adSfx.rings->next;
        linearFree(adSfx.rings->data); free(adSfx.rings); adSfx.rings = next;
    }
    linearFree(adSfx.pool);
    memset(&adSfx, 0, sizeof(adSfx));
}

// Game thread only. -1 declines, 0 already cached, 1 newly admitted. Budget
// and floor apply to preloads; a play uses UINT32_MAX and the normal cache cap.
MM_CODE static int mmSfxAdmit(int32_t soundIndex, uint32_t budget, uint32_t floorBytes) {
    CtrAudioSystem* ca = mm.ca;
    if (!ca || !ca->ndspReady || !mm.streams || !mm.attempted || mm.storeResult ||
        ca->shutDown || !ca->base.audioGroups || soundIndex < 0 || (uint32_t)soundIndex >= ca->soundCount) return -1;
    CachedSfx* c = &ca->sfxCache[soundIndex];
    if (c->pcm) return 0;
    CtrAdpcmAsset asset;
    if (!CtrAdpcmCatalog_sound(&mm.store.catalog, (uint32_t)soundIndex, &asset) ||
        !(asset.flags & CTR_ADPCM_ASSET_PROJECTED_MONO)) return -1;
    const Sound* sound = &ca->base.audioGroups[0]->sond.sounds[soundIndex];
    // No external or cross-group alias can borrow this data.win receipt.
    if (!soundIsInAudo(sound) || sound->audioGroup != 0 || sound->audioFile < 0 ||
        (uint32_t)sound->audioFile >= ca->base.audioGroups[0]->audo.count ||
        asset.rate > 48000u || asset.frames > UINT32_MAX / 2u || asset.bytes > budget ||
        asset.frames * (asset.sourceBits / 8u) > SFX_CACHE_MAX_ENTRY_BYTES) return -1;
#ifdef ENABLE_WAD16
    if (usJobFor(soundIndex)) return -1;
#endif
    bool busy = false;
    LightLock_Lock(&ca->loadLock);
    for (unsigned i = 0; i < LOAD_SLOTS; i++)
        if (ca->loads[i].soundIndex == soundIndex && ca->loads[i].state != LOAD_FREE) busy = true;
    for (unsigned i = 0; i < PREP_SLOTS; i++)
        if (ca->preps[i].soundIndex == soundIndex && ca->preps[i].state != PREP_FREE) busy = true;
    LightLock_Unlock(&ca->loadLock);
    if (busy) return -1;
    uint32_t need = asset.bytes + (adSfx.pool ? 0 : AS_POOL_BYTES);
    if ((uint64_t)ca->cacheBytes + asset.bytes > CTR_SFX_CACHE_BYTES ||
        (uint64_t)linearSpaceFree() < (uint64_t)need + floorBytes) return -1;

    u64 started = svcGetSystemTick();
    CtrAdpcmFile file = {0};
    int reason = CtrAdpcmStore_openSound(&mm.store, (uint32_t)soundIndex, &file, &asset);
    AdSfxMeta* m = NULL;
    if (!reason) {
        m = __real_calloc(1, sizeof(*m));
        if (!m) reason = CTR_ADPCM_STORE_HEAP;
    }
    if (!reason) {
        m->data = linearAlloc(asset.bytes);
        if (!m->data) reason = CTR_ADPCM_STORE_HEAP;
        else if (!CtrAdpcmStore_readAt(&file, 0, m->data, asset.bytes) ||
            !CtrAdpcmPcm_open(&m->initial, m->data, asset.bytes) ||
            m->initial.info.channels != 1 || m->initial.info.rate != asset.rate ||
            m->initial.info.frames != asset.frames) reason = CTR_ADPCM_STORE_PAYLOAD;
    }
    CtrAdpcmStore_closeFile(&file);
    if (!reason) {
        // At most two block prefixes, never a whole-sound seam scan.
        CtrAdpcmPcm cursor = m->initial;
        int16_t ends[2];
        uint32_t n = asset.frames > 1 ? 2 : 1;
        if (!CtrAdpcmPcm_read(&cursor, 0, 1, m->first, 1, NULL) ||
            !CtrAdpcmPcm_read(&cursor, asset.frames - n, n, ends, n, NULL)) reason = CTR_ADPCM_STORE_PAYLOAD;
        else {
            m->prev[0] = ends[0]; m->last[0] = ends[n - 1];
            int32_t step = abs((int32_t)m->first[0] - m->last[0]);
            m->seam.trigger[0] = asset.sourceBits == 16 && asset.frames >= 4 * UTA_TAIL_MAX &&
                step > 0 && (uint32_t)step >= UTA_SEAM_RATIO * asset.seamMaxStep;
        }
    }
    if (!reason && !adSfx.pool) {
        adSfx.pool = linearAlloc(AS_POOL_BYTES);
        if (!adSfx.pool) reason = CTR_ADPCM_STORE_HEAP;
    }
    if (reason) {
        if (m) { linearFree(m->data); free(m); }
        CtrAdpcm_musicLegacy.post(ca, msFallback, (long)soundIndex, reason);
        return -1;
    }
    adSfxReap();
    m->cache = c;
    m->originalBytes = asset.frames * (asset.sourceBits / 8u); // baseline already projects dual mono
    LightLock_Lock(CtrAdpcm_sfxView.lock);
    m->next = adSfx.metas; adSfx.metas = m;
    c->pcm = m->data; c->bytes = asset.bytes; c->frames = asset.frames;
    c->sampleRate = asset.rate; c->sourceChannels = 2; // preserve original pan policy
    c->format = NDSP_FORMAT_MONO_PCM16; c->reload = SFX_RELOAD_AUDO_WAV;
    c->lastUse = 0; c->lastPlayTick = svcGetSystemTick();
    ca->cacheBytes += c->bytes;
    LightLock_Unlock(CtrAdpcm_sfxView.lock);
    ca->soundLengths[soundIndex] = (float)c->frames / (float)c->sampleRate;
    u64 elapsed = svcGetSystemTick() - started;
    if (budget == UINT32_MAX) { ca->windowCacheLoads++; ca->windowLoadTicks += elapsed; }
    adSfx.installed++;
    CtrAdpcm_musicLegacy.post(ca, msInstalled, (long)soundIndex, (unsigned long)m->originalBytes,
        (unsigned long)c->bytes, (unsigned long)c->frames, (unsigned long)c->sampleRate, TICKS_TO_MS(elapsed));
    return 1;
}

MM_CODE static int32_t msPlay(AudioSystem* audio, int32_t sound, int32_t priority, bool loop) {
    mmSfxAdmit(sound, UINT32_MAX, 0);
    return mm.inner->playSound(audio, sound, priority, loop);
}

MM_CODE static void msUpdate(AudioSystem* audio, float dt) {
    mm.inner->update(audio, dt);
    if (!adSfx.pool) return;
    adSfxReap();
    adSfx.gameFrames++;
    u64 now = svcGetSystemTick();
    if (now - adSfx.lastLog < UTA_LOG_TICKS) return;
    adSfx.lastLog = now;
    // Callback counters share the feeder lock. No I/O while it is held.
    LightLock_Lock(CtrAdpcm_sfxView.lock);
    uint64_t ticks = adSfx.ticks, calls = adSfx.calls, frames = adSfx.decodedFrames, maximum = adSfx.maxTicks;
    uint32_t cores = adSfx.cores;
    bool unknown = adSfx.coreUnavailable;
    LightLock_Unlock(CtrAdpcm_sfxView.lock);
    CtrAdpcm_musicLegacy.post(mm.ca, msCost, (unsigned long long)calls, (unsigned long long)frames,
        TICKS_TO_MS(ticks), TICKS_TO_MS(maximum), (unsigned long)cores, (unsigned)unknown,
        (unsigned long)adSfx.gameFrames, (unsigned long)AS_POOL_BYTES, (unsigned long)adSfx.ringBytes,
        (unsigned long)adSfx.ringPeak, (unsigned long)adSfx.ringFailures, (unsigned long)adSfx.installed);
}
