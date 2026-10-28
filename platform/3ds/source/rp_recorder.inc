// The room plan recorder (z_roomplan.c, CTRPORT_ROOMPLAN_RECORDER builds only: build.sh ... roomplan-rec). Never in a
// release or timing build. Writes roomplan-rec.txt in the game folder, one line per first use in each room visit:
//   V <visit> <room> <battlegroup or -1> <frame>     a room change (the frame the cache began)
//   T <dframe> <tpag>                                a texture page item drawn whole or in part (sprites, fonts)
//   S <dframe> <tpag> <x> <y> <w> <h>                part of an item drawn (room tiles, drawSpritePart), in the
//                                                    item's texels from its first one
//   P <dframe> <page> <tile>                         a tile the texture cache was asked for
//   C <dframe> <code entry>                          a code entry's first execution in this visit, including
//                                                    entries already resident from earlier visits;
//                                                    dframe -1: in the room change, before the first frame
//   A <dframe> <sound>                               a sound's first play in the visit (its SOND index; utsfx,
//                                                    tools/roomplan/sound_records.py); dframe -1 as for C
// dframe counts the cache's frames since the room change. Indices only.
//
// Hooks: CtrTextures_beginFrame (room changes, and an outer vtable put in front of the renderer's for the draws),
// CtrTextures_ensureTiles(But), DataWin_loadCodeBytes and Runner_create, all wrapped (CMakeLists.txt); Runner_create
// also puts an outer table in front of the audio system's for the plays.

#include <3ds.h>
#include <sys/stat.h>

#include "audio_system.h"
#include "instance.h"
#include "int_rvalue_hashmap.h"
#include "renderer.h"
#include "runner.h"
#include "vm.h"

#ifdef ENABLE_WAD16
#define REC_DIR "sdmc:/3ds/undertale-3ds"
#else
#define REC_DIR "sdmc:/3ds/deltarune-3ds"
#endif
#define REC_PATH REC_DIR "/roomplan-rec.txt"

__attribute__((section(".bss.late.roomplan"))) static struct {
    FILE* out;
    Runner* runner;
    DataWin* dataWin;
    DataWin* headerFor; // the data.win the last H line describes
    int32_t room;
    uint32_t visit;
    uint32_t frame;      // the cache's frame
    uint32_t visitFrame; // when the visit began
    uint8_t* seenTpag;   // bitsets, cleared at each visit
    uint8_t* seenTile;
    uint8_t* seenCode;
    uint8_t* seenSound;
    uint32_t tpagBits, tileBits, codeBits, soundBits;
    int32_t* smallSprite; // per page item: the small sprite it is a frame of (recSmallMap), or -1
    uint32_t smallCount;  // its items: the data.win's, as loaded (items added later, for surface sprites, are past it)
    DataWin* smallFor;
    CtrTextureCache* cache;
    Renderer* renderer;
    const RendererVtable* inner;
    RendererVtable outer;
    AudioSystemVtable* audioInner;
    AudioSystemVtable audioOuter;
} rec;

ROOMPLAN_TEXT static bool recFirst(uint8_t* bits, uint32_t count, uint32_t index) {
    if (bits == nullptr || index >= count) return false;
    uint8_t mask = (uint8_t) (1u << (index & 7));
    if (bits[index >> 3] & mask) return false;
    bits[index >> 3] |= mask;
    return true;
}

ROOMPLAN_TEXT static uint32_t recDframe(void) {
    // A visit begun by a code read in the step that changes rooms starts with the next frame (see below).
    return rec.frame >= rec.visitFrame ? rec.frame - rec.visitFrame : 0;
}

ROOMPLAN_TEXT static void recTpag(int32_t tpag) {
    if (rec.out == nullptr || 0 > tpag || !recFirst(rec.seenTpag, rec.tpagBits, (uint32_t) tpag)) return;
    fprintf(rec.out, "T %lu %ld\n", (unsigned long) recDframe(), (long) tpag);
}

// DELTARUNE's HUD draws one frame of a small sprite at a time and picks it by state: a battle button lit when chosen,
// a heart facing the way it moves, a menu button. A record holds the frames its playthrough happened to show, and the
// others then load on the game thread the first time a player sees them. So a sprite of at most REC_SMALL_FRAMES
// frames, each at most REC_SMALL_SIDE texels a side, has every frame noted when any is drawn (room 109 with the
// Knight's scenes: 8 more tiles; every frame of every sprite: 34). Big or long animations are left as recorded.
// UNDERTALE's plans already add every frame of each drawn sprite at build time (build_plan.py whole_set).
#define REC_SMALL_FRAMES 8u
#define REC_SMALL_SIDE 64u

ROOMPLAN_TEXT static void recSmallMap(DataWin* dw) {
#ifndef ENABLE_WAD16
    if (rec.smallFor == dw) return;
    free(rec.smallSprite);
    rec.smallCount = dw->tpag.count;
    rec.smallSprite = malloc(rec.smallCount * sizeof(int32_t) + 1);
    rec.smallFor = rec.smallSprite != nullptr ? dw : nullptr;
    if (rec.smallSprite == nullptr) return;
    for (uint32_t i = 0; rec.smallCount > i; i++) rec.smallSprite[i] = -1;
    for (uint32_t i = 0; dw->sprt.count > i; i++) {
        const Sprite* sp = &dw->sprt.sprites[i];
        if (sp->tpagIndices == nullptr || 2 > sp->textureCount || REC_SMALL_FRAMES < sp->textureCount) continue;
        bool small = true;
        for (uint32_t f = 0; small && sp->textureCount > f; f++) {
            int32_t t = sp->tpagIndices[f];
            small = 0 <= t && rec.smallCount > (uint32_t) t && REC_SMALL_SIDE >= dw->tpag.items[t].sourceWidth &&
                    REC_SMALL_SIDE >= dw->tpag.items[t].sourceHeight;
        }
        for (uint32_t f = 0; small && sp->textureCount > f; f++) {
            if (0 > rec.smallSprite[sp->tpagIndices[f]]) rec.smallSprite[sp->tpagIndices[f]] = (int32_t) i;
        }
    }
#else
    (void) dw;
#endif
}

ROOMPLAN_TEXT static void recFrames(int32_t tpag) {
    recTpag(tpag);
    DataWin* dw = rec.dataWin;
    if (rec.smallSprite == nullptr || rec.smallFor != dw || 0 > tpag || rec.smallCount <= (uint32_t) tpag || 0 > rec.smallSprite[tpag]) return;
    const Sprite* sp = &dw->sprt.sprites[rec.smallSprite[tpag]];
    for (uint32_t f = 0; sp->textureCount > f; f++) recTpag(sp->tpagIndices[f]);
}

#define REC_PARTS 16384
__attribute__((section(".bss.late.roomplan"))) static uint32_t recParts[REC_PARTS];

ROOMPLAN_TEXT static void recPart(int32_t tpag, float fx, float fy, float fw, float fh) {
    if (rec.out == nullptr || 0 > tpag || 0 >= fw || 0 >= fh) return;
    int32_t x = (int32_t) fx, y = (int32_t) fy, w = (int32_t) (fx + fw + 0.999f) - x, h = (int32_t) (fy + fh + 0.999f) - y;
    uint32_t key = 2166136261u;
    const int32_t v[5] = {tpag, x, y, w, h};
    for (int i = 0; 5 > i; i++) key = (key ^ (uint32_t) v[i]) * 16777619u;
    if (key == 0) key = 1;
    for (uint32_t n = 0, at = key % REC_PARTS; REC_PARTS > n; n++, at = (at + 1) % REC_PARTS) {
        if (recParts[at] == key) return;
        if (recParts[at] == 0) {
            recParts[at] = key;
            fprintf(rec.out, "S %lu %ld %ld %ld %ld %ld\n", (unsigned long) recDframe(), (long) tpag, (long) x, (long) y, (long) w, (long) h);
            return;
        }
    }
}

ROOMPLAN_TEXT static void recFont(Renderer* renderer) {
    DataWin* dw = renderer->dataWin;
    int32_t font = renderer->drawFont;
    if (dw == nullptr || 0 > font || (uint32_t) font >= dw->font.count) return;
    const Font* f = &dw->font.fonts[font];
    // A font the game made from a sprite (font_add_sprite(_ext): DELTARUNE's damage and HP numbers) has no page
    // item of its own (tpagIndex -1): its glyphs are the sprite's frames, and any of them can be drawn next (the
    // numbers change), so every frame is noted.
    if (f->isSpriteFont) {
        if (0 > f->spriteIndex || (uint32_t) f->spriteIndex >= dw->sprt.count) return;
        const Sprite* s = &dw->sprt.sprites[f->spriteIndex];
        for (uint32_t i = 0; s->tpagIndices != nullptr && s->textureCount > i; i++) recTpag(s->tpagIndices[i]);
        return;
    }
    recTpag(f->tpagIndex);
}

ROOMPLAN_TEXT static void recDrawSprite(Renderer* r, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale,
                                        float angleDeg, uint32_t color, float alpha) {
    recFrames(tpagIndex);
    rec.inner->drawSprite(r, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
}

ROOMPLAN_TEXT static void recDrawSpritePart(Renderer* r, int32_t tpagIndex, float srcOffX, float srcOffY, float srcW, float srcH, float x, float y,
                                            float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha) {
    recPart(tpagIndex, srcOffX, srcOffY, srcW, srcH);
    rec.inner->drawSpritePart(r, tpagIndex, srcOffX, srcOffY, srcW, srcH, x, y, xscale, yscale, angleDeg, pivotX, pivotY, color, alpha);
}

ROOMPLAN_TEXT static void recDrawSpritePartColor(Renderer* r, int32_t tpagIndex, float srcOffX, float srcOffY, float srcW, float srcH, float x,
                                                 float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color1,
                                                 uint32_t color2, uint32_t color3, uint32_t color4, float alpha) {
    recPart(tpagIndex, srcOffX, srcOffY, srcW, srcH);
    rec.inner->drawSpritePartColor(r, tpagIndex, srcOffX, srcOffY, srcW, srcH, x, y, xscale, yscale, angleDeg, pivotX, pivotY, color1, color2,
                                   color3, color4, alpha);
}

ROOMPLAN_TEXT static void recDrawSpritePos(Renderer* r, int32_t tpagIndex, float x1, float y1, float x2, float y2, float x3, float y3, float x4,
                                           float y4, float alpha) {
    recFrames(tpagIndex);
    rec.inner->drawSpritePos(r, tpagIndex, x1, y1, x2, y2, x3, y3, x4, y4, alpha);
}

ROOMPLAN_TEXT static void recDrawSpriteTiled(Renderer* r, int32_t tpagIndex, float originX, float originY, float x, float y, float xscale,
                                             float yscale, bool tileX, bool tileY, float roomW, float roomH, uint32_t color, float alpha) {
    recTpag(tpagIndex);
    rec.inner->drawSpriteTiled(r, tpagIndex, originX, originY, x, y, xscale, yscale, tileX, tileY, roomW, roomH, color, alpha);
}

ROOMPLAN_TEXT static void recDrawTiledPart(Renderer* r, int32_t tpagIndex, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH, float dstX,
                                           float dstY, float dstW, float dstH, uint32_t color, float alpha) {
    recTpag(tpagIndex);
    rec.inner->drawTiledPart(r, tpagIndex, srcX, srcY, srcW, srcH, dstX, dstY, dstW, dstH, color, alpha);
}

ROOMPLAN_TEXT static void recDrawText(Renderer* r, const char* text, float x, float y, float xscale, float yscale, float angleDeg,
                                      float lineSeparation) {
    recFont(r);
    rec.inner->drawText(r, text, x, y, xscale, yscale, angleDeg, lineSeparation);
}

ROOMPLAN_TEXT static void recDrawTextColor(Renderer* r, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t c1,
                                           int32_t c2, int32_t c3, int32_t c4, float alpha, float lineSeparation) {
    recFont(r);
    rec.inner->drawTextColor(r, text, x, y, xscale, yscale, angleDeg, c1, c2, c3, c4, alpha, lineSeparation);
}

ROOMPLAN_TEXT static void recDrawTextUI(Renderer* r, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t c1,
                                        int32_t c2, int32_t c3, int32_t c4, float alpha, float lineSeparation) {
    recFont(r);
    rec.inner->drawTextUI(r, text, x, y, xscale, yscale, angleDeg, c1, c2, c3, c4, alpha, lineSeparation);
}

// Install once per renderer lifetime. Later wrappers can save our callbacks: wrapping their table again would
// make rec.inner point back through those saved callbacks and loop (the tower decoration queue installs at draw).
ROOMPLAN_TEXT static void recInstall(Renderer* renderer) {
    // Once per renderer (Runner_create's wrap, before the first room change). Crisp text (z_smalltext.c) installs its
    // table at a room change over whatever is on top and calls through it, and again whenever its table isn't on top:
    // wrapping it, or letting it wrap us twice, made each table call the other forever (a hang in the first draw).
    if (renderer == nullptr || renderer == rec.renderer || renderer->vtable == &rec.outer) return;
    rec.renderer = renderer;
    rec.inner = renderer->vtable;
    rec.outer = *rec.inner;
    // Only entries the renderer has: the engine falls back to others where one is missing (drawTile).
#define REC_HOOK(name, fn) if (rec.inner->name != nullptr) rec.outer.name = fn
    REC_HOOK(drawSprite, recDrawSprite);
    REC_HOOK(drawSpritePart, recDrawSpritePart);
    REC_HOOK(drawSpritePartColor, recDrawSpritePartColor);
    REC_HOOK(drawSpritePos, recDrawSpritePos);
    REC_HOOK(drawSpriteTiled, recDrawSpriteTiled);
    REC_HOOK(drawTiledPart, recDrawTiledPart);
    REC_HOOK(drawText, recDrawText);
    REC_HOOK(drawTextColor, recDrawTextColor);
    REC_HOOK(drawTextUI, recDrawTextUI);
#undef REC_HOOK
    renderer->vtable = &rec.outer;
}


ROOMPLAN_TEXT static void recAlloc(uint8_t** bits, uint32_t* count, uint32_t want) {
    if (*bits != nullptr && *count == want) return;
    free(*bits);
    *bits = calloc((want + 7) / 8 + 1, 1);
    *count = *bits != nullptr ? want : 0;
}

ROOMPLAN_TEXT static void recVisit(CtrTextureCache* cache, Runner* runner) {
    DataWin* dw = runner->dataWin;
    if (rec.out == nullptr) {
        rec.out = fopen(REC_PATH, "w");
        if (rec.out == nullptr) return;
    }
    // One H line per data.win (DELTARUNE: at each chapter's first visit); the visits after it are that data.win's.
    if (rec.headerFor != dw) {
        rec.headerFor = dw;
        fprintf(rec.out, "H roomplan-rec 1 rooms %lu tpag %lu pages %lu codes %lu\n", (unsigned long) dw->room.count,
                (unsigned long) dw->tpag.count, (unsigned long) cache->pageCount, (unsigned long) dw->code.count);
    }
    rec.dataWin = dw;
    recSmallMap(dw);
    recAlloc(&rec.seenTpag, &rec.tpagBits, dw->tpag.count);
    recAlloc(&rec.seenTile, &rec.tileBits, cache->pageCount * CTR_PAGE_MAX_TILES);
    recAlloc(&rec.seenCode, &rec.codeBits, dw->code.count);
    recAlloc(&rec.seenSound, &rec.soundBits, dw->sond.count);
    memset(rec.seenTpag, 0, (rec.tpagBits + 7) / 8);
    memset(rec.seenTile, 0, (rec.tileBits + 7) / 8);
    memset(rec.seenCode, 0, (rec.codeBits + 7) / 8);
    if (rec.seenSound != nullptr) memset(rec.seenSound, 0, (rec.soundBits + 7) / 8);
    memset(recParts, 0, sizeof(recParts));
    rec.room = runner->currentRoomIndex;
    rec.visit++;
    rec.visitFrame = rec.frame;
    fprintf(rec.out, "V %lu %ld %ld %lu\n", (unsigned long) rec.visit, (long) rec.room, (long) battleGroup(runner), (unsigned long) rec.frame);
    fflush(rec.out);
}

void __real_CtrTextures_beginFrame(CtrTextureCache* cache, uint32_t frame);
void __wrap_CtrTextures_beginFrame(CtrTextureCache* cache, uint32_t frame);

ROOMPLAN_TEXT void __wrap_CtrTextures_beginFrame(CtrTextureCache* cache, uint32_t frame) {
    __real_CtrTextures_beginFrame(cache, frame);
    rec.frame = frame;
    Runner* runner = cache->runner;
    if (runner == nullptr) return;
    rec.cache = cache;
    recInstall(runner->renderer);
    if (runner != rec.runner || runner->currentRoomIndex != rec.room) {
        rec.runner = runner;
        recVisit(cache, runner);
    } else if (rec.out != nullptr && (frame & 31) == 0) {
        fflush(rec.out);
    }
}

ROOMPLAN_TEXT static void recTiles(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1) {
    if (rec.out == nullptr || page == nullptr || !page->layoutKnown) return;
    uint32_t p = (uint32_t) (page - cache->pages);
    for (int32_t ty = ty0; ty1 >= ty; ty++) {
        for (int32_t tx = tx0; tx1 >= tx; tx++) {
            if (0 > tx || 0 > ty || tx >= page->tilesX || ty >= page->tilesY) continue;
            uint32_t t = (uint32_t) (ty * page->tilesX + tx);
            if (recFirst(rec.seenTile, rec.tileBits, p * CTR_PAGE_MAX_TILES + t)) {
                fprintf(rec.out, "P %lu %lu %lu\n", (unsigned long) recDframe(), (unsigned long) p, (unsigned long) t);
            }
        }
    }
}

void __real_CtrTextures_ensureTiles(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1);
void __wrap_CtrTextures_ensureTiles(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1);
ROOMPLAN_TEXT void __wrap_CtrTextures_ensureTiles(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1) {
    __real_CtrTextures_ensureTiles(cache, page, tx0, ty0, tx1, ty1);
    recTiles(cache, page, tx0, ty0, tx1, ty1);
}

void __real_CtrTextures_ensureTilesBut(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1, uint64_t drawnElsewise);
void __wrap_CtrTextures_ensureTilesBut(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1, uint64_t drawnElsewise);
ROOMPLAN_TEXT void __wrap_CtrTextures_ensureTilesBut(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1,
                                                     uint64_t drawnElsewise) {
    __real_CtrTextures_ensureTilesBut(cache, page, tx0, ty0, tx1, ty1, drawnElsewise);
    recTiles(cache, page, tx0, ty0, tx1, ty1);
}

// Called immediately before dispatching each event or script, regardless of AOT or bytecode residency.
void RoomPlan_recordCode(DataWin* dw, int32_t codeIndex);
__attribute__((noinline, section(".text.late.planrec"))) void RoomPlan_recordCode(DataWin* dw, int32_t codeIndex) {
    if (rec.out != nullptr && dw == rec.dataWin && codeIndex >= 0 && (uint32_t) codeIndex < dw->code.count) {
        // The room's Create events run in the step that changes rooms, before the next frame begins: their code
        // belongs to the new room's visit.
        if (rec.runner != nullptr && rec.cache != nullptr && rec.runner->dataWin == dw && rec.runner->currentRoomIndex != rec.room) {
            recVisit(rec.cache, rec.runner);
            // The cache's frame is still the last room's: the new room's first frame is the next one, as when
            // CtrTextures_beginFrame begins the visit.
            rec.visitFrame = rec.frame + 1;
        }
        uint32_t index = (uint32_t) codeIndex;
        if (recFirst(rec.seenCode, rec.codeBits, index)) {
            // -1: in the room change itself (the new room's Create events), before its first frame.
            long df = rec.frame >= rec.visitFrame ? (long) (rec.frame - rec.visitFrame) : -1L;
            fprintf(rec.out, "C %ld %lu\n", df, (unsigned long) index);
        }
    }
}

// utsfx: each visit's sounds, for the sound plan (z_utsfx.c). Every play goes through the audio system's playSound
// (audio_play_sound and the like); the first of each SOND index in a visit is written. A play in the step that changes
// rooms (the new room's Create events) begins the new visit, as a code read there does.
ROOMPLAN_TEXT static int32_t recPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    if (rec.out != nullptr && rec.runner != nullptr && rec.cache != nullptr && rec.runner->dataWin == rec.dataWin && soundIndex >= 0) {
        if (rec.runner->currentRoomIndex != rec.room) {
            recVisit(rec.cache, rec.runner);
            rec.visitFrame = rec.frame + 1;
        }
        if (recFirst(rec.seenSound, rec.soundBits, (uint32_t) soundIndex)) {
            long df = rec.frame >= rec.visitFrame ? (long) (rec.frame - rec.visitFrame) : -1L;
            fprintf(rec.out, "A %ld %ld\n", df, (long) soundIndex);
        }
    }
    return rec.audioInner->playSound(audio, soundIndex, priority, loop);
}

ROOMPLAN_TEXT static void recInstallAudio(AudioSystem* audio) {
    if (audio == nullptr || audio->vtable == nullptr || audio->vtable == &rec.audioOuter) return;
    rec.audioInner = audio->vtable;
    rec.audioOuter = *audio->vtable;
    rec.audioOuter.playSound = recPlaySound;
    audio->vtable = &rec.audioOuter;
}

// The New models keep the bytecode in memory, so nothing is read back after boot. The recorder moves it to its file as
// the Old models do (main.c, moveBytecodeToCache), before the runner exists, so each code entry's first run reads it
// back through DataWin_loadCodeBytes, where the Old models page it. Execution recording is independent of reads.
Runner* __real_Runner_create(DataWin* dataWin, VMContext* vm, Renderer* renderer, FileSystem* fileSystem, AudioSystem* audioSystem,
                             uint32_t randomSeed);
Runner* __wrap_Runner_create(DataWin* dataWin, VMContext* vm, Renderer* renderer, FileSystem* fileSystem, AudioSystem* audioSystem,
                             uint32_t randomSeed);
ROOMPLAN_TEXT Runner* __wrap_Runner_create(DataWin* dataWin, VMContext* vm, Renderer* renderer, FileSystem* fileSystem,
                                           AudioSystem* audioSystem, uint32_t randomSeed) {
    rec.runner = nullptr;
    rec.renderer = nullptr; // main creates a new renderer per runner; its allocation can reuse the old address
    // Our table goes in before the first room change, so that Crisp text's (z_smalltext.c, installed there) lies on top.
    recInstall(renderer);
    rec.cache = nullptr;
    rec.headerFor = nullptr; // a new data.win can reuse the last one's address
    rec.smallFor = nullptr;
    if (dataWin->bytecodeFile == nullptr && dataWin->bytecodeBuffer != nullptr) {
        mkdir(REC_DIR "/cache", 0777);
#ifdef ENABLE_WAD16
        bool moved = DataWin_moveBytecodeToFile(dataWin, REC_DIR "/cache/bytecode.bin");
#else
        char path[96];
        snprintf(path, sizeof(path), REC_DIR "/cache/roomplan-%s.bin", rp.part);
        bool moved = DataWin_moveBytecodeToFile(dataWin, path);
#endif
        logInfo("Room plan recorder: bytecode %s the file, as on the Old models\n", moved ? "moved to" : "could not be moved to");
    }
    recInstallAudio(audioSystem);
    return __real_Runner_create(dataWin, vm, renderer, fileSystem, audioSystem, randomSeed);
}
