// UNDERTALE's exact-dual-mono sound effects as DSP-ADPCM, from one pack beside data.win (adpcm-sfx.dtsp).
//
// The builder makes the pack from the player's data.win (tools/audio/sfxpack.py): a 128 B header, a table of
// 28 B rows sorted by sound index, then each sound's DTAD v1 payload (one channel, whole 32-bit words). At audio
// startup (boot, never a play frame) the game thread opens it once, checks it and keeps it open:
//   - the catalog id: the pack format and the mono projection policy this runtime decodes (apCatalog);
//   - the data.win SHA-256 the builder hashed, against the boot data check's digest of the data.win in use;
//   - the exact pack size, and every row inside it.
// Each install then checks its row's first 24 B and the payload bytes it has just read against the row's check
// (UT-012: a zeroed, stale or damaged payload passes the DTAD block checks and would play silence or noise; c26rev
// F3: a changed sound ID, offset or length would play another sound's payload); a mismatch plays that one sound
// from its original PCM, counts "bad checks" and logs the sound (msBad).
// No timestamps and no receipts, so a pack copied by FTP works. Anything wrong turns the pack off for the session
// and every sound takes its original PCM path.
// Planned sounds come in through utsfx: the room change's preload reads the payload on the game thread (the frame
// that isn't shown) and the copy worker reads the rest through its own descriptor; the game thread only installs
// them. A sound played before either has it costs one seek and read of its payload on the open pack: no open or
// stat in a play frame. The cached entry is the payload; the existing PCM feeder decodes it in pieces (pieces.inc)
// and keeps the source's channel count (2) for the original pan policy.
#include "ctr_adpcm_pcm.h"
#include "ctr_wrap_chain.h"

#define MM_RO __attribute__((section(".rodata.late.adpcm")))
#define MM_BSS __attribute__((section(".bss.late.adpcm")))
extern void* __real_calloc(size_t, size_t);
extern void* __real_malloc(size_t);

#define AS_MONO_ONLY
#define AS_FRAME_BYTES 2u
#define AS_PIECE_FRAMES 256u
#define AS_POOL_BYTES (VOICE_COUNT * UTA_RING * AS_PIECE_FRAMES * AS_FRAME_BYTES)
#include "ctr_adpcm_sfx_types.inc"

#define AP_HEADER 128u
#define AP_ROW 28u
#define AP_MAGIC 0x50535444u // "DTSP"
#define AP_VERSION 2u

typedef struct {
    uint16_t sound;
    uint8_t sourceBits, reserved;
    uint32_t offset, bytes, frames, rate, seamMaxStep;
    uint32_t check; // sfxpack.py row_check: the 24 B above, then the payload
} ApRow;
_Static_assert(sizeof(ApRow) == AP_ROW && offsetof(ApRow, check) == AP_ROW - 4u, "pack row layout");

static struct {
    CtrAudioSystem* ca;
    AudioSystemVtable outer;
    AudioSystemVtable* inner;
    ApRow* rows;
    uint32_t count;
    int fd;
    int status;
} mm MM_BSS;
static char mmDigest[65] MM_BSS, mmWin[512] MM_BSS, apPath[512] MM_BSS;
// The copy worker's own descriptor plus one (0: not open yet). The worker opens it and keeps it.
static int apWorkerFd MM_BSS;

static struct {
    uint8_t* pool;
    AdSfxRing* rings;
    AdSfxMeta* metas;
    AdSfxPhysical physical[VOICE_COUNT];
    uint64_t ticks, calls, decodedFrames, maxTicks, lastLog;
    uint32_t cores, gameFrames, installed, ringBytes, ringPeak, ringFailures, rawDropped;
    uint32_t badChecks, badSound, badLogged; // apPrepare payload checks (UT-012), logged by msUpdate
    bool coreUnavailable;
    uint8_t flush, flushLogged; // AD_FLUSH_*: how pieces reach the DSP (pieces.inc adFlush)
} adSfx MM_BSS;

// SHA-256 of "DTSP v2\0exact-dual-mono-wave-v2\0DTAD v1\0fnv1a-words-row24-payload" (tools/audio/sfxpack.py
// CATALOG_ID).
static const uint8_t apCatalog[32] MM_RO = {
    0xcb, 0x03, 0x5f, 0x8a, 0x70, 0x59, 0xb1, 0xd8, 0x2b, 0xb5, 0xb1, 0x6b, 0x88, 0xb8, 0xfc, 0xba,
    0x42, 0x94, 0x30, 0xe7, 0xfe, 0xa4, 0x95, 0xe9, 0xd3, 0x03, 0x25, 0x0f, 0xcd, 0x63, 0x63, 0x00,
};
static const char apName[] MM_RO = "/adpcm-sfx.dtsp";
static const char apOpened[] MM_RO = "ADPCM SFX pack: status %d (1 no digest, 2 missing, 3 header/size/catalog, 4 other data.win, 5 table, 6 no feeder workspace); %lu sounds; %.3f ms\n";
static const char msInstalled[] MM_RO = "ADPCM MONO sound %ld: %lu -> %lu B cache; %lu frames, %lu Hz, source ch 2; %.3f ms game-thread admission; no WAV load/copy\n";
// The counters that matter come first: a line longer than MESSAGE_LENGTH (192) loses its end.
static const char msFlush[] MM_RO = "ADPCM SFX pieces flushed through %s\n";
static const char msBad[] MM_RO = "ADPCM SFX pack: sound %lu failed its payload check (%lu so far); original PCM\n";
static const char msFlushHow[3][32] MM_RO = { "the kernel", "the dsp service", "the dsp service (svc failed)" };
static const char msCost[] MM_RO = "[adpcm-mono] installed %lu; raw dropped %lu; bad checks %lu; ring failures %lu, peak %lu B, extra %lu B, base %lu B; %lu updates; pieces %llu, %llu frames, %.3f ms decode, %.3f ms max; cores 0x%lX/%u\n";

// Preserve original callgraph/data references: this table is resolved by the
// assembler and checked by the layout gate, not GCC's varpool walk.
typedef struct {
    bool (*open)(CtrAudioSystem*, StreamContext*);
    void (*post)(CtrAudioSystem*, const char*, ...);
    int (*physical)(int);
} MmLegacy;
extern const MmLegacy CtrAdpcm_musicLegacy;
__asm__(".pushsection .rodata.late.adpcm,\"a\",%progbits\n"
        ".p2align 2\n.global CtrAdpcm_musicLegacy\n"
        ".type CtrAdpcm_musicLegacy,%object\nCtrAdpcm_musicLegacy:\n"
        ".word openStream, postMessage, utaPhys\n"
        ".size CtrAdpcm_musicLegacy, .-CtrAdpcm_musicLegacy\n.popsection\n");

#include "ctr_adpcm_sfx_pieces.inc"

// The feeder is on and has both linear workspaces (tail and seam pieces).
MM_CODE static bool apFeeder(void) {
    return *CtrAdpcm_sfxView.active && *CtrAdpcm_sfxView.tail && *CtrAdpcm_sfxView.seam;
}

// The pack's row for a sound, or NULL (pack off, or not one of its sounds). The rows never change after startup,
// so the worker may read them too.
MM_CODE static const ApRow* apRow(int32_t sound) {
    for (uint32_t i = 0; i < mm.count; i++)
        if (mm.rows[i].sound == sound) return &mm.rows[i];
    return NULL;
}

// Game thread at audio startup: open, check and keep the pack; any failure leaves it off (mm.count 0).
MM_CODE static int apOpen(CtrAudioSystem* ca) {
    // The feeder's workspaces (allocated when the callback is set, before this) decode pack sounds into PCM pieces.
    // Without them the feeder would queue a sound's own buffer: compressed bytes played as PCM. The pack stays off.
    if (!apFeeder()) return 6;
    if (!mmDigest[0] || !ca->dataWinPath || strcmp(ca->dataWinPath, mmWin)) return 1;
    char* slash = strrchr(mmWin, '/');
    size_t dir = slash ? (size_t)(slash - mmWin) : 0;
    if (dir + sizeof(apName) > sizeof(apPath)) return 1;
    memcpy(apPath, mmWin, dir);
    const char* name = apName;
    __asm__("" : "+r"(name)); // keep the late copy: GCC would fold the literal into .rodata.str1.1 (warm)
    memcpy(apPath + dir, name, sizeof(apName));
    mm.fd = open(apPath, O_RDONLY);
    if (mm.fd < 0) return 2;
    uint8_t h[AP_HEADER];
    uint32_t count, bytes;
    off_t size = lseek(mm.fd, 0, SEEK_END);
    if (!usReadAt(mm.fd, 0, h, AP_HEADER)) return 3;
    memcpy(&count, h + 8, 4);
    memcpy(&bytes, h + 12, 4);
    if (usLe32(h) != AP_MAGIC || usLe32(h + 4) != AP_VERSION || size != (off_t)bytes ||
        memcmp(h + 16, apCatalog, 32)) return 3;
    if (memcmp(h + 48, mmDigest, 64)) return 4;
    if (!count || count > ca->soundCount || count > (bytes - AP_HEADER) / AP_ROW) return 5;
    ApRow* rows = __real_malloc(count * AP_ROW);
    if (!rows) return 5;
    bool ok = usReadAt(mm.fd, AP_HEADER, (uint8_t*)rows, count * AP_ROW);
    uint32_t start = AP_HEADER + count * AP_ROW;
    for (uint32_t i = 0; ok && i < count; i++) {
        const ApRow* r = &rows[i];
        ok = r->sound < ca->soundCount && (!i || r->sound > rows[i - 1].sound) &&
            (r->sourceBits == 8 || r->sourceBits == 16) && r->bytes > CTR_ADPCM_HEADER_BYTES && !(r->bytes & 3u) &&
            r->offset >= start && r->offset <= bytes && r->bytes <= bytes - r->offset &&
            r->rate && r->rate <= 48000u && r->frames &&
            r->frames <= SFX_CACHE_MAX_ENTRY_BYTES / (r->sourceBits / 8u);
    }
    if (!ok) { free(rows); return 5; }
    mm.rows = rows;
    mm.count = count;
    return 0;
}

// Decode-side checks and the seam data the original cache scan would have made, on the resident payload.
// Pure computation on m->data: the game thread or the copy worker.
MM_CODE static bool apPrepare(AdSfxMeta* m, const ApRow* r) {
    // The row and the payload as read must be the ones the builder wrote (sfxpack.py row_check: FNV-1a's step over
    // the 32-bit words of the row's first 24 B, then of the payload; apOpen takes only whole-word rows): a damaged
    // payload, or a row now naming another sound, offset or length, plays that sound from its original PCM.
    uint32_t h = 0x811C9DC5u, w;
    for (uint32_t i = 0; i < AP_ROW - 4u + r->bytes; i += 4) {
        memcpy(&w, i < AP_ROW - 4u ? (const uint8_t*)r + i : (const uint8_t*)m->data + i - (AP_ROW - 4u), 4);
        h = (h ^ w) * 0x01000193u;
    }
    if (h != r->check) {
        __atomic_store_n(&adSfx.badSound, r->sound, __ATOMIC_RELAXED);
        __atomic_fetch_add(&adSfx.badChecks, 1u, __ATOMIC_RELAXED);
        return false;
    }
    if (!CtrAdpcmPcm_open(&m->initial, m->data, r->bytes) || m->initial.info.channels != 1 ||
        m->initial.info.rate != r->rate || m->initial.info.frames != r->frames) return false;
    // At most two block prefixes, never a whole-sound seam scan.
    CtrAdpcmPcm cursor = m->initial;
    int16_t ends[2];
    uint32_t n = r->frames > 1 ? 2 : 1;
    if (!CtrAdpcmPcm_read(&cursor, 0, 1, m->first, 1, NULL) ||
        !CtrAdpcmPcm_read(&cursor, r->frames - n, n, ends, n, NULL)) return false;
    m->prev[0] = ends[0];
    m->last[0] = ends[n - 1];
    int32_t step = abs((int32_t)m->first[0] - m->last[0]);
    m->seam.trigger[0] = r->sourceBits == 16 && r->frames >= 4 * UTA_TAIL_MAX &&
        step > 0 && (uint32_t)step >= UTA_SEAM_RATIO * r->seamMaxStep;
    m->originalBytes = r->frames * (r->sourceBits / 8u); // the original cache's mono copy
    return true;
}

MM_CODE static void apFree(AdSfxMeta* m) {
    if (m) { linearFree(m->data); free(m); }
}

// Game thread: into the cache as buildSfx would put the sound's first copy (never played yet). The only place a pack
// payload is published; false (the caller frees it, the sound takes its original path) unless the feeder can decode.
MM_CODE static bool apInstall(CtrAudioSystem* ca, int32_t sound, AdSfxMeta* m) {
    CachedSfx* c = &ca->sfxCache[sound];
    if (!apFeeder()) return false;
    if (!adSfx.pool) adSfx.pool = UTA_FAULT_ALLOC(3, AS_POOL_BYTES);
    if (!adSfx.pool) return false;
    adSfxReap();
    m->cache = c;
    LightLock_Lock(CtrAdpcm_sfxView.lock);
    m->next = adSfx.metas; adSfx.metas = m;
    c->pcm = m->data; c->bytes = m->initial.bytes; c->frames = m->initial.info.frames;
    c->sampleRate = m->initial.info.rate; c->sourceChannels = 2; // preserve original pan policy
    c->format = NDSP_FORMAT_MONO_PCM16; c->reload = SFX_RELOAD_AUDO_WAV;
    c->lastUse = 0; c->lastPlayTick = svcGetSystemTick();
    ca->cacheBytes += c->bytes;
    LightLock_Unlock(CtrAdpcm_sfxView.lock);
    ca->soundLengths[sound] = (float)c->frames / (float)c->sampleRate;
    adSfx.installed++;
    return true;
}

// Neither the game's own loader nor a background decode has the sound on its way.
MM_CODE static bool apIdle(CtrAudioSystem* ca, int32_t sound) {
    bool busy = false;
    LightLock_Lock(&ca->loadLock);
    for (unsigned i = 0; i < LOAD_SLOTS; i++)
        if (ca->loads[i].soundIndex == sound && ca->loads[i].state != LOAD_FREE) busy = true;
    for (unsigned i = 0; i < PREP_SLOTS; i++)
        if (ca->preps[i].soundIndex == sound && ca->preps[i].state != PREP_FREE) busy = true;
    LightLock_Unlock(&ca->loadLock);
    return !busy;
}

// Game thread. -2 not a pack sound (the caller takes the original path), -1 not now, 0 already cached,
// 1 admitted. A preload (budget below UINT32_MAX) leaves a sound the worker has to the worker.
MM_CODE static int apAdmit(int32_t sound, uint32_t budget, uint32_t floorBytes) {
    CtrAudioSystem* ca = mm.ca;
    if (!ca || !ca->ndspReady || ca->shutDown || !ca->base.audioGroups || sound < 0 ||
        (uint32_t)sound >= ca->soundCount) return -2;
    const ApRow* r = apRow(sound);
    if (!r) return -2;
    CachedSfx* c = &ca->sfxCache[sound];
    if (c->pcm) return 0;
    if (r->bytes > budget || (budget != UINT32_MAX && usJobFor(sound)) || !apIdle(ca, sound)) return -1;
    uint32_t need = r->bytes + (adSfx.pool ? 0 : AS_POOL_BYTES);
    if ((uint64_t)ca->cacheBytes + r->bytes > CTR_SFX_CACHE_BYTES ||
        (uint64_t)linearSpaceFree() < (uint64_t)need + floorBytes) return -1;
    u64 started = svcGetSystemTick();
    AdSfxMeta* m = __real_calloc(1, sizeof(*m));
    if (!m) return -1;
    m->data = linearAlloc(r->bytes);
    if (!m->data || !usReadAt(mm.fd, r->offset, m->data, r->bytes) || !apPrepare(m, r) || !apInstall(ca, sound, m)) {
        apFree(m);
        return -1;
    }
    u64 elapsed = svcGetSystemTick() - started;
    if (budget == UINT32_MAX) { ca->windowCacheLoads++; ca->windowLoadTicks += elapsed; }
    CtrAdpcm_musicLegacy.post(ca, msInstalled, (long)sound, (unsigned long)m->originalBytes,
        (unsigned long)c->bytes, (unsigned long)c->frames, (unsigned long)c->sampleRate, TICKS_TO_MS(elapsed));
    return 1;
}

// A planned sound the room change had no time for: utsfx queues its job as usual (its own checks, worker start-up
// and cap accounting, at the payload's size), then the job is marked to read the pack. The worker can't have
// taken it yet: a job queued this frame waits for the next frame (usw.held).
MM_CODE static int apQueue(int32_t sound, uint32_t kib, uint32_t floorBytes) {
    const ApRow* r = mm.ca && mm.ca->ndspReady ? apRow(sound) : NULL;
    if (r) kib = (r->bytes + 1023u) / 1024u;
    int got = __real_CtrAudioSystem_queueSfx(sound, kib, floorBytes);
    if (got == 1 && r) {
        LightLock_Lock(&usw.lock);
        for (int i = 0; i < UTSFX_JOBS; i++)
            if (usw.jobs[i].state == UTSFX_JOB_QUEUED && usw.jobs[i].soundIndex == sound) usw.jobs[i].header = AP_JOB;
        LightLock_Unlock(&usw.lock);
    }
    return got;
}

// Copy worker: the payload through the worker's own descriptor, checked and prepared; job->pcm carries the
// heap metadata (its data the payload) to the game thread's install.
MM_CODE static bool apCopy(UtSfxJob* job) {
    const ApRow* r = apRow(job->soundIndex);
    if (!r) return false;
    if (!apWorkerFd) apWorkerFd = open(apPath, O_RDONLY) + 1; // a failed open is tried again next job
    if (!apWorkerFd || (uint64_t)r->bytes + usw.floorBytes > linearSpaceFree()) return false;
    AdSfxMeta* m = __real_calloc(1, sizeof(*m));
    if (!m) return false;
    m->data = linearAlloc(r->bytes);
    if (!m->data || !usReadAt(apWorkerFd - 1, r->offset, m->data, r->bytes) || !apPrepare(m, r)) {
        apFree(m);
        return false;
    }
    job->pcm = (uint8_t*)m;
    job->bytes = r->bytes;
    return true;
}

// Game thread (usInstall): a finished pack job goes in under the same conditions as a WAV copy, or is dropped.
MM_CODE static void apFinish(CtrAudioSystem* ca, UtSfxJob* job, bool done) {
    AdSfxMeta* m = (AdSfxMeta*)job->pcm;
    int32_t sound = job->soundIndex;
    job->pcm = NULL;
    if (!done) { usw.failed++; return; }
    if (ca && ca == mm.ca && ca->base.audioGroups && sound >= 0 && (uint32_t)sound < ca->soundCount &&
        !ca->sfxCache[sound].pcm && CTR_SFX_CACHE_BYTES >= ca->cacheBytes + job->bytes && apIdle(ca, sound) &&
        apInstall(ca, sound, m)) {
        usw.installed++;
        usw.installedKb += (job->bytes + 1023u) / 1024u;
        return;
    }
    apFree(m);
    usw.dropped++;
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

// A play of a pack sound that isn't cached: its payload goes to the game's own background reader, the way the
// original queues the WAV (requestLoad), so the voice waits there (SOURCE_PENDING: loadInFlight) and this thread does
// no SD access. collectLoads drops a slot whose group is out of range without touching any AudioEntry, so a slot it
// sees before apCollect does only falls back to the original path. False: no reader or no free slot.
#define AP_GROUP INT32_MAX
MM_CODE static bool apRequest(CtrAudioSystem* ca, int32_t sound, const ApRow* r) {
    if (!ca->loader) return false;
    int slot = -1;
    LightLock_Lock(&ca->loadLock);
    for (int i = 0; i < LOAD_SLOTS; i++) {
        LoadRequest* q = &ca->loads[i];
        if (q->state == LOAD_FREE) { if (slot < 0) slot = i; }
        else if (q->soundIndex == sound && q->state != LOAD_FAILED) slot = LOAD_SLOTS; // already on its way
    }
    if (slot >= 0 && slot < LOAD_SLOTS) {
        LoadRequest* q = &ca->loads[slot];
        q->soundIndex = sound; q->group = AP_GROUP; q->path = apPath;
        q->offset = r->offset; q->size = r->bytes; q->header = 0;
        q->data = NULL; q->dataSize = 0; q->state = LOAD_QUEUED;
    }
    LightLock_Unlock(&ca->loadLock);
    if (slot < 0) return false;
    if (slot < LOAD_SLOTS) LightEvent_Signal(&ca->loadEvent);
    return true;
}

// Game thread, before the original update resolves waiting voices: finished pack reads go into the cache (a copy
// into linear memory, the checks, no I/O); the voice then starts from the cache. Anything else frees the slot and
// the voice takes the original path.
MM_CODE static void apCollect(CtrAudioSystem* ca) {
    for (int i = 0; i < LOAD_SLOTS; i++) {
        LoadRequest* q = &ca->loads[i];
        LightLock_Lock(&ca->loadLock);
        LoadState state = q->state;
        LightLock_Unlock(&ca->loadLock);
        if (q->group != AP_GROUP || (state != LOAD_DONE && state != LOAD_FAILED)) continue;
        u64 started = svcGetSystemTick();
        int32_t sound = q->soundIndex;
        const ApRow* r = apRow(sound);
        AdSfxMeta* m = NULL;
        bool ok = state == LOAD_DONE && r && q->dataSize == r->bytes && !ca->sfxCache[sound].pcm &&
            (uint64_t)ca->cacheBytes + r->bytes <= CTR_SFX_CACHE_BYTES &&
            (uint64_t)linearSpaceFree() >= (uint64_t)r->bytes + (adSfx.pool ? 0 : AS_POOL_BYTES) &&
            (m = __real_calloc(1, sizeof(*m))) && (m->data = linearAlloc(r->bytes));
        if (ok) {
            memcpy(m->data, q->data, r->bytes);
            ok = apPrepare(m, r) && apInstall(ca, sound, m);
        }
        if (!ok) apFree(m);
        free(q->data);
        q->data = NULL;
        LightLock_Lock(&ca->loadLock);
        q->state = LOAD_FREE;
        LightLock_Unlock(&ca->loadLock);
        if (!ok) continue;
        u64 elapsed = svcGetSystemTick() - started;
        ca->windowCacheLoads++;
        ca->windowLoadTicks += elapsed;
        CtrAdpcm_musicLegacy.post(ca, msInstalled, (long)sound, (unsigned long)m->originalBytes,
            (unsigned long)r->bytes, (unsigned long)r->frames, (unsigned long)r->rate, TICKS_TO_MS(elapsed));
    }
}

MM_CODE static int32_t msPlay(AudioSystem* audio, int32_t sound, int32_t priority, bool loop) {
    CtrAudioSystem* ca = mm.ca;
    const ApRow* r = ca && ca->ndspReady && !ca->shutDown && ca->base.audioGroups && sound >= 0 &&
        (uint32_t)sound < ca->soundCount && !ca->sfxCache[sound].pcm ? apRow(sound) : NULL;
    if (r && !apRequest(ca, sound, r)) apAdmit(sound, UINT32_MAX, 0); // no reader slot: read it here, as the original would
    return mm.inner->playSound(audio, sound, priority, loop);
}

MM_CODE static void msUpdate(AudioSystem* audio, float dt) {
    if ((CtrAudioSystem*)audio == mm.ca && mm.count) apCollect(mm.ca);
    mm.inner->update(audio, dt);
    // No pool yet: nothing installed. A pack whose every install failed its check still logs them (msBad).
    if (!adSfx.pool && !__atomic_load_n(&adSfx.badChecks, __ATOMIC_RELAXED)) return;
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
    uint8_t flush = adSfx.flush;
    LightLock_Unlock(CtrAdpcm_sfxView.lock);
    if (flush != adSfx.flushLogged && flush >= AD_FLUSH_SVC && flush <= AD_FLUSH_DSP_AFTER_SVC) {
        adSfx.flushLogged = flush;
        CtrAdpcm_musicLegacy.post(mm.ca, msFlush, msFlushHow[flush - 1]);
    }
    uint32_t bad = __atomic_load_n(&adSfx.badChecks, __ATOMIC_RELAXED);
    if (bad != adSfx.badLogged) {
        adSfx.badLogged = bad;
        CtrAdpcm_musicLegacy.post(mm.ca, msBad, (unsigned long)__atomic_load_n(&adSfx.badSound, __ATOMIC_RELAXED),
            (unsigned long)bad);
    }
    CtrAdpcm_musicLegacy.post(mm.ca, msCost, (unsigned long)adSfx.installed, (unsigned long)adSfx.rawDropped,
        (unsigned long)bad,
        (unsigned long)adSfx.ringFailures, (unsigned long)adSfx.ringPeak, (unsigned long)adSfx.ringBytes,
        (unsigned long)AS_POOL_BYTES, (unsigned long)adSfx.gameFrames, (unsigned long long)calls,
        (unsigned long long)frames, TICKS_TO_MS(ticks), TICKS_TO_MS(maximum), (unsigned long)cores, (unsigned)unknown);
}

MM_CODE static void mmCleanup(void) {
    msCleanup();
    // The rows stay: the detached copy worker may still be reading them at exit. No new admissions.
    mm.count = 0;
    if (mm.fd >= 0) close(mm.fd);
    mm.fd = -1;
}

MM_CODE static void mmInit(AudioSystem* audio, DataWin* dw, FileSystem* fs) {
    mm.inner->init(audio, dw, fs);
    CtrAudioSystem* ca = (CtrAudioSystem*)audio;
    if (!ca->ndspReady) return;
    u64 started = svcGetSystemTick();
    mm.status = apOpen(ca);
    if (mm.status && mm.fd >= 0) { close(mm.fd); mm.fd = -1; }
    CtrAdpcm_musicLegacy.post(ca, apOpened, mm.status, (unsigned long)mm.count, TICKS_TO_MS(svcGetSystemTick() - started));
}

MM_CODE static void mmDestroy(AudioSystem* audio) {
    mm.inner->destroy(audio);
    mmCleanup();
    mm.ca = NULL;
}

CtrAudioSystem* __real_CtrAudioSystem_create(void);
MM_CODE CtrAudioSystem* __wrap_CtrAudioSystem_create(void) {
    CtrAudioSystem* ca = __real_CtrAudioSystem_create();
    memset(&mm, 0, sizeof(mm));
    mm.fd = -1;
    mm.ca = ca; mm.inner = ca->base.vtable; mm.outer = *mm.inner;
    mm.outer.init = mmInit; mm.outer.destroy = mmDestroy;
    mm.outer.playSound = msPlay; mm.outer.update = msUpdate;
    ca->base.vtable = &mm.outer;
    atexit(mmCleanup); // original audio shutdown is registered later, so runs first
    return ca;
}

MM_CODE CtrDataVersionResult CtrDataVersion_chainAdpcmPack(CTR_DATAVERSION_CHAIN_PARAMS) {
    CtrDataVersionResult result = CTR_DATAVERSION_AFTER_ADPCM(path, game, part, cachePath, progress, cached, digest);
    mmWin[0] = mmDigest[0] = 0;
    if (result != CTR_DATA_UNREADABLE && path && digest && strlen(path) < sizeof(mmWin) && strlen(digest) == 64) {
        memcpy(mmWin, path, strlen(path) + 1);
        memcpy(mmDigest, digest, 65);
    }
    return result;
}
