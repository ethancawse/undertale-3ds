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
#ifdef ENABLE_WAD16
static Thread packopenThreadCreate(ThreadFunc, void*, size_t, int, int, bool);
static void packopenPrime(CtrAudioSystem*, const char*);
#endif
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
    bool shutDown; struct SlState* slState; // seamlocal's (UNDERTALE): in the hole before voices

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
    float* soundLengths; // per SOND index, < 0 until known; UT future offsets follow
    bool* warned; // per SOND index: unsupported format already reported
    uint8_t* soundForms; // per SOND index, SoundForm
    bool* noBackgroundRead; // per SOND index: a background read failed, read it directly

    StreamEntry streamEntries[MAX_STREAM_ENTRIES]; int32_t* lcStop; // lcstop's (UNDERTALE): in the hole before streams
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
    u64 frameTicks; // audio work since the last update() returned; the [audio] "game ms/frame" figure. From stage 75zk (sndcut) UNDERTALE's stops and deferred burst plays are not in it
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
#if defined(ENABLE_WAD16)
void* audparLengthStorage(size_t bytes, const char* file, int line);
void audparClearVoice(Voice* v, CtrAudioSystem* ca, int32_t soundIndex);
float audparGetTrackPosition(AudioSystem* audio, int32_t soundOrInstance);
void audparSetTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds);
#endif
#line 434

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
    GUN_STREAM_CLOSE(&s->decoder);
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
#ifdef ENABLE_WAD16
// Retain the release entry slot; only the failed-open policy changes in late text.
#define openStream upd1AudioOpen
__attribute__((noinline, noipa, used, section(".text.late.upd1")))
#endif
#line 737
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
            if (s->path != NULL
#ifndef ENABLE_WAD16
                && access(s->path, F_OK) != 0 && (errno == ENOENT || errno == ENOTDIR)
#endif
            ) {
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

#ifdef ENABLE_WAD16
#undef openStream
#if defined(__arm__)
__attribute__((naked, noinline, used, section(".text.openStream")))
static bool openStream(CtrAudioSystem* ca __attribute__((unused)), StreamContext* s __attribute__((unused))) {
    __asm__("b upd1AudioOpen\n.space 1632");
}
#else
static bool openStream(CtrAudioSystem* ca, StreamContext* s) { return upd1AudioOpen(ca, s); }
#endif
#endif
#line 835

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
__attribute__((noinline, noipa, optimize("Os"), section(".text.audofix_late"))) static void audofixSeekClear(StreamContext* s); static void serviceStream(CtrAudioSystem* ca, StreamContext* s, DecodeStats* stats) {
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
        audofixSeekClear(s); // audofix F2: not once stopStreamVoice has given the channel up (file end)
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

#if defined(ENABLE_WAD16)
#define LightEvent_WaitTimeout lmDecodeWait
#endif
#line 971
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

#undef LightEvent_WaitTimeout
#line 1007
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
#if defined(ENABLE_WAD16) && defined(CTR_AUDIO_PCM_HASH)
    latSample((CtrAudioSystem*) data);
#endif
#line 1028
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
        CTR_LOADER_WAIT(&ca->loadEvent);
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
                if (fd >= 0) CTR_LOADER_CLOSE(fd);
                fd = CTR_LOADER_OPEN(req->path, O_RDONLY);
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
#ifdef ENABLE_WAD16
    ca->loader = packopenThreadCreate(loaderThreadMain, ca, LOADER_STACK_SIZE, priority, ca->threadCore == 0 ? -2 : ca->threadCore, false);
#else
    ca->loader = threadCreate(loaderThreadMain, ca, LOADER_STACK_SIZE, priority, ca->threadCore == 0 ? -2 : ca->threadCore, false);
#endif
#line 1189
    return ca->loader != NULL;
}

static void stopLoaderThread(CtrAudioSystem* ca) {
    if (ca->loader == NULL) return;
    ca->loaderQuit = true;
    LightEvent_Signal(&ca->loadEvent);
    CTR_LOADER_JOIN(ca->loader, U64_MAX);
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
#ifdef CTRPORT_PACER_ARENA // E1b: static, as a global here GCC put two existing late functions in another order
static void CtrArena_loadSignal(LightEvent* ev); // (flowendPlaySound, CtrAudioSystem_chainFlowburst): check_layout
#endif
#line 1254
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
#ifdef CTRPORT_PACER_ARENA // E1b (UNDERTALE Old): a read over PA_SMALL gets its block here (ctr_audio_system.c's end)
    CtrArena_loadSignal(&ca->loadEvent);
#else
    LightEvent_Signal(&ca->loadEvent);
#endif
#line 1283
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
__attribute__((noinline, noipa, optimize("Os"), section(".text.audofix_late"))) static void audofixReleaseBlob(CtrAudioSystem* ca, int32_t soundIndex, DataWin* group, AudioEntry* entry); static float soundLength(CtrAudioSystem* ca, int32_t soundIndex) { __asm__ volatile(".rept 30; nop; .endr"); // audofix F1: keeps the size (the release is a late call)
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
            if (smallWav) audofixReleaseBlob(ca, soundIndex, group, entry); // audofix F1: kept while a stream reads it (file end)
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

__attribute__((unused)) static int findFreeStreamContext(CtrAudioSystem* ca) { // audofix S2: audofixFreeStreamContext copies it (file end)
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
__attribute__((noinline, noipa, optimize("Os"), section(".text.audofix_late"))) static int audofixFreeStreamContext(CtrAudioSystem* ca); static int claimStreamContext(CtrAudioSystem* ca, int32_t priority, int forVoice) {
    int ctx = audofixFreeStreamContext(ca); ML_CLAIM(ca, ctx); // audofix S2 (file end; ML_CLAIM: musicloop, its nops: same size)
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
    GOM_PUBLISH(&s->lock); // gomusic: the request notes its frame under this lock, so the opener can't take it first (file end)
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
        GUN_STREAM_CLOSE(&s->decoder);
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
#if defined(ENABLE_WAD16)
    ca->soundLengths = (float*) audparLengthStorage(allocCount * sizeof(float), __FILE__, 2131);
#else
#line 2131
    ca->soundLengths = (float*) safeMalloc(allocCount * sizeof(float));
#endif
#line 2132
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
#if defined(ENABLE_WAD16)
    audparClearVoice(v, ca, soundIndex);
#else
    memset(v, 0, sizeof(*v));
#endif
#line 2631
    v->instanceId = nextInstanceId(ca);
    v->soundIndex = soundIndex;
    v->priority = priority;
    v->serial = ca->serialCounter++;
    v->loop = loop;
    v->gain = gain;
    v->fadeTarget = gain;
    v->pitch = pitch;
    v->emitterGain = 1.0f;
#if !defined(ENABLE_WAD16)
    v->ctx = -1;
#endif
#line 2641
#if defined(ENABLE_WAD16) && defined(CTR_AUDIO_PCM_HASH)
    latRequest(ca, vi);
#endif
#line 2641

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

#if defined(ENABLE_WAD16)
__attribute__((noinline)) static bool ctrIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    bool lcstopIsPlaying(AudioSystem* audio, int32_t soundOrInstance); // lcstop (file end), then audparIsPlaying
    return lcstopIsPlaying(audio, soundOrInstance);
}
#else
static bool ctrIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (voiceMatches(ca, i, soundOrInstance) && !ca->voices[i].paused) return true;
    }
    return false;
}
#endif
#line 2760

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

#if defined(ENABLE_WAD16) && defined(CTR_AUDIO_PCM_HASH)
    if ((uint32_t) soundOrInstance - 0x80000001u < 255u) {
        latMark((uint32_t) soundOrInstance - 0x80000000u);
        return 0.0f;
    }
#endif
#line 2875
#if defined(ENABLE_WAD16)
    return audparGetTrackPosition(audio, soundOrInstance);
#else

    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (!voiceMatches(ca, i, soundOrInstance)) continue;
        return GetTime_virtualClockOn() ? clockPosition(ca, &ca->voices[i]) : voicePosition(ca, i);
    }
    return 0.0f;
#endif
}
#line 2882

static void ctrSetTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds) {
#if defined(ENABLE_WAD16)
    audparSetTrackPosition(audio, soundOrInstance, positionSeconds);
#else

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
#endif
}
#line 2910

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

__attribute__((noinline, section(".text.pcmwait_late"))) static void pcmWaitUpdate(AudioSystem* audio, float deltaTime); __attribute__((noinline, noipa, optimize("Os"), section(".text.audofix_late"))) static bool audofixIsPlaying(AudioSystem* audio, int32_t id); FLOWEND_BURST_DECL AUDOFIX_FEED_DECL LCSTOP_DECL static AudioSystemVtable ctrAudioSystemVtable = {
    .init = ctrInit,
    .destroy = ctrDestroy,
    .update = CTR_UPDATE_ENTRY,
    .playSound = CTR_PLAY_ENTRY,
    .setSoundSpatial = ctrSetSoundSpatial,
    .setSoundEmitterGain = ctrSetSoundEmitterGain,
    .setListenerPosition = ctrSetListenerPosition,
    .stopSound = CTR_STOP_ENTRY,
    .stopAll = CTR_STOPALL_ENTRY, // lcstop (file end)
    .isPlaying = audofixIsPlaying, // audofix M1 (file end)
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
// - No fade (stopdead; playtest 10-29: all game audio stops dead on the menu frame and resumes at the same sample):
//   the open pauses every game channel at once, in the game frame that shows the menu's first page, and the close
//   lets them go on at the next audio update. The master volume is never touched. (Until 11-01 the master volume
//   stepped down over six audio updates before the channels paused and back up over six after the close, and the
//   hold also waited while a menu sound played, so the music went on for about 200 ms under the menu and came back
//   late after it.) Neither the open nor the close waits on the DSP on the game thread (smoothux menu1: a fade
//   of six 5 ms sleeps once held the menu's first frames 35 ms and the game's first frame after the close 30 ms).
// - While held, the audio update runs with no elapsed time, so the game's fades, group fades and sound clocks hold
//   too; an open video's clock holds with its sound (CtrVideo_menuPause).
// - The release is deferred to the next audio update: Quit leaves the loop without one, so the music doesn't come
//   back for the moment before the game exits; Resume and the Performance test's picker run one on their next frame.
// - Sleep doesn't use the suspend flag and HOME leaves this hold alone (this file's end). NDSP saves and restores itself,
//   and the video has its own mask bits), so they nest with this hold either way round.
// - The menu's own sounds (CtrAudio_menuSound, from ctr_optmenu.c) start unpaused during the hold, one read in the
//   background too (audofix M1: it starts at a later update, and the held update unpauses it through the vtable's
//   isPlaying). They take a game voice like any sound (a free one, else the game's lowest-priority one, held or
//   not) and use up an instance id; pausemus's exact menu-sound path (no game voice or id) stays parked.
// - Not held (pausemus's reading): positioned sounds' gain ramps run on the
//   system clock (rampStep), and UNDERTALE's callback-fed SFX tails (utaFrameCallback) keep counting down; a stream
//   decodes into a buffer the DSP had already freed before the pause (the same samples, played after the close).
#define MENUPAUSE_CODE __attribute__((noinline, section(".text.late.menupause")))
#define MENUPAUSE_BSS __attribute__((section(".bss.late.menupause")))
#define MENUPAUSE_RO __attribute__((section(".rodata.late.menupause")))
#define MENUPAUSE_RAMP_FRAMES 6 // menuSetLevel's full-volume step (the old fade's length)

void CtrVideo_menuPause(bool pause);
void __real_CtrLegal_close(void);
void __real_CtrMenu_close(void* menu);
void __wrap_CtrLegal_close(void);
void __wrap_CtrMenu_close(void* menu);

// 0 when not held; MENUPAUSE_RAMP_FRAMES + 1 from the open to its first audio update; 1 once the channels are paused.
static uint8_t menuHeld MENUPAUSE_BSS;
static bool menuReleasePending MENUPAUSE_BSS;
static u64 menuHeldSince MENUPAUSE_BSS;
static void (*menuRealUpdate)(AudioSystem* audio, float deltaTime) MENUPAUSE_BSS;
// The menu's sounds (change gamedir2): new code and state in that change's own late sections, at the end of the late
// blocks (late_reserve.ld), named outside .text.late.* and .bss.late.* so the catch-alls leave them there.
#define MENUSOUND_CODE __attribute__((noinline, section(".text_gamedir2_late")))
static int32_t menuVoice __attribute__((section(".bss.gamedir2_late"))); // the instance of the menu's last sound
// The vtable's functions, called through a pointer the compiler can't see through: a plain ca->base.vtable->suspend
// call was speculatively devirtualized (ctrSuspend inlined behind a check), which changed where ctrSuspend's own copy
// was emitted and moved every function after it in this object by 72 B.
#define MENUPAUSE_OPAQUE(fn) ({ __typeof__(fn) opaque_ = (fn); __asm__("" : "+r"(opaque_)); opaque_; })

static const char menuHeldText[] MENUPAUSE_RO = "Audio: held for the options menu (%lu underruns so far)\n";
static const char menuReleasedText[] MENUPAUSE_RO = "Audio: released after the options menu, held %.1f s (%lu underruns so far)\n";

// The master volume (applyMasterGain's value at level MENUPAUSE_RAMP_FRAMES; the old fade stepped through the rest).
MENUPAUSE_CODE __attribute__((noipa)) static void menuSetLevel(CtrAudioSystem* ca, int level) {
    float gain = ca->masterGain * ca->listenerGain;
    if (gain < 0.0f) gain = 0.0f;
    menuHeld = (uint8_t) (level + 1);
    if (ca->ndspReady) ndspSetMasterVol(gain * (float) level / (float) MENUPAUSE_RAMP_FRAMES);
}

// The audio update while held (stopdead). The open's first update (in the frame that opened the menu: main.c's paused
// branch runs it after CtrMenu_open) pauses every game channel where it is; no time passes after that. The menu's sound,
// started just before it, stays out: a DSP frame between its start and this pause split it, heard as two blips (menusound,
// 11-06). The close: the channels go on at the next update, from the same sample, with the game's time running again; a
// menu sound still sounding plays on beside them. One read in the background starts unpaused (isPlaying: audofix M1).
// The .rept: nops at the entry, so menuHeldUpdate keeps its earlier 0x244 B and nothing after it moves.
MENUPAUSE_CODE static void menuHeldUpdate(AudioSystem* audio, float deltaTime) {
    __asm__ volatile(".rept 26\n nop\n .endr");
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    if (menuVoice != 0) (void) MENUPAUSE_OPAQUE(ca->base.vtable->isPlaying)(audio, menuVoice);
    if (menuReleasePending) {
        if (menuHeld == 1) { // the channels are paused (else the menu closed before its first update)
            CtrVideo_menuPause(false);
            MENUPAUSE_OPAQUE(ca->base.vtable->resume)(&ca->base);
            logInfo(menuReleasedText, TICKS_TO_MS(svcGetSystemTick() - menuHeldSince) / 1000.0, (unsigned long) CtrAudioSystem_underruns(ca));
        }
        menuSetLevel(ca, MENUPAUSE_RAMP_FRAMES); // the master volume as the game set it (the hold never moves it)
        menuHeld = 0;
        menuReleasePending = false;
        ctrAudioSystemVtable.update = menuRealUpdate;
        MENUPAUSE_OPAQUE(menuRealUpdate)(audio, deltaTime);
        return;
    }
    if (menuHeld > 1) {
        ca->suspended = true;
        for (int i = 0; i < VOICE_COUNT; i++) {
            if (ca->voices[i].instanceId != menuVoice && voiceLive(ca, i)) applyVoice(ca, i);
        }
        menuHeld = 1;
        ML_MENU_PAUSE(true);
        menuHeldSince = svcGetSystemTick();
        logInfo(menuHeldText, (unsigned long) CtrAudioSystem_underruns(ca));
    }
    MENUPAUSE_OPAQUE(menuRealUpdate)(audio, 0.0f);
}

// The menu's open: the hold starts at its first audio update (menuHeldUpdate), later in the same frame.
MENUPAUSE_CODE static void menuHold(void) {
    CtrAudioSystem* ca = gShutdownTarget;
    if (menuHeld) {
        menuReleasePending = false; // opened again before the update that releases it: stays held
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
    uint16_t nextSeq; uint8_t mlIn, slSet; // in padding: musicloop's fade-in after a resume (mlFrameLock), seamlocal's
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

#if defined(ENABLE_WAD16) || defined(CTRPORT_ADPCM_MUSIC) || defined(CTRPORT_ADPCM_SFXPACK)
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
AS_CODE static bool adSfxPacked(const void* data);
#undef UTA_RAW_ADD
#define UTA_RAW_ADD(p, buf) do { if (!adSfxPacked((buf)->data_vaddr)) UTA_PHYS_ADD(p, buf); } while (0)
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
    UTA_DECLICK_FLUSH(dst, m * ch * 2u);
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
    // Reset first, then zero the pieces: until the reset, libctru's list still links them and the NDSP thread can
    // walk it at any moment (a zeroed piece ends the list early: the 10-30 o3ds and 10-22 o2ds crashes at
    // ndspiReadChnState, docs/3ds-hardware.md).
    UTA_PHYS_RESET(p);
    utaForget(p, c);
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
#ifdef ENABLE_WAD16
    lmPaused(p, paused);
#else
    UTA_PHYS_PAUSED(p, paused);
#endif
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
        UTA_RAW_ADD(p, buf);
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
            UTA_RAW_ADD(p, buf); // not fed here: played and cut as before
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
        UTA_RAW_ADD(p, buf);
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
        UTA_DECLICK_FLUSH(dst, n * c->bytes);
        utaQueue(p, c, dst, n, 0, 0, UTA_PIECE_TAIL);
        utaTails++;
#ifdef CTR_AUDIO_CAPTURE_WRAP_H
        CtrCap_noteTail(p, n);
#endif
    }
}

UTA_LATE __attribute__((used)) static void utaFrameCallback(void* data) {
    u64 t0 = svcGetSystemTick();
    if (utaActive) {
        ML_FRAME_LOCK(&utaLock);
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
#if defined(ENABLE_WAD16) && defined(CTR_AUDIO_PCM_HASH)
    latReset();
    __asm__ volatile("nop"); // Preserve the next original double literal pool's phase in this observer.
#endif
    if (callback == NULL) {
        // Shutting down: no more tails; the channels are still reset through the mapping, which covers all of them.
        utaActive = false;
#ifdef ENABLE_WAD16
        lmRegister(NULL, NULL);
#else
        UTA_PHYS_CALLBACK(NULL, NULL);
#endif
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
    if (utaTailPcm == NULL) utaTailPcm = (uint8_t*) UTA_FAULT_ALLOC(1, UTA_CHANNELS * UTA_TAIL_MAX * 4);
    if (utaSeamPcm == NULL) utaSeamPcm = (uint8_t*) UTA_FAULT_ALLOC(2, UTA_CHANNELS * UTA_SEAM_MAX * 4);
    utaEngineCallback = callback;
    utaActive = true;
#ifdef ENABLE_WAD16
    ML_REGISTER(utaFrameCallback, data);
#else
    UTA_PHYS_CALLBACK(utaFrameCallback, data);
#endif
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
    uint32_t installed, installedKb, dropped, failed; struct HplState* hplState; // hplcopy's (UT Old Tremor): in the hole before workTicks
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
#ifdef CTR_AUDIO_PCM_HASH
    latCall((CtrAudioSystem*) audio, soundIndex);
#endif
#line 4543
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
// change spikerec (z_spikerec.c, playtest builds only): the game thread's sound load time in this stats window so far
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

int32_t __real_CtrDecoder_read(CtrDecoder* dec, int16_t* out, uint32_t maxFrames); int32_t audofixReadLink(CtrDecoder* dec, int16_t* out, uint32_t maxFrames);
int32_t __wrap_CtrDecoder_read(CtrDecoder* dec, int16_t* out, uint32_t maxFrames);

// ===[ flowhitch rev 3 (change flowend r22c; UNDERTALE): the first buffer of a spare, decoded ahead ]===
// A stream's first buffer (4,096 frames, 93 ms of sound) is decoded before anything is queued: 37-45 ms on the Old
// models even for a stream opened ahead (Azahar Old, timing mode, run r22-hdiag-flowey-old), so a pre-opened song
// started 1-2 frames after its play call and a shed cue replayed as a stream 3-10 frames after (its open too). The
// opener decodes each spare's first buffer at its next wait (FlowhitchPre, one per spare: the next-file spare, the
// phase-song spare and three shed-cue spares), and the decode thread's first reads of the stream that took that spare
// copy it. A spare's decoder position is set back to 0 after the decode ahead, so the stream reads frame 0 first, as
// before, and the decoder continues where the copy ends (pcm_offset == end). A seek or another decoder fails that test.
#if defined(ENABLE_WAD16) && defined(CTRPORT_HAVE_TREMOR) // Tremor's decoder state (stb_vorbis builds: the real calls)
#define FLOWHITCH_SLOTS 3 // the shed cues' spares (below)
#define FLOWHITCH_PRE (2 + FLOWHITCH_SLOTS)
#define FLOWHITCH_PRE_FRAMES STREAM_BUFFER_FRAMES
typedef struct {
    const void* key; // the spare's vorbis_info: a taken spare's decoder carries it
    int16_t* pcm; // FLOWHITCH_PRE_FRAMES frames, interleaved; opener-owned (freed once no stream reads it)
    uint32_t frames;
    ogg_int64_t end; // the decoder's pcm_offset after them
} FlowhitchPre;
static FlowhitchPre flowhitchPre[FLOWHITCH_PRE] __attribute__((section(".bss.flowhitch_late")));
int32_t CtrAudioSystem_chainReadFlowhitch(CtrDecoder* dec, int16_t* out, uint32_t maxFrames);
#define FLOWHITCH_READ_NEXT CtrAudioSystem_chainReadFlowhitch
bool CtrAudioSystem_chainOpenFlowhitch(CtrDecoder* dec, const char* path);
#define FLOWHITCH_OPEN_NEXT CtrAudioSystem_chainOpenFlowhitch
void CtrAudioSystem_chainNoteFlowhitch(const char* state); void CtrAudioSystem_chainNoteLoadclass(const char* state); void CtrAudioSystem_chainNotePair(const char* state); void CtrAudioSystem_chainNoteBootahead(const char* state);
#define FLOWHITCH_NOTE_NEXT CtrAudioSystem_chainNoteBootahead // bootahead's link, then loadclass 4c's, then loadclass's (all at this file's end), then flowhitch's
void __real_CtrReadAhead_shutdown(void); // UNDERTALE's link wraps it (usstop, at this file's end)
void CtrAudioSystem_chainStopFlowhitch(void);
#if defined(CTRPORT_TARGET_O3DS) // UNDERTALE Old (Tremor): hplcopy's workers are stopped and joined first (its block)
void hplShutdownLink(void) __asm__("hplShutdown");
#define FLOWHITCH_STOP_NEXT hplShutdownLink
// usstop's aptExit calls the real one through hplAptExit (its block): a worker still running keeps the heap there.
void hplAptExitLink(void) __asm__("hplAptExit");
#define __real_aptExit hplAptExitLink
#else
#define FLOWHITCH_STOP_NEXT CtrAudioSystem_chainStopFlowhitch
#endif
#line 4933

// The read link after flowend's (decode thread, and the rare other reader): a taken spare's first frames come from
// the copy decoded ahead.
__attribute__((noinline, noipa, optimize("Os"), section(".text.flowhitch_late"))) int32_t CtrAudioSystem_chainReadFlowhitch(
    CtrDecoder* dec, int16_t* out, uint32_t maxFrames) {
    for (int k = 0; dec->kind == CTR_DECODER_VORBIS && FLOWHITCH_PRE > k; k++) {
        const FlowhitchPre* e = &flowhitchPre[k];
        uint32_t at = (uint32_t) dec->positionFrames;
        if (e->pcm == NULL || e->key != dec->vf.vi || dec->positionFrames >= e->frames || dec->vf.pcm_offset != e->end) continue;
        uint32_t n = e->frames - at;
        if (n > maxFrames) n = maxFrames;
        memcpy(out, e->pcm + at * dec->channels, n * dec->channels * sizeof(int16_t));
        __sync_synchronize(); // the opener frees the copy once this position passes its end
        dec->positionFrames = at + n;
        return (int32_t) n;
    }
    return AUDOFIX_READ_LINK(dec, out, maxFrames);
}
#else
#define FLOWHITCH_READ_NEXT __real_CtrDecoder_read
#define FLOWHITCH_OPEN_NEXT __real_CtrDecoder_openFile
#define FLOWHITCH_NOTE_NEXT __real_CtrWatchdog_note
#endif
#line 4905 // flowhitch rev 3: main's numbering (the __LINE__ trap)
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
            int fd = CTR_LOADER_OPEN_ONCE(req->path, O_RDONLY);
            off_t at = (off_t) (req->offset - req->header);
            bool ok = data != NULL && fd >= 0 && lseek(fd, at, SEEK_SET) == at && read(fd, data, total) == (ssize_t) total;
            if (fd >= 0) CTR_LOADER_CLOSE_ONCE(fd);
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
    return FLOWHITCH_READ_NEXT(dec, out, maxFrames);
}

#ifdef ENABLE_WAD16
#include "ctr_wrap_chain.h" // shedSfx: flowend -> flowburst -> utflowey2 (UNDERTALE Old) -> real
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

// ===[ flowhitch: a sound the linear guard shed plays as a stream for the rest of the room (late code, UNDERTALE) ]===
// keepLinearFloor sheds cached sound effects when textures need linear memory. A shed sound played again in the same
// room missed the cache: it played as a stream while the audio loader probed and decoded it into the cache again
// (probe 144-190 ms, decode 2.4 s on the Old models), taking back the linear memory the guard had just freed. Omega
// Flowey's turn-4 buzz and its destroyed theme did this, and the game thread waited on the newlib heap lock or
// the loader's loadLock for 10-69 ms in that window (bfprobe3, o3DS XL): held frames of 42.7-59.4 ms (change flowend
// r20b, case 2). Each sound of at least FLOWHITCH_MIN_BYTES that a shed frees is marked FORM_STREAMED, so its later
// plays stream as before but queue no new probe or decode from streamWhileCaching, until the next room change puts
// its form back. Other paths can still cache it again (a small embedded WAV's buildSfx, a prepare job already in
// flight, the sound plan's worker); those keep valid playback and only lessen the saving. A smaller one
// is decoded again as before: one played often is cheaper to decode once than to open at every play (Omega Flowey's
// 74 KB flame loop, marked too in a first build, was opened 49 times instead of 14 in the Azahar run). Plays, voices
// and samples are the same; only which buffer a later play reads from differs (the stream's, not the cache copy's).
#define FLOWHITCH_MAX 32u
#define FLOWHITCH_MIN_BYTES (256u * 1024u)
#define FLOWHITCH_SOUNDS 1024u
static int16_t flowhitchSound[FLOWHITCH_MAX] __attribute__((section(".bss.flowhitch_late")));
static uint8_t flowhitchForm[FLOWHITCH_MAX] __attribute__((section(".bss.flowhitch_late")));
static uint32_t flowhitchCount __attribute__((section(".bss.flowhitch_late")));

// The shedSfx chain's link after flowend's (ctr_wrap_chain.h), on the game thread, as every play is.
__attribute__((noinline, noipa, optimize("Os"), section(".text.flowhitch_late"))) uint32_t CtrAudioSystem_chainFlowhitch(uint32_t bytes) {
    CtrAudioSystem* ca = gShutdownTarget;
    uint32_t n = ca != NULL ? ca->soundCount : 0;
    if (n > FLOWHITCH_SOUNDS) n = FLOWHITCH_SOUNDS;
    uint32_t was[FLOWHITCH_SOUNDS / 32u];
    for (uint32_t i = 0; n > i; i++) {
        if ((i & 31u) == 0) was[i >> 5] = 0; // each word set before its first bit (c26rev r33 LOW)
        if (ca->sfxCache[i].pcm != NULL && ca->sfxCache[i].bytes >= FLOWHITCH_MIN_BYTES) was[i >> 5] |= 1u << (i & 31u);
    }
    uint32_t shed = CTR_SHEDSFX_AFTER_FLOWHITCH(bytes);
    for (uint32_t i = 0; n > i && FLOWHITCH_MAX > flowhitchCount; i++) {
        if ((was[i >> 5] >> (i & 31u) & 1u) && ca->sfxCache[i].pcm == NULL) {
            flowhitchSound[flowhitchCount] = (int16_t) i;
            flowhitchForm[flowhitchCount++] = ca->soundForms[i];
            ca->soundForms[i] = FORM_STREAMED;
        }
    }
    return shed;
}

// The beginSfxPlan chain's link after flowend's: at a room change the shed sounds get their forms back (the last
// room's first mark last, so a sound marked twice gets the form it had before both).
__attribute__((noinline, noipa, optimize("Os"), section(".text.flowhitch_late"))) void CtrAudioSystem_chainPlanFlowhitch(void) {
    CtrAudioSystem* ca = gShutdownTarget;
    for (uint32_t i = flowhitchCount; ca != NULL && i-- > 0;) ca->soundForms[(uint16_t) flowhitchSound[i]] = flowhitchForm[i];
    flowhitchCount = 0;
    __real_CtrAudioSystem_beginSfxPlan();
}
#line 5070 // flowhitch: main's numbering (the __LINE__ trap)
__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) void __wrap_CtrAudioSystem_beginSfxPlan(void) {
    flowendRaiseKept(true);
    flowendKeepCount = 0;
    CTR_BEGINSFXPLAN_AFTER_FLOWEND();
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
    if (got == -2) got = CTR_PRELOADSFX_AFTER_FLOWEND(soundIndex, bytes, floorBytes);
#else
    int got = CTR_PRELOADSFX_AFTER_FLOWEND(soundIndex, bytes, floorBytes);
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

// ===[ flowend burst: a step's plays past the first few start at the frame's audio update (late code, UNDERTALE) ]===
// Photoshop Flowey's finale makes 61-63 plays in a single step, about 21 voices are free, and claimVoice gives
// each later play the oldest voice: about 40 voices were started (DSP channel set up, the feeder's first three DSP
// frames queued; with the SFX pack, decoded and flushed) and stopped again (a tail) inside the same step, none of
// them heard past a blip. 22-29 ms of a 41.8 ms step went into the plays (o3ds row flowend-fd2-o3ds-1A, flowdiag2).
// From the step's FLOWEND_BURST_FIRST+1th play of a cached sound on, the voice is claimed and filled in exactly as
// playSoundImpl does, but left VOICE_PENDING, as a sound still being read is: the frame's audio update (after the
// draw, before the frame is shown) starts it with the same startVoiceSource, and a pending voice taken by a later
// play is only freed. The voices left after the step are the same ones (claimVoice ranks a pending voice as an SFX
// voice), so the game sees the same instances, gains, pauses and positions. A step with at most FLOWEND_BURST_FIRST
// plays (every ordinary one, and the options menu's sounds: a paused frame runs no step) is unchanged.
#define FLOWEND_BURST_FIRST 4u
static uint32_t flowendBurstPlays __attribute__((section(".bss.flowburst_late")));
// The voices left pending this way, one bit each (VOICE_COUNT 24). sfxInUse counts VOICE_SFX voices only, so
// until a pending voice starts, evictSfx could free its sound (c26rev r4 F2). Only buildSfx (a cache miss's play)
// and collectPreps (in the update's startPendingVoices) evict, and keepLinearFloor's shed frees an unused sound
// played over SFX_SHED_RECENT_MS ago (c26rev r11d), so they are started first: before any play this code hands to
// ctrPlaySound, at the update before ctrUpdate runs (main.c: the room-change preload comes after the update), and in
// the shed's wrap chain. The sound is then in the cache and in use, as on the old path, and starts without a read.
static uint32_t flowendDeferred __attribute__((section(".bss.flowburst_late"))); static void inflightStartDeferred(CtrAudioSystem* ca, int32_t soundIndex);

// Late copies of claimVoice and nextInstanceId (the same code): calling those from here would take them out of
// ctrPlaySound, where they are inlined, and grow early text. voiceLive and stopVoice are out of line already.
__attribute__((noinline, optimize("Os"), section(".text.flowburst_late"))) static int flowendClaimVoice(CtrAudioSystem* ca, int32_t priority) {
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

__attribute__((noinline, optimize("Os"), section(".text.flowburst_late"))) static int32_t flowendNextInstanceId(CtrAudioSystem* ca) {
    for (;;) {
        int32_t id = SOUND_INSTANCE_ID_BASE + (int32_t) (ca->instanceCounter++ % INSTANCE_ID_SPAN);
        bool used = false;
        for (int i = 0; i < VOICE_COUNT; i++) {
            if (ca->voices[i].kind != VOICE_FREE && ca->voices[i].instanceId == id) used = true;
        }
        if (!used) return id;
    }
}

// startVoiceSource (out of line already) through a table the compiler can't see into, as ctr_adpcm_pack.inc's
// CtrAdpcm_musicLegacy does: a direct call from here changes the order GCC emits the uta functions it calls in
// (late text moves; check_layout).
typedef struct {
    bool (*start)(CtrAudioSystem*, int, VoiceSource*);
} FlowendLegacy;
extern const FlowendLegacy CtrAudio_flowendLegacy;
__asm__(".pushsection .rodata.flowburst_in_text,\"a\",%progbits\n.p2align 2\n.global CtrAudio_flowendLegacy\n"
        ".type CtrAudio_flowendLegacy,%object\nCtrAudio_flowendLegacy:\n.word startVoiceSource\n"
        ".size CtrAudio_flowendLegacy, .-CtrAudio_flowendLegacy\n.popsection\n");

__attribute__((noinline, optimize("Os"), section(".text.flowburst_late"))) static void flowendStartDeferred(CtrAudioSystem* ca) {
    uint32_t m = flowendDeferred;
    flowendDeferred = 0;
    if (!ca->ndspReady) return; // left pending: startPendingVoices takes them when the DSP is back
    for (int vi = 0; m != 0; vi++, m >>= 1) {
        Voice* v = &ca->voices[vi];
        // A bit whose voice was stolen or stopped since (now free, or another play's) is skipped.
        if (!(m & 1u) || v->kind != VOICE_PENDING || v->waitStream || ca->sfxCache[v->soundIndex].pcm == NULL) continue;
        VoiceSource src;
        memset(&src, 0, sizeof(src));
        src.sfx = &ca->sfxCache[v->soundIndex];
        CtrAudio_flowendLegacy.start(ca, vi, &src);
    }
}

// The shedSfx chain's link after flowend's kept-plan one (ctr_wrap_chain.h): a texture allocation on the game thread
// (keepLinearFloor) may shed in the step or the draw, before the update has started the deferred voices.
__attribute__((noinline, noipa, optimize("Os"), section(".text.flowburst_late"))) uint32_t CtrAudioSystem_chainFlowburst(uint32_t bytes) {
    CtrAudioSystem* ca = gShutdownTarget;
    if (ca != NULL && flowendDeferred != 0) flowendStartDeferred(ca);
    return CTR_SHEDSFX_AFTER_FLOWBURST(bytes);
}

__attribute__((noinline, optimize("Os"), section(".text.flowburst_late"))) static int32_t flowendPlaySound(AudioSystem* audio,
                                                                                                         int32_t soundIndex,
                                                                                                         int32_t priority, bool loop) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    if (FLOWEND_BURST_FIRST >= ++flowendBurstPlays || !ca->initialized || !ca->ndspReady || 0 > soundIndex ||
        (uint32_t) soundIndex >= ca->soundCount || ca->sfxCache[soundIndex].pcm == NULL) {
        if (flowendDeferred != 0) inflightStartDeferred(ca, soundIndex); // a miss here may evict (buildSfx); not one being read (inflight)
        return ctrPlaySound(audio, soundIndex, priority, loop);
    }
    ca->windowPlays++;
    int32_t id = -1;
    int vi = flowendClaimVoice(ca, priority);
    if (vi >= 0) {
        const Sound* sound = &ca->base.audioGroups[0]->sond.sounds[soundIndex];
        CachedSfx* c = &ca->sfxCache[soundIndex];
        Voice* v = &ca->voices[vi];
        rampRelease(ca, vi);
        audparClearVoice(v, ca, soundIndex);
        id = v->instanceId = flowendNextInstanceId(ca);
        v->soundIndex = soundIndex;
        v->priority = priority;
        v->serial = ca->serialCounter++;
        v->loop = loop;
        v->gain = sound->volume;
        v->fadeTarget = sound->volume;
        v->pitch = sound->pitch > 0.0f ? sound->pitch : 1.0f;
        v->emitterGain = 1.0f;
        /* audparClearVoice supplies ctx and snapshots the asset offset. */
#if defined(ENABLE_WAD16) && defined(CTR_AUDIO_PCM_HASH)
        latRequest(ca, vi);
#endif
#line 5229
        v->kind = VOICE_PENDING;
        flowendDeferred |= 1u << vi;
        // As played now for the cache's shedding and eviction until the update starts it (sfxInUse counts SFX voices).
        c->lastUse = ++ca->useClock;
        c->lastPlayTick = svcGetSystemTick();
    }
    __asm__ volatile(".rept 13; nop; .endr"); // sndcut: no frameTicks (end of file); inflight: its call passes soundIndex (one more instruction); the same size, so nothing moves
    return id;
}

#ifdef FLOWEND_BURST_TEST
// Gate build only (FLOWEND_BURST_TEST, never shipped): once, about 20 s in, with the cache pushed over its cap (a fake
// CTR_SFX_CACHE_BYTES added to cacheBytes, so every eviction pass frees all it may), a deferred play of a cached sound X:
// (1) then a cache miss's play of a small embedded sound Y (read here, built under the cap: buildSfx's evictSfx), and
// (2) the update's order (flowendStartDeferred, then evictSfx as collectPreps does, also forced). Each time X must stay
// cached and its voice must be VOICE_SFX on X's entry at once: started, without a read. (3) Control: a deferred play,
// then evictSfx with no start first, frees X (the c26rev r4 F2 mechanism, so the test can fail).
static uint32_t flowendTestFrames __attribute__((section(".bss.flowburst_late")));
__attribute__((noinline, section(".text.flowburst_test"))) static int flowendTestVoice(CtrAudioSystem* ca, int32_t id) {
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (ca->voices[i].kind != VOICE_FREE && ca->voices[i].instanceId == id) return i;
    }
    return -1;
}
__attribute__((noinline, section(".text.flowburst_test"))) static bool flowendTestStarted(CtrAudioSystem* ca, int32_t id, int32_t x) {
    int vi = flowendTestVoice(ca, id);
    return vi >= 0 && ca->sfxCache[x].pcm != NULL && ca->voices[vi].kind == VOICE_SFX && ca->voices[vi].sfx == &ca->sfxCache[x];
}
__attribute__((noinline, section(".text.flowburst_test"))) static bool flowendTestPending(CtrAudioSystem* ca, int32_t id) {
    int vi = flowendTestVoice(ca, id);
    return vi >= 0 && ca->voices[vi].kind == VOICE_PENDING && (flowendDeferred & (1u << vi)) != 0;
}
__attribute__((noinline, section(".text.flowburst_test"))) static void flowendBurstTest(CtrAudioSystem* ca) {
    if (flowendTestFrames == 0xFFFFFFFFu || !ca->initialized || !ca->ndspReady || 600u > ++flowendTestFrames) return;
    int32_t x = -1, y = -1;
    for (uint32_t i = 0; i < ca->soundCount; i++) {
        const Sound* sound = &ca->base.audioGroups[0]->sond.sounds[i];
        if (ca->sfxCache[i].pcm != NULL) {
            if (x < 0 && !sfxInUse(ca, &ca->sfxCache[i])) x = (int32_t) i;
        } else if (y < 0 && soundIsInAudo(sound) && !loadInFlight(ca, (int32_t) i)) {
            DataWin* group = NULL;
            AudioEntry* entry = audoEntryFor(ca, sound, &group);
            if (entry != NULL && entry->present && entry->dataSize > 64u && entry->dataSize < 64u * 1024u) y = (int32_t) i;
        }
    }
    if (x < 0 || y < 0) return; // next frame
    flowendTestFrames = 0xFFFFFFFFu;
    uint32_t savedPlays = flowendBurstPlays;
    // (1) a miss at the cap after a deferred play
    flowendBurstPlays = FLOWEND_BURST_FIRST;
    int32_t id1 = flowendPlaySound(&ca->base, x, 0, false);
    bool pend1 = flowendTestPending(ca, id1);
    bool nbr = ca->noBackgroundRead[y];
    ca->noBackgroundRead[y] = true;
    uint32_t loads = ca->windowCacheLoads, bytes0 = ca->cacheBytes;
    ca->cacheBytes += CTR_SFX_CACHE_BYTES;
    int32_t idy = flowendPlaySound(&ca->base, y, 0, false);
    ca->cacheBytes -= CTR_SFX_CACHE_BYTES;
    ca->noBackgroundRead[y] = nbr;
    bool ok1 = pend1 && flowendTestStarted(ca, id1, x);
    logInfo("flowend burst test 1 (miss at the cap): %s; x %ld pending %d, y %ld id %ld, cache loads +%lu, cache %lu -> %lu B\n",
            ok1 ? "PASS" : "FAIL", (long) x, (int) pend1, (long) y, (long) idy, (unsigned long) (ca->windowCacheLoads - loads),
            (unsigned long) bytes0, (unsigned long) ca->cacheBytes);
    // (2) the update's order: start the deferred voices, then collectPreps' eviction passes
    if (ca->sfxCache[x].pcm != NULL) {
        flowendBurstPlays = FLOWEND_BURST_FIRST;
        int32_t id2 = flowendPlaySound(&ca->base, x, 0, false);
        bool pend2 = flowendTestPending(ca, id2);
        flowendStartDeferred(ca);
        bytes0 = ca->cacheBytes;
        ca->cacheBytes += CTR_SFX_CACHE_BYTES;
        evictSfx(ca, 1u, false);
        while (evictSfx(ca, 1u, true)) {}
        ca->cacheBytes -= CTR_SFX_CACHE_BYTES;
        bool ok2 = pend2 && flowendTestStarted(ca, id2, x);
        logInfo("flowend burst test 2 (update order, evict at the cap): %s; pending %d, cache %lu -> %lu B\n", ok2 ? "PASS" : "FAIL",
                (int) pend2, (unsigned long) bytes0, (unsigned long) ca->cacheBytes);
        ctrStopSound(&ca->base, id2);
    }
    ctrStopSound(&ca->base, id1);
    if (idy >= 0) ctrStopSound(&ca->base, idy);
    // (3) control: no start before the eviction
    if (ca->sfxCache[x].pcm != NULL) {
        flowendBurstPlays = FLOWEND_BURST_FIRST;
        int32_t id3 = flowendPlaySound(&ca->base, x, 0, false);
        bool pend3 = flowendTestPending(ca, id3);
        ca->cacheBytes += CTR_SFX_CACHE_BYTES;
        evictSfx(ca, 1u, false);
        ca->cacheBytes -= CTR_SFX_CACHE_BYTES;
        bool lost = ca->sfxCache[x].pcm == NULL;
        logInfo("flowend burst test 3 (control, no start first): %s; pending %d, x freed %d\n", pend3 && lost ? "PASS" : "FAIL",
                (int) pend3, (int) lost);
        ctrStopSound(&ca->base, id3);
        flowendDeferred = 0;
    } else {
        logInfo("flowend burst test 3 (control): FAIL; x not cached after test 2\n");
    }
    // (4) the shed path: Y (embedded, read again now if test 3 freed it) deferred, its play time aged past
    // SFX_SHED_RECENT_MS, outside the kept plan, then keepLinearFloor's call with the cache pushed past the shed floor.
    // Its voice must be started (VOICE_SFX on Y) and Y still cached. (5) Control: the same, but the chain's next link
    // called directly (no drain): Y is freed.
    uint32_t keep = flowendKeepCount;
    flowendKeepCount = 0;
    if (ca->sfxCache[y].pcm == NULL) {
        ca->noBackgroundRead[y] = true;
        int32_t idr = ctrPlaySound(&ca->base, y, 0, false);
        ca->noBackgroundRead[y] = nbr;
        if (idr >= 0) ctrStopSound(&ca->base, idr);
    }
    int ran = 0;
    for (int t = 4; t <= 5 && ca->sfxCache[y].pcm != NULL; t++, ran++) {
        flowendBurstPlays = FLOWEND_BURST_FIRST;
        int32_t id4 = flowendPlaySound(&ca->base, y, 0, false);
        bool pend4 = flowendTestPending(ca, id4);
        ca->sfxCache[y].lastPlayTick -= 11ULL * SYSCLOCK_ARM11;
        bytes0 = ca->cacheBytes;
        ca->cacheBytes += 2u * SFX_SHED_KEEP_BYTES;
        uint32_t shed = t == 4 ? __wrap_CtrAudioSystem_shedSfx(2u * SFX_SHED_KEEP_BYTES)
                               : CTR_SHEDSFX_AFTER_FLOWBURST(2u * SFX_SHED_KEEP_BYTES);
        ca->cacheBytes -= 2u * SFX_SHED_KEEP_BYTES;
        bool cached = ca->sfxCache[y].pcm != NULL;
        bool ok = t == 4 ? pend4 && flowendTestStarted(ca, id4, y) : pend4 && !cached;
        logInfo("flowend burst test %d (shed, aged %s): %s; y %ld pending %d, y cached %d, shed %lu B, cache %lu -> %lu B\n", t,
                t == 4 ? "via the chain" : "control, no drain", ok ? "PASS" : "FAIL", (long) y, (int) pend4, (int) cached,
                (unsigned long) shed, (unsigned long) bytes0, (unsigned long) ca->cacheBytes);
        ctrStopSound(&ca->base, id4);
    }
    if (ran < 2) logInfo("flowend burst test 4/5: FAIL; only %d ran (y not cached)\n", ran);
    flowendKeepCount = keep;
    flowendDeferred = 0;
    flowendBurstPlays = savedPlays;
}
#endif

__attribute__((noinline, optimize("Os"), section(".text.flowburst_late"))) static void flowendBurstUpdate(AudioSystem* audio,
                                                                                                         float deltaTime) {
#ifdef FLOWEND_BURST_TEST
    flowendBurstTest((CtrAudioSystem*) audio);
#endif
    flowendBurstPlays = 0;
    flowendStartDeferred((CtrAudioSystem*) audio); // before ctrUpdate's collectPreps (it may evict)
    pcmWaitUpdate(audio, deltaTime);
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

#ifdef ENABLE_WAD16
// flowend r20 (UNDERTALE): Photoshop Flowey's phase songs are opened ahead into a second spare. The fight's bullet
// master plays its phase song at its Create and its SOUL warning a fixed frame count later (754 frames into part1,
// 678 into part2), so a song that opens late (part1 195-231 ms, part2 again 146-167 ms on the Old models, cand26)
// puts every warning that far ahead of the music, where PC's lands on it. The number guess cannot reach them: part1
// follows the laugh, part2's own guess is part3 (never played), and the one spare is kept 120 s for the SOUL songs.
// This table names the song after each, as the FNV-1a hash of its file name before the extension (the data's own
// names are looked up, as z_textfront.c's scripts are): intro and laugh -> part1 (the laugh plays from memory, with
// no open to guess from), part1 -> part2, part2 -> part1 (the second round), destroyed3 -> wind2 (the end-choice
// room after the restart); a 0 closes the spare (the finale: part1 is not played again). Its own spare, so the 6s_N
// chain keeps the first; a table name skips the number guess. Same quiet second and take as the first spare; each
// open is followed by a quiet second before the other spare's.
uint32_t RoomPlan_fnv1a(const uint8_t* data, size_t size); // z_roomplan.c
static CtrDecoder flowendSong __attribute__((section(".bss.flowpre_late")));
static char flowendSongPath[FLOWEND_AHEAD_PATH] __attribute__((section(".bss.flowpre_late")));
static uint8_t flowendSongState __attribute__((section(".bss.flowpre_late"))); // 0 none, 1 path wanted, 2 open
static const uint32_t flowendSongs[][2] __attribute__((section(".rodata.flowend_in_text"))) = {
    {0x26B8DF80u, 0x5638720Au}, {0x328062AFu, 0x5638720Au}, {0x5638720Au, 0x55387077u}, {0x55387077u, 0x5638720Au},
    {0x3B1C020Eu, 0}, {0x3EE40FFEu, 0x9B1C49AAu}};
extern const uint32_t CtrLoadmix_songs[2][2]; // Additional guesses reuse this spare and flowhitch's first buffer.
#ifdef CTR_AUDIO_PCM_HASH
bool CtrLoadmix_preparing(void); // The diagnostic selector covers the entire unit, including these guesses.
#endif
static const char flowendSongName[] __attribute__((section(".rodata.flowend_in_text"))) = "%.*s%.*s%s\0\0\0\0"; // NULs after: a phase pad

__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) static void flowendSongDrop(void) {
    if (flowendSongState == 2) CtrDecoder_close(&flowendSong);
    flowendSongState = 0;
}

// The FNV-1a hash of a file name's last part before its extension; *len gets that length.
__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) static uint32_t flowendSongHash(const char* file, const char** start, size_t* len) {
    const char* base = strrchr(file, '/');
    base = base != NULL ? base + 1 : file;
    const char* dot = strrchr(base, '.');
    *start = base;
    *len = dot != NULL ? (size_t) (dot - base) : strlen(base);
    return RoomPlan_fnv1a((const uint8_t*) base, *len);
}

// Opener thread, at the quiet second after opening path: true when the table names path (its number guess is skipped).
__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) static bool flowendSongGuess(CtrAudioSystem* ca, const char* path) {
    __asm__ volatile("nop"); // phase pad: the loader and update code after keeps stage72's phase
#if !defined(CTR_AUDIO_PCM_HASH)
    // Retain rev 3m's 444 B release slot: the extra table entries simplified
    // its body by 64 B. Every following existing late body keeps its address.
    __asm__ volatile(".rept 16\n nop\n .endr");
#endif
    const char* base;
    size_t len;
    uint32_t h = flowendSongHash(path, &base, &len);
    const size_t originalCount = sizeof(flowendSongs) / sizeof(flowendSongs[0]);
    size_t count = originalCount + 2;
#ifdef CTR_AUDIO_PCM_HASH
    if (!CtrLoadmix_preparing()) count = originalCount;
#endif
    for (size_t k = 0; count > k; k++) {
        const uint32_t* song = k < originalCount ? flowendSongs[k] : CtrLoadmix_songs[k - originalCount];
        if (song[0] != h) continue;
        if (song[1] == 0) {
            flowendSongDrop();
            return false;
        }
        for (uint32_t i = 0; ca->base.audioGroups[0]->sond.count > i; i++) {
            const char* file = ca->base.audioGroups[0]->sond.sounds[i].file;
            const char* fb;
            size_t fl;
            if (file == NULL || flowendSongHash(file, &fb, &fl) != song[1]) continue;
            char guess[FLOWEND_AHEAD_PATH];
            int n = snprintf(guess, sizeof(guess), flowendSongName, (int) (base - path), path, (int) fl, fb, base + len);
            if (n > 0 && FLOWEND_AHEAD_PATH > n && (flowendSongState == 0 || strcmp(flowendSongPath, guess) != 0)) {
                flowendSongDrop();
                memcpy(flowendSongPath, guess, (size_t) n + 1);
                flowendSongState = 1;
            }
            return true;
        }
        return false;
    }
    return false;
}

// Opener thread: the play that asks for the spare's path takes it as it is (as the first spare's take does).
__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) static bool flowendSongTake(CtrDecoder* dec, const char* path) {
    if (flowendSongState != 2 || strcmp(flowendSongPath, path) != 0) return false;
    memcpy(dec, &flowendSong, sizeof(*dec));
    dec->vf.datasource = dec; // Tremor's callbacks get the decoder
    if (dec->vf.ready_state >= FLOWEND_INITSET) dec->vf.vb.vd = &dec->vf.vd;
    flowendSongState = 0;
    return true;
}

// Opener thread, at a quiet second: open the wanted song (true: this quiet second is used).
__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) static bool flowendSongOpen(CtrAudioSystem* ca, u64 t0) {
    if (flowendSongState != 1) return false;
    bool ok = __real_CtrDecoder_openFile(&flowendSong, flowendSongPath);
    if (ok && flowendSong.kind != CTR_DECODER_VORBIS) {
        CtrDecoder_close(&flowendSong);
        ok = false;
    }
    flowendSongState = ok ? 2 : 0;
    flowendLastOpen = svcGetSystemTick(); // the first spare waits for a quiet second of its own
    if (ok) postMessage(ca, flowendAheadLog, flowendSongPath, TICKS_TO_MS(flowendLastOpen - t0));
    return true;
}
#define FLOWEND_SONG_OPEN \
    if (flowendSongOpen(ca, t0)) return;
#else
#define FLOWEND_SONG_OPEN
#endif

// Opener thread, at the quiet second after opening path: the next created stream's path, if path is a created
// stream's. The main thread owns the entries; a path read here is copied at once and only ever opened (a stale one
// fails to open or is not asked for).
__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) static void flowendGuessNext(CtrAudioSystem* ca, const char* path) {
#ifdef ENABLE_WAD16
    if (flowendSongGuess(ca, path)) return;
#endif
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

#ifdef ENABLE_WAD16
// ===[ flowhitch rev 3: three spares for the shed cues (change flowend r22c) ]===
// A cue of FLOWHITCH_MIN_BYTES or more that the guard shed replays as a stream for the rest of the room; its open
// took 160-310 ms on the Old consoles (alarm, repeat_1), where cand27's cache hit started in the frame of the call.
// At a quiet second, as the other spares, the opener keeps open the files of the three shed cues played most recently
// (Omega Flowey replays alarm, repeat_1 and endnote), and a replay takes its spare as the others are taken. A room change
// unmarks them; they close. Only the open of a cue's first play after a shed is paid, as before.
#include "ctr_open_files.h" // the spares are a kept-open cache: an open that finds the card's files crowded closes them
// Each held decoder (760 B with Tremor) is on the heap, not in late BSS: the image stays at stage75i's page count.
static CtrDecoder* flowhitchSlot[FLOWHITCH_SLOTS] __attribute__((section(".bss.flowhitch_late"))); // with its path
static char* flowhitchSlotPath[FLOWHITCH_SLOTS] __attribute__((section(".bss.flowhitch_late"))); // NULL: closed
static int16_t flowhitchSlotSound[FLOWHITCH_SLOTS] __attribute__((section(".bss.flowhitch_late")));
static int16_t flowhitchNoFile __attribute__((section(".bss.flowhitch_late"))); // sound index + 1 of a cue no spare can hold
#define FLOWHITCH_RECENT 4
static int16_t flowhitchRecent[FLOWHITCH_RECENT] __attribute__((section(".bss.flowhitch_late"))); // sound index + 1, latest first
static int8_t flowhitchKeeper __attribute__((section(".bss.flowhitch_late"))); // CtrOpenFiles keeper id + 2 (1: none free)
// The keeper's owner is flowhitchCa, not the audio system: a wake another thread copied before the keeper went can
// still arrive after the audio system is freed (exit's rooms.dtm write follows the audio stop). flowhitchWaking counts
// wakes inside flowhitchWake; the stop clears flowhitchCa, then waits for it to reach 0 (all three sequentially
// consistent: a wake either sees NULL or is counted before the stop reads the count).
static CtrAudioSystem* flowhitchCa __attribute__((section(".bss.flowhitch_late")));
static uint32_t flowhitchWaking __attribute__((section(".bss.flowhitch_late")));

#include "renderer.h" // rev 4's room link reads the runner's room
#include "runner.h"
// flowhitch rev 4 (change flowend r25): the musical's two cold song starts. Its first song has no open before it in the
// room to guess from, and the song it plays twice in a row is cold the second time (the number-guess spare already
// holds the song after it): 179-193 and 185-188 ms of open on the Old models (cand26), each a song start that late
// against the PC game's. The room link marks the musical's room (the FNV-1a hash of its name, as the song table's);
// at the opener's waits the phase-song spare (idle there) takes the first song's file once the room is entered, and
// the repeated song's file again once it starts. Both are opened at a quiet second, as every spare is; a spare left
// over is closed when the room changes (either song) or another file opens (the repeated one). Rev 4's code and statics
// are in their own sections after rev 3's (.text/.bss.flowhitch4_late, late_reserve.ld), so rev 3's keep their addresses.
#define FLOWHITCH_SONG_ROOM 0xD60BB10Fu // the room's name
#define FLOWHITCH_SONG_FIRST 0x9A2B3ECEu // its first song's file name, before the extension
#define FLOWHITCH_SONG_AGAIN 0x992B3D3Bu // the song it plays twice in a row
static uint32_t flowhitchRoomSeq __attribute__((section(".bss.flowhitch4_late"))); // game thread: room changes so far
static uint8_t flowhitchRoomIs __attribute__((section(".bss.flowhitch4_late"))); // game thread: in the musical's room
static uint32_t flowhitchRoomSeen __attribute__((section(".bss.flowhitch4_late"))); // opener: the changes handled
static uint8_t flowhitchSongMine __attribute__((section(".bss.flowhitch4_late"))); // opener: 1 the repeat, 2 the first song
static uint8_t flowhitchAgainDone __attribute__((section(".bss.flowhitch4_late"))); // opener: the repeat was asked for

static void flowhitchSongs(CtrAudioSystem* ca); // rev 4, after the note link (defined there: a function before it that
// reads flowend's statics first reorders them)
static void flowhitchOwner(CtrAudioSystem* ca); // rev 4, the same

// Opener thread (CtrOpenFiles_poll, or inside its own open that failed): an open found the card's files crowded, so
// every spare closes, as a cache's kept-open files do, and that open is tried again. Up to 5 files open at a quiet
// second; the card's open allowance ran out at 14-18 files on consoles (docs/3ds-hardware.md, FS OutOfResource).
__attribute__((noinline, optimize("Os"), section(".text.flowhitch_late"))) static void flowhitchShed(void* owner) {
    (void) owner;
    flowendDropAhead();
    flowendSongDrop();
    for (int j = 0; FLOWHITCH_SLOTS > j; j++) {
        if (flowhitchSlotPath[j] == NULL) continue;
        CtrDecoder_close(flowhitchSlot[j]);
        free(flowhitchSlot[j]);
        free(flowhitchSlotPath[j]);
        flowhitchSlotPath[j] = NULL;
    }
}

// Any thread: has the opener poll soon (its wait ends). Nothing once the audio system stopped (flowhitchCa NULL).
__attribute__((noinline, optimize("Os"), section(".text.flowhitch_late"))) static void flowhitchWake(void* owner) {
    (void) owner;
    __atomic_add_fetch(&flowhitchWaking, 1, __ATOMIC_SEQ_CST);
    CtrAudioSystem* ca = __atomic_load_n(&flowhitchCa, __ATOMIC_SEQ_CST);
    if (ca != NULL) LightEvent_Signal(&ca->openEvent);
    __atomic_sub_fetch(&flowhitchWaking, 1, __ATOMIC_SEQ_CST);
}

// shutdownAudio's CtrReadAhead_shutdown call (through usstop's wrap), after the opener and the decode thread are
// joined and before the audio system is freed: the keeper goes, no wake can reach the audio system any more, and the
// spares close and their first buffers are freed here (no thread of this audio system is left to read them), so
// the files the exit opens next (rooms.dtm's write) have the card's allowance, and a later audio system starts clean.
__attribute__((noinline, noipa, optimize("Os"), section(".text.flowhitch_late"))) void CtrAudioSystem_chainStopFlowhitch(void) {
    if (flowhitchKeeper > 1) CtrOpenFiles_removeKeeper(flowhitchKeeper - 2);
    flowhitchKeeper = 0;
    __atomic_store_n(&flowhitchCa, NULL, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&flowhitchWaking, __ATOMIC_SEQ_CST) != 0) svcSleepThread(100000);
    flowhitchShed(NULL);
    for (int k = 0; FLOWHITCH_PRE > k; k++) {
        free(flowhitchPre[k].pcm);
        flowhitchPre[k].pcm = NULL;
    }
    __real_CtrReadAhead_shutdown();
}

// Whether the guard shed sound si this room (the game thread's marks; a stale read only opens or closes a spare).
__attribute__((noinline, optimize("Os"), section(".text.flowhitch_late"))) static bool flowhitchMarked(int32_t si) {
    for (uint32_t i = 0; flowhitchCount > i; i++) {
        if (flowhitchSound[i] == si) return true;
    }
    return false;
}

// The open link after flowend's (opener thread, an open no other spare holds): a shed cue's spare, else the file.
__attribute__((noinline, noipa, optimize("Os"), section(".text.flowhitch_late"))) bool CtrAudioSystem_chainOpenFlowhitch(
    CtrDecoder* dec, const char* path) {
    bool ok = false;
    for (int j = 0; FLOWHITCH_SLOTS > j && !ok; j++) {
        if (flowhitchSlotPath[j] == NULL || strcmp(flowhitchSlotPath[j], path) != 0) continue;
        memcpy(dec, flowhitchSlot[j], sizeof(*dec));
        dec->vf.datasource = dec; // Tremor's callbacks get the decoder
        if (dec->vf.ready_state >= FLOWEND_INITSET) dec->vf.vb.vd = &dec->vf.vd;
        free(flowhitchSlot[j]);
        free(flowhitchSlotPath[j]);
        flowhitchSlotPath[j] = NULL;
        ok = true;
    }
    if (!ok) ok = __real_CtrDecoder_openFile(dec, path);
    // The sound of the stream that owns dec, if a cue (its first play caches it) or a shed one: the latest first.
    CtrAudioSystem* ca = gShutdownTarget;
    uintptr_t off = (uintptr_t) dec - (uintptr_t) &ca->streams[0].decoder;
    if (!ok || off % sizeof(StreamContext) != 0 || off / sizeof(StreamContext) >= STREAM_CONTEXTS) return ok;
    int32_t vi = ca->streams[off / sizeof(StreamContext)].owner;
    int32_t si = vi >= 0 && VOICE_COUNT > vi ? ca->voices[vi].soundIndex : -1;
    if (si < 0 || (uint32_t) si >= ca->soundCount || (ca->soundForms[si] == FORM_STREAMED && !flowhitchMarked(si))) return ok;
    int i = 0;
    while (FLOWHITCH_RECENT - 1 > i && flowhitchRecent[i] != si + 1) i++;
    for (; i > 0; i--) flowhitchRecent[i] = flowhitchRecent[i - 1];
    flowhitchRecent[0] = (int16_t) (si + 1);
    return ok;
}

// The note link before flowend's spare work (opener thread, at each wait): frees the copies no stream reads, decodes
// a held spare's first buffer, and at a quiet second with no other spare to open, keeps the shed cues' spares.
__attribute__((noinline, noipa, optimize("Os"), section(".text.flowhitch_late"))) void CtrAudioSystem_chainNoteFlowhitch(const char* state) {
    __real_CtrWatchdog_note(state);
    CtrAudioSystem* ca = gShutdownTarget;
    if (ca != NULL && threadGetCurrent() == ca->opener && strcmp(state, flowendWaiting) == 0) flowhitchOwner(ca);
    if (ca == NULL || flowendAheadOwner != ca || threadGetCurrent() != ca->opener || strcmp(state, flowendWaiting) != 0 || ca->openerQuit) return;
    if (flowhitchKeeper == 0) {
        __atomic_store_n(&flowhitchCa, ca, __ATOMIC_SEQ_CST);
        flowhitchKeeper = (int8_t) (CtrOpenFiles_addKeeper(flowhitchShed, flowhitchWake, NULL) + 2);
    }
    CtrOpenFiles_poll();
    for (int i = 0; STREAM_CONTEXTS > i; i++) {
        if (ca->streams[i].state == CTX_OPEN_REQUESTED) return;
    }
    flowhitchSongs(ca);
    CtrDecoder* held[FLOWHITCH_PRE]; // no initializer list: a constant copy would land in .rodata, before the late sections
    held[0] = flowendAheadState == 2 ? &flowendAhead : NULL;
    held[1] = flowendSongState == 2 ? &flowendSong : NULL;
    for (int j = 0; FLOWHITCH_SLOTS > j; j++) held[2 + j] = flowhitchSlotPath[j] != NULL ? flowhitchSlot[j] : NULL;
    for (int k = 0; FLOWHITCH_PRE > k; k++) {
#ifdef CTR_AUDIO_PCM_HASH
        if (!latPreparationOn()) continue; // Matched diagnostic control: leave a spare at source frame zero.
#endif
        FlowhitchPre* e = &flowhitchPre[k];
        CtrDecoder* d = held[k];
        if (e->pcm != NULL && (d == NULL || d->vf.vi != e->key || d->vf.pcm_offset != e->end)) { // its spare went or was read
            bool reading = false;
            for (int i = 0; STREAM_CONTEXTS > i; i++) {
                const CtrDecoder* sd = &ca->streams[i].decoder;
                if (sd->kind == CTR_DECODER_VORBIS && sd->vf.vi == e->key && e->frames > sd->positionFrames) reading = true;
            }
            if (reading) continue;
            free(e->pcm);
            e->pcm = NULL;
        }
        if (d == NULL || e->pcm != NULL || d->positionFrames != 0) continue;
        uint32_t ch = d->channels, got = 0;
        int16_t* pcm = malloc(FLOWHITCH_PRE_FRAMES * ch * sizeof(int16_t));
        int32_t r = 1;
        while (pcm != NULL && FLOWHITCH_PRE_FRAMES > got && r > 0) {
            r = __real_CtrDecoder_read(d, pcm + got * ch, FLOWHITCH_PRE_FRAMES - got);
            if (r > 0) got += (uint32_t) r;
        }
        d->positionFrames = 0;
        e->key = d->vf.vi;
        e->end = d->vf.pcm_offset;
        e->frames = got;
        e->pcm = pcm;
        return; // one decode ahead per wait
    }
    if (flowendAheadState == 1 || flowendGuessDue || flowendSongState == 1 || svcGetSystemTick() - flowendLastOpen < (uint64_t) (FLOWEND_AHEAD_IDLE_MS * CPU_TICKS_PER_MSEC))
        return;
    int32_t want[FLOWHITCH_SLOTS]; // the three shed cues played most recently
    int w = 0;
    for (int i = 0; FLOWHITCH_RECENT > i && FLOWHITCH_SLOTS > w; i++) {
        if (flowhitchRecent[i] != 0 && flowhitchMarked(flowhitchRecent[i] - 1)) want[w++] = flowhitchRecent[i] - 1;
    }
    while (FLOWHITCH_SLOTS > w) want[w++] = -1;
    for (int j = 0; FLOWHITCH_SLOTS > j; j++) {
        bool keep = flowhitchSlotPath[j] == NULL;
        for (w = 0; FLOWHITCH_SLOTS > w; w++) keep |= flowhitchSlotSound[j] == want[w];
        if (!keep) {
            CtrDecoder_close(flowhitchSlot[j]);
            free(flowhitchSlot[j]);
            free(flowhitchSlotPath[j]);
            flowhitchSlotPath[j] = NULL;
            return;
        }
    }
    for (w = 0; FLOWHITCH_SLOTS > w; w++) {
        int32_t si = want[w];
        int j = -1;
        bool held = false;
        for (int i = 0; FLOWHITCH_SLOTS > i; i++) {
            if (flowhitchSlotPath[i] == NULL) j = i;
            else if (flowhitchSlotSound[i] == si) held = true;
        }
        if (si < 0 || (uint32_t) si >= ca->soundCount || si + 1 == flowhitchNoFile || held || j < 0) continue;
        const Sound* sound = &ca->base.audioGroups[0]->sond.sounds[si];
        char* path = (sound->flags & (AUDIO_ENTRY_FLAG_REGULAR | AUDIO_ENTRY_FLAG_IS_EMBEDDED | AUDIO_ENTRY_FLAG_IS_COMPRESSED)) ==
                             AUDIO_ENTRY_FLAG_REGULAR
                         ? resolveExternalPath(ca, sound)
                         : NULL; // a file of its own, as playSoundImpl streams it
        CtrDecoder* d = malloc(sizeof(CtrDecoder));
        bool ok = path != NULL && d != NULL && __real_CtrDecoder_openFile(d, path);
        if (ok && d->kind != CTR_DECODER_VORBIS) {
            CtrDecoder_close(d);
            ok = false;
        }
        flowendLastOpen = svcGetSystemTick();
        if (ok) {
            flowhitchSlot[j] = d;
            flowhitchSlotPath[j] = path;
            flowhitchSlotSound[j] = (int16_t) si;
        } else {
            free(d);
            free(path);
            flowhitchNoFile = (int16_t) (si + 1);
        }
        return;
    }
}

// The FNV-1a hash of a name's last part before its extension (flowendSongHash's; its own copy: a new caller of a
// flowend function reorders flowend's late code).
__attribute__((noinline, optimize("Os"), section(".text.flowhitch4_late"))) static uint32_t flowhitchFileHash(const char* file) {
    const char* base = strrchr(file, '/');
    base = base != NULL ? base + 1 : file;
    const char* dot = strrchr(base, '.');
    return RoomPlan_fnv1a((const uint8_t*) base, dot != NULL ? (size_t) (dot - base) : strlen(base));
}

// Game thread: a room change, into the musical's room or not (a static function's stores: the chain link's own
// would reorder flowend's late statics).
__attribute__((noinline, optimize("Os"), section(".text.flowhitch4_late"))) static void flowhitchRoomMark(uint8_t is) {
    __atomic_store_n(&flowhitchRoomIs, is, __ATOMIC_RELAXED);
    __atomic_add_fetch(&flowhitchRoomSeq, 1, __ATOMIC_RELEASE);
}

#if defined(CTRPORT_TARGET_O3DS) && !defined(CTRPORT_DODGEFRONT)
#error "UNDERTALE Old: this file needs CTRPORT_DODGEFRONT (CMakeLists.txt), or its link skips textfront's chain"
#endif
void CTR_PREFETCHROOM_AFTER_FLOWHITCH(struct Renderer* renderer);
// The prefetchRoom chain's link after roomplan's (game thread, once per room change).
__attribute__((noinline, noipa, optimize("Os"), section(".text.flowhitch4_late"))) void CtrRenderer_chainFlowhitch(struct Renderer* renderer) {
    Runner* runner = renderer->runner;
    DataWin* dw = renderer->dataWin;
    int32_t room = runner != NULL ? runner->currentRoomIndex : -1;
    const char* name = dw != NULL && room >= 0 && dw->room.count > (uint32_t) room ? dw->room.rooms[room].name : NULL;
    flowhitchRoomMark(name != NULL && flowhitchFileHash(name) == FLOWHITCH_SONG_ROOM);
    CTR_PREFETCHROOM_AFTER_FLOWHITCH(renderer);
}

// Opener thread, at a wait: the spares' owner. A game that has opened no file yet (a bench that boots into the musical's
// room; the first open sets it) gets the opener's system, so the room's first song can open ahead. A spare an earlier
// system left still waits for an open to drop it (the open wrap's own switch). The static's address comes through asm:
// a new C reference to it reorders flowend's late statics; noipa and the clobber make the callers load it again.
__attribute__((noinline, noipa, optimize("Os"), section(".text.flowhitch4_late"))) static void flowhitchOwner(CtrAudioSystem* ca) {
    CtrAudioSystem** owner;
    // The address word sits inside this function (a pool the assembler adds at the section end is in no symbol).
    __asm__ volatile("ldr %0, 1f\n\tb 2f\n1:\t.word flowendAheadOwner\n2:" : "=r"(owner) : : "memory");
    if (*owner == NULL) *owner = ca;
    __asm__ volatile("" : : : "memory");
}

// Opener thread: closes the phase-song spare if it is still the one this asked for (hash: that song's).
__attribute__((noinline, optimize("Os"), section(".text.flowhitch4_late"))) static void flowhitchSongLeave(uint32_t hash) {
    if (flowendSongState != 0 && flowhitchFileHash(flowendSongPath) == hash) {
        if (flowendSongState == 2) CtrDecoder_close(&flowendSong);
        flowendSongState = 0;
    }
    flowhitchSongMine = 0;
}

// Opener thread, at each wait (before the spares' quiet-second work): asks the phase-song spare for the musical's songs.
__attribute__((noinline, optimize("Os"), section(".text.flowhitch4_late"))) static void flowhitchSongs(CtrAudioSystem* ca) {
    if (flowendSongState == 0) flowhitchSongMine = 0;
    uint32_t seq = __atomic_load_n(&flowhitchRoomSeq, __ATOMIC_ACQUIRE);
    uint8_t in = __atomic_load_n(&flowhitchRoomIs, __ATOMIC_RELAXED);
    if (seq != flowhitchRoomSeen) {
        // A room change: the spare this asked for in the room before (either song) is dropped first.
        if (flowhitchSongMine != 0) flowhitchSongLeave(flowhitchSongMine == 1 ? FLOWHITCH_SONG_AGAIN : FLOWHITCH_SONG_FIRST);
        if (in && flowendSongState != 0) return; // the spare is busy: asked again at a later wait
        flowhitchRoomSeen = seq;
        for (uint32_t i = 0; in && ca->base.audioGroups[0]->sond.count > i; i++) {
            const Sound* sound = &ca->base.audioGroups[0]->sond.sounds[i];
            if (sound->file == NULL || flowhitchFileHash(sound->file) != FLOWHITCH_SONG_FIRST) continue;
            char* path = resolveExternalPath(ca, sound);
            if (path != NULL && FLOWEND_AHEAD_PATH > strlen(path)) {
                strcpy(flowendSongPath, path);
                flowendSongState = 1;
                flowhitchSongMine = 2;
            }
            free(path);
            break;
        }
    }
    if (!in) return; // the repeated song is asked for only in the musical's room
    // The file opened last (flowendLastPath; the hash of a short name, once a wait).
    uint32_t last = flowhitchFileHash(flowendLastPath);
    // The first song opened some other way (cold: its spare was not open yet): its request is dropped, so the spare is
    // free for the repeated song.
    if (flowhitchSongMine == 2 && last == FLOWHITCH_SONG_FIRST) flowhitchSongLeave(FLOWHITCH_SONG_FIRST);
    if (last != FLOWHITCH_SONG_AGAIN) {
        flowhitchAgainDone = 0;
        if (flowhitchSongMine == 1) flowhitchSongLeave(FLOWHITCH_SONG_AGAIN);
    } else if (!flowhitchAgainDone && flowendSongState == 0) {
        flowhitchAgainDone = 1;
        memcpy(flowendSongPath, flowendLastPath, FLOWEND_AHEAD_PATH);
        flowendSongState = 1;
        flowhitchSongMine = 1;
    }
}
#endif
#line 5562 // flowhitch rev 3: main's numbering (the __LINE__ trap)
__attribute__((noinline, optimize("Os"), section(".text.flowend_late"))) bool __wrap_CtrDecoder_openFile(CtrDecoder* dec, const char* path) {
    CtrAudioSystem* ca = gShutdownTarget;
    if (ca == NULL || ca->opener == NULL || threadGetCurrent() != ca->opener) return __real_CtrDecoder_openFile(dec, path);
    if (flowendAheadOwner != ca) {
        flowendDropAhead();
#ifdef ENABLE_WAD16
        flowendSongDrop();
#endif
    }
    flowendAheadOwner = ca;
    flowendLastOpen = svcGetSystemTick();
    bool ok = true;
    if (flowendAheadState == 2 && strcmp(flowendAheadPath, path) == 0) {
        memcpy(dec, &flowendAhead, sizeof(*dec));
        dec->vf.datasource = dec; // Tremor's callbacks get the decoder
        if (dec->vf.ready_state >= FLOWEND_INITSET) dec->vf.vb.vd = &dec->vf.vd;
        flowendAheadState = 0;
    } else
#ifdef ENABLE_WAD16
    if (!flowendSongTake(dec, path))
#endif
    {
        ok = FLOWHITCH_OPEN_NEXT(dec, path);
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
#ifdef ENABLE_WAD16 // phase pad: the late code after this keeps stage72's phase mod 32
#ifdef CTRPORT_TARGET_O3DS
    __asm__ volatile("nop; nop; nop; nop; nop; nop");
#else
    
#endif
#endif
    FLOWHITCH_NOTE_NEXT(state);
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
#ifdef ENABLE_WAD16
    if ((flowendAheadState != 1 && !flowendGuessDue && flowendSongState != 1) || flowendAheadOwner != ca || ca->openerQuit ||
        FLOWEND_AHEAD_IDLE_MS > TICKS_TO_MS(t0 - flowendLastOpen))
        return;
#else
    if ((flowendAheadState != 1 && !flowendGuessDue) || flowendAheadOwner != ca || ca->openerQuit || FLOWEND_AHEAD_IDLE_MS > TICKS_TO_MS(t0 - flowendLastOpen)) return;
#endif
    for (int i = 0; STREAM_CONTEXTS > i; i++) {
        if (ca->streams[i].state == CTX_OPEN_REQUESTED) return;
    }
    if (flowendGuessDue) {
        flowendGuessDue = 0;
        flowendGuessNext(ca, flowendLastPath);
        FLOWEND_SONG_OPEN
        if (flowendAheadState != 1) return;
    }
    FLOWEND_SONG_OPEN
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

// change stallfix (rev 3, ctr_benchheld.inc): the game thread's sound load time in this stats window so far (logWindowStats
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
// ===[ flowpitch: every voice at the game's frame clock (late code, both games; change flowend, 2026-10-28) ]===
// Every rate this file sets goes through here (ctr_audio_utaudio.h's ndspChnSetRate): each voice, music, sound effect
// or stream, gets its rate times CTR_MUSIC_RATE_SCALE (ctr_music_clock.h), so its samples keep pace with the game's
// 29.9156 frames a second, as the whole game does (review decision 10-28: no music/sfx split, nothing to classify). Set
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
//   A clear or a reset (ndspChnWaveBufClear, ndspChnReset) only stages the cut: libctru marks the buffers done and
//   the NDSP thread hands the change to the DSP after a later frame callback, so the DSP may still read what the
//   uta layer has just forgotten (utaClearPhys and utaChnReset forget the channel's pieces at once). Every block
//   therefore may be read while a clear or reset is under way and until PCMWAIT_CUT_CALLBACKS frame callbacks have
//   run since the latest one was staged (both are wrapped, CMakeLists.txt: the wrap counts the calls under way and,
//   after the call, notes utaCb, the frame callbacks so far, keeping the latest; ~20 ms).
// - uta shut down but the DSP still on (shutdownAudio between ndspSetCallback(NULL) and ndspExit: the channels' states
//   are no longer kept, the DSP still plays what was queued): every block may be read, so every free waits.
// - DSP off (before the uta callback is set, or after ndspExit: both wrapped, a flag of this code's own, so no
//   check reads the audio system, which UNDERTALE's detached sound-plan worker or a texture worker can outlive): nothing
//   reads linear memory, so the whole list is freed and every free is at once. shutdownAudio's frees after ndspExit
//   (the stream buffers, then the cached sounds) so drain the list after ndspChnReset and ndspExit, at UNDERTALE's exit
//   and at DELTARUNE's game change (nothing waiting lives on into the next chapter).
// The list has no size limit and is lock-free (a note is pushed with a compare-and-swap; a flush takes the whole list
// with an exchange and pushes back what is still read), so any thread may use it, under any lock. Notes come from a
// fixed pool (PCMWAIT_NOTES, more than the blocks the channels can read at once), then the heap without relief; with
// neither the block is kept for good (logged, its bytes counted against the cache for good), never freed under the DSP.
// A scan re-checks utaActive under the lock, and the wrapped ndspSetCallback(NULL) (shutdownAudio's first step, after
// utaActive goes false) takes and drops the lock once, so no scan still reads adpcm's metadata when shutdown frees it.
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
void __wrap_linearFree(void* mem); void audofixFreeLink(void* mem);
u32 __wrap_linearSpaceFree(void);
void* __real_malloc(size_t size);
void __real_free(void* ptr);
void* __real_CtrMemory_linearAllocOnce(size_t size);
void* __wrap_CtrMemory_linearAllocOnce(size_t size);
void CtrAudio_pcmWaitFree(void* mem);
u32 CtrAudio_pcmWaitSpaceFree(void);
void __real_ndspChnReset(int id);
void __real_ndspChnWaveBufClear(int id);
void __real_ndspSetCallback(ndspCallback callback, void* data);
void __real_ndspExit(void);
void __wrap_ndspChnReset(int id);
void __wrap_ndspChnWaveBufClear(int id);
void __wrap_ndspSetCallback(ndspCallback callback, void* data);
void __wrap_ndspExit(void);
#define PCMWAIT_CUT_CALLBACKS 4u // staged in one NDSP frame, sent the next, the DSP's frame after; one more to spare
static uint32_t pcmWaitCutCb PCMWAIT_BSS; // utaCb at the latest clear or reset staged (it only moves forward)
static uint32_t pcmWaitCutting PCMWAIT_BSS; // clears and resets under way (entered, not yet stamped)
static volatile uint8_t pcmWaitDspOn PCMWAIT_BSS; // the uta callback set and ndspExit not yet returned
// this file's linearFree and linearSpaceFree calls (and only this file's)
__asm__(".set linearFree, CtrAudio_pcmWaitFree\n.set linearSpaceFree, CtrAudio_pcmWaitSpaceFree\n");

typedef struct {
    UtaChan* channels;
    ndspWaveBuf (*buffers)[UTA_RING];
    LightLock* lock;
    volatile bool* active;
    CtrAudioSystem* volatile* target;
    volatile uint32_t* callbacks;
    CtrAudioSystem* volatile* uta;
} PcmWaitUtaView;
extern const PcmWaitUtaView ctrPcmWaitUta;
__asm__(".pushsection .rodata.pcmwait_in_text,\"a\",%progbits\n"
        ".p2align 2\n.global ctrPcmWaitUta\n"
        ".type ctrPcmWaitUta,%object\nctrPcmWaitUta:\n"
        ".word utaChan, utaWb, utaLock, utaActive, gShutdownTarget, utaCb, utaCa\n"
        ".size ctrPcmWaitUta, .-ctrPcmWaitUta\n.popsection\n");
static const char pcmWaitLog[] __attribute__((section(".rodata.pcmwait_in_text"))) =
    "Audio (pcm wait): a cached sound's PCM (%lu KB) is still read by a stopped channel; its free waits (%lu so far)\n";
static const char pcmWaitKeptLog[] __attribute__((section(".rodata.pcmwait_in_text"))) =
    "Audio (pcm wait): no note for a waiting block; its PCM (%lu KB) is kept, never freed\n";

enum { PCMWAIT_SCAN, PCMWAIT_KEEP, PCMWAIT_DSP_OFF };
// What a check can know: uta running (scan its channels), uta shut down with the DSP still on (keep everything), or
// the DSP off (nothing reads linear memory). Reads only statics: the audio system may be gone.
PCMWAIT_TEXT static int pcmWaitState(void) {
    if (*ctrPcmWaitUta.active) return PCMWAIT_SCAN;
    return pcmWaitDspOn ? PCMWAIT_KEEP : PCMWAIT_DSP_OFF;
}

// A clear or reset under way, or one staged fewer than PCMWAIT_CUT_CALLBACKS frame callbacks ago (under the uta lock:
// the callback count is written under it). The count of those under way is read first: once it reads 0, the stamp of
// every clear that has left is seen.
PCMWAIT_TEXT static bool pcmWaitCutPending(const PcmWaitUtaView* v) {
    if (__atomic_load_n(&pcmWaitCutting, __ATOMIC_ACQUIRE) != 0) return true;
    return PCMWAIT_CUT_CALLBACKS > *v->callbacks - __atomic_load_n(&pcmWaitCutCb, __ATOMIC_RELAXED);
}

// Whether the uta layer may still read [pcm, pcm + size): a stopped channel's segments (PENDING: its tail is built from
// them next; TAILING: what it queued before the tail still plays), any queued or playing DSP buffer in the block, or
// (DSP-ADPCM) a channel's decoder reading the cached copy: 1. Can't tell (the lock taken, uta shut down with the DSP
// on, a clear or reset not yet seen by the DSP): 2. Not read: 0.
PCMWAIT_TEXT static int pcmWaitBusy(const uint8_t* pcm, size_t size) {
    const PcmWaitUtaView* v = &ctrPcmWaitUta;
    int state = pcmWaitState();
    if (state != PCMWAIT_SCAN) return state == PCMWAIT_KEEP ? 2 : 0;
    if (pcm == NULL || size == 0) return 0;
    // Under the lock the frame callback doesn't run, and once a voice is stopped it moves its channel only toward idle
    // (PENDING to TAILING after its tail is copied out, buffers QUEUED to PLAYING to DONE); uta shut down after the
    // check above only freezes the states it saw.
    if (LightLock_TryLock(v->lock) != 0) return 2;
    if (!*v->active) { // shut down since the check above: the DSP is still on (the wrapped ndspSetCallback(NULL) waits
        LightLock_Unlock(v->lock); // for this lock), and adpcm's metadata may go once the lock is dropped
        return 2;
    }
    int busy = 0;
    for (int p = 0; UTA_CHANNELS > p && !busy; p++) {
        const UtaChan* c = &v->channels[p];
        if (c->state == UTA_PENDING || c->state == UTA_TAILING) {
            for (int i = 0; c->segs > i && 2 > i; i++) {
                const uint8_t* d = c->seg[i].data;
                if (d >= pcm && pcm + size > d) busy = 1;
            }
        }
        for (int k = 0; UTA_RING > k && !busy; k++) {
            const ndspWaveBuf* w = &v->buffers[p][k];
            const uint8_t* d = (const uint8_t*) w->data_vaddr;
            if ((w->status == NDSP_WBUF_QUEUED || w->status == NDSP_WBUF_PLAYING) && d >= pcm && pcm + size > d) busy = 1;
        }
#if defined(CTRPORT_ADPCM_SFX) || defined(CTRPORT_ADPCM_MUSIC) || defined(CTRPORT_ADPCM_SFXPACK)
        const AdSfxMeta* m = adSfx.physical[p].meta;
        if (m != NULL && m->data != NULL && m->data >= pcm && pcm + size > m->data) busy = 1;
#endif
    }
    // Not seen read, but a cut the DSP may not have taken yet.
    if (!busy && pcmWaitCutPending(v)) busy = 2;
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
            AUDOFIX_FREE_LINK(n->pcm);
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
    int busy = size != 0 ? pcmWaitBusy(pcm, size) : 0;
    if (busy != 0) {
        if (busy == 1) { // a channel is seen to read it (not a cut the DSP may not have taken yet)
            uint32_t ever = __atomic_add_fetch(&pcmWaitEver, 1, __ATOMIC_RELAXED);
            if (16 >= ever || (ever & (ever - 1)) == 0) // the first 16, then 32, 64...
                logInfo(pcmWaitLog, (unsigned long) (size / 1024u), (unsigned long) ever);
        }
        PcmWaitNote* n = pcmWaitNew();
        if (n == NULL) {
            logInfo(pcmWaitKeptLog, (unsigned long) (size / 1024u));
            __atomic_fetch_add(&pcmWaitBytes, size, __ATOMIC_RELAXED); // lost for good: it counts against the cache
            return;
        }
        n->pcm = pcm;
        n->size = size;
        __atomic_fetch_add(&pcmWaitBytes, size, __ATOMIC_RELAXED);
        __atomic_fetch_add(&pcmWaitPending, 1, __ATOMIC_RELAXED);
        pcmWaitPush(n);
        return;
    }
    AUDOFIX_FREE_LINK(mem);
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

// The cut stamp moves only forward: a clear that read the count and was held up before storing it can't put back an
// older count over a later clear's (that later clear's wait would end early). The count is read after the stamp it
// replaces, so it is never older than that stamp's (a later stamp makes the swap fail and the count is read again).
// No comparison of counts: a stamp of any age is replaced (none is too old to compare, flushrev F2).
PCMWAIT_TEXT static void pcmWaitStampCut(void) {
    uint32_t cur = __atomic_load_n(&pcmWaitCutCb, __ATOMIC_ACQUIRE);
    uint32_t now;
    do {
        now = *ctrPcmWaitUta.callbacks;
    } while (!__atomic_compare_exchange_n(&pcmWaitCutCb, &cur, now, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

// Every clear and reset (any file; CMakeLists.txt's wraps): the DSP takes the cut a few frame callbacks after libctru
// stages it. Under way (counted from entry, so however long the caller is held up before or inside the call) every
// block may be read; after it the stamp is a count read once it is staged, so the wait starts there.
PCMWAIT_TEXT static void pcmWaitCut(void (*real)(int), int id) {
    __atomic_fetch_add(&pcmWaitCutting, 1, __ATOMIC_SEQ_CST);
    real(id);
    pcmWaitStampCut();
    __atomic_fetch_sub(&pcmWaitCutting, 1, __ATOMIC_RELEASE);
}

PCMWAIT_TEXT void __wrap_ndspChnReset(int id) {
    pcmWaitCut(__real_ndspChnReset, id);
}

PCMWAIT_TEXT void __wrap_ndspChnWaveBufClear(int id) {
    pcmWaitCut(__real_ndspChnWaveBufClear, id);
}

// The uta callback set: the DSP is on. Taken away (shutdownAudio, utaActive already false): once every scan that saw
// uta running is done (the lock taken and dropped), shutdown goes on.
PCMWAIT_TEXT void __wrap_ndspSetCallback(ndspCallback callback, void* data) {
    const PcmWaitUtaView* v = &ctrPcmWaitUta;
    if (callback != NULL) {
        pcmWaitDspOn = 1;
    } else if (*v->uta != NULL) { // the lock is set up (utaSetCallback)
        LightLock_Lock(v->lock);
        LightLock_Unlock(v->lock);
    }
    __real_ndspSetCallback(callback, data);
}

// The DSP off once ndspExit returns (its NDSP thread joined).
PCMWAIT_TEXT void __wrap_ndspExit(void) {
    __real_ndspExit();
    pcmWaitDspOn = 0;
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

#ifdef ENABLE_WAD16
// ===[ usstop: the sound plan's worker is idle before the audio system's memory goes (UT-002, credfix 2026-10-29) ]===
//
// The copy worker above is detached and never stopped. At the exit it could still be inside an FS read into linear
// memory (usw.piece, a copy's PCM) while shutdownAudio freed the sound cache and libctru's __libctru_exit unmapped the
// linear heap and the heap (its stack) before svcExitProcess: memory freed under a thread still in the kernel, the
// class of the o2DS end-of-run kernel panics (docs/3ds-hardware.md). shutdownAudio's one CtrReadAhead_shutdown call
// comes after the decode threads are joined and before ndspChnReset, ndspExit and the frees; UNDERTALE's link wraps
// it (CMakeLists.txt), so shutdownAudio keeps its code. The stop holds the worker (usw.held, under its lock: it takes
// no new job) and waits up to 2 s for a copy under way to finish (one sound). The worker then waits on its event, a
// static address, with no read in flight and no memory of its own in use; the process ends it there. (Holding rather
// than ending it leaves the worker and every existing function byte for byte as they are.)
//
// A copy still under way after 2 s (an SD read that does not come back) must not have memory unmapped under it, and
// the exit must still end (no wait without an end: a read that never returns would leave the player on a frozen
// screen). So the wait only records the request (usWant), and the thread that really owns the exit decides, at its
// own aptExit, from what it can see there (rev 3, after c26rev round 10):
// - The owner is the one thread past z_leavekeep.c's CTR_EXIT_ONCE: main returning from main, or a filesRecover or
//   lab leave thread (CtrLeavekeep_exit). Only the owner runs __appExit and its aptExit (a lab leave also calls
//   aptExit itself, earlier, before the gate: see the return address below).
// - It keeps the heap when main asked (usWant) or when its own exit is already a keep exit (lkKeepHeap = itself: a
//   filesRecover owner keeps it whether main's wait has run out yet or not). CtrLeavekeep_ownerKeep adds the owner
//   to lkEntered, so __wrap_svcControlMemory's "a second thread is in the exit" test holds: a count that only grows,
//   so no other thread can undo the keep (rev 2 overwrote lkKeepHeap with main's id, and a filesRecover owner's
//   frees then went through; c26rev r10 F1).
// - The keep needs no return address at the frees. The first thread into this aptExit records, once (an atomic
//   compare-and-swap, before it calls the real aptExit), whether the loader gave a return address: no aptExit has
//   run before that, so the address it reads is the loader's. Every later entry, the owner's too, uses that record:
//   a lab leave's own aptExit (ctr_lab.c, before it reaches the exit gate) may have set aptExit's address already, so
//   reading the address again at the owner would take it for a loader's (rev 4, c26rev r11b). When the loader gave
//   none and aptExit set its own (aptExitProcess, for an application that is not chainloading: a .3dsx on Azahar, a
//   CIA), the address goes aside until svcExitProcess, where libctru's __libctru_exit would have called it (after
//   envDestroyHandles and __sync_fini). Main never reads the return address. A loader that gave an address of its
//   own keeps the old exit: it gets its memory back as before.
// - A keep on a record of "no loader address" is final (rev 5, c26rev r11f): it sets usVeto, and every MEMOP_FREE
//   after that is skipped below z_leavekeep.c's test (its call to the real svcControlMemory comes here first). That
//   test also wants a NULL return address, and a lab leave's aptExit that overlaps the owner's (past its last
//   reference, not yet at its store of aptExitProcess) can set one after the owner's wrap has looked: the frees
//   would then go through. Nothing clears usVeto. That late address is libctru's own hand-back, called once by
//   __libctru_exit after the skipped frees, as without this fix.
// - svcExitProcess's wrap lets only the owner take the set-aside address, with an atomic exchange: the 15 s waiter in
//   CTR_EXIT_ONCE and main.c's out-of-memory exit go straight to svcExitProcess as before (c26rev r10 F4).
bool CtrLeavekeep_ownerKeep(bool want);
#define USSTOP_TEXT __attribute__((noinline, section(".text.usstop_late")))
#define USSTOP_RO __attribute__((section(".rodata.usstop_in_text")))
#define USSTOP_BSS __attribute__((section(".bss.usstop_late")))
extern void (*__system_retAddr)(void);
static bool usWant USSTOP_BSS;            // the wait ran out with a copy still under way
static void (*usExitTo)(void) USSTOP_BSS; // aptExit's return address, set aside by the owner until svcExitProcess
static u32 usLoader USSTOP_BSS;           // 0 before the first aptExit, then 1: the loader gave none, 2: it gave one
// usVeto: the owner kept the heap with no loader address, so no more heap frees. Its own section, placed after every
// existing late BSS selector (c26rev r11g): in .bss.usstop_late it would move usExitTo and the late statics after it.
static bool usVeto __attribute__((section(".bss.usstop_veto")));
#ifdef CTRPORT_EXITTRAIL
// The provoke test (exit trail builds only, --ini usprovoke=N, or N in the game folder's usprovoke.txt for Azahar,
// which passes no arguments): a thread like the worker (its core, priority and stack size) holds a job slot BUSY and
// reads the data file in 32 KB pieces into usw.piece, back to back, so an FS read is in flight when the exit frees
// memory. 1: the stop is skipped (cand25's exit: the reads run until the process ends). 2: the stop holds it; the
// reader ends its job at the next piece, as the worker would after its copy. 3 to 12: the reader never ends its job
// (a copy whose read does not come back), so the wait runs out:
// 3: main owns the exit; the heap is kept (rev 3).
// 4: as 3 without the keep (099bfe4c4's exit, the control arm: the heap is unmapped under the reader).
// 5: c26rev r10 F1's order on rev 3: 5 ms into the wait a thread G takes the exit as filesRecover does
//    (CtrLeavekeep_exit: keep = owner = G) and stops before aptExit until main's wait has run out and marked; main
//    then sleeps 8 s (still in its teardown, before its own _exit) while G's exit runs on.
// 6: as 5 with rev 2's mark and srvExit (542eaccb4: keep = main, the return address set aside at srvExit), the
//    control arm for F1.
// 7: c26rev r10 F4 on rev 3: as 3, plus a thread W that calls svcExitProcess (as the 15 s waiter and main.c's
//    out-of-memory exit do) while the owner holds the loaded set-aside address in svcExitProcess's wrap.
// 8: as 7 with rev 2's take (load, clear, call: no owner test, no exchange), the control arm for F4.
// 9: c26rev r11b F1 on rev 4: as 5, and after main has marked, a thread L calls aptExit as a lab leave does before
//    the exit gate (ctr_lab.c) and returns (it is the first aptExit: the real one sets aptExitProcess as the return
//    address); only then does G go into its aptExit.
// 10: as 9 with rev 3's own read of the return address at G's aptExit, the control arm for r11b.
// 11: c26rev r11f on rev 5: as 9, but L stops INSIDE the real aptExit, past its last reference and before its
//    store of aptExitProcess (at its first srvGetServiceHandleDirect: the APT command aptExit sends just before that
//    store). Main marks once L has stopped; G runs its whole aptExit wrap (the keep and the veto, with no address to
//    set aside), then lets L go on and waits until L's aptExit has returned (the store done) before its frees.
// 12: as 11 without the veto (rev 4), the control arm for r11f.
// The exit's log is closed before atexit runs shutdownAudio, so the test's lines go to the boot trail (labboot.txt,
// lab runs), to usstop.txt in the game folder, and to Azahar's log (svcOutputDebugString; usSay makes Azahar flush).
// With usprovoke.txt (Azahar), the run also leaves after its bench as --exit-after would.
// The test's code and constants go in the ordinary .text and .rodata (test builds only: the late code reserve
// has no room for them), compiled for size: in these builds the late code starts after the ordinary code, so test
// code also comes out of the late reserve.
#define USSTOP_TTEXT __attribute__((noinline, optimize("Os"), section(".text.usstop_test")))
#define USSTOP_TRO __attribute__((section(".rodata.usstop_test")))
#include <stdio.h>
#include "ctr_launch_args.h"
CtrLaunchArgs* CtrBenchIo_launch(void);
void CtrLeavekeep_state(u32* out);
void CtrLeavekeep_rev2Keep(void);
void CtrLeavekeep_exit(int status) __attribute__((noreturn));
static const char usProvokeKey[] USSTOP_TRO = "usprovoke=";
static const char usProvokeFile[] USSTOP_TRO = "sdmc:/3ds/undertale-3ds/usprovoke.txt";
static const char usProvokeFileMode[] USSTOP_TRO = "r";
static const char usKeptLog[] USSTOP_TRO = "usstop: a copy is still under way: the heap is kept for the exit\n";
static const char usUnkeptLog[] USSTOP_TRO = "usstop: a copy is still under way: control arm, the heap is NOT kept\n";
static const char usRev2Log[] USSTOP_TRO = "usstop: a copy is still under way: rev 2's mark (control arm)\n";
static int usMode USSTOP_BSS;
static const char usTrailPath[] USSTOP_TRO = "sdmc:/3ds/undertale-3ds/usboot.txt";
static volatile uint32_t usStackAt USSTOP_BSS; // a word of the stalled reader's stack (heap)
static const char usExitBoth[] USSTOP_TRO = "usstop: after the heap frees: the copy's buffer and its stack are still mapped";
static const char usExitGone[] USSTOP_TRO = "usstop: after the heap frees: the copy's buffer and its stack are UNMAPPED";
static const char usExitMixed[] USSTOP_TRO = "usstop: after the heap frees: the copy's buffer or its stack is UNMAPPED";
static volatile uint32_t usSpinCount USSTOP_BSS;
static const char usExitRan[] USSTOP_TRO = "usstop: after it the stalled copy went on writing its buffer and stack for 50 ms";
static const char usExitStill[] USSTOP_TRO = "usstop: after it the stalled copy did NOT run";
static const char usRetLog[] USSTOP_TRO = "usstop: loader return address 0x%08lx\n";
static const char usNotePath[] USSTOP_TRO = "sdmc:/3ds/undertale-3ds/usstop.txt";
static const char usNoteMode[] USSTOP_TRO = "a";
static char usNoteText[160] USSTOP_BSS;
static volatile uint32_t usG USSTOP_BSS;      // 5, 6, 9 to 12: G's thread id + 1
static volatile uint32_t usMarked USSTOP_BSS; // 5, 6, 9 to 12: main's wait has run out and marked
static volatile uint32_t usW USSTOP_BSS;      // 7, 8: W's thread id + 1
static volatile uint32_t usWGo USSTOP_BSS;    // 7, 8: the owner holds the loaded address
static volatile uint32_t usWDone USSTOP_BSS;  // 7, 8: W has taken it or gone past
static bool usR2Keep USSTOP_BSS;              // 6: rev 2's decision (return address NULL at the timeout)
static volatile uint32_t usLDone USSTOP_BSS;  // 9 to 12: L's aptExit has returned
static volatile uint32_t usL USSTOP_BSS;      // 11, 12: L's thread id + 1
static volatile uint32_t usLParked USSTOP_BSS; // 11, 12: L has stopped inside the real aptExit
static volatile uint32_t usLGo USSTOP_BSS;    // 11, 12: G's aptExit wrap is done; L goes on
static volatile uint32_t usVetoed USSTOP_BSS; // MEMOP_FREE calls the veto skipped
#define US_G(m) ((m) == 5 || (m) == 6 || (m) >= 9) // the modes with G
#define US_W(m) ((m) == 7 || (m) == 8)             // the modes with W
#include <stdarg.h>
#include "ctr_boot_trail.h"
USSTOP_TTEXT static u32 usSelf(void) {
    u32 id = 0;
    svcGetThreadId(&id, CUR_THREAD_HANDLE);
    return id + 1;
}
// Azahar's log: the line, then a failing svcOpenThread (an Error entry) so Azahar flushes the log to the file.
USSTOP_TTEXT static void usSay(const char* text) {
    svcOutputDebugString(text, (s32) strlen(text));
    Handle none;
    svcOpenThread(&none, CUR_PROCESS_HANDLE, 0xFFFFFFF0u);
}
USSTOP_TTEXT static void usNote(const char* format, ...) {
    va_list list;
    va_start(list, format);
    vsnprintf(usNoteText, sizeof(usNoteText), format, list);
    va_end(list);
    svcOutputDebugString(usNoteText, (s32) strlen(usNoteText));
    CtrBootTrail_mark(usNoteText);
    FILE* f = fopen(usNotePath, usNoteMode);
    if (f != NULL) {
        fputs(usNoteText, f);
        fclose(f);
    }
}
// z_leavekeep.c's state, hex, no stdio: keep, owner, entered, retAddr, frees skipped, frees passed on.
USSTOP_TTEXT static void usState(const char* head, u32* state) {
    CtrLeavekeep_state(state);
    static char line[112] USSTOP_BSS;
    static const char hex[] USSTOP_TRO = "0123456789abcdef";
    u32 n = 0;
    for (const char* c = head; *c != '\0'; c++) line[n++] = *c;
    for (int i = 0; 6 > i; i++) {
        line[n++] = ' ';
        for (int s = 28; s >= 0; s -= 4) line[n++] = hex[(state[i] >> s) & 15u];
    }
    line[n] = '\0';
    usSay(line);
    CtrBootTrail_mark(line);
}
// 3 to 12: right after __libctru_exit's heap frees (its next call is envDestroyHandles; svcExitProcess follows), is
// the stalled copy's memory still there? svcQueryMemory says, deterministically; the boot trail (its file system
// session still open here) and Azahar's log carry it. No stdio or heap: in 4, 6, 10 and 12 the heap is gone.
static const char usAfterHead[] USSTOP_TRO = "usstop: after the frees: leavekeep state";
static const char usVetoSkipped[] USSTOP_TRO = "usstop: after the frees: the veto skipped them";
void __real_envDestroyHandles(void);
void __wrap_envDestroyHandles(void);
USSTOP_TTEXT void __wrap_envDestroyHandles(void) {
    if (usMode >= 3) {
        MemInfo info;
        PageInfo page;
        uint32_t stack = usStackAt;
        bool buffer = R_SUCCEEDED(svcQueryMemory(&info, &page, (u32) (uintptr_t) usw.piece)) && info.state != MEMSTATE_FREE;
        bool stacked = stack != 0 && R_SUCCEEDED(svcQueryMemory(&info, &page, stack)) && info.state != MEMSTATE_FREE;
        const char* text = buffer && stacked ? usExitBoth : !buffer && !stacked ? usExitGone : usExitMixed;
        usSay(text);
        CtrBootTrail_mark(text);
        u32 state[6];
        usState(usAfterHead, state);
        if (usVetoed != 0) usSay(usVetoSkipped);
        // Give the stalled copy 50 ms to run (it writes its buffer and stack every 1 ms): in 4, 6, 10 and 12 Azahar
        // logs each write to the unmapped memory as an error (a console faults on the first).
        uint32_t spins = usSpinCount;
        svcSleepThread(50000000LL);
        text = usSpinCount != spins ? usExitRan : usExitStill;
        usSay(text);
        CtrBootTrail_mark(text);
    }
    __real_envDestroyHandles();
}
// srvExit, after z_exittrail.c's wrap (exit trail builds only): 6 does rev 2's srvExit (a return address aptExit set
// goes aside when rev 2's timeout saw none); 3 to 12 say, before __libctru_exit's frees, what z_leavekeep.c's test
// will decide (it runs on the same values next).
static const char usBeforeHead[] USSTOP_TRO = "usstop: before the frees: leavekeep state";
static const char usWillSkip[] USSTOP_TRO = "usstop: before the frees: the heap frees will be SKIPPED";
static const char usWillFree[] USSTOP_TRO = "usstop: before the frees: the heap frees will go THROUGH";
static const char usVetoSet[] USSTOP_TRO = "usstop: before the frees: the owner's veto is set";
void __real_srvExit(void);
void CtrSrvExit_chainUsstop(void);
USSTOP_TTEXT void CtrSrvExit_chainUsstop(void) {
    __real_srvExit();
    if (usMode == 6 && usR2Keep && __system_retAddr != NULL) {
        usExitTo = __system_retAddr;
        __system_retAddr = NULL;
    }
    if (usMode >= 3) {
        u32 s[6];
        usState(usBeforeHead, s);
        // Rev 5: the veto skips them whatever z_leavekeep.c's test says.
        bool veto = __atomic_load_n(&usVeto, __ATOMIC_ACQUIRE);
        if (veto) usSay(usVetoSet);
        usSay(veto || (s[3] == 0 && ((s[0] != 0 && s[0] == s[1]) || s[2] > 1)) ? usWillSkip : usWillFree);
    }
}
int __real_CtrLaunchArgs_sendFilesMB(const CtrLaunchArgs* args);
int __wrap_CtrLaunchArgs_sendFilesMB(const CtrLaunchArgs* args);
USSTOP_TTEXT int __wrap_CtrLaunchArgs_sendFilesMB(const CtrLaunchArgs* args) {
    FILE* f = fopen(usProvokeFile, usProvokeFileMode);
    if (f != NULL) {
        ((CtrLaunchArgs*) args)->exitAfter = true;
        fclose(f);
    }
    return __real_CtrLaunchArgs_sendFilesMB(args);
}
static const char usProvokeLog[] USSTOP_TRO = "usstop: provoke %d, %lu reads before the stop\n";
static const char usStopLog[] USSTOP_TRO = "usstop: idle after %lu ms (%s), %lu reads\n";
static const char usIdle[] USSTOP_TRO = "idle";
static const char usTimedOut[] USSTOP_TRO = "TIMED OUT";
USSTOP_TTEXT static void usProvokeMain(void* arg) {
    UtSfxJob* job = (UtSfxJob*) arg;
    int fd = open(usw.path, O_RDONLY);
    uint32_t at = 0;
    for (;;) {
        if (fd >= 0 && usReadAt(fd, at, usw.piece, UTSFX_PIECE)) at += UTSFX_PIECE;
        else at = 0;
        usSpinCount = usSpinCount + 1u;
        LightLock_Lock(&usw.lock);
        bool stop = usw.held && 3 > usMode;
        if (stop) job->state = UTSFX_JOB_FAILED;
        LightLock_Unlock(&usw.lock);
        if (stop) break;
        if (usw.held) {
            // 3 to 12: the read that does not come back. The job stays BUSY and the thread goes on using its stack
            // and usw.piece (heap and linear heap), 1 ms apart, until the process ends it or an unmap faults it.
            volatile uint32_t mark[8];
            usStackAt = (uint32_t) (uintptr_t) &mark[0];
            for (uint32_t n = 0;; n++) {
                mark[n & 7u] = n;
                usw.piece[n & (UTSFX_PIECE - 1u)] = (uint8_t) mark[n & 7u];
                usSpinCount = usSpinCount + 1u;
                svcSleepThread(1000000LL);
            }
        }
    }
    if (fd >= 0) close(fd);
}
USSTOP_TTEXT static int usProvoke(void) {
    int mode = 0;
    CtrLaunchArgs* args = CtrBenchIo_launch();
    for (int i = 0; args != NULL && args->iniCount > i; i++) {
        if (strncmp(args->ini[i], usProvokeKey, sizeof(usProvokeKey) - 1) == 0) mode = atoi(args->ini[i] + sizeof(usProvokeKey) - 1);
    }
    if (mode == 0) {
        FILE* f = fopen(usProvokeFile, usProvokeFileMode);
        if (f != NULL) {
            int c = fgetc(f);
            int d = fgetc(f);
            if (c >= '1' && c <= '9') mode = c - '0';
            if (mode != 0 && d >= '0' && d <= '9') mode = mode * 10 + (d - '0');
            fclose(f);
        }
    }
    if (1 > mode || mode > 12) return 0;
    usMode = mode;
    UtSfxJob* job = NULL;
    LightLock_Lock(&usw.lock);
    for (int i = 0; UTSFX_JOBS > i && job == NULL; i++) {
        if (usw.jobs[i].state == UTSFX_JOB_FREE) job = &usw.jobs[i];
    }
    if (job != NULL) job->state = UTSFX_JOB_BUSY;
    LightLock_Unlock(&usw.lock);
    if (job == NULL) return 0;
    CtrAudioSystem* ca = gShutdownTarget;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    priority += 2;
    if (priority > 0x3F) priority = 0x3F;
    int core = ca == NULL || ca->threadCore == 0 ? -2 : ca->threadCore;
    if (threadCreate(usProvokeMain, job, UTSFX_STACK, priority, core, true) == NULL) {
        job->state = UTSFX_JOB_FREE;
        return 0;
    }
    svcSleepThread(200000000LL); // reading by now
    usNote(usProvokeLog, mode, (unsigned long) usSpinCount);
    // 3 to 12: the loader's return address (NULL on Luma3DS's hb:ldr and on Azahar; aptExit may set its own later).
    if (mode >= 3) {
        // The boot trail outlives fsExit (z_exittrail.c); a lab run has it open already (labboot.txt).
        CtrBootTrail_open(usTrailPath);
        usNote(usRetLog, (unsigned long) (uintptr_t) __system_retAddr);
    }
    return mode;
}
// 5, 6, 9 to 12: G, a thread that takes the exit as filesRecover does (ctr_exit_files.c: CtrLeavekeep_exit).
static const char usGTakes[] USSTOP_TRO = "usstop: G takes the exit (CtrLeavekeep_exit, as filesRecover)";
static const char usGWaits[] USSTOP_TRO = "usstop: G owns the exit and waits before aptExit for main's timeout";
static const char usGGoes[] USSTOP_TRO = "usstop: G goes on with aptExit";
static const char usMainMarked[] USSTOP_TRO = "usstop: main's wait ran out and marked; main sleeps 8 s before its own _exit";
static const char usNoThread[] USSTOP_TRO = "usstop: the test thread did not start";
USSTOP_TTEXT static void usGMain(void* arg) {
    (void) arg;
    usG = usSelf();
    usSay(usGTakes);
    CtrLeavekeep_exit(1);
}
// 7, 8: W, a second thread into svcExitProcess while the owner holds the loaded address.
USSTOP_TTEXT static void usWMain(void* arg) {
    (void) arg;
    usW = usSelf();
    for (int ms = 0; 30000 > ms && !usWGo; ms++) svcSleepThread(1000000LL);
    svcExitProcess();
}
// 9 to 12: L, a lab leave's aptExit before the exit gate (ctr_lab.c leave: aptExit, then CtrExitFiles_labExit). L is
// not the exit's owner; after its aptExit it stays parked (a leave would wait at the gate behind G).
static const char usLCalls[] USSTOP_TRO = "usstop: L calls aptExit before the exit gate (as a lab leave)";
static const char usLAfter[] USSTOP_TRO = "usstop: L's aptExit returned; return address now 0x%08lx\n";
static const char usLReturned[] USSTOP_TRO = "usstop: L's aptExit is done; main marks next, then G goes into its aptExit";
static const char usLStuck[] USSTOP_TRO = "usstop: L's aptExit did NOT return (or stop) in 5 s";
static const char usLStopped[] USSTOP_TRO = "usstop: L has stopped inside the real aptExit; main marks next, then G goes in";
static const char usLParks[] USSTOP_TRO = "usstop: L stops inside the real aptExit (past its last reference, before its store)";
static const char usLGoesOn[] USSTOP_TRO = "usstop: L goes on inside the real aptExit";
static const char usGWrapDone[] USSTOP_TRO = "usstop: G's aptExit wrap is done; it lets L go on and waits for L's aptExit";
static const char usGAfterL[] USSTOP_TRO = "usstop: L's aptExit has returned; G goes on to its frees";
USSTOP_TTEXT static void usLMain(void* arg) {
    (void) arg;
    usL = usSelf();
    usSay(usLCalls);
    aptExit();
    usNote(usLAfter, (unsigned long) (uintptr_t) __system_retAddr);
    usLDone = 1;
    for (;;) svcSleepThread(1000000000LL);
}
USSTOP_TTEXT static void usStartTest(ThreadFunc main) {
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    if (priority > 0x18) priority -= 1;
    if (threadCreate(main, NULL, 0x8000, priority, -2, true) == NULL) usSay(usNoThread);
}
USSTOP_TTEXT static void usAfterMark(int mode) {
    if (mode >= 9) {
        usStartTest(usLMain);
        volatile uint32_t* until = mode >= 11 ? &usLParked : &usLDone;
        for (int ms = 0; 5000 > ms && !*until; ms++) svcSleepThread(1000000LL);
        usSay(!*until ? usLStuck : mode >= 11 ? usLStopped : usLReturned);
    }
    if (US_G(mode)) {
        usSay(usMainMarked);
        usMarked = 1;
        svcSleepThread(8000000000LL);
    }
    if (US_W(mode)) usStartTest(usWMain);
}
// 11, 12: L's stop inside the real aptExit. Its first call out of apt.o after the last reference is
// srvGetServiceHandleDirect (srv.o), in the APT command it sends just before it stores aptExitProcess.
Result __real_srvGetServiceHandleDirect(Handle* out, const char* name);
Result __wrap_srvGetServiceHandleDirect(Handle* out, const char* name);
USSTOP_TTEXT Result __wrap_srvGetServiceHandleDirect(Handle* out, const char* name) {
    if (usMode >= 11 && usL != 0 && usSelf() == usL && !usLParked) {
        usSay(usLParks);
        usLParked = 1;
        for (int ms = 0; 10000 > ms && !usLGo; ms++) svcSleepThread(1000000LL);
        usSay(usLGoesOn);
    }
    return __real_srvGetServiceHandleDirect(out, name);
}
// 11, 12: the end of G's aptExit wrap: L goes on, and G waits until L's aptExit has returned.
USSTOP_TTEXT static void usGWrapEnd(void) {
    if (usMode >= 11 && usSelf() == usG) {
        usSay(usGWrapDone);
        usLGo = 1;
        for (int ms = 0; 5000 > ms && !usLDone; ms++) svcSleepThread(1000000LL);
        usSay(usLDone ? usGAfterL : usLStuck);
    }
}
// svcExitProcess's hooks (7, 8): the owner, holding the loaded address, lets W in and waits for it (up to 500 ms).
static const char usClaimOwner[] USSTOP_TRO = "usstop: svcExitProcess: the exit's owner takes the set-aside return address";
static const char usClaimSecond[] USSTOP_TRO = "usstop: svcExitProcess: a SECOND caller takes the set-aside return address";
static const char usDirectSecond[] USSTOP_TRO = "usstop: svcExitProcess: the second caller does not own the exit: straight to svcExitProcess";
USSTOP_TTEXT static void usLoaded(void) {
    if (US_W(usMode) && usSelf() != usW) {
        usWGo = 1;
        for (int ms = 0; 500 > ms && !usWDone; ms++) svcSleepThread(1000000LL);
    }
}
USSTOP_TTEXT static void usClaimed(void) {
    bool second = usW != 0 && usSelf() == usW;
    usSay(second ? usClaimSecond : usClaimOwner);
    if (second) {
        usWDone = 1;
        svcSleepThread(1000000000LL);
    }
}
USSTOP_TTEXT static void usDirect(void) {
    if (US_W(usMode) && usW != 0 && usSelf() == usW) {
        usSay(usDirectSecond);
        usWDone = 1;
        svcSleepThread(3000000000LL);
    }
}
// 6 and 8: rev 2's take (542eaccb4): load, clear, call.
USSTOP_TTEXT static void usRev2ExitTo(void) {
    void (*to)(void) = usExitTo;
    if (to != NULL) {
        usLoaded();
        usExitTo = NULL;
        usClaimed();
        to();
    }
}
#endif
void __real_CtrReadAhead_shutdown(void);
void __wrap_CtrReadAhead_shutdown(void);
#ifndef FLOWHITCH_STOP_NEXT
#define FLOWHITCH_STOP_NEXT __real_CtrReadAhead_shutdown
#endif
USSTOP_TEXT void __wrap_CtrReadAhead_shutdown(void) {
    if (usw.started) {
#ifdef CTRPORT_EXITTRAIL
        int mode = usProvoke();
        u64 t0 = svcGetSystemTick();
        if (mode != 1) {
#endif
        bool busy = true;
        for (int ms = 0; 2000 > ms; ms++) {
            busy = false;
            LightLock_Lock(&usw.lock);
            usw.held = true;
            for (int i = 0; UTSFX_JOBS > i; i++) {
                if (usw.jobs[i].state == UTSFX_JOB_BUSY) busy = true;
            }
            LightLock_Unlock(&usw.lock);
            if (!busy) break;
#ifdef CTRPORT_EXITTRAIL
            if (ms == 5 && US_G(mode)) usStartTest(usGMain);
#endif
            svcSleepThread(1000000LL);
        }
#ifdef CTRPORT_EXITTRAIL
        usNote(usStopLog, (unsigned long) ((svcGetSystemTick() - t0) / CPU_TICKS_PER_MSEC), busy ? usTimedOut : usIdle,
                (unsigned long) usSpinCount);
        if (busy) usNote(mode == 4 ? usUnkeptLog : mode == 6 ? usRev2Log : usKeptLog);
        if (busy && mode == 6) {
            usR2Keep = __system_retAddr == NULL;
            CtrLeavekeep_rev2Keep();
        }
        if (mode != 4 && mode != 6)
#endif
        // Only the request: the exit's owner acts on it at its aptExit (below).
        if (busy) __atomic_store_n(&usWant, true, __ATOMIC_RELEASE);
#ifdef CTRPORT_EXITTRAIL
        if (busy) usAfterMark(mode);
        }
#endif
    }
    FLOWHITCH_STOP_NEXT(); // flowhitch's stop link (change flowend r22e), which calls the real one
}
// aptExit (exit trail builds: after z_exittrail.c's wrap). Only the exit's owner gets a keep from
// CtrLeavekeep_ownerKeep; the lab leave's own earlier aptExit call (ctr_lab.c, not the owner yet) passes through, but
// it may be the first entry, and then it is the one that records the loader's return address.
#ifdef CTRPORT_EXITTRAIL
#define USSTOP_APT_EXIT CtrAptExit_chainUsstop
#else
#define USSTOP_APT_EXIT __wrap_aptExit
#endif
void __real_aptExit(void);
void USSTOP_APT_EXIT(void);
USSTOP_TEXT void USSTOP_APT_EXIT(void) {
#ifdef CTRPORT_EXITTRAIL
    if (US_G(usMode) && usSelf() == usG) {
        usSay(usGWaits);
        for (int ms = 0; 6000 > ms && !usMarked; ms++) svcSleepThread(1000000LL);
        usSay(usGGoes);
    }
#endif
    // The loader's return address, recorded by the first entry only (1: none, 2: one of its own); once recorded,
    // usLoader never changes, so the load after the compare-and-swap gives every entry the first entry's record.
    u32 seen = 0;
    __atomic_compare_exchange_n(&usLoader, &seen, 1u + (__system_retAddr != NULL), false, __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE);
    bool none = __atomic_load_n(&usLoader, __ATOMIC_ACQUIRE) == 1u;
#ifdef CTRPORT_EXITTRAIL
    if (usMode == 10) none = __system_retAddr == NULL; // rev 3's own read (control arm)
#endif
    __real_aptExit();
#ifdef CTRPORT_EXITTRAIL
    if (usMode != 6)
#endif
    if (none && CtrLeavekeep_ownerKeep(__atomic_load_n(&usWant, __ATOMIC_ACQUIRE))) {
        // Rev 5: final. A return address another thread's aptExit stores after this can no longer let the frees go.
#ifdef CTRPORT_EXITTRAIL
        if (usMode != 12) // 12: rev 4 (control arm)
#endif
        __atomic_store_n(&usVeto, true, __ATOMIC_RELEASE);
        void (*to)(void) = __system_retAddr;
        if (to != NULL) {
            __atomic_store_n(&usExitTo, to, __ATOMIC_RELEASE);
            __system_retAddr = NULL;
        }
    }
#ifdef CTRPORT_EXITTRAIL
    usGWrapEnd();
#endif
}
// Rev 5: z_leavekeep.c's __wrap_svcControlMemory calls this for every svcControlMemory it passes on (UNDERTALE: its
// __real_svcControlMemory is renamed to this). After the owner's veto, a MEMOP_FREE is skipped (the kernel frees the
// process's memory when it ends the process); every other call goes on to the real svcControlMemory.
Result __real_svcControlMemory(u32* addr_out, u32 addr0, u32 addr1, u32 size, MemOp op, MemPerm perm);
Result CtrSvcControlMemory_usstop(u32* addr_out, u32 addr0, u32 addr1, u32 size, MemOp op, MemPerm perm);
USSTOP_TEXT Result CtrSvcControlMemory_usstop(u32* addr_out, u32 addr0, u32 addr1, u32 size, MemOp op, MemPerm perm) {
    if ((op & MEMOP_OP_MASK) == MEMOP_FREE && __atomic_load_n(&usVeto, __ATOMIC_ACQUIRE)) {
#ifdef CTRPORT_EXITTRAIL
        usVetoed = usVetoed + 1u;
#endif
        return 0;
    }
    return __real_svcControlMemory(addr_out, addr0, addr1, size, op, perm);
}
// Rev 5 pad (lead 10-29): rev 5 grew this file's .text.usstop_late by 92 B (aptExit +16, the function above 76), so
// everything after it lost its cand26 cache-line phase (92 = 28 mod 32), the per-frame flowend burst code too. The
// svcExitProcess wrap therefore has its own section, and late_reserve.ld puts this 4 B word between the two: 92 + 4 =
// 96 = 3 x 32, every later late symbol keeps its cand26 phase. Never executed. Re-check if usstop's code size changes.
__attribute__((used, section(".text.usstop_pad"))) static const u32 usstopPad = 0;
void __real_svcExitProcess(void) __attribute__((noreturn));
void __wrap_svcExitProcess(void) __attribute__((noreturn));
__attribute__((noinline, section(".text.usstop_exit"))) void __wrap_svcExitProcess(void) {
#ifdef CTRPORT_EXITTRAIL
    if (usMode == 6 || usMode == 8) usRev2ExitTo();
#endif
    if (__atomic_load_n(&usExitTo, __ATOMIC_ACQUIRE) != NULL) {
#ifdef CTRPORT_EXITTRAIL
        usLoaded();
#endif
        // Only the exit's owner takes it, once (CtrLeavekeep_ownerKeep(true) is true only there); any other caller
        // ends the process straight away, as before.
        if (CtrLeavekeep_ownerKeep(true)) {
            void (*to)(void) = __atomic_exchange_n(&usExitTo, NULL, __ATOMIC_ACQ_REL);
            if (to != NULL) {
#ifdef CTRPORT_EXITTRAIL
                usClaimed();
#endif
                to(); // aptExitProcess sends APT_CloseApplication and returns; the process ends next, as in libctru
            }
        }
    }
#ifdef CTRPORT_EXITTRAIL
    usDirect();
#endif
    __real_svcExitProcess();
}
#endif

#if defined(ENABLE_WAD16) && defined(CTR_AUDIO_PCM_HASH)
#include "runner.h"
#include "ctr_renderer.h"
#include "ctr_wrap_chain.h"
#include "ctr_loadaudio_observer.inc"
#endif

#if defined(ENABLE_WAD16)
#include <citro3d.h>
#include "runner.h"
#include "ctr_renderer.h"
#include "ctr_wrap_chain.h"
#include "ctr_loadaudio_mix.inc"
#endif

#if defined(ENABLE_WAD16)
// sndcut (UNDERTALE): the vtable's stopSound, ctrStopSound's loop with voiceLive's free-voice test in line. A free
// voice is skipped without the call (voiceLive returns false for it and changes nothing); every other voice goes
// through voiceLive, which retires a finished one, then the same match, in the same order. A burst stops and replays
// the same sounds about 60 times in one step, so this and flowendPlaySound's deferred play leave out the frameTicks
// pair (two kernel calls each): the [audio] line's game time no longer counts them. ctrStopSound stays for
// DELTARUNE; the pointer keeps it, unchanged, in UNDERTALE's early text.
__attribute__((used, section(".rodata.sndcut_in_text"))) static void (*const sndcutKeep)(AudioSystem*, int32_t) = ctrStopSound;
__attribute__((noinline, optimize("Os"), section(".text.sndcut_late"))) static void sndcutStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    bool instance = isInstanceId(soundOrInstance);
    for (int i = 0; i < VOICE_COUNT; i++) {
        const Voice* v = &ca->voices[i];
        if (v->kind == VOICE_FREE || !voiceLive(ca, i)) continue;
        if ((instance ? v->instanceId : v->soundIndex) == soundOrInstance) stopVoice(ca, i);
    }
}

// inflight (change flowend r35; UNDERTALE): flowendPlaySound's miss path starts the step's deferred voices first because
// the play may build the sound's buffer and evict (buildSfx). A miss whose background read is already on its way
// can't: resolveSoundSource returns SOURCE_PENDING before any build (loadInFlight, copied here so ctr_audio_system.c's
// early text keeps its code). Omega Flowey's burst plays one sound effect 30 times in one step; when keepLinearFloor has
// shed it since its last play, the first play queues its read and every later one missed, so all 57 deferred voices
// were started and stopped again inside the step (o2ds row flowend-setp34: the first burst's stops 6.9 ms, plays
// 13.2 ms, against 0.6 and 0.8; its frame 57.5 ms). Those voices now wait for the update, as in every other burst.
__attribute__((noinline, optimize("Os"), section(".text.inflight_late"))) static void inflightStartDeferred(CtrAudioSystem* ca, int32_t soundIndex) {
    bool keep = false;
    if (soundIndex >= 0 && (uint32_t) soundIndex < ca->soundCount && ca->sfxCache[soundIndex].pcm == NULL) {
        bool done = false, reading = false, freeSlot = false;
        LightLock_Lock(&ca->loadLock);
        for (int i = 0; i < LOAD_SLOTS; i++) {
            LoadState state = ca->loads[i].state;
            if (state == LOAD_FREE) freeSlot = true;
            else if (ca->loads[i].soundIndex == soundIndex && state == LOAD_DONE) done = true;
            else if (ca->loads[i].soundIndex == soundIndex && (state == LOAD_QUEUED || state == LOAD_BUSY)) reading = true;
        }
        LightLock_Unlock(&ca->loadLock);
        // DONE stays in flight until collectLoads (this thread). A read still queued or busy may fail before the play
        // looks (c26rev r81): resolveSoundSource then asks again, and that is pending too only if a slot is free and
        // background reads are still allowed for it. Only this thread takes a free slot or sets noBackgroundRead.
        keep = done || (reading && freeSlot && !ca->noBackgroundRead[soundIndex]);
    }
    if (!keep) flowendStartDeferred(ca);
}

// utburst (change flowend r37, UNDERTALE): Omega Flowey's held vine sound calls (engine/src/z_utburst.c) are made
// together, a vine's stops then plays, each play's sound stopped again by the next vine's stops; from the batch's
// first play on, the step's plays are left for the update to start (flowendPlaySound's deferral, as from the step's
// FLOWEND_BURST_FIRST+1th play). Named outside .text.late.* so no catch-all above takes it; last.
__attribute__((noinline, optimize("Os"), section(".text.utburst_late"))) void CtrAudio_burstDeferAll(void) {
    if (FLOWEND_BURST_FIRST > flowendBurstPlays) flowendBurstPlays = FLOWEND_BURST_FIRST;
}
#endif

#if defined(ENABLE_WAD16) && defined(CTRPORT_TARGET_O3DS)
// omegaflowey L2 (UNDERTALE Old): the sound plan's stream scene for the new room (z_utsfx.c, after beginSfxPlan put
// the last room's marks back). Each listed sound that isn't cached or streamed already gets flowhitch's mark: it
// plays as a stream for the rest of the room, with no linear cache copy and no background decode (streamWhileCaching
// skips FORM_STREAMED), and the next room change puts its form back (chainPlanFlowhitch). Omega Flowey's fight
// decoded nine such sounds (296-914 KB each, 6.4 MB in all) into the cache in the background, and keepLinearFloor
// shed 7.4-7.9 MB of the room's other sounds for them, which were then read and decoded again in play (change
// omegaflowey). Plays, voices and samples are the same; a play reads the
// stream's buffer, not a cache copy. Returns how many it marked.
__attribute__((noinline, optimize("Os"), section(".text.omegaflowey_late"))) uint32_t CtrAudioSystem_streamPlan(const uint8_t* entries, uint32_t count) {
    CtrAudioSystem* ca = gShutdownTarget;
    uint32_t before = flowhitchCount;
    if (ca == NULL) return 0;
    for (; count > 0 && FLOWHITCH_MAX > flowhitchCount; count--, entries += 4) {
        uint16_t si;
        memcpy(&si, entries, 2);
        if (si >= ca->soundCount || ca->soundForms[si] == FORM_STREAMED || ca->sfxCache[si].pcm != NULL) continue;
        flowhitchSound[flowhitchCount] = (int16_t) si;
        flowhitchForm[flowhitchCount++] = ca->soundForms[si];
        ca->soundForms[si] = FORM_STREAMED;
    }
    return flowhitchCount - before;
}
#endif

// audofix (c26rev r90 + review B; both games): fixes last in .late.text so ctr_audio_system.c's early text keeps its
// addresses (each call site keeps its function's size, nop-padded where the call is shorter than the code it replaces).
//
// M1: the options menu's hold waits while the menu's last sound plays (menuHeldUpdate), and pauses the game's sounds
// (setSuspended) once its fade is down. A menu sound that missed the SFX cache (shed since its last play, so read in
// the background) only starts at a later audio update, inside the hold, so applyVoice started it paused like the
// game's sounds: it never played on, the hold waited on it for good, and the game's sounds stayed paused after the menu
// closed, until a later menu sound found the cache. The hold asks the vtable's isPlaying about the menu's sound, so
// isPlaying (this wrapper; ctrIsPlaying for every other id, and when no hold is on) unpauses that sound first, as
// CtrAudio_menuSound starts a cached one: it plays as soon as it is read, then the release goes on. (menuHeldUpdate
// keeps its code: a direct call there grew it, the vtable entry is a pointer of the same size.)
__attribute__((noinline, noipa, optimize("Os"), section(".text.audofix_late"))) static bool audofixIsPlaying(AudioSystem* audio, int32_t id) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    for (int i = 0; menuHeld != 0 && id == menuVoice && i < VOICE_COUNT; i++) {
        if (ca->voices[i].instanceId != id || !voiceLive(ca, i)) continue;
        bool held = ca->suspended;
        ca->suspended = false;
        applyVoice(ca, i);
        ca->suspended = held;
    }
    return ctrIsPlaying(audio, id);
}

// F1: soundLength frees a small WAV's AUDO blob once it has read the length (the sound is copied into the SFX cache
// from a fresh read when it plays). When that copy found no linear memory, prepareAudoSource streams the WAV from the
// blob itself, so a length query while that stream plays, or while its voice waits for a stream context, freed the
// memory the stream reads. The blob is now kept then, as collectPreps and prepareAudoSource keep it.
__attribute__((noinline, noipa, optimize("Os"), section(".text.audofix_late"))) static void audofixReleaseBlob(CtrAudioSystem* ca, int32_t soundIndex, DataWin* group, AudioEntry* entry) {
    bool inFlight = false; // prepInFlight's check, copied: early text has no copy of it to call
    LightLock_Lock(&ca->loadLock);
    for (int i = 0; i < PREP_SLOTS; i++) {
        if (ca->preps[i].state != PREP_FREE && ca->preps[i].soundIndex == soundIndex) inFlight = true;
    }
    LightLock_Unlock(&ca->loadLock);
    if (inFlight || blobStreaming(ca, entry->data)) return;
    if (group->mappedFile == NULL && group->lazyLoadAudio && group->lazyLoadFile != NULL && entry->data != NULL) {
        free(entry->data); // releaseAudoBlob, copied
        entry->data = NULL;
    }
}

// F2: serviceStream reads stopRequested, unlocks, then locks again for a pending seek. stopStreamVoice gives the channel
// up under the lock (a new voice may take it at once) and leaves seekPending set, so a stop between the two locks let
// the seek clear the new voice's queued buffers. The seek's own state reset is harmless (the next pass finishes the
// stream); only the clear needs the channel, and the lock is held here.
__attribute__((noinline, noipa, optimize("Os"), section(".text.audofix_late"))) static void audofixSeekClear(StreamContext* s) {
    if (!s->stopRequested) ndspChnWaveBufClear(s->channel);
}

// S2: a voice that needs a stream context when all are taken stops the least important live stream, unless one is on
// its way out already (owner -1). A stream that has played out (CTX_DONE) keeps its owner until the update's voice pass
// (voiceLive) lets it go, and the update starts pending voices before that pass, so a voice waiting for a context in
// the update after a stream ended stopped a live one while a finished context sat there. A finished stream's voice is
// now let go first, as that pass would, and its context taken.
__attribute__((noinline, noipa, optimize("Os"), section(".text.audofix_late"))) static int audofixFreeStreamContext(CtrAudioSystem* ca) {
    for (int i = 0; i < STREAM_CONTEXTS; i++) {
        StreamContext* s = &ca->streams[i];
        if (s->state == CTX_DONE && s->owner >= 0) voiceLive(ca, s->owner);
    }
    for (int i = 0; i < STREAM_CONTEXTS; i++) { // findFreeStreamContext's check (not inlined into Os code; no early copy)
        StreamContext* s = &ca->streams[i];
        if (s->state == CTX_FREE || (s->state == CTX_DONE && s->owner < 0)) return i;
    }
    return -1;
}

#if defined(ENABLE_WAD16) && defined(CTRPORT_HAVE_TREMOR)
// audofix drone (playtest 11-01, UNDERTALE): a stream whose sound is whole in the SFX cache reads the cached copy.
// The rising drone before Asriel (a looped sound, its pitch +0.007 a frame from 0.8 to about
// 4.06) stuttered and cut out in the fade to white on the Old 3DS: a stream is decoded from Ogg Vorbis at the file's
// rate, so its decode cost grows with the pitch (this one: about 27% of core 1 per 1x), and from about 3.65x the
// decode thread could not keep up (95% busy, 22 ms queued; the playtest's log). The sound had been decoded into the cache
// in the background long before (streamWhileCaching), but a playing stream never looked at the copy.
// Exact: the copy is the same decoder's output for the same file (runPrepDecode), so frame n of the copy is frame n
// of the stream, and the read hands the stream the frames it would have decoded, from its own position. The loop
// seek, utaDecRead's seam and the queueing are unchanged (this link sits under them: flowhitch's read link's last
// call, in front of the real read).
// Game thread (each audio update): a stream's context gets its sound's copy (pointer, length, rate, channels and
// the stream's name), and the copy counts as just played, so the texture cache's shedding passes it by and LRU
// eviction takes it last. Any free of a published copy (pcmwait's real frees, at once or after its wait) takes it off
// first and waits for a copy in progress (audofixFeeding: a Dekker pair, full barriers on both sides). Decode thread: a context whose
// copy matches its decoder (Vorbis, channels, rate, length, name: a new stream in the context is not fed until the
// next update) copies from the decoder's position and moves it on. When the copy goes while fed, the real decoder is
// sought to that position first (it stood where the feed began), so the stream goes on exactly.
#define AUDOFIX_FEED_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text.audofix_feed")))
#define AUDOFIX_FEED_BSS __attribute__((section(".bss.audofix_late")))
typedef struct {
    const int16_t* volatile pcm; // the copy, or NULL; written by the game thread only
    uint32_t frames, rate, channels;
    char name[sizeof(((StreamContext*) 0)->name)];
} AudofixFeed;
static AudofixFeed audofixFeeds[STREAM_CONTEXTS] AUDOFIX_FEED_BSS;
static const int16_t* volatile audofixFeeding[STREAM_CONTEXTS] AUDOFIX_FEED_BSS; // the copy the decode thread reads now
static uint32_t audofixFedEnd[STREAM_CONTEXTS] AUDOFIX_FEED_BSS; // decode thread: where its last copy left the decoder
static const char audofixFeedLog[] __attribute__((section(".rodata.audofix_in_text"))) =
    "Audio: stream '%s' reads its cached copy from frame %lu\n";
void audofixFree(void* mem);

AUDOFIX_FEED_CODE static void audofixUnfeed(int k) {
    const int16_t* old = audofixFeeds[k].pcm;
    audofixFeeds[k].pcm = NULL;
    __sync_synchronize();
    while (old != NULL && audofixFeeding[k] == old) svcSleepThread(0);
}

AUDOFIX_FEED_CODE static void audofixUpdate(AudioSystem* audio, float deltaTime) {
    flowendBurstUpdate(audio, deltaTime);
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    for (int k = 0; k < STREAM_CONTEXTS; k++) {
        StreamContext* s = &ca->streams[k];
        AudofixFeed* f = &audofixFeeds[k];
        CachedSfx* c = NULL;
        int vi = s->owner;
        if (vi >= 0 && VOICE_COUNT > vi) {
            Voice* v = &ca->voices[vi];
            if (v->kind == VOICE_STREAM && v->ctx == k && v->soundIndex >= 0 && ca->soundCount > (uint32_t) v->soundIndex)
                c = &ca->sfxCache[v->soundIndex];
        }
        const int16_t* pcm = NULL;
        if (c != NULL && (c->format == NDSP_FORMAT_STEREO_PCM16 || c->format == NDSP_FORMAT_MONO_PCM16)) pcm = (const int16_t*) c->pcm;
        if (pcm != f->pcm || (pcm != NULL && memcmp(f->name, s->name, sizeof(f->name)) != 0)) {
            audofixUnfeed(k);
            if (pcm != NULL) {
                f->frames = c->frames;
                f->rate = c->sampleRate;
                f->channels = c->format == NDSP_FORMAT_STEREO_PCM16 ? 2u : 1u;
                memcpy(f->name, s->name, sizeof(f->name));
                __sync_synchronize();
                f->pcm = pcm;
            }
        }
        if (pcm != NULL) {
            c->lastPlayTick = svcGetSystemTick();
            if (c->lastUse != ca->useClock) c->lastUse = ++ca->useClock;
        }
    }
}

// pcmwait's real frees (CtrAudio_pcmWaitFree's and pcmWaitFlush's: every linearFree of this file ends there).
AUDOFIX_FEED_CODE void audofixFree(void* mem) {
    for (int k = 0; mem != NULL && k < STREAM_CONTEXTS; k++) {
        if (audofixFeeds[k].pcm == mem) audofixUnfeed(k);
    }
    __wrap_linearFree(mem);
}

// flowhitch's read link's last call (any thread; only the stream contexts' decoders are fed).
AUDOFIX_FEED_CODE int32_t audofixFeedRead(CtrDecoder* dec, int16_t* out, uint32_t maxFrames) {
    CtrAudioSystem* ca = *ctrPcmWaitUta.target;
    uintptr_t off = (uintptr_t) dec - (uintptr_t) &ca->streams[0].decoder;
    uint32_t k = off / sizeof(StreamContext);
    if (ca != NULL && dec->kind == CTR_DECODER_VORBIS && off % sizeof(StreamContext) == 0 && STREAM_CONTEXTS > k) {
        AudofixFeed* f = &audofixFeeds[k];
        const int16_t* p = f->pcm;
        audofixFeeding[k] = p;
        __sync_synchronize();
        uint32_t at = (uint32_t) dec->positionFrames, n = 0;
        bool feed = p != NULL && f->pcm == p && dec->channels == f->channels && dec->sampleRate == f->rate &&
                    dec->totalFrames == f->frames && f->frames >= dec->positionFrames &&
                    memcmp(f->name, ca->streams[k].name, sizeof(f->name)) == 0;
        if (feed) {
            n = f->frames - at;
            if (n > maxFrames) n = maxFrames;
            memcpy(out, p + at * f->channels, n * f->channels * sizeof(int16_t));
        }
        __sync_synchronize();
        audofixFeeding[k] = NULL;
        if (feed) {
            if (audofixFedEnd[k] == 0) postMessage(ca, audofixFeedLog, ca->streams[k].name, (unsigned long) at);
            dec->positionFrames = at + n;
            audofixFedEnd[k] = at + n;
            return (int32_t) n;
        }
        if (audofixFedEnd[k] == at && at != 0) (CtrDecoder_seek)(dec, at);
        audofixFedEnd[k] = 0;
    }
    return __real_CtrDecoder_read(dec, out, maxFrames);
}
// The two links by name: aliases GCC doesn't see, so flowhitch's and pcmwait's functions call what looks to it like
// the same external function as before (a call to a function of this file reorders flowend's block: b8).
__asm__(".set audofixReadLink, " HPL_READ_LINK "\n.set audofixFreeLink, audofixFree\n"); // hplcopy's read first on Old
#endif

#if defined(ENABLE_WAD16) && defined(CTRPORT_TARGET_O3DS)
// ===[ omegaflowey W1: a planned compressed sound effect is decoded at the room change (late code, UNDERTALE Old) ]===
// Omega Flowey's flame loop (a 0.43 s embedded Ogg) is stopped and played again by every flame, one every ~67 ms. Not
// cached yet, each play opened a memory stream (12-52 ms on the opener thread), and the background decode its first
// play asked for took 1.68 s on the Old models' share of the system core, so the first ~1.7 s of each attack opened
// 16-18 streams: the plays sounded ~20-23 ms of their ~67 ms (some none), and held frames clustered there (change
// omegaflowey). The copy was then shed between attacks, and the next one began
// again. The sound plan now lists such a sound first in its room's scene (build_sfxplan.py's warm sounds): flowend's
// preload wrapper keeps it from shedding for the room, and here, where the real preloadSfx copies WAVs only, its
// blob is read (as its first play would read it) and its decode into the cache is asked for at the room change. Plays
// then find the cache copy: the same decoder's samples, from the play's own frame. Everything else goes on to the
// real one. Game thread (z_utsfx.c's preload). 1 asked, 0 cached already, -1 not now.
__attribute__((noinline, noipa, used, optimize("Os"), section(".omegaflowey_flame"))) int flamePreload(int32_t soundIndex, uint32_t bytes,
                                                                                                      uint32_t floorBytes) {
    CtrAudioSystem* ca = gShutdownTarget;
    if (ca == NULL || !ca->ndspReady || ca->base.audioGroups == NULL || soundIndex < 0 || (uint32_t) soundIndex >= ca->soundCount ||
        (ca->base.audioGroups[0]->sond.sounds[soundIndex].flags & AUDIO_ENTRY_FLAG_IS_COMPRESSED) == 0) {
        return __real_CtrAudioSystem_preloadSfx(soundIndex, bytes, floorBytes);
    }
    if (ca->sfxCache[soundIndex].pcm != NULL) return 0;
    // Not while a read or decode of it is on its way (its play takes that one).
    bool busy = false;
    LightLock_Lock(&ca->loadLock);
    for (int i = 0; i < LOAD_SLOTS; i++) busy |= ca->loads[i].soundIndex == soundIndex && ca->loads[i].state != LOAD_FREE;
    for (int i = 0; i < PREP_SLOTS; i++) busy |= ca->preps[i].soundIndex == soundIndex && ca->preps[i].state != PREP_FREE;
    LightLock_Unlock(&ca->loadLock);
    CachedSfx* sfx = NULL;
    const uint8_t* mem = NULL;
    uint32_t memSize = 0;
    u64 loadTicks = ca->windowLoadTicks; // the [audio] line's load time counts plays only
    int got = -1;
    if (!busy && ca->loader != NULL && prepareAudoSource(ca, soundIndex, &sfx, &mem, &memSize) && sfx == NULL && mem != NULL) {
        LightLock_Lock(&ca->loadLock);
        for (int i = 0; i < PREP_SLOTS && 0 > got; i++) {
            PrepJob* job = &ca->preps[i];
            if (job->state != PREP_FREE) continue;
            memset(job, 0, sizeof(*job));
            job->soundIndex = soundIndex;
            job->mem = mem;
            job->memSize = memSize;
            job->state = PREP_PROBE;
            got = 1;
        }
        LightLock_Unlock(&ca->loadLock);
        if (got > 0) LightEvent_Signal(&ca->loadEvent);
    }
    ca->windowLoadTicks = loadTicks;
    return got;
}
// flowend's preload wrapper calls the link by this name (ctr_wrap_chain.h): an alias GCC doesn't see, as audofix's.
__asm__(".set CtrAudioSystem_chainFlame, flamePreload\n");
#endif

#if defined(ENABLE_WAD16) && defined(CTRPORT_PACER_SHARE) // CMakeLists.txt: UT Old, Tremor, the checked libvorbisidec.a
// ===[ pacer C3: Vorbis streams with the same codebooks share one decoded set (late code, UNDERTALE Old) ]===
// Tremor's vorbis_synthesis_init (a stream's first read, on the decode thread, core 1) decodes every codebook of the
// stream's setup header again for each stream: one calloc, then per book vorbis_book_init_decode (sort, code words,
// unquantized values, first table: ~470 allocations for UNDERTALE's main header) and the static book's free. UNDERTALE
// has 216 Vorbis streams but only 5 distinct setup headers; 206 streams (sound effects and music alike)
// carry the same 38 books, so the work is the same every time (change pacer, C3). Here the first
// stream with a given set of static books builds the decoded books once, and every later stream whose static books are
// byte for byte equal (every field vorbis_book_init_decode reads: dim, entries, each length, map type, q_min, q_delta,
// q_quant, q_sequencep and each quantized value) uses that set. vorbis_book_init_decode is a pure function of those
// fields and decoding only reads the decoded book, so the samples are unchanged. The static books are freed and NULLed
// as Tremor's own loop does, and Tremor's init then finds ci->fullbooks set and skips its book loop (block.o, 0x98).
// vorbis_info_clear's wrap takes a shared set off ci->fullbooks before Tremor frees the rest and drops the stream's
// reference without a lock (it also runs on the main and decode threads). When a release leaves a set idle, every
// other idle set but the newest is freed right there, on that thread, as Tremor's clear frees a stream's books today
// (claimed by compare-and-swap, so never twice and never while held): at most one idle set is ever kept, for the
// next stream of its header; a new set takes a free slot, else the idle set's. Up to 3 sets: with all 3 in use a stream keeps the books it built (Tremor's clear frees
// them). A field that does not fit the compare key or a failed build falls through to Tremor's own loop unchanged.
// The offsets below are Tremor's private codec_setup_info and codebook (no installed header declares them): CMake
// defines CTRPORT_PACER_SHARE only for the libvorbisidec.a whose disassembly they were read from (sha256 check), so
// any other Tremor build runs its own path. Weak references, as C2's: a strong one would pull Tremor's members out of libvorbisidec.a in another order and
// move every Tremor function; vorbisfile.o's own calls bring them in.
typedef struct PacerStaticBook {
    long dim;
    long entries;
    long* lengthlist;
    int maptype;
    long q_min;
    long q_delta;
    int q_quant;
    int q_sequencep;
    long* quantlist;
} PacerStaticBook; // Tremor 1.2.1's static_codebook (0x24 B; block.o / sharedbook.o)
#define PACER_CI_BOOKS(ci) (*(int*) ((char*) (ci) + 0x1c))
#define PACER_CI_BOOK_PARAM(ci) ((PacerStaticBook**) ((char*) (ci) + 0x820))
#define PACER_CI_FULLBOOKS(ci) (*(void**) ((char*) (ci) + 0xc20))
#define PACER_CODEBOOK_BYTES 0x34
#define PACER_SETS 3
_Static_assert(sizeof(long) == 4 && sizeof(int) == 4, "Tremor 1.2.1 layout");
int vorbis_book_init_decode(void* dest, const PacerStaticBook* s) __attribute__((weak));
void vorbis_book_clear(void* b) __attribute__((weak));
void vorbis_staticbook_destroy(PacerStaticBook* s) __attribute__((weak));
long _book_maptype1_quantvals(const PacerStaticBook* s) __attribute__((weak));
int __real_vorbis_synthesis_init(vorbis_dsp_state* v, vorbis_info* vi) __attribute__((weak));
void __real_vorbis_info_clear(vorbis_info* vi) __attribute__((weak));
int __wrap_vorbis_synthesis_init(vorbis_dsp_state* v, vorbis_info* vi);
void __wrap_vorbis_info_clear(vorbis_info* vi);
typedef struct PacerShareSet {
    void* fullbooks; // NULL: free slot
    uint8_t* key;
    int keyBytes;
    int books;
    int refs; // streams using the set (atomic)
    u32 idle; // when refs last reached 0 (pacerShare.clock)
} PacerShareSet;
typedef struct PacerShare {
    LightLock lock;
    u32 clock;
    PacerShareSet sets[PACER_SETS];
} PacerShare;
__attribute__((section(".pacer_share_data"))) static PacerShare pacerShare = {.lock = 1}; // 1 = libctru's unlocked
#define PACER_CODE __attribute__((optimize("Os"), section(".pacer_share")))
// Per stream init: whole set-up, then the books part ("o" own: Tremor's loop, the figure is its whole init; "s" shared,
// "b" built and kept, "k" built but every slot busy, so the stream owns it; "n" none: decoded before, a re-init).
static const char pacerShareLog[] __attribute__((section(".pacer_share_ro"))) = "Audio: Vorbis set-up %lu us, books %c %lu us\n";
static const char pacerShareHow[] __attribute__((section(".pacer_share_ro"))) = "osbkn";
#define PACER_US(ticks) ((unsigned long) ((u32) (ticks) / 268u)) // 268.1 ARM11 ticks a microsecond
#ifndef PACER_PAUSE
#define PACER_PAUSE(n) // a yield at each publication point in the host stress test
#endif

// The compare key: per book dim, entries, then maptype, q_min, q_delta, q_quant, q_sequencep (4 B each, as stored),
// each length (1 B) and with a map each quantized value (2 B). Written to out, compared with cmp, or only counted.
typedef struct PacerKeyWalk {
    uint8_t* out;
    const uint8_t* cmp;
    int n;
    int bad;
} PacerKeyWalk;

PACER_CODE static void pacerKeyPut(PacerKeyWalk* w, const void* p, long count, int bytes) {
    for (long i = 0; i < count && !w->bad; i++) {
        uint32_t v; // every field and list element is 4 B (long and int on the 3DS)
        memcpy(&v, (const char*) p + 4 * i, 4);
        if (bytes < 4 && v >> (8 * bytes))
            w->bad = 1; // does not fit: no key, Tremor's own loop
        for (int b = 0; b < bytes; b++, w->n++) {
            uint8_t c = (uint8_t) (v >> (8 * b));
            if (w->out)
                w->out[w->n] = c;
            else if (w->cmp && w->cmp[w->n] != c)
                w->bad = 1;
        }
    }
}

// The key's length, or -1 (a difference from cmp, or a field that does not fit).
PACER_CODE static int pacerShareKey(void* ci, uint8_t* out, const uint8_t* cmp) {
    PacerKeyWalk w = {out, cmp, 0, 0};
    for (int i = 0; i < PACER_CI_BOOKS(ci) && !w.bad; i++) {
        const PacerStaticBook* s = PACER_CI_BOOK_PARAM(ci)[i];
        if (!s || !s->lengthlist || (s->maptype && !s->quantlist))
            return -1;
        pacerKeyPut(&w, &s->dim, 2, 4);
        pacerKeyPut(&w, &s->maptype, 5, 4);
        pacerKeyPut(&w, s->lengthlist, s->entries, 1);
        if (s->maptype)
            pacerKeyPut(&w, s->quantlist, s->maptype == 1 ? _book_maptype1_quantvals(s) : s->entries * s->dim, 2);
    }
    return w.bad ? -1 : w.n;
}


// Frees a set's decoded books (vorbis_info_clear's own steps for them) and its key. Never under the lock.
PACER_CODE static void pacerShareFree(PacerShareSet* set) {
    for (int i = 0; set->fullbooks && i < set->books; i++)
        vorbis_book_clear((char*) set->fullbooks + i * PACER_CODEBOOK_BYTES);
    free(set->fullbooks);
    free(set->key);
}

// refs: n >= 0 streams hold the set (0 = idle), -1 = claimed by the one thread that takes it out of its slot. pin:
// n -> n + 1 unless claimed; claim: 0 -> -1 only. Both fail on a claimed set, so a set is freed once and never while
// held. A slot is free once its fullbooks is NULL; only an install under the lock fills it, and it holds the set
// (refs 1, stored first) before it publishes fullbooks (last, release): a set in use never shows refs 0, so a claim
// that wins has an idle set whichever install it came from (no generation count needed). Readers acquire fullbooks.
PACER_CODE static int pacerShareCas(PacerShareSet* set, int pin) {
    int r = __atomic_load_n(&set->refs, __ATOMIC_SEQ_CST);
    do {
        if (r < 0 || (!pin && r))
            return 0;
        PACER_PAUSE(1);
    } while (!__atomic_compare_exchange_n(&set->refs, &r, pin ? r + 1 : -1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    return 1;
}

// A stream lets go of a set. Lock free: vorbis_info_clear also runs on the main thread and the decode thread, which
// must never wait on a lock that a starved low-priority worker holds (change pacer lk3w1: newlib's heap lock, 28 ms).
// used: a stream's real use (stamps the set newest; a key compare that missed does not). The stamp is stored while the
// reference is held. When the count reaches 0, the older of it and each other idle set is claimed and freed right here, on
// this thread, as Tremor's clear frees a stream's books today: at most one idle set is ever kept (sequentially consistent
// count and loads: of two sets going idle at once, at least one release sees the other).
PACER_CODE static void pacerShareRelease(PacerShareSet* set, int used) {
    if (used)
        __atomic_store_n(&set->idle, __atomic_add_fetch(&pacerShare.clock, 1, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    PACER_PAUSE(2);
    if (__atomic_sub_fetch(&set->refs, 1, __ATOMIC_SEQ_CST))
        return;
    PACER_PAUSE(3);
    for (int i = 0; i < PACER_SETS; i++) { // of this set and each other idle set, the older goes
        PacerShareSet* s = &pacerShare.sets[i];
        if (s == set || !__atomic_load_n(&s->fullbooks, __ATOMIC_SEQ_CST) || __atomic_load_n(&s->refs, __ATOMIC_SEQ_CST))
            continue;
        PacerShareSet* old =
            __atomic_load_n(&s->idle, __ATOMIC_RELAXED) < __atomic_load_n(&set->idle, __ATOMIC_RELAXED) ? s : set;
        PACER_PAUSE(4);
        if (pacerShareCas(old, 0)) {
            PacerShareSet gone = *old;
            old->key = NULL;
            PACER_PAUSE(5);
            __atomic_store_n(&old->fullbooks, NULL, __ATOMIC_SEQ_CST); // the slot is free from here: never touched again
            pacerShareFree(&gone);
        }
        if (old == set)
            return;
    }
}

// The static books' end, as Tremor's loop after each book it decodes.
PACER_CODE static void pacerShareDropStatic(void* ci) {
    for (int i = 0; i < PACER_CI_BOOKS(ci); i++) {
        vorbis_staticbook_destroy(PACER_CI_BOOK_PARAM(ci)[i]);
        PACER_CI_BOOK_PARAM(ci)[i] = NULL;
    }
}

// Puts a decoded set on ci: 1 found (shared), 2 built and kept for later streams, 3 built but every slot is in use
// (ci owns it, Tremor's clear frees it), or 0: Tremor's own loop runs (a misfit key or a failed build). Runs on the
// stream's decoding thread (the decode thread, the loader or the opener; all core 1). The lock orders pins and
// installs only (a few loads, stores and compare-and-swaps, no heap calls); compares, builds and frees run outside it.
PACER_CODE static int pacerShareBooks(void* ci) {
    PacerShareSet mine = {NULL, NULL, pacerShareKey(ci, NULL, NULL), PACER_CI_BOOKS(ci), 1, 0};
    if (mine.keyBytes <= 0)
        return 0;
    PacerShareSet* take[PACER_SETS] = {NULL};
    LightLock_Lock(&pacerShare.lock);
    for (int i = 0; i < PACER_SETS; i++) { // same-shape sets are candidates, pinned while their keys are compared
        PacerShareSet* set = &pacerShare.sets[i];
        if (__atomic_load_n(&set->fullbooks, __ATOMIC_ACQUIRE) && set->books == mine.books &&
            set->keyBytes == mine.keyBytes && pacerShareCas(set, 1))
            take[i] = set;
    }
    LightLock_Unlock(&pacerShare.lock);
    PacerShareSet* hit = NULL;
    for (int i = 0; i < PACER_SETS; i++) {
        if (!take[i])
            continue;
        if (!hit && pacerShareKey(ci, NULL, take[i]->key) == mine.keyBytes)
            hit = take[i];
        else
            pacerShareRelease(take[i], 0);
    }
    if (hit) {
        PACER_CI_FULLBOOKS(ci) = hit->fullbooks;
        pacerShareDropStatic(ci);
        return 1;
    }
    // A miss: build the set here, as Tremor's loop would.
    mine.key = malloc(mine.keyBytes);
    mine.fullbooks = calloc(mine.books, PACER_CODEBOOK_BYTES);
    int built = mine.key && mine.fullbooks;
    if (built)
        pacerShareKey(ci, mine.key, NULL);
    for (int i = 0; built && i < mine.books; i++)
        built = !vorbis_book_init_decode((char*) mine.fullbooks + i * PACER_CODEBOOK_BYTES, PACER_CI_BOOK_PARAM(ci)[i]);
    if (!built) { // vorbis_book_clear also frees what a failed book left (all-zero books clear to nothing)
        pacerShareFree(&mine);
        return 0;
    }
    pacerShareDropStatic(ci);
    PACER_CI_FULLBOOKS(ci) = mine.fullbooks;
    // A free slot, else the idle set's (claimed here, freed below). Two streams that built the same set at once would
    // each keep a slot: correct, only less shared.
    PacerShareSet* slot = NULL;
    PacerShareSet drop = {NULL, NULL, 0, 0, 0, 0};
    LightLock_Lock(&pacerShare.lock);
    for (int i = 0; i < PACER_SETS && !slot; i++)
        if (!__atomic_load_n(&pacerShare.sets[i].fullbooks, __ATOMIC_SEQ_CST))
            slot = &pacerShare.sets[i];
    for (int i = 0; i < PACER_SETS && !slot; i++)
        if (pacerShareCas(&pacerShare.sets[i], 0)) {
            slot = &pacerShare.sets[i];
            drop = *slot;
        }
    if (slot) { // held before it is visible, fullbooks last (R155 F1: a plain struct copy let a release free it)
        __atomic_store_n(&slot->refs, 1, __ATOMIC_SEQ_CST);
        PACER_PAUSE(6);
        slot->key = mine.key;
        slot->keyBytes = mine.keyBytes;
        slot->books = mine.books;
        __atomic_store_n(&slot->idle, 0, __ATOMIC_RELAXED);
        PACER_PAUSE(7);
        __atomic_store_n(&slot->fullbooks, mine.fullbooks, __ATOMIC_RELEASE);
    }
    LightLock_Unlock(&pacerShare.lock);
    if (!slot)
        free(mine.key); // ci keeps the books
    pacerShareFree(&drop);
    return slot ? 2 : 3;
}

PACER_CODE int __wrap_vorbis_synthesis_init(vorbis_dsp_state* v, vorbis_info* vi) {
    void* ci = vi->codec_setup;
    u64 t0 = svcGetSystemTick();
    int how = PACER_CI_FULLBOOKS(ci) ? 4 : pacerShareBooks(ci);
    u64 t1 = svcGetSystemTick();
    int ret = __real_vorbis_synthesis_init(v, vi);
    u64 t2 = svcGetSystemTick();
    logInfo(pacerShareLog, PACER_US(t2 - t0), pacerShareHow[how], PACER_US(how ? t1 - t0 : t2 - t1));
    return ret;
}

// No lock: this stream's reference keeps its set in its slot, so the slot's pointer cannot change under the compare.
PACER_CODE void __wrap_vorbis_info_clear(vorbis_info* vi) {
    void* ci = vi->codec_setup;
    for (int i = 0; ci && PACER_CI_FULLBOOKS(ci) && i < PACER_SETS; i++) {
        PacerShareSet* set = &pacerShare.sets[i];
        if (__atomic_load_n(&set->fullbooks, __ATOMIC_ACQUIRE) == PACER_CI_FULLBOOKS(ci)) {
            PACER_CI_FULLBOOKS(ci) = NULL;
            pacerShareRelease(set, 1);
        }
    }
    __real_vorbis_info_clear(vi);
}
#endif

#if defined(ENABLE_WAD16) && defined(CTRPORT_PACER_ARENA) // CMakeLists.txt: UNDERTALE Old 3DS, newlib's allocators wrapped
// ===[ pacer A + E1: the audio threads' blocks come from an arena of their own (late code, UNDERTALE Old) ]===
// newlib has one heap lock for every thread. On the Old models the audio threads (decode, stream opener, loader, UT sfx
// worker) all run on core 1, whose app time limit (SYSCORE_PERCENT) pauses them whatever their priority, and the loader
// (lowest) is also preempted by the decode thread's refills: one paused inside malloc or free held the lock, and the
// game thread waited 23.6 and 28.5 ms for it at its next allocation (omegaflowey's lock trace, 56d83f348 / f81e401c3;
// the loader's cache-decode open and close, f13782 and f5456). Here those threads' blocks of up to PA_SMALL bytes come
// from one block carved from the heap at their first allocation, under a lock only they take, and every other thread's
// stay on the heap. E1: pacer A took only the audio threads' blocks of up to 31 bytes and their callocs, because
// devkitARM's malloc asks _memalign_r for 32-byte alignment above 31 bytes and A sent memalign to the heap (the E1
// arenalog census: ~10 heap-lock takes a second left in Omega Flowey, 0.7 a second with blocks up to 16 KB). The arena
// now takes alignments up to 32, realloc(NULL), and blocks up to 256 KB (the loader's sound file reads, Tremor's larger
// tables, the 66 KB tail probe), and a heap block an audio thread frees waits for the game thread to free it
// (paHeapDrain), so the audio threads take the heap lock only at the carving, when the arena is full (counted and
// logged) and for a realloc of a heap block or a larger block (none in the census). Every free and realloc goes by
// address, whatever the thread. Another thread never takes the arena's lock: an arena block it frees goes on a
// lock-free list that the next audio-thread call frees, and its realloc of one copies it to the heap. newlib's own
// calloc and realloc read heap chunk headers, so an audio thread inside them allocates from the heap (bypass);
// ctr_memory.c's malloc_usable_size reads the arena's own header (no wrap: wrapping newlib's would link its member
// earlier and move every newlib function after it). Arena full or not carved: the heap, counted and logged. The
// allocator: blocks with a size word (in use, block before in use) and a closing size word while free, free lists by
// power of two with a bitmap, first fit in the request's own list, split and coalesced both ways.
#ifndef PACER_ARENA_KB // E1 arenalog (Azahar, every audio block up to PA_SMALL): ut_flowey 950,808 B in use at its peak
#define PACER_ARENA_KB 1280
#endif
#define PA_BYTES ((uint32_t) PACER_ARENA_KB * 1024u)
#define PA_SPAN (PA_BYTES + 8u) // the block carved from the heap
#define PA_SMALL (256u * 1024u) // E1: a larger block (none seen; the loader's largest sound file was 226,184 B) uses the heap
#define PA_BINS 16
#define PA_USED 1u
#define PA_PREV 2u // the block before this one is in use
#define PA_MIN ((uint32_t) ((8 + 2 * sizeof(char*) + 7) & ~7u)) // size word, two links, closing size word
#define PA_HEAD(b) (*(uint32_t*) (b))
#define PA_SIZE(b) (PA_HEAD(b) & ~7u)
#define PA_NEXT(b) (((char**) ((b) + 4))[0])
#define PA_BACK(b) (((char**) ((b) + 4))[1])
#define PA_CODE __attribute__((optimize("Os"), section(".pacer_arena")))
#ifndef PA_TEST_HOOK_MINE // the host test's scheduling points; empty in the game
#define PA_TEST_HOOK_MINE()
#define PA_TEST_HOOK_CARVE()
#endif
#ifndef PACER_ARENA_SECTION_DATA
#define PACER_ARENA_SECTION_DATA __attribute__((section(".pacer_arena_data")))
#endif
typedef struct PacerArena {
    LightLock lock;
    uint32_t state; // 0 not carved, 1 being carved, 2 ready, 3 no heap for it
    uintptr_t lo; // arena addresses [lo, lo + PA_SPAN); 0 until the arena is valid, then never changed
    char* deferred; // arena blocks other threads freed (lock-free push, emptied under the lock)
    char* heapFreed; // E1: heap blocks audio threads freed (lock-free push, freed by the game thread's next call)
    uint32_t bits; // non-empty free lists
    char* bins[PA_BINS];
    uint8_t bypass[4]; // per audio thread: inside newlib's calloc, realloc or memalign
    Thread who[4]; // each audio thread kind's running thread (CtrArena_runThread), or NULL
    uint32_t used, peak, fallbacks;
    CtrAudioSystem* ca; // E1b: the audio system whose load slots rd[] serves (CtrArena_loadSignal)
    char* rd[LOAD_SLOTS]; // E1b: load slot i's block for a read over PA_SMALL, put by the game thread
    uint32_t rdN[LOAD_SLOTS]; // its size
#ifdef PACER_ARENA_LOG
    uint32_t logged, big, bigMax, allocs[4], req[4]; // req: E1 census, the kind's wrapped call in progress
#endif
} PacerArena;
PACER_ARENA_SECTION_DATA static PacerArena pa = {.lock = 1}; // 1 = libctru's unlocked
static const char paLogFull[] __attribute__((section(".pacer_arena_ro"))) =
    "Audio: arena %lu KB (state %lu) full, %lu bytes from the heap (%lu so far)\n";
static const char paLogReady[] __attribute__((section(".pacer_arena_ro"))) = "Audio: arena %lu KB at %p\n";
#ifdef PACER_ARENA_LOG
static const char paLogPeak[] __attribute__((section(".pacer_arena_ro"))) =
    "Audio: arena peak %lu used %lu largest free %lu fallbacks %lu big %lu (max %lu) allocs d%lu o%lu l%lu s%lu\n";
// E1 test word census: every time an audio thread takes newlib's heap lock (ctr_watchdog.c's __malloc_lock wrap calls
// CtrArena_noteHeapLock under PACER_ARENA_LOG), its kind, the wrapped call in progress (req: kind of call << 28 | size;
// 1 malloc, 2 calloc, 3 realloc, 4 memalign, 5 free; 0 a newlib-internal call) and the code addresses on its stack, in
// a ring logged later by an audio thread outside every lock (lost: entries the ring overwrote before that). Counts per
// kind on each line. Never in a release.
static const char paLogHl[] __attribute__((section(".pacer_arena_ro"))) =
    "Audio: arena heaplock #%lu k%lu n %lu age %lu req %08lx: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx (d%lu o%lu l%lu s%lu lost %lu)\n";
#define PA_HL_N 256
typedef struct PaHeapLock {
    uint32_t seq, who, n, req, ms, w[8];
} PaHeapLock;
static PaHeapLock paHl[PA_HL_N];
static uint32_t paHlCount[4], paHlPut, paHlGot, paHlLost;
extern char __late_text_end[];
#define PA_REQ(who, code, n) \
    do { \
        if (who) pa.req[(who) - 1] = ((uint32_t) (code) << 28) | ((uint32_t) (n) & 0x0fffffffu); \
    } while (0)
#else
#define PA_REQ(who, code, n) ((void) 0)
#endif
void* __real__malloc_r(struct _reent* r, size_t n);
void* __real__calloc_r(struct _reent* r, size_t c, size_t n);
void* __real__realloc_r(struct _reent* r, void* p, size_t n);
void* __real__memalign_r(struct _reent* r, size_t a, size_t n);
size_t malloc_usable_size(void* p);
void __real__free_r(struct _reent* r, void* p);
void* __wrap__malloc_r(struct _reent* r, size_t n);
void* __wrap__calloc_r(struct _reent* r, size_t c, size_t n);
void* __wrap__realloc_r(struct _reent* r, void* p, size_t n);
void* __wrap__memalign_r(struct _reent* r, size_t a, size_t n);
size_t CtrArena_usableSize(void* p); // ctr_memory.c's malloc_usable_size calls end here
void CtrArena_freeR(struct _reent* r, void* p); // ctr_memory.c's free path ends here

// The calling thread: 1-4 for the decode thread, the opener, the loader and the UT sfx worker, 0 for any other (the
// main thread's handle is NULL). pa.who holds each kind's running thread: a thread stores its own handle there as it
// starts and takes it out when its entry function returns, before libctru can free its Thread or hand the address to a
// new thread, so a stored handle always names a live thread and a match can only be the caller itself. Nothing is read
// through a handle and no audio-system object is read (r2 read the audio system's thread handles, which ctrDestroy
// frees while other threads may still be allocating: c26rev R159 F2). Every thread starts through ctr_memory.c's
// threadTrampoline: the link wraps threadCreate, so libctru's Thread always holds the trampoline as its entry (r3 read
// that and never matched). The trampoline calls CtrLab_runThread with the real entry, and memory_report.c's wrap of
// it ends here under CTRPORT_PACER_ARENA (only its call's target changes; a check script reads the
// chain in each ELF). Not a thread-local byte: a .tbss variable moves all of .bss (r4 draft, check_layout).
// One owner per kind: a thread claims its kind's slot only while it is empty (compare-and-swap) and only the owner
// clears it, so a second thread of a kind alive at the same time stays on the heap for its whole life. The owner alone
// reaches its kind's per-kind state (pa.bypass[], read and written without atomics). The game starts no such second
// thread (decode, opener and loader start once per audio start-up and are joined at shut-down; the sfx worker starts
// once), but the claim does not depend on that.
#ifndef PA_TEST_HOOK_WHO
#define PA_TEST_HOOK_WHO()
#endif
PA_CODE static int paWho(void) {
    Thread t = threadGetCurrent();
    PA_TEST_HOOK_WHO();
    if (!t) return 0;
    for (int k = 0; k < 4; k++)
        if (__atomic_load_n(&pa.who[k], __ATOMIC_RELAXED) == t) return k + 1;
    return 0;
}
void __real_CtrLab_runThread(void (*entry)(void*), void* arg);
void CtrArena_runThread(void (*entry)(void*), void* arg);
PA_CODE void CtrArena_runThread(void (*entry)(void*), void* arg) {
    int k = entry == decodeThreadMain ? 1
            : entry == openerThreadMain ? 2
            : entry == loaderThreadMain ? 3
            : entry == usWorkerMain     ? 4
                                        : 0;
    Thread t = k ? threadGetCurrent() : NULL, none = NULL;
    if (t && !__atomic_compare_exchange_n(&pa.who[k - 1], &none, t, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        t = NULL; // the slot has an owner: this thread stays on the heap for its whole life
    __real_CtrLab_runThread(entry, arg);
    if (t) __atomic_compare_exchange_n(&pa.who[k - 1], &t, NULL, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

// An arena block's address lies in [lo, lo + PA_SPAN). lo is one word, stored once (release) after the arena is
// valid and never again, so a reader sees 0 or the final base, never a mix. r2 stored lo and span separately, and a
// free(NULL) that read the old lo and the new span took NULL for an arena block (c26rev R159 F1). NULL, every address
// while lo is 0 and every heap address go to newlib. A relaxed load is enough: a thread holding an arena block got it
// through a hand-off that already orders the base's store before it, and a non-zero lo is always the final one.
PA_CODE static int paMine(const void* p) {
    uintptr_t lo = __atomic_load_n(&pa.lo, __ATOMIC_RELAXED);
    PA_TEST_HOOK_MINE();
    return p && lo && (uintptr_t) p - lo < PA_SPAN;
}

PA_CODE static int paBin(uint32_t s) {
    int b = 27 - __builtin_clz(s); // 16-31 B: 0
    return b < PA_BINS ? b : PA_BINS - 1;
}

PA_CODE static void paInsert(char* b, uint32_t s) { // b free, s bytes; the block before is in use
    PA_HEAD(b) = s | PA_PREV;
    *(uint32_t*) (b + s - 4) = s;
    PA_HEAD(b + s) &= ~PA_PREV;
    int i = paBin(s);
    PA_NEXT(b) = pa.bins[i];
    PA_BACK(b) = NULL;
    if (pa.bins[i]) PA_BACK(pa.bins[i]) = b;
    pa.bins[i] = b;
    pa.bits |= 1u << i;
}

PA_CODE static void paRemove(char* b) {
    int i = paBin(PA_SIZE(b));
    if (PA_NEXT(b)) PA_BACK(PA_NEXT(b)) = PA_BACK(b);
    if (PA_BACK(b))
        PA_NEXT(PA_BACK(b)) = PA_NEXT(b);
    else if (!(pa.bins[i] = PA_NEXT(b)))
        pa.bits &= ~(1u << i);
}

PA_CODE static char* paTake(uint32_t s) {
    int i = paBin(s);
    char* b = pa.bins[i];
    while (b && PA_SIZE(b) < s) b = PA_NEXT(b);
    if (!b) {
        uint32_t m = pa.bits & ~((2u << i) - 1u); // every block of a higher list fits
        if (!m) return NULL;
        b = pa.bins[__builtin_ctz(m)];
    }
    paRemove(b);
    uint32_t have = PA_SIZE(b);
    if (have - s >= PA_MIN) {
        paInsert(b + s, have - s);
        have = s;
    } else {
        PA_HEAD(b + have) |= PA_PREV;
    }
    PA_HEAD(b) = have | PA_USED | PA_PREV;
    pa.used += have;
    if (pa.used > pa.peak) pa.peak = pa.used;
    return b + 4;
}

PA_CODE static void paPut(char* b) {
    uint32_t s = PA_SIZE(b);
    pa.used -= s;
    char* n = b + s;
    if (!(PA_HEAD(n) & PA_USED)) {
        paRemove(n);
        s += PA_SIZE(n);
    }
    if (!(PA_HEAD(b) & PA_PREV)) {
        uint32_t back = ((uint32_t*) b)[-1];
        b -= back;
        paRemove(b);
        s += back;
    }
    paInsert(b, s);
}

// E1: a block of s bytes (s a multiple of 8, at least PA_MIN) whose user pointer is a multiple of a (16 or 32).
// devkitARM's malloc asks _memalign_r for 32-byte alignment for every request over 31 bytes, so without this an audio
// thread's every larger malloc (all of Tremor's and libogg's) bypassed the arena. Takes s + a + PA_MIN, then gives the
// leading piece (when the user pointer is not aligned yet) and a large enough tail back through paPut, each first made
// an in-use block of its own so paPut's coalescing keeps the free lists' invariant (no two free blocks adjacent).
PA_CODE static char* paTakeAligned(uint32_t s, uint32_t a) {
    char* q = paTake(s + a + PA_MIN);
    if (!q) return NULL;
    char* b = q - 4;
    uint32_t have = PA_SIZE(b);
    uint32_t g = (uint32_t) (-(uintptr_t) q & (a - 1)); // to the next multiple of a; q is 8-aligned
    if (g && g < PA_MIN) g += a; // the leading piece must hold a free block's links
    if (g) {
        char* nb = b + g;
        PA_HEAD(nb) = (have - g) | PA_USED | PA_PREV;
        PA_HEAD(b) = g | PA_USED | (PA_HEAD(b) & PA_PREV);
        paPut(b);
        b = nb;
        have -= g;
    }
    if (have - s >= PA_MIN) {
        char* t = b + s;
        PA_HEAD(t) = (have - s) | PA_USED | PA_PREV;
        PA_HEAD(b) = s | PA_USED | (PA_HEAD(b) & PA_PREV);
        paPut(t);
    }
    return b + 4;
}

// E1: a heap block an audio thread frees (a stream's path, a block from before the carving or from a full arena) waits
// on pa.heapFreed for the game thread's next malloc or free, which frees it: an audio thread never takes newlib's heap
// lock to free (the census's last class: ~0.1 a second, the decode thread at a stream's end), and no other thread
// holds it longer for them. The game thread allocates every frame, so a block waits about a frame. The link is the
// block's first word.
PA_CODE static void paHeapDrain(struct _reent* r) { // the game thread only: libctru's main thread has no Thread
    if (!__atomic_load_n(&pa.heapFreed, __ATOMIC_RELAXED) || threadGetCurrent()) return;
    for (char* p = __atomic_exchange_n(&pa.heapFreed, NULL, __ATOMIC_ACQUIRE); p;) {
        char* next = *(char**) p;
        __real__free_r(r, p);
        p = next;
    }
}

PA_CODE static void paLockDrain(void) {
    LightLock_Lock(&pa.lock);
    for (char* p = __atomic_exchange_n(&pa.deferred, NULL, __ATOMIC_ACQUIRE); p;) {
        char* next = *(char**) p;
        paPut(p - 4);
        p = next;
    }
}

#ifdef PACER_ARENA_LOG
PA_CODE static void paLogNow(void) { // test word: on each new 8 KB of peak
    LightLock_Lock(&pa.lock);
    uint32_t largest = 0, peak = pa.peak, used = pa.used;
    for (int i = 0; i < PA_BINS; i++)
        for (char* b = pa.bins[i]; b; b = PA_NEXT(b))
            if (PA_SIZE(b) > largest) largest = PA_SIZE(b);
    LightLock_Unlock(&pa.lock);
    logInfo(paLogPeak, (unsigned long) peak, (unsigned long) used, (unsigned long) largest, (unsigned long) pa.fallbacks,
            (unsigned long) pa.big, (unsigned long) pa.bigMax, (unsigned long) pa.allocs[0], (unsigned long) pa.allocs[1],
            (unsigned long) pa.allocs[2], (unsigned long) pa.allocs[3]);
}

void CtrArena_noteHeapLock(void);
PA_CODE __attribute__((noinline)) void CtrArena_noteHeapLock(void) { // inside the heap lock: no log, no allocation
    int who = paWho();
    if (!who) return;
    uint32_t n = __atomic_add_fetch(&paHlCount[who - 1], 1, __ATOMIC_RELAXED);
    uint32_t i = __atomic_fetch_add(&paHlPut, 1, __ATOMIC_RELAXED);
    volatile uint32_t mark = 0;
    const volatile uint32_t* s;
    __asm__("" : "=r"(s) : "0"(&mark)); // the stack above mark: hidden from -Warray-bounds
    PaHeapLock* e = &paHl[i % PA_HL_N];
    __atomic_store_n(&e->seq, 0, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    int k = 0;
    for (int j = 1; j < 112 && k < 8; j++) {
        uint32_t v = s[j];
        if (v >= 0x100000u && v < (uint32_t) (uintptr_t) __late_text_end) e->w[k++] = v;
    }
    while (k < 8) e->w[k++] = 0;
    e->n = n;
    e->who = (uint32_t) who;
    e->req = pa.req[who - 1];
    e->ms = (uint32_t) (svcGetSystemTick() / 268111u);
    __atomic_store_n(&e->seq, i + 1, __ATOMIC_RELEASE);
}

PA_CODE static void paHlDump(void) { // an audio thread, outside every lock
    for (;;) {
        uint32_t g = __atomic_load_n(&paHlGot, __ATOMIC_RELAXED), put = __atomic_load_n(&paHlPut, __ATOMIC_RELAXED);
        if (g >= put) return;
        if (put - g > PA_HL_N) { // overwritten before it was logged
            if (__atomic_compare_exchange_n(&paHlGot, &g, put - PA_HL_N, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                __atomic_add_fetch(&paHlLost, put - PA_HL_N - g, __ATOMIC_RELAXED);
            continue;
        }
        PaHeapLock* e = &paHl[g % PA_HL_N];
        if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != g + 1) return; // still being written
        PaHeapLock c = *e;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&e->seq, __ATOMIC_RELAXED) != g + 1) continue; // overwritten while copied
        if (!__atomic_compare_exchange_n(&paHlGot, &g, g + 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) continue;
        logInfo(paLogHl, (unsigned long) g, (unsigned long) c.who, (unsigned long) c.n,
                (unsigned long) ((uint32_t) (svcGetSystemTick() / 268111u) - c.ms), (unsigned long) c.req,
                (unsigned long) c.w[0], (unsigned long) c.w[1], (unsigned long) c.w[2], (unsigned long) c.w[3],
                (unsigned long) c.w[4], (unsigned long) c.w[5], (unsigned long) c.w[6], (unsigned long) c.w[7],
                (unsigned long) paHlCount[0], (unsigned long) paHlCount[1], (unsigned long) paHlCount[2],
                (unsigned long) paHlCount[3], (unsigned long) paHlLost);
    }
}
#endif

PA_CODE static void* paAlloc(struct _reent* r, int who, size_t n, uint32_t a) { // a: 8, 16 or 32; NULL: the heap
    uint32_t st = 0;
    if (pa.bypass[who - 1]) return NULL;
    if (__atomic_load_n(&pa.state, __ATOMIC_ACQUIRE) != 2 && __atomic_compare_exchange_n(&pa.state, &st, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        char* m = __real__malloc_r(r, PA_BYTES + 8); // 8-aligned: blocks start 4 past a multiple of 8
        if (m) {
            PA_HEAD(m + 4 + PA_BYTES) = PA_USED; // the end
            paInsert(m + 4, PA_BYTES);
            PA_TEST_HOOK_CARVE();
            __atomic_store_n(&pa.lo, (uintptr_t) m, __ATOMIC_RELEASE); // published whole, after the arena is valid
            logInfo(paLogReady, (unsigned long) PACER_ARENA_KB, (void*) m);
        }
        __atomic_store_n(&pa.state, m ? 2 : 3, __ATOMIC_RELEASE);
    }
    char* p = NULL;
    st = __atomic_load_n(&pa.state, __ATOMIC_ACQUIRE);
    if (st == 2) {
        paLockDrain();
        uint32_t s = n < PA_MIN - 4 ? PA_MIN : (uint32_t) (n + 11) & ~7u;
        p = a > 8 ? paTakeAligned(s, a) : paTake(s);
        LightLock_Unlock(&pa.lock);
#ifdef PACER_ARENA_LOG
        pa.allocs[who - 1]++;
        paHlDump();
        if (pa.peak >= pa.logged + 8192 || !(pa.allocs[who - 1] & 2047)) { // E1: also every 2048 of a kind's
            if (pa.peak > pa.logged) pa.logged = pa.peak;
            paLogNow();
        }
#endif
    }
    if (!p && st != 1) { // full, or no heap for it (not: another audio thread is carving it right now)
        uint32_t f = __atomic_add_fetch(&pa.fallbacks, 1, __ATOMIC_RELAXED);
        if (!(f & (f - 1))) logInfo(paLogFull, (unsigned long) PACER_ARENA_KB, (unsigned long) st, (unsigned long) n, (unsigned long) f);
    }
    return p;
}

// E1b: a loader read over PA_SMALL (an AUDO sound up to 424,840 B: utstep's DREAM census, the 6 heap-lock takes E1
// left, all the loader's) reads into a block the game thread took from the heap as it queued the read: requestLoad's
// signal ends here. Under loadLock the game thread gives each queued slot over PA_SMALL a 32-aligned block of its
// size + header + 1 (readRequest asks for size + header, flowend's piece read one more), replaces a queued slot's block
// of another size, and frees the block of a slot that is neither queued nor the loader's busy one (its read never
// asked: an open that failed). It never touches the busy slot's: the loader takes that (paBigTake, an exchange) while
// the slot is busy, and only the loader makes a slot busy or ends it. The block is then the read's like any other: the
// game thread adopts or frees it (collectLoads), and a loader free of it goes to the game thread (paHeapDrain).
PA_CODE static void CtrArena_loadSignal(LightEvent* ev) {
    CtrAudioSystem* ca = (CtrAudioSystem*) ((char*) ev - offsetof(CtrAudioSystem, loadEvent));
    LightLock_Lock(&ca->loadLock);
    __atomic_store_n(&pa.ca, ca, __ATOMIC_RELEASE);
    for (int i = 0; i < LOAD_SLOTS && !threadGetCurrent(); i++) { // the game thread only (it queues every read)
        LoadRequest* q = &ca->loads[i];
        uint32_t t = q->size + q->header + 1;
        int queued = q->state == LOAD_QUEUED && t > PA_SMALL;
        if (q->state != LOAD_BUSY && pa.rd[i] && (!queued || pa.rdN[i] != t)) {
            __real__free_r(_REENT, __atomic_exchange_n(&pa.rd[i], NULL, __ATOMIC_RELAXED));
        }
        if (queued && !pa.rd[i]) {
            pa.rdN[i] = t;
            __atomic_store_n(&pa.rd[i], (char*) __real__memalign_r(_REENT, 32, t), __ATOMIC_RELEASE);
        }
    }
    LightLock_Unlock(&ca->loadLock);
    LightEvent_Signal(ev);
}

PA_CODE static void* paBigTake(int who, size_t n) { // the loader, n over PA_SMALL: its busy slot's block, or NULL
    CtrAudioSystem* ca = __atomic_load_n(&pa.ca, __ATOMIC_ACQUIRE);
    if (who != 3 || !ca) return NULL;
    for (int i = 0; i < LOAD_SLOTS; i++) {
        if (ca->loads[i].state == LOAD_BUSY && __atomic_load_n(&pa.rd[i], __ATOMIC_ACQUIRE) && pa.rdN[i] - n <= 1u)
            return __atomic_exchange_n(&pa.rd[i], NULL, __ATOMIC_ACQUIRE);
    }
    return NULL;
}

PA_CODE void* __wrap__malloc_r(struct _reent* r, size_t n) {
    int w = paWho(), who = n <= PA_SMALL ? w : 0;
    void* p = who ? paAlloc(r, who, n, 8) : n > PA_SMALL ? paBigTake(w, n) : NULL;
    if (!w) paHeapDrain(r);
#ifdef PACER_ARENA_LOG
    if (n > PA_SMALL && w) {
        pa.big++;
        if (n > pa.bigMax) pa.bigMax = n;
    }
    if (p) return p;
    PA_REQ(w, 1, n);
    p = __real__malloc_r(r, n);
    PA_REQ(w, 0, 0);
    return p;
#else
    return p ? p : __real__malloc_r(r, n);
#endif
}
void* paCallocZero(void* p, int c, size_t n); // numcache (z_numcache.c): calloc's zeroing, the whole block
PA_CODE void* __wrap__calloc_r(struct _reent* r, size_t c, size_t n) {
    size_t t;
    int who = paWho();
    void* p = NULL;
    if (who && !__builtin_mul_overflow(c, n, &t) && t <= PA_SMALL && (p = paAlloc(r, who, t, 8))) return paCallocZero(p, 0, t);
    if (who) pa.bypass[who - 1]++;
    PA_REQ(who, 2, c * n);
    p = __real__calloc_r(r, c, n);
    PA_REQ(who, 0, 0);
    if (who) pa.bypass[who - 1]--;
    return p;
}

PA_CODE void* __wrap__realloc_r(struct _reent* r, void* p, size_t n) {
    if (paMine(p)) {
        size_t have = PA_SIZE((char*) p - 4) - 4;
        if (n <= have) return p;
        void* q = __wrap__malloc_r(r, n);
        if (q) {
            memcpy(q, p, have);
            CtrArena_freeR(r, p);
        }
        return q;
    }
    if (!p) return __wrap__malloc_r(r, n); // E1: realloc(NULL) is malloc, so an audio thread's goes to the arena
    int who = paWho();
    if (who) pa.bypass[who - 1]++;
    PA_REQ(who, 3, n);
    void* q = __real__realloc_r(r, p, n);
    PA_REQ(who, 0, 0);
    if (who) pa.bypass[who - 1]--;
    return q;
}

PA_CODE void* __wrap__memalign_r(struct _reent* r, size_t a, size_t n) {
    int who = paWho();
    void* p; // E1: devkitARM's malloc is _memalign_r(32) above 31 bytes; alignments up to 32 take an arena block
    if (who && a <= 32 && !(a & (a - 1)) && n <= PA_SMALL && (p = paAlloc(r, who, n, a < 8 ? 8 : (uint32_t) a))) return p;
    if (a <= 32 && n > PA_SMALL && (p = paBigTake(who, n))) return p; // E1b
    if (who) pa.bypass[who - 1]++;
    PA_REQ(who, 4, n);
    void* q = __real__memalign_r(r, a, n);
    PA_REQ(who, 0, 0);
    if (who) pa.bypass[who - 1]--;
    return q;
}

PA_CODE size_t CtrArena_usableSize(void* p) {
    return paMine(p) ? PA_SIZE((char*) p - 4) - 4 : malloc_usable_size(p);
}

PA_CODE void CtrArena_freeR(struct _reent* r, void* p) {
    if (!paMine(p)) {
        if (p && paWho()) { // E1: freed by the game thread's next call (paHeapDrain)
            char* head = __atomic_load_n(&pa.heapFreed, __ATOMIC_RELAXED);
            do *(char**) p = head;
            while (!__atomic_compare_exchange_n(&pa.heapFreed, &head, (char*) p, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
            return;
        }
        __real__free_r(r, p);
        paHeapDrain(r);
    } else if (paWho()) {
        paLockDrain();
        paPut((char*) p - 4);
        LightLock_Unlock(&pa.lock);
    } else {
        char* head = __atomic_load_n(&pa.deferred, __ATOMIC_RELAXED);
        do *(char**) p = head;
        while (!__atomic_compare_exchange_n(&pa.deferred, &head, (char*) p, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    }
}
#endif

#if defined(ENABLE_WAD16)
// gomusic (change omegaflowey, UNDERTALE only). The Flowey heart's first game over starts one song three times in one
// game step, as three looping streams at one gain. On PC the copies start within a buffer of each other and
// sound as one louder song. Here the opener thread opens streams
// one at a time (60-350 ms each), so the copies started 134-536 ms apart: the song heard over itself, as an echo.
// This code acts on that one song and no other (gomSong, by asset name: the name is equal on Steam and GOG). Every other
// sound gets no stamp (gomRequested), so nothing below holds, plans, seeks or scales it.
// Copies of the song asked for in one game step form a group (gomRequested, from startStream). A copy can start a
// step or more after it was asked for: it waited for a stream context, or (the first play of a song in a session) for
// the cache probe that finds the song too long to cache (CACHING_STREAMS_MAX). gomUpdate notes each step's last voice
// serial, so such a copy still joins the step it was asked for in (gomStepOf). The first member to open plays at once
// (the leader). Each later one opens paused (gomPhysPaused holds its first pause write), is sent to
// the sample the leader will be at GOM_MARGIN DSP frames on, and is unpaused in the frame callback in which the leader
// reaches that sample (gomDspCallback), so it joins sample-locked. Until then the leader's mix is scaled by the copies
// still to join that will sound as it does (same pitch and gain: gomLeaderMix, through gomPhysMix), so the song is as
// loud from its first sample as on PC, where every copy starts at once; at each join the leader's scale drops by one in
// the callback that unpauses the copy.
// Only exact copies join: same step, same pitch and gain (gomSame). The game sets those by separate calls after its
// play, the second game over its pitches a step after, so nothing is held, planned or scaled until the step after the
// request has ended (gomSettled). Anything else runs alone, as it did before this code: a copy that opens before then,
// a different pitch or gain, an expired step, and any pause, gain or pitch change, seek or stop on a copy still waiting,
// or on its leader. A copy that leaves goes back to its first sample, whether or not its plan's seek was taken, and
// stays held until that is queued (gomLeave); a seek of the game's is kept.
// The menu's pause (the suspend hook pauses the channels and leaves the voices as they are) is no time at all here: the
// callback's clock and its plans, joins and bounds hold until the menu lets go, so a join goes on after it where it was.
// One log line per copy, posted from the game thread (gomUpdate): when it was heard after the step that asked for it,
// and the copies' measured offset.
#define GOM_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text_gomusic")))
#define GOM_BSS __attribute__((section(".bss.gomusic_late")))
#define GOM_MARGIN 30 // DSP frames (4.9 ms each) from a follower's plan to its join: its seek and first buffers
#define GOM_LEAD 1 // frames from the callback that unpauses a channel to the first frame it plays
#define GOM_TRIES 3 // plans before a follower runs alone
#define GOM_HOLD_FRAMES 120 // a follower waits this long (about 590 ms) past its hold or planned join, then runs alone
#define GOM_MEASURE 40 // frames after a join to measure the offset again
#define GOM_UNSET (-2147483647 - 1)
#define GOM_STEPS 32 // steps remembered (about 1 s): a copy that started later than that is not joined

enum { GOM_NONE, GOM_LEADER, GOM_HELD, GOM_PLANNED, GOM_JOINED, GOM_DONE, GOM_LEAVING };

typedef struct {
    volatile uint32_t serial; // the owner voice's serial at the request
    volatile int32_t owner, sound;
    uint32_t batch0, batch1; // the game step: the audio window's start and frame count
    volatile uint8_t group; // 0, or the group's first context + 1
    volatile uint8_t phase;
    uint8_t tries, logged;
    int8_t phys; // physical channel + 1 while held; 0 otherwise (the zeroed BSS holds nothing)
    uint8_t copy;
    const char* said; // the log line's word for what it did
    uint32_t target; // a follower's first sample
    uint32_t joinFrame; // held: the callback it was held in; planned: the callback it joins at; joined: the one it joined in
    int32_t residual, offset[2];
    u64 cueTick, heardTick;
    // the leader's position, for the opener's plans: written in the callback (gom.seq), read with a retry
    uint32_t snapPos, snapFrame;
    float snapStep;
    float factor; // a leader's mix scale (gomLeaderMix); 0 until set
} GomCtx;

static struct {
    GomCtx c[STREAM_CONTEXTS];
    volatile uint32_t held; // physical channels held paused (bit p)
    volatile uint32_t frame; // frame callbacks
    volatile uint32_t seq; // odd while a leader snapshot is written
    volatile uint8_t active; // a group is open
    uint32_t stepNext; // steps noted (gomUpdate)
    volatile uint32_t mixSet; // contexts whose mix gomPhysMix has kept (bit k)
    volatile uint32_t logs; // contexts with a log line for gomUpdate to post (bit k)
    float mix[STREAM_CONTEXTS][12]; // each stream's mix as the engine last set it
    struct {
        uint32_t batch0, batch1, serialEnd; // a step's audio window and the first voice serial after it
    } steps[GOM_STEPS];
} gom GOM_BSS;

static const char gomJoinFormat[] __attribute__((section(".rodata.gomusic_in_text"))) =
    "Audio: gomusic '%s' copy %u %s %.0f ms after its cue: sample %lu, residual %ld, offset %ld then %ld, %u plan(s)\n";
// (named arrays: a string literal here would go to the file's .rodata.str1.1 and move the main rodata)
static const char gomLed[] __attribute__((section(".rodata.gomusic_in_text"))) = "led, heard";
static const char gomJoined[] __attribute__((section(".rodata.gomusic_in_text"))) = "joined, heard";
static const char gomRanAlone[] __attribute__((section(".rodata.gomusic_in_text"))) = "ran alone, heard";
static const char gomSong[] __attribute__((section(".rodata.gomusic_in_text"))) = "mus_gameover";

GOM_CODE static int gomPhys(const StreamContext* s) {
    int id = s->channel;
    return utaMapped && id >= 0 && id < UTA_CHANNELS ? utaMap[id] : id;
}

// The sample a stream's channel plays from, as the DSP last reported it. Under s->lock.
GOM_CODE static bool gomPos(const StreamContext* s, int p, uint32_t* out) {
    if (!s->opened || !s->everQueued || (unsigned) p >= UTA_CHANNELS) return false;
    u16 seq = UTA_PHYS_SEQ(p);
    u32 pos = UTA_PHYS_POS(p);
    for (int i = 0; i < STREAM_BUFFERS; i++) {
        const StreamBuffer* b = &s->bufs[i];
        if ((b->wb.status == NDSP_WBUF_PLAYING || b->wb.status == NDSP_WBUF_QUEUED) && b->wb.sequence_id == seq) {
            uint64_t f = b->startFrame + pos;
            if (s->totalFrames > 0) f %= s->totalFrames;
            *out = (uint32_t) f;
            return true;
        }
    }
    return false;
}

GOM_CODE static bool gomStamped(CtrAudioSystem* ca, int k) {
    const StreamContext* s = &ca->streams[k];
    const GomCtx* g = &gom.c[k];
    int owner = s->owner;
    return owner >= 0 && owner < VOICE_COUNT && g->owner == owner && g->serial == ca->voices[owner].serial &&
           s->state != CTX_FREE && s->state != CTX_DONE;
}

// Game thread, each audio update before the engine's own (the vtable's update entry): the voices played since the last
// update (serials below serialCounter) were asked for in the step just run, in the audio window noted with them.
#ifdef GOM_UPDATE_NEXT
GOM_CODE static void gomUpdate(AudioSystem* audio, float deltaTime) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    uint32_t i = gom.stepNext % GOM_STEPS;
    gom.steps[i].batch0 = (uint32_t) ca->windowStart;
    gom.steps[i].batch1 = ca->windowFrames;
    gom.steps[i].serialEnd = ca->serialCounter;
    gom.stepNext++;
    uint32_t logs = __atomic_exchange_n(&gom.logs, 0u, __ATOMIC_ACQ_REL); // not from the callback: posting takes a lock
    for (int k = 0; logs != 0 && k < STREAM_CONTEXTS; k++) {
        if (!((logs >> k) & 1u)) continue;
        const GomCtx* g = &gom.c[k];
        utaPost(ca, gomJoinFormat, ca->streams[k].name, (unsigned) g->copy, g->said,
                g->heardTick ? TICKS_TO_MS(g->heardTick - g->cueTick) : -1.0, (unsigned long) g->target, (long) g->residual,
                (long) g->offset[0], (long) g->offset[1], (unsigned) g->tries);
    }
    GOM_UPDATE_NEXT(audio, deltaTime);
}
#endif

// A request's voice asked for in an earlier step (it waited): that step's audio window, as if it had started there.
// Its step is the oldest noted one whose serialEnd is above its serial. 1 when it found one, 0 when it was asked for
// in this step, -1 when longer ago than GOM_STEPS (it joins no group).
GOM_CODE static int gomStepOf(GomCtx* g) {
    int found = -1;
    uint32_t n = gom.stepNext < GOM_STEPS ? gom.stepNext : GOM_STEPS;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = (gom.stepNext - 1 - i) % GOM_STEPS;
        if ((int32_t) (g->serial - gom.steps[e].serialEnd) >= 0) break; // asked for after this step
        found = (int) e;
        if (i + 1 == n && gom.stepNext > GOM_STEPS) return -1; // older than the steps noted
    }
    if (found < 0) return 0;
    g->batch0 = gom.steps[found].batch0;
    g->batch1 = gom.steps[found].batch1;
    return 1;
}

// The request's step and the one after it have ended, so the calls the game makes after its play have been made: gain
// in the step itself, the second game over's pitches in the next (its alarm, set to 1 by the alarm that plays).
GOM_CODE static bool gomSettled(const GomCtx* g) {
    uint32_t n = gom.stepNext;
    return n >= 2 && (int32_t) (gom.steps[(n - 2) % GOM_STEPS].serialEnd - g->serial) > 0;
}

// Two members that would sound as one from one position: same pitch and gain, neither paused.
GOM_CODE static bool gomSame(const CtrAudioSystem* ca, int a, int b) {
    const Voice* x = &ca->voices[gom.c[a].owner];
    const Voice* y = &ca->voices[gom.c[b].owner];
    return x->pitch == y->pitch && x->gain == y->gain && x->emitterGain == y->emitterGain && !x->paused && !y->paused &&
           !x->spatial && !y->spatial;
}

// A context gives its hold up (any thread): the channel gets the engine's own pause state, through the room-load and
// play-through holds, whoever owns it by now.
GOM_CODE static void gomUnhold(CtrAudioSystem* ca, GomCtx* g) {
    int p = __atomic_exchange_n(&g->phys, (int8_t) 0, __ATOMIC_ACQ_REL) - 1;
    if (p < 0) return;
    __atomic_fetch_and(&gom.held, ~(1u << p), __ATOMIC_ACQ_REL);
    int i = *CtrLoadmix_legacy.mapped ? CtrLoadmix_legacy.inverse[p] : p;
    const UtaChan* c = &CtrLoadmix_legacy.channels[p];
    bool logical = c->state != UTA_IDLE ? c->paused : ca->suspended || ca->voices[i].paused;
    // (The pause write's own entry: a direct call from here up to startStream makes GCC emit two existing late functions
    // in another order, and check_layout refuses that. With the bit clear it goes straight on to ptPhysPaused.)
    audofixPtPaused(p, logical || lmHeld());
}

GOM_CODE static void gomLog(int k, const char* said) {
    gom.c[k].said = said;
    gom.c[k].logged = 1;
    __atomic_fetch_or(&gom.logs, 1u << k, __ATOMIC_ACQ_REL);
}

// A member leaves its group and runs alone, as it would without this code. (A leader never does: its copies leave it.)
GOM_CODE static void gomAlone(CtrAudioSystem* ca, int k) {
    GomCtx* g = &gom.c[k];
    g->group = 0;
    g->phase = GOM_DONE;
    g->heardTick = svcGetSystemTick();
    gomUnhold(ca, g);
    gomLog(k, gomRanAlone);
}

// Game thread, in place of startStream's unlock: stamp the new request and group it with the streams of the same sound
// asked for in the same step, then let the lock go, so the opener can't open a request nobody has stamped (it wakes by itself).
GOM_CODE static void gomRequested(LightLock* lock) {
    CtrAudioSystem* ca = utaCa;
    for (int k = 0; ca != NULL && k < STREAM_CONTEXTS; k++) {
        StreamContext* s = &ca->streams[k];
        GomCtx* g = &gom.c[k];
        if (s->state != CTX_OPEN_REQUESTED || s->owner < 0 || s->owner >= VOICE_COUNT || gomStamped(ca, k)) continue;
        const Voice* v = &ca->voices[s->owner];
        gomUnhold(ca, g); // a held copy stopped and this context reused before the callback saw it
        g->group = 0;
        g->phase = GOM_NONE;
        g->tries = g->logged = g->copy = 0;
        __atomic_fetch_and(&gom.logs, ~(1u << k), __ATOMIC_ACQ_REL);
        if (strcmp(s->name, gomSong) != 0) { // any other sound: no stamp, so everything below passes it by
            g->owner = -1;
            continue;
        }
        g->target = 0;
        g->residual = 0;
        g->offset[0] = g->offset[1] = GOM_UNSET;
        g->sound = v->soundIndex;
        g->batch0 = (uint32_t) ca->windowStart;
        g->batch1 = ca->windowFrames;
        g->cueTick = svcGetSystemTick();
        g->heardTick = 0;
        g->snapStep = 0.0f;
        g->factor = 0.0f;
        __atomic_fetch_and(&gom.mixSet, ~(1u << k), __ATOMIC_ACQ_REL);
        g->owner = s->owner;
        g->serial = v->serial;
        if (s->seekPending) continue; // a start position of its own: not a copy
        int asked = gomStepOf(g); // asked for in an earlier step: that step's copies
        if (asked < 0) { // too long ago to know its step: joins nothing, and nothing joins it
            g->sound = -1;
            continue;
        }
        bool waited = asked > 0;
        uint8_t copies = 1;
        for (int j = 0; j < STREAM_CONTEXTS; j++) {
            GomCtx* h = &gom.c[j];
            if (j == k || !gomStamped(ca, j) || h->sound != g->sound || h->batch0 != g->batch0 || h->batch1 != g->batch1 ||
                ca->streams[j].seekPending || ca->streams[j].stopRequested)
                continue;
            if (waited) g->cueTick = h->cueTick; // heard: from the step's first request
            if (h->group == 0) {
                h->copy = 1;
                h->group = (uint8_t) (j + 1);
            }
            g->group = h->group;
            copies++;
        }
        if (g->group != 0) {
            g->copy = copies;
            gom.active = 1;
#ifdef CTR_AUDIO_CAPTURE_WRAP_H
            CtrCap_trigger();
#endif
        }
    }
    LightLock_Unlock(lock);
}

// The group's leader: the member that opened first (a member that opened before its group formed counts).
GOM_CODE static int gomLeader(CtrAudioSystem* ca, int group, int self) {
    for (int j = 0; j < STREAM_CONTEXTS; j++) {
        if (j != self && gom.c[j].group == group && gom.c[j].phase == GOM_LEADER && gomStamped(ca, j)) return j;
    }
    for (int j = 0; j < STREAM_CONTEXTS; j++) {
        if (j != self && gom.c[j].group == group && gom.c[j].phase == GOM_NONE && ca->streams[j].opened && gomStamped(ca, j)) {
            gom.c[j].phase = GOM_LEADER;
            return j;
        }
    }
    return -1;
}

// A follower's plan: the sample the leader will be at when the callback `margin` frames on unpauses it, as a seek.
// Under the follower's lock; needs the leader's snapshot (false when there is none yet).
GOM_CODE static bool gomPlan(CtrAudioSystem* ca, int k, int leader, uint32_t margin) {
    GomCtx* g = &gom.c[k];
    const GomCtx* l = &gom.c[leader];
    StreamContext* s = &ca->streams[k];
    uint32_t pos, frame;
    float step;
    for (;;) {
        uint32_t seq = gom.seq;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        pos = l->snapPos;
        frame = l->snapFrame;
        step = l->snapStep;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (!(seq & 1u) && seq == gom.seq) break;
    }
    if (step <= 0.0f || s->sampleRate == 0) return false;
    uint32_t join = gom.frame + margin;
    double target = (double) pos + (double) (join - frame + GOM_LEAD) * step;
    if (s->totalFrames > 0) target = fmod(target, (double) s->totalFrames);
    g->target = (uint32_t) (target + 0.5);
    g->joinFrame = join;
    g->tries++;
    s->seekPending = true;
    s->seekSeconds = ((double) g->target + 0.5) / (double) s->sampleRate;
    g->phase = GOM_PLANNED;
    return true;
}

// A leader's mix scale: itself plus each copy still to join (not opened, held or planned) whose voice has its pitch and
// gain, so it sounds as all of them would together. Written when it changes. Under the leader's lock.
GOM_CODE static void gomLeaderMix(CtrAudioSystem* ca, int k) {
    GomCtx* l = &gom.c[k];
    if (!((gom.mixSet >> k) & 1u) || l->owner < 0 || l->owner >= VOICE_COUNT || !gomSettled(l)) return;
    float f = 1.0f;
    for (int j = 0; j < STREAM_CONTEXTS; j++) {
        const GomCtx* h = &gom.c[j];
        if (j == k || h->group != l->group || !gomStamped(ca, j) || ca->streams[j].stopRequested) continue;
        if (h->phase != GOM_NONE && h->phase != GOM_HELD && h->phase != GOM_PLANNED) continue;
        if (gomSame(ca, j, k)) f += 1.0f;
    }
    if (f == l->factor || (l->factor == 0.0f && f == 1.0f)) return;
    l->factor = f;
    float m[12];
    for (int i = 0; i < 12; i++) m[i] = gom.mix[k][i] * f;
    GOM_MIX_NEXT(gomPhys(&ca->streams[k]), m);
}

// utaChnSetMix's physical mix write (game or opener thread, under the stream's lock), through gomMixLink: keeps a
// stream's mix and scales a leader's.
GOM_CODE void gomPhysMix(int p, float mix[12]) {
    CtrAudioSystem* ca = utaCa;
    for (int k = 0; ca != NULL && k < STREAM_CONTEXTS; k++) {
        const StreamContext* s = &ca->streams[k];
        if (s->channel < 0 || gomPhys(s) != p || !gomStamped(ca, k)) continue;
        memcpy(gom.mix[k], mix, sizeof(gom.mix[k]));
        __atomic_fetch_or(&gom.mixSet, 1u << k, __ATOMIC_ACQ_REL);
        float f = gom.c[k].factor;
        if (f > 1.0f) {
            float m[12];
            for (int i = 0; i < 12; i++) m[i] = mix[i] * f;
            GOM_MIX_NEXT(p, m);
            return;
        }
        break;
    }
    GOM_MIX_NEXT(p, mix);
}

// lmPaused's physical pause write (any thread), before the play-through hold. A group member's first write is the
// opener's (openStream, under the stream's lock): a follower is held from there.
GOM_CODE void gomPhysPaused(int p, bool paused) {
    CtrAudioSystem* ca = utaCa;
    if ((unsigned) p < 32 && ((gom.held >> p) & 1u)) {
        paused = true;
    } else if (gom.active && ca != NULL && (unsigned) p < UTA_CHANNELS) {
        for (int k = 0; k < STREAM_CONTEXTS; k++) {
            StreamContext* s = &ca->streams[k];
            GomCtx* g = &gom.c[k];
            if (g->group == 0 || g->phase != GOM_NONE || s->state != CTX_OPENING || s->opened || gomPhys(s) != p ||
                !gomStamped(ca, k))
                continue;
            int leader = gomLeader(ca, g->group, k);
            if (leader < 0) {
                g->phase = GOM_LEADER; // plays at once, at the group's gain
                gomLeaderMix(ca, k);
                break;
            }
            if (s->seekPending || ca->streams[leader].seekPending || !gomSettled(g) || !gomSame(ca, k, leader)) {
                gomAlone(ca, k);
                break;
            }
            g->phys = (int8_t) (p + 1);
            g->phase = GOM_HELD;
            g->joinFrame = gom.frame;
            s->sampleRate = s->decoder.sampleRate;
            s->totalFrames = s->decoder.totalFrames;
            gomPlan(ca, k, leader, GOM_MARGIN); // seeks before the decode thread fills a buffer; else the callback plans
            __atomic_fetch_or(&gom.held, 1u << p, __ATOMIC_ACQ_REL);
            paused = true;
            break;
        }
    }
    ptPhysPaused(p, paused);
}

// The callback's release of a held channel, at its join.
GOM_CODE static void gomRelease(CtrAudioSystem* ca, GomCtx* g, uint32_t now) {
    g->phase = GOM_JOINED;
    g->joinFrame = now;
    g->heardTick = svcGetSystemTick();
    gomUnhold(ca, g);
}

// A seek that is not this code's plan, on a copy still waiting. Under s->lock.
GOM_CODE static bool gomSeekedAway(const StreamContext* s, const GomCtx* g) {
    if (g->phase == GOM_HELD) return s->seekPending;
    if (s->seekPending) return s->seekSeconds != ((double) g->target + 0.5) / (double) s->sampleRate;
    return s->everQueued && s->bufs[0].startFrame != g->target;
}

// A copy still waiting leaves its group. The base path has it at its first sample, so its plan, taken or not, gives way
// to a seek there, and it stays held until that is queued (GOM_LEAVING). A seek of the game's stays. Under s->lock.
GOM_CODE static void gomLeave(CtrAudioSystem* ca, int k, uint32_t now) {
    GomCtx* g = &gom.c[k];
    StreamContext* s = &ca->streams[k];
    if (g->phase != GOM_PLANNED || gomSeekedAway(s, g)) {
        gomAlone(ca, k);
        return;
    }
    s->seekPending = true;
    s->seekSeconds = 0.0;
    g->phase = GOM_LEAVING;
    g->joinFrame = now;
    LightEvent_Signal(&ca->event);
}

// Buffers queued to start from. Under s->lock.
GOM_CODE static bool gomReady(const StreamContext* s) {
    if ((s->state != CTX_PLAYING && s->state != CTX_DRAINING) || !s->opened || s->seekPending || !s->everQueued) return false;
    int queued = 0;
    for (int i = 0; i < STREAM_BUFFERS; i++) {
        u8 st = s->bufs[i].wb.status;
        if (st == NDSP_WBUF_QUEUED || st == NDSP_WBUF_PLAYING) queued++;
    }
    return queued >= 2 || s->state == CTX_DRAINING;
}

// Leader minus follower, in samples, as the DSP last reported both. Under the follower's lock.
GOM_CODE static bool gomOffset(CtrAudioSystem* ca, int leader, const StreamContext* s, int p, int32_t* out) {
    if (leader < 0 || LightLock_TryLock(&ca->streams[leader].lock) != 0) return false;
    uint32_t lpos, fpos;
    bool ok = gomPos(&ca->streams[leader], gomPhys(&ca->streams[leader]), &lpos) && gomPos(s, p, &fpos);
    LightLock_Unlock(&ca->streams[leader].lock);
    if (ok) *out = (int32_t) (lpos - fpos);
    return ok;
}

// The DSP frame callback, first in the chain: then the play-through hold's, audofix's and the engine's.
GOM_CODE static void gomDspCallback(void* data) {
    ptDspCallback(data);
    CtrAudioSystem* ca = (CtrAudioSystem*) data;
    // (menuHeld too: the menu's own sounds clear the suspend flag for a moment.) A stop still gives its channel back below.
    bool frozen = ca->suspended || menuHeld != 0;
    if (!frozen) gom.frame++;
    if (!gom.active) return;
    uint32_t now = gom.frame;
    bool open = false;
    for (int k = 0; k < STREAM_CONTEXTS; k++) {
        GomCtx* g = &gom.c[k];
        if (g->group == 0 || g->phase == GOM_DONE) continue;
        StreamContext* s = &ca->streams[k];
        if (!gomStamped(ca, k) || s->stopRequested) { // stopped or gone: its channel is the engine's again
            gomUnhold(ca, g);
            g->phase = GOM_DONE;
            continue;
        }
        open = true;
        if (frozen) continue;
        if (g->phase == GOM_NONE) {
            // opened unheld (before its group formed): the leader, or, behind another leader, left as it plays
            if (s->opened && gomLeader(ca, g->group, -1) != k) g->phase = GOM_DONE;
            continue;
        }
        if (LightLock_TryLock(&s->lock) != 0) continue;
        int p = gomPhys(s);
        if (g->phase == GOM_LEADER) {
            uint32_t pos;
            if (gomPos(s, p, &pos)) {
                if (!g->heardTick && pos > 0) g->heardTick = svcGetSystemTick();
                gom.seq++;
                __atomic_thread_fence(__ATOMIC_RELEASE);
                g->snapPos = pos;
                g->snapFrame = now;
                g->snapStep = 160.0f * CtrLoadmix_legacy.channels[p].rate / (float) NDSP_SAMPLE_RATE;
                __atomic_thread_fence(__ATOMIC_RELEASE);
                gom.seq++;
            }
            if (g->heardTick && !g->logged) gomLog(k, gomLed);
            bool pending = false; // the leader's snapshot serves its group's held and joining members
            for (int j = 0; j < STREAM_CONTEXTS; j++) {
                uint8_t ph = gom.c[j].phase;
                if (j != k && gom.c[j].group == g->group && (ph == GOM_NONE || ph == GOM_HELD || ph == GOM_PLANNED || ph == GOM_JOINED))
                    pending = true;
            }
            if (g->logged && !pending) g->phase = GOM_DONE;
        } else if (g->phase == GOM_HELD || g->phase == GOM_PLANNED) {
            int leader = gomLeader(ca, g->group, k);
            uint32_t lpos = 0;
            bool have = false;
            if (leader >= 0 && LightLock_TryLock(&ca->streams[leader].lock) == 0) {
                have = gomPos(&ca->streams[leader], gomPhys(&ca->streams[leader]), &lpos);
                LightLock_Unlock(&ca->streams[leader].lock);
            }
            bool late = (int32_t) (now - g->joinFrame) > GOM_HOLD_FRAMES;
            bool differs = leader < 0 || ca->streams[leader].seekPending || !gomSame(ca, k, leader) || gomSeekedAway(s, g);
            if (late || (differs && (g->phase == GOM_HELD || s->seekPending || s->everQueued))) {
                gomLeave(ca, k, now); // the leader stopped or never got there, or either copy changed: no longer a copy
            } else if (differs) {
                // a seek just taken with nothing queued from it: whose it was shows in a moment
            } else if (g->phase == GOM_HELD) {
                if (gomPlan(ca, k, leader, GOM_MARGIN)) LightEvent_Signal(&ca->event);
            } else if (have && (int32_t) (now - g->joinFrame) >= 0) {
                float step = gom.c[leader].snapStep;
                int32_t half = (int32_t) (step * 0.5f);
                int32_t residual = (int32_t) (g->target - (lpos + (uint32_t) ((float) GOM_LEAD * step + 0.5f)));
                bool ready = gomReady(s) && s->bufs[0].startFrame == g->target;
                g->residual = residual;
                if (ready && residual <= half && residual >= -half) {
                    gomRelease(ca, g, now);
                } else if (ready && residual > half) {
                    // the leader is behind its plan (held by a room load, say): wait for it
                } else if (g->tries < GOM_TRIES) {
                    gomPlan(ca, k, leader, GOM_MARGIN << g->tries);
                    LightEvent_Signal(&ca->event);
                } else {
                    gomLeave(ca, k, now);
                }
            }
        } else if (g->phase == GOM_LEAVING) {
            if (gomReady(s) || (int32_t) (now - g->joinFrame) > GOM_HOLD_FRAMES) gomAlone(ca, k);
        } else if (g->phase == GOM_JOINED) {
            uint32_t since = now - g->joinFrame;
            int leader = gomLeader(ca, g->group, k);
            if (g->offset[0] == GOM_UNSET && since >= GOM_LEAD + 3) gomOffset(ca, leader, s, p, &g->offset[0]);
            else if (g->offset[0] != GOM_UNSET && g->offset[1] == GOM_UNSET && since >= GOM_MEASURE)
                gomOffset(ca, leader, s, p, &g->offset[1]);
            if (leader < 0 || since > GOM_MEASURE + 20 || g->offset[1] != GOM_UNSET) {
                gomLog(k, gomJoined);
                g->phase = GOM_DONE;
            }
        }
        LightLock_Unlock(&s->lock);
    }
    // After every join above: a leader's scale drops in the callback that unpauses the copy, so both reach the same
    // DSP frame.
    for (int k = 0; k < STREAM_CONTEXTS; k++) {
        GomCtx* g = &gom.c[k];
        if (g->group == 0 || (g->phase != GOM_LEADER && !(g->factor > 1.0f)) || !gomStamped(ca, k) || ca->streams[k].stopRequested)
            continue;
        if (LightLock_TryLock(&ca->streams[k].lock) != 0) continue;
        gomLeaderMix(ca, k);
        LightLock_Unlock(&ca->streams[k].lock);
    }
    uint32_t held = gom.held;
    for (int p = 0; held != 0 && p < UTA_CHANNELS; p++) {
        if ((held >> p) & 1u) UTA_PHYS_PAUSED(p, true); // after every other writer in this callback
    }
    if (!open && held == 0) gom.active = 0;
}
#endif

#if defined(ENABLE_WAD16) && defined(CTRPORT_TARGET_O3DS)
// sfxshed (z_utstall.c's utshedFinish, UNDERTALE Old): every sound of the room's plan (4 bytes an entry: sound, size) counts
// as just played from here to CtrAudioSystem_keepPlanEnd, so its shed and its passes keep them all (shedVictim's rule).
// flowend's list holds the plan's first FLOWEND_KEEP only. played: a play time an entry; on the game thread, as every play.
__attribute__((noinline, noipa, section(".text_sfxshed"))) void CtrAudioSystem_keepPlan(const uint8_t* plan, uint32_t count, uint64_t* played) {
    CtrAudioSystem* ca = gShutdownTarget;
    u64 now = svcGetSystemTick();
    for (uint32_t i = 0; ca != NULL && count > i; i++) {
        uint16_t sound;
        memcpy(&sound, plan + (size_t) i * 4, 2);
        if (sound >= ca->soundCount) continue;
        played[i] = ca->sfxCache[sound].lastPlayTick;
        ca->sfxCache[sound].lastPlayTick = now;
    }
}

// The play times back, last first (a sound planned twice gets the time it had before both).
__attribute__((noinline, noipa, section(".text_sfxshed"))) void CtrAudioSystem_keepPlanEnd(const uint8_t* plan, uint32_t count, const uint64_t* played) {
    CtrAudioSystem* ca = gShutdownTarget;
    for (uint32_t i = count; ca != NULL && i-- > 0;) {
        uint16_t sound;
        memcpy(&sound, plan + (size_t) i * 4, 2);
        if (sound < ca->soundCount) ca->sfxCache[sound].lastPlayTick = played[i];
    }
}
#endif

// audpar: UT 1.08 includes paused instances in audio_is_playing (PC audpar-pause-01).
// Music controllers use that membership to avoid starting another copy while a
// track is paused. Keep audofixIsPlaying's menu hold handling and its call target.
// The old 132 B query slot becomes a 4 B jump, with an unreachable 128 B tail.
#if defined(ENABLE_WAD16)
__attribute__((noinline, noipa, optimize("Os"), section(".text.late.audpar")))
bool audparIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (voiceMatches(ca, i, soundOrInstance)) return true;
    }
    return false;
}
#if defined(__3DS__) && defined(NDEBUG)
__asm__(".pushsection .text.ctrIsPlaying, 1\n.space 128\n.popsection");
#endif
#endif

// audpar: PC UT stores asset positions as future-play offsets. Put the defaults
// after the existing length cache in the same allocation, so normal destruction
// owns them and stop-all / game_restart do not reset them. No struct layout moves.
#if defined(ENABLE_WAD16)
#define AUDPAR_OFFSET_CODE __attribute__((noinline, noipa, section(".text.late.audpar.offset")))
AUDPAR_OFFSET_CODE void* audparLengthStorage(size_t bytes, const char* file, int line) {
    void* p = safeMallocFunction(bytes * 2u, file, line);
    memset((uint8_t*) p + bytes, 0, bytes);
    return p;
}

AUDPAR_OFFSET_CODE void audparClearVoice(Voice* v, CtrAudioSystem* ca, int32_t soundIndex) {
    memset(v, 0, sizeof(*v));
    v->ctx = -1;
    if (soundIndex >= 0 && (uint32_t) soundIndex < ca->soundCount) {
        float offset = ca->soundLengths[ca->soundCount + (uint32_t) soundIndex];
        v->startPosition = offset;
        v->clockSeconds = offset;
        v->virtualSeconds = offset;
    }
}

AUDPAR_OFFSET_CODE
float audparGetTrackPosition(AudioSystem* audio, int32_t soundOrInstance) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    if (soundOrInstance >= 0 && (uint32_t) soundOrInstance < ca->soundCount)
        return ca->soundLengths[ca->soundCount + (uint32_t) soundOrInstance];
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (!voiceMatches(ca, i, soundOrInstance)) continue;
        return GetTime_virtualClockOn() ? clockPosition(ca, &ca->voices[i]) : voicePosition(ca, i);
    }
    return 0.0f;
}

AUDPAR_OFFSET_CODE
void audparSetTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    if (soundOrInstance >= 0 && (uint32_t) soundOrInstance < ca->soundCount) {
        ca->soundLengths[ca->soundCount + (uint32_t) soundOrInstance] = positionSeconds < 0.0f ? 0.0f : positionSeconds;
        return;
    }
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

#undef AUDPAR_OFFSET_CODE
#if defined(__3DS__) && defined(NDEBUG) && !defined(CTR_AUDIO_PCM_HASH)
__asm__(".pushsection .text.ctrGetTrackPosition, 1\n.space 980\n.popsection\n"
        ".pushsection .text.ctrSetTrackPosition, 1\n.space 396\n.popsection\n");
#endif
#endif

#if defined(ENABLE_WAD16) && defined(CTRPORT_TARGET_O3DS) && defined(CTRPORT_HAVE_TREMOR)
// ===[ gounder: copies of the song at unequal pitches share one decode (late code, UNDERTALE Old) ]===
// The heart's Alarm 8, the second game over, plays the song three times in one step, gain 0.45, and its Alarm 5 sets the
// pitches a step later: 2.81, 3 and 2.64. Alarm 6 ends it 190 steps on, so it lasts about 6.3 s. Three decoders ask for
// 8.45 audio seconds per second, at about 250 ms per audio second on an Old model: about 211% of the decode thread, so
// about 125 underrun lines a run on the o2ds (New has 0).
// All three play one file from its first sample, so they want one stream of samples, each at its own speed. So one
// decoder fills a ring and each copy's reads copy out of the ring. The fastest copy's read is what decodes: pitch 3's
// demand, about 75% of the thread, and a memcpy for the others. Every copy gets the decoder's samples byte for byte.
// The ring only has to hold what lies between the slowest copy and the fastest: (3 - 2.64) * 6.33 s = 2.28 s = 100,600
// frames, plus up to 16,384 frames each read position is ahead of what is heard. GUN_RING_FRAMES holds 160,000, which
// lasts as long as the game over runs at 21 frames a second or better.
// The copies after the first open no decoder, read-ahead ring or file, so they start with the first instead of 400 and
// 1,500 ms behind it on the o2ds.
// Only copies gomusic has grouped (same song, gomSong, same step) and not joined: the first game over's are equal and
// stay gomusic's, and a lone game over has no group. Whether they are unequal is known once gomSettled says so, so the
// first copy's open waits for that, usually not at all: its open takes longer than two steps on these models.
// A copy that can't go on here goes back to a decoder of its own at the same sample, then runs as it would have: a game
// seek, the song's end, the ring having moved past it, the decoding copy stopped. In the game only a stop happens, of all
// three, at once. Threads: the opener opens, the decode thread reads and closes. The last copy to go frees the ring: it is
// in the heap, never linear memory (640 KB; a failed allocation leaves the base path), for as long as the copies last.
// statsLock keeps a join and the free apart. shutdownAudio's close, past both joins, retires any group still there.
// gomusic's state and helpers are reached by asm aliases, and nothing of gomusic's calls this block: a direct use adds
// call-graph edges, and GCC then emits audofix's, pcmwait's and gomusic's functions in another order (check_layout).
#define GUN_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text_gounder")))
#define GUN_BSS __attribute__((section(".bss.gounder_late")))
#define GUN_RING_FRAMES 160000u
#define GUN_SETTLE_TRIES 30 // 5 ms each: a stalled game thread can't hold the song's first sound longer than this
#define GUN_LOG_STEP 32000u // a log line each time the slowest copy falls this much further behind (about 0.7 s)

static struct {
    int16_t* volatile ring;
    volatile uint32_t members; // contexts reading from the ring (bit k)
    volatile int32_t source; // the context whose decoder fills it + 1; 0 once it no longer does
    uint32_t head; // frames decoded; the ring holds [head - GUN_RING_FRAMES, head)
    uint32_t lagPeak, lagLog;
    uint32_t channels, sampleRate;
    uint64_t totalFrames;
    uint32_t batch0, batch1;
    uint8_t group, copies;
} gun GUN_BSS;

extern __typeof__(gom) gunGom;
bool gunGomStamped(CtrAudioSystem* ca, int k);
bool gunGomSettled(const GomCtx* g);
bool gunGomSame(const CtrAudioSystem* ca, int a, int b);

static const char gunBeginFormat[] __attribute__((section(".rodata.gounder_in_text"))) =
    "Audio: gounder '%s' shared decode: %u KB ring, waited %d ms for the pitches\n";
static const char gunJoinFormat[] __attribute__((section(".rodata.gounder_in_text"))) =
    "Audio: gounder context %d shares the decode, head %lu\n";
static const char gunLagFormat[] __attribute__((section(".rodata.gounder_in_text"))) =
    "Audio: gounder slowest copy %lu frames behind, head %lu\n";
static const char gunLeftFormat[] __attribute__((section(".rodata.gounder_in_text"))) =
    "Audio: gounder context %d left at sample %lu, head %lu\n";
static const char gunEndFormat[] __attribute__((section(".rodata.gounder_in_text"))) =
    "Audio: gounder done: %u copies, %lu frames decoded, slowest %lu frames behind\n";
static const char gunNoMemory[] __attribute__((section(".rodata.gounder_in_text"))) =
    "Audio: gounder no memory for the ring, the copies decode apart\n";

// A stream's decoder: its context, or -1 for any other decoder.
GUN_CODE static int gunContext(const CtrAudioSystem* ca, const CtrDecoder* dec) {
    if (ca == NULL) return -1;
    uintptr_t off = (uintptr_t) dec - (uintptr_t) &ca->streams[0].decoder;
    uint32_t k = off / sizeof(StreamContext);
    return off % sizeof(StreamContext) == 0 && k < STREAM_CONTEXTS ? (int) k : -1;
}

// The ring's decoder can still be read. Decode thread, the only one that closes a playing stream's decoder.
GUN_CODE static bool gunSourceLive(const CtrAudioSystem* ca) {
    int src = gun.source - 1;
    if (src < 0) return false;
    const StreamContext* s = &ca->streams[src];
    if (((gun.members >> src) & 1u) && s->state == CTX_PLAYING && !s->stopRequested) return true;
    gun.source = 0;
    return false;
}

// A copy has gone. The last takes the ring with it. Each thread only drops its own copy, and a read needs its copy's bit.
GUN_CODE static void gunDrop(CtrAudioSystem* ca, int k) {
    int16_t* freed = NULL;
    unsigned copies = 0;
    uint32_t head = 0, lag = 0;
    LightLock_Lock(&ca->statsLock);
    __atomic_fetch_and(&gun.members, ~(1u << k), __ATOMIC_ACQ_REL);
    if (gun.source == k + 1) gun.source = 0;
    if (gun.members == 0) {
        freed = gun.ring;
        gun.ring = NULL;
        gun.source = 0;
        copies = gun.copies;
        head = gun.head;
        lag = gun.lagPeak;
    }
    LightLock_Unlock(&ca->statsLock);
    if (freed != NULL) { // the opener may have begun another group by now
        free(freed);
        utaPost(ca, gunEndFormat, copies, (unsigned long) head, (unsigned long) lag);
    }
}

// A copy goes on with a decoder of its own, at its sample. dec->positionFrames is that sample throughout. The decoding
// copy has its decoder already, at the ring's head; the others open one, on the calling thread. False when it can't.
GUN_CODE static bool gunLeave(CtrAudioSystem* ca, int k, CtrDecoder* dec) {
    StreamContext* s = &ca->streams[k];
    uint64_t at = dec->positionFrames;
    uint32_t head = gun.head; // this group's: once it has dropped, the opener may begin another
    utaPost(ca, gunLeftFormat, k, (unsigned long) at, (unsigned long) head);
    gunDrop(ca, k);
    if (dec->vorbisOpen) return at == head || (CtrDecoder_seek)(dec, at);
    if (s->path == NULL || !GUN_OPEN_FILE_NEXT(dec, s->path)) return false;
    CtrDecoder_startReadAhead(dec);
    return at == 0 || (CtrDecoder_seek)(dec, at);
}

// The stream layer's read (utaDecRead; any thread, but only the decode thread reads a member's decoder).
GUN_CODE int32_t gunRead(CtrDecoder* dec, int16_t* out, uint32_t maxFrames) {
    uint32_t members = gun.members;
    if (members == 0) return GUN_READ_NEXT(dec, out, maxFrames);
    CtrAudioSystem* ca = utaCa;
    int k = gunContext(ca, dec);
    if (k < 0 || !((members >> k) & 1u)) return GUN_READ_NEXT(dec, out, maxFrames);
    uint32_t at = (uint32_t) dec->positionFrames, ch = gun.channels;
    if (at == gun.head && gunSourceLive(ca)) {
        CtrDecoder* d = &ca->streams[gun.source - 1].decoder;
        uint64_t mine = d->positionFrames;
        uint32_t w = gun.head % GUN_RING_FRAMES;
        d->positionFrames = gun.head;
        int32_t got = GUN_READ_NEXT(d, gun.ring + w * ch, GUN_RING_FRAMES - w);
        d->positionFrames = mine;
        if (got > 0) gun.head += (uint32_t) got;
        else gun.source = 0; // the song's end or a decode error: its copy finds the same on its own decoder
    }
    uint32_t lag = gun.head - at;
    if (lag == 0 || lag > GUN_RING_FRAMES) return gunLeave(ca, k, dec) ? GUN_READ_NEXT(dec, out, maxFrames) : -1;
    if (lag > gun.lagPeak) {
        gun.lagPeak = lag;
        if (lag >= gun.lagLog) {
            gun.lagLog = lag + GUN_LOG_STEP;
            utaPost(ca, gunLagFormat, (unsigned long) lag, (unsigned long) gun.head);
        }
    }
    uint32_t r = at % GUN_RING_FRAMES, n = lag < maxFrames ? lag : maxFrames;
    if (n > GUN_RING_FRAMES - r) n = GUN_RING_FRAMES - r;
    memcpy(out, gun.ring + r * ch, n * ch * sizeof(int16_t));
    dec->positionFrames = at + n;
    return (int32_t) n;
}

// The stream layer's seek: a copy's own decoder serves it.
GUN_CODE bool gunSeek(CtrDecoder* dec, uint64_t frame) {
    uint32_t members = gun.members;
    if (members != 0) {
        CtrAudioSystem* ca = utaCa;
        int k = gunContext(ca, dec);
        if (k >= 0 && ((members >> k) & 1u) && !gunLeave(ca, k, dec)) return false;
    }
    return GUN_SEEK_NEXT(dec, frame);
}

// Opener thread, the first copy just opened: the ring, if its group's copies turn out unequal.
GUN_CODE static void gunBegin(CtrAudioSystem* ca, int k, const CtrDecoder* dec) {
    StreamContext* s = &ca->streams[k];
    const GomCtx* g = &gunGom.c[k];
    int waited = 0;
    while (!gunGomSettled(g) && waited < GUN_SETTLE_TRIES && !s->stopRequested) {
        svcSleepThread(5000000LL);
        waited++;
    }
    if (!gunGomSettled(g) || s->stopRequested || s->seekPending || s->path == NULL || !gunGomStamped(ca, k)) return;
    bool differs = false;
    for (int j = 0; j < STREAM_CONTEXTS; j++) {
        if (j != k && gunGom.c[j].group == g->group && gunGomStamped(ca, j) && !ca->streams[j].stopRequested && !gunGomSame(ca, j, k))
            differs = true;
    }
    if (!differs) return;
    bool allowed = CtrMemory_allowRelief(false); // an optional ring: it never goes to linear memory for want of heap
    int16_t* ring = (int16_t*) malloc(GUN_RING_FRAMES * dec->channels * sizeof(int16_t));
    CtrMemory_allowRelief(allowed);
    if (ring == NULL) {
        utaPost(ca, gunNoMemory);
        return;
    }
    LightLock_Lock(&ca->statsLock);
    gun.head = 0;
    gun.lagPeak = 0;
    gun.lagLog = GUN_LOG_STEP;
    gun.channels = dec->channels;
    gun.sampleRate = dec->sampleRate;
    gun.totalFrames = dec->totalFrames;
    gun.group = g->group;
    gun.batch0 = g->batch0;
    gun.batch1 = g->batch1;
    gun.copies = 1;
    gun.source = k + 1;
    gun.ring = ring;
    __atomic_store_n(&gun.members, 1u << k, __ATOMIC_RELEASE);
    LightLock_Unlock(&ca->statsLock);
    utaPost(ca, gunBeginFormat, s->name, (unsigned) (GUN_RING_FRAMES * dec->channels * sizeof(int16_t) / 1024u), waited * 5);
}

// Opener thread, a later copy of the group sharing the decode: the context's decoder only holds the song's format and
// the copy's position (no file, no Tremor state: CtrDecoder_close has nothing to do for it, and the read below serves it).
GUN_CODE static bool gunJoin(CtrAudioSystem* ca, int k, CtrDecoder* dec) {
    const StreamContext* s = &ca->streams[k];
    const GomCtx* g = &gunGom.c[k];
    bool ok = false;
    LightLock_Lock(&ca->statsLock);
    int src = gun.source - 1;
    if (gun.ring != NULL && src >= 0 && src != k && g->group != 0 && g->group == gun.group && g->batch0 == gun.batch0 &&
        g->batch1 == gun.batch1 && gun.head <= GUN_RING_FRAMES && ((gun.members >> src) & 1u) && ca->streams[src].state == CTX_PLAYING &&
        !ca->streams[src].stopRequested && !s->seekPending && !s->stopRequested && s->path != NULL) {
        memset(dec, 0, sizeof(*dec));
        dec->kind = CTR_DECODER_VORBIS;
        dec->channels = gun.channels;
        dec->sampleRate = gun.sampleRate;
        dec->totalFrames = gun.totalFrames;
        gun.copies++;
        __atomic_fetch_or(&gun.members, 1u << k, __ATOMIC_ACQ_REL);
        ok = true;
    }
    LightLock_Unlock(&ca->statsLock);
    if (ok) utaPost(ca, gunJoinFormat, k, (unsigned long) gun.head);
    return ok;
}

// The stream layer's opens (utaDecOpenFile, utaDecOpenMemory). A context opening again has left whatever it read before.
GUN_CODE bool gunOpenFile(CtrDecoder* dec, const char* path) {
    CtrAudioSystem* ca = utaCa;
    int k = gunContext(ca, dec);
    if (k < 0) return GUN_OPEN_FILE_NEXT(dec, path);
    if ((gun.members >> k) & 1u) gunDrop(ca, k);
    if (ca->opener == NULL || threadGetCurrent() != ca->opener || !gunGomStamped(ca, k)) return GUN_OPEN_FILE_NEXT(dec, path);
    if (gunJoin(ca, k, dec)) return true;
    bool ok = GUN_OPEN_FILE_NEXT(dec, path);
    if (ok && dec->kind == CTR_DECODER_VORBIS && gun.ring == NULL && gunGom.c[k].group != 0) gunBegin(ca, k, dec);
    return ok;
}

GUN_CODE bool gunOpenMemory(CtrDecoder* dec, const uint8_t* data, uint32_t size) {
    int k = gunContext(utaCa, dec);
    if (k >= 0 && ((gun.members >> k) & 1u)) gunDrop(utaCa, k);
    return GUN_OPEN_MEMORY_NEXT(dec, data, size);
}

// finishStream's close, on the decode thread: a copy's way out. The decoder's own close has nothing to do for a copy that
// reads the ring, and the decoding copy's closes its decoder.
GUN_CODE void gunClose(CtrDecoder* dec) {
    if (gun.members != 0) {
        CtrAudioSystem* ca = utaCa;
        int k = gunContext(ca, dec);
        if (k >= 0 && ((gun.members >> k) & 1u)) gunDrop(ca, k);
    }
    CtrDecoder_close(dec);
}
__asm__(".set gunOpenFileLink, gunOpenFile\n.set gunOpenMemoryLink, gunOpenMemory\n.set gunReadLink, gunRead\n"
        ".set gunSeekLink, gunSeek\n.set gunCloseLink, gunClose\n.set gunGom, gom\n.set gunGomStamped, gomStamped\n"
        ".set gunGomSettled, gomSettled\n.set gunGomSame, gomSame\n");
#endif

// ===[ HOME: the sound stops with the picture (homeaudio) ]===
// Called on the game thread around libctru's jump to the HOME Menu (z_argcrash.c's end). Until now nothing here ran on
// HOME: the frozen picture went on under the music until aptDspSleep, late in the jump. The hold is the options menu's
// (setSuspended, see the menupause block): it ORs into each voice's pause, so a sound the game paused stays paused, and
// the channels stop in place at the DSP's next frame and go on from the same sample. No fade and no wait: the stop is the
// same kind of hard stop the DSP's sleep is, moved earlier, and the master volume is never touched. Time in the game's
// sounds was already held, as no frame runs and no audio update with it.
// - With the options menu open (or its hold not yet released) the game's sounds are already held and the menu owns the
//   hold: HOME leaves that hold alone, and returns to the menu with it. The menu's own sounds are out of that hold on
//   purpose and were still sounding, so HOME holds those itself (homeaudioHoldVoices), each by its instance, and lets only
//   those go on a running return. A menu sound still being read has no channel yet, and starts at a later audio update
//   as it always has. A stream not yet opened takes the pause from its stored flag when it opens.
// - The resume comes after the DSP is back and the real aptMainLoop has returned. Close from the HOME Menu returns
//   false and resumes nothing (shutdownAudio stops everything, as after the menu's Quit).
// - A second jump right after the return (libctru's flag) freezes again on the next call; a suspended flag already set
//   by anyone else leaves the hold theirs.
// - Sleep and Rosalina need nothing: libctru puts the DSP to sleep before the game's sleep hook, and Rosalina stops every
//   thread and the DSP together.
#ifdef ENABLE_WAD16
#define HOMEAUDIO_CODE __attribute__((noinline, noipa, section(".text_homeaudio")))
static bool homeaudioHeld __attribute__((section(".bss.homeaudio_late")));
static int32_t homeaudioVoices[VOICE_COUNT] __attribute__((section(".bss.homeaudio_late"))); // by voice: the instance held, or 0

// A voice counts as sounding by the pause last asked of its channel, not the DSP's: the load mix can hold a channel the
// voice still means to play. Every voice start goes through applyVoice, or takes its stream's flag, so the value is its own.
HOMEAUDIO_CODE static void homeaudioHoldVoices(CtrAudioSystem* ca) {
    bool held = ca->suspended;
    ca->suspended = true;
    for (int i = 0; i < VOICE_COUNT; i++) {
        homeaudioVoices[i] = 0;
        if (!voiceLive(ca, i)) continue;
        Voice* v = &ca->voices[i];
        if (v->kind == VOICE_SFX ? utaChan[utaPhys(i)].paused : v->kind != VOICE_STREAM || ca->streams[v->ctx].paused) continue;
        homeaudioVoices[i] = v->instanceId;
        applyVoice(ca, i);
    }
    ca->suspended = held;
}

HOMEAUDIO_CODE void CtrAudio_homeFreeze(void) {
    CtrAudioSystem* ca = gShutdownTarget;
    if (ca == NULL || !ca->initialized || !ca->ndspReady || ca->shutDown) return;
    if (ca->suspended || menuHeld != 0) {
        homeaudioHoldVoices(ca);
        return;
    }
    MENUPAUSE_OPAQUE(ca->base.vtable->suspend)(&ca->base);
    homeaudioHeld = true;
}

HOMEAUDIO_CODE void CtrAudio_homeThaw(void) {
    CtrAudioSystem* ca = gShutdownTarget;
    bool usable = ca != NULL && ca->initialized && !ca->shutDown;
    for (int i = 0; i < VOICE_COUNT; i++) {
        int32_t id = homeaudioVoices[i];
        if (id == 0) continue;
        homeaudioVoices[i] = 0;
        if (!usable || ca->voices[i].instanceId != id || !voiceLive(ca, i)) continue;
        bool held = ca->suspended;
        ca->suspended = false;
        applyVoice(ca, i);
        ca->suspended = held;
    }
    if (!homeaudioHeld) return;
    homeaudioHeld = false;
    if (usable) MENUPAUSE_OPAQUE(ca->base.vtable->resume)(&ca->base);
}
#endif

// packopen: startup only, while the loading bar owns the screen. Use a zero-byte
// request so the loader opens and owns its descriptor through the normal keeper.
// No sound is admitted, and no new persistent storage or in-play hook is needed.
#ifdef ENABLE_WAD16
#define PACKOPEN_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text.late.packopen")))
PACKOPEN_CODE static void packopenPrime(CtrAudioSystem* ca, const char* path) {
    if (path == NULL) return;
    LoadRequest* req = &ca->loads[0]; // startup: no sounds or other requests yet
    LightLock_Lock(&ca->loadLock);
    if (req->state != LOAD_FREE) {
        LightLock_Unlock(&ca->loadLock);
        return; // preserve an init-time request; normal first use can open later
    }
    memset(req, 0, sizeof(*req));
    req->path = path;
    req->soundIndex = -1;
    req->state = LOAD_QUEUED;
    LightLock_Unlock(&ca->loadLock);
    LightEvent_Signal(&ca->loadEvent);
    for (;;) {
        LightLock_Lock(&ca->loadLock);
        bool done = req->state == LOAD_DONE || req->state == LOAD_FAILED;
        LightLock_Unlock(&ca->loadLock);
        if (done) break;
        svcSleepThread(1000000); // loader can finish before boot proceeds
    }
    free(req->data); // readRequest allocates one byte even for an empty request
    LightLock_Lock(&ca->loadLock);
    memset(req, 0, sizeof(*req)); // LOAD_FREE, no synthetic sound reaches collectLoads
    LightLock_Unlock(&ca->loadLock);
}

// Same call signature as threadCreate: only the cold loader-start call retargets.
PACKOPEN_CODE static Thread packopenThreadCreate(ThreadFunc entry, void* arg, size_t stack,
        int priority, int core, bool detached) {
    Thread thread = threadCreate(entry, arg, stack, priority, core, detached);
    if (thread != NULL) {
        CtrAudioSystem* ca = arg;
        packopenPrime(ca, ca->groupPaths[0]);
    }
    return thread;
}

#ifdef CTRPORT_ADPCM_SFXPACK
// apOpen has already checked the pack and built its permanent path. Warm only
// valid packs, on the loader's own thread, after its main-thread handle is ready.
PACKOPEN_CODE static int packopenOpen(CtrAudioSystem* ca) {
    int status = apOpen(ca);
    if (status == 0 && ca->loader != NULL) packopenPrime(ca, apPath);
    return status;
}
#endif
#endif

// ===[ loadclass: the boot's two songs and the cast roll's first song opened ahead (change loadclass, UNDERTALE) ]===
// The intro's slides run on alarms in frames from its Create, and its song (story.ogg) is played 4 frames in; the title
// plays its noise burst (intronoise.ogg) on the frame the title is drawn; the cast roll starts its first song (cast_1)
// at its Create and rolls the names in frames. Each of these opened cold: story 188 ms p50 (2,395 console opens, 96%
// over 100 ms), intronoise 199 ms p50, cast_1 the same kind, so the music trailed the picture by that much where the PC
// game's song sounds within 22 ms of its call (pcref). This link uses flowend's
// phase-song spare, as its table rows and flowhitch rev 4 do, and its take: (1) at the opener's first waits, before any
// file has been opened (the boot, behind the loading bar), it opens story.ogg into the spare at once, so the intro's play
// takes it; (2) once story.ogg has opened it asks the spare for intronoise.ogg, and once leave.ogg (the true ending's
// last song before the roll) has opened, for cast_1.ogg: flowend opens them at its quiet second. A boot spare nobody
// took goes as soon as any other file opens (or the musical's room, flowhitch rev 4's, wants the spare); an
// intronoise spare goes once a file other than story.ogg opened last. Each song is asked for once: the spare's path
// stays its last request. Its own section after packopen's (late_reserve.ld), so no earlier late code moves; its own
// copies of flowend's helpers (a new caller of a flowend function reorders flowend's late code). No constants, no
// statics: the hashes are the FNV-1a of the file names before the extension (RoomPlan_fnv1a, as the table's).
// (3) The random battles' song (loadclass 4b). Every random encounter outside Hard Mode plays the same song from the
// battle room's first frame, opened cold there (maintainer's card, s8b7: 604 ms the first time, 322-355 ms each time after).
// The battle starter object's Create sets the area song's volume to 0 and pauses it, then flashes the SOUL with the
// noise burst sound for 32 frames (about 1.07 s) before the room changes; the SOUL's fall sound comes in the same
// frames. A song paused at volume 0 while either sound plays opens the battle song into the spare at once, so the
// battle's play takes it. The request ends when that paused song resumes or goes (the spare, if untaken, is dropped);
// an untaken spare also goes once another file opened since the request (its hash is kept after the path's NUL).
#if defined(ENABLE_WAD16) && defined(CTRPORT_HAVE_TREMOR)
#define LOADCLASS_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text.late.loadclass")))
#define LOADCLASS_STORY 0x11DB2AFEu // the intro story's song
#define LOADCLASS_NOISE 0xA5B89721u // the title's noise burst
#define LOADCLASS_LEAVE 0x72B05C5Cu // the true ending's last song before the roll
#define LOADCLASS_CAST1 0xD1C9C77Au // the cast roll's first song
#define LOADCLASS_BATTLE 0x49B7442Eu // the random battles' song
#define LOADCLASS_FLASH 0xC86C9FD9u // the battle transition's noise burst (a sound's name, not a file's)
#define LOADCLASS_FALL 0xE858600Eu // the SOUL's fall to the battle box (a sound's name)
#define LOADCLASS_STASH (FLOWEND_AHEAD_PATH - 4) // where the battle request keeps the last opened file's hash

// The FNV-1a hash of a name's last part before its extension (flowhitchFileHash's, its own copy).
LOADCLASS_CODE static uint32_t loadclassHash(const char* file) {
    const char* base = strrchr(file, '/');
    base = base != NULL ? base + 1 : file;
    const char* dot = strrchr(base, '.');
    return RoomPlan_fnv1a((const uint8_t*) base, dot != NULL ? (size_t) (dot - base) : strlen(base));
}

// Opener thread: the phase-song spare goes (flowendSongDrop's, its own copy).
LOADCLASS_CODE static void loadclassDrop(void) {
    if (flowendSongState == 2) CtrDecoder_close(&flowendSong);
    flowendSongState = 0;
}

// Opener thread: asks the phase-song spare for the sound whose file name hashes to want, by the path its play opens
// (true: asked). An earlier request or open spare is replaced, as a table row replaces it.
LOADCLASS_CODE static bool loadclassAsk(CtrAudioSystem* ca, uint32_t want) {
    for (uint32_t i = 0; ca->base.audioGroups[0]->sond.count > i; i++) {
        const Sound* sound = &ca->base.audioGroups[0]->sond.sounds[i];
        if (sound->file == NULL || loadclassHash(sound->file) != want) continue;
        char* path = resolveExternalPath(ca, sound);
        bool ok = path != NULL && FLOWEND_AHEAD_PATH > strlen(path);
        if (ok) {
            loadclassDrop();
            strcpy(flowendSongPath, path);
            flowendSongState = 1;
        }
        free(path);
        return ok;
    }
    return false;
}

// Opener thread: opens the spare's asked-for path at once (the boot's and the battle's: no quiet second comes first).
LOADCLASS_CODE static void loadclassOpen(CtrAudioSystem* ca) {
    u64 t0 = svcGetSystemTick();
    bool ok = __real_CtrDecoder_openFile(&flowendSong, flowendSongPath);
    if (ok && flowendSong.kind != CTR_DECODER_VORBIS) {
        CtrDecoder_close(&flowendSong);
        ok = false;
    }
    flowendSongState = ok ? 2 : 0;
    flowendLastOpen = svcGetSystemTick(); // the other spares wait for a quiet second of their own
    if (ok) postMessage(ca, flowendAheadLog, flowendSongPath, TICKS_TO_MS(flowendLastOpen - t0));
}

// Opener thread: 0 no song paused at volume 0; 1 one is; 2 one is while the transition's flash or fall sounds. The
// game thread owns the voices: each field is read once, and a stale read only delays or skips a guess.
LOADCLASS_CODE static int loadclassBattle(CtrAudioSystem* ca) {
    int paused = 0, flash = 0;
    uint32_t count = ca->base.audioGroups[0]->sond.count;
    for (int i = 0; VOICE_COUNT > i; i++) {
        const volatile Voice* v = &ca->voices[i];
        VoiceKind kind = v->kind;
        int32_t index = v->soundIndex;
        if (kind == VOICE_STREAM) {
            if (v->paused && v->fadeTarget == 0.0f) paused = 1;
        } else if ((kind == VOICE_SFX || kind == VOICE_PENDING) && index >= 0 && count > (uint32_t) index) {
            const char* name = ca->base.audioGroups[0]->sond.sounds[index].name;
            uint32_t h = name != NULL ? loadclassHash(name) : 0;
            if (h == LOADCLASS_FLASH || h == LOADCLASS_FALL) flash = 1;
        }
    }
    return paused + (paused & flash);
}

// The note link before flowhitch's (opener thread, at each note; flowend's wrap calls it first).
LOADCLASS_CODE void CtrAudioSystem_chainNoteLoadclass(const char* state) {
    CtrAudioSystem* ca = gShutdownTarget;
    bool mine = ca != NULL && ca->opener != NULL && threadGetCurrent() == ca->opener && strcmp(state, flowendWaiting) == 0;
    // A spare of this link's nobody took goes first, so flowhitch's link (rev 4) finds the spare free in the same wait.
    if (mine && flowendAheadOwner == ca && flowendSongState != 0) {
        uint32_t held = loadclassHash(flowendSongPath);
        uint32_t last = flowendLastPath[0] != 0 ? loadclassHash(flowendLastPath) : 0;
        uint32_t stash;
        memcpy(&stash, flowendSongPath + LOADCLASS_STASH, sizeof(stash));
        if ((held == LOADCLASS_STORY && (last != 0 || __atomic_load_n(&flowhitchRoomIs, __ATOMIC_RELAXED))) || (held == LOADCLASS_NOISE && last != LOADCLASS_STORY) ||
            (held == LOADCLASS_BATTLE && (last != stash || __atomic_load_n(&flowhitchRoomIs, __ATOMIC_RELAXED))))
            loadclassDrop();
    }
    CtrAudioSystem_chainNoteFlowhitch(state);
    if (!mine || flowendAheadOwner != ca || ca->openerQuit) return;
    for (int i = 0; STREAM_CONTEXTS > i; i++) {
        if (ca->streams[i].state == CTX_OPEN_REQUESTED) return; // a play waits: never ahead of it
    }
    if (flowendLastPath[0] == 0) {
        // The boot: no file opened yet and the spare never asked for. Opened now, not at a quiet second: the boot's
        // tile loads leave none before the intro's play.
        if (flowendSongState != 0 || flowendSongPath[0] != 0) return;
        if (!loadclassAsk(ca, LOADCLASS_STORY)) {
            flowendSongPath[0] = '-'; // no such file: asked once (a name no hash matches; no constant)
            flowendSongPath[1] = 0;
            return;
        }
        loadclassOpen(ca);
        return;
    }
    uint32_t last = loadclassHash(flowendLastPath);
    // (3) The battle song: asked for once per pause (the path stays its request until the paused song resumes or goes).
    int battle = loadclassBattle(ca);
    bool asked = flowendSongPath[0] != 0 && loadclassHash(flowendSongPath) == LOADCLASS_BATTLE;
    if (asked && battle == 0) {
        loadclassDrop();
        flowendSongPath[0] = 0;
        return;
    }
    if (battle == 2 && !asked && flowendSongState == 0 && !__atomic_load_n(&flowhitchRoomIs, __ATOMIC_RELAXED)) {
        if (loadclassAsk(ca, LOADCLASS_BATTLE) && LOADCLASS_STASH > strlen(flowendSongPath)) {
            memcpy(flowendSongPath + LOADCLASS_STASH, &last, sizeof(last));
            loadclassOpen(ca);
        } else {
            loadclassDrop();
        }
        return;
    }
    uint32_t want = last == LOADCLASS_STORY ? LOADCLASS_NOISE : last == LOADCLASS_LEAVE ? LOADCLASS_CAST1 : 0;
    if (want == 0 || (flowendSongPath[0] != 0 && loadclassHash(flowendSongPath) == want)) return; // asked once
    loadclassAsk(ca, want);
}
#endif

// ===[ musicloop: a stopped stream fades out as on PC; a loop seam is smoothed on both channels ]===
//
// PC (UNDERTALE 1.08, GameMaker's OpenAL Soft) never cuts a playing stream: audio_stop_sound fades it out linearly from
// its gain to silence over about 24 ms (change musicloop's pcref captures: the Ruins and first boss tracks, also stopped halfway
// through an audio_sound_gain fade). A track started in the same step starts at once and at its full gain while the old
// one fades. A pause is a plain cut on PC too, so pauses are left alone. Here a stop cleared the stream's channel: the
// DSP cut the music mid-waveform at its next 160-sample frame (a click). Now the stop leaves the stream's queued buffers
// on its channel and rewrites their samples in place: as they are for the frames the DSP may already have read past the
// position it reports, then the 24 ms ramp, then silence. The channel is marked as holding a tail (utaudio's TAILING, no
// segments, cur = the stream context), so a voice that starts a sound on it is moved to another channel (utaChnReset),
// the frame callback cuts it once the ramp has played (one stand-in piece keeps it live until tailEnd), and the stream
// context is not handed out again until then (its buffers are still queued). A stream already silent (mix 0, as after
// a game fade to 0), paused, or with no more queued than the DSP may have read is cleared as before.
//
// A loop seam: utaudio smooths a seam whose step is at least 4x the file's largest step, decided per channel, so
// one deep loop's right channel (2438 against 677, 3.6x) kept its step beside a smoothed left channel (Undyne's house,
// the True Lab). Now once one channel qualifies the other is smoothed too (utaudio's own ~3 ms decaying offset: on a
// channel whose ends meet, the small difference between the end's slope continued and the start).
//
// The hooks (ctr_audio_utaudio.h) keep every existing function's size and call graph; see there.
#if defined(ENABLE_WAD16)
#define ML_CODE __attribute__((noinline, noipa, used, section(".text_musicloop")))
#define ML_FADE_SECONDS 0.024f // PC's stop fade
#define ML_LEAD_FRAMES 4 // DSP frames past the reported position that the DSP may have read before the samples change

// The file's own function, by its assembler name (no new call-graph edge).
UtaSeam* mlSeamFor(CtrDecoder* dec, int* index) __asm__("utaSeamFor");

// True while a stopped stream's buffers fade out on a channel, so its context keeps them.
ML_CODE static bool mlFading(int ctx) {
    bool fading = false;
    LightLock_Lock(&utaLock);
    for (int p = 0; p < UTA_CHANNELS; p++) {
        const UtaChan* c = &utaChan[p];
        if (c->state == UTA_TAILING && c->segs == 0 && c->cur == ctx) fading = true;
    }
    LightLock_Unlock(&utaLock);
    return fading;
}

// claimStreamContext (ML_CLAIM, after audofixFreeStreamContext): the free context k, unless its buffers still fade; then
// another free one, or -1 (the claim waits a frame, as when every context is taken and one is on its way out).
ML_CODE static int mlClaim(CtrAudioSystem* ca, int k) {
    if (k < 0 || !mlFading(k)) return k;
    for (int i = 0; i < STREAM_CONTEXTS; i++) {
        const StreamContext* s = &ca->streams[i];
        if ((s->state == CTX_FREE || (s->state == CTX_DONE && s->owner < 0)) && !mlFading(i)) return i;
    }
    return -1;
}

// Game thread, from stopStreamVoice (under the stream's lock; channel p idle in utaudio). True when the stream's
// buffers were left to fade out on p; false when p is to be cleared now.
ML_CODE static bool mlFade(int p) {
    CtrAudioSystem* ca = utaCa;
    if (ca == NULL) return false;
    int id = utaInv[p], ctx = -1;
    for (int k = 0; k < STREAM_CONTEXTS && ctx < 0; k++) {
        const StreamContext* t = &ca->streams[k];
        if (t->channel == id && t->stopRequested && t->opened && t->state != CTX_FREE && t->state != CTX_DONE) ctx = k;
    }
    if (ctx < 0) return false;
    StreamContext* s = &ca->streams[ctx];
    UtaChan* c = &utaChan[p];
    if (s->paused || c->paused || s->channels < 1 || s->channels > 2) return false;
    bool audible = false;
    for (int i = 0; i < 12; i++) audible |= s->mix[i] != 0.0f;
    if (!audible) return false; // already silent: the game faded it out
    float rate = c->rate > 0.0f ? c->rate : (float) s->sampleRate;
    if (!(rate > 0.0f)) return false;
    float perFrame = rate * (160.0f / 32728.498f); // source frames in a DSP frame
    uint32_t lead = (uint32_t) (ML_LEAD_FRAMES * perFrame) + 8;
    uint32_t len = (uint32_t) (rate * ML_FADE_SECONDS + 0.5f);

    // The queued buffers in play order, and the frames of the first one already played.
    u16 seq = UTA_PHYS_SEQ(p);
    u32 pos = UTA_PHYS_POS(p);
    ndspWaveBuf* order[STREAM_BUFFERS];
    int n = 0;
    for (int i = 0; i < STREAM_BUFFERS; i++) {
        ndspWaveBuf* w = &s->bufs[i].wb;
        if (w->status != NDSP_WBUF_QUEUED && w->status != NDSP_WBUF_PLAYING) continue;
        int j = n++;
        while (j > 0 && (u16) (order[j - 1]->sequence_id - seq) > (u16) (w->sequence_id - seq)) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = w;
    }
    if (n == 0) return false;
    uint32_t skip = order[0]->sequence_id == seq ? (pos < order[0]->nsamples ? pos : order[0]->nsamples) : 0;
    // Azahar's DSP takes in a buffer whole when it starts it, so changes to the playing one are never heard: the ramp
    // starts no earlier than the next buffer (up to one buffer, 93 ms at 44.1 kHz, later than PC's).
    if (order[0]->sequence_id == seq && order[0]->nsamples - skip > lead) lead = order[0]->nsamples - skip;
    uint32_t total = 0;
    for (int i = 0; i < n; i++) total += order[i]->nsamples;
    total -= skip;
    if (total <= lead + 8) return false; // about to run out anyway
    if (len > total - lead) len = total - lead; // the stream ends sooner: the ramp ends with it

    // From the first frame not yet played: [lead, lead + len) becomes the ramp and everything queued after it silence,
    // so a cut that comes late still cuts silence.
    uint32_t chans = s->channels, at = 0, stop = lead + len;
    for (int i = 0; i < n; i++) {
        ndspWaveBuf* w = order[i];
        uint32_t first = i == 0 ? skip : 0, frames = w->nsamples - first, next = at + frames;
        int16_t* x = (int16_t*) w->data_pcm16 + first * chans; // frame at of the queue
        uint32_t a = at > lead ? at : lead, b = next < stop ? next : stop;
        for (uint32_t f = a; f < b; f++) {
            float g = ((float) (stop - f) - 0.5f) / (float) len;
            for (uint32_t k = 0; k < chans; k++) {
                float v = (float) x[(f - at) * chans + k] * g;
                x[(f - at) * chans + k] = (int16_t) (v < 0.0f ? v - 0.5f : v + 0.5f);
            }
        }
        uint32_t z = at > stop ? at : stop;
        if (z < next) memset(x + (z - at) * chans, 0, (next - z) * chans * sizeof(int16_t));
        if (a < next) UTA_DECLICK_FLUSH(x + (a - at) * chans, (next - a) * chans * sizeof(int16_t));
        at = next;
    }

    LightLock_Lock(&utaLock);
    bool idle = c->state == UTA_IDLE;
    if (idle) {
        c->state = UTA_TAILING;
        c->segs = 0;
        c->cur = (uint8_t) ctx;
        c->started = 1;
        utaWb[p][0].status = NDSP_WBUF_QUEUED; // a stand-in for the stream's buffers: the callback cuts at tailEnd
        c->tailEnd = utaCb + (uint32_t) ((float) (lead + len) / perFrame) + 6;
    }
    LightLock_Unlock(&utaLock);
    return idle;
}

// UTA_PHYS_CLEAR: a stopped stream's channel fades (mlFade); every other clear is the physical one.
ML_CODE static void mlPhysClear(int p) {
    if (utaActive && p >= 0 && p < UTA_CHANNELS && utaChan[p].state == UTA_IDLE && mlFade(p)) return;
    ML_CLEAR_NEXT(p);
}


// utaDecRead's read (UTA_PHYS_READ). At a loop splice, when one channel's step qualifies for smoothing, a channel whose
// step doesn't is given the largest step that lets it qualify (step / 4), so utaDecRead smooths both. Only the splice
// of a measured looping stream is touched, and maxStep is used for nothing else once measured (each splice of the file
// has the same step); the seam's log line shows the lowered value for that channel.
ML_CODE static int32_t mlRead(CtrDecoder* dec, int16_t* out, uint32_t maxFrames) {
    int32_t got = ML_READ_NEXT(dec, out, maxFrames);
    if (got <= 0 || dec->channels != 2) return got;
    int idx = 0;
    UtaSeam* s = mlSeamFor(dec, &idx);
    if (s == NULL || !s->splice || !s->measured || s->have < 2 || !utaCa->streams[idx].loop) return got;
    int32_t step[2];
    bool any = false;
    for (int c = 0; c < 2; c++) {
        step[c] = abs((int32_t) out[c] - (int32_t) s->last[c]);
        if (step[c] > 0 && step[c] >= UTA_SEAM_RATIO * s->maxStep[c]) any = true;
    }
    for (int c = 0; c < 2 && any; c++)
        if (step[c] > 0 && step[c] < UTA_SEAM_RATIO * s->maxStep[c]) s->maxStep[c] = step[c] / UTA_SEAM_RATIO;
    return got;
}

// The options menu's hold and HOME stop the game's sound dead, as on PC. A stopped stream's fade has no voice left for
// their pause to reach, so it is cut here at once, the way the frame callback cuts it at tailEnd (utaClearPhys, by its
// assembler name: no new call-graph edge). Cached sound effects' short tails are left as they were.
void mlClearPhys(int p, UtaChan* c) __asm__("utaClearPhys");
void mlHomeFreezeNext(void) __asm__("CtrAudio_homeFreeze");
ML_CODE static void mlCutFades(void) {
    if (!utaActive) return;
    LightLock_Lock(&utaLock);
    for (int p = 0; p < UTA_CHANNELS; p++) {
        UtaChan* c = &utaChan[p];
        if (c->state == UTA_TAILING && c->segs == 0) mlClearPhys(p, c);
    }
    LightLock_Unlock(&utaLock);
}

// menuHeldUpdate, as the hold starts (ML_MENU_PAUSE(true)): the fades first, then the picture as before.
ML_CODE void mlMenuPause(bool pause) {
    if (pause) mlCutFades();
    CtrVideo_menuPause(pause);
}

// homeaudioMainLoop (z_argcrash.c), before the jump to HOME: the fades first, then the freeze as before.
ML_CODE void mlHomeFreeze(void) {
    CtrAudioSystem* ca = gShutdownTarget;
    if (ca != NULL && ca->initialized && ca->ndspReady && !ca->shutDown) mlCutFades();
    mlHomeFreezeNext();
}

// F5: a resumed sound fades in, as on PC. There, audio_resume_sound (the game's caster_resume) and audio_resume_all
// (a timer's Draw GUI End, when the window gets focus back) bring a paused sound back linearly from silence over about
// 24 ms (pcref musicloop-pcq-pc-01, musicloop-pcr-pc-01/02: 13 of 16 resumes, of a stream and of a cached tone; the
// other 3 started at once, with no pattern by call or sound). Here the DSP took the channel back at its full mix (a
// click). The options menu's and HOME's release (the vtable's resume) stand for that focus: the game's sounds come back
// the same way.
//
// How: a resume first arms the physical channel of each voice it will unpause (mlArm, mlIn = ML_IN_ARMED: only a
// channel that is paused now, so a sound still playing never dips). The unpause's own mix write (mlPhysMix, the last
// step before the DSP) keeps the mix as the target and writes silence, and its pause write (mlPhysPaused) is held back
// (ML_IN_WANT). The DSP ramps every mix change across its frame, from the gain it latched last, and it latches the mix
// of a paused channel too (Azahar's HLE source.cpp, its firmware model; its capture of the first build of this fix
// showed the frame after the resume falling from full). So the silence must reach the DSP a frame before the unpause:
// the frame callback (mlFrameLock, under utaLock; libctru runs it before it sends that frame's writes) marks it sent
// (ML_IN_LATCHED), and the next callback unpauses the channel with the first step. One step a frame (4.9 ms), each
// PC's line at the frame's end, j * ML_IN_STEP: 0.20, 0.41, 0.61, 0.82, then the mix itself, so the DSP's ramps draw
// PC's line from silence (24.4 ms), one frame (two at most) after the resume. A mix the game sets meanwhile (a gain
// fade) becomes the target. A channel paused again holds its step; a stop (its fade-out or stop pending) ends the fade
// (one not yet heard goes back to its mix and its held unpause, as before this fix), and a reset (a new sound on the
// channel) clears it. F1's way (rewriting the next queued buffer's samples) can't serve here: the paused buffer
// resumes first, an edit to the buffer the DSP plays may come too late (Azahar never sees one), and a cached sound
// plays the cache's shared copy.
//
// State, without new .bss: mlIn sits in UtaChan's padding; the target (left and right: the engine's mixes are front
// left and right only) in ca->ramps[p], the positioned-sound ramp of the voice with that number, which UNDERTALE never
// uses (no audio_play_sound_at/on, emitter or listener call in its code). A channel whose ramp is active, or a mix with
// more than front left and right, is left unfaded. The pause write comes from any thread, under utaLock or a stream's
// lock, so mlPhysPaused takes no lock: it and the callback change ML_IN_WANT by compare-and-swap. Once mlIn is 0 the
// mix and pause writes go straight to the DSP, unlocked, so a fade ends (mlRetire) with its own last writes first and
// mlIn = 0 last: a write that sees 0 then lands after them, never under them (R432c F1).
#define ML_IN_ARMED 1 // a resume will unpause it
#define ML_IN_ZERO 2 // its silent mix is written, maybe not sent yet
#define ML_IN_LATCHED 3 // the DSP has latched the silence; the unpause may go. Then ML_IN_LATCHED + j: step j written
#define ML_IN_WANT 0x80 // an unpause held back (the last pause write asked to play)
#define ML_IN_STEP ((160.0f / 32728.498f) / ML_FADE_SECONDS) // the gain PC's line gains in one DSP frame
void mlRegisterNext(ndspCallback callback, void* data) __asm__("lmRegister");
void mlResumeSoundNext(AudioSystem* audio, int32_t soundOrInstance) __asm__("ctrResumeSound");
void mlResumeAllNext(AudioSystem* audio) __asm__("ctrResumeAll");
void mlResumeNext(AudioSystem* audio) __asm__("ctrResume");
int mlPhysOf(int id) __asm__("utaPhys");

// The gain at the end of fade-in frame j (1 = the first frame heard): PC's line there; 1 once past its end.
ML_CODE static float mlInGain(int j) {
    float g = (float) j * ML_IN_STEP;
    return g < 1.0f ? g : 1.0f;
}

// Channel p's fade target at gain g (physical write).
ML_CODE static void mlInMix(int p, float g) {
    const MixRamp* r = &utaCa->ramps[p];
    float m[12] = { 0 };
    m[0] = r->tgt[0] * g;
    m[1] = r->tgt[1] * g;
    ML_MIX_NEXT(p, m);
}

// Ends channel p's fade (under utaLock; the caller has written its last mix): a held unpause goes out, then mlIn = 0.
// A pause written meanwhile (it clears ML_IN_WANT first) fails the exchange, and the loop pauses the channel again.
ML_CODE static void mlRetire(int p) {
    UtaChan* c = &utaChan[p];
    uint8_t v = __atomic_load_n(&c->mlIn, __ATOMIC_ACQUIRE);
    bool on = false; // this call's last pause write was an unpause
    do {
        bool want = (v & ML_IN_WANT) != 0;
        if (want != on) ML_PAUSED_NEXT(p, !want);
        on = want;
    } while (!__atomic_compare_exchange_n(&c->mlIn, &v, (uint8_t) 0, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

// kind 0: audio_resume_sound(id); 1: audio_resume_all; 2: the suspend's release (options menu, HOME). Game thread.
ML_CODE static void mlArm(CtrAudioSystem* ca, int kind, int32_t id) {
    if (!utaActive) return;
    bool instance = id >= SOUND_INSTANCE_ID_BASE && id < AUDIO_STREAM_INDEX_BASE;
    LightLock_Lock(&utaLock);
    for (int i = 0; i < VOICE_COUNT; i++) {
        const Voice* v = &ca->voices[i];
        if ((v->kind != VOICE_SFX && v->kind != VOICE_STREAM) || v->spatial) continue;
        if (kind == 2 ? !ca->suspended || v->paused : ca->suspended || !v->paused) continue;
        if (kind == 0 && (instance ? v->instanceId != id : v->soundIndex != id)) continue;
        int p = mlPhysOf(i);
        if ((unsigned) p < UTA_CHANNELS && utaChan[p].paused && !ca->ramps[p].active)
            __atomic_store_n(&utaChan[p].mlIn, (uint8_t) ML_IN_ARMED, __ATOMIC_RELEASE);
    }
    LightLock_Unlock(&utaLock);
}

// After the resume: a channel it didn't write a mix to (a stream not open yet) isn't faded.
ML_CODE static void mlDisarm(void) {
    if (!utaActive) return;
    LightLock_Lock(&utaLock);
    for (int p = 0; p < UTA_CHANNELS; p++)
        if (utaChan[p].mlIn == ML_IN_ARMED) __atomic_store_n(&utaChan[p].mlIn, (uint8_t) 0, __ATOMIC_RELEASE);
    LightLock_Unlock(&utaLock);
}

ML_CODE static void mlResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    mlArm((CtrAudioSystem*) audio, 0, soundOrInstance);
    mlResumeSoundNext(audio, soundOrInstance);
    mlDisarm();
}

ML_CODE static void mlResumeAll(AudioSystem* audio) {
    mlArm((CtrAudioSystem*) audio, 1, 0);
    mlResumeAllNext(audio);
    mlDisarm();
}

ML_CODE static void mlResume(AudioSystem* audio) {
    mlArm((CtrAudioSystem*) audio, 2, 0);
    mlResumeNext(audio);
    mlDisarm();
}

// utaSetCallback's registration of the frame callback (audio start, before any sound, so before usw copies the vtable;
// the system's current vtable is patched too, should that copy ever come first).
ML_CODE void mlRegister(ndspCallback callback, void* data) {
    CtrAudioSystem* ca = utaCa;
    for (int k = 0; callback != NULL && k < 2; k++) {
        AudioSystemVtable* t = k == 0 ? &ctrAudioSystemVtable : ca != NULL ? (AudioSystemVtable*) ca->base.vtable : NULL;
        if (t == NULL) continue;
        t->resumeSound = mlResumeSound;
        t->resumeAll = mlResumeAll;
        t->resume = mlResume;
    }
    mlRegisterNext(callback, data);
}

// The physical mix write (gomPhysMix's, after its group scaling). Game, opener or NDSP thread; never under utaLock.
ML_CODE void mlPhysMix(int p, float mix[12]) {
    CtrAudioSystem* ca = utaCa;
    if (utaActive && ca != NULL && (unsigned) p < UTA_CHANNELS && __atomic_load_n(&utaChan[p].mlIn, __ATOMIC_ACQUIRE) != 0) {
        LightLock_Lock(&utaLock);
        UtaChan* c = &utaChan[p];
        bool front = true;
        for (int i = 2; i < 12; i++) front &= mix[i] == 0.0f;
        int s = c->mlIn & ~ML_IN_WANT;
        if (s != 0 && front && !ca->ramps[p].active) {
            if (s == ML_IN_ARMED) __atomic_store_n(&c->mlIn, (uint8_t) ML_IN_ZERO, __ATOMIC_RELEASE); // the unpause's write
            ca->ramps[p].tgt[0] = mix[0];
            ca->ramps[p].tgt[1] = mix[1];
            mlInMix(p, s > ML_IN_LATCHED ? mlInGain(s - ML_IN_LATCHED) : 0.0f);
            LightLock_Unlock(&utaLock);
            return;
        }
        ML_MIX_NEXT(p, mix);
        mlRetire(p);
        LightLock_Unlock(&utaLock);
        return;
    }
    ML_MIX_NEXT(p, mix);
}

// The physical pause write (UTA_PHYS_PAUSED: lmPaused's, the load and play-through holds', gomusic's; any thread). A
// channel whose silence hasn't reached the DSP stays paused: an unpause is held for the frame callback (ML_IN_WANT), a
// pause takes that back.
ML_CODE void mlPhysPaused(int p, bool paused) {
    if ((unsigned) p < UTA_CHANNELS) {
        uint8_t v = __atomic_load_n(&utaChan[p].mlIn, __ATOMIC_ACQUIRE);
        while ((v & ~ML_IN_WANT) == ML_IN_ZERO || (v & ~ML_IN_WANT) == ML_IN_LATCHED) {
            uint8_t w = paused ? (uint8_t) (v & ~ML_IN_WANT) : (uint8_t) (v | ML_IN_WANT);
            if (__atomic_compare_exchange_n(&utaChan[p].mlIn, &v, w, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                if (!paused) return; // the channel stays paused (it is: it was paused when armed)
                break;
            }
        }
    }
    ML_PAUSED_NEXT(p, paused);
}

// The frame callback's lock (utaFrameCallback, NDSP thread, each DSP frame; this frame's writes go out after the
// callback): then each fading channel's next step.
ML_CODE void mlFrameLock(LightLock* lock) {
    LightLock_Lock(lock);
    CtrAudioSystem* ca = utaCa;
    for (int p = 0; p < UTA_CHANNELS && ca != NULL; p++) {
        UtaChan* c = &utaChan[p];
        uint8_t v = __atomic_load_n(&c->mlIn, __ATOMIC_ACQUIRE);
        int s = v & ~ML_IN_WANT;
        if (s < ML_IN_ZERO) continue;
        if (ca->ramps[p].active || c->state == UTA_TAILING || c->state == UTA_PENDING) { // positioned, or stopped
            if (s <= ML_IN_LATCHED) mlInMix(p, 1.0f); // never heard: its mix and its held unpause, as before the fade
            mlRetire(p);
            continue;
        }
        if (s == ML_IN_ZERO) { // the silence goes out after this callback
            while (!__atomic_compare_exchange_n(&c->mlIn, &v, (uint8_t) ((v & ML_IN_WANT) | ML_IN_LATCHED), false,
                                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            }
            continue;
        }
        if (s == ML_IN_LATCHED) {
            if (!(v & ML_IN_WANT)) continue; // not unpaused yet
            mlInMix(p, mlInGain(1));
            ML_PAUSED_NEXT(p, false);
            if (!__atomic_compare_exchange_n(&c->mlIn, &v, (uint8_t) (ML_IN_LATCHED + 1), false, __ATOMIC_ACQ_REL,
                                             __ATOMIC_ACQUIRE)) { // paused again meanwhile (another core): undo
                ML_PAUSED_NEXT(p, true);
                mlInMix(p, 0.0f);
            }
            continue;
        }
        if (c->paused) continue; // paused again mid-fade: holds its step
        float g = mlInGain(s - ML_IN_LATCHED + 1);
        mlInMix(p, g);
        if (g < 1.0f) __atomic_store_n(&c->mlIn, (uint8_t) (s + 1), __ATOMIC_RELEASE);
        else mlRetire(p);
    }
}
#endif

#if defined(ENABLE_WAD16)
// papvoice (UNDERTALE; z_utsfx.c's UtSfx_chainPlanVoices, at every room change): the plan's small text voices are read
// on the game thread at their play, not left pending for the audio loader (noBackgroundRead, the flag a failed
// background read sets), so the writer's next letter, which stops and plays the voice again, finds it playing from
// the cache. Same sound index throughout: the read fills that sound's own cache entry, and a worker copy of it that
// finishes later is dropped (usInstall). On UNDERTALE Old the ADPCM pack's play asks its reader first, so a marked
// sound in the pack is admitted here first, read from the pack on this thread (apAdmit, as msPlay does without a
// reader slot). One already on its way (apIdle: a loader read or a decode has it) takes msPlay's usual path and waits
// for that read. One the pack can't admit now (the cache's cap, linear memory) skips msPlay, whose reader request would
// leave it pending (R435 F1: a 1,792 B voice with 1,024 B left under the cap): the original play reads its WAV here
// (noBackgroundRead), and buildSfx makes room as for any play. Named outside .text.late.* (the catch-all leaves
// ctr_audio_system.o to its own lines anyway); appended, so no earlier code moves.
#ifdef CTRPORT_ADPCM_SFXPACK
__attribute__((noinline, optimize("Os"), section(".text_papvoice"))) static int32_t pvPlay(AudioSystem* audio, int32_t sound, int32_t priority, bool loop) {
    CtrAudioSystem* ca = mm.ca;
    if (ca != NULL && 0 <= sound && ca->soundCount > (uint32_t) sound && ca->noBackgroundRead[sound] && ca->sfxCache[sound].pcm == NULL &&
        0 > apAdmit(sound, UINT32_MAX, 0) && apIdle(ca, sound)) {
        return mm.inner->playSound(audio, sound, priority, loop);
    }
    return msPlay(audio, sound, priority, loop);
}
#endif

__attribute__((noinline, optimize("Os"), section(".text_papvoice"))) void CtrAudioSystem_voiceDirect(const uint8_t* entries, uint32_t count) {
    CtrAudioSystem* ca = gShutdownTarget;
    if (ca == NULL || entries == NULL) return;
    for (uint32_t i = 0; count > i; i++) {
        uint16_t sound;
        memcpy(&sound, entries + (size_t) i * 4, 2);
        if (ca->soundCount > sound) ca->noBackgroundRead[sound] = true;
    }
#ifdef CTRPORT_ADPCM_SFXPACK
    if (mm.ca == ca && mm.outer.playSound == msPlay) mm.outer.playSound = pvPlay;
#endif
}

// z_utsfx.c's usPreload queues its worker entries here (an external call, as queueSfx was, so usPreload keeps its
// code). Every entry but an added voice goes to flowend's queueSfx as before. A text voice the census added has
// PAPVOICE_KIB in its planned size (build_sfxplan.py's VOICE_KIB): it takes flowend's queueSfx's own path to the
// worker, without the flag, and its layer call, but is never put in flowend's kept list. So the scene keeps the same
// FLOWEND_KEEP sounds as without its voices, and the voice's cache entry keeps its own lastUse: flowendRaiseKept, which
// the layer call and each audio update run, raises only the kept list's sounds to FLOWEND_KEPT_USE (R435 F2; R435b F2:
// putting the voice in the list and taking it out after the call left a cached voice raised, tied with a kept sound for
// LRU eviction).
#define PAPVOICE_KIB 0x8000u
__attribute__((noinline, optimize("Os"), section(".text_papvoice"))) int CtrAudioSystem_queuePlanned(int32_t soundIndex, uint32_t kib, uint32_t floorBytes) {
    if ((kib & PAPVOICE_KIB) == 0) return __wrap_CtrAudioSystem_queueSfx(soundIndex, kib, floorBytes);
    kib &= ~PAPVOICE_KIB;
#ifdef CTRPORT_ADPCM_MUSIC
    int got = mmSfxPlanned(soundIndex) ? -1 : __real_CtrAudioSystem_queueSfx(soundIndex, kib, floorBytes);
#elif defined(CTRPORT_ADPCM_SFXPACK)
    int got = apQueue(soundIndex, kib, floorBytes);
#else
    int got = __real_CtrAudioSystem_queueSfx(soundIndex, kib, floorBytes);
#endif
    flowendLayer();
    return got;
}
#endif

// logfix (change scanout, logaudit G8): the way in to postMessage for z_logfix.c, whose CtrLogfix_post the sound-effect
// pack's posts now come to (ctr_adpcm_pack.inc's table). One branch in the appended late code (.text_logfix, an appended
// late section, where a new name belongs), its arguments untouched: no code here changes or moves.
#ifdef CTRPORT_ADPCM_SFXPACK
void CtrLogfix_postMessage(void* ca, const char* format, ...);
__attribute__((naked, noinline, section(".text_logfix"))) void CtrLogfix_postMessage(void* ca __attribute__((unused)),
                                                                                     const char* format __attribute__((unused)), ...) {
    __asm__("b postMessage");
}

// A record of several lines (CtrLogfix_post's split one) goes in whole or not at all: under one hold of the lock, its
// lines take that many free slots in a row, so no other thread's line comes between them; with too few free, none is
// queued and each counts as a dropped message, as postMessage counts one. fill writes line i into its slot (one copy,
// no formatting: the lock's hold stays short); nothing here waits for room or allocates.
bool CtrLogfix_postLines(void* vp, uint32_t count, void (*fill)(void* ctx, uint32_t i, char* slot, uint32_t size), void* ctx);
__attribute__((noinline, noipa, section(".text_logfix"))) bool CtrLogfix_postLines(void* vp, uint32_t count,
                                                                                   void (*fill)(void*, uint32_t, char*, uint32_t),
                                                                                   void* ctx) {
    CtrAudioSystem* ca = (CtrAudioSystem*) vp;
    LightLock_Lock(&ca->msgLock);
    bool room = MESSAGE_SLOTS >= count && MESSAGE_SLOTS - count >= ca->msgCount;
    if (room) {
        for (uint32_t i = 0; count > i; i++) fill(ctx, i, ca->messages[(ca->msgHead + ca->msgCount + i) % MESSAGE_SLOTS], MESSAGE_LENGTH);
        ca->msgCount += count;
    } else {
        ca->msgDropped += count;
    }
    LightLock_Unlock(&ca->msgLock);
    return room;
}
#endif

// ===[ loadclass 4c: two layers of one song join sample-locked (change loadclass, UNDERTALE) ]===
// (Respun after the S8b noon line: the code reviewed as R434c..R434g, in its own late section .text_lcpair, so no
// earlier late code moves. Its only hooks into earlier code are call targets: flowend's note call goes to
// CtrAudioSystem_chainNotePair (the join line, then loadclass's link), and the load mix's frame-callback
// registration and pause alias to this block's links, as described below.)
// Toriel's house plays its song and a quiet layer of it (gain 0.01, raised by the house's rooms) in one step, as two
// looping streams; in its slowed version, the same song twice at pitch 0.4, gains 1 and 0.01. On PC both start
// together and stay in step. Here the opener thread opens one stream at a time, so the layer started one open later:
// 836 ms in maintainer's o3DS log (s8b7), and it stays that far behind, so when a room raises it the two are heard out of
// step. Holding the first until the second opened would start the house's song that much later, so instead the first
// plays at once (the leader) and the layer joins it at the leader's sample, as gomusic joins copies of one song: its
// first pause write holds it (gomusic's hold bit, so every pause write keeps it paused), it is sent to the sample the
// leader will be at GOM_MARGIN DSP frames on, and it is unpaused in the frame callback in which the leader gets there.
// From then on the two play as on PC; before it only the layer is missing (gain 0.01 in the house's first rooms).
// Only these pairs (by name hash): the song and its layer, or the slowed song twice. Only two plays next to each other
// (voice serials one apart: the house's music object plays them one after the other), asked for in the same game step
// (no step gomusic noted ended between them: loadclassSameStep) and at equal game clocks (clockSeconds: so neither was
// paused through an update since). Same pitch, same rate, neither paused nor given a start position (a consumed one
// too: seekSeconds).
// Anything else runs as before. A layer that can't join (its leader stopped, paused, seeked or replaced, its own pause,
// a plan missed GOM_TRIES times, or GOM_HOLD_FRAMES past its plan) leaves as gomusic's copies do (gomLeave): its plan,
// taken or not, gives way to a seek to its first sample, and it stays held until that is queued, so it plays from its
// start as before this code; a seek of the game's stays. LOADCLASS_LEAD_FRAMES bounds only when a layer may still be
// admitted after its leader's open; an admitted layer's wait is bounded by its plans and GOM_HOLD_FRAMES (DSP frames,
// which a menu or a suspend does not count).
// The leader is noted in gomusic's context slot for its stream, which gomusic leaves alone for any other song (group 0,
// no stamp): copy LOADCLASS_LEADS, its voice's serial in batch0, its sample in the snapshot gomPlan reads. A layer held
// to join is copy LOADCLASS_JOINS, the leader's context in sound and its voice's serial in batch1. gomusic's state and helpers are reached by asm aliases,
// as gounder's are: a direct use adds call-graph edges, and GCC then emits gomusic's functions and strings in another order. One log
// line per layer, through gomusic's format (copy 2), from the opener's note: when it was heard after the leader first
// was, its sample and residual, and the measured offset at the join and GOM_MEASURE frames on (0 = in step).
#if defined(ENABLE_WAD16) && defined(CTRPORT_HAVE_TREMOR)
#define LCPAIR_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text_lcpair")))
#define LOADCLASS_HOUSE1 0x43DABBAAu // the house's song (a sound's name)
#define LOADCLASS_HOUSE2 0x42DABA17u // its quiet layer (a sound's name)
#define LOADCLASS_TORIEL 0x4BB394B4u // the slowed house's song, both layers (a sound's name)
#define LOADCLASS_LEADS 0xA6 // gomusic slot copy value: a layer song that played first
#define LOADCLASS_JOINS 0xA5 // gomusic slot copy value: a layer held to join its leader
#define LOADCLASS_LEAD_FRAMES 600 // DSP frames (about 2.9 s) a leader waits for a layer after its open

extern __typeof__(gom) loadclassGom;
extern __typeof__(menuHeld) loadclassMenuHeld;
int loadclassGomPhys(const StreamContext* s);
bool loadclassGomPos(const StreamContext* s, int p, uint32_t* out);
bool loadclassGomPlan(CtrAudioSystem* ca, int k, int leader, uint32_t margin);
bool loadclassGomReady(const StreamContext* s);
bool loadclassGomSeekedAway(const StreamContext* s, const GomCtx* g);
bool loadclassGomOffset(CtrAudioSystem* ca, int leader, const StreamContext* s, int p, int32_t* out);
void loadclassGomUnhold(CtrAudioSystem* ca, GomCtx* g);
void loadclassGomDsp(void* data);
void loadclassGomPaused(int p, bool paused);
extern const char loadclassJoinFormat[], loadclassJoined[], loadclassRanAlone[];
__asm__(".set loadclassGom, gom\n.set loadclassMenuHeld, menuHeld\n.set loadclassGomPhys, gomPhys\n.set loadclassGomPos, gomPos\n"
        ".set loadclassGomPlan, gomPlan\n.set loadclassGomReady, gomReady\n.set loadclassGomSeekedAway, gomSeekedAway\n"
        ".set loadclassGomOffset, gomOffset\n.set loadclassGomUnhold, gomUnhold\n.set loadclassGomDsp, gomDspCallback\n"
        ".set loadclassGomPaused, gomPhysPaused\n.set loadclassJoinFormat, gomJoinFormat\n.set loadclassJoined, gomJoined\n"
        ".set loadclassRanAlone, gomRanAlone\n.set loadclassCallbackLink, loadclassSetCallback\n");

LCPAIR_CODE static bool loadclassLayer(const char* name) {
    uint32_t h = loadclassHash(name);
    return h == LOADCLASS_HOUSE1 || h == LOADCLASS_HOUSE2 || h == LOADCLASS_TORIEL;
}

// The two songs are one of the pairs: the song and its layer (either opens first), or the slowed song twice.
LCPAIR_CODE static bool loadclassPair(const char* a, const char* b) {
    uint32_t x = loadclassHash(a), y = loadclassHash(b);
    return (x == LOADCLASS_HOUSE1 && y == LOADCLASS_HOUSE2) || (x == LOADCLASS_HOUSE2 && y == LOADCLASS_HOUSE1) ||
           (x == LOADCLASS_TORIEL && y == LOADCLASS_TORIEL);
}

// Two voices one serial apart asked for in the same game step, and at equal game clocks. The step: gomUpdate notes, at
// each audio update, the first voice serial after the step just run (serialEnd; it only rises). A step ended between
// the two plays exactly when some step's serialEnd is the later serial. The ring holds the last GOM_STEPS steps: when
// every step it holds ended after both (none below the later serial; the zeroed BSS counts as below), it can't tell
// and says no. The opener reads it while the game thread writes it, so a scan of single words is no snapshot: two
// updates during one scan can replace the proof after it was read (R434f-1). It is read as one (loadclassRingSteps).
// The clocks (the game thread writes them): a read during its update that does not hold still is taken as not equal.
// Either no: the layer runs alone as before.
// The opener reads gomusic's ring of step ends while the game thread writes it (gomUpdate, one entry per audio update,
// with no lock or sequence count of its own). Read entry by entry, a scan can see an entry from before two updates
// and the rest from after them, and the entry that proved a step between the two plays is gone (R434f-1). So the ring
// is read twice, with a barrier between, and the first read counts only if the second matches it word for word. An
// entry only rises (each write is the voice serial counter, which only rises), so an entry read the same twice held
// that value all the time between, and every entry's two reads span the barrier: the first read is the ring as it was
// at that instant. A step ending at the later serial ends the check at once (a no needs no snapshot). Up to three
// tries, then no. True: one instant's ring reaches back before both plays and none of its steps ended between them.
LCPAIR_CODE static bool loadclassRingSteps(uint32_t later) {
    uint32_t seen[GOM_STEPS]; // no initializer list: a constant copy would land in .rodata
    for (int t = 0; 3 > t; t++) {
        bool before = false, same = true;
        for (int i = 0; GOM_STEPS > i; i++) {
            uint32_t end = *(const volatile uint32_t*) &loadclassGom.steps[i].serialEnd;
            if (end == later) return false; // a step ended between them
            before |= (int32_t) (end - later) < 0;
            seen[i] = end;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        for (int i = 0; GOM_STEPS > i; i++) same &= *(const volatile uint32_t*) &loadclassGom.steps[i].serialEnd == seen[i];
        if (same) return before;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    }
    return false;
}

LCPAIR_CODE static bool loadclassSameStep(const Voice* v, const Voice* w) {
    uint32_t later = (int32_t) (v->serial - w->serial) > 0 ? v->serial : w->serial;
    // The ring as one instant (loadclassRingSteps): it reaches back before both plays and no step in it ended between them.
    if (!loadclassRingSteps(later)) return false;
    const volatile double* a = &v->clockSeconds;
    const volatile double* b = &w->clockSeconds;
    for (int i = 0; 3 > i; i++) {
        double x = *a, y = *b;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (x == *a && y == *b) return x == y;
    }
    return false;
}

// A noted stream still plays for the voice it was noted for.
LCPAIR_CODE static bool loadclassAlive(const CtrAudioSystem* ca, int k) {
    const StreamContext* s = &ca->streams[k];
    int owner = s->owner;
    StreamState state = s->state;
    return owner >= 0 && VOICE_COUNT > owner && ca->voices[owner].serial == loadclassGom.c[k].batch0 && !s->stopRequested &&
           state != CTX_FREE && state != CTX_DONE;
}

// The pause link before gomusic's hold (any thread). A layer song's first pause write on the opener thread is its open's
// (openStream, under its lock, its rate set): next to a leader that plays, it is held and planned; otherwise it leads.
// (Other threads write its channel while it opens too: the load mix's repairs, before its decoder has a rate.)
LCPAIR_CODE void loadclassPhysPaused(int p, bool paused) {
    CtrAudioSystem* ca = gShutdownTarget;
    bool opener = ca != NULL && ca->opener != NULL && threadGetCurrent() == ca->opener;
    for (int k = 0; opener && UTA_CHANNELS > p && p >= 0 && STREAM_CONTEXTS > k; k++) {
        StreamContext* s = &ca->streams[k];
        GomCtx* g = &loadclassGom.c[k];
        if (s->state != CTX_OPENING || s->opened || g->group != 0 || g->copy != 0 || loadclassGomPhys(s) != p) continue;
        int owner = s->owner;
        if (owner < 0 || owner >= VOICE_COUNT || s->sampleRate == 0 || !loadclassLayer(s->name)) break;
        const Voice* v = &ca->voices[owner];
        int lead = -1;
        for (int j = 0; STREAM_CONTEXTS > j; j++) {
            const StreamContext* t = &ca->streams[j];
            const GomCtx* h = &loadclassGom.c[j];
            uint32_t apart = v->serial - h->batch0;
            if (j == k || h->copy != LOADCLASS_LEADS || !t->opened || !loadclassAlive(ca, j) || (apart != 1u && apart != ~0u) ||
                loadclassGom.frame - h->joinFrame > LOADCLASS_LEAD_FRAMES)
                continue;
            const Voice* w = &ca->voices[t->owner];
            if (loadclassPair(t->name, s->name) && !t->seekPending && !s->seekPending && s->seekSeconds == 0.0 && !v->paused &&
                !w->paused && !v->spatial && !w->spatial && v->pitch == w->pitch && t->sampleRate == s->decoder.sampleRate &&
                loadclassSameStep(v, w))
                lead = j;
        }
        g->batch0 = v->serial;
        g->tries = g->logged = 0;
        g->heardTick = 0;
        if (lead < 0) {
            g->snapStep = 0.0f; // no snapshot until the callback takes one
            g->joinFrame = loadclassGom.frame;
            g->copy = LOADCLASS_LEADS;
            break;
        }
        g->sound = lead;
        g->batch1 = loadclassGom.c[lead].batch0; // the leader's voice: a new song in its context is not it
        g->target = 0;
        g->residual = 0;
        g->offset[0] = g->offset[1] = GOM_UNSET;
        g->cueTick = loadclassGom.c[lead].heardTick ? loadclassGom.c[lead].heardTick : svcGetSystemTick();
        g->phys = (int8_t) (p + 1);
        g->phase = GOM_HELD;
        g->joinFrame = loadclassGom.frame;
        g->copy = LOADCLASS_JOINS;
        loadclassGomPlan(ca, k, lead, GOM_MARGIN); // the seek before the decode thread fills a buffer; else the callback plans
        __atomic_fetch_or(&loadclassGom.held, 1u << p, __ATOMIC_ACQ_REL);
        paused = true;
        break;
    }
    loadclassGomPaused(p, paused);
}

// A layer lets go: its channel gets the engine's pause state, where it is.
LCPAIR_CODE static void loadclassLetGo(CtrAudioSystem* ca, GomCtx* g, const char* said) {
    g->said = said;
    g->heardTick = svcGetSystemTick();
    loadclassGomUnhold(ca, g);
    __atomic_store_n(&g->phase, (uint8_t) GOM_DONE, __ATOMIC_RELEASE);
}

// A layer that can't join leaves, as gomLeave has a copy leave: a plan of this code's, taken or not, gives way to a seek
// to its first sample and it stays held until that is queued (GOM_LEAVING); held without a plan, it is at its first
// sample already; a seek of the game's stays. Under its lock.
LCPAIR_CODE static void loadclassLeave(CtrAudioSystem* ca, int k, uint32_t now) {
    GomCtx* g = &loadclassGom.c[k];
    StreamContext* s = &ca->streams[k];
    if (g->phase != GOM_PLANNED || loadclassGomSeekedAway(s, g)) {
        loadclassLetGo(ca, g, loadclassRanAlone);
        return;
    }
    s->seekPending = true;
    s->seekSeconds = 0.0;
    g->phase = GOM_LEAVING;
    g->joinFrame = now;
    LightEvent_Signal(&ca->event);
}

// A layer's step in the frame callback, under its lock.
LCPAIR_CODE static void loadclassFollow(CtrAudioSystem* ca, int k, uint32_t now) {
    GomCtx* g = &loadclassGom.c[k];
    StreamContext* s = &ca->streams[k];
    int lead = g->sound;
    if (g->phase == GOM_LEAVING) { // its first sample queued (or its wait over): plays from there
        if (loadclassGomReady(s) || (int32_t) (now - g->joinFrame) > GOM_HOLD_FRAMES) loadclassLetGo(ca, g, loadclassRanAlone);
        return;
    }
    if (lead < 0 || lead >= STREAM_CONTEXTS) {
        loadclassLetGo(ca, g, loadclassRanAlone);
        return;
    }
    StreamContext* t = &ca->streams[lead];
    int p = loadclassGomPhys(s);
    uint32_t lpos = 0;
    bool have = false;
    if (LightLock_TryLock(&t->lock) == 0) {
        have = loadclassGomPos(t, loadclassGomPhys(t), &lpos);
        LightLock_Unlock(&t->lock);
    }
    bool leads = loadclassGom.c[lead].copy == LOADCLASS_LEADS && loadclassGom.c[lead].batch0 == g->batch1 && loadclassAlive(ca, lead) &&
                 !t->seekPending && !ca->voices[t->owner].paused && !ca->voices[s->owner].paused &&
                 ca->voices[t->owner].pitch == ca->voices[s->owner].pitch;
    if (g->phase == GOM_JOINED) {
        uint32_t since = now - g->joinFrame;
        if (g->offset[0] == GOM_UNSET && since >= GOM_LEAD + 3) loadclassGomOffset(ca, lead, s, p, &g->offset[0]);
        else if (g->offset[0] != GOM_UNSET && g->offset[1] == GOM_UNSET && since >= GOM_MEASURE)
            loadclassGomOffset(ca, lead, s, p, &g->offset[1]);
        if (!leads || since > GOM_MEASURE + 20 || g->offset[1] != GOM_UNSET) __atomic_store_n(&g->phase, (uint8_t) GOM_DONE, __ATOMIC_RELEASE);
        return;
    }
    if (loadclassGomSeekedAway(s, g)) {
        loadclassLetGo(ca, g, loadclassRanAlone); // the game's seek: where the game put it
    } else if (!leads || (int32_t) (now - g->joinFrame) > GOM_HOLD_FRAMES) {
        loadclassLeave(ca, k, now);
    } else if (g->phase == GOM_HELD) {
        if (loadclassGomPlan(ca, k, lead, GOM_MARGIN)) LightEvent_Signal(&ca->event);
    } else if (have && (int32_t) (now - g->joinFrame) >= 0) {
        float step = loadclassGom.c[lead].snapStep;
        int32_t half = (int32_t) (step * 0.5f);
        int32_t residual = (int32_t) (g->target - (lpos + (uint32_t) ((float) GOM_LEAD * step + 0.5f)));
        bool ready = loadclassGomReady(s) && s->bufs[0].startFrame == g->target;
        g->residual = residual;
        if (ready && residual <= half && residual >= -half) {
            g->said = loadclassJoined;
            g->phase = GOM_JOINED;
            g->joinFrame = now;
            g->heardTick = svcGetSystemTick();
            loadclassGomUnhold(ca, g);
        } else if (ready && residual > half) {
            // the leader is behind its plan (held by a room load, say): wait for it
        } else if (GOM_TRIES > g->tries) {
            loadclassGomPlan(ca, k, lead, GOM_MARGIN << g->tries);
            LightEvent_Signal(&ca->event);
        } else {
            loadclassLeave(ca, k, now);
        }
    }
}

// The DSP frame link before gomusic's (registered in its place by lmRegister): gomusic's chain first, then each leader's
// sample is noted and each layer stepped, and a held layer's channel is kept paused after every other writer.
LCPAIR_CODE static void loadclassDspCallback(void* data) {
    loadclassGomDsp(data); // gomusic's chain first
    CtrAudioSystem* ca = (CtrAudioSystem*) data;
    bool frozen = ca->suspended || loadclassMenuHeld != 0;
    uint32_t now = loadclassGom.frame;
    for (int k = 0; STREAM_CONTEXTS > k; k++) {
        GomCtx* g = &loadclassGom.c[k];
        uint8_t copy = g->copy;
        if ((copy != LOADCLASS_LEADS && copy != LOADCLASS_JOINS) || g->group != 0) continue;
        StreamContext* s = &ca->streams[k];
        if (!loadclassAlive(ca, k)) {
            if (copy == LOADCLASS_JOINS) loadclassGomUnhold(ca, g); // stopped or gone: its channel is the engine's again
            g->copy = 0;
            continue;
        }
        if (frozen || (copy == LOADCLASS_JOINS && __atomic_load_n(&g->phase, __ATOMIC_ACQUIRE) == GOM_DONE)) continue;
        if (LightLock_TryLock(&s->lock) != 0) continue;
        if (copy == LOADCLASS_JOINS) {
            loadclassFollow(ca, k, now);
        } else {
            int p = loadclassGomPhys(s);
            uint32_t pos;
            if (loadclassGomPos(s, p, &pos)) {
                if (!g->heardTick && pos > 0) g->heardTick = svcGetSystemTick();
                loadclassGom.seq++;
                __atomic_thread_fence(__ATOMIC_RELEASE);
                g->snapPos = pos;
                g->snapFrame = now;
                g->snapStep = 160.0f * CtrLoadmix_legacy.channels[p].rate / (float) NDSP_SAMPLE_RATE;
                __atomic_thread_fence(__ATOMIC_RELEASE);
                loadclassGom.seq++;
            }
            bool waited = false; // a layer still on its way keeps the leader noted
            for (int j = 0; STREAM_CONTEXTS > j; j++) {
                if (loadclassGom.c[j].copy == LOADCLASS_JOINS && loadclassGom.c[j].sound == k) waited = true;
            }
            if (!waited && now - g->joinFrame > LOADCLASS_LEAD_FRAMES) g->copy = 0;
        }
        LightLock_Unlock(&s->lock);
    }
    for (int k = 0; STREAM_CONTEXTS > k; k++) {
        int p = loadclassGom.c[k].phys - 1;
        if (loadclassGom.c[k].copy == LOADCLASS_JOINS && p >= 0) UTA_PHYS_PAUSED(p, true); // after every other writer
    }
}

// lmRegister's registration: this link in gomusic's place (gomusic's is the one it calls first).
void loadclassMissCallback(void* data); LCPAIR_CODE void loadclassSetCallback(ndspCallback callback, void* data) {
    UTA_PHYS_CALLBACK(callback != NULL ? loadclassMissCallback : NULL, data); // loadclass 8's line first (below)
}

// The opener's note: one log line per layer done (its slot then goes back to gomusic's untouched state).
LCPAIR_CODE static void loadclassPairLog(CtrAudioSystem* ca) {
    for (int k = 0; STREAM_CONTEXTS > k; k++) {
        GomCtx* g = &loadclassGom.c[k];
        if (g->copy != LOADCLASS_JOINS || __atomic_load_n(&g->phase, __ATOMIC_ACQUIRE) != GOM_DONE) continue;
        postMessage(ca, loadclassJoinFormat, ca->streams[k].name, 2u, g->said, g->heardTick > g->cueTick ? TICKS_TO_MS(g->heardTick - g->cueTick) : 0.0,
                    (unsigned long) g->target, (long) g->residual, (long) g->offset[0], (long) g->offset[1], (unsigned) g->tries);
        g->copy = 0;
    }
}

// flowend's note link (FLOWHITCH_NOTE_NEXT; opener thread, at each note): a layer's join line, then loadclass's link.
void CtrAudioSystem_chainNoteLater(const char* state); LCPAIR_CODE void CtrAudioSystem_chainNotePair(const char* state) {
    CtrAudioSystem* ca = gShutdownTarget;
    if (ca != NULL && ca->opener != NULL && threadGetCurrent() == ca->opener && strcmp(state, flowendWaiting) == 0) loadclassPairLog(ca);
    CtrAudioSystem_chainNoteLater(state); // 4a/4b's link with 9 (below)
}
#endif

// ===[ loadclass 8: one log line when the DSP reports frames the ndsp thread missed (a diagnostic: log only) ]===
// (Respun after the S8b noon line: reviewed as loadclass 8 and R434g, in section .text_lclater with its template
// in .rodata.lclater_in_text, both after loadclass 4c's; called from loadclassMissCallback, which 4c's
// registration names: this line first, then 4c's callback, as before.)
//
// libctru's ndspSync adds the second u16 of the DSP's status block (ndspVars[5]) to ndspGetDroppedFrames: DSP frames
// (160 samples, about 4.889 ms) that went by without the ndsp thread. A long session on an Old 3DS had it rise in
// steps of 117 (572 ms), each with core 1's input worker silent: while it waits, the DSP plays on with only the
// buffers it was given, so a song may stop for part of that time. At each such report this line gives the DSP's count
// (its ms are those missed frames, not a measured silence) and the streams that look dry as the callback runs: an
// unlocked look at the buffer states libctru's sync just read, not a record of the gap. It reads the word in the DSP
// callback right after that sync (ndspVars, ndspBufferId: libctru globals, declared as its internal header does), so
// it keeps no state. Counted: the streams playing, the dry ones with the first one's name, the ones the opener thread
// is opening (core 1's work). Dry: one played, none playing (a channel paused anywhere keeps its buffer playing), and
// one still queued or the file not at its end. No other libctru call, so nothing new is linked. Lossy: it takes a
// message slot only when their lock is free (the ndsp thread never waits): a busy lock drops the line uncounted, a
// full queue counts it in msgDropped. The game thread logs it: no printf here, '#' takes a number, '~' the dry name.
#if defined(ENABLE_WAD16) && defined(CTRPORT_HAVE_TREMOR)
#define LCLATER_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text_lclater")))
extern void* ndspVars[16][2];
extern u16 ndspBufferId;
static const char loadclassMissText[] __attribute__((section(".rodata.lclater_in_text"), aligned(4))) =
    "Audio: DSP missed # frames (# ms); streams # playing, # ran dry ~; # opening\n";

LCLATER_CODE static void loadclassDspMiss(CtrAudioSystem* ca) {
    unsigned missed = ((const volatile u16*) ndspVars[5][ndspBufferId])[1];
    if (missed == 0) return;
    unsigned n[5] = {missed, (missed * 4889u + 500u) / 1000u, 0, 0, 0};
    const char* dry = NULL;
    for (int k = 0; STREAM_CONTEXTS > k; k++) {
        StreamContext* s = &ca->streams[k];
        StreamState state = s->state;
        if (state == CTX_OPENING) n[4]++;
        if (state != CTX_PLAYING || !s->opened || s->paused) continue;
        n[2]++;
        bool played = false, playing = false, queued = false;
        for (int b = 0; STREAM_BUFFERS > b; b++) {
            u8 status = s->bufs[b].wb.status;
            played |= status == NDSP_WBUF_DONE;
            playing |= status == NDSP_WBUF_PLAYING;
            queued |= status == NDSP_WBUF_QUEUED;
        }
        if (played && !playing && (queued || !s->eof)) {
            n[3]++;
            if (dry == NULL) dry = s->name;
        }
    }
    if (LightLock_TryLock(&ca->msgLock) != 0) return;
    if (ca->msgCount >= MESSAGE_SLOTS) {
        ca->msgDropped++;
        LightLock_Unlock(&ca->msgLock);
        return;
    }
    char* o = ca->messages[(ca->msgHead + ca->msgCount) % MESSAGE_SLOTS];
    char* end = o + MESSAGE_LENGTH - 1;
    int i = 0;
    for (const char* t = loadclassMissText; *t != 0 && end > o; t++) {
        if (*t == '#' && 5 > i) {
            char d[10];
            int m = 0;
            unsigned v = n[i++];
            do d[m++] = (char) ('0' + v % 10u); while ((v /= 10u) != 0 && 10 > m);
            while (m > 0 && end > o) *o++ = d[--m];
        } else if (*t == '~') {
            if (dry == NULL) *o++ = '-';
            for (int c = 0; dry != NULL && 47 > c && dry[c] != 0 && end > o; c++) *o++ = dry[c];
        } else {
            *o++ = *t;
        }
    }
    *o = 0;
    ca->msgCount++;
    LightLock_Unlock(&ca->msgLock);
}

// 4c's frame-callback registration names this: the DSP's report first (it is this frame's), then 4c's callback.
LCLATER_CODE void loadclassMissCallback(void* data) {
    loadclassDspMiss((CtrAudioSystem*) data);
    loadclassDspCallback(data);
}
#endif

// ===[ loadclass 9: a sound a scene plays later, opened ahead when its scene starts (change loadclass, UNDERTALE) ]===
// A song the game loads is only an index (no file is read) until its play opens it, and on maintainer's card an open
// takes 150-560 ms. The opener opens one stream at a time, and a tile read of the game thread waits behind it on the
// card (kit-1108b: Mettaton EX's burst opened in 478 ms and that frame drew in 488 ms). Five scenes play such a sound
// long after a sound of their own has started, and that sound names it:
// - Endogeny's battle song -> the song of its fourth pet (maintainer, 11-08: "huge lag ... when it switches song").
// - Mettaton EX's and NEO's battle songs -> the burst of his hearts and his end (one play: later ones are cached).
// - The punch of the fake SAVE that breaks after Asgore (played three times first) -> the same burst, its explosion
//   (maintainer, 11-08: the sounds "when your save is breaking ... feel delayed"; 440 ms on his card).
// - The song of Asgore's last scene -> the cymbal Flowey's face appears with (540 ms on his card; the laugh played on
//   the same frame then waited behind it, about 1 s in all; it still opens at its play, after the cymbal, one spare).
// PC plays each on the frame the game asks for it. The burst is a sound inside data.win: it opens as a section of it
// (CtrDecoder_sectionPath, a WAV), as its play does; the others are files of their own (Ogg Vorbis).
// Once a voice plays the first sound and the phase-song spare is free, the later one is asked for and opened into
// the spare at once (logged "opened '...' ahead"), in the scene's first frames; its play takes it (flowendSongTake).
// Asked once per play of the first sound, opened or not. An untaken spare goes once no table sound plays and the room
// has changed since one last played (Mettaton EX frees every sound before his end's burst, in the same room). The
// room change count is flowhitch rev 4's; it rises after the new room's Create events, so it is retaken at each wait
// a table sound plays. A spare holding a section is only ever this link's (the others ask for files of their own).
// Both words sit after the path's NUL, before 4b's. Census: loadclass 9b. Hashes as 4a's.
// (Respun after the S8b noon line: reviewed as loadclass 9 and LC9-1, in section .text_lclater. 4a/4b's note link
// is frozen there, so its copy below (CtrAudioSystem_chainNoteLater, which 4c's link calls) carries 9's two calls
// and LC9-1's rule (loadclassAskShort); the frozen one stays where it was, no longer called.)
#if defined(ENABLE_WAD16) && defined(CTRPORT_HAVE_TREMOR)
#define LOADCLASS_DOGFIGHT 0x9F85B175u // Endogeny's battle song
#define LOADCLASS_DOGSONG 0x8EC6202Cu // the song its fourth pet plays
#define LOADCLASS_EXFIGHT 0xE38C099Du // Mettaton EX's battle song
#define LOADCLASS_NEOFIGHT 0x2D44ACAEu // Mettaton NEO's battle song
#define LOADCLASS_BURST 0x7E219FC4u // the heart burst of both, and the breaking SAVE's explosion (inside data.win)
#define LOADCLASS_PUNCH 0x6A7AE213u // the punch the fake SAVE takes before it breaks (a cached sound effect)
#define LOADCLASS_ASGOREEND 0xE44107E0u // the song of Asgore's last scene
#define LOADCLASS_CYMBAL 0x991996E3u // the cymbal Flowey's face appears with
#define LOADCLASS_SECTION 0x74636573u // a spare path's first four bytes when it names a section of a file
#define LOADCLASS_ROOM (FLOWEND_AHEAD_PATH - 8) // the room change count when a table sound last played
#define LOADCLASS_ASKED (FLOWEND_AHEAD_PATH - 12) // the later sound asked for while the first plays (0: none)

// LC9-1: 4a's ask (loadclassAsk), its path short of 9's words as loadclassAskAny's sections are.
LCLATER_CODE static bool loadclassAskShort(CtrAudioSystem* ca, uint32_t want) {
    for (uint32_t i = 0; ca->base.audioGroups[0]->sond.count > i; i++) {
        const Sound* sound = &ca->base.audioGroups[0]->sond.sounds[i];
        if (sound->file == NULL || loadclassHash(sound->file) != want) continue;
        char* path = resolveExternalPath(ca, sound);
        bool ok = path != NULL && LOADCLASS_ASKED > strlen(path); // short of 9's words, as its sections (LC9-1)
        if (ok) {
            loadclassDrop();
            strcpy(flowendSongPath, path);
            flowendSongState = 1;
        }
        free(path);
        return ok;
    }
    return false;
}

// The sound played later in the scene whose first sound is song (0: none).
LCLATER_CODE static uint32_t loadclassLater(uint32_t song) {
    if (song == LOADCLASS_DOGFIGHT) return LOADCLASS_DOGSONG;
    if (song == LOADCLASS_ASGOREEND) return LOADCLASS_CYMBAL;
    return song == LOADCLASS_EXFIGHT || song == LOADCLASS_NEOFIGHT || song == LOADCLASS_PUNCH ? LOADCLASS_BURST : 0;
}

// Opener thread: asks the spare for the sound whose file name hashes to want, by the path its play opens: a section
// of data.win for a big sound inside it (none when it is in memory: it then plays from there), else its own file
// (loadclassAskShort). Either path stays short of the words after it, or it is not asked for (true: asked).
LCLATER_CODE static bool loadclassAskAny(CtrAudioSystem* ca, uint32_t want) {
    for (uint32_t i = 0; ca->base.audioGroups[0]->sond.count > i; i++) {
        const Sound* sound = &ca->base.audioGroups[0]->sond.sounds[i];
        if (sound->file == NULL || loadclassHash(sound->file) != want) continue;
        if (!soundIsInAudo(sound)) return loadclassAskShort(ca, want);
        DataWin* group = NULL;
        AudioEntry* entry = audoEntryFor(ca, sound, &group);
        char* path = entry != NULL ? audoSectionPath(ca, sound, group, entry) : NULL;
        bool ok = path != NULL && LOADCLASS_ASKED > strlen(path);
        if (ok) {
            loadclassDrop();
            strcpy(flowendSongPath, path);
            flowendSongState = 1;
        }
        free(path);
        return ok;
    }
    return false;
}

// Opener thread: opens the asked-for path at once, as loadclassOpen does, but a WAV is kept too (a section of
// data.win). See loadclassRewind for flowhitch's decode ahead.
LCLATER_CODE static void loadclassOpenAny(CtrAudioSystem* ca) {
    u64 t0 = svcGetSystemTick();
    bool ok = __real_CtrDecoder_openFile(&flowendSong, flowendSongPath);
    if (ok && flowendSong.kind != CTR_DECODER_VORBIS && flowendSong.kind != CTR_DECODER_WAV) {
        CtrDecoder_close(&flowendSong);
        ok = false;
    }
    flowendSongState = ok ? 2 : 0;
    flowendLastOpen = svcGetSystemTick(); // the other spares wait for a quiet second of their own
    if (ok) postMessage(ca, flowendAheadLog, flowendSongPath, TICKS_TO_MS(flowendLastOpen - t0));
}

// Opener thread, at a wait with no play waiting (CtrAudioSystem_chainNoteLoadclass): true when it opened a sound. The
// game thread owns the voices: each field is read once, and a stale read only delays a request or a drop.
LCLATER_CODE static bool loadclassAhead(CtrAudioSystem* ca) {
    uint32_t seq = __atomic_load_n(&flowhitchRoomSeq, __ATOMIC_ACQUIRE);
    uint32_t room = 0, asked = 0, want = 0, head;
    // The words sit after a path short of them only (LC9-1): a longer one (another spare's, held or taken) keeps every
    // byte, and nothing is asked for or remembered while it is in the buffer (a play then opens cold, as before 9).
    bool words = LOADCLASS_ASKED > strnlen(flowendSongPath, FLOWEND_AHEAD_PATH);
    if (words) memcpy(&room, flowendSongPath + LOADCLASS_ROOM, sizeof(room));
    if (words) memcpy(&asked, flowendSongPath + LOADCLASS_ASKED, sizeof(asked));
    uint32_t count = ca->base.audioGroups[0]->sond.count;
    for (int i = 0; VOICE_COUNT > i && want == 0; i++) {
        const volatile Voice* v = &ca->voices[i];
        VoiceKind kind = v->kind;
        int32_t index = v->soundIndex;
        if (kind == VOICE_FREE || index < 0 || count <= (uint32_t) index) continue;
        const char* file = ca->base.audioGroups[0]->sond.sounds[index].file;
        if (file != NULL) want = loadclassLater(loadclassHash(file));
    }
    if (want != 0) {
        room = seq;
    } else {
        uint32_t held = flowendSongState != 0 ? loadclassHash(flowendSongPath) : 0;
        memcpy(&head, flowendSongPath, sizeof(head));
        bool mine = held == LOADCLASS_DOGSONG || held == LOADCLASS_BURST || held == LOADCLASS_CYMBAL || head == LOADCLASS_SECTION;
        if (room != seq && flowendSongState != 0 && mine) loadclassDrop();
        asked = 0;
    }
    bool open = want != 0 && want != asked && flowendSongState == 0 && words;
    if (open) {
        asked = want; // tried once, opened or not
        open = loadclassAskAny(ca, want);
    }
    words = LOADCLASS_ASKED > strnlen(flowendSongPath, FLOWEND_AHEAD_PATH); // (the asked-for path, if one was)
    if (words) memcpy(flowendSongPath + LOADCLASS_ROOM, &room, sizeof(room));
    if (words) memcpy(flowendSongPath + LOADCLASS_ASKED, &asked, sizeof(asked));
    if (open) loadclassOpenAny(ca);
    return open;
}

// Opener thread, right after flowhitch's link in each wait: that link decodes a held spare's first frames ahead and
// sets it back to frame 0 by its position count alone, since its copy serves a Vorbis stream only (keyed by the
// Vorbis state). A WAV spare's section is left past them, and its play would skip them. So it goes back to its first
// frame here, in the same wait (a seek, no read; the copy is never read for it). One that cannot is dropped.
LCLATER_CODE static void loadclassRewind(void) {
    CtrDecoder* d = &flowendSong;
    if (flowendSongState != 2 || d->kind != CTR_DECODER_WAV || !d->sectioned || d->positionFrames != 0 || d->secPos == d->wavDataStart) return;
    if (!CtrDecoder_seek(d, 0)) loadclassDrop();
}

// 4a/4b's note link (CtrAudioSystem_chainNoteLoadclass, above) with 9's two calls and LC9-1's ask; 4c's link calls it.
LCLATER_CODE void CtrAudioSystem_chainNoteLater(const char* state) {
    CtrAudioSystem* ca = gShutdownTarget;
    bool mine = ca != NULL && ca->opener != NULL && threadGetCurrent() == ca->opener && strcmp(state, flowendWaiting) == 0;
    // A spare of this link's nobody took goes first, so flowhitch's link (rev 4) finds the spare free in the same wait.
    if (mine && flowendAheadOwner == ca && flowendSongState != 0) {
        uint32_t held = loadclassHash(flowendSongPath);
        uint32_t last = flowendLastPath[0] != 0 ? loadclassHash(flowendLastPath) : 0;
        uint32_t stash;
        memcpy(&stash, flowendSongPath + LOADCLASS_STASH, sizeof(stash));
        if ((held == LOADCLASS_STORY && (last != 0 || __atomic_load_n(&flowhitchRoomIs, __ATOMIC_RELAXED))) || (held == LOADCLASS_NOISE && last != LOADCLASS_STORY) ||
            (held == LOADCLASS_BATTLE && (last != stash || __atomic_load_n(&flowhitchRoomIs, __ATOMIC_RELAXED))))
            loadclassDrop();
    }
    CtrAudioSystem_chainNoteFlowhitch(state); if (mine) loadclassRewind(); // (9) a WAV spare it read ahead, back to its start
    if (!mine || flowendAheadOwner != ca || ca->openerQuit) return;
    for (int i = 0; STREAM_CONTEXTS > i; i++) {
        if (ca->streams[i].state == CTX_OPEN_REQUESTED) return; // a play waits: never ahead of it
    }
    if (flowendLastPath[0] == 0) {
        // The boot: no file opened yet and the spare never asked for. Opened now, not at a quiet second: the boot's
        // tile loads leave none before the intro's play.
        if (flowendSongState != 0 || flowendSongPath[0] != 0) return;
        if (!loadclassAskShort(ca, LOADCLASS_STORY)) {
            flowendSongPath[0] = '-'; // no such file: asked once (a name no hash matches; no constant)
            flowendSongPath[1] = 0;
            return;
        }
        loadclassOpen(ca);
        return;
    }
    uint32_t last = loadclassHash(flowendLastPath); if (loadclassAhead(ca)) return; // (9) a later song, below
    // (3) The battle song: asked for once per pause (the path stays its request until the paused song resumes or goes).
    int battle = loadclassBattle(ca);
    bool asked = flowendSongPath[0] != 0 && loadclassHash(flowendSongPath) == LOADCLASS_BATTLE;
    if (asked && battle == 0) {
        loadclassDrop();
        flowendSongPath[0] = 0;
        return;
    }
    if (battle == 2 && !asked && flowendSongState == 0 && !__atomic_load_n(&flowhitchRoomIs, __ATOMIC_RELAXED)) {
        if (loadclassAskShort(ca, LOADCLASS_BATTLE) && LOADCLASS_STASH > strlen(flowendSongPath)) {
            memcpy(flowendSongPath + LOADCLASS_STASH, &last, sizeof(last));
            loadclassOpen(ca);
        } else {
            loadclassDrop();
        }
        return;
    }
    uint32_t want = last == LOADCLASS_STORY ? LOADCLASS_NOISE : last == LOADCLASS_LEAVE ? LOADCLASS_CAST1 : 0;
    if (want == 0 || (flowendSongPath[0] != 0 && loadclassHash(flowendSongPath) == want)) return; // asked once
    loadclassAskShort(ca, want);
}
#endif

#if defined(ENABLE_WAD16) && defined(CTRPORT_TARGET_O3DS) && defined(CTRPORT_HAVE_TREMOR)
// ===[ hplcopy: a short looping stream played fast reads a copy decoded on the app core (UNDERTALE Old) ]===
//
// The True Lab's cooler and fan rooms (258, 260) loop a 6.36 s song at pitch 3: the lab fog object's caster_loop at
// the room's start, and the Amalgamate dog event's in room 260, mid-room, beside the room's song. A stream is decoded
// from Ogg Vorbis at the file's rate, so pitch 3 decodes 3 s of sound a second: about 80% of the Old models' share of
// core 1 for the loop alone and ~107% beside the song, and the streams ran dry 2.1-2.6 s after they started, at or
// after the first seam (ethanlog5 row 8, change musicloop's report). Now a looping stream played at pitch 2 or more whose
// decoded samples fit HPL_MAX_BYTES is decoded once more, whole, by a thread on the app core (core 0) at the lowest
// priority, below the game thread's, so it runs only while the game thread waits (for the vblank, mostly). Its memory
// comes from the heap, only while the heap keeps the tile reader's reserve beside it. Once the copy is whole, the
// stream's decoder reads come from it, from the decoder's own position, and core 1 only queues buffers. Exact: the
// copy is the same decoder's output for the same file from frame 0 (a Vorbis decoder's output doesn't depend on how
// its reads are cut), so frame n of the copy is frame n of the stream; utaDecRead's seam, the loop seek and the
// queueing are unchanged, as with audofix's drone feed (this read goes first, then audofix's: flowhitch's read link's
// last call). Until the copy is whole, and when the heap had no room, no thread could start or the open failed, the
// stream decodes as before. The update looks at the voices after each step (the game thread): a looping stream voice
// at pitch 2 or more whose sound has no copy asks for one; once no stream voice plays that sound (caster_free's stop at
// the room's end, a stop of everything, a stolen voice), the copy is freed, or a thread still decoding is told to stop
// and free it. (The game pitches the sound itself, caster_loop's audio_sound_pitch, and never frees a stream: its
// caster_load returns a sound's index.) A decoder the copy fed stands where the feed began;
// when the feed stops in the middle (the copy freed), it is sought to the stream's position first, so the stream goes
// on exactly.
// Reach (change musicloop's census of UNDERTALE 1.08's pitch calls): the looping streams set to pitch 2 or more are
// that loop (rooms 258 and 260) and a 1.26 s loop at 2.5 (Papyrus's tile puzzle). Nothing else starts the
// thread.
#include <malloc.h>
#define HPL_CODE __attribute__((noinline, noipa, used, optimize("Os"), section(".text_hplcopy")))
#define HPL_RO __attribute__((section(".rodata.hplcopy_in_text")))
#define HPL_JOBS 2
#define HPL_MIN_PITCH 2.0f
#define HPL_MAX_BYTES (1536u * 1024u) // decoded PCM16
#define HPL_STACK (64u * 1024u) // the loader thread's, which runs the same decoder
#define HPL_PRIORITY 0x3F // the lowest; the game thread runs at 0x30
#define HPL_PIECE 4096u // frames a read; the stop is looked for between reads
#define HPL_STOP_MS 2000 // the shutdown's wait for the workers, at most (usstop's for the SFX worker is the same)
enum { HPL_RUNNING = 1, HPL_READY, HPL_FAILED, HPL_CANCELLED };

typedef struct {
    int32_t sound; // the sound its voice plays (game thread)
    int32_t state; // RUNNING, then READY or FAILED (the thread) or CANCELLED (the game thread), by compare-and-swap
    char* path; // the stream's file (its own copy of the path)
    char name[sizeof(((StreamContext*) 0)->name)]; // the stream contexts' name for it (voiceName: the base name)
    int16_t* pcm; // the copy, whole once READY
    uint32_t frames, rate, channels, bytes;
    u64 start, done; // ticks: asked for; whole or given up
    uint8_t why; // when FAILED: 1 + an index into hplWhy
    bool told; // reader: the outcome is in the log
} HplJob;
// The state, on the heap: allocated once, zeroed, by the registration (hplRegister, at boot), never freed. Its pointer
// sits in usw's alignment hole (before workTicks), so hplcopy keeps no late data (RULES 11-08); usw outlives the audio
// system, as the exit's hplAptExit needs. No state (no heap room at boot): hplcopy is off, every stream as before.
struct HplState {
    HplJob* volatile jobs[HPL_JOBS]; // the game thread's; a reader marks the one it looks at in feeding
    HplJob* volatile feeding[STREAM_CONTEXTS];
    uint32_t fedEnd[STREAM_CONTEXTS]; // reader: where its last copy left the decoder
    // The workers (game thread), joined: one stays here from its start until it has ended and is joined, whether its
    // job is still in jobs or was let go, so at most HPL_JOBS run or wind down at once.
    Thread threads[HPL_JOBS];
    void (*loaderTo)(void); // the loader's return address, as the registration (at boot) saw it
};
typedef struct HplState HplState;
#define HPL_STATE() __atomic_load_n(&usw.hplState, __ATOMIC_ACQUIRE)
_Static_assert(__builtin_offsetof(__typeof__(usw), workTicks) ==
               (__builtin_offsetof(__typeof__(usw), failed) + 4 + _Alignof(u64) - 1) / _Alignof(u64) * _Alignof(u64),
               "usw's hplState must sit in the hole before workTicks (usw keeps its size)");
static const char hplStartLog[] HPL_RO = "Audio: stream '%s' loops at pitch %.2f: decoding a copy on the app core\n";
static const char hplFeedLog[] HPL_RO = "Audio: stream '%s' reads its decoded copy from frame %lu (%lu KB, whole after %lu ms)\n";
static const char hplFailLog[] HPL_RO = "Audio: stream '%s' decodes as before (its copy: %s after %lu ms)\n";
static const char hplWhy[][16] HPL_RO = {"no file", "too long", "no heap room", "short read", "no thread"};

#ifdef CTRPORT_EXITTRAIL
// The provoke test (exit trail builds only; hplprovoke.txt in the game folder, for Azahar, which gives a .3dsx no
// loader address): at the registration a loader's return address is faked (hplFakeLoader, which notes that it was
// called and with how many workers alive, then returns: libctru's svcExitProcess follows), and a copy's worker sleeps
// 20 s before its third read, so hplStop's wait runs out with it alive. 1: hplAptExit as built. 2: the control arm,
// hplAptExit returns after the real aptExit (the exit before c26rev3's re-check). Notes: usstop's (Azahar's log).
static int hplTest;
static const char hplTestFile[] USSTOP_TRO = "sdmc:/3ds/undertale-3ds/hplprovoke.txt";
static const char hplTestOn[] USSTOP_TRO = "hplprovoke: mode %d, loader address faked\n";
static const char hplTestHeld[] USSTOP_TRO = "hplprovoke: the worker sleeps 20 s before its third read\n";
static const char hplTestApt[] USSTOP_TRO = "hplprovoke: aptExit: arm %d, workers alive %d, loader record %lu, return address %lx\n";
static const char hplTestLoader[] USSTOP_TRO = "hplprovoke: RETURNED INTO THE LOADER with %d workers alive\n";
USSTOP_TTEXT static int hplAlive(void) {
    HplState* s = HPL_STATE();
    int n = 0;
    for (int t = 0; s != NULL && t < HPL_JOBS; t++) n += __atomic_load_n(&s->threads[t], __ATOMIC_ACQUIRE) != NULL;
    return n;
}
USSTOP_TTEXT static void hplFakeLoader(void) {
    usNote(hplTestLoader, hplAlive());
}
USSTOP_TTEXT static void hplTestStart(void) {
    FILE* f = fopen(hplTestFile, usProvokeFileMode);
    if (f == NULL) return;
    int c = fgetc(f);
    fclose(f);
    if (c != '1' && c != '2') return;
    hplTest = c - '0';
    __system_retAddr = hplFakeLoader;
    usNote(hplTestOn, hplTest);
}
USSTOP_TTEXT static void hplTestRead(uint32_t got) {
    if (hplTest != 0 && got >= 2 * HPL_PIECE) { // a read may return fewer frames than asked
        usNote(hplTestHeld);
        svcSleepThread(20000000000LL);
    }
}
#endif

// The file's own functions, by their assembler names (no new call-graph edge).
void hplUpdateNext(AudioSystem* audio, float deltaTime) __asm__("gomUpdate");
void hplPost(CtrAudioSystem* ca, const char* fmt, ...) __asm__("postMessage");
int32_t hplReadNext(CtrDecoder* dec, int16_t* out, uint32_t maxFrames) __asm__("audofixFeedRead");
void hplRegisterNext(ndspCallback callback, void* data) __asm__("mlRegister");
void hplStopNext(void) __asm__("CtrAudioSystem_chainStopFlowhitch");
uint32_t CtrTileReader_heapReserve(void);
extern u32 __ctru_heap_size;

// The thread (app core, lowest priority). Opens the file afresh, takes the copy's memory when the heap keeps its
// reserve beside it, decodes to the end and hands the outcome over by one compare-and-swap. When the game thread has
// let the job go meanwhile (CANCELLED), it opens nothing and takes no memory from then on, stops at the next read and
// frees everything itself. Nothing of the audio system is touched here (the log lines are the reader's).
HPL_CODE static void hplWork(void* arg) {
    HplJob* job = (HplJob*) arg;
    CtrMemory_allowRelief(false); // a failed allocation is NULL: no reliever, no linear memory
    bool go = __atomic_load_n(&job->state, __ATOMIC_ACQUIRE) == HPL_RUNNING;
    CtrDecoder* dec = go ? (CtrDecoder*) malloc(sizeof(CtrDecoder)) : NULL;
    int16_t* pcm = NULL;
    uint32_t got = 0, frames = 0, ch = 0;
    uint8_t why = 1;
    if (dec != NULL && __real_CtrDecoder_openFile(dec, job->path)) {
        uint64_t total = dec->totalFrames;
        ch = dec->channels;
        job->rate = dec->sampleRate;
        why = 2;
        if (dec->kind == CTR_DECODER_VORBIS && total > 0 && ch - 1 <= 1 && total * ch * sizeof(int16_t) <= HPL_MAX_BYTES) {
            frames = (uint32_t) total;
            job->bytes = frames * ch * (uint32_t) sizeof(int16_t);
            why = 3;
            struct mallinfo mi = mallinfo();
            if ((uint64_t) __ctru_heap_size >= (uint64_t) (uint32_t) mi.uordblks + CtrTileReader_heapReserve() + job->bytes &&
                __atomic_load_n(&job->state, __ATOMIC_ACQUIRE) == HPL_RUNNING)
                pcm = (int16_t*) malloc(job->bytes);
        }
        if (pcm != NULL) why = 4;
        while (pcm != NULL && frames > got && __atomic_load_n(&job->state, __ATOMIC_ACQUIRE) == HPL_RUNNING) {
            uint32_t want = frames - got;
            if (want > HPL_PIECE) want = HPL_PIECE;
#ifdef CTRPORT_EXITTRAIL
            hplTestRead(got);
#endif
            int32_t n = __real_CtrDecoder_read(dec, pcm + got * ch, want);
            if (n <= 0) break;
            got += (uint32_t) n;
        }
        int16_t past[2];
        if (pcm != NULL && got == frames && __real_CtrDecoder_read(dec, past, 1) != 0) got = 0; // it must end there too
        CtrDecoder_close(dec);
    }
    free(dec);
    bool whole = pcm != NULL && got == frames;
    if (!whole) {
        free(pcm);
        pcm = NULL;
    }
    job->pcm = pcm;
    job->frames = frames;
    job->channels = ch;
    job->why = why;
    job->done = svcGetSystemTick();
    int32_t running = HPL_RUNNING;
    if (!__atomic_compare_exchange_n(&job->state, &running, whole ? HPL_READY : HPL_FAILED, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) { // let go meanwhile: no one else holds it now
        free(pcm);
        free(job->path);
        free(job);
    }
}

// Lets job j go (game thread). No reader can take it once it is out of jobs, and one reading it now is waited for (a
// Dekker pair, full barriers on both sides). A job still decoding is told to stop and frees itself; else it is freed
// here.
HPL_CODE static void hplRelease(HplState* s, int j) {
    HplJob* job = s->jobs[j];
    s->jobs[j] = NULL;
    __sync_synchronize();
    for (int k = 0; k < STREAM_CONTEXTS; k++) {
        while (s->feeding[k] == job) svcSleepThread(0);
    }
    int32_t running = HPL_RUNNING;
    if (!__atomic_compare_exchange_n(&job->state, &running, HPL_CANCELLED, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        free(job->pcm);
        free(job->path);
        free(job);
    }
}

// Joins the workers that have ended (game thread; a look, no wait). True when none is left.
HPL_CODE static bool hplReap(HplState* s) {
    bool none = true;
    for (int t = 0; t < HPL_JOBS; t++) {
        if (s->threads[t] == NULL) continue;
        if (threadJoin(s->threads[t], 0) == 0) { // its timeout, 0x09401BFE, is not negative
            threadFree(s->threads[t]);
            s->threads[t] = NULL;
        } else {
            none = false;
        }
    }
    return none;
}

// Asks for voice vi's copy in slot j, its worker in thread slot t (game thread): the path and name of its stream
// context, read under the context's lock while the voice owns it. A job that can't start stays in its slot as FAILED,
// so it isn't asked for again while the sound plays.
HPL_CODE static void hplStart(HplState* st, CtrAudioSystem* ca, int vi, int j, int t) {
    const Voice* v = &ca->voices[vi];
    StreamContext* s = &ca->streams[v->ctx];
    HplJob* job = (HplJob*) calloc(1, sizeof(HplJob));
    if (job == NULL) return;
    job->sound = v->soundIndex;
    job->state = HPL_RUNNING;
    job->start = svcGetSystemTick();
    LightLock_Lock(&s->lock);
    if (s->owner == vi && s->path != NULL) job->path = strdup(s->path);
    snprintf(job->name, sizeof(job->name), "%s", s->name);
    LightLock_Unlock(&s->lock);
    if (job->path == NULL || (st->threads[t] = threadCreate(hplWork, job, HPL_STACK, HPL_PRIORITY, 0, false)) == NULL) {
        job->why = job->path == NULL ? 1 : 5; // no file (or not this voice's any more), or no thread
        free(job->path);
        job->path = NULL;
        job->done = job->start;
        job->state = HPL_FAILED;
        __atomic_store_n(&st->jobs[j], job, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&st->jobs[j], job, __ATOMIC_RELEASE);
    hplPost(ca, hplStartLog, job->name, (double) v->pitch);
}

// The vtable's update (game thread): the file's own (gomUpdate), then the copies follow the voices. A copy whose sound
// no stream voice plays any more is let go; a looping stream voice at pitch 2 or more whose sound has none asks for
// one (one job a sound, two at once at most, and a worker of a job let go still counts until it has ended). A worker
// that has ended is joined at the next update. With no such voice, no copy and no worker, this is a look at 24 voices.
// No state: the file's own alone.
HPL_CODE static void hplUpdate(AudioSystem* audio, float deltaTime) {
    hplUpdateNext(audio, deltaTime);
    HplState* s = HPL_STATE();
    if (s == NULL) return;
    hplReap(s);
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    for (int j = 0; j < HPL_JOBS; j++) {
        if (s->jobs[j] == NULL) continue;
        bool live = false;
        for (int i = 0; i < VOICE_COUNT; i++) {
            const Voice* v = &ca->voices[i];
            if ((v->kind == VOICE_STREAM || v->kind == VOICE_PENDING) && v->soundIndex == s->jobs[j]->sound) live = true;
        }
        if (!live) hplRelease(s, j);
    }
    for (int i = 0; i < VOICE_COUNT; i++) {
        const Voice* v = &ca->voices[i];
        if (v->kind != VOICE_STREAM || !v->loop || !(v->pitch >= HPL_MIN_PITCH) || v->ctx < 0 || v->ctx >= STREAM_CONTEXTS)
            continue;
        int spare = -1, worker = -1;
        bool have = false;
        for (int j = 0; j < HPL_JOBS; j++) {
            if (s->jobs[j] == NULL) spare = j;
            else if (s->jobs[j]->sound == v->soundIndex) have = true;
        }
        if (have || spare < 0) continue;
        for (int t = 0; t < HPL_JOBS; t++) if (s->threads[t] == NULL) worker = t;
        if (worker >= 0) hplStart(s, ca, i, spare, worker);
    }
}

// flowhitch's read link's last call, in front of audofix's (decode thread, and the opener's first reads; only the
// stream contexts' decoders are fed): a stream whose copy is whole reads it from its decoder's position. No state:
// audofix's read alone.
HPL_CODE int32_t hplFeedRead(CtrDecoder* dec, int16_t* out, uint32_t maxFrames) {
    CtrAudioSystem* ca = *ctrPcmWaitUta.target;
    HplState* s = HPL_STATE();
    uintptr_t off = (uintptr_t) dec - (uintptr_t) &ca->streams[0].decoder;
    uint32_t k = off / sizeof(StreamContext);
    if (s != NULL && ca != NULL && dec->kind == CTR_DECODER_VORBIS && off % sizeof(StreamContext) == 0 && STREAM_CONTEXTS > k) {
        uint32_t at = (uint32_t) dec->positionFrames;
        for (int j = 0; j < HPL_JOBS; j++) {
            HplJob* job = s->jobs[j];
            if (job == NULL) continue;
            s->feeding[k] = job;
            __sync_synchronize();
            int32_t n = -1;
            if (s->jobs[j] == job && strncmp(job->name, ca->streams[k].name, sizeof(job->name)) == 0) {
                int32_t state = __atomic_load_n(&job->state, __ATOMIC_ACQUIRE);
                unsigned long ms = (unsigned long) ((job->done - job->start) / CPU_TICKS_PER_MSEC);
                if (state == HPL_READY && dec->channels == job->channels && dec->sampleRate == job->rate &&
                    dec->totalFrames == job->frames && job->frames >= at) {
                    n = (int32_t) (job->frames - at);
                    if ((uint32_t) n > maxFrames) n = (int32_t) maxFrames;
                    memcpy(out, job->pcm + at * job->channels, (uint32_t) n * job->channels * sizeof(int16_t));
                    if (!job->told) hplPost(ca, hplFeedLog, job->name, (unsigned long) at, (unsigned long) (job->bytes / 1024u), ms);
                    job->told = true;
                } else if (state == HPL_FAILED && !job->told) {
                    hplPost(ca, hplFailLog, job->name, hplWhy[(job->why - 1u) % 5u], ms);
                    job->told = true;
                }
            }
            __sync_synchronize();
            s->feeding[k] = NULL;
            if (n >= 0) {
                dec->positionFrames = at + (uint32_t) n;
                s->fedEnd[k] = at + (uint32_t) n;
                return n;
            }
        }
        if (s->fedEnd[k] == at && at != 0) (CtrDecoder_seek)(dec, at);
        s->fedEnd[k] = 0;
    }
    return hplReadNext(dec, out, maxFrames);
}

// Lets every job go (game thread): a reader still in one is waited for, a worker still decoding is told to stop.
HPL_CODE static void hplCancel(HplState* s) {
    for (int j = 0; j < HPL_JOBS; j++) {
        if (s->jobs[j] != NULL) hplRelease(s, j);
    }
}

// The audio system's shutdown (game thread): every job is let go, then the workers are joined, HPL_STOP_MS at most. A
// worker that hasn't ended by then (a read stuck on the card) is left running with its memory, and the exit keeps the
// heap: usWant, as usstop asks for the SFX worker, and, when the loader gave a return address, hplAptExit (below).
HPL_CODE static void hplStop(HplState* s) {
    hplCancel(s);
    for (int ms = 0; !hplReap(s); ms++) {
        if (ms >= HPL_STOP_MS) {
            __atomic_store_n(&usWant, true, __ATOMIC_RELEASE);
            return;
        }
        svcSleepThread(1000000LL);
    }
}

// shutdownAudio's CtrReadAhead_shutdown (through usstop's wrap; flowhitch's stop link): after the decode thread and
// the opener are joined, before the audio system is freed. The workers stop, then flowhitch's stop. This is the one
// teardown: shutdownAudio is the only caller of the unregistration (utaSetCallback(NULL), which calls lmRegister
// directly, not through hplRegister), and it always comes here after it. No worker exists without the registration
// (nor without the state).
HPL_CODE void hplShutdown(void) {
    HplState* s = HPL_STATE();
    if (s != NULL) hplStop(s);
    hplStopNext();
}

// utaSetCallback's registration (ML_REGISTER, once, at boot): the state is allocated, then the vtable's update becomes
// hplUpdate where it is still the file's own (both tables, as mlRegister does; before usw and the pause menu copy it),
// then mlRegister. No heap room for the state: the tables stay as they are, and hplcopy is off.
HPL_CODE void hplRegister(ndspCallback callback, void* data) {
    CtrAudioSystem* ca = utaCa;
#ifdef CTRPORT_EXITTRAIL
    if (callback != NULL && hplTest == 0) hplTestStart();
#endif
    HplState* s = HPL_STATE();
    if (callback != NULL && s == NULL && (s = (HplState*) calloc(1, sizeof(HplState))) != NULL) {
        s->loaderTo = __system_retAddr; // no aptExit has run yet: the loader's own address or NULL
        __atomic_store_n(&usw.hplState, s, __ATOMIC_RELEASE);
    }
    for (int k = 0; callback != NULL && s != NULL && k < 2; k++) {
        AudioSystemVtable* t = k == 0 ? &ctrAudioSystemVtable : ca != NULL ? (AudioSystemVtable*) ca->base.vtable : NULL;
        if (t != NULL && t->update == hplUpdateNext) t->update = hplUpdate;
    }
    hplRegisterNext(callback, data);
}

// The exit's aptExit: usstop's call to the real one comes here on Old Tremor (the stop link's #define). usstop keeps
// the heap only when the loader gave no return address. With one, libctru's __libctru_exit unmaps the linear heap and
// the heap (a worker's stack, decoder and copy), then returns into the loader, or ends the process when aptExit
// cleared the address (hb:ldr's chainload). A worker not joined by now (its read outlasted hplStop's wait) must not
// have that happen under it (c26rev3's re-check). So on the exit's owner the exit keeps the heap as usstop's own keep
// does (usVeto: no more heap frees) and ends the process at svcExitProcess; the kernel frees its memory with it. The
// loader's own address is dropped, so nothing returns into it; an address aptExit set itself (aptExitProcess, on a
// close the system ordered) goes aside for svcExitProcess's wrap, as in usstop's keep. Every worker joined (the
// ordinary exit), no loader address (usstop decides from usWant) or another thread: nothing changes.
#undef __real_aptExit
void __real_aptExit(void);
HPL_CODE void hplAptExit(void) {
    __real_aptExit();
    HplState* s = HPL_STATE();
    bool live = false;
    for (int t = 0; s != NULL && t < HPL_JOBS; t++) live |= __atomic_load_n(&s->threads[t], __ATOMIC_ACQUIRE) != NULL;
#ifdef CTRPORT_EXITTRAIL
    if (hplTest != 0) usNote(hplTestApt, hplTest, hplAlive(), (unsigned long) usLoader,
            (unsigned long) (uintptr_t) __system_retAddr);
    if (hplTest == 2) return;
#endif
    if (!live || __atomic_load_n(&usLoader, __ATOMIC_ACQUIRE) != 2u || !CtrLeavekeep_ownerKeep(true)) return;
    __atomic_store_n(&usVeto, true, __ATOMIC_RELEASE);
    void (*to)(void) = __system_retAddr;
    __system_retAddr = NULL;
    if (to != NULL && to != s->loaderTo) __atomic_store_n(&usExitTo, to, __ATOMIC_RELEASE);
}
#endif

// ===[ seamlocal (change musicloop, row 23; UNDERTALE): a loop seam is measured against the steps around it ]===
// PC loops a file whole, end straight into start, so a file whose end doesn't meet its start clicks there on PC too.
// utaudio smooths a seam whose step is at least 4x the file's largest step anywhere (and musicloop's F2 then smooths
// both channels). A quiet seam in a file with one loud moment elsewhere was kept, clicks and all. The rule here: a
// seam is smoothed when its step is at least 4x the largest step within 441 frames (10 ms at 44.1 kHz) either side of
// it, and at least 512 (-36 dBFS), so a quiet seam stays exactly as PC has it. Over UNDERTALE 1.08's 212 music files
// that smooths 8 more (6 of them played by the game) and keeps the 6 smoothed before; 3 of its 231 embedded sounds
// change, none of them looped by the game (tools/audio/seamlocal_ut108.tsv, from tools/audio/seamlocal_model.py).
// - Streams: utaDecRead's decoder read (UTA_PHYS_READ) goes through slRead, which during the measured first pass (from
//   frame 0, not seeked) keeps the largest of the first 441 steps and, in a ring on the heap, the last 441. At the
//   pass's end it gives utaDecRead's seam test max(local, 128) as the file's largest step: 4x that is "4x the local
//   steps and at least 512", and the splice's test, its log line and F2 are unchanged. No ring (no heap room, or a
//   read out of order): 32767, so the seam is kept as on PC.
// - Its state (SlState: the rings' pointers and the channels' marks) is on the heap, allocated once by the registration
//   at boot; its pointer is in the audio system's alignment hole after shutDown, so seamlocal keeps no late data (RULES
//   11-08). No state (no heap room at boot): seamlocal is off, and every seam follows the file-wide rule as before.
// - Cached loops: right after a play (playSound) and after each update (where pending voices start), a looping PCM16
//   channel whose seam piece isn't built yet gets its triggers from the same rule on its copy (the first and last 441
//   steps, outside utaLock), both channels once either qualifies, as F2 does for streams. The ADPCM pack's sounds keep
//   the pack's own rule (none of them is looped by the game with a different verdict).
// utaSetCallback's registration (ML_REGISTER, one changed external call target) goes through slRegister, which points
// the vtable's update and play entries at slUpdate and slPlaySound and then runs the rest of the chain.
#if defined(ENABLE_WAD16)
#define SL_CODE __attribute__((noinline, noipa, used, optimize("Os"), section(".text_seamlocal")))
#define SL_W 441u // frames: 10 ms at 44.1 kHz
#define SL_FLOOR 128 // 4x it is 512: a smaller step is left as it is
#define SL_STR2(x) #x
#define SL_STR(x) SL_STR2(x)
#if defined(CTRPORT_TARGET_O3DS) && defined(CTRPORT_HAVE_TREMOR)
#define SL_REGISTER_NEXT "hplRegister" // hplcopy's registration, then musicloop's
#define SL_UPDATE_NEXT "hplUpdate" // hplcopy's update, which then runs the file's own
#else
#define SL_REGISTER_NEXT "mlRegister"
#define SL_UPDATE_NEXT SL_STR(CTR_UPDATE_ENTRY)
#endif

typedef struct {
    uint32_t next; // the frame the next read should start at
    uint32_t k; // the ring slot of frame next's step
    int16_t last[2]; // frame next - 1
    uint16_t head[2]; // the largest of steps 1..SL_W (step i: frame i - 1 to frame i)
    uint16_t ring[2][SL_W]; // the last SL_W steps
} SlRing;

struct SlState {
    SlRing* volatile ring[STREAM_CONTEXTS]; // a stream context's measured pass (decode thread)
    // The segment (seq) each channel's triggers were last set for (game thread). It counts only while the channel's
    // slSet is 1: a reset (utaChnReset's memset) clears slSet and restarts the channel's seq at 1, so the play after it
    // is measured again even if no update saw the channel idle in between (c26rev3's re-check).
    uint16_t fixed[UTA_CHANNELS];
};
typedef struct SlState SlState;
#define SL_STATE(ca) __atomic_load_n(&(ca)->slState, __ATOMIC_ACQUIRE)
_Static_assert(__builtin_offsetof(CtrAudioSystem, voices) ==
               (__builtin_offsetof(CtrAudioSystem, shutDown) + 1 + _Alignof(Voice) - 1) / _Alignof(Voice) * _Alignof(Voice),
               "the audio system's slState must sit in the hole after shutDown (the struct keeps its layout)");

int32_t slReadNext(CtrDecoder* dec, int16_t* out, uint32_t maxFrames) __asm__("mlRead");
UtaSeam* slSeamFor(CtrDecoder* dec, int* index) __asm__("utaSeamFor");
void slRegisterNext(ndspCallback callback, void* data) __asm__(SL_REGISTER_NEXT);
void slUpdateFile(AudioSystem* audio, float deltaTime) __asm__(SL_STR(CTR_UPDATE_ENTRY));
void slUpdateNext(AudioSystem* audio, float deltaTime) __asm__(SL_UPDATE_NEXT);
int32_t slPlayNext(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) __asm__(SL_STR(CTR_PLAY_ENTRY));
#if defined(CTRPORT_ADPCM_SFX) || defined(CTRPORT_ADPCM_MUSIC) || defined(CTRPORT_ADPCM_SFXPACK)
bool slPacked(const void* data) __asm__("adSfxPacked");
#endif

SL_CODE static void slDrop(SlState* st, int idx) {
    SlRing* r = st->ring[idx];
    st->ring[idx] = NULL;
    free(r);
}

// The local rule's largest step for channel c of a stream's measured pass.
SL_CODE static int32_t slLocal(const SlRing* r, uint32_t c) {
    int32_t m = r->head[c];
    for (uint32_t i = 0; i < SL_W; i++) if (r->ring[c][i] > m) m = r->ring[c][i];
    return m < SL_FLOOR ? SL_FLOOR : m;
}

// utaDecRead's read: the stream's own, measured on the way through. Core 1 (the decode thread). No state: the read
// alone, so utaDecRead's own file-wide measure stands.
SL_CODE int32_t slRead(CtrDecoder* dec, int16_t* out, uint32_t maxFrames) {
    uint64_t at = dec->positionFrames;
    int32_t got = slReadNext(dec, out, maxFrames);
    CtrAudioSystem* ca = utaCa;
    SlState* st = ca != NULL ? SL_STATE(ca) : NULL;
    if (st == NULL) return got;
    int idx = 0;
    UtaSeam* s = slSeamFor(dec, &idx);
    if (s == NULL) return got;
    uint32_t ch = dec->channels;
    if (!s->fromZero || s->measured || !ca->streams[idx].loop || ch < 1 || ch > 2 || got < 0) {
        if (st->ring[idx] != NULL) slDrop(st, idx);
        return got;
    }
    SlRing* r = st->ring[idx];
    if (at == 0) { // a pass from frame 0 starts: measure it from the beginning
        if (r == NULL) r = st->ring[idx] = (SlRing*) malloc(sizeof(SlRing));
        if (r != NULL) memset(r, 0, sizeof(*r));
    } else if (r != NULL && at != r->next) {
        slDrop(st, idx); // a read out of order: no local measure for this pass
        r = NULL;
    }
    if (got == 0) { // the pass's end: utaDecRead marks it measured, and maxStep is its seam test's from here on
        for (uint32_t c = 0; c < ch; c++) s->maxStep[c] = r != NULL ? slLocal(r, c) : 32767;
        if (r != NULL) slDrop(st, idx);
        return got;
    }
    if (r == NULL) return got;
    uint32_t n = (uint32_t) got, k = r->k;
    for (uint32_t j = 0; j < n; j++) {
        uint32_t i = r->next + j;
        for (uint32_t c = 0; c < ch; c++) {
            int32_t x = out[j * ch + c];
            if (i > 0) {
                int32_t d = x - r->last[c];
                if (d < 0) d = -d;
                if (i <= SL_W && d > r->head[c]) r->head[c] = (uint16_t) d;
                r->ring[c][k] = (uint16_t) d;
            }
            r->last[c] = (int16_t) x;
        }
        if (i > 0 && ++k == SL_W) k = 0;
    }
    r->k = k;
    r->next += n;
    return got;
}

// The local rule on a whole copy: trig[c] for channels 0 and 1 (both once either qualifies, a nonzero step each);
// step and max (the local largest, at least SL_FLOOR) for the log line.
SL_CODE static void slTrigger(const int16_t* x, uint32_t frames, uint32_t ch, int8_t trig[2], int32_t step[2], int32_t max[2]) {
    bool any = false;
    step[0] = step[1] = max[0] = max[1] = 0;
    for (uint32_t c = 0; c < ch; c++) {
        int32_t m = 0;
        for (uint32_t i = 1; i < frames; i++) {
            if (i == SL_W + 1 && frames > 2 * SL_W + 1) i = frames - SL_W; // the first SL_W steps, then the last SL_W
            int32_t d = x[i * ch + c] - x[(i - 1) * ch + c];
            if (d < 0) d = -d;
            if (d > m) m = d;
        }
        max[c] = m < SL_FLOOR ? SL_FLOOR : m;
        step[c] = abs((int32_t) x[c] - (int32_t) x[(frames - 1) * ch + c]);
        if (step[c] >= UTA_SEAM_RATIO * max[c]) any = true;
    }
    for (uint32_t c = 0; c < 2; c++) trig[c] = (int8_t) (c < ch && any && step[c] > 0);
}

// The name of the cached sound whose copy holds data, for the log line (game thread, which owns the voices).
SL_CODE static const char* slSoundName(const uint8_t* data) {
    CtrAudioSystem* ca = utaCa;
    for (int id = 0; id < VOICE_COUNT; id++) {
        const Voice* v = &ca->voices[id];
        const CachedSfx* f = v->kind == VOICE_SFX ? v->sfx : NULL;
        if (f != NULL && f->pcm != NULL && data >= f->pcm && data < f->pcm + f->bytes && v->soundIndex >= 0 &&
            (uint32_t) v->soundIndex < ca->soundCount)
            return ca->base.audioGroups[0]->sond.sounds[v->soundIndex].name;
    }
    return utaNone;
}

// Game thread: each looping PCM16 channel whose seam piece isn't built gets the local rule's triggers, once per
// segment. One pass under utaLock finds the next such channel; its copy is read outside the lock (the frame callback
// waits on it), and the triggers are written back only if the channel is still on that segment. No state: nothing.
SL_CODE static void slCachedFix(void) {
    SlState* st = utaActive && utaCa != NULL ? SL_STATE(utaCa) : NULL;
    if (st == NULL) return;
    for (int p = 0; p < UTA_CHANNELS;) {
        UtaChan* c = NULL;
        const UtaSeg* g = NULL;
        const int16_t* x = NULL;
        LightLock_Lock(&utaLock);
        for (; p < UTA_CHANNELS && x == NULL; p++) {
            c = &utaChan[p];
            if (c->state != UTA_FEED) {
                st->fixed[p] = 0;
                continue;
            }
            g = NULL;
            for (int k = 0; k < c->segs && k < 2; k++) if (c->seg[k].looping) g = &c->seg[k];
            if (c->seamBuilt || g == NULL || (c->slSet && g->seq == st->fixed[p])) continue;
            if (g->frames < 4 * UTA_TAIL_MAX || c->chans < 1 || c->chans > 2 || c->bytes != 2 * c->chans) {
                st->fixed[p] = g->seq;
                c->slSet = 1;
                continue;
            }
#if defined(CTRPORT_ADPCM_SFX) || defined(CTRPORT_ADPCM_MUSIC) || defined(CTRPORT_ADPCM_SFXPACK)
            if (slPacked(g->data)) { // the pack's own rule
                st->fixed[p] = g->seq;
                c->slSet = 1;
                continue;
            }
#endif
            x = (const int16_t*) g->data;
        }
        uint32_t frames = x != NULL ? g->frames : 0, ch = x != NULL ? c->chans : 0;
        uint16_t seq = x != NULL ? g->seq : 0;
        LightLock_Unlock(&utaLock);
        if (x == NULL) return;
        int8_t trig[2];
        int32_t step[2], max[2];
        slTrigger(x, frames, ch, trig, step, max);
        LightLock_Lock(&utaLock);
        bool changed = false;
        if (c->state == UTA_FEED && !c->seamBuilt && g->seq == seq && g->data == (const uint8_t*) x) {
            changed = c->trigger[0] != trig[0] || c->trigger[1] != trig[1];
            c->trigger[0] = trig[0];
            c->trigger[1] = trig[1];
            st->fixed[p - 1] = seq;
            c->slSet = 1;
        }
        LightLock_Unlock(&utaLock);
        if (changed && utaPost != NULL)
            utaPost(utaCa, utaSeamFormat, slSoundName((const uint8_t*) x), utaCached, (long) step[0], (long) max[0],
                    (long) step[1], (long) max[1], trig[0] | trig[1] ? utaSmoothed : utaKept);
    }
}

SL_CODE static int32_t slPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    int32_t id = slPlayNext(audio, soundIndex, priority, loop);
    if (loop) slCachedFix();
    return id;
}

SL_CODE static void slUpdate(AudioSystem* audio, float deltaTime) {
    slUpdateNext(audio, deltaTime);
    slCachedFix();
}

// Both tables, where they still hold the file's own entries, before the rest of the chain runs (it and later layers
// copy them: the load clock keeps the play entry it finds, usw and the pause menu copy the table). On UNDERTALE Old
// with Tremor, slUpdate runs hplUpdate, so hplRegister then finds the update already taken and leaves it. The state is
// allocated first (once, at boot); with no heap room for it the tables stay as they are, and seamlocal is off.
SL_CODE void slRegister(ndspCallback callback, void* data) {
    CtrAudioSystem* ca = utaCa;
    SlState* st = ca != NULL ? SL_STATE(ca) : NULL;
    if (callback != NULL && ca != NULL && st == NULL && (st = (SlState*) calloc(1, sizeof(SlState))) != NULL)
        __atomic_store_n(&ca->slState, st, __ATOMIC_RELEASE);
    for (int k = 0; callback != NULL && st != NULL && k < 2; k++) {
        AudioSystemVtable* t = k == 0 ? &ctrAudioSystemVtable : ca != NULL ? (AudioSystemVtable*) ca->base.vtable : NULL;
        if (t == NULL) continue;
        if (t->update == slUpdateFile) t->update = slUpdate;
        if (t->playSound == slPlayNext) t->playSound = slPlaySound;
    }
    slRegisterNext(callback, data);
}
#endif

#if defined(ENABLE_WAD16)
// lcstop (change loadclass, 11-09; UNDERTALE): PC UNDERTALE keeps a stopped sound in its asset's membership for a short
// while. After a stop (by asset, by instance or of everything), asking whether the ASSET plays still answers true in
// the step of the stop and the next step; the stopped INSTANCE answers false at once, a second stop inside the window
// does not lengthen it, and a sound played again at once is a new live instance (pcref loadclass-sem-2/3 and
// loadclass-semb-1). The port retired the asset at once, so a music controller that checks its song in the step that
// stopped everything (a room change from an object's destruction) started the song again where PC stays silent
// (aheadtp r75). Instance queries and every sound's physical stop are unchanged: the window only adds an answer when
// the live-voice query (audparIsPlaying) says no.
// The window is the stop's step and the next one for every sound (c26rev2 LCSEM-2). PC answers that way in every run,
// for stops the bench issued and for the chase room's natural stop (aheadtp r75) alike. A sound streamed from its own
// file answered one step more for the bench's stops (issued at a step's start), but not for the chase room's natural
// one (nat-real-2: PC's window is wall time); there the port keeps its old answer, false, rather than guess.
// The window's END counts game steps (the runner's frame count, which Runner_step advances once per step on the game
// thread, and which stands still while the options menu holds the game). Whether a stop OPENS a window does depend on
// the sound's backend state at the stop, as the live-voice query already does (c26rev2 LCSEM-3): a sound whose last
// buffer the DSP has finished, or whose stream is done, is no longer live, so stopping it notes nothing.
// One note per asset (its last step; 0 = none), so no stop is ever dropped however many assets stop at once
// (c26rev2 LCSEM-1). The notes come from the heap at the first stop that needs one (4 B per asset, kept for the audio
// system's life), so lcstop keeps no late data (RULES 11-08); their pointer sits in the audio system's alignment hole
// before streams. With no heap room the window is off and the old answers stay. When the notes are allocated, the
// audio system's own vtable (the one the engine calls through) gets lcstopDestroy as its destroy, which frees them with
// the audio system (c26rev2 LCSEM-4). The static vtable keeps naming ctrDestroy, so nothing before this block moves.
// A note's step is the stop's step + 1, which is never 0 while the frame count stays in its (signed) range.
#define LCSTOP_CODE __attribute__((noinline, optimize("Os"), section(".text_lcstop")))
_Static_assert(__builtin_offsetof(CtrAudioSystem, streams) ==
                   (__builtin_offsetof(CtrAudioSystem, streamEntries) + sizeof(((CtrAudioSystem*) 0)->streamEntries) +
                    _Alignof(StreamContext) - 1) / _Alignof(StreamContext) * _Alignof(StreamContext),
               "the audio system's lcStop must sit in the hole before streams (the struct keeps its layout)");
Runner* CtrHeldslow_liveRunner(void);
bool audparIsPlaying(AudioSystem* audio, int32_t soundOrInstance);
// The file's own functions, by their assembler names (no new call-graph edge, so the file's order stays).
bool lcstopVoiceLive(CtrAudioSystem* ca, int vi) __asm__("voiceLive");
void lcstopVoiceStop(CtrAudioSystem* ca, int vi) __asm__("stopVoice");
void lcstopStopAllFile(AudioSystem* audio) __asm__("ctrStopAll");
void lcstopDestroyFile(AudioSystem* audio) __asm__("ctrDestroy");
// The vtable no longer names these two; the pointers keep them, unchanged, where they are.
__attribute__((used, section(".rodata_lcstop_in_text"))) static void (*const lcstopKeepStop)(AudioSystem*, int32_t) =
    sndcutStopSound;
__attribute__((used, section(".rodata_lcstop_in_text"))) static void (*const lcstopKeepAll)(AudioSystem*) = ctrStopAll;

LCSTOP_CODE static int32_t lcstopStep(void) {
    Runner* runner = CtrHeldslow_liveRunner();
    return runner != NULL ? (int32_t) runner->frameCount : 0;
}

// voiceFinished's test, copied without voiceLive's retirement, so stopAll still stops (and ramps out) what it did.
// Branches, not a switch: a jump table would be a constant in the main read-only data and move what follows it.
__attribute__((noinline, optimize("Os", "no-jump-tables", "no-tree-switch-conversion"), section(".text_lcstop"))) static bool
lcstopFinished(CtrAudioSystem* ca, const Voice* v) {
    if (v->kind == VOICE_SFX) return !v->loop && (v->wbCount == 0 || v->wb[v->wbCount - 1].status == NDSP_WBUF_DONE);
    if (v->kind == VOICE_STREAM) return ca->streams[v->ctx].state == CTX_DONE;
    if (v->kind == VOICE_VIRTUAL) return !v->loop && v->virtualSeconds >= v->virtualLength;
    return v->kind != VOICE_PENDING;
}

// ctrDestroy, then the notes (ctrDestroy frees the audio system, so the pointer is read first).
LCSTOP_CODE static void lcstopDestroy(AudioSystem* audio) {
    int32_t* notes = ((CtrAudioSystem*) audio)->lcStop;
    lcstopDestroyFile(audio);
    free(notes);
}

LCSTOP_CODE static void lcstopNote(CtrAudioSystem* ca, const Voice* v, int32_t step) {
    int32_t sound = v->soundIndex;
    if (sound < 0 || (uint32_t) sound >= ca->soundCount) return; // created streams keep the old answer
    if (ca->lcStop == NULL) {
        if ((ca->lcStop = (int32_t*) calloc(ca->soundCount, sizeof(int32_t))) == NULL) return; // no room: no window
        ca->base.vtable->destroy = lcstopDestroy;
    }
    ca->lcStop[sound] = (int32_t) ((uint32_t) step + 1u);
}

// sndcutStopSound's loop, noting each sound it stops.
LCSTOP_CODE static void lcstopStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    bool instance = soundOrInstance >= SOUND_INSTANCE_ID_BASE && soundOrInstance < AUDIO_STREAM_INDEX_BASE;
    int32_t step = lcstopStep();
    for (int i = 0; i < VOICE_COUNT; i++) {
        const Voice* v = &ca->voices[i];
        if (v->kind == VOICE_FREE || !lcstopVoiceLive(ca, i)) continue;
        if ((instance ? v->instanceId : v->soundIndex) != soundOrInstance) continue;
        lcstopNote(ca, v, step);
        lcstopVoiceStop(ca, i);
    }
}

// Notes every live sound, then ctrStopAll as before. The runner's reset (a game restart, which leaves no room
// current) stops everything with no window and forgets earlier stops, as PC does (pcref loadclass-semc-1).
LCSTOP_CODE static void lcstopStopAll(AudioSystem* audio) {
    CtrAudioSystem* ca = (CtrAudioSystem*) audio;
    Runner* runner = CtrHeldslow_liveRunner();
    if (runner == NULL || runner->currentRoomIndex < 0) {
        if (ca->lcStop != NULL) memset(ca->lcStop, 0, ca->soundCount * sizeof(int32_t));
        lcstopStopAllFile(audio);
        return;
    }
    int32_t step = (int32_t) runner->frameCount;
    for (int i = 0; i < VOICE_COUNT; i++) {
        const Voice* v = &ca->voices[i];
        if (v->kind != VOICE_FREE && !lcstopFinished(ca, v)) lcstopNote(ca, v, step);
    }
    lcstopStopAllFile(audio);
}

// ctrIsPlaying's target: the live voices first (audparIsPlaying, paused ones included), then an asset's window.
LCSTOP_CODE bool lcstopIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    if (audparIsPlaying(audio, soundOrInstance)) return true;
    CtrAudioSystem* ca = (CtrAudioSystem*) audio; // instances and created streams: no window
    if (ca->lcStop == NULL || soundOrInstance < 0 || (uint32_t) soundOrInstance >= ca->soundCount) return false;
    int32_t until = ca->lcStop[soundOrInstance];
    return until != 0 && (int32_t) ((uint32_t) until - (uint32_t) lcstopStep()) >= 0;
}
#endif

// bootahead (first update, changes bootmask and bootahead2): the menu song named to the spare, at the end so nothing above
// moves; its note link starts flowend's chain (FLOWHITCH_NOTE_NEXT).
// Compile this unit separately: original audio bodies keep their optimizer inputs and slots.
#ifdef CTRPORT_SFXLATE_UNIT
#include "ctr_sfxlate_cooler.inc"
#endif
#line 11682
#include "ctr_bootahead.inc"
