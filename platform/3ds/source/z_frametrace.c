// frametrace (10-28, benchfix): what each frame of a bench window did, for holds whose CPU and GPU work
// was low. One test word, build.sh ... frametrace (its own build directory, never a release): CMakeLists.txt compiles
// this file, its link wraps and two accessors only then (ctr_benchmark.c's and ctr_frame_pacer.c's, at their ends),
// so a build without the word links exactly the objects it did before.
//
// While a bench window is measured, frametrace keeps in RAM (allocated once, as the bench starts; nothing is written
// during the window):
// - one record a frame: the main loop's phases from its watchdog marks (wait for frame, step, draw, audio update,
//   room change, present), the GPU waits, the pacer's state as Step begins (vblank, slot start and length, delay,
//   the last flip's vblank, flips), texture loads, and whether the frame changed room (its present is skipped);
// - one event for every SD/RomFS call (open, read, write, flush, close, rename, delete: newlib's devices, the log
//   writer, rooms.dtm, saves, the texture cache and the streams all end in FSUSER/FSFILE calls), every audio decode
//   read (stream refills on the decode thread, sound loads on the loader: CtrDecoder_read), and every draw-time tile
//   load (CtrTextures_ensureTiles calls that loaded tiles: page, tiles, where from), each with its thread, start and
//   length, tagged with the frame under way when it began.
// When the window ends (in the next frame, or as --exit-after leaves), the records go to <game dir>/frametrace.txt
// (started afresh by the run's first window, then appended; sent back with --send-files), and one log line says so.
// Times are system ticks taken at the calls; recording is a few loads and stores (and one atomic add an event).
//
// frametrace.txt, per window:
//   # frametrace 1 <game> <target> scene <id> window <first>-<last> frames <n> events <n> lost <frames>/<events>
//   # F frame t wait step draw audio room present gpuwait gpu vbwait vbstep slot len delay shown flips tex texms fl
//   # E frame t ms thread kind arg [file]
//   F lines (one a frame) and E lines (one an event), in time order. Times in ms; t from the window's first frame's
//   "wait for frame" mark (its vblank is vblank 0).
// F, one frame (the frame number is the bench's, as held_frame lines use):
//   wait     "wait for frame" to "step": the wait for the frame's slot, the frame's begin (GPU queue), input and the
//            bench's frame begin.
//   step     Runner_step. draw: Draw and what follows until the audio update.
//   audio    the audio update and what follows until "room change".
//   room     "room change" to "present": the room change call (a room load: fl 1).
//   present  "present" to the next frame's "wait for frame": CtrRenderer_endFrame (its GPU waits, the flip's hand-off),
//            the bench's frame end.
//   gpuwait  CtrGpu_waitIdle time in the frame (any phase). gpu: the renderer's last finished GPU frame, as Step began.
//   vbwait, vbstep   the vblank counter at the "wait for frame" and "step" marks.
//   slot, len, delay   the pacer's slot start (vblank), slot length and delay as Step began. shown: the vblank the last
//            flip shows on (pacer lastShown); flips: top-screen flips counted since the frame before's Step.
//   tex, texms   texture page loads in the frame and their time (the renderer's totals, wait mark to wait mark).
//   fl       bits: 1 the frame changed room (main.c's skipPresent); 2 a value was clipped; 4 the pacer was skipping
//            (the frame goes unshown); 8 its slot began a whole vblank late; 16 the pacer shifted its schedule since
//            the frame before.
//   A window longer than the records (n3ds 5400 frames and 8192 events, o3ds 2700 and 4096) keeps its start; the
//   header's lost counts say what didn't fit.
// E, one event: the frame under way at its start, its start (ms), its length (ms), the thread ("main" or the
//   watchdog's thread name, spaces as _; others t<n>_p<priority>), the kind and its argument:
//   O open (handle), R read (bytes), W write (bytes), F flush, C close, N rename, D delete: [file] is the end of the
//     path (the newest file opened as that handle);
//   A decode: frames the read returned (on "audio decode": a stream refill; on the loader: a sound load's piece);
//   T tiles: "page <page> tiles <n> <source>": source ram, read (read-ahead), cache (SD cache file) or image (decoded
//     from the data.win's texture), the bucket the load counted in (two loads in one call: the first's).
// tools/analysis/frametrace_report.py reads the file (a frame and its neighbours with their events, the slowest
// frames, the SD writes).

#if defined(CTRPORT_FRAMETRACE)

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctr_audio_decoder.h"
#include "ctr_benchmark.h"
#include "ctr_frame_pacer.h"
#include "ctr_gpu_queue.h"
#include "ctr_link_files.h"
#include "ctr_renderer.h"
#include "ctr_textures.h"
#include "ctr_watchdog.h"
#include "log.h"
#include "runner.h"

#define FT_TEXT __attribute__((noinline, section(".text.frametrace_late")))
#define FT_CONST __attribute__((aligned(4), section(".rodata.frametrace_in_text")))
#define FT_BSS __attribute__((section(".bss.frametrace_late")))

#ifdef CTRPORT_TARGET_N3DS
#define FT_FRAMES 5400u // 3 minutes at 30 fps; 216 KB
#define FT_EVENTS 8192u // 160 KB
#else
#define FT_FRAMES 2700u // 90 s at 30 fps; 108 KB
#define FT_EVENTS 4096u // 80 KB
#endif
#define FT_FILES 48u
#define FT_THREADS 16u
#define FT_SHIFT 6               // event and frame times: system ticks >> 6 (0.24 us)
#define FT_TICKS_PER_TENTH 26811u // 0.1 ms (SYSCLOCK_ARM11 is 268111856 Hz)

typedef struct {
    uint32_t frame;
    uint32_t t;      // its "wait for frame" mark, ticks >> FT_SHIFT after the window's
    uint16_t seg[6]; // 0.1 ms: wait, step, draw, audio, room, present
    uint16_t gpuWait, gpu, texMs; // 0.1 ms
    uint16_t vbWait, vbStep, slot, shown; // vblanks after the window's first wait mark
    uint8_t len, delay, flips, tex, flags, pad;
} FtFrame;
_Static_assert(sizeof(FtFrame) == 40, "frametrace frame record size");

typedef struct {
    uint32_t t, dur; // ticks >> FT_SHIFT
    uint32_t arg;
    int16_t frame; // from the window's first (-1: the frame before it)
    char kind;
    uint8_t thread, file, src;
    uint16_t pad;
} FtEvent;
_Static_assert(sizeof(FtEvent) == 20, "frametrace event size");

#define FT_FLAG_ROOM 1u
#define FT_FLAG_CLIPPED 2u
#define FT_FLAG_SKIPPING 4u
#define FT_FLAG_SLOT_LATE 8u
#define FT_FLAG_SHIFTED 16u

// The frame under way on the main thread (its marks; always kept, so a window's first frame has its own).
typedef struct {
    bool valid, inWindow;
    uint8_t flags;
    uint32_t frame;
    uint64_t tWait, tStep, tDraw, tAudio, tRoom, tPresent, gpuWaitAt;
    uint32_t vbWait, vbStep;
    uint32_t pacer[8]; // CtrPacer_ftState's: slot start, slot length, delay, last shown, flips, shifts, skipping, late
    float gpu;
} FtCurrent;

static struct {
    volatile bool on;
    bool allocTried, fileStarted;
    void* mainTls;
    FtFrame* frames;
    FtEvent* events;
    uint32_t frameCap, eventCap, nFrames, framesLost;
    volatile uint32_t nEvents; // claimed, past eventCap too
    uint64_t base;             // the window's first wait mark
    uint32_t baseVb, first, lastFlips, lastShifts, texLoads;
    float texMs;
    int32_t start;
    volatile uint32_t frame;
    char id[48];
    const CtrBenchSession* session;
    FtCurrent cur;
    void* tls[FT_THREADS];
    int32_t prio[FT_THREADS]; // their priorities when first seen (unnamed threads print as t<n>_p<priority>)
    volatile uint32_t threads;
} ft FT_BSS;
// The files open on any thread (from boot, so long-open ones such as the log and the streams keep their names): the
// handle and the end of the path. A slot is freed when its file closes outside a window; one closed in a window keeps
// its name for the dump (marked closed, so a new file given the same handle takes another slot).
#define FT_FILE_BUSY 0xFFFFFFFEu   // being filled
#define FT_FILE_CLOSED 0xFFFFFFFDu // closed in a window (or a rename, a delete, a failed open): name only
static struct {
    Handle handle;
    char name[40];
} ftFiles[FT_FILES] FT_BSS;
static volatile uint32_t ftFileNext FT_BSS;

static const char ftMarkWait[] FT_CONST = "wait for frame";
static const char ftMarkStep[] FT_CONST = "step";
static const char ftMarkDraw[] FT_CONST = "draw";
static const char ftMarkAudio[] FT_CONST = "audio update";
static const char ftMarkRoom[] FT_CONST = "room change";
static const char ftMarkPresent[] FT_CONST = "present";
static const char ftFileName[] FT_CONST = "frametrace.txt";
static const char ftPathFormat[] FT_CONST = "%s/%s";
static const char ftWrite[] FT_CONST = "w";
static const char ftAppend[] FT_CONST = "a";
static const char ftTitle[] FT_CONST = CTRPORT_GAME_TITLE;
#ifdef CTRPORT_TARGET_N3DS
static const char ftTarget[] FT_CONST = "n3ds";
#else
static const char ftTarget[] FT_CONST = "o3ds";
#endif
static const char ftHeader[] FT_CONST =
    "# frametrace 1 %s %s scene %s window %lu-%lu frames %lu events %lu lost %lu/%lu\n"
    "# F frame t wait step draw audio room present gpuwait gpu vbwait vbstep slot len delay shown flips tex texms fl\n"
    "# E frame t ms thread kind arg [file]\n";
static const char ftFrameLine[] FT_CONST =
    "F %lu %lu.%lu %u.%u %u.%u %u.%u %u.%u %u.%u %u.%u %u.%u %u.%u %u %u %u %u %u %u %u %u %u.%u %u\n";
static const char ftEventLine[] FT_CONST = "E %ld %lu.%lu %lu.%02lu %s %c %ld%s%s\n";
static const char ftTileLine[] FT_CONST = "E %ld %lu.%lu %lu.%02lu %s T page %lu tiles %lu %s\n";
static const char ftSpace[] FT_CONST = " ";
static const char ftNone[] FT_CONST = "";
static const char ftMain[] FT_CONST = "main";
static const char ftThreadFormat[] FT_CONST = "t%lu_p%ld";
static const char ftSources[4][6] FT_CONST = {"ram", "read", "cache", "image"};
static const char ftLogLine[] FT_CONST =
    "frametrace: %s window %lu-%lu: %lu frames, %lu events (lost %lu frames, %lu events) in %s (%lu bytes, %lu ms)\n";
static const char ftLogNoMemory[] FT_CONST = "frametrace: no memory for the records (%lu bytes); nothing is recorded\n";
static const char ftLogOpenFailed[] FT_CONST = "frametrace: couldn't open %s; window %lu-%lu not written\n";

// The accessors (ctr_benchmark.c's and ctr_frame_pacer.c's ends, frametrace builds only).
const CtrBenchSession* CtrBench_ftState(bool* measuring, int32_t* start, const char** id);
void CtrPacer_ftState(uint32_t* out); // 8 values

// ===[ Recording ]===

static inline __attribute__((always_inline)) uint32_t ftAt(uint64_t tick) {
    return (uint32_t) ((tick - ft.base) >> FT_SHIFT);
}

FT_TEXT static uint16_t ftTenths(uint64_t ticks, uint8_t* flags) {
    uint64_t t = ticks / FT_TICKS_PER_TENTH;
    if (t > 65535u) {
        *flags |= FT_FLAG_CLIPPED;
        return 65535u;
    }
    return (uint16_t) t;
}

FT_TEXT static uint16_t ftTenthsMs(float ms, uint8_t* flags) {
    if (!(ms > 0.0f)) return 0;
    float t = ms * 10.0f + 0.5f;
    if (t >= 65535.0f) {
        *flags |= FT_FLAG_CLIPPED;
        return 65535u;
    }
    return (uint16_t) t;
}

FT_TEXT static uint8_t ftByte(uint32_t n, uint8_t* flags) {
    if (n > 255u) {
        *flags |= FT_FLAG_CLIPPED;
        return 255u;
    }
    return (uint8_t) n;
}

FT_TEXT static uint8_t ftThread(void) {
    void* tls = getThreadLocalStorage();
    if (tls == ft.mainTls) return 0;
    uint32_t n = ft.threads;
    for (uint32_t i = 1; i < n && i < FT_THREADS; i++)
        if (ft.tls[i] == tls) return (uint8_t) i;
    uint32_t i = __atomic_fetch_add(&ft.threads, 1, __ATOMIC_RELAXED);
    if (i == 0) i = __atomic_fetch_add(&ft.threads, 1, __ATOMIC_RELAXED); // slot 0 is main's
    if (i >= FT_THREADS) return FT_THREADS; // printed as t16: more threads than slots
    s32 prio = -1;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    ft.prio[i] = prio;
    ft.tls[i] = tls;
    return (uint8_t) i;
}

// The slot (+1) of the open file h, or 0.
FT_TEXT static uint8_t ftFileOf(Handle h) {
    if (h == 0) return 0;
    for (uint32_t i = 0; i < FT_FILES; i++)
        if (ftFiles[i].handle == h) return (uint8_t) (i + 1);
    return 0;
}

// Name-only slots (files closed in the window, renames, deletes) are freed once the window is written.
FT_TEXT static void ftFilesRelease(void) {
    for (uint32_t i = 0; i < FT_FILES; i++)
        if (ftFiles[i].handle == FT_FILE_CLOSED) ftFiles[i].handle = 0;
}

// Any thread, from boot: a file was opened as handle h (0: a name only). Returns its slot (+1).
FT_TEXT static uint8_t ftFileOpened(Handle h, FS_Path path) {
    uint32_t start = __atomic_fetch_add(&ftFileNext, 1, __ATOMIC_RELAXED), i = start % FT_FILES;
    for (uint32_t k = 0; k < FT_FILES; k++) {
        Handle expect = 0;
        uint32_t j = (start + k) % FT_FILES;
        if (__atomic_compare_exchange_n(&ftFiles[j].handle, &expect, FT_FILE_BUSY, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            i = j;
            break;
        }
    }
    // (All taken: the rotating slot is reused.)
    ftFiles[i].handle = FT_FILE_BUSY;
    char* d = ftFiles[i].name;
    const uint32_t cap = sizeof(ftFiles[i].name) - 1;
    uint32_t n = 0;
    if (path.type == PATH_UTF16 && path.data != nullptr) {
        const u16* s = (const u16*) path.data;
        uint32_t chars = path.size / 2;
        while (chars > 0 && s[chars - 1] == 0) chars--;
        for (uint32_t k = chars > cap ? chars - cap : 0; k < chars; k++) d[n++] = s[k] > 0x20 && s[k] < 0x80 ? (char) s[k] : '_';
    } else if (path.type == PATH_ASCII && path.data != nullptr) {
        const char* s = (const char*) path.data;
        uint32_t chars = (uint32_t) strnlen(s, path.size);
        for (uint32_t k = chars > cap ? chars - cap : 0; k < chars; k++) d[n++] = s[k] > 0x20 ? s[k] : '_';
    }
    d[n] = '\0';
    __atomic_store_n(&ftFiles[i].handle, h != 0 ? h : FT_FILE_CLOSED, __ATOMIC_RELEASE);
    return (uint8_t) (i + 1);
}

// Any thread: one event that began at t0 (the frame then under way is taken at its start: frame).
FT_TEXT static void ftEvent(char kind, uint32_t frame, uint64_t t0, uint32_t arg, uint8_t file, uint8_t src) {
    uint64_t t1 = svcGetSystemTick();
    if (!ft.on) return;
    uint32_t i = __atomic_fetch_add(&ft.nEvents, 1, __ATOMIC_RELAXED);
    if (i >= ft.eventCap) return;
    FtEvent* e = &ft.events[i];
    e->dur = (uint32_t) ((t1 - t0) >> FT_SHIFT);
    e->t = ftAt(t0 > ft.base ? t0 : ft.base); // one begun before the window: at its start
    e->arg = arg;
    int32_t df = (int32_t) (frame - ft.first);
    e->frame = (int16_t) (df < -1 ? -1 : df > 32767 ? 32767 : df);
    e->thread = ftThread();
    e->file = file;
    e->src = src;
    __atomic_store_n(&e->kind, kind, __ATOMIC_RELEASE); // last: the dump skips one still being written
}

// ===[ Frames ]===

FT_TEXT static void ftCloseFrame(uint64_t now, uint64_t gpuWait) {
    if (ft.nFrames >= ft.frameCap) {
        ft.framesLost++;
        return;
    }
    const FtCurrent* c = &ft.cur;
    FtFrame* f = &ft.frames[ft.nFrames++];
    uint8_t flags = c->flags;
    uint64_t marks[7] = {c->tWait, c->tStep, c->tDraw, c->tAudio, c->tRoom, c->tPresent, now};
    // Each phase runs to the next mark the frame reached (a frame the bench paused has no step, draw or audio marks).
    for (uint32_t i = 0; i < 6; i++) {
        uint32_t j = i + 1;
        while (j < 6 && marks[j] == 0) j++;
        f->seg[i] = marks[i] != 0 && marks[j] >= marks[i] ? ftTenths(marks[j] - marks[i], &flags) : 0;
    }
    f->frame = c->frame;
    f->t = ftAt(c->tWait);
    f->gpuWait = ftTenths(gpuWait - c->gpuWaitAt, &flags);
    f->gpu = ftTenthsMs(c->gpu, &flags);
    f->vbWait = (uint16_t) (c->vbWait - ft.baseVb);
    f->vbStep = (uint16_t) (c->vbStep - ft.baseVb);
    f->slot = (uint16_t) (c->pacer[0] - ft.baseVb);
    f->len = ftByte(c->pacer[1], &flags);
    f->delay = ftByte(c->pacer[2], &flags);
    f->shown = (uint16_t) (c->pacer[3] - ft.baseVb);
    f->flips = ftByte(c->pacer[4] - ft.lastFlips, &flags);
    ft.lastFlips = c->pacer[4];
    if (c->pacer[5] != ft.lastShifts) flags |= FT_FLAG_SHIFTED;
    ft.lastShifts = c->pacer[5];
    if (c->pacer[6] != 0) flags |= FT_FLAG_SKIPPING;
    if (c->pacer[7] != 0) flags |= FT_FLAG_SLOT_LATE;
    CtrRenderStats rs = CtrRenderer_getStats(ft.session->renderer);
    f->tex = ftByte(rs.textureLoads - ft.texLoads, &flags);
    f->texMs = ftTenthsMs(rs.textureLoadMs - ft.texMs, &flags);
    ft.texLoads = rs.textureLoads;
    ft.texMs = rs.textureLoadMs;
    f->flags = flags;
    f->pad = 0;
}

FT_TEXT static void ftAlloc(void) {
    ft.allocTried = true;
    for (uint32_t div = 1; div <= 8; div *= 2) {
        uint32_t frames = FT_FRAMES / div, events = FT_EVENTS / div;
        void* p = malloc(frames * sizeof(FtFrame) + events * sizeof(FtEvent));
        if (p != nullptr) {
            ft.frames = (FtFrame*) p;
            ft.events = (FtEvent*) (ft.frames + frames);
            ft.frameCap = frames;
            ft.eventCap = events;
            return;
        }
    }
    logWarn(ftLogNoMemory, (unsigned long) (FT_FRAMES / 8 * sizeof(FtFrame) + FT_EVENTS / 8 * sizeof(FtEvent)));
}

// The window opens: the frame under way is its first.
FT_TEXT static void ftOpen(int32_t start, const char* id) {
    ft.base = ft.cur.tWait;
    ft.baseVb = ft.cur.vbWait;
    ft.start = start;
    ft.first = ft.cur.frame;
    ft.nFrames = ft.framesLost = 0;
    for (uint32_t i = 0; i < ft.eventCap; i++) ft.events[i].kind = 0;
    ft.nEvents = 0;
    snprintf(ft.id, sizeof(ft.id), ftPathFormat + 3, id != nullptr ? id : ftNone); // "%s"
    ft.lastFlips = ft.cur.pacer[4];
    ft.lastShifts = ft.cur.pacer[5];
    CtrRenderStats rs = CtrRenderer_getStats(ft.session->renderer);
    ft.texLoads = rs.textureLoads;
    ft.texMs = rs.textureLoadMs;
    __atomic_store_n(&ft.on, true, __ATOMIC_RELEASE);
}

// ===[ Writing a window out ]===

static FT_TEXT int ftByStart(const void* a, const void* b) {
    const FtEvent* x = (const FtEvent*) a;
    const FtEvent* y = (const FtEvent*) b;
    if (x->kind == 0 || y->kind == 0) return (x->kind == 0) - (y->kind == 0); // unwritten ones last
    return x->t < y->t ? -1 : x->t > y->t ? 1 : 0;
}

// ticks >> FT_SHIFT as microseconds.
FT_TEXT static uint32_t ftUs(uint32_t t) {
    return (uint32_t) (((uint64_t) t << FT_SHIFT) * 1000000ull / SYSCLOCK_ARM11);
}

typedef struct {
    FILE* f;
    char buf[4096];
    uint32_t n, bytes;
} FtOut;

FT_TEXT static void ftFlushOut(FtOut* o) {
    if (o->n > 0) fwrite(o->buf, 1, o->n, o->f);
    o->bytes += o->n;
    o->n = 0;
}

FT_TEXT static char* ftRoom(FtOut* o) {
    if (o->n > sizeof(o->buf) - 256) ftFlushOut(o);
    return o->buf + o->n;
}

FT_TEXT static void ftFrameOut(FtOut* o, const FtFrame* f) {
    uint32_t us = ftUs(f->t);
    int k = snprintf(ftRoom(o), 256, ftFrameLine, (unsigned long) f->frame, (unsigned long) (us / 1000),
                     (unsigned long) (us % 1000 / 100), f->seg[0] / 10, f->seg[0] % 10, f->seg[1] / 10, f->seg[1] % 10,
                     f->seg[2] / 10, f->seg[2] % 10, f->seg[3] / 10, f->seg[3] % 10, f->seg[4] / 10, f->seg[4] % 10,
                     f->seg[5] / 10, f->seg[5] % 10, f->gpuWait / 10, f->gpuWait % 10, f->gpu / 10, f->gpu % 10,
                     f->vbWait, f->vbStep, f->slot, f->len, f->delay, f->shown, f->flips, f->tex, f->texMs / 10,
                     f->texMs % 10, f->flags);
    if (k > 0) o->n += (uint32_t) (k < 256 ? k : 255);
}

FT_TEXT static void ftEventOut(FtOut* o, const FtEvent* e) {
    char thread[24];
    const char* name = e->thread == 0 ? ftMain : e->thread < FT_THREADS ? CtrWatchdog_threadNameOf(ft.tls[e->thread]) : nullptr;
    if (name != nullptr) {
        uint32_t i = 0;
        for (; name[i] != '\0' && i < sizeof(thread) - 1; i++) thread[i] = name[i] == ' ' ? '_' : name[i];
        thread[i] = '\0';
    } else {
        snprintf(thread, sizeof(thread), ftThreadFormat, (unsigned long) e->thread,
                 (long) (e->thread < FT_THREADS ? ft.prio[e->thread] : -1));
    }
    uint32_t t = ftUs(e->t), d = ftUs(e->dur);
    long frame = (long) ft.first + e->frame;
    int k;
    if (e->kind == 'T') {
        k = snprintf(ftRoom(o), 256, ftTileLine, frame, (unsigned long) (t / 1000), (unsigned long) (t % 1000 / 100),
                     (unsigned long) (d / 1000), (unsigned long) (d % 1000 / 10), thread, (unsigned long) (e->arg >> 16),
                     (unsigned long) (e->arg & 0xFFFFu), ftSources[e->src & 3]);
    } else {
        const char* file = e->file > 0 && e->file <= FT_FILES ? ftFiles[e->file - 1].name : ftNone;
        k = snprintf(ftRoom(o), 256, ftEventLine, frame, (unsigned long) (t / 1000), (unsigned long) (t % 1000 / 100),
                     (unsigned long) (d / 1000), (unsigned long) (d % 1000 / 10), thread, e->kind, (long) (int32_t) e->arg,
                     file[0] != '\0' ? ftSpace : ftNone, file);
    }
    if (k > 0) o->n += (uint32_t) (k < 256 ? k : 255);
}

// The window ended (or another began): its records go to frametrace.txt. Main thread, outside any window.
FT_TEXT static void ftDump(void) {
    __atomic_store_n(&ft.on, false, __ATOMIC_RELEASE);
    uint64_t t0 = svcGetSystemTick();
    uint32_t claimed = ft.nEvents, n = claimed < ft.eventCap ? claimed : ft.eventCap;
    qsort(ft.events, n, sizeof(FtEvent), ftByStart);
    while (n > 0 && ft.events[n - 1].kind == 0) n--;
    char path[256];
    snprintf(path, sizeof(path), ftPathFormat, ft.session->gameDir, ftFileName);
    unsigned long last = ft.nFrames > 0 ? (unsigned long) ft.frames[ft.nFrames - 1].frame : (unsigned long) ft.first;
    static FtOut out FT_BSS;
    out.f = fopen(path, ft.fileStarted ? ftAppend : ftWrite);
    if (out.f == nullptr) {
        logWarn(ftLogOpenFailed, path, (unsigned long) ft.first, last);
        return;
    }
    if (!ft.fileStarted) CtrLinkFiles_note(path);
    ft.fileStarted = true;
    out.n = out.bytes = 0;
    int k = snprintf(out.buf, sizeof(out.buf), ftHeader, ftTitle, ftTarget, ft.id, (unsigned long) ft.first, last,
                     (unsigned long) ft.nFrames, (unsigned long) n, (unsigned long) ft.framesLost,
                     (unsigned long) (claimed - n));
    if (k > 0) out.n = (uint32_t) k;
    uint32_t i = 0, j = 0;
    while (i < ft.nFrames || j < n) {
        if (j >= n || (i < ft.nFrames && ft.frames[i].t <= ft.events[j].t)) ftFrameOut(&out, &ft.frames[i++]);
        else ftEventOut(&out, &ft.events[j++]);
    }
    ftFlushOut(&out);
    fclose(out.f);
    ftFilesRelease();
    logInfo(ftLogLine, ft.id, (unsigned long) ft.first, last, (unsigned long) ft.nFrames, (unsigned long) n,
            (unsigned long) ft.framesLost, (unsigned long) (claimed - n), path, (unsigned long) out.bytes,
            (unsigned long) ((svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000)));
}

// ===[ Wraps ]===

void __real_CtrWatchdog_mark(const char* phase);
void __wrap_CtrWatchdog_mark(const char* phase);

FT_TEXT void __wrap_CtrWatchdog_mark(const char* phase) {
    __real_CtrWatchdog_mark(phase);
    char c = phase[0];
    if (c != 'w' && c != 's' && c != 'd' && c != 'a' && c != 'r' && c != 'p') return;
    void* tls = getThreadLocalStorage();
    FtCurrent* cur = &ft.cur;
    if (c == 'w') {
        if (strcmp(phase, ftMarkWait) != 0) return;
        if (ft.mainTls == nullptr) ft.mainTls = tls;
        if (tls != ft.mainTls) return;
        uint64_t now = svcGetSystemTick();
        uint64_t gpuWait = CtrGpu_waitTotalTicks();
        if (cur->valid && cur->inWindow && ft.on) ftCloseFrame(now, gpuWait);
        uint32_t frame = cur->frame + 1;
        memset(cur, 0, sizeof(*cur));
        cur->valid = true;
        cur->frame = frame;
        cur->tWait = now;
        cur->vbWait = C3D_FrameCounter(0);
        cur->gpuWaitAt = gpuWait;
        ft.frame = frame;
        // A window that ended in a frame the bench paused (its next scene loading, its results): no step mark follows.
        if (ft.on) {
            bool measuring = false;
            int32_t start = 0;
            const char* id = nullptr;
            CtrBench_ftState(&measuring, &start, &id);
            if (!measuring || start != ft.start) ftDump();
        }
        return;
    }
    if (tls != ft.mainTls || !cur->valid) return;
    if (c == 's') {
        if (strcmp(phase, ftMarkStep) != 0) return;
        cur->tStep = svcGetSystemTick();
        cur->vbStep = C3D_FrameCounter(0);
        bool measuring = false;
        int32_t start = 0;
        const char* id = nullptr;
        const CtrBenchSession* s = CtrBench_ftState(&measuring, &start, &id);
        if (s->runner != nullptr) ft.frame = cur->frame = (uint32_t) s->runner->frameCount;
        if (!ft.allocTried && CtrBench_running()) ftAlloc();
        if (ft.on && (!measuring || start != ft.start)) ftDump();
        if (!measuring || ft.frames == nullptr || s->runner == nullptr || s->renderer == nullptr) return;
        ft.session = s;
        CtrPacer_ftState(cur->pacer);
        cur->gpu = CtrRenderer_getStats(s->renderer).gpuMs;
        if (!ft.on) ftOpen(start, id);
        cur->inWindow = true;
    } else if (c == 'd') {
        if (strcmp(phase, ftMarkDraw) == 0) cur->tDraw = svcGetSystemTick();
    } else if (c == 'a') {
        if (strcmp(phase, ftMarkAudio) == 0) cur->tAudio = svcGetSystemTick();
    } else if (c == 'r') {
        if (strcmp(phase, ftMarkRoom) != 0) return;
        cur->tRoom = svcGetSystemTick();
        // main.c skips this frame's present when it changed room (the same test).
        if (cur->inWindow && ft.session->runner->pendingRoom != -1) cur->flags |= FT_FLAG_ROOM;
    } else if (c == 'p') {
        if (strcmp(phase, ftMarkPresent) == 0) cur->tPresent = svcGetSystemTick();
    }
}

// --exit-after leaves in the frame the last window ended in (main.c), before any mark of the next.
bool __real_CtrBench_takeFinished(void);
bool __wrap_CtrBench_takeFinished(void);
FT_TEXT bool __wrap_CtrBench_takeFinished(void) {
    bool finished = __real_CtrBench_takeFinished();
    if (finished && ft.on) ftDump();
    return finished;
}

// The SD card and RomFS calls (newlib's sdmc and romfs devices and the engine's own file code all end in these).
Result __real_FSUSER_OpenFile(Handle* out, FS_Archive archive, FS_Path path, u32 flags, u32 attributes);
Result __wrap_FSUSER_OpenFile(Handle* out, FS_Archive archive, FS_Path path, u32 flags, u32 attributes);
FT_TEXT Result __wrap_FSUSER_OpenFile(Handle* out, FS_Archive archive, FS_Path path, u32 flags, u32 attributes) {
    uint32_t frame = ft.frame;
    uint64_t t0 = svcGetSystemTick();
    Result r = __real_FSUSER_OpenFile(out, archive, path, flags, attributes);
    Handle h = R_SUCCEEDED(r) && out != nullptr ? *out : 0;
    uint8_t file = h != 0 || ft.on ? ftFileOpened(h, path) : 0; // (failed opens only in a window)
    ftEvent('O', frame, t0, (uint32_t) h, file, 0);
    return r;
}

Result __real_FSUSER_OpenFileDirectly(Handle* out, FS_ArchiveID id, FS_Path archivePath, FS_Path filePath, u32 flags,
                                      u32 attributes);
Result __wrap_FSUSER_OpenFileDirectly(Handle* out, FS_ArchiveID id, FS_Path archivePath, FS_Path filePath, u32 flags,
                                      u32 attributes);
FT_TEXT Result __wrap_FSUSER_OpenFileDirectly(Handle* out, FS_ArchiveID id, FS_Path archivePath, FS_Path filePath,
                                              u32 flags, u32 attributes) {
    uint32_t frame = ft.frame;
    uint64_t t0 = svcGetSystemTick();
    Result r = __real_FSUSER_OpenFileDirectly(out, id, archivePath, filePath, flags, attributes);
    Handle h = R_SUCCEEDED(r) && out != nullptr ? *out : 0;
    uint8_t file = h != 0 || ft.on ? ftFileOpened(h, filePath) : 0;
    ftEvent('O', frame, t0, (uint32_t) h, file, 0);
    return r;
}

Result __real_FSUSER_RenameFile(FS_Archive a, FS_Path p, FS_Archive b, FS_Path q);
Result __wrap_FSUSER_RenameFile(FS_Archive a, FS_Path p, FS_Archive b, FS_Path q);
FT_TEXT Result __wrap_FSUSER_RenameFile(FS_Archive a, FS_Path p, FS_Archive b, FS_Path q) {
    if (!ft.on) return __real_FSUSER_RenameFile(a, p, b, q);
    uint32_t frame = ft.frame;
    uint64_t t0 = svcGetSystemTick();
    Result r = __real_FSUSER_RenameFile(a, p, b, q);
    ftEvent('N', frame, t0, (uint32_t) r, ftFileOpened(0, q), 0);
    return r;
}

Result __real_FSUSER_DeleteFile(FS_Archive archive, FS_Path path);
Result __wrap_FSUSER_DeleteFile(FS_Archive archive, FS_Path path);
FT_TEXT Result __wrap_FSUSER_DeleteFile(FS_Archive archive, FS_Path path) {
    if (!ft.on) return __real_FSUSER_DeleteFile(archive, path);
    uint32_t frame = ft.frame;
    uint64_t t0 = svcGetSystemTick();
    Result r = __real_FSUSER_DeleteFile(archive, path);
    ftEvent('D', frame, t0, (uint32_t) r, ftFileOpened(0, path), 0);
    return r;
}

Result __real_FSFILE_Read(Handle h, u32* got, u64 offset, void* buffer, u32 size);
Result __wrap_FSFILE_Read(Handle h, u32* got, u64 offset, void* buffer, u32 size);
FT_TEXT Result __wrap_FSFILE_Read(Handle h, u32* got, u64 offset, void* buffer, u32 size) {
    if (!ft.on) return __real_FSFILE_Read(h, got, offset, buffer, size);
    uint32_t frame = ft.frame;
    uint64_t t0 = svcGetSystemTick();
    Result r = __real_FSFILE_Read(h, got, offset, buffer, size);
    ftEvent('R', frame, t0, size, ftFileOf(h), 0);
    return r;
}

Result __real_FSFILE_Write(Handle h, u32* put, u64 offset, const void* buffer, u32 size, u32 flags);
Result __wrap_FSFILE_Write(Handle h, u32* put, u64 offset, const void* buffer, u32 size, u32 flags);
FT_TEXT Result __wrap_FSFILE_Write(Handle h, u32* put, u64 offset, const void* buffer, u32 size, u32 flags) {
    if (!ft.on) return __real_FSFILE_Write(h, put, offset, buffer, size, flags);
    uint32_t frame = ft.frame;
    uint64_t t0 = svcGetSystemTick();
    Result r = __real_FSFILE_Write(h, put, offset, buffer, size, flags);
    ftEvent('W', frame, t0, size, ftFileOf(h), 0);
    return r;
}

Result __real_FSFILE_Flush(Handle h);
Result __wrap_FSFILE_Flush(Handle h);
FT_TEXT Result __wrap_FSFILE_Flush(Handle h) {
    if (!ft.on) return __real_FSFILE_Flush(h);
    uint32_t frame = ft.frame;
    uint64_t t0 = svcGetSystemTick();
    Result r = __real_FSFILE_Flush(h);
    ftEvent('F', frame, t0, 0, ftFileOf(h), 0);
    return r;
}

Result __real_FSFILE_Close(Handle h);
Result __wrap_FSFILE_Close(Handle h);
FT_TEXT Result __wrap_FSFILE_Close(Handle h) {
    uint8_t file = ftFileOf(h);
    if (!ft.on) {
        if (file > 0) ftFiles[file - 1].handle = 0;
        return __real_FSFILE_Close(h);
    }
    uint32_t frame = ft.frame;
    uint64_t t0 = svcGetSystemTick();
    Result r = __real_FSFILE_Close(h);
    if (file > 0) ftFiles[file - 1].handle = FT_FILE_CLOSED;
    ftEvent('C', frame, t0, 0, file, 0);
    return r;
}

// Audio decode reads: ctr_audio_system.c's own wrap (flowend's, renamed ftInner_CtrDecoder_read in frametrace
// builds by CMakeLists.txt) runs inside this one.
int32_t ftInner_CtrDecoder_read(CtrDecoder* dec, int16_t* out, uint32_t maxFrames);
int32_t __wrap_CtrDecoder_read(CtrDecoder* dec, int16_t* out, uint32_t maxFrames);
FT_TEXT int32_t __wrap_CtrDecoder_read(CtrDecoder* dec, int16_t* out, uint32_t maxFrames) {
    if (!ft.on) return ftInner_CtrDecoder_read(dec, out, maxFrames);
    uint32_t frame = ft.frame;
    uint64_t t0 = svcGetSystemTick();
    int32_t got = ftInner_CtrDecoder_read(dec, out, maxFrames);
    ftEvent('A', frame, t0, (uint32_t) got, 0, 0);
    return got;
}

// Draw-time tile loads (the renderer's calls; ensureTiles' own call to ensureTilesBut is inside its object).
static inline __attribute__((always_inline)) uint32_t ftLoads(const CtrTextureCache* c) {
    return c->fromRam.count + c->fromRead.count + c->fromFile.count + c->fromImage.count;
}

FT_TEXT static void ftTiles(const CtrTextureCache* c, const CtrPage* page, uint32_t frame, uint64_t t0, uint32_t loads,
                            uint32_t ram, uint32_t read, uint32_t file, uint32_t tiles) {
    uint8_t src = c->fromRam.count != ram ? 0 : c->fromRead.count != read ? 1 : c->fromFile.count != file ? 2 : 3;
    uint32_t added = c->fromRam.tiles + c->fromRead.tiles + c->fromFile.tiles + c->fromImage.tiles - tiles;
    uint32_t id = page >= c->pages && page < c->pages + c->pageCount ? (uint32_t) (page - c->pages) : 0xFFFFu;
    (void) loads;
    ftEvent('T', frame, t0, (id & 0xFFFFu) << 16 | (added > 0xFFFFu ? 0xFFFFu : added), 0, src);
}

void __real_CtrTextures_ensureTilesBut(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1,
                                       int32_t ty1, uint64_t drawnElsewise);
void __wrap_CtrTextures_ensureTilesBut(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1,
                                       int32_t ty1, uint64_t drawnElsewise);
FT_TEXT void __wrap_CtrTextures_ensureTilesBut(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0,
                                               int32_t tx1, int32_t ty1, uint64_t drawnElsewise) {
    if (!ft.on) {
        __real_CtrTextures_ensureTilesBut(cache, page, tx0, ty0, tx1, ty1, drawnElsewise);
        return;
    }
    uint32_t frame = ft.frame, loads = ftLoads(cache);
    uint32_t ram = cache->fromRam.count, read = cache->fromRead.count, file = cache->fromFile.count;
    uint32_t tiles = cache->fromRam.tiles + cache->fromRead.tiles + cache->fromFile.tiles + cache->fromImage.tiles;
    uint64_t t0 = svcGetSystemTick();
    __real_CtrTextures_ensureTilesBut(cache, page, tx0, ty0, tx1, ty1, drawnElsewise);
    if (ftLoads(cache) != loads) ftTiles(cache, page, frame, t0, loads, ram, read, file, tiles);
}

void __real_CtrTextures_ensureTiles(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1);
void __wrap_CtrTextures_ensureTiles(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1);
FT_TEXT void __wrap_CtrTextures_ensureTiles(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1,
                                            int32_t ty1) {
    if (!ft.on) {
        __real_CtrTextures_ensureTiles(cache, page, tx0, ty0, tx1, ty1);
        return;
    }
    uint32_t frame = ft.frame, loads = ftLoads(cache);
    uint32_t ram = cache->fromRam.count, read = cache->fromRead.count, file = cache->fromFile.count;
    uint32_t tiles = cache->fromRam.tiles + cache->fromRead.tiles + cache->fromFile.tiles + cache->fromImage.tiles;
    uint64_t t0 = svcGetSystemTick();
    __real_CtrTextures_ensureTiles(cache, page, tx0, ty0, tx1, ty1);
    if (ftLoads(cache) != loads) ftTiles(cache, page, frame, t0, loads, ram, read, file, tiles);
}

#endif
