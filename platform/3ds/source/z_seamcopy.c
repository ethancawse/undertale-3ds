// Seam copies (change drawbatch; UNDERTALE on the Old models only): the copies themselves. ctr_renderer.c's end says what they are for and
// draws from them; this file makes, keeps and frees them. Late code only (platform/3ds/late_reserve.ld): its sections
// are named outside .text.late.* and listed there, so nothing that existed before moves.
//
// A copy holds the texels a draw takes from up to 2 x 2 cache tiles: each tile's part, with the ring of texels around it
// that a filter may read, at the part's own place on a texture of the tile's size and format. The ring is widened to
// whole 8 x 8 blocks of the tiled texture, so each part is one GX copy: every texel copied is the tile's own,
// at its own place. A part on a tile's far side and one on the next tile's near side so sit apart on the copy (a draw
// some 16 texels narrower than a tile), and each part is drawn with the tile's own texture coordinates: every texel any
// filter reads for it, and the clamping at the texture's edges, are the tile's, so the picture is the same bit for bit.
// Copies are made when drawn, of 16-bit tiles only (SEAM_COPY_BYTES_MAX), with at most SEAM_FILL_MAX bytes of blocks
// copied a frame; at most SEAM_SLOTS of them and SEAM_BYTES_MAX in all, at most SEAM_BUILDS_PER_FRAME a frame, and freed
// no sooner than two frames after they were last drawn (the GPU may still read them until then, as with
// smartCopySlot's copies): when their room is needed, when a tile they copied changed, or after SEAM_IDLE_FRAMES unused.
//
// They live at the top of VRAM, as the Old models' tiles' copies do (ctr_renderer.c, "Tiles' copies in VRAM on the Old
// models"), and take nothing from linear memory or the texture budget: in linear memory, even with its total kept
// clear of every floor, a copy's blocks came and went among what the game holds there, so a 288 KB block a screenshot
// wanted and a 424 KB one for a sound's copy were not to be had with 4.4 MB free in all, and a loan from the budget
// left fewer tiles resident than without copies, which moved when sound effects were shed. The CPU can't write VRAM on
// a console (docs/3ds-hardware.md), so a copy is filled by GX copies from the tiles (seamFill), one for each tile's
// rows of blocks. They are queued ahead of everything drawn from the copy, which all goes to the GPU later in the same
// GX queue, and a tile drawn in a frame stays until the frame is done. VRAM has room to spare in UNDERTALE (2 MB free at
// the least, with the tiles' copies there): anything else that wants it comes first.
//
// A copy saves the draws that a texture change between parts costs, so a region gets one only once it has been drawn
// SEAM_ADMIT_DRAWS times in one frame (a region drawn once a frame saves a draw or none). When the copies drawn from
// fill the room, a region drawn SEAM_RETIRE_FACTOR times as often in this frame as the least drawn copy was (in this
// frame or the one before) retires that copy: it is drawn from no more and freed two frames later, as a changed one
// is, and its region may not have a copy again for SEAM_COOLDOWN_FRAMES, so the region that outranked it gets the
// room. A bullet drawn a hundred times a frame so outranks a sprite drawn once.
//
// The copies are looked after at every frame's start (CtrSeam_beginFrame, in place of CtrTextures_beginFrame), drawn
// from or not: idle and changed ones are freed there, so a copy never outlives its use by more than SEAM_IDLE_FRAMES
// however the game goes on. They belong to one texture cache, whose tiles they copy: they are all freed before it is
// destroyed (CtrSeam_destroy, on CtrTextures_destroy's way), and a frame of another cache frees any left. A try at a
// copy takes its share of the frame's SEAM_BUILDS_PER_FRAME and SEAM_FILL_MAX before the allocation; when that fails,
// that region waits SEAM_RETRY_FRAMES and every region SEAM_FAIL_FRAMES.
//
// A room change frees them all first (CtrSeam_prefetchRoom, in place of CtrTextures_prefetchRoom), so a new room's
// surfaces find VRAM with no copies in it. And they keep clear of what else VRAM is for: none is made with less than
// SEAM_VRAM_FLOOR free after it, all are freed at a frame's start with less than SEAM_VRAM_LOW free (above what the tiles'
// copies leave free, MIRROR_RESERVE, so they go first and those copies are made as if these weren't there), and a VRAM
// texture or render target that wants room turns them all to stale ones (CtrSeam_vramWanted). None is drawn from while
// 3D is shown (seamLookup).
//
// Most draws that cross a seam never get a copy (too wide, of 32-bit tiles, or drawn once a frame), and looking each one
// up costs more than the draw can save. So a draw is looked up only when it can use the answer (ctr_seamcopy.h's
// CtrSeam_worthAsking and CtrSeam_demand): when its region was drawn before in this frame, or a copy held covers it.
// Nothing here depends on how long the game has been doing something else: the same draws get the same copies whenever
// they come. The first draw of a region in a frame costs one hash and one store; CtrSeam_drawn.seen forgets a region only
// when another takes its place, and the frame number is in the hash, so no two regions keep doing that frame after frame.
// A lookup that finds the region is waiting (or every region is) notes that there, and the frame's other draws of it go by.
// One that finds no copy can hold it notes that in CtrSeam_drawn.noFit, and its draws go by for the next
// CTR_SEAM_NOFIT_FRAMES frames, as no copy that is current covers what can't have a copy (seamFits' tests can only fail
// for a larger region, on the same tiles, if they fail for this one). What makes a region fit is a tile changing, so it is
// looked up again that soon after, whatever it has been drawn like. The frame that notes it goes on looking, so a
// region with the same key is seen in that frame, and can't be kept out for good.
#include "ctr_seamcopy.h"

#if defined(ENABLE_WAD16) && !defined(CTRPORT_SMART_NO_DIRECT) && !defined(CTRPORT_SEAM_OFF)
#include <math.h>
#include <string.h>

#include "ctr_memory.h"
#include "log.h"

void __real_CtrTextures_destroy(CtrTextureCache* cache); // CtrTextures_destroy itself (it is wrapped: CMakeLists.txt)
void* CtrMemory_vramAllocTop(size_t size); // ctr_memory.c
void* CtrVramTouch_touch(void* mem, size_t bytes); // ctr_vram_touch.c: on Azahar, as every new VRAM block
bool CtrSeam_gxRoom(CtrTextureCache* cache, uint32_t count); // ctr_renderer.c's end
bool __real_C3D_TexInitWithParams(C3D_Tex* tex, C3D_TexCube* cube, C3D_TexInitParams p); // citro3d's (wrapped: CMakeLists.txt)
C3D_RenderTarget* __real_C3D_RenderTargetCreate(int width, int height, GPU_COLORBUF colorFmt, C3D_DEPTHTYPE depthFmt);

#define SEAM_SLOTS 4
#define SEAM_BYTES_MAX (256u * 1024u) // two copies
#define SEAM_COPY_BYTES_MAX (128u * 1024u) // a 16-bit 256 x 256 tile
#define SEAM_FILL_MAX (64u * 1024u) // bytes of blocks copied from tiles a frame
#define SEAM_COOLDOWN_FRAMES 30u
#define SEAM_BUILDS_PER_FRAME 4u
#define SEAM_IDLE_FRAMES 300u
#define SEAM_VRAM_FLOOR (1024u * 1024u)
#define SEAM_VRAM_LOW (768u * 1024u) // ctr_renderer.c's MIRROR_RESERVE is 512 KB; lower than the floor, or freeing a copy could let the next one be made
#define SEAM_ADMIT_DRAWS 2u
#define SEAM_RETIRE_FACTOR 4u
#define SEAM_ASKS 8 // regions without a copy whose draws are being counted
#define SEAM_RETRY_FRAMES 60u // a region no copy could be laid out or had for waits this long before it is tried again
#define SEAM_FAIL_FRAMES 30u // and after an allocation failed, every region waits this long
#define SEAM_TEXT __attribute__((noinline, noclone, section(".drawbatch_text")))
#define SEAM_RO __attribute__((section(".drawbatch_text_ro")))

typedef struct {
    const CtrPage* page;
    int32_t x0, y0, x1, y1;
    uint32_t frame, count; // drawn count times in frame `frame`
    uint32_t need; // the count at which a copy is tried in this frame
    uint32_t until; // no copy is tried before this frame
} SeamAsk;

static struct {
    CtrTextureCache* cache; // the copies' owner: whose tiles they copy, whose frames look after them
    uint32_t buildFrame, buildsInFrame;
    uint32_t fillInFrame;
    uint32_t failUntil; // no copy is tried before this frame
    uint32_t bytes, peakBytes, builds, hits, freed, refused, retired, failed;
    const CtrPage* coolPage; // the region of the copy retired last, and the frame it may have a copy again
    int32_t coolX0, coolY0, coolX1, coolY1;
    uint32_t coolUntil;
    SeamCopy slots[SEAM_SLOTS];
    SeamAsk asks[SEAM_ASKS];
} store __attribute__((section(".bss.drawbatch_late")));

// The paid late-data reserve holds these zeroed counters so S7 stays in its last bss page.
CtrSeamDrawn CtrSeam_drawn __attribute__((section(".data.drawbatch_late"))) = {0};

static const char seamPeakFmt[] SEAM_RO = "Seam copies: %lu B held at most (%lu made, %lu freed, %lu refused, %lu retired)\n";
static const char seamBuildFmt[] SEAM_RO =
    "Seam copy %lu: page %ld texels %ld,%ld to %ld,%ld on %ldx%ld tiles, %dx%d format %d (%lu B, %lu B held)\n";
static const char seamNoneFmt[] SEAM_RO = "Seam copies: none held from frame %lu (%lu made, %lu freed, %lu refused, %lu failed)\n";

static void SEAM_TEXT seamFree(SeamCopy* k) {
    C3D_TexDelete(&k->tex);
    store.bytes -= k->bytes;
    CtrSeam_drawn.held = store.bytes;
    store.freed++;
    k->page = nullptr;
}

// Whether the tiles k copied still hold what it copied: each one copied resident with the same texels, each one
// left out still clear.
static bool SEAM_TEXT seamCurrent(const SeamCopy* k) {
    for (int32_t l = 0; k->nty > l; l++) {
        for (int32_t m = 0; k->ntx > m; m++) {
            const CtrTile* t = &k->page->tiles[(k->ty0 + l) * k->page->tilesX + k->tx0 + m];
            if (k->data[l][m] == nullptr ? t->valid : !t->valid || t->tex.data != k->data[l][m] || t->format != k->format) return false;
        }
    }
    return true;
}

// Draws from copy c in this frame, and in the frame before it.
static inline __attribute__((always_inline)) uint32_t seamDrawsNow(const SeamCopy* c, uint32_t frame) {
    return c->hitsFrame == frame ? c->hitsNow : 0u;
}
static inline __attribute__((always_inline)) uint32_t seamDrawsLast(const SeamCopy* c, uint32_t frame) {
    return c->hitsFrame == frame ? c->hitsLast : c->hitsFrame + 1u == frame ? c->hitsNow : 0u;
}

// The count of page texels [x0, x1) x [y0, y1): how many times they have been drawn in this frame without a copy, this
// draw included.
static SeamAsk* SEAM_TEXT seamAsk(const CtrPage* page, uint32_t frame, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    SeamAsk* o = &store.asks[0];
    uint32_t least = 0xffffffffu;
    for (int32_t i = 0; SEAM_ASKS > i; i++) {
        SeamAsk* a = &store.asks[i];
        uint32_t n = a->page != nullptr && a->frame == frame ? a->count : 0u;
        if (a->page == page && a->x0 == x0 && a->y0 == y0 && a->x1 == x1 && a->y1 == y1) {
            if (a->frame != frame) a->need = SEAM_ADMIT_DRAWS;
            a->frame = frame;
            a->count = n + 1u;
            return a;
        }
        if (least > n) least = n, o = a;
    }
    o->page = page;
    o->x0 = x0, o->y0 = y0, o->x1 = x1, o->y1 = y1;
    o->frame = frame;
    o->count = 1u;
    o->need = SEAM_ADMIT_DRAWS;
    o->until = frame;
    return o;
}

static inline __attribute__((always_inline)) uint32_t seamBpp(uint8_t format) {
    return format == CTR_TILE_RGBA8 ? 4u : format == CTR_TILE_RGB8 ? 3u : 2u;
}

// Each part of a copy with its ring, widened to whole 8 x 8 blocks, on its tile (inclusive): column m's texels lx[m][0]
// to lx[m][1], row l's ly[l][0] to ly[l][1].
typedef struct {
    int32_t lx[2][2], ly[2][2];
    uint32_t fill; // bytes of blocks copied
} SeamRings;

// k's page, texels, tiles, format and tile texels for a copy of page texels [x0, x1) x [y0, y1), and its parts' rings:
// false when no copy can hold them.
static bool SEAM_TEXT seamLayout(CtrPage* page, int32_t x0, int32_t y0, int32_t x1, int32_t y1, SeamCopy* k, SeamRings* g) {
    int32_t tileW = page->tileW, tileH = page->tileH;
    int32_t tx0 = x0 / tileW, ty0 = y0 / tileH, tx1 = (x1 - 1) / tileW, ty1 = (y1 - 1) / tileH;
    if (tx1 >= page->tilesX || ty1 >= page->tilesY || tx1 - tx0 > 1 || ty1 - ty0 > 1) return false;
    memset(k, 0, sizeof(*k));
    k->page = page;
    k->x0 = x0, k->y0 = y0, k->x1 = x1, k->y1 = y1;
    k->tx0 = tx0, k->ty0 = ty0, k->ntx = tx1 - tx0 + 1, k->nty = ty1 - ty0 + 1;
    // The ring is the texel before the part and the one after it, where the tile has them (at the tile's edge clamping
    // reads the edge texel, as it does at the copy's), out to whole blocks (tiles are whole blocks). Two columns' (rows')
    // parts must not meet.
    for (int32_t m = 0; k->ntx > m; m++) {
        int32_t s = (tx0 + m) * tileW, a = x0 > s ? x0 - s : 0, b = x1 < s + tileW ? x1 - s : tileW;
        g->lx[m][0] = (0 < a ? a - 1 : 0) & ~7;
        g->lx[m][1] = (tileW > b ? b : tileW - 1) | 7;
    }
    for (int32_t l = 0; k->nty > l; l++) {
        int32_t s = (ty0 + l) * tileH, a = y0 > s ? y0 - s : 0, b = y1 < s + tileH ? y1 - s : tileH;
        g->ly[l][0] = (0 < a ? a - 1 : 0) & ~7;
        g->ly[l][1] = (tileH > b ? b : tileH - 1) | 7;
    }
    if ((k->ntx == 2 && g->lx[1][1] >= g->lx[0][0]) || (k->nty == 2 && g->ly[1][1] >= g->ly[0][0])) return false;
    bool any = false;
    for (int32_t l = 0; k->nty > l; l++) {
        for (int32_t m = 0; k->ntx > m; m++) {
            const CtrTile* t = &page->tiles[(ty0 + l) * page->tilesX + tx0 + m];
            if (!t->valid) {
                // A clear tile draws nothing (emitPageTiles skips it); one not loaded yet can't be copied.
                if (t->format != CTR_TILE_EMPTY || t->reserved) return false;
                continue;
            }
            if (t->tex.width != tileW || t->tex.height != tileH || (any && t->format != k->format)) return false;
            k->format = t->format;
            k->data[l][m] = t->tex.data;
            any = true;
        }
    }
    // What the parts' blocks come to.
    g->fill = 0;
    for (int32_t l = 0; k->nty > l; l++) {
        for (int32_t m = 0; k->ntx > m; m++) {
            if (k->data[l][m] != nullptr) g->fill += (uint32_t) ((g->lx[m][1] - g->lx[m][0] + 1) * (g->ly[l][1] - g->ly[l][0] + 1));
        }
    }
    g->fill *= seamBpp(k->format);
    return any;
}

// Queues a GX copy of each of k's parts (with their rings) to its place on the copy, its place on its tile. A tiled
// texture's blocks lie row by row, each block's 64 texels together (ctr_renderer.c's holoMorton), so a part is lines of
// whole blocks with a gap between them, in units of 16 bytes (a block is 128 bytes at 16 bits a texel). Only the parts
// are written: nothing draws from the texels between them.
static void SEAM_TEXT seamFill(const SeamCopy* k, const SeamRings* g) {
    int32_t blocksW = k->page->tileW >> 3;
    uint32_t block = 64u * seamBpp(k->format), line = (uint32_t) blocksW * block;
    for (int32_t l = 0; k->nty > l; l++) {
        for (int32_t m = 0; k->ntx > m; m++) {
            if (k->data[l][m] == nullptr) continue;
            int32_t bx0 = g->lx[m][0] >> 3, by0 = g->ly[l][0] >> 3;
            uint32_t run = (uint32_t) ((g->lx[m][1] >> 3) - bx0 + 1) * block, rows = (uint32_t) ((g->ly[l][1] >> 3) - by0 + 1);
            uint32_t at = ((uint32_t) by0 * (uint32_t) blocksW + (uint32_t) bx0) * block;
            u32 dim = GX_BUFFER_DIM(run >> 4, (line - run) >> 4);
            GX_TextureCopy((u32*) ((const uint8_t*) k->data[l][m] + at), dim, (u32*) ((uint8_t*) k->tex.data + at), dim, run * rows,
                           GX_TRANSFER_RAW_COPY(1));
        }
    }
}

// Whether this frame may make another copy, copying fill bytes of blocks.
static bool SEAM_TEXT seamMayBuild(uint32_t frame, uint32_t fill) {
    if (store.buildFrame != frame) {
        store.buildFrame = frame;
        store.buildsInFrame = 0;
        store.fillInFrame = 0;
    }
    return SEAM_BUILDS_PER_FRAME > store.buildsInFrame && SEAM_FILL_MAX >= store.fillInFrame + fill;
}

// VRAM for a copy could not be had: its region waits, and every region a while.
static void SEAM_TEXT seamFailed(SeamAsk* ask, uint32_t frame) {
    ask->until = frame + SEAM_RETRY_FRAMES;
    store.failUntil = frame + SEAM_FAIL_FRAMES;
    store.failed++;
}

// A new copy of page texels [x0, x1) x [y0, y1), counted by `ask`, or nullptr if it can't be had.
static SeamCopy* SEAM_TEXT seamBuild(CtrTextureCache* cache, CtrPage* page, uint32_t frame, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                                     SeamAsk* ask) {
    uint32_t asked = ask->count;
    SeamCopy k;
    SeamRings g;
    if (!seamLayout(page, x0, y0, x1, y1, &k, &g)) {
        // Its tiles differ in size or format, or one is not loaded yet: tried again later, not at every draw.
        ask->until = frame + SEAM_RETRY_FRAMES;
        return nullptr;
    }
    k.bytes = (uint32_t) page->tileW * (uint32_t) page->tileH * seamBpp(k.format);
    if (k.bytes > SEAM_COPY_BYTES_MAX) {
        ask->until = frame + SEAM_RETRY_FRAMES;
        return nullptr;
    }
    if (!seamMayBuild(frame, g.fill)) {
        // This frame's copies are spent: the next frame, or later if no frame's would do.
        ask->until = frame + (g.fill > SEAM_FILL_MAX ? SEAM_RETRY_FRAMES : 1u);
        return nullptr;
    }
    // A region whose copy was retired waits, so the one that outranked it gets the room.
    if (store.coolPage == page && store.coolX0 == x0 && store.coolY0 == y0 && store.coolX1 == x1 && store.coolY1 == y1 &&
        (int32_t) (store.coolUntil - frame) > 0) {
        ask->until = store.coolUntil;
        return nullptr;
    }
    // A slot, and room under SEAM_BYTES_MAX: copies not drawn this frame or the one before go, oldest first.
    SeamCopy* slot;
    for (;;) {
        slot = nullptr;
        for (int32_t i = 0; SEAM_SLOTS > i && slot == nullptr; i++) {
            if (store.slots[i].page == nullptr) slot = &store.slots[i];
        }
        if (slot != nullptr && SEAM_BYTES_MAX >= store.bytes + k.bytes) break;
        SeamCopy* old = nullptr;
        for (int32_t i = 0; SEAM_SLOTS > i; i++) {
            SeamCopy* c = &store.slots[i];
            if (c->page != nullptr && frame - c->usedFrame >= 2 && (old == nullptr || frame - c->usedFrame > frame - old->usedFrame)) old = c;
        }
        if (old == nullptr) {
            // Every copy has been drawn lately: the least drawn retires if this region outranks it.
            SeamCopy* weak = nullptr;
            uint32_t least = 0;
            for (int32_t i = 0; SEAM_SLOTS > i; i++) {
                SeamCopy* c = &store.slots[i];
                if (c->page == nullptr || c->stale) continue;
                uint32_t n = seamDrawsNow(c, frame), l = seamDrawsLast(c, frame);
                n = n > l ? n : l;
                if (weak == nullptr || least > n) weak = c, least = n;
            }
            if (weak != nullptr && asked / SEAM_RETIRE_FACTOR >= least) {
                weak->stale = true;
                store.retired++;
                store.coolPage = weak->page;
                store.coolX0 = weak->x0, store.coolY0 = weak->y0, store.coolX1 = weak->x1, store.coolY1 = weak->y1;
                store.coolUntil = frame + SEAM_COOLDOWN_FRAMES;
                // Its room is free two frames on.
                ask->until = frame + 2u;
            } else if (weak != nullptr) {
                // Tried again when drawn often enough in this frame to outrank the least drawn copy.
                ask->need = least * SEAM_RETIRE_FACTOR;
            } else {
                // Every copy is on its way out.
                ask->until = frame + 1u;
            }
            store.refused++;
            return nullptr;
        }
        seamFree(old);
    }
    // The try is charged before the allocation, which is what costs.
    store.buildsInFrame++;
    store.fillInFrame += g.fill;
    // The copy's texture is a tile's with another place in memory (as a tile's copy in VRAM is), and there is one GX copy
    // a tile.
    const CtrTile* first = nullptr;
    uint32_t parts = 0;
    for (int32_t l = 0; k.nty > l; l++) {
        for (int32_t m = 0; k.ntx > m; m++) {
            if (k.data[l][m] == nullptr) continue;
            parts++;
            if (first == nullptr) first = &page->tiles[(k.ty0 + l) * page->tilesX + k.tx0 + m];
        }
    }
    void* vram = first->tex.size != k.bytes || SEAM_VRAM_FLOOR + k.bytes > CtrMemory_vramFreeNow() ? nullptr : CtrMemory_vramAllocTop(k.bytes);
    if (vram == nullptr) {
        seamFailed(ask, frame);
        return nullptr;
    }
    if (!CtrSeam_gxRoom(cache, parts)) {
        vramFree(vram);
        seamFailed(ask, frame);
        return nullptr;
    }
    CtrVramTouch_touch(vram, k.bytes);
    k.tex = first->tex;
    k.tex.data = vram;
    seamFill(&k, &g);
    C3D_TexSetWrap(&k.tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    k.usedFrame = frame;
    k.builtFrame = frame;
    k.hitsFrame = frame, k.hitsNow = asked, k.hitsLast = asked;
    *slot = k;
    store.builds++;
    store.bytes += k.bytes;
    CtrSeam_drawn.held = store.bytes;
    if (store.bytes > store.peakBytes) {
        store.peakBytes = store.bytes;
        logInfo(seamPeakFmt, (unsigned long) store.peakBytes, (unsigned long) store.builds, (unsigned long) store.freed,
                (unsigned long) store.refused, (unsigned long) store.retired);
    }
    if (8 >= store.builds || (store.builds & 0xffu) == 0) {
        logInfo(seamBuildFmt, (unsigned long) store.builds, (long) (page - cache->pages), (long) x0, (long) y0, (long) x1, (long) y1, (long) k.ntx,
                (long) k.nty, (int) page->tileW, (int) page->tileH, (int) k.format, (unsigned long) k.bytes, (unsigned long) store.bytes);
    }
    return slot;
}

// The least whole number at or above f.
static inline __attribute__((always_inline)) int32_t seamCeil(float f) {
    int32_t i = (int32_t) f;
    return (float) i < f ? i + 1 : i;
}

// Whether page texels [x0, x1) x [y0, y1) could have a copy: on at most 2 x 2 tiles of 16 bits a texel and the tile
// size, whose parts with their rings, out to whole blocks, don't meet. These are seamLayout's and seamBuild's tests, by shifts (the ARM11
// divides in software) and without their work, so that a region no copy can hold, drawn many times a frame, costs a
// few instructions a draw and is not counted.
static bool SEAM_TEXT seamFits(const CtrPage* page, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    int32_t tileW = page->tileW, tileH = page->tileH;
    if (0 >= tileW || 0 >= tileH || (tileW & (tileW - 1)) != 0 || (tileH & (tileH - 1)) != 0) return false;
    int32_t sw = __builtin_ctz((unsigned) tileW), sh = __builtin_ctz((unsigned) tileH);
    int32_t tx0 = x0 >> sw, ty0 = y0 >> sh, tx1 = (x1 - 1) >> sw, ty1 = (y1 - 1) >> sh;
    if (tx1 >= page->tilesX || ty1 >= page->tilesY || tx1 - tx0 > 1 || ty1 - ty0 > 1) return false;
    if (tx1 != tx0) {
        int32_t a = x0 - (tx0 << sw), b = x1 - (tx1 << sw);
        if (((tileW > b ? b : tileW - 1) | 7) >= ((0 < a ? a - 1 : 0) & ~7)) return false;
    }
    if (ty1 != ty0) {
        int32_t a = y0 - (ty0 << sh), b = y1 - (ty1 << sh);
        if (((tileH > b ? b : tileH - 1) | 7) >= ((0 < a ? a - 1 : 0) & ~7)) return false;
    }
    for (int32_t ty = ty0; ty1 >= ty; ty++) {
        for (int32_t tx = tx0; tx1 >= tx; tx++) {
            const CtrTile* t = &page->tiles[ty * page->tilesX + tx];
            if (t->valid && (seamBpp(t->format) != 2u || t->tex.width != tileW || t->tex.height != tileH)) return false;
        }
    }
    return true;
}

SeamCopy* SEAM_TEXT CtrSeam_lookup(CtrTextureCache* cache, CtrPage* page, uint32_t frame, float fx0, float fy0, float fx1, float fy1) {
    uint32_t* seen = CtrSeam_drawn.seenNow;
    CtrSeam_drawn.seenNow = nullptr;
    if (cache != store.cache || page == nullptr || page < cache->pages || page >= cache->pages + cache->pageCount ||
        !page->layoutKnown || (page->tilesX == 1 && page->tilesY == 1))
        return nullptr;
    // The texels rounded out (truncating is flooring here, and a texel bound is past the page's edge when its ceiling is).
    if (0.0f > fx0 || 0.0f > fy0 || fx1 > (float) page->width || fy1 > (float) page->height) return nullptr;
    int32_t x0 = (int32_t) fx0, y0 = (int32_t) fy0, x1 = seamCeil(fx1), y1 = seamCeil(fy1);
    if (x0 >= x1 || y0 >= y1) return nullptr;
    SeamCopy* c = nullptr;
    for (int32_t i = 0; SEAM_SLOTS > i && c == nullptr; i++) {
        SeamCopy* s = &store.slots[i];
        if (s->page != page || s->stale || x0 < s->x0 || y0 < s->y0 || x1 > s->x1 || y1 > s->y1) continue;
        if (!seamCurrent(s)) {
            s->stale = true;
            continue;
        }
        c = s;
    }
    if (c == nullptr) {
        if (!seamFits(page, x0, y0, x1, y1)) {
            if (seen != nullptr) {
                uint32_t key = CtrSeam_drawn.keyNow;
                uint32_t i = (key * 0x9e3779b1u) >> CTR_SEAM_NOFIT_SHIFT;
                CtrSeam_drawn.noFit[i].key = key;
                CtrSeam_drawn.noFit[i].frame = frame;
            }
            return nullptr;
        }
        SeamAsk* ask = seamAsk(page, frame, x0, y0, x1, y1);
        // CtrSeam_demand let the draw before it go uncounted.
        if (seen != nullptr && ask->count == 1u) ask->count = 2u;
        if ((int32_t) (ask->until - frame) > 0 || (int32_t) (store.failUntil - frame) > 0) {
            // Nothing changes that in this frame.
            if (seen != nullptr) *seen ^= 1u;
            return nullptr;
        }
        return ask->need > ask->count ? nullptr : seamBuild(cache, page, frame, x0, y0, x1, y1, ask);
    }
    c->usedFrame = frame;
    if (c->hitsFrame != frame) {
        c->hitsLast = c->hitsFrame + 1u == frame ? c->hitsNow : 0u;
        c->hitsNow = 0;
        c->hitsFrame = frame;
    }
    c->hitsNow++;
    store.hits++;
    return c;
}

bool SEAM_TEXT CtrSeam_covered(int32_t x, int32_t y) {
    for (int32_t i = 0; SEAM_SLOTS > i; i++) {
        const SeamCopy* s = &store.slots[i];
        if (s->page != nullptr && x >= s->x0 && y >= s->y0 && x < s->x1 && y < s->y1) return true;
    }
    return false;
}

// Frees every copy, and forgets what was counted and timed: those frames are the owner's, and the next owner counts its
// own.
static void SEAM_TEXT seamDrain(void) {
    for (int32_t i = 0; SEAM_SLOTS > i; i++) {
        SeamCopy* c = &store.slots[i];
        if (c->page == nullptr) continue;
        C3D_TexDelete(&c->tex);
        store.bytes -= c->bytes;
        store.freed++;
        c->page = nullptr;
    }
    memset(store.asks, 0, sizeof(store.asks));
    memset(CtrSeam_drawn.seen, 0, sizeof(CtrSeam_drawn.seen));
    memset(CtrSeam_drawn.noFit, 0, sizeof(CtrSeam_drawn.noFit));
    CtrSeam_drawn.seenNow = nullptr;
    CtrSeam_drawn.held = 0;
    store.coolPage = nullptr;
    store.coolUntil = 0;
    store.failUntil = 0;
    store.buildFrame = 0;
    store.buildsInFrame = 0;
    store.fillInFrame = 0;
    store.cache = nullptr;
}

void SEAM_TEXT CtrSeam_beginFrame(CtrTextureCache* cache, uint32_t frame) {
    CtrTextures_beginFrame(cache, frame);
    if (store.cache != cache) {
        // Another cache's copies: their owner is gone.
        seamDrain();
        store.cache = cache;
    }
    // Copies changed, or not drawn from for SEAM_IDLE_FRAMES, go once the GPU is done with them.
    bool held = store.bytes != 0;
    for (int32_t i = 0; SEAM_SLOTS > i; i++) {
        SeamCopy* c = &store.slots[i];
        uint32_t age = frame - c->usedFrame;
        if (c->page != nullptr && age >= 2 && (c->stale || age > SEAM_IDLE_FRAMES)) seamFree(c);
    }
    if (store.bytes != 0 && SEAM_VRAM_LOW > CtrMemory_vramFreeNow()) {
        for (int32_t i = 0; SEAM_SLOTS > i; i++) {
            if (store.slots[i].page != nullptr) seamFree(&store.slots[i]);
        }
    }
    if (held && store.bytes == 0) {
        logInfo(seamNoneFmt, (unsigned long) frame, (unsigned long) store.builds, (unsigned long) store.freed, (unsigned long) store.refused,
                (unsigned long) store.failed);
    }
}

void SEAM_TEXT CtrSeam_prefetchRoom(CtrTextureCache* cache, Runner* runner) {
    if (store.cache == cache && store.bytes != 0) {
        seamDrain();
        store.cache = cache;
        logInfo(seamNoneFmt, (unsigned long) cache->frame, (unsigned long) store.builds, (unsigned long) store.freed, (unsigned long) store.refused,
                (unsigned long) store.failed);
    }
    CtrTextures_prefetchRoom(cache, runner);
}

void SEAM_TEXT CtrSeam_destroy(CtrTextureCache* cache) {
    if (store.cache == cache) seamDrain();
    __real_CtrTextures_destroy(cache);
}

void SEAM_TEXT CtrSeam_vramWanted(void) {
    for (int32_t i = 0; SEAM_SLOTS > i; i++) store.slots[i].stale = true;
}

bool SEAM_TEXT CtrSeam_texInit(C3D_Tex* tex, C3D_TexCube* cube, C3D_TexInitParams p) {
    bool made = __real_C3D_TexInitWithParams(tex, cube, p);
    if (!made && p.onVram) CtrSeam_vramWanted();
    return made;
}

C3D_RenderTarget* SEAM_TEXT CtrSeam_renderTarget(int width, int height, GPU_COLORBUF colorFmt, C3D_DEPTHTYPE depthFmt) {
    CtrSeam_vramWanted();
    return __real_C3D_RenderTargetCreate(width, height, colorFmt, depthFmt);
}

uint32_t SEAM_TEXT CtrSeam_peakBytes(void) {
    return store.peakBytes;
}
#endif

// layout (z_layout.c, dsplan R10): every copy freed at once, when the options menu's layout change needs both screen
// targets and they don't fit: the copies are a cache, given up before the change can fail. The caller has waited for
// the GPU. Appended, in layout's late section; nothing above calls it.
#if defined(ENABLE_WAD16) && !defined(CTRPORT_SMART_NO_DIRECT) && !defined(CTRPORT_SEAM_OFF)
__attribute__((noinline, noclone, section(".text_layout"))) uint32_t CtrSeam_dropAll(void) {
    uint32_t bytes = store.bytes;
    for (int32_t i = 0; SEAM_SLOTS > i; i++) {
        if (store.slots[i].page != nullptr) seamFree(&store.slots[i]);
    }
    return bytes - store.bytes;
}
#endif
