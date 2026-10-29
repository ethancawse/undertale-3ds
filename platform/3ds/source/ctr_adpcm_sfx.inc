// Included after ctr_audio_system.c: private types stay out of the engine ABI.
// First game adapter: replace idle PCM cache entries only after a verified
// background read. The original play/claim/pending/position paths stay intact.
#include "ctr_adpcm_catalog.h"
#include "ctr_adpcm_pcm.h"
#include "ctr_wrap_chain.h"
#include "sha1.h"
#include <limits.h>
#include <malloc.h>

// The normal malloc wrappers may spill into linear memory. These workspaces
// never reach the DSP, so decline conversion if the ordinary heap is full.
extern void* __real_malloc(size_t);
extern void* __real_calloc(size_t, size_t);

#define AS_BSS __attribute__((section(".bss.late.adpcm")))
#define AS_RO __attribute__((section(".rodata.late.adpcm")))
#define AS_FRAME_BYTES 4u
#define AS_PIECE_FRAMES 256u
#define AS_POOL_BYTES (VOICE_COUNT * UTA_RING * AS_PIECE_FRAMES * AS_FRAME_BYTES)

#include "ctr_adpcm_sfx_types.inc"

typedef struct {
    int state; // 0 idle, 1 queued, 2 busy, 3 ready (jobLock)
    uint32_t sound, oldBytes;
    uint8_t* old;
    CtrAdpcmAsset asset;
    char source[512], payload[512];
    AdSfxMeta* result;
} AdSfxJob;

static struct {
    CtrAudioSystem* ca;
    AudioSystemVtable outer;
    AudioSystemVtable* inner;
    uint8_t* index;
    CtrAdpcmCatalog catalog;
    uint8_t* pool;
    AdSfxRing* rings;
    AdSfxMeta* metas;
    AdSfxPhysical physical[VOICE_COUNT];
    LightLock jobLock;
    LightEvent event;
    Thread worker;
    volatile bool quit;
    AdSfxJob job;
    uint8_t* attempted;
    uint32_t scan, installed, originalBytes, compressedBytes;
    uint64_t ticks, calls, decodedFrames, maxTicks;
    uint64_t lastLog;
    uint32_t cores, gameFrames;
    uint32_t ringBytes, ringPeak, ringFailures;
    uint32_t lastMemoryCache, lastMemoryRings, peakOriginalLinear;
    bool coreUnavailable;
    char root[512], dataDir[512];
} adSfx AS_BSS;
static char adDigest[65] AS_BSS, adWin[512] AS_BSS;
static const char adIndexPath[] AS_RO = "%s/audio-adpcm/index/%s.dtai";
static const char adFilePath[] AS_RO = "%s/%s";
static const char adObjectPath[] AS_RO = "%s/audio-adpcm/objects/%s.dtad";
static const char adReadMode[] AS_RO = "rb";
static const char adFallback[] AS_RO = "ADPCM SFX DISABLED: %s; expected %s/audio-adpcm/index/%s.dtai; using PCM/Vorbis fallback\n";
static const char adMissingIndex[] AS_RO = "index missing or inaccessible (checked data and game roots)";
static const char adBadIndex[] AS_RO = "index unreadable, invalid or ordinary heap unavailable";
static const char adHex[] AS_RO = "0123456789abcdef";
static const char adEnabled[] AS_RO = "ADPCM SFX: verified index; %lu B base PCM pieces; cache worker requested core %d\n";
static const char adInstalled[] AS_RO = "ADPCM SFX: sound %lu cache %lu -> %lu B; totals %lu -> %lu B; %lu entries; worker scan %.6f ms, %lu source frames (wall)\n";
static const char adCost[] AS_RO = "[adpcm-sfx] reads %llu, decoded %llu source frames, %.6f ms total, %.6f ms max, core mask 0x%lX; scratch %lu B; game frames %lu; core unavailable %u; ring peak %lu B; ring failures %lu\n";
static const char adMemoryLog[] AS_RO = "[adpcm-memory] cache_linear %lu B, original_equivalent_linear %lu B, scratch_linear %lu B, metadata_heap_usable %lu B, temporary_linear %lu B; accounted_linear_delta %ld B; process_heap_used %lu B, process_linear_free %lu B; original_peak %lu B, worker_state %d, ring_failures %lu\n";

#include "ctr_adpcm_sfx_pieces.inc"

// Game-thread snapshots include allocator rounding and classify malloc's
// possible linear fallback. Global heap use includes worker stacks and
// in-flight allocations not yet published as job.result. The equivalent
// cache comparison is explicitly separate from a paired process A/B.
extern u32 __ctru_heap, __ctru_heap_size, __ctru_linear_heap, __ctru_linear_heap_size;
AS_CODE static void adFootprint(const void* ptr, uint32_t* heap, uint32_t* linear) {
    uintptr_t at = (uintptr_t)ptr;
    if (!ptr) return;
    if (at >= __ctru_linear_heap && at - __ctru_linear_heap < __ctru_linear_heap_size)
        *linear += (uint32_t)linearGetSize((void*)ptr);
    else if (at >= __ctru_heap && at - __ctru_heap < __ctru_heap_size)
        *heap += (uint32_t)malloc_usable_size((void*)ptr);
}

AS_CODE static void adMemory(CtrAudioSystem* ca) {
    if (ca->cacheBytes == adSfx.lastMemoryCache && adSfx.ringBytes == adSfx.lastMemoryRings) return;
    adSfx.lastMemoryCache = ca->cacheBytes;
    adSfx.lastMemoryRings = adSfx.ringBytes;
    uint32_t payload = 0, equivalent = 0, heap = 0, scratch = 0, temporary = 0;
    LightLock_Lock(&adSfx.jobLock);
    LightLock_Lock(CtrAdpcm_sfxView.lock);
    for (uint32_t i = 0; i < ca->soundCount; i++) {
        CachedSfx* c = &ca->sfxCache[i];
        if (!c->pcm) continue;
        uint32_t bytes = (uint32_t)linearGetSize(c->pcm);
        AdSfxMeta* m = adMeta(c);
        payload += bytes;
        equivalent += m ? m->originalLinearBytes : bytes;
    }
    adFootprint(adSfx.pool, &heap, &scratch);
    adFootprint(adSfx.index, &heap, &scratch);
    adFootprint(adSfx.attempted, &heap, &scratch);
    for (AdSfxMeta* m = adSfx.metas; m; m = m->next) adFootprint(m, &heap, &scratch);
    for (AdSfxRing* ring = adSfx.rings; ring; ring = ring->next) {
        adFootprint(ring, &heap, &scratch);
        adFootprint(ring->data, &heap, &scratch);
    }
    if (adSfx.job.result) {
        adFootprint(adSfx.job.result, &heap, &temporary);
        adFootprint(adSfx.job.result->data, &heap, &temporary);
    }
    int state = adSfx.job.state;
    LightLock_Unlock(CtrAdpcm_sfxView.lock);
    LightLock_Unlock(&adSfx.jobLock);
    if (equivalent > adSfx.peakOriginalLinear) adSfx.peakOriginalLinear = equivalent;
    logInfo(adMemoryLog, (unsigned long)payload, (unsigned long)equivalent, (unsigned long)scratch,
        (unsigned long)heap, (unsigned long)temporary, (long)((int32_t)equivalent - (int32_t)payload - (int32_t)scratch - (int32_t)temporary),
        (unsigned long)mallinfo().uordblks, (unsigned long)linearSpaceFree(),
        (unsigned long)adSfx.peakOriginalLinear, state, (unsigned long)adSfx.ringFailures);
}

AS_CODE static bool adFileHash(FILE* file, uint64_t offset, uint32_t bytes, const uint8_t expected[20]) {
    if (offset > LONG_MAX || fseek(file, (long)offset, SEEK_SET) != 0) return false;
    SHA1_CTX hash;
    SHA1Init(&hash);
    uint8_t buffer[4096], actual[20];
    while (bytes) {
        uint32_t n = bytes < sizeof(buffer) ? bytes : sizeof(buffer);
        if (fread(buffer, 1, n, file) != n) return false;
        SHA1Update(&hash, buffer, n);
        bytes -= n;
    }
    SHA1Final(actual, &hash);
    return memcmp(actual, expected, 20) == 0;
}

AS_CODE static AdSfxMeta* adLoad(const AdSfxJob* job) {
    FILE* source = fopen(job->source, adReadMode);
    bool ok = source && adFileHash(source, job->asset.sourceOffset, job->asset.sourceBytes, job->asset.sourceSha1);
    if (ok && !(job->asset.flags & CTR_ADPCM_ASSET_EMBEDDED))
        ok = job->asset.sourceOffset == 0 && job->asset.sourceBytes <= LONG_MAX &&
            fseek(source, 0, SEEK_END) == 0 && ftell(source) == (long)job->asset.sourceBytes;
    if (source) fclose(source);
    if (!ok) return NULL;
    // These buffers never reach the DSP. Decline conversion if the heap
    // cannot hold them; do not spill private decode work into tile memory.
    AdSfxMeta* m = __real_calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->data = __real_malloc(job->asset.bytes);
    FILE* file = fopen(job->payload, adReadMode);
    ok = m->data && file && adFileHash(file, 0, job->asset.bytes, job->asset.payloadSha1) &&
        job->asset.bytes <= LONG_MAX && fseek(file, 0, SEEK_END) == 0 &&
        ftell(file) == (long)job->asset.bytes && fseek(file, 0, SEEK_SET) == 0 &&
        fread(m->data, 1, job->asset.bytes, file) == job->asset.bytes &&
        CtrAdpcmPcm_open(&m->initial, m->data, job->asset.bytes) &&
        m->initial.info.frames == job->asset.frames && m->initial.info.rate == job->asset.rate &&
        m->initial.info.channels == job->asset.channels;
    if (file) fclose(file);
    if (!ok) { free(m->data); free(m); return NULL; }
    CtrAdpcmPcm cursor = m->initial;
    int32_t maxStep[2] = {0};
    int16_t previous[2] = {0};
    uint64_t scanStart = svcGetSystemTick();
    for (uint32_t first = 0; first < cursor.info.frames;) {
        int16_t pcm[256 * 2];
        uint32_t n = cursor.info.frames - first;
        if (n > 256) n = 256;
        if (!CtrAdpcmPcm_read(&cursor, first, n, pcm, n, NULL)) { free(m->data); free(m); return NULL; }
        for (uint32_t j = 0; j < n; j++) for (uint32_t ch = 0; ch < cursor.info.channels; ch++) {
            int16_t value = pcm[j * cursor.info.channels + ch];
            if (first + j == 0) m->first[ch] = value;
            else {
                int32_t delta = abs((int32_t)value - previous[ch]);
                if (delta > maxStep[ch]) maxStep[ch] = delta;
            }
            m->prev[ch] = previous[ch];
            m->last[ch] = previous[ch] = value;
        }
        first += n;
    }
    m->scanTicks = svcGetSystemTick() - scanStart;
    for (unsigned ch = 0; ch < cursor.info.channels; ch++) {
        int32_t step = abs((int32_t)m->first[ch] - m->last[ch]);
        m->seam.trigger[ch] = step > 0 && step >= UTA_SEAM_RATIO * maxStep[ch];
    }
    return m;
}

AS_CODE static void adWorker(void* unused) {
    (void)unused;
    while (!adSfx.quit) {
        LightLock_Lock(&adSfx.jobLock);
        bool queued = adSfx.job.state == 1;
        if (queued) adSfx.job.state = 2;
        LightLock_Unlock(&adSfx.jobLock);
        if (queued) {
            AdSfxMeta* result = adLoad(&adSfx.job);
            LightLock_Lock(&adSfx.jobLock);
            adSfx.job.result = result;
            adSfx.job.state = 3;
            LightLock_Unlock(&adSfx.jobLock);
        } else LightEvent_WaitTimeout(&adSfx.event, DECODE_IDLE_TIMEOUT_NS);
    }
}

AS_CODE static void adReleaseMeta(AdSfxMeta* m) { if (m) { free(m->data); free(m); } }

AS_CODE static void adCleanup(void) {
    adSfx.quit = true;
    LightEvent_Signal(&adSfx.event);
    if (adSfx.worker) { threadJoin(adSfx.worker, U64_MAX); threadFree(adSfx.worker); adSfx.worker = NULL; }
    adReleaseMeta(adSfx.job.result);
    adSfx.job.result = NULL;
    // Cache payloads are owned/freed by the original audio shutdown. The
    // list retains only metadata for deferred tails, never a second owner.
    for (AdSfxMeta* m = adSfx.metas; m;) { AdSfxMeta* next = m->next; free(m); m = next; }
    adSfx.metas = NULL;
    for (AdSfxRing* ring = adSfx.rings; ring;) {
        AdSfxRing* next = ring->next;
        linearFree(ring->data); free(ring); ring = next;
    }
    adSfx.rings = NULL;
    adSfx.ringBytes = 0;
    linearFree(adSfx.pool);
    adSfx.pool = NULL;
    free(adSfx.index); adSfx.index = NULL;
    free(adSfx.attempted); adSfx.attempted = NULL;
}

AS_CODE static void adDestroy(AudioSystem* audio) {
    // Join the file-only worker before freeing the audio system or index.
    adSfx.quit = true;
    LightEvent_Signal(&adSfx.event);
    if (adSfx.worker) { threadJoin(adSfx.worker, U64_MAX); threadFree(adSfx.worker); adSfx.worker = NULL; }
    adSfx.inner->destroy(audio);
    adCleanup();
    adSfx.ca = NULL;
}

AS_CODE static void adInstall(CtrAudioSystem* ca) {
    AdSfxJob* job = &adSfx.job;
    AdSfxMeta* m = job->result;
    if (!m) return;
    CachedSfx* c = &ca->sfxCache[job->sound];
    bool live = false;
    for (unsigned i = 0; i < VOICE_COUNT; i++) if (ca->voices[i].kind == VOICE_SFX && ca->voices[i].sfx == c) live = true;
    LightLock_Lock(CtrAdpcm_sfxView.lock);
    // Original PCM tails can still borrow this cache even after its logical
    // voice became free. Wait for that physical lease before replacing it.
    for (unsigned p = 0; p < VOICE_COUNT; p++) if (CtrAdpcm_sfxView.channels[p].state != UTA_IDLE)
        for (unsigned s = 0; s < CtrAdpcm_sfxView.channels[p].segs; s++) {
            uintptr_t address = (uintptr_t)CtrAdpcm_sfxView.channels[p].seg[s].data;
            if (address >= (uintptr_t)job->old && address < (uintptr_t)job->old + job->oldBytes) live = true;
        }
    if (!live && c->pcm == job->old && c->bytes == job->oldBytes) {
        uint8_t* resident = linearAlloc(job->asset.bytes);
        if (resident) {
            memcpy(resident, m->data, job->asset.bytes);
            free(m->data);
            m->data = resident;
            m->initial.data = resident;
            // Stored channel pointers belonged to the old malloc source;
            // a fresh open also verifies the installed linear copy.
            if (!CtrAdpcmPcm_open(&m->initial, resident, job->asset.bytes)) abort();
            m->originalLinearBytes = (uint32_t)linearGetSize(c->pcm);
            linearFree(c->pcm);
            ca->cacheBytes = ca->cacheBytes - c->bytes + job->asset.bytes;
            m->originalBytes = c->bytes;
            c->pcm = resident;
            c->bytes = job->asset.bytes;
            m->cache = c;
            m->next = adSfx.metas;
            adSfx.metas = m;
            adSfx.originalBytes += m->originalBytes;
            adSfx.compressedBytes += c->bytes;
            adSfx.installed++;
            adSfx.attempted[job->sound] = 2;
            job->result = NULL;
        }
    }
    LightLock_Unlock(CtrAdpcm_sfxView.lock);
    if (!job->result) logInfo(adInstalled, (unsigned long)job->sound, (unsigned long)m->originalBytes,
        (unsigned long)c->bytes, (unsigned long)adSfx.originalBytes, (unsigned long)adSfx.compressedBytes,
        (unsigned long)adSfx.installed, TICKS_TO_MS(m->scanTicks), (unsigned long)m->initial.info.frames);
}

AS_CODE static void adUpdate(AudioSystem* audio, float dt) {
    adSfx.inner->update(audio, dt);
    CtrAudioSystem* ca = (CtrAudioSystem*)audio;
    if (!adSfx.worker || ca->shutDown) return;
    adSfxReap();
    adSfx.gameFrames++;
    LightLock_Lock(&adSfx.jobLock);
    if (adSfx.job.state == 3) {
        adInstall(ca);
        // A play or eviction may have won the race with the read. Release
        // the temporary result and retry only once this sound is idle again;
        // a long loop must not hold the sole worker job and its heap buffer.
        if (adSfx.job.result) {
            adReleaseMeta(adSfx.job.result);
            adSfx.job.result = NULL;
            adSfx.attempted[adSfx.job.sound] = 0;
        }
        adSfx.job.state = 0;
    }
    if (adSfx.job.state == 0) {
        for (uint32_t scanned = 0; scanned < ca->soundCount; scanned++) {
            uint32_t i = adSfx.scan++ % ca->soundCount;
            CachedSfx* c = &ca->sfxCache[i];
            if (adSfx.attempted[i] == 2 && !adMeta(c)) adSfx.attempted[i] = 0;
            if (adSfx.attempted[i] || !c->pcm || adMeta(c)) continue;
            bool live = false;
            for (unsigned v = 0; v < VOICE_COUNT; v++)
                if (ca->voices[v].kind == VOICE_SFX && ca->voices[v].sfx == c) live = true;
            if (live) continue;
            adSfx.attempted[i] = 1;
            CtrAdpcmAsset a;
            unsigned channels = c->format & 3;
            if (((c->format >> 2) & 3) != 1 || !CtrAdpcmCatalog_sound(&adSfx.catalog, i, &a) ||
                a.frames != c->frames || a.rate != c->sampleRate || a.rate > 48000 || a.bytes >= c->bytes ||
                (channels != a.channels && !(channels == 1 && a.channels == 2 && (a.flags & CTR_ADPCM_ASSET_DUAL_MONO)))) continue;
            AdSfxJob* job = &adSfx.job;
            job->sound = i; job->old = c->pcm; job->oldBytes = c->bytes; job->asset = a;
            const char* name = a.path;
            const char* root = adSfx.root;
            if (a.flags & CTR_ADPCM_ASSET_EMBEDDED) {
                const char* slash = strrchr(name, '/');
                if (slash) name = slash + 1;
                root = adSfx.dataDir;
            }
            char key[65];
            for (unsigned k = 0; k < 32; k++) { key[k * 2] = adHex[a.key[k] >> 4]; key[k * 2 + 1] = adHex[a.key[k] & 15]; }
            key[64] = 0;
            int src = snprintf(job->source, sizeof(job->source), adFilePath, root, name);
            int dst = snprintf(job->payload, sizeof(job->payload), adObjectPath, adSfx.root, key);
            if (src < 0 || src >= (int)sizeof(job->source) || dst < 0 || dst >= (int)sizeof(job->payload)) continue;
            job->state = 1;
            LightEvent_Signal(&adSfx.event);
            break;
        }
    }
    LightLock_Unlock(&adSfx.jobLock);
    adMemory(ca);
    uint64_t now = svcGetSystemTick();
    if (now - adSfx.lastLog >= 5ULL * SYSCLOCK_ARM11) {
        LightLock_Lock(CtrAdpcm_sfxView.lock);
        uint64_t ticks = adSfx.ticks, calls = adSfx.calls, frames = adSfx.decodedFrames, maximum = adSfx.maxTicks;
        uint32_t cores = adSfx.cores;
        bool coreUnavailable = adSfx.coreUnavailable;
        adSfx.ticks = adSfx.calls = adSfx.decodedFrames = adSfx.maxTicks = adSfx.cores = 0;
        LightLock_Unlock(CtrAdpcm_sfxView.lock);
        logInfo(adCost, (unsigned long long)calls, (unsigned long long)frames, TICKS_TO_MS(ticks),
            TICKS_TO_MS(maximum), (unsigned long)cores, (unsigned long)(AS_POOL_BYTES + adSfx.ringBytes),
            (unsigned long)adSfx.gameFrames, (unsigned)coreUnavailable,
            (unsigned long)(AS_POOL_BYTES + adSfx.ringPeak), (unsigned long)adSfx.ringFailures);
        adSfx.gameFrames = 0;
        adSfx.lastLog = now;
    }
}

AS_CODE static void adInit(AudioSystem* audio, DataWin* dw, FileSystem* fs) {
    adSfx.inner->init(audio, dw, fs);
    CtrAudioSystem* ca = (CtrAudioSystem*)audio;
    if (!ca->ndspReady || !*CtrAdpcm_sfxView.active || !*CtrAdpcm_sfxView.tail || !*CtrAdpcm_sfxView.seam || !adDigest[0] ||
        strcmp(ca->dataWinPath ? ca->dataWinPath : adHex, adWin)) return;
    uint8_t sha[32];
    for (unsigned i = 0; i < 32; i++) {
        const char* a = strchr(adHex, adDigest[i * 2]);
        const char* b = strchr(adHex, adDigest[i * 2 + 1]);
        if (!a || !b) return;
        sha[i] = (uint8_t)((a - adHex) * 16 + b - adHex);
    }
    size_t length = strlen(adWin);
    if (length >= sizeof(adSfx.dataDir)) return;
    memcpy(adSfx.dataDir, adWin, length + 1);
    char* slash = strrchr(adSfx.dataDir, '/');
    if (!slash) return;
    *slash = 0;
    memcpy(adSfx.root, adSfx.dataDir, strlen(adSfx.dataDir) + 1);
    FILE* file = NULL;
    for (unsigned depth = 0; depth < 2 && !file; depth++) {
        char path[640];
        snprintf(path, sizeof(path), adIndexPath, adSfx.root, adDigest);
        file = fopen(path, adReadMode);
        if (!file && depth == 0) { char* end = strrchr(adSfx.root, '/'); if (end) *end = 0; }
    }
    if (!file) { logWarn(adFallback, adMissingIndex, adSfx.root, adDigest); return; }
    bool ok = fseek(file, 0, SEEK_END) == 0;
    long bytes = ok ? ftell(file) : -1;
    ok = ok && bytes >= 128 && (unsigned long)bytes <= CTR_ADPCM_INDEX_LIMIT && fseek(file, 0, SEEK_SET) == 0;
    if (ok) adSfx.index = __real_malloc((size_t)bytes);
    ok = ok && adSfx.index && fread(adSfx.index, 1, (size_t)bytes, file) == (size_t)bytes &&
        CtrAdpcmCatalog_parse(adSfx.index, (uint32_t)bytes, sha, ca->soundCount, &adSfx.catalog);
    fclose(file);
    if (!ok) {
        logWarn(adFallback, adBadIndex, adSfx.root, adDigest);
        free(adSfx.index); adSfx.index = NULL; return;
    }
    adSfx.pool = linearAlloc(AS_POOL_BYTES);
    adSfx.attempted = __real_calloc(ca->soundCount ? ca->soundCount : 1, 1);
    if (!adSfx.pool || !adSfx.attempted) { adCleanup(); return; }
    adSfx.worker = threadCreate(adWorker, NULL, 32 * 1024, ca->threadPriority < 0x3f ? ca->threadPriority + 1 : 0x3f,
                                ca->threadCore == 0 ? -2 : ca->threadCore, false);
    if (!adSfx.worker) { adCleanup(); return; }
    adSfx.lastMemoryCache = UINT32_MAX;
    logInfo(adEnabled, (unsigned long)AS_POOL_BYTES, ca->threadCore);
    adMemory(ca);
}

CtrAudioSystem* __real_CtrAudioSystem_create(void);
AS_CODE CtrAudioSystem* __wrap_CtrAudioSystem_create(void) {
    CtrAudioSystem* ca = __real_CtrAudioSystem_create();
    memset(&adSfx, 0, sizeof(adSfx));
    adSfx.ca = ca;
    adSfx.inner = ca->base.vtable;
    adSfx.outer = *adSfx.inner;
    adSfx.outer.init = adInit;
    adSfx.outer.update = adUpdate;
    adSfx.outer.destroy = adDestroy;
    ca->base.vtable = &adSfx.outer;
    LightLock_Init(&adSfx.jobLock);
    LightEvent_Init(&adSfx.event, RESET_ONESHOT);
    atexit(adCleanup); // registered before the original init's NDSP shutdown
    return ca;
}

AS_CODE CtrDataVersionResult CtrDataVersion_chainAdpcm(CTR_DATAVERSION_CHAIN_PARAMS) {
    CtrDataVersionResult result = CTR_DATAVERSION_AFTER_ADPCM(path, game, part, cachePath, progress, cached, digest);
    adWin[0] = adDigest[0] = 0;
    if (result != CTR_DATA_UNREADABLE && path && digest && strlen(path) < sizeof(adWin) && strlen(digest) == 64) {
        memcpy(adWin, path, strlen(path) + 1);
        memcpy(adDigest, digest, 65);
    }
    return result;
}
