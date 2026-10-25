// Exact grouped mesh; B2m is deliberately not included.
#include "ctr_fountain_uniforms.h"
#include "ctr_fountain_shape.h"
// Keep the normal page call visible to GCC's recursive call-graph ordering,
// without folding the entire existing region dispatcher into the late hook.
static __typeof__(emitPageRegion) emitPageRegion __attribute__((noinline));
_Static_assert(MAX_UNIFORMS <= FOUNTAIN_HANDLE_FIRST, "Fountain handles overlap base uniforms");
_Static_assert(sizeof(((CtrRenderer*)0)->shaderFxOf) <= FOUNTAIN_HANDLE_SHADERS, "Fountain handle namespace too small");
static const char pillarTime[] PILLAR_RO = "iTime";
static const char pillarOffset[] PILLAR_RO = "iPosOffset";
static const char pillarUvs[] PILLAR_RO = "uvs";
static const char pillarShaderName[] PILLAR_RO = "shd_fountaineffect";
static const char pillarBoundaryError[] PILLAR_RO = "Renderer: fountain boundary capacity exceeded\n";
static const char pillarNoTarget[] PILLAR_RO = "Renderer: no target for fountain shader draw\n";
typedef struct CtrFountainState {
    CtrRenderer* owner;
    struct CtrFountainState* next;
    CtrFountainUniforms* uniforms;
} CtrFountainState;
static CtrFountainState* fountainStates __attribute__((section(".bss.late.pillar")));
static PILLAR_LATE CtrFountainState* fountainFind(CtrRenderer* r) {
    for (CtrFountainState* s = fountainStates; s; s = s->next) if (s->owner == r) return s;
    return nullptr;
}
static PILLAR_LATE CtrFountainState* fountainGet(CtrRenderer* r) {
    CtrFountainState* s = fountainFind(r);
    if (s == nullptr) {
        s = calloc(1, sizeof(*s));
        if (s == nullptr) abort();
        s->owner = r; s->next = fountainStates; fountainStates = s;
    }
    return s;
}
// Public fountain inputs live outside the base sequential allocator. Neither
// query order nor adding a supported fountain can renumber another shader.
static PILLAR_LATE int32_t fountainUniformSlot(const char* name) {
    if (name == nullptr) return -1;
    if (strcmp(name, pillarTime) == 0) return 0;
    if (strcmp(name, pillarOffset) == 0) return 1;
    if (strcmp(name, pillarUvs) == 0) return 2;
    // iResolution is unused in the shader, and desktop returns -1 for it.
    return -1;
}

static PILLAR_LATE CtrFountainUniforms* fountainUniformFind(CtrRenderer* r, int32_t shader) {
    CtrFountainState* state = fountainFind(r);
    if (state != nullptr) {
        for (CtrFountainUniforms* u = state->uniforms; u != nullptr; u = u->next)
            if (u->shader == shader) return u;
    }
    return nullptr;
}

static PILLAR_LATE int32_t fountainUniformHandle(CtrRenderer* r, int32_t shader, const char* name) {
    int32_t slot = fountainUniformSlot(name);
    if (slot < 0 || shader < 0 || shader >= FOUNTAIN_HANDLE_SHADERS) return -1;
    CtrFountainUniforms* u = fountainUniformFind(r, shader);
    if (u == nullptr) {
        CtrFountainState* state = fountainGet(r);
        u = calloc(1, sizeof(*u));
        if (u == nullptr) abort();
        u->shader = shader;
        u->next = state->uniforms;
        state->uniforms = u;
    }
    u->issued |= 1u << slot;
    return FOUNTAIN_HANDLE_FIRST + shader * FOUNTAIN_HANDLE_SLOTS + slot;
}

static PILLAR_LATE const float* fountainUniformValue(CtrRenderer* r, const char* name) {
    static const float zero[4] PILLAR_RO = {0};
    int32_t slot = fountainUniformSlot(name);
    if (slot < 0) return zero;
    CtrFountainUniforms* u = fountainUniformFind(r, r->fxShader);
    return u != nullptr && (u->issued & (1u << slot)) != 0 ? u->values[slot] : zero;
}

static PILLAR_LATE void fountainUniformDispatch(
    CtrRenderer* r, int32_t handle, const float* values, uint32_t count) {
    uint32_t index = (uint32_t) handle - FOUNTAIN_HANDLE_FIRST;
    if (index >= FOUNTAIN_HANDLE_LIMIT - FOUNTAIN_HANDLE_FIRST) {
        return;
    }
    uint32_t slot = index % FOUNTAIN_HANDLE_SLOTS;
    if (slot >= 3) return;
    int32_t shader = (int32_t) (index / FOUNTAIN_HANDLE_SLOTS);
    CtrFountainUniforms* u = fountainUniformFind(r, shader);
    // An arbitrary integer in the range cannot create a new binding.
    if (u == nullptr || (u->issued & (1u << slot)) == 0) return;
    if (count > 4) count = 4;
    if (memcmp(u->values[slot], values, count * sizeof(float)) == 0) return;
    bool current = r->shaderFx == PILLAR_FX && r->fxShader == shader;
    if (current) flushBatch(r);
    memcpy(u->values[slot], values, count * sizeof(float));
    if (current && r->inFrame) applyTexEnv(r);
}

// The fountain has no source-alpha dependency. Unit 0/1 sample independent
// wrapped coordinates. Averaging first avoids clamping their sum before tint.
// Vertex RGB supplies the tint twice; vertex alpha carries horizontal falloff.
static PILLAR_LATE void drawFountainBatch(CtrRenderer* r, uint32_t count) {
    C3D_TexEnv saved[6], env[6];
    for (int32_t i = 0; 6 > i; i++) {
        saved[i] = *C3D_GetTexEnv(i);
        C3D_TexEnvInit(&env[i]);
    }
    C3D_TexEnvSrc(&env[0], C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE1, GPU_CONSTANT);
    C3D_TexEnvFunc(&env[0], C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvColor(&env[0], 0xFF808080u);
    C3D_TexEnvSrc(&env[0], C3D_Alpha, GPU_CONSTANT, 0, 0);
    C3D_TexEnvFunc(&env[0], C3D_Alpha, GPU_REPLACE);
    for (int32_t i = 1; 3 > i; i++) {
        C3D_TexEnvSrc(&env[i], C3D_RGB, GPU_PREVIOUS, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(&env[i], C3D_RGB, GPU_MODULATE);
    }
    C3D_TexEnvSrc(&env[3], C3D_RGB, GPU_PREVIOUS, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvOpRgb(&env[3], GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA, 0);
    C3D_TexEnvFunc(&env[3], C3D_RGB, GPU_MODULATE);
    C3D_TexEnvScale(&env[3], C3D_RGB, GPU_TEVSCALE_2);
    bool linear = r->filterOverride != 0 ? r->filterOverride == FILTER_LINEAR : r->base.texFilter;
    GPU_TEXTURE_FILTER_PARAM filter = linear ? GPU_LINEAR : GPU_NEAREST;
    C3D_TexSetFilter(r->batchTex, filter, filter);
    C3D_TexSetFilter(r->batchTexB, filter, filter);
    C3D_BindProgram(r->pairProgram);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, r->pairTc1Loc,
                 r->batchTc1[0], r->batchTc1[1], r->batchTc1[2], r->batchTc1[3]);
    C3D_TexBind(0, r->batchTex);
    C3D_TexBind(1, r->batchTexB);
    for (int32_t i = 0; 6 > i; i++) *C3D_GetTexEnv(i) = env[i];
    if (r->gameProjection && drawingBothEyes(r)) drawBothEyes(r, (int) r->batchStart, (int) count);
    else drawRange(r, (int) r->batchStart, (int) count);
    for (int32_t i = 0; 6 > i; i++) *C3D_GetTexEnv(i) = saved[i];
    CITRO_TEX(1) = nullptr;
    CITRO_FLAGS |= CITRO_FLAG_TEX0 << 1;
    C3D_BindProgram(r->batchProgram);
    r->citroCleanValid = false;
}

#define FOUNTAIN_CUTS_MAX 256
typedef struct { float u, v; } CtrFountainPoint;

static PILLAR_LATE void fountainCut(float* cuts, int32_t* count, float x, float lo, float hi) {
    if (x <= lo || x >= hi) return;
    if (*count >= FOUNTAIN_CUTS_MAX) {
        logError(pillarBoundaryError);
        abort();
    }
    cuts[(*count)++] = x;
}

static PILLAR_LATE int fountainCutCompare(const void* a, const void* b) {
    float x = *(const float*) a, y = *(const float*) b;
    return (x > y) - (x < y);
}

// All wrap and tile boundaries for both scrolling samples, plus the centre
// bend in falloff. A vertical grid bounds interpolation error in the sine edge.
static PILLAR_LATE int32_t fountainCuts(float* cuts, float lo, float hi, float start, float span,
                            int32_t tileSize, int32_t tiles, const float* phase, bool vertical) {
    int32_t n = 0;
    cuts[n++] = lo;
    cuts[n++] = hi;
    fountainCut(cuts, &n, 0.5f, lo, hi);
    for (int32_t k = 0; 2 > k; k++) {
        for (int32_t j = 0; tiles >= j; j++) {
            float q = ((float) (j * tileSize) - start) / span;
            if (q < 0.0f || q > 1.0f) continue;
            float base = q - phase[k];
            float first = ceilf(lo - base);
            for (float m = first; hi > base + m; m += 1.0f) fountainCut(cuts, &n, base + m, lo, hi);
        }
        float first = ceilf(lo + phase[k]);
        for (float m = first; hi > m - phase[k]; m += 1.0f) fountainCut(cuts, &n, m - phase[k], lo, hi);
    }
    if (vertical) {
        for (int32_t i = 1; 64 > i; i++) fountainCut(cuts, &n, lo + (hi - lo) * (float) i / 64.0f, lo, hi);
    }
    qsort(cuts, (size_t) n, sizeof(*cuts), fountainCutCompare);
    int32_t out = 1;
    for (int32_t i = 1; n > i; i++) {
        if (cuts[i] > cuts[out - 1] + 0.0000001f) cuts[out++] = cuts[i];
    }
    return out;
}

// Clip the UV polygon to the linearized left or right wavy edge in this row.
static PILLAR_LATE int32_t fountainClip(const CtrFountainPoint* in, int32_t n, CtrFountainPoint* out,
                            float v0, float v1, float edge0, float edge1, bool right) {
    int32_t count = 0;
    for (int32_t i = 0; n > i; i++) {
        CtrFountainPoint a = in[i], b = in[(i + 1) % n];
        float ea = edge0 + (edge1 - edge0) * ((a.v - v0) / (v1 - v0));
        float eb = edge0 + (edge1 - edge0) * ((b.v - v0) / (v1 - v0));
        float da = right ? 1.0f - ea - a.u : a.u - ea;
        float db = right ? 1.0f - eb - b.u : b.u - eb;
        if (da >= 0.0f) out[count++] = a;
        if ((da >= 0.0f) != (db >= 0.0f)) {
            float f = da / (da - db);
            out[count++] = (CtrFountainPoint) { a.u + (b.u - a.u) * f, a.v + (b.v - a.v) * f };
        }
    }
    return count;
}

static PILLAR_LATE C3D_Tex* fountainTile(CtrRenderer* r, CtrPage* page, float x, float y, int32_t* tx, int32_t* ty) {
    *tx = minI(maxI((int32_t) floorf(x / (float) page->tileW), 0), page->tilesX - 1);
    *ty = minI(maxI((int32_t) floorf(y / (float) page->tileH), 0), page->tilesY - 1);
    CtrTextures_ensureTiles(&r->textures, page, *tx, *ty, *tx, *ty);
    CtrTile* tile = &page->tiles[*ty * page->tilesX + *tx];
    if (!tile->valid) return page->formatsKnown && tile->format == CTR_TILE_EMPTY ? &r->textures.clear : nullptr;
    return CtrTextures_drawTex(tile);
}
// pillarchk: a new second-sample transform with the same texture pair. batchUnitFor keeps the batch's textures
// and never copies palNextTc1, so set it on the (now empty) batch, or the cell samples with the last cell's offset.
// Out of line in its own late section so that fountainMeshRegion keeps its size and later late code its address.
static __attribute__((noinline, section(".text.late.pillarchk"))) void fountainTc1Flush(CtrRenderer* r) {
    flushBatch(r);
    memcpy(r->batchTc1, r->palNextTc1, sizeof(r->batchTc1));
}
#line 214
static PILLAR_LATE void fountainMeshRegion(CtrRenderer* r, CtrPage* page, float sx0, float sy0, float sx1, float sy1,
                            const float* px, const float* py, const CtrColor* colors) {
    const float* uv = fountainUniformValue(r, pillarUvs);
    const float* offset = fountainUniformValue(r, pillarOffset);
    float time = fountainUniformValue(r, pillarTime)[0];
    float origin[2] = { uv[0] * (float) page->width, uv[1] * (float) page->height };
    float span[2] = { uv[2] * (float) page->width, uv[3] * (float) page->height };
    if (!(span[0] > 0.0f && span[1] > 0.0f && sx1 > sx0 && sy1 > sy0)) return;
    float u0 = (sx0 - origin[0]) / span[0], u1 = (sx1 - origin[0]) / span[0];
    float v0 = (sy0 - origin[1]) / span[1], v1 = (sy1 - origin[1]) / span[1];
    float loU = maxF(u0, 0.0f), hiU = minF(u1, 1.0f);
    float loV = v0, hiV = v1;
    if (!(hiU > loU && hiV > loV)) return;
    if (quadOutsideView(r, px, py)) return;
    if (hiV - loV > 1.0f) {
        float middle = loV + 1.0f;
        float t = (middle - v0) / (v1 - v0);
        float x[4], y[4];
        CtrColor c[4];
        const float ss[4] = {0, 1, 1, 0}, tt[4] = {0, 0, t, t};
        for (int i = 0; 4 > i; i++) quadPoint(px, py, colors, ss[i], tt[i], &x[i], &y[i], &c[i]);
        fountainMeshRegion(r, page, sx0, sy0, sx1, origin[1] + middle * span[1], x, y, c);
        const float tb[4] = {t, t, 1, 1};
        for (int i = 0; 4 > i; i++) quadPoint(px, py, colors, ss[i], tb[i], &x[i], &y[i], &c[i]);
        fountainMeshRegion(r, page, sx0, origin[1] + middle * span[1], sx1, sy1, x, y, c);
        return;
    }
    float phaseX[2] = { offset[0] + time * 0.008f, offset[0] - time * 0.009f };
    float phaseY[2] = { offset[1] + time * 0.005f, offset[1] + time * 0.007f };
    for (int32_t k = 0; 2 > k; k++) {
        phaseX[k] -= floorf(phaseX[k]);
        phaseY[k] -= floorf(phaseY[k]);
    }
    float xc[FOUNTAIN_CUTS_MAX], yc[FOUNTAIN_CUTS_MAX];
    int32_t nx = fountainCuts(xc, loU, hiU, origin[0], span[0], page->tileW, page->tilesX, phaseX, false);
    int32_t ny = fountainCuts(yc, loV, hiV, origin[1], span[1], page->tileH, page->tilesY, phaseY, true);
    float edges[FOUNTAIN_CUTS_MAX];
    for (int32_t j = 0; ny > j; j++) edges[j] = (1.0f + sinf(time + yc[j] * FOUNTAIN_WAVE_FREQUENCY)) * FOUNTAIN_EDGE;
    flushBatch(r);
    // A convex quad's cells have disjoint coverage. Walk down each column so
    // sample-wrap transforms stay together instead of flushing per column per
    // row. Preserve row order for folded/nonconvex draw_sprite_pos quads.
    bool positive = true, negative = true;
    for (int32_t k = 0; 4 > k; k++) {
        int32_t m = (k + 1) % 4, n = (k + 2) % 4;
        float turn = (px[m] - px[k]) * (py[n] - py[m]) - (py[m] - py[k]) * (px[n] - px[m]);
        positive &= turn >= 0.0f;
        negative &= turn <= 0.0f;
    }
    bool columnMajor = positive || negative;
    int32_t outer = columnMajor ? nx : ny, inner = columnMajor ? ny : nx;
    for (int32_t outerIndex = 0; outer - 1 > outerIndex; outerIndex++) {
        for (int32_t innerIndex = 0; inner - 1 > innerIndex; innerIndex++) {
            int32_t i = columnMajor ? outerIndex : innerIndex, j = columnMajor ? innerIndex : outerIndex;
            float a = yc[j], b = yc[j + 1];
            float edgeA = edges[j], edgeB = edges[j + 1];
            CtrFountainPoint rect[4] = { {xc[i], a}, {xc[i + 1], a}, {xc[i + 1], b}, {xc[i], b} };
            CtrFountainPoint mid[8], poly[8];
            int32_t n = fountainClip(rect, 4, mid, a, b, edgeA, edgeB, false);
            if (3 > n) continue;
            n = fountainClip(mid, n, poly, a, b, edgeA, edgeB, true);
            if (3 > n) continue;
            float centreU = (xc[i] + xc[i + 1]) * 0.5f, centreV = (a + b) * 0.5f;
            float shiftX[2], shiftY[2];
            C3D_Tex* tex[2];
            int32_t tileX[2], tileY[2];
            for (int32_t k = 0; 2 > k; k++) {
                shiftX[k] = phaseX[k] - floorf(centreU + phaseX[k]);
                shiftY[k] = phaseY[k] - floorf(centreV + phaseY[k]);
                tex[k] = fountainTile(r, page, origin[0] + (centreU + shiftX[k]) * span[0],
                                      origin[1] + (centreV + shiftY[k]) * span[1], &tileX[k], &tileY[k]);
            }
            if (tex[0] == nullptr || tex[1] == nullptr) continue;
            r->palNextB = tex[1];
            r->palNextTc1[0] = r->palNextTc1[1] = 1.0f;
            r->palNextTc1[2] = ((shiftX[1] - shiftX[0]) * span[0]) / (float) page->tileW + (float) (tileX[0] - tileX[1]);
            r->palNextTc1[3] = ((shiftY[1] - shiftY[0]) * span[1]) / (float) page->tileH + (float) (tileY[0] - tileY[1]);
            // A new transform with the same texture pair: fountainTc1Flush sets it on the batch (see there).
            // Keep the generic batch lookup unchanged for every other caller.
            if (memcmp(r->palNextTc1, r->batchTc1, sizeof(r->batchTc1)) != 0) fountainTc1Flush(r);
            CtrVertex* verts = reserveVertices(r, tex[0], (uint32_t) (n - 2) * 3u);
            if (verts == nullptr) continue;
            CtrVertex corners[8];
            for (int32_t q = 0; n > q; q++) {
                CtrFountainPoint p = poly[q];
                float x, y;
                CtrColor c;
                quadPoint(px, py, colors, (p.u - u0) / (u1 - u0), (p.v - v0) / (v1 - v0), &x, &y, &c);
                c.a = unitToByte(1.0f - fabsf(p.u - 0.5f) * 0.75f);
                float u = (origin[0] + (p.u + shiftX[0]) * span[0]) / (float) page->tileW - (float) tileX[0];
                float v = (origin[1] + (p.v + shiftY[0]) * span[1]) / (float) page->tileH - (float) tileY[0];
                setVertex(&corners[q], x, y, r->vertexZ, u, v, c);
            }
            for (int32_t k = 1; n - 1 > k; k++) {
                *verts++ = corners[0];
                *verts++ = corners[k];
                *verts++ = corners[k + 1];
            }
        }
    }
    flushBatch(r);
    r->palNextB = nullptr;
}


// Existing bodies retain their sections. Only call targets redirect to these
// late dispatchers; their ordinary branch always invokes the original path.
static PILLAR_LATE void pillarDrawPairBatch(CtrRenderer* r, uint32_t count) {
    if (r->shaderFx == PILLAR_FX && !r->fxSuspended) drawFountainBatch(r, count);
    else drawPairBatch(r, count);
}
static PILLAR_LATE void pillarEmitPageRegion(CtrRenderer* r, CtrPage* p,
    float x0, float y0, float x1, float y1, const float* x, const float* y, const CtrColor* c) {
    if (r->shaderFx != PILLAR_FX || r->fxSuspended) {
        emitPageRegion(r,p,x0,y0,x1,y1,x,y,c); return;
    }
    if (recordingSurface(r) != nullptr) {
        // Match the existing callers' unused return value, preserving GCC's
        // windowRecordedSurface.isra.0 body. Success clears s->recorded;
        // every failure leaves that surface recorded. Check the bound surface,
        // rather than recordingSurface(), which may also report dialogue.
        CtrSurface* target = getSurface(r, r->boundSurface);
        bool eligible = target != nullptr && target->recorded && r->inFrame;
        pillarWindowRecordedSurface(r);
        target = getSurface(r, r->boundSurface);
        if (!eligible || target == nullptr || target->recorded) { logError(pillarNoTarget); return; }
    }
    fountainMeshRegion(r,p,x0,y0,x1,y1,x,y,c);
}
static PILLAR_LATE void pillarEmitPageRegionDirect(CtrRenderer* r, CtrPage* p, bool data,
    float x0, float y0, float x1, float y1, const float* x, const float* y, const CtrColor* c) {
    if (r->shaderFx == PILLAR_FX && !r->fxSuspended)
        pillarEmitPageRegion(r,p,x0,y0,x1,y1,x,y,c);
    else emitPageRegionDirect(r,p,data,x0,y0,x1,y1,x,y,c);
}
static PILLAR_LATE void pillarSurfaceQuad(CtrRenderer* r, CtrSurface* s, C3D_Tex* tex,
    float tw, float th, float x0, float y0, float x1, float y1,
    const float* x, const float* y, const CtrColor* c) {
    if (r->shaderFx != PILLAR_FX || r->fxSuspended) {
        PILLAR_OPAQUE(emitQuad)(r,tex,x,y,x0/tw,y0/th,x1/tw,y1/th,c); return;
    }
    (void)s;
    CtrTile tile = {0}; tile.valid = true; tile.mirror = tex;
    CtrPage page = {0};
    page.width = page.tileW = (int32_t)tw;
    page.height = page.tileH = (int32_t)th;
    page.tilesX = page.tilesY = 1; page.tiles = &tile;
    fountainMeshRegion(r,&page,x0,y0,x1,y1,x,y,c);
}
static PILLAR_LATE bool pillarSurfaceFxDraw(const CtrRenderer* r) {
    return (!r->fxSuspended && r->shaderFx == PILLAR_FX) || surfaceFxDraw(r);
}
static PILLAR_LATE bool pillarShader(CtrRenderer* r, int32_t shader) {
    const char* name = shaderName(&r->base, shader);
    return name != nullptr && strcmp(name,pillarShaderName) == 0;
}
static PILLAR_LATE void pillarGpuSetShader(Renderer* renderer, int32_t shader) {
    CtrRenderer* r = (CtrRenderer*)renderer;
    if (shader >= 0 && shader < (int32_t)sizeof(r->shaderFxOf) && pillarShader(r,shader))
        r->shaderFxOf[shader] = (uint8_t)(PILLAR_FX+1);
    PILLAR_OPAQUE(ctrGpuSetShader)(renderer,shader);
}
static PILLAR_LATE int32_t pillarShaderGetUniform(Renderer* renderer, int32_t shader, char* name) {
    CtrRenderer* r = (CtrRenderer*)renderer;
    if (pillarShader(r,shader)) return fountainUniformHandle(r,shader,name);
    return PILLAR_OPAQUE(ctrShaderGetUniform)(renderer,shader,name);
}
static PILLAR_LATE void pillarShaderSetUniformF(Renderer* renderer, int32_t handle, int32_t count,
    float a, float b, float c, float d) {
    if ((uint32_t)handle - FOUNTAIN_HANDLE_FIRST >= FOUNTAIN_HANDLE_LIMIT - FOUNTAIN_HANDLE_FIRST) {
        PILLAR_OPAQUE(ctrShaderSetUniformF)(renderer,handle,count,a,b,c,d); return;
    }
    float values[4] = {a,b,c,d};
    fountainUniformDispatch((CtrRenderer*)renderer,handle,values,count < 0 ? 0 : (uint32_t)count);
}
static PILLAR_LATE void pillarShaderSetUniformFArray(Renderer* renderer, int32_t handle, float* values, uint32_t count) {
    if ((uint32_t)handle - FOUNTAIN_HANDLE_FIRST >= FOUNTAIN_HANDLE_LIMIT - FOUNTAIN_HANDLE_FIRST) {
        PILLAR_OPAQUE(ctrShaderSetUniformFArray)(renderer,handle,values,count); return;
    }
    if (values) fountainUniformDispatch((CtrRenderer*)renderer,handle,values,count);
}
static PILLAR_LATE void pillarShaderSetUniformI(Renderer* renderer, int32_t handle, int32_t count,
    int32_t a, int32_t b, int32_t c, int32_t d) {
    if ((uint32_t)handle - FOUNTAIN_HANDLE_FIRST >= FOUNTAIN_HANDLE_LIMIT - FOUNTAIN_HANDLE_FIRST) {
        PILLAR_OPAQUE(ctrShaderSetUniformI)(renderer,handle,count,a,b,c,d); return;
    }
    float values[4] = {(float)a,(float)b,(float)c,(float)d};
    fountainUniformDispatch((CtrRenderer*)renderer,handle,values,count < 0 ? 0 : (uint32_t)count);
}
static PILLAR_LATE void pillarDestroy(Renderer* renderer) {
    CtrFountainState** link = &fountainStates;
    while (*link) {
        CtrFountainState* s = *link;
        if (s->owner != (CtrRenderer*)renderer) { link = &s->next; continue; }
        while (s->uniforms) { CtrFountainUniforms* u = s->uniforms; s->uniforms = u->next; free(u); }
        *link = s->next; free(s); break;
    }
    PILLAR_OPAQUE(ctrDestroy)(renderer);
}

// The dispatchers remove formerly inlined bodies from these functions.
// Retain their Release input-section extents after the code and literal pools.
// ctrDrawSpriteTiled uses its existing in-function knfuse trap tail instead,
// so its separate post-link function-size guard remains unchanged.
// The linked gate must prove the body starts and next symbols are unchanged,
// and that no branch enters these tails, before allowing symbol-size changes.
#if defined(__3DS__) && defined(NDEBUG) && !defined(CTRPORT_RENDER_TIMING)
static void __attribute__((used, noinline, section(".text.late.pillar"))) pillarRetainedSlots(void) {
    __asm__(".pushsection .text.flushBatch, 1\n.space 1048\n.popsection\n"
            ".pushsection .text.drawSurfaceImpl, 1\n.space 48\n.popsection\n");
}
#endif
