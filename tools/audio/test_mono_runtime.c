// Appended to the production-piece harness. Real catalog/store/resident decoder;
// only platform ownership, cache bookkeeping and original playback are simulated.
#include "ctr_adpcm_store.h"
#include <stdarg.h>
#include <limits.h>
#define MM_CODE
#define MM_RO
#define MM_BSS
#define LOAD_SLOTS 2
#define PREP_SLOTS 2
#define LOAD_FREE 0
#define PREP_FREE 0
#define SFX_CACHE_MAX_ENTRY_BYTES 1048576u
#define CTR_SFX_CACHE_BYTES 6291456u
#define UTA_LOG_TICKS 5000u
#define UTA_SEAM_RATIO 4u
#define NDSP_FORMAT_MONO_PCM16 5u
#define SFX_RELOAD_AUDO_WAV 1u
#define TICKS_TO_MS(n) ((double)(n))
typedef uint64_t u64;
typedef struct { int32_t audioGroup, audioFile; bool embedded; } Sound;
typedef struct { struct { Sound* sounds; } sond; struct { uint32_t count; } audo; } DataWin;
typedef struct AudioSystem AudioSystem;
typedef struct {
    int32_t (*playSound)(AudioSystem*, int32_t, int32_t, bool);
    void (*update)(AudioSystem*, float);
} AudioSystemVtable;
struct AudioSystem { DataWin** audioGroups; };
typedef struct {
    AudioSystem base;
    bool ndspReady, shutDown;
    uint32_t soundCount, cacheBytes, windowCacheLoads;
    uint64_t windowLoadTicks;
    float* soundLengths;
    CachedSfx* sfxCache;
    LightLock loadLock;
    struct { int32_t soundIndex; unsigned state; } loads[LOAD_SLOTS], preps[PREP_SLOTS];
} CtrAudioSystem;
static struct {
    CtrAudioSystem* ca;
    AudioSystemVtable* inner;
    void* streams;
    bool attempted;
    int storeResult;
    CtrAdpcmStore store;
} mm;
static unsigned fallbackMessages, installedMessages, legacyPlays;
static int lastFallback, failLinearAt;
static bool failNextHeap, failPayloadRead;
static unsigned linearCalls;
static void* admissionLinear(uint32_t n) {
    linearCalls++;
    return failLinearAt == (int)linearCalls ? NULL : linearAlloc(n);
}
static void* admissionHeap(size_t n, size_t size) {
    if (failNextHeap) { failNextHeap = false; return NULL; }
    return calloc(n, size);
}
static bool admissionRead(void* owner, uint64_t off, void* out, uint32_t n) {
    return !failPayloadRead && CtrAdpcmStore_readAt(owner, off, out, n);
}
static bool soundIsInAudo(const Sound* sound) { return sound->embedded; }
static uint32_t linearSpaceFree(void) { return UINT32_MAX; }
static void message(CtrAudioSystem* ca, const char* fmt, ...) {
    assert(ca == mm.ca);
    va_list ap; va_start(ap, fmt);
    if (strstr(fmt, "fallback")) {
        (void)va_arg(ap, long); lastFallback = va_arg(ap, int); fallbackMessages++;
    } else if (strstr(fmt, "ADPCM MONO sound")) installedMessages++;
    va_end(ap);
}
static const struct { void (*post)(CtrAudioSystem*, const char*, ...); } CtrAdpcm_musicLegacy = {message};
#undef __real_calloc
#define __real_calloc admissionHeap
#define linearAlloc admissionLinear
#define CtrAdpcmStore_readAt admissionRead
// MONO_PRODUCTION
#undef linearAlloc
#undef CtrAdpcmStore_readAt

static int32_t legacyPlay(AudioSystem* audio, int32_t sound, int32_t priority, bool loop) {
    (void)audio; assert(sound == 0 && priority == 9 && loop); legacyPlays++;
    return 100001;
}

static void checkEmpty(const CachedSfx* cache, const CtrAudioSystem* ca) {
    assert(!cache->pcm && !cache->bytes && !ca->cacheBytes && !adSfx.metas && !adSfx.pool);
}

int main(int argc, char** argv) {
    assert(argc == 4);
    uint8_t sha[32];
    for (unsigned i = 0; i < 32; i++) { unsigned b; assert(sscanf(argv[2] + i * 2, "%2x", &b) == 1); sha[i] = (uint8_t)b; }
    Sound sound = {0, 0, true}; DataWin group = {{&sound}, {1}}; DataWin* groups[] = {&group};
    CachedSfx cache = {0}; float length = 0;
    CtrAudioSystem ca = {.base={groups}, .ndspReady=true, .soundCount=1, .sfxCache=&cache, .soundLengths=&length};
    AudioSystemVtable legacy = {legacyPlay, NULL};
    mm.ca = &ca; mm.streams = &ca; mm.inner = &legacy; mm.attempted = true;
    assert(CtrAdpcmStore_open(&mm.store, argv[1], sha, 1, malloc) == 0);
    CtrAdpcmAsset asset;
    assert(CtrAdpcmCatalog_sound(&mm.store.catalog, 0, &asset));
    assert(mmSfxPlanned(0) && !mmSfxPlanned(-1) && !mmSfxPlanned(1));
    assert(asset.channels == 1 && asset.sourceChannels == 2 && asset.frames == 2045);
    assert(mmSfxAdmit(-1, UINT32_MAX, 0) == -1);
    assert(mmSfxAdmit(1, UINT32_MAX, 0) == -1);
    sound.embedded = false; assert(mmSfxAdmit(0, UINT32_MAX, 0) == -1); sound.embedded = true;
    sound.audioGroup = 1; assert(mmSfxAdmit(0, UINT32_MAX, 0) == -1); sound.audioGroup = 0;
    ca.loads[0].state = 1; assert(mmSfxAdmit(0, UINT32_MAX, 0) == -1); ca.loads[0].state = 0;
    ca.preps[0].state = 1; assert(mmSfxAdmit(0, UINT32_MAX, 0) == -1); ca.preps[0].state = 0;
    assert(mmSfxAdmit(0, asset.bytes - 1, 0) == -1);
    assert(mmSfxAdmit(0, UINT32_MAX, UINT32_MAX) == -1);
    checkEmpty(&cache, &ca);
    mm.store.receiptResult = CTR_ADPCM_STORE_RECEIPT;
    assert(msPlay(&ca.base, 0, 9, true) == 100001 && legacyPlays == 1);
    assert(lastFallback == CTR_ADPCM_STORE_RECEIPT); checkEmpty(&cache, &ca);
    mm.store.receiptResult = 0;
    failNextHeap = true; assert(mmSfxAdmit(0, UINT32_MAX, 0) == -1); checkEmpty(&cache, &ca);
    failLinearAt = (int)linearCalls + 1;
    assert(mmSfxAdmit(0, UINT32_MAX, 0) == -1); checkEmpty(&cache, &ca);
    failLinearAt = (int)linearCalls + 2; // payload succeeded, pieces failed
    assert(mmSfxAdmit(0, UINT32_MAX, 0) == -1); checkEmpty(&cache, &ca);
    failLinearAt = 0; failPayloadRead = true;
    assert(mmSfxAdmit(0, UINT32_MAX, 0) == -1); checkEmpty(&cache, &ca);
    failPayloadRead = false;
    assert(mmSfxAdmit(0, UINT32_MAX, 0) == 1);
    assert(cache.pcm && cache.bytes == asset.bytes && cache.frames == asset.frames && cache.sampleRate == asset.rate);
    assert(cache.format == NDSP_FORMAT_MONO_PCM16 && cache.sourceChannels == 2);
    assert(cache.reload == SFX_RELOAD_AUDO_WAV && ca.cacheBytes == cache.bytes);
    assert(ca.windowCacheLoads == 1 && ca.windowLoadTicks && length == (float)asset.frames / asset.rate);
    assert(installedMessages == 1 && fallbackMessages == 5 && AS_POOL_BYTES == 98304u);
    AdSfxMeta* meta = adMeta(&cache); assert(meta && meta->originalBytes == asset.frames * 2);
    FILE* ref = fopen(argv[3], "rb"); assert(ref);
    int16_t pcm[2045]; assert(fread(pcm, 2, 2045, ref) == 2045); fclose(ref);
    assert(meta->first[0] == pcm[0] && meta->prev[0] == pcm[2043] && meta->last[0] == pcm[2044]);
    assert(mmSfxAdmit(0, UINT32_MAX, 0) == 0 && installedMessages == 1);
    assert(msPlay(&ca.base, 0, 9, true) == 100001 && legacyPlays == 2);
    // Original eviction owns/frees the payload; reap owns only its metadata.
    linearFree(cache.pcm); memset(&cache, 0, sizeof(cache)); ca.cacheBytes = 0;
    adSfxReap(); assert(!adSfx.metas);
    assert(mmSfxAdmit(0, asset.bytes, 0) == 1 && ca.windowCacheLoads == 1); // preload counters stay original
    linearFree(cache.pcm); memset(&cache, 0, sizeof(cache)); ca.cacheBytes = 0;
    msCleanup(); msCleanup();
    CtrAdpcmStore_close(&mm.store);
    puts("Mono first admission: original play fallback, missing receipt, failures, preloads, exact rate/frames/pan and eviction passed");
    return 0;
}
