// Room-entry attribution trace (roomload), compiled only into profile builds (CTRPORT_RENDER_TIMING).
//
// Each frame, after the room change and the room's texture prefetch, main.c calls RoomloadTrace_frame. It logs a
// line for the first ROOMLOAD_TRACE_FRAMES frames of each room, and for any later frame that was slow or read
// anything from the SD card:
//
//     [rl] f <frame> <room> +<frames in room> int <ms> main <ms> step <ms> draw <ms> other <ms> tex <loads>/<ms>
//          code <reads>/<KB>/<ms> inst <instances>
//
// int is the interval from the previous frame's start to this one's (the previous frame's whole period, including
// its wait for the vblank); main is this frame's CPU time up to the call; other is main less Step and Draw (the room
// change, the prefetch, the end of the frame). Each bytecode read is also logged when it happens:
//
//     [rl] code <frame> <name> <bytes> B <ms> ms
//
// Release builds compile neither the calls nor this file's body.

#ifdef CTRPORT_RENDER_TIMING

#include "z_roomload_trace.h"

#include "data_win.h"
#include "gettime.h"
#include "log.h"
#include "runner.h"
#include "stb_ds.h"

#define ROOMLOAD_TRACE_FRAMES 8
#define ROOMLOAD_SLOW_MS 36.0

static struct {
    int32_t room;
    int32_t framesInRoom;
    uint64_t lastFrameStart;
    uint32_t texLoads;
    float texMs;
    uint32_t codeLoads;
    uint64_t codeBytes;
    uint64_t codeNanos;
    int32_t frame;
} trace = {-1, 0, 0, 0, 0.0f, 0, 0, 0, 0};

void RoomloadTrace_frame(Runner* runner, Renderer* renderer, uint64_t frameStart, double stepMs, double drawMs) {
    double mainMs = (double) (nowNanos() - frameStart) / 1e6;
    double intervalMs = trace.lastFrameStart != 0 ? (double) (frameStart - trace.lastFrameStart) / 1e6 : 0.0;
    trace.lastFrameStart = frameStart;
    if (runner->currentRoomIndex != trace.room) {
        trace.room = runner->currentRoomIndex;
        trace.framesInRoom = 0;
    } else {
        trace.framesInRoom++;
    }
    trace.frame = runner->frameCount;
    CtrRenderStats rs = CtrRenderer_getStats(renderer);
    const DataWinCodeStats* cs = &runner->dataWin->codeStats;
    uint32_t texLoads = rs.textureLoads - trace.texLoads;
    double texMs = (double) (rs.textureLoadMs - trace.texMs);
    uint32_t codeLoads = cs->loads - trace.codeLoads;
    uint64_t codeBytes = cs->loadedBytes - trace.codeBytes;
    double codeMs = (double) (cs->loadNanos - trace.codeNanos) / 1e6;
    trace.texLoads = rs.textureLoads;
    trace.texMs = rs.textureLoadMs;
    trace.codeLoads = cs->loads;
    trace.codeBytes = cs->loadedBytes;
    trace.codeNanos = cs->loadNanos;
    if (ROOMLOAD_TRACE_FRAMES > trace.framesInRoom || mainMs > ROOMLOAD_SLOW_MS || intervalMs > ROOMLOAD_SLOW_MS || texLoads > 0 || codeLoads > 0) {
        logInfo("[rl] f %d %s +%d int %.1f main %.1f step %.1f draw %.1f other %.1f tex %u/%.1f code %u/%u/%.1f inst %d\n",
                runner->frameCount, runner->currentRoom != nullptr ? runner->currentRoom->name : "?", (int) trace.framesInRoom, intervalMs,
                mainMs, stepMs, drawMs, mainMs - stepMs - drawMs, (unsigned) texLoads, texMs, (unsigned) codeLoads,
                (unsigned) (codeBytes / 1024), codeMs, (int) arrlen(runner->instances));
    }
}

void RoomloadTrace_codeRead(const char* name, uint32_t bytes, uint64_t startNanos) {
    logInfo("[rl] code %d %s %u B %.1f ms\n", (int) trace.frame, name != nullptr ? name : "?", (unsigned) bytes,
            (double) (nowNanos() - startNanos) / 1e6);
}

#endif
