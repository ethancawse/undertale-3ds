#include "ctr_audio_system.h"

#include <3ds.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "audio_system.h"
#include "ctr_audio_decoder.h"
#include "ctr_log.h"
#include "ctr_notice.h"
#include "ctr_watchdog.h"
#include "data_win.h"
#include "gettime.h"
#include "log.h"
#include "stb_ds.h"
#include "utils.h"
#include "ctr_audio_utaudio.h" // utaudio: late code at the end of this file
#line 24

// ===[ Limits ]===

#define SOUND_INSTANCE_ID_BASE 100000
#define AUDIO_STREAM_INDEX_BASE 300000
#define INSTANCE_ID_SPAN (AUDIO_STREAM_INDEX_BASE - SOUND_INSTANCE_ID_BASE)

// One voice per NDSP channel; the voice index is the channel number.
#define VOICE_COUNT 24
// Streams that can play at once (music, crossfades, compressed SFX).
#define STREAM_CONTEXTS 8
// Per stream: 4 buffers of 4096 frames is ~370 ms of audio at 44.1 kHz.
#define STREAM_BUFFERS 4
#define STREAM_BUFFER_FRAMES 4096
// Frames one stream may decode per wake once it has 2+ buffers queued, so one
// stream cannot starve the others (or the game, on a shared core).
#define STREAM_SLICE_FRAMES 2048
#define MAX_STREAM_ENTRIES 32

#ifndef CTR_SFX_CACHE_BYTES
#define CTR_SFX_CACHE_BYTES (6u * 1024u * 1024u)
#endif
// Embedded WAVs bigger than this once decoded are streamed instead of cached.
#define SFX_CACHE_MAX_ENTRY_BYTES (2u * 1024u * 1024u)
// Embedded sounds at least this big are never read into the heap whole: they
// play straight from their section of data.win (or the audiogroup file) like
// a sound in its own file, streamed or decoded from there into the cache.
// DELTARUNE has 9 MB WAVs (cyber_battle_backing in chapters 2-5), more than
// the largest free piece of a fragmented heap.
#define AUDO_SECTION_MIN_BYTES (512u * 1024u)
// When linear memory runs short, the cache gives some back before texture
// tiles do (CtrAudioSystem_shedSfx), but it keeps this much, and never a
// sound played in the last SFX_SHED_RECENT_MS. DELTARUNE's benchmark scenes
// held at most 1.7 MB.
#define SFX_SHED_KEEP_BYTES (2u * 1024u * 1024u)
#define SFX_SHED_RECENT_MS 10000.0

#define DECODE_STACK_SIZE (128 * 1024)
#define DECODE_IDLE_TIMEOUT_NS (100LL * 1000 * 1000)
// Old 3DS: the share of the system core the decode thread may use there
// (the loader and read-ahead threads share it). In Azahar's Old 3DS, one
// 44.1 kHz stereo stream needs about 7% of a full core (UNDERTALE's music)
// to 9% (DELTARUNE's). Under libctru's usual 30% that was 22-30% of the
// capped time per stream, and 42% in Waterfall's two at once, before
// crossfades and openings; under 50% Waterfall takes 25%. The cap only
// limits: what the threads don't use, the system keeps.
#define SYSCORE_PERCENT 50u
#define STATS_INTERVAL_SECONDS 5.0f
#define SLOW_LOAD_MS 5.0

#define MESSAGE_SLOTS 16
#define MESSAGE_LENGTH 192

// Embedded sounds being read by the loader thread at once.
#define LOAD_SLOTS 16
// The loader also decodes Vorbis (see PrepJob).
#define LOADER_STACK_SIZE (64 * 1024)
// Short compressed sounds (and external files) up to this size once decoded
// are decoded into the SFX cache on the loader thread instead of being
// streamed: opening a stream costs tens of ms of decode thread time on every
// play, and some sounds play many times at once. ~5.9 s of 44.1 kHz stereo.
#define PREP_SLOTS 8
#define PREP_MAX_PCM_BYTES (1024u * 1024u)
// Streams of one sound allowed while it is being decoded into the cache; more
// copies wait for the cache.
#define CACHING_STREAMS_MAX 2
// How long a voice may wait for a stream context to be let go.
#define STREAM_WAIT_MS 250.0

#define TICKS_TO_MS(t) ((double) (t) / CPU_TICKS_PER_MSEC)

// ===[ Types ]===

typedef struct {
    uint8_t* pcm; // linear memory, NULL when not cached
    uint32_t bytes;
    uint32_t frames;
    uint32_t sampleRate;
    uint16_t format; // NDSP_FORMAT_*
    uint8_t reload; // SfxReload: what playing it again after an eviction takes
    uint8_t sourceChannels; // the file's, before identical stereo channels were stored as mono; only mono sounds pan
    uint32_t lastUse;
    u64 lastPlayTick;
} CachedSfx;

// What it takes to play a sound again once it has left the cache, cheapest
// first. A sound from its own file streams from it at once while it is decoded
// into the cache again (its first play went the same way). One kept in
// data.win waits for its data to be read in the background first: a WAV is
// then copied, compressed audio decoded on the loader thread meanwhile.
typedef enum {
    SFX_RELOAD_FILE,
    SFX_RELOAD_AUDO_WAV,
    SFX_RELOAD_AUDO_DECODE,
} SfxReload;

typedef struct {
    ndspWaveBuf wb;
    int16_t* pcm; // linear memory, STREAM_BUFFER_FRAMES stereo frames
    uint64_t startFrame; // decoder position of pcm[0]
} StreamBuffer;

typedef enum {
    CTX_FREE,
    CTX_OPEN_REQUESTED,
    CTX_OPENING, // the opener thread is opening it
    CTX_PLAYING,
    CTX_DRAINING,
    CTX_DONE,
} StreamState;

// A music stream, shared between the game thread and the decode thread.
//
// The game thread fills in the request and moves FREE/DONE -> OPEN_REQUESTED.
// From then on the opener thread owns the decoder and the path: it moves the
// state to OPENING, opens the stream and hands it to the decode thread as
// PLAYING (or ends it as DONE). From PLAYING on the decode thread owns the
// decoder, the buffers and the path, and it alone moves the state forward
// until DONE. (Without an opener thread, the decode thread opens streams
// itself, between top-ups.) Everything marked "lock"
// is only touched with the lock held, and so is the NDSP channel: the decode
// thread queues buffers under the lock and only while stopRequested is false,
// so once the game thread has set stopRequested (and cleared the channel) the
// channel is free for reuse straight away.
typedef struct {
    LightLock lock;
    volatile StreamState state; // lock for writes
    int32_t owner; // voice index or -1; game thread only
    int channel;

    // Request: written by the game thread before OPEN_REQUESTED.
    char* path; // malloc'd; freed by the decode thread
    const uint8_t* memData; // AUDO blob, not owned (kept resident)
    uint32_t memSize;
    char name[48];
    bool loop;

    // lock
    float mix[12];
    float pitch;
    bool paused;
    bool stopRequested;
    bool seekPending;
    double seekSeconds;
    bool opened; // channel configured and owned by this stream
    bool failed;
    uint32_t sampleRate;
    uint32_t channels;
    uint64_t totalFrames;

    // Decode thread only (the opener thread's until PLAYING).
    CtrDecoder decoder;
    StreamBuffer bufs[STREAM_BUFFERS];
    int nextBuf;
    uint32_t fillFrames;
    bool eof;
    bool everQueued;
    bool starved;
#ifdef CTR_AUDIO_PCM_HASH
    // Diagnosis builds: FNV-1a over every buffer queued since the stream
    // opened or last seeked (a "segment"), logged every PCM_HASH_EVERY buffers
    // and when the stream ends, to compare builds sample for sample.
    uint32_t hash;
    uint32_t hashSegment;
    uint32_t hashBuffers;
    uint64_t hashFrames;
#endif
} StreamContext;

typedef enum {
    VOICE_FREE,
    VOICE_SFX,
    VOICE_STREAM,
    VOICE_VIRTUAL, // silent mode: only a clock
    VOICE_PENDING, // waiting for the loader thread or a stream context; starts in update()
} VoiceKind;

// How a SOND asset plays, once known.
typedef enum {
    FORM_UNKNOWN,
    FORM_CACHED, // decoded into the SFX cache (and again after an eviction)
    FORM_STREAMED, // too long to cache (or not understood by the probe)
} SoundForm;

typedef enum {
    PREP_FREE,
    PREP_PROBE, // queued: find out the sound's format and length
    PREP_MEASURED, // small enough: the game thread allocates the PCM buffer
    PREP_DECODE, // queued: decode into pcm (NULL when it could not be allocated)
    PREP_BUSY, // the loader is working on it
    PREP_DONE, // the game thread installs the result
} PrepState;

// One sound being decoded into the SFX cache by the loader thread. Linear
// memory is only allocated on the game thread (libctru's allocator has no
// lock), hence the round trip through PREP_MEASURED. The loader writes the
// results while PREP_BUSY; the game thread reads them after it sees
// PREP_MEASURED or PREP_DONE.
typedef struct {
    PrepState state; // loadLock
    int32_t soundIndex;
    char* path; // external file (owned), or NULL for an AUDO blob
    const uint8_t* mem; // AUDO blob; stays loaded while the job runs
    uint32_t memSize;

    bool ok; // DONE: pcm holds `frames` frames
    bool stream; // DONE: play it as a stream instead
    uint32_t channels;
    uint32_t sampleRate;
    uint32_t frames;
    uint8_t* pcm; // linear, from the game thread
    u64 probeTicks, decodeTicks;
} PrepJob;

typedef enum {
    LOAD_FREE,
    LOAD_QUEUED,
    LOAD_BUSY,
    LOAD_DONE,
    LOAD_FAILED,
} LoadState;

// One AUDO entry read by the loader thread. The game thread fills in the
// request and queues it; the loader reads it into a malloc'd blob and marks it
// done; the game thread hands the blob to the AudioEntry and frees the slot.
typedef struct {
    LoadState state; // loadLock
    int32_t soundIndex;
    int32_t group;
    const char* path; // the group's file (CtrAudioSystem.groupPaths)
    uint32_t offset;
    uint32_t size;
    uint32_t header; // 4 when the 4 byte size field is read too (AudioEntry.sizeIsUpperBound)
    uint8_t* data;
    uint32_t dataSize;
    u64 readTicks;
} LoadRequest;

typedef struct {
    VoiceKind kind;
    int32_t instanceId;
    int32_t soundIndex;
    int32_t priority;
    uint32_t serial;
    bool loop;
    bool paused;

    float gain;
    float fadeStart, fadeTarget, fadeRemaining, fadeTotal;
    float pitch;
    // Seconds played by the game's clock: update()'s steps times the pitch,
    // from the play call, while not paused. Under the virtual clock (state-hash
    // runs) the track position comes from this, not from the DSP, so it is the
    // same in every run whenever the stream actually opened and filled.
    double clockSeconds;

    bool spatial;
    float sx, sy, sz, falloffRef, falloffMax, falloffFactor;
    float emitterGain; // audio_emitter_gain of the emitter it plays on; 1 otherwise

    // VOICE_SFX
    CachedSfx* sfx;
    ndspWaveBuf wb[2];
    uint32_t wbStart[2];
    int wbCount;

    // VOICE_STREAM
    int32_t ctx;
    float lastPosition;

    // VOICE_VIRTUAL
    double virtualSeconds;
    float virtualLength;

    // VOICE_PENDING
    float startPosition; // set_track_position before it starts
    bool waitStream; // has its source, waits for a stream context
    char* pendPath; // owned
    const uint8_t* pendMem;
    uint32_t pendMemSize;
    u64 pendSince;
} Voice;

// Where a voice's audio comes from: the SFX cache, a file (owned) or an AUDO blob.
typedef struct {
    CachedSfx* sfx;
    char* path;
    const uint8_t* mem;
    uint32_t memSize;
} VoiceSource;

typedef enum {
    SOURCE_FAILED,
    SOURCE_READY,
    SOURCE_PENDING, // being read or decoded in the background
} SourceStatus;

// A positioned voice's left and right gains move to their targets on the decode thread (rampStep), which wakes every
// DSP frame (about 5 ms), at most full scale per MIX_RAMP_SECONDS, so a pan that jumps (a sound crossing the
// listener) doesn't click. Voices that aren't positioned have no ramp: their mix is set directly, as it always was.
#define MIX_RAMP_SECONDS 0.05f

typedef struct {
    bool active;
    bool stream; // the voice's mix is streams[ctx]'s
    int ctx;
    float cur[2], tgt[2];
} MixRamp;

typedef struct {
    bool active;
    char* path;
    float initialGain;
    float initialPitch;
    float lengthSeconds; // < 0 until computed
} StreamEntry;

struct CtrAudioSystem {
    AudioSystem base;
    FileSystem* fileSystem;
    bool initialized;
    bool ndspReady;
    bool suspended;
    bool shutDown;

    Voice voices[VOICE_COUNT];
    int32_t voiceLimit;
    // Positioned voices' left/right gains (see rampStep), by voice (= DSP channel). rampLock covers them and
    // rampTick; it is taken before a stream's lock, never after.
    LightLock rampLock;
    MixRamp ramps[VOICE_COUNT];
    u64 rampTick;
    volatile bool rampsActive;
    uint32_t instanceCounter;
    uint32_t serialCounter;
    uint32_t useClock;
    float masterGain;
    float listenerGain;

    uint32_t soundCount;
    CachedSfx* sfxCache; // per SOND index
    uint32_t cacheBytes;
    float* soundLengths; // per SOND index, < 0 until known
    bool* warned; // per SOND index: unsupported format already reported
    uint8_t* soundForms; // per SOND index, SoundForm
    bool* noBackgroundRead; // per SOND index: a background read failed, read it directly

    StreamEntry streamEntries[MAX_STREAM_ENTRIES];
    StreamContext streams[STREAM_CONTEXTS];

    Thread thread;
    LightEvent event;
    volatile bool quit;
    int threadCore;
    s32 threadPriority;

    // Opens streams, so that a slow open (100 ms to 1 s on an Old 3DS's SD
    // card) never keeps the decode thread from refilling the streams already
    // playing. Same core, just below the decode thread. NULL: the decode
    // thread opens them itself.
    Thread opener;
    LightEvent openEvent;
    volatile bool openerQuit;

    // Decode and opener thread statistics (statsLock).
    LightLock statsLock;
    u64 threadBusyTicks;
    u64 threadDecodeTicks;
    double threadDecodedSeconds;
    float leastQueuedMs; // least PCM queued ahead of a playing stream at a decode pass; < 0 when none
    uint32_t opens;
    u64 longestOpenTicks;
    uint32_t threadUnderruns;
    uint32_t totalUnderruns; // since start, for CtrAudioSystem_underruns

    // Messages from the decode thread, logged by update() (logging is not thread-safe).
    LightLock msgLock;
    char messages[MESSAGE_SLOTS][MESSAGE_LENGTH];
    char** missingFiles; // decoder/opener -> main thread; allocated only on failure
    uint32_t msgHead, msgCount, msgDropped;

    // Loader thread for embedded sounds. groupPaths[i] is audio group i's
    // file, or NULL when that group can't be read in the background.
    char* dataWinPath;
    char** groupPaths; // stb_ds array, parallel to base.audioGroups
    LoadRequest loads[LOAD_SLOTS];
    PrepJob preps[PREP_SLOTS]; // loadLock, like loads
    LightLock loadLock;
    LightEvent loadEvent;
    Thread loader;
    volatile bool loaderQuit;

    // Game thread statistics.
    u64 frameTicks; // audio work since the last update() returned
    u64 windowTicks, windowMaxFrameTicks;
    u64 windowLoadTicks;
    u64 windowStart;
    uint32_t windowFrames;
    float windowSeconds;
    uint32_t windowPlays, windowCacheHits, windowCacheLoads, windowStreamStarts, windowBackgroundLoads;
    uint32_t windowDecodes; // sounds decoded into the cache by the loader
    u64 windowDecodeTicks;
};

#ifdef CTR_AUDIO_SELFTEST
void CtrAudioSelfTest_step(AudioSystem* audio, float dt);
#endif

static CtrAudioSystem* gShutdownTarget = NULL;
static bool gAtexitRegistered = false;

// ===[ Small helpers ]===

static bool isInstanceId(int32_t id) {
    return id >= SOUND_INSTANCE_ID_BASE && id < AUDIO_STREAM_INDEX_BASE;
}

static float clampPitch(float pitch) {
    if (!(pitch > 0.01f)) return 0.01f;
    if (pitch > 8.0f) return 8.0f;
    return pitch;
}

// Log a line to the SD log only, keeping routine audio chatter off the console.
// The console goes back to how it was: turning it on here put the next lines
// of the frame on the bottom screen in magnifier mode, where they stayed while
// the magnifier left a blank screen alone.
static void logQuiet(const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    bool console = CtrLog_consoleEnabled();
    CtrLog_setConsoleEnabled(false);
    logInfo("%s", line);
    CtrLog_setConsoleEnabled(console);
}

// Decode thread -> update(): queue a log line.
static void postMessage(CtrAudioSystem* ca, const char* fmt, ...) {
    char line[MESSAGE_LENGTH];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    LightLock_Lock(&ca->msgLock);
    if (ca->msgCount < MESSAGE_SLOTS) {
        memcpy(ca->messages[(ca->msgHead + ca->msgCount) % MESSAGE_SLOTS], line, sizeof(line));
        ca->msgCount++;
    } else {
        ca->msgDropped++;
    }
    LightLock_Unlock(&ca->msgLock);
}

static void drainMessages(CtrAudioSystem* ca) {
    for (;;) {
        char line[MESSAGE_LENGTH];
        uint32_t dropped = 0;
        LightLock_Lock(&ca->msgLock);
        if (ca->msgCount == 0) {
            char** missing = ca->missingFiles;
            ca->missingFiles = NULL;
            dropped = ca->msgDropped;
            ca->msgDropped = 0;
            LightLock_Unlock(&ca->msgLock);
            for (ptrdiff_t i = 0; arrlen(missing) > i; i++) {
                CtrNotice_missing("Missing audio", missing[i]);
                free(missing[i]);
            }
            arrfree(missing);
            if (dropped > 0) logQuiet("Audio: %lu decode thread messages dropped\n", (unsigned long) dropped);
            return;
        }
        memcpy(line, ca->messages[ca->msgHead], sizeof(line));
        ca->msgHead = (ca->msgHead + 1) % MESSAGE_SLOTS;
        ca->msgCount--;
        LightLock_Unlock(&ca->msgLock);
        logQuiet("%s", line);
    }
}

// A positioned voice's gain from its distance to the listener (by the model audio_falloff_set_model chose) and its
// emitter's gain.
static float spatialGain(const AudioSystem* audio, const Voice* v) {
    if (!v->spatial) return 1.0f;
    float dx = v->sx - audio->listenerX;
    float dy = v->sy - audio->listenerY;
    float dz = v->sz - audio->listenerZ;
    float distance = sqrtf(dx * dx + dy * dy + dz * dz);
    return AudioSystem_falloffGain(audio->falloffModel, distance, v->falloffRef, v->falloffMax, v->falloffFactor) * v->emitterGain;
}

static bool soundIsInAudo(const Sound* sound) {
    bool isRegular = (sound->flags & AUDIO_ENTRY_FLAG_REGULAR) == AUDIO_ENTRY_FLAG_REGULAR;
    bool isEmbedded = (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) != 0;
    bool isCompressed = (sound->flags & AUDIO_ENTRY_FLAG_IS_COMPRESSED) != 0;
    return !isRegular || isEmbedded || isCompressed;
}

static void missingAudio(CtrAudioSystem* ca, const char* file) {
    if (ca->dataWinPath == NULL) return;
    const char* slash = strrchr(ca->dataWinPath, '/');
    if (slash == NULL) return;
    char path[700];
    int length = snprintf(path, sizeof(path), "%.*s/%s", (int) (slash - ca->dataWinPath), ca->dataWinPath, file);
    if (length >= 0 && (size_t) length < sizeof(path)) CtrNotice_missing("Missing audio", path);
}

static char* resolveExternalPath(CtrAudioSystem* ca, const Sound* sound) {
    const char* file = sound->file;
    if (file == NULL || file[0] == '\0') return NULL;
    char filename[512];
    if (strchr(file, '.') != NULL) snprintf(filename, sizeof(filename), "%s", file);
    else snprintf(filename, sizeof(filename), "%s.ogg", file);
    return ca->fileSystem->vtable->resolvePath(ca->fileSystem, filename);
}

static bool pathIsMp3(const char* path) {
    size_t length = strlen(path);
    return length >= 4 && strcasecmp(path + length - 4, ".mp3") == 0;
}

static const char* baseName(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

// Returns the AUDO entry of an embedded sound, or NULL (with a warning) if the
// sound's group or file index is out of range.
static AudioEntry* audoEntryFor(CtrAudioSystem* ca, const Sound* sound, DataWin** outGroup) {
    if (sound->audioGroup < 0 || sound->audioGroup >= (int32_t) arrlen(ca->base.audioGroups)) {
        logWarn("Audio: sound '%s' is in audio group %d, which is not loaded\n", sound->name, sound->audioGroup);
        return NULL;
    }
    DataWin* group = ca->base.audioGroups[sound->audioGroup];
    // The group's file is missing, which ctrGroupLoad reported once.
    if (group->audo.count == 0) return NULL;
    if (sound->audioFile < 0 || (uint32_t) sound->audioFile >= group->audo.count) {
        logWarn("Audio: invalid audio file index %d for sound '%s'\n", sound->audioFile, sound->name);
        return NULL;
    }
    *outGroup = group;
    return &group->audo.entries[sound->audioFile];
}

// The section path (CtrDecoder_sectionPath) a big embedded sound plays from,
// or NULL when it is read into memory as usual: small, already in memory, or
// in a group read whole.
static char* audoSectionPath(CtrAudioSystem* ca, const Sound* sound, const DataWin* group, const AudioEntry* entry) {
    if (entry->data != NULL || !entry->present || entry->dataSize < AUDO_SECTION_MIN_BYTES) return NULL;
    if (group->mappedFile != NULL || group->lazyLoadFile == NULL) return NULL;
    if (sound->audioGroup >= (int32_t) arrlen(ca->groupPaths) || ca->groupPaths[sound->audioGroup] == NULL) return NULL;
    return CtrDecoder_sectionPath(ca->groupPaths[sound->audioGroup], entry->dataOffset, entry->dataSize, entry->sizeIsUpperBound);
}

// Loads an AUDO entry. DataWin_loadAudoIfNeeded reads through data.win's FILE,
// whose 128 KB buffer turns every small sound into a 128 KB read; reading the
// exact range through the descriptor is several times faster for short SFX.
// The descriptor offset is restored afterwards, so the FILE's own idea of its
// position (and its buffer) stays valid. Out of memory, the entry is left
// without data: the sound is skipped, never the game stopped.
static void loadAudoBlob(DataWin* group, uint32_t audioFile) {
    AudioEntry* entry = &group->audo.entries[audioFile];
    if (entry->data != NULL || !entry->present || entry->dataSize == 0) return;
    if (group->lazyLoadFile != NULL && group->mappedFile == NULL) {
        int fd = fileno(group->lazyLoadFile);
        off_t saved = fd >= 0 ? lseek(fd, 0, SEEK_CUR) : -1;
        if (saved >= 0) {
            // The parser may have left the size unread (see AudioEntry); it is the 4 bytes before the data.
            uint32_t header = entry->sizeIsUpperBound ? 4 : 0;
            uint32_t total = entry->dataSize + header;
            off_t start = (off_t) (entry->dataOffset - header);
            uint8_t* data = (uint8_t*) malloc(total);
            if (data == NULL) return;
            bool ok = lseek(fd, start, SEEK_SET) == start;
            uint32_t done = 0;
            while (ok && done < total) {
                ssize_t n = read(fd, data + done, total - done);
                if (n <= 0) ok = false;
                else done += (uint32_t) n;
            }
            lseek(fd, saved, SEEK_SET);
            if (ok) {
                if (header > 0) {
                    uint32_t exact;
                    memcpy(&exact, data, sizeof(exact));
                    if (exact <= entry->dataSize) entry->dataSize = exact;
                    memmove(data, data + header, entry->dataSize);
                    entry->sizeIsUpperBound = false;
                }
                entry->data = data;
                return;
            }
            free(data);
        }
    }
    DataWin_loadAudoIfNeeded(group, audioFile);
}

// The blob of an AUDO entry can be dropped once decoded only when the group
// reads AUDO lazily from its file (then DataWin_loadAudoIfNeeded reloads it).
static void releaseAudoBlob(DataWin* group, AudioEntry* entry) {
    if (group->mappedFile == NULL && group->lazyLoadAudio && group->lazyLoadFile != NULL && entry->data != NULL) {
        free(entry->data);
        entry->data = NULL;
    }
}

// ===[ Decode thread ]===

static int countQueued(const StreamContext* s) {
    int queued = 0;
    for (int i = 0; i < STREAM_BUFFERS; i++) {
        u8 status = s->bufs[i].wb.status;
        if (status == NDSP_WBUF_QUEUED || status == NDSP_WBUF_PLAYING) queued++;
    }
    return queued;
}

static void finishStream(StreamContext* s) {
    CtrDecoder_close(&s->decoder);
    free(s->path);
    s->path = NULL;
    s->memData = NULL;
    LightLock_Lock(&s->lock);
    s->opened = false;
    s->state = CTX_DONE;
    LightLock_Unlock(&s->lock);
}

// Frames of PCM queued on a stream's channel that the DSP has yet to play. Under the lock.
static uint32_t framesAhead(const StreamContext* s) {
    u16 seq = ndspChnGetWaveBufSeq(s->channel);
    u32 played = ndspChnGetSamplePos(s->channel);
    uint32_t frames = 0;
    for (int i = 0; i < STREAM_BUFFERS; i++) {
        const ndspWaveBuf* wb = &s->bufs[i].wb;
        if (wb->status != NDSP_WBUF_QUEUED && wb->status != NDSP_WBUF_PLAYING) continue;
        frames += wb->nsamples;
        if (wb->sequence_id == seq) frames -= played < wb->nsamples ? played : wb->nsamples;
    }
    return frames;
}

#ifdef CTR_AUDIO_PCM_HASH
#define PCM_HASH_EVERY 32

static void pcmHashStart(StreamContext* s) {
    s->hash = 2166136261u;
    s->hashSegment++;
    s->hashBuffers = 0;
    s->hashFrames = 0;
}

static void pcmHashLog(CtrAudioSystem* ca, StreamContext* s, const char* why);

static void pcmHashAdd(CtrAudioSystem* ca, StreamContext* s, const int16_t* pcm, uint32_t frames) {
    const uint8_t* p = (const uint8_t*) pcm;
    uint32_t bytes = frames * s->channels * sizeof(int16_t);
    uint32_t h = s->hash;
    for (uint32_t i = 0; i < 4; i++) h = (h ^ ((frames >> (8 * i)) & 0xFFu)) * 16777619u;
    for (uint32_t i = 0; i < bytes; i++) h = (h ^ p[i]) * 16777619u;
    s->hash = h;
    s->hashBuffers++;
    s->hashFrames += frames;
    if (s->hashBuffers % PCM_HASH_EVERY == 0) pcmHashLog(ca, s, "at");
}
#endif

typedef struct {
    u64 decodeTicks;
    double decodedSeconds;
    uint32_t underruns;
    u64 passStart; // when this pass began
    u64 lastPassStart; // when the previous one began
} DecodeStats;

#ifdef CTR_AUDIO_PCM_HASH
static void pcmHashLog(CtrAudioSystem* ca, StreamContext* s, const char* why) {
    if (s->hashBuffers == 0) return;
    postMessage(ca, "Audio: pcm '%s' segment %lu %s buffer %lu, %llu frames, hash %08lX\n", s->name, (unsigned long) s->hashSegment,
                why, (unsigned long) s->hashBuffers, (unsigned long long) s->hashFrames, (unsigned long) s->hash);
}
#endif

// Moves a stream to a position, as a seek the game asked for. Decode thread,
// or the opener thread for a seek asked for before the stream opened.
static void seekStream(CtrAudioSystem* ca, StreamContext* s, double seekSeconds, u64* ticks) {
    uint64_t frame = seekSeconds > 0 ? (uint64_t) (seekSeconds * s->sampleRate) : 0;
    if (s->totalFrames > 0 && frame >= s->totalFrames) frame = s->loop ? frame % s->totalFrames : s->totalFrames;
    u64 t0 = svcGetSystemTick();
    if (!CtrDecoder_seek(&s->decoder, frame)) {
        postMessage(ca, "Audio: seek to %.2f s failed in '%s'\n", seekSeconds, s->name);
        CtrDecoder_seek(&s->decoder, 0);
    }
    *ticks += svcGetSystemTick() - t0;
}

// Opens a stream the game asked for: OPEN_REQUESTED -> OPENING -> PLAYING,
// or DONE if it was stopped or can't be opened. A seek asked for before the
// open is done here too, so the decode thread gets a stream ready to fill.
// Returns true if it tried to open one.
static bool openStream(CtrAudioSystem* ca, StreamContext* s) {
    LightLock_Lock(&s->lock);
    bool requested = s->state == CTX_OPEN_REQUESTED;
    bool stop = s->stopRequested;
    if (requested && !stop) s->state = CTX_OPENING;
    LightLock_Unlock(&s->lock);
    if (!requested) return false;
    if (stop) {
        finishStream(s);
        return false;
    }

    u64 t0 = svcGetSystemTick();
    bool ok = s->path != NULL ? CtrDecoder_openFile(&s->decoder, s->path) : CtrDecoder_openMemory(&s->decoder, s->memData, s->memSize);
    u64 openTicks = svcGetSystemTick() - t0;
    double openMs = TICKS_TO_MS(openTicks);
    bool readAhead = ok && s->path != NULL && CtrDecoder_startReadAhead(&s->decoder);

    // A position set before the stream opened.
    double seekSeconds = 0.0;
    bool doSeek = false;
    LightLock_Lock(&s->lock);
    if (ok && s->seekPending && !s->stopRequested) {
        s->seekPending = false;
        seekSeconds = s->seekSeconds;
        doSeek = true;
        s->sampleRate = s->decoder.sampleRate;
        s->totalFrames = s->decoder.totalFrames;
    }
    LightLock_Unlock(&s->lock);
#ifdef CTR_AUDIO_PCM_HASH
    s->hashSegment = 0;
    pcmHashStart(s);
#endif
    u64 seekTicks = 0;
    if (doSeek) {
        seekStream(ca, s, seekSeconds, &seekTicks);
#ifdef CTR_AUDIO_PCM_HASH
        pcmHashStart(s);
#endif
    }

    LightLock_Lock(&s->lock);
    bool proceed = ok && !s->stopRequested;
    if (proceed) {
        int ch = s->channel;
        s->sampleRate = s->decoder.sampleRate;
        s->channels = s->decoder.channels;
        s->totalFrames = s->decoder.totalFrames;
        ndspChnReset(ch);
        ndspChnSetInterp(ch, NDSP_INTERP_POLYPHASE);
        ndspChnSetFormat(ch, s->channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
        ndspChnSetRate(ch, (float) s->sampleRate * clampPitch(s->pitch));
        ndspChnSetMix(ch, s->mix);
        ndspChnSetPaused(ch, s->paused);
        for (int i = 0; i < STREAM_BUFFERS; i++) s->bufs[i].wb.status = NDSP_WBUF_FREE;
        s->nextBuf = 0;
        s->fillFrames = 0;
        s->eof = false;
        s->everQueued = false;
        s->starved = false;
        s->opened = true;
        s->state = CTX_PLAYING;
    } else if (!ok) {
        s->failed = true;
    }
    LightLock_Unlock(&s->lock);

    LightLock_Lock(&ca->statsLock);
    ca->opens++;
    if (openTicks + seekTicks > ca->longestOpenTicks) ca->longestOpenTicks = openTicks + seekTicks;
    LightLock_Unlock(&ca->statsLock);

    if (!proceed) {
        if (!ok) {
            postMessage(ca, "Audio: could not open stream '%s' (missing, or not Ogg Vorbis, WAV or an MP4 with AAC audio)\n", s->name);
            // resolvePath builds a path even for missing files. Check only
            // after an open fails, and hand the full path to the main thread.
            if (s->path != NULL && access(s->path, F_OK) != 0 && (errno == ENOENT || errno == ENOTDIR)) {
                char* missing = safeStrdup(s->path);
                LightLock_Lock(&ca->msgLock);
                arrput(ca->missingFiles, missing);
                LightLock_Unlock(&ca->msgLock);
            }
        }
        finishStream(s);
        return true;
    }
    postMessage(ca, "Audio: stream '%s' started: %lu Hz, %lu ch, %.1f s, opened in %.1f ms%s%s\n", s->name,
                (unsigned long) s->sampleRate, (unsigned long) s->channels,
                s->sampleRate > 0 ? (double) s->totalFrames / s->sampleRate : 0.0, openMs,
                s->path == NULL ? "" : readAhead ? ", read ahead" : ", read directly (no free ring)",
                doSeek ? ", seeked" : "");
    LightEvent_Signal(&ca->event);
    return true;
}

// Tops up one playing stream and finishes it once it has played out or was
// stopped. Opening is the opener thread's (openStream).
#ifdef CTRPORT_ADPCM_MUSIC
#define MM_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text.late.adpcm")))
MM_CODE static bool mmDecOpen(CtrDecoder*, const char*);
MM_CODE static bool mmDecSeek(CtrDecoder*, uint64_t, bool*);
MM_CODE static bool mmDecRead(CtrDecoder*, int16_t*, uint32_t, int32_t*);
MM_CODE static void mmService(CtrAudioSystem*, StreamContext*, DecodeStats*);
MM_CODE static void mmServiceLock(LightLock*);
MM_CODE static void mmServiceUnlock(LightLock*);
MM_CODE static bool mmControl(int, unsigned, const void*);
MM_CODE static void mmFrame(void);
MM_CODE static int mmSfxAdmit(int32_t, uint32_t, uint32_t);
MM_CODE static bool mmSfxPlanned(int32_t);
#elif defined(CTRPORT_ADPCM_SFXPACK)
#define MM_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text.late.adpcm")))
MM_CODE static int apAdmit(int32_t, uint32_t, uint32_t);
MM_CODE static int apQueue(int32_t, uint32_t, uint32_t);
#endif
#line 827
static void serviceStream(CtrAudioSystem* ca, StreamContext* s, DecodeStats* stats) {
#ifdef CTRPORT_ADPCM_MUSIC
    mmServiceLock(&s->lock);
#else
    LightLock_Lock(&s->lock);
#endif
    StreamState state = s->state;
    bool stop = s->stopRequested;
#ifdef CTRPORT_ADPCM_MUSIC
    mmServiceUnlock(&s->lock);
#else
    LightLock_Unlock(&s->lock);
#endif
#line 832
    if (state != CTX_PLAYING && state != CTX_DRAINING) return;
    if (stop) {
#ifdef CTR_AUDIO_PCM_HASH
        pcmHashLog(ca, s, "stopped after");
#endif
        finishStream(s);
        return;
    }

    // A seek drops everything queued and refills from the new position.
    bool doSeek = false;
    double seekSeconds = 0.0;
    LightLock_Lock(&s->lock);
    if (s->seekPending) {
        s->seekPending = false;
        seekSeconds = s->seekSeconds;
        doSeek = true;
        ndspChnWaveBufClear(s->channel);
        for (int i = 0; i < STREAM_BUFFERS; i++) s->bufs[i].wb.status = NDSP_WBUF_FREE;
        s->nextBuf = 0;
        s->fillFrames = 0;
        s->eof = false;
        s->everQueued = false;
        s->starved = false;
        s->state = CTX_PLAYING;
        state = CTX_PLAYING;
    }
    LightLock_Unlock(&s->lock);
    if (doSeek) {
#ifdef CTR_AUDIO_PCM_HASH
        pcmHashLog(ca, s, "seeked after");
        pcmHashStart(s);
#endif
        seekStream(ca, s, seekSeconds, &stats->decodeTicks);
    }

    if (state == CTX_PLAYING) {
        uint32_t budget = STREAM_SLICE_FRAMES;
        for (;;) {
            LightLock_Lock(&s->lock);
            int queued = countQueued(s);
            StreamBuffer* b = &s->bufs[s->nextBuf];
            bool bufFree = b->wb.status == NDSP_WBUF_FREE || b->wb.status == NDSP_WBUF_DONE;
            bool interrupted = s->stopRequested || s->seekPending;
            // A stop or seek empties the queue on purpose.
            bool underrun = !interrupted && queued == 0 && s->everQueued && !s->starved && !s->paused;
            if (underrun) s->starved = true;
            if (queued > 0) s->starved = false;
            LightLock_Unlock(&s->lock);

            if (underrun) {
                stats->underruns++;
                postMessage(ca, "Audio: underrun in stream '%s' (%.0f ms since the previous decode pass began)\n", s->name,
                            TICKS_TO_MS(stats->passStart - stats->lastPassStart));
            }
            if (interrupted || !bufFree) break;
            if (budget == 0 && queued >= 2) break;

            if (s->fillFrames == 0) b->startFrame = s->decoder.positionFrames;
            u64 t0 = svcGetSystemTick();
            int32_t got = CtrDecoder_read(&s->decoder, b->pcm + s->fillFrames * s->channels, STREAM_BUFFER_FRAMES - s->fillFrames);
            stats->decodeTicks += svcGetSystemTick() - t0;
            if (got > 0) {
                s->fillFrames += (uint32_t) got;
                budget = (uint32_t) got >= budget ? 0 : budget - (uint32_t) got;
                stats->decodedSeconds += (double) got / (double) s->sampleRate;
            } else {
                if (got < 0) postMessage(ca, "Audio: decode error in stream '%s'\n", s->name);
                bool looped = false;
                if (got == 0 && s->loop && s->decoder.positionFrames > 0) {
                    t0 = svcGetSystemTick();
                    looped = CtrDecoder_seek(&s->decoder, 0);
                    stats->decodeTicks += svcGetSystemTick() - t0;
                }
                if (!looped) s->eof = true;
            }

            if (s->fillFrames == STREAM_BUFFER_FRAMES || (s->eof && s->fillFrames > 0)) {
                uint32_t bytes = s->fillFrames * s->channels * sizeof(int16_t);
                DSP_FlushDataCache(b->pcm, bytes);
                b->wb.data_pcm16 = b->pcm;
                b->wb.nsamples = s->fillFrames;
                b->wb.looping = false;
                LightLock_Lock(&s->lock);
                bool queue = !s->stopRequested && !s->seekPending;
                if (queue) {
                    ndspChnWaveBufAdd(s->channel, &b->wb);
                    s->everQueued = true;
                }
                LightLock_Unlock(&s->lock);
#ifdef CTR_AUDIO_PCM_HASH
                if (queue) pcmHashAdd(ca, s, b->pcm, s->fillFrames);
#endif
                s->nextBuf = (s->nextBuf + 1) % STREAM_BUFFERS;
                s->fillFrames = 0;
            }

            if (s->eof) {
                LightLock_Lock(&s->lock);
                if (s->state == CTX_PLAYING) s->state = CTX_DRAINING;
                LightLock_Unlock(&s->lock);
                state = CTX_DRAINING;
                break;
            }
        }
    }

    if (state == CTX_DRAINING) {
        LightLock_Lock(&s->lock);
        bool drained = countQueued(s) == 0 && !s->seekPending;
        LightLock_Unlock(&s->lock);
        if (drained) {
            postMessage(ca, "Audio: stream '%s' finished\n", s->name);
#ifdef CTR_AUDIO_PCM_HASH
            pcmHashLog(ca, s, "finished after");
#endif
            finishStream(s);
        }
    }
}

// The least PCM any playing stream has queued, in ms, or -1 when none is playing.
static float leastQueuedMs(CtrAudioSystem* ca) {
    float least = -1.0f;
    for (int i = 0; i < STREAM_CONTEXTS; i++) {
        StreamContext* s = &ca->streams[i];
        LightLock_Lock(&s->lock);
        if (s->state == CTX_PLAYING && s->opened && s->everQueued && !s->paused && !s->stopRequested && !s->seekPending &&
            s->sampleRate > 0) {
            float ms = (float) framesAhead(s) * 1000.0f / ((float) s->sampleRate * clampPitch(s->pitch));
            if (least < 0.0f || ms < least) least = ms;
        }
        LightLock_Unlock(&s->lock);
    }
    return least;
}

static void rampStep(CtrAudioSystem* ca);

static void decodeThreadMain(void* arg) {
    CtrAudioSystem* ca = (CtrAudioSystem*) arg;
    CtrWatchdog_nameThread("audio decode");
    u64 lastPassStart = svcGetSystemTick();
    while (!ca->quit) {
        CtrWatchdog_note("waiting for the DSP");
        LightEvent_WaitTimeout(&ca->event, DECODE_IDLE_TIMEOUT_NS);
        if (ca->quit) break;
        rampStep(ca);
        CtrWatchdog_note("servicing streams");
        u64 start = svcGetSystemTick();
        DecodeStats stats = {0};
        stats.passStart = start;
        stats.lastPassStart = lastPassStart;
        lastPassStart = start;
        float least = leastQueuedMs(ca);
        for (int i = 0; i < STREAM_CONTEXTS; i++) serviceStream(ca, &ca->streams[i], &stats);
        // Without an opener thread: open at most one new stream, after the
        // playing ones have been topped up.
        if (ca->opener == NULL) {
            for (int i = 0; i < STREAM_CONTEXTS; i++) {
                if (openStream(ca, &ca->streams[i])) break;
            }
        }
        u64 busy = svcGetSystemTick() - start;
        LightLock_Lock(&ca->statsLock);
        ca->threadBusyTicks += busy;
        ca->threadDecodeTicks += stats.decodeTicks;
        ca->threadDecodedSeconds += stats.decodedSeconds;
        if (least >= 0.0f && (ca->leastQueuedMs < 0.0f || least < ca->leastQueuedMs)) ca->leastQueuedMs = least;
        ca->threadUnderruns += stats.underruns;
        ca->totalUnderruns += stats.underruns;
        LightLock_Unlock(&ca->statsLock);
    }
}

static void openerThreadMain(void* arg) {
    CtrAudioSystem* ca = (CtrAudioSystem*) arg;
    CtrWatchdog_nameThread("audio open");
    while (!ca->openerQuit) {
        CtrWatchdog_note("waiting for a stream to open");
        LightEvent_WaitTimeout(&ca->openEvent, DECODE_IDLE_TIMEOUT_NS);
        if (ca->openerQuit) break;
        CtrWatchdog_note("opening streams");
        for (int i = 0; i < STREAM_CONTEXTS && !ca->openerQuit; i++) openStream(ca, &ca->streams[i]);
    }
}

uint32_t CtrAudioSystem_underruns(CtrAudioSystem* ca) {
    LightLock_Lock(&ca->statsLock);
    uint32_t n = ca->totalUnderruns;
    LightLock_Unlock(&ca->statsLock);
    return n;
}

// Runs on NDSP's thread once per DSP frame (~4.9 ms).
static void ndspFrameCallback(void* data) {
    LightEvent_Signal(&((CtrAudioSystem*) data)->event);
}

// ===[ Loader thread ]===

// Reads one request through the loader's own descriptor (data.win's FILE
// belongs to the game thread). Returns the blob, or NULL.
static uint8_t* readRequest(int fd, const LoadRequest* req, uint32_t* outSize) {
    uint32_t total = req->size + req->header;
    uint8_t* data = (uint8_t*) malloc(total > 0 ? total : 1);
    if (data == NULL) return NULL;
    bool ok = lseek(fd, (off_t) (req->offset - req->header), SEEK_SET) == (off_t) (req->offset - req->header);
    uint32_t done = 0;
    while (ok && done < total) {
        ssize_t n = read(fd, data + done, total - done);
        if (n <= 0) ok = false;
        else done += (uint32_t) n;
    }
    if (!ok) {
        free(data);
        return NULL;
    }
    uint32_t size = req->size;
    if (req->header > 0) {
        uint32_t exact;
        memcpy(&exact, data, sizeof(exact));
        if (exact <= size) size = exact;
        memmove(data, data + req->header, size);
    }
    *outSize = size;
    return data;
}

// Loader thread: decides whether a sound is short enough to cache. The probe
// reads only headers, so a long track costs a file open and two reads here.
static PrepState runPrepProbe(PrepJob* job) {
    u64 start = svcGetSystemTick();
    CtrAudioInfo info;
    bool ok = job->path != NULL ? CtrDecoder_probeFile(job->path, &info) : CtrDecoder_probeMemory(job->mem, job->memSize, &info);
    job->probeTicks = svcGetSystemTick() - start;
    job->ok = false;
    job->stream = false;
    if (!ok) return PREP_DONE;
    job->channels = info.channels;
    job->sampleRate = info.sampleRate;
    job->frames = info.totalFrames <= UINT32_MAX ? (uint32_t) info.totalFrames : 0;
    uint64_t bytes = info.totalFrames * info.channels * sizeof(int16_t);
    if (job->frames == 0 || bytes > PREP_MAX_PCM_BYTES) {
        job->stream = true;
        return PREP_DONE;
    }
    return PREP_MEASURED;
}

// Loader thread: decodes the whole sound into the buffer the game thread gave.
// A failure here falls back to streaming, which reports its own errors.
static void runPrepDecode(CtrAudioSystem* ca, PrepJob* job) {
    u64 start = svcGetSystemTick();
    job->ok = false;
    job->stream = true;
    if (job->pcm == NULL) return;
    CtrDecoder dec;
    bool opened = job->path != NULL ? CtrDecoder_openFile(&dec, job->path) : CtrDecoder_openMemory(&dec, job->mem, job->memSize);
    uint32_t done = 0;
    if (opened && dec.channels == job->channels) {
        int16_t* out = (int16_t*) job->pcm;
        while (done < job->frames && !ca->loaderQuit) {
            int32_t got = CtrDecoder_read(&dec, out + done * job->channels, job->frames - done);
            if (0 >= got) break;
            done += (uint32_t) got;
        }
        job->sampleRate = dec.sampleRate;
    }
    if (opened) CtrDecoder_close(&dec);
    if (done > 0 && !ca->loaderQuit) {
        job->frames = done;
        DSP_FlushDataCache(job->pcm, done * job->channels * sizeof(int16_t));
        job->ok = true;
        job->stream = false;
    }
    job->decodeTicks = svcGetSystemTick() - start;
}

static void loaderThreadMain(void* arg) {
    CtrAudioSystem* ca = (CtrAudioSystem*) arg;
    int fd = -1;
    const char* openPath = NULL;
    CtrWatchdog_nameThread("audio loader");
    while (!ca->loaderQuit) {
        CtrWatchdog_note("waiting for work");
        LightEvent_Wait(&ca->loadEvent);
        CtrWatchdog_note("taking a job");
        for (;;) {
            if (ca->loaderQuit) break;
            LoadRequest* req = NULL;
            PrepJob* job = NULL;
            PrepState jobState = PREP_FREE;
            // Reads first: they are quick, and a voice may be waiting on one.
            LightLock_Lock(&ca->loadLock);
            for (int i = 0; i < LOAD_SLOTS; i++) {
                if (ca->loads[i].state == LOAD_QUEUED) {
                    req = &ca->loads[i];
                    req->state = LOAD_BUSY;
                    break;
                }
            }
            for (int i = 0; req == NULL && i < PREP_SLOTS; i++) {
                if (ca->preps[i].state == PREP_PROBE || ca->preps[i].state == PREP_DECODE) {
                    job = &ca->preps[i];
                    jobState = job->state;
                    job->state = PREP_BUSY;
                    break;
                }
            }
            LightLock_Unlock(&ca->loadLock);
            if (job != NULL) {
                PrepState next = PREP_DONE;
                CtrWatchdog_note(jobState == PREP_PROBE ? "probing a sound to decode" : "decoding a sound into the cache");
                if (jobState == PREP_PROBE) next = runPrepProbe(job);
                else runPrepDecode(ca, job);
                CtrWatchdog_note("finishing a decode");
                LightLock_Lock(&ca->loadLock);
                job->state = next;
                LightLock_Unlock(&ca->loadLock);
                CtrWatchdog_note("taking a job");
                continue;
            }
            if (req == NULL) break;

            CtrWatchdog_note("reading a sound");
            u64 start = svcGetSystemTick();
            if (openPath != req->path) {
                if (fd >= 0) close(fd);
                fd = open(req->path, O_RDONLY);
                openPath = fd >= 0 ? req->path : NULL;
            }
            uint32_t size = 0;
            uint8_t* data = fd >= 0 ? readRequest(fd, req, &size) : NULL;
            LightLock_Lock(&ca->loadLock);
            req->data = data;
            req->dataSize = size;
            req->readTicks = svcGetSystemTick() - start;
            req->state = data != NULL ? LOAD_DONE : LOAD_FAILED;
            LightLock_Unlock(&ca->loadLock);
            CtrWatchdog_note("taking a job");
        }
    }
    if (fd >= 0) close(fd);
}

static bool startLoaderThread(CtrAudioSystem* ca) {
    // On the decode thread's core and below it (and below the game thread):
    // it mostly waits on the SD card, and its decoding into the SFX cache must
    // never delay the streams. Sharing the game's core (Old 3DS without the
    // system core) it gets only the time the game leaves.
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    priority += 1;
    if (priority > 0x3F) priority = 0x3F;
    ca->loaderQuit = false;
    ca->loader = threadCreate(loaderThreadMain, ca, LOADER_STACK_SIZE, priority, ca->threadCore == 0 ? -2 : ca->threadCore, false);
    return ca->loader != NULL;
}

static void stopLoaderThread(CtrAudioSystem* ca) {
    if (ca->loader == NULL) return;
    ca->loaderQuit = true;
    LightEvent_Signal(&ca->loadEvent);
    threadJoin(ca->loader, U64_MAX);
    threadFree(ca->loader);
    ca->loader = NULL;
    for (int i = 0; i < LOAD_SLOTS; i++) {
        free(ca->loads[i].data);
        ca->loads[i].data = NULL;
        ca->loads[i].state = LOAD_FREE;
    }
    for (int i = 0; i < PREP_SLOTS; i++) {
        PrepJob* job = &ca->preps[i];
        free(job->path);
        if (job->pcm != NULL) linearFree(job->pcm);
        memset(job, 0, sizeof(*job));
        job->state = PREP_FREE;
    }
}

// Queues a background read of a sound's AUDO entry. False when it can't be
// read in the background (the caller then loads it synchronously).
static bool requestLoad(CtrAudioSystem* ca, int32_t soundIndex, int32_t groupIndex, DataWin* group, const AudioEntry* entry) {
    if (ca->loader == NULL || groupIndex < 0 || groupIndex >= (int32_t) arrlen(ca->groupPaths) || ca->groupPaths[groupIndex] == NULL) return false;
    if (group->lazyLoadFile == NULL || group->mappedFile != NULL || !entry->present || entry->dataSize == 0) return false;
    LightLock_Lock(&ca->loadLock);
    int freeSlot = -1;
    for (int i = 0; i < LOAD_SLOTS; i++) {
        LoadRequest* r = &ca->loads[i];
        if (r->state == LOAD_FREE) {
            if (freeSlot < 0) freeSlot = i;
        } else if (r->soundIndex == soundIndex && r->state != LOAD_FAILED) {
            LightLock_Unlock(&ca->loadLock);
            return true;
        }
    }
    if (freeSlot >= 0) {
        LoadRequest* r = &ca->loads[freeSlot];
        r->soundIndex = soundIndex;
        r->group = groupIndex;
        r->path = ca->groupPaths[groupIndex];
        r->header = entry->sizeIsUpperBound ? 4 : 0;
        r->offset = entry->dataOffset;
        r->size = entry->dataSize;
        r->data = NULL;
        r->dataSize = 0;
        r->state = LOAD_QUEUED;
    }
    LightLock_Unlock(&ca->loadLock);
    if (freeSlot < 0) return false;
    LightEvent_Signal(&ca->loadEvent);
    return true;
}

// Whether a background read of the sound is still on its way.
static bool loadInFlight(CtrAudioSystem* ca, int32_t soundIndex) {
    bool inFlight = false;
    LightLock_Lock(&ca->loadLock);
    for (int i = 0; i < LOAD_SLOTS; i++) {
        LoadState state = ca->loads[i].state;
        if (ca->loads[i].soundIndex == soundIndex && (state == LOAD_QUEUED || state == LOAD_BUSY || state == LOAD_DONE)) inFlight = true;
    }
    LightLock_Unlock(&ca->loadLock);
    return inFlight;
}

// Hands finished reads to their AudioEntry. Game thread.
static void collectLoads(CtrAudioSystem* ca) {
    for (int i = 0; i < LOAD_SLOTS; i++) {
        LoadRequest* r = &ca->loads[i];
        LightLock_Lock(&ca->loadLock);
        LoadState state = r->state;
        LightLock_Unlock(&ca->loadLock);
        if (state != LOAD_DONE && state != LOAD_FAILED) continue;

        const Sound* sound = &ca->base.audioGroups[0]->sond.sounds[r->soundIndex];
        if (state == LOAD_DONE && r->group < (int32_t) arrlen(ca->base.audioGroups)) {
            DataWin* group = ca->base.audioGroups[r->group];
            AudioEntry* entry = &group->audo.entries[sound->audioFile];
            if (entry->data == NULL) {
                entry->data = r->data;
                entry->dataSize = r->dataSize;
                entry->sizeIsUpperBound = false;
                r->data = NULL;
            }
            ca->windowBackgroundLoads++;
            if (TICKS_TO_MS(r->readTicks) > SLOW_LOAD_MS) {
                logQuiet("Audio: read '%s' (%lu bytes) in the background in %.1f ms\n", sound->name, (unsigned long) r->dataSize,
                         TICKS_TO_MS(r->readTicks));
            }
        } else if (state == LOAD_FAILED) {
            logQuiet("Audio: background read of '%s' failed, loading it directly\n", sound->name);
            ca->noBackgroundRead[r->soundIndex] = true;
        }
        free(r->data);
        r->data = NULL;
        LightLock_Lock(&ca->loadLock);
        r->state = LOAD_FREE;
        LightLock_Unlock(&ca->loadLock);
    }
}

// Queues decoding a sound into the SFX cache. Takes ownership of path when it
// returns true; false when there is no loader or no free slot.
static bool requestPrepare(CtrAudioSystem* ca, int32_t soundIndex, char* path, const uint8_t* mem, uint32_t memSize) {
    if (ca->loader == NULL) return false;
    PrepJob* job = NULL;
    LightLock_Lock(&ca->loadLock);
    for (int i = 0; i < PREP_SLOTS && job == NULL; i++) {
        if (ca->preps[i].state != PREP_FREE) continue;
        job = &ca->preps[i];
        memset(job, 0, sizeof(*job));
        job->soundIndex = soundIndex;
        job->path = path;
        job->mem = mem;
        job->memSize = memSize;
        job->state = PREP_PROBE;
    }
    LightLock_Unlock(&ca->loadLock);
    if (job == NULL) return false;
    LightEvent_Signal(&ca->loadEvent);
    return true;
}

static bool prepInFlight(CtrAudioSystem* ca, int32_t soundIndex) {
    bool inFlight = false;
    LightLock_Lock(&ca->loadLock);
    for (int i = 0; i < PREP_SLOTS; i++) {
        if (ca->preps[i].state != PREP_FREE && ca->preps[i].soundIndex == soundIndex) inFlight = true;
    }
    LightLock_Unlock(&ca->loadLock);
    return inFlight;
}

// ===[ SFX cache ]===

static bool sfxInUse(CtrAudioSystem* ca, const CachedSfx* c) {
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (ca->voices[i].kind == VOICE_SFX && ca->voices[i].sfx == c) return true;
    }
    return false;
}

static void freeSfx(CtrAudioSystem* ca, CachedSfx* c) {
    linearFree(c->pcm);
    ca->cacheBytes -= c->bytes;
    c->pcm = NULL;
    c->bytes = 0;
}

// Evicts least recently used entries that no voice is playing until `need`
// more bytes fit under the cap (or nothing else can go). Returns false if
// nothing could be evicted.
static bool evictSfx(CtrAudioSystem* ca, uint32_t need, bool force) {
    bool evictedAny = false;
    while (force || ca->cacheBytes + need > CTR_SFX_CACHE_BYTES) {
        CachedSfx* victim = NULL;
        for (uint32_t i = 0; i < ca->soundCount; i++) {
            CachedSfx* c = &ca->sfxCache[i];
            if (c->pcm == NULL || sfxInUse(ca, c)) continue;
            if (victim == NULL || c->lastUse < victim->lastUse) victim = c;
        }
        if (victim == NULL) break;
        freeSfx(ca, victim);
        evictedAny = true;
        if (force) break;
    }
    return evictedAny;
}

// The entry shedSfx gives up next, or NULL: of those no larger than
// maxBytes, not playing and not played lately, the cheapest to play again,
// then the largest, then the least recently played.
static CachedSfx* shedVictim(CtrAudioSystem* ca, uint32_t maxBytes, u64 now) {
    CachedSfx* victim = NULL;
    for (uint32_t i = 0; i < ca->soundCount; i++) {
        CachedSfx* c = &ca->sfxCache[i];
        if (c->pcm == NULL || c->bytes > maxBytes || TICKS_TO_MS(now - c->lastPlayTick) < SFX_SHED_RECENT_MS || sfxInUse(ca, c)) continue;
        if (victim != NULL) {
            if (c->reload != victim->reload) {
                if (c->reload > victim->reload) continue;
            } else if (c->bytes != victim->bytes) {
                if (c->bytes < victim->bytes) continue;
            } else if (c->lastUse > victim->lastUse) {
                continue;
            }
        }
        victim = c;
    }
    return victim;
}

uint32_t CtrAudioSystem_shedSfx(uint32_t bytes) {
    CtrAudioSystem* ca = gShutdownTarget;
    if (ca == NULL) return 0;
    u64 now = svcGetSystemTick();
    uint32_t shed = 0;
    while (bytes > shed && ca->cacheBytes > SFX_SHED_KEEP_BYTES) {
        CachedSfx* victim = shedVictim(ca, ca->cacheBytes - SFX_SHED_KEEP_BYTES, now);
        if (victim == NULL) break;
        shed += victim->bytes;
        freeSfx(ca, victim);
    }
    return shed;
}

#include "ctr_sfxcopy.inc" // sfxcopy: sfxIsDualMono, sfxCopyPcm (late code)
#line 1412 // sfxcopy: main's numbering (the __LINE__ trap)
// Copies a PCM WAV into linear memory in the format NDSP plays. Stereo files
// whose channels are identical are stored as mono. False if the WAV is empty
// or (with *outOfMemory set) no linear block that size is free.
static bool buildSfx(CtrAudioSystem* ca, CachedSfx* c, const CtrDecoder* wav, const uint8_t* blob, bool* outOfMemory) {
    *outOfMemory = false;
    const uint8_t* src = blob + wav->wavDataStart;
    uint32_t bytesPerSample = wav->wavBits / 8u;
    uint32_t frames = (uint32_t) wav->totalFrames;
    if (frames == 0) return false;
    bool dualMono = wav->channels == 2 && sfxIsDualMono(src, frames, bytesPerSample);
    uint32_t outChannels = dualMono ? 1 : wav->channels;
    uint32_t bytes = frames * outChannels * bytesPerSample;

    evictSfx(ca, bytes, false);
    uint8_t* pcm = (uint8_t*) linearAlloc(bytes);
    while (pcm == NULL && evictSfx(ca, bytes, true)) pcm = (uint8_t*) linearAlloc(bytes);
    if (pcm == NULL) {
        *outOfMemory = true;
        return false;
    }

    sfxCopyPcm(pcm, src, frames, bytesPerSample, dualMono, bytes);
#line 1448 // sfxcopy: main's numbering (the __LINE__ trap)
    DSP_FlushDataCache(pcm, bytes);

    c->pcm = pcm;
    c->bytes = bytes;
    c->frames = frames;
    c->sampleRate = wav->sampleRate;
    c->sourceChannels = (uint8_t) wav->channels;
    if (bytesPerSample == 2) c->format = outChannels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16;
    else c->format = outChannels == 2 ? NDSP_FORMAT_STEREO_PCM8 : NDSP_FORMAT_MONO_PCM8;
    c->reload = SFX_RELOAD_AUDO_WAV;
    c->lastPlayTick = svcGetSystemTick();
    ca->cacheBytes += bytes;
    return true;
}

// Whether a stream plays straight from this AUDO blob, or a voice waiting for
// a stream context is about to.
static bool blobStreaming(CtrAudioSystem* ca, const uint8_t* data) {
    bool streaming = false;
    for (int i = 0; i < STREAM_CONTEXTS; i++) {
        StreamContext* s = &ca->streams[i];
        LightLock_Lock(&s->lock);
        if (s->state != CTX_FREE && s->state != CTX_DONE && s->memData == data) streaming = true;
        LightLock_Unlock(&s->lock);
    }
    for (int i = 0; i < VOICE_COUNT; i++) {
        const Voice* v = &ca->voices[i];
        if (v->kind == VOICE_PENDING && v->waitStream && v->pendMem == data) streaming = true;
    }
    return streaming;
}

// Moves the loader's sound decoding along: gives measured sounds a buffer
// and installs finished ones in the cache. Game thread.
static void collectPreps(CtrAudioSystem* ca) {
    for (int i = 0; i < PREP_SLOTS; i++) {
        PrepJob* job = &ca->preps[i];
        LightLock_Lock(&ca->loadLock);
        PrepState state = job->state;
        LightLock_Unlock(&ca->loadLock);
        if (state == PREP_MEASURED) {
            uint32_t bytes = job->frames * job->channels * (uint32_t) sizeof(int16_t);
            evictSfx(ca, bytes, false);
            uint8_t* pcm = (uint8_t*) linearAlloc(bytes);
            while (pcm == NULL && evictSfx(ca, bytes, true)) pcm = (uint8_t*) linearAlloc(bytes);
            job->pcm = pcm;
            LightLock_Lock(&ca->loadLock);
            job->state = PREP_DECODE;
            LightLock_Unlock(&ca->loadLock);
            LightEvent_Signal(&ca->loadEvent);
            continue;
        }
        if (state != PREP_DONE) continue;

        int32_t si = job->soundIndex;
        const Sound* sound = &ca->base.audioGroups[0]->sond.sounds[si];
        if (job->ok) {
            CachedSfx* c = &ca->sfxCache[si];
            if (c->pcm == NULL) {
                c->pcm = job->pcm;
                c->bytes = job->frames * job->channels * (uint32_t) sizeof(int16_t);
                c->frames = job->frames;
                c->sampleRate = job->sampleRate;
                c->format = job->channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16;
                c->sourceChannels = (uint8_t) job->channels;
                c->lastUse = ++ca->useClock;
                c->reload = job->path != NULL ? SFX_RELOAD_FILE : SFX_RELOAD_AUDO_DECODE;
                c->lastPlayTick = svcGetSystemTick();
                ca->cacheBytes += c->bytes;
            } else {
                linearFree(job->pcm);
            }
            job->pcm = NULL;
            ca->soundForms[si] = FORM_CACHED;
            ca->soundLengths[si] = (float) c->frames / (float) c->sampleRate;
            ca->windowDecodes++;
            ca->windowDecodeTicks += job->probeTicks + job->decodeTicks;
            logQuiet("Audio: decoded '%s' (%.2f s, %lu KB) into the cache in the background: probe %.1f ms, decode %.1f ms\n",
                     sound->name, (double) ca->soundLengths[si], (unsigned long) (c->bytes / 1024u), TICKS_TO_MS(job->probeTicks),
                     TICKS_TO_MS(job->decodeTicks));
            // The blob is not needed any more (it is read again after an eviction).
            DataWin* group = NULL;
            AudioEntry* entry = job->mem != NULL ? audoEntryFor(ca, sound, &group) : NULL;
            if (entry != NULL && entry->data == job->mem && !blobStreaming(ca, entry->data)) releaseAudoBlob(group, entry);
        } else {
            // Too long, or the probe could not make sense of it: it plays as a
            // stream, which reports a file it cannot open on every play.
            if (job->pcm != NULL) linearFree(job->pcm);
            job->pcm = NULL;
            ca->soundForms[si] = FORM_STREAMED;
            if (job->stream && job->sampleRate > 0 && job->frames > 0) {
                ca->soundLengths[si] = (float) job->frames / (float) job->sampleRate;
            }
        }
        free(job->path);
        job->path = NULL;
        LightLock_Lock(&ca->loadLock);
        job->state = PREP_FREE;
        LightLock_Unlock(&ca->loadLock);
    }
}

// ===[ Lengths ]===

static float streamEntryLength(CtrAudioSystem* ca, int32_t streamIndex) {
    int32_t slot = streamIndex - AUDIO_STREAM_INDEX_BASE;
    if (slot < 0 || slot >= MAX_STREAM_ENTRIES || !ca->streamEntries[slot].active) return 0.0f;
    StreamEntry* entry = &ca->streamEntries[slot];
    if (entry->lengthSeconds >= 0.0f) return entry->lengthSeconds;
    entry->lengthSeconds = 0.0f;
    CtrAudioInfo info;
    if (CtrDecoder_probeFile(entry->path, &info) && info.sampleRate > 0) {
        entry->lengthSeconds = (float) info.totalFrames / (float) info.sampleRate;
    }
    return entry->lengthSeconds;
}

// Length of a SOND asset in seconds, cached once known. Before a sound has
// played this probes its headers on the game thread (for an external file an
// open and two small reads; no decoder setup).
static float soundLength(CtrAudioSystem* ca, int32_t soundIndex) {
    if (soundIndex >= AUDIO_STREAM_INDEX_BASE) return streamEntryLength(ca, soundIndex);
    if (soundIndex < 0 || (uint32_t) soundIndex >= ca->soundCount) return 0.0f;
    if (ca->soundLengths[soundIndex] >= 0.0f) return ca->soundLengths[soundIndex];
    CachedSfx* c = &ca->sfxCache[soundIndex];
    if (c->pcm != NULL) {
        ca->soundLengths[soundIndex] = (float) c->frames / (float) c->sampleRate;
        return ca->soundLengths[soundIndex];
    }

    const Sound* sound = &ca->base.audioGroups[0]->sond.sounds[soundIndex];
    CtrAudioInfo info;
    bool ok = false;
    if (soundIsInAudo(sound)) {
        DataWin* group = NULL;
        AudioEntry* entry = audoEntryFor(ca, sound, &group);
        if (entry == NULL) return 0.0f;
        char* section = audoSectionPath(ca, sound, group, entry);
        if (section != NULL) {
            ok = CtrDecoder_probeFile(section, &info);
            free(section);
        } else {
            loadAudoBlob(group, (uint32_t) sound->audioFile);
            if (entry->data != NULL) ok = CtrDecoder_probeMemory(entry->data, entry->dataSize, &info);
            // Small WAVs are cached from a fresh read when they play; compressed or
            // huge sounds play from the blob, so keep those.
            bool smallWav = ok && info.kind == CTR_DECODER_WAV && info.totalFrames * info.channels * 2u <= SFX_CACHE_MAX_ENTRY_BYTES;
            if (smallWav && !prepInFlight(ca, soundIndex)) releaseAudoBlob(group, entry);
        }
    } else {
        char* path = resolveExternalPath(ca, sound);
        if (path != NULL && !pathIsMp3(path)) ok = CtrDecoder_probeFile(path, &info);
        free(path);
    }
    float seconds = ok && info.sampleRate > 0 ? (float) info.totalFrames / (float) info.sampleRate : 0.0f;
    ca->soundLengths[soundIndex] = seconds;
    return seconds;
}

// ===[ Voices ]===

static void streamApply(CtrAudioSystem* ca, Voice* v, const float mix[12], bool paused);
static void startPendingVoices(CtrAudioSystem* ca);

static float voiceGain(CtrAudioSystem* ca, const Voice* v) {
    return v->gain * AudioSystem_soundGroupGain(&ca->base, v->soundIndex) * spatialGain(&ca->base, v);
}

// Left and right gains of a mono sound at pan (-1 left .. 1 right): constant power, sqrt(2) cos and sin of
// (pan + 1) pi/4, so the centre keeps the gain unpanned sounds get and a sound part-way across keeps its loudness.
// Each side is capped at the gain itself (NDSP's mix doesn't clip-protect), so a hard-panned sound is 3 dB quieter on
// its side than OpenAL, whose centre is 3 dB down instead.
static void panGains(float pan, float gain, float out[2]) {
    if (pan == 0.0f) {
        out[0] = out[1] = gain;
        return;
    }
    float phi = (pan + 1.0f) * 0.78539816f;
    float l = 1.41421356f * cosf(phi), r = 1.41421356f * sinf(phi);
    out[0] = gain * (l < 1.0f ? (l > 0.0f ? l : 0.0f) : 1.0f);
    out[1] = gain * (r < 1.0f ? (r > 0.0f ? r : 0.0f) : 1.0f);
}

// Sets a positioned voice's target gains and puts in mix the ones to apply now. With rampLock held.
static void rampTarget(CtrAudioSystem* ca, int vi, float mix[12]) {
    Voice* v = &ca->voices[vi];
    // Like OpenAL, only mono sources are positioned; stereo ones keep their channels. A stream's channel count is
    // known once it opens; until then it plays centred, and moves to its pan through the ramp.
    uint32_t channels = 0;
    if (v->kind == VOICE_SFX) {
        channels = v->sfx->sourceChannels;
    } else {
        StreamContext* s = &ca->streams[v->ctx];
        LightLock_Lock(&s->lock);
        channels = s->opened ? s->channels : 0;
        LightLock_Unlock(&s->lock);
    }
    float target[2] = { mix[0], mix[1] };
    if (channels == 1) panGains(AudioSystem_panPosition(&ca->base, v->sx, v->sy, v->sz), mix[0], target);
    MixRamp* r = &ca->ramps[vi];
    bool stream = v->kind == VOICE_STREAM;
    if (!r->active || r->stream != stream || (stream && r->ctx != v->ctx)) {
        // Just positioned (the sound has only just started): no ramp from where it was.
        r->active = true;
        r->stream = stream;
        r->ctx = stream ? v->ctx : -1;
        r->cur[0] = target[0];
        r->cur[1] = target[1];
        if (!ca->rampsActive) ca->rampTick = svcGetSystemTick();
        ca->rampsActive = true;
    }
    r->tgt[0] = target[0];
    r->tgt[1] = target[1];
    mix[0] = r->cur[0];
    mix[1] = r->cur[1];
}

// The voice no longer plays what its ramp was for. Game thread.
static void rampRelease(CtrAudioSystem* ca, int vi) {
    if (!ca->ramps[vi].active) return;
    LightLock_Lock(&ca->rampLock);
    ca->ramps[vi].active = false;
    LightLock_Unlock(&ca->rampLock);
}

// Moves the ramps on. Decode thread, after each wake.
static void rampStep(CtrAudioSystem* ca) {
    if (!ca->rampsActive) return;
    LightLock_Lock(&ca->rampLock);
    u64 now = svcGetSystemTick();
    float seconds = (float) (now - ca->rampTick) / (float) SYSCLOCK_ARM11;
    ca->rampTick = now;
    float stepMax = (seconds < MIX_RAMP_SECONDS ? seconds : MIX_RAMP_SECONDS) / MIX_RAMP_SECONDS;
    bool any = false;
    for (int ch = 0; ch < VOICE_COUNT; ch++) {
        MixRamp* r = &ca->ramps[ch];
        if (!r->active) continue;
        any = true;
        if (r->cur[0] == r->tgt[0] && r->cur[1] == r->tgt[1]) continue;
        for (int i = 0; i < 2; i++) {
            float d = r->tgt[i] - r->cur[i];
            if (d > stepMax) d = stepMax;
            if (d < -stepMax) d = -stepMax;
            r->cur[i] = fabsf(r->tgt[i] - r->cur[i]) <= stepMax ? r->tgt[i] : r->cur[i] + d;
        }
        if (!r->stream) {
            float mix[12] = {0};
            mix[0] = r->cur[0];
            mix[1] = r->cur[1];
            ndspChnSetMix(ch, mix);
        } else {
            StreamContext* s = &ca->streams[r->ctx];
            LightLock_Lock(&s->lock);
            if (s->channel == ch && !s->stopRequested) {
                s->mix[0] = r->cur[0];
                s->mix[1] = r->cur[1];
                if (s->opened) ndspChnSetMix(ch, s->mix);
            }
            LightLock_Unlock(&s->lock);
        }
    }
    ca->rampsActive = any;
    LightLock_Unlock(&ca->rampLock);
}

static void applyVoice(CtrAudioSystem* ca, int vi) {
    Voice* v = &ca->voices[vi];
    if (v->kind != VOICE_SFX && v->kind != VOICE_STREAM) return;
    float mix[12] = {0};
    float gain = voiceGain(ca, v);
    if (gain < 0) gain = 0;
    mix[0] = gain;
    mix[1] = gain;
    bool paused = v->paused || ca->suspended;
    if (v->spatial) {
        LightLock_Lock(&ca->rampLock);
        rampTarget(ca, vi, mix);
    }
    if (v->kind == VOICE_SFX) {
        ndspChnSetMix(vi, mix);
        ndspChnSetRate(vi, (float) v->sfx->sampleRate * clampPitch(v->pitch));
        ndspChnSetPaused(vi, paused);
    } else {
        streamApply(ca, v, mix, paused);
    }
    if (v->spatial) LightLock_Unlock(&ca->rampLock);
}

static void queueSfxBuffers(Voice* v, int ch, uint32_t startFrame) {
    CachedSfx* c = v->sfx;
    uint32_t frameBytes = c->bytes / c->frames;
    if (startFrame >= c->frames) startFrame = v->loop ? startFrame % c->frames : c->frames;
    memset(v->wb, 0, sizeof(v->wb));
    v->wbCount = 0;
    if (startFrame > 0 && startFrame < c->frames) {
        ndspWaveBuf* wb = &v->wb[v->wbCount];
        wb->data_vaddr = c->pcm + startFrame * frameBytes;
        wb->nsamples = c->frames - startFrame;
        wb->looping = false;
        v->wbStart[v->wbCount++] = startFrame;
    }
    if (startFrame == 0 || v->loop) {
        ndspWaveBuf* wb = &v->wb[v->wbCount];
        wb->data_vaddr = c->pcm;
        wb->nsamples = c->frames;
        wb->looping = v->loop;
        v->wbStart[v->wbCount++] = 0;
    }
    for (int i = 0; i < v->wbCount; i++) ndspChnWaveBufAdd(ch, &v->wb[i]);
}

static void stopStreamVoice(CtrAudioSystem* ca, Voice* v) {
    StreamContext* s = &ca->streams[v->ctx];
    bool stopped = false;
    LightLock_Lock(&s->lock);
    if (s->state != CTX_DONE && !s->stopRequested) {
        s->stopRequested = true;
        if (s->opened) ndspChnWaveBufClear(s->channel);
        s->opened = false;
        stopped = true;
    }
    LightLock_Unlock(&s->lock);
    if (stopped && !ca->shutDown) logQuiet("Audio: stream '%s' stopped\n", s->name);
    s->owner = -1;
    v->ctx = -1;
    LightEvent_Signal(&ca->event);
    LightEvent_Signal(&ca->openEvent);
}

static void stopVoice(CtrAudioSystem* ca, int vi) {
    Voice* v = &ca->voices[vi];
    rampRelease(ca, vi);
    if (v->kind == VOICE_SFX) ndspChnWaveBufClear(vi);
    else if (v->kind == VOICE_STREAM) stopStreamVoice(ca, v);
    free(v->pendPath);
    v->pendPath = NULL;
    v->waitStream = false;
    v->kind = VOICE_FREE;
    v->sfx = NULL;
}

static bool voiceFinished(CtrAudioSystem* ca, const Voice* v) {
    switch (v->kind) {
        case VOICE_SFX:
            return !v->loop && (v->wbCount == 0 || v->wb[v->wbCount - 1].status == NDSP_WBUF_DONE);
        case VOICE_STREAM:
            return ca->streams[v->ctx].state == CTX_DONE;
        case VOICE_VIRTUAL:
            return !v->loop && v->virtualSeconds >= v->virtualLength;
        case VOICE_PENDING:
            return false;
        default:
            return true;
    }
}

// True if the voice is playing (or paused). Finished voices are retired here,
// so every query sees up-to-date state without waiting for update().
static bool voiceLive(CtrAudioSystem* ca, int vi) {
    Voice* v = &ca->voices[vi];
    if (v->kind == VOICE_FREE) return false;
    if (!voiceFinished(ca, v)) return true;
    if (v->kind == VOICE_STREAM) {
        ca->streams[v->ctx].owner = -1;
        v->ctx = -1;
    }
    v->kind = VOICE_FREE;
    v->sfx = NULL;
    return false;
}

static bool voiceMatches(CtrAudioSystem* ca, int vi, int32_t soundOrInstance) {
    if (!voiceLive(ca, vi)) return false;
    const Voice* v = &ca->voices[vi];
    return isInstanceId(soundOrInstance) ? v->instanceId == soundOrInstance : v->soundIndex == soundOrInstance;
}

static int32_t nextInstanceId(CtrAudioSystem* ca) {
    for (;;) {
        int32_t id = SOUND_INSTANCE_ID_BASE + (int32_t) (ca->instanceCounter++ % INSTANCE_ID_SPAN);
        bool used = false;
        for (int i = 0; i < VOICE_COUNT; i++) {
            if (ca->voices[i].kind != VOICE_FREE && ca->voices[i].instanceId == id) used = true;
        }
        if (!used) return id;
    }
}

// Finds a voice for a new sound. When all voices (or audio_channel_num) are in
// use, the lowest-priority voice is stopped, as long as its priority is not
// above the new sound's; ties go to SFX first, then the oldest.
static int claimVoice(CtrAudioSystem* ca, int32_t priority) {
    int active = 0;
    int freeIndex = -1;
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (voiceLive(ca, i)) active++;
        else if (freeIndex < 0) freeIndex = i;
    }
    if (freeIndex >= 0 && active < ca->voiceLimit) return freeIndex;

    int victim = -1;
    for (int i = 0; i < VOICE_COUNT; i++) {
        Voice* v = &ca->voices[i];
        if (v->kind == VOICE_FREE || v->priority > priority) continue;
        if (victim < 0) {
            victim = i;
            continue;
        }
        Voice* best = &ca->voices[victim];
        if (v->priority != best->priority) {
            if (v->priority < best->priority) victim = i;
        } else if ((v->kind == VOICE_STREAM) != (best->kind == VOICE_STREAM)) {
            if (v->kind != VOICE_STREAM) victim = i;
        } else if (v->serial < best->serial) {
            victim = i;
        }
    }
    if (victim < 0) return -1;
    stopVoice(ca, victim);
    return victim;
}

// ===[ Streams (game thread side) ]===

static void streamApply(CtrAudioSystem* ca, Voice* v, const float mix[12], bool paused) {
    StreamContext* s = &ca->streams[v->ctx];
    LightLock_Lock(&s->lock);
    memcpy(s->mix, mix, sizeof(s->mix));
    s->pitch = v->pitch;
    s->paused = paused;
    if (s->opened && !s->stopRequested) {
        ndspChnSetMix(s->channel, s->mix);
        ndspChnSetRate(s->channel, (float) s->sampleRate * clampPitch(s->pitch));
        ndspChnSetPaused(s->channel, paused);
    }
    LightLock_Unlock(&s->lock);
}

static int findFreeStreamContext(CtrAudioSystem* ca) {
    for (int i = 0; i < STREAM_CONTEXTS; i++) {
        StreamContext* s = &ca->streams[i];
        if (s->state == CTX_FREE || (s->state == CTX_DONE && s->owner < 0)) return i;
    }
    return -1;
}

#define CLAIM_WAIT (-2)

// A free stream context, -1 if none can be had, or CLAIM_WAIT when one is on
// its way out: the caller retries on a later frame rather than blocking the
// game thread while the decode thread finishes whatever it is doing.
static int claimStreamContext(CtrAudioSystem* ca, int32_t priority, int forVoice) {
    int ctx = findFreeStreamContext(ca);
    if (ctx >= 0) return ctx;

    // All contexts busy. If none is already on its way out (stopped, waiting
    // for the decode thread to let go), stop the least important stream.
    bool releasing = false;
    for (int i = 0; i < STREAM_CONTEXTS; i++) {
        if (ca->streams[i].owner < 0) releasing = true;
    }
    if (!releasing) {
        int victim = -1;
        for (int i = 0; i < VOICE_COUNT; i++) {
            Voice* v = &ca->voices[i];
            if (i == forVoice || v->kind != VOICE_STREAM || v->priority > priority) continue;
            if (victim < 0 || v->priority < ca->voices[victim].priority ||
                (v->priority == ca->voices[victim].priority && v->serial < ca->voices[victim].serial)) victim = i;
        }
        if (victim < 0) return -1;
        stopVoice(ca, victim);
    }
    LightEvent_Signal(&ca->event);
    return CLAIM_WAIT;
}

static void startStream(CtrAudioSystem* ca, int vi, int ctx, char* path, const uint8_t* mem, uint32_t memSize, const char* name) {
    Voice* v = &ca->voices[vi];
    StreamContext* s = &ca->streams[ctx];
    float gain = voiceGain(ca, v);
    LightLock_Lock(&s->lock);
    s->owner = vi;
    s->channel = vi;
    s->path = path;
    s->memData = mem;
    s->memSize = memSize;
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->loop = v->loop;
    memset(s->mix, 0, sizeof(s->mix));
    s->mix[0] = gain;
    s->mix[1] = gain;
    s->pitch = v->pitch;
    s->paused = v->paused || ca->suspended;
    s->stopRequested = false;
    s->seekPending = false;
    s->seekSeconds = 0.0;
    s->opened = false;
    s->failed = false;
    s->sampleRate = 0;
    s->channels = 0;
    s->totalFrames = 0;
    // A position set while the voice was pending applies once it opens.
    if (v->startPosition > 0.0f) {
        s->seekPending = true;
        s->seekSeconds = v->startPosition;
    }
    v->lastPosition = v->startPosition;
    s->state = CTX_OPEN_REQUESTED;
    LightLock_Unlock(&s->lock);
    v->ctx = ctx;
    LightEvent_Signal(ca->opener != NULL ? &ca->openEvent : &ca->event);
}

static float streamPosition(CtrAudioSystem* ca, Voice* v) {
    StreamContext* s = &ca->streams[v->ctx];
    float position = v->lastPosition;
    LightLock_Lock(&s->lock);
    if (s->seekPending) {
        position = (float) s->seekSeconds;
    } else if (s->opened && s->sampleRate > 0) {
        u16 seq = ndspChnGetWaveBufSeq(s->channel);
        u32 samplePos = ndspChnGetSamplePos(s->channel);
        for (int i = 0; i < STREAM_BUFFERS; i++) {
            const StreamBuffer* b = &s->bufs[i];
            if ((b->wb.status == NDSP_WBUF_PLAYING || b->wb.status == NDSP_WBUF_QUEUED) && b->wb.sequence_id == seq) {
                uint64_t frame = b->startFrame + samplePos;
                if (s->totalFrames > 0) frame %= s->totalFrames;
                position = (float) frame / (float) s->sampleRate;
                break;
            }
        }
    }
    LightLock_Unlock(&s->lock);
    v->lastPosition = position;
    return position;
}

// ===[ Vtable: lifecycle ]===

static void shutdownAudio(CtrAudioSystem* ca) {
    if (ca->shutDown) return;
    ca->shutDown = true;
    if (!ca->ndspReady) return;

    ndspSetCallback(NULL, NULL);
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (ca->voices[i].kind != VOICE_FREE) stopVoice(ca, i);
    }
    stopLoaderThread(ca);
    // The opener first: it hands streams to the decode thread. An open in
    // progress finishes first; shutdown closes whatever it opened.
    if (ca->opener != NULL) {
        ca->openerQuit = true;
        LightEvent_Signal(&ca->openEvent);
        threadJoin(ca->opener, U64_MAX);
        threadFree(ca->opener);
        ca->opener = NULL;
    }
    ca->quit = true;
    LightEvent_Signal(&ca->event);
    threadJoin(ca->thread, U64_MAX);
    threadFree(ca->thread);
    ca->thread = NULL;

    for (int i = 0; i < STREAM_CONTEXTS; i++) {
        StreamContext* s = &ca->streams[i];
        CtrDecoder_close(&s->decoder);
        free(s->path);
        s->path = NULL;
        s->state = CTX_FREE;
    }
    CtrReadAhead_shutdown();
    for (int ch = 0; ch < VOICE_COUNT; ch++) ndspChnReset(ch);
    ndspExit();
    ca->ndspReady = false;

    for (int i = 0; i < STREAM_CONTEXTS; i++) {
        for (int b = 0; b < STREAM_BUFFERS; b++) {
            linearFree(ca->streams[i].bufs[b].pcm);
            ca->streams[i].bufs[b].pcm = NULL;
        }
    }
    for (uint32_t i = 0; i < ca->soundCount; i++) {
        if (ca->sfxCache[i].pcm != NULL) freeSfx(ca, &ca->sfxCache[i]);
    }
}

// main.c returns without destroying the runner, so NDSP is also shut down at
// exit; leaving it running breaks audio in the next app on real hardware.
static void shutdownAtExit(void) {
    if (gShutdownTarget != NULL) shutdownAudio(gShutdownTarget);
}

static bool allocStreamBuffers(CtrAudioSystem* ca) {
    for (int i = 0; i < STREAM_CONTEXTS; i++) {
        for (int b = 0; b < STREAM_BUFFERS; b++) {
            int16_t* pcm = (int16_t*) linearAlloc(STREAM_BUFFER_FRAMES * 2 * sizeof(int16_t));
            if (pcm == NULL) return false;
            ca->streams[i].bufs[b].pcm = pcm;
        }
    }
    return true;
}

static void freeStreamBuffers(CtrAudioSystem* ca) {
    for (int i = 0; i < STREAM_CONTEXTS; i++) {
        for (int b = 0; b < STREAM_BUFFERS; b++) {
            linearFree(ca->streams[i].bufs[b].pcm);
            ca->streams[i].bufs[b].pcm = NULL;
        }
    }
}

// Raises the app's system core time limit to at least `percent`; never lowers
// it. Returns false if it is lower and cannot be raised.
static bool syscoreTimeLimitAtLeast(u32 percent) {
    u32 current = 0;
    if (R_FAILED(APT_GetAppCpuTimeLimit(&current))) return false;
    if (current >= percent) return true;
    return R_SUCCEEDED(APT_SetAppCpuTimeLimit(percent));
}

static bool startDecodeThread(CtrAudioSystem* ca) {
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    // Just above the game thread, so decoding preempts it when they share a core.
    priority -= 1;
    if (priority < 0x18) priority = 0x18;
    if (priority > 0x3F) priority = 0x3F;
    ca->threadPriority = priority;

    bool isNew3ds = false;
    APT_CheckNew3DS(&isNew3ds);
    ca->thread = NULL;
    // A New 3DS in Old 3DS mode (a .3dsx started from an Old 3DS title's
    // context) gives the app no third core though the check above still says
    // New 3DS: this fails there and it goes on as an Old 3DS. The .cias may
    // use core 2 on a New 3DS (CanAccessCore2, cia/*.rsf).
    if (isNew3ds) {
        ca->thread = threadCreate(decodeThreadMain, ca, DECODE_STACK_SIZE, priority, 2, false);
        ca->threadCore = 2;
    }
    // Otherwise the system core (core 1). On the game's core, opening a track
    // (~50 ms of header and codebook setup) and decoding (7-9% of the core
    // per stream, in bursts) preempt the game thread, and a room's new music showed up as a
    // 75-100 ms hitch on entering it. The app may use the system core only up
    // to its time limit, so make sure the limit leaves room for decoding.
    // Stream buffers hold ~370 ms, which covers the system core's slower and
    // capped scheduling. The texture prepare screen borrows more of the
    // system core and gives back whatever limit it found, but it runs and
    // ends before audio starts.
    if (ca->thread == NULL && syscoreTimeLimitAtLeast(SYSCORE_PERCENT)) {
        ca->thread = threadCreate(decodeThreadMain, ca, DECODE_STACK_SIZE, priority, 1, false);
        ca->threadCore = 1;
        u32 limit = 0;
        APT_GetAppCpuTimeLimit(&limit);
        logInfo("Audio: system core time limit %lu%% (wanted %u%%)\n", (unsigned long) limit, SYSCORE_PERCENT);
    }
    if (ca->thread == NULL) {
        ca->thread = threadCreate(decodeThreadMain, ca, DECODE_STACK_SIZE, priority, -2, false);
        ca->threadCore = 0;
    }
    if (ca->thread == NULL) return false;
    // The opener on the same core, one step below: the decode thread's
    // refills always preempt an open, and an open takes what they leave
    // (and the time it waits on the card). A seek at the open decodes, so
    // it gets the decode thread's stack. Either way the open itself is the
    // same calls, only on another thread.
    s32 openerPriority = priority < 0x3F ? priority + 1 : 0x3F;
    ca->opener = threadCreate(openerThreadMain, ca, DECODE_STACK_SIZE, openerPriority, ca->threadCore == 0 ? -2 : ca->threadCore, false);
    if (ca->opener == NULL) logWarn("Audio: no stream opener thread, the decode thread opens streams itself\n");
    return true;
}

static void ctrInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    ca->base.dw = dataWin;
    arrput(ca->base.audioGroups, dataWin);
    ca->fileSystem = fileSystem;

    ca->soundCount = dataWin->sond.count;
    uint32_t allocCount = ca->soundCount > 0 ? ca->soundCount : 1;
    ca->sfxCache = (CachedSfx*) safeCalloc(allocCount, sizeof(CachedSfx));
    ca->soundLengths = (float*) safeMalloc(allocCount * sizeof(float));
    for (uint32_t i = 0; i < allocCount; i++) ca->soundLengths[i] = -1.0f;
    ca->warned = (bool*) safeCalloc(allocCount, sizeof(bool));
    ca->soundForms = (uint8_t*) safeCalloc(allocCount, sizeof(uint8_t));
    ca->noBackgroundRead = (bool*) safeCalloc(allocCount, sizeof(bool));
    ca->initialized = true;
    ca->windowStart = svcGetSystemTick();

    Result rc = ndspInit();
    if (R_FAILED(rc)) {
        logWarn("Audio: NDSP failed to start (0x%08lX), running silently. Sound needs sdmc:/3ds/dspfirm.cdc (dump it with DSP1).\n",
                (unsigned long) rc);
        CtrNotice_missing("Audio unavailable; check DSP firmware", "sdmc:/3ds/dspfirm.cdc");
        return;
    }
    if (!allocStreamBuffers(ca)) {
        logWarn("Audio: out of linear memory for stream buffers, running silently\n");
        freeStreamBuffers(ca);
        ndspExit();
        return;
    }
    if (!startDecodeThread(ca)) {
        logWarn("Audio: could not start the decode thread, running silently\n");
        freeStreamBuffers(ca);
        ndspExit();
        return;
    }

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspSetClippingMode(NDSP_CLIP_SOFT);
    ndspSetMasterVol(1.0f);
    ndspSetCallback(ndspFrameCallback, ca);
    ca->ndspReady = true;

    arrput(ca->groupPaths, ca->dataWinPath != NULL ? safeStrdup(ca->dataWinPath) : NULL);
    if (!startLoaderThread(ca)) logWarn("Audio: could not start the loader thread, loading sounds directly\n");
    // Read-ahead next to the decode thread and just above it: it only waits
    // on the card, and should ask for the next chunk as soon as one arrives.
    s32 readAheadPriority = ca->threadPriority > 0x18 ? ca->threadPriority - 1 : 0x18;
    if (CtrReadAhead_init(ca->threadCore == 0 ? -2 : ca->threadCore, readAheadPriority)) {
        logInfo("Audio: stream read-ahead on, %d rings of %lu KB in linear memory\n", CtrReadAhead_slotCount(),
                (unsigned long) (CtrReadAhead_slotBytes() / 1024u));
    } else {
        logWarn("Audio: no stream read-ahead (out of linear memory or threads), streams read the SD card directly\n");
    }

    gShutdownTarget = ca;
    if (!gAtexitRegistered) {
        atexit(shutdownAtExit);
        gAtexitRegistered = true;
    }
    logInfo("Audio: NDSP ready (%s decoder, decode thread on core %d prio 0x%lX, %s, %d voices, %lu KB SFX cache)\n",
            CtrDecoder_vorbisBackendName(), ca->threadCore, (unsigned long) ca->threadPriority,
            ca->opener != NULL ? "opener thread below it" : "no opener thread", VOICE_COUNT, (unsigned long) (CTR_SFX_CACHE_BYTES / 1024u));
}

static void ctrDestroy(AudioSystem* audio) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    shutdownAudio(ca);
    drainMessages(ca); // includes any missing file reported during shutdown
    if (gShutdownTarget == ca) gShutdownTarget = NULL;

    for (int i = 0; i < MAX_STREAM_ENTRIES; i++) free(ca->streamEntries[i].path);
    for (int32_t i = 0; i < (int32_t) arrlen(ca->groupPaths); i++) free(ca->groupPaths[i]);
    arrfree(ca->groupPaths);
    free(ca->dataWinPath);
    free(ca->sfxCache);
    free(ca->soundLengths);
    free(ca->warned);
    free(ca->soundForms);
    free(ca->noBackgroundRead);
    free(audio->groupGains);
    for (int32_t i = 1; i < (int32_t) arrlen(audio->audioGroups); i++) DataWin_free(audio->audioGroups[i]);
    arrfree(audio->audioGroups);
    free(ca);
}

// ===[ Vtable: update ]===

#ifdef CTRPORT_AUDIO_METRICS
#include "ctr_adpcm_metrics.h"
AM_CODE static void amStatsLock(LightLock* lock);
AM_CODE static void amStatsUnlock(LightLock* lock);
#define LightLock_Lock amStatsLock
#define LightLock_Unlock amStatsUnlock
#endif
#line 2210
static void logWindowStats(CtrAudioSystem* ca) {
    u64 now = svcGetSystemTick();
    double wallMs = TICKS_TO_MS(now - ca->windowStart);
    LightLock_Lock(&ca->statsLock);
    u64 busy = ca->threadBusyTicks, decode = ca->threadDecodeTicks;
    double decoded = ca->threadDecodedSeconds;
    uint32_t underruns = ca->threadUnderruns;
    float leastQueued = ca->leastQueuedMs;
    uint32_t opens = ca->opens;
    u64 longestOpen = ca->longestOpenTicks;
    ca->threadBusyTicks = ca->threadDecodeTicks = 0;
    ca->threadDecodedSeconds = 0.0;
    ca->threadUnderruns = 0;
    ca->leastQueuedMs = -1.0f;
    ca->opens = 0;
    ca->longestOpenTicks = 0;
    LightLock_Unlock(&ca->statsLock);

    int voices = 0, streams = 0;
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (ca->voices[i].kind == VOICE_FREE) continue;
        voices++;
        if (ca->voices[i].kind == VOICE_STREAM) streams++;
    }
    double frames = ca->windowFrames > 0 ? (double) ca->windowFrames : 1.0;
    char leastText[24] = "-";
    if (leastQueued >= 0.0f) snprintf(leastText, sizeof(leastText), "%.0f ms", (double) leastQueued);
    if (ca->ndspReady) {
        CtrReadAheadStats ra;
        CtrReadAhead_takeStats(&ra);
        if (ra.bytesRead > 0 || ra.waits > 0) {
            char lowest[32] = "-";
            if (ra.lowestAheadBytes != UINT32_MAX) snprintf(lowest, sizeof(lowest), "%lu KB", (unsigned long) (ra.lowestAheadBytes / 1024u));
            logQuiet("[audio] read-ahead: %lu KB read, slowest read %.1f ms; %lu waits for the card (%.1f ms, longest %.1f ms); "
                     "least ahead %s\n",
                     (unsigned long) (ra.bytesRead / 1024u), TICKS_TO_MS(ra.longestReadTicks), (unsigned long) ra.waits,
                     TICKS_TO_MS(ra.waitTicks), TICKS_TO_MS(ra.longestWaitTicks), lowest);
        }
        logQuiet("[audio] game %.3f ms/frame avg %.3f max; plays %lu (cache hits %lu, loads %lu taking %.1f ms, background reads %lu, "
                 "background decodes %lu taking %.1f ms, streams %lu); voices %d (streams %d); cache %.2f MB; decode thread core %d: "
                 "busy %.2f%%, %.1f ms per audio second, %lu underruns; DSP dropped %lu; least queued %s; opens %lu (longest %.1f ms)\n",
                 TICKS_TO_MS(ca->windowTicks) / frames, TICKS_TO_MS(ca->windowMaxFrameTicks),
                 (unsigned long) ca->windowPlays, (unsigned long) ca->windowCacheHits, (unsigned long) ca->windowCacheLoads,
                 TICKS_TO_MS(ca->windowLoadTicks), (unsigned long) ca->windowBackgroundLoads, (unsigned long) ca->windowDecodes,
                 TICKS_TO_MS(ca->windowDecodeTicks), (unsigned long) ca->windowStreamStarts, voices, streams,
                 (double) ca->cacheBytes / (1024.0 * 1024.0), ca->threadCore,
                 wallMs > 0 ? 100.0 * TICKS_TO_MS(busy) / wallMs : 0.0, decoded > 0 ? TICKS_TO_MS(decode) / decoded : 0.0,
                 (unsigned long) underruns, (unsigned long) ndspGetDroppedFrames(), leastText, (unsigned long) opens,
                 TICKS_TO_MS(longestOpen));
    } else {
        logQuiet("[audio] silent: game %.3f ms/frame avg %.3f max; plays %lu; voices %d\n", TICKS_TO_MS(ca->windowTicks) / frames,
                 TICKS_TO_MS(ca->windowMaxFrameTicks), (unsigned long) ca->windowPlays, voices);
    }
    ca->windowStart = now;
    ca->windowTicks = ca->windowMaxFrameTicks = ca->windowLoadTicks = 0;
    ca->windowFrames = 0;
    ca->windowSeconds = 0.0f;
    ca->windowPlays = ca->windowCacheHits = ca->windowCacheLoads = ca->windowStreamStarts = ca->windowBackgroundLoads = 0;
    ca->windowDecodes = 0;
    ca->windowDecodeTicks = 0;
}

#ifdef CTRPORT_AUDIO_METRICS
#undef LightLock_Lock
#undef LightLock_Unlock
#endif
#line 2272
static void ctrUpdate(AudioSystem* audio, float deltaTime) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    u64 t0 = svcGetSystemTick();
    if (!ca->initialized) return;

    bool groupChanged = AudioSystem_updateGroupGains(audio, deltaTime);
    drainMessages(ca);
    if (ca->ndspReady) startPendingVoices(ca);

    for (int i = 0; i < VOICE_COUNT; i++) {
        if (!voiceLive(ca, i)) continue;
        Voice* v = &ca->voices[i];
        bool changed = groupChanged || v->spatial;
        if (v->fadeRemaining > 0.0f) {
            v->fadeRemaining -= deltaTime;
            if (v->fadeRemaining <= 0.0f) {
                v->fadeRemaining = 0.0f;
                v->gain = v->fadeTarget;
            } else {
                v->gain = v->fadeStart + (v->fadeTarget - v->fadeStart) * (1.0f - v->fadeRemaining / v->fadeTotal);
            }
            changed = true;
        }
        if (!v->paused && !ca->suspended) v->clockSeconds += (double) deltaTime * clampPitch(v->pitch);
        if (v->kind == VOICE_VIRTUAL) {
            if (!v->paused && !ca->suspended) v->virtualSeconds += (double) deltaTime * clampPitch(v->pitch);
        } else if (changed) {
            applyVoice(ca, i);
        }
        // Remember stream lengths once the decode thread knows them.
        if (v->kind == VOICE_STREAM && v->soundIndex >= 0 && (uint32_t) v->soundIndex < ca->soundCount &&
            ca->soundLengths[v->soundIndex] < 0.0f) {
            StreamContext* s = &ca->streams[v->ctx];
            LightLock_Lock(&s->lock);
            if (s->opened && s->sampleRate > 0) ca->soundLengths[v->soundIndex] = (float) s->totalFrames / (float) s->sampleRate;
            LightLock_Unlock(&s->lock);
        }
    }

    ca->frameTicks += svcGetSystemTick() - t0;
    ca->windowTicks += ca->frameTicks;
    if (ca->frameTicks > ca->windowMaxFrameTicks) ca->windowMaxFrameTicks = ca->frameTicks;
    ca->frameTicks = 0;
    ca->windowFrames++;
    ca->windowSeconds += deltaTime;
    if (ca->windowSeconds >= STATS_INTERVAL_SECONDS) logWindowStats(ca);
#ifdef CTR_AUDIO_SELFTEST
    CtrAudioSelfTest_step(audio, deltaTime);
#endif
}

// ===[ Vtable: playback ]===

// Gets an embedded sound ready to play: from the SFX cache, decoded into it
// (reading data.win here if the blob isn't loaded yet), or as an AUDO blob to
// stream. Returns false if the sound can't play. Only WAVs are opened here;
// that is just a header parse, where opening Vorbis sets up its codebooks.
static bool prepareAudoSource(CtrAudioSystem* ca, int32_t soundIndex, CachedSfx** outSfx, const uint8_t** outMem, uint32_t* outMemSize) {
    const Sound* sound = &ca->base.audioGroups[0]->sond.sounds[soundIndex];
    const char* name = sound->name;
    CachedSfx* c = &ca->sfxCache[soundIndex];
    if (c->pcm != NULL) {
        *outSfx = c;
        ca->windowCacheHits++;
        return true;
    }
    DataWin* group = NULL;
    AudioEntry* entry = audoEntryFor(ca, sound, &group);
    if (entry == NULL) return false;
    u64 loadStart = svcGetSystemTick();
    loadAudoBlob(group, (uint32_t) sound->audioFile);
    u64 readTicks = svcGetSystemTick() - loadStart;
    if (entry->data == NULL) {
        // Out of memory or unreadable: this play is skipped, and a later one tries again.
        if (!ca->warned[soundIndex]) logWarn("Audio: couldn't load the %lu bytes of sound '%s'; skipping it\n", (unsigned long) entry->dataSize, name);
        ca->warned[soundIndex] = true;
        return false;
    }
    CtrDecoderKind kind = CtrDecoder_sniff(entry->data, entry->dataSize);
    CtrDecoder dec;
    if (kind == CTR_DECODER_NONE || (kind == CTR_DECODER_WAV && !CtrDecoder_openMemory(&dec, entry->data, entry->dataSize))) {
        if (!ca->warned[soundIndex]) logWarn("Audio: sound '%s' is not PCM WAV or Ogg Vorbis\n", name);
        ca->warned[soundIndex] = true;
        return false;
    }
    bool cacheable = kind == CTR_DECODER_WAV && dec.wavDataBytes <= SFX_CACHE_MAX_ENTRY_BYTES;
    bool outOfMemory = false;
    bool built = cacheable && buildSfx(ca, c, &dec, entry->data, &outOfMemory);
    if (cacheable && !built && !outOfMemory) {
        CtrDecoder_close(&dec);
        return false;
    }
    if (built) {
        CtrDecoder_close(&dec);
        // An earlier play may still stream from the blob (below).
        if (!blobStreaming(ca, entry->data)) releaseAudoBlob(group, entry);
        *outSfx = c;
        ca->windowCacheLoads++;
        ca->soundLengths[soundIndex] = (float) c->frames / (float) c->sampleRate;
    } else if (outOfMemory) {
        // Textures can leave linear memory too full (or too broken up) for
        // the copy. Then the WAV streams from the blob, which stays loaded,
        // through the stream buffers, which were allocated at start: the same
        // samples, and no linear memory is taken from anything else. A later
        // play tries the copy again.
        uint32_t wavBytes = dec.wavDataBytes;
        CtrDecoder_close(&dec);
        logInfo("Audio: no linear memory for a copy of '%s' (%lu bytes), streaming it from memory\n", name, (unsigned long) wavBytes);
        *outMem = entry->data;
        *outMemSize = entry->dataSize;
    } else {
        // Compressed (or huge) sounds stream from the AUDO blob, which stays loaded.
        if (kind == CTR_DECODER_WAV) CtrDecoder_close(&dec);
        *outMem = entry->data;
        *outMemSize = entry->dataSize;
    }
    u64 loadTicks = svcGetSystemTick() - loadStart;
    ca->windowLoadTicks += loadTicks;
    if (TICKS_TO_MS(loadTicks) > SLOW_LOAD_MS) {
        logQuiet("Audio: loading '%s' (%lu bytes) took %.1f ms, %.1f ms of it reading data.win\n", name, (unsigned long) entry->dataSize,
                 TICKS_TO_MS(loadTicks), TICKS_TO_MS(readTicks));
    }
    return true;
}

// Live streams of a SOND asset, playing or waiting to open.
static int streamsOfSound(CtrAudioSystem* ca, int32_t soundIndex) {
    int count = 0;
    for (int i = 0; i < VOICE_COUNT; i++) {
        const Voice* v = &ca->voices[i];
        bool streaming = v->kind == VOICE_STREAM || (v->kind == VOICE_PENDING && v->waitStream);
        if (streaming && v->soundIndex == soundIndex && voiceLive(ca, i)) count++;
    }
    return count;
}

// Whether a sound that is not cached yet plays as a stream now. A short sound
// is decoded into the cache in the background the first time it plays, so
// later plays (often several at once) start at no cost instead of each
// opening a stream. The first play streams meanwhile, so it starts as soon as
// before (the decode can take most of a second on the Old 3DS, where it gets
// only part of the system core); copies beyond the first few wait for the
// cache. Queues the decode if needed. Takes ownership of path, which it hands
// to the decode job (and copies for the stream) as needed.
static SourceStatus streamWhileCaching(CtrAudioSystem* ca, int32_t soundIndex, char* path, const uint8_t* mem, uint32_t memSize,
                                       VoiceSource* src) {
    SoundForm form = (SoundForm) ca->soundForms[soundIndex];
    bool caching = prepInFlight(ca, soundIndex);
    if (!caching && form != FORM_STREAMED) {
        char* jobPath = path != NULL ? safeStrdup(path) : NULL;
        caching = requestPrepare(ca, soundIndex, jobPath, mem, memSize);
        if (!caching) free(jobPath);
    }
    if (caching && streamsOfSound(ca, soundIndex) >= CACHING_STREAMS_MAX) {
        free(path);
        return SOURCE_PENDING;
    }
    src->path = path;
    src->mem = mem;
    src->memSize = memSize;
    return SOURCE_READY;
}

// Finds where a SOND asset plays from, without blocking: the SFX cache, a
// stream, or SOURCE_PENDING while it is read (or, for extra copies, decoded)
// in the background; update() asks again each frame.
static SourceStatus resolveSoundSource(CtrAudioSystem* ca, int32_t soundIndex, VoiceSource* src) {
    memset(src, 0, sizeof(*src));
    const Sound* sound = &ca->base.audioGroups[0]->sond.sounds[soundIndex];
    CachedSfx* c = &ca->sfxCache[soundIndex];
    if (c->pcm != NULL) {
        src->sfx = c;
        ca->windowCacheHits++;
        return SOURCE_READY;
    }
    if (loadInFlight(ca, soundIndex)) return SOURCE_PENDING;

    if (soundIsInAudo(sound)) {
        DataWin* group = NULL;
        AudioEntry* entry = audoEntryFor(ca, sound, &group);
        if (entry == NULL) return SOURCE_FAILED;
        // A big one plays from its section of the file, like a sound in its own file.
        char* section = audoSectionPath(ca, sound, group, entry);
        if (section != NULL) return streamWhileCaching(ca, soundIndex, section, NULL, 0, src);
        // Not read yet: read it in the background and start the voice when it's in.
        if (entry->data == NULL && !ca->noBackgroundRead[soundIndex] && requestLoad(ca, soundIndex, sound->audioGroup, group, entry)) {
            return SOURCE_PENDING;
        }
        if (entry->data == NULL) loadAudoBlob(group, (uint32_t) sound->audioFile);
        if (entry->data != NULL && CtrDecoder_sniff(entry->data, entry->dataSize) == CTR_DECODER_VORBIS) {
            return streamWhileCaching(ca, soundIndex, NULL, entry->data, entry->dataSize, src);
        }
        return prepareAudoSource(ca, soundIndex, &src->sfx, &src->mem, &src->memSize) ? SOURCE_READY : SOURCE_FAILED;
    }

    char* path = resolveExternalPath(ca, sound);
    if (path == NULL) {
        logWarn("Audio: could not resolve the file of sound '%s'\n", sound->name);
        return SOURCE_FAILED;
    }
    if (pathIsMp3(path)) {
        if (!ca->warned[soundIndex]) logWarn("Audio: '%s' is MP3, which is not supported\n", sound->name);
        ca->warned[soundIndex] = true;
        free(path);
        return SOURCE_FAILED;
    }
    return streamWhileCaching(ca, soundIndex, path, NULL, 0, src);
}

static const char* voiceName(CtrAudioSystem* ca, const Voice* v) {
    if (v->soundIndex >= AUDIO_STREAM_INDEX_BASE) {
        int32_t slot = v->soundIndex - AUDIO_STREAM_INDEX_BASE;
        if (slot < MAX_STREAM_ENTRIES && ca->streamEntries[slot].path != NULL) return baseName(ca->streamEntries[slot].path);
        return "stream";
    }
    return ca->base.audioGroups[0]->sond.sounds[v->soundIndex].name;
}

// Hands voice vi a stream context and starts its stream, or leaves it pending
// (holding its source) until a context is let go. Takes ownership of path.
// Returns false (with the voice freed) if no stream can be had.
static bool beginStreamVoice(CtrAudioSystem* ca, int vi, char* path, const uint8_t* mem, uint32_t memSize) {
    Voice* v = &ca->voices[vi];
    int ctx = claimStreamContext(ca, v->priority, vi);
    if (ctx == CLAIM_WAIT) {
        if (!v->waitStream) {
            v->kind = VOICE_PENDING;
            v->waitStream = true;
            v->pendPath = path;
            v->pendMem = mem;
            v->pendMemSize = memSize;
            v->pendSince = svcGetSystemTick();
            return true;
        }
        if (TICKS_TO_MS(svcGetSystemTick() - v->pendSince) < STREAM_WAIT_MS) return true;
        logWarn("Audio: timed out waiting for a stream to stop\n");
        ctx = -1;
    }
    if (v->waitStream) {
        path = v->pendPath;
        mem = v->pendMem;
        memSize = v->pendMemSize;
        v->pendPath = NULL;
        v->waitStream = false;
    }
    if (ctx < 0) {
        logWarn("Audio: no free stream for '%s'\n", voiceName(ca, v));
        free(path);
        v->kind = VOICE_FREE;
        return false;
    }
    v->kind = VOICE_STREAM;
    startStream(ca, vi, ctx, path, mem, memSize, voiceName(ca, v));
    ca->windowStreamStarts++;
    return true;
}

// Starts voice vi (already claimed and filled in) from a cached SFX, or as a
// stream of a file or AUDO blob. Takes ownership of src->path. Returns false
// (with the voice freed) if no stream context is available.
static bool startVoiceSource(CtrAudioSystem* ca, int vi, VoiceSource* src) {
    Voice* v = &ca->voices[vi];
    CachedSfx* sfx = src->sfx;
    if (sfx != NULL) {
        v->kind = VOICE_SFX;
        v->sfx = sfx;
        sfx->lastUse = ++ca->useClock;
        sfx->lastPlayTick = svcGetSystemTick();
        ndspChnReset(vi);
        ndspChnSetInterp(vi, NDSP_INTERP_POLYPHASE);
        ndspChnSetFormat(vi, sfx->format);
        applyVoice(ca, vi);
        uint32_t startFrame = v->startPosition > 0.0f ? (uint32_t) (v->startPosition * (float) sfx->sampleRate) : 0;
        queueSfxBuffers(v, vi, startFrame);
        return true;
    }
    return beginStreamVoice(ca, vi, src->path, src->mem, src->memSize);
}

// Starts the voices whose background reads or decodes have finished, and the
// ones waiting for a stream context. Game thread.
static void startPendingVoices(CtrAudioSystem* ca) {
    collectLoads(ca);
    collectPreps(ca);
    for (int vi = 0; vi < VOICE_COUNT; vi++) {
        Voice* v = &ca->voices[vi];
        if (v->kind != VOICE_PENDING) continue;
        if (v->waitStream) {
            beginStreamVoice(ca, vi, NULL, NULL, 0);
            continue;
        }
        VoiceSource src;
        SourceStatus status = resolveSoundSource(ca, v->soundIndex, &src);
        if (status == SOURCE_PENDING) continue;
        if (status == SOURCE_FAILED) {
            v->kind = VOICE_FREE;
            continue;
        }
        startVoiceSource(ca, vi, &src);
    }
}

static int32_t playSoundImpl(CtrAudioSystem* ca, int32_t soundIndex, int32_t priority, bool loop) {
    if (!ca->initialized) return -1;
    ca->windowPlays++;

    float gain, pitch;
    VoiceSource src;
    memset(&src, 0, sizeof(src));
    float silentLength = 0.0f;
    bool pending = false;

    if (soundIndex >= AUDIO_STREAM_INDEX_BASE) {
        int32_t slot = soundIndex - AUDIO_STREAM_INDEX_BASE;
        if (slot >= MAX_STREAM_ENTRIES || !ca->streamEntries[slot].active) {
            logWarn("Audio: invalid stream index %d\n", soundIndex);
            return -1;
        }
        StreamEntry* entry = &ca->streamEntries[slot];
        gain = entry->initialGain;
        pitch = entry->initialPitch;
        if (ca->ndspReady) src.path = safeStrdup(entry->path);
        else silentLength = streamEntryLength(ca, soundIndex);
    } else {
        if (soundIndex < 0 || (uint32_t) soundIndex >= ca->soundCount) {
            logWarn("Audio: invalid sound index %d\n", soundIndex);
            return -1;
        }
        const Sound* sound = &ca->base.audioGroups[0]->sond.sounds[soundIndex];
        gain = sound->volume;
        pitch = sound->pitch > 0.0f ? sound->pitch : 1.0f;

        if (!ca->ndspReady) {
            if (!soundIsInAudo(sound)) {
                char* external = resolveExternalPath(ca, sound);
                bool mp3 = external != NULL && pathIsMp3(external);
                free(external);
                if (mp3) {
                    if (!ca->warned[soundIndex]) logWarn("Audio: '%s' is MP3, which is not supported\n", sound->name);
                    ca->warned[soundIndex] = true;
                    return -1;
                }
            }
            silentLength = utsSilentLength(ca, soundIndex);
        } else {
            SourceStatus status = resolveSoundSource(ca, soundIndex, &src);
            if (status == SOURCE_FAILED) return -1;
            pending = status == SOURCE_PENDING;
        }
    }

    int vi = claimVoice(ca, priority);
    if (vi < 0) {
        free(src.path);
        return -1;
    }
    Voice* v = &ca->voices[vi];
    rampRelease(ca, vi);
    memset(v, 0, sizeof(*v));
    v->instanceId = nextInstanceId(ca);
    v->soundIndex = soundIndex;
    v->priority = priority;
    v->serial = ca->serialCounter++;
    v->loop = loop;
    v->gain = gain;
    v->fadeTarget = gain;
    v->pitch = pitch;
    v->emitterGain = 1.0f;
    v->ctx = -1;

    if (!ca->ndspReady) {
        v->kind = VOICE_VIRTUAL;
        v->virtualLength = silentLength;
        return v->instanceId;
    }

    if (pending) {
        v->kind = VOICE_PENDING;
        return v->instanceId;
    }
    return startVoiceSource(ca, vi, &src) ? v->instanceId : -1;
}

static int32_t ctrPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    u64 t0 = svcGetSystemTick();
    int32_t id = playSoundImpl(ca, soundIndex, priority, loop);
    ca->frameTicks += svcGetSystemTick() - t0;
    return id;
}

static void ctrSetSoundEmitterGain(AudioSystem* audio, int32_t instanceId, float gain) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (!isInstanceId(instanceId) || !voiceMatches(ca, i, instanceId)) continue;
        ca->voices[i].emitterGain = gain;
        applyVoice(ca, i);
    }
}

static void ctrSetSoundSpatial(AudioSystem* audio, int32_t instanceId, float x, float y, float z, float ref, float max, float factor) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (!isInstanceId(instanceId) || !voiceMatches(ca, i, instanceId)) continue;
        Voice* v = &ca->voices[i];
        v->spatial = true;
        v->sx = x;
        v->sy = y;
        v->sz = z;
        v->falloffRef = ref > 0 ? ref : 0.0001f;
        v->falloffMax = max > 0 ? max : 0.0001f;
        v->falloffFactor = factor;
        applyVoice(ca, i);
    }
}

static void ctrSetListenerPosition(AudioSystem* audio, float x, float y, float z) {
    audio->listenerX = x;
    audio->listenerY = y;
    audio->listenerZ = z;
}

static void ctrStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    u64 t0 = svcGetSystemTick();
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (voiceMatches(ca, i, soundOrInstance)) stopVoice(ca, i);
    }
    ca->frameTicks += svcGetSystemTick() - t0;
}

static void ctrStopAll(AudioSystem* audio) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    u64 t0 = svcGetSystemTick();
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (ca->voices[i].kind != VOICE_FREE) stopVoice(ca, i);
    }
    ca->frameTicks += svcGetSystemTick() - t0;
}

static bool ctrIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (voiceMatches(ca, i, soundOrInstance) && !ca->voices[i].paused) return true;
    }
    return false;
}

static void setPaused(CtrAudioSystem* ca, int32_t soundOrInstance, bool all, bool paused) {
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (all ? !voiceLive(ca, i) : !voiceMatches(ca, i, soundOrInstance)) continue;
        ca->voices[i].paused = paused;
        applyVoice(ca, i);
    }
}

static void ctrPauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    setPaused((CtrAudioSystem*) audio, soundOrInstance, false, true);
}

static void ctrResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    setPaused((CtrAudioSystem*) audio, soundOrInstance, false, false);
}

static void ctrPauseAll(AudioSystem* audio) {
    setPaused((CtrAudioSystem*) audio, 0, true, true);
}

static void ctrResumeAll(AudioSystem* audio) {
    setPaused((CtrAudioSystem*) audio, 0, true, false);
}

static void setSuspended(CtrAudioSystem* ca, bool suspended) {
    ca->suspended = suspended;
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (voiceLive(ca, i)) applyVoice(ca, i);
    }
}

static void ctrSuspend(AudioSystem* audio) {
    setSuspended((CtrAudioSystem*) audio, true);
}

static void ctrResume(AudioSystem* audio) {
    setSuspended((CtrAudioSystem*) audio, false);
}

// ===[ Vtable: gain, pitch, position ]===

static void setVoiceGain(CtrAudioSystem* ca, int vi, float gain, uint32_t timeMs) {
    Voice* v = &ca->voices[vi];
    if (timeMs == 0) {
        v->gain = gain;
        v->fadeTarget = gain;
        v->fadeRemaining = 0.0f;
        applyVoice(ca, vi);
    } else {
        v->fadeStart = v->gain;
        v->fadeTarget = gain;
        v->fadeTotal = (float) timeMs / 1000.0f;
        v->fadeRemaining = v->fadeTotal;
    }
}

static StreamEntry* streamEntryFor(CtrAudioSystem* ca, int32_t streamIndex) {
    int32_t slot = streamIndex - AUDIO_STREAM_INDEX_BASE;
    if (slot < 0 || slot >= MAX_STREAM_ENTRIES || !ca->streamEntries[slot].active) return NULL;
    return &ca->streamEntries[slot];
}

static void ctrSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    // A sound asset's gain multiplies each of its instances' own gains, as in
    // GameMaker, instead of replacing them: credfixSetSoundGain, late code at
    // the end of this file (credfix). This body is a jump there; the rest of
    // its slot is padding that nothing reaches, so nothing after it moves.
    // The line count is kept, so every later __LINE__ in the file is too.
    void credfixSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs);
    // Through credcapSetSoundGain, which caps what a voice plays at to 1, as GameMaker does (both games, measured
    // on PC: pcref credfix-hell4 for UNDERTALE 1.08, credfix-drcap3 for DELTARUNE).
    void credcapSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs);
    // credcapSetSoundGain runs credfixSetSoundGain first; this slot keeps its line count.
    credcapSetSoundGain(audio, soundOrInstance, gain, timeMs);
}

static float ctrGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    // An asset's own gain, or an instance's own gain apart from its asset's:
    // credfixGetSoundGain, late code at the end of this file (credfix), as
    // ctrSetSoundGain above.
    float credfixGetSoundGain(AudioSystem* audio, int32_t soundOrInstance);
    return credfixGetSoundGain(audio, soundOrInstance);
    //
    //
    //
    //
}

static void ctrSetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    if (soundOrInstance >= AUDIO_STREAM_INDEX_BASE) {
        StreamEntry* entry = streamEntryFor(ca, soundOrInstance);
        if (entry != NULL) entry->initialPitch = pitch;
    }
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (!voiceMatches(ca, i, soundOrInstance)) continue;
        ca->voices[i].pitch = pitch;
        applyVoice(ca, i);
    }
}

static float ctrGetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    if (soundOrInstance >= AUDIO_STREAM_INDEX_BASE) {
        StreamEntry* entry = streamEntryFor(ca, soundOrInstance);
        if (entry != NULL) return entry->initialPitch;
    }
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (voiceMatches(ca, i, soundOrInstance)) return ca->voices[i].pitch;
    }
    return 1.0f;
}

static float voicePosition(CtrAudioSystem* ca, int vi) {
    Voice* v = &ca->voices[vi];
    switch (v->kind) {
        case VOICE_SFX: {
            u16 seq = ndspChnGetWaveBufSeq(vi);
            u32 samplePos = ndspChnGetSamplePos(vi);
            for (int i = 0; i < v->wbCount; i++) {
                u8 status = v->wb[i].status;
                if ((status == NDSP_WBUF_PLAYING || status == NDSP_WBUF_QUEUED) && v->wb[i].sequence_id == seq) {
                    return (float) (v->wbStart[i] + samplePos) / (float) v->sfx->sampleRate;
                }
            }
            return 0.0f;
        }
        case VOICE_STREAM:
            return streamPosition(ca, v);
        case VOICE_VIRTUAL:
            if (v->loop && v->virtualLength > 0.0f) return (float) fmod(v->virtualSeconds, v->virtualLength);
            return (float) v->virtualSeconds;
        case VOICE_PENDING:
            return v->startPosition;
        default:
            return 0.0f;
    }
}

static float voiceLength(CtrAudioSystem* ca, const Voice* v) {
    if (v->kind == VOICE_SFX) return (float) v->sfx->frames / (float) v->sfx->sampleRate;
    if (v->kind == VOICE_VIRTUAL) return v->virtualLength;
    return soundLength(ca, v->soundIndex);
}

// The position by the game's clock (virtual clock only): wrapped for a loop,
// held at the end otherwise, once the length is known.
static float clockPosition(CtrAudioSystem* ca, const Voice* v) {
    float length = voiceLength(ca, v);
    if (length <= 0.0f) return (float) v->clockSeconds;
    if (v->loop) return (float) fmod(v->clockSeconds, (double) length);
    return v->clockSeconds < (double) length ? (float) v->clockSeconds : length;
}

static float ctrGetTrackPosition(AudioSystem* audio, int32_t soundOrInstance) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (!voiceMatches(ca, i, soundOrInstance)) continue;
        return GetTime_virtualClockOn() ? clockPosition(ca, &ca->voices[i]) : voicePosition(ca, i);
    }
    return 0.0f;
}

static void ctrSetTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    u64 t0 = svcGetSystemTick();
    if (positionSeconds < 0.0f) positionSeconds = 0.0f;
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (!voiceMatches(ca, i, soundOrInstance)) continue;
        Voice* v = &ca->voices[i];
        v->clockSeconds = positionSeconds;
        if (v->kind == VOICE_SFX) {
            ndspChnWaveBufClear(i);
            queueSfxBuffers(v, i, (uint32_t) (positionSeconds * (float) v->sfx->sampleRate));
        } else if (v->kind == VOICE_STREAM) {
            StreamContext* s = &ca->streams[v->ctx];
            LightLock_Lock(&s->lock);
            s->seekPending = true;
            s->seekSeconds = positionSeconds;
            LightLock_Unlock(&s->lock);
            v->lastPosition = positionSeconds;
            LightEvent_Signal(&ca->event);
        } else if (v->kind == VOICE_VIRTUAL) {
            v->virtualSeconds = positionSeconds;
        } else if (v->kind == VOICE_PENDING) {
            v->startPosition = positionSeconds;
        }
    }
    ca->frameTicks += svcGetSystemTick() - t0;
}

static float ctrGetSoundLength(AudioSystem* audio, int32_t soundOrInstance) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    if (!ca->initialized) return 0.0f;
    u64 t0 = svcGetSystemTick();
    float seconds = 0.0f;
    if (isInstanceId(soundOrInstance)) {
        for (int i = 0; i < VOICE_COUNT; i++) {
            if (!voiceMatches(ca, i, soundOrInstance)) continue;
            seconds = voiceLength(ca, &ca->voices[i]);
            break;
        }
    } else {
        seconds = soundLength(ca, soundOrInstance);
    }
    ca->frameTicks += svcGetSystemTick() - t0;
    return seconds;
}

// ===[ Vtable: master, channels, groups ]===

static void applyMasterGain(CtrAudioSystem* ca) {
    float gain = ca->masterGain * ca->listenerGain;
    if (gain < 0.0f) gain = 0.0f;
    if (ca->ndspReady) ndspSetMasterVol(gain);
}

static void ctrSetMasterGain(AudioSystem* audio, float gain) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    ca->masterGain = gain;
    applyMasterGain(ca);
}

static void ctrSetMasterGainForListener(AudioSystem* audio, float gain, int32_t listenerId) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    if (listenerId != 0) return; // Only one listener (no spatial panning).
    ca->listenerGain = gain;
    applyMasterGain(ca);
}

static void ctrSetChannelCount(AudioSystem* audio, int32_t count) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    if (count < 1) count = 1;
    if (count > VOICE_COUNT) count = VOICE_COUNT;
    ca->voiceLimit = count;
}

static void ctrSetGroupGain(AudioSystem* audio, int32_t groupIndex, float gain, uint32_t timeMs) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    AudioSystem_setGroupGain(audio, groupIndex, gain, timeMs);
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (voiceLive(ca, i) && AudioSystem_soundGroup(audio, ca->voices[i].soundIndex) == groupIndex) applyVoice(ca, i);
    }
}

static void ctrGroupLoad(AudioSystem* audio, int32_t groupIndex) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    if (groupIndex <= 0 || audio->dw->agrp.count <= (uint32_t) groupIndex) {
        logWarn("Audio: wanted to load audio group %d, which does not exist in AGRP\n", groupIndex);
        return;
    }
    AudioGroup* group = &audio->dw->agrp.audioGroups[groupIndex];
    char name[64];
    const char* file = group->path;
    if (file == NULL) {
        snprintf(name, sizeof(name), "audiogroup%d.dat", groupIndex);
        file = name;
    }
    // Like the original runner, a missing file is not an error.
    // groupPaths stays parallel to audioGroups (init added group 0).
    while (arrlen(ca->groupPaths) < arrlen(audio->audioGroups)) arrput(ca->groupPaths, NULL);
    if (!ca->fileSystem->vtable->fileExists(ca->fileSystem, file)) {
        // The game ships the file, so the copied game folder is incomplete
        // (one Steam install of DELTARUNE lacked Chapter 5's). Each sound
        // in the group then plays silently without a warning of its own.
        uint32_t silent = 0;
        for (uint32_t i = 0; audio->dw->sond.count > i; i++) {
            const Sound* sound = &audio->dw->sond.sounds[i];
            if (sound->present && sound->audioGroup == groupIndex && soundIsInAudo(sound)) silent++;
        }
        logWarn("Audio: audio group %d (%s) not found, so its %u sounds are silent; copy it from the PC game's folder\n", groupIndex,
                file, (unsigned) silent);
        missingAudio(ca, file);
        DataWin* empty = (DataWin*) safeCalloc(1, sizeof(DataWin));
        arrput(audio->audioGroups, empty);
        arrput(ca->groupPaths, NULL);
        return;
    }
    DataWinParserOptions options = {0};
    options.parseAudo = true;
    options.lazyLoadAudio = audio->dw->lazyLoadAudio;
    if (audio->dw->mappedFile) options.loadType = DATAWINLOADTYPE_MAP_FILE;
    char* resolved = ca->fileSystem->vtable->resolvePath(ca->fileSystem, file);
    DataWin* loaded = DataWin_parse(resolved, options);
    arrput(audio->audioGroups, loaded);
    arrput(ca->groupPaths, resolved);
}

static bool ctrGroupIsLoaded(AudioSystem* audio, int32_t groupIndex) {
    return arrlen(audio->audioGroups) > groupIndex;
}

// ===[ Vtable: create_stream ]===

static int32_t ctrCreateStream(AudioSystem* audio, const char* filename) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    for (int i = 0; i < MAX_STREAM_ENTRIES; i++) {
        StreamEntry* entry = &ca->streamEntries[i];
        if (entry->active) continue;
        char* resolved = ca->fileSystem->vtable->resolvePath(ca->fileSystem, filename);
        if (resolved == NULL) {
            logWarn("Audio: could not resolve stream file '%s'\n", filename);
            missingAudio(ca, filename);
            return -1;
        }
        entry->active = true;
        entry->path = resolved;
        entry->initialGain = 1.0f;
        entry->initialPitch = 1.0f;
        entry->lengthSeconds = -1.0f;
        logQuiet("Audio: created stream %d for '%s'\n", AUDIO_STREAM_INDEX_BASE + i, resolved);
        return AUDIO_STREAM_INDEX_BASE + i;
    }
    logWarn("Audio: no free stream slots for '%s'\n", filename);
    return -1;
}

static bool ctrDestroyStream(AudioSystem* audio, int32_t streamIndex) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    StreamEntry* entry = streamEntryFor(ca, streamIndex);
    if (entry == NULL) return false;
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (voiceMatches(ca, i, streamIndex)) stopVoice(ca, i);
    }
    free(entry->path);
    entry->path = NULL;
    entry->active = false;
    return true;
}

// ===[ Creation ]===

__attribute__((noinline, section(".text.pcmwait_late"))) static void pcmWaitUpdate(AudioSystem* audio, float deltaTime); static AudioSystemVtable ctrAudioSystemVtable = {
    .init = ctrInit,
    .destroy = ctrDestroy,
    .update = pcmWaitUpdate,
    .playSound = ctrPlaySound,
    .setSoundSpatial = ctrSetSoundSpatial,
    .setSoundEmitterGain = ctrSetSoundEmitterGain,
    .setListenerPosition = ctrSetListenerPosition,
    .stopSound = ctrStopSound,
    .stopAll = ctrStopAll,
    .isPlaying = ctrIsPlaying,
    .pauseSound = ctrPauseSound,
    .resumeSound = ctrResumeSound,
    .pauseAll = ctrPauseAll,
    .resumeAll = ctrResumeAll,
    .suspend = ctrSuspend,
    .resume = ctrResume,
    .setSoundGain = ctrSetSoundGain,
    .getSoundGain = ctrGetSoundGain,
    .setSoundPitch = ctrSetSoundPitch,
    .getSoundPitch = ctrGetSoundPitch,
    .getTrackPosition = ctrGetTrackPosition,
    .setTrackPosition = ctrSetTrackPosition,
    .getSoundLength = ctrGetSoundLength,
    .setMasterGain = ctrSetMasterGain,
    .setMasterGainForListener = ctrSetMasterGainForListener,
    .setChannelCount = ctrSetChannelCount,
    .setGroupGain = ctrSetGroupGain,
    .groupLoad = ctrGroupLoad,
    .groupIsLoaded = ctrGroupIsLoaded,
    .createStream = ctrCreateStream,
    .destroyStream = ctrDestroyStream,
};

CtrAudioSystem* CtrAudioSystem_create(void) {
    CtrAudioSystem* ca = (CtrAudioSystem*) safeCalloc(1, sizeof(CtrAudioSystem));
    ca->base.vtable = &ctrAudioSystemVtable;
    ca->voiceLimit = VOICE_COUNT;
    ca->masterGain = 1.0f;
    ca->listenerGain = 1.0f;
    // Runner_reset calls stopAll before init, so everything stopAll touches
    // must be valid from here on.
    for (int i = 0; i < VOICE_COUNT; i++) ca->voices[i].ctx = -1;
    for (int i = 0; i < STREAM_CONTEXTS; i++) {
        LightLock_Init(&ca->streams[i].lock);
        ca->streams[i].owner = -1;
        ca->streams[i].state = CTX_FREE;
    }
    LightLock_Init(&ca->statsLock);
    LightLock_Init(&ca->rampLock);
    LightLock_Init(&ca->msgLock);
    LightEvent_Init(&ca->event, RESET_ONESHOT);
    LightEvent_Init(&ca->openEvent, RESET_ONESHOT);
    ca->leastQueuedMs = -1.0f;
    LightLock_Init(&ca->loadLock);
    LightEvent_Init(&ca->loadEvent, RESET_ONESHOT);
    return ca;
}

void CtrAudioSystem_setDataWinPath(CtrAudioSystem* ca, const char* path) {
    free(ca->dataWinPath);
    ca->dataWinPath = path != NULL ? safeStrdup(path) : NULL;
}

// ===[ Options menu pause (menupause) ]===
// While the in-game options menu is open every sound holds where it is: the game step was already paused, the music
// was not ("i think the game does pause, buyt the music doesnt"). Late code, linked in by two wraps
// (CMakeLists.txt): the menu's open wrap (ctr_screens.c) calls CtrLegal_close first, and CtrLegal_close has no other
// caller outside ctr_legal.c, so __wrap_CtrLegal_close holds the audio as the menu opens; __wrap_CtrMenu_close
// releases it on every close (Resume, Quit, Performance test and the scheduled test menu).
// - The hold is the suspend hook (setSuspended), which ORs into each voice's pause without touching it, so a sound
//   the game paused stays paused afterwards. NDSP pauses a channel in place: the queued buffers stay queued and play
//   on from the same sample, and the decode thread finds no free buffer (no decoding, no underrun: a paused stream
//   never counts as one).
// - No click: the master volume steps down to silence before the channels pause, and back up after, one step per
//   audio update (a game frame), so neither the open nor the close waits on the DSP on the game thread (smoothux
//   menu1: the earlier 6 steps of a 5 ms sleep each held the menu's first frames 35 ms and the game's first frame
//   after the close 30 ms).
// - While held, the audio update runs with no elapsed time, so the game's fades, group fades and sound clocks hold
//   too; an open video's clock holds with its sound (CtrVideo_menuPause).
// - The release is deferred to the next audio update: Quit leaves the loop without one, so the music doesn't come
//   back for the moment before the game exits; Resume and the Performance test's picker run one on their next frame.
// - HOME and sleep don't use the suspend flag (NDSP saves and restores the channels and the master volume itself,
//   and the video has its own mask bits), so they nest with this hold either way round.
// - The menu's own sounds (CtrAudio_menuSound, from ctr_optmenu.c) start unpaused during the hold; the fade waits
//   while the last one sounds (before this they started paused and all played at the close).
#define MENUPAUSE_CODE __attribute__((noinline, section(".text.late.menupause")))
#define MENUPAUSE_BSS __attribute__((section(".bss.late.menupause")))
#define MENUPAUSE_RO __attribute__((section(".rodata.late.menupause")))
#define MENUPAUSE_RAMP_FRAMES 6

void CtrVideo_menuPause(bool pause);
void __real_CtrLegal_close(void);
void __real_CtrMenu_close(void* menu);
void __wrap_CtrLegal_close(void);
void __wrap_CtrMenu_close(void* menu);

// 0 when not held; else 1 + the fade's step (MENUPAUSE_RAMP_FRAMES at full volume, 0 silent: the channels suspended).
static uint8_t menuHeld MENUPAUSE_BSS;
static bool menuReleasePending MENUPAUSE_BSS;
static u64 menuHeldSince MENUPAUSE_BSS;
static void (*menuRealUpdate)(AudioSystem* audio, float deltaTime) MENUPAUSE_BSS;
// The menu's sounds (gamedir2): new code and state in its own late sections, at the end of the late
// blocks (late_reserve.ld), named outside .text.late.* and .bss.late.* so the catch-alls leave them there.
#define MENUSOUND_CODE __attribute__((noinline, section(".text_gamedir2_late")))
static int32_t menuVoice __attribute__((section(".bss.gamedir2_late"))); // the instance of the menu's last sound
// The vtable's functions, called through a pointer the compiler can't see through: a plain ca->base.vtable->suspend
// call was speculatively devirtualized (ctrSuspend inlined behind a check), which changed where ctrSuspend's own copy
// was emitted and moved every function after it in this object by 72 B.
#define MENUPAUSE_OPAQUE(fn) ({ __typeof__(fn) opaque_ = (fn); __asm__("" : "+r"(opaque_)); opaque_; })

static const char menuHeldText[] MENUPAUSE_RO = "Audio: held for the options menu (%lu underruns so far)\n";
static const char menuReleasedText[] MENUPAUSE_RO = "Audio: released after the options menu, held %.1f s (%lu underruns so far)\n";

// The master volume at a step of the fade (applyMasterGain's value at MENUPAUSE_RAMP_FRAMES); the step kept in menuHeld.
MENUPAUSE_CODE static void menuSetLevel(CtrAudioSystem* ca, int level) {
    float gain = ca->masterGain * ca->listenerGain;
    if (gain < 0.0f) gain = 0.0f;
    menuHeld = (uint8_t) (level + 1);
    if (ca->ndspReady) ndspSetMasterVol(gain * (float) level / (float) MENUPAUSE_RAMP_FRAMES);
}

// The audio update while held. The menu closed: the channels back first, then a step up per update with the game's
// time running; the hold ends at full volume. The menu open: a step down, the channels suspended once silent, and no
// time passes. A close during the fade down, or an open during the fade up, turns the fade round where it is.
// The menu's own sounds (menuSound) play at full volume: the fade, either way, waits while one sounds, and once the
// game's sounds are silent and paused the master volume is back up for them.
MENUPAUSE_CODE static void menuHeldUpdate(AudioSystem* audio, float deltaTime) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    int level = menuHeld - 1;
    if (menuVoice != 0 && MENUPAUSE_OPAQUE(ca->base.vtable->isPlaying)(audio, menuVoice)) {
        MENUPAUSE_OPAQUE(menuRealUpdate)(audio, 0.0f);
        return;
    }
    if (menuReleasePending) {
        if (level == 0) {
            if (ca->ndspReady) ndspSetMasterVol(0.0f);
            CtrVideo_menuPause(false);
            MENUPAUSE_OPAQUE(ca->base.vtable->resume)(&ca->base);
            logInfo(menuReleasedText, TICKS_TO_MS(svcGetSystemTick() - menuHeldSince) / 1000.0, (unsigned long) CtrAudioSystem_underruns(ca));
        }
        level = ca->ndspReady ? level + 1 : MENUPAUSE_RAMP_FRAMES;
        menuSetLevel(ca, level);
        if (level >= MENUPAUSE_RAMP_FRAMES) {
            menuHeld = 0;
            menuReleasePending = false;
            ctrAudioSystemVtable.update = menuRealUpdate;
        }
        MENUPAUSE_OPAQUE(menuRealUpdate)(audio, deltaTime);
        return;
    }
    if (level > 0) {
        level = ca->ndspReady ? level - 1 : 0;
        menuSetLevel(ca, level);
        if (level == 0) {
            MENUPAUSE_OPAQUE(ca->base.vtable->suspend)(&ca->base);
            menuSetLevel(ca, MENUPAUSE_RAMP_FRAMES);
            menuHeld = 1;
            CtrVideo_menuPause(true);
            menuHeldSince = svcGetSystemTick();
            logInfo(menuHeldText, (unsigned long) CtrAudioSystem_underruns(ca));
        }
    }
    MENUPAUSE_OPAQUE(menuRealUpdate)(audio, 0.0f);
}

// The menu's open: the hold starts at full volume; its steps down follow at the audio updates.
MENUPAUSE_CODE static void menuHold(void) {
    CtrAudioSystem* ca = gShutdownTarget;
    if (menuHeld) {
        menuReleasePending = false; // opened again before the fade up ended: back down from where it is
        return;
    }
    if (ca == NULL || !ca->initialized) return;
    menuRealUpdate = MENUPAUSE_OPAQUE(ctrAudioSystemVtable.update);
    ctrAudioSystemVtable.update = menuHeldUpdate;
    menuReleasePending = false;
    menuHeld = MENUPAUSE_RAMP_FRAMES + 1;
    __asm__ volatile("nop"); // one instruction: the section keeps main's size, so nothing linked after it moves
}

// A sound of the options menu (the game's own cursor and choice sounds), started unpaused while the hold has the
// game's sounds paused.
MENUSOUND_CODE void CtrAudio_menuSound(AudioSystem* audio, int32_t id) {
    CtrAudioSystem* ca = gShutdownTarget;
    if (ca == NULL || id < 0) return;
    bool held = ca->suspended;
    ca->suspended = false;
    menuVoice = MENUPAUSE_OPAQUE(audio->vtable->playSound)(audio, id, 100, false);
    ca->suspended = held;
}

MENUPAUSE_CODE void __wrap_CtrLegal_close(void) {
    __real_CtrLegal_close();
    menuHold();
}

MENUPAUSE_CODE void __wrap_CtrMenu_close(void* menu) {
    __real_CtrMenu_close(menu);
    if (menuHeld) menuReleasePending = true;
}

// ===[ utaudio: the setup notice for a missing sound file, a pass-through (late code; see ctr_audio_utaudio.h) ]===
//
// An earlier version skipped the notice for two music files, on the belief that Steam never ships them. That
// was wrong (corrected 2026-10-21): UNDERTALE 1.08's Windows and macOS depots both
// ship them (212 .ogg files; a Steam "Verify integrity of game files" restores them), so a missing one is a broken
// install and gets the notice like any other file. utaNoticeMissing only hands over to CtrNotice_missing now.
// Its slot and the table's keep their earlier sizes, never read, so nothing linked after them moves.

// Never read: the earlier 40 B table in the late text block, kept as zeros (late_reserve.ld). The fill below names it.
static const char utaUnshipped[][20] __attribute__((used, section(".rodata.utaudio_in_text"))) = {
    "",
    "",
};

// Still a C function with its one callee, so GCC orders the file's functions and strings as in main and callers
// see the real callee (a naked stub changes both: docs/3ds-hardware.md). A branch over the fill, then the tail call:
// 4 + 196 + 4 B, the earlier 204 B (main f9aaa519). The fill's first word names the table, so --gc-sections keeps it.
UTA_LATE_R5 static void utaNoticeMissing(const char* kind, const char* path) {
    __asm__ volatile("b 1f\n\t"
                     ".word utaUnshipped\n\t"
                     ".space 192\n"
                     "1:");
    UTA_PHYS_NOTICE(kind, path);
}
//
//
//
//
//

// ===[ utaudio: declick on cut and loop seams (late code; see ctr_audio_utaudio.h) ]===
//
// A sound effect stopped mid-waveform clicks: NDSP cuts it at a DSP frame boundary. A clear followed by a new buffer
// can't hide that, because the voice restarts and the DSP's resampler restarts with empty history: the first output
// samples ramp in from zero. So a cached sound effect is never
// cleared while it sounds. This layer gives the DSP its samples in pieces of about a DSP frame, chained on the same
// voice, one frame ahead of what the DSP has committed. A stop then queues a short faded continuation after the last
// piece instead, on the same voice: the resampler carries on and the join is exact to the sample. The game sees its
// buffers (status, sequence number, position) as if they had been queued whole. A looping buffer wraps here, so a loop
// whose ends don't meet gets the same seam smoothing as streams (a decaying offset over its first 3 ms).

#define UTA_BSS __attribute__((section(".bss.late.utaudio")))
#define UTA_RODATA __attribute__((section(".rodata.late.utaudio")))
#define UTA_CHANNELS VOICE_COUNT
#define UTA_RING 8 // pieces per channel: queued, playing or done
#define UTA_TAIL_MAX 160 // frames of a tail or a seam piece: 3 ms at up to 53 kHz
#define UTA_TAIL_SECONDS 0.003f
#define UTA_SEAM_MAX 320 // frames of a seam piece: its 3 ms, filled out to a DSP frame (at up to 65 kHz)
// DSP frames of the sound queued beyond the frame the status reports: the next one (committed when this callback
// runs), the one this callback's update reaches, and one more, so a callback that runs a frame late leaves no gap.
#define UTA_AHEAD_FRAMES 3
#define UTA_DSP_RATE 32728.498 // NDSP's output rate (SYSCLOCK_ARM11 / 8192)
#define UTA_SEAM_RATIO 4
#define UTA_SEAM_SECONDS_X1000 3
#define UTA_LOG_TICKS (5ULL * SYSCLOCK_ARM11)

enum { UTA_IDLE, UTA_FEED, UTA_PENDING, UTA_TAILING };
enum { UTA_PIECE_SOUND, UTA_PIECE_SEAM, UTA_PIECE_TAIL };

// One of the engine's buffers (Voice.wb[]), as this layer plays it.
typedef struct {
    const uint8_t* data;
    uint32_t frames;
    ndspWaveBuf* mirror; // the engine's buffer; NULL once the voice was stopped
    uint16_t seq; // the sequence number the engine's buffer was given
    uint8_t looping;
} UtaSeg;

typedef struct {
    uint32_t off; // the piece's first frame in its segment
    uint8_t seg, kind;
} UtaPiece;

// A physical channel, as this layer feeds it.
typedef struct {
    uint8_t state;
    uint8_t segs, cur; // segments added; the one the cursor is in
    uint8_t ring; // the next ring slot to try
    uint8_t paused, started, splice; // splice: the cursor came back to a loop's start from the end
    uint8_t chans, bytes; // channels, bytes per frame
    int8_t trigger[2]; // a loop seam to smooth, per channel
    uint8_t seamBuilt;
    uint16_t nextSeq;
    uint32_t off; // the cursor: the next frame of segment cur to queue
    uint32_t seamN, tailEnd;
    float rate;
    UtaSeg seg[2];
    UtaPiece piece[UTA_RING];
} UtaChan;

// A stream's loop seam.
typedef struct {
    bool measured; // a pass from frame 0 to the end was read: maxStep is the file's
    bool fromZero; // the current pass started at frame 0 and wasn't seeked
    bool atEnd;
    bool splice; // sent back to 0 from the end: the next read starts the loop again
    bool logged;
    uint8_t have;
    int16_t last[2], prev[2];
    int32_t maxStep[2];
    float offset[2];
    uint32_t remain, total;
} UtaSeam;

// Whether a cached sound's loop seam is smoothed, measured once per cached copy.
#define UTA_SEAM_SOUNDS 16
typedef struct {
    int32_t sound;
    const uint8_t* pcm;
    uint32_t frames;
    uint16_t format;
    int8_t trigger[2];
} UtaSeamSound;

#if defined(CTRPORT_ADPCM_MUSIC) || defined(CTRPORT_ADPCM_SFXPACK)
#include "ctr_adpcm_uta_state.inc"
#else
static CtrAudioSystem* utaCa UTA_BSS;
static volatile bool utaActive UTA_BSS;
static volatile bool utaMapped UTA_BSS; // utaMap holds the mapping (identity until a voice first moves)
static LightLock utaLock UTA_BSS;
static int8_t utaMap[UTA_CHANNELS] UTA_BSS; // logical (the engine's voice) -> physical channel
static int8_t utaInv[UTA_CHANNELS] UTA_BSS; // physical -> logical
static UtaChan utaChan[UTA_CHANNELS] UTA_BSS;
static ndspWaveBuf utaWb[UTA_CHANNELS][UTA_RING] UTA_BSS;
static uint8_t* utaTailPcm UTA_BSS; // linear: UTA_CHANNELS * UTA_TAIL_MAX stereo s16 frames
static uint8_t* utaSeamPcm UTA_BSS; // linear: UTA_CHANNELS * UTA_SEAM_MAX stereo s16 frames, the seam pieces
static UtaSeamSound utaSeamSounds[UTA_SEAM_SOUNDS] UTA_BSS;
static uint8_t utaSeamNext UTA_BSS;
static ndspCallback utaEngineCallback UTA_BSS;
// postMessage, called through a pointer: a direct call from here would add call-graph edges, and GCC would then
// emit the file's functions in another order (check_layout).
static void (*utaPost)(CtrAudioSystem* ca, const char* fmt, ...) UTA_BSS;
static UtaSeam utaSeams[STREAM_CONTEXTS] UTA_BSS;
static uint32_t utaTails UTA_BSS, utaYields UTA_BSS, utaMoves UTA_BSS, utaUnstarted UTA_BSS, utaHard UTA_BSS;
static uint32_t utaStarved UTA_BSS, utaPieces UTA_BSS, utaLoggedTails UTA_BSS, utaCbCount UTA_BSS;
static uint32_t utaCb UTA_BSS;
#ifdef CTR_AUDIO_CAPTURE_WRAP_H
static uint32_t utaProbe UTA_BSS;
#endif
static uint8_t utaPeak UTA_BSS, utaPeakAll UTA_BSS, utaPeakHeld UTA_BSS;
static u64 utaCbTicks UTA_BSS, utaCbMaxTicks UTA_BSS, utaLastLog UTA_BSS;

#endif
#line 3368
static const char utaLogFormat[] UTA_RODATA = "Audio: declick %lu tails, %lu yielded, %lu moved, %lu unstarted, %lu hard, "
                                              "%lu starved; %lu pieces; peak %u channels (%u in all, %u holding a tail); "
                                              "callback %.1f us avg, %.1f max\n";
static const char utaSeamFormat[] UTA_RODATA = "Audio: loop seam of '%s'%s: ch0 step %ld (max %ld), ch1 step %ld (max %ld): %s\n";
static const char utaSmoothed[] UTA_RODATA = "smoothed";
static const char utaKept[] UTA_RODATA = "kept";
static const char utaCached[] UTA_RODATA = " (cached)";
static const char utaNone[] UTA_RODATA = "";

#if defined(CTRPORT_ADPCM_SFX) || defined(CTRPORT_ADPCM_MUSIC) || defined(CTRPORT_ADPCM_SFXPACK)
#if defined(CTRPORT_ADPCM_MUSIC) || defined(CTRPORT_ADPCM_SFXPACK)
#define AS_CODE MM_CODE
#else
#define AS_CODE __attribute__((noinline, noipa, section(".text.late.adpcm")))
#endif
AS_CODE static void adSfxForget(int p);
AS_CODE static void adSfxRate(int p, float rate);
AS_CODE static void adSfxSegment(int p, UtaChan* c, Voice* v, ndspWaveBuf* wb);
AS_CODE static const uint8_t* adSfxPiece(int p, const UtaChan* c, const UtaSeg* g, uint32_t* frames);
AS_CODE static const UtaSeamSound* adSfxSeam(const Voice* v);
AS_CODE static bool adSfxOffsets(const UtaChan* c, float out[2]);
AS_CODE static const int16_t* adSfxSeamPcm(int p, const UtaChan* c, const UtaSeg* g, uint32_t frames, int16_t* dst);
AS_CODE static void adSfxCaptureTail(int p, const UtaChan* c);
AS_CODE static const uint8_t* adSfxTailSample(int p, const UtaChan* c, const UtaSeg* g, uint32_t j);
#endif
UTA_LATE static int utaPhys(int id) {
#line 3389
    return utaMapped && id >= 0 && id < UTA_CHANNELS ? utaMap[id] : id;
}

// Every 5 s while tails are made, from the game thread: the counts, the busiest moment and the callback's cost.
UTA_LATE static void utaMaybeLog(void) {
    if (threadGetCurrent() != NULL || utaCa == NULL) return;
    u64 now = svcGetSystemTick();
    if (now - utaLastLog < UTA_LOG_TICKS || utaTails == utaLoggedTails) return;
    LightLock_Lock(&utaLock);
    uint32_t count = utaCbCount;
    double avg = count > 0 ? (double) utaCbTicks / count / (CPU_TICKS_PER_MSEC / 1000.0) : 0.0;
    double max = (double) utaCbMaxTicks / (CPU_TICKS_PER_MSEC / 1000.0);
    unsigned peak = utaPeak, held = utaPeakHeld;
    if (utaPeak > utaPeakAll) utaPeakAll = utaPeak;
    utaPeak = 0;
    utaPeakHeld = 0;
    utaCbTicks = 0;
    utaCbMaxTicks = 0;
    utaCbCount = 0;
    LightLock_Unlock(&utaLock);
    utaLastLog = now;
    utaLoggedTails = utaTails;
    utaPost(utaCa, utaLogFormat, (unsigned long) utaTails, (unsigned long) utaYields, (unsigned long) utaMoves,
                (unsigned long) utaUnstarted, (unsigned long) utaHard, (unsigned long) utaStarved, (unsigned long) utaPieces,
                peak, (unsigned) utaPeakAll, held, avg, max);
}

// True if a stream that hasn't been stopped plays on the engine's channel (a stopped one never touches it again).
UTA_LATE static bool utaStreamOn(int id) {
    for (int i = 0; i < STREAM_CONTEXTS; i++) {
        StreamContext* s = &utaCa->streams[i];
        if (s->channel == id && s->state != CTX_FREE && s->state != CTX_DONE && !s->stopRequested) return true;
    }
    return false;
}

// The channel holds none of this layer's pieces any more (after a clear or a reset).
UTA_LATE static void utaForget(int p, UtaChan* c) {
#if defined(CTRPORT_ADPCM_SFX) || defined(CTRPORT_ADPCM_MUSIC) || defined(CTRPORT_ADPCM_SFXPACK)
    adSfxForget(p);
#endif
#line 3427
    c->state = UTA_IDLE;
    c->segs = 0;
    memset(utaWb[p], 0, sizeof(utaWb[p]));
}

UTA_LATE static void utaClearPhys(int p, UtaChan* c) {
    UTA_PHYS_CLEAR(p);
    utaForget(p, c);
}

UTA_LATE static bool utaLive(const ndspWaveBuf* w) {
    return w->status == NDSP_WBUF_QUEUED || w->status == NDSP_WBUF_PLAYING;
}

// Frames of the sound queued and not played yet. NDSP thread, or under the lock.
UTA_LATE static uint32_t utaUnplayed(int p) {
    u16 seq = UTA_PHYS_SEQ(p);
    uint32_t u = 0;
    for (int k = 0; k < UTA_RING; k++) {
        const ndspWaveBuf* w = &utaWb[p][k];
        if (w->status == NDSP_WBUF_QUEUED) {
            u += w->nsamples;
        } else if (w->status == NDSP_WBUF_PLAYING) {
            uint32_t pos = w->sequence_id == seq ? UTA_PHYS_POS(p) : 0;
            u += pos < w->nsamples ? w->nsamples - pos : 0;
        }
    }
    return u;
}

UTA_LATE static int utaPendingPieces(int p) {
    int n = 0;
    for (int k = 0; k < UTA_RING; k++) n += utaLive(&utaWb[p][k]);
    return n;
}

UTA_LATE static void utaQueue(int p, UtaChan* c, const void* data, uint32_t frames, uint8_t seg, uint32_t off, uint8_t kind) {
    for (int k = 0; k < UTA_RING; k++) {
        int slot = (c->ring + k) % UTA_RING;
        ndspWaveBuf* w = &utaWb[p][slot];
        if (utaLive(w)) continue;
        memset(w, 0, sizeof(*w));
        w->data_vaddr = data;
        w->nsamples = frames;
        w->looping = false;
        c->piece[slot].seg = seg;
        c->piece[slot].off = off;
        c->piece[slot].kind = kind;
        c->ring = (uint8_t) ((slot + 1) % UTA_RING);
        UTA_PHYS_ADD(p, w);
        utaPieces++;
        return;
    }
}

// A loop's seam: its length (3 ms) and, per channel, the step to take out (the end's slope continued, minus the
// start), which decays over it as for streams.
UTA_LATE static uint32_t utaSeamOffsets(const UtaChan* c, const UtaSeg* g, float off[2]) {
    uint32_t n = (uint32_t) (c->rate * UTA_SEAM_SECONDS_X1000 / 1000.0f);
    if (n < 8) n = 8;
    if (n > UTA_TAIL_MAX) n = UTA_TAIL_MAX;
#if defined(CTRPORT_ADPCM_SFX) || defined(CTRPORT_ADPCM_MUSIC) || defined(CTRPORT_ADPCM_SFXPACK)
    if (adSfxOffsets(c, off)) return n;
#endif
    const int16_t* x = (const int16_t*) g->data;
    uint32_t ch = c->chans;
#line 3490
    for (uint32_t k = 0; k < 2; k++) {
        off[k] = 0.0f;
        if (k >= ch || !c->trigger[k]) continue;
        int32_t last = x[(g->frames - 1) * ch + k], prev = x[(g->frames - 2) * ch + k];
        off[k] = (float) (2 * last - prev - (int32_t) x[k]);
    }
    return n;
}

UTA_LATE static float utaSeamWeight(uint32_t j, uint32_t n) {
    return j < n ? 0.5f * (1.0f + cosf(3.14159265f * (float) j / (float) n)) : 0.0f;
}

UTA_LATE static int16_t utaClamp16(float v) {
    int32_t s = (int32_t) (v < 0.0f ? v - 0.5f : v + 0.5f);
    return (int16_t) (s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}

// The loop's first 3 ms with the seam's step taken out, then the sound as it is up to a whole DSP frame (so no piece
// ends within a DSP frame of the one before), built once per sound on its channel.
UTA_LATE static void utaBuildSeam(int p, UtaChan* c, const UtaSeg* g, uint32_t frame) {
    float off[2];
    uint32_t n = utaSeamOffsets(c, g, off);
    uint32_t m = frame > n ? frame : n;
    if (m > UTA_SEAM_MAX) m = UTA_SEAM_MAX;
#if defined(CTRPORT_ADPCM_SFX) || defined(CTRPORT_ADPCM_MUSIC) || defined(CTRPORT_ADPCM_SFXPACK)
    int16_t* dst = (int16_t*) (utaSeamPcm + (uint32_t) p * UTA_SEAM_MAX * 4);
    const int16_t* x = adSfxSeamPcm(p, c, g, m, dst);
#else
    const int16_t* x = (const int16_t*) g->data;
    int16_t* dst = (int16_t*) (utaSeamPcm + (uint32_t) p * UTA_SEAM_MAX * 4);
#endif
#line 3517
    uint32_t ch = c->chans;
    for (uint32_t j = 0; j < m; j++) {
        float w = utaSeamWeight(j, n);
        for (uint32_t k = 0; k < ch; k++) dst[j * ch + k] = utaClamp16((float) x[j * ch + k] + off[k] * w);
    }
    DSP_FlushDataCache(dst, m * ch * 2u);
    c->seamN = m;
    c->seamBuilt = 1;
}

// True if the cursor, just back at a loop's start, gets the seam smoothed (PCM16 only, as measured).
UTA_LATE static bool utaSeamAt(const UtaChan* c, const UtaSeg* g) {
    return c->splice && c->off == 0 && (c->trigger[0] || c->trigger[1]) && g->frames >= 4 * UTA_TAIL_MAX &&
           c->bytes == 2 * c->chans;
}

// Moves the cursor past the end of its segment: back to a loop's start, or on to the next segment. False at the end.
UTA_LATE static bool utaAdvance(UtaChan* c) {
    const UtaSeg* g = &c->seg[c->cur];
    if (c->off < g->frames) return true;
    if (g->looping) {
        c->off = 0;
        c->splice = 1;
        return true;
    }
    if (c->cur + 1 < c->segs) {
        c->cur++;
        c->off = 0;
        c->splice = c->seg[c->cur].looping; // the sound's end, then its start again
        return true;
    }
    return false;
}

// Queues pieces until UTA_AHEAD_FRAMES DSP frames of the sound are ahead of what the DSP has played.
UTA_LATE static void utaFeed(int p, UtaChan* c) {
    if (c->state != UTA_FEED || c->segs == 0) return;
    double step = (double) c->rate / UTA_DSP_RATE;
    uint32_t frame = (uint32_t) (160.0 * step) + 1; // source frames in a DSP frame
    uint32_t target = UTA_AHEAD_FRAMES * frame + 4;
    uint32_t u = utaUnplayed(p);
    for (int guard = 0; guard < UTA_RING && u < target; guard++) {
        if (utaPendingPieces(p) >= UTA_RING - 1 || !utaAdvance(c)) return;
        UtaSeg* g = &c->seg[c->cur];
        if (utaSeamAt(c, g)) {
            if (!c->seamBuilt) utaBuildSeam(p, c, g, frame);
            utaQueue(p, c, utaSeamPcm + (uint32_t) p * UTA_SEAM_MAX * 4, c->seamN, c->cur, 0, UTA_PIECE_SEAM);
            c->off = c->seamN;
            c->splice = 0;
            u += c->seamN;
            continue;
        }
        c->splice = 0;
        uint32_t n = target - u;
        if (n < frame) n = frame;
        if (n + frame > g->frames - c->off) n = g->frames - c->off; // no piece shorter than a DSP frame after it
#if defined(CTRPORT_ADPCM_SFX) || defined(CTRPORT_ADPCM_MUSIC) || defined(CTRPORT_ADPCM_SFXPACK)
        const uint8_t* source = adSfxPiece(p, c, g, &n);
        if (!source) return;
        utaQueue(p, c, source, n, c->cur, c->off, UTA_PIECE_SOUND);
#else
        utaQueue(p, c, g->data + c->off * c->bytes, n, c->cur, c->off, UTA_PIECE_SOUND);
#endif
#line 3574
        c->off += n;
        u += n;
    }
}

// The engine's buffers' status: queued until a piece of theirs plays, done when all of a non-looping one's has.
UTA_LATE static void utaMirror(int p, UtaChan* c) {
    for (int i = 0; i < c->segs; i++) {
        UtaSeg* g = &c->seg[i];
        if (g->mirror == NULL) continue;
        bool started = false, live = false;
        for (int k = 0; k < UTA_RING; k++) {
            const ndspWaveBuf* w = &utaWb[p][k];
            if (c->piece[k].seg != i || c->piece[k].kind == UTA_PIECE_TAIL || w->status == NDSP_WBUF_FREE) continue;
            if (w->status != NDSP_WBUF_QUEUED) started = true;
            if (utaLive(w)) live = true;
        }
        bool queuedAll = c->cur > i || (c->cur == i && c->off >= g->frames);
        u8 st = g->mirror->status;
        if (!g->looping && queuedAll && !live && (started || st == NDSP_WBUF_PLAYING)) st = NDSP_WBUF_DONE;
        else if (started && st == NDSP_WBUF_QUEUED) st = NDSP_WBUF_PLAYING;
        g->mirror->status = st;
    }
}

UTA_LATE static void utaChnReset(int id) {
    if (!utaActive || id < 0 || id >= UTA_CHANNELS) {
        UTA_PHYS_RESET(utaPhys(id));
        return;
    }
    LightLock_Lock(&utaLock);
    int p = utaMap[id];
    if (utaChan[p].state == UTA_TAILING || utaChan[p].state == UTA_PENDING) {
        // The voice's channel is still playing the end of its last sound: take one whose voice is free, the voice's
        // own number first, and give that voice this channel.
        int q = -1;
        for (int i = 0; i < UTA_CHANNELS && q < 0; i++) {
            int c = (id + i) % UTA_CHANNELS;
            int owner = utaInv[c];
            if (c != p && utaChan[c].state != UTA_TAILING && utaChan[c].state != UTA_PENDING && owner != id &&
                utaCa->voices[owner].kind == VOICE_FREE && !utaStreamOn(owner))
                q = c;
        }
        if (q >= 0) {
            int other = utaInv[q];
            utaMap[id] = (int8_t) q;
            utaInv[q] = (int8_t) id;
            utaMap[other] = (int8_t) p;
            utaInv[p] = (int8_t) other;
            utaMapped = true;
            utaMoves++;
            p = q;
        } else {
            utaYields++; // every channel is busy: the tail gives its own up (the reset cuts it)
        }
    }
    UtaChan* c = &utaChan[p];
    memset(c, 0, sizeof(UtaChan));
    utaForget(p, c);
    UTA_PHYS_RESET(p);
    LightLock_Unlock(&utaLock);
}

UTA_LATE static void utaChnSetInterp(int id, ndspInterpType type) {
    UTA_PHYS_INTERP(utaPhys(id), type);
}

UTA_LATE static void utaChnSetFormat(int id, u16 format) {
#ifdef CTRPORT_ADPCM_MUSIC
    if (mmControl(id, 4, NULL)) return;
#endif
#line 3642
    UTA_PHYS_FORMAT(utaPhys(id), format);
}

UTA_LATE static void utaChnSetRate(int id, float rate) {
#ifdef CTRPORT_ADPCM_MUSIC
    if (mmControl(id, 1, &rate)) return;
#endif
#line 3646
    int p = utaPhys(id);
#if defined(CTRPORT_ADPCM_SFX) || defined(CTRPORT_ADPCM_MUSIC) || defined(CTRPORT_ADPCM_SFXPACK)
    adSfxRate(p, rate);
#endif
#line 3637
    if (p >= 0 && p < UTA_CHANNELS) utaChan[p].rate = rate;
    UTA_PHYS_RATE(p, rate);
}

UTA_LATE static void utaChnSetMix(int id, float mix[12]) {
#ifdef CTRPORT_ADPCM_MUSIC
    if (mmControl(id, 0, mix)) return;
#endif
#line 3642
    UTA_PHYS_MIX(utaPhys(id), mix);
}

UTA_LATE static void utaChnSetPaused(int id, bool paused) {
#ifdef CTRPORT_ADPCM_MUSIC
    if (mmControl(id, 2, &paused)) return;
#endif
#line 3646
    int p = utaPhys(id);
    if (p >= 0 && p < UTA_CHANNELS) utaChan[p].paused = paused;
    UTA_PHYS_PAUSED(p, paused);
}

// A sound effect this layer feeds is not cut here: the next frame callback ends it with its tail (utaStop). The voice is
// free for the game at once, as before, and its buffers are no longer written.
UTA_LATE static void utaChnWaveBufClear(int id) {
#ifdef CTRPORT_ADPCM_MUSIC
    if (mmControl(id, 3, NULL)) return;
#endif
#line 3654
    int p = utaPhys(id);
    if (utaActive && id >= 0 && id < UTA_CHANNELS && p >= 0 && p < UTA_CHANNELS) {
        LightLock_Lock(&utaLock);
        UtaChan* c = &utaChan[p];
        if (c->state == UTA_FEED || c->state == UTA_PENDING) {
#if defined(CTRPORT_ADPCM_SFX) || defined(CTRPORT_ADPCM_MUSIC) || defined(CTRPORT_ADPCM_SFXPACK)
            adSfxCaptureTail(p, c);
#endif
#line 3669
            for (int i = 0; i < 2; i++) c->seg[i].mirror = NULL;
            if (c->state == UTA_FEED && !c->paused) c->state = UTA_PENDING;
            else utaClearPhys(p, c);
            LightLock_Unlock(&utaLock);
            utaMaybeLog();
            return;
        }
        if (c->state == UTA_IDLE) utaForget(p, c);
        LightLock_Unlock(&utaLock);
    }
    UTA_PHYS_CLEAR(p);
}

// Whether a cached sound's loop seam is smoothed, per channel; measured once per cached copy. Game thread.
UTA_LATE static const UtaSeamSound* utaSeamSound(const Voice* v) {
    const CachedSfx* s = v->sfx;
#line 3685
    int chans = s->format & 3;
    if (((s->format >> 2) & 3) != 1 || chans < 1 || chans > 2 || s->frames < 4 * UTA_TAIL_MAX) return NULL;
    if (v->soundIndex < 0 || (uint32_t) v->soundIndex >= utaCa->soundCount) return NULL;
    for (int i = 0; i < UTA_SEAM_SOUNDS; i++) {
        UtaSeamSound* e = &utaSeamSounds[i];
        if (e->sound == v->soundIndex && e->pcm == s->pcm && e->frames == s->frames && e->format == s->format) return e;
    }
    const int16_t* x = (const int16_t*) s->pcm;
    int32_t maxStep[2] = { 0, 0 }, step[2] = { 0, 0 };
    UtaSeamSound* e = &utaSeamSounds[utaSeamNext++ % UTA_SEAM_SOUNDS];
    e->sound = v->soundIndex;
    e->pcm = s->pcm;
    e->frames = s->frames;
    e->format = s->format;
    bool any = false;
    for (int c = 0; c < 2; c++) {
        e->trigger[c] = 0;
        if (c >= chans) continue;
        int32_t m = 0, prev = x[c];
        for (uint32_t i = 1; i < s->frames; i++) {
            int32_t cur = x[i * (uint32_t) chans + (uint32_t) c], d = cur - prev;
            if (d < 0) d = -d;
            if (d > m) m = d;
            prev = cur;
        }
        maxStep[c] = m;
        step[c] = abs((int32_t) x[c] - (int32_t) x[(s->frames - 1) * (uint32_t) chans + (uint32_t) c]);
        e->trigger[c] = step[c] > 0 && step[c] >= UTA_SEAM_RATIO * m;
        if (e->trigger[c]) any = true;
    }
    if (any) {
        utaPost(utaCa, utaSeamFormat, utaCa->base.audioGroups[0]->sond.sounds[v->soundIndex].name, utaCached,
                    (long) step[0], (long) maxStep[0], (long) step[1], (long) maxStep[1], utaSmoothed);
    }
    return e;
}

UTA_LATE static void utaChnWaveBufAdd(int id, ndspWaveBuf* buf) {
    int p = utaPhys(id);
    Voice* v = utaActive && id >= 0 && id < UTA_CHANNELS ? &utaCa->voices[id] : NULL;
    if (v == NULL || v->kind != VOICE_SFX || v->sfx == NULL || (buf != &v->wb[0] && buf != &v->wb[1]) || p < 0 ||
        p >= UTA_CHANNELS) {
        UTA_PHYS_ADD(p, buf);
        return;
    }
    int chans = v->sfx->format & 3, encoding = (v->sfx->format >> 2) & 3;
    // Measured before the lock: the first scan of a sound reads all of it, and the frame callback waits on the lock.
#if defined(CTRPORT_ADPCM_SFX) || defined(CTRPORT_ADPCM_MUSIC) || defined(CTRPORT_ADPCM_SFXPACK)
    const UtaSeamSound* seam = NULL;
    if (buf->looping && utaSeamPcm != NULL) {
        seam = adSfxSeam(v);
        if (!seam) seam = utaSeamSound(v);
    }
#else
    const UtaSeamSound* seam = buf->looping && utaSeamPcm != NULL ? utaSeamSound(v) : NULL;
#endif
#line 3733
    LightLock_Lock(&utaLock);
    UtaChan* c = &utaChan[p];
    if (c->state == UTA_PENDING || c->state == UTA_TAILING) {
        // Cleared and refilled without a reset: a seek (set_track_position), not a stop. Cut as before.
        utaClearPhys(p, c);
    }
    if (c->state == UTA_IDLE && c->segs == 0) {
        if (chans < 1 || chans > 2 || encoding > 1 || utaTailPcm == NULL || utaSeamPcm == NULL) {
            LightLock_Unlock(&utaLock);
            UTA_PHYS_ADD(p, buf); // not fed here: played and cut as before
            return;
        }
        c->state = UTA_FEED;
        c->cur = 0;
        c->off = 0;
        c->started = 0;
        c->splice = 0;
        c->seamBuilt = 0;
        c->chans = (uint8_t) chans;
        c->bytes = (uint8_t) (chans * (encoding == 1 ? 2 : 1));
        c->trigger[0] = c->trigger[1] = 0;
        if (c->rate <= 0.0f) c->rate = (float) v->sfx->sampleRate;
    }
    if (c->state != UTA_FEED || c->segs >= 2) {
        LightLock_Unlock(&utaLock);
        UTA_PHYS_ADD(p, buf);
        return;
    }
    UtaSeg* g = &c->seg[c->segs];
#if defined(CTRPORT_ADPCM_SFX) || defined(CTRPORT_ADPCM_MUSIC) || defined(CTRPORT_ADPCM_SFXPACK)
    adSfxSegment(p, c, v, buf);
#endif
#line 3762
    g->data = (const uint8_t*) buf->data_vaddr;
    g->frames = buf->nsamples;
    g->looping = buf->looping;
    g->mirror = buf;
    if (++c->nextSeq == 0) c->nextSeq = 1;
    g->seq = c->nextSeq;
    if (seam != NULL && buf->looping) {
        c->trigger[0] = seam->trigger[0];
        c->trigger[1] = seam->trigger[1];
    }
    buf->status = NDSP_WBUF_QUEUED;
    buf->sequence_id = g->seq;
    buf->next = NULL;
    c->segs++;
    utaFeed(p, c);
    LightLock_Unlock(&utaLock);
}

// The engine asks which of its buffers plays and where: the piece the DSP plays, mapped back to its segment.
UTA_LATE static int utaPlayingPiece(int p) {
    u16 seq = UTA_PHYS_SEQ(p);
    for (int k = 0; k < UTA_RING; k++) {
        if (utaLive(&utaWb[p][k]) && utaWb[p][k].sequence_id == seq) return k;
    }
    return -1;
}

UTA_LATE static u16 utaChnGetWaveBufSeq(int id) {
    int p = utaPhys(id);
    if (!utaActive || p < 0 || p >= UTA_CHANNELS) return UTA_PHYS_SEQ(p);
    LightLock_Lock(&utaLock);
    UtaChan* c = &utaChan[p];
    u16 seq;
    if (c->state == UTA_IDLE) {
        seq = UTA_PHYS_SEQ(p);
    } else {
        int k = utaPlayingPiece(p);
        seq = k < 0 || c->piece[k].kind == UTA_PIECE_TAIL ? 0 : c->seg[c->piece[k].seg].seq;
    }
    LightLock_Unlock(&utaLock);
    return seq;
}

UTA_LATE static u32 utaChnGetSamplePos(int id) {
    int p = utaPhys(id);
    if (!utaActive || p < 0 || p >= UTA_CHANNELS) return UTA_PHYS_POS(p);
    LightLock_Lock(&utaLock);
    UtaChan* c = &utaChan[p];
    u32 pos;
    if (c->state == UTA_IDLE) {
        pos = UTA_PHYS_POS(p);
    } else {
        int k = utaPlayingPiece(p);
        pos = k < 0 || c->piece[k].kind == UTA_PIECE_TAIL ? 0 : c->piece[k].off + UTA_PHYS_POS(p);
    }
    LightLock_Unlock(&utaLock);
    return pos;
}

// NDSP thread: the stop the game made since the last callback. Nothing more of the sound is queued; what is queued
// plays, and then its continuation for 3 ms, faded out, on the same voice.
UTA_LATE static void utaStop(int p, UtaChan* c, uint32_t cb) {
    if (!c->started) {
        utaClearPhys(p, c); // not started on the DSP yet: none of it was heard
        utaUnstarted++;
        return;
    }
#ifdef CTR_AUDIO_CAPTURE_WRAP_H
    if (utaProbe++ & 1) {
        CtrCap_noteTail(p, 0); // (0 frames: a plain cut)
        utaClearPhys(p, c); // diagnosis: every other stop a plain cut, made here, as the old cut was
        utaHard++;
        return;
    }
#endif
    uint32_t n = (uint32_t) (c->rate * UTA_TAIL_SECONDS + 0.5f);
    if (n < 8) n = 8;
    if (n > UTA_TAIL_MAX) n = UTA_TAIL_MAX;
    uint8_t* dst = utaTailPcm + (uint32_t) p * UTA_TAIL_MAX * 4;
    bool pcm16 = c->bytes == 2 * c->chans;
    bool any = false;
    float seamOff[2] = { 0.0f, 0.0f };
    uint32_t seamN = 0;
    for (uint32_t j = 0; j < n; j++) {
        float w = 0.5f * (1.0f + cosf(3.14159265f * ((float) j + 0.5f) / (float) n));
        const uint8_t* src = NULL;
        float add[2] = { 0.0f, 0.0f };
        if (utaAdvance(c)) {
            const UtaSeg* g = &c->seg[c->cur];
            if (utaSeamAt(c, g)) seamN = utaSeamOffsets(c, g, seamOff);
            else if (c->splice) seamN = 0;
            c->splice = 0;
            if (c->off < seamN) {
                float sw = utaSeamWeight(c->off, seamN);
                add[0] = seamOff[0] * sw;
                add[1] = seamOff[1] * sw;
            }
#if defined(CTRPORT_ADPCM_SFX) || defined(CTRPORT_ADPCM_MUSIC) || defined(CTRPORT_ADPCM_SFXPACK)
            src = adSfxTailSample(p, c, g, j);
#else
            src = g->data + c->off * c->bytes;
#endif
#line 3860
            c->off++;
            any = true;
        }
        for (int ch = 0; ch < c->chans; ch++) {
            if (pcm16) {
                float v = src != NULL ? ((float) ((const int16_t*) src)[ch] + add[ch]) * w : 0.0f;
                ((int16_t*) dst)[j * c->chans + ch] = utaClamp16(v);
            } else {
                float v = src != NULL ? (float) ((const int8_t*) src)[ch] * w : 0.0f;
                ((int8_t*) dst)[j * c->chans + ch] = (int8_t) (v < 0.0f ? v - 0.5f : v + 0.5f);
            }
        }
    }
    c->state = UTA_TAILING;
    double step = (double) c->rate / UTA_DSP_RATE;
    c->tailEnd = cb + (uint32_t) ((double) (utaUnplayed(p) + n) / step / 160.0) + 4;
    if (any) {
        DSP_FlushDataCache(dst, n * c->bytes);
        utaQueue(p, c, dst, n, 0, 0, UTA_PIECE_TAIL);
        utaTails++;
#ifdef CTR_AUDIO_CAPTURE_WRAP_H
        CtrCap_noteTail(p, n);
#endif
    }
}

UTA_LATE static void utaFrameCallback(void* data) {
    u64 t0 = svcGetSystemTick();
    if (utaActive) {
        LightLock_Lock(&utaLock);
        unsigned busy = 0, held = 0;
        uint32_t cb = ++utaCb; // frame callbacks so far (not ndspGetFrameCount: libctru's, not linked in main)
        for (int p = 0; p < UTA_CHANNELS; p++) {
            UtaChan* c = &utaChan[p];
            if (c->state == UTA_IDLE) {
                if (utaCa->voices[utaInv[p]].kind != VOICE_FREE) busy++;
                continue;
            }
            int live = utaPendingPieces(p);
            if (!c->started) {
                for (int k = 0; k < UTA_RING && !c->started; k++) {
                    u8 st = utaWb[p][k].status;
                    if (st == NDSP_WBUF_PLAYING || st == NDSP_WBUF_DONE) c->started = 1;
                }
            }
            if (c->state == UTA_FEED) {
                utaMirror(p, c);
                if (c->started && live == 0 && !c->paused) {
                    UtaSeg* g = &c->seg[c->cur];
                    bool more = c->off < g->frames || g->looping || c->cur + 1 < c->segs;
                    if (more) utaStarved++; // the DSP ran out: a gap (counted, and fed again at once)
                    else utaForget(p, c); // it played to its end (its buffers are marked done above)
                }
                utaFeed(p, c);
            } else if (c->state == UTA_PENDING) {
                utaStop(p, c, cb);
            } else if (c->state == UTA_TAILING && (live == 0 || (int32_t) (cb - c->tailEnd) >= 0)) {
                if (live > 0) utaClearPhys(p, c);
                else utaForget(p, c);
            }
            // In use: the engine's voice on it is live, or it holds a tail. (Azahar keeps reporting a channel playing
            // after its last buffer, so ndspChnIsPlaying can't tell.)
            if (c->state == UTA_TAILING || c->state == UTA_PENDING) held++;
            if (c->state == UTA_TAILING || c->state == UTA_PENDING || utaCa->voices[utaInv[p]].kind != VOICE_FREE) busy++;
        }
        if (busy > utaPeak) utaPeak = (uint8_t) busy;
        if (held > utaPeakHeld) utaPeakHeld = (uint8_t) held;
        u64 dt = svcGetSystemTick() - t0; // this layer's cost, without the engine's callback
        utaCbTicks += dt;
        utaCbCount++;
        if (dt > utaCbMaxTicks) utaCbMaxTicks = dt;
        LightLock_Unlock(&utaLock);
    }
#ifdef CTRPORT_ADPCM_MUSIC
    mmFrame();
#endif
#line 3933
    utaEngineCallback(data);
}

UTA_LATE static void utaSetCallback(ndspCallback callback, void* data) {
    if (callback == NULL) {
        // Shutting down: no more tails; the channels are still reset through the mapping, which covers all of them.
        utaActive = false;
        UTA_PHYS_CALLBACK(NULL, NULL);
        return;
    }
    utaCa = (CtrAudioSystem*) data;
    utaPost = postMessage;
    if (!utaMapped) {
        for (int i = 0; i < UTA_CHANNELS; i++) {
            utaMap[i] = (int8_t) i;
            utaInv[i] = (int8_t) i;
        }
    }
    LightLock_Init(&utaLock);
    memset(utaChan, 0, sizeof(utaChan));
    memset(utaWb, 0, sizeof(utaWb));
    if (utaTailPcm == NULL) utaTailPcm = (uint8_t*) linearAlloc(UTA_CHANNELS * UTA_TAIL_MAX * 4);
    if (utaSeamPcm == NULL) utaSeamPcm = (uint8_t*) linearAlloc(UTA_CHANNELS * UTA_SEAM_MAX * 4);
    utaEngineCallback = callback;
    utaActive = true;
    UTA_PHYS_CALLBACK(utaFrameCallback, data);
}

// The loop seam of the stream whose decoder this is, or NULL for any other decoder (sounds decoded into the cache).
UTA_LATE static UtaSeam* utaSeamFor(CtrDecoder* dec, int* index) {
#line 3963
    if (utaCa == NULL) return NULL;
    uintptr_t base = (uintptr_t) &utaCa->streams[0], d = (uintptr_t) dec;
    if (d < base || d >= base + sizeof(utaCa->streams)) return NULL;
    uintptr_t off = d - base;
    if (off % sizeof(StreamContext) != offsetof(StreamContext, decoder)) return NULL;
    *index = (int) (off / sizeof(StreamContext));
    return &utaSeams[*index];
}

UTA_LATE static bool utaDecOpenFile(CtrDecoder* dec, const char* path) {
#line 3973
    int i;
    UtaSeam* s = utaSeamFor(dec, &i);
    if (s != NULL) {
        memset(s, 0, sizeof(*s));
        s->fromZero = true;
    }
#ifdef CTRPORT_ADPCM_MUSIC
    if (mmDecOpen(dec, path)) return true;
#endif
#line 3979
    return UTA_PHYS_OPEN_FILE(dec, path);
}

UTA_LATE static bool utaDecOpenMemory(CtrDecoder* dec, const uint8_t* data, uint32_t size) {
    int i;
    UtaSeam* s = utaSeamFor(dec, &i);
    if (s != NULL) {
        memset(s, 0, sizeof(*s));
        s->fromZero = true;
    }
    return UTA_PHYS_OPEN_MEMORY(dec, data, size);
}

UTA_LATE static bool utaDecSeek(CtrDecoder* dec, uint64_t frame) {
#line 3993
    int i;
    UtaSeam* s = utaSeamFor(dec, &i);
    if (s != NULL) {
        bool loop = frame == 0 && s->atEnd;
        s->splice = loop;
        s->fromZero = frame == 0;
        s->atEnd = false;
        if (!loop) s->remain = 0;
    }
#ifdef CTRPORT_ADPCM_MUSIC
    bool result;
    if (mmDecSeek(dec, frame, &result)) return result;
#endif
#line 4002
    return UTA_PHYS_SEEK(dec, frame);
}

UTA_LATE static int32_t utaDecRead(CtrDecoder* dec, int16_t* out, uint32_t maxFrames) {
#ifdef CTRPORT_ADPCM_MUSIC
    int32_t got;
    if (!mmDecRead(dec, out, maxFrames, &got)) got = UTA_PHYS_READ(dec, out, maxFrames);
#else
    int32_t got = UTA_PHYS_READ(dec, out, maxFrames);
#endif
#line 4007
    int idx = 0;
    UtaSeam* s = utaSeamFor(dec, &idx);
    if (s == NULL || !utaCa->streams[idx].loop) return got;
    if (got == 0) {
        s->atEnd = true;
        if (s->fromZero) s->measured = true;
        return got;
    }
    uint32_t chans = dec->channels;
    if (got < 0 || chans == 0 || chans > 2) return got;
    uint32_t n = (uint32_t) got;
    bool measuring = s->fromZero && !s->measured;
    if (s->splice) {
        s->splice = false;
        if (s->measured && s->have >= 2) {
            // The step a sample-exact loop makes here, against the file's own largest; a step that big is the file's
            // end not meeting its start. Continue the end's slope instead, and let the difference decay over ~3 ms.
            int32_t step[2] = { 0, 0 };
            bool any = false;
            for (uint32_t c = 0; c < chans; c++) {
                step[c] = abs((int32_t) out[c] - (int32_t) s->last[c]);
                s->offset[c] = 0.0f;
                if (step[c] > 0 && step[c] >= UTA_SEAM_RATIO * s->maxStep[c]) {
                    s->offset[c] = (float) (2 * (int32_t) s->last[c] - (int32_t) s->prev[c] - (int32_t) out[c]);
                    any = true;
                }
            }
            if (any) s->remain = s->total = dec->sampleRate * UTA_SEAM_SECONDS_X1000 / 1000;
            if (!s->logged) {
                s->logged = true;
                utaPost(utaCa, utaSeamFormat, utaCa->streams[idx].name, utaNone, (long) step[0], (long) s->maxStep[0], (long) step[1],
                            (long) s->maxStep[1], any ? utaSmoothed : utaKept);
            }
        }
    } else if (measuring && s->have > 0) {
        for (uint32_t c = 0; c < chans; c++) {
            int32_t d = abs((int32_t) out[c] - (int32_t) s->last[c]);
            if (d > s->maxStep[c]) s->maxStep[c] = d;
        }
    }
    if (measuring) {
        for (uint32_t c = 0; c < chans; c++) {
            int32_t m = s->maxStep[c];
            for (uint32_t j = 1; j < n; j++) {
                int32_t d = abs((int32_t) out[j * chans + c] - (int32_t) out[(j - 1) * chans + c]);
                if (d > m) m = d;
            }
            s->maxStep[c] = m;
        }
    }
    for (uint32_t c = 0; c < chans; c++) {
        s->prev[c] = n >= 2 ? out[(n - 2) * chans + c] : s->last[c];
        s->last[c] = out[(n - 1) * chans + c];
    }
    s->have = s->have + n >= 2 ? 2 : (uint8_t) (s->have + n);
    if (s->remain > 0) {
        uint32_t k = n < s->remain ? n : s->remain;
        for (uint32_t j = 0; j < k; j++) {
            float w = 0.5f * (1.0f + cosf(3.14159265f * (float) (s->total - s->remain + j) / (float) s->total));
            for (uint32_t c = 0; c < chans; c++) {
                float v = (float) out[j * chans + c] + s->offset[c] * w;
                int32_t x = (int32_t) (v < 0.0f ? v - 0.5f : v + 0.5f);
                out[j * chans + c] = (int16_t) (x > 32767 ? 32767 : x < -32768 ? -32768 : x);
            }
        }
        s->remain -= k;
    }
    return got;
}

// ===[ utstall5: a silent play's length from the WAV header (UNDERTALE) ]===
//
// Without the DSP a play is only a clock, so it needs the sound's length (soundLength). For a small embedded sound
// (under AUDO_SECTION_MIN_BYTES, not in memory, its group read lazily from its file) soundLength read the whole blob
// (loadAudoBlob) to probe it from memory. utsWavHead runs the same walk as CtrDecoder_probeMemory on that blob (sniff,
// openCommon, wavOpen) over its first UTS_HEAD_BYTES, read through the same descriptor, and gives the same length. It
// answers only when every byte the walk reads is in those first bytes and the walk succeeds; for anything else
// (Ogg Vorbis, a header past them, a failed walk, a read error) soundLength runs as before.
#if defined(ENABLE_WAD16)
#define UTS_HEAD_BYTES 512u
// Four bytes as a little-endian word: the chunk ids compare as numbers, so this adds no string to the object.
#define UTS_LE32(p) ((uint32_t) (p)[0] | ((uint32_t) (p)[1] << 8) | ((uint32_t) (p)[2] << 16) | ((uint32_t) (p)[3] << 24))

// 1 with the frames and rate the decoder would find; 0 when soundLength must decide.
UTS_LATE static int utsWavHead(const uint8_t* head, uint32_t have, uint32_t size, uint64_t* frames, uint32_t* rate) {
    if (size < 12u || have < 12u || UTS_LE32(head) != 0x46464952u || UTS_LE32(head + 8) != 0x45564157u) return 0;
    uint32_t pos = 12u;
    uint16_t formatTag = 0, channels = 0, bits = 0;
    uint32_t sampleRate = 0;
    bool haveFormat = false;
    for (;;) {
        if (pos > have || have - pos < 8u) return 0;
        uint32_t id = UTS_LE32(head + pos);
        uint32_t length = UTS_LE32(head + pos + 4);
        pos += 8u;
        long offset;
        if (id == 0x20746d66u && length >= 16) {
            if (have - pos < 16u) return 0;
            const uint8_t* fmt = head + pos;
            formatTag = (uint16_t) (fmt[0] | (fmt[1] << 8));
            channels = (uint16_t) (fmt[2] | (fmt[3] << 8));
            sampleRate = UTS_LE32(fmt + 4);
            bits = (uint16_t) (fmt[14] | (fmt[15] << 8));
            haveFormat = true;
            pos += 16u;
            offset = (long) (length - 16 + (length & 1));
        } else if (id == 0x61746164u) {
            uint32_t dataBytes = length;
            if (dataBytes > size - pos) dataBytes = size - pos;
            if (!haveFormat || formatTag != 1 || (channels != 1 && channels != 2) || (bits != 8 && bits != 16) || sampleRate == 0) return 0;
            *frames = dataBytes / (channels * (bits / 8u));
            *rate = sampleRate;
            return 1;
        } else {
            offset = (long) (length + (length & 1));
        }
        // srcSeek on memory: the walk fails past the end, so soundLength decides.
        long target = (long) pos + offset;
        if (target < 0 || target > (long) size) return 0;
        pos = (uint32_t) target;
    }
}

UTS_LATE static float utsSilentLength(CtrAudioSystem* ca, int32_t soundIndex) {
    if (soundIndex < 0 || (uint32_t) soundIndex >= ca->soundCount || ca->soundLengths[soundIndex] >= 0.0f ||
        ca->sfxCache[soundIndex].pcm != NULL) {
        return soundLength(ca, soundIndex);
    }
    const Sound* sound = &ca->base.audioGroups[0]->sond.sounds[soundIndex];
    if (!soundIsInAudo(sound) || sound->audioGroup < 0 || sound->audioGroup >= (int32_t) arrlen(ca->base.audioGroups)) {
        return soundLength(ca, soundIndex);
    }
    DataWin* group = ca->base.audioGroups[sound->audioGroup];
    if (group->audo.count == 0 || sound->audioFile < 0 || (uint32_t) sound->audioFile >= group->audo.count) {
        return soundLength(ca, soundIndex);
    }
    AudioEntry* entry = &group->audo.entries[sound->audioFile];
    // soundLength's loadAudoBlob path through the descriptor, and only that one.
    if (entry->data != NULL || !entry->present || entry->dataSize == 0 || entry->dataSize >= AUDO_SECTION_MIN_BYTES ||
        group->lazyLoadFile == NULL || group->mappedFile != NULL) {
        return soundLength(ca, soundIndex);
    }
    int fd = fileno(group->lazyLoadFile);
    off_t saved = fd >= 0 ? lseek(fd, 0, SEEK_CUR) : -1;
    if (saved < 0) return soundLength(ca, soundIndex);
    uint32_t header = entry->sizeIsUpperBound ? 4 : 0;
    uint32_t want = (entry->dataSize < UTS_HEAD_BYTES ? entry->dataSize : UTS_HEAD_BYTES) + header;
    uint8_t buffer[UTS_HEAD_BYTES + 4];
    off_t start = (off_t) (entry->dataOffset - header);
    bool ok = lseek(fd, start, SEEK_SET) == start;
    uint32_t done = 0;
    while (ok && done < want) {
        ssize_t n = read(fd, buffer + done, want - done);
        if (n <= 0) ok = false;
        else done += (uint32_t) n;
    }
    lseek(fd, saved, SEEK_SET);
    if (!ok) return soundLength(ca, soundIndex);
    // The size loadAudoBlob would have settled on, with the same change to the entry it makes.
    uint32_t size = entry->dataSize;
    if (header > 0) {
        uint32_t exact;
        memcpy(&exact, buffer, sizeof(exact));
        if (exact <= size) size = exact;
    }
    uint32_t have = want - header < size ? want - header : size;
    uint64_t frames = 0;
    uint32_t rate = 0;
    if (size == 0 || utsWavHead(buffer + header, have, size, &frames, &rate) == 0) return soundLength(ca, soundIndex);
    if (header > 0) {
        entry->dataSize = size;
        entry->sizeIsUpperBound = false;
    }
    float seconds = (float) frames / (float) rate;
    ca->soundLengths[soundIndex] = seconds;
    return seconds;
}
#endif

#ifdef ENABLE_WAD16
// ===[ utsfx: a planned sound's copy before it plays (z_utsfx.c) ]===
//
// A WAV's first play copies it into linear memory (buildSfx) on the game thread, 6-11 ms for 150-425 KB on the Old
// models, in the middle of a fight. The sound plan names the sounds a scene plays; at the room change, in the frame
// that isn't shown, z_utsfx.c asks for each here. The copy is the one the first play would make (prepareAudoSource:
// the same read, the same samples), made earlier, so the play finds it cached and starts at once. A preload never
// pushes another sound out of the cache and keeps floorBytes of linear memory free. The copy is the first to evict
// under the cache cap (never played) and is kept from the texture cache's shedding as long as a sound just played is,
// so idle tiles make way for it, as for a first play's copy. Without the DSP (silent runs) nothing is done.
// Late code (platform/3ds/late_reserve.ld) at the end of the file: nothing before it moves.
#define UTSFX_CODE __attribute__((noinline, section(".text.late.utsfx")))
#define UTSFX_DATA __attribute__((section(".bss.late.utsfx")))

// ---[ The copy worker ]---
//
// What the room change's frame has no time for, a thread of its own copies in the background: it reads the WAV
// through its own descriptor in pieces, makes buildSfx's copy (the same bytes: identical stereo channels kept as mono,
// 8-bit made signed) in linear memory and hands it back; the game thread puts it in the cache (an outer vtable's
// update and playSound, and the next room change), as the first play would have. A play that comes first takes
// today's path, and the copy that comes after it is dropped. Nothing is ever pushed out of the cache for one, and the
// worker keeps the floor of linear memory free. The thread never touches the audio system: a job carries all it needs.
#define UTSFX_JOBS 32
#define UTSFX_PIECE (32u * 1024u) // a multiple of every frame size (1, 2, 4)
#define UTSFX_HEAD 512u           // the WAV header must lie in the first read
#define UTSFX_STACK (16u * 1024u)
#define UTSFX_PATH 256

enum { UTSFX_JOB_FREE, UTSFX_JOB_QUEUED, UTSFX_JOB_BUSY, UTSFX_JOB_DONE, UTSFX_JOB_FAILED };

typedef struct {
    volatile uint8_t state;
    uint8_t sourceChannels;
    uint16_t format;
    int32_t soundIndex;
    uint32_t seq;
    // In: where the blob lies, as requestLoad has it, and the plan's size of the copy (KiB).
    uint32_t offset, size, header, kib;
    // Out.
    uint8_t* pcm;
    uint32_t bytes, frames, sampleRate, exactSize;
    u64 ticks;
} UtSfxJob;

UTSFX_DATA static struct {
    UtSfxJob jobs[UTSFX_JOBS];
    LightLock lock;
    LightEvent event;
    Thread thread;
    bool started, startFailed;
    volatile bool held; // queued in the room change's frame: the worker waits for the next frame (after the room's texture prefetch)
    uint32_t seq;
    uint32_t floorBytes;
    uint8_t* piece; // linear memory, the worker's
    char path[UTSFX_PATH];
    CtrAudioSystem* wrapped;
    const AudioSystemVtable* inner;
    AudioSystemVtable outer;
    uint32_t installed, installedKb, dropped, failed;
    u64 workTicks;
} usw; UTSFX_DATA static volatile uint32_t usDoneCount; UTSFX_DATA static uint32_t usDoneSeen; static void usFastCheck(void);

UTSFX_CODE static bool usReadAt(int fd, uint32_t at, uint8_t* out, uint32_t n) {
    if (lseek(fd, (off_t) at, SEEK_SET) != (off_t) at) return false;
    uint32_t done = 0;
    while (n > done) {
        ssize_t got = read(fd, out + done, n - done);
        if (0 >= got) return false;
        done += (uint32_t) got;
    }
    return true;
}

UTSFX_CODE static uint32_t usLe32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

// Worker: the job's copy, exactly as prepareAudoSource's buildSfx makes it from the blob loadAudoBlob reads, or false
// (then the game's own path does whatever it does today). The WAV parse is ctr_audio_decoder.c's wavOpen on a blob of
// the entry's exact size, within the bytes read here.
UTSFX_CODE static bool usCopy(int fd, UtSfxJob* job) {
    uint8_t* buf = usw.piece;
    uint32_t size = job->size;
    uint32_t head = job->header + size;
    if (head > UTSFX_HEAD) head = UTSFX_HEAD;
    if (job->header > job->offset || !usReadAt(fd, job->offset - job->header, buf, head)) return false;
    if (job->header > 0) {
        uint32_t exact = usLe32(buf);
        if (size >= exact) size = exact;
    }
    if (size == 0) return false;
    const uint8_t* b = buf + job->header;
    uint32_t avail = head - job->header;
    if (avail > size) avail = size;
    // wavOpen: srcRead fails short of the blob's end, srcSeek past it.
    // Tags as numbers: no string constants in .rodata (its reserve on the Old models is nearly used up).
    if (12 > avail || usLe32(b) != 0x46464952u || usLe32(b + 8) != 0x45564157u) return false; // the RIFF and WAVE tags
    uint32_t pos = 12;
    uint16_t formatTag = 0, channels = 0, bits = 0;
    uint32_t rate = 0, dataStart = 0, dataBytes = 0;
    bool haveFormat = false;
    for (;;) {
        if (pos + 8 > size || pos + 8 > avail) return false;
        uint32_t length = usLe32(b + pos + 4);
        bool isFmt = usLe32(b + pos) == 0x20746D66u && length >= 16; // "fmt "
        bool isData = usLe32(b + pos) == 0x61746164u;              // "data"
        pos += 8;
        if (isFmt) {
            if (pos + 16 > size || pos + 16 > avail) return false;
            formatTag = (uint16_t) (b[pos] | (b[pos + 1] << 8));
            channels = (uint16_t) (b[pos + 2] | (b[pos + 3] << 8));
            rate = usLe32(b + pos + 4);
            bits = (uint16_t) (b[pos + 14] | (b[pos + 15] << 8));
            haveFormat = true;
            pos += 16;
        }
        if (isData) {
            dataStart = pos;
            dataBytes = length;
            if (dataBytes > size - dataStart) dataBytes = size - dataStart;
            break;
        }
        // The seek: a 32-bit long offset from here, within the blob.
        int32_t skip = isFmt ? (int32_t) (length - 16u + (length & 1u)) : (int32_t) (length + (length & 1u));
        int64_t target = (int64_t) pos + skip;
        if (0 > target || target > (int64_t) size) return false;
        pos = (uint32_t) target;
    }
    if (!haveFormat || formatTag != 1 || (channels != 1 && channels != 2) || (bits != 8 && bits != 16) || rate == 0) return false;
    if (dataBytes > SFX_CACHE_MAX_ENTRY_BYTES) return false;
    uint32_t bps = bits / 8u;
    uint32_t frames = dataBytes / (channels * bps);
    if (frames == 0) return false;
    // The plan's size says which buildSfx made: a stereo WAV it stored as mono has a copy half the size. The copy
    // checks every frame and gives up if the WAV isn't what the plan says.
    bool mono = channels == 2 && job->kib == (frames * bps + 1023u) / 1024u;
    uint32_t outChannels = mono ? 1 : channels;
    uint32_t bytes = frames * outChannels * bps;
    if ((uint64_t) bytes + usw.floorBytes > linearSpaceFree()) return false;
    uint8_t* pcm = (uint8_t*) linearAlloc(bytes);
    if (pcm == NULL) return false;
    uint32_t total = frames * channels * bps;
    uint32_t done = 0, out = 0;
    bool dual = true, ok = true;
    while (ok && total > done) {
        uint32_t n = total - done > UTSFX_PIECE ? UTSFX_PIECE : total - done;
        if (!usReadAt(fd, job->offset + dataStart + done, buf, n)) {
            ok = false;
            break;
        }
        if (bps == 2) {
            if (channels == 2) {
                for (uint32_t i = 0; n > i; i += 4) {
                    bool same = buf[i] == buf[i + 2] && buf[i + 1] == buf[i + 3];
                    if (!same) dual = false;
                    if (mono) {
                        if (!same) break;
                        pcm[out++] = buf[i];
                        pcm[out++] = buf[i + 1];
                    }
                }
                if (mono && !dual) ok = false;
                if (!mono) {
                    memcpy(pcm + out, buf, n);
                    out += n;
                }
            } else {
                memcpy(pcm + out, buf, n);
                out += n;
            }
        } else if (channels == 2) {
            for (uint32_t i = 0; n > i; i += 2) {
                bool same = buf[i] == buf[i + 1];
                if (!same) dual = false;
                if (mono) {
                    if (!same) break;
                    pcm[out++] = buf[i] ^ 0x80;
                } else {
                    pcm[out++] = buf[i] ^ 0x80;
                    pcm[out++] = buf[i + 1] ^ 0x80;
                }
            }
            if (mono && !dual) ok = false;
        } else {
            for (uint32_t i = 0; n > i; i++) pcm[out++] = buf[i] ^ 0x80;
        }
        done += n;
    }
    // A stereo WAV whose channels all match is one buildSfx would have stored as mono.
    if (ok && (out != bytes || (channels == 2 && !mono && dual))) ok = false;
    if (!ok) {
        linearFree(pcm);
        return false;
    }
    DSP_FlushDataCache(pcm, bytes);
    job->pcm = pcm;
    job->bytes = bytes;
    job->frames = frames;
    job->sampleRate = rate;
    job->exactSize = size;
    job->sourceChannels = (uint8_t) channels;
    if (bps == 2) job->format = outChannels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16;
    else job->format = outChannels == 2 ? NDSP_FORMAT_STEREO_PCM8 : NDSP_FORMAT_MONO_PCM8;
    return true;
}

bool CtrMemory_allowRelief(bool allow); // ctr_memory.h: off, so a failed malloc here never takes linear memory

#ifdef CTRPORT_ADPCM_SFXPACK
#define AP_JOB 0xFFFFFFFFu // job->header of a pack job (WAV jobs: 0 or 4)
MM_CODE static bool apCopy(UtSfxJob* job);
MM_CODE static void apFinish(CtrAudioSystem* ca, UtSfxJob* job, bool done);
#endif
#line 4394
UTSFX_CODE static void usWorkerMain(void* arg) {
    (void) arg;
    CtrMemory_allowRelief(false);
    int fd = -1;
    for (;;) {
        LightEvent_Wait(&usw.event);
        for (;;) {
            UtSfxJob* job = NULL;
            LightLock_Lock(&usw.lock);
            for (int i = 0; UTSFX_JOBS > i; i++) {
                UtSfxJob* j = &usw.jobs[i];
                if (j->state == UTSFX_JOB_QUEUED && (job == NULL || job->seq > j->seq)) job = j;
            }
            if (usw.held) job = NULL;
            if (job != NULL) job->state = UTSFX_JOB_BUSY;
            LightLock_Unlock(&usw.lock);
            if (job == NULL) break;
            u64 start = svcGetSystemTick();
#ifdef CTRPORT_ADPCM_SFXPACK
            bool ok;
            if (job->header == AP_JOB) ok = apCopy(job);
            else {
                if (0 > fd) fd = open(usw.path, O_RDONLY);
                ok = fd >= 0 && usCopy(fd, job);
            }
#else
            if (0 > fd) fd = open(usw.path, O_RDONLY);
            bool ok = fd >= 0 && usCopy(fd, job);
#endif
#line 4414
            LightLock_Lock(&usw.lock);
            job->ticks = svcGetSystemTick() - start;
            job->state = ok ? UTSFX_JOB_DONE : UTSFX_JOB_FAILED; usDoneCount = usDoneCount + 1u; // usfast: one more finished
            LightLock_Unlock(&usw.lock);
        }
    }
}

#ifdef UTSFX_VERIFY
#define UTSFX_VERIFY_CONST __attribute__((aligned(4), section(".rodata.utsfx_in_text")))
static const char usVerifySame[] UTSFX_VERIFY_CONST = "utsfx verify '%s': same (%lu bytes, %lu frames, format %u)\n";
static const char usVerifyDiff[] UTSFX_VERIFY_CONST = "utsfx verify '%s': DIFFERENT (%lu bytes, %lu frames, format %u)\n";
static const char usVerifyGaveUp[] UTSFX_VERIFY_CONST = "utsfx verify '%s': the worker gave up\n";
#endif

// Game thread: puts finished copies in the cache, as buildSfx does at a first play (never played yet: the first to
// shed and evict), unless the sound got there first, is on its way by the game's own path, or doesn't fit under the
// cap without pushing something out.
UTSFX_CODE static void usInstall(void) {
    CtrAudioSystem* ca = gShutdownTarget; if (usDoneCount == usDoneSeen) { usFastCheck(); return; } // usfast: none finished
    // The worker's results are read after its lock (the worker may run on another core).
    uint32_t finished = 0;
    LightLock_Lock(&usw.lock); usDoneSeen = usDoneCount; // usfast: under the lock, so it covers every state seen below
    for (int i = 0; UTSFX_JOBS > i; i++) {
        if (usw.jobs[i].state == UTSFX_JOB_DONE || usw.jobs[i].state == UTSFX_JOB_FAILED) finished |= 1u << i;
    }
    LightLock_Unlock(&usw.lock);
    for (int i = 0; UTSFX_JOBS > i; i++) {
        if ((finished & (1u << i)) == 0) continue;
        UtSfxJob* job = &usw.jobs[i];
        uint8_t state = job->state;
        usw.workTicks += job->ticks;
#ifdef CTRPORT_ADPCM_SFXPACK
        if (job->header == AP_JOB) apFinish(ca, job, state == UTSFX_JOB_DONE);
        else
#endif
#line 4446
        if (state == UTSFX_JOB_FAILED) {
            usw.failed++;
#ifdef UTSFX_VERIFY
            if (ca != NULL && job->soundIndex >= 0 && ca->soundCount > (uint32_t) job->soundIndex) {
                logInfo(usVerifyGaveUp, ca->base.audioGroups[0]->sond.sounds[job->soundIndex].name);
            }
#endif
        } else {
            int32_t si = job->soundIndex;
            bool keep = ca != NULL && ca->base.audioGroups != NULL && si >= 0 && ca->soundCount > (uint32_t) si && ca->sfxCache[si].pcm == NULL &&
                        CTR_SFX_CACHE_BYTES >= ca->cacheBytes + job->bytes;
            AudioEntry* entry = NULL;
            if (keep) {
                const Sound* sound = &ca->base.audioGroups[0]->sond.sounds[si];
                DataWin* group = ca->base.audioGroups[sound->audioGroup];
                entry = &group->audo.entries[sound->audioFile];
                if (entry->data != NULL || entry->dataOffset != job->offset) keep = false;
                LightLock_Lock(&ca->loadLock);
                for (int k = 0; LOAD_SLOTS > k; k++) {
                    if (ca->loads[k].soundIndex == si && ca->loads[k].state != LOAD_FREE) keep = false;
                }
                for (int k = 0; PREP_SLOTS > k; k++) {
                    if (ca->preps[k].soundIndex == si && ca->preps[k].state != PREP_FREE) keep = false;
                }
                LightLock_Unlock(&ca->loadLock);
            }
#ifdef UTSFX_VERIFY
            // Gate build only (define it at the top of this section; never in a shipped build): the first play's own
            // copy (prepareAudoSource, a synchronous read here), compared with the worker's, then dropped.
            if (keep) {
                CachedSfx* c = &ca->sfxCache[si];
                CachedSfx* ref = NULL;
                const uint8_t* mem = NULL;
                uint32_t memSize = 0;
                bool ok = prepareAudoSource(ca, si, &ref, &mem, &memSize) && ref == c;
                bool same = ok && c->bytes == job->bytes && c->frames == job->frames && c->sampleRate == job->sampleRate && c->format == job->format &&
                            c->sourceChannels == job->sourceChannels && c->reload == SFX_RELOAD_AUDO_WAV && entry->dataSize == job->exactSize &&
                            !entry->sizeIsUpperBound && memcmp(c->pcm, job->pcm, job->bytes) == 0;
                logInfo(same ? usVerifySame : usVerifyDiff, ca->base.audioGroups[0]->sond.sounds[si].name, (unsigned long) job->bytes,
                        (unsigned long) job->frames, (unsigned) job->format);
                if (ok) {
                    ca->cacheBytes -= c->bytes;
                    linearFree(c->pcm);
                    c->pcm = NULL;
                    c->bytes = 0;
                }
            }
#endif
            if (keep) {
                CachedSfx* c = &ca->sfxCache[si];
                c->pcm = job->pcm;
                c->bytes = job->bytes;
                c->frames = job->frames;
                c->sampleRate = job->sampleRate;
                c->sourceChannels = job->sourceChannels;
                c->format = job->format;
                c->reload = SFX_RELOAD_AUDO_WAV;
                // Least recently used under the cap, but kept from the texture cache's shedding for as long as a
                // sound just played is (SFX_SHED_RECENT_MS): its tiles not drawn lately give way first, as they do for
                // a first play's copy.
                c->lastUse = 0;
                c->lastPlayTick = svcGetSystemTick();
                ca->cacheBytes += job->bytes;
                ca->soundLengths[si] = (float) c->frames / (float) c->sampleRate;
                // loadAudoBlob learns the exact size when it reads the blob.
                if (entry->sizeIsUpperBound && job->header > 0 && entry->dataSize >= job->exactSize) {
                    entry->dataSize = job->exactSize;
                    entry->sizeIsUpperBound = false;
                }
                usw.installed++;
                usw.installedKb += (job->bytes + 1023u) / 1024u;
            } else {
                linearFree(job->pcm);
                usw.dropped++;
            }
            job->pcm = NULL;
        }
        LightLock_Lock(&usw.lock);
        job->state = UTSFX_JOB_FREE;
        LightLock_Unlock(&usw.lock);
    }
}

UTSFX_CODE static void usUpdate(AudioSystem* audio, float deltaTime) {
    if (usw.started) {
        if (usw.held) {
            LightLock_Lock(&usw.lock);
            usw.held = false;
            LightLock_Unlock(&usw.lock);
            LightEvent_Signal(&usw.event);
        }
        usInstall();
    }
    usw.inner->update(audio, deltaTime);
}

UTSFX_CODE static int32_t usPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    if (usw.started) usInstall();
    return usw.inner->playSound(audio, soundIndex, priority, loop);
}

UTSFX_CODE static bool usJobFor(int32_t soundIndex) {
    for (int i = 0; UTSFX_JOBS > i; i++) {
        if (usw.jobs[i].state != UTSFX_JOB_FREE && usw.jobs[i].soundIndex == soundIndex) return true;
    }
    return false;
}

// At a room change, before the plan's sounds are asked for: finished copies go in, copies still waiting for the
// worker are dropped (they were the last room's).
void CtrAudioSystem_beginSfxPlan(void);
UTSFX_CODE void CtrAudioSystem_beginSfxPlan(void) {
    if (!usw.started) return;
    usInstall();
    LightLock_Lock(&usw.lock);
    for (int i = 0; UTSFX_JOBS > i; i++) {
        if (usw.jobs[i].state == UTSFX_JOB_QUEUED) usw.jobs[i].state = UTSFX_JOB_FREE;
    }
    LightLock_Unlock(&usw.lock);
}

// The worker's copies put in the cache, dropped and failed since the last call, and its time on them (ms).
void CtrAudioSystem_sfxPlanStats(uint32_t out[4], double* ms);
UTSFX_CODE void CtrAudioSystem_sfxPlanStats(uint32_t out[4], double* ms) {
    out[0] = usw.installed;
    out[1] = usw.installedKb;
    out[2] = usw.dropped;
    out[3] = usw.failed;
    *ms = TICKS_TO_MS(usw.workTicks);
    usw.installed = usw.installedKb = usw.dropped = usw.failed = 0;
    usw.workTicks = 0;
}

// Queues a planned sound's copy for the worker: 1 queued, 0 already cached or queued, -1 not now (no DSP, the game's
// own path has it, not a small embedded sound read lazily from its file, over the cap with what is queued, no worker).
// kib: the plan's size of the copy.
int CtrAudioSystem_queueSfx(int32_t soundIndex, uint32_t kib, uint32_t floorBytes);
UTSFX_CODE int CtrAudioSystem_queueSfx(int32_t soundIndex, uint32_t kib, uint32_t floorBytes) {
    CtrAudioSystem* ca = gShutdownTarget;
    if (ca == NULL || !ca->ndspReady || ca->base.audioGroups == NULL || soundIndex < 0 || (uint32_t) soundIndex >= ca->soundCount) return -1;
    if (ca->sfxCache[soundIndex].pcm != NULL) return 0;
    if (usJobFor(soundIndex)) return 0;
    const Sound* sound = &ca->base.audioGroups[0]->sond.sounds[soundIndex];
    bool isRegular = (sound->flags & AUDIO_ENTRY_FLAG_REGULAR) == AUDIO_ENTRY_FLAG_REGULAR;
    if (isRegular && (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) == 0) return -1;
    int32_t g = sound->audioGroup;
    if (g < 0 || g >= (int32_t) arrlen(ca->base.audioGroups) || g >= (int32_t) arrlen(ca->groupPaths) || ca->groupPaths[g] == NULL) return -1;
    DataWin* group = ca->base.audioGroups[g];
    if (group->lazyLoadFile == NULL || group->mappedFile != NULL || sound->audioFile < 0 || (uint32_t) sound->audioFile >= group->audo.count) return -1;
    AudioEntry* entry = &group->audo.entries[sound->audioFile];
    if (!entry->present || entry->data != NULL || entry->dataSize == 0 || entry->dataSize >= AUDO_SECTION_MIN_BYTES) return -1;
    bool busy = false;
    LightLock_Lock(&ca->loadLock);
    for (int i = 0; i < LOAD_SLOTS; i++) {
        if (ca->loads[i].soundIndex == soundIndex && ca->loads[i].state != LOAD_FREE) busy = true;
    }
    for (int i = 0; i < PREP_SLOTS; i++) {
        if (ca->preps[i].soundIndex == soundIndex && ca->preps[i].state != PREP_FREE) busy = true;
    }
    LightLock_Unlock(&ca->loadLock);
    if (busy) return -1;
    // One file for the worker (the plan's sounds are all in data.win).
    size_t length = strlen(ca->groupPaths[g]);
    if (length >= UTSFX_PATH) return -1;
    if (usw.path[0] == '\0') memcpy(usw.path, ca->groupPaths[g], length + 1);
    else if (strcmp(usw.path, ca->groupPaths[g]) != 0) return -1;
    uint64_t planned = (uint64_t) ca->cacheBytes + (uint64_t) kib * 1024u;
    int slot = -1;
    for (int i = 0; UTSFX_JOBS > i; i++) {
        uint8_t state = usw.jobs[i].state;
        if (state == UTSFX_JOB_FREE) {
            if (0 > slot) slot = i;
        } else if (state != UTSFX_JOB_FAILED) {
            planned += state == UTSFX_JOB_DONE ? usw.jobs[i].bytes : (uint64_t) usw.jobs[i].kib * 1024u;
        }
    }
    if (0 > slot || planned > CTR_SFX_CACHE_BYTES) return -1;
    if (!usw.started) {
        if (usw.startFailed) return -1;
        usw.startFailed = true;
        usw.piece = (uint8_t*) linearAlloc(UTSFX_PIECE);
        if (usw.piece == NULL) return -1;
        LightLock_Init(&usw.lock);
        LightEvent_Init(&usw.event, RESET_ONESHOT);
        // As the loader thread, one step further below: it mostly waits on the card, and a voice waiting on the
        // loader's read comes first.
        s32 priority = 0x30;
        svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        priority += 2;
        if (priority > 0x3F) priority = 0x3F;
        usw.thread = threadCreate(usWorkerMain, NULL, UTSFX_STACK, priority, ca->threadCore == 0 ? -2 : ca->threadCore, true);
        if (usw.thread == NULL) {
            linearFree(usw.piece);
            usw.piece = NULL;
            return -1;
        }
        usw.startFailed = false;
        usw.started = true;
    }
    if (usw.wrapped != ca) {
        usw.wrapped = ca;
        usw.inner = ca->base.vtable;
        usw.outer = *ca->base.vtable;
        usw.outer.update = usUpdate;
        usw.outer.playSound = usPlaySound;
        ca->base.vtable = &usw.outer;
    }
    usw.floorBytes = floorBytes;
    UtSfxJob* job = &usw.jobs[slot];
    job->soundIndex = soundIndex;
    job->seq = ++usw.seq;
    job->offset = entry->dataOffset;
    job->size = entry->dataSize;
    job->header = entry->sizeIsUpperBound ? 4 : 0;
    job->kib = kib;
    job->pcm = NULL;
    job->bytes = 0;
    LightLock_Lock(&usw.lock);
    job->state = UTSFX_JOB_QUEUED;
    usw.held = true;
    LightLock_Unlock(&usw.lock);
    return 1;
}

// 1 copied, 0 already cached, -1 not now (no DSP, being read, not a small embedded sound, no room); the plan's
// sounds are small PCM WAVs. bytes: an upper bound of the copy's size.
int CtrAudioSystem_preloadSfx(int32_t soundIndex, uint32_t bytes, uint32_t floorBytes);

UTSFX_CODE int CtrAudioSystem_preloadSfx(int32_t soundIndex, uint32_t bytes, uint32_t floorBytes) {
    CtrAudioSystem* ca = gShutdownTarget;
    if (ca == NULL || !ca->ndspReady || ca->base.audioGroups == NULL || soundIndex < 0 || (uint32_t) soundIndex >= ca->soundCount) return -1;
    if (ca->sfxCache[soundIndex].pcm != NULL) return 0;
    if (usJobFor(soundIndex)) return -1; // the worker has it
    const Sound* sound = &ca->base.audioGroups[0]->sond.sounds[soundIndex];
    bool isRegular = (sound->flags & AUDIO_ENTRY_FLAG_REGULAR) == AUDIO_ENTRY_FLAG_REGULAR;
    if (isRegular && (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) == 0) return -1;
    if (sound->audioGroup < 0 || sound->audioGroup >= (int32_t) arrlen(ca->base.audioGroups)) return -1;
    DataWin* group = ca->base.audioGroups[sound->audioGroup];
    if (sound->audioFile < 0 || (uint32_t) sound->audioFile >= group->audo.count) return -1;
    AudioEntry* entry = &group->audo.entries[sound->audioFile];
    if (!entry->present || entry->dataSize == 0 || entry->dataSize >= AUDO_SECTION_MIN_BYTES) return -1;
    if (ca->cacheBytes + bytes > CTR_SFX_CACHE_BYTES || linearSpaceFree() < bytes + floorBytes) return -1;
    // A background read or decode of it is on its way: its play takes that.
    bool busy = false;
    LightLock_Lock(&ca->loadLock);
    for (int i = 0; i < LOAD_SLOTS; i++) {
        if (ca->loads[i].soundIndex == soundIndex && ca->loads[i].state != LOAD_FREE) busy = true;
    }
    for (int i = 0; i < PREP_SLOTS; i++) {
        if (ca->preps[i].soundIndex == soundIndex && ca->preps[i].state != PREP_FREE) busy = true;
    }
    LightLock_Unlock(&ca->loadLock);
    if (busy) return -1;
    bool hadData = entry->data != NULL;
    // The game thread's own load counters (the [audio] line) count plays only.
    uint32_t loads = ca->windowCacheLoads;
    u64 loadTicks = ca->windowLoadTicks;
    CachedSfx* sfx = NULL;
    const uint8_t* mem = NULL;
    uint32_t memSize = 0;
    bool ok = prepareAudoSource(ca, soundIndex, &sfx, &mem, &memSize);
    ca->windowCacheLoads = loads;
    ca->windowLoadTicks = loadTicks;
    if (ok && sfx != NULL) {
        sfx->lastUse = 0;
        sfx->lastPlayTick = svcGetSystemTick(); // as the worker's copies (usInstall)
        return 1;
    }
    // Not copied (out of memory, or not a PCM WAV after all): the blob goes back as its play would find it.
    if (!hadData && entry->data != NULL && !blobStreaming(ca, entry->data) && group->mappedFile == NULL && group->lazyLoadAudio &&
        group->lazyLoadFile != NULL) {
        free(entry->data);
        entry->data = NULL;
    }
    return -1;
}
#endif

#ifdef CTRPORT_SPIKEREC
// spikerec (z_spikerec.c, playtest builds only): the game thread's sound load time in this stats window so far
// (logWindowStats clears it), read once a frame on the game thread.
__attribute__((noinline, section(".text.spikerec_late"))) uint64_t CtrAudioSystem_spikeLoadTicks(AudioSystem* audio) {
    return ((CtrAudioSystem*) audio)->windowLoadTicks;
}
#endif

// ===[ Sound asset gain (credfix) ]===
//
// GameMaker keeps a gain for each sound asset apart from each playing instance's own gain, and plays an instance at
// their product. Measured on PC (pcref, UNDERTALE 1.08): an asset's audio_sound_get_gain starts at its SOND volume and
// a new instance's at 1; setting either one leaves the other's reading alone; and in the Mt. Ebott picture's fade object, which sets
// its noise track's asset gain to 0.9 every step and its instance's to the flash's alpha, the captured mix's noise follows
// the alpha (silent between flashes). Before this, an asset's gain replaced its instances' gains, so that noise played
// at 0.9 all through the Mt. Ebott picture.
//
// The asset's gain is kept in its Sound's volume, which only playSoundImpl reads: a new instance starts at it, as at
// 1 x the asset's gain. Each voice's own gain is kept here; a voice's gain (Voice.gain) is the product. Streams
// (audio_create_stream) are as before. Late code (platform/3ds/late_reserve.ld) at the end of the file: nothing
// before it moves; ctrSetSoundGain and ctrGetSoundGain jump here.
#define CREDFIX_CODE __attribute__((noinline, section(".text.credfix_late")))
#define CREDFIX_BSS __attribute__((section(".bss.credfix_late")))

static float credfixVoiceGain[VOICE_COUNT] CREDFIX_BSS;
static int32_t credfixVoiceId[VOICE_COUNT] CREDFIX_BSS;

void credfixSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs);
float credfixGetSoundGain(AudioSystem* audio, int32_t soundOrInstance);

// Diagnostic (pcmhash builds, which compare builds' audio; never Release): logs the first time, per sound and kind, that
// a gain plays or reads differently from before credfix (an asset's gain replacing its instances' gains), so a run of
// the test scenes lists the scenes whose sound changed.
#ifdef CTR_AUDIO_PCM_HASH
static uint8_t credfixLogged[3][512];
static void credfixNote(CtrAudioSystem* ca, int kind, int32_t sound, float before, float now) {
    static const char* const what[3] = {"instance gain set", "asset gain set", "instance gain read"};
    if (sound < 0 || sound >= 4096 || before == now || (credfixLogged[kind][sound >> 3] & (1u << (sound & 7)))) return;
    credfixLogged[kind][sound >> 3] |= (uint8_t) (1u << (sound & 7));
    logInfo("credfix gain: %s, sound %ld %s: %.3f before, %.3f now\n", what[kind], (long) sound,
            ca->base.audioGroups[0]->sond.sounds[sound].name, (double) before, (double) now);
}
#define CREDFIX_NOTE(ca, kind, sound, before, now) credfixNote(ca, kind, sound, before, now)
#else
#define CREDFIX_NOTE(ca, kind, sound, before, now) ((void) 0)
#endif

// The asset a voice belongs to, or NULL for a stream's voice.
static Sound* CREDFIX_CODE credfixAsset(CtrAudioSystem* ca, int32_t soundIndex) {
    if (soundIndex < 0 || (uint32_t) soundIndex >= ca->soundCount) return NULL;
    return &ca->base.audioGroups[0]->sond.sounds[soundIndex];
}

// A voice's own gain: 1 until the game sets it.
static float CREDFIX_CODE credfixOwnGain(CtrAudioSystem* ca, int vi) {
    return credfixVoiceId[vi] == ca->voices[vi].instanceId ? credfixVoiceGain[vi] : 1.0f;
}

void CREDFIX_CODE credfixSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    if (soundOrInstance >= AUDIO_STREAM_INDEX_BASE) {
        StreamEntry* entry = streamEntryFor(ca, soundOrInstance);
        if (entry != NULL) entry->initialGain = gain;
        // Before GameMaker 2024.11 the gain of a playing create_stream sound
        // could not be changed (same as the miniaudio backend).
        if (!DataWin_isVersionAtLeast(audio->dw, 2024, 11, 0, 0)) return;
        for (int i = 0; i < VOICE_COUNT; i++) {
            if (voiceMatches(ca, i, soundOrInstance)) setVoiceGain(ca, i, gain, timeMs);
        }
        return;
    }
    if (isInstanceId(soundOrInstance)) {
        for (int i = 0; i < VOICE_COUNT; i++) {
            if (!voiceMatches(ca, i, soundOrInstance)) continue;
            Sound* asset = credfixAsset(ca, ca->voices[i].soundIndex);
            if (asset == NULL) {
                setVoiceGain(ca, i, gain, timeMs);
                continue;
            }
            credfixVoiceGain[i] = gain;
            credfixVoiceId[i] = ca->voices[i].instanceId;
            CREDFIX_NOTE(ca, 0, ca->voices[i].soundIndex, gain, gain * asset->volume);
            setVoiceGain(ca, i, gain * asset->volume, timeMs);
        }
        return;
    }
    Sound* asset = credfixAsset(ca, soundOrInstance);
    if (asset == NULL) return;
    asset->volume = gain;
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (!voiceMatches(ca, i, soundOrInstance)) continue;
        CREDFIX_NOTE(ca, 1, soundOrInstance, gain, credfixOwnGain(ca, i) * gain);
        setVoiceGain(ca, i, credfixOwnGain(ca, i) * gain, timeMs);
    }
}

float CREDFIX_CODE credfixGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    if (soundOrInstance >= AUDIO_STREAM_INDEX_BASE) {
        StreamEntry* entry = streamEntryFor(ca, soundOrInstance);
        return entry != NULL ? entry->initialGain : 0.0f;
    }
    if (!isInstanceId(soundOrInstance)) {
        const Sound* asset = credfixAsset(ca, soundOrInstance);
        return asset != NULL ? asset->volume : 0.0f;
    }
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (!voiceMatches(ca, i, soundOrInstance)) continue;
        const Sound* asset = credfixAsset(ca, ca->voices[i].soundIndex);
        if (asset == NULL) return ca->voices[i].gain;
        // Through a fade the voice's gain moves; its own share of it is the product over the asset's gain.
        float own = asset->volume > 0.0f ? ca->voices[i].gain / asset->volume : credfixOwnGain(ca, i);
        CREDFIX_NOTE(ca, 2, ca->voices[i].soundIndex, ca->voices[i].gain, own);
        return own;
    }
    return 0.0f;
}

// UNDERTALE (GameMaker 1.4) plays a sound at gain 1 at most, however high its asset and instance gains go, and reads
// the gains back as set. Measured on PC (pcref credfix-hell4, UNDERTALE 1.08): the friends scene (Flowey takes the
// SOULs) raises its noise track's asset gain 0.005 a frame from 0.6 to 2.935, and audio_sound_get_gain reads 2.935; the
// captured mix gets 4 dB louder until the gain reaches 1 (20 log 1/0.6 = 4.4 dB), then stays at -10.7 dBFS RMS,
// unclipped. NDSP has no such cap, so the 3DS played that track at up to 2.9 times and clipped. So: credfixSetSoundGain,
// then every voice it set above 1 set to 1 (the game's gains, kept by credfix, read back unchanged). UNDERTALE reads
// only asset gains back (caster_get_volume on caster_load's asset indices), which this leaves as set. Late code after
// the dodge front (platform/3ds/late_reserve.ld): nothing before it moves; ctrSetSoundGain's slot jumps here.
// (This block keeps the line count it had with its UNDERTALE-only guards, so every later __LINE__ stays put.)
// DELTARUNE's runner does the same (pcref credfix-drcap3, chapter 2, one fresh bell-sound strike per asset gain: peaks
// 0.145 at gain 1, 0.077 at 0.5, 0.035 at 0.25, and 0.150, 0.149, 0.143 at 1.5, 2 and 4), so both games take it.
void credcapSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs)
    __attribute__((noinline, section(".text.credcap_late")));
void credcapSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    credfixSetSoundGain(audio, soundOrInstance, gain, timeMs);
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (ca->voices[i].fadeTarget > 1.0f && voiceMatches(ca, i, soundOrInstance)) setVoiceGain(ca, i, 1.0f, timeMs);
    }
}

// ctrSetSoundGain's and ctrGetSoundGain's slots keep main's extent (0x144 and 0xd4 bytes on all four targets): the
// padding after each jump, in a later subsection of the function's own section, is past its code and outside its
// symbol, and nothing branches there (tools/analysis/check_credfix_slots.py). A top-level asm, so the functions' own
// code is the same as without it.
#define CREDFIX_SETGAIN_PAD "320" // 0x144 - the 4-byte jump
#define CREDFIX_GETGAIN_PAD "208" // 0xd4 - the 4-byte jump
#include "ctr_audio_system_credfix_layout.inc" // before the pads below: it must name every section first
#if defined(__3DS__) && defined(NDEBUG)
__asm__(".pushsection .text.ctrSetSoundGain, 1\n.space " CREDFIX_SETGAIN_PAD "\n.popsection");
__asm__(".pushsection .text.ctrGetSoundGain, 1\n.space " CREDFIX_GETGAIN_PAD "\n.popsection");
#endif

// sfxcopy (aotprof): with buildSfx's loops in ctr_sfxcopy.inc's late code, prepareAudoSource is 424 B shorter
// (0x658 -> 0x4b0 on all four targets, GCC 16.1.0). Zero bytes after its code, in a later subsection of its own
// section and outside its symbol, keep its slot at main's extent; nothing branches there
// (tools/analysis/check_sfxcopy_slot.py).
#define SFXCOPY_PAD "424"
#if defined(__3DS__) && defined(NDEBUG)
__asm__(".pushsection .text.prepareAudoSource, 1\n.space " SFXCOPY_PAD "\n.popsection");
#endif
// usfast (aotprof): usInstall runs on every play, audio update and room change, and took the worker's lock and scanned
// all its jobs each time. The worker counts finished jobs (done or failed) under its lock, beside the state write
// (usDoneCount); usInstall reads the count under the lock when it scans (usDoneSeen) and returns at once while the two
// match: no job has finished since, and every scan frees every job it finds, so nothing waits for a later scan. A
// count the game core has not seen yet is found by the next call, as if the worker had finished a moment later. The
// lines above sit on existing lines and this block before the #line below, so no line number moves (__LINE__ trap).
#ifdef UTSFX_FASTCHECK
// Gate build only (define it at the top of the utsfx section; never in a shipped build): each time usInstall skips,
// take the lock and look for a finished job whose count it has already seen. One would be a lost install.
static uint32_t usFastSkips UTSFX_DATA, usFastLost UTSFX_DATA;
UTSFX_CODE static void usFastCheck(void) {
    bool lost = false;
    LightLock_Lock(&usw.lock);
    if (usDoneCount == usDoneSeen) {
        for (int i = 0; UTSFX_JOBS > i; i++) {
            if (usw.jobs[i].state == UTSFX_JOB_DONE || usw.jobs[i].state == UTSFX_JOB_FAILED) lost = true;
        }
    }
    LightLock_Unlock(&usw.lock);
    usFastSkips++;
    if (lost) usFastLost++;
    if (lost || (usFastSkips & 255u) == 0) {
        logInfo("utsfx fastcheck: %lu skips, %lu lost, %lu done, %lu installed\n", (unsigned long) usFastSkips,
                (unsigned long) usFastLost, (unsigned long) usDoneCount, (unsigned long) usw.installed);
    }
}
#else
static inline void usFastCheck(void) {} // shipped builds: the skip makes no call
#endif
#line 4889 // sfxcopy: main's numbering (the __LINE__ trap)

// ===[ flowend: queued sound reads go between the pieces of a cache decode (late code, both games) ]===
// The loader thread ran a whole-sound decode into the SFX cache to its end (runPrepDecode), and a sound read queued
// meanwhile waited for all of it: Photoshop Flowey's mouth beam (its beam sound) queued behind the alarm's
// 2.9-3.1 s decode on the Old models and was heard about a second late (row flowend-o3ds-1: the read finished 68 ms
// after the decode). The loader's decode reads are wrapped (CMakeLists.txt, --wrap=CtrDecoder_read): on the loader
// thread, each read asks for at most FLOWEND_DECODE_FRAMES frames (about 27 ms of decoding on the Old 3DS) and first
// does one queued read, as loaderThreadMain does them (readRequest's work), through a descriptor opened for it and
// closed after (a game change can swap the file behind a path; these reads are rare). runPrepDecode already loops
// until it has every frame, and a decoder's output doesn't depend on how its reads are cut, so the cached sound is
// the same. Other threads' reads pass straight through. Compiled for size: UNDERTALE Old's late text budget.
#define FLOWEND_DECODE_FRAMES 2048u

int32_t __real_CtrDecoder_read(CtrDecoder* dec, int16_t* out, uint32_t maxFrames);
int32_t __wrap_CtrDecoder_read(CtrDecoder* dec, int16_t* out, uint32_t maxFrames);

#ifdef ENABLE_WAD16
// A cached looping sound's seam check (utaSeamSound) read every frame of the copy on the game thread at its first
// looped play: 591 KB took 17.4 ms and 870 KB 25.9 ms of game-thread audio on the o2ds (argutflowey L5). For a
// background decode the loader measures the same largest step per channel here, piece by piece as it decodes (the
// samples are still in the data cache), in step with the job's slot. flowendSeamCollect gives the result to
// utaSeamSound's table before the copy is installed. Synchronous loads keep the scan.
void flowendSeamMeasure(CtrAudioSystem* ca, const int16_t* out, int32_t got);
void flowendSeamCollect(CtrAudioSystem* ca);
// Global, not static: GCC emits this object's static late functions before its global ones, so these two then sit
// after flowendGuessNext and leave it at its old phase (its literal pool holds an 8-byte-aligned double).
static uint32_t flowendSeamAt[PREP_SLOTS] __attribute__((section(".bss.flowend_seam")));
static int32_t flowendSeamMax[PREP_SLOTS][2] __attribute__((section(".bss.flowend_seam")));

__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) void flowendSeamMeasure(CtrAudioSystem* ca,
                                                                                                      const int16_t* out, int32_t got) {
    for (int k = 0; k < PREP_SLOTS; k++) {
        PrepJob* job = &ca->preps[k];
        const int16_t* x = (const int16_t*) job->pcm;
        uint32_t ch = job->channels, i = (uint32_t) (out - x), end = i + (uint32_t) got * ch;
        if (job->state != PREP_BUSY || x == NULL || ch - 1 > 1 || out < x || i >= job->frames * ch) continue;
        int32_t* m = flowendSeamMax[k];
        if (i == 0) m[0] = m[1] = 0, i = ch;
        else if (i != flowendSeamAt[k]) got = -1; // not in order: no result for this copy
        if (got < 0) end = 0;
        for (; end > i; i++) { // samples, interleaved: channel i & (ch - 1)
            int32_t d = abs((int32_t) x[i] - (int32_t) x[i - ch]);
            if (d > m[i & (ch - 1)]) m[i & (ch - 1)] = d;
        }
        flowendSeamAt[k] = end;
        return;
    }
}
#endif

__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) int32_t __wrap_CtrDecoder_read(CtrDecoder* dec,
                                                                                                       int16_t* out, uint32_t maxFrames) {
    CtrAudioSystem* ca = gShutdownTarget;
    if (ca != NULL && ca->loader != NULL && threadGetCurrent() == ca->loader) {
        if (maxFrames > FLOWEND_DECODE_FRAMES) maxFrames = FLOWEND_DECODE_FRAMES;
        LoadRequest* req = NULL;
        LightLock_Lock(&ca->loadLock);
        for (int i = 0; i < LOAD_SLOTS && req == NULL; i++) {
            if (ca->loads[i].state == LOAD_QUEUED) {
                req = &ca->loads[i];
                req->state = LOAD_BUSY;
            }
        }
        LightLock_Unlock(&ca->loadLock);
        if (req != NULL) { // one read per piece: the next waits at most a piece
            u64 start = svcGetSystemTick();
            uint32_t total = req->size + req->header, size = req->size;
            uint8_t* data = (uint8_t*) malloc(total + 1);
            int fd = open(req->path, O_RDONLY);
            off_t at = (off_t) (req->offset - req->header);
            bool ok = data != NULL && fd >= 0 && lseek(fd, at, SEEK_SET) == at && read(fd, data, total) == (ssize_t) total;
            if (fd >= 0) close(fd);
            if (ok && req->header > 0) {
                uint32_t exact;
                memcpy(&exact, data, sizeof(exact));
                if (exact <= size) size = exact;
                memmove(data, data + req->header, size);
            }
            if (!ok) { // the game thread then loads it directly (collectLoads)
                free(data);
                data = NULL;
            }
            LightLock_Lock(&ca->loadLock);
            req->data = data;
            req->dataSize = size;
            req->readTicks = svcGetSystemTick() - start;
            req->state = data != NULL ? LOAD_DONE : LOAD_FAILED;
            LightLock_Unlock(&ca->loadLock);
        }
#ifdef ENABLE_WAD16
        int32_t got = __real_CtrDecoder_read(dec, out, maxFrames);
        flowendSeamMeasure(ca, out, got);
        return got;
#endif
    }
    return __real_CtrDecoder_read(dec, out, maxFrames);
}

#ifdef ENABLE_WAD16
#include "ctr_wrap_chain.h" // shedSfx: flowend -> utflowey2 (UNDERTALE Old) -> real
// ===[ flowend: the room's planned sounds stay out of the texture cache's shedding (late code, UNDERTALE only) ]===
// The sound plan copies a scene's sounds at the room change (z_utsfx.c), but keepLinearFloor's shedSfx then took the
// largest cheap-to-reload sound first, so a planned sound played once and then left idle for a while was the first
// to go: Photoshop Flowey's mouth beam (328 KB) was shed 37 ms before its finale play and read again at play time
// (Azahar Old run wf3, +604 s). The room's planned sounds (what z_utsfx.c asks for after CtrAudioSystem_beginSfxPlan,
// at most FLOWEND_KEEP) count as just played during a shed, as a sound played in the last SFX_SHED_RECENT_MS does:
// other sounds and idle tiles go first. Wrapped (CMakeLists.txt), so ctr_textures.c and z_utsfx.c keep their code.
// They also go last when a sound's buffer finds no linear block (evictSfx's forced pass, least recently used first):
// at the 3DS's own memory (Azahar memcap 59184, run wfB) the beam went there when the glockenspiel music's decode came in at
// +598 s, as the plan's copies start at lastUse 0 and a play sets the next use count. Each audio update (the sfx
// worker's vtable layer, usw.outer.update) puts the kept sounds' lastUse at the top; a play resets it while that sound
// plays (sfxInUse keeps it then), and the next room's plan gives the last room's back.
#define FLOWEND_KEEP 8
#define FLOWEND_KEPT_USE 0xFFFFFFFFu
static int16_t flowendKeep[FLOWEND_KEEP] __attribute__((section(".bss.flowend_late")));
static uint32_t flowendKeepCount __attribute__((section(".bss.flowend_late")));

void __real_CtrAudioSystem_beginSfxPlan(void);
int __real_CtrAudioSystem_preloadSfx(int32_t soundIndex, uint32_t bytes, uint32_t floorBytes);
int __real_CtrAudioSystem_queueSfx(int32_t soundIndex, uint32_t kib, uint32_t floorBytes);
uint32_t __real_CtrAudioSystem_shedSfx(uint32_t bytes);
void __wrap_CtrAudioSystem_beginSfxPlan(void);
int __wrap_CtrAudioSystem_preloadSfx(int32_t soundIndex, uint32_t bytes, uint32_t floorBytes);
int __wrap_CtrAudioSystem_queueSfx(int32_t soundIndex, uint32_t kib, uint32_t floorBytes);
uint32_t __wrap_CtrAudioSystem_shedSfx(uint32_t bytes);

// The kept sounds that are in the cache: lastUse at the top (back = false: the last room's back to the bottom).
__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) static void flowendRaiseKept(bool back) {
    CtrAudioSystem* ca = gShutdownTarget;
    if (ca == NULL) return;
    for (uint32_t i = 0; flowendKeepCount > i; i++) {
        CachedSfx* c = &ca->sfxCache[(uint16_t) flowendKeep[i]];
        if (c->pcm != NULL && (!back || c->lastUse == FLOWEND_KEPT_USE)) c->lastUse = back ? 0 : FLOWEND_KEPT_USE;
    }
}

// Each audio update, before collectPreps installs a finished background decode: its seam verdict, from the loader's
// measure, goes into a free or stale entry of utaSeamSound's table (never over a live one), keyed as utaSeamSound
// keys it (sound, copy, frames, format), so the first looped play finds it and reads nothing. Same verdict; the
// "loop seam" log line is left out (late text). A stale entry (its copy left the cache) can only match its own sound
// decoded again, the same samples.
__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) void flowendSeamCollect(CtrAudioSystem* ca) {
    if (ca == NULL) return;
    for (int k = 0; k < PREP_SLOTS; k++) {
        PrepJob* job = &ca->preps[k];
        const int16_t* x = (const int16_t*) job->pcm;
        uint32_t n = job->frames, ch = job->channels;
        // The loader's writes are seen once its PREP_DONE is (written under loadLock).
        if (__atomic_load_n(&job->state, __ATOMIC_ACQUIRE) != PREP_DONE || !job->ok || x == NULL || flowendSeamAt[k] != n * ch ||
            4 * UTA_TAIL_MAX > n) {
            continue;
        }
        flowendSeamAt[k] = 0;
        UtaSeamSound* e = NULL;
        for (UtaSeamSound* t = utaSeamSounds; t < utaSeamSounds + UTA_SEAM_SOUNDS; t++) {
            if (t->pcm == NULL || ca->sfxCache[t->sound].pcm != t->pcm) e = t;
        }
        if (e == NULL) continue;
        e->sound = job->soundIndex;
        e->pcm = (const uint8_t*) x;
        e->frames = n;
        e->format = ch == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16;
        for (uint32_t c = 0; c < 2; c++) {
            int32_t step = ch > c ? abs((int32_t) x[c] - (int32_t) x[(n - 1) * ch + c]) : 0;
            e->trigger[c] = step > 0 && step >= UTA_SEAM_RATIO * flowendSeamMax[k][c];
        }
    }
}

__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) static void flowendUpdate(AudioSystem* audio, float deltaTime) {
    flowendRaiseKept(false);
    flowendSeamCollect(gShutdownTarget);
    usUpdate(audio, deltaTime);
}

// After the plan's calls: the worker's layer (set up at its first queued copy) runs flowendUpdate.
__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) static void flowendLayer(void) {
    flowendRaiseKept(false);
    if (usw.wrapped != NULL && usw.wrapped == gShutdownTarget && usw.outer.update == usUpdate) usw.outer.update = flowendUpdate;
}

__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) void __wrap_CtrAudioSystem_beginSfxPlan(void) {
    flowendRaiseKept(true);
    flowendKeepCount = 0;
    __real_CtrAudioSystem_beginSfxPlan();
}

__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) static void flowendKeepSound(int32_t soundIndex) {
    CtrAudioSystem* ca = gShutdownTarget;
    if (ca != NULL && 0 <= soundIndex && ca->soundCount > (uint32_t) soundIndex && FLOWEND_KEEP > flowendKeepCount) {
        flowendKeep[flowendKeepCount++] = (int16_t) soundIndex;
    }
}

__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) int __wrap_CtrAudioSystem_preloadSfx(int32_t soundIndex, uint32_t bytes,
                                                                                                           uint32_t floorBytes) {
    flowendKeepSound(soundIndex);
#ifdef CTRPORT_ADPCM_MUSIC
    int got = mmSfxAdmit(soundIndex, bytes, floorBytes);
    if (got < 0) got = __real_CtrAudioSystem_preloadSfx(soundIndex, bytes, floorBytes);
#elif defined(CTRPORT_ADPCM_SFXPACK)
    int got = apAdmit(soundIndex, bytes, floorBytes);
    if (got == -2) got = __real_CtrAudioSystem_preloadSfx(soundIndex, bytes, floorBytes);
#else
    int got = __real_CtrAudioSystem_preloadSfx(soundIndex, bytes, floorBytes);
#endif
#line 5072
    flowendLayer();
    return got;
}

__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) int __wrap_CtrAudioSystem_queueSfx(int32_t soundIndex, uint32_t kib,
                                                                                                         uint32_t floorBytes) {
    // A sound preloadSfx couldn't copy in the frame is queued next: it is already in the list.
    if (flowendKeepCount == 0 || flowendKeep[flowendKeepCount - 1] != soundIndex) flowendKeepSound(soundIndex);
#ifdef CTRPORT_ADPCM_MUSIC
    int got = mmSfxPlanned(soundIndex) ? -1 : __real_CtrAudioSystem_queueSfx(soundIndex, kib, floorBytes);
#elif defined(CTRPORT_ADPCM_SFXPACK)
    int got = apQueue(soundIndex, kib, floorBytes);
#else
    int got = __real_CtrAudioSystem_queueSfx(soundIndex, kib, floorBytes);
#endif
#line 5081
    flowendLayer();
    return got;
}

// keepLinearFloor's call, on the game thread (as every play is): the kept sounds' play times read as now for the
// shed, then go back.
__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) uint32_t __wrap_CtrAudioSystem_shedSfx(uint32_t bytes) {
    CtrAudioSystem* ca = gShutdownTarget;
    uint32_t n = ca != NULL ? flowendKeepCount : 0;
    u64 played[FLOWEND_KEEP];
    u64 now = svcGetSystemTick();
    for (uint32_t i = 0; n > i; i++) {
        CachedSfx* c = &ca->sfxCache[(uint16_t) flowendKeep[i]];
        played[i] = c->lastPlayTick;
        c->lastPlayTick = now;
    }
    uint32_t shed = CTR_SHEDSFX_AFTER_FLOWEND(bytes);
    for (uint32_t i = n; i-- > 0;) ca->sfxCache[(uint16_t) flowendKeep[i]].lastPlayTick = played[i];
    return shed;
}
#endif

#ifdef CTRPORT_HAVE_TREMOR
// ===[ flowend: the next created stream opened ahead of its play (late code, both games) ]===
// A stream's open (CtrDecoder_openFile: the file, then Tremor's headers and seekable scan) takes 110-240 ms on the Old
// models, and the music starts that much after its cue: Photoshop Flowey's finale_3 (the heavy drums) came in 192 ms
// after finale_2 stopped (row flowend-o3ds-1). A game creates the parts of a piece of music together and plays them in
// that order (audio_create_stream takes the lowest free slot, so later creations have higher slots), so when the opener
// thread opens a created stream, the active created stream in the next higher slot is the likely next one. UNDERTALE
// plays its music as sounds with their own files (no created streams), named in order: a <name>_1, its loop
// <name>_1_l, <name>_2, <name>_3 (and the fight's other numbered music), so for any other file the guess
// is the name with its last number one higher, keeping what follows the number if that file is there, else without it
// (<name>_1_l -> <name>_2; nothing there, no guess). Once the
// opener has had nothing to open for FLOWEND_AHEAD_IDLE_MS and no stream is waiting, it opens that one into a spare
// decoder (at its "waiting" note: CtrWatchdog_note is wrapped), and the play that asks for that path takes it as it is,
// as if just opened: the same file, the same Tremor state, nothing read since. One spare at a time; a new guess closes
// an unused one only after FLOWEND_AHEAD_KEEP_MS, and the guess itself also waits for the quiet second (its SD lookups
// must not delay a cue). Only Ogg Vorbis files are kept. Wrapped (CMakeLists.txt), so openStream and the opener keep their
// code; other threads' opens pass straight through.
#define FLOWEND_AHEAD_IDLE_MS 1000.0
#define FLOWEND_AHEAD_PATH 256
#define FLOWEND_INITSET 4 // Tremor's ready_state once the decoder is set up: then vf.vb.vd points into vf
#define FLOWEND_AHEAD_KEEP_MS 120000.0 // an unused spare outlives other opens this long (part1 -> alarm, 6s_1, saved -> part2)

static CtrDecoder flowendAhead __attribute__((section(".bss.flowend_late")));
static char flowendAheadPath[FLOWEND_AHEAD_PATH] __attribute__((section(".bss.flowend_late")));
static char flowendLastPath[FLOWEND_AHEAD_PATH] __attribute__((section(".bss.flowend_late"))); // last opened, to guess from
static u64 flowendAheadTick __attribute__((section(".bss.flowend_late")));
static uint8_t flowendGuessDue __attribute__((section(".bss.flowend_late")));
static CtrAudioSystem* flowendAheadOwner __attribute__((section(".bss.flowend_late")));
static u64 flowendLastOpen __attribute__((section(".bss.flowend_late")));
static uint32_t flowendCtxSig __attribute__((section(".bss.flowend_late")));
static uint8_t flowendAheadState __attribute__((section(".bss.flowend_late"))); // 0 none, 1 path wanted, 2 open
static const char flowendWaiting[] __attribute__((section(".rodata.flowend_in_text"))) = "waiting for a stream to open";
static const char flowendAheadLog[] __attribute__((section(".rodata.flowend_in_text"))) = "Audio: opened '%s' ahead in %.1f ms\n";
static const char flowendNextName[] __attribute__((section(".rodata.flowend_in_text"))) = "%.*s%lu%s";

bool __real_CtrDecoder_openFile(CtrDecoder* dec, const char* path);
bool __wrap_CtrDecoder_openFile(CtrDecoder* dec, const char* path);
void __real_CtrWatchdog_note(const char* state);
void __wrap_CtrWatchdog_note(const char* state);
uint32_t CtrTextures_flowendLoads(void); // ctr_textures.c's end: tile loads so far

// Opener thread: the spare goes (it was another audio system's, or a new guess replaces it).
__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) static void flowendDropAhead(void) {
    if (flowendAheadState == 2) CtrDecoder_close(&flowendAhead);
    flowendAheadState = 0;
}

// Opener thread, at the quiet second after opening path: the next created stream's path, if path is a created
// stream's. The main thread owns the entries; a path read here is copied at once and only ever opened (a stale one
// fails to open or is not asked for).
__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) static void flowendGuessNext(CtrAudioSystem* ca, const char* path) {
    // An unused spare stays a while: the next part often follows other music (part1, alarm, 6s_1, saved, part2).
    if (flowendAheadState == 2 && FLOWEND_AHEAD_KEEP_MS > TICKS_TO_MS(svcGetSystemTick() - flowendAheadTick)) return;
    int i = 0;
    while (MAX_STREAM_ENTRIES > i && !(ca->streamEntries[i].active && ca->streamEntries[i].path != NULL && strcmp(ca->streamEntries[i].path, path) == 0)) i++;
    for (i++; MAX_STREAM_ENTRIES > i; i++) {
        const char* next = ca->streamEntries[i].path;
        if (!ca->streamEntries[i].active || next == NULL || strcmp(next, path) == 0) continue;
        size_t n = strnlen(next, FLOWEND_AHEAD_PATH);
        if (n >= FLOWEND_AHEAD_PATH || (flowendAheadState != 0 && strcmp(flowendAheadPath, next) == 0)) return;
        flowendDropAhead();
        memcpy(flowendAheadPath, next, n + 1);
        flowendAheadState = 1;
        return;
    }
    // Not a created stream (or the last one): the next number. Only the file name's digits count.
    const char* base = strrchr(path, '/');
    base = base != NULL ? base + 1 : path;
    const char* dot = strrchr(base, '.');
    if (dot == NULL) dot = base + strlen(base);
    const char* end = dot;
    while (end > base && (end[-1] < '0' || end[-1] > '9')) end--;
    const char* start = end;
    while (start > base && start[-1] >= '0' && start[-1] <= '9') start--;
    if (start == end || end - start > 6) return;
    char guess[FLOWEND_AHEAD_PATH];
    unsigned long number = strtoul(start, NULL, 10) + 1;
    int n = snprintf(guess, sizeof(guess), flowendNextName, (int) (start - path), path, number, end);
    if (n <= 0 || n >= FLOWEND_AHEAD_PATH) return;
    if (end != dot && access(guess, F_OK) != 0) {
        n = snprintf(guess, sizeof(guess), flowendNextName, (int) (start - path), path, number, dot);
        if (n <= 0 || n >= FLOWEND_AHEAD_PATH) return;
    }
    if (access(guess, F_OK) != 0 || (flowendAheadState != 0 && strcmp(flowendAheadPath, guess) == 0)) return;
    flowendDropAhead();
    memcpy(flowendAheadPath, guess, (size_t) n + 1);
    flowendAheadState = 1;
}

__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) bool __wrap_CtrDecoder_openFile(CtrDecoder* dec, const char* path) {
    CtrAudioSystem* ca = gShutdownTarget;
    if (ca == NULL || ca->opener == NULL || threadGetCurrent() != ca->opener) return __real_CtrDecoder_openFile(dec, path);
    if (flowendAheadOwner != ca) flowendDropAhead();
    flowendAheadOwner = ca;
    flowendLastOpen = svcGetSystemTick();
    bool ok = true;
    if (flowendAheadState == 2 && strcmp(flowendAheadPath, path) == 0) {
        memcpy(dec, &flowendAhead, sizeof(*dec));
        dec->vf.datasource = dec; // Tremor's callbacks get the decoder
        if (dec->vf.ready_state >= FLOWEND_INITSET) dec->vf.vb.vd = &dec->vf.vd;
        flowendAheadState = 0;
    } else {
        ok = __real_CtrDecoder_openFile(dec, path);
    }
    // The guess (its SD lookups cost 20-45 ms) waits for the quiet second with the open ahead, off the cue's path.
    size_t n = strnlen(path, FLOWEND_AHEAD_PATH);
    if (ok && FLOWEND_AHEAD_PATH > n) {
        memcpy(flowendLastPath, path, n + 1);
        flowendGuessDue = 1;
    }
    return ok;
}

__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) void __wrap_CtrWatchdog_note(const char* state) {
    __real_CtrWatchdog_note(state);
    CtrAudioSystem* ca = gShutdownTarget;
    if (ca == NULL || ca->opener == NULL || threadGetCurrent() != ca->opener || strcmp(state, flowendWaiting) != 0) return;
    // Any stream context changing (a state or a path: short streamed sounds such as a flame loop open from memory,
    // not through CtrDecoder_openFile) counts as the opener being busy: the spare waits for a quiet second, so it
    // never holds up a sound that is about to open (Azahar run wfC lost a flameloop play to a 96 ms open ahead).
    // A tile load counts too: the game thread's tile reads and this open share the card.
    uint32_t sig = CtrTextures_flowendLoads();
    for (int i = 0; STREAM_CONTEXTS > i; i++) sig = sig * 33u + (uint32_t) ca->streams[i].state + (uint32_t) (uintptr_t) ca->streams[i].path;
    u64 t0 = svcGetSystemTick();
    if (sig != flowendCtxSig) {
        flowendCtxSig = sig;
        flowendLastOpen = t0;
    }
    if ((flowendAheadState != 1 && !flowendGuessDue) || flowendAheadOwner != ca || ca->openerQuit || FLOWEND_AHEAD_IDLE_MS > TICKS_TO_MS(t0 - flowendLastOpen)) return;
    for (int i = 0; STREAM_CONTEXTS > i; i++) {
        if (ca->streams[i].state == CTX_OPEN_REQUESTED) return;
    }
    if (flowendGuessDue) {
        flowendGuessDue = 0;
        flowendGuessNext(ca, flowendLastPath);
        if (flowendAheadState != 1) return;
    }
    bool ok = __real_CtrDecoder_openFile(&flowendAhead, flowendAheadPath);
    if (ok && flowendAhead.kind != CTR_DECODER_VORBIS) {
        CtrDecoder_close(&flowendAhead);
        ok = false;
    }
    flowendAheadState = ok ? 2 : 0;
    flowendAheadTick = svcGetSystemTick();
    if (ok) postMessage(ca, flowendAheadLog, flowendAheadPath, TICKS_TO_MS(svcGetSystemTick() - t0));
}
#endif

// stallfix (rev 3, ctr_benchheld.inc): the game thread's sound load time in this stats window so far (logWindowStats
// clears it), read once a measured bench frame on the game thread, for the bench's held-frame lines. Late code at the
// end of the file (platform/3ds/late_reserve.ld, .text.stallfix_late). Not in DELTARUNE Old builds (CTRPORT_BENCH_HELD).
#ifdef CTRPORT_BENCH_HELD
uint64_t CtrAudioSystem_heldLoadTicks(AudioSystem* audio);
__attribute__((noinline, section(".text.stallfix_late"))) uint64_t CtrAudioSystem_heldLoadTicks(AudioSystem* audio) {
    return ((CtrAudioSystem*) audio)->windowLoadTicks;
}
#endif

// ADPCM measurement only: same stats critical section as the original log,
// before it resets its counters. Existing call sites only change targets.
#ifdef CTRPORT_AUDIO_METRICS
static u64 amWindowTicks AM_BSS;
static uint32_t amWindowFrames AM_BSS;
static uint32_t amTotalFrames AM_BSS;
#include <malloc.h>
static const char amTimingLog[] AM_RO =
    "[audiometrics] decode %llu ticks / %lu game frames = %.6f ms/frame; core %d\n";
static const char amMemoryLog[] AM_RO =
    "[audiometrics] sfx %lu B (%lu PCM8, %lu PCM16); adpcm_same_channels %lu B; "
    "stream_linear %lu B; stream_context_heap %lu B; decoder_heap_usable %lu B; "
    "decoder_linear_requested %lu B; tracked %lu allocations; overflow %lu\n";
static const char amSfxHeapLog[] AM_RO =
    "[audiometrics] sfx_metadata %lu heap B %lu linear B; audo_blobs %lu heap B %lu linear B\n";
static const char amProcessLog[] AM_RO =
    "[audio-process] updates %lu; heap_used %lu B; linear_free %lu B; sfx_linear %lu B; heap_capacity %lu B; linear_capacity %lu B\n";
extern u32 __ctru_heap_size, __ctru_linear_heap_size;

AM_CODE static void amStatsLock(LightLock* lock) {
    LightLock_Lock(lock);
    CtrAudioSystem* ca = (CtrAudioSystem*)((uint8_t*)lock - offsetof(CtrAudioSystem, statsLock));
    amWindowTicks = ca->threadDecodeTicks;
    amWindowFrames = ca->windowFrames;
    amTotalFrames += amWindowFrames;
}

AM_CODE static void amStatsUnlock(LightLock* lock) {
    LightLock_Unlock(lock);
    CtrAudioSystem* ca = (CtrAudioSystem*)((uint8_t*)lock - offsetof(CtrAudioSystem, statsLock));
    uint32_t pcm8 = 0, pcm16 = 0, adpcm = 0, streamBytes = 0, sfxLinear = 0;
    for (uint32_t i = 0; i < ca->soundCount; ++i) {
        const CachedSfx* c = &ca->sfxCache[i];
        if (!c->pcm || !c->bytes) continue;
        sfxLinear += (uint32_t)linearGetSize(c->pcm);
        if (((c->format >> 2) & 3u) == NDSP_ENCODING_PCM8) pcm8 += c->bytes;
        else pcm16 += c->bytes;
        uint32_t channels = c->format & 3u;
        // DTAD header, DSP frames, and a history for each 2,044-frame block.
        // This estimate assumes the cache's existing mono collapse is kept.
        adpcm += 128u + channels * (8u * ((c->frames + 13u) / 14u) +
                                   8u * ((c->frames + 2043u) / 2044u));
    }
    for (int i = 0; i < STREAM_CONTEXTS; ++i)
        for (int j = 0; j < STREAM_BUFFERS; ++j)
            if (ca->streams[i].bufs[j].pcm)
                streamBytes += STREAM_BUFFER_FRAMES * 2u * sizeof(int16_t);
    AmAllocationStats stats;
    amAllocationStats(&stats);
    uint32_t metadataHeap = 0, metadataLinear = 0, blobHeap = 0, blobLinear = 0;
    uint32_t count = ca->soundCount ? ca->soundCount : 1;
    amMemoryUsage(ca->sfxCache, count * sizeof(CachedSfx), &metadataHeap, &metadataLinear);
    amMemoryUsage(ca->soundLengths, count * sizeof(float), &metadataHeap, &metadataLinear);
    amMemoryUsage(ca->warned, count * sizeof(bool), &metadataHeap, &metadataLinear);
    amMemoryUsage(ca->soundForms, count * sizeof(uint8_t), &metadataHeap, &metadataLinear);
    amMemoryUsage(ca->noBackgroundRead, count * sizeof(bool), &metadataHeap, &metadataLinear);
    for (int i = 0; i < arrlen(ca->base.audioGroups); ++i) {
        const DataWin* group = ca->base.audioGroups[i];
        if (!group || group->mappedFile || !group->lazyLoadAudio) continue;
        for (uint32_t j = 0; j < group->audo.count; ++j) {
            const AudioEntry* entry = &group->audo.entries[j];
            amMemoryUsage(entry->data, entry->dataSize, &blobHeap, &blobLinear);
        }
    }
    logQuiet(amTimingLog, (unsigned long long)amWindowTicks, (unsigned long)amWindowFrames,
             amWindowFrames ? TICKS_TO_MS(amWindowTicks) / amWindowFrames : 0.0, ca->threadCore);
    logQuiet(amMemoryLog, (unsigned long)ca->cacheBytes, (unsigned long)pcm8, (unsigned long)pcm16,
             (unsigned long)adpcm, (unsigned long)streamBytes, (unsigned long)sizeof(ca->streams),
             (unsigned long)stats.heapBytes, (unsigned long)stats.linearRequestedBytes,
             (unsigned long)stats.allocations, (unsigned long)stats.overflow);
    logQuiet(amSfxHeapLog, (unsigned long)metadataHeap, (unsigned long)metadataLinear,
             (unsigned long)blobHeap, (unsigned long)blobLinear);
    logQuiet(amProcessLog, (unsigned long)amTotalFrames, (unsigned long)mallinfo().uordblks,
             (unsigned long)linearSpaceFree(), (unsigned long)sfxLinear,
             (unsigned long)__ctru_heap_size, (unsigned long)__ctru_linear_heap_size);
}
#endif

#ifdef CTRPORT_ADPCM_SFX
#include "ctr_adpcm_sfx.inc"
#endif

#ifdef CTRPORT_ADPCM_MUSIC
#include "ctr_adpcm_music.inc"
#endif

#ifdef CTRPORT_ADPCM_SFXPACK
#include "ctr_adpcm_pack.inc"
#endif
// ===[ flowpitch: every voice at the game's frame clock (late code, both games; flowend, 2026-10-28) ]===
// Every rate this file sets goes through here (ctr_audio_utaudio.h's ndspChnSetRate): each voice, music, sound effect
// or stream, gets its rate times CTR_MUSIC_RATE_SCALE (ctr_music_clock.h), so its samples keep pace with the game's
// 29.9156 frames a second, as the whole game does (ruling 10-28: no music/sfx split, nothing to classify). Set
// with the rate, so once per start and per pitch, gain or pause change, as before; nothing per frame. What the game
// reads stays in the sound's own time: the track position (the DSP's sample position over the sample rate), lengths
// and loops are counted in samples, and audio_sound_get_pitch returns the game's pitch. The silent path (no DSP)
// never gets here. The log line is claimed with an atomic exchange: the decode thread (stream opens) and the game
// thread both set rates.
#include "ctr_music_clock.h"
#define FLOWPITCH_LATE __attribute__((noinline, noipa, optimize("Os"), section(".text.flowpitch_late")))
static bool flowpitchLogged __attribute__((section(".bss.flowpitch_late")));
static const char flowpitchLog[] __attribute__((section(".rodata.flowpitch_in_text"))) =
    "Audio: every voice plays at x%.6f of its rate, the game's frame clock (%.4f Hz screens)\n";

FLOWPITCH_LATE static void flowpitchChnSetRate(int id, float rate) {
    CtrAudioSystem* ca = utaCa;
    if (ca != NULL && utaPost != NULL && !__atomic_exchange_n(&flowpitchLogged, true, __ATOMIC_RELAXED))
        utaPost(ca, flowpitchLog, (double) CTR_MUSIC_RATE_SCALE, 60.0 * (double) CTR_MUSIC_RATE_SCALE);
    utaChnSetRate(id, rate * CTR_MUSIC_RATE_SCALE);
}

// ctr_video.c's tickClock calls this in place of its u64-to-double conversion (__aeabi_ul2d, base-standard calling
// convention, so tickClock keeps its code and size): a video's wall clock (one not driven by the game's frame clock)
// runs at the game's pace too. Without it a video with no audio of its own ran 0.28% ahead of the sound the game plays
// beside it (DELTARUNE Ch5's garden intro and its OGG: 84 ms over 30 s). It lives here, not in ctr_video.c, because a
// callee defined in that file reorders its functions.
__attribute__((noinline, noipa, optimize("Os"), pcs("aapcs"), section(".text.flowpitch_late")))
double flowpitchVideoUl2d(u64 ticks) {
    return (double) ticks * (double) CTR_MUSIC_RATE_SCALE;
}

// ===[ A linear block the DSP still reads is freed only once nothing reads it (late; both games) ]===
// stopVoice frees the voice at once (kind VOICE_FREE, so sfxInUse no longer sees it), but the uta layer keeps reading
// the sound's PCM after that: a stopped channel is UTA_PENDING until the next frame callback builds its tail from the
// PCM (utaStop), and the pieces it had queued (UTA_AHEAD_FRAMES DSP frames of the sound) stay queued or playing out of
// the PCM until the tail ends (UTA_TAILING, cut at tailEnd). With DSP-ADPCM sound effects (CTRPORT_ADPCM_SFX/SFXPACK,
// and MUSIC's mono adapter) a channel's decoder also reads the cached copy (adSfx.physical[p].meta->data) until
// utaForget. A free in that window (evictSfx, CtrAudioSystem_shedSfx, keepLinearFloor's shed) gave the block back
// while the DSP or the callback still read it, and the next linear allocation (a texture tile, another sound) could
// take it. The window scales with 1/pitch (tailEnd counts the queued source frames in DSP frames): ~20-70 ms at pitch
// 1, seconds near the 0.01 floor.
// Every linearFree in this file goes through CtrAudio_pcmWaitFree: a block a uta channel may still read is noted on a
// list and freed (through ctr_memory.c's wrap, as before) once no channel reads it; any other block is freed at once,
// as before. A block that may be read is never freed and nothing waits. Whether it may be read:
// - uta running (utaActive): a scan of the channels under the uta lock. If the lock is taken (the frame callback, a
//   reset, adpcm's reaper freeing its rings under it), the answer is "may be read": the block waits for the next check.
// - uta shut down but the DSP still on (shutdownAudio between ndspSetCallback(NULL) and ndspExit: the channels' states
//   are no longer kept, the DSP still plays what was queued): every block may be read, so every free waits.
// - DSP off (no audio system, or its ndspReady false: after ndspExit in shutdownAudio, or a silent start): nothing
//   reads linear memory, so the whole list is freed and every free is at once. shutdownAudio's frees after ndspExit
//   (the stream buffers, then the cached sounds) so drain the list after ndspChnReset and ndspExit, at UNDERTALE's exit
//   and at DELTARUNE's game change (nothing waiting lives on into the next chapter).
// The list has no size limit and is lock-free (a note is pushed with a compare-and-swap; a flush takes the whole list
// with an exchange and pushes back what is still read), so any thread may use it, under any lock. Notes come from a
// fixed pool (PCMWAIT_NOTES, more than the blocks the channels can read at once), then the heap without relief; with
// neither the block is kept for good (logged), never freed under the DSP.
// The list is flushed by every free in this file, by the audio update, by this file's linearSpaceFree (a file-local
// alias like linearFree's: the sound plan's floor checks) and by any linear allocation that fails, in any file
// (ctr_memory_lock.c's linearAlloc entry calls CtrMemory_linearAllocOnce, which CMakeLists.txt wraps: on failure the
// idle waiting blocks are freed and the allocation is tried once more; nothing in the game calls linearMemAlign).
// The cache's byte count drops at the free, as before; the audio update then counts the bytes still waiting against
// the cache's cap (ca->cacheBytes, the game thread's), until they are freed.
// The redirections are file-local aliases of the names linearFree and linearSpaceFree (the .set below), not a change
// to the code above: GCC's call graph, and so every existing function's code and order, stay as they were (a changed
// call in freeSfx alone moved UNDERTALE's .text.late.utsfx functions); only the branch targets change. The update
// reaches the list through the vtable (ctrAudioSystemVtable's .update names pcmWaitUpdate, which then runs ctrUpdate;
// UNDERTALE's sound plan layer and the pause menu call the vtable's update through their copies, so they reach it
// too).
// This is a second layer: every free path still skips a sound a voice plays (sfxInUse: kind VOICE_SFX with that sfx,
// which also holds a start that is deferred and not yet given to the DSP, the voice keeping its kind and sfx until its
// buffers go out or it is stopped). It covers what sfxInUse can't see, the channel of a voice already stopped.
// The uta layer's statics and the audio system are read through a literal table (as adpcm's CtrAdpcm_sfxView does): a
// direct reference from this new code would add references GCC walks, and it then emitted the file's existing BSS in
// another order.
#define PCMWAIT_TEXT __attribute__((noinline, section(".text.pcmwait_late")))
#define PCMWAIT_BSS __attribute__((section(".bss.pcmwait_late")))
#define PCMWAIT_NOTES 64 // 24 channels: two segments each, plus adpcm's rings, with room to spare
typedef struct PcmWaitNote {
    struct PcmWaitNote* next;
    uint8_t* pcm;
    size_t size;
} PcmWaitNote;
static PcmWaitNote* pcmWaitHead PCMWAIT_BSS; // the waiting blocks' notes
static uint32_t pcmWaitPending PCMWAIT_BSS; // notes on the list
static uint32_t pcmWaitBytes PCMWAIT_BSS; // their blocks' bytes
static uint32_t pcmWaitEver PCMWAIT_BSS; // blocks that had to wait, ever
static uint32_t pcmWaitUsed[PCMWAIT_NOTES / 32] PCMWAIT_BSS; // the pool's notes in use
static PcmWaitNote pcmWaitPool[PCMWAIT_NOTES] PCMWAIT_BSS;
static uint32_t pcmWaitOff PCMWAIT_BSS; // frees made with the DSP off (a new count: what was counted is gone)
static uint32_t pcmWaitCountedOff PCMWAIT_BSS;
static uint32_t pcmWaitCounted PCMWAIT_BSS; // bytes the update added to pcmWaitCountedCa's cacheBytes
static CtrAudioSystem* pcmWaitCountedCa PCMWAIT_BSS;
void __wrap_linearFree(void* mem);
u32 __wrap_linearSpaceFree(void);
void* __real_malloc(size_t size);
void __real_free(void* ptr);
void* __real_CtrMemory_linearAllocOnce(size_t size);
void* __wrap_CtrMemory_linearAllocOnce(size_t size);
void CtrAudio_pcmWaitFree(void* mem);
u32 CtrAudio_pcmWaitSpaceFree(void);
// this file's linearFree and linearSpaceFree calls (and only this file's)
__asm__(".set linearFree, CtrAudio_pcmWaitFree\n.set linearSpaceFree, CtrAudio_pcmWaitSpaceFree\n");

typedef struct {
    UtaChan* channels;
    ndspWaveBuf (*buffers)[UTA_RING];
    LightLock* lock;
    volatile bool* active;
    CtrAudioSystem* volatile* target;
} PcmWaitUtaView;
extern const PcmWaitUtaView ctrPcmWaitUta;
__asm__(".pushsection .rodata.pcmwait_in_text,\"a\",%progbits\n"
        ".p2align 2\n.global ctrPcmWaitUta\n"
        ".type ctrPcmWaitUta,%object\nctrPcmWaitUta:\n"
        ".word utaChan, utaWb, utaLock, utaActive, gShutdownTarget\n"
        ".size ctrPcmWaitUta, .-ctrPcmWaitUta\n.popsection\n");
static const char pcmWaitLog[] __attribute__((section(".rodata.pcmwait_in_text"))) =
    "Audio (pcm wait): a cached sound's PCM (%lu KB) is still read by a stopped channel; its free waits (%lu so far)\n";
static const char pcmWaitKeptLog[] __attribute__((section(".rodata.pcmwait_in_text"))) =
    "Audio (pcm wait): no note for a waiting block; its PCM (%lu KB) is kept, never freed\n";

enum { PCMWAIT_SCAN, PCMWAIT_KEEP, PCMWAIT_DSP_OFF };
// What a check can know: uta running (scan its channels), uta shut down with the DSP still on (keep everything), or
// the DSP off (nothing reads linear memory).
PCMWAIT_TEXT static int pcmWaitState(void) {
    const PcmWaitUtaView* v = &ctrPcmWaitUta;
    if (*v->active) return PCMWAIT_SCAN;
    CtrAudioSystem* ca = *v->target;
    return ca != NULL && ca->ndspReady ? PCMWAIT_KEEP : PCMWAIT_DSP_OFF;
}

// Whether the uta layer may still read [pcm, pcm + size): a stopped channel's segments (PENDING: its tail is built from
// them next; TAILING: what it queued before the tail still plays), any queued or playing DSP buffer in the block, or
// (DSP-ADPCM) a channel's decoder reading the cached copy. Can't tell (the lock taken, uta shut down with the DSP on):
// true.
PCMWAIT_TEXT static bool pcmWaitBusy(const uint8_t* pcm, size_t size) {
    const PcmWaitUtaView* v = &ctrPcmWaitUta;
    int state = pcmWaitState();
    if (state != PCMWAIT_SCAN) return state == PCMWAIT_KEEP;
    if (pcm == NULL || size == 0) return false;
    // Under the lock the frame callback doesn't run, and once a voice is stopped it moves its channel only toward idle
    // (PENDING to TAILING after its tail is copied out, buffers QUEUED to PLAYING to DONE); uta shut down after the
    // check above only freezes the states it saw.
    if (LightLock_TryLock(v->lock) != 0) return true;
    bool busy = false;
    for (int p = 0; UTA_CHANNELS > p && !busy; p++) {
        const UtaChan* c = &v->channels[p];
        if (c->state == UTA_PENDING || c->state == UTA_TAILING) {
            for (int i = 0; c->segs > i && 2 > i; i++) {
                const uint8_t* d = c->seg[i].data;
                if (d >= pcm && pcm + size > d) busy = true;
            }
        }
        for (int k = 0; UTA_RING > k && !busy; k++) {
            const ndspWaveBuf* w = &v->buffers[p][k];
            const uint8_t* d = (const uint8_t*) w->data_vaddr;
            if ((w->status == NDSP_WBUF_QUEUED || w->status == NDSP_WBUF_PLAYING) && d >= pcm && pcm + size > d) busy = true;
        }
#if defined(CTRPORT_ADPCM_SFX) || defined(CTRPORT_ADPCM_MUSIC) || defined(CTRPORT_ADPCM_SFXPACK)
        const AdSfxMeta* m = adSfx.physical[p].meta;
        if (m != NULL && m->data != NULL && m->data >= pcm && pcm + size > m->data) busy = true;
#endif
    }
    LightLock_Unlock(v->lock);
    return busy;
}

// A note: the pool's first free one (claimed with a compare-and-swap on its word), else the heap (newlib's, without
// relief: the caller may hold any lock), else NULL.
PCMWAIT_TEXT static PcmWaitNote* pcmWaitNew(void) {
    for (int w = 0; PCMWAIT_NOTES / 32 > w; w++) {
        uint32_t used = __atomic_load_n(&pcmWaitUsed[w], __ATOMIC_RELAXED);
        while (used != 0xFFFFFFFFu) {
            uint32_t bit = (uint32_t) __builtin_ctz(~used);
            if (__atomic_compare_exchange_n(&pcmWaitUsed[w], &used, used | (1u << bit), true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                return &pcmWaitPool[w * 32 + bit];
        }
    }
    return (PcmWaitNote*) __real_malloc(sizeof(PcmWaitNote));
}

PCMWAIT_TEXT static void pcmWaitDelete(PcmWaitNote* n) {
    if (n >= pcmWaitPool && pcmWaitPool + PCMWAIT_NOTES > n) {
        uint32_t i = (uint32_t) (n - pcmWaitPool);
        __atomic_fetch_and(&pcmWaitUsed[i / 32], ~(1u << (i % 32)), __ATOMIC_RELEASE);
    } else {
        __real_free(n);
    }
}

// Onto the list (any thread). Pushes and whole-list takes only, so a note never comes off between a reader's load of
// the head and its compare-and-swap.
PCMWAIT_TEXT static void pcmWaitPush(PcmWaitNote* n) {
    PcmWaitNote* head = __atomic_load_n(&pcmWaitHead, __ATOMIC_ACQUIRE);
    do n->next = head;
    while (!__atomic_compare_exchange_n(&pcmWaitHead, &head, n, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

// Frees the waiting blocks no channel reads any more, every one with the DSP off (any thread); the rest go back on
// the list. Nothing comes off while uta is shut down with the DSP still on.
PCMWAIT_TEXT static void pcmWaitFlush(void) {
    if (__atomic_load_n(&pcmWaitHead, __ATOMIC_ACQUIRE) == NULL || pcmWaitState() == PCMWAIT_KEEP) return;
    PcmWaitNote* n = __atomic_exchange_n(&pcmWaitHead, NULL, __ATOMIC_ACQ_REL);
    while (n != NULL) {
        PcmWaitNote* next = n->next;
        if (pcmWaitBusy(n->pcm, n->size)) {
            pcmWaitPush(n);
        } else {
            __wrap_linearFree(n->pcm);
            __atomic_fetch_sub(&pcmWaitBytes, n->size, __ATOMIC_RELAXED);
            __atomic_fetch_sub(&pcmWaitPending, 1, __ATOMIC_RELAXED);
            pcmWaitDelete(n);
        }
        n = next;
    }
}

// Every linearFree of this file (the .set above).
PCMWAIT_TEXT void CtrAudio_pcmWaitFree(void* mem) {
    pcmWaitFlush();
    uint8_t* pcm = (uint8_t*) mem;
    int state = pcmWaitState();
    if (state == PCMWAIT_DSP_OFF) __atomic_fetch_add(&pcmWaitOff, 1, __ATOMIC_RELAXED);
    size_t size = pcm != NULL && state != PCMWAIT_DSP_OFF ? linearGetSize(pcm) : 0;
    if (size != 0 && pcmWaitBusy(pcm, size)) {
        uint32_t ever = __atomic_add_fetch(&pcmWaitEver, 1, __ATOMIC_RELAXED);
        if (16 >= ever || (ever & (ever - 1)) == 0) // the first 16, then 32, 64...
            logInfo(pcmWaitLog, (unsigned long) (size / 1024u), (unsigned long) ever);
        PcmWaitNote* n = pcmWaitNew();
        if (n == NULL) {
            logInfo(pcmWaitKeptLog, (unsigned long) (size / 1024u));
            return;
        }
        n->pcm = pcm;
        n->size = size;
        __atomic_fetch_add(&pcmWaitBytes, size, __ATOMIC_RELAXED);
        __atomic_fetch_add(&pcmWaitPending, 1, __ATOMIC_RELAXED);
        pcmWaitPush(n);
        return;
    }
    __wrap_linearFree(mem);
}

// Every linearSpaceFree of this file (the .set above): the idle waiting blocks are freed first.
PCMWAIT_TEXT u32 CtrAudio_pcmWaitSpaceFree(void) {
    pcmWaitFlush();
    return __wrap_linearSpaceFree();
}

// ctr_memory_lock.c's linearAlloc entry (any file, any thread; CMakeLists.txt's wrap): once more
// after freeing the idle waiting blocks, if that could help. The linear lock isn't held here.
PCMWAIT_TEXT void* __wrap_CtrMemory_linearAllocOnce(size_t size) {
    void* p = __real_CtrMemory_linearAllocOnce(size);
    if (p == NULL && __atomic_load_n(&pcmWaitHead, __ATOMIC_ACQUIRE) != NULL) {
        pcmWaitFlush();
        p = __real_CtrMemory_linearAllocOnce(size);
    }
    return p;
}

// Game thread: the bytes still waiting count against the cache's cap (evictSfx, the sound plan) until they are freed.
// A new audio system, or frees made with the DSP off since (shutdownAudio's), start the count afresh.
PCMWAIT_TEXT static void pcmWaitCount(void) {
    CtrAudioSystem* ca = *ctrPcmWaitUta.target;
    uint32_t off = __atomic_load_n(&pcmWaitOff, __ATOMIC_RELAXED);
    if (ca != pcmWaitCountedCa || off != pcmWaitCountedOff) {
        pcmWaitCountedCa = ca;
        pcmWaitCountedOff = off;
        pcmWaitCounted = 0;
    }
    if (ca == NULL || !ca->ndspReady) return;
    uint32_t bytes = __atomic_load_n(&pcmWaitBytes, __ATOMIC_RELAXED);
    ca->cacheBytes += bytes - pcmWaitCounted;
    pcmWaitCounted = bytes;
}

// The vtable's update: the waiting blocks first, then the audio update as before.
PCMWAIT_TEXT static void pcmWaitUpdate(AudioSystem* audio, float deltaTime) {
    pcmWaitFlush();
    pcmWaitCount();
    MENUPAUSE_OPAQUE(&ctrUpdate)(audio, deltaTime);
}

