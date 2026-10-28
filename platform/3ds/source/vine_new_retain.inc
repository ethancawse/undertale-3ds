/* Exact tower face retention on DELTARUNE New 3DS (flowergpu, late code only).
 *
 * Chapter 5's Flower Castle clears each tower face (tower_front, tower_back) every frame and draws some 23 layers onto
 * it with the tower shader (drawTowerStrips): about 70 ms of the GPU's frame before natsurf6. Where the tower stands still
 * (ch5_flowcut) every frame repeats the same clear and the same draws of the same, unchanged layers. A face whose whole
 * clear and every following tower draw exactly repeat the recipe its pixels were last drawn from keeps those pixels:
 * nothing is drawn. Anything else draws as before.
 *
 * How it stays exact. tools/analysis/vine_old_parent.py routes ctr_renderer.c's calls of clearBoundTarget,
 * drawTowerStrips and flushLazy here, without changing any original body. A face's recipe is the clear (colour, size)
 * and, per tower draw, the CtrLazyDraw that deferTowerDraw would keep for it (the layer, its content version, source
 * rectangle, quad, colours, tower shader uniforms and the whole draw state lazyCapture saves) plus the layer's texture
 * identity. Recording starts at a whole clear and stops at anything not a plain tower draw onto the unchanged face.
 *   - A clear that repeats the recipe's clear, onto a face whose version is still the one its recipe left, is put off:
 *     the face gets a marker entry in its list of draws put off (CtrSurface.lazy) and a new version.
 *   - Each following tower draw that repeats the recipe's next draw is put off in that same list (as deferTowerDraw
 *     would, with the layer's lazyRefs), so every existing barrier sees it: sampling the face, reading it, copying to it,
 *     drawing on it otherwise, changing one of the layers, remaking or freeing the face. Such a barrier reaches
 *     flushLazy, routed here, which draws the clear and the draws put off so far, in order: what drawing at once would
 *     have given.
 *   - When the last draw of the recipe repeats, the list is dropped: the face's pixels are exactly what the clear and
 *     those draws give. A draw that differs first draws what was put off, then itself, and recording goes on.
 *   - A whole clear while draws are put off restarts the comparison (the pixels still hold the whole recipe).
 * Only draws deferTowerDraw could put off are recorded (game projection, no pass under way, no frame skip, no 3D). */
#define VR_CODE __attribute__((noinline, used, optimize("Os", "inline-functions", "no-tree-switch-conversion"), section(".text.late.flowergpu")))
#define VR_RO   __attribute__((section(".rodata.late.flowergpu")))
#define VR_BSS  __attribute__((section(".bss.late.flowergpu")))
#define VR_CALL(name) ({ __typeof__(&(name)) vrFn = &(name); __asm__("" : "+r"(vrFn)); vrFn; })

#define VR_SLOTS 4
#define VR_CAP 64
#define VR_MAGIC 0x56524e54 // the marker entry's srcLeft; its srcTop is the slot
#define VB_MAGIC 0x56424e44 // a band list's marker srcLeft (cut 2): srcTop, srcHeight the rows drawn, srcWidth the draws

typedef struct {
    const void* pixels;
    int32_t width, height, shift, texW, texH, fmt;
} VrIdentity;

typedef struct {
    bool used, recording, deferring;
    int32_t face;
    uint32_t count, idx, savedVersion, lastFrame;
    CtrColor clearColor;
    int32_t clearW, clearH;
    VrIdentity faceId;
    CtrLazyDraw* sigs;     // VR_CAP of them
    VrIdentity* sources;   // VR_CAP of them
    CtrLazyDraw* stash;    // the face's band list (cut 2) while its repeated clear is put off
} VrSlot;

static VrSlot vrSlots[VR_SLOTS] VR_BSS;
static CtrRenderer* vrOwner VR_BSS;
static uint32_t vrSerial VR_BSS;
static bool vrReplaying VR_BSS;
static bool vrDisabled VR_BSS;
static bool vrLogged VR_BSS;
static VrSlot* vrPend VR_BSS;   // the slot whose clear the face's next clear during a replay draws instead
static int32_t vrPendFace VR_BSS;
static const char vrFmtKept[] VR_RO = "Renderer: kept a tower face whose clear and %u draws repeated exactly (logged once)\n";
static const char vrFmtOff[] VR_RO = "Renderer: tower face retention off (content serial wrapped at %u)\n";

static VR_CODE void vrIdentityOf(const CtrSurface* s, VrIdentity* id) {
    memset(id, 0, sizeof(*id));
    id->pixels = s->tex.data;
    id->width = s->width;
    id->height = s->height;
    id->shift = s->shift;
    id->texW = s->tex.width;
    id->texH = s->tex.height;
    id->fmt = (int32_t) s->tex.fmt | (s->spilled ? 0x100 : 0);
}

static VR_CODE bool vrMarkerOk(const CtrSurface* face, const VrSlot* slot) {
    return face != nullptr && face->lazy != nullptr && face->lazy[0].layer == -1 && face->lazy[0].srcLeft == VR_MAGIC &&
           face->lazy[0].srcTop == (int32_t) (slot - vrSlots);
}

// Copies of getSurface and lazyCapture: the original compile inlines those everywhere, so they have no symbol.
static VR_CODE CtrSurface* vrSurface(CtrRenderer* r, int32_t id) {
    if (0 > id || (uint32_t) id >= r->surfaceCount) return nullptr;
    CtrSurface* s = &r->surfaces[id];
    return s->used ? s : nullptr;
}

static VR_CODE void vrLazyCapture(CtrRenderer* r, CtrLazyDraw* d) {
    CtrTargetState* st = &d->target;
    st->viewport = r->viewport;
    st->scissorEnabled = r->scissorEnabled;
    st->scissor = r->scissor;
    st->hostPass = r->hostPass;
    memcpy(st->gmlMatrices, r->base.gmlMatrices, sizeof(st->gmlMatrices));
    st->cameraCurrent = r->base.cameraCurrent;
    st->portX = r->base.CPortX;
    st->portY = r->base.CPortY;
    st->portW = r->base.CPortW;
    st->portH = r->base.CPortH;
    d->blendFactors = r->base.blendFactors;
    d->blendMode = r->blendMode;
    d->blendEnable = r->blendEnable;
    d->alphaTestEnable = r->alphaTestEnable;
    d->alphaTestRef = r->alphaTestRef;
    memcpy(d->colorWrite, r->colorWrite, sizeof(d->colorWrite));
    d->fogEnable = r->fogEnable;
    d->fogColor = r->fogColor;
    d->texFilter = r->base.texFilter;
    d->filterOverride = r->filterOverride;
    d->shaderFx = r->shaderFx;
    d->fxShader = r->fxShader;
}

// A face whose list of draws put off is a band list (cut 2): drawn so far only in the rows its marker names.
static VR_CODE bool vrIsBand(const CtrSurface* face) {
    return face != nullptr && face->lazy != nullptr && face->lazy[0].layer == -1 && face->lazy[0].srcLeft == VB_MAGIC;
}

// Frees a list of draws put off that is no surface's, giving back its layers' references (as dropLazy).
static VR_CODE void vrReleaseList(CtrRenderer* r, CtrLazyDraw** list) {
    if (*list == nullptr) return;
    for (ptrdiff_t i = 0; arrlen(*list) > i; i++) {
        CtrSurface* layer = vrSurface(r, (*list)[i].layer);
        if (layer != nullptr && layer->lazyRefs != 0) layer->lazyRefs--;
    }
    arrfree(*list);
    *list = nullptr;
}

// applyColorWrite's GPU write mask. Only a quad clear (r->quadClears, the start-up probe) depends on it.
static VR_CODE void vrApplyColorWrite(CtrRenderer* r) {
    int mask = (r->colorWrite[0] ? GPU_WRITE_RED : 0) | (r->colorWrite[1] ? GPU_WRITE_GREEN : 0) | (r->colorWrite[2] ? GPU_WRITE_BLUE : 0) |
               (r->colorWrite[3] ? GPU_WRITE_ALPHA : 0);
    C3D_DepthTest(false, GPU_ALWAYS, (GPU_WRITEMASK) mask);
}

// Draws what is put off on a face with a pending clear: the clear, then the draws put off, in order. The original
// flushLazy does it all, keeping the marker entry (no surface has id -1, so it draws nothing for it): marking the face
// to be cleared makes the bindSurface there clear it first, through Vine_wrap_clearBoundTarget, which draws the
// recipe's clear in place of the transparent one.
static VR_CODE void vrMaterialize(CtrRenderer* r, int32_t faceId, VrSlot* slot) {
    CtrSurface* face = &r->surfaces[faceId];
    bool extra = (size_t) arrlen(face->lazy) != (size_t) slot->idx + 1; // draws deferTowerDraw added
    slot->deferring = false;
    vrReleaseList(r, &slot->stash); // the clear about to be drawn replaces what it held
    VrSlot* outerPend = vrPend;
    int32_t outerFace = vrPendFace;
    vrPend = slot;
    vrPendFace = faceId;
    face->needsClear = true;
    bool was = vrReplaying;
    vrReplaying = true;
    VR_CALL(flushLazy)(r, faceId);
    vrReplaying = was;
    face = vrSurface(r, faceId);
    if (vrPend == slot && face != nullptr) face->needsClear = false; // not reached (flushLazy found no frame)
    vrPend = outerPend;
    vrPendFace = outerFace;
    slot->count = slot->idx;
    slot->recording = !extra && face != nullptr && face->lazy == nullptr;
    if (face != nullptr) slot->savedVersion = face->version;
}

static VR_CODE void vrReset(VrSlot* slot) {
    slot->recording = false;
    slot->deferring = false;
    slot->count = 0;
    slot->idx = 0;
}

// The slot of surface id, or nullptr. Forgets every slot for a new renderer; turns retention off for good if the
// content serial wraps (versions could repeat).
static VR_CODE VrSlot* vrFind(CtrRenderer* r, int32_t id) {
    if (vrOwner != r) {
        for (int32_t k = 0; VR_SLOTS > k; k++) vrReset(&vrSlots[k]), vrSlots[k].used = false;
        vrOwner = r;
        vrSerial = r->contentSerial;
    }
    if (vrDisabled) return nullptr;
    if (r->contentSerial < vrSerial) {
        vrDisabled = true;
        logInfo(vrFmtOff, (unsigned) r->contentSerial);
        for (int32_t k = 0; VR_SLOTS > k; k++) {
            VrSlot* slot = &vrSlots[k];
            CtrSurface* face = slot->used ? vrSurface(r, slot->face) : nullptr;
            if (slot->deferring && vrMarkerOk(face, slot) && r->inFrame) vrMaterialize(r, slot->face, slot);
            vrReset(slot);
        }
        return nullptr;
    }
    vrSerial = r->contentSerial;
    if (0 > id) return nullptr;
    for (int32_t k = 0; VR_SLOTS > k; k++) {
        if (vrSlots[k].used && vrSlots[k].face == id) return &vrSlots[k];
    }
    return nullptr;
}

static VR_CODE void vrAlloc(CtrRenderer* r, int32_t id) {
    VrSlot* pick = nullptr;
    for (int32_t k = 0; VR_SLOTS > k; k++) {
        VrSlot* slot = &vrSlots[k];
        if (!slot->used) {
            pick = slot;
            break;
        }
        if (slot->deferring || slot->lastFrame == r->frame) continue;
        if (pick == nullptr || pick->lastFrame > slot->lastFrame) pick = slot;
    }
    if (pick == nullptr) return;
    if (pick->sigs == nullptr) {
        pick->sigs = malloc(VR_CAP * sizeof(CtrLazyDraw));
        pick->sources = malloc(VR_CAP * sizeof(VrIdentity));
        if (pick->sigs == nullptr || pick->sources == nullptr) {
            free(pick->sigs);
            free(pick->sources);
            pick->sigs = nullptr;
            pick->sources = nullptr;
            pick->used = false;
            return;
        }
    }
    vrReset(pick);
    pick->used = true;
    pick->face = id;
    pick->lastFrame = r->frame;
}

// A face whose clear may start or repeat a recipe.
static VR_CODE bool vrFaceOk(CtrRenderer* r, int32_t id, const CtrSurface* face) {
    return face != nullptr && r->inFrame && r->boundTarget != nullptr && r->base.runner != nullptr &&
           id != r->base.runner->applicationSurfaceId && !face->recorded && !face->windowed && face->parked == nullptr &&
           face->skipRec == nullptr && face->lazyRefs == 0 && !r->skip.active && !r->quadClears && !r->depth.active;
}

// A tower draw deferTowerDraw could put off, onto face, of layer s.
static VR_CODE bool vrDrawOk(CtrRenderer* r, int32_t faceId, const CtrSurface* face, int32_t layerId, const CtrSurface* s) {
    return vrFaceOk(r, faceId, face) && s != nullptr && layerId != faceId && !s->recorded && !s->windowed && s->lazy == nullptr &&
           s->skipRec == nullptr && s->skipRealRefs == 0 && s->tex.data != nullptr && r->gameProjection && r->palNextB == nullptr &&
           r->shrink.batchMode == 0 && r->crtPass == 0 && r->layer.mode == 0 && !r->plainTexEnv && r->opAlphaCut == 0 &&
           !r->depth.flat && !r->fxSuspended && r->skip.towerUni == nullptr;
}

#ifdef CTRPORT_FLOWERGPU_VRAMPROBE
// The VRAM probe (test only, CTRPORT_FLOWERGPU_VRAMPROBE; it also routes surfaceTexture here). Tower layers are
// 360x360 RGBA8 surfaces, 736 KB each in memory, all 23 in FCRAM. While a tower draw runs, a layer's texture is
// swapped for a raw copy in VRAM if free VRAM holds one (tryVramTarget never makes room), refreshed when the layer's
// version changes, so gputrace can compare one pass sampling VRAM with its FCRAM neighbours in the same frame. Each
// layer's texels are also counted once (alpha values, colours) to price a smaller exact format.
#define VR_VRAM_LAYERS 8
typedef struct {
    int32_t id;
    uint32_t version;
    const void* from;
    bool valid;
    C3D_Tex tex;
    C3D_RenderTarget* target;
} VrVramLayer;
static VrVramLayer vrVram[VR_VRAM_LAYERS] VR_BSS;
static int32_t vrVramCount VR_BSS;
static uint32_t vrVramTryFrame VR_BSS;
static uint32_t vrVramCopies VR_BSS;
static bool vrInTower VR_BSS;
static uint32_t vrScanned[8] VR_BSS;
static const char vrFmtVram[] VR_RO = "[vramprobe] frame %u layer %d copied to VRAM (%u KB, %d layers there, %u KB VRAM free)\n";
static const char vrFmtNoVram[] VR_RO = "[vramprobe] frame %u layer %d: no free VRAM for %u KB (%u KB free, %d layers there)\n";
static const char vrFmtScan[] VR_RO =
    "[vramprobe] layer %d %dx%d fmt %d tex %ux%u: %u opaque, %u partial, %u clear texels, %u alpha values, %u colours under "
    "alpha (%06x %06x %06x %06x), %u more\n";
static const char vrFmtCopies[] VR_RO = "[vramprobe] frame %u: %u layer copies so far, %d layers in VRAM\n";

static VR_CODE void vrLayerScan(CtrRenderer* r, int32_t id, const CtrSurface* s, const C3D_Tex* tex) {
    (void) r;
    if (0 > id || 256 <= id || (vrScanned[id >> 5] & (1u << (id & 31))) != 0) return;
    vrScanned[id >> 5] |= 1u << (id & 31);
    uint32_t alphas[8] = { 0 }, colours[4] = { 0 };
    uint32_t opaque = 0, partial = 0, clear = 0, nc = 0, more = 0;
    if (tex->fmt == GPU_RGBA8) {
        GSPGPU_InvalidateDataCache(tex->data, tex->size);
        const uint32_t* px = (const uint32_t*) tex->data;
        for (uint32_t i = 0; tex->size / 4u > i; i++) {
            uint32_t v = px[i], a = v & 0xffu;
            alphas[a >> 5] |= 1u << (a & 31u);
            if (a == 0) {
                clear++;
                continue;
            }
            if (a == 0xffu) opaque++;
            else partial++;
            uint32_t rgb = v >> 8, k = 0;
            while (nc > k && colours[k] != rgb) k++;
            if (nc > k) continue;
            if (4u > nc) colours[nc++] = rgb;
            else more++;
        }
    }
    uint32_t values = 0;
    for (int32_t k = 0; 8 > k; k++) values += (uint32_t) __builtin_popcount(alphas[k]);
    logInfo(vrFmtScan, (int) id, (int) s->width, (int) s->height, (int) tex->fmt, (unsigned) tex->width, (unsigned) tex->height,
            (unsigned) opaque, (unsigned) partial, (unsigned) clear, (unsigned) values, (unsigned) nc, (unsigned) colours[0],
            (unsigned) colours[1], (unsigned) colours[2], (unsigned) colours[3], (unsigned) more);
}

VR_CODE C3D_Tex* Vine_wrap_surfaceTexture(CtrRenderer* r, int32_t id) {
    C3D_Tex* tex = VR_CALL(surfaceTexture)(r, id);
    if (!vrInTower || tex == nullptr) return tex;
    CtrSurface* s = vrSurface(r, id);
    if (s == nullptr || s->width != 360 || s->height != 360) return tex;
    vrLayerScan(r, id, s, tex);
    uintptr_t at = (uintptr_t) tex->data;
    if (at >= OS_VRAM_VADDR && OS_VRAM_VADDR + OS_VRAM_SIZE > at) return tex;
    VrVramLayer* v = nullptr;
    for (int32_t i = 0; vrVramCount > i && v == nullptr; i++) {
        if (vrVram[i].id == id) v = &vrVram[i];
    }
    uint32_t kb = tex->size / 1024u;
    if (v == nullptr) {
        if (VR_VRAM_LAYERS <= vrVramCount || (vrVramTryFrame != 0 && r->frame - vrVramTryFrame < 32u)) return tex;
        v = &vrVram[vrVramCount];
        memset(v, 0, sizeof(*v));
        int32_t rows = (int32_t) (tex->size / ((uint32_t) tex->width * 4u));
        if (tex->fmt != GPU_RGBA8 || !VR_CALL(tryVramTarget)(&v->tex, &v->target, tex->width, tex->height, rows)) {
            vrVramTryFrame = r->frame == 0 ? 1 : r->frame;
            logInfo(vrFmtNoVram, (unsigned) r->frame, (int) id, (unsigned) kb, (unsigned) (vramSpaceFree() / 1024u), (int) vrVramCount);
            return tex;
        }
        v->id = id;
        vrVramCount++;
        logInfo(vrFmtVram, (unsigned) r->frame, (int) id, (unsigned) kb, (int) vrVramCount, (unsigned) (vramSpaceFree() / 1024u));
    }
    if (!v->valid || v->version != s->version || v->from != tex->data) {
        VR_CALL(splitFrame)(r);
        VR_CALL(queueTextureCopy)(r, tex, &v->tex, tex->width);
        v->valid = true;
        v->version = s->version;
        v->from = tex->data;
        if ((++vrVramCopies & 127u) == 1u) logInfo(vrFmtCopies, (unsigned) r->frame, (unsigned) vrVramCopies, (int) vrVramCount);
    }
    v->tex.param = tex->param;
    v->tex.border = tex->border;
    v->tex.lodParam = tex->lodParam;
    return &v->tex;
}
#endif

// The tower draw: with the VRAM probe on, the probe's layer swap around it; with it off, cut 3's shared depth texture.
#ifndef CTRPORT_FLOWERGPU_VRAMPROBE
/* Cut 3: the tower's depth layers drawn from one texture in VRAM.
 *
 * Twenty of the tower's layers (ch5_climb, ch5_flower, ch5_flowcut) are cuts of one depth sprite: the same 180x180
 * page region tiled over a 360x360 surface, each kept where its alpha is above that layer's cut (the depth-map shader; patternAdd
 * notes each as that region with its cut). Each is a 736 KB texture in FCRAM, where the GPU fetches texels several
 * times slower than from VRAM (gputrace: 7.5-10.8 ms a face pass against 1.3 ms), and VRAM has no room for them. One
 * 256x512 RGBA8 texture in VRAM (368 KB of stored rows) stands in for them all: a texel there has the layers' colour
 * and, as alpha, one more than the highest cut of the layers keeping it (0 where none does). Drawn with a layer's cut
 * as the replay alpha cut (alphaCutStages: alpha up to the cut becomes 0, the rest 255), it gives that layer's alpha (0
 * or 255) and, where the layer keeps the texel, its colour; elsewhere alpha 0, which the normal blend leaves out. It
 * holds the layers' columns 0-255; a strip whose columns all lie at 180 or beyond samples it 180 columns to the left,
 * where the layers repeat.
 *
 * How it stays exact.
 *   - Built on the CPU, one layer a frame, once the same layers (surfaces, versions, textures, one page region) have
 *     been drawn 4 frames running, each read after a data cache invalidate. Kept only if every layer has alpha 0 or 255
 *     only, every kept texel has the same colour in every layer keeping it, and every layer, in all its rows, keeps
 *     exactly the texels whose shared alpha is above its cut, at columns 0-255 and, 180 columns to the left, at columns
 *     180-359. Copied to VRAM by the GX, from free VRAM beyond MIRROR_RESERVE; nothing is moved.
 *   - drawTowerStrips runs as before, with the layer's cut set (on the layer itself the cut changes nothing: its alpha
 *     is 0 or 255). tools/analysis/vine_old_parent.py routes its own two flushBatch calls here; the second draws the
 *     strips. When that batch is quads of the layer's texture alone, under the normal blend, nearest filtering and the
 *     plain path, every quad's u moves onto the shared texture (u' = 2u, or 2u - 180/256) and the batch is drawn from
 *     it; a sample keeps its place inside the texel (a whole number of texels moved, texels as wide in u'), so nearest
 *     sampling takes the same texel, and v is unchanged (same rows, same height). A quad starting left of column 0 (a
 *     strip at the cylinder's edge; about a quarter of the batches) samples column 0 there on both textures, as both
 *     clamp to the edge. Otherwise, or if a quad spans columns both under 180 and past 255, the batch is drawn from the
 *     layer, as before.
 *   - A layer's version or texture changing, or 90 frames without a tower draw, drops the texture (freed 3 frames on). */
#define VD_MAX 24
#define VD_COLS 256  // the shared texture's columns
#define VD_SHIFT 180 // the region's width: strips at or past this column sample this many columns to the left
#define VD_IDLE 0
#define VD_BUILD 1
#define VD_READY 2

typedef struct {
    int32_t id;
    uint32_t version;
    const void* data;
    uint8_t cut;
} VdLayer;

typedef struct {
    uint8_t state;
    bool active;           // a shared layer's tower draw is running
    bool changed;          // the layers noted changed this frame
    bool loggedReady, loggedUse;
    uint8_t flushes;       // routed flushBatch calls in that draw
    uint8_t fails;
    uint8_t cut;           // that draw's layer cut
    int32_t count, step;
    uint32_t frame, stable, lastUse, retryFrame, readyFrame, freeFrame, size;
    CtrPattern key;
    VdLayer layer[VD_MAX];
    uint32_t* stage;       // the texture being built, in linear memory
    void* freeStage;       // freed at freeFrame
    void* freeVram;
    C3D_Tex tex;
    const C3D_Tex* layerTex;
    C3D_Tex* drawTex;      // the texture that draw's strips come from: tex, or a plain copy's (cut 4b)
    CtrRenderer* owner;    // the renderer the layers are of, and its content serial when last seen
    uint32_t serial;
} VdState;
static VdState vd VR_BSS;
static const uint8_t vdMx[8] VR_RO = { 0, 1, 4, 5, 16, 17, 20, 21 };  // a texel's x within its 8x8 tile
static const uint8_t vdMy[8] VR_RO = { 0, 2, 8, 10, 32, 34, 40, 42 }; // a tile row's y (in some order)
static const char vdFmtReady[] VR_RO = "Renderer: %d tower depth layers share one VRAM texture (%u KB) (logged once)\n";
static const char vdFmtUse[] VR_RO = "Renderer: drew tower depth layer strips from the shared VRAM texture (logged once)\n";
static const char vdFmtFail[] VR_RO = "Renderer: tower depth layers not shared: %s at step %d of %d, %u KB VRAM free, mirror extra %u KB (logged up to 3 times)\n";
static const char vdWhyVram[] VR_RO = "no free VRAM";
static const char vdWhyMem[] VR_RO = "no linear memory";
static const char vdWhyGone[] VR_RO = "a layer changed";
static const char vdWhyCheck[] VR_RO = "a layer differs";

/* climbnoise (Azahar only): the shared depth texture (cut 3) and the plain copies (cut 4b) store 368 of their 512
 * rows. In Azahar's VRAM the cached texture then reaches into the render targets after it, and the tower drew missing
 * or foreign texels; on a console it is exact (docs/3ds-hardware.md, "A VRAM texture whose stored rows stop short of
 * its height drew wrong texels in Azahar"). So in Azahar both go to linear memory, where they are exact. The VRAM checks
 * stay, but the textures take none in Azahar, so it can make more plain copies than a console does (same pixels). On a
 * console: one test of vramtouch's cached flag at each make and each free. */
bool CtrVramTouch_emulator(void); // ctr_vram_touch.c's vramTouchOn (an alias): svcGetSystemInfo(0x20000, 0), cached
static __attribute__((noinline, used, section(".text.climbnoise_late"))) void cnTowerFree(void* tex) {
    if (CtrVramTouch_emulator()) linearFree(tex);
    else vramFree(tex);
}

// Whether s can be a shared layer: an unchanged 360x360 cut of one 180x180 page region, in a 512x512 RGBA8 texture.
static VR_CODE bool vdCandidate(const CtrSurface* s) {
    const CtrPattern* p = &s->pattern;
    return !s->recorded && !s->windowed && s->shift == 0 && s->width == 360 && s->height == 360 && s->tex.fmt == GPU_RGBA8 &&
           s->tex.width == 512 && s->tex.height == 512 && s->tex.size >= 512u * 4u * 360u && s->tex.data != nullptr &&
           p->version == s->version && 0 <= p->pageId && p->w == VD_SHIFT && p->h == VD_SHIFT && 2 <= p->cut && 254 >= p->cut;
}

static VR_CODE bool vdSameKey(const CtrPattern* p) {
    const CtrPattern* k = &vd.key;
    return p->pageId == k->pageId && p->sx == k->sx && p->sy == k->sy && p->w == k->w && p->h == k->h && p->ox == k->ox &&
           p->oy == k->oy && p->cells == k->cells;
}

// Back to noting layers afresh; the texture (if made) and the stage are freed once the GPU is past them.
static VR_CODE void vdDrop(CtrRenderer* r) {
    if (vd.freeStage != nullptr) linearFree(vd.freeStage);
    if (vd.freeVram != nullptr) cnTowerFree(vd.freeVram);
    vd.freeStage = vd.stage;
    vd.freeVram = vd.tex.data;
    vd.freeFrame = vd.freeStage != nullptr || vd.freeVram != nullptr ? r->frame + 3u : 0u;
    vd.stage = nullptr;
    memset(&vd.tex, 0, sizeof(vd.tex));
    vd.state = VD_IDLE;
    vd.count = 0;
    vd.stable = 0;
    vd.changed = false;
}

static VR_CODE void vdFail(CtrRenderer* r, const char* why) {
    if (3 > vd.fails && !vd.loggedReady) {
        logInfo(vdFmtFail, why, (int) vd.step, (int) (2 * vd.count + 1), (unsigned) (CtrMemory_vramFreeNow() / 1024u),
                (unsigned) (r->mirror.extra / 1024u));
    }
    vdDrop(r);
    vd.retryFrame = r->frame + (120u << vd.fails);
    if (4 > vd.fails) vd.fails++;
}

// A tower draw of layer id that isn't drawn from the shared texture: noted while the layers are gathered.
static VR_CODE void vdNote(int32_t id, const CtrSurface* s) {
    if (vd.state != VD_IDLE || !vdCandidate(s)) return;
    const CtrPattern* p = &s->pattern;
    if (vd.count == 0) vd.key = *p;
    else if (!vdSameKey(p)) return;
    for (int32_t i = 0; vd.count > i; i++) {
        VdLayer* l = &vd.layer[i];
        if (l->id != id) continue;
        if (l->version != s->version || l->data != s->tex.data || l->cut != p->cut) {
            *l = (VdLayer) { id, s->version, s->tex.data, p->cut };
            vd.changed = true;
        }
        return;
    }
    if (VD_MAX <= vd.count) return;
    vd.layer[vd.count++] = (VdLayer) { id, s->version, s->tex.data, p->cut };
    vd.changed = true;
}

// Step 1 for one layer: its kept texels at columns 0-255 into the stage, each colour checked against the layers
// before it, alpha raised to its cut + 1.
static VR_CODE bool vdMerge(const VdLayer* l, const CtrSurface* s) {
    const uint32_t* src = (const uint32_t*) s->tex.data;
    uint32_t mark = (uint32_t) l->cut + 1u, rowWords = (uint32_t) s->tex.width * 8u;
    int32_t tileRows = (s->height + 7) >> 3;
    for (int32_t tr = 0; tileRows > tr; tr++) {
        const uint32_t* a = src + (uint32_t) tr * rowWords;
        uint32_t* d = vd.stage + (uint32_t) tr * VD_COLS * 8u;
        for (uint32_t k = 0; VD_COLS * 8u > k; k++) {
            uint32_t t = a[k], alpha = t & 0xffu;
            if (alpha == 0) continue;
            if (alpha != 0xffu) return false;
            uint32_t was = d[k];
            if ((was & 0xffu) == 0) d[k] = (t & ~0xffu) | mark;
            else if (((was ^ t) >> 8) != 0) return false;
            else if (mark > (was & 0xffu)) d[k] = (was & ~0xffu) | mark;
        }
    }
    return true;
}

// Step 2 for one layer: kept exactly where the shared alpha is above its cut, at columns 0-255, and at columns
// 180-359 against the shared texel 180 to the left (colour too, alpha 0 or 255 only).
static VR_CODE bool vdCheck(const VdLayer* l, const CtrSurface* s) {
    const uint32_t* src = (const uint32_t*) s->tex.data;
    uint32_t cut = l->cut, rowWords = (uint32_t) s->tex.width * 8u;
    int32_t tileRows = (s->height + 7) >> 3;
    for (int32_t tr = 0; tileRows > tr; tr++) {
        const uint32_t* a = src + (uint32_t) tr * rowWords;
        const uint32_t* d = vd.stage + (uint32_t) tr * VD_COLS * 8u;
        for (uint32_t k = 0; VD_COLS * 8u > k; k++) {
            if (((a[k] & 0xffu) != 0) != ((d[k] & 0xffu) > cut)) return false;
        }
        for (int32_t yb = 0; 8 > yb; yb++) {
            uint32_t my = vdMy[yb];
            for (int32_t x = VD_SHIFT; s->width > x; x++) {
                int32_t y = x - VD_SHIFT;
                uint32_t t = a[(uint32_t) (x >> 3) * 64u + vdMx[x & 7] + my];
                uint32_t dv = d[(uint32_t) (y >> 3) * 64u + vdMx[y & 7] + my];
                uint32_t alpha = t & 0xffu;
                if (alpha != 0 && alpha != 0xffu) return false;
                bool kept = alpha != 0;
                if (kept != ((dv & 0xffu) > cut)) return false;
                if (kept && ((t ^ dv) >> 8) != 0) return false;
            }
        }
    }
    return true;
}

// One step of the build, at most one a frame: the VRAM texture and the stage, then each layer merged, then each
// checked, then the copy to VRAM.
static VR_CODE void vdBuildStep(CtrRenderer* r) {
    int32_t n = vd.count;
    if (vd.step == 0) {
        CtrSurface* s0 = vrSurface(r, vd.layer[0].id);
        if (s0 == nullptr) {
            vdFail(r, vdWhyGone);
            return;
        }
        vd.size = s0->tex.size;
        uint32_t rows = vd.size / (512u * 4u), bytes = rows * VD_COLS * 4u;
        // MIRROR_RESERVE stays free for surfaces. The tiles' copies keep more free (mirror.extra, for a surface that
        // wanted VRAM, and the shrink strip's share), but in these rooms VRAM is already below that, so no copy would
        // be made there; and the 736 KB layers that set mirror.extra find no block that large.
        uint32_t reserve = MIRROR_RESERVE;
        if (reserve + bytes > CtrMemory_vramFreeNow() || !VR_CALL(initSurfaceTex)(&vd.tex, VD_COLS, 512, (int32_t) rows, !CtrVramTouch_emulator())) {
            memset(&vd.tex, 0, sizeof(vd.tex));
            vdFail(r, vdWhyVram);
            return;
        }
        vd.stage = (uint32_t*) linearAlloc(bytes);
        if (vd.stage == nullptr) {
            vdFail(r, vdWhyMem);
            return;
        }
        memset(vd.stage, 0, bytes);
        vd.step = 1;
        return;
    }
    if (2 * n >= vd.step) {
        const VdLayer* l = &vd.layer[(vd.step - 1) % n];
        CtrSurface* s = vrSurface(r, l->id);
        bool same = s != nullptr && vdCandidate(s) && s->version == l->version && s->tex.data == l->data && s->tex.size == vd.size &&
                    s->pattern.cut == l->cut;
        if (same && s->spilled) {
            // Its newest pixels still in its scratch: its texture isn't them.
            const CtrScratch* sc = &r->scratches[s->scratch];
            same = !(sc->pendingStore && sc->owner == l->id) && !s->storeQueued;
        }
        if (!same) {
            vdFail(r, vdWhyGone);
            return;
        }
        GSPGPU_InvalidateDataCache(s->tex.data, s->tex.size);
        if (!(n >= vd.step ? vdMerge(l, s) : vdCheck(l, s))) {
            vdFail(r, vdWhyCheck);
            return;
        }
        vd.step++;
        return;
    }
    uint32_t bytes = vd.size / (512u * 4u) * VD_COLS * 4u;
    GSPGPU_FlushDataCache(vd.stage, bytes);
    C3D_Tex from = vd.tex;
    from.data = vd.stage;
    VR_CALL(queueTextureCopy)(r, &from, &vd.tex, VD_COLS);
    if (vd.freeStage != nullptr) linearFree(vd.freeStage);
    vd.freeStage = vd.stage;
    vd.stage = nullptr;
    vd.freeFrame = r->frame + 3u;
    vd.state = VD_READY;
    vd.readyFrame = r->frame + 1u;
    vd.fails = 0;
    if (!vd.loggedReady) {
        vd.loggedReady = true;
        logInfo(vdFmtReady, (int) n, (unsigned) (bytes / 1024u));
    }
}

// The first tower draw of a frame: the layers noted last frame counted as stable or not, or a build step.
static VR_CODE void vdFrame(CtrRenderer* r) {
    if (vd.state == VD_BUILD) {
        vdBuildStep(r);
        return;
    }
    if (vd.state != VD_IDLE) return;
    vd.stable = vd.changed ? 0u : vd.stable + 1u;
    vd.changed = false;
    if (4u <= vd.stable && 4 <= vd.count && 0 <= (int32_t) (r->frame - vd.retryFrame)) {
        vd.state = VD_BUILD;
        vd.step = 0;
    }
}

/* Cut 4b: the tower's plain layers drawn from copies in VRAM, given back whenever VRAM is wanted.
 *
 * Three of the tower's layers in each room (cut 0, so not cut 3's shared layers) are 360x360 surfaces in 512x512 RGBA8
 * FCRAM textures, about 4.3 ms of the GPU's frame in the climb. Each repeats every 180 columns, so its columns 0-255
 * hold every texel its strips sample, exactly as for cut 3's shared texture: a strip at or past column 180 samples the
 * copy 180 columns to the left (vdRemap). A plain layer drawn unchanged 4 frames running gets a 256x512 RGBA8 copy of
 * its columns 0-255 (368 KB), when VRAM keeps PC_MARGIN free beyond it.
 *
 * How it stays exact.
 *   - The copy is the layer's own texels: built on the CPU into linear memory a few tile rows a frame, each read after
 *     a data cache invalidate, then copied to VRAM by the GX. Kept only if the layer, in all its rows, has at every
 *     column 180-359 the texel 180 columns to the left. Its surface, version and texture are checked at every step.
 *   - The strips' batch is drawn from the copy only as cut 3 draws from the shared texture (vdBatchOk, vdRemap: nearest
 *     filtering, every quad moved, or none), with the layer's own state; only the texture and u change.
 *   - A layer's version or texture changing, or 90 frames without a tower draw, drops its copy (freed 3 frames on).
 *   - Anything that wants VRAM (yieldMirrors, reclaimVram, routed here) takes the copies first: those not drawn in this
 *     frame are freed at once (held until the GPU is past them), those drawn in it 3 frames on, and none is taken again
 *     for PC_PAUSE frames. The layer is then drawn from its FCRAM texture, as before. */
#define PC_MAX 3
#define PC_MARGIN (256u * 1024u) // VRAM left free beyond a copy when it is taken
#define PC_PAUSE 600u            // frames without new copies after VRAM was wanted
#define PC_STEP_ROWS 9           // tile rows checked a frame
#define PC_IDLE 0
#define PC_BUILD 1
#define PC_READY 2
#define PC_BAD 3                 // not periodic: not tried again until the layer changes

typedef struct {
    int32_t id;                  // -1: unused
    uint32_t version;
    const void* data;
    uint8_t state;
    int32_t row;                 // next tile row to check (PC_BUILD)
    uint32_t stable, lastSeen, lastDraw, readyFrame;
    uint32_t* stage;
    C3D_Tex tex;
} PcCopy;

typedef struct {
    PcCopy e[PC_MAX];
    bool busy;                   // any entry, or a free pending
    bool loggedUse;
    uint8_t logs, evictLogs, fails;
    uint32_t pauseUntil, freeFrame;
    void* freeStage[PC_MAX];
    void* freeVram[PC_MAX];
} PcState;
static PcState pc VR_BSS;
static const char pcFmtReady[] VR_RO = "Renderer: tower plain layer %d copied to VRAM (%u KB, %u KB VRAM free after) (logged up to 3 times)\n";
static const char pcFmtUse[] VR_RO = "Renderer: drew tower plain layer strips from a VRAM copy (logged once)\n";
static const char pcFmtFail[] VR_RO = "Renderer: tower plain layer %d not copied: %s, %u KB VRAM free (logged up to 3 times)\n";
static const char pcFmtEvict[] VR_RO = "Renderer: gave up %d tower plain layer copies (%u KB) for %u KB of VRAM wanted (logged up to 3 times)\n";
static const char pcWhyRepeat[] VR_RO = "columns 180-359 differ";

// Whether s can have a plain copy: an unchanged 360x360 surface in a 512x512 RGBA8 texture, not one of cut 3's layers.
static VR_CODE bool pcCandidate(const CtrSurface* s) {
    return !s->recorded && !s->windowed && s->shift == 0 && s->width == 360 && s->height == 360 && s->tex.fmt == GPU_RGBA8 &&
           s->tex.width == 512 && s->tex.height == 512 && s->tex.size >= 512u * 4u * 360u && s->tex.data != nullptr && !vdCandidate(s);
}

// Hands entry i's memory over to be freed once the GPU is past it (3 frames on), and empties it.
static VR_CODE void pcRelease(CtrRenderer* r, int32_t i) {
    PcCopy* e = &pc.e[i];
    if (pc.freeStage[i] != nullptr) linearFree(pc.freeStage[i]);
    if (pc.freeVram[i] != nullptr) cnTowerFree(pc.freeVram[i]);
    pc.freeStage[i] = e->stage;
    pc.freeVram[i] = e->tex.data;
    if (e->stage != nullptr || e->tex.data != nullptr) pc.freeFrame = r->frame + 3u;
    e->stage = nullptr;
    memset(&e->tex, 0, sizeof(e->tex));
    e->state = PC_IDLE;
    e->stable = 0;
}

static VR_CODE void pcDropAll(CtrRenderer* r) {
    for (int32_t i = 0; PC_MAX > i; i++) {
        pcRelease(r, i);
        pc.e[i].id = -1;
    }
}

static VR_CODE void pcFail(CtrRenderer* r, int32_t i, const char* why, bool bad) {
    if (3 > pc.logs) {
        pc.logs++;
        logInfo(pcFmtFail, (int) pc.e[i].id, why, (unsigned) (CtrMemory_vramFreeNow() / 1024u));
    }
    pcRelease(r, i);
    if (bad) pc.e[i].state = PC_BAD;
    else pc.pauseUntil = r->frame + (120u << pc.fails);
    if (!bad && 4 > pc.fails) pc.fails++;
}

// One build step of entry i: the VRAM texture and the stage, then PC_STEP_ROWS tile rows checked and copied, then
// the copy to VRAM.
static VR_CODE void pcBuildStep(CtrRenderer* r, int32_t i) {
    PcCopy* e = &pc.e[i];
    CtrSurface* s = vrSurface(r, e->id);
    bool same = s != nullptr && pcCandidate(s) && s->version == e->version && s->tex.data == e->data;
    if (same && s->spilled) {
        // Its newest pixels still in its scratch: its texture isn't them.
        const CtrScratch* sc = &r->scratches[s->scratch];
        same = !(sc->pendingStore && sc->owner == e->id) && !s->storeQueued;
    }
    if (!same) {
        pcFail(r, i, vdWhyGone, false);
        return;
    }
    uint32_t rows = s->tex.size / (512u * 4u), bytes = rows * VD_COLS * 4u;
    if (e->row < 0) {
        if (bytes + PC_MARGIN > CtrMemory_vramFreeNow() || !VR_CALL(initSurfaceTex)(&e->tex, VD_COLS, 512, (int32_t) rows, !CtrVramTouch_emulator())) {
            memset(&e->tex, 0, sizeof(e->tex));
            pcFail(r, i, vdWhyVram, false);
            return;
        }
        e->stage = (uint32_t*) linearAlloc(bytes);
        if (e->stage == nullptr) {
            pcFail(r, i, vdWhyMem, false);
            return;
        }
        e->row = 0;
        return;
    }
    const uint32_t* src = (const uint32_t*) s->tex.data;
    int32_t tileRows = (int32_t) (rows >> 3), checkRows = (s->height + 7) >> 3;
    if (checkRows > e->row) {
        int32_t end = e->row + PC_STEP_ROWS < checkRows ? e->row + PC_STEP_ROWS : checkRows;
        GSPGPU_InvalidateDataCache(src + (uint32_t) e->row * 512u * 8u, (uint32_t) (end - e->row) * 512u * 8u * 4u);
        for (int32_t tr = e->row; end > tr; tr++) {
            const uint32_t* a = src + (uint32_t) tr * 512u * 8u;
            for (int32_t yb = 0; 8 > yb; yb++) {
                uint32_t my = vdMy[yb];
                for (int32_t x = VD_SHIFT; s->width > x; x++) {
                    int32_t y = x - VD_SHIFT;
                    if (a[(uint32_t) (x >> 3) * 64u + vdMx[x & 7] + my] != a[(uint32_t) (y >> 3) * 64u + vdMx[y & 7] + my]) {
                        pcFail(r, i, pcWhyRepeat, true);
                        return;
                    }
                }
            }
            memcpy(e->stage + (uint32_t) tr * VD_COLS * 8u, a, VD_COLS * 8u * 4u);
        }
        e->row = end;
        return;
    }
    // The stored rows past the surface's (never sampled) as they are, then the copy.
    for (int32_t tr = checkRows; tileRows > tr; tr++) {
        const uint32_t* a = src + (uint32_t) tr * 512u * 8u;
        GSPGPU_InvalidateDataCache(a, VD_COLS * 8u * 4u);
        memcpy(e->stage + (uint32_t) tr * VD_COLS * 8u, a, VD_COLS * 8u * 4u);
    }
    GSPGPU_FlushDataCache(e->stage, bytes);
    C3D_Tex from = e->tex;
    from.data = e->stage;
    VR_CALL(queueTextureCopy)(r, &from, &e->tex, VD_COLS);
    if (pc.freeStage[i] != nullptr) linearFree(pc.freeStage[i]);
    pc.freeStage[i] = e->stage;
    pc.freeFrame = r->frame + 3u;
    e->stage = nullptr;
    e->state = PC_READY;
    e->readyFrame = r->frame + 1u;
    pc.fails = 0;
    if (3 > pc.logs) {
        pc.logs++;
        logInfo(pcFmtReady, (int) e->id, (unsigned) (bytes / 1024u), (unsigned) (CtrMemory_vramFreeNow() / 1024u));
    }
}

// The first tower draw of a frame: entries seen last frame counted as stable, then one build step at most (none while
// cut 3 builds).
static VR_CODE void pcFrame(CtrRenderer* r) {
    int32_t build = -1, start = -1;
    for (int32_t i = 0; PC_MAX > i; i++) {
        PcCopy* e = &pc.e[i];
        if (e->id < 0) continue;
        e->stable = e->lastSeen + 1u == r->frame ? e->stable + 1u : 0u;
        if (e->state == PC_BUILD && build < 0) build = i;
        if (e->state == PC_IDLE && 4u <= e->stable && start < 0) start = i;
    }
    if (vd.state == VD_BUILD) return;
    if (build < 0 && start >= 0 && 0 <= (int32_t) (r->frame - pc.pauseUntil)) {
        pc.e[start].state = PC_BUILD;
        pc.e[start].row = -1;
        build = start;
    }
    if (build >= 0) pcBuildStep(r, build);
}

// A tower draw of surface id not drawn from cut 3's texture: noted, and its copy if one is ready.
static VR_CODE PcCopy* pcUse(CtrRenderer* r, int32_t id, const CtrSurface* s) {
    if (!pcCandidate(s)) return nullptr;
    PcCopy* e = nullptr;
    int32_t slot = -1;
    for (int32_t i = 0; PC_MAX > i && e == nullptr; i++) {
        if (pc.e[i].id == id) e = &pc.e[i];
        else if (pc.e[i].id < 0 && slot < 0) slot = i;
    }
    if (e == nullptr) {
        if (slot < 0) return nullptr;
        e = &pc.e[slot];
        *e = (PcCopy) { .id = id, .version = s->version, .data = s->tex.data, .lastSeen = r->frame };
        pc.busy = true;
        return nullptr;
    }
    if (e->version != s->version || e->data != s->tex.data) {
        pcRelease(r, (int32_t) (e - pc.e));
        e->version = s->version;
        e->data = s->tex.data;
    }
    e->lastSeen = r->frame;
    if (e->state != PC_READY || 0 > (int32_t) (r->frame - e->readyFrame)) return nullptr;
    e->lastDraw = r->frame;
    return e;
}

// From vdHousekeep: frees what the GPU is past; drops the copies once tower draws stop.
static VR_CODE void pcHousekeep(CtrRenderer* r) {
    if (pc.freeFrame != 0 && 0 <= (int32_t) (r->frame - pc.freeFrame)) {
        for (int32_t i = 0; PC_MAX > i; i++) {
            if (pc.freeStage[i] != nullptr) linearFree(pc.freeStage[i]);
            if (pc.freeVram[i] != nullptr) cnTowerFree(pc.freeVram[i]);
            pc.freeStage[i] = nullptr;
            pc.freeVram[i] = nullptr;
        }
        pc.freeFrame = 0;
    }
    bool any = false;
    for (int32_t i = 0; PC_MAX > i; i++) any = any || pc.e[i].id >= 0;
    if (any && r->frame - vd.lastUse > 90u) {
        pcDropAll(r);
        any = false;
    }
    pc.busy = any || pc.freeFrame != 0;
}

// VRAM wanted: every copy goes. Those not drawn in this frame are freed now (vramFree holds them until the GPU is past
// what was handed to it); those drawn in it go 3 frames on. True if any was freed now.
static VR_CODE bool pcEvict(CtrRenderer* r, size_t need) {
    int32_t gone = 0;
    uint32_t bytes = 0;
    bool now = false;
    for (int32_t i = 0; PC_MAX > i; i++) {
        PcCopy* e = &pc.e[i];
        if (e->tex.data == nullptr && e->stage == nullptr) continue;
        gone++;
        bytes += e->tex.size;
        if (r->inFrame && e->lastDraw == r->frame && e->state == PC_READY) {
            pcRelease(r, i);
            continue;
        }
        if (e->stage != nullptr) linearFree(e->stage);
        if (e->tex.data != nullptr) cnTowerFree(e->tex.data);
        e->stage = nullptr;
        memset(&e->tex, 0, sizeof(e->tex));
        e->state = PC_IDLE;
        e->stable = 0;
        now = true;
    }
    if (gone == 0) return false;
    pc.pauseUntil = r->frame + PC_PAUSE;
    if (3 > pc.evictLogs) {
        pc.evictLogs++;
        logInfo(pcFmtEvict, (int) gone, (unsigned) (bytes / 1024u), (unsigned) (need / 1024u));
    }
    return now;
}

// A new renderer (game_change; its address may be the old one's, its content serial starts again) starts afresh.
static VR_CODE void vdOwn(CtrRenderer* r) {
    if (vd.owner == r && r->contentSerial >= vd.serial) {
        vd.serial = r->contentSerial;
        return;
    }
    vdDrop(r);
    pcDropAll(r);
    vd.owner = r;
    vd.serial = r->contentSerial;
    vd.fails = 0;
    vd.retryFrame = r->frame;
    vd.lastUse = r->frame;
    vd.frame = r->frame - 1u;
}

// From clearBoundTarget: frees what the GPU is past, and drops the layers once tower draws stop.
static VR_CODE void vdHousekeep(CtrRenderer* r) {
    vdOwn(r);
    if (vd.freeFrame != 0 && 0 <= (int32_t) (r->frame - vd.freeFrame)) {
        if (vd.freeStage != nullptr) linearFree(vd.freeStage);
        if (vd.freeVram != nullptr) cnTowerFree(vd.freeVram);
        vd.freeStage = nullptr;
        vd.freeVram = nullptr;
        vd.freeFrame = 0;
    }
    if ((vd.state != VD_IDLE || vd.count != 0) && r->frame - vd.lastUse > 90u) vdDrop(r);
    if (pc.busy) pcHousekeep(r);
}
#define VD_HOUSEKEEP(r) ((vd.state != VD_IDLE || vd.count != 0 || vd.freeFrame != 0 || pc.busy) ? vdHousekeep(r) : (void) 0)

// The original tower draw; a shared layer's with its cut set and its strips' batch drawn from the shared texture.
static VR_CODE bool vrTowerDraw(CtrRenderer* r, int32_t surfaceId, CtrSurface* s, int32_t srcLeft, int32_t srcTop, int32_t srcWidth,
                                int32_t srcHeight, const float* px, const float* py, const CtrColor* cs, const TowerUniforms* uni) {
    vdOwn(r);
    if (vd.frame != r->frame) {
        vd.frame = r->frame;
        vdFrame(r);
        if (pc.busy) pcFrame(r);
    }
    vd.lastUse = r->frame;
    const VdLayer* l = nullptr;
    if (vd.state == VD_READY) {
        for (int32_t i = 0; vd.count > i && l == nullptr; i++) {
            if (vd.layer[i].id == surfaceId) l = &vd.layer[i];
        }
        if (l != nullptr && (s->recorded || s->version != l->version || s->tex.data != l->data)) {
            vdDrop(r);
            l = nullptr;
        } else if (0 > (int32_t) (r->frame - vd.readyFrame)) {
            l = nullptr;
        }
    } else {
        vdNote(surfaceId, s);
    }
    PcCopy* plain = l == nullptr ? pcUse(r, surfaceId, s) : nullptr;
    if (l == nullptr && plain == nullptr) return VR_CALL(drawTowerStrips)(r, surfaceId, s, srcLeft, srcTop, srcWidth, srcHeight, px, py, cs, uni);
    vd.layerTex = &s->tex;
    vd.drawTex = plain != nullptr ? &plain->tex : &vd.tex;
    vd.cut = plain != nullptr ? 0 : l->cut;
    vd.flushes = 0;
    vd.active = true;
    bool drawn = VR_CALL(drawTowerStrips)(r, surfaceId, s, srcLeft, srcTop, srcWidth, srcHeight, px, py, cs, uni);
    vd.active = false;
    return drawn;
}

// Whether the pending batch is a shared layer's strips that the shared texture can draw: quads of its texture alone,
// drawn by flushBatch's plain path with its cut stages, nearest filtering and the normal blend.
static VR_CODE bool vdBatchOk(CtrRenderer* r) {
    uint32_t count = r->vertexCount - r->batchStart;
    bool linear = r->filterOverride != 0 ? r->filterOverride == FILTER_LINEAR : r->base.texFilter;
    const BlendFactors* f = &r->base.blendFactors;
    return vd.drawTex != nullptr && (vd.drawTex != &vd.tex || vd.state == VD_READY) && r->batchTex == vd.layerTex && count != 0 && (count & 3u) == 0 && r->batchQuads && r->batchTexCount == 1 &&
           r->batchTexB == nullptr && r->addHold.mode == ADD_HOLD_OFF && r->shrink.batchMode == 0 && r->layer.mode == 0 && !r->shoujoBands &&
           !r->colorMask && !r->palSurfPass && r->crtPass == 0 && !linear && r->towerStrips && !r->plainTexEnv && r->opAlphaCut == 0 &&
           (r->shaderFx == SHADER_FX_TOWER || r->shaderFx == SHADER_FX_NONE) && r->blendEnable && !r->blendOverridden &&
           r->blendMode == bm_normal && f->src == bm_src_alpha && f->dst == bm_inv_src_alpha && f->dstAlpha == bm_inv_src_alpha;
}

// Moves every quad's u onto the shared texture, or none if a quad can't be. One pass (cut 5a): each quad is checked
// and moved while its lines are in the cache, its old u kept aside; a quad that can't be moved puts the earlier ones
// back from there. A second pass read the whole batch again (~650 KB a frame in the climb). Batches of more quads
// than are kept aside are left as they are. The lines of the quad 4 ahead are asked for first (PLD, within the batch):
// 42 us a call down to 32-36 in the climb, 32 to 22 in the flower (New 3DS, flowergpu4 rm2 rows).
#define VD_SAVE_QUADS 2048
static float vdSave[2 * VD_SAVE_QUADS] VR_BSS;
static VR_CODE bool vdRemap(CtrRenderer* r) {
    CtrVertex* v = &r->vertices[r->batchStart];
    uint32_t count = r->vertexCount - r->batchStart;
    if (count > 4u * VD_SAVE_QUADS) return false;
    float width = (float) vd.layerTex->width;
    for (uint32_t i = 0; count > i; i += 4) {
        if (count > i + 16u) {
            const char* ahead = (const char*) &v[i + 16];
            __builtin_prefetch(ahead);
            __builtin_prefetch(ahead + 32);
            __builtin_prefetch(ahead + 64);
        }
        // towerQuad's corners: u0, u1, u1, u0.
        float u0 = v[i].u, u1 = v[i + 1].u;
        float a = u0 * width, b = u1 * width;
        float lo = a < b ? a : b, hi = a < b ? b : a;
        // Shift 0 when the quad ends by column 256; a start left of column 0 (a strip at the cylinder's edge) samples
        // column 0 on both textures (clamp to edge, and vdCheck has their columns 0 equal). Shift 180 when it starts at
        // 180 or beyond and ends by 436.
        if (v[i + 3].u != u0 || v[i + 2].u != u1 || !(lo == lo) ||
            !((float) VD_COLS >= hi || (lo >= (float) VD_SHIFT && (float) (VD_SHIFT + VD_COLS) >= hi))) {
            for (uint32_t k = 0; i > k; k += 4) {
                v[k].u = vdSave[k / 2u];
                v[k + 3].u = vdSave[k / 2u];
                v[k + 1].u = vdSave[k / 2u + 1u];
                v[k + 2].u = vdSave[k / 2u + 1u];
            }
            return false;
        }
        vdSave[i / 2u] = u0;
        vdSave[i / 2u + 1u] = u1;
        float shift = lo >= (float) VD_SHIFT ? (float) VD_SHIFT : 0.0f;
        float ua = (a - shift) * (1.0f / (float) VD_COLS), ub = (b - shift) * (1.0f / (float) VD_COLS);
        v[i].u = ua;
        v[i + 3].u = ua;
        v[i + 1].u = ub;
        v[i + 2].u = ub;
    }
    // Rewritten after flushVertexCache wrote them back (a split while they were made): written back again.
    if (r->vertexFlushed > r->batchStart) r->vertexFlushed = r->batchStart;
    return true;
}
#else
#define VD_HOUSEKEEP(r) ((void) 0)
static VR_CODE bool vrTowerDraw(CtrRenderer* r, int32_t surfaceId, CtrSurface* s, int32_t srcLeft, int32_t srcTop, int32_t srcWidth,
                                int32_t srcHeight, const float* px, const float* py, const CtrColor* cs, const TowerUniforms* uni) {
    bool was = vrInTower;
    vrInTower = true;
    bool drawn = VR_CALL(drawTowerStrips)(r, surfaceId, s, srcLeft, srcTop, srcWidth, srcHeight, px, py, cs, uni);
    vrInTower = was;
    return drawn;
}
#endif

/* Cut 2: a tower face's rows nobody samples are not drawn.
 *
 * The tower object draws each 240x720 face (tower_front, tower_back) whole, unrotated, at _top = cameray() - 60 + y:
 * the view shows about two thirds of its rows. The faces are sampled every frame, so deferTowerDraw (which refuses
 * faces seen lately) leaves their tower draws to be drawn at once, before anything samples them. Here every tower draw
 * deferTowerDraw could put off onto a 240x720 face is put off the same way (face->lazy, the layer's lazyRefs, a new
 * face version), sampled or not. Vine_wrap_drawSurfaceImpl works out, for an unrotated draw of such a face, the face
 * rows the bound target's viewport (and scissor) can show, 2 rows wider either side and aligned to 8; flushLazy, reached
 * through surfaceTexture, then draws the list scissored to those rows, and keeps it on the face behind a band marker. A
 * later draw of the face within those rows needs nothing more. Any other use - a draw of other rows, a copy, a read, a
 * layer changing (flushLazyUsing finds the real layer ids), drawing on the face - reaches flushLazy, which first draws the
 * rows above and below, each scissored, then any draws put off since, and drops the list. Scissoring only leaves
 * fragments out, and each pixel's value depends only on the draws that cover it, in order, so the face ends exactly as
 * drawn at once. A whole clear drops the list, as it drops any; retention keeps it while a repeated clear is put off. */
static int32_t vrBandFace VR_BSS;   // the face a band draw is scissored on, + 1 (0: none)
static int32_t vrBandY0 VR_BSS, vrBandY1 VR_BSS;
static int32_t vrSampleFace VR_BSS; // the face drawSurfaceImpl is drawing, + 1, while it knows the rows shown
static int32_t vrSampleY0 VR_BSS, vrSampleY1 VR_BSS;
static bool vrBandLogged VR_BSS, vrRestLogged VR_BSS;
static const char vrFmtBand[] VR_RO = "Renderer: drew tower face %d rows %d-%d first, the rows its draw shows (logged once)\n";
static const char vrFmtRest[] VR_RO = "Renderer: drew the other rows of tower face %d when it was used otherwise (logged once)\n";

// The tower draw, scissored to the band while one is drawn on this face. A draw drawTowerStrips declines falls back
// to flushLazy's drawSurfaceQuad, which must keep to the band too: the band scissor is left on for it.
static VR_CODE bool vrBandDraw(CtrRenderer* r, int32_t surfaceId, CtrSurface* s, int32_t srcLeft, int32_t srcTop, int32_t srcWidth,
                               int32_t srcHeight, const float* px, const float* py, const CtrColor* cs, const TowerUniforms* uni) {
    CtrSurface* face = vrBandFace != 0 && r->boundSurface == vrBandFace - 1 ? vrSurface(r, r->boundSurface) : nullptr;
    if (face == nullptr) return vrTowerDraw(r, surfaceId, s, srcLeft, srcTop, srcWidth, srcHeight, px, py, cs, uni);
    CtrLazyDraw saved;
    memset(&saved, 0, sizeof(saved));
    vrLazyCapture(r, &saved);
    CtrTargetState band = saved.target;
    int32_t x0 = 0, x1 = face->width, y0 = vrBandY0, y1 = vrBandY1;
    if (band.scissorEnabled) {
        if (band.scissor.x > x0) x0 = band.scissor.x;
        if (band.scissor.y > y0) y0 = band.scissor.y;
        if (band.scissor.x + band.scissor.w < x1) x1 = band.scissor.x + band.scissor.w;
        if (band.scissor.y + band.scissor.h < y1) y1 = band.scissor.y + band.scissor.h;
    }
    if (x0 >= x1 || y0 >= y1) return true; // none of it in these rows
    band.scissorEnabled = true;
    band.scissor = (CtrRect) { x0, y0, x1 - x0, y1 - y0 };
    VR_CALL(restoreState)(r, &band);
    bool drawn = vrTowerDraw(r, surfaceId, s, srcLeft, srcTop, srcWidth, srcHeight, px, py, cs, uni);
    if (drawn) VR_CALL(restoreState)(r, &saved.target);
    return drawn;
}

// flushLazy's drawing of the list on faceId, scissored to rows [y0, y1) (nothing if the list is empty).
static VR_CODE void vrBandRun(CtrRenderer* r, int32_t faceId, CtrLazyDraw* list, int32_t y0, int32_t y1) {
    CtrSurface* face = vrSurface(r, faceId);
    if (face == nullptr || list == nullptr) {
        vrReleaseList(r, &list);
        return;
    }
    face->lazy = list;
    int32_t outer = vrBandFace, outerY0 = vrBandY0, outerY1 = vrBandY1;
    bool was = vrReplaying;
    vrBandFace = faceId + 1;
    vrBandY0 = y0;
    vrBandY1 = y1;
    vrReplaying = true;
    VR_CALL(flushLazy)(r, faceId);
    vrReplaying = was;
    vrBandFace = outer;
    vrBandY0 = outerY0;
    vrBandY1 = outerY1;
}

// A copy of n draws, holding their layers' references.
static VR_CODE CtrLazyDraw* vrCopyDraws(CtrRenderer* r, const CtrLazyDraw* from, int32_t n) {
    if (0 >= n) return nullptr;
    CtrLazyDraw* copy = nullptr;
    arrsetlen(copy, n);
    memcpy(copy, from, (size_t) n * sizeof(*copy));
    for (int32_t i = 0; n > i; i++) {
        CtrSurface* layer = vrSurface(r, copy[i].layer);
        if (layer != nullptr) layer->lazyRefs++;
    }
    return copy;
}

// Drawing a face's put-off draws changes its version but not what the recipe gives: a recording slot that matched
// the version before follows it, so the next repeat of the recipe is still recognised.
static VR_CODE void vrFollowVersion(CtrRenderer* r, int32_t faceId, uint32_t before) {
    VrSlot* slot = vrFind(r, faceId);
    CtrSurface* face = vrSurface(r, faceId);
    if (slot != nullptr && face != nullptr && slot->recording && !slot->deferring && slot->savedVersion == before) {
        slot->savedVersion = face->version;
    }
}

// Draws the rows of a band list's draws it hasn't drawn, then any draws put off after it, and drops it.
static VR_CODE void vrBandComplete(CtrRenderer* r, int32_t faceId) {
    CtrSurface* face = vrSurface(r, faceId);
    if (!vrIsBand(face)) return;
    uint32_t before = face->version;
    CtrLazyDraw* list = face->lazy;
    face->lazy = nullptr;
    int32_t a = list[0].srcTop, b = a + list[0].srcHeight, total = (int32_t) arrlen(list) - 1, n = list[0].srcWidth;
    if (n > total) n = total;
    if (0 < a) vrBandRun(r, faceId, vrCopyDraws(r, list + 1, n), 0, a);
    face = vrSurface(r, faceId);
    if (face != nullptr && face->height > b) vrBandRun(r, faceId, vrCopyDraws(r, list + 1, n), b, face->height);
    face = vrSurface(r, faceId);
    if (face != nullptr && total > n) {
        face->lazy = vrCopyDraws(r, list + 1 + n, total - n);
        VR_CALL(flushLazy)(r, faceId);
    }
    vrReleaseList(r, &list);
    vrFollowVersion(r, faceId, before);
    if (!vrRestLogged) {
        vrRestLogged = true;
        logInfo(vrFmtRest, (int) faceId);
    }
}

// Draws the face's list scissored to rows [y0, y1) and leaves it there as a band list.
static VR_CODE void vrBandFlush(CtrRenderer* r, int32_t faceId, int32_t y0, int32_t y1) {
    CtrSurface* face = vrSurface(r, faceId);
    uint32_t before = face->version;
    CtrLazyDraw* list = face->lazy;
    int32_t n = (int32_t) arrlen(list);
    CtrLazyDraw* band = nullptr;
    arrsetlen(band, n + 1);
    memset(&band[0], 0, sizeof(band[0]));
    band[0].layer = -1;
    band[0].srcLeft = VB_MAGIC;
    band[0].srcTop = y0;
    band[0].srcHeight = y1 - y0;
    band[0].srcWidth = n;
    memcpy(band + 1, list, (size_t) n * sizeof(*band));
    vrBandRun(r, faceId, list, y0, y1);
    face = vrSurface(r, faceId);
    if (face == nullptr || face->lazy != nullptr) {
        arrfree(band); // not drawn (no frame): the list stays as it was
        return;
    }
    for (int32_t i = 1; n >= i; i++) {
        CtrSurface* layer = vrSurface(r, band[i].layer);
        if (layer != nullptr) layer->lazyRefs++;
    }
    face->lazy = band;
    vrFollowVersion(r, faceId, before);
    if (!vrBandLogged) {
        vrBandLogged = true;
        logInfo(vrFmtBand, (int) faceId, (int) y0, (int) y1);
    }
}

// Puts off a tower draw onto a 240x720 face as deferTowerDraw would, sampled lately or not.
static VR_CODE bool vrBandDefer(CtrRenderer* r, int32_t faceId, CtrSurface* face, int32_t layerId, CtrSurface* layer, int32_t srcLeft,
                                int32_t srcTop, int32_t srcWidth, int32_t srcHeight, const float* px, const float* py, const CtrColor* cs,
                                const TowerUniforms* uni) {
    if (face == nullptr || face->width != 240 || face->height != 720 || !vrDrawOk(r, faceId, face, layerId, layer)) return false;
    if (vrIsBand(face)) {
        vrBandComplete(r, faceId);
        face = vrSurface(r, faceId);
        if (face == nullptr) return false;
    }
    if (face->lazy != nullptr && (0 > face->lazy[0].layer || arrlen(face->lazy) >= LAZY_MAX_DRAWS)) return false;
    CtrLazyDraw d;
    memset(&d, 0, sizeof(d));
    d.layer = layerId;
    d.layerVersion = layer->version;
    d.srcLeft = srcLeft;
    d.srcTop = srcTop;
    d.srcWidth = srcWidth;
    d.srcHeight = srcHeight;
    memcpy(d.px, px, sizeof(d.px));
    memcpy(d.py, py, sizeof(d.py));
    memcpy(d.cs, cs, sizeof(d.cs));
    d.uni = *uni;
    vrLazyCapture(r, &d);
    arrput(face->lazy, d);
    layer->lazyRefs++;
    face->version = ++r->contentSerial;
    return true;
}

// drawSurfaceImpl, knowing the rows of a 240x720 face with draws put off that this draw of it can show.
VR_CODE void Vine_wrap_drawSurfaceImpl(CtrRenderer* r, int32_t surfaceId, int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight,
                                       float x, float y, float xscale, float yscale, float angleDeg, const CtrColor* cs) {
    CtrSurface* s = vrSurface(r, surfaceId);
    int32_t outer = vrSampleFace, outerY0 = vrSampleY0, outerY1 = vrSampleY1;
    vrSampleFace = 0;
    if (s != nullptr && s->lazy != nullptr && s->width == 240 && s->height == 720 && angleDeg == 0.0f && r->boundTarget != nullptr &&
        r->inFrame && surfaceId != r->boundSurface) {
        int32_t top = srcTop, height = srcHeight;
        if (0 > srcWidth) top = 0, height = s->height;
        float ignored, ya, yb;
        VR_CALL(worldToPixel)(r, x, y, &ignored, &ya);
        VR_CALL(worldToPixel)(r, x, yscale * (float) height + y, &ignored, &yb);
        float v0 = (float) r->viewport.y, v1 = (float) (r->viewport.y + r->viewport.h);
        if (r->scissorEnabled) {
            if ((float) r->scissor.y > v0) v0 = (float) r->scissor.y;
            if ((float) (r->scissor.y + r->scissor.h) < v1) v1 = (float) (r->scissor.y + r->scissor.h);
        }
        float span = yb - ya;
        if (fabsf(span) >= 1.0f && v1 > v0 && 0 < height) {
            float t0 = (v0 - ya) / span, t1 = (v1 - ya) / span;
            if (t0 > t1) {
                float swap = t0;
                t0 = t1;
                t1 = swap;
            }
            t0 = t0 < 0.0f ? 0.0f : (t0 > 1.0f ? 1.0f : t0);
            t1 = t1 < 0.0f ? 0.0f : (t1 > 1.0f ? 1.0f : t1);
            int32_t r0 = (int32_t) floorf((float) top + t0 * (float) height) - 2, r1 = (int32_t) ceilf((float) top + t1 * (float) height) + 2;
            r0 = r0 < 0 ? 0 : r0 & ~7;
            r1 = (r1 + 7) & ~7;
            if (r1 > s->height) r1 = s->height;
            if (r1 > r0) {
                vrSampleFace = surfaceId + 1;
                vrSampleY0 = r0;
                vrSampleY1 = r1;
            }
        }
    }
    VR_CALL(drawSurfaceImpl)(r, surfaceId, srcLeft, srcTop, srcWidth, srcHeight, x, y, xscale, yscale, angleDeg, cs);
    vrSampleFace = outer;
    vrSampleY0 = outerY0;
    vrSampleY1 = outerY1;
}

VR_CODE void Vine_wrap_clearBoundTarget(CtrRenderer* r, CtrColor color, int32_t targetW, int32_t targetH) {
    VD_HOUSEKEEP(r);
    int32_t id = r->boundSurface;
    if (vrReplaying && vrPend != nullptr && id == vrPendFace) {
        // flushLazy's bindSurface clearing a face that had its clear put off (vrMaterialize): that clear instead.
        VrSlot* pend = vrPend;
        vrPend = nullptr;
        bool kept[4];
        memcpy(kept, r->colorWrite, sizeof(kept));
        memset(r->colorWrite, true, sizeof(r->colorWrite));
        if (r->quadClears) vrApplyColorWrite(r);
        VR_CALL(clearBoundTarget)(r, pend->clearColor, pend->clearW, pend->clearH);
        memcpy(r->colorWrite, kept, sizeof(kept));
        if (r->quadClears) vrApplyColorWrite(r);
        return;
    }
    VrSlot* slot = vrReplaying ? nullptr : vrFind(r, id);
    if (slot == nullptr || !r->inFrame) {
        VR_CALL(clearBoundTarget)(r, color, targetW, targetH);
        return;
    }
    slot->lastFrame = r->frame;
    CtrSurface* face = vrSurface(r, id);
    bool whole = r->colorWrite[0] && r->colorWrite[1] && r->colorWrite[2] && r->colorWrite[3];
    VrIdentity now;
    memset(&now, 0, sizeof(now));
    if (face != nullptr) vrIdentityOf(face, &now);
    bool same = whole && vrFaceOk(r, id, face) && slot->recording && slot->count > 0 && face->version == slot->savedVersion &&
                colorsEqual(color, slot->clearColor) && targetW == slot->clearW && targetH == slot->clearH &&
                memcmp(&now, &slot->faceId, sizeof(now)) == 0;
    if (slot->deferring) {
        if (same && vrMarkerOk(face, slot) && (size_t) arrlen(face->lazy) == (size_t) slot->idx + 1) {
            // Nothing showed what was put off, so the pixels still hold the whole recipe: compare again from the start.
            for (ptrdiff_t k = 1; arrlen(face->lazy) > k; k++) {
                CtrSurface* layer = vrSurface(r, face->lazy[k].layer);
                if (layer != nullptr && layer->lazyRefs != 0) layer->lazyRefs--;
            }
            arrsetlen(face->lazy, 1);
            slot->idx = 0;
            face->version = ++r->contentSerial;
            slot->savedVersion = face->version;
            face->skipLive = false;
            r->clearedTarget = nullptr;
            return;
        }
        if (vrMarkerOk(face, slot)) {
            if (whole && r->boundTarget != nullptr) VR_CALL(dropLazy)(r, face); // all of it is cleared away now
            else vrMaterialize(r, id, slot);
        }
        vrReleaseList(r, &slot->stash);
        vrReset(slot);
        same = false;
    }
    if (same && (face->lazy == nullptr || vrIsBand(face))) {
        VR_CALL(flushBatch)(r);
        // The face's band list stands for its pixels: kept aside, it is put back if the whole recipe repeats.
        vrReleaseList(r, &slot->stash);
        slot->stash = face->lazy;
        face->lazy = nullptr;
        CtrLazyDraw marker;
        memset(&marker, 0, sizeof(marker));
        marker.layer = -1;
        marker.srcLeft = VR_MAGIC;
        marker.srcTop = (int32_t) (slot - vrSlots);
        arrput(face->lazy, marker);
        face->version = ++r->contentSerial;
        slot->savedVersion = face->version;
        face->skipLive = false;
        r->clearedTarget = nullptr;
        slot->deferring = true;
        slot->idx = 0;
        return;
    }
    VR_CALL(clearBoundTarget)(r, color, targetW, targetH);
    face = vrSurface(r, id);
    if (face != nullptr && whole && vrFaceOk(r, id, face) && face->lazy == nullptr) {
        slot->recording = true;
        slot->count = 0;
        slot->clearColor = color;
        slot->clearW = targetW;
        slot->clearH = targetH;
        vrIdentityOf(face, &slot->faceId);
        slot->savedVersion = face->version;
    } else {
        vrReset(slot);
    }
}

static VR_CODE void dqInstall(CtrRenderer* r);
VR_CODE bool Vine_wrap_drawTowerStrips(CtrRenderer* r, int32_t surfaceId, CtrSurface* s, int32_t srcLeft, int32_t srcTop,
                                       int32_t srcWidth, int32_t srcHeight, const float* px, const float* py,
                                       const CtrColor* cs, const TowerUniforms* uni) {
    dqInstall(r);
    if (vrReplaying || !r->inFrame) return vrBandDraw(r, surfaceId, s, srcLeft, srcTop, srcWidth, srcHeight, px, py, cs, uni);
    int32_t faceId = r->boundSurface;
    VrSlot* slot = vrFind(r, faceId);
    CtrSurface* face = vrSurface(r, faceId);
    if (slot == nullptr) {
        if (!vrDisabled && vrDrawOk(r, faceId, face, surfaceId, s)) vrAlloc(r, faceId);
        if (vrBandDefer(r, faceId, face, surfaceId, s, srcLeft, srcTop, srcWidth, srcHeight, px, py, cs, uni)) return true;
        return vrBandDraw(r, surfaceId, s, srcLeft, srcTop, srcWidth, srcHeight, px, py, cs, uni);
    }
    slot->lastFrame = r->frame;
    bool ok = vrDrawOk(r, faceId, face, surfaceId, s);
    CtrLazyDraw sig;
    memset(&sig, 0, sizeof(sig)); // so that only a changed field, or padding, tells two apart
    VrIdentity source;
    memset(&source, 0, sizeof(source));
    if (ok) {
        sig.layer = surfaceId;
        sig.layerVersion = s->version;
        sig.srcLeft = srcLeft;
        sig.srcTop = srcTop;
        sig.srcWidth = srcWidth;
        sig.srcHeight = srcHeight;
        memcpy(sig.px, px, sizeof(sig.px));
        memcpy(sig.py, py, sizeof(sig.py));
        memcpy(sig.cs, cs, sizeof(sig.cs));
        sig.uni = *uni;
        vrLazyCapture(r, &sig);
        vrIdentityOf(s, &source);
    }
    if (slot->deferring) {
        uint32_t i = slot->idx;
        if (ok && vrMarkerOk(face, slot) && slot->count > i && face->version == slot->savedVersion &&
            (size_t) arrlen(face->lazy) == (size_t) i + 1 && memcmp(&sig, &slot->sigs[i], sizeof(sig)) == 0 &&
            memcmp(&source, &slot->sources[i], sizeof(source)) == 0) {
            arrput(face->lazy, sig);
            s->lazyRefs++;
            face->version = ++r->contentSerial; // as deferTowerDraw
            slot->savedVersion = face->version;
            slot->idx = i + 1;
            if (slot->idx == slot->count) {
                // The whole recipe repeated: the face already holds what it gives (its band list, if it had one).
                VR_CALL(dropLazy)(r, face);
                face->lazy = slot->stash;
                slot->stash = nullptr;
                slot->deferring = false;
                slot->savedVersion = face->version;
                if (!vrLogged) {
                    vrLogged = true;
                    logInfo(vrFmtKept, (unsigned) slot->count);
                }
            }
            return true;
        }
        if (vrMarkerOk(face, slot)) vrMaterialize(r, faceId, slot);
        else vrReleaseList(r, &slot->stash), vrReset(slot);
        face = vrSurface(r, faceId);
    }
    if (!slot->recording) {
        if (vrBandDefer(r, faceId, face, surfaceId, s, srcLeft, srcTop, srcWidth, srcHeight, px, py, cs, uni)) return true;
        return vrBandDraw(r, surfaceId, s, srcLeft, srcTop, srcWidth, srcHeight, px, py, cs, uni);
    }
    bool keep = ok && face->version == slot->savedVersion && VR_CAP > slot->count;
    if (vrBandDefer(r, faceId, face, surfaceId, s, srcLeft, srcTop, srcWidth, srcHeight, px, py, cs, uni)) {
        // Put off, it will be drawn exactly as now: the recipe goes on.
        face = vrSurface(r, faceId);
        if (keep && face != nullptr) {
            slot->sigs[slot->count] = sig;
            slot->sources[slot->count] = source;
            slot->count++;
            slot->savedVersion = face->version;
        } else {
            vrReset(slot);
        }
        return true;
    }
    bool drawn = vrBandDraw(r, surfaceId, s, srcLeft, srcTop, srcWidth, srcHeight, px, py, cs, uni);
    face = vrSurface(r, faceId);
    if (keep && drawn && face != nullptr && face->lazy == nullptr) {
        slot->sigs[slot->count] = sig;
        slot->sources[slot->count] = source;
        slot->count++;
        slot->savedVersion = face->version;
    } else {
        vrReset(slot);
    }
    return drawn;
}

// drawTowerStrips' own flushBatch calls (cut 3, routed by tools/analysis/vine_old_parent.py): the second draws its
// strips, from the shared depth texture when vdBatchOk and vdRemap allow; any other is flushBatch as it was.
VR_CODE void Vine_tower_flushBatch(CtrRenderer* r) {
#ifndef CTRPORT_FLOWERGPU_VRAMPROBE
    if (vd.active && vd.flushes++ == 1 && vdBatchOk(r) && vdRemap(r)) {
        C3D_Tex* layer = r->batchTex;
        C3D_Tex* to = vd.drawTex;
        to->param = layer->param;
        to->border = layer->border;
        to->lodParam = layer->lodParam;
        r->batchTex = to;
        r->batchTexes[0] = to;
        r->opAlphaCut = vd.cut;
        VR_CALL(applyTexEnv)(r);
        VR_CALL(flushBatch)(r);
        r->opAlphaCut = 0;
        VR_CALL(applyTexEnv)(r);
        r->batchTex = layer;
        r->batchTexes[0] = layer;
        if (to != &vd.tex) {
            if (!pc.loggedUse) {
                pc.loggedUse = true;
                logInfo(pcFmtUse);
            }
        } else if (!vd.loggedUse) {
            vd.loggedUse = true;
            logInfo(vdFmtUse);
        }
        return;
    }
#endif
    VR_CALL(flushBatch)(r);
}

VR_CODE void Vine_wrap_flushLazy(CtrRenderer* r, int32_t faceId) {
    if (r->inFrame) {
        CtrSurface* face = vrSurface(r, faceId);
        if (face != nullptr && face->lazy != nullptr && face->lazy[0].layer == -1 && face->lazy[0].srcLeft == VR_MAGIC) {
            int32_t k = face->lazy[0].srcTop;
            VrSlot* slot = 0 <= k && VR_SLOTS > k ? &vrSlots[k] : nullptr;
            if (slot != nullptr && slot->used && slot->face == faceId && slot->deferring) {
                vrMaterialize(r, faceId, slot);
                return;
            }
        }
        if (vrIsBand(face)) {
            int32_t a = face->lazy[0].srcTop, b = a + face->lazy[0].srcHeight;
            if (vrSampleFace == faceId + 1 && vrSampleY0 >= a && b >= vrSampleY1) return; // those rows are drawn
            vrBandComplete(r, faceId);
            return;
        }
        if (face != nullptr && face->lazy != nullptr && vrSampleFace == faceId + 1 && !vrReplaying && face->width == 240 &&
            face->height == 720 && 0 <= face->lazy[0].layer) {
            vrBandFlush(r, faceId, vrSampleY0, vrSampleY1);
            return;
        }
    }
    VR_CALL(flushLazy)(r, faceId);
}

/* Cut 5b: the vertex ring wraps without waiting for the GPU when nothing it takes can still be read.
 *
 * makeVertexRoom (inlined into reserveBatch) wraps the ring mid-frame: when this frame's vertices began past the room
 * the draw needs, it waits for the GPU to go idle and takes [0, frameVertexStart). The GPU is then still drawing the
 * last frame (it gets a frame only at its end), so the flower climb waited ~1 ms a frame on average, ~2.5 ms on the
 * frames that wrapped. Only the last frame's vertices, [its start, this frame's start), can still be in use once every
 * frame handed to the GPU before the last one began is done: CtrGpu_completions has reached the CtrGpu_completionDue
 * of then. So when the last frame took no wrap, that count is reached and the draw fits below the last frame's start,
 * the wrap takes [0, the last frame's start) at once: frameVertexStart, which the caller copies into vertexLimit, is
 * lowered to it. Cut 5c: the count is CtrGpu_drawCompletions, which counts a frame whose draws are done though its
 * screen transfers wait for their vblank (CtrGpu_completions waited for that vblank too: 82 of 339 wraps still waited
 * in dbg13); and when that count is all that is missing, the wrap waits for those draws alone (40 ms at most, then
 * the full wait), not for the last frame as well. A frame that then needs more wraps again, which runs the frame so far (the existing path, as for a
 * frame running past its own start). Anything not proven waits as before. The frames are told apart at their first
 * draw (reserveBatch, routed here), before any wrap of theirs. */
typedef struct {
    CtrRenderer* r;
    uint32_t frame, start, due;     // the frame being drawn: its number, first vertex, completionDue at its first draw
    uint32_t prevStart, prevDue;    // the same of the last frame
    uint32_t need;                  // the vertices the draw being reserved takes, with the quads' padding (0: none)
    uint32_t skipped, waited, slept;
    bool wrapped, prevOk;           // this frame took a wrap; the last frame's vertices were [prevStart, start)
} VrRing;
static VrRing vrRing VR_BSS;
static const char vrRingFmt[] VR_RO = "Renderer: vertex ring wrapped %u times without waiting for the whole GPU (%u of them for earlier frames' draws), %u waited\n";

VR_CODE CtrVertex* Vine_wrap_reserveBatch(CtrRenderer* r, C3D_Tex* tex, uint32_t count, bool multiSafe, uint32_t quads) {
    VrRing* g = &vrRing;
    if (g->frame != r->frame || g->r != r) {
        g->prevOk = g->r == r && g->frame + 1u == r->frame && !g->wrapped && r->frameVertexStart > g->start;
        g->prevStart = g->start;
        g->prevDue = g->due;
        g->r = r;
        g->frame = r->frame;
        g->start = r->frameVertexStart;
        // With the GPU idle everything handed to it is done already (only this thread hands it work).
        g->due = CtrGpu_busy() ? CtrGpu_completionDue() : CtrGpu_completions();
        g->wrapped = false;
    }
    uint32_t saved = g->need;
    g->need = (4u * quads > count ? 4u * quads : count) + 3u;
    CtrVertex* v = VR_CALL(reserveBatch)(r, tex, count, multiSafe, quads);
    g->need = saved;
    return v;
}

// reserveBatch's two waits for the GPU, both in makeVertexRoom: the wrap above, and the one outside a frame.
VR_CODE void Vine_ring_waitIdle(void) {
    VrRing* g = &vrRing;
    CtrRenderer* r = g->r;
    bool ok = r != nullptr && g->prevOk && !g->wrapped && r->inFrame && r->frame == g->frame && r->vertexLimit == VERTEX_CAPACITY &&
              r->frameVertexStart == g->start && g->need != 0 && g->prevStart >= g->need;
    g->wrapped = true;
    if (ok && 0 > (int32_t) (CtrGpu_drawCompletions() - g->prevDue)) {
        uint64_t t0 = svcGetSystemTick();
        do {
            svcSleepThread(100 * 1000);
            ok = 0 <= (int32_t) (CtrGpu_drawCompletions() - g->prevDue);
        } while (!ok && SYSCLOCK_ARM11 / 25u > svcGetSystemTick() - t0);
        if (ok) g->slept++;
    }
    if (ok) {
        r->frameVertexStart = g->prevStart;
        if ((++g->skipped & 63u) == 1u) logInfo(vrRingFmt, (unsigned) g->skipped, (unsigned) g->slept, (unsigned) g->waited);
        return;
    }
    g->waited++;
    CtrGpu_waitIdle();
}

// makeVertexRoom's other wrap, which runs the frame so far.
VR_CODE void Vine_ring_endFrameEarly(CtrRenderer* r, CtrSuspendedFrame* f) {
    vrRing.wrapped = true;
    VR_CALL(endFrameEarly)(r, f);
}

#ifndef CTRPORT_FLOWERGPU_VRAMPROBE
// Cut 4b: anything wanting VRAM takes the tower's plain layer copies first (see pcEvict).
VR_CODE bool Vine_wrap_yieldMirrors(CtrRenderer* r, size_t need) {
    if (pc.busy && pcEvict(r, need)) return true;
    return VR_CALL(yieldMirrors)(r, need);
}

VR_CODE bool Vine_wrap_reclaimVram(CtrRenderer* r, size_t need) {
    bool any = pc.busy && pcEvict(r, need);
    return VR_CALL(reclaimVram)(r, need) || any;
}
#endif

/* ===[ Tower decorations drawn in runs (flowergpu4) ]===
 * Each decoration instance on the tower draws its fill (one of two sprites), then sets the outline shader, draws its
 * outline (one of two more sprites) and resets the shader: about 45 shader changes a frame in the climb, each a flush
 * and a texture combiner rebuild. Here a run of consecutive decoration draws is held and drawn as all its fills, one
 * shader change, all its outlines. Fills keep their order and outlines keep theirs; a fill only moves ahead of the
 * outlines held before it, so the pixels are the same unless such a fill and outline touch a pixel. A fill whose
 * bounds meet a held outline's draws the run first. The bounds are the quad as ctrDrawSprite places it, padded by
 * DQ_PAD_PX target pixels for what can move or grow the emitted quad afterwards (the pixel grid, native spans, edge
 * fitting: under 4 together).
 *  - Anything else that reaches the vtable draws the run first: every other entry is an 8-byte stub that tail-jumps
 *    to the entry it replaced, after drawing the run if one is held.
 *  - What a draw reads besides its vtable call is kept per draw and put back while the run is drawn: the instance's
 *    depth (3D disparity), the deep skip's muting, and the instance and event that the dialogue redirect and the frame
 *    skip look at. A run holds draws of one 3D disparity, so both eyes see the bounds equally far apart.
 *  - setSkipWrappers swaps entries of the table it finds on top (this one, which holds none of its functions), so the
 *    table this one replaced is brought along here: the stubs and wrappers look at the skip state on every call.
 *  - The sprites and the shader are found by their names' FNV-1a hashes, at each renderer's first tower draw. The
 *    renderer's destroy (game_change, exit) forgets it, so the next renderer, at whatever address, looks again. */
#define DQ_NV 82
#define DQ_CAP 96
#define DQ_SPAN 1024
#define DQ_PAD_PX 4.0f
#define DQ_STR2(x) #x
#define DQ_STR(x) DQ_STR2(x)
_Static_assert(sizeof(RendererVtable) == DQ_NV * sizeof(void*), "decoration runs: RendererVtable is not DQ_NV pointers");

typedef struct {
    int32_t tpag;
    float x, y, ox, oy, xs, ys, ang, alpha;
    uint32_t color;
    float x0, y0, x1, y1; // the quad's bounds, padded (drawing units)
    struct Instance* inst;
    int32_t evObj, evType, depth;
    uint8_t rim, muted;
} DqDraw;

typedef void (*DqFn)(void);
RendererVtable Vine_dqPrev VR_BSS __attribute__((used));
// [0]: a run is held; [1]: the skip wrappers' state Vine_dqPrev was last brought to. The stubs read both as a halfword.
volatile uint8_t Vine_dqState[4] VR_BSS __attribute__((used, aligned(4)));
const bool* Vine_dqWrappedAt VR_BSS __attribute__((used)); // &r->skip.wrapped
static struct {
    CtrRenderer* r;
    RendererVtable outer;
    int32_t tlo, thi, rimShader;
    uint8_t kind[DQ_SPAN]; // by tpag - tlo: 1 fill, 2 outline
    DqDraw d[DQ_CAP];
    int n, logShader;
    float disp;
#ifdef CTRPORT_FLOWERGPU_DQSTATS
    uint32_t runs, brOverlap, brDisp, brCap, deferred, lastLog;
#endif
#ifdef CTRPORT_FLOWERGPU_DQCHECK
    float ex[DQ_CAP][4], kx, ky, maxEx;
    uint8_t exOk[DQ_CAP];
    uint32_t pairs, viol, unchecked, checked;
#endif
} dq VR_BSS;
static const uint32_t dqHash[5] VR_RO = { 0xa8c598afu, 0x047fe2c0u, 0x7b925370u, 0xb732e4b3u, 0x59cd47d7u }; // 2 fills, 2 outlines, shader
static const int32_t dqGuess[5] VR_RO = { 8388, 772, 6053, 5594, 30 };                                    // where they are in v0.0.253
static const struct {
    uint16_t at;
    DqFn normal, skipped;
} dqSkip[] VR_RO = {
#define DQ_SKIP(field, normal, skipped) { (uint16_t) offsetof(RendererVtable, field), (DqFn) normal, (DqFn) skipped },
    SKIP_WRAPPERS(DQ_SKIP)
#undef DQ_SKIP
};
static const char dqFmtOn[] VR_RO = "Renderer: tower decorations drawn in runs (tpag %d-%d, shader %d)\n";
#ifdef CTRPORT_FLOWERGPU_DQSTATS
static const char dqFmtStat[] VR_RO = "DQ4 f%u runs %u breaks overlap %u disp %u cap %u deferred %u (per 120 frames)\n";
#endif
#ifdef CTRPORT_FLOWERGPU_DQCHECK
static const char dqFmtChk[] VR_RO = "DQCHK f%u pairs %u violations %u checked %u unchecked %u max excess %d/100 px\n";
#endif

static VR_CODE uint32_t dqFnv(const char* s) {
    uint32_t h = 0x811c9dc5u;
    if (s != nullptr)
        for (; *s != '\0'; s++) h = (h ^ (uint8_t) *s) * 0x01000193u;
    return h;
}

static VR_CODE int32_t dqSpriteIndex(const DataWin* dw, int which) {
    uint32_t g = (uint32_t) dqGuess[which];
    if (dw->sprt.count > g && dqFnv(dw->sprt.sprites[g].name) == dqHash[which]) return (int32_t) g;
    for (uint32_t i = 0; dw->sprt.count > i; i++)
        if (dqFnv(dw->sprt.sprites[i].name) == dqHash[which]) return (int32_t) i;
    return -1;
}

static VR_CODE int32_t dqShaderIndex(const DataWin* dw) {
    uint32_t g = (uint32_t) dqGuess[4];
    if (dw->shdr.count > g && dqFnv(dw->shdr.shaders[g].name) == dqHash[4]) return (int32_t) g;
    for (uint32_t i = 0; dw->shdr.count > i; i++)
        if (dqFnv(dw->shdr.shaders[i].name) == dqHash[4]) return (int32_t) i;
    return -1;
}

// Brings Vine_dqPrev to skip state on, as setSkipWrappers would have brought it on top.
static VR_CODE void dqPropagate(bool on) {
    for (size_t i = 0; sizeof(dqSkip) / sizeof(dqSkip[0]) > i; i++) {
        DqFn e, from = on ? dqSkip[i].normal : dqSkip[i].skipped, to = on ? dqSkip[i].skipped : dqSkip[i].normal;
        memcpy(&e, (const char*) &Vine_dqPrev + dqSkip[i].at, sizeof(e));
        if (e == from) memcpy((char*) &Vine_dqPrev + dqSkip[i].at, &to, sizeof(to));
    }
}

static inline __attribute__((always_inline)) void dqContext(Renderer* rr, const DqDraw* e) {
    Runner* run = rr->runner;
    run->drawDepth = e->depth;
    rr->drawMuted = e->muted != 0;
    if (run->vmContext != nullptr) {
        run->vmContext->currentInstance = e->inst;
        run->vmContext->currentEventObjectIndex = e->evObj;
        run->vmContext->currentEventType = e->evType;
    }
}

#ifdef CTRPORT_FLOWERGPU_DQCHECK
// Test: the emitted vertices' bounds of draw i, from vertex c0 on (unchecked if the vertex ring wrapped meanwhile),
// and how far they reach past the unpadded bounds, in target pixels.
static VR_CODE void dqCheckDraw(int i, uint32_t c0) {
    CtrRenderer* r = dq.r;
    dq.exOk[i] = 0;
    if (c0 >= r->vertexCount) return;
    float b[4] = { 1e30f, 1e30f, -1e30f, -1e30f };
    for (uint32_t k = c0; r->vertexCount > k; k++) {
        const CtrVertex* v = &r->vertices[k];
        b[0] = v->x < b[0] ? v->x : b[0];
        b[1] = v->y < b[1] ? v->y : b[1];
        b[2] = v->x > b[2] ? v->x : b[2];
        b[3] = v->y > b[3] ? v->y : b[3];
    }
    memcpy(dq.ex[i], b, sizeof(b));
    dq.exOk[i] = 1;
    dq.checked++;
    const DqDraw* e = &dq.d[i];
    float px = DQ_PAD_PX / dq.kx, py = DQ_PAD_PX / dq.ky;
    float a = ((e->x0 + px) - b[0]) * dq.kx, c = (b[2] - (e->x1 - px)) * dq.kx;
    float d = ((e->y0 + py) - b[1]) * dq.ky, f = (b[3] - (e->y1 - py)) * dq.ky;
    float m = maxF(maxF(a, c), maxF(d, f));
    if (m > dq.maxEx) dq.maxEx = m;
}

// Test: every outline held before a later fill of the run; their emitted bounds must not meet.
static VR_CODE void dqCheckRun(void) {
    for (int i = 0; dq.n > i; i++) {
        if (!dq.d[i].rim) continue;
        for (int j = i + 1; dq.n > j; j++) {
            if (dq.d[j].rim) continue;
            if (!dq.exOk[i] || !dq.exOk[j]) {
                dq.unchecked++;
                continue;
            }
            dq.pairs++;
            if (dq.ex[j][0] <= dq.ex[i][2] && dq.ex[i][0] <= dq.ex[j][2] && dq.ex[j][1] <= dq.ex[i][3] && dq.ex[i][1] <= dq.ex[j][3])
                dq.viol++;
        }
    }
}
#endif

// Draws the held run: its fills, then (if any) the outline shader and its outlines, each in its own context.
VR_CODE void Vine_dqFlush(void) {
    if (!Vine_dqState[0]) return;
    Vine_dqState[0] = 0;
    Renderer* rr = &dq.r->base;
    Runner* run = rr->runner;
    VMContext* vm = run->vmContext;
    DqDraw saved;
    saved.depth = run->drawDepth;
    saved.muted = (uint8_t) rr->drawMuted;
    saved.inst = vm != nullptr ? vm->currentInstance : nullptr;
    saved.evObj = vm != nullptr ? vm->currentEventObjectIndex : -1;
    saved.evType = vm != nullptr ? vm->currentEventType : -1;
    bool rims = false;
    for (int pass = 0; 2 > pass; pass++) {
        for (int i = 0; dq.n > i; i++) {
            DqDraw* e = &dq.d[i];
            if (e->rim != pass) continue;
            dqContext(rr, e);
            if (pass == 1 && !rims) {
                rims = true;
                Vine_dqPrev.gpuSetShader(rr, dq.rimShader);
            }
#ifdef CTRPORT_FLOWERGPU_DQCHECK
            uint32_t c0 = dq.r->vertexCount;
#endif
            Vine_dqPrev.drawSprite(rr, e->tpag, e->x, e->y, e->ox, e->oy, e->xs, e->ys, e->ang, e->color, e->alpha);
#ifdef CTRPORT_FLOWERGPU_DQCHECK
            dqCheckDraw(i, c0);
#endif
        }
    }
    if (rims && dq.logShader != dq.rimShader) Vine_dqPrev.gpuResetShader(rr);
    if (!rims && dq.logShader == dq.rimShader) Vine_dqPrev.gpuSetShader(rr, dq.rimShader);
    rr->currentShader = dq.logShader;
    dqContext(rr, &saved);
#ifdef CTRPORT_FLOWERGPU_DQCHECK
    dqCheckRun();
#endif
    dq.n = 0;
}

// The stubs' slow way and the wrappers' first step when due: the run drawn, then the skip state brought over.
VR_CODE void Vine_dqSync(void) {
    Vine_dqFlush();
    uint8_t w = (uint8_t) *Vine_dqWrappedAt;
    if (w != Vine_dqState[1]) {
        dqPropagate(w != 0);
        Vine_dqState[1] = w;
    }
}

static inline __attribute__((always_inline)) bool dqStale(void) {
    return (uint8_t) *Vine_dqWrappedAt != Vine_dqState[1];
}

// Entry i: "mov ip, #i; b Vine_dqCommon" (8 bytes each, contiguous).
__attribute__((naked, used, target("arm"), section(".text.late.flowergpu"))) void Vine_dqStubs(void) {
    __asm__(".set dqi, 0\n\t.rept " DQ_STR(DQ_NV) "\n\tmov ip, #dqi\n\tb Vine_dqCommon\n\t.set dqi, dqi + 1\n\t.endr\n");
}

// Goes on to Vine_dqPrev's entry ip with r0-r3, d0-d7 and the stack as the caller left them. First, when either is
// due, draws the held run and brings the skip state over: the halfword is wrapped << 8 when no run is held and the
// skip state is the one last brought over.
__attribute__((naked, used, target("arm"), section(".text.late.flowergpu"))) void Vine_dqCommon(void) {
    __asm__("push {r0, r1}\n\t"
            "ldr r0, .Ldq_wat\n\t"
            "ldr r0, [r0]\n\t"
            "ldrb r1, [r0]\n\t"
            "ldr r0, .Ldq_state\n\t"
            "ldrh r0, [r0]\n\t"
            "cmp r0, r1, lsl #8\n\t"
            "bne .Ldq_slow\n"
            ".Ldq_go:\n\t"
            "ldr r0, .Ldq_prev\n\t"
            "ldr ip, [r0, ip, lsl #2]\n\t"
            "pop {r0, r1}\n\t"
            "bx ip\n"
            ".Ldq_slow:\n\t"
            "pop {r0, r1}\n\t"
            "push {r0, r1, r2, r3, ip, lr}\n\t"
            "vpush {d0-d7}\n\t"
            "bl Vine_dqSync\n\t"
            "vpop {d0-d7}\n\t"
            "pop {r0, r1, r2, r3, ip, lr}\n\t"
            "push {r0, r1}\n\t"
            "b .Ldq_go\n"
            ".Ldq_wat: .word Vine_dqWrappedAt\n"
            ".Ldq_state: .word Vine_dqState\n"
            ".Ldq_prev: .word Vine_dqPrev\n");
}

// Whether the decoration draw (k: 1 fill, 2 outline) was held.
static VR_CODE bool dqHold(Renderer* renderer, int k, int32_t tpag, float x, float y, float ox, float oy, float xs, float ys, float ang,
                           uint32_t color, float alpha) {
    CtrRenderer* r = dq.r;
    if (renderer != &r->base) return false;
    int sh = Vine_dqState[0] ? dq.logShader : renderer->currentShader;
    if (k == 1 ? sh != -1 : sh != dq.rimShader) return false;
    // Drawing units to target pixels (an upright projection only), as updateGridMap finds them.
    const C3D_Mtx* m = &r->projection;
    if (m->r[0].y != 0.0f || m->r[1].x != 0.0f) return false;
    float kx = fabsf(m->r[0].x * 0.5f * (float) r->viewport.w), ky = fabsf(m->r[1].y * 0.5f * (float) r->viewport.h);
    if (!(kx > 1.0f / 64.0f && ky > 1.0f / 64.0f)) return false;
    const TexturePageItem* it = &renderer->dataWin->tpag.items[tpag];
    float lx0 = (float) it->targetX - ox, ly0 = (float) it->targetY - oy;
    float lx1 = lx0 + (float) it->targetWidth, ly1 = ly0 + (float) it->targetHeight;
    Matrix4f t;
    setDrawTransform(&t, x, y, xs, ys, ang);
    float px[4], py[4];
    transformPoint(&t, lx0, ly0, &px[0], &py[0]);
    transformPoint(&t, lx1, ly0, &px[1], &py[1]);
    transformPoint(&t, lx1, ly1, &px[2], &py[2]);
    transformPoint(&t, lx0, ly1, &px[3], &py[3]);
    // A NaN corner is drawn as it comes (minF and maxF would drop it).
    if (!(px[0] == px[0] && px[1] == px[1] && px[2] == px[2] && px[3] == px[3] && py[0] == py[0] && py[1] == py[1] && py[2] == py[2] &&
          py[3] == py[3]))
        return false;
    float bx0 = minF(minF(px[0], px[1]), minF(px[2], px[3])) - DQ_PAD_PX / kx;
    float bx1 = maxF(maxF(px[0], px[1]), maxF(px[2], px[3])) + DQ_PAD_PX / kx;
    float by0 = minF(minF(py[0], py[1]), minF(py[2], py[3])) - DQ_PAD_PX / ky;
    float by1 = maxF(maxF(py[0], py[1]), maxF(py[2], py[3])) + DQ_PAD_PX / ky;
    if (!(bx1 - bx0 < 1.0e6f && by1 - by0 < 1.0e6f)) return false; // infinite
    float disp = depthDisparity(r);
    if (Vine_dqState[0]) {
        bool cap = dq.n == DQ_CAP, dispChange = disp != dq.disp, overlap = false;
        if (!cap && !dispChange && k == 1) {
            for (int i = 0; dq.n > i; i++) {
                const DqDraw* o = &dq.d[i];
                if (o->rim && bx0 <= o->x1 && o->x0 <= bx1 && by0 <= o->y1 && o->y0 <= by1) {
                    overlap = true;
                    break;
                }
            }
        }
        if (cap || dispChange || overlap) {
#ifdef CTRPORT_FLOWERGPU_DQSTATS
            dq.brCap += cap;
            dq.brDisp += dispChange && !cap;
            dq.brOverlap += overlap;
#endif
            Vine_dqFlush();
        }
    }
    if (!Vine_dqState[0]) {
        // A run starts at a fill drawn plainly: no shader, the normal blend, the game's projection.
        if (k != 1 || r->blendMode != bm_normal || !r->blendEnable || r->blendOverridden || !r->gameProjection) return false;
        Vine_dqState[0] = 1;
        dq.logShader = -1;
        dq.disp = disp;
#ifdef CTRPORT_FLOWERGPU_DQSTATS
        dq.runs++;
#endif
#ifdef CTRPORT_FLOWERGPU_DQCHECK
        dq.kx = kx;
        dq.ky = ky;
#endif
    }
    DqDraw* e = &dq.d[dq.n++];
    e->tpag = tpag;
    e->x = x, e->y = y, e->ox = ox, e->oy = oy, e->xs = xs, e->ys = ys, e->ang = ang, e->alpha = alpha;
    e->color = color;
    e->x0 = bx0, e->y0 = by0, e->x1 = bx1, e->y1 = by1;
    Runner* run = renderer->runner;
    VMContext* vm = run->vmContext;
    e->inst = vm != nullptr ? vm->currentInstance : nullptr;
    e->evObj = vm != nullptr ? vm->currentEventObjectIndex : -1;
    e->evType = vm != nullptr ? vm->currentEventType : -1;
    e->depth = run->drawDepth;
    e->rim = (uint8_t) (k == 2);
    e->muted = (uint8_t) renderer->drawMuted;
#ifdef CTRPORT_FLOWERGPU_DQSTATS
    dq.deferred++;
#endif
    return true;
}

static VR_CODE void dqW_drawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale,
                                   float yscale, float angleDeg, uint32_t color, float alpha) {
    if (dqStale()) Vine_dqSync();
    uint32_t o = (uint32_t) tpagIndex - (uint32_t) dq.tlo;
    int k = (uint32_t) (dq.thi - dq.tlo) >= o ? dq.kind[o] : 0;
    if (k != 0 && dqHold(renderer, k, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha)) return;
    Vine_dqFlush();
    Vine_dqPrev.drawSprite(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
}

static VR_CODE void dqW_gpuSetShader(Renderer* renderer, int32_t shaderIndex) {
    if (dqStale()) Vine_dqSync();
    if (Vine_dqState[0] && shaderIndex == dq.rimShader) {
        dq.logShader = shaderIndex;
        renderer->currentShader = shaderIndex;
        return;
    }
    Vine_dqFlush();
    Vine_dqPrev.gpuSetShader(renderer, shaderIndex);
}

static VR_CODE void dqW_gpuResetShader(Renderer* renderer) {
    if (dqStale()) Vine_dqSync();
    if (Vine_dqState[0]) {
        dq.logShader = -1;
        renderer->currentShader = -1;
        return;
    }
    Vine_dqPrev.gpuResetShader(renderer);
}

// The decorations set the normal blend, which they already have (ctrGpuSetBlendMode returns at once then).
static VR_CODE void dqW_gpuSetBlendMode(Renderer* renderer, int32_t mode) {
    if (dqStale()) Vine_dqSync();
    if (Vine_dqState[0] && mode == dq.r->blendMode) return;
    Vine_dqFlush();
    Vine_dqPrev.gpuSetBlendMode(renderer, mode);
}

// The renderer goes (game_change, exit): the next one, perhaps at the same address, finds the decorations again.
static VR_CODE void dqW_destroy(Renderer* renderer) {
    if (renderer == &dq.r->base) {
        Vine_dqState[0] = 0;
        dq.n = 0;
        dq.r = nullptr;
    }
    Vine_dqPrev.destroy(renderer);
}

#if defined(CTRPORT_FLOWERGPU_DQSTATS) || defined(CTRPORT_FLOWERGPU_DQCHECK)
// Test builds: the counts every 120 frames.
static VR_CODE void dqW_beginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    if (dqStale()) Vine_dqSync();
    Vine_dqFlush();
    Vine_dqPrev.beginFrame(renderer, gameW, gameH, windowW, windowH);
    uint32_t f = dq.r->frame;
#ifdef CTRPORT_FLOWERGPU_DQSTATS
    if (f - dq.lastLog >= 120u) {
        logInfo(dqFmtStat, (unsigned) f, (unsigned) dq.runs, (unsigned) dq.brOverlap, (unsigned) dq.brDisp, (unsigned) dq.brCap,
                (unsigned) dq.deferred);
        dq.runs = dq.brOverlap = dq.brDisp = dq.brCap = dq.deferred = 0;
        dq.lastLog = f;
#ifdef CTRPORT_FLOWERGPU_DQCHECK
        logInfo(dqFmtChk, (unsigned) f, (unsigned) dq.pairs, (unsigned) dq.viol, (unsigned) dq.checked, (unsigned) dq.unchecked,
                (int) (dq.maxEx * 100.0f));
#endif
    }
#endif
    (void) f;
}
#endif

// At each tower draw: the first one of a renderer finds the decorations and puts the table on top.
static VR_CODE void dqInstall(CtrRenderer* r) {
    // Each renderer is looked at once (its destroy forgets it); the table is never put on top of itself.
    if (dq.r == r || r->base.vtable == &dq.outer) return;
    memset(&dq, 0, sizeof(dq));
    Vine_dqState[0] = 0;
    dq.r = r;
    dq.tlo = 0;
    dq.thi = -1;
    Renderer* rr = &r->base;
    const DataWin* dw = rr->dataWin;
    if (dw == nullptr) return;
    int32_t spr[4];
    for (int s = 0; 4 > s; s++)
        if ((spr[s] = dqSpriteIndex(dw, s)) < 0) return;
    int32_t shader = dqShaderIndex(dw);
    if (shader < 0) return;
    int32_t lo = INT32_MAX, hi = -1;
    for (int pass = 0; 2 > pass; pass++) {
        for (int s = 0; 4 > s; s++) {
            const Sprite* sp = &dw->sprt.sprites[spr[s]];
            for (uint32_t f = 0; sp->tpagIndices != nullptr && sp->textureCount > f; f++) {
                int32_t t = sp->tpagIndices[f];
                if (0 > t || (uint32_t) t >= dw->tpag.count) continue;
                if (pass == 0) {
                    lo = t < lo ? t : lo;
                    hi = t > hi ? t : hi;
                } else {
                    dq.kind[t - lo] = (uint8_t) (2 > s ? 1 : 2);
                }
            }
        }
        if (pass == 0 && (hi < lo || hi - lo >= DQ_SPAN)) return;
    }
    dq.tlo = lo;
    dq.thi = hi;
    dq.rimShader = shader;
    dq.logShader = -1;
    Vine_dqWrappedAt = &r->skip.wrapped;
    Vine_dqState[1] = (uint8_t) r->skip.wrapped;
    Vine_dqPrev = *rr->vtable;
    uintptr_t stubs = (uintptr_t) &Vine_dqStubs;
    for (int i = 0; DQ_NV > i; i++) {
        uintptr_t v;
        memcpy(&v, (const char*) &Vine_dqPrev + (size_t) i * sizeof(void*), sizeof(v));
        if (v != 0) v = stubs + 8u * (uintptr_t) i;
        memcpy((char*) &dq.outer + (size_t) i * sizeof(void*), &v, sizeof(v));
    }
    dq.outer.drawSprite = Vine_dqPrev.drawSprite != nullptr ? dqW_drawSprite : nullptr;
    dq.outer.gpuSetShader = Vine_dqPrev.gpuSetShader != nullptr ? dqW_gpuSetShader : nullptr;
    dq.outer.gpuResetShader = Vine_dqPrev.gpuResetShader != nullptr ? dqW_gpuResetShader : nullptr;
    dq.outer.gpuSetBlendMode = Vine_dqPrev.gpuSetBlendMode != nullptr ? dqW_gpuSetBlendMode : nullptr;
    dq.outer.destroy = Vine_dqPrev.destroy != nullptr ? dqW_destroy : nullptr;
#if defined(CTRPORT_FLOWERGPU_DQSTATS) || defined(CTRPORT_FLOWERGPU_DQCHECK)
    dq.outer.beginFrame = Vine_dqPrev.beginFrame != nullptr ? dqW_beginFrame : nullptr;
#ifdef CTRPORT_FLOWERGPU_DQSTATS
    dq.lastLog = r->frame;
#endif
#endif
    rr->vtable = &dq.outer;
    logInfo(dqFmtOn, (int) lo, (int) hi, (int) shader);
}
