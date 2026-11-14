// menutime (change scanout, ethanlog5 row 13, 11-08; UNDERTALE only): the options menu's open and close, timed for the
// log; its glyphs read at a room load so that its first open never reads the card; and the press that closes it no
// longer redraws its page.
//
// maintainer's o3DS log of 11-08 (kit 1108c): the first open after boot held its frame for 90 ms, then the page's font tile
// was read from the SD card on the game thread ("Textures: page 2 ... 1 tiles from SD cache in 11 ms" between
// "Display: view_snap" and "Audio: held for the options menu"); later opens took 22-27 ms and held nothing. His rule:
// the menu opens, moves and closes within 1-2 frames, as a native menu does. A close that saved a changed setting held
// 41 ms, and nothing said where the time went.
//
// The timing line. One line after the frame that opened or closed the menu ends (the frame's next wait starts):
//   Menu: opened in a frame of F ms: display caps C, open O (page P, screens' open S, vblank waits W), display set-up
//   D; T texture loads in L ms
//   Menu: closed in a frame of F ms: update U (vblank waits W)[, its page not redrawn], save V (display caps C), close
//   O (vblank waits W), display set-up D; T texture loads in L ms
// F runs from the end of that frame's wait for its slot to the start of the next wait: the game thread's whole work for
// the frame, as [pace]'s "main" counts it. The parts are main.c's calls, timed here: the display caps it passes to
// the open or the save, the open (argui's page drawn and copied, the screens' open that holds the audio, then the
// sound), the menu's update that took the closing press, the save, the close, and the renderer's display set-up later
// in the same frame. The page and the vblank
// waits come from two calls in ctr_optmenu.c (the screens' open and gspWaitForEvent) pointed here by name, and the
// texture loads are the texture cache's own load counts. Log only: every timed call is made as before, with the
// same arguments, in the same order.
//
// The glyphs. ctr_optmenu.c reads the menu font's glyphs into its table at each room load from the tiles resident then
// (CtrOptmenu_prepareGlyphs, peeks only, so the room's memory is untouched); a glyph whose tile wasn't resident at any
// load was sampled at the page's first draw, which then read the tile (and opened its page's cache file) on the game
// thread inside the open. Here, inside the game's load, behind the loading bar: z_loadscreen.c's call of the first
// room's set-up comes here (CtrMenutime_initFirstRoom), which makes it, and then, before the bar is finished, draws each
// of the 96 glyphs once through the menu's own painter into a small scratch column, which takes any glyph still missing
// exactly as the page's first draw would (the same code). Once a session, whatever happens: never again at a later
// room change or during play, so a failure (no texture cache, no memory for the column) just leaves the first open
// as it was. It never makes room by evicting a tile: a glyph is drawn only when the tiles it may read are resident or
// fit in the cache's budget and in free linear memory as they are, else it is left for the first open. While it runs
// the cache's room record is off, so the tile isn't added to the room's remembered tiles (which would read it at that
// room's every later load). The page's tile then stays an ordinary resident tile, evicted as any other: the table no
// longer needs it. argui learns its texture cache and font at the first room's prefetch, a frame later, so for the
// pass it is given them (the same values, looked up the same way) and then given back what it had.
//
// The closing press (CtrMenutime_update, below): its update no longer draws the page again just before the close
// clears it.
//
// The Custom page's previews. A on Smoothing while it is Custom read the page's previews (the build's menu data,
// romfs:/fonts/undertale.dtsm, 26 KB) from the card inside that press's frame, at every menu open (they are freed at
// the close): sweep J timed that press's frame at 67 ms against 50 ms for any other page. Now an idle frame of the
// open menu reads them ahead (CtrMenutime_update: the third update with no press since the open, on the Options page
// with no Legal or Credits page up, while Smoothing is Custom), so the press finds them in memory. The same call
// (z_smalltext.c's CtrSmooth_menuOpen, which does nothing when they are in), the same memory, freed at the same close:
// only the frame it lands in moves, to one where nothing is drawn. If Custom is chosen later in the open, the read
// waits for the idle frames after that. The read's time is logged the first time in a session.
//
// main.c's, ctr_optmenu.c's, z_loadscreen.c's and z_roomplan.c's calls come here by name (CMake's compile
// definitions): the same calls with other targets, so no existing code or address changes. Last in late text
// (.text_menutime), its constants after it, its state last in the late data's paid reserve (.data.menutime_late,
// explicit zeroes: new statics after .bss would move __end__ over a page on the Old 3DS), so nothing else moves.
// CMake leaves this file out of DELTARUNE, whose calls keep their targets.
#if defined(ENABLE_WAD16)
#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "audio_system.h"
#include "ctr_display.h"
#include "ctr_menu.h"
#include "ctr_renderer.h"
#include "ctr_texture_pack.h"
#include "ctr_textures.h"
#include "data_win.h"
#include "log.h"
#include "runner.h"
#include "stb_ds.h"

#define MT_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text_menutime")))
#define MT_RO __attribute__((section(".rodata.menutime_in_text")))
#define MT_DATA __attribute__((section(".data.menutime_late")))

// argui's state (ctr_argui.c, a global): the same layout (as ctr_optmenu.c and z_argbind.c declare it).
typedef struct {
    uint8_t cfg[3];
    bool menuIsOpen;
    uint8_t sx;
    uint8_t sy;
    uint8_t adv;
    bool rgb565;
    char fpsBuf[3];
    uint16_t hintFrames;
    uint16_t fpsFrames;
    int16_t assetIds[5]; // sndMove, sndSelect, fntSmall, fntTheme, sprHeartSmall
    uint32_t fpsVb;
    void* fb;
    CtrTextureCache* texCache;
    AudioSystem* audio;
} ArguiState;
extern ArguiState CtrArgui_state;

// The calls' own targets (the names main.c, ctr_optmenu.c, z_loadscreen.c and z_roomplan.c used before), and the
// other files' functions called here.
void Upd1_menuOpen(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps);
void CtrMenu_chainOpenInputlate(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps);
void __real_Runner_initFirstRoom(Runner* runner);
void __real_CtrTextures_init(CtrTextureCache* cache, DataWin* dataWin, FileSystem* fileSystem, uint32_t budgetBytes);
void __real_CtrOptmenu_text(uint16_t* fb, int32_t x, int32_t y, const char* s, uint32_t col);
u32 CtrLegalui_mark(u32 folded);
bool CtrLegal_shown(void);
uint32_t CtrSmooth_get(void);
void CtrSmooth_menuOpen(void);
bool CtrOptmenu_pressResumes(const CtrMenu* menu, u32 down);
void CtrOptmenu_flushPage(CtrMenu* menu);
// argui's names for its theme font and SOUL cursor (ctr_argui.c's aliases of its own nameFntTheme and
// nameSprHeartSmall), looked up as its prefetch looks them up.
extern const char CtrArgui_nameFntTheme[];
extern const char CtrArgui_nameSprHeartSmall[];

// Named arrays, not literals: a literal's mergeable section could let the linker fold another object's string into one
// of these and move what follows.
static const char mtLogOpen[] MT_RO = "Menu: opened in a frame of %.1f ms: display caps %.1f, open %.1f (page %.1f, "
                                      "screens' open %.1f, vblank waits %.1f), display set-up %.1f; %u texture loads in %.1f ms\n";
static const char mtLogClose[] MT_RO = "Menu: closed in a frame of %.1f ms: update %.1f (vblank waits %.1f)%s, save %.1f "
                                       "(display caps %.1f), close %.1f (vblank waits %.1f), display set-up %.1f; %u "
                                       "texture loads in %.1f ms\n";
static const char mtSkipped[] MT_RO = ", its page not redrawn";
static const char mtLogIters[] MT_RO = "Menu: the %s's frame and the 7 after it took %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f ms "
                                       "(loop to loop, with the waits)\n";
static const char mtOpened[] MT_RO = "open";
static const char mtClosed[] MT_RO = "close";
static const char mtPressed[] MT_RO = "press";
static const char mtNothing[] MT_RO = "";
static const char mtLogGlyphs[] MT_RO = "Menu: glyphs read in the first room's load in %.1f ms (%u texture loads; %u left for "
                                        "the first open)\n";
static const char mtLogPreviews[] MT_RO = "Menu: the Custom page's previews read ahead in an idle frame in %.1f ms\n";

enum { MT_NONE, MT_OPEN, MT_CLOSE };

static struct {
    uint64_t frameStart; // the end of this frame's wait for its slot
    uint64_t capsStart, capsTicks; // the last display caps call
    uint64_t openStart, pageTicks, screensTicks, openTicks;
    uint64_t saveTicks, closeTicks, waitTicks, displayTicks;
    double loadMs;   // the texture cache's load time when the event began (all kinds of load)
    uint32_t loads;  // and its load count
    uint8_t kind;    // MT_OPEN or MT_CLOSE: a line is due when this frame ends
    bool timing;     // inside the timed open or close (the vblank waits count)
} mt MT_DATA = {};

// The menu's update (main.c's, once a frame while it is open): kept apart from mt, which an open or a close clears.
static struct {
    uint64_t start, ticks, waitTicks; // the last update, and its vblank waits
    CtrMenu* menu;   // its menu, while the update runs
    bool resumes;    // the press closes the menu: its first CtrLegalui_mark call hides the menu from the page's redraw
    bool hidden;     // menu->open was cleared for the update's flush
    bool skipped;    // and so the page was not drawn
    bool timing;     // inside the update (the vblank waits count)
    bool pressed;    // a press was handled since the last frame wait (its first CtrLegalui_mark call)
} mtp MT_DATA = {};

// The loop's iterations after an open, a close or a press (main.c's frame wait to its next one): 8, then one line.
enum { MT_ITERS = 8 };
static struct {
    uint64_t last;            // the last frame wait's start
    float ms[MT_ITERS];
    const char* what;         // the event being followed (nullptr: none)
    uint8_t n;
} mti MT_DATA = {};

// The glyph pass: the renderer's texture cache (z_roomplan.c's call of its set-up comes here), and whether the pass
// was tried this session. The Custom page's previews: due in this open, the idle updates since, logged this session.
enum { MT_PREVIEW_IDLE = 3 };
static struct {
    CtrTextureCache* cache;
    bool tried;
    bool previewsDue;
    bool previewsLogged;
    uint8_t idle;
} mtg MT_DATA = {};

MT_CODE static uint64_t mtTick(void) {
    return svcGetSystemTick();
}

MT_CODE static double mtMs(uint64_t ticks) {
    return (double) ticks / (double) CPU_TICKS_PER_MSEC;
}

// The texture cache's loads so far (from its RAM copies, its cache files and the game's own images).
MT_CODE static uint32_t mtLoads(double* ms) {
    const CtrTextureCache* cache = CtrArgui_state.texCache;
    *ms = 0.0;
    if (cache == nullptr) return 0;
    *ms = cache->fromRam.msTotal + cache->fromFile.msTotal + cache->fromImage.msTotal;
    return cache->fromRam.count + cache->fromFile.count + cache->fromImage.count;
}

// An open or a close begins: its line is logged when the frame ends. The caps call made for it in this frame counts.
MT_CODE static void mtBegin(uint8_t kind) {
    if (mt.kind == kind) return;
    uint64_t caps = mt.capsStart >= mt.frameStart ? mt.capsTicks : 0;
    uint64_t frameStart = mt.frameStart, capsStart = mt.capsStart;
    memset(&mt, 0, sizeof(mt));
    mt.frameStart = frameStart;
    mt.capsStart = capsStart;
    mt.capsTicks = caps;
    mt.kind = kind;
    mt.loads = mtLoads(&mt.loadMs);
}

// main.c's display caps (at the open, the save, the load and an --ini override).
CtrDisplayCaps CtrMenutime_caps(Renderer* renderer);
MT_CODE CtrDisplayCaps CtrMenutime_caps(Renderer* renderer) {
    uint64_t t = mtTick();
    CtrDisplayCaps caps = CtrRenderer_capsAuditfixh(renderer);
    mt.capsStart = t;
    mt.capsTicks = mtTick() - t;
    return caps;
}

// main.c's open (upd1's link, then argui's and the screens').
void CtrMenutime_open(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps);
MT_CODE void CtrMenutime_open(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps) {
    mtBegin(MT_OPEN);
    mtg.previewsDue = true;
    mtg.idle = 0;
    mt.timing = true;
    mt.openStart = mtTick();
    Upd1_menuOpen(menu, console, settings, caps);
    mt.openTicks = mtTick() - mt.openStart;
    mt.timing = false;
}

// ctr_optmenu.c's call of the screens' open, after its page is shown: the time before it is the page's.
void CtrMenutime_screensOpen(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps);
MT_CODE void CtrMenutime_screensOpen(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps) {
    uint64_t t = mtTick();
    if (mt.kind == MT_OPEN && mt.timing) mt.pageTicks = t - mt.openStart;
    CtrMenu_chainOpenInputlate(menu, console, settings, caps);
    if (mt.kind == MT_OPEN && mt.timing) mt.screensTicks = mtTick() - t;
}

// ctr_optmenu.c's vblank waits (its page's copy and credwait's).
void CtrMenutime_wait(GSPGPU_Event id, bool nextEvent);
MT_CODE void CtrMenutime_wait(GSPGPU_Event id, bool nextEvent) {
    uint64_t t = mtTick();
    gspWaitForEvent(id, nextEvent);
    if (mt.timing) mt.waitTicks += mtTick() - t;
    if (mtp.timing) mtp.waitTicks += mtTick() - t;
}

// main.c's save of the menu's changes, before its close.
void CtrMenutime_saveEdits(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps);
MT_CODE void CtrMenutime_saveEdits(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps) {
    mtBegin(MT_CLOSE);
    uint64_t t = mtTick();
    CtrBtnprompt_saveEdits(before, after, path, caps);
    mt.saveTicks = mtTick() - t;
}

// main.c's close (the menu's Resume, Quit or performance test, and a scheduled test pause's).
void CtrMenutime_close(CtrMenu* menu);
MT_CODE void CtrMenutime_close(CtrMenu* menu) {
    mtBegin(MT_CLOSE);
    mt.timing = true;
    uint64_t t = mtTick();
    CtrArgui_menuClose(menu);
    mt.closeTicks = mtTick() - t;
    mt.timing = false;
}

// main.c's display set-up, every frame: timed in a frame that opened or closed the menu.
void CtrMenutime_setDisplay(Renderer* renderer, const CtrDisplaySettings* settings);
MT_CODE void CtrMenutime_setDisplay(Renderer* renderer, const CtrDisplaySettings* settings) {
    if (mt.kind == MT_NONE) {
        CtrRenderer_setDisplay(renderer, settings);
        return;
    }
    uint64_t t = mtTick();
    CtrRenderer_setDisplay(renderer, settings);
    mt.displayTicks += mtTick() - t;
}

// main.c's wait for the frame's slot, every frame: the frame that opened or closed the menu has ended, so its line.
void CtrMenutime_beginFrame(Renderer* renderer);
MT_CODE void CtrMenutime_beginFrame(Renderer* renderer) {
    uint64_t entry = mtTick();
    float iteration = mti.last != 0 ? (float) mtMs(entry - mti.last) : 0.0f;
    mti.last = entry;
    if (mti.what != nullptr) {
        mti.ms[mti.n++] = iteration;
        if (mti.n == MT_ITERS) {
            logInfo(mtLogIters, mti.what, (double) mti.ms[0], (double) mti.ms[1], (double) mti.ms[2], (double) mti.ms[3],
                    (double) mti.ms[4], (double) mti.ms[5], (double) mti.ms[6], (double) mti.ms[7]);
            mti.what = nullptr;
        }
    } else if (mt.kind != MT_NONE || mtp.pressed) {
        // The frame that just ended had the open, the close or a press: it is the first of the 8.
        mti.what = mt.kind == MT_OPEN ? mtOpened : (mt.kind == MT_CLOSE ? mtClosed : mtPressed);
        mti.ms[0] = iteration;
        mti.n = 1;
    }
    mtp.pressed = false;
    if (mt.kind != MT_NONE) {
        uint64_t frame = mtTick() - mt.frameStart;
        double loadMs = 0.0;
        uint32_t loads = mtLoads(&loadMs) - mt.loads;
        loadMs -= mt.loadMs;
        if (mt.kind == MT_OPEN) {
            logInfo(mtLogOpen, mtMs(frame), mtMs(mt.capsTicks), mtMs(mt.openTicks), mtMs(mt.pageTicks), mtMs(mt.screensTicks),
                    mtMs(mt.waitTicks), mtMs(mt.displayTicks), (unsigned) loads, loadMs);
        } else {
            bool press = mtp.start >= mt.frameStart; // the update that closed the menu, in this frame
            logInfo(mtLogClose, mtMs(frame), mtMs(press ? mtp.ticks : 0), mtMs(press ? mtp.waitTicks : 0),
                    press && mtp.skipped ? mtSkipped : mtNothing, mtMs(mt.saveTicks + mt.capsTicks), mtMs(mt.capsTicks),
                    mtMs(mt.closeTicks), mtMs(mt.waitTicks), mtMs(mt.displayTicks), (unsigned) loads, loadMs);
        }
        mt.kind = MT_NONE;
    }
    CtrRenderer_beginFrame(renderer);
    mt.frameStart = mtTick();
}

// main.c's update of the open menu, once a frame (the press's keys, then the page's redraw and copy if it changed).
// optKeys marks the page changed at every press, the one that closes the menu too; that press's update then drew the
// whole page again and copied it at the screen's vblank (two vblank waits), and main.c's close cleared it in the same
// frame. So when ctr_optmenu.c's own rules say that this press closes the menu with Resume (CtrOptmenu_pressResumes),
// menu->open is cleared from the update's first CtrLegalui_mark call (optKeysHeard's, after the update's own open
// check and before the keys) until the update returns: the keys are handled exactly as before, and the flush, which
// returns at once for a closed menu, draws and copies nothing. The page on screen is the one the press left
// unchanged. Should the action not be a close after all, the page is flushed again with the menu open
// (CtrOptmenu_flushPage: the update's flush alone, no keys, no held-key or Credits tick), which draws and copies it as
// the first call would have (the press marked it changed), so the frame shows the same page as before.
CtrMenuAction CtrMenutime_update(CtrMenu* menu, u32 down);
MT_CODE CtrMenuAction CtrMenutime_update(CtrMenu* menu, u32 down) {
    mtp.start = mtTick();
    mtp.waitTicks = 0;
    mtp.menu = menu;
    mtp.skipped = false;
    mtp.resumes = CtrOptmenu_pressResumes(menu, down);
    mtp.timing = true;
    CtrMenuAction action = CtrArgui_menuUpdate(menu, down);
    mtp.resumes = false;
    if (mtp.hidden) {
        mtp.hidden = false;
        menu->open = true;
        mtp.skipped = true;
        if (action != MENU_ACTION_RESUME && action != MENU_ACTION_QUIT && action != MENU_ACTION_PERF_TEST) {
            CtrOptmenu_flushPage(menu);
            mtp.skipped = false;
        }
    }
    mtp.timing = false;
    mtp.menu = nullptr;
    if (mtg.previewsDue && down == 0 && action == MENU_ACTION_NONE && menu->open && menu->page == MENU_PAGE_OPTIONS &&
        !CtrLegal_shown() && (CtrSmooth_get() & 0x80u) != 0 && ++mtg.idle >= MT_PREVIEW_IDLE) {
        mtg.previewsDue = false;
        uint64_t t = mtTick();
        CtrSmooth_menuOpen();
        if (!mtg.previewsLogged) logInfo(mtLogPreviews, mtMs(mtTick() - t));
        mtg.previewsLogged = true;
    }
    mtp.ticks = mtTick() - mtp.start;
    return action;
}

// ctr_optmenu.c's calls of the Legal pages' mark (optKeysHeard's, at each press before its keys, and the press's
// sound check after them): the first, in an update whose press closes the menu, hides the menu from the flush.
u32 CtrMenutime_legalMark(u32 folded);
MT_CODE u32 CtrMenutime_legalMark(u32 folded) {
    mtp.pressed = true; // a press is being handled (optKeysHeard's)
    if (mtp.resumes && mtp.menu != nullptr && mtp.menu->open) {
        mtp.resumes = false;
        mtp.hidden = true;
        mtp.menu->open = false;
    }
    return CtrLegalui_mark(folded);
}

// The scratch column the glyphs are drawn into: the painter writes fb[(x + gx) * 240 + (239 - y - gy)] for a glyph's
// set texels (gx 0-8, gy 0-11); at x 0, y 228 that is indices 0 to 8 * 240 + 11.
#define MT_SCRATCH_TEXELS (8u * 240u + 12u)
// Linear memory a glyph's tile loads leave free at least: the cache makes room by evicting when a tile's memory can't
// be had (ctr_textures.c's allocateTile), so the pass loads nothing it isn't sure of.
#define MT_LINEAR_SPARE (1u << 20)

// z_roomplan.c's call of the texture cache's set-up (the renderer's, through roomplan's wrap): the same call, then the
// cache is noted for the glyph pass.
void CtrMenutime_texturesInit(CtrTextureCache* cache, DataWin* dataWin, FileSystem* fileSystem, uint32_t budgetBytes);
MT_CODE void CtrMenutime_texturesInit(CtrTextureCache* cache, DataWin* dataWin, FileSystem* fileSystem, uint32_t budgetBytes) {
    __real_CtrTextures_init(cache, dataWin, fileSystem, budgetBytes);
    mtg.cache = cache;
}

// Whether a glyph's read can load what it needs without evicting anything: every tile of the page under the glyph's
// source rectangle (x0, y0, w, h inside its texture item; a superset of the texels optTexel reads) is resident, known
// to be empty, or fits in the budget and in free linear memory with the others. A page whose layout can't be had is
// fine: the painter reads nothing from it either.
MT_CODE static bool mtGlyphFits(CtrTextureCache* cache, const DataWin* dw, int32_t tpagIndex, uint32_t x0, uint32_t y0, uint32_t w,
                                uint32_t h) {
    if (tpagIndex < 0 || w == 0 || h == 0) return true;
    const TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    CtrPage* page = CtrTextures_acquire(cache, (uint32_t) tpag->texturePageId);
    if (page == nullptr) return true;
    uint32_t px = tpag->sourceX + x0, py = tpag->sourceY + y0, need = 0;
    for (uint32_t ty = py >> 8; (py + h - 1u) >> 8 >= ty && (uint32_t) page->tilesY > ty; ty++) {
        for (uint32_t tx = px >> 8; (px + w - 1u) >> 8 >= tx && (uint32_t) page->tilesX > tx; tx++) {
            const CtrTile* tile = &page->tiles[ty * (uint32_t) page->tilesX + tx];
            if (tile->valid || (page->formatsKnown && tile->format == CTR_TILE_EMPTY)) continue;
            uint32_t texel = page->formatsKnown ? CtrPack_texelBytes((CtrTileFormat) tile->format) : 4u;
            need += (uint32_t) page->tileW * (uint32_t) page->tileH * texel;
        }
    }
    return need == 0 || (cache->residentBytes + need <= cache->budgetBytes && linearSpaceFree() >= need + MT_LINEAR_SPARE);
}

// z_loadscreen.c's call of the first room's set-up (inside its wrap: the bar is up until that returns): the set-up,
// then the glyph pass, once a session. Each of the heart and 32..126 (the table's 96 slots) is drawn into the scratch
// column if mtGlyphFits says its tiles can be had without evicting; the others are left for the first open.
void CtrMenutime_initFirstRoom(Runner* runner);
MT_CODE void CtrMenutime_initFirstRoom(Runner* runner) {
    __real_Runner_initFirstRoom(runner);
    if (mtg.tried) return;
    mtg.tried = true; // never again this session, whatever happens below
    CtrTextureCache* cache = mtg.cache;
    if (cache == nullptr || cache->dataWin == nullptr) return;
    uint16_t* scratch = (uint16_t*) malloc(MT_SCRATCH_TEXELS * sizeof(uint16_t));
    if (scratch == nullptr) return;
    const DataWin* dw = cache->dataWin;
    CtrTextureCache* oldCache = CtrArgui_state.texCache;
    int16_t oldFont = CtrArgui_state.assetIds[3], oldHeart = CtrArgui_state.assetIds[4];
    int32_t font = shget(runner->assetsByName, CtrArgui_nameFntTheme),
            heart = shget(runner->assetsByName, CtrArgui_nameSprHeartSmall);
    CtrArgui_state.texCache = cache;
    CtrArgui_state.assetIds[3] = (int16_t) font;
    CtrArgui_state.assetIds[4] = (int16_t) heart;
    uint64_t t = mtTick();
    double ms0 = 0.0, ms1 = 0.0;
    uint32_t loads0 = mtLoads(&ms0), left = 0;
    int32_t record = cache->recordRoom;
    cache->recordRoom = -1; // not the room's remembered tiles
    for (uint32_t slot = 0; 96u > slot; slot++) {
        char s[2] = {(char) (slot == 0 ? 1u : slot + 31u), 0};
        bool fits = true;
        if (slot == 0) {
            // optTexel's heart: the small heart sprite's first frame, 9 x 9 from its item's corner.
            if (heart >= 0) fits = mtGlyphFits(cache, dw, dw->sprt.sprites[heart].tpagIndices[0], 0, 0, 9, 9);
        } else if (font >= 0 && dw->font.fonts[font].glyphLUT[(uint8_t) s[0]] != nullptr) {
            const FontGlyph* gl = dw->font.fonts[font].glyphLUT[(uint8_t) s[0]];
            fits = mtGlyphFits(cache, dw, dw->font.fonts[font].tpagIndex, gl->sourceX, gl->sourceY, gl->sourceWidth, gl->sourceHeight);
        }
        if (!fits) {
            left++;
            continue;
        }
        __real_CtrOptmenu_text(scratch, 0, 228, s, 0xFFFFu);
    }
    cache->recordRoom = record;
    free(scratch);
    uint32_t loads = mtLoads(&ms1) - loads0;
    logInfo(mtLogGlyphs, mtMs(mtTick() - t), (unsigned) loads, (unsigned) left);
    CtrArgui_state.texCache = oldCache;
    CtrArgui_state.assetIds[3] = oldFont;
    CtrArgui_state.assetIds[4] = oldHeart;
}
#endif
