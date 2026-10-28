// Per-frame runner state hashing for regression testing (--state-hash <file>).
//
// After every frame we hash the observable game state (room, RNG, every instance's built-in properties and self
// variables, globals, structs, ds_maps/ds_lists) and, separately, every draw call the frame issued to the renderer.
// Two runs over the same input playback must produce byte-identical files; the first differing line points at the
// frame where behaviour diverged.
//
// Self-variable maps are hashed order-independently (sum of per-entry hashes), so the hash only depends on the values
// stored, not on hashmap layout. Value hashes include the RValue type, so a change of representation (int32 vs real)
// is reported as a difference too.
//
// The runner switches to a virtual clock (GetTime_enableVirtualClock) that advances one room-speed frame per step, so
// time-based GML repeats exactly. To find out *what* diverged, set STATE_HASH_DUMP_FRAME=<frame>: the per-instance
// running hashes and every variable's hash for that frame are written to state_hash_dump.txt in the working directory;
// diff two dumps.

#include "state_hash.h"

#include "runner.h"
#include "renderer.h"
#include "instance.h"
#include "gml_array.h"
#include "stb_ds.h"
#include "gettime.h"

#include <string.h>
#include <stdlib.h>
#ifdef CTRPORT_OFFDRAW
// offdraw's diagnostic (platform/3ds/diag/ctr_offdraw.c): offdraw.txt's hashskip=<object> leaves that object's
// instances out of the state hash and its sprite's drawSprite records out of the draw hash (hashskip=kill: only the
// kill objects' instances past their kill line).
bool CtrOffdraw_hashSkip(void* inst);
bool CtrOffdraw_skipDraw(int32_t tpag, float x, float y, float xs, float ys);
#define OFFDRAW_SKIP(inst) CtrOffdraw_hashSkip(inst)
#define OFFDRAW_SKIP_DRAW(tpag, x, y, xs, ys) CtrOffdraw_skipDraw(tpag, x, y, xs, ys)
static int32_t OFFDRAW_HASHED(Runner* runner, int32_t n) {
    int32_t kept = 0;
    for (int32_t i = 0; n > i; i++) kept += !OFFDRAW_SKIP(runner->instances[i]);
    return kept;
}
#else
#define OFFDRAW_SKIP(inst) false
#define OFFDRAW_SKIP_DRAW(tpag, x, y, xs, ys) false
#define OFFDRAW_HASHED(runner, n) (n)
#endif
#line 28

static FILE* g_out = nullptr;
static uint64_t g_drawHash = 0;
static uint64_t g_drawCalls = 0;
static RendererVtable g_wrappedVtable;
static const RendererVtable* g_originalVtable = nullptr;

// Optional literal transcript for an offline comparator. It contains no
// whitelist and never substitutes a value in the runtime hash.
static FILE* g_trace = nullptr;
static bool g_traceMix = false;
static const char* g_traceScope = "?";
static int32_t g_traceObject = -1;
static uint32_t g_traceInstance = 0;
static RendererVtable g_traceVtable;
static const RendererVtable* g_traceOriginal = nullptr;

static int32_t traceGetter(Renderer* r, int32_t shader, char* name, bool sampler) {
    int32_t handle = sampler ? g_traceOriginal->shaderGetSamplerIndex(r, shader, name)
                             : g_traceOriginal->shaderGetUniform(r, shader, name);
    if (g_trace != nullptr) {
        fprintf(g_trace, "U %c %d %d ", sampler ? 'S' : 'U', shader, handle);
        for (const unsigned char* p = (const unsigned char*) name; *p; p++) fprintf(g_trace, "%02x", *p);
        fputc('\n', g_trace);
    }
    return handle;
}

static int32_t traceUniform(Renderer* r, int32_t shader, char* name) { return traceGetter(r, shader, name, false); }
static int32_t traceSampler(Renderer* r, int32_t shader, char* name) { return traceGetter(r, shader, name, true); }

void StateHash_traceBegin(const char* path, Renderer* renderer) {
    g_trace = fopen(path, "wb");
    if (g_trace == nullptr) { logError("Could not open state transcript '%s'\n", path); exit(1); }
    setvbuf(g_trace, nullptr, _IOFBF, 65536);
    fputs("STATE_TRACE 1\n", g_trace);
    g_traceOriginal = renderer->vtable;
    g_traceVtable = *renderer->vtable;
    if (g_traceVtable.shaderGetUniform != nullptr) g_traceVtable.shaderGetUniform = traceUniform;
    if (g_traceVtable.shaderGetSamplerIndex != nullptr) g_traceVtable.shaderGetSamplerIndex = traceSampler;
    renderer->vtable = &g_traceVtable;
}

static inline uint64_t mix64(uint64_t h, uint64_t v) {
    if (g_traceMix) fprintf(g_trace, "M %llx\n", (unsigned long long) v);
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27;
    return h;
}

static inline uint64_t hashBytes(uint64_t h, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*) data;
    uint64_t f = 0xCBF29CE484222325ull;
    for (size_t i = 0; len > i; i++) {
        f ^= p[i];
        f *= 0x100000001B3ull;
    }
    return mix64(h, f ^ len);
}

static inline uint64_t hashFloat(uint64_t h, float f) {
    uint32_t bits;
    if (f != f) bits = 0x7FC00000u; else memcpy(&bits, &f, 4);
    return mix64(h, bits);
}

static inline uint64_t hashDouble(uint64_t h, double d) {
    uint64_t bits;
    if (d != d) bits = 0x7FF8000000000000ull; else memcpy(&bits, &d, 8);
    return mix64(h, bits);
}

static inline uint64_t hashString(uint64_t h, const char* s) {
    if (s == nullptr) return mix64(h, 0xDEADull);
    return hashBytes(h, s, strlen(s));
}

static uint64_t hashRValue(uint64_t h, RValue v, int depth);

static uint64_t hashArray(uint64_t h, GMLArray* arr, int depth) {
    if (arr == nullptr) return mix64(h, 0xA11ull);
    if (depth > 4) return mix64(h, 0xDEE9ull);
    if (arr->type == GML_LEGACY_ARRAY) {
        h = mix64(h, (uint64_t) arr->legacy.rowCount);
        for (int32_t r = 0; arr->legacy.rowCount > r; r++) {
            GMLArrayRow* row = &arr->legacy.rows[r];
            h = mix64(h, (uint64_t) row->length);
            for (int32_t c = 0; row->length > c; c++) {
                if (g_traceMix) fprintf(g_trace, "I %d %lld\n", depth + 1, (long long) r * 32000 + c);
                h = hashRValue(h, row->data[c], depth + 1);
            }
        }
    } else {
        h = mix64(h, (uint64_t) arr->modern.length);
        for (int32_t i = 0; arr->modern.length > i; i++) {
            if (g_traceMix) fprintf(g_trace, "I %d %d\n", depth + 1, i);
            h = hashRValue(h, arr->modern.data[i], depth + 1);
        }
    }
    return h;
}

static uint64_t hashRValue(uint64_t h, RValue v, int depth) {
    if (g_traceMix) fprintf(g_trace, "R %d %d\n", depth, v.type);
    h = mix64(h, v.type);
    switch (v.type) {
        case RVALUE_REAL:
            if (g_traceMix) fputs("N\n", g_trace);
            return hashDouble(h, (double) v.real);
        case RVALUE_INT32:
            if (g_traceMix) fputs("N\n", g_trace);
            return mix64(h, (uint32_t) v.int32);
        case RVALUE_BOOL:
            return mix64(h, (uint32_t) v.int32);
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64: return mix64(h, (uint64_t) v.int64);
#endif
        case RVALUE_STRING: return hashString(h, v.string);
        case RVALUE_ARRAY: return hashArray(h, v.array, depth);
        case RVALUE_STRUCT: return mix64(h, v.structInst != nullptr ? v.structInst->instanceId : 0);
#if IS_WAD17_OR_HIGHER_ENABLED
        case RVALUE_METHOD:
            return v.method != nullptr ? mix64(mix64(h, (uint64_t) v.method->codeIndex), (uint64_t) v.method->boundInstanceId) : h;
#endif
        case RVALUE_ASSETREF: return mix64(mix64(h, (uint32_t) v.int32), v.assetRefType);
        default: return h;
    }
}

static FILE* g_dump = nullptr;

static uint64_t hashVarMap(uint64_t h, IntRValueHashMap* map) {
    bool tracing = g_traceMix;
    if (tracing) fprintf(g_trace, "B %s %d %u %u\n", g_traceScope, g_traceObject, g_traceInstance, map->count);
    uint64_t sum = 0;
    if (map->entries != nullptr) {
        for (uint32_t i = 0; map->capacity > i; i++) {
            IntRValueEntry* e = &map->entries[i];
            if (e->key == INT_RVALUE_HASHMAP_EMPTY_KEY) continue;
            if (tracing) fprintf(g_trace, "V %d\n", e->key);
            uint64_t eh = hashRValue(mix64(0x5E1Full, (uint32_t) e->key), e->value, 0);
            if (tracing) fprintf(g_trace, "E %016llx\n", (unsigned long long) eh);
            if (g_dump != nullptr) fprintf(g_dump, "  var %d type %d %016llx\n", e->key, e->value.type, (unsigned long long) eh);
            sum += eh;
        }
    }
    g_traceMix = false;
    uint64_t result = mix64(mix64(h, map->count), sum);
    g_traceMix = tracing;
    if (tracing) fprintf(g_trace, "C %016llx %016llx\n", (unsigned long long) sum, (unsigned long long) result);
    return result;
}

static uint64_t hashInstance(uint64_t h, Instance* inst) {
    h = mix64(h, inst->instanceId);
    h = mix64(h, (uint32_t) inst->objectIndex);
    h = hashFloat(h, inst->x); h = hashFloat(h, inst->y);
    h = hashFloat(h, inst->xprevious); h = hashFloat(h, inst->yprevious);
    h = hashFloat(h, inst->xstart); h = hashFloat(h, inst->ystart);
    h = mix64(h, (uint64_t) inst->persistent | ((uint64_t) inst->solid << 1) | ((uint64_t) inst->active << 2) | ((uint64_t) inst->destroyed << 3) | ((uint64_t) inst->visible << 4));
    h = mix64(h, (uint32_t) inst->spriteIndex);
    h = hashFloat(h, inst->imageSpeed); h = hashFloat(h, inst->imageIndex);
    h = hashFloat(h, inst->imageXscale); h = hashFloat(h, inst->imageYscale); h = hashFloat(h, inst->imageAlpha);
    h = hashDouble(h, (double) inst->imageAngle);
    h = mix64(h, inst->imageBlend);
    h = mix64(h, (uint32_t) inst->depth);
    h = mix64(h, (uint32_t) inst->maskIndex);
    h = hashFloat(h, inst->speed); h = hashFloat(h, inst->direction);
    h = hashFloat(h, inst->hspeed); h = hashFloat(h, inst->vspeed);
    h = hashFloat(h, inst->friction); h = hashFloat(h, inst->gravity); h = hashFloat(h, inst->gravityDirection);
    h = mix64(h, (uint32_t) inst->pathIndex); h = hashFloat(h, inst->pathPosition); h = hashFloat(h, inst->pathSpeed);
    for (int i = 0; GML_ALARM_COUNT > i; i++) h = mix64(h, (uint32_t) inst->alarm[i]);
    h = mix64(h, (uint32_t) inst->timelineIndex); h = hashFloat(h, inst->timelinePosition);
    g_traceScope = "O"; g_traceObject = inst->objectIndex; g_traceInstance = inst->instanceId;
    return hashVarMap(h, &inst->selfVars);
}

static uint64_t hashRunner(Runner* runner) {
    uint64_t h = 0x57A7Eull;
    h = mix64(h, (uint32_t) runner->frameCount);
    h = mix64(h, (uint32_t) runner->currentRoomIndex);
    h = hashBytes(h, runner->random.state, sizeof(runner->random.state));
    h = mix64(h, runner->random.index);

    int32_t n = (int32_t) arrlen(runner->instances);
    h = mix64(h, (uint32_t) OFFDRAW_HASHED(runner, n));
    for (int32_t i = 0; n > i; i++) {
        if (OFFDRAW_SKIP(runner->instances[i])) continue; h = hashInstance(h, runner->instances[i]);
        if (g_dump != nullptr) fprintf(g_dump, "inst %d obj %d -> %016llx\n", runner->instances[i]->instanceId, runner->instances[i]->objectIndex, (unsigned long long) h);
    }

    if (runner->vmContext->globalScopeInstance != nullptr) {
        g_traceScope = "G"; g_traceObject = -1; g_traceInstance = 0;
        h = hashVarMap(h, &runner->vmContext->globalScopeInstance->selfVars);
    }

    int32_t structCount = (int32_t) arrlen(runner->structInstances);
    h = mix64(h, (uint32_t) structCount);
    for (int32_t i = 0; structCount > i; i++) {
        Instance* s = runner->structInstances[i];
        h = mix64(h, s->instanceId);
        g_traceScope = "S"; g_traceObject = s->objectIndex; g_traceInstance = s->instanceId;
        h = hashVarMap(h, &s->selfVars);
    }

    // UNDERTALE keeps all of its text in one big ds_map, so contents are hashed every 16th frame only (sizes always).
    bool fullMaps = (runner->frameCount % 16) == 0;
    int32_t mapCount = (int32_t) arrlen(runner->dsMapPool);
    h = mix64(h, (uint32_t) mapCount);
    for (int32_t i = 0; mapCount > i; i++) {
        DsMapEntry* map = runner->dsMapPool[i];
        uint64_t sum = 0;
        int32_t len = (int32_t) shlen(map);
        if (fullMaps) {
            // Map entries start independent hashes; emit their literal sum
            // below. ds_map entries are never variable whitelist selectors.
            bool tracing = g_traceMix;
            g_traceMix = false;
            for (int32_t j = 0; len > j; j++) sum += hashRValue(hashString(0x3A9ull, map[j].key), map[j].value, 0);
            g_traceMix = tracing;
        }
        h = mix64(mix64(h, (uint32_t) len), sum);
    }

    int32_t listCount = (int32_t) arrlen(runner->dsListPool);
    h = mix64(h, (uint32_t) listCount);
    for (int32_t i = 0; listCount > i; i++) {
        DsList* list = &runner->dsListPool[i];
        int32_t len = (int32_t) arrlen(list->items);
        h = mix64(h, (uint32_t) len | ((uint64_t) list->freed << 32));
        for (int32_t j = 0; len > j; j++) h = hashRValue(h, list->items[j], 0);
    }
    return h;
}

// ===[ Draw call hashing ]===

// Every renderer entry point that can change pixels is wrapped. Calls that write pixels (DH_BEGIN) count towards the
// frame's draw-call column; state, pass and surface-lifecycle calls (DS_BEGIN) only mix into the hash. Only calls made
// at nesting depth 0 are hashed: a backend that calls its own vtable from inside a wrapped call (the GL renderer's
// surfaceCopy and present use drawSurface) does not add records, so the hash follows what the engine asked for.
static int32_t g_dhDepth = 0;

#define DH_BEGIN(tag) if (g_dhDepth == 0) { uint64_t h = mix64(g_drawHash, tag); g_drawCalls++
#define DS_BEGIN(tag) if (g_dhDepth == 0) { uint64_t h = mix64(g_drawHash, tag)
#define DH_F(v) h = hashFloat(h, (v))
#define DH_I(v) h = mix64(h, (uint64_t) (uint32_t) (v))
#define DH_END() g_drawHash = h; }
#define DH_CALL(call) do { g_dhDepth++; call; g_dhDepth--; } while (0)

static void w_drawSprite(Renderer* r, int32_t tpag, float x, float y, float ox, float oy, float xs, float ys, float a, uint32_t c, float al) {
    if (!OFFDRAW_SKIP_DRAW(tpag, x, y, xs, ys)) { DH_BEGIN(1); DH_I(tpag); DH_F(x); DH_F(y); DH_F(ox); DH_F(oy); DH_F(xs); DH_F(ys); DH_F(a); DH_I(c); DH_F(al); DH_END(); }
    DH_CALL(g_originalVtable->drawSprite(r, tpag, x, y, ox, oy, xs, ys, a, c, al));
}

static void w_drawSpritePart(Renderer* r, int32_t tpag, float sx, float sy, float sw, float sh, float x, float y, float xs, float ys, float a, float px, float py, uint32_t c, float al) {
    DH_BEGIN(2); DH_I(tpag); DH_F(sx); DH_F(sy); DH_F(sw); DH_F(sh); DH_F(x); DH_F(y); DH_F(xs); DH_F(ys); DH_F(a); DH_F(px); DH_F(py); DH_I(c); DH_F(al); DH_END();
    DH_CALL(g_originalVtable->drawSpritePart(r, tpag, sx, sy, sw, sh, x, y, xs, ys, a, px, py, c, al));
}

static void w_drawSpritePartColor(Renderer* r, int32_t tpag, float sx, float sy, float sw, float sh, float x, float y, float xs, float ys, float a, float px, float py, uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4, float al) {
    DH_BEGIN(3); DH_I(tpag); DH_F(sx); DH_F(sy); DH_F(sw); DH_F(sh); DH_F(x); DH_F(y); DH_F(xs); DH_F(ys); DH_F(a); DH_F(px); DH_F(py); DH_I(c1); DH_I(c2); DH_I(c3); DH_I(c4); DH_F(al); DH_END();
    DH_CALL(g_originalVtable->drawSpritePartColor(r, tpag, sx, sy, sw, sh, x, y, xs, ys, a, px, py, c1, c2, c3, c4, al));
}

static void w_drawSpritePos(Renderer* r, int32_t tpag, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float al) {
    DH_BEGIN(4); DH_I(tpag); DH_F(x1); DH_F(y1); DH_F(x2); DH_F(y2); DH_F(x3); DH_F(y3); DH_F(x4); DH_F(y4); DH_F(al); DH_END();
    DH_CALL(g_originalVtable->drawSpritePos(r, tpag, x1, y1, x2, y2, x3, y3, x4, y4, al));
}

static void w_drawRectangle(Renderer* r, float x1, float y1, float x2, float y2, uint32_t c, float al, bool outline) {
    DH_BEGIN(5); DH_F(x1); DH_F(y1); DH_F(x2); DH_F(y2); DH_I(c); DH_F(al); DH_I(outline); DH_END();
    DH_CALL(g_originalVtable->drawRectangle(r, x1, y1, x2, y2, c, al, outline));
}

static void w_drawRectangleColor(Renderer* r, float x1, float y1, float x2, float y2, uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4, float al, bool outline) {
    DH_BEGIN(6); DH_F(x1); DH_F(y1); DH_F(x2); DH_F(y2); DH_I(c1); DH_I(c2); DH_I(c3); DH_I(c4); DH_F(al); DH_I(outline); DH_END();
    DH_CALL(g_originalVtable->drawRectangleColor(r, x1, y1, x2, y2, c1, c2, c3, c4, al, outline));
}

static void w_drawLine(Renderer* r, float x1, float y1, float x2, float y2, float w, uint32_t c, float al) {
    DH_BEGIN(7); DH_F(x1); DH_F(y1); DH_F(x2); DH_F(y2); DH_F(w); DH_I(c); DH_F(al); DH_END();
    DH_CALL(g_originalVtable->drawLine(r, x1, y1, x2, y2, w, c, al));
}

static void w_drawTriangle(Renderer* r, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t c1, uint32_t c2, uint32_t c3, float al, bool outline) {
    DH_BEGIN(8); DH_F(x1); DH_F(y1); DH_F(x2); DH_F(y2); DH_F(x3); DH_F(y3); DH_I(c1); DH_I(c2); DH_I(c3); DH_F(al); DH_I(outline); DH_END();
    DH_CALL(g_originalVtable->drawTriangle(r, x1, y1, x2, y2, x3, y3, c1, c2, c3, al, outline));
}

// Hashed as the drawTriangle calls it stands for, so a renderer with or without the fan gives the same hash.
static void w_drawTriangleFan(Renderer* r, float cx, float cy, const float* xs, const float* ys, int32_t n, uint32_t c1, uint32_t c2, float al) {
    for (int32_t i = 0; n - 1 > i; i++) {
        DH_BEGIN(8); DH_F(cx); DH_F(cy); DH_F(xs[i]); DH_F(ys[i]); DH_F(xs[i + 1]); DH_F(ys[i + 1]); DH_I(c1); DH_I(c2); DH_I(c2); DH_F(al); DH_I(false); DH_END();
    }
    DH_CALL(g_originalVtable->drawTriangleFan(r, cx, cy, xs, ys, n, c1, c2, al));
}

// Hashed as the drawLine calls it stands for, so a renderer with or without it gives the same hash.
static void w_drawLines(Renderer* r, const float* xy, int32_t n, float w, uint32_t c, float al) {
    for (int32_t i = 0; n > i; i++) {
        const float* l = &xy[4 * i];
        DH_BEGIN(7); DH_F(l[0]); DH_F(l[1]); DH_F(l[2]); DH_F(l[3]); DH_F(w); DH_I(c); DH_F(al); DH_END();
    }
    DH_CALL(g_originalVtable->drawLines(r, xy, n, w, c, al));
}

static void w_drawLineColor(Renderer* r, float x1, float y1, float x2, float y2, float w, uint32_t c1, uint32_t c2, float al) {
    DH_BEGIN(9); DH_F(x1); DH_F(y1); DH_F(x2); DH_F(y2); DH_F(w); DH_I(c1); DH_I(c2); DH_F(al); DH_END();
    DH_CALL(g_originalVtable->drawLineColor(r, x1, y1, x2, y2, w, c1, c2, al));
}

static void w_drawText(Renderer* r, const char* text, float x, float y, float xs, float ys, float a, float sep) {
    DH_BEGIN(10); h = hashString(h, text); DH_F(x); DH_F(y); DH_F(xs); DH_F(ys); DH_F(a); DH_F(sep);
    DH_I(r->drawColor); DH_F(r->drawAlpha); DH_I(r->drawFont); DH_I(r->drawHalign); DH_I(r->drawValign); DH_END();
    DH_CALL(g_originalVtable->drawText(r, text, x, y, xs, ys, a, sep));
}

static void w_drawTextColor(Renderer* r, const char* text, float x, float y, float xs, float ys, float a, int32_t c1, int32_t c2, int32_t c3, int32_t c4, float al, float sep) {
    DH_BEGIN(11); h = hashString(h, text); DH_F(x); DH_F(y); DH_F(xs); DH_F(ys); DH_F(a); DH_I(c1); DH_I(c2); DH_I(c3); DH_I(c4); DH_F(al); DH_F(sep);
    DH_I(r->drawFont); DH_I(r->drawHalign); DH_I(r->drawValign); DH_END();
    DH_CALL(g_originalVtable->drawTextColor(r, text, x, y, xs, ys, a, c1, c2, c3, c4, al, sep));
}

static void w_drawTextUI(Renderer* r, const char* text, float x, float y, float xs, float ys, float a, int32_t c1, int32_t c2, int32_t c3, int32_t c4, float al, float sep) {
    DH_BEGIN(12); h = hashString(h, text); DH_F(x); DH_F(y); DH_F(xs); DH_F(ys); DH_F(a); DH_I(c1); DH_I(c2); DH_I(c3); DH_I(c4); DH_F(al); DH_F(sep);
    DH_I(r->drawFont); DH_I(r->drawHalign); DH_I(r->drawValign); DH_END();
    DH_CALL(g_originalVtable->drawTextUI(r, text, x, y, xs, ys, a, c1, c2, c3, c4, al, sep));
}

static void w_drawVertex(Renderer* r, float x, float y, float z, uint32_t c, float al, float u, float v) {
    DH_BEGIN(13); DH_F(x); DH_F(y); DH_F(z); DH_I(c); DH_F(al); DH_F(u); DH_F(v); DH_END();
    DH_CALL(g_originalVtable->drawVertex(r, x, y, z, c, al, u, v));
}

static void w_clearScreen(Renderer* r, uint32_t c, float al) {
    DH_BEGIN(14); DH_I(c); DH_F(al); DH_END();
    DH_CALL(g_originalVtable->clearScreen(r, c, al));
}

static void w_drawTile(Renderer* r, RoomTile* tile, float ox, float oy) {
    // field by field: the struct has padding after useSpriteDefinition
    DH_BEGIN(15); DH_I(tile->x); DH_I(tile->y); DH_I(tile->useSpriteDefinition); DH_I(tile->backgroundDefinition);
    DH_I(tile->sourceX); DH_I(tile->sourceY); DH_I(tile->width); DH_I(tile->height); DH_I(tile->tileDepth);
    DH_I(tile->instanceID); DH_F(tile->scaleX); DH_F(tile->scaleY); DH_I(tile->color); DH_F(tile->alpha);
    DH_F(ox); DH_F(oy); DH_END();
    DH_CALL(g_originalVtable->drawTile(r, tile, ox, oy));
}

static void w_drawSpriteTiled(Renderer* r, int32_t tpag, float ox, float oy, float x, float y, float xs, float ys, bool tx, bool ty, float rw, float rh, uint32_t c, float al) {
    DH_BEGIN(16); DH_I(tpag); DH_F(ox); DH_F(oy); DH_F(x); DH_F(y); DH_F(xs); DH_F(ys); DH_I(tx); DH_I(ty); DH_F(rw); DH_F(rh); DH_I(c); DH_F(al); DH_END();
    DH_CALL(g_originalVtable->drawSpriteTiled(r, tpag, ox, oy, x, y, xs, ys, tx, ty, rw, rh, c, al));
}

// ---[ Primitives and vertex buffers ]---

static void w_primitiveBegin(Renderer* r, int32_t type) {
    DS_BEGIN(17); DH_I(type); DH_END();
    DH_CALL(g_originalVtable->primitiveBegin(r, type));
}

static void w_primitiveBeginTexture(Renderer* r, int32_t type, int32_t texture) {
    DS_BEGIN(18); DH_I(type); DH_I(texture); DH_END();
    DH_CALL(g_originalVtable->primitiveBeginTexture(r, type, texture));
}

static void w_primitiveEnd(Renderer* r) {
    DH_BEGIN(19); DH_END();
    DH_CALL(g_originalVtable->primitiveEnd(r));
}

// The format, the submit arguments and every byte of vertex data the buffer holds.
static void w_drawVertexBuffer(Renderer* r, VertexBuffer* vb, int32_t primitive, int32_t texture, int32_t offset, int32_t count) {
    DH_BEGIN(20); DH_I(primitive); DH_I(texture); DH_I(offset); DH_I(count);
    if (vb != nullptr) {
        DH_I(vb->vertexSize);
        h = mix64(h, (uint64_t) vb->size);
        const VertexFormat* f = vb->format;
        if (f != nullptr) {
            DH_I(f->numElements); DH_I(f->stride);
            for (int32_t i = 0; f->numElements > i && 16 > i; i++) {
                DH_I(f->elements[i].usage); DH_I(f->elements[i].type); DH_I(f->elements[i].offset); DH_I(f->elements[i].size);
            }
        } else {
            DH_I(-1);
        }
        if (vb->data != nullptr) h = hashBytes(h, vb->data, vb->size); else DH_I(-1);
    } else {
        DH_I(-1);
    }
    DH_END();
    DH_CALL(g_originalVtable->drawVertexBuffer(r, vb, primitive, texture, offset, count));
}

// ---[ Sprites made from surfaces ]---

static int32_t w_createSpriteFromSurface(Renderer* r, int32_t surfaceID, int32_t x, int32_t y, int32_t w, int32_t hh, bool removeback, bool smooth, int32_t xorig, int32_t yorig) {
    int32_t ret;
    DH_CALL(ret = g_originalVtable->createSpriteFromSurface(r, surfaceID, x, y, w, hh, removeback, smooth, xorig, yorig));
    DS_BEGIN(21); DH_I(surfaceID); DH_I(x); DH_I(y); DH_I(w); DH_I(hh); DH_I(removeback); DH_I(smooth); DH_I(xorig); DH_I(yorig); DH_I(ret); DH_END();
    return ret;
}

static void w_deleteSprite(Renderer* r, int32_t spriteIndex) {
    DS_BEGIN(22); DH_I(spriteIndex); DH_END();
    DH_CALL(g_originalVtable->deleteSprite(r, spriteIndex));
}

// ---[ Blend and other GPU state ]---

static void w_gpuSetBlendMode(Renderer* r, int32_t mode) {
    DS_BEGIN(23); DH_I(mode); DH_END();
    DH_CALL(g_originalVtable->gpuSetBlendMode(r, mode));
}

static void w_gpuSetBlendModeExt(Renderer* r, int32_t sf, int32_t df, int32_t sfa, int32_t dfa) {
    DS_BEGIN(24); DH_I(sf); DH_I(df); DH_I(sfa); DH_I(dfa); DH_END();
    DH_CALL(g_originalVtable->gpuSetBlendModeExt(r, sf, df, sfa, dfa));
}

static void w_gpuSetBlendEnable(Renderer* r, bool enable) {
    DS_BEGIN(25); DH_I(enable); DH_END();
    DH_CALL(g_originalVtable->gpuSetBlendEnable(r, enable));
}

static void w_gpuSetTexFilter(Renderer* r, bool enable) {
    DS_BEGIN(26); DH_I(enable); DH_END();
    DH_CALL(g_originalVtable->gpuSetTexFilter(r, enable));
}

static void w_gpuSetAlphaTestEnable(Renderer* r, bool enable) {
    DS_BEGIN(27); DH_I(enable); DH_END();
    DH_CALL(g_originalVtable->gpuSetAlphaTestEnable(r, enable));
}

static void w_gpuSetAlphaTestRef(Renderer* r, uint8_t ref) {
    DS_BEGIN(28); DH_I(ref); DH_END();
    DH_CALL(g_originalVtable->gpuSetAlphaTestRef(r, ref));
}

static void w_gpuSetColorWriteEnable(Renderer* r, bool red, bool green, bool blue, bool alpha) {
    DS_BEGIN(29); DH_I(red); DH_I(green); DH_I(blue); DH_I(alpha); DH_END();
    DH_CALL(g_originalVtable->gpuSetColorWriteEnable(r, red, green, blue, alpha));
}

static void w_gpuSetFog(Renderer* r, bool enable, uint32_t color) {
    DS_BEGIN(30); DH_I(enable); DH_I(color); DH_END();
    DH_CALL(g_originalVtable->gpuSetFog(r, enable, color));
}

// ---[ Surfaces ]---

static int32_t w_createSurface(Renderer* r, int32_t width, int32_t height) {
    int32_t ret;
    DH_CALL(ret = g_originalVtable->createSurface(r, width, height));
    DS_BEGIN(31); DH_I(width); DH_I(height); DH_I(ret); DH_END();
    return ret;
}

static bool w_setRenderTarget(Renderer* r, int32_t surfaceID, bool implicitApplicationSurface) {
    bool ret;
    DH_CALL(ret = g_originalVtable->setRenderTarget(r, surfaceID, implicitApplicationSurface));
    DS_BEGIN(32); DH_I(surfaceID); DH_I(implicitApplicationSurface); DH_I(ret); DH_END();
    return ret;
}

static int32_t w_ensureApplicationSurface(Renderer* r, int32_t width, int32_t height) {
    int32_t ret;
    DH_CALL(ret = g_originalVtable->ensureApplicationSurface(r, width, height));
    DS_BEGIN(33); DH_I(width); DH_I(height); DH_I(ret); DH_END();
    return ret;
}

static void w_drawSurface(Renderer* r, int32_t id, int32_t sl, int32_t st, int32_t sw, int32_t sh, float x, float y, float xs, float ys, float a, uint32_t c, float al) {
    DH_BEGIN(34); DH_I(id); DH_I(sl); DH_I(st); DH_I(sw); DH_I(sh); DH_F(x); DH_F(y); DH_F(xs); DH_F(ys); DH_F(a); DH_I(c); DH_F(al); DH_END();
    DH_CALL(g_originalVtable->drawSurface(r, id, sl, st, sw, sh, x, y, xs, ys, a, c, al));
}

static void w_drawSurfaceColor(Renderer* r, int32_t id, int32_t sl, int32_t st, int32_t sw, int32_t sh, float x, float y, float xs, float ys, float a, uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4, float al) {
    DH_BEGIN(35); DH_I(id); DH_I(sl); DH_I(st); DH_I(sw); DH_I(sh); DH_F(x); DH_F(y); DH_F(xs); DH_F(ys); DH_F(a); DH_I(c1); DH_I(c2); DH_I(c3); DH_I(c4); DH_F(al); DH_END();
    DH_CALL(g_originalVtable->drawSurfaceColor(r, id, sl, st, sw, sh, x, y, xs, ys, a, c1, c2, c3, c4, al));
}

static void w_drawSurfaceTiled(Renderer* r, int32_t id, float x, float y, float xs, float ys, float rw, float rh, uint32_t c, float al) {
    DH_BEGIN(36); DH_I(id); DH_F(x); DH_F(y); DH_F(xs); DH_F(ys); DH_F(rw); DH_F(rh); DH_I(c); DH_F(al); DH_END();
    DH_CALL(g_originalVtable->drawSurfaceTiled(r, id, x, y, xs, ys, rw, rh, c, al));
}

static void w_surfaceResize(Renderer* r, int32_t id, int32_t width, int32_t height) {
    DS_BEGIN(37); DH_I(id); DH_I(width); DH_I(height); DH_END();
    DH_CALL(g_originalVtable->surfaceResize(r, id, width, height));
}

static void w_surfaceFree(Renderer* r, int32_t id) {
    DS_BEGIN(38); DH_I(id); DH_END();
    DH_CALL(g_originalVtable->surfaceFree(r, id));
}

static void w_surfaceCopy(Renderer* r, int32_t dest, int32_t dx, int32_t dy, int32_t src, int32_t sx, int32_t sy, int32_t sw, int32_t sh, bool part) {
    DH_BEGIN(39); DH_I(dest); DH_I(dx); DH_I(dy); DH_I(src); DH_I(sx); DH_I(sy); DH_I(sw); DH_I(sh); DH_I(part); DH_END();
    DH_CALL(g_originalVtable->surfaceCopy(r, dest, dx, dy, src, sx, sy, sw, sh, part));
}

// The pixels are the surface's size as the renderer reports it; the only caller, buffer_set_surface, checks the same.
static bool w_surfaceSetPixels(Renderer* r, int32_t id, const uint8_t* rgba) {
    int32_t w = 0, hh = 0;
    if (g_dhDepth == 0 && g_originalVtable->getSurfaceWidth != nullptr && g_originalVtable->getSurfaceHeight != nullptr) {
        w = (int32_t) g_originalVtable->getSurfaceWidth(r, id);
        hh = (int32_t) g_originalVtable->getSurfaceHeight(r, id);
    }
    DH_BEGIN(40); DH_I(id); DH_I(w); DH_I(hh);
    if (rgba != nullptr && w > 0 && hh > 0) h = hashBytes(h, rgba, (size_t) w * (size_t) hh * 4); else DH_I(-1);
    DH_END();
    bool ret;
    DH_CALL(ret = g_originalVtable->surfaceSetPixels(r, id, rgba));
    DS_BEGIN(40); DH_I(ret); DH_END();
    return ret;
}

static void w_surfaceUploadPixels(Renderer* r, int32_t id, int32_t w, int32_t hh, const uint8_t* rgba) {
    DH_BEGIN(41); DH_I(id); DH_I(w); DH_I(hh);
    if (rgba != nullptr && w > 0 && hh > 0) h = hashBytes(h, rgba, (size_t) w * (size_t) hh * 4); else DH_I(-1);
    DH_END();
    DH_CALL(g_originalVtable->surfaceUploadPixels(r, id, w, hh, rgba));
}

static void w_drawTiledPart(Renderer* r, int32_t tpag, int32_t sx, int32_t sy, int32_t sw, int32_t sh, float dx, float dy, float dw, float dh, uint32_t c, float al) {
    DH_BEGIN(42); DH_I(tpag); DH_I(sx); DH_I(sy); DH_I(sw); DH_I(sh); DH_F(dx); DH_F(dy); DH_F(dw); DH_F(dh); DH_I(c); DH_F(al); DH_END();
    DH_CALL(g_originalVtable->drawTiledPart(r, tpag, sx, sy, sw, sh, dx, dy, dw, dh, c, al));
}

// ---[ Shaders, texture stages and matrices ]---

static void w_gpuSetShader(Renderer* r, int32_t shader) {
    DS_BEGIN(43); DH_I(shader); DH_END();
    DH_CALL(g_originalVtable->gpuSetShader(r, shader));
}

static void w_gpuResetShader(Renderer* r) {
    DS_BEGIN(44); DH_END();
    DH_CALL(g_originalVtable->gpuResetShader(r));
}

static void w_shaderSetUniformF(Renderer* r, int32_t handle, int32_t count, float v1, float v2, float v3, float v4) {
    DS_BEGIN(45); DH_I(handle); DH_I(count); DH_F(v1); DH_F(v2); DH_F(v3); DH_F(v4); DH_END();
    DH_CALL(g_originalVtable->shaderSetUniformF(r, handle, count, v1, v2, v3, v4));
}

static void w_shaderSetUniformFArray(Renderer* r, int32_t handle, float* values, uint32_t count) {
    DS_BEGIN(46); DH_I(handle); DH_I(count);
    if (values != nullptr) { for (uint32_t i = 0; count > i; i++) DH_F(values[i]); } else DH_I(-1);
    DH_END();
    DH_CALL(g_originalVtable->shaderSetUniformFArray(r, handle, values, count));
}

static void w_shaderSetUniformI(Renderer* r, int32_t handle, int32_t count, int32_t v1, int32_t v2, int32_t v3, int32_t v4) {
    DS_BEGIN(47); DH_I(handle); DH_I(count); DH_I(v1); DH_I(v2); DH_I(v3); DH_I(v4); DH_END();
    DH_CALL(g_originalVtable->shaderSetUniformI(r, handle, count, v1, v2, v3, v4));
}

static void w_textureSetStage(Renderer* r, int32_t slot, uint32_t texID) {
    DS_BEGIN(48); DH_I(slot); DH_I(texID); DH_END();
    DH_CALL(g_originalVtable->textureSetStage(r, slot, texID));
}

static void w_setMatrix(Renderer* r, int32_t type, Matrix4f m) {
    DS_BEGIN(49); DH_I(type); for (int32_t i = 0; 16 > i; i++) DH_F(m.m[i]); DH_END();
    DH_CALL(g_originalVtable->setMatrix(r, type, m));
}

// ---[ Frame, view and GUI passes ]---

static void w_beginFrame(Renderer* r, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    DS_BEGIN(50); DH_I(gameW); DH_I(gameH); DH_I(windowW); DH_I(windowH); DH_END();
    DH_CALL(g_originalVtable->beginFrame(r, gameW, gameH, windowW, windowH));
}

static void w_endFrameInit(Renderer* r) {
    DS_BEGIN(51); DH_END();
    DH_CALL(g_originalVtable->endFrameInit(r));
}

static void w_endFrameEnd(Renderer* r) {
    DS_BEGIN(52); DH_END();
    DH_CALL(g_originalVtable->endFrameEnd(r));
}

// The runner sets cameraCurrent just before these, and backends read it.
static void w_beginView(Renderer* r, int32_t vx, int32_t vy, int32_t vw, int32_t vh, int32_t px, int32_t py, int32_t pw, int32_t ph, float a) {
    DS_BEGIN(53); DH_I(vx); DH_I(vy); DH_I(vw); DH_I(vh); DH_I(px); DH_I(py); DH_I(pw); DH_I(ph); DH_F(a); DH_I(r->cameraCurrent); DH_END();
    DH_CALL(g_originalVtable->beginView(r, vx, vy, vw, vh, px, py, pw, ph, a));
}

static void w_endView(Renderer* r) {
    DS_BEGIN(54); DH_END();
    DH_CALL(g_originalVtable->endView(r));
}

static void w_applyProjection(Renderer* r, const Matrix4f* view, const Matrix4f* proj) {
    DS_BEGIN(55); DH_I(r->cameraCurrent);
    if (view != nullptr) { for (int32_t i = 0; 16 > i; i++) DH_F(view->m[i]); } else DH_I(-1);
    if (proj != nullptr) { for (int32_t i = 0; 16 > i; i++) DH_F(proj->m[i]); } else DH_I(-1);
    DH_END();
    DH_CALL(g_originalVtable->applyProjection(r, view, proj));
}

static void w_beginGUI(Renderer* r, int32_t guiW, int32_t guiH, int32_t px, int32_t py, int32_t pw, int32_t ph, int32_t target) {
    DS_BEGIN(56); DH_I(guiW); DH_I(guiH); DH_I(px); DH_I(py); DH_I(pw); DH_I(ph); DH_I(target); DH_END();
    DH_CALL(g_originalVtable->beginGUI(r, guiW, guiH, px, py, pw, ph, target));
}

static void w_setGuiProjection(Renderer* r, int32_t guiW, int32_t guiH, int32_t pw, int32_t ph, bool userSurface) {
    DS_BEGIN(57); DH_I(guiW); DH_I(guiH); DH_I(pw); DH_I(ph); DH_I(userSurface); DH_END();
    DH_CALL(g_originalVtable->setGuiProjection(r, guiW, guiH, pw, ph, userSurface));
}

static void w_endGUI(Renderer* r) {
    DS_BEGIN(58); DH_END();
    DH_CALL(g_originalVtable->endGUI(r));
}

#ifdef BS_REACH_GATE
#include "state_hash_reach.inc"
#endif

#define WRAP(name) if (g_wrappedVtable.name != nullptr) g_wrappedVtable.name = w_##name

void StateHash_begin(const char* path, Renderer* renderer) {
    g_out = fopen(path, "wb");
    if (g_out == nullptr) {
        logError("Could not open state hash file '%s'\n", path);
        exit(1);
    }
#ifdef PLATFORM_3DS
    setvbuf(g_out, nullptr, _IOFBF, 16384);
#endif
    g_originalVtable = renderer->vtable;
    g_wrappedVtable = *renderer->vtable;
    WRAP(drawSprite); WRAP(drawSpritePart); WRAP(drawSpritePartColor); WRAP(drawSpritePos);
    WRAP(drawRectangle); WRAP(drawRectangleColor); WRAP(drawLine); WRAP(drawTriangle); WRAP(drawTriangleFan); WRAP(drawLines); WRAP(drawLineColor);
    WRAP(drawText); WRAP(drawTextColor); WRAP(drawTextUI); WRAP(drawVertex); WRAP(clearScreen);
    WRAP(drawTile); WRAP(drawSpriteTiled);
    WRAP(primitiveBegin); WRAP(primitiveBeginTexture); WRAP(primitiveEnd); WRAP(drawVertexBuffer);
    WRAP(createSpriteFromSurface); WRAP(deleteSprite);
    WRAP(gpuSetBlendMode); WRAP(gpuSetBlendModeExt); WRAP(gpuSetBlendEnable); WRAP(gpuSetTexFilter);
    WRAP(gpuSetAlphaTestEnable); WRAP(gpuSetAlphaTestRef); WRAP(gpuSetColorWriteEnable); WRAP(gpuSetFog);
    WRAP(createSurface); WRAP(setRenderTarget); WRAP(ensureApplicationSurface);
    WRAP(drawSurface); WRAP(drawSurfaceColor); WRAP(drawSurfaceTiled); WRAP(surfaceResize); WRAP(surfaceFree); WRAP(surfaceCopy);
    WRAP(surfaceSetPixels); WRAP(surfaceUploadPixels); WRAP(drawTiledPart);
    WRAP(gpuSetShader); WRAP(gpuResetShader); WRAP(shaderSetUniformF); WRAP(shaderSetUniformFArray); WRAP(shaderSetUniformI);
    WRAP(textureSetStage); WRAP(setMatrix);
    WRAP(beginFrame); WRAP(endFrameInit); WRAP(endFrameEnd); WRAP(beginView); WRAP(endView); WRAP(applyProjection);
    WRAP(beginGUI); WRAP(setGuiProjection); WRAP(endGUI);
    renderer->vtable = &g_wrappedVtable;
#ifndef PLATFORM_3DS
    const char* tracePath = getenv("STATE_HASH_TRACE");
    if (tracePath != nullptr && *tracePath != '\0') StateHash_traceBegin(tracePath, renderer);
#endif
#ifdef BS_REACH_GATE
    ReachGate_beginSession(path);
#endif
}

void StateHash_endFrame(Runner* runner) {
    if (g_out == nullptr) return;
    static int dumpFrame = -2;
    if (dumpFrame == -2) { const char* e = getenv("STATE_HASH_DUMP_FRAME"); dumpFrame = e ? atoi(e) : -1; }
    if (runner->frameCount == dumpFrame) g_dump = fopen("state_hash_dump.txt", "wb");
    if (g_trace != nullptr) fprintf(g_trace, "F %d %d\n", runner->frameCount, runner->currentRoomIndex);
    g_traceMix = g_trace != nullptr;
#ifdef BS_REACH_GATE
    uint64_t stateHash = ReachGate_reachMode() ? hashRunnerReach(runner) : hashRunner(runner);
#else
    uint64_t stateHash = hashRunner(runner);
#endif
    g_traceMix = false;
    if (g_trace != nullptr) fprintf(g_trace, "H %016llx %016llx %llu\n", (unsigned long long) stateHash,
                                   (unsigned long long) g_drawHash, (unsigned long long) g_drawCalls);
    if (g_dump != nullptr) { fclose(g_dump); g_dump = nullptr; }
    fprintf(g_out, "%d %d %016llx %016llx %llu\n", runner->frameCount, runner->currentRoomIndex,
            (unsigned long long) stateHash, (unsigned long long) g_drawHash, (unsigned long long) g_drawCalls);
#ifdef BS_REACH_GATE
    ReachGate_endFrame(runner);
#endif
#ifdef PLATFORM_3DS
    // Every write to the SD card holds the game up for a while.
    if (runner->frameCount % 64 == 0)
#endif
    {
        fflush(g_out);
        if (g_trace != nullptr) fflush(g_trace);
    }
    g_drawHash = 0;
    g_drawCalls = 0;

    GMLReal speed = Runner_getEffectiveGameSpeed(runner);
    GetTime_advanceVirtualClock(speed > 0.0 ? (uint64_t) (1000000000.0 / speed) : 33333333ull);
}

void StateHash_end(void) {
#ifdef BS_REACH_GATE
    ReachGate_endSession();
#endif
    if (g_trace != nullptr) { fclose(g_trace); g_trace = nullptr; }
    if (g_out != nullptr) {
        fclose(g_out);
        g_out = nullptr;
    }
}

#if defined(UTK_PLAIN_RUNS)
// utkill: whether the transcript wraps the renderer's table. The runner's plain draw runs (utkDrawRoom in
// runner.c) send a run's later draws past the table, so the renderer keeps them off while it does.
__attribute__((noinline, section(".text.late.utkill"))) bool StateHash_wrapping(void) {
    return g_traceOriginal != nullptr;
}

// utkill: whether the hash wraps the table. Runs stay on then, and the runner gives each of a run's
// later draws to StateHash_runDraw: the record w_drawSprite makes of the drawSprite it stands for, so a hashed run
// sees every draw the engine asks for, runs or not.
__attribute__((noinline, section(".text.late.utkill"))) bool StateHash_hashing(void) {
    return g_originalVtable != nullptr;
}

__attribute__((noinline, section(".text.late.utkill"))) void StateHash_runDraw(int32_t tpag, float x, float y, float ox, float oy, float xs,
                                                                             float ys, float a, uint32_t c, float al) {
    if (g_originalVtable == nullptr) return;
    if (!OFFDRAW_SKIP_DRAW(tpag, x, y, xs, ys)) { DH_BEGIN(1); DH_I(tpag); DH_F(x); DH_F(y); DH_F(ox); DH_F(oy); DH_F(xs); DH_F(ys); DH_F(a); DH_I(c); DH_F(al); DH_END(); }
}
#endif
