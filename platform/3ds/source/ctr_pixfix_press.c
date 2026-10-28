// pixfix, task E: DELTARUNE's attack-bar "PRESS" (spr_pressfront_b) drawn for the 3DS's screen. Late code only
// (platform/3ds/late_reserve.ld): nothing here moves existing code or data, and nothing runs per draw.
//
// obj_attackpress draws spr_pressfront_b 1:1 in the 640x480 battle view: a black panel (sprite columns 40-74, all 38
// rows; Chapters 3 and 4 store only the panel, at target x 40) with PRESS in white, letters 4 texels wide and 9 tall
// with one-texel strokes, 6 texels apart (columns 44-71, rows 6-14). At native drawing each output pixel shows one
// 2x2 block of surface pixels, so every letter is 2 pixels wide and 4.5 tall, and no choice of texels makes an R:
// the even texels read "FFESS", the line pick "PLESS", and Smart's average shows grey blocks with a light band through
// the word (the R reads as a 5x3 array of pixels).
//
// The word is drawn again for that grid instead, as one would draw it for the screen: P and R 3 pixels wide, E and S
// 2, one pixel apart, 16x5 pixels in all (pixpressRows), at native pixels 21-36 and rows 3-7 of the sprite, inside the
// panel. In texels that is columns 42-73 and rows 6-15, each pixel a 2x2 block of white or black: a block of one
// colour shows the same at either screen parity, and plainly drawn or averaged. The rest of the panel is untouched.
//
// The texels are rewritten in the tile itself as it loads, just before the tile's flush (C3D_TexFlush is wrapped:
// CMakeLists.txt, DELTARUNE only). Every way a page tile gets its texels (decoded from the data, read from the SD
// cache or decompressed from RAM, on the game thread or settled from the reader's) ends in that flush, before anything
// can draw or read the tile: Smart's verdicts and copies, the VRAM mirrors and the texture probe all see the new
// word, and an evicted tile is rewritten again when it reloads. The SD cache is written from the decoder's own
// buffers, so it keeps the data's texels. White is all ones and black the alpha bits alone in each tile format, so
// both are exact. A tile is rewritten only where every texel it would change is already black or white.
//
// The sprite is looked up once per chapter, after CtrTextures_init (wrapped) has set up the pages. It is matched by
// name and by its 75x38 size, and each frame's item by its layout (unscaled, the word's rectangle inside it); anything
// else is left alone. The pages are forgotten at the start of each chapter's renderer init (CtrPacer_init, wrapped),
// whose first flush comes before its CtrTextures_init. UNDERTALE builds compile this file empty.

#include <stdint.h>
#include <string.h>

#include "common.h"
#include "ctr_textures.h"
#include "data_win.h"

#ifdef ENABLE_WAD17

#define PIXPRESS_TEXT __attribute__((noinline, section(".text.late.pixpress")))
#define PIXPRESS_CONST __attribute__((aligned(4), section(".rodata.pixpress_in_text")))

#define PIXPRESS_X 42 // the word's first texel column in the sprite
#define PIXPRESS_Y 6  // and row
#define PIXPRESS_MAX 4

static const char pixpressName[20] PIXPRESS_CONST = "spr_pressfront_b";
// P3 R3 E2 S2 S2: one row of native pixels each, bit 15 the leftmost.
static const uint16_t pixpressRows[5] PIXPRESS_CONST = {0xEEDB, 0xAA92, 0xEEDB, 0x8C89, 0x8ADB};
// Bytes per texel by CtrTileFormat (0: no texels to rewrite), and black's first byte (alpha, where the format has
// one; white is all ones). Tables rather than a switch, which gcc would turn into a table in the main .rodata.
static const uint8_t pixpressSize[CTR_TILE_FORMAT_COUNT] PIXPRESS_CONST = {
    [CTR_TILE_RGBA5551] = 2, [CTR_TILE_RGBA8] = 4, [CTR_TILE_EMPTY] = 0, [CTR_TILE_RGB8] = 3, [CTR_TILE_RGB565] = 2};
static const uint8_t pixpressBlack[CTR_TILE_FORMAT_COUNT] PIXPRESS_CONST = {[CTR_TILE_RGBA5551] = 0x01, [CTR_TILE_RGBA8] = 0xFF};
static const char pixpressLog[] PIXPRESS_CONST ="Native: PRESS drawn for the screen (%u items, page %d)\n";

__attribute__((section(".bss.late.pixpress"))) static struct {
    uint32_t count; // 0: nothing to rewrite
    struct {
        CtrPage* page;
        int32_t x, y; // the word's top-left texel on the page
    } at[PIXPRESS_MAX];
} pixpress;

// The renderer's init calls CtrPacer_init (once, ctr_frame_pacer.c) before its white texture's flush, which comes
// before CtrTextures_init: the previous chapter's pages are forgotten there, before that flush can look at them.
void __real_CtrPacer_init(void);
void __wrap_CtrPacer_init(void);
PIXPRESS_TEXT void __wrap_CtrPacer_init(void) {
    pixpress.count = 0;
    __real_CtrPacer_init();
}

void __real_CtrTextures_init(CtrTextureCache* cache, DataWin* dataWin, FileSystem* fileSystem, uint32_t budgetBytes);
void __wrap_CtrTextures_init(CtrTextureCache* cache, DataWin* dataWin, FileSystem* fileSystem, uint32_t budgetBytes);
void Repack_texturesInit(CtrTextureCache* cache, DataWin* dataWin, FileSystem* fileSystem, uint32_t budgetBytes);

PIXPRESS_TEXT void __wrap_CtrTextures_init(CtrTextureCache* cache, DataWin* dataWin, FileSystem* fileSystem, uint32_t budgetBytes) {
    pixpress.count = 0;
#ifndef CTRPORT_TARGET_O3DS
    // repack (ctr_repack.inc in ctr_renderer.c) adds the extra pages there.
    Repack_texturesInit(cache, dataWin, fileSystem, budgetBytes);
#else
    __real_CtrTextures_init(cache, dataWin, fileSystem, budgetBytes);
#endif
    for (uint32_t i = 0; dataWin->sprt.count > i; i++) {
        const Sprite* sp = &dataWin->sprt.sprites[i];
        if (sp->name == nullptr || strcmp(sp->name, pixpressName) != 0 || sp->width != 75 || sp->height != 38) continue;
        for (uint32_t f = 0; sp->textureCount > f && PIXPRESS_MAX > pixpress.count; f++) {
            int32_t t = sp->tpagIndices[f];
            if (0 > t || dataWin->tpag.count <= (uint32_t) t) continue;
            const TexturePageItem* item = &dataWin->tpag.items[t];
            int32_t lx = PIXPRESS_X - item->targetX, ly = PIXPRESS_Y - item->targetY;
            if (0 > item->texturePageId || cache->pageCount <= (uint32_t) item->texturePageId || item->sourceWidth != item->targetWidth ||
                item->sourceHeight != item->targetHeight || 0 > lx || 0 > ly || lx + 32 > item->sourceWidth || ly + 10 > item->sourceHeight) {
                continue;
            }
            // Frames that share the item's texels each get an entry; the second rewrite writes what is there.
            uint32_t k = pixpress.count++;
            pixpress.at[k].page = &cache->pages[item->texturePageId];
            pixpress.at[k].x = item->sourceX + lx;
            pixpress.at[k].y = item->sourceY + ly;
        }
        if (pixpress.count != 0) logInfo(pixpressLog, (unsigned) pixpress.count, (int) (pixpress.at[0].page - cache->pages));
        return;
    }
}

// Rewrites the word's texels that lie in tile, if it is one of the word's tiles; pass 0 checks them, pass 1 writes.
static PIXPRESS_TEXT void pixpressTile(CtrTile* tile) {
    for (uint32_t k = 0; pixpress.count > k; k++) {
        CtrPage* page = pixpress.at[k].page;
        if (!page->layoutKnown || page->tiles > tile || tile >= page->tiles + page->tilesX * page->tilesY) continue;
        if (tile->format >= CTR_TILE_FORMAT_COUNT || pixpressSize[tile->format] == 0) continue;
        uint32_t size = pixpressSize[tile->format], black = pixpressBlack[tile->format];
        int32_t index = (int32_t) (tile - page->tiles);
        int32_t x0 = pixpress.at[k].x - index % page->tilesX * page->tileW, y0 = pixpress.at[k].y - index / page->tilesX * page->tileH;
        uint8_t* data = tile->tex.data;
        for (int32_t pass = 0; 2 > pass; pass++) {
            for (int32_t y = 0; 10 > y; y++) {
                uint32_t ly = (uint32_t) (y0 + y);
                if (ly >= (uint32_t) page->tileH) continue;
                for (int32_t x = 0; 32 > x; x++) {
                    uint32_t lx = (uint32_t) (x0 + x);
                    if (lx >= (uint32_t) page->tileW) continue;
                    size_t i = (size_t) (ly >> 3) * (size_t) (tile->tex.width / 8) * 64 + (size_t) (lx >> 3) * 64 +
                               ((lx & 1) | ((ly & 1) << 1) | ((lx & 2) << 1) | ((ly & 2) << 2) | ((lx & 4) << 2) | ((ly & 4) << 3));
                    uint8_t* p = data + i * size;
                    if (pass == 0) {
                        bool white = true, dark = p[0] == black;
                        for (uint32_t b = 0; size > b; b++) {
                            white = white && p[b] == 0xFF;
                            dark = dark && (b == 0 || p[b] == 0);
                        }
                        if (!white && !dark) goto next; // not the data's PRESS: leave the tile as it is
                    } else if ((pixpressRows[y >> 1] >> (15 - (x >> 1)) & 1) != 0) {
                        memset(p, 0xFF, size);
                    } else {
                        memset(p, 0, size);
                        p[0] = (uint8_t) black;
                    }
                }
            }
        }
    next:;
    }
}

void __real_C3D_TexFlush(C3D_Tex* tex);
void __wrap_C3D_TexFlush(C3D_Tex* tex);

// A page tile is a CtrTile, whose first member is its C3D_Tex; any other texture lies outside the word's pages' tiles.
PIXPRESS_TEXT void __wrap_C3D_TexFlush(C3D_Tex* tex) {
    if (pixpress.count != 0) pixpressTile((CtrTile*) tex);
    __real_C3D_TexFlush(tex);
}

#endif // ENABLE_WAD17
