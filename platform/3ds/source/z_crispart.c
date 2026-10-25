// Approved Crisp art (crispart). Build-time coordinates/derivation only; no game pixels in source.
// The pack is read once per chapter with crisp_art=1, before its first room. The texture is made at the first
// matching native draw. The texture is private: it never reserves game sprite, TPAG or surface indices.
// Only the build tool's approved records are emitted; this loader has no fixed frame-count limit in the format.
// Key off: no pack allocation and no renderer hook. All new text/constants/state are in this file's late sections.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ctr_crispart.h"

#if defined(ENABLE_WAD17) || defined(CRISPART_HOST_TEST)
#define CA_TEXT __attribute__((noinline, section(".text.late.crispart")))
#define CA_CONST __attribute__((aligned(4), section(".rodata.late.crispart")))
#define CA_BSS __attribute__((section(".bss.late.crispart")))
static const char caHex[] CA_CONST = "0123456789abcdef";

// Validate before exposing any records or allocating GPU storage. In particular count/offset arithmetic cannot
// wrap, records cannot alias payloads or one another, and texture bounds cannot turn a malformed file into writes.
CA_TEXT bool Crispart_check(const uint8_t* data, uint32_t size, const char* digest, const char* part,
                           uint32_t dataBytes, uint32_t gameId, uint64_t timestamp) {
    if (data == NULL || size < sizeof(CaHeader) || size > CA_MAX_SIZE || digest == NULL || strlen(digest) != 64 || part == NULL) return false;
    const CaHeader* h = (const CaHeader*) data;
    if (sizeof(CaHeader) != 96 || sizeof(CaFrame) != 72 || h->magic != CA_MAGIC || h->version != CA_VERSION ||
        h->headerBytes != sizeof(CaHeader) || h->fileBytes != size || h->count == 0 ||
        h->count > (size - sizeof(CaHeader)) / sizeof(CaFrame) || h->dataBytes != dataBytes ||
        h->gameId != gameId || h->timestamp != timestamp || memchr(h->part, 0, sizeof(h->part)) == NULL || strcmp(h->part, part) != 0) return false;
    for (uint32_t i = 0; 32 > i; i++) {
        uint32_t v = h->digest[i];
        if (digest[2 * i] != caHex[v >> 4] || digest[2 * i + 1] != caHex[v & 15u]) return false;
    }
    const CaFrame* frames = (const CaFrame*) (data + sizeof(CaHeader));
    uint32_t at = sizeof(CaHeader) + h->count * sizeof(CaFrame);
    for (uint32_t i = 0; h->count > i; i++) {
        const CaFrame* f = &frames[i];
        if (memchr(f->name, 0, sizeof(f->name)) == NULL || f->name[0] == 0 || f->reserved != 0 || f->flags > CA_COMMAND ||
            f->sourceW == 0 || f->sourceH == 0 || f->w == 0 || f->h == 0 || f->w > 126 || f->h > 126 ||
            f->x == 0 || f->y == 0 || (uint32_t) f->x + f->w >= CA_ATLAS_WIDTH || (uint32_t) f->y + f->h >= 1024 ||
            f->offset != at || f->bytes != (uint32_t) f->w * f->h * 4u || f->bytes > size - at ||
            (i > 0 && frames[i - 1].tpag >= f->tpag) ||
            (f->flags == CA_COMMAND && (f->sourceW != 31 || f->sourceH != 32))) return false;
        // Gutters and pixels of distinct frames may not overlap in the shared atlas.
        for (uint32_t j = 0; i > j; j++) {
            const CaFrame* p = &frames[j];
            if (f->x < p->x + p->w + 1u && p->x < f->x + f->w + 1u &&
                f->y < p->y + p->h + 1u && p->y < f->y + f->h + 1u) return false;
        }
        at += f->bytes;
    }
    return at == size;
}

CA_TEXT void Crispart_writeAtlas(uint32_t* texels, const uint8_t* data) {
    const CaHeader* h = (const CaHeader*) data;
    const CaFrame* frames = (const CaFrame*) (data + sizeof(CaHeader));
    for (uint32_t i = 0; h->count > i; i++) {
        const CaFrame* f = &frames[i];
        for (uint32_t y = 0; f->h > y; y++) {
            for (uint32_t x = 0; f->w > x; x++) {
                uint32_t tx = f->x + x, ty = f->y + y;
                uint32_t mx = tx & 7u, my = ty & 7u;
                uint32_t morton = (mx & 1u) | ((my & 1u) << 1) | ((mx & 2u) << 1) | ((my & 2u) << 2) |
                                  ((mx & 4u) << 2) | ((my & 4u) << 3);
                uint32_t at = (ty >> 3) * (CA_ATLAS_WIDTH / 8u) * 64u + (tx >> 3) * 64u + morton;
                const uint8_t* p = data + f->offset + (y * f->w + x) * 4u;
                texels[at] = ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
            }
        }
    }
}

#ifndef CRISPART_HOST_TEST
#include <3ds.h>
#include <citro3d.h>
#include "common.h"
#include "ctr_benchromfs.h"
#include "ctr_display.h"
#include "ctr_renderer.h"
#include "ctr_texture_pack.h"
#include "ctr_wrap_chain.h"
#include "data_win.h"
#include "log.h"
#include "renderer.h"
#include "runner.h"

extern const void* const Cmdlabel_refs[1];
extern const int32_t* const Crispart_cmdlabelMode;
bool Crispart_native(Renderer* renderer);
bool Crispart_isCrisp(Renderer* renderer);
uint32_t Crispart_smart(Renderer* renderer, uint32_t state);
void Crispart_drawTexture(Renderer* renderer, C3D_Tex* tex, const CaFrame* frame,
                         float x, float y, uint32_t color, float alpha);

static const char caIni[] CA_CONST = CTRPORT_GAME_DIR "/settings.ini";
static const char caKey[] CA_CONST = " crisp_art = %d";
static const char caSave[] CA_CONST = "crisp_art = %d\n";
static const char caRead[] CA_CONST = "r";
static const char caAppend[] CA_CONST = "a";
static const char caReadBin[] CA_CONST = "rb";
static const char caRomfs[] CA_CONST = "romfs";
static const char caPath[] CA_CONST = "romfs:/art/%s.dtca";
static const char caLoadLog[] CA_CONST = "Crisp art: %s: %u approved frames; pack %u B CPU\n";
static const char caSkipLog[] CA_CONST = "Crisp art: %s unavailable, stale or invalid; base art retained\n";
static const char caAtlasLog[] CA_CONST = "Crisp art: atlas %ux%u, private; retained texture %u B GPU, staging 0 B CPU\n";
static const char caAtlasFail[] CA_CONST = "Crisp art: atlas allocation failed; base art retained\n";
static const char caHeads[4][16] CA_CONST = {"spr_headkris", "spr_headsusie", "spr_headralsei", "spr_headnoelle"};

typedef void (*CaSpriteDraw)(Renderer*, int32_t, float, float, float, float, float, float, float, uint32_t, float);
typedef void (*CaPartDraw)(Renderer*, int32_t, float, float, float, float, float, float, float, float, float, float, float, uint32_t, float);
typedef void (*CaHook)(void);
typedef struct {
    CaSpriteDraw spriteDraw;
    CaPartDraw partDraw;
    void (*destroy)(Renderer* renderer);
    __typeof__(((RendererVtable*) 0)->drawText) text;
    __typeof__(((RendererVtable*) 0)->drawTextColor) textColor, textUI;
    __typeof__(((RendererVtable*) 0)->drawSpritePartColor) partColor;
    __typeof__(((RendererVtable*) 0)->drawSpritePos) spritePos;
} CaHooks;
_Static_assert(sizeof(CaHooks) == 8 * sizeof(CaHook), "Crisp hooks must be contiguous pointers");
static struct {
    int8_t given; // --ini value + 1, 0 = absent
    bool known;
    char digest[65], part[32];
    uint8_t* file;
    uint32_t bytes, count, lo, span, height;
    const CaFrame* frames;
    Renderer* owner;
    int32_t atlas; // 0 not made, -1 failed, 1 ready
    bool inCommand;
    int8_t setting; // cached file value + 1; absent/default becomes 1, read only once per boot
    C3D_Tex texture;
    int32_t faces[16]; // the four Smart-kept frames (0,5,8,9) of each hero, resolved once per chapter
    uint32_t faceCount;
} ca CA_BSS;
// Shared ARM wrapper dispatch indexes these five fields directly. Keep their ABI explicit.
_Static_assert(sizeof(CaHook) == 4 && __builtin_offsetof(CaHooks, text) == 12 &&
    __builtin_offsetof(CaHooks, textColor) == 16 && __builtin_offsetof(CaHooks, textUI) == 20 &&
    __builtin_offsetof(CaHooks, partColor) == 24 && __builtin_offsetof(CaHooks, spritePos) == 28,
    "Crisp ARM callback offsets changed");
CaHooks Crispart_savedHooks CA_BSS;


bool __real_CtrArgui_override(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps);
void __real_CtrArgui_saveEdits(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps);
bool CtrArgui_chainOverrideCrispart(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps);
void CtrArgui_chainSaveEditsCrispart(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps);
void __real_CtrRenderer_loadFontVerdicts(Renderer* renderer);
void __wrap_CtrRenderer_loadFontVerdicts(Renderer* renderer);

static CA_TEXT int8_t caParse(const char* line) {
    int v;
    return sscanf(line, caKey, &v) == 1 && (uint32_t) v <= 1u ? (int8_t) (v + 1) : 0;
}

static CA_TEXT int8_t caReadKey(const char* path) {
    int8_t v = 0;
    FILE* f = fopen(path, caRead);
    if (f != nullptr) {
        char line[128];
        while (fgets(line, sizeof(line), f) != nullptr) {
            int8_t p = caParse(line);
            if (p != 0) v = p;
        }
        fclose(f);
    }
    return v;
}

CA_TEXT bool CtrArgui_chainOverrideCrispart(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps) {
    int8_t v = caParse(line);
    if (v != 0) {
        ca.given = v;
        return true;
    }
    return __real_CtrArgui_override(settings, line, caps);
}

CA_TEXT void CtrArgui_chainSaveEditsCrispart(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps) {
    int8_t v = caReadKey(path);
    ca.setting = v != 0 ? v : 1; // options-save boundary may refresh the cached file setting
    __real_CtrArgui_saveEdits(before, after, path, caps);
    if (v == 0) return; // preserve the file's setting, never persist a temporary --ini override
    FILE* f = fopen(path, caAppend);
    if (f != nullptr) {
        fprintf(f, caSave, (int) v - 1);
        fclose(f);
    }
}

static CA_TEXT void caRelease(void) {
    free(ca.file);
    ca.file = nullptr;
    ca.count = ca.bytes = 0;
}

// roomplan -> crispart -> real; reuse the boot check's verified SHA, never hash data.win in the draw loop.
CA_TEXT CtrDataVersionResult CTR_DATAVERSION_CRISPART(CTR_DATAVERSION_CHAIN_PARAMS) {
    CtrDataVersionResult result = CTR_DATAVERSION_AFTER_CRISPART(path, game, part, cachePath, progress, cached, digest);
    caRelease();
    ca.owner = nullptr;
    ca.known = result == CTR_DATA_KNOWN && digest != nullptr && part != nullptr && strlen(part) < sizeof(ca.part);
    ca.digest[0] = ca.part[0] = '\0';
    if (ca.known) {
        memcpy(ca.digest, digest, 65);
        memcpy(ca.part, part, strlen(part) + 1);
    }
    return result;
}

static CA_TEXT const Sprite* caSprite(const DataWin* dw, const char* name) {
    for (uint32_t s = 0; dw->sprt.count > s; s++) {
        const Sprite* sp = &dw->sprt.sprites[s];
        if (sp->name != nullptr && strcmp(sp->name, name) == 0) return sp;
    }
    return nullptr;
}

static CA_TEXT bool caLoad(Renderer* renderer) {
    caRelease();
    if (!ca.known) return false;
    char path[80];
    snprintf(path, sizeof(path), caPath, ca.part);
    if (R_FAILED(CtrRomfs_init(caRomfs))) return false;
    FILE* f = fopen(path, caReadBin);
    if (f == nullptr) return false;
    long size = 0;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    uint8_t* data = nullptr;
    if (size >= (long) sizeof(CaHeader) && size <= (long) CA_MAX_SIZE && fseek(f, 0, SEEK_SET) == 0) {
        data = malloc((size_t) size);
        if (data != nullptr && fread(data, 1, (size_t) size, f) != (size_t) size) {
            free(data);
            data = nullptr;
        }
    }
    fclose(f);
    DataWin* dw = renderer->dataWin;
    CtrPackKey key = CtrPack_keyFor(dw);
    if (data == nullptr) return false;
    if (!Crispart_check(data, (uint32_t) size, ca.digest, ca.part, key.dataWinSize, key.gameId, key.timestamp)) goto invalid;
    const CaHeader* h = (const CaHeader*) data;
    const CaFrame* frames = (const CaFrame*) (data + sizeof(CaHeader));
    uint32_t height = 0;
    for (uint32_t i = 0; h->count > i; i++) {
        const CaFrame* frame = &frames[i];
        const Sprite* sp = caSprite(dw, frame->name);
        if (sp == nullptr || frame->frame >= sp->textureCount || frame->tpag >= dw->tpag.count ||
            sp->tpagIndices[frame->frame] != (int32_t) frame->tpag || sp->width != frame->sourceW || sp->height != frame->sourceH ||
            sp->originX != frame->originX || sp->originY != frame->originY) goto invalid;
        if (height < (uint32_t) frame->y + frame->h + 1u) height = (uint32_t) frame->y + frame->h + 1u;
    }
    ca.file = data;
    ca.bytes = (uint32_t) size;
    ca.count = h->count;
    ca.frames = frames;
    ca.lo = frames[0].tpag;
    ca.span = frames[h->count - 1].tpag - ca.lo;
    ca.height = 8;
    while (ca.height < height) ca.height *= 2;
    ca.atlas = 0;
    ca.faceCount = 0;
    // Four named sprites, four selected frame bits each: at most 16 entries.
    for (uint32_t k = 0; 4 > k; k++) {
        const Sprite* sp = caSprite(dw, caHeads[k]);
        if (sp == nullptr) continue;
        for (uint32_t f = 0; sp->textureCount > f && 10 > f; f++) {
            if ((0x321u & (1u << f)) != 0) ca.faces[ca.faceCount++] = sp->tpagIndices[f];
        }
    }
    return true;
invalid:
    free(data);
    return false;
}

// Cheap rejection for non-art draws; no allocations and no name lookups in the frame path.
static CA_TEXT const CaFrame* caFind(int32_t tpag) {
    if ((uint32_t) tpag - ca.lo > ca.span) return nullptr;
    uint32_t lo = 0, hi = ca.count;
    while (hi > lo) {
        uint32_t m = lo + (hi - lo) / 2;
        if (ca.frames[m].tpag < (uint32_t) tpag) lo = m + 1;
        else hi = m;
    }
    return lo < ca.count && ca.frames[lo].tpag == (uint32_t) tpag ? &ca.frames[lo] : nullptr;
}

uint32_t Crispart_smoothBegin(Renderer* renderer, int32_t tpag);
void Crispart_smoothEnd(Renderer* renderer, uint32_t keep);

// Asset-based fallback: the named face frames and all font draws retain Smart's normal classification.
// A future approved glyph hook can still draw its pack; no room or font-index constants are baked in here.
CA_TEXT uint32_t Crispart_smoothBegin(Renderer* renderer, int32_t tpag) {
    if (!Crispart_isCrisp(renderer)) return 0;
    if (tpag >= 0) {
        uint32_t i = 0;
        while (ca.faceCount > i && ca.faces[i] != tpag) i++;
        if (i == ca.faceCount) return 0;
    }
    return Crispart_native(renderer) ? 2u | Crispart_smart(renderer, 2) : 0;
}

CA_TEXT void Crispart_smoothEnd(Renderer* renderer, uint32_t keep) {
    if ((keep & 2u) != 0) Crispart_smart(renderer, keep & 1u);
}

static CA_TEXT bool caAtlas(Renderer* renderer) {
    (void) renderer;
    if (ca.atlas != 0) return ca.atlas > 0;
    ca.atlas = -1;
    if (!C3D_TexInit(&ca.texture, (u16) CA_ATLAS_WIDTH, (u16) ca.height, GPU_RGBA8)) {
        logInfo(caAtlasFail);
        return false;
    }
    memset(ca.texture.data, 0, ca.texture.size);
    Crispart_writeAtlas((uint32_t*) ca.texture.data, ca.file);
    C3D_TexFlush(&ca.texture);
    C3D_TexSetFilter(&ca.texture, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&ca.texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    ca.atlas = 1;
    logInfo(caAtlasLog, (unsigned) CA_ATLAS_WIDTH, (unsigned) ca.height, (unsigned) ca.texture.size);
    return true;
}

static CA_TEXT void caDraw(Renderer* renderer, const CaFrame* f, float x, float y, uint32_t color, float alpha) {
    // Whole native pixels, matching smalltext's floor of the source top-left. No panel/group translation.
    Crispart_drawTexture(renderer, &ca.texture, f, 2.0f * floorf(x * 0.5f), 2.0f * floorf(y * 0.5f), color, alpha);
}

static CA_TEXT void caDestroy(Renderer* renderer) {
    void (*destroy)(Renderer*) = Crispart_savedHooks.destroy;
    if (ca.atlas > 0) {
        C3D_FrameSync(); // queued GPU draws must finish before this private texture is released
        C3D_TexDelete(&ca.texture);
    }
    ca.atlas = 0;
    caRelease();
    ca.owner = nullptr;
    destroy(renderer);
}

static CA_TEXT void caDrawPart(Renderer* renderer, int32_t tpag, float sx, float sy, float w, float h, float x, float y,
                               float xs, float ys, float angle, float px, float py, uint32_t color, float alpha) {
    // Only the box draw made by cmdlabel for a full approved command sprite. Its separate label draw uses scale 2
    // and cannot match this condition. Arbitrary draw_sprite_part calls keep their original clipping semantics.
    const CaFrame* f = ca.inCommand ? caFind(tpag) : nullptr;
    if (f != nullptr && f->flags == CA_COMMAND && xs == 1.0f && ys == 1.0f && angle == 0.0f && sx == 0.0f && sy == 0.0f &&
        Crispart_native(renderer)) {
        const TexturePageItem* item = &renderer->dataWin->tpag.items[tpag];
        int32_t tx = item->targetX, ty = item->targetY;
        if (w == (float) item->sourceWidth && h == (float) (26 - item->targetY) && caAtlas(renderer)) {
            caDraw(renderer, f, x - (float) tx, y - (float) ty, color, alpha);
            return;
        }
    }
    uint32_t keep = Crispart_smoothBegin(renderer, tpag);
    Crispart_savedHooks.partDraw(renderer, tpag, sx, sy, w, h, x, y, xs, ys, angle, px, py, color, alpha);
    Crispart_smoothEnd(renderer, keep);
}

static CA_TEXT void caDrawSprite(Renderer* renderer, int32_t tpag, float x, float y, float ox, float oy, float xs, float ys,
                                 float angle, uint32_t color, float alpha) {
    const CaFrame* f = Crispart_isCrisp(renderer) ? caFind(tpag) : nullptr;
    if (f != nullptr && xs == 1.0f && ys == 1.0f && angle == 0.0f && Crispart_native(renderer)) {
        if (f->flags != CA_COMMAND) {
            if (caAtlas(renderer)) {
                caDraw(renderer, f, x - ox, y - oy, color, alpha);
                return;
            }
        } else if (*Crispart_cmdlabelMode > 0) {
            ca.inCommand = true;
            Crispart_savedHooks.spriteDraw(renderer, tpag, x, y, ox, oy, xs, ys, angle, color, alpha);
            ca.inCommand = false;
            return;
        } else if (caAtlas(renderer)) {
            // cmdlabel=0: retain its original label rows at the original scale, after the derived box.
            const TexturePageItem* item = &renderer->dataWin->tpag.items[tpag];
            caDraw(renderer, f, x - ox, y - oy, color, alpha);
            int32_t from = 26 - item->targetY;
            if (from < 0) from = 0;
            if (from < item->sourceHeight) {
                Crispart_savedHooks.partDraw(renderer, tpag, 0.0f, (float) from, (float) item->sourceWidth, (float) (item->sourceHeight - from),
                            x - ox + (float) item->targetX, y - oy + (float) (item->targetY + from),
                            1.0f, 1.0f, 0.0f, 0.0f, 0.0f, color, alpha);
            }
            return;
        }
    }
    uint32_t keep = Crispart_smoothBegin(renderer, tpag);
    Crispart_savedHooks.spriteDraw(renderer, tpag, x, y, ox, oy, xs, ys, angle, color, alpha);
    Crispart_smoothEnd(renderer, keep);
}

// All five callbacks return void and use at most s0-s15. Color callbacks have
// exactly two stack arguments (c3,c4); plain text and positioned sprites have none.
// Share the register/stack preservation instead of emitting it in each C wrapper.
__typeof__(*(((RendererVtable*) 0)->drawText)) Crispart_text;
__typeof__(*(((RendererVtable*) 0)->drawTextColor)) Crispart_textColor;
__typeof__(*(((RendererVtable*) 0)->drawTextUI)) Crispart_textUI;
__typeof__(*(((RendererVtable*) 0)->drawSpritePartColor)) Crispart_partColor;
__typeof__(*(((RendererVtable*) 0)->drawSpritePos)) Crispart_spritePos;

__asm__(
    ".pushsection .text.late.crispart,\"ax\",%progbits\n"
    ".arm\n"
    ".balign 4\n"
    ".global Crispart_text\n"
    ".type Crispart_text, %function\n"
    "Crispart_text: mov ip, #3\n"
    "b Crispart_smartCall\n"
    ".size Crispart_text, .-Crispart_text\n"
    ".global Crispart_textColor\n"
    ".type Crispart_textColor, %function\n"
    "Crispart_textColor: mov ip, #4\n"
    "b Crispart_smartCall\n"
    ".size Crispart_textColor, .-Crispart_textColor\n"
    ".global Crispart_textUI\n"
    ".type Crispart_textUI, %function\n"
    "Crispart_textUI: mov ip, #5\n"
    "b Crispart_smartCall\n"
    ".size Crispart_textUI, .-Crispart_textUI\n"
    ".global Crispart_partColor\n"
    ".type Crispart_partColor, %function\n"
    "Crispart_partColor: mov ip, #6\n"
    "b Crispart_smartCall\n"
    ".size Crispart_partColor, .-Crispart_partColor\n"
    ".global Crispart_spritePos\n"
    ".type Crispart_spritePos, %function\n"
    "Crispart_spritePos: mov ip, #7\n"
    "b Crispart_smartCall\n"
    ".size Crispart_spritePos, .-Crispart_spritePos\n"
    ".type Crispart_smartCall, %function\n"
    "Crispart_smartCall:\n"
    // 40 B integer frame + 64 B VFP frame: every BL/BLX remains 8 B aligned.
    "push {r0-r7, ip, lr}\n"
    "vpush {d0-d7}\n"
    "mov r4, r0\n"
    "mov r6, ip\n"
    "cmp ip, #6\n"
    "mvnlo r1, #0\n" // text: tpag=-1; sprite calls retain their tpag in r1
    "bl Crispart_smoothBegin\n"
    "mov r5, r0\n"
    "vpop {d0-d7}\n"
    "ldmia sp, {r0-r3}\n"
    // Restore c3/c4 at outgoing SP only for color callbacks (indexes 4..6).
    // r0/r1 have already been restored, so their saved slots can hold the copy.
    "sub ip, r6, #4\n"
    "cmp ip, #2\n"
    "ldrls r7, [sp, #40]\n"
    "strls r7, [sp, #0]\n"
    "ldrls r7, [sp, #44]\n"
    "strls r7, [sp, #4]\n"
    "ldr ip, =Crispart_savedHooks\n"
    "ldr r6, [ip, r6, lsl #2]\n"
    "blx r6\n"
    "mov r0, r4\n"
    "mov r1, r5\n"
    "bl Crispart_smoothEnd\n"
    "add sp, sp, #16\n"
    "pop {r4-r7, ip, pc}\n"
    ".size Crispart_smartCall, .-Crispart_smartCall\n"
    ".ltorg\n"
    ".popsection\n"
);

// Matching member order in CaHooks. Null optional hooks retain their fallback.
static const uint16_t caHookOffsets[] CA_CONST = {
    __builtin_offsetof(RendererVtable, drawSprite), __builtin_offsetof(RendererVtable, drawSpritePart),
    __builtin_offsetof(RendererVtable, destroy), __builtin_offsetof(RendererVtable, drawText),
    __builtin_offsetof(RendererVtable, drawTextColor), __builtin_offsetof(RendererVtable, drawTextUI),
    __builtin_offsetof(RendererVtable, drawSpritePartColor), __builtin_offsetof(RendererVtable, drawSpritePos)
};
static const CaHook caHookFunctions[] CA_CONST = {
    (CaHook) caDrawSprite, (CaHook) caDrawPart, (CaHook) caDestroy, (CaHook) Crispart_text,
    (CaHook) Crispart_textColor, (CaHook) Crispart_textUI, (CaHook) Crispart_partColor, (CaHook) Crispart_spritePos
};

// Startup/room-load hook: keys have been applied, renderer/TPAG parsed; off never changes the renderer's table.
CA_TEXT void __wrap_CtrRenderer_loadFontVerdicts(Renderer* renderer) {
    __real_CtrRenderer_loadFontVerdicts(renderer);
    if (renderer == nullptr || renderer->dataWin == nullptr) return;
    if (ca.owner == renderer) return; // remember off and unavailable decisions too; reset at chapter check
    ca.owner = renderer;
    if (ca.given == 0 && ca.setting == 0) {
        ca.setting = caReadKey(caIni);
        if (ca.setting == 0) ca.setting = 1;
    }
    int8_t value = ca.given != 0 ? ca.given : ca.setting;
    if (value != 2) return;
    if (!caLoad(renderer)) {
        logInfo(caSkipLog, ca.part);
        return;
    }
    RendererVtable* vt = renderer->vtable;
    if (vt == (const RendererVtable*) Cmdlabel_refs[0]) return; // only the outer table; leave frame-skip swaps live
    for (uint32_t i = 0; sizeof(caHookFunctions) / sizeof(caHookFunctions[0]) > i; i++) {
        char* slot = (char*) vt + caHookOffsets[i];
        CaHook original;
        memcpy(&original, slot, sizeof(original));
        memcpy((char*) &Crispart_savedHooks + i * sizeof(CaHook), &original, sizeof(original));
        if (original != nullptr) memcpy(slot, &caHookFunctions[i], sizeof(original));
    }
    logInfo(caLoadLog, ca.part, (unsigned) ca.count, (unsigned) ca.bytes);
}
#endif // !CRISPART_HOST_TEST
#endif // ENABLE_WAD17 || CRISPART_HOST_TEST
