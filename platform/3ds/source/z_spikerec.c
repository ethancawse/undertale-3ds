// spikerec: the spike recorder, for playtest builds only (build.sh ... spikerec; CMakeLists.txt compiles this file
// and its three link wraps only then, so release builds are unchanged).
//
// Every frame that misses its slot in normal play gets one 32-byte record in a fixed RAM ring (SPIKEREC_RING records,
// 8 KiB, in the late statics: no heap use while playing). The ring is written to GAME_DIR/spikes.txt only in the hidden
// room-change frame (after the room load, before the frame ends: that frame is never presented), at the exit, and when
// HOME suspends the game. A ring that fills between two room changes counts the frames it drops; recording never
// waits. tools/analysis/spikerec_report.py turns the file into ranked lists.
//
// Nothing in main.c changes. The main loop already marks its phases for the watchdog, and those calls are wrapped
// (-Wl,--wrap=CtrWatchdog_mark): "step", "draw", "audio update", "room change", "present" and "wait for frame" give
// each frame's Step, Draw and other CPU time. The room-change call (CtrBench_handleRoomChange, once a frame) hands
// over the runner, and the game thread's SD card reads are counted at FSFILE_Read. Only the call targets of those
// three functions change; no existing code or data moves (check_layout against the build without the word).
//
// A frame missed its slot when the vblank count from its Step's start to the next frame's Step's start is more than
// its slot (60 / room speed vblanks, 2 at 30 fps): the next slot began late. A late top-screen flip (one that stayed up
// more than a slot, CtrPacer_getFlipTotals) also makes a record: the frame was shown late though the CPU was in time,
// usually the GPU. A flip comes one or two frames after the frame that caused it, so such a record names a frame or
// two after the one at fault.
//
// spikes.txt, appended to across sessions (a lab run with --send-files starts it afresh and gets it back at the exit;
// lines are text; times in ms with one decimal):
//   # spikerec 2 <game> <target> <console> <build> ring <records> tps <ticks per second>
//   S <frame> <room index> <room name> per <ms> step <ms> draw <ms> other <ms> load <ms> gpu <ms> miss <vblanks>
//     flip <vblanks> tile <loads>/<from image>/<ms> snd <ms> sd <reads>/<ms> code <reads> under <underruns> fl <flags>
//   R <room index> <room name> enter <frame> frames <n> ms <ms> over30 <n> spikes <n> miss <vblanks> flip <vblanks>
//   F <room|exit|suspend> frame <frame> records <n> dropped <n> prevflush <ms> self <us> max <us>
// S, one spiking frame:
//   per    its Step's start to the next frame's Step's start: its whole period, waiting for the slot included.
//   step, draw   Runner_step, and Draw up to the end of the GUI layer, as main.c times them.
//   other  the rest of the frame's CPU time: the audio update, then from the room-change call's end to the next wait
//          (CtrRenderer_endFrame, which waits for the GPU's frame before, the room's texture prefetch, logging).
//   load   the room-change call: the hidden room load in a room-change frame (fl bit 1), close to 0 otherwise. The
//          recorder's own write in that frame is not included (F's prevflush).
//   gpu    the larger of the GPU times of the last frame the GPU had finished at this frame's room-change call and at
//          the next frame's.
//   miss   vblanks the next slot began late. flip: vblanks late flips stayed up past their slots, from this frame's
//          room-change call to the next frame's.
//   The counts and times below cover from the previous frame's room-change call's end to this frame's: this frame's
//   Step, Draw, audio update and room load, the previous frame's end and the wait for this frame's slot.
//   tile   texture page loads, those decoded from the data.win's image (the rest come from RAM or the cache file),
//          and their time (ctr_textures.c, on the game thread).
//   snd    the game thread's sound loads (ctr_audio_system.c prepareAudoSource: reading and copying a sound).
//   sd     FSFILE_Read calls on the game thread (data.win, RomFS, saves, cache files) and their time; tile and sound
//          loads from the SD card count here too.
//   code   bytecode reads from the data.win (DataWinCodeStats). under: music stream underruns.
//   fl     bit 1 room-change frame, bit 2 a value was clipped at 6553.5 ms or 255.
// R, a stretch of frames in one room up to a flush: frames counted, wall time, frames whose Step + Draw took over
//   30 ms (the headroom line, docs/perf/undertale-work-headroom.md), and the stretch's spiking frames in play with their
//   miss and flip totals, counted even when the ring was full (hidden room-load frames not included). A room visit
//   with a flush inside it (HOME) has two.
// F, a flush: why, the frame, the records written and dropped since the last flush, how long the last flush took, and
//   the recorder's own time a frame since the last flush (mean and longest, microseconds; its writes not included).
// A frame across the pause menu, HOME or sleep is not judged (its period includes the pause).

#if defined(CTRPORT_SPIKEREC)

#include <3ds.h>
#include <citro3d.h>
#include <fcntl.h>
#include <reent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ctr_audio_system.h"
#include "ctr_benchmark.h"
#include "ctr_build_id.h"
#include "ctr_frame_pacer.h"
#include "ctr_link_files.h"
#include "ctr_renderer.h"
#include "data_win.h"
#include "log.h"
#include "runner.h"

#define SR_TEXT __attribute__((noinline, section(".text.spikerec_late")))
#define SR_CONST __attribute__((aligned(4), section(".rodata.spikerec_in_text")))
#define SR_BSS __attribute__((section(".bss.spikerec_late")))

#define SPIKEREC_RING 256u
// 0.1 ms in system ticks (SYSCLOCK_ARM11 is 268111856 Hz).
#define SR_TICKS_PER_TENTH 26811u
#define SR_OVER30_TICKS (30ull * 268111856ull / 1000ull)

typedef struct {
    uint32_t frame;
    uint16_t room;
    uint16_t period, step, draw, other, load, gpu, tile, sound, sd; // 0.1 ms
    uint8_t tiles, tilesImage, sdReads, codeReads;
    uint8_t missed, flipMissed, underruns, flags;
} SpikeRecord;
_Static_assert(sizeof(SpikeRecord) == 32, "spike record size");

#define SR_FLAG_ROOMCHANGE 1u
#define SR_FLAG_CLIPPED 2u

// Counters read at the end of each room-change call.
typedef struct {
    uint32_t texLoads, texImage;
    float texMs;
    uint32_t codeLoads;
    uint64_t soundTicks; // the audio system's window total, cleared each stats window
    uint32_t underruns;
    uint32_t sdReads;
    uint64_t sdTicks;
} SrCounters;

static SpikeRecord srRing[SPIKEREC_RING] SR_BSS;

static struct {
    bool hooked, fileFailed, stale, discont, waited, stepSeen, haveAt;
    int fdPlus1;
    void* mainTls;
    aptHookCookie cookie;
    // The phase marks of the frame under way (present and wait: of the frame before, until this frame reaches them).
    uint64_t tStep, tDraw, tAudio, tRoom, tPresent, tWait;
    uint32_t vbStep;
    // The game thread's FSFILE_Read totals.
    uint32_t sdReads;
    uint64_t sdTicks;
    // The frame waiting for its period: judged at the next frame's room-change call.
    // Kept raw: the record is filled (divisions and all) only for a frame that turns out to have missed its slot.
    bool pending;
    uint8_t pendFlags;
    uint16_t pendRoomIndex;
    uint32_t pendFrame, pendVb, pendSlot;
    uint64_t pendStep, pendDrawTicks, pendStepTicks, pendAudio, pendRoom, pendFlush;
    float pendGpu;
    SrCounters pendAt; // the counters at the pending frame's previous call (sr.at: at its own)
    CtrFlipStats flips; // at the last room-change call's end
    SrCounters at;
    Runner* runner;
    uint32_t count, dropped, flushes;
    uint64_t lastFlushTicks;
    // The recorder's own time in the room-change call each frame (its flushes aside), since the last flush.
    uint64_t selfTicks;
    uint32_t selfMax, selfFrames;
    // The stretch of frames in one room since the last flush, and its spikes in play (counted even when the ring is
    // full).
    int32_t segRoom;
    uint32_t segEntry, segFrames, segOver30, segSpikes, segMiss, segFlip;
    uint64_t segStart;
} sr SR_BSS;

static const char srPath[] SR_CONST = CTRPORT_GAME_DIR "/spikes.txt";
static const char srHeader[] SR_CONST = "# spikerec 2 %s %s %s %s ring %u tps %u\n";
static const char srTitle[] SR_CONST = CTRPORT_GAME_TITLE;
#ifdef CTRPORT_TARGET_N3DS
static const char srTarget[] SR_CONST = "n3ds";
#else
static const char srTarget[] SR_CONST = "o3ds";
#endif
static const char srNew[] SR_CONST = "New";
static const char srOld[] SR_CONST = "Old";
static const char srSpike[] SR_CONST = "S %lu %u %s per %u.%u step %u.%u draw %u.%u other %u.%u load %u.%u gpu %u.%u miss %u flip %u "
                                       "tile %u/%u/%u.%u snd %u.%u sd %u/%u.%u code %u under %u fl %u\n";
static const char srVisit[] SR_CONST = "R %ld %s enter %lu frames %lu ms %lu over30 %lu spikes %lu miss %lu flip %lu\n";
static const char srFlushLine[] SR_CONST = "F %s frame %lu records %lu dropped %lu prevflush %u.%u self %lu max %lu\n";
static const char srOpened[] SR_CONST = "spikerec: recording missed slots to %s (ring of %u)\n";
static const char srOpenFailed[] SR_CONST = "spikerec: couldn't open %s; nothing is recorded\n";
static const char srUnknown[] SR_CONST = "?";
static const char srWhyRoom[] SR_CONST = "room";
static const char srWhyExit[] SR_CONST = "exit";
static const char srWhySuspend[] SR_CONST = "suspend";
// The main loop's phase marks (main.c).
static const char srMarkStep[] SR_CONST = "step";
static const char srMarkDraw[] SR_CONST = "draw";
static const char srMarkAudio[] SR_CONST = "audio update";
static const char srMarkRoom[] SR_CONST = "room change";
static const char srMarkPresent[] SR_CONST = "present";
static const char srMarkWait[] SR_CONST = "wait for frame";
static const char srMarkExit[] SR_CONST = "save flush";
static const char srMarkLoad[] SR_CONST = "checking data.win version";

void __real_CtrWatchdog_mark(const char* phase);
void __wrap_CtrWatchdog_mark(const char* phase);
void __real_CtrBench_handleRoomChange(Runner* runner);
void __wrap_CtrBench_handleRoomChange(Runner* runner);
Result __real_FSFILE_Read(Handle handle, u32* bytesRead, u64 offset, void* buffer, u32 size);
Result __wrap_FSFILE_Read(Handle handle, u32* bytesRead, u64 offset, void* buffer, u32 size);
// ctr_audio_system.c's accessor at its end (spikerec builds only).
uint64_t CtrAudioSystem_spikeLoadTicks(AudioSystem* audio);

SR_TEXT static uint16_t srTenths(uint64_t ticks, uint8_t* flags) {
    uint64_t t = ticks / SR_TICKS_PER_TENTH;
    if (t > 65535u) {
        *flags |= SR_FLAG_CLIPPED;
        return 65535u;
    }
    return (uint16_t) t;
}

SR_TEXT static uint16_t srTenthsMs(float ms, uint8_t* flags) {
    if (!(ms > 0.0f)) return 0;
    float t = ms * 10.0f + 0.5f;
    if (t >= 65535.0f) {
        *flags |= SR_FLAG_CLIPPED;
        return 65535u;
    }
    return (uint16_t) t;
}

SR_TEXT static uint8_t srByte(uint32_t n, uint8_t* flags) {
    if (n > 255u) {
        *flags |= SR_FLAG_CLIPPED;
        return 255u;
    }
    return (uint8_t) n;
}

// Vblanks late flips stayed up past the slot, between two flip totals.
SR_TEXT static uint32_t srLateFlips(const CtrFlipStats* before, const CtrFlipStats* now, uint32_t slot) {
    uint32_t late = 0;
    for (uint32_t i = 0; CTR_FLIP_BUCKETS > i; i++) {
        uint32_t vblanks = i + 1;
        if (vblanks > slot) late += (now->count[i] - before->count[i]) * (vblanks - slot);
    }
    return late;
}

SR_TEXT static void srReadCounters(Runner* runner, SrCounters* c, float* gpuMs) {
    CtrRenderStats rs = CtrRenderer_getStats(runner->renderer);
    c->texLoads = rs.textureLoads;
    c->texImage = rs.textureImageLoads;
    c->texMs = rs.textureLoadMs;
    c->codeLoads = runner->dataWin != nullptr ? runner->dataWin->codeStats.loads : 0;
    c->soundTicks = runner->audioSystem != nullptr ? CtrAudioSystem_spikeLoadTicks(runner->audioSystem) : 0;
    c->underruns = runner->audioSystem != nullptr ? CtrAudioSystem_underruns((CtrAudioSystem*) runner->audioSystem) : 0;
    c->sdReads = sr.sdReads;
    c->sdTicks = sr.sdTicks;
    *gpuMs = rs.gpuMs;
}

SR_TEXT static const char* srRoomName(Runner* runner, int32_t room) {
    if (sr.stale || runner == nullptr || runner->dataWin == nullptr || room < 0 || (uint32_t) room >= runner->dataWin->room.count) return srUnknown;
    const char* name = runner->dataWin->room.rooms[room].name;
    return name != nullptr ? name : srUnknown;
}

SR_TEXT static void srWrite(const char* text, int length) {
    if (sr.fdPlus1 <= 0 || length <= 0) return;
    // _write_r, not write(): the release build already links _write_r, while write() would pull a 48 B libc object
    // into the main text and shift every function after it in the playtest build.
    if (_write_r(_REENT, sr.fdPlus1 - 1, text, (size_t) length) != (_ssize_t) length) {
        close(sr.fdPlus1 - 1);
        sr.fdPlus1 = 0;
        sr.fileFailed = true;
    }
}

// Writes the ring, the room stretch so far and a flush line. Game thread only.
SR_TEXT static void srFlush(Runner* runner, const char* why) {
    uint64_t start = svcGetSystemTick();
    if (sr.fdPlus1 <= 0 && !sr.fileFailed) {
        // A lab run with --send-files (ctr_link_files.h) starts the file afresh and sends it to the PC at the exit; a
        // playtest appends, session after session.
        bool lab = CtrLinkFiles_enabled();
        int fd = open(srPath, O_WRONLY | O_CREAT | (lab ? O_TRUNC : O_APPEND), 0666);
        if (fd >= 0 && lab) CtrLinkFiles_note(srPath);
        if (fd < 0) {
            sr.fileFailed = true;
            logInfo(srOpenFailed, srPath);
        } else {
            sr.fdPlus1 = fd + 1;
            logInfo(srOpened, srPath, SPIKEREC_RING);
            bool isNew = false;
            APT_CheckNew3DS(&isNew);
            char line[160];
            srWrite(line, snprintf(line, sizeof line, srHeader, srTitle, srTarget, isNew ? srNew : srOld, CTRPORT_COMMIT,
                                   SPIKEREC_RING, (unsigned) SYSCLOCK_ARM11));
        }
    }
    if (sr.fdPlus1 > 0) {
        // Lines are gathered into one buffer and written a buffer at a time: few, larger SD card writes.
        char buf[1536];
        int used = 0;
        for (uint32_t i = 0; sr.count > i; i++) {
            const SpikeRecord* r = &srRing[i];
            if (used > (int) sizeof buf - 320) {
                srWrite(buf, used);
                used = 0;
            }
            int n = snprintf(buf + used, sizeof buf - (size_t) used, srSpike, (unsigned long) r->frame, (unsigned) r->room,
                             srRoomName(runner, r->room), r->period / 10u, r->period % 10u, r->step / 10u, r->step % 10u, r->draw / 10u,
                             r->draw % 10u, r->other / 10u, r->other % 10u, r->load / 10u, r->load % 10u, r->gpu / 10u, r->gpu % 10u,
                             (unsigned) r->missed, (unsigned) r->flipMissed, (unsigned) r->tiles, (unsigned) r->tilesImage, r->tile / 10u,
                             r->tile % 10u, r->sound / 10u, r->sound % 10u, (unsigned) r->sdReads, r->sd / 10u, r->sd % 10u,
                             (unsigned) r->codeReads, (unsigned) r->underruns, (unsigned) r->flags);
            if (n > 0 && n < (int) sizeof buf - used) used += n;
        }
        if (used > (int) sizeof buf - 320) {
            srWrite(buf, used);
            used = 0;
        }
        if (sr.segFrames > 0) {
            int n = snprintf(buf + used, sizeof buf - (size_t) used, srVisit, (long) sr.segRoom, srRoomName(runner, sr.segRoom),
                             (unsigned long) sr.segEntry, (unsigned long) sr.segFrames,
                             (unsigned long) ((svcGetSystemTick() - sr.segStart) / (SYSCLOCK_ARM11 / 1000u)), (unsigned long) sr.segOver30,
                             (unsigned long) sr.segSpikes, (unsigned long) sr.segMiss, (unsigned long) sr.segFlip);
            if (n > 0 && n < (int) sizeof buf - used) used += n;
        }
        uint32_t last = (uint32_t) (sr.lastFlushTicks / SR_TICKS_PER_TENTH);
        // self: the recorder's mean and longest time a frame, in microseconds.
        uint32_t perUs = SYSCLOCK_ARM11 / 1000000u;
        uint32_t self = sr.selfFrames > 0 ? (uint32_t) (sr.selfTicks / sr.selfFrames) / perUs : 0;
        int n = snprintf(buf + used, sizeof buf - (size_t) used, srFlushLine, why, (unsigned long) (runner != nullptr ? runner->frameCount : 0),
                         (unsigned long) sr.count, (unsigned long) sr.dropped, last / 10u, last % 10u, (unsigned long) self,
                         (unsigned long) (sr.selfMax / perUs));
        if (n > 0 && n < (int) sizeof buf - used) used += n;
        srWrite(buf, used);
    }
    sr.count = 0;
    sr.dropped = 0;
    sr.selfTicks = 0;
    sr.selfMax = 0;
    sr.selfFrames = 0;
    sr.segFrames = 0;
    sr.segOver30 = 0;
    sr.segSpikes = 0;
    sr.segMiss = 0;
    sr.segFlip = 0;
    sr.stale = false;
    sr.flushes++;
    sr.lastFlushTicks = svcGetSystemTick() - start;
}

SR_TEXT static void srOnApt(APT_HookType hook, void* param) {
    (void) param;
    if (hook != APTHOOK_ONSUSPEND && hook != APTHOOK_ONSLEEP && hook != APTHOOK_ONEXIT) return;
    sr.discont = true;
    // Only the game thread touches the ring; HOME's suspend comes there (aptMainLoop).
    if (hook == APTHOOK_ONSUSPEND && getThreadLocalStorage() == sr.mainTls && sr.runner != nullptr) srFlush(sr.runner, srWhySuspend);
}

// The frame waiting for its period, judged now that the next frame's Step has begun.
SR_TEXT static void srJudge(Runner* runner) {
    if (!sr.pending) return;
    sr.pending = false;
    if (sr.discont || !sr.stepSeen) return;
    uint32_t vblanks = sr.vbStep - sr.pendVb;
    uint32_t slot = sr.pendSlot;
    CtrFlipStats flips;
    CtrPacer_getFlipTotals(&flips);
    uint32_t lateFlips = srLateFlips(&sr.flips, &flips, slot);
    if (slot >= vblanks && lateFlips == 0) return;
    uint32_t missed = vblanks > slot ? vblanks - slot : 0;
    if (!(sr.pendFlags & SR_FLAG_ROOMCHANGE)) {
        sr.segSpikes++;
        sr.segMiss += missed;
        sr.segFlip += lateFlips;
    }
    if (sr.count >= SPIKEREC_RING) {
        sr.dropped++;
        return;
    }
    SpikeRecord* r = &srRing[sr.count++];
    const SrCounters* c = &sr.at;
    const SrCounters* a = &sr.pendAt;
    memset(r, 0, sizeof *r);
    r->frame = sr.pendFrame;
    r->room = sr.pendRoomIndex;
    r->flags = sr.pendFlags;
    r->step = srTenths(sr.pendStepTicks, &r->flags);
    r->draw = srTenths(sr.pendDrawTicks, &r->flags);
    r->tiles = srByte(c->texLoads - a->texLoads, &r->flags);
    r->tilesImage = srByte(c->texImage - a->texImage, &r->flags);
    r->tile = srTenthsMs(c->texMs - a->texMs, &r->flags);
    r->sound = srTenths(c->soundTicks >= a->soundTicks ? c->soundTicks - a->soundTicks : c->soundTicks, &r->flags);
    r->sdReads = srByte(c->sdReads - a->sdReads, &r->flags);
    r->sd = srTenths(c->sdTicks - a->sdTicks, &r->flags);
    r->codeReads = srByte(c->codeLoads - a->codeLoads, &r->flags);
    r->underruns = srByte(c->underruns - a->underruns, &r->flags);
    float gpuMs = CtrRenderer_getStats(runner->renderer).gpuMs;
    if (sr.pendGpu > gpuMs) gpuMs = sr.pendGpu;
    r->gpu = srTenthsMs(gpuMs, &r->flags);
    r->missed = srByte(missed, &r->flags);
    r->flipMissed = srByte(lateFlips, &r->flags);
    r->period = srTenths(sr.tStep - sr.pendStep, &r->flags);
    // The audio update and the room-change call's mark, then from that call's end to the wait (present and wait marks
    // of that frame: this frame has not reached its own yet).
    uint64_t other = sr.pendRoom - sr.pendAudio;
    if (sr.tWait > sr.tPresent) other += sr.tWait - sr.tPresent;
    r->other = srTenths(other, &r->flags);
    uint64_t load = sr.tPresent > sr.pendRoom ? sr.tPresent - sr.pendRoom : 0;
    load = load > sr.pendFlush ? load - sr.pendFlush : 0;
    r->load = srTenths(load, &r->flags);
}

SR_TEXT void __wrap_CtrBench_handleRoomChange(Runner* runner) {
    uint64_t selfStart = svcGetSystemTick();
    if (!sr.hooked) {
        sr.hooked = true;
        aptHook(&sr.cookie, srOnApt, nullptr);
    }
    if (runner != sr.runner) {
        // A new game (DELTARUNE's chapter switch): the ring's room numbers belong to the last one.
        if (sr.runner != nullptr) {
            sr.stale = sr.count > 0 || sr.segFrames > 0;
            sr.discont = true;
        }
        sr.runner = runner;
        sr.pending = false;
        sr.haveAt = false;
    }
    srJudge(runner);
    bool changing = runner->pendingRoom != -1;
    int32_t room = runner->currentRoomIndex;
    uint32_t frame = (uint32_t) runner->frameCount;
    uint64_t stepTicks = sr.stepSeen ? sr.tDraw - sr.tStep : 0;
    uint64_t drawTicks = sr.stepSeen ? sr.tAudio - sr.tDraw : 0;
    if (sr.segFrames == 0) {
        sr.segRoom = room;
        sr.segEntry = frame;
        sr.segStart = svcGetSystemTick();
    }
    sr.segFrames++;
    if (stepTicks + drawTicks > SR_OVER30_TICKS) sr.segOver30++;
    GMLReal speed = Runner_getEffectiveGameSpeed(runner);
    uint32_t slot = speed > 0.0 ? (uint32_t) (60.0 / speed + 0.5) : 2u;
    if (slot == 0) slot = 1;

    uint64_t selfPause = svcGetSystemTick();
    __real_CtrBench_handleRoomChange(runner);

    uint64_t flushTicks = 0;
    if (changing) {
        srFlush(runner, srWhyRoom);
        flushTicks = sr.lastFlushTicks;
    }
    uint64_t selfResume = svcGetSystemTick();
    // This frame's counters, from the last call's end to this one's.
    SrCounters c;
    float gpuMs;
    srReadCounters(runner, &c, &gpuMs);
    CtrPacer_getFlipTotals(&sr.flips);
    if (sr.stepSeen && !sr.discont && sr.haveAt) {
        sr.pendFrame = frame;
        sr.pendRoomIndex = (uint16_t) room;
        sr.pendFlags = changing ? SR_FLAG_ROOMCHANGE : 0;
        sr.pendStepTicks = stepTicks;
        sr.pendDrawTicks = drawTicks;
        sr.pendAt = sr.at;
        sr.pending = true;
        sr.pendStep = sr.tStep;
        sr.pendVb = sr.vbStep;
        sr.pendAudio = sr.tAudio;
        sr.pendRoom = sr.tRoom;
        sr.pendFlush = flushTicks;
        sr.pendSlot = slot;
        sr.pendGpu = gpuMs;
    }
    sr.at = c;
    sr.haveAt = true;
    sr.discont = false;
    sr.stepSeen = false;
    uint32_t self = (uint32_t) (selfPause - selfStart + svcGetSystemTick() - selfResume);
    sr.selfTicks += self;
    sr.selfFrames++;
    if (self > sr.selfMax) sr.selfMax = self;
}

SR_TEXT void __wrap_CtrWatchdog_mark(const char* phase) {
    __real_CtrWatchdog_mark(phase);
    // The main loop's marks are string literals; most checks end at the first character.
    char c = phase[0];
    if (c == 's') {
        if (strcmp(phase, srMarkStep) == 0) {
            if (sr.mainTls == nullptr) sr.mainTls = getThreadLocalStorage();
            if (!sr.waited && sr.stepSeen) sr.discont = true; // a Step without a room-change call: not the main loop
            sr.tStep = svcGetSystemTick();
            sr.vbStep = C3D_FrameCounter(0);
            sr.stepSeen = true;
            sr.waited = false;
        } else if (strcmp(phase, srMarkExit) == 0 && sr.runner != nullptr) {
            srFlush(sr.runner, srWhyExit);
            sr.runner = nullptr;
            sr.pending = false;
        }
    } else if (c == 'c') {
        // A game loading (DELTARUNE's next chapter): the last game's runner is gone, and its room numbers with it.
        if (strcmp(phase, srMarkLoad) == 0 && sr.runner != nullptr) {
            sr.stale = sr.count > 0 || sr.segFrames > 0;
            sr.runner = nullptr;
            sr.pending = false;
            sr.haveAt = false;
        }
    } else if (c == 'd') {
        if (strcmp(phase, srMarkDraw) == 0) sr.tDraw = svcGetSystemTick();
    } else if (c == 'a') {
        if (strcmp(phase, srMarkAudio) == 0) sr.tAudio = svcGetSystemTick();
    } else if (c == 'r') {
        if (strcmp(phase, srMarkRoom) == 0) sr.tRoom = svcGetSystemTick();
    } else if (c == 'p') {
        if (strcmp(phase, srMarkPresent) == 0) sr.tPresent = svcGetSystemTick();
    } else if (c == 'w') {
        if (strcmp(phase, srMarkWait) == 0) {
            // A second wait without a Step between: the loop went round paused (the options menu), so the next period
            // includes the pause.
            if (sr.waited) sr.discont = true;
            else sr.tWait = svcGetSystemTick();
            sr.waited = true;
        }
    }
}

SR_TEXT Result __wrap_FSFILE_Read(Handle handle, u32* bytesRead, u64 offset, void* buffer, u32 size) {
    if (sr.mainTls == nullptr || getThreadLocalStorage() != sr.mainTls) return __real_FSFILE_Read(handle, bytesRead, offset, buffer, size);
    uint64_t start = svcGetSystemTick();
    Result result = __real_FSFILE_Read(handle, bytesRead, offset, buffer, size);
    sr.sdTicks += svcGetSystemTick() - start;
    sr.sdReads++;
    return result;
}

#endif
