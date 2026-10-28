// citro3d renderer for the Butterscotch engine.
//
// The game draws into an off-screen application surface (a render target
// texture at the game's window size, kept at the top screen's resolution with
// native drawing). Once per frame the surface is presented according to the
// display settings (ctr_display.h): fitted to the 400 or 800 pixel wide top
// screen, per eye in stereo HD, and with small text magnified on the bottom
// screen. Presentation never changes what the game draws.
#ifndef CTRPORT_CTR_RENDERER_H
#define CTRPORT_CTR_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#include "ctr_display.h"
#include "file_system.h"
#include "renderer.h"

typedef struct {
    uint32_t drawCalls;
    uint32_t vertices;
    uint32_t splits;
    float gpuMs;
    // Texture cache totals since the data.win was loaded.
    uint32_t textureLoads; // page loads (from RAM, the cache file or the image)
    uint32_t textureImageLoads; // those from the image: pages the texture cache doesn't have yet
    uint32_t textureEvictions;
    uint32_t packedReclaims, packedReclaimBytes;
    uint32_t promotions, failedPromotions;
    float textureLoadMs;
    // Smart smoothing totals since start: boxes averaged at present, and
    // their application surface pixels.
    uint32_t smartBoxes;
    uint64_t smartPixels;
    // Frames presented from a half-resolution application surface (native
    // drawing) and from a full one, since the renderer started.
    uint32_t nativeFrames;
    uint32_t fullFrames;
} CtrRenderStats;

// appW x appH is the application surface size. screenshotDir receives shot_<frame>.bmp files, and the
// player's shot_<number>.bmp files numbered after the existing shots.
Renderer* CtrRenderer_create(FileSystem* fileSystem, int32_t appW, int32_t appH, const char* screenshotDir);

// Bracket each main loop iteration. Draw calls made outside the bracket are dropped.
void CtrRenderer_beginFrame(Renderer* renderer);
void CtrRenderer_endFrame(Renderer* renderer);
// Draws the current frame but doesn't show it: the next CtrRenderer_endFrame
// leaves both screens as they are.
void CtrRenderer_skipPresent(Renderer* renderer);

// Frame skip: late frames keep the game at its speed, and up to maxInRow
// frames in a row that start a whole frame late may go unshown (0 turns it
// all off; see CtrPacer_waitForSlot). Applies from the next
// CtrRenderer_beginFrame. A skipped frame runs the game's Draw events as
// usual, but what they draw on the application surface is dropped and the
// screens keep the last frame. Draws on other surfaces still happen. While
// the game reads the application surface's pixels, nothing is skipped.
void CtrRenderer_setFrameSkip(Renderer* renderer, int32_t maxInRow);
// Whether the frame begun last is skipped.
bool CtrRenderer_frameSkipped(Renderer* renderer);
// Auto's policy (CtrPacer_setSkipPolicy): 0 as above, 1 keeps skipping while behind.
void CtrRenderer_setFrameSkipPolicy(Renderer* renderer, int32_t policy);
// Fixed 2:1: with on, frames are shown without catching up, and with skipNext the next
// CtrRenderer_beginFrame's frame goes unshown without waiting for a slot (unless the game reads the application
// surface or a screenshot is due). The caller names every other frame.
void CtrRenderer_setHalfRate(Renderer* renderer, bool on, bool skipNext);
// Auto's levels (frame_skip 1 with frame_skip_auto, 3 and 4; docs/perf/deepskip.md phase 3): mode 0 off, 1 the pacer
// picks the level, 2 it stays at fixedLevel (1: 20 shown fps, 2: 15). Frames go unshown only while setFrameSkip allows.
void CtrRenderer_setFrameSkipLevels(Renderer* renderer, int32_t mode, int32_t fixedLevel);
// Skipped frames still draw on the application surface (deep frame skip 3).
void CtrRenderer_setFrameSkipKeepApp(Renderer* renderer, bool on);
// Fixed 2:1 overlap (frame_skip_overlap): an unshown frame doesn't wait for the shown frame
// before it to leave the GPU; its own GPU work, if any, waits behind that frame in the GPU's queue
// (CtrGpu_frameEndBehind), and the next frame's step starts at once. The pacer then lets shown frames trail their
// slots by up to two slots (CtrPacer_setHalfRateOverlap).
void CtrRenderer_setFrameSkipOverlap(Renderer* renderer, bool on);
// Record and replay (frame_skip_record, docs/perf/deepskip.md phase 5): a skipped frame's muted draws on surfaces kept
// from earlier frames are recorded and drawn when something reads the surface, instead of left out. Off: left out.
void CtrRenderer_setFrameSkipRecord(Renderer* renderer, bool on);

typedef struct {
    uint32_t skipped;    // frames
    uint32_t dropped;    // draws on the application surface left out
    uint32_t staleReads; // reads of the application surface in skipped frames, which saw the frame before
    uint32_t muted;      // muted draws left out on other surfaces (deep frame skip)
    // Skipped frames that still queued GPU work (draws, command lists, fills, copies), their draw calls, GX commands
    // before the last command list (fills and copies among them), and those handed to the GPU behind the frame
    // before (frame_skip_overlap).
    uint32_t workFrames, workDraws, workCommands, workFills, workCopies, behind;
} CtrSkipStats;

// Record and replay's counts: draws recorded, recordings drawn (and their draws), dropped unread (a whole clear or the
// surface going), surfaces whose draws went straight on for a full recording or a draw that can't be recorded,
// recordings drawn into a temporary surface, lost (needed outside a frame, or no temporary surface), drawn from inside
// a draw (should stay 0), and the most memory recordings held at once (bytes).
typedef struct {
    uint32_t recOps, replays, replayOps, discards, overflows, unrecordable, snapshots, lost, lateSyncs;
    uint32_t peakBytes;
} CtrSkipRecStats;

// Since the last call; peakBytes since the last call.
CtrSkipRecStats CtrRenderer_takeSkipRecStats(Renderer* renderer);
// Since the renderer was made; peakBytes since CtrRenderer_resetSkipRecPeak.
CtrSkipRecStats CtrRenderer_getSkipRecTotals(Renderer* renderer);
void CtrRenderer_resetSkipRecPeak(Renderer* renderer);

// The counts since the last call.
CtrSkipStats CtrRenderer_takeSkipStats(Renderer* renderer);
// The counts since the renderer was made (CtrRenderer_takeSkipStats doesn't reset them).
CtrSkipStats CtrRenderer_getSkipTotals(Renderer* renderer);

// Display modes. A change takes effect at the next CtrRenderer_beginFrame.
// Settings the console can't show are switched off. While the bottom mode
// isn't BOTTOM_MODE_MAGNIFIER the renderer leaves the bottom screen alone.
void CtrRenderer_setDisplay(Renderer* renderer, const CtrDisplaySettings* settings);
CtrDisplaySettings CtrRenderer_getDisplay(Renderer* renderer);
// Lends the bottom screen to the consoles (for the options menu) regardless
// of the settings, or takes it back.
void CtrRenderer_yieldBottomScreen(Renderer* renderer, bool yield);
// Whether the renderer draws the bottom screen with the current settings.
bool CtrRenderer_ownsBottomScreen(Renderer* renderer);
// Whether the bottom screen shows dialogue (display.dialogueBottom), which
// the stats console then leaves alone.
bool CtrRenderer_dialogueShown(Renderer* renderer);
CtrDisplayCaps CtrRenderer_getDisplayCaps(Renderer* renderer);

// Limits presentation to fps frames per second (vsync based).
void CtrRenderer_setFrameRate(Renderer* renderer, float fps);

// Logs what the GPU runs for the next frame, and its time ("[gputrace]"), in
// profile builds (CTRPORT_RENDER_TIMING); elsewhere does nothing.
void CtrRenderer_traceNextFrame(Renderer* renderer);

// flickerprobe.txt, a diagnostic that is off without the file: on each frame
// of a window ("frames 20-200" by default) the GPU copies the application
// surface's text box (x 80-560, y 280-480 of a 640x480 surface) to memory
// after the frame's draws, and its white pixels (red, green and blue all over
// 200) are counted once the GPU is done, without waiting for it. The counts
// go to flickerprobe.log in the screenshot folder. Words in the file switch
// GPU paths off: nooverlap, slow (no shortcut draws), nomulti (no
// multi-texture batches), flush (the command buffer and vertices are also
// written back from the CPU's cache by this core at each frame's end); all
// is the four. unitsoff (not in all) undoes the multi-texture fix: units 1
// and 2 are switched off after each multi-texture batch, as before.
// With "eyes" the box is Omega Flowey's left upper eye (x 144-232, y
// 104-168, in the final battle) and its warm pixels count, its eyelid's colours: a
// frame is present from 950 (about 1235 with the eyelid drawn, 665 without
// it, in Azahar), and extra over 1.2 times the median present count.
// Returns false if there is no such file.
bool CtrRenderer_loadFlickerProbe(Renderer* renderer, const char* path);

// The same with the words given (--flicker-probe); source names them in the log.
void CtrRenderer_startFlickerProbe(Renderer* renderer, const char* words, const char* source);

// True once the probe's window is over and its counts are in the log (as
// "flickerprobe <tag> frames: present 20-33, absent 34-36, ..." and "counts"
// lines and a summary, so an unattended run's log carries them) and in
// flickerprobe-<tag>.log. With --exit-after the run then leaves.
bool CtrRenderer_flickerProbeDone(Renderer* renderer);

// Leaving before the window is over (--timeout, quitting): reports what was
// counted, the frames not reached as uncounted. Does nothing otherwise.
void CtrRenderer_endFlickerProbe(Renderer* renderer);

// The texture probe, a diagnostic (see "Texture probe" in ctr_renderer.c):
// probeTextures has tiles and palette copies note their texels' hash from
// then on; probeNextFrame checks them all at the next frame's start
// ("[texcheck]"), and with batches lists the frame's batches and the
// textures they draw with ("[texprobe]").
void CtrRenderer_probeTextures(Renderer* renderer);
void CtrRenderer_probeNextFrame(Renderer* renderer, bool batches);

// Saves the application surface to a BMP at the start of the next frame, as shot_<frame>.bmp.
void CtrRenderer_requestScreenshot(Renderer* renderer);
// The player's screenshot (L+SELECT): the same, at the next free shot_<number>.bmp, so shots never overwrite each
// other across boots. An automatic request for the same frame wins and keeps the frame-numbered name.
void CtrRenderer_requestPlayerScreenshot(Renderer* renderer);
// The same, with suffix after the frame number in the file names (top_00405p.bmp), for a second shot of a frame
// number: one taken while paused, say.
void CtrRenderer_requestScreenshotSuffixed(Renderer* renderer, const char* suffix);
// True if the last beginFrame attempted a full screenshot. Its incoming frame
// interval includes capture I/O; benchmark timing must leave that interval out.
bool CtrRenderer_screenshotCaptured(Renderer* renderer);
// Also covers the notice: painting it waits for a bottom-screen vblank. The
// interval in which the notice expires is affected too, before normal waits resume.
bool CtrRenderer_screenshotTimingAffected(Renderer* renderer);
// Saves only the application surface, as it was left by the frame that just
// ended, to a BMP at path in the next CtrRenderer_beginFrame, after its wait
// for the frame slot, so the frame before keeps its own time. The main loop
// counts CtrRenderer_surfaceShotMs (what the save took in the frame begun
// last, 0 in frames without one) to the new frame, which a test leaves out.
// The few frames after it aren't skipped for being late.
void CtrRenderer_requestSurfaceShot(Renderer* renderer, const char* path);
double CtrRenderer_surfaceShotMs(Renderer* renderer);

// Stats of the last finished frame.
CtrRenderStats CtrRenderer_getStats(Renderer* renderer);
void CtrRenderer_logStats(Renderer* renderer);
#ifdef CTRPORT_RENDER_TIMING
// The texture cache's packed set and page loads, with label (CtrTextures_logPackedSet).
void CtrRenderer_logPackedSet(Renderer* renderer, const char* label);
#endif

// Loads the texture pages the current room starts with. Call after a room
// change, outside the beginFrame/endFrame bracket.
void CtrRenderer_prefetchRoom(Renderer* renderer);

// Video frames (ctr_video.c). A video surface is an ordinary surface whose
// pixels come from the decoder rather than from drawing. Surfaces larger
// than the GPU can sample keep every (1 << *shift)th pixel of every such row,
// and are drawn at their full size. -1 if there is no memory for one.
int32_t CtrRenderer_createVideoSurface(Renderer* renderer, int32_t width, int32_t height, int32_t* shift);
// Replaces the surface's pixels with a width x height frame (multiples of 8)
// of linear RGB565, halved in both directions by averaging when `halve` is
// set. The GPU reads `pixels` after the frame ends: they must stay as they
// are until CtrRenderer_fenceDone(*fence). Only inside a frame. False if the
// frame couldn't be queued (no memory, or the surface can't take it now).
bool CtrRenderer_uploadVideoFrame(Renderer* renderer, int32_t surfaceId, const void* pixels, int32_t width, int32_t height, bool halve,
                                  uint32_t* fence);
// Fills the surface with an opaque colour. Only inside a frame.
bool CtrRenderer_fillVideoSurface(Renderer* renderer, int32_t surfaceId, uint8_t red, uint8_t green, uint8_t blue);
// Whether the GPU is done with what was queued when *fence was set.
bool CtrRenderer_fenceDone(Renderer* renderer, uint32_t fence);
// Frees the memory uploads keep between frames. Call once a video is over.
void CtrRenderer_releaseVideo(Renderer* renderer);

// Stops background texture packing. Call before exiting.
void CtrRenderer_stopBackgroundWork(Renderer* renderer);

// Debug (--force-materialize, ctr_launch_args.h): every surface the game makes
// from then on starts recorded, as one VRAM has no room for does, and is drawn
// into a texture from its quads at its first draw (materializeRecorded), so
// that path runs on any scene. Off by default.
void CtrRenderer_setForceMaterialize(Renderer* renderer, bool on);

// The objects whose surfaces stay at full resolution with native drawing,
// because their code reads surface pixels back or saves them: one flag per
// object index (VM_findSurfaceReaders). The renderer takes and frees the array.
void CtrRenderer_setSurfaceReaders(Renderer* renderer, bool* readers, uint32_t count);

// Test (--slider-at): the 3D slider's position, 0 to 1, from the next frame
// on, in place of the real one; below 0 gives the real one back.
void CtrRenderer_setSliderOverride(Renderer* renderer, float position);

// Runs the self-test `name` (--selftest, ctr_launch_args.h) in frames of its
// own, before the game's first, and logs its results ("selftest ..." lines).
// "smart": one-pixel patterns through each smoothing path, the top screen
// read back. "fcram_target": render targets in linear memory against VRAM
// ones, for correctness (byte comparisons) and speed (GPU ms per pass).
// "mirror_cancel": a tile's VRAM copy dropped before its queued fill ran
// leaves nothing landing in the freed VRAM. False if there is no such test.
bool CtrRenderer_selfTest(Renderer* renderer, const char* name);

// Call after C3D_Fini, before gfxExit: drops the APT hook and puts the top
// screen back in 400 px mode for whatever runs next.
void CtrRenderer_leaveForExit(Renderer* renderer);

// Zero-alpha draws (pmengine E5x): late wrappers that the vtable points at in front of the draws, defined at the
// end of ctr_renderer.c. ctr_renderer.c expands E5X_DECLARE in place of a blank line above SKIP_WRAPPERS, so no line
// above the warm functions moves, and the wrappers stay static: new global symbols reorder the linker's GOT.
#define E5X_LATE __attribute__((noinline, section(".text.late.pmengine")))
#define E5X_DECLARE                                                                                                                                  \
    static void partDrawSprite(Renderer*, int32_t, float, float, float, float, float, float, float, float, float, float, float, uint32_t, float); \
    static void partEmit(CtrRenderer*, CtrPage*, float, float, float, float, const float*, const float*, const CtrColor*); \
    static void e5xDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale,     \
                              float angleDeg, uint32_t color, float alpha);                                                                          \
    static void e5xDrawSpritePart(Renderer* renderer, int32_t tpagIndex, float srcOffX, float srcOffY, float srcW, float srcH, float x, float y,     \
                                  float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha);              \
    static void e5xDrawSpritePartColor(Renderer* renderer, int32_t tpagIndex, float srcOffX, float srcOffY, float srcW, float srcH, float x,         \
                                       float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color1,             \
                                       uint32_t color2, uint32_t color3, uint32_t color4, float alpha);                                              \
    static void e5xDrawSpritePos(Renderer* renderer, int32_t tpagIndex, float x1, float y1, float x2, float y2, float x3, float y3, float x4,        \
                                 float y4, float alpha);                                                                                             \
    static void e5xDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline);             \
    static void e5xDrawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color1, uint32_t color2,                  \
                                      uint32_t color3, uint32_t color4, float alpha, bool outline);                                                  \
    static void e5xDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha);                   \
    static void e5xDrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2,          \
                                 float alpha);                                                                                                       \
    static void e5xDrawTriangle(Renderer* renderer, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t color1, uint32_t color2,    \
                                uint32_t color3, float alpha, bool outline);                                                                         \
    static void e5xDrawTriangleFan(Renderer* renderer, float cx, float cy, const float* xs, const float* ys, int32_t pointCount,                     \
                                   uint32_t centerColor, uint32_t edgeColor, float alpha);                                                           \
    static void e5xDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg,                      \
                            float lineSeparation);                                                                                                   \
    static void e5xDrawTextColor(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t c1,     \
                                 int32_t c2, int32_t c3, int32_t c4, float alpha, float lineSeparation);                                             \
    static void e5xDrawSpriteTiled(Renderer* renderer, int32_t tpagIndex, float originX, float originY, float x, float y, float xscale,              \
                                   float yscale, bool tileX, bool tileY, float roomW, float roomH, uint32_t color, float alpha);


// UNDERTALE's console prompts (z_btnprompt_ctr.c): CtrRenderer_setDisplay and CtrDisplay_saveEdits, then the prompts
// name the buttons of the map just applied. main.c calls these where it applies the loaded settings and saves the menu's.
void CtrBtnprompt_setDisplay(Renderer* renderer, const CtrDisplaySettings* settings);
void CtrBtnprompt_saveEdits(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps);

// Diagnostic builds only (build.sh surfcensus, natsurf6): wraps the vtable to log how every game surface is made,
// bound and drawn out. Call after the renderer's own wrappers are in place.
#ifdef CTRPORT_SURFACE_CENSUS
void CtrSurfaceCensus_install(Renderer* renderer);
#else
#define CtrSurfaceCensus_install(renderer) ((void) 0)
#endif

// Plain sprite draws (utkill, end of ctr_renderer.c): the draw e5xDrawSprite hands a sprite to. DELTARUNE's region
// draw differs (pillarEmitPageRegion) and timing builds count inside the chain, so both keep ctrDrawSprite.
#if !defined(ENABLE_WAD17) && !defined(CTRPORT_RENDER_TIMING)
#define UTK_DRAW_SPRITE utkDrawSprite
#else
#define UTK_DRAW_SPRITE ctrDrawSprite
#endif

#endif
