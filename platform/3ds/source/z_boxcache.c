// boxcache: a bottom-screen dialogue box that hasn't changed is not drawn again, and one that only gained letters gets
// only the new ones drawn (change boxcache, first update; UNDERTALE Old only).
//
// With dialogue on the bottom screen, presentDialogue clears the bottom target, draws the frame's recorded box, portrait
// and glyph ops into it and has it transferred, every frame the box shows: on the o2ds about 2.9 ms of a frame, and the
// lab quiz intro held 7-8 frames with it. A box that waits for input, or a line typed out and held, records the same ops
// frame after frame, and a line being typed records them again with new ones after them.
//
// When the ops last drawn are the start of this frame's (all of them, or the same ops and then new ones, each new one
// inside the box's bounds, so the box's scale and place on the screen are the same), the bottom target still holds the
// last draw, as nothing else draws on it while a box shows (the frame's start leaves it alone: bottomLeftBlank).
// presentDialogue then draws only the new ops onto it, or none: its clear of the target is skipped (ctr_renderer.c's
// C3D_FrameBufClear calls come here first), and the old ops are given page -1 for the frame, which its loop skips
// (surfaceSpritePage finds no slot -1) and which they get back after it. Each pixel then gets the same ops in the same
// order as a full draw: the old ones last frame, onto the same clear, and the new ones now. The new ones' bounds keep
// the box's, so every op lands where a full draw puts it. Where the clear was, the old ops' texture cache uses are made
// instead, in their order, so the cache sees the full draw's calls (the new ops' come next, from the draw), and a box
// kept for minutes ages, records and promotes its tiles as a drawn one. Everything else about the frame is a full draw's:
// presentDialogue's counters and state, the bind, and the transfer, so the bottom screen's buffer gets the whole box
// every frame it shows, as before. That buffer has other writers (the log's writer thread prints the stats and log
// consoles there, and a line queued before a box opened can land after its transfer; the FPS counter; Rosalina's blackout;
// keep-lit's fill), and whatever they leave lasts until the next frame's transfer, as without boxcache (R422). The
// saving is the clear and the old ops' draws, the GPU's and the CPU's.
//
// Same means: the same ops, as many as last time, with the same bytes (all but CtrRecOp's padding byte), by a 64-bit
// hash of those; the same bottom target and colour buffer, smoothing, application surface size, room visit and box row;
// the box shown, the bottom screen the game's, no gap since the last draw, dialogue still on the bottom. The base is a
// draw whose ops were all a colour or a data.win page with its layout known and every tile the draw asks for
// (emitPageTiles's range) resident or known empty, and no op dropped, before the draw: that draw loaded none of them
// and drew all of them. They stay resident through the frame's end, as the cache evicts no tile the frame has used; if
// it evicted anything there all the same, they are checked again after the draw. The same holds again before each frame
// that reuses them (all its ops), so a full draw would draw them the same again; if the cache evicted anything between
// that check and the hidden ops' clear, they are checked again there, and if they are no longer whole the frame draws
// them all after a real clear, and starts over. Anything else (a surface's page, a
// page or tile missing, a menu, a closed box, a settings change, which leaves dlg.shown false) draws in full and starts
// over. So does a frame that starts 200 ms or more after the last one: the app may have been suspended meanwhile (HOME,
// sleep, Rosalina's menu), its target with it. The renderer's APT hook ends a box too (displayDirty, then dlg.shown
// false); the time gate doesn't rely on it.
//
// Hooks, so no instruction elsewhere changes: CtrRenderer_endFrame's wrap chain gets a last link (postkick's call to the
// real one comes here), and ctr_renderer.c's target clears come here first, both by CMake: the same calls, other
// targets.
//
// This is its own file because code added to ctr_renderer.c, even at its end, changes how GCC lays out that file (a
// call from presentDialogue into the same file reordered its .bss; the new declarations renumbered its switch tables;
// z_keeplit.c found the same with inlining). The renderer's fields are private to that file, so their offsets are in
// z_boxcache_layout.h, which ctr_renderer.c's end checks.
//
// paintchg's opt-in lab extension below: at final submission a whole,
// unchanged box may omit its transfer under saved Other screen Off. CPU
// writers dirty one union; the console writer must be idle and claimed without
// waiting. FPS/hint, screenshots, yielded/game-bottom and foreign owners retain
// transfers. APT and the existing Rosalina hold explicitly invalidate reuse.
// In particular the renderer's effective bottomMode is not the saved setting:
// the original 2DS narrows Off to Console to retain its shared backlight.
// Default builds retain every transfer until the every-frame LCD gate passes.

#if defined(ENABLE_WAD16) && defined(CTRPORT_TARGET_O3DS)
#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "ctr_array.h"
#include "ctr_dialogue.h"
#include "ctr_display.h"
#include "ctr_renderer.h"
#include "ctr_textures.h"
#include "runner.h"
#include "z_boxcache_layout.h"

typedef struct {
    int32_t pageId;
    float sx0, sy0, sx1, sy1;
    float x[4], y[4];
    uint8_t rest[BOXCACHE_OP_SIZE - 52];
} BxOp;
typedef BxOp CtrRecOp;
typedef struct BxSurface CtrSurface; // only pointers to it here

#define BOXCACHE_OP_MINE(member, type, offset) \
    _Static_assert(__builtin_offsetof(BxOp, member) == (offset), "BxOp." #member);
BOXCACHE_OP_MINE(pageId, int32_t, 0)
BOXCACHE_OP_MINE(sx0, float, 4)
BOXCACHE_OP_MINE(sy0, float, 8)
BOXCACHE_OP_MINE(sx1, float, 12)
BOXCACHE_OP_MINE(sy1, float, 16)
BOXCACHE_OP_MINE(x, float[4], 20)
BOXCACHE_OP_MINE(y, float[4], 36)
_Static_assert(sizeof(BxOp) == BOXCACHE_OP_SIZE && BOXCACHE_OP_SIZE % 4 == 0, "BxOp's size");

// The renderer's field `name` (z_boxcache_layout.h), as an lvalue.
#define BOXCACHE_TYPE(name, member, type, offset) typedef type BxT_##name;
#define BOXCACHE_OFFSET(name, member, type, offset) BxO_##name = (offset),
BOXCACHE_FIELDS(BOXCACHE_TYPE)
enum { BOXCACHE_FIELDS(BOXCACHE_OFFSET) };
#define BX(r, name) (*(BxT_##name*) (void*) ((char*) (r) + BxO_##name))

// paintchg: ca8140068's cache, with its process-lifetime state on the heap.
// One root in the appended change BSS; allocation failure retains the full draw path.
#define BX_CODE __attribute__((noinline, section(".text.late.paintchg")))
#define BOXCACHE_TYPED_MAX 512
#define BOXCACHE_LATE_TICKS ((uint64_t) SYSCLOCK_ARM11 / 5)
typedef struct {
    const void* target;
    const void* color;
    uint32_t visit;
    int32_t appW, appH;
    uint32_t top;
    int32_t smoothing;
} BxKey;
typedef struct {
#if !defined(CTRPORT_PAINTCHG_RELEASE)
    uint32_t kept, typed;
#endif
    BxKey key;
    uint32_t count, hashA, hashB;
    uint64_t lastTick;
    float x0, y0, x1, y1;
    int16_t hidden[BOXCACHE_TYPED_MAX];
    uint32_t typedFrom;
    bool cleared;
    uint32_t evictions;
    void* renderer;
    aptHookCookie apt;
    uint32_t dirty;
#if !defined(CTRPORT_PAINTCHG_RELEASE)
    uint32_t skipped;
#endif
    bool same;
} BxState;
static BxState* bxRoot __attribute__((section(".bss.late.paintchg"))); _Static_assert(sizeof(bxRoot) == 4 || sizeof(void*) != 4, "z_sdmigrate.c carves these 4 B from smSpare (SM_CARVE_BOXCACHE)");
#if !defined(CTRPORT_PAINTCHG_RELEASE)
#define CtrBoxcache_kept (bxRoot->kept)
#define CtrBoxcache_typed (bxRoot->typed)
#else
#define CtrBoxcache_kept 0u
#define CtrBoxcache_typed 0u
#endif
// The root is installed once and never replaced/freed by production code.
// Capture it per entry so calls into the renderer/cache do not force repeated
// global-root reloads for each field in the same lifetime-stable block.
#define bxBaseKey (state->key)
#define bxBaseCount (state->count)
#define bxBaseA (state->hashA)
#define bxBaseB (state->hashB)
#define bxLastTick (state->lastTick)
#define bxBaseX0 (state->x0)
#define bxBaseY0 (state->y0)
#define bxBaseX1 (state->x1)
#define bxBaseY1 (state->y1)
#define bxHidden (state->hidden)
#define bxTypedFrom (state->typedFrom)
#define bxCleared (state->cleared)
#define bxEvictions (state->evictions)
#define bxRenderer (state->renderer)

static BX_CODE void bxApt(APT_HookType event, void* param) {
    (void) event;
    BxState* state = param;
    __atomic_store_n(&state->dirty, 1, __ATOMIC_RELEASE);
}

static BX_CODE bool bxReady(void) {
    if (bxRoot == nullptr) {
        BxState* state = calloc(1, sizeof(*state));
        bxRoot = state != nullptr ? state : (BxState*) (uintptr_t) 1;
        if (state != nullptr) aptHook(&state->apt, bxApt, state);
    }
    return (uintptr_t) bxRoot > 1;
}

BX_CODE void CtrBoxcache_frameBufClear(C3D_FrameBuf* fb, C3D_ClearBits clearBits, u32 clearColor, u32 clearDepth);
void __real_CtrRenderer_endFrame(Renderer* renderer);
BX_CODE void CtrRenderer_chainEndFrameBoxcache(Renderer* renderer);

// Share the idle provider's nonblocking console claim. Its checks include the
// queue and pending framebuffer flush, with a recheck while holding the lock.
bool CtrBottomPaint_claimConsole(void);
void CtrLog_endConsole(void);
bool CtrArgui_keepLit(u32 screen);
extern bool CtrScreens_onBottom, CtrScreens_otherOff;
bool CtrDimlight_rosalinaHeld(void);
void CtrDimlight_rosalinaSleep(s64 ns);

// Every CPU LCD paint's existing flush invalidates the union. Texture flushes
// through these sources conservatively invalidate too; the GPU queue's whole
// linear-heap flush is deliberately outside this route.
BX_CODE Result CtrPaintchg_cpuFlush(const void* data, u32 size) {
    if ((uintptr_t) bxRoot > 1) __atomic_store_n(&bxRoot->dirty, 1, __ATOMIC_RELEASE);
    return GSPGPU_FlushDataCache(data, size);
}

// Rosalina can borrow the LCD without an APT event or a 200 ms gap.
BX_CODE void CtrPaintchg_rosalinaSleep(s64 ns) {
    bool held = CtrDimlight_rosalinaHeld();
    CtrDimlight_rosalinaSleep(ns);
    if ((held || CtrDimlight_rosalinaHeld()) && (uintptr_t) bxRoot > 1)
        __atomic_store_n(&bxRoot->dirty, 1, __ATOMIC_RELEASE);
}

// Submission is after the clear hook's second residency check. An early bind
// decision could otherwise hide a mid-pass eviction's required full repaint.
// Holding the console lock costs no wait: failure keeps the original transfer.
// sonrev10 C1: opt-in lab path only until every-frame LCD identity is qualified.
// The default keeps R422b's clear/replay cache AND every original transfer.
#ifndef CTRPORT_PAINTCHG_TRANSFER_OMISSION
#define CTRPORT_PAINTCHG_TRANSFER_OMISSION 0
#endif
BX_CODE void CtrPaintchg_frameEnd(u8 flags) {
#if CTRPORT_PAINTCHG_TRANSFER_OMISSION
    BxState* state = bxRoot;
    bool claimed = false;
    if ((uintptr_t) state > 1 && bxRenderer != nullptr) {
        void* r = bxRenderer;
        C3D_RenderTarget* target = BX(r, BOTTOM_TARGET);
        if (target != nullptr && target->used && BX(r, IN_FRAME) && !BX(r, SKIP_PRESENT) && !BX(r, SKIP_ACTIVE) &&
            !BX(r, BOTTOM_YIELDED)) {
            bool exclusive = CtrScreens_otherOff && !CtrScreens_onBottom &&
                             !CtrArgui_keepLit(2) && !BX(r, SCREENSHOT_AFFECTED) && !CtrDimlight_rosalinaHeld();
            if (exclusive) claimed = CtrBottomPaint_claimConsole();
            // Consume dirt only when this submission actually refreshes the LCD.
            // Later producers retain their bit for the next frame.
            uint32_t dirty = __atomic_exchange_n(&state->dirty, 0, __ATOMIC_ACQ_REL);
            if (claimed && dirty == 0 && state->same && bxTypedFrom != 0 && bxCleared) {
                target->used = false;
#if !defined(CTRPORT_PAINTCHG_RELEASE)
                state->skipped++;
#endif
            }
        }
    }
    C3D_FrameEnd(flags);
    if (claimed) CtrLog_endConsole();
#else
    C3D_FrameEnd(flags);
#endif
}

static BX_CODE void bxKeyOf(void* r, BxKey* k) {
    Runner* runner = ((Renderer*) r)->runner;
    int32_t id = runner->applicationSurfaceId;
    const char* app = 0 <= id && (uint32_t) id < BX(r, SURFACE_COUNT) ? (const char*) BX(r, SURFACES) + (size_t) id * BOXCACHE_SURFACE_SIZE : nullptr;
    if (app != nullptr && !*(const bool*) (app + BOXCACHE_SURFACE_USED)) app = nullptr;
    C3D_RenderTarget* target = BX(r, BOTTOM_TARGET);
    memset(k, 0, sizeof(*k)); // keys compare whole, padding too
    k->target = target;
    k->color = target != nullptr ? target->frameBuf.colorBuf : nullptr;
    k->visit = BX(r, TEXTURES).recordVisit;
    k->appW = app != nullptr ? *(const int32_t*) (app + BOXCACHE_SURFACE_WIDTH) : -1;
    k->appH = app != nullptr ? *(const int32_t*) (app + BOXCACHE_SURFACE_HEIGHT) : -1;
    memcpy(&k->top, &BX(r, DLG_TOP), sizeof(k->top));
    k->smoothing = (int32_t) BX(r, DISPLAY).smoothing;
}

// Two 32-bit lanes over each op's words, the last without its padding byte. Each step is one multiply a change (the
// constants are ones GCC keeps as a multiply, not shifts and adds), and change b folds its high bits down, so a change
// confined to the top bits of two words can't cancel in both lanes. Every frame with a box hashes its ops, so this is
// most of boxcache's own cost.
// It runs on from *ha and *hb (BOXCACHE_HASH_A and _B to start), so a prefix's hash is a step on the way to the whole.
#define BOXCACHE_HASH_A 2166136261u
#define BOXCACHE_HASH_B 0x9E3779B9u
static BX_CODE void bxHash(const BxOp* ops, ptrdiff_t from, ptrdiff_t to, uint32_t* ha, uint32_t* hb) {
    uint32_t a = *ha, b = *hb;
    for (ptrdiff_t i = from; to > i; i++) {
        const uint32_t* w = (const uint32_t*) (const void*) &ops[i];
        uint32_t v;
        for (uint32_t k = 0; BOXCACHE_OP_SIZE / 4 - 1 > k; k++) {
            v = w[k];
            a = (a ^ v) * 0x85EBCA77u;
            b = (b + v) * 0x9E3779B1u;
            b ^= b >> 15;
        }
        v = w[BOXCACHE_OP_SIZE / 4 - 1] & 0x00FFFFFFu; // the last word's top byte: padding
        a = (a ^ v) * 0x85EBCA77u;
        b = (b + v) * 0x9E3779B1u;
        b ^= b >> 15;
    }
    *ha = a;
    *hb = b;
}

// The tiles emitPageTiles asks the cache for when it draws page's source rectangle sx0..sy1 under no palette shader
// (presentDialogue's): its own sums, so the same tiles; false if it asks for none.
static BX_CODE bool bxTiles(const CtrPage* page, const BxOp* op, int32_t* tx0, int32_t* ty0, int32_t* tx1, int32_t* ty1) {
    float tileW = (float) page->tileW;
    float tileH = (float) page->tileH;
    if (page->tilesX == 1 && page->tilesY == 1) {
        *tx0 = *ty0 = *tx1 = *ty1 = 0;
        return true;
    }
    float sx0 = op->sx0, sy0 = op->sy0, sx1 = op->sx1, sy1 = op->sy1;
    float w = sx1 - sx0;
    float h = sy1 - sy0;
    if (w == 0.0f || h == 0.0f) return false;
    float minX = sx0 < sx1 ? sx0 : sx1, maxX = sx0 < sx1 ? sx1 : sx0;
    float minY = sy0 < sy1 ? sy0 : sy1, maxY = sy0 < sy1 ? sy1 : sy0;
    *tx0 = (int32_t) (minX / tileW), *tx1 = (int32_t) ((maxX - 0.001f) / tileW);
    *ty0 = (int32_t) (minY / tileH), *ty1 = (int32_t) ((maxY - 0.001f) / tileH);
    if (0 > *tx0) *tx0 = 0;
    if (0 > *ty0) *ty0 = 0;
    if (*tx1 >= page->tilesX) *tx1 = page->tilesX - 1;
    if (*ty1 >= page->tilesY) *ty1 = page->tilesY - 1;
    return *tx0 <= *tx1 && *ty0 <= *ty1;
}

// Whether presentDialogue draws every op whole: a colour, or a data.win page whose layout is known and whose tiles the
// draw asks for are all resident or known empty (so nothing is loaded or missed in the draw).
static BX_CODE bool bxDrawsWhole(void* r, const BxOp* ops, ptrdiff_t n) {
    const CtrTextureCache* t = &BX(r, TEXTURES);
    for (ptrdiff_t i = 0; n > i; i++) {
        const BxOp* op = &ops[i];
        if (op->pageId == BOXCACHE_COLOR_PAGE) continue;
        if (0 > op->pageId || (uint32_t) op->pageId >= t->pageCount) return false;
        const CtrPage* page = &t->pages[op->pageId];
        if (!page->layoutKnown || page->tiles == nullptr || 0 >= page->tilesX || 0 >= page->tilesY) return false;
        int32_t tx0, ty0, tx1, ty1;
        if (!bxTiles(page, op, &tx0, &ty0, &tx1, &ty1)) continue;
        for (int32_t ty = ty0; ty1 >= ty; ty++) {
            for (int32_t tx = tx0; tx1 >= tx; tx++) {
                const CtrTile* tile = &page->tiles[ty * page->tilesX + tx];
                if (!tile->valid && (!page->formatsKnown || tile->format != CTR_TILE_EMPTY)) return false;
            }
        }
    }
    return true;
}

// presentDialogue's bounds of ops from..to, folded on from *x0..*y1 as it folds them (minF and maxF: a compare each).
static BX_CODE void bxBounds(const BxOp* ops, ptrdiff_t from, ptrdiff_t to, float* x0, float* y0, float* x1, float* y1) {
    for (ptrdiff_t i = from; to > i; i++) {
        const float* x = ops[i].x;
        const float* y = ops[i].y;
        float a = x[0] < x[1] ? x[0] : x[1], b = x[2] < x[3] ? x[2] : x[3];
        float ox0 = a < b ? a : b;
        a = x[0] > x[1] ? x[0] : x[1], b = x[2] > x[3] ? x[2] : x[3];
        float ox1 = a > b ? a : b;
        a = y[0] < y[1] ? y[0] : y[1], b = y[2] < y[3] ? y[2] : y[3];
        float oy0 = a < b ? a : b;
        a = y[0] > y[1] ? y[0] : y[1], b = y[2] > y[3] ? y[2] : y[3];
        float oy1 = a > b ? a : b;
        *x0 = *x0 < ox0 ? *x0 : ox0;
        *y0 = *y0 < oy0 ? *y0 : oy0;
        *x1 = *x1 > ox1 ? *x1 : ox1;
        *y1 = *y1 > oy1 ? *y1 : oy1;
    }
}

// Whether ops from..to lie inside the base's bounds (every corner; a NaN never does), so folding them in changes none.
static BX_CODE bool bxInside(const BxOp* ops, ptrdiff_t from, ptrdiff_t to) {
    BxState* state = bxRoot;
    for (ptrdiff_t i = from; to > i; i++) {
        for (int32_t k = 0; 4 > k; k++) {
            float x = ops[i].x[k], y = ops[i].y[k];
            if (!(x >= bxBaseX0 && bxBaseX1 >= x && y >= bxBaseY0 && bxBaseY1 >= y)) return false;
        }
    }
    return true;
}

// A hidden op's page from the 2 bytes kept for it (2 KB more of .bss took S7's data segment a page further, 4 KB of RAM):
// the colour page as -1, a data.win page as itself (a typed frame needs every page id under 32768).
static BX_CODE int32_t bxPageOf(int16_t kept) {
    return kept == -1 ? BOXCACHE_COLOR_PAGE : (int32_t) kept;
}

// What the draw does to the texture cache, op by op, in its order: each page's use (CtrTextures_acquire), and its
// tiles' (CtrTextures_ensureTilesBut: their use, the tiles in VRAM wanted, the room's record), so that a kept box ages
// and records nothing differently from a drawn one. pages: the ones the ops had before they were hidden.
static BX_CODE void bxTouch(void* r, const BxOp* ops, ptrdiff_t n, const int16_t* pages) {
    CtrTextureCache* t = &BX(r, TEXTURES);
    for (ptrdiff_t i = 0; n > i; i++) {
        const BxOp* op = &ops[i];
        int32_t pageId = bxPageOf(pages[i]);
        if (pageId == BOXCACHE_COLOR_PAGE) continue;
        CtrPage* page = CtrTextures_acquire(t, (uint32_t) pageId);
        int32_t tx0, ty0, tx1, ty1;
        if (page != nullptr && bxTiles(page, op, &tx0, &ty0, &tx1, &ty1)) CtrTextures_ensureTilesBut(t, page, tx0, ty0, tx1, ty1, 0);
    }
}

// ctr_renderer.c's clears. On a frame with hidden ops, the bottom target's first (presentDialogue's, right before its
// ops) is not made: the target keeps the last draw, and the hidden ops' cache uses go here instead. If a tile was evicted
// since they were checked and one of theirs is gone, they get their pages back here and the clear is made: a full draw.
BX_CODE void CtrBoxcache_frameBufClear(C3D_FrameBuf* fb, C3D_ClearBits clearBits, u32 clearColor, u32 clearDepth) {
    BxState* state = bxRoot;
    if ((uintptr_t) state > 1 && bxTypedFrom != 0 && !bxCleared && fb == &BX(bxRenderer, BOTTOM_TARGET)->frameBuf) {
        BxOp* ops = BX(bxRenderer, DLG_OPS);
        if (BX(bxRenderer, TEXTURES).evictionCount != bxEvictions) {
            for (uint32_t i = 0; bxTypedFrom > i; i++) ops[i].pageId = bxPageOf(bxHidden[i]);
            if (!bxDrawsWhole(bxRenderer, ops, (ptrdiff_t) bxTypedFrom)) {
                bxTypedFrom = 0; // so the frame's end finds no clear made here: it starts over
                C3D_FrameBufClear(fb, clearBits, clearColor, clearDepth);
                return;
            }
            for (uint32_t i = 0; bxTypedFrom > i; i++) ops[i].pageId = BOXCACHE_SKIP_PAGE;
        }
        bxCleared = true;
        bxTouch(bxRenderer, ops, (ptrdiff_t) bxTypedFrom, bxHidden);
        return;
    }
    C3D_FrameBufClear(fb, clearBits, clearColor, clearDepth);
}

BX_CODE void CtrRenderer_chainEndFrameBoxcache(Renderer* renderer) {
    if (!bxReady()) { __real_CtrRenderer_endFrame(renderer); return; }
    BxState* state = bxRoot;
    void* r = renderer;
    uint64_t now = svcGetSystemTick();
    bool late = now - bxLastTick >= BOXCACHE_LATE_TICKS;
    bxLastTick = now;
    bool presents = BX(r, IN_FRAME) && !BX(r, SKIP_PRESENT) && !BX(r, SKIP_ACTIVE) && BX(r, BOTTOM_TARGET) != nullptr &&
                    !BX(r, BOTTOM_YIELDED);
    BxOp* ops = BX(r, DLG_OPS);
    ptrdiff_t n = presents && BX(r, DLG_ARMED) && !BX(r, DLG_DROPPED) ? arrlen(ops) : 0;
    uint32_t a = BOXCACHE_HASH_A, b = BOXCACHE_HASH_B;
    bool whole = false, typed = false;
    if (n > 0) {
        __typeof__(bxBaseKey) key;
        bxKeyOf(r, &key);
        whole = bxDrawsWhole(r, ops, n);
        bool fits = whole && !late && 0 < bxBaseCount && (uint32_t) n >= bxBaseCount && memcmp(&key, &bxBaseKey, sizeof(key)) == 0 &&
                    BX(r, DLG_SHOWN) && BX(r, DISPLAY).dialogueBottom && BX(r, DLG_GAP) == 0;
        ptrdiff_t m = fits ? (ptrdiff_t) bxBaseCount : 0;
        bxHash(ops, 0, m, &a, &b); // the ops last drawn, if these start with them
        bool start = fits && bxBaseA == a && bxBaseB == b;
        bxHash(ops, m, n, &a, &b); // and on to this frame's whole hash
        typed = start && BOXCACHE_TYPED_MAX >= m && 32768u >= BX(r, TEXTURES).pageCount && bxInside(ops, m, n); // none new: kept
    }
    uint32_t shown = BX(r, DLG_SHOWN_FRAMES);
    uint32_t evictions = BX(r, TEXTURES).evictionCount;
    bxRenderer = r;
    state->same = typed && n == (ptrdiff_t) bxBaseCount;
    if (typed) {
        bxTypedFrom = bxBaseCount; // the ops last drawn are hidden; the new ones, if any, draw onto the last draw
        bxCleared = false;
        bxRenderer = r;
        bxEvictions = evictions;
        for (uint32_t i = 0; bxTypedFrom > i; i++) {
            bxHidden[i] = ops[i].pageId == BOXCACHE_COLOR_PAGE ? -1 : (int16_t) ops[i].pageId; // whole: 0 <= pageId < pageCount
            ops[i].pageId = BOXCACHE_SKIP_PAGE;
        }
    }
    __real_CtrRenderer_endFrame(renderer);
    bxRenderer = nullptr;
    ops = BX(r, DLG_OPS); // read again: the frame's end adds none, but if it ever did, the array may have moved
    if (typed) {
        for (uint32_t i = 0; bxTypedFrom > i; i++) ops[i].pageId = bxPageOf(bxHidden[i]);
        bool cleared = bxCleared;
        bxTypedFrom = 0;
        bxCleared = false;
        if (!cleared) { // presentDialogue didn't draw as asked, or drew them all (a tile gone): start over
            bxBaseCount = 0;
            return;
        }
#if !defined(CTRPORT_PAINTCHG_RELEASE)
        if (n == (ptrdiff_t) bxBaseCount) CtrBoxcache_kept++;
        else CtrBoxcache_typed++;
#endif
    }
    // The base: a draw of whole ops, still whole after it (checked again only if the frame's end evicted any tile).
    if (BX(r, DLG_SHOWN_FRAMES) == shown + 1u && whole && arrlen(ops) == n &&
        (BX(r, TEXTURES).evictionCount == evictions || bxDrawsWhole(r, ops, n))) {
        bxBaseCount = (uint32_t) n;
        bxBaseA = a;
        bxBaseB = b;
        bxKeyOf(r, &bxBaseKey);
        if (!typed) { // a kept or typed frame's ops are inside the bounds already
            bxBaseX0 = bxBaseY0 = __builtin_inff();
            bxBaseX1 = bxBaseY1 = -__builtin_inff();
            bxBounds(ops, 0, n, &bxBaseX0, &bxBaseY0, &bxBaseX1, &bxBaseY1);
        }
    } else if (BX(r, DLG_SHOWN_FRAMES) != shown || !BX(r, DLG_SHOWN) || BX(r, BOTTOM_YIELDED) || BX(r, BOTTOM_TARGET) == nullptr) {
        bxBaseCount = 0;
    }
}
#endif
