// Frame pacing against the top screen's vblank count.
//
// Each frame gets a slot of whole vblanks (two at 30 fps) counted from the
// vblank its slot began on, and is shown on the vblank that ends the slot.
// citro3d alone would show a frame on whichever vblank followed the GPU
// finishing it, so a frame done early was shown a vblank early and the one
// before it for a vblank less: the display alternated between one and three
// vblanks a frame while the frame rate stayed at 30. Here a frame finished
// early waits for the slot's last vblank before it is flipped in. While the
// GPU misses that deadline, the late picture may stay longer. No extra
// input-delay vblank is added to subsequent pictures (ctr_frame_pacer.c).
#ifndef CTRPORT_CTR_FRAME_PACER_H
#define CTRPORT_CTR_FRAME_PACER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <3ds.h>

void CtrPacer_init(void);

// The game's frame rate. Each frame then gets 60 / fps vblanks (never fewer than one).
void CtrPacer_setFrameRate(float fps);

// Waits for the frame's slot to begin. Call right before the frame begins,
// and CtrPacer_frameBegun once the GPU is done with the frame before.
//
// A frame that starts after its slot has begun starts the schedule over, and
// one the GPU finished late may move the next slot to its actual flip.
// Every later picture keeps the same low-delay deadline; no adaptive
// cushion is armed or recovered.
//
// With catchUp, the slots keep to their schedule instead (they never move),
// so the game keeps its speed: a frame less than a slot behind is shown as soon as it is done,
// and one a whole slot behind goes unshown, up to skipMax in a row (the
// function then returns false without waiting). Skipping stops for a few
// seconds after a skipped frame took a whole slot regardless, since its step
// and draw events alone are the slow part. A frame two slots behind (a room
// loading) starts the schedule over.
bool CtrPacer_waitForSlot(bool catchUp, int32_t skipMax);
void CtrPacer_frameBegun(void);
// Skip policy 1: no pause in skipping after a skipped frame took a whole slot, and frames up to
// four slots behind are skipped (rather than two) before the schedule starts over. 0 is the default above.
void CtrPacer_setSkipPolicy(int32_t policy);
// Fixed 2:1 frame skip: in place of CtrPacer_waitForSlot for a frame that goes unshown regardless. It doesn't wait;
// its slot passes, and the next frame's slot follows it.
void CtrPacer_skipSlot(void);
// Fixed 2:1 overlap (frame_skip_overlap): an unshown frame doesn't wait for the shown frame
// before it, so the GPU runs that one during the unshown frame and the next shown frame's step and draw. A shown
// frame is late if it flips after the end of its own slot. No extra adaptive
// input delay is added, including in this explicit overlap mode.
void CtrPacer_setHalfRateOverlap(bool on);

// Top screen flips by how many vblanks the flip before stayed up: count[i] is i + 1 vblanks, the last bucket that
// many or more. sum and sumSq (in vblanks) give the mean and spread.
#define CTR_FLIP_BUCKETS 8
typedef struct {
    uint32_t count[CTR_FLIP_BUCKETS];
    uint32_t flips;
    uint64_t sum, sumSq;
} CtrFlipStats;
// The flips since CtrPacer_init.
void CtrPacer_getFlipTotals(CtrFlipStats* out);

// Auto's levels (frame_skip 1, 3 and 4; docs/perf/deepskip.md, phase 3). Level 0 shows every step, 1 two of three
// (20 fps at 30 steps a second), 2 one of two (15), 3 one of three (10). Logic slots keep to a schedule that never
// follows a late frame; shown frames flip on an even beat of 2, 3, 4 or 6 vblanks.
#define CTR_LEVEL_COUNT 4
typedef struct {
    // Top screen flips by level (CtrFlipStats), and the longest any stayed up, in vblanks.
    CtrFlipStats flips[CTR_LEVEL_COUNT];
    uint32_t maxVblanks[CTR_LEVEL_COUNT];
    // Steps run at each level, level changes, and schedule resets (a step 3 slots behind).
    uint32_t steps[CTR_LEVEL_COUNT];
    uint32_t changes, resets;
} CtrLevelTotals;
// On: CtrPacer_levelWait replaces CtrPacer_waitForSlot. automatic picks the level; otherwise it stays at fixedLevel.
void CtrPacer_setLevelMode(bool on, bool automatic, int32_t fixedLevel);
// A new chapter: level 0, costs measured afresh.
void CtrPacer_levelReset(void);
// A new room: the level stays; no decision for a second while its textures load.
void CtrPacer_levelRoomChanged(void);
// Waits for the step's slot if it is shown and early. Returns false if the step goes unshown (never when !mayskip).
bool CtrPacer_levelWait(bool mayskip);
// The GPU's time on the frame before this one, read after waiting for it.
void CtrPacer_levelGpu(float gpuMs);
void CtrPacer_getLevelTotals(CtrLevelTotals* out);
// Logs a [level] line: the level, its costs and projections.
void CtrPacer_logLevelStats(void);
// The performance test's line for a scene: steps, flips (mean, p95, max) per level, changes a minute.
void CtrPacer_logLevels(const char* id, const CtrLevelTotals* start, double seconds);
// Test hook (pacestress.txt "cpu<ms>", "cpuskip<ms>"): holds the main thread up that long on each shown or unshown step.
void CtrPacer_stressStep(bool shown);

// Bracket C3D_FrameEnd for the frame that shows on the screens; a frame
// CtrPacer_waitForSlot said to skip calls neither.
void CtrPacer_frameEnding(void);
void CtrPacer_frameEnded(float gpuMs);

// Logs a [pace] line covering the frames since the last call, then starts over.
void CtrPacer_logStats(void);

// From the top screen's vblank callback (GSP event thread): flips what was
// held for this vblank.
void CtrPacer_onVBlank(void);

// Whether a screen's last flip has been shown, so its other buffer can be
// written: no flip pending in GSP, and none held here.
bool CtrPacer_screenReady(gfxScreen_t screen);

// libctru's back buffer (gfxGetFramebuffer) and the buffer on screen, read
// with no flip under way on another thread.
u8* CtrPacer_backFramebuffer(gfxScreen_t screen, gfx3dSide_t side);
u8* CtrPacer_shownFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16* width, u16* height);

// Test hook: if the file exists, "jitter<ms>" holds the main thread up for up
// to that many milliseconds (at random) before each frame's end, and "nogate"
// lets screen transfers through without waiting for the last flip (the tear
// check's negative control; see CtrGpu_setGateEnabled).
void CtrPacer_loadStress(const char* path);

// For hang reports (ctr_watchdog.c): the pacer's state, read without its lock.
void CtrPacer_describeStall(char* out, size_t size);

#endif
