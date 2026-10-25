// UNDERTALE's KR label kept sharp under Smart smoothing (smoothux; UNDERTALE only). Late code only
// (platform/3ds/late_reserve.ld): nothing here moves existing code or data.
//
// In Sans's fight the HUD shows "KR" beside the HP bar: the sprite spr_krmeter (23x10 texels, two colours, strokes 3
// texels wide with gaps of 2), drawn 1:1 in the 640x480 battle view. Smart smoothing's item test (ctr_renderer.c's
// itemVerdict) finds no phase at which the 2:1 pick keeps all its ink, so it calls the item fine and averages it: a grey
// "KR" beside the HUD's sharp letters and numbers (reported as blurry; the rule: the HUD looks
// sharp in every mode). Smoothing off draws it sharp. The HUD's font (fnt_curs) is kept sharp by the font table
// (tools/aot/fontverdict.py's thick-font rule); KR is a sprite, so it needs this.
//
// What it does: the renderer keeps one verdict per data item (smart.itemState, from Krsharp_verdicts at ctr_renderer.c's
// end), and a verdict of ITEM_KNOWN alone is what itemVerdict gives the items it keeps sharp (smartKeepSharp:
// "coarse", drawn as smoothing off draws them). Once per frame, before the frame's draws, every frame of spr_krmeter
// gets that verdict if it does not have it yet, so itemVerdict never looks at it (smartSpriteFine reads the verdict
// first). Smoothing off never reads the fine bit and the item has no phase either way, so Off draws exactly as before;
// under Smart only the KR label changes. The list in itemVerdict (smartKeepSharpSprites) is left alone: changing it
// would move itemVerdict's code and UNDERTALE's main constants.
//
// The hook: main.c calls CtrRenderer_setFrameSkipKeepApp once per frame, before the game's step and draw, and nothing
// else calls or wraps it (CMakeLists.txt wraps it for UNDERTALE). The sprite's frames are looked up once per data.win.
// The verdict table is made here if no draw has made it yet (the renderer makes it, zeroed, at the first item it looks
// at), so the first frame that draws these items has their verdicts too (before, the first 2:1
// battle frame made the table in its Draw and drew Bones and Numbers smoothed for that one frame). A zeroed table read
// by no draw changes nothing; it costs one byte per data item from the first frame.
// The cost: a pointer compare and up to KRSHARP_MAX byte reads a frame. DELTARUNE builds compile the file empty.
//
// The Smoothing menu's Bones and Numbers parts (z_smalltext.c) work the same way: the items z_smalltext.c
// lists for a part set to Crisp get the same verdict each frame; a part set back to Smooth gives its items verdict 0
// once (not looked at: itemVerdict looks again at the next draw, as for an item never drawn).
//
// The other way round, Mettaton EX's "HAPPY BREAKTIME" card, which was hard to read when drawn
// unsmoothed with Smart smoothing on. Mettaton's turn 12 ("Time for our union-regulated break!",
// obj_mettattackgen's attack 41) puts obj_happybreaktime in the box: spr_happybreaktime by the default draw, 1:1 in the
// 640x480 battle view at image_alpha 0.75. It is 120x40 texels, opaque black and white: "HAPPY" in strokes two texels
// wide, "BREAKTIME" in strokes one texel wide. itemVerdict keeps it sharp twice over (its black backdrop makes the
// 2x2-block count coarse, and it is over SMART_ITEM_MAX), so the 2:1 pick drops every other row and column of
// BREAKTIME. The rulings (10-24: sprite text with no remake is Smooth) want it averaged: each frame, before the frame's
// draws, every frame of the sprite gets the verdict itemVerdict gives a fine item whose texels are all opaque
// (ITEM_KNOWN | ITEM_FINE | ITEM_CLEAN) with its ink box the whole item, so smartSpriteFine averages it as it does any
// fine item. No texel is read: the sprite's frames are opaque, checked on UNDERTALE 1.08's data (3,616 black and 1,184
// white texels in frame 0, all black in frame 1). The verdict is set from the first frame on, so the card's first
// frame (it is made in a step, after this hook) is averaged too. Smoothing off reads neither bit: Off is unchanged.

#include <stdint.h>
#include <string.h>

#include "common.h"
#include "data_win.h"
#include "log.h"
#include "renderer.h"

#ifndef ENABLE_WAD17

#define KRSHARP_TEXT __attribute__((noinline, section(".text.late.krsharp")))
#define KRSHARP_RO __attribute__((aligned(4), section(".rodata.krsharp_in_text")))
#define KRSHARP_MAX 4
#define KRSHARP_KNOWN 0x80u // ctr_renderer.c's ITEM_KNOWN: looked at, not fine, no phase

uint8_t* Krsharp_verdicts(Renderer* renderer); // ctr_renderer.c's end: smart.itemState, made if not yet
uint8_t* Krsharp_ink(Renderer* renderer, int32_t tpagIndex); // ctr_renderer.c's end: the item's smart.itemInk box
#define KRSHARP_SMOOTH 0xE0u // ITEM_KNOWN | ITEM_FINE | ITEM_CLEAN

// z_smalltext.c's sharp groups (its CtrSmoothSharp).
typedef struct {
    const DataWin* dataWin;
    uint32_t count;
    uint8_t on, applied;
    uint8_t group[96];
    int32_t item[96];
} KrsharpGroups;
extern KrsharpGroups CtrSmooth_sharp;

static const char krsharpName[12] KRSHARP_RO = "spr_krmeter";
static const char krsharpSmoothName[20] KRSHARP_RO = "spr_happybreaktime";
static const char krsharpLog[] KRSHARP_RO = "Smart smoothing: KR label kept sharp (%u items)\n";

static struct {
    const DataWin* dataWin; // the data the items were looked up in
    uint32_t count;
    int32_t items[KRSHARP_MAX];
    bool logged;
    uint32_t smoothCount;
    int32_t smooth[KRSHARP_MAX]; // spr_happybreaktime's frames
} krsharp __attribute__((section(".bss.late.krsharp")));

static KRSHARP_TEXT void krsharpFind(const DataWin* dataWin) {
    krsharp.dataWin = dataWin;
    krsharp.count = 0;
    krsharp.smoothCount = 0;
    krsharp.logged = false;
    for (uint32_t s = 0; dataWin->sprt.count > s; s++) {
        const Sprite* sp = &dataWin->sprt.sprites[s];
        if (sp->name == nullptr) continue;
        bool sharp = strcmp(sp->name, krsharpName) == 0;
        if (!sharp && strcmp(sp->name, krsharpSmoothName) != 0) continue;
        uint32_t* n = sharp ? &krsharp.count : &krsharp.smoothCount;
        int32_t* list = sharp ? krsharp.items : krsharp.smooth;
        for (uint32_t f = 0; sp->textureCount > f && KRSHARP_MAX > *n; f++) {
            int32_t t = sp->tpagIndices[f];
            if (0 <= t && dataWin->tpag.count > (uint32_t) t) list[(*n)++] = t;
        }
    }
}

void __real_CtrRenderer_setFrameSkipKeepApp(Renderer* renderer, bool on);
void __wrap_CtrRenderer_setFrameSkipKeepApp(Renderer* renderer, bool on);

KRSHARP_TEXT void __wrap_CtrRenderer_setFrameSkipKeepApp(Renderer* renderer, bool on) {
    __real_CtrRenderer_setFrameSkipKeepApp(renderer, on);
    const DataWin* dataWin = renderer->dataWin;
    if (dataWin == nullptr) return;
    if (krsharp.dataWin != dataWin) krsharpFind(dataWin);
    KrsharpGroups* g = &CtrSmooth_sharp;
    bool groups = g->dataWin == dataWin && (g->on | g->applied) != 0;
    if ((krsharp.count | krsharp.smoothCount) == 0 && !groups) return;
    uint8_t* verdicts = Krsharp_verdicts(renderer);
    if (verdicts == nullptr) return; // out of memory: as before the table is made
    for (uint32_t i = 0; krsharp.count > i; i++) {
        uint8_t* v = &verdicts[krsharp.items[i]];
        if (*v == KRSHARP_KNOWN) continue;
        *v = KRSHARP_KNOWN;
        if (!krsharp.logged) {
            krsharp.logged = true;
            logInfo(krsharpLog, (unsigned) krsharp.count);
        }
    }
    for (uint32_t i = 0; krsharp.smoothCount > i; i++) {
        int32_t t = krsharp.smooth[i];
        if (verdicts[t] == KRSHARP_SMOOTH) continue;
        uint8_t* ink = Krsharp_ink(renderer, t);
        if (ink == nullptr) break; // out of memory: itemVerdict's own verdict stands
        const TexturePageItem* item = &dataWin->tpag.items[t];
        ink[0] = ink[1] = 0;
        ink[2] = (uint8_t) (item->sourceWidth > 255 ? 255 : item->sourceWidth);
        ink[3] = (uint8_t) (item->sourceHeight > 255 ? 255 : item->sourceHeight);
        verdicts[t] = KRSHARP_SMOOTH;
    }
    if (!groups) return;
    uint32_t off = (uint32_t) (g->applied & ~g->on);
    for (uint32_t i = 0; g->count > i; i++) {
        uint32_t bit = 1u << g->group[i];
        uint8_t* v = &verdicts[g->item[i]];
        if ((g->on & bit) != 0) *v = KRSHARP_KNOWN;
        else if ((off & bit) != 0) *v = 0;
    }
    g->applied = g->on;
}

#endif // ENABLE_WAD17
