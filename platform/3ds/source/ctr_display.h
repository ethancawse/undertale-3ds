// Display settings: how the game image is drawn and shown on the two screens.
//
// The renderer draws the game into an application surface (640x480 for both
// games), at the top screen's resolution with native drawing, else in full.
// The other settings only change how that surface is presented (and, in depth
// mode, add a right-eye copy of it). All of them can be switched at any time.
#ifndef CTRPORT_CTR_DISPLAY_H
#define CTRPORT_CTR_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "ctr_input_map.h"

typedef enum {
    TOP_MODE_STANDARD, // 400x240, the frame shrunk to fit
    TOP_MODE_WIDE,     // 800x240 wide mode: full horizontal resolution, no 3D
    TOP_MODE_COUNT
} CtrTopMode;
// top_mode = 2 was Zoom, a 1:1 crop of the frame, retired on 2026-10-19 (Smart
// smoothing and the magnifier made the whole frame readable). Settings files
// and --ini values that still say 2 load as Standard.
#define TOP_MODE_RETIRED_ZOOM 2

typedef enum {
    BOTTOM_MODE_MAGNIFIER, // small text copied at full resolution
    BOTTOM_MODE_OFF,       // backlight off
    BOTTOM_MODE_CONSOLE,   // stats and log
    BOTTOM_MODE_COUNT
} CtrBottomMode;

// What the 3D slider does in standard mode.
typedef enum {
    STEREO_NORMAL, // both eyes see the same image
    STEREO_HD,     // each eye gets alternate columns of the full-resolution frame, so together they see 640
    STEREO_DEPTH,  // HD, and the right eye's layers are shifted so the room sits behind the screen
    STEREO_MODE_COUNT
} CtrStereoMode;

// How the frame is filtered down to the top screen (smoothing in the file:
// 0 off, 1 full, 2 smart; see "Smart smoothing" in ctr_renderer.c).
typedef enum {
    SMOOTHING_OFF,   // one pixel of each 2x2 block (the detail-keeping shrink)
    SMOOTHING_FULL,  // every 2x2 block averaged
    SMOOTHING_SMART, // off, but text and sprites with detail finer than a screen pixel averaged
    SMOOTHING_COUNT
} CtrSmoothing;
// The resolution the game is drawn at (see "Native drawing" in ctr_renderer.c).
typedef enum {
    DRAWING_NATIVE, // the top screen's (320x240 for 640x480), faster; full while wide mode, HD 3D or the magnifier needs it
    DRAWING_FULL,   // always the game's own, shrunk (or smoothed) to the screen
    DRAWING_COUNT
} CtrDrawing;

// Whether a frame may go unshown so the game keeps its speed (ctr_frame_pacer.h).
typedef enum {
    FRAME_SKIP_OFF,
    FRAME_SKIP_AUTO, // a frame that starts late runs its events but isn't drawn
    FRAME_SKIP_HALF, // every other frame runs its events but isn't drawn: the game at full speed, shown at half
    FRAME_SKIP_20,   // Auto's level 1 fixed: two of every three steps shown, 20 fps at 30 steps a second (phase 3)
    FRAME_SKIP_15,   // Auto's level 2 fixed: one of every two steps shown, 15 fps
    FRAME_SKIP_COUNT
} CtrFrameSkip;

// Frames that may go unshown in a row: one keeps the display at half the
// game's frame rate at worst, two at a third.
#define FRAME_SKIP_MAX_LIMIT 2

typedef struct {
    CtrTopMode topMode;
    CtrStereoMode stereoMode;
    // Filter the frame down to the screen. Off shows one pixel of each 2x2
    // block, the first unless another is a line or dot one pixel wide that
    // would be lost, which is crisper and keeps thin lines (see
    // "Detail-keeping shrink" in ctr_renderer.c). Smart does that for pixel
    // art and averages fine detail such as small text.
    CtrSmoothing smoothing;
    // Native drawing shows each 2x2 block's first pixel, as smoothing off
    // does but without keeping thin lines; Smart averages its fine draws
    // there too, and Full draws in full.
    CtrDrawing drawing;
    CtrBottomMode bottomMode;
    // Overworld and cutscene dialogue boxes are drawn on the bottom screen
    // instead of the top (see "Dialogue on the bottom screen" in
    // ctr_renderer.c). bottomMode says what the bottom screen shows while
    // there is no dialogue.
    bool dialogueBottom;
    // Not a display setting, but saved with them: the key each button sends
    // (the options menu's Controls page). The older confirm_with_b = 1 is read
    // as A and B swapped, the layout Japanese 3DS games use.
    CtrInputMap controls;
    // Nor is this: whether first-run texture preparation also packs on the
    // system core (prepare_syscore), for comparing on the console.
    bool prepareSyscore;
    // For testing texture preparation on a console, never saved (so give
    // them with --ini for one run): repack_textures deletes the cache files
    // so they are all prepared again, and sd_write_test writes a test file
    // several ways first and logs how fast each went.
    bool repackTextures;
    bool sdWriteTest;
    int textureCacheRun; // texture_cache_run: separate cache directory for non-destructive cold tests (0: normal)
    bool cachePublishDirect; // cache_publish_direct: direct archive delete/rename (0: stdio A/B reference)
    bool loadTrace; // load_trace: request/phase diagnostics, buffered until each startup interval ends
    // room_memory_reset: deletes the record of each room's first tiles
    // before loading, so a bench run starts from the same state whatever
    // ran before it on the console (the record is per data.win, not per build).
    bool roomMemoryReset;
    int openFilesMax; // open_files_max: kept-open files close once this many are open (0: the built-in default)
    int openFilesFakeMax; // open_files_fake_max: for testing, opens past this many fail (0: off)
    CtrFrameSkip frameSkip;
    // 1 or 2 (FRAME_SKIP_MAX_LIMIT); only set in the file.
    int32_t frameSkipMax;
    // Deep frame skip, only set in the file: on a skipped frame, 0 runs every Draw event and
    // draws everything but the application surface; 1 leaves out the Draw events the list (bench/drawskip_<game>.txt)
    // proves draw only, and the other events' draws on surfaces drawn before; 2 also mutes the events that keep a
    // picture of the frame (afterimages); 3 is 1 with the application surface still drawn (the picture afterimages
    // and trails carry into the next frame). FRAME_SKIP_HALF uses at least 1.
    int32_t frameSkipDeep;
    // Auto's policy, only set in the file: 0 as before, 1 keeps skipping while behind (CtrPacer_setSkipPolicy).
    int32_t frameSkipPolicy;
    // Fixed 2:1's overlap, only set in the file: 1 lets an unshown frame end without waiting for the GPU to finish the
    // shown frame before it (CtrRenderer_setFrameSkipOverlap). 0 by default.
    int32_t frameSkipOverlap;
    // Record and replay, only set in the file: 1 (the default) a skipped frame's muted draws on surfaces kept from
    // earlier frames are recorded and drawn when the surface is read (CtrRenderer_setFrameSkipRecord); 0 they are left
    // out, as before phase 5, kept for comparison.
    int32_t frameSkipRecord;
    // Which Auto, only set in the file: 1 (the default) the level controller (docs/perf/deepskip.md phase 3), 0 the
    // Auto before it (skips a frame that starts a slot late), kept for comparison.
    int32_t frameSkipAuto;
    // Nor this: textures hold the game's colours exactly instead of rounded
    // to 5 bits a channel (New 3DS only, see CtrPack_setExact). Slower in
    // heavy rooms; takes effect when a data.win is next loaded.
    bool exactColours;
    // bench_frames: a benchmark scene settles for a fixed number of game frames after it is ready
    // (BENCH_SETTLE_FRAMES, ctr_benchmark.c) and then measures this many game frames, instead of 3 s and 20 s.
    // Arms of an A/B that differ in speed then measure the same game frames. 0 (the default): the timed window.
    // Not for tours or battle sweeps; never saved (so give it with --ini for one run).
    int32_t benchFrames;
} CtrDisplaySettings;

typedef struct {
    bool wide;   // 800-pixel mode works on everything but the original 2DS
    bool stereo; // no 3D on either 2DS
} CtrDisplayCaps;

CtrDisplayCaps CtrDisplay_queryCaps(void);

void CtrDisplay_defaults(CtrDisplaySettings* settings);
// Missing or unreadable files leave the defaults in place. Settings the
// console can't show are switched off.
void CtrDisplay_load(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps);
// One "key = value" line in the file's format, applied on top (launch arguments' --ini). Returns false if the line
// isn't one or the key is unknown. Settings the console can't show are switched off again.
bool CtrDisplay_override(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps);
void CtrDisplay_save(const CtrDisplaySettings* settings, const char* path);
// Saves what the options menu changed (before -> after) on top of the file's
// own settings, so settings given for one run (--ini) are not written back
// unless the menu changed them.
void CtrDisplay_saveEdits(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps);

const char* CtrDisplay_topModeName(CtrTopMode mode);
const char* CtrDisplay_drawingName(CtrDrawing mode);
const char* CtrDisplay_stereoModeName(CtrStereoMode mode);
const char* CtrDisplay_bottomModeName(CtrBottomMode mode);
const char* CtrDisplay_frameSkipName(CtrFrameSkip mode);
const char* CtrDisplay_smoothingName(CtrSmoothing mode);

void CtrArgui_load(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps);
bool CtrArgui_override(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps);
void CtrArgui_saveEdits(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps);
#ifdef CTRPORT_CTR_ARGUI_REDIRECT
#define CtrDisplay_load CtrArgui_load
#define CtrDisplay_override CtrArgui_override
#define CtrDisplay_saveEdits CtrArgui_saveEdits
#endif

#endif
