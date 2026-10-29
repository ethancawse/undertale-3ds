// Shared production feeder pieces, leases and tails. No file I/O in callbacks.
AS_CODE static AdSfxMeta* adMeta(const CachedSfx* c) {
    for (AdSfxMeta* m = adSfx.metas; m; m = m->next)
        if (m->cache == c && m->data == c->pcm && m->initial.bytes == c->bytes) return m;
    return NULL;
}

AS_CODE static bool adRead(AdSfxPhysical* p, uint32_t first, uint32_t count, int16_t* out, unsigned channels) {
#ifdef AS_MONO_ONLY
    if (channels != 1 || p->cursor.info.channels != 1) return false;
    uint64_t start = svcGetSystemTick();
    uint32_t work = 0;
    bool ok = CtrAdpcmPcm_read(&p->cursor, first, count, out, count, &work);
#else
    if (channels != p->cursor.info.channels && !(channels == 1 && p->cursor.info.channels == 2)) return false;
    uint64_t start = svcGetSystemTick();
    uint32_t work = 0;
    bool ok = true;
    if (channels == p->cursor.info.channels) {
        ok = CtrAdpcmPcm_read(&p->cursor, first, count, out, count, &work);
        count = 0;
    }
    // Identical-channel sources may retain the old cache's mono projection.
    // The source-channel policy used by applyVoice is unchanged.
    while (count && ok) {
        int16_t temp[256 * 2];
        uint32_t n = count < 256 ? count : 256, decoded = 0;
        ok = CtrAdpcmPcm_read(&p->cursor, first, n, temp, n, &decoded);
        work += decoded;
        if (ok) for (uint32_t j = 0; j < n; j++) out[j] = temp[j * 2];
        out += n;
        first += n;
        count -= n;
    }
#endif
    uint64_t ticks = svcGetSystemTick() - start;
    adSfx.ticks += ticks;
    adSfx.calls++;
    adSfx.decodedFrames += work;
    if (ticks > adSfx.maxTicks) adSfx.maxTicks = ticks;
    // Some Azahar versions do not implement this SVC. Preserve "unknown"
    // and avoid thousands of repeated host errors; never infer a core ID.
    if (!adSfx.coreUnavailable) {
        int core = svcGetProcessorID();
        if (core >= 0 && core < 32) adSfx.cores |= 1u << core;
        else adSfx.coreUnavailable = true;
    }
    return ok;
}

AS_CODE static void adSfxForget(int p) {
    adSfx.physical[p].meta = NULL;
    adSfx.physical[p].ring = NULL;
    adSfx.physical[p].tailReady = false;
}

// Game thread, under the feeder lock. Each slot holds at least one DSP
// frame at the requested source rate, so seven live slots can cover the
// original three-frame lookahead even at pitch 8. Old generations remain
// pinned by their wave buffers until adSfxReap runs on the game thread.
AS_CODE static void adSfxReserve(int p, float rate) {
    AdSfxPhysical* a = &adSfx.physical[p];
    if (!a->meta) return;
    uint32_t need = (uint32_t)(160.0 * (double)rate / UTA_DSP_RATE) + 1u;
    uint32_t capacity = a->ring ? a->ring->frames : AS_PIECE_FRAMES;
    if (need <= capacity) return;
    // Converted sources are <=48 kHz and the engine clamps pitch to 8.
    // Reject an invalid rate rather than overflowing a size calculation.
    if (need > 2048u) { adSfx.ringFailures++; return; }
    AdSfxRing* ring = __real_calloc(1, sizeof(*ring));
    if (!ring) { adSfx.ringFailures++; return; }
    ring->frames = (need + 31u) & ~31u;
    ring->bytes = UTA_RING * ring->frames * AS_FRAME_BYTES;
    ring->data = linearAlloc(ring->bytes);
    if (!ring->data) { free(ring); adSfx.ringFailures++; return; }
    ring->next = adSfx.rings;
    adSfx.rings = ring;
    a->ring = ring;
    adSfx.ringBytes += ring->bytes;
    if (adSfx.ringBytes > adSfx.ringPeak) adSfx.ringPeak = adSfx.ringBytes;
}

AS_CODE static void adSfxRate(int p, float rate) {
    if (!adSfx.pool || p < 0 || p >= VOICE_COUNT) return;
    LightLock_Lock(CtrAdpcm_sfxView.lock);
    adSfxReserve(p, rate);
    LightLock_Unlock(CtrAdpcm_sfxView.lock);
}

AS_CODE static void adSfxReap(void) {
    LightLock_Lock(CtrAdpcm_sfxView.lock);
    for (AdSfxRing** link = &adSfx.rings; *link;) {
        AdSfxRing* ring = *link;
        bool live = false;
        for (unsigned p = 0; p < VOICE_COUNT; p++) {
            if (adSfx.physical[p].ring == ring) live = true;
            for (unsigned k = 0; k < UTA_RING; k++) {
                const ndspWaveBuf* w = &CtrAdpcm_sfxView.buffers[p][k];
                uintptr_t address = (uintptr_t)w->data_vaddr;
                if (utaLive(w) && address >= (uintptr_t)ring->data &&
                    address < (uintptr_t)ring->data + ring->bytes) live = true;
            }
        }
        if (live) { link = &ring->next; continue; }
        *link = ring->next;
        adSfx.ringBytes -= ring->bytes;
        linearFree(ring->data);
        free(ring);
    }
    // Old cache metadata is retained only while a deferred physical tail
    // references it. No payload is freed here: the original cache owns it.
    for (AdSfxMeta** link = &adSfx.metas; *link;) {
        AdSfxMeta* m = *link;
        bool live = m->cache->pcm == m->data && m->cache->bytes == m->initial.bytes;
        for (unsigned p = 0; p < VOICE_COUNT; p++) if (adSfx.physical[p].meta == m) live = true;
        if (live) { link = &m->next; continue; }
        *link = m->next;
        free(m);
    }
    LightLock_Unlock(CtrAdpcm_sfxView.lock);
}

AS_CODE static void adSfxSegment(int p, UtaChan* c, Voice* v, ndspWaveBuf* wb) {
    AdSfxPhysical* a = &adSfx.physical[p];
    if (c->segs == 0) {
        a->meta = adMeta(v->sfx);
        a->tailReady = false;
        if (a->meta) {
            a->cursor = a->meta->initial;
            adSfxReserve(p, c->rate);
        }
    }
    a->start[c->segs] = v->wbStart[wb == &v->wb[1] ? 1 : 0];
}

AS_CODE static const uint8_t* adSfxPiece(int p, const UtaChan* c, const UtaSeg* g, uint32_t* frames) {
    AdSfxPhysical* a = &adSfx.physical[p];
    if (!a->meta) return g->data + c->off * c->bytes;
    uint32_t capacity = a->ring ? a->ring->frames : AS_PIECE_FRAMES;
    uint8_t* data = a->ring ? a->ring->data : adSfx.pool + (uint32_t)p * UTA_RING * AS_PIECE_FRAMES * AS_FRAME_BYTES;
    if (*frames > capacity) *frames = capacity;
    for (unsigned k = 0; k < UTA_RING; k++) {
        unsigned slot = (c->ring + k) % UTA_RING;
        // Exactly the slot utaQueue will choose. Its previous generation
        // can be reused only after that same DSP lease has ended.
        if (utaLive(&CtrAdpcm_sfxView.buffers[p][slot])) continue;
        uint8_t* dst = data + slot * capacity * AS_FRAME_BYTES;
        if (!adRead(a, a->start[c->cur] + c->off, *frames, (int16_t*)dst, c->chans)) return NULL;
        DSP_FlushDataCache(dst, *frames * c->bytes);
        return dst;
    }
    // No overwrite of PCM still owned by the DSP. The feeder retries next callback.
    return NULL;
}

AS_CODE static const UtaSeamSound* adSfxSeam(const Voice* v) {
    AdSfxMeta* m = adMeta(v->sfx);
    return m ? &m->seam : NULL;
}

AS_CODE static bool adSfxOffsets(const UtaChan* c, float out[2]) {
    AdSfxMeta* m = adSfx.physical[c - CtrAdpcm_sfxView.channels].meta;
    if (!m) return false;
    for (unsigned k = 0; k < 2; k++) out[k] = k < c->chans && c->trigger[k] ?
        (float)(2 * (int32_t)m->last[k] - m->prev[k] - m->first[k]) : 0.0f;
    return true;
}

AS_CODE static const int16_t* adSfxSeamPcm(int p, const UtaChan* c, const UtaSeg* g,
                                         uint32_t frames, int16_t* dst) {
    AdSfxPhysical* a = &adSfx.physical[p];
    if (!a->meta) return (const int16_t*)g->data;
    if (!adRead(a, a->start[c->cur], frames, dst, c->chans)) memset(dst, 0, frames * c->bytes);
    return dst;
}

AS_CODE static void adSfxCaptureTail(int p, const UtaChan* c) {
    AdSfxPhysical* a = &adSfx.physical[p];
    if (!a->meta || c->state != UTA_FEED || c->paused) return;
    uint32_t n = (uint32_t)(c->rate * UTA_TAIL_SECONDS + 0.5f);
    if (n < 8) n = 8;
    if (n > UTA_TAIL_MAX) n = UTA_TAIL_MAX;
    UtaChan cursor = *c;
    int16_t* dst = (int16_t*)(*CtrAdpcm_sfxView.tail + (uint32_t)p * UTA_TAIL_MAX * 4u);
    for (uint32_t done = 0; done < n;) {
        if (!utaAdvance(&cursor)) { memset(dst, 0, (n - done) * c->bytes); break; }
        uint32_t count = cursor.seg[cursor.cur].frames - cursor.off;
        if (count > n - done) count = n - done;
        if (!count) { memset(dst, 0, (n - done) * c->bytes); break; }
        if (!adRead(a, a->start[cursor.cur] + cursor.off, count, dst, c->chans))
            memset(dst, 0, count * c->bytes);
        cursor.off += count;
        dst += count * c->chans;
        done += count;
    }
    // Tail PCM is captured while the game still owns the cache entry. Its
    // subsequent eviction cannot invalidate the deferred callback's reads.
    a->tailReady = true;
}

AS_CODE static const uint8_t* adSfxTailSample(int p, const UtaChan* c, const UtaSeg* g, uint32_t j) {
    if (adSfx.physical[p].tailReady) return *CtrAdpcm_sfxView.tail + (uint32_t)p * UTA_TAIL_MAX * 4u + j * c->bytes;
    return g->data + c->off * c->bytes;
}

