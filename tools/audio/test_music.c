// Exercise the production game adapter with its real SD store and queue.
#define main component_player_main
#include "test_player.c"
#undef main
#include <stdarg.h>
#include <math.h>

#define CTR_WRAP_CHAIN_H
#define CTR_ADPCM_MUSIC_HOST
#define MM_CODE
#define STREAM_CONTEXTS 8
#define STREAM_BUFFERS 4
#define STREAM_BUFFER_FRAMES 4096
enum { CTX_FREE, CTX_OPEN_REQUESTED, CTX_OPENING, CTX_PLAYING, CTX_DRAINING, CTX_DONE };
typedef unsigned StreamState;
enum { CTR_DECODER_NONE };
typedef int LightLock;
static unsigned held;
static void LightLock_Init(LightLock* lock) { *lock = 0; }
static void LightLock_Lock(LightLock* lock) { assert(!*lock); *lock = 1; held++; }
static void LightLock_Unlock(LightLock* lock) { assert(*lock && held); *lock = 0; held--; }
typedef struct {} DataWin;
typedef struct {} FileSystem;
typedef struct AudioSystem AudioSystem;
typedef struct {
    void (*init)(AudioSystem*, DataWin*, FileSystem*);
    void (*destroy)(AudioSystem*);
} AudioSystemVtable;
struct AudioSystem { AudioSystemVtable* vtable; };
typedef struct { unsigned kind, sampleRate, channels; uint64_t totalFrames, positionFrames; } CtrDecoder;
typedef struct {
    LightLock lock;
    unsigned state;
    int channel;
    char* path;
    const void* memData;
    const char* name;
    bool loop, paused, stopRequested, seekPending, opened, everQueued, failed;
    double seekSeconds;
    float pitch, mix[12];
    uint32_t sampleRate, channels;
    uint64_t totalFrames;
    CtrDecoder decoder;
    struct { uint8_t* pcm; ndspWaveBuf wb; uint64_t startFrame; } bufs[4];
} StreamContext;
typedef struct {
    AudioSystem base;
    bool ndspReady;
    const char* dataWinPath;
    unsigned soundCount;
    int event;
    StreamContext streams[8];
} CtrAudioSystem;
typedef struct { int ctx; float lastPosition; } Voice;
typedef struct { unsigned calls; } DecodeStats;
typedef int CtrDataVersionResult;
#define CTR_DATA_UNREADABLE 9
#define CTR_DATAVERSION_CHAIN_PARAMS const char* path, int game, int part, const char* cachePath, void* progress, bool* cached, char* digest
static CtrDataVersionResult checkData(CTR_DATAVERSION_CHAIN_PARAMS) {
    (void)path; (void)game; (void)part; (void)cachePath; (void)progress; (void)cached; (void)digest;
    return 0;
}
#define CTR_DATAVERSION_AFTER_ADPCM checkData
void* __real_malloc(size_t n) { return malloc(n); }
static bool failNextAllocation;
void* __real_calloc(size_t n, size_t size) {
    if (failNextAllocation) { failNextAllocation = false; return NULL; }
    return calloc(n, size);
}

static CtrAudioSystem systemUnderTest;
static unsigned legacyOpens, messages;
static bool cancelDuringRead, failRead;
static int preflightCancellation;
static uint64_t exactSeekFrame;
static bool physicalOpen(CtrDecoder* decoder, const char* path) { (void)decoder; assert(path && !held); return true; }
static bool physicalSeek(CtrDecoder* decoder, uint64_t frame) { assert(!held); decoder->positionFrames = frame; exactSeekFrame = frame; return true; }
static const int16_t* seamReference;
static int32_t physicalRead(CtrDecoder* decoder, int16_t* out, uint32_t capacity) {
    uint32_t left = (uint32_t)(decoder->totalFrames - decoder->positionFrames);
    uint32_t n = capacity < left ? capacity : left;
    memcpy(out, seamReference + decoder->positionFrames * decoder->channels, n * decoder->channels * 2);
    decoder->positionFrames += n;
    return (int32_t)n;
}
static bool physicalMemory(CtrDecoder* decoder, const uint8_t* data, uint32_t size) {
    (void)decoder; (void)data; (void)size; return false;
}
#define UTA_PHYS_OPEN_FILE physicalOpen
#define UTA_PHYS_OPEN_MEMORY physicalMemory
#define UTA_PHYS_READ physicalRead
#define UTA_PHYS_SEEK physicalSeek
static bool monitoredRead(void* owner, uint64_t offset, void* out, uint32_t bytes) {
    assert(!held && !inCallback); // Real file reads never exclude the DSP or game thread.
    if (failRead) return false;
    if (cancelDuringRead) systemUnderTest.streams[0].stopRequested = true;
    return CtrAdpcmStore_readAt(owner, offset, out, bytes);
}
static CtrAdpcmStoreResult monitoredOpen(const CtrAdpcmStore* store, const char* path,
    CtrAdpcmFile* file, CtrAdpcmAsset* asset) {
    assert(!held && !inCallback);
    CtrAdpcmStoreResult result = CtrAdpcmStore_openFile(store, path, file, asset);
    if (preflightCancellation == 1) systemUnderTest.streams[0].stopRequested = true;
    if (preflightCancellation == 2) systemUnderTest.streams[0].seekPending = true;
    return result;
}
#define CtrAdpcmStore_readAt monitoredRead
#define CtrAdpcmStore_openFile monitoredOpen
#include "ctr_adpcm_music.inc"
#undef CtrAdpcmStore_readAt
#undef CtrAdpcmStore_openFile
#include CTR_ADPCM_TEST_SEAM_HEADER

static bool legacyOpen(CtrAudioSystem* ca, StreamContext* s) {
    (void)ca;
    assert(!held);
    legacyOpens++;
    s->state = s->stopRequested ? CTX_DONE : CTX_PLAYING;
    return !s->stopRequested;
}
static void legacyPost(CtrAudioSystem* ca, const char* format, ...) { (void)ca; (void)format; assert(!held); messages++; }
static int legacyPhysical(int id) { return id; }
const MmLegacy CtrAdpcm_musicLegacy = {legacyOpen, legacyPost, legacyPhysical};
static void originalInit(AudioSystem* audio, DataWin* dw, FileSystem* fs) { (void)audio; (void)dw; (void)fs; }
static void originalDestroy(AudioSystem* audio) { (void)audio; }
static AudioSystemVtable original = {originalInit, originalDestroy};
CtrAudioSystem* __real_CtrAudioSystem_create(void) {
    memset(&systemUnderTest, 0, sizeof(systemUnderTest));
    systemUnderTest.base.vtable = &original;
    systemUnderTest.ndspReady = true;
    return &systemUnderTest;
}

static void openAgain(StreamContext* s, const char* path) {
    assert(!mm.streams[0].pcm && !mm.streams[0].file.file);
    memset(s, 0, sizeof(*s));
    s->channel = 6; s->state = CTX_OPEN_REQUESTED;
    s->name = "synthetic dual mono"; s->path = strdup(path);
    s->pitch = 1.25f; s->mix[0] = .4f; s->mix[1] = .8f;
    uint8_t* buffer = malloc(16384 + 31);
    assert(buffer);
    s->bufs[0].pcm = align32(buffer);
    // Other buffers are irrelevant: the adapter borrows only this existing allocation.
    s->bufs[1].pcm = buffer;
}
static void release(StreamContext* s) { free(s->bufs[1].pcm); free(s->path); s->path = NULL; }
static void nativeFrame(void) { inCallback = true; mmFrame(); inCallback = false; }

int main(int argc, char** argv) {
    assert(argc == 9 && strlen(argv[2]) == 64);
    CtrAudioSystem* ca = __wrap_CtrAudioSystem_create();
    strcpy(mmWin, argv[1]); strcpy(mmDigest, argv[2]);
    ca->dataWinPath = argv[1]; ca->soundCount = 1;
    ca->base.vtable->init(&ca->base, NULL, NULL);
    assert(mm.streams);
    StreamContext* s = &ca->streams[0];
    DecodeStats stats = {0};
    // All converted layouts use the existing PCM feeder, including a true
    // mono source and the dual-mono source previously sent to the native queue.
    unsigned before = mutations;
    for (unsigned layout = 0; layout < 2; layout++) {
        const char* path = argv[layout ? 7 : 3];
        uint32_t referenceBytes;
        int16_t* reference = (int16_t*)load(argv[layout ? 8 : 6], &referenceBytes);
        uint32_t frames = layout ? 2173 : 2045, channels = layout ? 1 : 2;
        uint32_t hz = layout ? 22050 : 32000;
        assert(referenceBytes == frames * channels * 2);
        for (unsigned loop = 0; loop < 2; loop++) {
            openAgain(s, path); s->state = CTX_OPENING; s->loop = loop;
            assert(mmDecOpen(&s->decoder, path) && mm.streams[0].pcm);
            assert(mm.streams[0].file.file && mutations == before);
            assert(s->decoder.channels == channels && s->decoder.sampleRate == hz);
            assert(s->decoder.totalFrames == frames && s->decoder.positionFrames == 0);
            for (unsigned op = 0; op < 5; op++) assert(!mmControl(6, op, NULL));
            mmService(ca, s, &stats); // OPENING cannot retire this new cursor.
            assert(mm.streams[0].pcm && mm.streams[0].file.file);
            bool ok = false;
            assert(mmDecSeek(&s->decoder, 1017, &ok) && ok);
            assert(!legacyOpens && s->decoder.positionFrames == 1017);
            s->state = CTX_PLAYING;
            for (unsigned pass = 0; pass < 4; pass++) {
                assert(mmDecSeek(&s->decoder, pass ? 0 : 1017, &ok) && ok);
                uint32_t at = (uint32_t)s->decoder.positionFrames;
                while (at < frames) {
                    int16_t out[99 * 2 + 2]; int32_t got;
                    uint32_t count = frames - at < 99 ? frames - at : 99;
                    out[99 * channels] = 12345;
                    assert(mmDecRead(&s->decoder, out, 99, &got) && got == (int32_t)count);
                    assert(!memcmp(out, reference + at * channels, count * channels * 2));
                    assert(out[99 * channels] == 12345);
                    at += count; assert(s->decoder.positionFrames == at);
                    nativeFrame(); assert(mutations == before);
                }
                int16_t out[2]; int32_t got;
                assert(mmDecRead(&s->decoder, out, 1, &got) && got == 0);
            }
            s->stopRequested = true; s->state = CTX_DONE;
            mmServiceLock(&s->lock); assert(s->state == CTX_DONE); mmServiceUnlock(&s->lock);
            assert(!mm.streams[0].pcm && !mm.streams[0].file.file && mutations == before);
            release(s);
        }
        free(reference);
    }
    // A stop arriving during metadata admission falls back without claiming a channel.
    openAgain(s, argv[3]); s->state = CTX_OPENING;
    preflightCancellation = 1;
    assert(!mmDecOpen(&s->decoder, s->path));
    assert(s->stopRequested && !mm.streams[0].pcm && !mm.streams[0].file.file);
    assert(mutations == before); release(s);
    // An initial non-block-aligned seek remains in ADPCM, with no reopen to Vorbis.
    openAgain(s, argv[3]); s->state = CTX_OPENING; preflightCancellation = 2;
    assert(mmDecOpen(&s->decoder, s->path) && s->seekPending);
    bool result = false;
    assert(mmDecSeek(&s->decoder, 1017, &result) && result);
    assert(!legacyOpens && s->decoder.positionFrames == 1017);
    preflightCancellation = 0;
    // A read can observe cancellation; only the original worker retires it.
    cancelDuringRead = true;
    int16_t cancelled[2]; int32_t cancelledCount;
    assert(mmDecRead(&s->decoder, cancelled, 1, &cancelledCount) && cancelledCount == 1);
    assert(s->stopRequested && mm.streams[0].file.file && mutations == before);
    cancelDuringRead = false; s->state = CTX_DONE; mmService(ca, s, &stats); release(s);
    // Workspace exhaustion keeps the original fallback available, with no descriptor leak.
    openAgain(s, argv[3]); s->state = CTX_OPENING; failNextAllocation = true;
    assert(!mmDecOpen(&s->decoder, s->path));
    assert(!mm.streams[0].pcm && !mm.streams[0].file.file && mutations == before);
    release(s);
    // A failed SD read stays a decoder error; an exact seek permits retry.
    openAgain(s, argv[3]); s->state = CTX_OPENING;
    assert(mmDecOpen(&s->decoder, s->path));
    failRead = true;
    assert(mmDecRead(&s->decoder, cancelled, 1, &cancelledCount) && cancelledCount == -1);
    assert(mm.streams[0].pcm->failed && mm.streams[0].file.file);
    failRead = false; result = false;
    assert(mmDecSeek(&s->decoder, 0, &result) && result);
    assert(mmDecRead(&s->decoder, cancelled, 1, &cancelledCount) && cancelledCount == 1);
    s->state = CTX_DONE; mmService(ca, s, &stats); release(s);
    // True stereo uses the unchanged one-channel PCM feeder throughout. Its
    // decoded samples equal the independent Python render, including arbitrary
    // packet sizes, exact seeks, final partial blocks and repeated loop reads.
    uint32_t referenceBytes;
    int16_t* reference = (int16_t*)load(argv[5], &referenceBytes);
    assert(referenceBytes == 12269 * 4);
    openAgain(s, argv[4]); s->state = CTX_OPENING; s->loop = true;
    s->seekPending = true; // The original opener's late initial seek is legal.
    assert(mmDecOpen(&s->decoder, s->path));
    assert(mm.streams[0].pcm && mm.streams[0].file.file);
    assert(s->decoder.sampleRate == 44100 && s->decoder.channels == 2 && s->decoder.totalFrames == 12269);
    result = false; assert(mmDecSeek(&s->decoder, 1017, &result) && result);
    assert(s->decoder.positionFrames == 1017);
    assert(mmDecSeek(&s->decoder, UINT64_MAX, &result) && !result && s->decoder.positionFrames == 1017);
    s->state = CTX_PLAYING; s->seekPending = false;
    mmServiceLock(&s->lock); assert(s->state == CTX_PLAYING); mmServiceUnlock(&s->lock);
    assert(!mmControl(6, 4, NULL)); // Format, pitch, mix and pause stay in original wrappers.
    int16_t output[4096 * 2 + 2];
    int32_t got;
    for (unsigned pass = 0; pass < 4; pass++) {
        uint32_t first = pass ? 0 : 1017;
        assert(mmDecSeek(&s->decoder, first, &result) && result);
        while (first < 12269) {
            uint32_t capacity = first % 3 ? 4096 : 73;
            uint32_t count = capacity < 12269 - first ? capacity : 12269 - first;
            output[capacity * 2] = 12345; output[capacity * 2 + 1] = -12345;
            assert(mmDecRead(&s->decoder, output, capacity, &got) && got == (int32_t)count);
            assert(!memcmp(output, reference + first * 2, count * 4));
            assert(output[capacity * 2] == 12345 && output[capacity * 2 + 1] == -12345);
            first += count; assert(s->decoder.positionFrames == first);
            nativeFrame(); assert(mutations == before);
        }
        assert(mmDecRead(&s->decoder, output, 4096, &got) && got == 0);
    }
    s->state = CTX_DONE; mmServiceLock(&s->lock); mmServiceUnlock(&s->lock);
    assert(!mm.streams[0].pcm && !mm.streams[0].file.file);
    assert(!mmDecRead(&s->decoder, output, 1, &got));
    free(reference); release(s);
    // Compile the actual production seam wrappers too. Compare B with that
    // same seam layer reading the independent render, for three whole passes.
    reference = (int16_t*)load(argv[5], &referenceBytes);
    seamReference = reference; utaCa = ca; utaPost = legacyPost;
    assert(!utaDecOpenMemory(&s->decoder, NULL, 0));
    int16_t expected[4096 * 2];
    UtaSeam softwareSeam = {0}, referenceSeam = {0};
    openAgain(s, argv[4]); s->state = CTX_OPENING; s->loop = true;
    assert(utaDecOpenFile(&s->decoder, s->path)); s->state = CTX_PLAYING;
    assert(utaSeams[0].fromZero);
    softwareSeam = referenceSeam = utaSeams[0];
    for (unsigned pass = 0; pass < 3; pass++) {
        for (uint32_t at = 0; at < 12269;) {
            utaSeams[0] = referenceSeam;
            s->decoder.positionFrames = at;
            CtrAdpcmContinuation* cursor = mm.streams[0].pcm;
            mm.streams[0].pcm = NULL;
            int32_t count = utaDecRead(&s->decoder, expected, 4096);
            referenceSeam = utaSeams[0]; mm.streams[0].pcm = cursor;
            utaSeams[0] = softwareSeam; s->decoder.positionFrames = at;
            got = utaDecRead(&s->decoder, output, 4096);
            assert(got == count && !memcmp(output, expected, (size_t)got * 4));
            softwareSeam = utaSeams[0];
            assert(!memcmp(&softwareSeam, &referenceSeam, sizeof(softwareSeam)));
            at += (uint32_t)got;
        }
        assert(utaDecRead(&s->decoder, output, 4096) == 0 && utaSeams[0].atEnd);
        assert(utaDecSeek(&s->decoder, 0) && utaSeams[0].splice);
        softwareSeam = referenceSeam = utaSeams[0];
    }
    assert(softwareSeam.logged && softwareSeam.total == 132); // 3 ms at 44,100 Hz.
    s->state = CTX_DONE; mmService(ca, s, &stats); free(reference); release(s);
    // A seek arriving between prepare and the original opener's seek check
    // keeps the same source/cursor and preserves the exact frame.
    openAgain(s, argv[3]); s->state = CTX_OPENING;
    assert(mmDecOpen(&s->decoder, s->path));
    result = false;
    assert(mmDecSeek(&s->decoder, 1017, &result) && result && s->decoder.positionFrames == 1017 && !legacyOpens);
    assert(mm.streams[0].pcm && mm.streams[0].file.file && mutations == before);
    s->state = CTX_DONE; mmService(ca, s, &stats);
    release(s);
    ca->base.vtable->destroy(&ca->base);
    assert(!mm.streams && !mm.store.index && !held && messages);
    puts("Game music adapter: mono/dual-mono/true-stereo PCM feeder, exact source samples/seeks/EOF, four loop reads, bounded heap and no locked I/O passed");
    return 0;
}
