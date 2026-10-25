#include "ctr_video_layout.inc"

// Video playback (video_open and friends) for the games' intro videos: MP4
// files with H.264 video and, in Chapter 3's case, AAC audio.
//
// - The audio track is streamed by the audio system from the same file
//   (ctr_audio_decoder.c), and its position is the clock the pictures follow.
//   A video without one (Chapter 5's: the game plays its music itself) runs
//   on the wall clock.
// - On a New 3DS the pictures come from its hardware H.264 decoder (MVD,
//   through libctru's mvdstd): a thread feeds it the samples and collects the
//   frames, as RGB565 in linear memory, into a few slots. Each game frame
//   uploads the newest one that is due onto the video surface with a GX
//   transfer (CtrRenderer_uploadVideoFrame), halved when the video is larger
//   than a texture can be.
// - Without the decoder (an Old 3DS, an emulator without it, or a decoder
//   that fails), the video plays without pictures: the surface has a notice, and
//   the clock, the audio, video_get_position and video_end are as they would
//   be, so the game's captions and timing still work.
// - Builds with openh264 linked in (DELTARUNE builds, tools/deps/openh264)
//   decode in software instead there, with Y2R doing the colour conversion
//   (see "Software decoding"): every frame of the copy of the video made on a
//   PC for the Old 3DS (<name>.3ds.mp4, tools/videos), or without one, the
//   video's keyframes as a slideshow. A New 3DS plays that copy (on its
//   decoder) in place of a stream past its decoder's level, Chapter 5's;
//   without one, that video plays without pictures there.
#include "video.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <dirent.h>

#include "log.h"
#include "runner.h"

#include "ctr_log.h"
#include "ctr_mp4.h"
#include "ctr_renderer.h"
#include "ctr_video_sw.h"
#include "ctr_video.h"

// Samples are read through a window this large (see CtrMp4).
#define VIDEO_WINDOW_BYTES (256 * 1024)
#define VIDEO_PATH_MAX 256
// Decoded frames waiting to be shown, being decoded or being uploaded:
// FEW_SLOTS, or for a copy decoded in software up to MAX_SLOTS (see
// startSwPlayback).
#define FEW_SLOTS 4
#define MAX_SLOTS 96
#define MIN_SLOTS 3
// Reordering (B-frames) keeps fewer than this many frames inside the decoder.
#define MAX_PENDING 32
// A video with audio whose audio hasn't started after this long runs on the
// wall clock; one without waits this long at most for its first frame.
#define START_TIMEOUT 1.5
// Samples fed without a frame coming out before the decoder is given up.
#define MAX_SILENT_SAMPLES 24
// How long one frame may take to come out of the decoder.
#define RENDER_TIMEOUT_MS 500.0
// Corner pixels set before the decoder writes a frame, to tell whether it has.
#define SENTINEL 0x1111u
#define MESSAGE_SLOTS 12
#define MESSAGE_LENGTH 192
// The decode thread's stack. Most of it is filled with STACK_PAINT when the
// thread starts, and how much of that it overwrote is logged at the close.
#define DECODE_STACK_BYTES (32 * 1024)
#define STACK_PAINT_BYTES (28 * 1024)
#define STACK_PAINT 0xA5u
// The hardware decoder's first steps and the first uploads are logged, from
// the diagnosis of a crash in Chapter 5's video on a New 3DS (the level gate
// in startDecoder). The lines were flushed to the card one by one then; they
// no longer are: on an Old 3DS a flush waits for the log writer, which the
// software decode thread keeps off the CPU, and the game stalled for up to a
// second at each one.
#define TRACE_SAMPLES 6
#define TRACE_UPLOADS 4
// A shown frame may be this much early (a frame is on screen a little after
// it is drawn).
#define SHOW_AHEAD (1.0 / 60.0)
// The clock moving this far between two updates of the video means the game
// stopped updating it for a while (frames then due at once aren't counted
// as dropped late).
#define VIDEO_UPDATE_GAP 0.25
// Set to 1 for a test pattern instead of black when there is no decoder
// (sdmc:/.../videotest.txt does the same at run time): top red, bottom blue,
// a white square at the top left. It checks the upload's orientation in an
// emulator.
#define TEST_PATTERN_FILE CTRPORT_GAME_DIR "/videotest.txt"
// Lets the hardware decoder try a stream past level 3.2 as a level 3.2 one
// (see startDecoder). Without it such a stream plays without pictures.
#define LEVEL_TRY_FILE CTRPORT_GAME_DIR "/videohd.txt"
// The largest texture side (the renderer's MAX_SURFACE_SIZE): a video past it
// is shown at half size.
#define VIDEO_TEXTURE_MAX 1024
// The copy of <name>.mp4 made for the Old 3DS by tools/videos/make-3ds-videos.ps1
// is <name>.3ds.mp4, next to it.
#define COPY_SUFFIX ".3ds.mp4"

#define TICKS_PER_SECOND ((double) SYSCLOCK_ARM11)

typedef enum {
    SLOT_FREE,
    SLOT_DECODING, // held by the decode thread
    SLOT_READY,    // a decoded frame, not yet shown
    SLOT_SHOWN,    // uploaded; free once the GPU has read it
} SlotState;

typedef struct {
    uint16_t* pixels; // linear RGB565, slotW x slotH
    SlotState state;
    double pts;
    uint32_t fence;
} FrameSlot;

typedef enum {
    MODE_PICTURES,    // the decode thread is making frames
    MODE_PICTURELESS, // no decoder: a notice on the video surface
} Mode;

typedef struct {
    // Set by open().
    bool opened;
    char name[64];
    char path[VIDEO_PATH_MAX];
    CtrMp4 mp4; // the decode thread's once it runs; the copy's when fromCopy
    // The video's size, as the game is told and its surface is made, even
    // when the pictures come from a smaller copy.
    int32_t width, height;
    uint32_t codedW, codedH; // multiples of 16: the decoded pictures' size
    bool fromCopy;           // the pictures come from <name>.3ds.mp4 (see swOpenCopy)
    // The slots' pictures: the coded size, or half of it when the software
    // path halves them before they go to a slot (preHalved).
    uint32_t slotW, slotH;
    bool preHalved;
    double duration;
    double frameSeconds;
    bool hasAudio;
    uint32_t nalLengthSize;

    // Clock (game thread).
    bool paused;
    bool resumed; // resume() has been called since open()
    bool clockStarted;
    double clock;
    u64 lastTick;
    Runner* frameClockRunner; // state-hash or fixed-seed bench only
    int32_t lastFrame;
    unsigned suspended; // HOME/menu and sleep can overlap; independent of video_pause
    aptHookCookie aptCookie;
    double waited; // seconds played before the clock started
    bool audioSeen;
    double lastAudio;

    // Surface (game thread).
    Renderer* renderer;
    int32_t surfaceId;
    bool halve;
    int32_t pendingSlot; // chosen by update(), uploaded by draw()
    bool blankPending;
    bool blankDone;
    bool testPattern;
    bool keptOffHardware; // a stream past the New 3DS decoder's level, which isn't decoded in software yet either
    uint16_t* patternPixels;
    uint32_t patternFence;

    // Shared with the decode thread, under lock.
    LightLock lock;
    LightEvent wake; // a slot was freed, or quit
    FrameSlot slots[MAX_SLOTS];
    int32_t slotCount;
    Mode mode;
    double sharedClock;
    bool sharedClockStarted;
    bool decodeDone;
    char messages[MESSAGE_SLOTS][MESSAGE_LENGTH];
    int32_t messageCount, messagesDropped;

    // Decode thread.
    Thread thread;
    volatile bool quit;
    bool mvdOpen;
    MVDSTD_Config config;
    uint8_t* au; // linear
    uint32_t auCap;
    int32_t heldSlot;
    double pending[MAX_PENDING];
    int32_t pendingCount;

    // Statistics.
    u64 openTick;
    u64 waitTicks; // the decode thread waiting for a free slot
    double firstFrameSeconds;
    uint32_t fed, decoded, skippedRef, jumps, errors;
    uint32_t shown, lateDrops, uploadFailures;
    // Frames due all at once after the game went a while without updating
    // the video (so not the decoder's fault), and the clock at the last update.
    uint32_t gapDrops;
    double updateClock;
    double busySeconds, readSeconds, processMaxMs, renderMaxMs;
    uint32_t workBytes;
    uint32_t stackUsed; // bytes of the decode thread's painted stack it wrote
    uint8_t spsLevel;   // level_idc written into the SPS for the decoder, 0 to leave it
    uint32_t traceUploads;
#ifdef CTRPORT_SW_DECODE
    // Software decoding.
    int32_t swForcedTier, swTier;
    bool y2rOpen;
    // Linear: the picture Y2R converts, YUV 4:2:0, slotW x slotH: a copy, or
    // the picture at half size (preHalved), so the decoder can go on meanwhile.
    uint8_t* stage;
    int32_t convertSlot; // the slot Y2R is converting into, -1 for none
    double convertPts;
    u64 convertTicks; // the CPU's part of that conversion so far
    uint32_t tierChanges, keySkips, convertFailures;
    double convertSeconds, convertMaxMs, convertMsAvg;
#endif
} CtrVideo;

static CtrVideo* activeVideo;
static Runner* frameClockRunner;

void CtrVideo_setFrameClock(Runner* runner) { frameClockRunner = runner; }

static CtrVideo* impl(VideoDecoder* d) { return (CtrVideo*) d->impl; }

// A step of the hardware decoder or of an upload, for the log (any thread).
static void trace(const char* fmt, ...) {
    char line[MESSAGE_LENGTH];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    logInfo("Video trace: %s\n", line);
}

static double ticksToSeconds(u64 ticks) { return (double) ticks / TICKS_PER_SECOND; }

// ===[ Messages ]===
// The decode thread doesn't write the log itself: its lines are queued and
// written by the game thread.

static void post(CtrVideo* v, const char* fmt, ...) {
    char line[MESSAGE_LENGTH];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    LightLock_Lock(&v->lock);
    if (v->messageCount < MESSAGE_SLOTS) memcpy(v->messages[v->messageCount++], line, sizeof(line));
    else v->messagesDropped++;
    LightLock_Unlock(&v->lock);
}

static void flushMessages(CtrVideo* v) {
    char lines[MESSAGE_SLOTS][MESSAGE_LENGTH];
    LightLock_Lock(&v->lock);
    int32_t count = v->messageCount, dropped = v->messagesDropped;
    memcpy(lines, v->messages, sizeof(lines[0]) * (size_t) count);
    v->messageCount = 0;
    v->messagesDropped = 0;
    LightLock_Unlock(&v->lock);
    for (int32_t i = 0; count > i; i++) logInfo("%s", lines[i]);
    if (dropped > 0) logInfo("Video: %d more messages dropped\n", (int) dropped);
}

// ===[ Decode thread ]===

static void flushCache(const void* p, uint32_t size) { svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32) p, size); }
static void invalidateCache(const void* p, uint32_t size) { svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32) p, size); }

static void cornerOffsets(const CtrVideo* v, uint32_t corners[4]) {
    corners[0] = 0;
    corners[1] = v->slotW - 1;
    corners[2] = (v->slotH - 1) * v->slotW;
    corners[3] = v->slotH * v->slotW - 1;
}

// Before the decoder may write a frame into the slot.
static void armSlot(CtrVideo* v, FrameSlot* s) {
    uint32_t corners[4];
    cornerOffsets(v, corners);
    for (int i = 0; 4 > i; i++) {
        s->pixels[corners[i]] = SENTINEL;
        flushCache(&s->pixels[corners[i]], 2);
    }
    v->config.physaddr_outdata0 = osConvertVirtToPhys(s->pixels);
}

static bool VIDFIX_LATE slotWritten(CtrVideo* v, FrameSlot* s) {
    uint32_t corners[4];
    cornerOffsets(v, corners);
    bool written = false;
    for (int i = 0; 4 > i; i++) {
        invalidateCache(&s->pixels[corners[i]], 2);
        if (s->pixels[corners[i]] != SENTINEL) written = true;
    }
    return written;
}

// A free slot for the decoder to write into (the one held from before, if
// any), waiting for the game to show or drop a frame if there is none. -1
// once the thread is to stop.
static int32_t acquireSlot(CtrVideo* v) {
    if (v->heldSlot >= 0) return v->heldSlot;
    for (;;) {
        if (v->quit) return -1;
        LightLock_Lock(&v->lock);
        int32_t found = -1;
        for (int32_t i = 0; v->slotCount > i && found < 0; i++) {
            if (v->slots[i].state == SLOT_FREE) found = i;
        }
        if (found >= 0) v->slots[found].state = SLOT_DECODING;
        LightLock_Unlock(&v->lock);
        if (found >= 0) {
            v->heldSlot = found;
            return found;
        }
        u64 t0 = svcGetSystemTick();
        LightEvent_WaitTimeout(&v->wake, 50 * 1000 * 1000LL);
        v->waitTicks += svcGetSystemTick() - t0;
    }
}

static void pendingPush(CtrVideo* v, double pts) {
    if (v->pendingCount >= MAX_PENDING) {
        // More frames in than out: the decoder has lost some. Forget the oldest.
        memmove(v->pending, v->pending + 1, sizeof(v->pending[0]) * (MAX_PENDING - 1));
        v->pendingCount--;
    }
    int32_t i = v->pendingCount++;
    while (i > 0 && v->pending[i - 1] > pts) {
        v->pending[i] = v->pending[i - 1];
        i--;
    }
    v->pending[i] = pts;
}

// Frames come out of the decoder in display order, so each takes the
// earliest presentation time among the samples fed and not yet out.
static double pendingPop(CtrVideo* v, double fallback) {
    if (v->pendingCount == 0) return fallback;
    double pts = v->pending[0];
    v->pendingCount--;
    memmove(v->pending, v->pending + 1, sizeof(v->pending[0]) * (size_t) v->pendingCount);
    return pts;
}

// A line about a decoded frame, to check the decoder's output layout from a
// log: rows whose left half equals their right half, rows equal to the next
// row, the right-most and bottom-most non-black pixel, and the mean of each
// RGB565 field (top 5 bits, middle 6, low 5).
static void describeFrame(CtrVideo* v, FrameSlot* s) {
    uint32_t w = v->slotW, h = v->slotH, half = w / 2;
    invalidateCache(s->pixels, w * h * 2u);
    uint32_t sameHalves = 0, sameNext = 0, rows = 0, maxX = 0, maxY = 0;
    uint64_t sum[3] = { 0, 0, 0 }, count = 0;
    for (uint32_t y = 0; h > y; y += 8) {
        const uint16_t* row = s->pixels + (size_t) y * w;
        rows++;
        if (memcmp(row, row + half, half * 2u) == 0) sameHalves++;
        if (y + 1 < h && memcmp(row, row + w, w * 2u) == 0) sameNext++;
        for (uint32_t x = 0; w > x; x += 4) {
            uint16_t p = row[x];
            if (p != 0 && p != SENTINEL) {
                if (x > maxX) maxX = x;
                if (y > maxY) maxY = y;
            }
            sum[0] += p >> 11;
            sum[1] += (p >> 5) & 63;
            sum[2] += p & 31;
            count++;
        }
    }
    post(v, "Video: frame %lu: %lu of %lu rows with equal halves, %lu equal to the next row, non-black up to x %lu y %lu, means %.1f/%.1f/%.1f, "
            "centre 0x%04X\n",
         (unsigned long) v->decoded, (unsigned long) sameHalves, (unsigned long) rows, (unsigned long) sameNext, (unsigned long) maxX,
         (unsigned long) maxY, (double) sum[0] / (double) count, (double) sum[1] / (double) count, (double) sum[2] / (double) count,
         (unsigned) s->pixels[(size_t) (h / 2) * w + w / 2]);
}

static void publishSlotAt(CtrVideo* v, int32_t index, double pts) {
    LightLock_Lock(&v->lock);
    v->slots[index].pts = pts;
    v->slots[index].state = SLOT_READY;
    LightLock_Unlock(&v->lock);
    v->heldSlot = -1;
    v->decoded++;
    if (v->decoded == 1) v->firstFrameSeconds = ticksToSeconds(svcGetSystemTick() - v->openTick);
    if (v->decoded == 1 || v->decoded == 60) describeFrame(v, &v->slots[index]);
}

static void publishSlot(CtrVideo* v, int32_t index, double fallbackPts) { publishSlotAt(v, index, pendingPop(v, fallbackPts)); }

typedef enum { RENDER_FRAME, RENDER_NONE, RENDER_ERROR } RenderResult;

// Asks the decoder for its next frame, into the armed slot.
static RenderResult VIDFIX_LATE renderInto(CtrVideo* v, FrameSlot* s, Result* code) {
    u64 start = svcGetSystemTick();
    for (;;) {
        Result r = mvdstdRenderVideoFrame(&v->config, false);
        *code = r;
        // A changed corner is not a completion fence. MVD may still be
        // writing the rest of the picture while it reports BUSY.
        if (r == MVD_STATUS_OK) {
            double ms = ticksToSeconds(svcGetSystemTick() - start) * 1000.0;
            if (ms > v->renderMaxMs) v->renderMaxMs = ms;
            return slotWritten(v, s) ? RENDER_FRAME : RENDER_NONE;
        }
        if (r != MVD_STATUS_BUSY) return RENDER_ERROR;
        if (ticksToSeconds(svcGetSystemTick() - start) * 1000.0 > RENDER_TIMEOUT_MS) return RENDER_ERROR;
        svcSleepThread(250 * 1000);
    }
}

// Converts a sample's NAL units from 4-byte lengths to 00 00 01 start codes
// in place (each comes out a byte shorter). Returns the new size, 0 if the
// sample is malformed. *disposable: no slice in it is a reference.
static uint32_t toStartCodes(uint8_t* buf, uint32_t size, bool* disposable, uint8_t spsLevel) {
    uint32_t in = 0, out = 0;
    bool anySlice = false, anyReference = false;
    while (in + 4 <= size) {
        uint32_t len = ((uint32_t) buf[in] << 24) | ((uint32_t) buf[in + 1] << 16) | ((uint32_t) buf[in + 2] << 8) | buf[in + 3];
        in += 4;
        if (len == 0 || len > size - in) return 0;
        uint8_t header = buf[in];
        uint8_t type = header & 0x1F;
        if (type == 1 || type == 5) {
            anySlice = true;
            if ((header >> 5) & 3) anyReference = true;
        }
        if (type == 7 && spsLevel != 0 && len > 3) buf[in + 3] = spsLevel;
        buf[out] = 0;
        buf[out + 1] = 0;
        buf[out + 2] = 1;
        memmove(buf + out + 3, buf + in, len);
        out += 3 + len;
        in += len;
    }
    *disposable = anySlice && !anyReference;
    return out;
}

// The SPS and PPS from the avcC box, each fed on its own.
static bool feedParameterSets(CtrVideo* v) {
    const uint8_t* c = v->mp4.video.config;
    uint32_t size = v->mp4.video.configSize, pos = 5;
    for (int kind = 0; 2 > kind; kind++) {
        if (pos >= size) return false;
        uint32_t count = kind == 0 ? (c[pos] & 0x1F) : c[pos];
        pos++;
        for (uint32_t i = 0; count > i; i++) {
            if (pos + 2 > size) return false;
            uint32_t len = ((uint32_t) c[pos] << 8) | c[pos + 1];
            pos += 2;
            if (len > size - pos || len + 3 > v->auCap) return false;
            v->au[0] = 0;
            v->au[1] = 0;
            v->au[2] = 1;
            memcpy(v->au + 3, c + pos, len);
            pos += len;
            if (kind == 0 && v->spsLevel != 0 && len > 3) v->au[6] = v->spsLevel;
            flushCache(v->au, len + 3);
            trace("feeding the %s (%lu bytes, level byte %u)", kind == 0 ? "SPS" : "PPS", (unsigned long) len, kind == 0 && len > 3 ? v->au[6] : 0);
            Result r = mvdstdProcessVideoFrame(v->au, len + 3, 0, nullptr);
            trace("%s fed: 0x%08lX", kind == 0 ? "SPS" : "PPS", (unsigned long) r);
            post(v, "Video: %s %lu bytes: 0x%08lX\n", kind == 0 ? "SPS" : "PPS", (unsigned long) len, (unsigned long) r);
            if (!MVD_CHECKNALUPROC_SUCCESS(r)) return false;
        }
    }
    return true;
}

static double sampleSeconds(const CtrVideo* v, uint32_t index) {
    return (double) v->mp4.video.pts[index] / (double) v->mp4.video.timescale;
}

static void goPictureless(CtrVideo* v, const char* why) {
    post(v, "Video: %s; playing on without pictures\n", why);
    LightLock_Lock(&v->lock);
    v->mode = MODE_PICTURELESS;
    LightLock_Unlock(&v->lock);
}

// Feeds one sample (in v->au) and collects what comes out. False if the
// decoder failed.
static bool decodeSample(CtrVideo* v, uint32_t index, uint32_t size) {
    bool tracing = TRACE_SAMPLES > v->fed;
    double pts = sampleSeconds(v, index);
    pendingPush(v, pts);
    v->fed++;
    flushCache(v->au, size);
    uint32_t offset = 0;
    unsigned retries = 0;
    while (offset < size && !v->quit) {
        int32_t slot = acquireSlot(v);
        if (slot < 0) return true;
        FrameSlot* s = &v->slots[slot];
        armSlot(v, s);
        Result config = MVDSTD_SetConfig(&v->config);
        if (config != MVD_STATUS_OK) return false;
        MVDSTD_ProcessNALUnitOut out = { 0 };
        u64 t0 = svcGetSystemTick();
        Result r = mvdstdProcessVideoFrame(v->au + offset, size - offset, 0, &out);
        double ms = ticksToSeconds(svcGetSystemTick() - t0) * 1000.0;
        if (ms > v->processMaxMs) v->processMaxMs = ms;
        if (tracing) post(v, "Video: sample %lu at byte %lu (%lu bytes): process 0x%08lX in %.2f ms, %lu bytes remaining\n",
                          (unsigned long) index, (unsigned long) offset, (unsigned long) (size - offset),
                          (unsigned long) r, ms, (unsigned long) out.remaining_size);
        if (!MVD_CHECKNALUPROC_SUCCESS(r)) {
            v->errors++;
            post(v, "Video: sample %lu: the decoder said 0x%08lX\n", (unsigned long) index, (unsigned long) r);
            return false;
        }
        // ProcessNALUnit can stop before the end of an MP4 access unit
        // (for example after its SEI, before its IDR). Keep the tail: losing
        // that IDR leaves P pictures referring to an uninitialised picture
        // until the next keyframe. libctru does not loop over this for us.
        if (out.remaining_size > size - offset ||
            (r == MVD_STATUS_INCOMPLETEPROCESSING && out.remaining_size == 0)) {
            post(v, "Video: sample %lu: no progress processing its NAL units\n", (unsigned long) index);
            v->errors++;
            return false;
        }
        // Initialising the decoder can leave the whole NAL unconsumed.
        // Retry that same input, bounded so a broken decoder cannot spin.
        if (out.remaining_size == size - offset) {
            if (r != MVD_STATUS_INCOMPLETEPROCESSING || ++retries > 8) {
                v->errors++;
                return false;
            }
        } else retries = 0;
        // Only a completed process call with FRAMEREADY grants output.
        // In particular, an SEI's INCOMPLETEPROCESSING must keep its IDR tail.
        if (r == MVD_STATUS_FRAMEREADY) {
#ifdef CTRPORT_TARGET_N3DS
            // Process can deliver a reordered picture before the explicit
            // render delivers another. Preserve it first: rendering into the
            // same already-written slot both overwrites it and masks an empty
            // render. Each real output needs its own slot and pending PTS.
            if (slotWritten(v, s)) {
                publishSlot(v, slot, pts);
                // No outstanding picture means no second output to collect.
                // Continue consuming a possible NAL tail without rendering
                // the last picture twice (notably a no-reordering stream).
                if (v->pendingCount == 0) {
                    if (out.remaining_size == 0) break;
                    offset = size - out.remaining_size;
                    continue;
                }
                slot = acquireSlot(v);
                if (slot < 0) return true;
                s = &v->slots[slot];
                armSlot(v, s);
            }
#endif
            Result code = 0;
            RenderResult got = renderInto(v, s, &code);
            if (tracing) post(v, "Video: sample %lu: render 0x%08lX, %s\n", (unsigned long) index,
                              (unsigned long) code, got == RENDER_FRAME ? "completed picture" : "no completed picture");
#ifdef CTRPORT_TARGET_N3DS
            // OK with untouched output is normal while MVD is reordering.
            // It consumes no timestamp; retain the slot and keep feeding.
            // decodeMain bounds startup with MAX_SILENT_SAMPLES.
            if (got != RENDER_NONE)
#endif
            {
                if (got != RENDER_FRAME) {
                    v->errors++;
                    return false;
                }
                publishSlot(v, slot, pts);
            }
        }
        if (out.remaining_size == 0) break;
        offset = size - out.remaining_size;
    }
    return true;
}

// Fills the stack below the caller's frame (leaving 1 KB for the calls it
// makes) with STACK_PAINT; returns where the painted part starts.
static NOINLINE uint8_t* paintStack(void) {
    volatile uint8_t here = 0;
    uint8_t* top = (uint8_t*) (((uintptr_t) &here - 1024u) & ~(uintptr_t) 7u);
    uint8_t* low = top - STACK_PAINT_BYTES;
    memset(low, STACK_PAINT, STACK_PAINT_BYTES);
    return low;
}

// How many bytes of the painted part have been written since.
static uint32_t stackWritten(const uint8_t* low) {
    uint32_t untouched = 0;
    while (STACK_PAINT_BYTES > untouched && low[untouched] == STACK_PAINT) untouched++;
    return STACK_PAINT_BYTES - untouched;
}

static void VIDFIX_LATE decodeMain(void* arg) {
    CtrVideo* v = (CtrVideo*) arg;
    uint8_t* paintedStack = paintStack();
    const CtrMp4Track* t = &v->mp4.video;
    u64 busyTicks = 0, readTicks = 0;
    u64 t0 = svcGetSystemTick();
    bool ok = feedParameterSets(v);
    busyTicks += svcGetSystemTick() - t0;
    if (!ok) goPictureless(v, "the decoder took no SPS/PPS");
    uint32_t lastFrameAt = 0;
    for (uint32_t i = 0; ok && t->count > i && !v->quit; i++) {
        LightLock_Lock(&v->lock);
        double clock = v->sharedClock;
        bool started = v->sharedClockStarted && v->frameClockRunner == nullptr;
        LightLock_Unlock(&v->lock);
        // Far behind: on to the last keyframe that is due.
        if (started && sampleSeconds(v, i) < clock - 1.0) {
            uint32_t target = i;
            for (uint32_t j = i + 1; t->count > j && sampleSeconds(v, j) <= clock + 0.25; j++) {
                if (t->sync[j]) target = j;
            }
            if (target != i) {
                v->jumps++;
                i = target;
            }
        }
        u64 r0 = svcGetSystemTick();
        bool read = CtrMp4_readSample(&v->mp4, t, i, v->au);
        readTicks += svcGetSystemTick() - r0;
        if (!read) {
            goPictureless(v, "a sample could not be read");
            break;
        }
        bool disposable = false;
        uint32_t size = toStartCodes(v->au, t->sizes[i], &disposable, v->spsLevel);
        if (size == 0) continue;
        // Behind: frames nothing else refers to can go.
        if (disposable && started && sampleSeconds(v, i) < clock) {
            v->skippedRef++;
            continue;
        }
        u64 s0 = svcGetSystemTick(), waited = v->waitTicks;
        uint32_t before = v->decoded;
        ok = decodeSample(v, i, size);
        busyTicks += svcGetSystemTick() - s0 - (v->waitTicks - waited);
        if (!ok) goPictureless(v, "the decoder failed");
        if (v->fed % 240 == 0) {
            post(v, "Video: sample %lu at %.2f s (clock %.2f): %lu decoded of %lu fed, %lu skipped, decoder busy %.2f s, reads %.2f s\n",
                 (unsigned long) i, sampleSeconds(v, i), clock, (unsigned long) v->decoded, (unsigned long) v->fed,
                 (unsigned long) v->skippedRef, ticksToSeconds(busyTicks), ticksToSeconds(readTicks));
        }
        if (v->decoded != before) lastFrameAt = v->fed;
        else if (ok && v->fed - lastFrameAt >= MAX_SILENT_SAMPLES) {
            goPictureless(v, v->decoded == 0 ? "no frame came out of the decoder" : "the decoder stopped making frames");
            ok = false;
        }
    }
    // The frames still held for reordering: end the stream, then collect them.
    if (ok && !v->quit && v->mvdOpen) {
        static const uint8_t endOfStream[4] = { 0, 0, 1, 11 };
        memcpy(v->au, endOfStream, sizeof(endOfStream));
        flushCache(v->au, sizeof(endOfStream));
        uint32_t flushed = 0;
        while (v->pendingCount > 0 && !v->quit) {
            int32_t slot = acquireSlot(v);
            if (slot < 0) break;
            armSlot(v, &v->slots[slot]);
            if (MVDSTD_SetConfig(&v->config) != MVD_STATUS_OK ||
                mvdstdProcessVideoFrame(v->au, sizeof(endOfStream), 0, nullptr) != MVD_STATUS_FRAMEREADY) break;
            Result code = 0;
            FrameSlot* s = &v->slots[slot];
#ifdef CTRPORT_TARGET_N3DS
            bool fromProcess = slotWritten(v, s);
            if (fromProcess) {
                publishSlot(v, slot, 0.0);
                flushed++;
                if (v->pendingCount == 0) break;
                slot = acquireSlot(v);
                if (slot < 0) break;
                s = &v->slots[slot];
                armSlot(v, s);
            }
            RenderResult got = renderInto(v, s, &code);
            if (got == RENDER_NONE && fromProcess) continue;
            if (got != RENDER_FRAME) break;
#else
            if (renderInto(v, s, &code) != RENDER_FRAME) break;
#endif
            publishSlot(v, slot, 0.0);
            flushed++;
        }
        post(v, "Video: end of stream gave %lu more frames, %d never came out\n", (unsigned long) flushed, (int) v->pendingCount);
    }
    v->busySeconds = ticksToSeconds(busyTicks);
    v->readSeconds = ticksToSeconds(readTicks);
    LightLock_Lock(&v->lock);
    if (v->heldSlot >= 0) v->slots[v->heldSlot].state = SLOT_FREE;
    v->heldSlot = -1;
    v->decodeDone = true;
    LightLock_Unlock(&v->lock);
    v->stackUsed = stackWritten(paintedStack);
}

// ===[ Starting the decoder ]===

typedef struct {
    uint8_t bytes[64];
    uint32_t size, bit;
    bool bad;
} BitReader;

static uint32_t readBit(BitReader* b) {
    if (b->bit >= b->size * 8u) {
        b->bad = true;
        return 0;
    }
    uint32_t v = (b->bytes[b->bit >> 3] >> (7u - (b->bit & 7u))) & 1u;
    b->bit++;
    return v;
}

static uint32_t readBits(BitReader* b, uint32_t n) {
    uint32_t v = 0;
    while (n-- > 0) v = (v << 1) | readBit(b);
    return v;
}

static uint32_t readUe(BitReader* b) {
    uint32_t zeros = 0;
    while (!b->bad && readBit(b) == 0) {
        if (++zeros > 31) {
            b->bad = true;
            return 0;
        }
    }
    return ((1u << zeros) - 1u) + readBits(b, zeros);
}

// max_num_ref_frames from the first SPS in an avcC box. False if it can't be
// read (scaling matrices, which the games' videos don't have, aren't parsed).
static bool spsReferenceFrames(const uint8_t* config, uint32_t configSize, uint32_t* refs) {
    if (configSize < 8 || (config[5] & 0x1F) == 0) return false;
    uint32_t len = ((uint32_t) config[6] << 8) | config[7];
    if (len < 4 || 8 + len > configSize) return false;
    BitReader b = { .size = 0, .bit = 0, .bad = false };
    // Without the NAL header and the emulation prevention bytes.
    uint32_t zeros = 0;
    for (uint32_t i = 1; len > i && sizeof(b.bytes) > b.size; i++) {
        uint8_t c = config[8 + i];
        if (zeros >= 2 && c == 3) {
            zeros = 0;
            continue;
        }
        zeros = c == 0 ? zeros + 1 : 0;
        b.bytes[b.size++] = c;
    }
    uint32_t profile = readBits(&b, 8);
    readBits(&b, 16); // constraint flags, level
    readUe(&b);       // seq_parameter_set_id
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 || profile == 86 || profile == 118 ||
        profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135) {
        if (readUe(&b) == 3) readBit(&b); // chroma_format_idc, separate_colour_plane_flag
        readUe(&b);                        // bit_depth_luma_minus8
        readUe(&b);                        // bit_depth_chroma_minus8
        readBit(&b);                       // qpprime_y_zero_transform_bypass_flag
        if (readBit(&b)) return false;     // seq_scaling_matrix_present_flag
    }
    readUe(&b); // log2_max_frame_num_minus4
    uint32_t pocType = readUe(&b);
    if (pocType == 0) {
        readUe(&b); // log2_max_pic_order_cnt_lsb_minus4
    } else if (pocType == 1) {
        readBit(&b); // delta_pic_order_always_zero_flag
        readUe(&b);  // offset_for_non_ref_pic (se, same length)
        readUe(&b);  // offset_for_top_to_bottom_field
        uint32_t cycle = readUe(&b);
        for (uint32_t i = 0; cycle > i && 256 > i; i++) readUe(&b);
    }
    *refs = readUe(&b);
    return !b.bad;
}

static u8 mvdLevel(uint8_t level) {
    switch (level) {
        case 9: return MVD_H264_LEVEL_1_0B;
        case 10: return MVD_H264_LEVEL_1_0;
        case 11: return MVD_H264_LEVEL_1_1;
        case 12: return MVD_H264_LEVEL_1_2;
        case 13: return MVD_H264_LEVEL_1_3;
        case 20: return MVD_H264_LEVEL_2_0;
        case 21: return MVD_H264_LEVEL_2_1;
        case 22: return MVD_H264_LEVEL_2_2;
        case 30: return MVD_H264_LEVEL_3_0;
        case 31: return MVD_H264_LEVEL_3_1;
        case 32: return MVD_H264_LEVEL_3_2;
        case 40: return MVD_H264_LEVEL_4_0;
        case 41: return MVD_H264_LEVEL_4_1;
        case 42: return MVD_H264_LEVEL_4_2;
        case 50: return MVD_H264_LEVEL_5_0;
        case 51: return MVD_H264_LEVEL_5_1;
        default: return MVD_H264_LEVEL_5_2;
    }
}

static void freeBuffers(CtrVideo* v) {
    for (int32_t i = 0; MAX_SLOTS > i; i++) {
        // ctr_memory.c holds the free back while the GPU may still read it.
        if (v->slots[i].pixels != nullptr) linearFree(v->slots[i].pixels);
        v->slots[i].pixels = nullptr;
        v->slots[i].state = SLOT_FREE;
    }
    v->slotCount = 0;
    if (v->au != nullptr) linearFree(v->au);
    v->au = nullptr;
    v->auCap = 0;
#ifdef CTRPORT_SW_DECODE
    if (v->stage != nullptr) linearFree(v->stage);
    v->stage = nullptr;
#endif
}

// Opens the hardware decoder and its buffers. False (with nothing held) if
// there is none or it can't take this video.
static bool startDecoder(CtrVideo* v) {
    const CtrMp4Track* t = &v->mp4.video;
    bool isNew3ds = false;
    APT_CheckNew3DS(&isNew3ds);
    if (!isNew3ds) return false;
    // The decoder's system module only works with linear memory in the
    // 0x30000000 mapping, where a .3dsx's is and where the n3ds .cia's is
    // with its exheader's kernel 2.44 (platform/3ds/cia/n3ds.rsf). An older
    // exheader gets the 0x14000000 mapping, and the module brought the console
    // down on the first SPS either way: given the 0x14000000 address it read
    // it in its own space and faulted (Luma dump: a data abort in "mvd" at
    // far 1527C200), given the same memory's 0x30000000 address it hit a
    // svcBreak (a "kernel panic"). Such a build decodes in software instead.
    extern u32 __ctru_linear_heap;
    if (__ctru_linear_heap < 0x30000000u) {
        logInfo("Video: linear memory is in the old mapping (0x%08lX), which the hardware decoder can't take, so no hardware decoding\n",
                (unsigned long) __ctru_linear_heap);
        return false;
    }
    if (t->codec != CTR_MP4_FOURCC('a', 'v', 'c', '1') || t->configSize < 7 || t->count == 0) {
        logInfo("Video: not H.264 in an avcC box, so no hardware decoding\n");
        return false;
    }
    v->nalLengthSize = (t->config[4] & 3u) + 1u;
    if (v->nalLengthSize != 4) {
        logInfo("Video: %lu-byte NAL lengths aren't handled, so no hardware decoding\n", (unsigned long) v->nalLengthSize);
        return false;
    }
    // Asking for a service that isn't there waits for it forever (Azahar has
    // no decoder), so it is first asked for without waiting.
    Handle probe = 0;
    srvSetBlockingPolicy(true);
    Result reg = srvGetServiceHandle(&probe, "mvd:STD");
    srvSetBlockingPolicy(false);
    if (R_FAILED(reg)) {
        logInfo("Video: no hardware decoder service (0x%08lX)\n", (unsigned long) reg);
        return false;
    }
    svcCloseHandle(probe);
    // The decoder is only known to work up to level 3.2 (3dbrew's MVD page
    // lists 4 and up as untested). Chapter 5's 720p video is level 4.0, and
    // the decoder's system module crashed on it, taking the console down
    // (Luma dumps: a data abort in "mvd", a memset of a null pointer). So a
    // stream past 3.2 plays without pictures; with LEVEL_TRY_FILE one whose
    // frames and references fit level 3.2 is given to the decoder as level
    // 3.2: its SPS says so and the work buffer is sized for that.
    uint8_t level = t->config[3];
    if (level > 32) {
        FILE* f = fopen(LEVEL_TRY_FILE, "rb");
        if (f == nullptr) {
            logInfo("Video: level %u.%u is past what the hardware decoder takes (it crashed on Chapter 5's video), so no pictures\n",
                    level / 10u, level % 10u);
            v->keptOffHardware = true;
            return false;
        }
        fclose(f);
        uint32_t refs = 0;
        uint32_t frameMbs = (v->codedW / 16u) * (v->codedH / 16u);
        if (!spsReferenceFrames(t->config, t->configSize, &refs) || frameMbs > 5120u || frameMbs * (refs > 0 ? refs : 1u) > 20480u) {
            logInfo("Video: level %u.%u with %lu reference frames is past what the hardware decoder is known to take\n", level / 10u, level % 10u,
                    (unsigned long) refs);
            v->keptOffHardware = true;
            return false;
        }
        logInfo("Video: level %u.%u stream (%lu reference frames of %lu macroblocks) given to the decoder as level 3.2\n", level / 10u,
                level % 10u, (unsigned long) refs, (unsigned long) frameMbs);
        v->spsLevel = 32;
        level = 32;
    }
    MVDSTD_CalculateWorkBufSizeConfig calc;
    memset(&calc, 0, sizeof(calc));
    calc.level.enable = 1;
    calc.level.flag = MVD_CALC_WITH_LEVEL_FLAG_ENABLE_CALC | MVD_CALC_WITH_LEVEL_FLAG_ENABLE_EXTRA_OP | MVD_CALC_WITH_LEVEL_FLAG_UNK;
    calc.level.level = mvdLevel(level);
    calc.width = v->codedW;
    calc.height = v->codedH;
    u32 work = MVD_DEFAULT_WORKBUF_SIZE;
    trace("sizing the work buffer (%lux%lu, level %u)", (unsigned long) v->codedW, (unsigned long) v->codedH, level);
    Result r = mvdstdCalculateBufferSize(&calc, &work);
    trace("work buffer %lu bytes (0x%08lX); opening the decoder", (unsigned long) work, (unsigned long) r);
    if (R_FAILED(r) || work == 0) {
        logInfo("Video: the decoder couldn't size its work buffer (0x%08lX)\n", (unsigned long) r);
        return false;
    }
    r = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_BGR565, work, nullptr);
    if (r != 0) {
        logInfo("Video: no hardware decoder (mvdstdInit 0x%08lX, %.1f MB work buffer, %.1f MB linear free)\n", (unsigned long) r,
                (double) work / (1024.0 * 1024.0), (double) linearSpaceFree() / (1024.0 * 1024.0));
        return false;
    }
    v->mvdOpen = true;
    v->workBytes = work;
    logInfo("Video: hardware decoder open (%lux%lu coded, %.1f MB work buffer, %.1f MB linear free)\n", (unsigned long) v->codedW,
            (unsigned long) v->codedH, (double) work / (1024.0 * 1024.0), (double) linearSpaceFree() / (1024.0 * 1024.0));
    size_t frameBytes = (size_t) v->codedW * v->codedH * 2u;
    for (int32_t i = 0; FEW_SLOTS > i; i++) {
        v->slots[i].pixels = (uint16_t*) linearAlloc(frameBytes);
        if (v->slots[i].pixels == nullptr) break;
        v->slots[i].state = SLOT_FREE;
        v->slotCount++;
    }
    v->auCap = t->maxSize + 16;
    v->au = (uint8_t*) linearAlloc(v->auCap);
    if (v->slotCount < MIN_SLOTS || v->au == nullptr) {
        logInfo("Video: no linear memory for the decoder's frames (%d of %d, %.1f MB free)\n", (int) v->slotCount, FEW_SLOTS,
                (double) linearSpaceFree() / (1024.0 * 1024.0));
        freeBuffers(v);
        mvdstdExit();
        v->mvdOpen = false;
        return false;
    }
    mvdstdGenerateDefaultConfig(&v->config, v->codedW, v->codedH, v->codedW, v->codedH, nullptr, (u32*) v->slots[0].pixels,
                                (u32*) v->slots[0].pixels);
    trace("starting the decode thread");
    v->heldSlot = -1;
    v->pendingCount = 0;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    priority = priority + 2 > 0x3F ? 0x3F : priority + 2;
    v->thread = threadCreate(decodeMain, v, DECODE_STACK_BYTES, priority, 2, false);
    if (v->thread == nullptr) v->thread = threadCreate(decodeMain, v, DECODE_STACK_BYTES, priority, -2, false);
    if (v->thread == nullptr) {
        logInfo("Video: no decode thread\n");
        freeBuffers(v);
        mvdstdExit();
        v->mvdOpen = false;
        return false;
    }
    logInfo("Video: hardware decoding, %.1f MB work buffer, %d frames of %.1f MB, %.1f MB linear left\n", (double) work / (1024.0 * 1024.0),
            (int) v->slotCount, (double) frameBytes / (1024.0 * 1024.0), (double) linearSpaceFree() / (1024.0 * 1024.0));
    return true;
}

static void stopDecoder(CtrVideo* v) {
    bool hadThread = v->thread != nullptr;
    if (v->thread != nullptr) {
        v->quit = true;
        LightEvent_Signal(&v->wake);
        threadJoin(v->thread, U64_MAX);
        threadFree(v->thread);
        v->thread = nullptr;
    }
    if (v->mvdOpen) {
        u64 t0 = svcGetSystemTick();
        mvdstdExit();
        double exitMs = ticksToSeconds(svcGetSystemTick() - t0) * 1000.0;
        logInfo("Video: hardware decoder closed in %.1f ms (decode thread used %.1f of %d KB of stack)\n", exitMs,
                (double) v->stackUsed / 1024.0, DECODE_STACK_BYTES / 1024);
    } else if (hadThread) {
        logInfo("Video: decode thread used %.1f of %d KB of stack\n", (double) v->stackUsed / 1024.0, DECODE_STACK_BYTES / 1024);
    }
    v->mvdOpen = false;
#ifdef CTRPORT_SW_DECODE
    if (v->y2rOpen) CtrSwConvert_close();
    v->y2rOpen = false;
#endif
    freeBuffers(v);
}

// ===[ Clock ]===

static void tickClock(CtrVideo* v) {
    if (v->frameClockRunner != nullptr) {
        Runner* runner = v->frameClockRunner;
        int32_t frame = runner->frameCount;
        double speed = Runner_getEffectiveGameSpeed(runner);
        double dt = frame > v->lastFrame && speed > 0.0 ? (double) (frame - v->lastFrame) / speed : 0.0;
        v->lastFrame = frame;
        if (!v->resumed || v->paused || v->suspended) return;
        // Start at resume, independent of the DSP, stream startup and the
        // decode thread. Repeated queries in one game frame add no time.
        v->clockStarted = true;
        v->clock += dt;
        return;
    }
    u64 now = svcGetSystemTick();
    double dt = ticksToSeconds(now - v->lastTick);
    v->lastTick = now;
    if (!v->resumed || v->paused || v->suspended) return;
    if (v->clockStarted) {
        v->clock += dt;
        return;
    }
    v->waited += dt;
    LightLock_Lock(&v->lock);
    bool frameReady = v->mode == MODE_PICTURELESS;
    for (int32_t i = 0; v->slotCount > i && !frameReady; i++) frameReady = v->slots[i].state == SLOT_READY;
    LightLock_Unlock(&v->lock);
    // With audio, the audio starts it (setMasterClock); without, the first
    // frame does.
    if ((!v->hasAudio && frameReady) || v->waited >= START_TIMEOUT) {
        if (v->waited >= START_TIMEOUT) logInfo("Video: started on the wall clock after %.1f s (%s)\n", v->waited, v->hasAudio ? "no audio" : "no frame");
        v->clockStarted = true;
    }
}

static void publishClock(CtrVideo* v) {
    LightLock_Lock(&v->lock);
    v->sharedClock = v->clock;
    v->sharedClockStarted = v->clockStarted;
    LightLock_Unlock(&v->lock);
}

static void onVideoAptEvent(APT_HookType hook, void* param) {
    CtrVideo* v = param;
    unsigned bit = hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONRESTORE ? 1u : 2u;
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) {
        tickClock(v);
        v->suspended |= bit;
        publishClock(v);
        logInfo("Video: suspend at %.3f s (mask %u)\n", v->clock, v->suspended);
    } else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) {
        // Reset the anchor before any position/isRunning call can see the
        // system tick delta. Keep video_pause and an overlapping HOME or
        // sleep suspension intact.
        v->lastTick = svcGetSystemTick();
        if (v->frameClockRunner != nullptr) v->lastFrame = v->frameClockRunner->frameCount;
        v->suspended &= ~bit;
        logInfo("Video: restore at %.3f s (mask %u)\n", v->clock, v->suspended);
    }
}

void CtrVideo_debugSuspend(Runner* runner, unsigned seconds) {
    CtrVideo* v = activeVideo;
    if (v == nullptr || !v->opened) {
        logInfo("Video: fake suspend skipped (no open video)\n");
        return;
    }
    onVideoAptEvent(APTHOOK_ONSUSPEND, v);
    if (runner->audioSystem != nullptr) runner->audioSystem->vtable->suspend(runner->audioSystem);
    svcSleepThread((s64) seconds * 1000000000LL);
    onVideoAptEvent(APTHOOK_ONRESTORE, v);
    if (runner->audioSystem != nullptr) runner->audioSystem->vtable->resume(runner->audioSystem);
    tickClock(v);
    logInfo("Video: fake %u s suspend complete, position %.3f s\n", seconds, v->clock);
}

// ===[ Vtable ]===

static bool ctrVideoInit(void) { return true; }

static void ctrVideoQuit(MAYBE_UNUSED VideoDecoder* decoder) {}

#ifdef CTRPORT_SW_DECODE
// ===[ Software decoding measurement ]===
// For the Old 3DS evaluation: without the hardware decoder, a thread decodes
// in software as fast as it can and logs how long each kind of frame takes,
// and Y2R's conversion of each picture. The pictures are thrown away; the
// video plays without them. It runs when SW_CHOICE_FILE says "bench".
// - Transcoded copies of the video next to it, <name>.tx-<anything>.mp4 for
//   <name>.mp4, are measured in its place, in name order: the files a PC
//   transcode would give the Old 3DS, smaller and cheaper to decode.
// - Without any, the video itself, in the ways SW_CHOICE_FILE lists (the
//   names in swModes), or every frame.
// With more than one to measure, each gets a share of SW_BENCH_SECONDS from
// the start of its video, so the one run measures them all. "heavy" in
// SW_CHOICE_FILE starts each instead from the keyframe before its heaviest
// SW_HEAVY_SECONDS (by the samples' sizes), the stretch hardest to keep up with.
#define SW_STACK_BYTES (64 * 1024)
#define SW_CHOICE_FILE CTRPORT_GAME_DIR "/videosw.txt"
#define SW_BENCH_SECONDS 20.0 // about how long a bench scene stays with its video
#define SW_TRANSCODES_MAX 6
#define SW_HEAVY_SECONDS 6.0

typedef struct {
    const char* name;
    bool referenceOnly; // only reference frames fed: the reduced picture rate an Old 3DS could show
} SwMode;

static const SwMode swModes[] = {
    { "openh264", false },
    { "openh264-ref", true },
};
#define SW_MODE_COUNT ((int32_t) (sizeof(swModes) / sizeof(swModes[0])))
#define SW_DEFAULT_MODES 1 // every frame

// Whether a track is H.264 with 4-byte NAL lengths, as the decoders take it.
static bool isAvc(const CtrMp4Track* t) {
    return t->codec == CTR_MP4_FOURCC('a', 'v', 'c', '1') && t->configSize >= 7 && ((t->config[4] & 3u) + 1u) == 4;
}

// The SPS and PPS from the avcC box, as Annex B, through au.
static void swFeedParameterSets(const CtrMp4Track* t, uint8_t* au, uint32_t auCap, CtrSwDecoder* d) {
    const uint8_t* c = t->config;
    uint32_t size = t->configSize, pos = 5;
    for (int kind = 0; 2 > kind && size > pos; kind++) {
        uint32_t count = kind == 0 ? (c[pos] & 0x1F) : c[pos];
        pos++;
        for (uint32_t i = 0; count > i && size >= pos + 2; i++) {
            uint32_t len = ((uint32_t) c[pos] << 8) | c[pos + 1];
            pos += 2;
            if (len > size - pos || len + 3 > auCap) return;
            au[0] = 0;
            au[1] = 0;
            au[2] = 1;
            memcpy(au + 3, c + pos, len);
            pos += len;
            CtrSwPicture none;
            CtrSwDecoder_decode(d, au, len + 3, &none);
        }
    }
}

typedef struct {
    u64 ticks;
    uint32_t count;
} SwTally;

static double tallyMs(const SwTally* s) { return s->count > 0 ? ticksToSeconds(s->ticks) * 1000.0 / s->count : 0.0; }

static void tally(SwTally* s, u64 ticks) {
    s->ticks += ticks;
    s->count++;
}

// The keyframe before the stretch of `seconds` with the most bytes.
static uint32_t heaviestStart(const CtrMp4Track* t, double seconds) {
    uint32_t n = t->duration > 0.0 ? (uint32_t) (seconds * (double) t->count / t->duration) : 0;
    if (n == 0 || n >= t->count) return 0;
    uint64_t sum = 0, best = 0;
    uint32_t bestAt = 0;
    for (uint32_t i = 0; t->count > i; i++) {
        sum += t->sizes[i];
        if (i >= n) sum -= t->sizes[i - n];
        if (i + 1 >= n && sum > best) {
            best = sum;
            bestAt = i + 1 - n;
        }
    }
    while (bestAt > 0 && t->sync != nullptr && t->sync[bestAt] == 0) bestAt--;
    return bestAt;
}

// Decodes mp4's video from sample `first` (a keyframe) for up to
// budgetSeconds, and logs the times under label. When Y2R is open it converts
// each picture as playback does (swDeliver): a copy, while the next picture
// decodes.
static void swMeasure(CtrVideo* v, CtrMp4* mp4, const char* label, bool referenceOnly, double budgetSeconds, uint32_t first) {
    const CtrMp4Track* t = &mp4->video;
    if (!isAvc(t)) {
        post(v, "Video: bench %s: not H.264 with 4-byte lengths\n", label);
        return;
    }
    uint32_t auCap = t->maxSize + 16, codedW = (t->width + 15u) & ~15u, codedH = (t->height + 15u) & ~15u;
    uint8_t* au = (uint8_t*) linearAlloc(auCap);
    uint16_t* rgb = v->y2rOpen ? (uint16_t*) linearAlloc((size_t) codedW * codedH * 2u) : nullptr;
    uint8_t* stage = rgb != nullptr ? (uint8_t*) linearAlloc((size_t) codedW * codedH * 3u / 2u) : nullptr;
    if (stage == nullptr && rgb != nullptr) {
        linearFree(rgb);
        rgb = nullptr;
    }
    size_t heapBefore = mallinfo().uordblks;
    u64 o0 = svcGetSystemTick();
    CtrSwDecoder* d = au != nullptr ? CtrSwDecoder_open() : nullptr;
    double openMs = ticksToSeconds(svcGetSystemTick() - o0) * 1000.0;
    if (d == nullptr) {
        post(v, "Video: bench %s: the decoder didn't open (or no memory)\n", label);
        if (au != nullptr) linearFree(au);
        if (rgb != nullptr) linearFree(rgb);
        if (stage != nullptr) linearFree(stage);
        return;
    }
    swFeedParameterSets(t, au, auCap, d);
    SwTally key = { 0, 0 }, ref = { 0, 0 }, nonref = { 0, 0 }, copy = { 0, 0 }, wait = { 0, 0 };
    u64 readTicks = 0, maxTicks = 0, maxWaitTicks = 0, start = svcGetSystemTick();
    uint32_t pictures = 0, errors = 0, skipped = 0, convertFailures = 0, i = first < t->count ? first : 0;
    uint32_t from = i;
    bool converting = false;
    for (; t->count > i && !v->quit && budgetSeconds > ticksToSeconds(svcGetSystemTick() - start); i++) {
        u64 r0 = svcGetSystemTick();
        bool read = CtrMp4_readSample(mp4, t, i, au);
        readTicks += svcGetSystemTick() - r0;
        if (!read) break;
        bool disposable = false;
        uint32_t n = toStartCodes(au, t->sizes[i], &disposable, 0);
        if (n == 0) continue;
        if (disposable && referenceOnly) {
            skipped++;
            continue;
        }
        CtrSwPicture picture;
        u64 d0 = svcGetSystemTick();
        int32_t got = CtrSwDecoder_decode(d, au, n, &picture);
        u64 dt = svcGetSystemTick() - d0;
        if (dt > maxTicks) maxTicks = dt;
        tally(disposable ? &nonref : (t->sync == nullptr || t->sync[i] != 0) ? &key : &ref, dt);
        if (got < 0 && ++errors <= 3) {
            post(v, "Video: bench %s: error %ld at sample %lu (%s, %lu bytes)\n", label, (long) CtrSwDecoder_lastError(d), (unsigned long) i,
                 disposable ? "non-reference" : "reference", (unsigned long) n);
        }
        if (got != 1) continue;
        if (++pictures == 1) {
            post(v, "Video: bench %s: opened in %.1f ms; first picture %ldx%ld, strides %ld/%ld, decoder heap %.2f MB\n", label, openMs,
                 (long) picture.width, (long) picture.height, (long) picture.strides[0], (long) picture.strides[1],
                 (double) (mallinfo().uordblks - heapBefore) / (1024.0 * 1024.0));
        }
        if (rgb == nullptr) continue;
        if (converting) {
            u64 w0 = svcGetSystemTick();
            if (!CtrSwConvert_finish()) convertFailures++;
            u64 waited = svcGetSystemTick() - w0;
            tally(&wait, waited);
            if (waited > maxWaitTicks) maxWaitTicks = waited;
        }
        int32_t w = picture.width < (int32_t) t->width ? picture.width : (int32_t) t->width;
        int32_t h = picture.height < (int32_t) t->height ? picture.height : (int32_t) t->height;
        u64 c0 = svcGetSystemTick();
        CtrSwPicture staged;
        CtrSwPicture_copy(&picture, w, h, stage, (int32_t) codedW, (int32_t) codedH, &staged);
        converting = CtrSwConvert_start(&staged, w, h, rgb, (int32_t) codedW, t->height >= 720);
        if (!converting) convertFailures++;
        tally(&copy, svcGetSystemTick() - c0);
    }
    if (converting) {
        u64 w0 = svcGetSystemTick();
        if (!CtrSwConvert_finish()) convertFailures++;
        tally(&wait, svcGetSystemTick() - w0);
    }
    double wall = ticksToSeconds(svcGetSystemTick() - start);
    uint32_t frames = key.count + ref.count + nonref.count;
    double decodeSeconds = ticksToSeconds(key.ticks + ref.ticks + nonref.ticks);
    double fpsNeeded = t->duration > 0.0 ? (double) t->count / t->duration : 0.0;
    post(v, "Video: bench %s: %lux%lu, keyframes %.1f ms (%lu), other reference %.1f ms (%lu), non-reference %.1f ms (%lu), "
            "copy+Y2R start %.1f ms\n",
         label, (unsigned long) t->width, (unsigned long) t->height, tallyMs(&key), (unsigned long) key.count, tallyMs(&ref),
         (unsigned long) ref.count, tallyMs(&nonref), (unsigned long) nonref.count, tallyMs(&copy));
    post(v, "Video: bench %s: %.1f fps by the wall clock (%.1f decoding alone) of %.2f needed; Y2R waits %.1f ms (max %.1f, %lu failed)\n",
         label, wall > 0.0 ? (double) frames / wall : 0.0, frames > 0 ? (double) frames / decodeSeconds : 0.0, fpsNeeded, tallyMs(&wait),
         ticksToSeconds(maxWaitTicks) * 1000.0, (unsigned long) convertFailures);
    post(v, "Video: bench %s: slowest decode %.1f ms, %lu errors\n", label, ticksToSeconds(maxTicks) * 1000.0, (unsigned long) errors);
    double perSample = t->count > 0 ? t->duration / (double) t->count : 0.0;
    post(v, "Video: bench %s: %lu samples from %.1f s (%.1f s of video, %lu skipped) in %.1f s, %lu pictures, reads %.2f s\n", label,
         (unsigned long) (i - from), (double) from * perSample, (double) (i - from) * perSample, (unsigned long) skipped, wall,
         (unsigned long) pictures, ticksToSeconds(readTicks));
    CtrSwDecoder_close(d);
    linearFree(au);
    if (rgb != nullptr) linearFree(rgb);
    if (stage != nullptr) linearFree(stage);
}

// The modes SW_CHOICE_FILE names, in its order, or the default ones.
static int32_t swChoices(const SwMode* out[SW_MODE_COUNT]) {
    int32_t count = 0;
    FILE* f = fopen(SW_CHOICE_FILE, "rb");
    if (f != nullptr) {
        char text[128];
        size_t n = fread(text, 1, sizeof(text) - 1, f);
        fclose(f);
        text[n] = '\0';
        for (char* word = strtok(text, " ,\t\r\n"); word != nullptr && SW_MODE_COUNT > count; word = strtok(nullptr, " ,\t\r\n")) {
            for (int32_t m = 0; SW_MODE_COUNT > m; m++) {
                if (strcmp(word, swModes[m].name) == 0) out[count++] = &swModes[m];
            }
        }
        if (count > 0) return count;
    }
    for (int32_t m = 0; SW_DEFAULT_MODES > m; m++) out[count++] = &swModes[m];
    return count;
}

// Whether SW_CHOICE_FILE has the word (strtok_r: the decode thread asks too).
static bool swChoiceHas(const char* wanted) {
    FILE* f = fopen(SW_CHOICE_FILE, "rb");
    if (f == nullptr) return false;
    char text[128];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    char* rest = nullptr;
    for (char* word = strtok_r(text, " ,\t\r\n", &rest); word != nullptr; word = strtok_r(nullptr, " ,\t\r\n", &rest)) {
        if (strcmp(word, wanted) == 0) return true;
    }
    return false;
}

static int compareNames(const void* a, const void* b) { return strcmp((const char*) a, (const char*) b); }

// The transcoded copies next to the video at path (<name>.tx-*.mp4 for
// <name>.mp4), as full paths in name order.
static int32_t swTranscodes(const char* path, char out[][VIDEO_PATH_MAX], int32_t max) {
    const char* slash = strrchr(path, '/');
    if (slash == nullptr) return 0;
    char dir[VIDEO_PATH_MAX], prefix[96];
    size_t dirLen = (size_t) (slash - path);
    if (dirLen + 1 > sizeof(dir)) return 0;
    memcpy(dir, path, dirLen);
    dir[dirLen] = '\0';
    const char* base = slash + 1;
    const char* dot = strrchr(base, '.');
    int stemLen = dot != nullptr ? (int) (dot - base) : (int) strlen(base);
    if (snprintf(prefix, sizeof(prefix), "%.*s.tx-", stemLen, base) >= (int) sizeof(prefix)) return 0;
    size_t prefixLen = strlen(prefix);
    DIR* d = opendir(dir);
    if (d == nullptr) return 0;
    int32_t count = 0;
    for (struct dirent* e = readdir(d); e != nullptr && max > count; e = readdir(d)) {
        size_t len = strlen(e->d_name);
        if (prefixLen + 4 >= len || strncmp(e->d_name, prefix, prefixLen) != 0 || strcmp(e->d_name + len - 4, ".mp4") != 0) continue;
        if (snprintf(out[count], VIDEO_PATH_MAX, "%s/%s", dir, e->d_name) < VIDEO_PATH_MAX) count++;
    }
    closedir(d);
    qsort(out, (size_t) count, VIDEO_PATH_MAX, compareNames);
    return count;
}

static void swBenchMain(void* arg) {
    CtrVideo* v = (CtrVideo*) arg;
    uint8_t* paintedStack = paintStack();
    char transcodes[SW_TRANSCODES_MAX][VIDEO_PATH_MAX];
    int32_t count = swTranscodes(v->path, transcodes, SW_TRANSCODES_MAX);
    double fps = v->mp4.video.count > 0 ? (double) v->mp4.video.count / v->duration : 0.0;
    bool heavy = swChoiceHas("heavy");
    if (count > 0) {
        post(v, "Video: measuring %ld transcoded cop%s of %s, %.0f s each from %s (Y2R %s)\n", (long) count, count > 1 ? "ies" : "y", v->name,
             count > 1 ? SW_BENCH_SECONDS / count : v->duration, heavy ? "the heaviest part" : "the start", v->y2rOpen ? "too" : "unavailable");
        for (int32_t k = 0; count > k && !v->quit; k++) {
            // The label: "tx-..." without ".mp4".
            const char* tx = strstr(strrchr(transcodes[k], '/'), ".tx-") + 1;
            char label[48];
            snprintf(label, sizeof(label), "%.*s", (int) (strlen(tx) - 4), tx);
            CtrMp4 mp4;
            memset(&mp4, 0, sizeof(mp4));
            if (!CtrMp4_open(&mp4, transcodes[k], VIDEO_WINDOW_BYTES)) {
                post(v, "Video: bench %s: not an MP4 file\n", label);
                continue;
            }
            swMeasure(v, &mp4, label, false, count > 1 ? SW_BENCH_SECONDS / count : 1e9, heavy ? heaviestStart(&mp4.video, SW_HEAVY_SECONDS) : 0);
            CtrMp4_close(&mp4);
        }
    } else {
        const SwMode* modes[SW_MODE_COUNT];
        count = swChoices(modes);
        post(v, "Video: measuring %ld way(s) of software decoding, %s each (Y2R %s); the video needs %.1f fps\n", (long) count,
             count > 1 ? "a share of 20 s" : "the whole video", v->y2rOpen ? "too" : "unavailable", fps);
        for (int32_t k = 0; count > k && !v->quit; k++) {
            swMeasure(v, &v->mp4, modes[k]->name, modes[k]->referenceOnly, count > 1 ? SW_BENCH_SECONDS / count : 1e9,
                      heavy ? heaviestStart(&v->mp4.video, SW_HEAVY_SECONDS) : 0);
        }
    }
    post(v, "Video: software measurement done (stack %.1f of %d KB)\n", (double) stackWritten(paintedStack) / 1024.0, SW_STACK_BYTES / 1024);
}

static bool startSwBench(CtrVideo* v) {
    v->y2rOpen = CtrSwConvert_open();
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    priority = priority + 2 > 0x3F ? 0x3F : priority + 2;
    v->thread = threadCreate(swBenchMain, v, SW_STACK_BYTES, priority, -2, false);
    if (v->thread == nullptr) {
        if (v->y2rOpen) CtrSwConvert_close();
        v->y2rOpen = false;
        return false;
    }
    logInfo("Video: measuring software decoding on this console (no pictures)\n");
    return true;
}
#endif

#ifdef CTRPORT_SW_DECODE
// ===[ Software decoding ]===
// Where there is no hardware decoder (an Old 3DS), a thread decodes the
// samples in software, has Y2R convert each picture into a slot as RGB565,
// and hands the slots to the game as the hardware path does, so the game
// thread never waits for it.
//
// An Old 3DS can't decode the games' videos at their size and rate, so it
// plays one of two things:
// - the copy of the video made for it on a PC (COPY_SUFFIX, see swOpenCopy):
//   smaller pictures, encoded to be cheap to decode, every one of them. The
//   renderer scales them up to the video's size, and the audio still comes
//   from the video itself;
// - without a copy, the video's keyframes as a slideshow, each shown at its
//   time (they are decoded ahead) while the audio plays in full.
// How much of a copy it decodes still follows from whether decoding keeps
// up on this console, so a copy too heavy for it doesn't fall behind:
// - every frame, while that keeps up;
// - reference frames only, each picture held until the next one: the frames
//   nothing refers to are skipped, so nothing else breaks. Only for a stream
//   that has such frames (the copies have none: it would save nothing);
// - keyframes only, a slideshow, when even that doesn't keep up. From there
//   it tries the tier above again after a while (SW_PROBE_HOLD, longer each
//   time a try fails), as the decoder may only have been slow for a while.
// Whether a tier keeps up is judged over stretches of SW_BLOCK_SAMPLES
// samples by the time they took, the game thread's share of the CPU (it runs
// above this thread) and Y2R included, and slot waits (decoding ahead)
// left out. It steps down only when SW_BLOCKS_OVER stretches in a row took
// longer than the video's time for them and the pictures are behind the
// clock as well: on an Old 3DS one stretch of Chapter 3's copy took 132% of
// its time in the middle of the video, where the rest took 63-73%, and that
// alone cost 3 s of keyframes only. The video's first SW_WARMUP_SECONDS
// don't count: there the game's own start-up work made the first second's
// frames take half as long again as the rest.
// SW_CHOICE_FILE can fix the tier ("full", "ref" or "key"), for the video
// itself too when there is no copy; "bench" in it runs the measurement above
// instead. To check the tiers in an emulator, "slowstart" in it slows the
// decoding of the video's first SW_SLOW_START seconds (the first frame by
// 300 ms, the others by 30 ms each), which shouldn't change the tier, and
// "slowlong" that of its first SW_SLOW_LONG seconds (the others by a frame's
// time each), which should send it to keyframes only and back. "slowmid"
// slows 3 s from SW_SLOW_MID by 1.1 frame's time a frame (about 130% of the
// video's time in Azahar, like the heavy stretch of Chapter 3's copy on an
// Old 3DS), which a copy's frames decoded ahead should ride through;
// "shallow" decodes only FEW_SLOTS frames ahead, as before, to compare.
#define SW_BLOCK_SAMPLES 48     // about 1.6-2 s of video
#define SW_WARMUP_SECONDS 1.5   // of the clock, before a stretch is measured
#define SW_LAG_BEHIND 0.3       // seconds behind the clock that, with a stretch too slow, counts towards stepping down
#define SW_BLOCKS_OVER 2        // stretches in a row, too slow and behind, that step down
#define SW_LOAD_LOW 0.75        // the estimated share of the time below which reference frames only steps up
#define SW_PROBE_HOLD 3.0       // seconds of video in keyframes only before trying the tier above again
#define SW_PROBE_HOLD_MAX 24.0
#define SW_PROBE_TRIAL 10.0     // a try that steps down again within this many seconds of video failed
#define SW_SLOW_START 1.5
#define SW_SLOW_LONG 18.0
#define SW_SLOW_MID 15.0
#define SW_EMA 0.1
#define SW_AHEAD_SECONDS 2.0                 // how far ahead a copy is decoded, memory allowing
#define SW_LINEAR_KEEP (10u * 1024u * 1024u) // linear memory left free for the game, at least

typedef enum { SW_TIER_FULL, SW_TIER_REF, SW_TIER_KEY } SwTier;

static const char* swTierName(int32_t tier) {
    return tier == SW_TIER_FULL ? "every frame" : tier == SW_TIER_REF ? "reference frames only" : "keyframes only";
}

typedef struct {
    bool bench;
    int32_t tier; // -1: chosen as it goes
} SwConfig;

static SwConfig swConfig(void) {
    SwConfig c = { false, -1 };
    FILE* f = fopen(SW_CHOICE_FILE, "rb");
    if (f == nullptr) return c;
    char text[128];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    for (char* word = strtok(text, " ,\t\r\n"); word != nullptr; word = strtok(nullptr, " ,\t\r\n")) {
        if (strcmp(word, "bench") == 0) c.bench = true;
        else if (strcmp(word, "full") == 0) c.tier = SW_TIER_FULL;
        else if (strcmp(word, "ref") == 0) c.tier = SW_TIER_REF;
        else if (strcmp(word, "key") == 0) c.tier = SW_TIER_KEY;
    }
    return c;
}

static bool isKeyframe(const CtrMp4Track* t, uint32_t i) { return t->sync == nullptr || t->sync[i] != 0; }

static uint32_t swKeyframes(const CtrMp4Track* t) {
    uint32_t n = 0;
    for (uint32_t i = 0; t->count > i; i++) n += isKeyframe(t, i) ? 1u : 0u;
    return n;
}

// Waits for the picture Y2R is converting, if any, and hands it to the game.
static void swFinish(CtrVideo* v) {
    if (v->convertSlot < 0) return;
    u64 t0 = svcGetSystemTick();
    bool ok = CtrSwConvert_finish();
    double ms = ticksToSeconds(svcGetSystemTick() - t0 + v->convertTicks) * 1000.0;
    int32_t slot = v->convertSlot;
    v->convertSlot = -1;
    if (!ok) {
        // The slot stays held for the next picture.
        if (++v->convertFailures <= 3) post(v, "Video: Y2R didn't convert a %lux%lu picture\n", (unsigned long) v->slotW, (unsigned long) v->slotH);
        return;
    }
    v->convertSeconds += ms / 1000.0;
    if (ms > v->convertMaxMs) v->convertMaxMs = ms;
    publishSlotAt(v, slot, v->convertPts);
}

// A picture into a free slot, by Y2R, and on to the game. With overlap, Y2R
// converts a copy (or the half-size picture) while the decoder goes on to the
// next picture, which first waits for it (swFinish): the picture reaches the
// game a decode later, so only while decoding keeps ahead. False once the
// thread is to stop.
static bool swDeliver(CtrVideo* v, const CtrSwPicture* p, double pts, bool overlap) {
    swFinish(v);
    int32_t slot = acquireSlot(v);
    if (slot < 0) return false;
    u64 t0 = svcGetSystemTick();
    int32_t w = p->width < (int32_t) v->codedW ? p->width : (int32_t) v->codedW;
    int32_t h = p->height < (int32_t) v->codedH ? p->height : (int32_t) v->codedH;
    CtrSwPicture staged;
    if (v->preHalved) CtrSwPicture_halve(p, w, h, v->stage, (int32_t) v->slotW, (int32_t) v->slotH, &staged);
    else CtrSwPicture_copy(p, w, h, v->stage, (int32_t) v->slotW, (int32_t) v->slotH, &staged);
    // HD video is BT.709, by the video's own height: a copy keeps the YUV
    // values of the video it was scaled from.
    bool ok = CtrSwConvert_start(&staged, staged.width, staged.height, v->slots[slot].pixels, (int32_t) v->slotW, v->height >= 720);
    v->convertTicks = svcGetSystemTick() - t0;
    double shown = pendingPop(v, pts);
    if (!ok) {
        // The slot stays held for the next picture.
        if (++v->convertFailures <= 3) post(v, "Video: Y2R didn't convert a %ldx%ld picture\n", (long) staged.width, (long) staged.height);
        return true;
    }
    v->convertSlot = slot;
    v->convertPts = shown;
    if (!overlap) swFinish(v);
    return true;
}

// Everything the decoder still holds, on to the game; the decoder is then
// ready for a keyframe.
static bool swDrain(CtrVideo* v, CtrSwDecoder* d, double fallbackPts) {
    CtrSwPicture p;
    bool going = true;
    while (CtrSwDecoder_drain(d, &p) == 1) {
        if (going) going = swDeliver(v, &p, fallbackPts, false);
    }
    // A keyframe on its own (or the last picture) goes to the game now.
    swFinish(v);
    v->pendingCount = 0;
    return going;
}

static void swSetTier(CtrVideo* v, int32_t tier, const char* why) {
    post(v, "Video: software decoding: %s (%s)\n", swTierName(tier), why);
    v->swTier = tier;
    v->tierChanges++;
}

// Samples decoded in one tier, whose time decides whether the tier keeps up
// (see swPlayMain).
typedef struct {
    u64 ticks;        // the time they took, less the time spent waiting for a free slot
    uint32_t samples; // samples passed, decoded or skipped
    uint32_t skipped; // non-reference samples skipped
    bool jumped;      // whether it fell so far behind that it jumped to a keyframe
} SwBlock;

static void swPlayMain(void* arg) {
    CtrVideo* v = (CtrVideo*) arg;
    uint8_t* paintedStack = paintStack();
    const CtrMp4Track* t = &v->mp4.video;
    CtrSwDecoder* d = CtrSwDecoder_open();
    bool ok = d != nullptr;
    if (!ok) goPictureless(v, "the software decoder didn't open");
    else swFeedParameterSets(t, v->au, v->auCap, d);
    // The test hooks: which stretch of the video decodes slowly, and by how
    // much for each frame.
    bool slowLong = swChoiceHas("slowlong"), slowMid = !slowLong && swChoiceHas("slowmid");
    double slowFrom = slowMid ? SW_SLOW_MID : 0.0;
    double slowUntil = slowLong ? SW_SLOW_LONG : slowMid ? SW_SLOW_MID + 3.0 : swChoiceHas("slowstart") ? SW_SLOW_START : 0.0;
    s64 slowBy = slowLong ? (s64) (v->frameSeconds * 1e9) : slowMid ? (s64) (v->frameSeconds * 1.1e9) : 30000000LL;
    double costRef = 0.0, costNonref = 0.0; // ms per frame, as moving averages
    bool haveRef = false, haveNonref = false;
    uint32_t seenDisposable = 0, failuresInRow = 0;
    u64 busyTicks = 0, readTicks = 0;
    SwBlock block = { 0, 0, 0, false };
    bool counting = false; // whether the sample under way counts towards block
    u64 sampleStart = 0, waitAtStart = 0;
    double load = 0.0; // of the last whole block
    uint32_t over = 0; // blocks in a row too slow and behind, in this tier
    double hold = SW_PROBE_HOLD;
    double keyFrom = 0.0, probedAt = -1.0; // video times: of the step down to keyframes only, and of the last try
    for (uint32_t i = 0; ok && t->count > i && !v->quit; i++) {
        LightLock_Lock(&v->lock);
        double clock = v->sharedClock;
        bool started = v->sharedClockStarted && v->frameClockRunner == nullptr;
        LightLock_Unlock(&v->lock);
        u64 now = svcGetSystemTick();
        if (counting) {
            block.ticks += now - sampleStart - (v->waitTicks - waitAtStart);
            block.samples++;
        }
        sampleStart = now;
        waitAtStart = v->waitTicks;
        if (v->swTier == SW_TIER_KEY) {
            // The latest keyframe that is due, or else the next one.
            uint32_t target = UINT32_MAX;
            for (uint32_t j = i; t->count > j; j++) {
                if (!isKeyframe(t, j)) continue;
                if (target == UINT32_MAX || sampleSeconds(v, j) <= clock) target = j;
                if (sampleSeconds(v, j) > clock) break;
            }
            if (target == UINT32_MAX) break;
            if (target != i) {
                v->keySkips += target - i;
                i = target;
            }
            // From a keyframe, the tier above again after a while of the
            // video (not of the clock: keyframes are decoded well ahead).
            if (v->swForcedTier < 0 && sampleSeconds(v, i) - keyFrom >= hold) {
                char why[64];
                snprintf(why, sizeof(why), "trying again after %.0f s", hold);
                swSetTier(v, seenDisposable > 0 ? SW_TIER_REF : SW_TIER_FULL, why);
                probedAt = sampleSeconds(v, i);
                block = (SwBlock) { 0, 0, 0, false };
                over = 0;
                load = 0.0; // none measured yet in this tier
            }
        } else if (started && sampleSeconds(v, i) < clock - 1.0) {
            // Far behind: on to the last keyframe that is due.
            uint32_t target = i;
            for (uint32_t j = i + 1; t->count > j && sampleSeconds(v, j) <= clock + 0.25; j++) {
                if (isKeyframe(t, j)) target = j;
            }
            if (target != i) {
                v->jumps++;
                block.jumped = true;
                i = target;
            }
        }
        counting = started && clock >= SW_WARMUP_SECONDS && v->swTier != SW_TIER_KEY;
        u64 r0 = svcGetSystemTick();
        bool read = CtrMp4_readSample(&v->mp4, t, i, v->au);
        readTicks += svcGetSystemTick() - r0;
        if (!read) {
            goPictureless(v, "a sample could not be read");
            break;
        }
        bool disposable = false;
        uint32_t size = toStartCodes(v->au, t->sizes[i], &disposable, 0);
        if (size == 0) continue;
        if (disposable) seenDisposable++;
        double pts = sampleSeconds(v, i);
        if (disposable && (v->swTier != SW_TIER_FULL || (started && pts < clock))) {
            v->skippedRef++;
            if (v->swTier == SW_TIER_REF) block.skipped++;
            continue;
        }
        CtrSwPicture p;
        u64 d0 = svcGetSystemTick();
        int32_t got = CtrSwDecoder_decode(d, v->au, size, &p);
        u64 dt = svcGetSystemTick() - d0;
        busyTicks += dt;
        double ms = ticksToSeconds(dt) * 1000.0;
        if (disposable) {
            costNonref = haveNonref ? costNonref + SW_EMA * (ms - costNonref) : ms;
            haveNonref = true;
        } else {
            costRef = haveRef ? costRef + SW_EMA * (ms - costRef) : ms;
            haveRef = true;
        }
        v->fed++;
        if (pts >= slowFrom && pts < slowUntil) svcSleepThread(v->fed == 1 ? 300000000LL : slowBy);
        pendingPush(v, pts);
        if (got < 0) {
            v->errors++;
            if (v->errors <= 3) post(v, "Video: sample %lu: the software decoder said %ld\n", (unsigned long) i, (long) CtrSwDecoder_lastError(d));
            if (++failuresInRow >= 30) {
                goPictureless(v, "the software decoder kept failing");
                ok = false;
            }
        } else {
            failuresInRow = 0;
        }
        // Overlapped once the first picture is out, while a frame decodes in
        // less than a frame's time.
        bool overlap = v->decoded > 0 && haveRef && costRef < v->frameSeconds * 1000.0;
        if (got == 1 && !swDeliver(v, &p, pts, overlap)) break;
        if (v->swTier == SW_TIER_KEY && !swDrain(v, d, pts)) break;
        if (v->decoded > 0) v->convertMsAvg = v->convertSeconds * 1000.0 / (double) v->decoded;
        if (v->fed % 240 == 0) {
            post(v, "Video: sample %lu at %.2f s (clock %.2f): %s, %lu pictures, reference %.1f ms, non-reference %.1f ms, Y2R %.1f ms; "
                    "load %.2f\n",
                 (unsigned long) i, pts, clock, swTierName(v->swTier), (unsigned long) v->decoded, costRef, costNonref, v->convertMsAvg, load);
        }
        if (block.samples < SW_BLOCK_SAMPLES) continue;
        // A whole stretch measured: the share of the video's time for it
        // that it took.
        load = ticksToSeconds(block.ticks) / ((double) block.samples * v->frameSeconds);
        double lag = started ? clock - pts : 0.0;
        double skippedLoad = (double) block.skipped * (haveNonref ? costNonref : costRef * 0.9) / 1000.0 / ((double) block.samples * v->frameSeconds);
        bool jumped = block.jumped;
        block = (SwBlock) { 0, 0, 0, false };
        if (v->swForcedTier >= 0) continue;
        bool slow = load > 1.0 && (lag > SW_LAG_BEHIND || jumped);
        over = slow ? over + 1 : 0;
        if (slow && SW_BLOCKS_OVER > over) {
            post(v, "Video: software decoding: %s took %.0f%% of the video's time for samples to %lu, %.2f s behind%s; staying\n",
                 swTierName(v->swTier), load * 100.0, (unsigned long) i, lag, jumped ? ", jumped ahead" : "");
        } else if (slow) {
            bool toRef = v->swTier == SW_TIER_FULL && seenDisposable > 0;
            char why[96];
            snprintf(why, sizeof(why), "%s took %.0f%% of the video's time, %.2f s behind%s", swTierName(v->swTier), load * 100.0, lag,
                     jumped ? ", jumped ahead" : "");
            swSetTier(v, toRef ? SW_TIER_REF : SW_TIER_KEY, why);
            counting = false;
            over = 0;
            if (!toRef) {
                // A try of the tier above that failed waits longer for the next.
                bool failedTry = probedAt >= 0.0 && pts - probedAt < SW_PROBE_TRIAL;
                hold = failedTry ? (hold * 2.0 < SW_PROBE_HOLD_MAX ? hold * 2.0 : SW_PROBE_HOLD_MAX) : SW_PROBE_HOLD;
                keyFrom = started && clock > pts ? clock : pts;
                if (!swDrain(v, d, pts)) break;
            }
        } else if (v->swTier == SW_TIER_REF && load + skippedLoad < SW_LOAD_LOW) {
            swSetTier(v, SW_TIER_FULL, "every frame would keep up");
            counting = false;
            over = 0;
        }
    }
    if (ok && d != nullptr && !v->quit) swDrain(v, d, v->duration);
    v->busySeconds = ticksToSeconds(busyTicks);
    v->readSeconds = ticksToSeconds(readTicks);
    post(v, "Video: software decoding ended %s: reference %.1f ms, non-reference %.1f ms, Y2R %.1f ms (max %.1f), %lu keyframe skips, "
            "%lu tier changes\n",
         swTierName(v->swTier), costRef, costNonref, v->convertMsAvg, v->convertMaxMs, (unsigned long) v->keySkips,
         (unsigned long) v->tierChanges);
    CtrSwDecoder_close(d);
    // Y2R must be done with its slot before the slots can be freed.
    CtrSwConvert_finish();
    v->convertSlot = -1;
    LightLock_Lock(&v->lock);
    if (v->heldSlot >= 0) v->slots[v->heldSlot].state = SLOT_FREE;
    v->heldSlot = -1;
    v->decodeDone = true;
    LightLock_Unlock(&v->lock);
    v->stackUsed = stackWritten(paintedStack);
}

// The copy of the video made for the Old 3DS on a PC
// (tools/videos/make-3ds-videos.ps1): <name>.3ds.mp4 next to <name>.mp4. It
// is taken in the video's place for the pictures when it is H.264 of a size
// the decoder and a texture take (multiples of 16, no larger than the video
// or a texture), with as many frames over the same time as the video, which
// is what makes its frames line up with the video's audio and with the
// game's captions. True if v->mp4 is now the copy's. Says in the log which
// it is either way, and why a copy found isn't used: `what` names the copy,
// `without` says how the video plays without it.
static bool swOpenCopy(CtrVideo* v, const char* what, const char* without) {
    char path[VIDEO_PATH_MAX];
    size_t len = strlen(v->path);
    size_t stem = len >= 4 && strcasecmp(v->path + len - 4, ".mp4") == 0 ? len - 4 : len;
    if (snprintf(path, sizeof(path), "%.*s%s", (int) stem, v->path, COPY_SUFFIX) >= (int) sizeof(path)) return false;
    const char* name = strrchr(path, '/');
    name = name != nullptr ? name + 1 : path;
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        logInfo("Video: no %s, the %s, so %s plays %s (make the copy on a PC with tools/videos/make-3ds-videos.ps1)\n", name, what,
                v->name, without);
        return false;
    }
    fclose(f);
    CtrMp4 copy;
    memset(&copy, 0, sizeof(copy));
    if (!CtrMp4_open(&copy, path, VIDEO_WINDOW_BYTES)) {
        logWarn("Video: %s isn't an MP4 file; %s plays %s\n", name, v->name, without);
        return false;
    }
    const CtrMp4Track* t = &copy.video;
    const CtrMp4Track* o = &v->mp4.video;
    const char* why = nullptr;
    if (!isAvc(t)) why = "isn't H.264 with 4-byte NAL lengths";
    else if (t->width == 0 || t->height == 0 || t->width % 16u != 0 || t->height % 16u != 0) why = "isn't a multiple of 16 pixels each way";
    else if ((int32_t) t->width > v->width || (int32_t) t->height > v->height || t->width > VIDEO_TEXTURE_MAX || t->height > VIDEO_TEXTURE_MAX) {
        why = "is larger than the video or a texture";
    } else if (t->count != o->count || t->duration < o->duration - 0.25 || t->duration > o->duration + 0.25) {
        why = "doesn't have the video's frames (made from another version of it?)";
    }
    if (why != nullptr) {
        logWarn("Video: %s (%lux%lu, %lu frames, %.2f s) %s; %s plays %s. Make it again with tools/videos/make-3ds-videos.ps1\n", name,
                (unsigned long) t->width, (unsigned long) t->height, (unsigned long) t->count, t->duration, why, v->name, without);
        CtrMp4_close(&copy);
        return false;
    }
    CtrMp4_close(&v->mp4);
    v->mp4 = copy;
    v->fromCopy = true;
    v->codedW = t->width;
    v->codedH = t->height;
    v->slotW = v->codedW;
    v->slotH = v->codedH;
    logInfo("Video: playing %s from %s, its %s: %lux%lu, level %d, %lu keyframes, shown at %dx%d\n", v->name, name, what,
            (unsigned long) t->width, (unsigned long) t->height, t->configSize > 3 ? (int) t->config[3] : 0,
            (unsigned long) swKeyframes(t), (int) v->width, (int) v->height);
    return true;
}

// Starts software decoding into slots. False (with nothing held) if it can't.
static bool startSwPlayback(CtrVideo* v, const SwConfig* c) {
    const CtrMp4Track* t = &v->mp4.video;
    if (!isAvc(t)) return false;
    // A hash test waits for pictures instead of adapting to CPU load.
    v->swForcedTier = v->frameClockRunner != nullptr && c->tier < 0 ? SW_TIER_FULL : c->tier;
    v->swTier = c->tier >= 0 ? c->tier : SW_TIER_FULL;
    if (!CtrSwConvert_open()) {
        logInfo("Video: no Y2R, so no software decoding\n");
        return false;
    }
    v->y2rOpen = true;
    // A video the renderer halves (past the largest texture) is halved here
    // instead, as YUV before Y2R: a quarter of the memory for each slot, no
    // full-size copies for the renderer's halving transfers (the Old 3DS ran
    // out of linear memory with 720p slots), and a quarter of Y2R's work.
    // Either way Y2R converts from the stage (see swDeliver).
    v->preHalved = !v->fromCopy && (v->width > VIDEO_TEXTURE_MAX || v->height > VIDEO_TEXTURE_MAX) && v->width <= 2 * VIDEO_TEXTURE_MAX &&
                   v->height <= 2 * VIDEO_TEXTURE_MAX && (v->codedW / 2u) % 8u == 0 && (v->codedH / 2u) % 8u == 0;
    if (v->preHalved) {
        v->slotW = v->codedW / 2u;
        v->slotH = v->codedH / 2u;
    }
    v->stage = (uint8_t*) linearAlloc((size_t) v->slotW * v->slotH * 3u / 2u);
    v->convertSlot = -1;
    v->auCap = t->maxSize + 16;
    v->au = (uint8_t*) linearAlloc(v->auCap);
    size_t frameBytes = (size_t) v->slotW * v->slotH * 2u;
    // A copy decodes up to SW_AHEAD_SECONDS ahead, so that what the light
    // stretches save carries it through the heavy ones: on an Old 3DS
    // Chapter 3's copy decodes in 63-73% of the video's time on average but
    // took 115-132% of it for 3 s, which four frames ahead didn't cover.
    // SW_LINEAR_KEEP of linear memory stays free for the game.
    int32_t want = FEW_SLOTS;
    if (v->fromCopy && !swChoiceHas("shallow")) {
        size_t freeBytes = linearSpaceFree();
        size_t room = freeBytes > SW_LINEAR_KEEP ? (freeBytes - SW_LINEAR_KEEP) / frameBytes : 0;
        int32_t ahead = (int32_t) (SW_AHEAD_SECONDS / v->frameSeconds) + 2; // and the one decoding, and the one shown
        want = ahead < (int32_t) room ? ahead : (int32_t) room;
        if (want > MAX_SLOTS) want = MAX_SLOTS;
        if (want < FEW_SLOTS) want = FEW_SLOTS;
    }
    for (int32_t i = 0; want > i; i++) {
        v->slots[i].pixels = (uint16_t*) linearAlloc(frameBytes);
        if (v->slots[i].pixels == nullptr) break;
        v->slots[i].state = SLOT_FREE;
        v->slotCount++;
    }
    v->heldSlot = -1;
    v->pendingCount = 0;
    if (v->slotCount >= MIN_SLOTS && v->au != nullptr && v->stage != nullptr) {
        s32 priority = 0x30;
        svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        priority = priority + 2 > 0x3F ? 0x3F : priority + 2;
        v->thread = threadCreate(swPlayMain, v, SW_STACK_BYTES, priority, -2, false);
    }
    if (v->thread == nullptr) {
        logInfo("Video: no memory or thread for software decoding (%d slots, %.1f MB linear free)\n", (int) v->slotCount,
                (double) linearSpaceFree() / (1024.0 * 1024.0));
        freeBuffers(v);
        CtrSwConvert_close();
        v->y2rOpen = false;
        return false;
    }
    logInfo("Video: software decoding with openh264, %s, %d frames of %.2f MB%s (%.1f s ahead), %.1f MB linear left\n",
            c->tier >= 0 ? swTierName(c->tier) : "tier chosen as it goes", (int) v->slotCount, (double) frameBytes / (1024.0 * 1024.0),
            v->preHalved ? " (halved)" : "", (double) (v->slotCount - 2) * v->frameSeconds, (double) linearSpaceFree() / (1024.0 * 1024.0));
    return true;
}
#endif

static void ctrVideoClose(VideoDecoder* decoder) {
    CtrVideo* v = impl(decoder);
    if (!v->opened) return;
    aptUnhook(&v->aptCookie);
    if (activeVideo == v) activeVideo = nullptr;
    stopDecoder(v);
    flushMessages(v);
    logInfo("Video: closed %s%s at %.2f of %.2f s: %lu frames shown, %lu dropped late (and %lu after gaps in updates), %lu upload failures; decoded %lu of %lu fed "
            "(%lu non-reference skipped, %lu jumps, %lu errors), decoder busy %.2f s (%.1f fps possible), reads %.2f s, "
            "process max %.1f ms, render max %.1f ms, first frame after %.0f ms\n",
            v->name, v->fromCopy ? " (pictures from its copy)" : "", v->clock, v->duration, (unsigned long) v->shown, (unsigned long) v->lateDrops, (unsigned long) v->gapDrops, (unsigned long) v->uploadFailures,
            (unsigned long) v->decoded, (unsigned long) v->fed, (unsigned long) v->skippedRef, (unsigned long) v->jumps,
            (unsigned long) v->errors, v->busySeconds, v->busySeconds > 0.0 ? (double) v->decoded / v->busySeconds : 0.0, v->readSeconds, v->processMaxMs,
            v->renderMaxMs, v->firstFrameSeconds * 1000.0);
    CtrMp4_close(&v->mp4);
    if (v->patternPixels != nullptr) linearFree(v->patternPixels);
    v->patternPixels = nullptr;
    if (v->renderer != nullptr) CtrRenderer_releaseVideo(v->renderer);
    v->renderer = nullptr;
    v->opened = false;
}

static bool ctrVideoOpen(VideoDecoder* decoder, const char* url) {
    CtrVideo* v = impl(decoder);
    ctrVideoClose(decoder);
    memset(v, 0, sizeof(*v));
    LightLock_Init(&v->lock);
    LightEvent_Init(&v->wake, RESET_ONESHOT);
    v->surfaceId = -1;
    v->pendingSlot = -1;
    v->heldSlot = -1;
    v->openTick = svcGetSystemTick();
    v->lastTick = v->openTick;
    v->frameClockRunner = frameClockRunner;
    if (frameClockRunner != nullptr) {
        v->lastFrame = frameClockRunner->frameCount;
        logInfo("Video: frame clock (state-hash / fixed-seed bench)\n");
    }
    const char* slash = strrchr(url, '/');
    snprintf(v->name, sizeof(v->name), "%s", slash != nullptr ? slash + 1 : url);
    snprintf(v->path, sizeof(v->path), "%s", url);

    if (!CtrMp4_open(&v->mp4, url, VIDEO_WINDOW_BYTES)) {
        logWarn("Video: %s is missing or not an MP4 file\n", url);
        return false;
    }
    const CtrMp4Track* t = &v->mp4.video;
    v->hasAudio = v->mp4.audio.count > 0 && v->mp4.audio.codec == CTR_MP4_FOURCC('m', 'p', '4', 'a') && v->mp4.audio.configSize >= 2;
    v->duration = t->count > 0 ? t->duration : v->mp4.audio.duration;
    if (v->duration <= 0.0) v->duration = v->mp4.duration;
    if (t->count == 0 || t->width == 0 || t->height == 0) {
        if (!v->hasAudio) {
            logWarn("Video: %s has neither a video nor an audio track\n", url);
            CtrMp4_close(&v->mp4);
            return false;
        }
        v->width = 640;
        v->height = 480;
    } else {
        v->width = (int32_t) t->width;
        v->height = (int32_t) t->height;
    }
    v->codedW = ((uint32_t) v->width + 15u) & ~15u;
    v->codedH = ((uint32_t) v->height + 15u) & ~15u;
    v->slotW = v->codedW;
    v->slotH = v->codedH;
    v->frameSeconds = t->count > 0 ? v->duration / (double) t->count : 1.0 / 30.0;
    v->opened = true;
    aptHook(&v->aptCookie, onVideoAptEvent, v);
    activeVideo = v;
    logInfo("Video: %s: %dx%d, %lu frames, %.2f s, level %d%s, %.1f MB linear free\n", v->name, (int) v->width, (int) v->height,
            (unsigned long) t->count, v->duration, t->configSize > 3 ? (int) t->config[3] : 0, v->hasAudio ? ", AAC audio" : ", no audio",
            (double) linearSpaceFree() / (1024.0 * 1024.0));

    // The test pattern also leaves the decoder out, to check the upload
    // path on its own.
    FILE* f = fopen(TEST_PATTERN_FILE, "rb");
    v->testPattern = f != nullptr;
    if (f != nullptr) fclose(f);
    v->mode = MODE_PICTURELESS;
    if (t->count > 0 && !v->testPattern && startDecoder(v)) v->mode = MODE_PICTURES;
#ifdef CTRPORT_SW_DECODE
    // A stream a New 3DS keeps off its decoder (Chapter 5's level 4.0) isn't
    // decoded in software either: whether its CPU keeps up with it hasn't
    // been measured. Its copy made on a PC for the Old 3DS is level 3.0, so
    // that plays instead when there is one: on the decoder, or failing that
    // in software, which a New 3DS keeps up with at the copy's size.
    else if (v->keptOffHardware) {
        if (swOpenCopy(v, "3DS copy", "without pictures")) {
            v->keptOffHardware = false;
            if (startDecoder(v)) {
                v->mode = MODE_PICTURES;
                logInfo("Video: the copy plays on the hardware decoder\n");
            } else {
                SwConfig c = swConfig();
                c.bench = false;
                if (startSwPlayback(v, &c)) v->mode = MODE_PICTURES;
            }
        }
    } else if (t->count > 0 && !v->testPattern) {
        SwConfig c = swConfig();
        if (c.bench) startSwBench(v);
        else {
            // The copy if there is one, else the keyframes (unless
            // SW_CHOICE_FILE fixes the tier).
            char slideshow[48];
            snprintf(slideshow, sizeof(slideshow), "as a slideshow of its %lu keyframes", (unsigned long) swKeyframes(t));
            if (!swOpenCopy(v, "copy for the Old 3DS", slideshow) && c.tier < 0) c.tier = SW_TIER_KEY;
            if (startSwPlayback(v, &c)) v->mode = MODE_PICTURES;
        }
    }
#endif
    if (v->mode == MODE_PICTURELESS) {
        logInfo("Video: playing %s without pictures%s\n", v->name, v->testPattern ? " (test pattern, " TEST_PATTERN_FILE ")" : "");
    }
    return true;
}

static bool ctrVideoExtractAudio(MAYBE_UNUSED VideoDecoder* decoder, MAYBE_UNUSED Runner* runner, MAYBE_UNUSED const char* url) {
    return false;
}

static bool ctrVideoAudioInFile(VideoDecoder* decoder) { return impl(decoder)->opened && impl(decoder)->hasAudio; }

static bool ctrVideoIsRunning(VideoDecoder* decoder) {
    CtrVideo* v = impl(decoder);
    if (!v->opened) return false;
    tickClock(v);
    return v->clock < v->duration;
}

static bool ctrVideoIsPaused(VideoDecoder* decoder) { return impl(decoder)->paused; }

static void ctrVideoPause(VideoDecoder* decoder) {
    CtrVideo* v = impl(decoder);
    tickClock(v);
    v->paused = true;
}

static void ctrVideoResume(VideoDecoder* decoder) {
    CtrVideo* v = impl(decoder);
    tickClock(v);
    v->paused = false;
    v->resumed = true;
}

static void ctrVideoSetLoop(VideoDecoder* decoder, bool loop) {
    if (loop && impl(decoder)->opened) logInfo("Video: looping is not supported; %s plays once\n", impl(decoder)->name);
}

static void ctrVideoSetMasterClock(VideoDecoder* decoder, double positionSeconds) {
    CtrVideo* v = impl(decoder);
    tickClock(v);
    if (v->frameClockRunner != nullptr || !v->hasAudio || positionSeconds < 0.0 || v->paused || v->suspended) return;
    // The audio holds at 0 until its stream has started.
    if (!v->audioSeen && positionSeconds > 0.0) {
        v->audioSeen = true;
        if (!v->clockStarted) logInfo("Video: audio started %.0f ms after the video opened\n", ticksToSeconds(svcGetSystemTick() - v->openTick) * 1000.0);
        v->clockStarted = true;
    }
    if (v->audioSeen && positionSeconds != v->lastAudio) {
        v->clock = positionSeconds;
        v->lastAudio = positionSeconds;
    }
}

// A frame clock must select the same picture even when the decode thread is
// late. Pictures arrive in presentation order: a future picture (or EOF)
// proves all pictures due now have arrived. Keep the latest due one, freeing
// older ones so a full queue cannot stop the decoder reaching that boundary.
// No elapsed-time timeout or load-dependent tier decision belongs in a hash.
static void waitForPresentation(CtrVideo* v) {
    for (;;) {
        LightLock_Lock(&v->lock);
        bool complete = v->mode == MODE_PICTURELESS || v->decodeDone;
        bool freed = false;
        int32_t best = -1;
        for (int32_t i = 0; i < v->slotCount; i++) {
            FrameSlot* s = &v->slots[i];
            if (s->state == SLOT_SHOWN && v->renderer != nullptr && CtrRenderer_fenceDone(v->renderer, s->fence)) {
                s->state = SLOT_FREE;
                freed = true;
            }
            if (s->state != SLOT_READY) continue;
            if (s->pts > v->clock + SHOW_AHEAD) complete = true;
            else if (best < 0 || s->pts > v->slots[best].pts) best = i;
        }
        if (!complete) {
            for (int32_t i = 0; i < v->slotCount; i++) {
                if (i != best && v->slots[i].state == SLOT_READY) {
                    v->slots[i].state = SLOT_FREE;
                    freed = true;
                }
            }
        }
        LightLock_Unlock(&v->lock);
        if (freed) LightEvent_Signal(&v->wake);
        if (complete || v->quit) return;
        svcSleepThread(1000000LL);
    }
}

static int32_t ctrVideoUpdate(VideoDecoder* decoder) {
    CtrVideo* v = impl(decoder);
    if (!v->opened) return VIDEO_FRAME_NONE;
    tickClock(v);
    publishClock(v);
    flushMessages(v);
    if (v->frameClockRunner != nullptr) waitForPresentation(v);
    LightLock_Lock(&v->lock);
    Mode mode = v->mode;
    if (mode == MODE_PICTURELESS) {
        // Whatever the thread had made goes.
        for (int32_t i = 0; v->slotCount > i; i++) {
            if (v->slots[i].state == SLOT_READY) v->slots[i].state = SLOT_FREE;
        }
        LightLock_Unlock(&v->lock);
        if (v->blankDone) return VIDEO_FRAME_HELD;
        v->blankPending = true;
        return VIDEO_FRAME_READY;
    }
    bool freed = false;
    int32_t best = -1;
    for (int32_t i = 0; v->slotCount > i; i++) {
        FrameSlot* s = &v->slots[i];
        if (s->state == SLOT_SHOWN && v->renderer != nullptr && CtrRenderer_fenceDone(v->renderer, s->fence)) {
            s->state = SLOT_FREE;
            freed = true;
        }
        // Before the clock starts, the first frame shows at once.
        if (s->state == SLOT_READY && (s->pts <= v->clock + SHOW_AHEAD || !v->clockStarted)) {
            if (best < 0 || (v->clockStarted ? s->pts > v->slots[best].pts : s->pts < v->slots[best].pts)) best = i;
        }
    }
    double step = v->clock - v->updateClock;
    v->updateClock = v->clock;
    uint32_t gapped = 0;
    if (best >= 0) {
        // Older frames that are due are too late to show.
        for (int32_t i = 0; v->slotCount > i; i++) {
            FrameSlot* s = &v->slots[i];
            if (i != best && s->state == SLOT_READY && s->pts < v->slots[best].pts) {
                s->state = SLOT_FREE;
                if (step > VIDEO_UPDATE_GAP) gapped++;
                else v->lateDrops++;
                freed = true;
            }
        }
        v->gapDrops += gapped;
    }
    LightLock_Unlock(&v->lock);
    if (gapped > 0) logInfo("Video: %lu frames due at once at %.2f s, after %.2f s without an update\n", (unsigned long) gapped, v->clock, step);
    if (freed) LightEvent_Signal(&v->wake);
    if (best < 0) return v->shown > 0 ? VIDEO_FRAME_HELD : VIDEO_FRAME_NONE;
    v->pendingSlot = best;
    return VIDEO_FRAME_READY;
}

// The runner's 8x8 console font, rasterised into the video buffer. It never
// touches a screen console or the game's drawing state. The surface keeps
// the notice within the video's rectangle, including when GML scales it.
static void noticeLine(uint16_t* pixels, unsigned width, unsigned y, const char* text) {
    const ConsoleFont* font = &consoleGetDefault()->font;
    unsigned x0 = (width - (unsigned) strlen(text) * 8u) / 2u;
    for (unsigned i = 0; text[i]; i++) {
        unsigned ch = (unsigned char) text[i];
        if (ch < font->asciiOffset || ch - font->asciiOffset >= font->numChars) continue;
        const u8* glyph = font->gfx + (ch - font->asciiOffset) * 8u;
        for (unsigned row = 0; row < 8; row++) {
            for (unsigned col = 0; col < 8; col++) {
                if (glyph[row] & (0x80u >> col)) pixels[(y + row) * width + x0 + i * 8u + col] = 0xFFFF;
            }
        }
    }
}

// A notice, or the test pattern, for a video without pictures. Called only
// in MODE_PICTURELESS: waiting for a real first picture never shows it.
static void drawBlank(CtrVideo* v) {
    if (!v->testPattern) {
        const unsigned w = 320, h = 176; // GX dimensions are multiples of 8
        if (v->patternPixels == nullptr) {
            v->patternPixels = (uint16_t*) linearAlloc(w * h * 2u);
            if (v->patternPixels == nullptr) {
                v->blankDone = CtrRenderer_fillVideoSurface(v->renderer, v->surfaceId, 0, 0, 0);
                return;
            }
            memset(v->patternPixels, 0, w * h * 2u);
            noticeLine(v->patternPixels, w, 64, "This video needs its 3DS copy.");
            noticeLine(v->patternPixels, w, 84, "On a PC, run make-3ds-videos.");
            noticeLine(v->patternPixels, w, 104, "See docs/playing.md.");
            flushCache(v->patternPixels, w * h * 2u);
        }
        v->blankDone = CtrRenderer_uploadVideoFrame(v->renderer, v->surfaceId, v->patternPixels, w, h, false, &v->patternFence);
        return;
    }
    size_t count = (size_t) v->codedW * v->codedH;
    if (v->patternPixels == nullptr) {
        v->patternPixels = (uint16_t*) linearAlloc(count * 2u);
        if (v->patternPixels == nullptr) {
            v->testPattern = false;
            return;
        }
        for (uint32_t y = 0; v->codedH > y; y++) {
            for (uint32_t x = 0; v->codedW > x; x++) {
                uint16_t c = y < v->codedH / 2 ? (uint16_t) (31u << 11) : (uint16_t) 31u;
                if (x < 64 && y < 64) c = 0xFFFF;
                v->patternPixels[(size_t) y * v->codedW + x] = c;
            }
        }
        flushCache(v->patternPixels, (uint32_t) count * 2u);
    }
    v->blankDone = CtrRenderer_uploadVideoFrame(v->renderer, v->surfaceId, v->patternPixels, (int32_t) v->codedW, (int32_t) v->codedH,
                                                v->halve, &v->patternFence);}

// Whether frames are halved on their way to the surface: a video past the
// largest texture, unless the software path halved it already or the
// pictures come from a copy, which is small enough as it is (the renderer
// scales a frame to the surface).
static bool uploadHalves(const CtrVideo* v) { return v->halve && !v->preHalved && !v->fromCopy; }

static void ctrVideoDraw(VideoDecoder* decoder, Runner* runner, int32_t surfaceId) {
    CtrVideo* v = impl(decoder);
    if (v->renderer == nullptr) v->renderer = runner->renderer;
    v->surfaceId = surfaceId;
    if (v->blankPending) {
        v->blankPending = false;
        drawBlank(v);
        return;
    }
    int32_t index = v->pendingSlot;
    v->pendingSlot = -1;
    if (index < 0) return;
    FrameSlot* s = &v->slots[index];
    uint32_t fence = 0;
    bool tracing = TRACE_UPLOADS > v->traceUploads;
    if (tracing) {
        trace("upload %lu: queueing slot %d (%lux%lu%s)", (unsigned long) v->traceUploads, (int) index, (unsigned long) v->slotW,
              (unsigned long) v->slotH, uploadHalves(v) ? ", halved" : "");
    }
    bool ok = CtrRenderer_uploadVideoFrame(v->renderer, surfaceId, s->pixels, (int32_t) v->slotW, (int32_t) v->slotH, uploadHalves(v), &fence);
    if (tracing) trace("upload %lu: %s (the GPU runs it at the end of the frame)", (unsigned long) v->traceUploads++, ok ? "queued" : "failed");
    LightLock_Lock(&v->lock);
    s->fence = fence;
    s->state = ok ? SLOT_SHOWN : SLOT_FREE;
    LightLock_Unlock(&v->lock);
    if (ok) v->shown++;
    else {
        v->uploadFailures++;
        LightEvent_Signal(&v->wake);
    }
}

static double ctrVideoDuration(VideoDecoder* decoder) { return impl(decoder)->duration; }

static double ctrVideoPosition(VideoDecoder* decoder) {
    CtrVideo* v = impl(decoder);
    if (!v->opened) return 0.0;
    tickClock(v);
    return v->clock < v->duration ? v->clock : v->duration;
}

static int32_t ctrVideoWidth(VideoDecoder* decoder) { return impl(decoder)->width; }
static int32_t ctrVideoHeight(VideoDecoder* decoder) { return impl(decoder)->height; }

static int32_t ctrVideoCreateSurface(VideoDecoder* decoder, Runner* runner) {
    CtrVideo* v = impl(decoder);
    if (!v->opened) return -1;
    int32_t shift = 0;
    int32_t id = CtrRenderer_createVideoSurface(runner->renderer, v->width, v->height, &shift);
    if (id < 0) return -1;
    v->renderer = runner->renderer;
    v->surfaceId = id;
    v->halve = shift == 1;
    if (shift > 1) logWarn("Video: %dx%d is too large to show\n", (int) v->width, (int) v->height);
    return id;
}

static VideoDecoderVtable ctrVideoVtable = {
    .init = ctrVideoInit,
    .quit = ctrVideoQuit,
    .open = ctrVideoOpen,
    .close = ctrVideoClose,
    .extractAudio = ctrVideoExtractAudio,
    .isRunning = ctrVideoIsRunning,
    .isPaused = ctrVideoIsPaused,
    .pause = ctrVideoPause,
    .resume = ctrVideoResume,
    .setLoop = ctrVideoSetLoop,
    .setMasterClock = ctrVideoSetMasterClock,
    .update = ctrVideoUpdate,
    .draw = ctrVideoDraw,
    .duration = ctrVideoDuration,
    .position = ctrVideoPosition,
    .width = ctrVideoWidth,
    .height = ctrVideoHeight,
    .createSurface = ctrVideoCreateSurface,
    .audioInFile = ctrVideoAudioInFile,
};

VideoDecoder* VideoDecoder_createBackend(void) {
    VideoDecoder* decoder = (VideoDecoder*) calloc(1, sizeof(VideoDecoder));
    CtrVideo* v = (CtrVideo*) calloc(1, sizeof(CtrVideo));
    if (decoder == nullptr || v == nullptr) {
        free(decoder);
        free(v);
        return nullptr;
    }
    decoder->vtable = &ctrVideoVtable;
    decoder->impl = v;
    return decoder;
}

// ===[ Options menu pause (menupause) ]===
// The options menu holds the audio (ctr_audio_system.c, "Options menu pause"); an open video's clock holds with it, in
// a mask bit of its own so HOME, sleep and video_pause overlap it either way round.
#define MENUPAUSE_VIDEO_CODE __attribute__((noinline, section(".text.late.menupause")))
#define MENUPAUSE_VIDEO_BIT 4u

MENUPAUSE_VIDEO_CODE void CtrVideo_menuPause(bool pause) {
    CtrVideo* v = activeVideo;
    if (v == nullptr || !v->opened) return;
    if (pause) {
        tickClock(v);
        v->suspended |= MENUPAUSE_VIDEO_BIT;
        publishClock(v);
    } else {
        // As a restore from HOME: the menu's time doesn't count.
        v->lastTick = svcGetSystemTick();
        if (v->frameClockRunner != nullptr) v->lastFrame = v->frameClockRunner->frameCount;
        v->suspended &= ~MENUPAUSE_VIDEO_BIT;
    }
}
