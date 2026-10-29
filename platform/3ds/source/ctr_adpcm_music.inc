// Option B: every converted music stream uses the original PCM feeder.
// ADPCM decoding stays on its worker; logical voices, channels and seams stay original.
#include "ctr_adpcm_player.h"
#include "ctr_adpcm_store.h"
#include "ctr_wrap_chain.h"

#define MM_RO __attribute__((section(".rodata.late.adpcm")))
#define MM_BSS __attribute__((section(".bss.late.adpcm")))
extern void* __real_calloc(size_t, size_t);
extern void* __real_malloc(size_t);

typedef struct {
    CtrAdpcmFile file;
    CtrAdpcmContinuation* pcm; // bounded ordinary heap; never visible to DSP
} MmStream;

static struct {
    CtrAudioSystem* ca;
    AudioSystemVtable outer;
    AudioSystemVtable* inner;
    MmStream* streams;
    CtrAdpcmStore store;
    LightLock lock;
    int storeResult;
    bool attempted;
} mm MM_BSS;
static char mmDigest[65] MM_BSS, mmWin[512] MM_BSS;
static const char mmHex[] MM_RO = "0123456789abcdef";
static const char mmDisabled[] MM_RO = "ADPCM MUSIC DISABLED: index status %d; audio-adpcm/index/%s.dtai beside data.win or parent; Vorbis/PCM fallback\n";
static const char mmFallback[] MM_RO = "ADPCM MUSIC fallback '%s': status %d (8 projection, 10 cancel, 11 prepare, 12 missing/invalid receipt, 13 stale source, 14 stale payload)\n";
static const char mmReceipt[] MM_RO = "ADPCM MUSIC metadata preload: status %d, receipt %d, %lu B catalog+receipt heap, %.3f ms; no track hash on play\n";
#if CTRPORT_ADPCM_TEST_UNKNOWN_MTIME
static const char mmTestMtime[] MM_RO = "ADPCM TEST ONLY: detected emulator has unknown mtime; trust receipt identities, size and bounded header; MTIME CHECK NOT TESTED\n";
#endif
static const char mmPcmStarted[] MM_RO = "ADPCM MUSIC feeder '%s': %lu Hz, %lu source ch, %lu frames; %lu B heap cursor; 0 B new linear; existing decode worker\n";

// Preserve original callgraph/data references: this table is resolved by the
// assembler and checked by the layout gate, not GCC's varpool walk.
typedef struct {
    bool (*open)(CtrAudioSystem*, StreamContext*);
    void (*post)(CtrAudioSystem*, const char*, ...);
    int (*physical)(int);
} MmLegacy;
extern const MmLegacy CtrAdpcm_musicLegacy;
#ifndef CTR_ADPCM_MUSIC_HOST
__asm__(".pushsection .rodata.late.adpcm,\"a\",%progbits\n"
        ".p2align 2\n.global CtrAdpcm_musicLegacy\n"
        ".type CtrAdpcm_musicLegacy,%object\nCtrAdpcm_musicLegacy:\n"
        ".word openStream, postMessage, utaPhys\n"
        ".size CtrAdpcm_musicLegacy, .-CtrAdpcm_musicLegacy\n.popsection\n");
#endif

MM_CODE static bool mmStore(void) {
    if (mm.attempted) return mm.storeResult == CTR_ADPCM_STORE_OK;
    mm.attempted = true; // preloaded at init before any game play requests
    mm.storeResult = CTR_ADPCM_STORE_ARGUMENT;
    uint8_t sha[32];
    for (unsigned i = 0; i < 32; i++) {
        const char* a = strchr(mmHex, mmDigest[i * 2]);
        const char* b = strchr(mmHex, mmDigest[i * 2 + 1]);
        if (!a || !b) return false;
        sha[i] = (uint8_t)((a - mmHex) * 16 + b - mmHex);
    }
    mm.storeResult = CtrAdpcmStore_open(&mm.store, mmWin, sha, mm.ca->soundCount, __real_malloc);
    if (mm.storeResult) CtrAdpcm_musicLegacy.post(mm.ca, mmDisabled, mm.storeResult, mmDigest);
    return mm.storeResult == CTR_ADPCM_STORE_OK;
}

MM_CODE static int mmDecoderIndex(CtrDecoder* decoder) {
    if (mm.streams) for (unsigned i = 0; i < STREAM_CONTEXTS; i++)
        if (decoder == &mm.ca->streams[i].decoder) return (int)i;
    return -1;
}

MM_CODE static bool mmDecOpen(CtrDecoder* decoder, const char* path) {
    int index = mmDecoderIndex(decoder);
    if (index < 0 || !path) return false;
    StreamContext* s = &mm.ca->streams[index];
    MmStream* m = &mm.streams[index];
    // OPENING excludes worker retirement; this opener owns the decoder/path.
    CtrAdpcmStore_closeFile(&m->file);
    free(m->pcm); m->pcm = NULL;
    if (!mmStore()) return false;
    CtrAdpcmAsset asset;
    int reason = CtrAdpcmStore_lookupFile(&mm.store, path, &asset);
    LightLock_Lock(&s->lock);
    if (!reason && s->stopRequested) reason = 10;
    LightLock_Unlock(&s->lock);
    if (!reason) reason = CtrAdpcmStore_openFile(&mm.store, path, &m->file, &asset);
    if (!reason) {
        m->pcm = __real_calloc(1, sizeof(*m->pcm));
        if (!m->pcm) reason = CTR_ADPCM_STORE_HEAP;
        else {
            m->pcm->info = m->file.info;
            m->pcm->source = (CtrAdpcmSource){CtrAdpcmStore_readAt, &m->file, m->file.sourceBytes, NULL};
        }
    }
    LightLock_Lock(&s->lock);
    bool proceed = !reason && !s->stopRequested;
    if (proceed) {
        decoder->kind = CTR_DECODER_NONE;
        decoder->sampleRate = asset.rate; decoder->totalFrames = asset.frames;
        decoder->channels = asset.sourceChannels; decoder->positionFrames = 0;
    }
    LightLock_Unlock(&s->lock);
    if (!proceed) {
        CtrAdpcmStore_closeFile(&m->file);
        free(m->pcm); m->pcm = NULL;
        CtrAdpcm_musicLegacy.post(mm.ca, mmFallback, s->name, reason ? reason : 10);
        return false; // the unchanged late decoder wrapper opens Vorbis
    }
    CtrAdpcm_musicLegacy.post(mm.ca, mmPcmStarted, s->name, (unsigned long)asset.rate,
        (unsigned long)asset.sourceChannels, (unsigned long)asset.frames,
        (unsigned long)sizeof(*m->pcm));
    return true;
}

MM_CODE static bool mmDecSeek(CtrDecoder* decoder, uint64_t frame, bool* result) {
    int index = mmDecoderIndex(decoder);
    if (index < 0 || !mm.streams[index].pcm) return false;
    MmStream* m = &mm.streams[index];
    *result = frame <= UINT32_MAX && CtrAdpcmContinuation_seek(m->pcm, (uint32_t)frame);
    if (*result) decoder->positionFrames = frame;
    return true;
}

// Only the original opener/decode worker owns this cursor. The original read
// timing surrounds this call, and utaDecRead applies its loop seam afterwards.
MM_CODE static bool mmDecRead(CtrDecoder* decoder, int16_t* out, uint32_t capacity, int32_t* result) {
    int index = mmDecoderIndex(decoder);
    if (index < 0 || !mm.streams[index].pcm) return false;
    CtrAdpcmContinuation* cursor = mm.streams[index].pcm;
    *result = CtrAdpcmContinuation_read(cursor, out, capacity);
    decoder->positionFrames = cursor->position;
    return true;
}

// Retire a cursor only after the original feeder has retired its stream.
// Detach under the stream lock, then close/free outside it. OPENING belongs
// to the opener; a replacement cannot lose its file to the decode worker.
MM_CODE static void mmService(CtrAudioSystem* ca, StreamContext* s, DecodeStats* stats) {
    (void)stats;
    if (!mm.streams) return;
    MmStream* m = &mm.streams[s - ca->streams];
    CtrAdpcmFile file = {0};
    CtrAdpcmContinuation* cursor = NULL;
    LightLock_Lock(&s->lock);
    if (s->state == CTX_DONE || s->state == CTX_FREE) {
        file = m->file; memset(&m->file, 0, sizeof(m->file));
        cursor = m->pcm; m->pcm = NULL;
    }
    LightLock_Unlock(&s->lock);
    CtrAdpcmStore_closeFile(&file);
    free(cursor);
}

// The original worker's lock operands remain redirected so its warm body
// stays byte-identical. PCM work and all state transitions remain original.
MM_CODE static void mmServiceLock(LightLock* lock) {
    if (mm.streams) for (unsigned i = 0; i < STREAM_CONTEXTS; i++)
        if (lock == &mm.ca->streams[i].lock) {
            mmService(mm.ca, &mm.ca->streams[i], NULL);
            break;
        }
    LightLock_Lock(lock);
}

MM_CODE static void mmServiceUnlock(LightLock* lock) {
    LightLock_Unlock(lock);
}

// Keep the existing late wrappers' call edges. Option B never intercepts
// their format, gain, rate, pause or clear: the original feeder owns them.
MM_CODE static bool mmControl(int id, unsigned op, const void* value) {
    (void)id; (void)op; (void)value;
    return false;
}

MM_CODE static void mmFrame(void) {
    // No native queue, channel transition or DSP publication in this adapter.
}

#ifndef CTR_ADPCM_MUSIC_HOST
#include "ctr_adpcm_mono.inc"
#endif

MM_CODE static void mmCleanup(void) {
    // Original shutdown disabled callbacks, joined opener/decode and reset
    // channels before freeing the existing stream buffers. No DSP calls here.
    if (mm.streams) for (unsigned i = 0; i < STREAM_CONTEXTS; i++) {
        CtrAdpcmStore_closeFile(&mm.streams[i].file);
        free(mm.streams[i].pcm);
    }
    free(mm.streams); mm.streams = NULL;
#ifndef CTR_ADPCM_MUSIC_HOST
    msCleanup();
#endif
    CtrAdpcmStore_close(&mm.store);
}

MM_CODE static void mmInit(AudioSystem* audio, DataWin* dw, FileSystem* fs) {
    mm.inner->init(audio, dw, fs);
    CtrAudioSystem* ca = (CtrAudioSystem*)audio;
    if (!ca->ndspReady || !mmDigest[0] || !ca->dataWinPath || strcmp(ca->dataWinPath, mmWin)) return;
    mm.streams = __real_calloc(STREAM_CONTEXTS, sizeof(MmStream));
    if (mm.streams) {
#ifndef CTR_ADPCM_MUSIC_HOST
        u64 started = svcGetSystemTick();
#endif
        mmStore();
        double elapsed = 0;
#ifndef CTR_ADPCM_MUSIC_HOST
        elapsed = TICKS_TO_MS(svcGetSystemTick() - started);
#endif
        CtrAdpcm_musicLegacy.post(ca, mmReceipt, mm.storeResult, mm.store.receiptResult,
            (unsigned long)(mm.store.receiptBytes + (mm.store.index ?
                CTR_ADPCM_INDEX_HEADER + (mm.store.catalog.sounds + mm.store.catalog.files) * CTR_ADPCM_INDEX_RECORD +
                mm.store.catalog.stringBytes : 0)), elapsed);
#if CTRPORT_ADPCM_TEST_UNKNOWN_MTIME
        if (!mm.storeResult && !mm.store.receiptResult && mm.store.stampMode == 2)
            CtrAdpcm_musicLegacy.post(ca, mmTestMtime);
#endif
    }
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
    LightLock_Init(&mm.lock);
    mm.ca = ca; mm.inner = ca->base.vtable; mm.outer = *mm.inner;
    mm.outer.init = mmInit; mm.outer.destroy = mmDestroy;
#ifndef CTR_ADPCM_MUSIC_HOST
    mm.outer.playSound = msPlay; mm.outer.update = msUpdate;
#endif
    ca->base.vtable = &mm.outer;
    atexit(mmCleanup); // original audio shutdown is registered later, so runs first
    return ca;
}

MM_CODE CtrDataVersionResult CtrDataVersion_chainAdpcmMusic(CTR_DATAVERSION_CHAIN_PARAMS) {
    CtrDataVersionResult result = CTR_DATAVERSION_AFTER_ADPCM(path, game, part, cachePath, progress, cached, digest);
    mmWin[0] = mmDigest[0] = 0;
    if (result != CTR_DATA_UNREADABLE && path && digest && strlen(path) < sizeof(mmWin) && strlen(digest) == 64) {
        memcpy(mmWin, path, strlen(path) + 1);
        memcpy(mmDigest, digest, 65);
    }
    return result;
}
