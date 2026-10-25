// ===[ natsurf6: native-scale game surfaces of any even size ]===
//
// With Native drawing, natsurf makes a game surface native-scale (half resolution, texel i holds logical pixel 2i)
// only when it is exactly the application surface's size. docs/perf/natsurf6-census.md shows the other big surfaces
// (DELTARUNE ch4 titan, ch5 Flower Castle) and that some of them must stay full: buffers drawn out at 2x already
// hold one texel per top-screen pixel. The draws decide, so this learns from them:
//
//   1. A surface of an even size that would take at least NS6_MIN_TEXELS of texture, made by an object that doesn't
//      read surfaces back, starts full and is watched.
//   2. Every draw out of it is classified as the census does (platform/3ds/diag/ctr_surface_census.inc): 1:1 at the
//      right parity onto the half-resolution application surface or a native-scale surface is exact; odd or
//      fractional offsets, scaling, rotation, linear filtering, sampling through surface_get_texture and drawing into
//      a recorded surface are classes that may differ by a native pixel (allowed: they could not be
//      told from full side by side; the wide build, CTRPORT_NATSURF6_WIDE, reads others from natsurf6.txt);
//      a draw at 1.5x or more, into a full surface, a sprite copy or a readback keeps it full.
//   3. Seen drawn out on NS6_SEEN_FRAMES frames with nothing else, it is remade native-scale at the start of the next
//      frame (remakeSurface: nearest copy, so it holds what a native-scale surface made at the start would), and its
//      creator (object and size) is remembered: later surfaces of that creator are native-scale when made.
//
// Late code only (platform/3ds/late_reserve.ld), in its own file: code added to ctr_renderer.c changes how its
// existing functions compile (GCC's inliner shares one budget across a file), so that file only says where things
// are (Natsurf6_refs, Natsurf6_layout, data) and lets nativeSizeFor, moved to late code, say yes to the size in
// Natsurf6_want while this code asks for one. The hooks are an outer vtable, installed after the renderer's other
// wrappers: CtrRenderer_loadFlickerProbe (UNDERTALE) or CtrBtnprompt_setDisplay (DELTARUNE), once per load from main.c,
// is wrapped (CMakeLists.txt).
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "common.h"
#include "ctr_display.h"
#include "ctr_renderer.h"
#include "data_win.h"
#include "log.h"
#include "renderer.h"
#include "runner.h"

#define NS6_TEXT __attribute__((noinline, section(".text.late.natsurf6")))
#define NS6_CONST __attribute__((aligned(4), section(".rodata.natsurf6_in_text")))
#define NS6_MAX 128
#define NS6_CREATORS 64
#ifdef CTRPORT_NATSURF6_WIDE
#define NS6_MIN_TEXELS ((int32_t) ns6.minTexels) // natsurf6.txt's second number, default 256 * 256
#define NS6_SEEN_FRAMES ns6.seenFrames      // its third, default 8
#else
// Step 2: even surfaces of 65536 texels or more, watched 8 frames. Lossless was (NS6_SHADER) after
// 30 frames; it converted nothing in the heavy scenes.
#define NS6_MIN_TEXELS (256 * 256)
#define NS6_SEEN_FRAMES 8
#endif
#define NS6_TRIES 8

// ctr_renderer.c's end: addresses, then offsets in CtrRenderer and CtrSurface, and constants. Keep in step with it.
enum { REF_VTABLE, REF_REMAKE_SURFACE, REF_COUNT };
enum {
    L_SURFACES, L_SURFACE_COUNT, L_SURFACE_SIZE, L_S_USED, L_S_WIDTH, L_S_HEIGHT, L_S_TEX, L_S_SKIP_REC,
    L_S_SKIP_REAL_REFS, L_S_SHIFT, L_S_NATIVE_SCALE, L_S_WANT_NATIVE, L_S_RECORDED, L_S_NO_VRAM, L_DISPLAY, L_TOP_W,
    L_STEREO_DETAIL, L_READERS, L_READER_COUNT, L_BOUND_SURFACE, L_SHADER_FX, L_FILTER_OVERRIDE, L_PROJECTION,
    L_HOST_PASS, L_VIEWPORT, L_FRAME, L_SKIP_ACTIVE, L_SHADER_FX_NONE, L_FILTER_LINEAR, L_TOP_SCREEN_WIDE_W,
    L_MAX_SURFACE_SIZE, L_S_SPILLED, L_SHADER_FX_TOWER, L_S_NATIVE_HELD, L_COUNT
};
extern const void* const Natsurf6_refs[REF_COUNT];
extern const uint32_t Natsurf6_layout[L_COUNT];
int32_t Natsurf6_want[2] __attribute__((section(".bss.late.natsurf6")));

typedef bool (*Ns6RemakeSurface)(void* r, int32_t id, bool native);

#define NS6_AT(p, index, type) (*(type*) ((char*) (p) + Natsurf6_layout[index]))
#define NS6_VTABLE (*(const RendererVtable*) Natsurf6_refs[REF_VTABLE])
// The surface draws go through ctrVtable, whose entries are swapped while a frame is skipped (setSkipWrappers); the
// census build's own wrappers of them (ctr_surface_census.inc) are reached through the entries before ours instead.
#ifdef CTRPORT_SURFACE_CENSUS
#define NS6_DRAWS ns6.prev
#else
#define NS6_DRAWS NS6_VTABLE
#endif

// Classes of a draw out of a watched surface. The low ones may differ by a native pixel; the high ones keep it full.
enum {
    NS6_ODD = 1 << 0,         // integer offset of the wrong parity
    NS6_FRAC = 1 << 1,        // fractional offset
    NS6_SCALE = 1 << 2,       // scaled by less than 1.5
    NS6_ROT = 1 << 3,         // rotated
    NS6_SHADER = 1 << 4,      // through a game shader or a shader effect
    NS6_LINEAR = 1 << 5,      // linear filtering
    NS6_TEXTURE = 1 << 6,     // surface_get_texture
    NS6_TO_RECORDED = 1 << 7, // into a recorded surface (replayed where that one is drawn)
    NS6_SCREENRES = 1 << 8,   // drawn at 1.5x or more: already one texel per screen pixel
    NS6_TO_FULL = 1 << 9,     // into a full surface
    NS6_SPRITE = 1 << 10,     // copied to a sprite
    NS6_READ = 1 << 11,       // read back to the CPU
    NS6_HOST = 1 << 12,       // onto something that isn't a surface
    NS6_TOWER = 1 << 13,      // through the tower shader: its strips (drawTowerStrips) sample by the texture's own size
    NS6_DEPENDS = 1 << 15,    // into a surface still watched: decided with that one
};
// NS6_ONLY_SHORT (wide build's natsurf6.txt only): convert only surfaces that are recorded or spilled for want of
// VRAM, as pmengine's E1 verdict asked.
#define NS6_ONLY_SHORT (1u << 16)
// NS6_SOFT (wide build's natsurf6.txt only): a native-scale surface drawn out off its grid (odd, fractional, scaled or
// rotated) is drawn with linear filtering: soft edges instead of a pixel that jumps (against shimmer).
#define NS6_SOFT (1u << 17)
// NS6_NO_MARK (wide build's natsurf6.txt only): never mark a recorded surface to be native-scale when it gets a texture.
#define NS6_NO_MARK (1u << 18)
// Step 2's classes: a draw out may be off by a native pixel (odd, fractional, a little scaled, rotated, through a shader,
// filtered, read as a texture, into a recorded surface); drawn at 1.5x or more, into a full surface, copied to a sprite,
// read back, drawn to the screen or through the tower shader keeps it full.
#define NS6_ALLOWED_DEFAULT (NS6_ODD | NS6_FRAC | NS6_SCALE | NS6_ROT | NS6_SHADER | NS6_LINEAR | NS6_TEXTURE | NS6_TO_RECORDED)
#ifdef CTRPORT_NATSURF6_WIDE
#define NS6_ALLOWED (ns6.allowed & 0xFFFFu)
#else
#define NS6_ALLOWED NS6_ALLOWED_DEFAULT
#endif

enum { NS6_NONE, NS6_WATCH, NS6_NATIVE, NS6_FULL };
enum { NS6_UNKNOWN, NS6_YES, NS6_NO };

typedef struct {
    uint8_t state;
    uint8_t tries;   // remakes that failed; UINT8_MAX: to be made full again
    uint16_t seen;   // frames it was drawn out on, all allowed
    int16_t creator; // index into ns6.c, -1 none
    uint16_t classes;
    int32_t w, h;
    uint32_t lastFrame;
    uint32_t depFrame; // drawn into a surface still watched on this frame
} Ns6Surface;

typedef struct {
    int32_t obj, w, h;
    uint8_t verdict;
    uint8_t logged;
} Ns6Creator;

static struct {
    RendererVtable outer;
    RendererVtable prev; // the entries before ours (other wrappers'); skip-swapped ones are called through ctrVtable
    bool active;
    uint32_t allowed; // wide build: classes allowed, | NS6_ONLY_SHORT
    uint32_t minTexels, seenFrames;
    uint32_t nCreators;
    uint32_t made, remade, kept, late;
    Ns6Surface s[NS6_MAX];
    Ns6Creator c[NS6_CREATORS];
} ns6 __attribute__((section(".bss.late.natsurf6")));

static const char ns6LogRemade[] NS6_CONST = "Renderer: natsurf6: surface %d (%dx%d) by %s made native-scale at frame %u (%u KB freed)\n";
static const char ns6LogKept[] NS6_CONST = "Renderer: natsurf6: %dx%d surfaces by %s stay full (classes 0x%x)\n";
static const char ns6LogLate[] NS6_CONST = "Renderer: natsurf6: native-scale surface %d (%dx%d) by %s drawn as 0x%x at frame %u; made full again\n";
static const char ns6LogFailed[] NS6_CONST = "Renderer: natsurf6: surface %d (%dx%d) by %s could not be made native-scale; stays full\n";
static const char ns6LogMade[] NS6_CONST = "Renderer: natsurf6: surface %d (%dx%d) by %s made native-scale\n";
static const char ns6NoObject[] NS6_CONST = "?";
static const char ns6LogMarked[] NS6_CONST = "Renderer: natsurf6: recorded surface %d (%dx%d) by %s to be native-scale when it gets a texture\n";
#ifdef CTRPORT_NATSURF6_WIDE
static const char ns6MaskPath[] NS6_CONST = CTRPORT_GAME_DIR "/natsurf6.txt";
static const char ns6MaskRead[] NS6_CONST = "r";
static const char ns6MaskFormat[] NS6_CONST = "%x %u %u";
static const char ns6LogMask[] NS6_CONST = "Renderer: natsurf6 (wide build): classes 0x%x allowed, %u texels or more, after %u frames (%s)\n";
static const char ns6MaskDefault[] NS6_CONST = "default";
#endif

// The renderer's surface id, or nullptr when it isn't one in use.
static NS6_TEXT char* ns6Surface(Renderer* r, int32_t id) {
    if (0 > id || (uint32_t) id >= NS6_AT(r, L_SURFACE_COUNT, uint32_t)) return nullptr;
    char* s = NS6_AT(r, L_SURFACES, char*) + (size_t) id * Natsurf6_layout[L_SURFACE_SIZE];
    return NS6_AT(s, L_S_USED, bool) ? s : nullptr;
}

#define NS6_W(s) NS6_AT(s, L_S_WIDTH, int32_t)
#define NS6_H(s) NS6_AT(s, L_S_HEIGHT, int32_t)
#define NS6_NATIVE_SCALE(s) NS6_AT(s, L_S_NATIVE_SCALE, bool)
#define NS6_RECORDED(s) NS6_AT(s, L_S_RECORDED, bool)
#define NS6_BUSY(s) (NS6_AT(s, L_S_SKIP_REC, void*) != nullptr || NS6_AT(s, L_S_SKIP_REAL_REFS, uint32_t) != 0)
#define NS6_FRAME_OF(r) NS6_AT(r, L_FRAME, uint32_t)

static NS6_TEXT int32_t ns6Pow2(int32_t v) {
    int32_t p = 8;
    while (v > p) p <<= 1;
    return p;
}

static NS6_TEXT bool ns6NativeDisplay(Renderer* r) {
    const CtrDisplaySettings* d = &NS6_AT(r, L_DISPLAY, CtrDisplaySettings);
    return d->drawing == DRAWING_NATIVE && d->topMode == TOP_MODE_STANDARD &&
           NS6_AT(r, L_TOP_W, int32_t) != (int32_t) Natsurf6_layout[L_TOP_SCREEN_WIDE_W];
}

static NS6_TEXT int32_t ns6CurrentObject(Renderer* r) {
    const Runner* runner = r->runner;
    if (runner == nullptr || runner->vmContext == nullptr || runner->vmContext->currentInstance == nullptr) return -1;
    return runner->vmContext->currentInstance->objectIndex;
}

static NS6_TEXT const char* ns6ObjectName(Renderer* r, int32_t obj) {
    const Runner* runner = r->runner;
    if (runner == nullptr || runner->dataWin == nullptr || 0 > obj || (uint32_t) obj >= runner->dataWin->objt.count) return ns6NoObject;
    const char* name = runner->dataWin->objt.objects[obj].name;
    return name != nullptr ? name : ns6NoObject;
}

static NS6_TEXT const char* ns6CreatorName(Renderer* r, int32_t creator) {
    return 0 <= creator ? ns6ObjectName(r, ns6.c[creator].obj) : ns6NoObject;
}

// Whether a surface of this size made now is one to watch: native drawing, no 3D, an even size the GPU can draw to,
// big enough to matter, not the application surface's (natsurf already makes that one native-scale), and not made by
// an object that reads surfaces back (the renderer's surfaceReaders).
static NS6_TEXT bool ns6Eligible(Renderer* r, int32_t w, int32_t h) {
    if (!ns6NativeDisplay(r) || NS6_AT(r, L_STEREO_DETAIL, bool) || r->runner == nullptr) return false;
    int32_t max = (int32_t) Natsurf6_layout[L_MAX_SURFACE_SIZE];
    if (0 >= w || 0 >= h || w % 2 != 0 || h % 2 != 0 || w > max || h > max) return false;
    if (NS6_MIN_TEXELS > ns6Pow2(w) * ns6Pow2(h)) return false;
    const char* app = ns6Surface(r, r->runner->applicationSurfaceId);
    if (app != nullptr && NS6_W(app) == w && NS6_H(app) == h) return false;
    int32_t obj = ns6CurrentObject(r);
    const bool* readers = NS6_AT(r, L_READERS, bool*);
    if (readers != nullptr && 0 <= obj && NS6_AT(r, L_READER_COUNT, uint32_t) > (uint32_t) obj && readers[obj]) return false;
    return true;
}

static NS6_TEXT int32_t ns6Creator(Renderer* r, int32_t w, int32_t h) {
    int32_t obj = ns6CurrentObject(r);
    for (uint32_t i = 0; ns6.nCreators > i; i++) {
        if (ns6.c[i].obj == obj && ns6.c[i].w == w && ns6.c[i].h == h) return (int32_t) i;
    }
    if (ns6.nCreators >= NS6_CREATORS) return -1;
    Ns6Creator* c = &ns6.c[ns6.nCreators];
    c->obj = obj;
    c->w = w;
    c->h = h;
    c->verdict = NS6_UNKNOWN;
    c->logged = 0;
    return (int32_t) ns6.nCreators++;
}

// The watched or native-scale record of surface id, if it still describes that surface.
static NS6_TEXT Ns6Surface* ns6Track(Renderer* r, int32_t id) {
    if (0 > id || id >= NS6_MAX || ns6.s[id].state == NS6_NONE) return nullptr;
    const char* s = ns6Surface(r, id);
    if (s == nullptr || NS6_W(s) != ns6.s[id].w || NS6_H(s) != ns6.s[id].h) {
        ns6.s[id].state = NS6_NONE;
        return nullptr;
    }
    return &ns6.s[id];
}

static NS6_TEXT void ns6Forget(int32_t id) {
    if (0 <= id && id < NS6_MAX) ns6.s[id].state = NS6_NONE;
}

static NS6_TEXT void ns6Keep(Renderer* r, Ns6Surface* t, uint32_t classes) {
    t->state = NS6_FULL;
    ns6.kept++;
    if (0 > t->creator) return;
    Ns6Creator* c = &ns6.c[t->creator];
    c->verdict = NS6_NO;
    if (c->logged) return;
    c->logged = 1;
    logInfo(ns6LogKept, (int) c->w, (int) c->h, ns6ObjectName(r, c->obj), (unsigned) classes);
}

// Starts tracking surface id, just made (or resized) at w x h by the current object; native-scale from the start
// when that creator's surfaces have been before (the caller set Natsurf6_want for the renderer's call).
static NS6_TEXT void ns6Begin(Renderer* r, int32_t id, int32_t creator, uint8_t verdict, int32_t w, int32_t h) {
    if (0 > id || id >= NS6_MAX) return;
    Ns6Surface* t = &ns6.s[id];
    t->state = verdict == NS6_YES ? NS6_NATIVE : NS6_WATCH;
    t->tries = 0;
    t->seen = 0;
    t->creator = (int16_t) creator;
    t->classes = 0;
    t->w = w;
    t->h = h;
    t->lastFrame = UINT32_MAX;
    t->depFrame = UINT32_MAX;
    ns6.active = true;
    if (verdict == NS6_YES) {
        const char* s = ns6Surface(r, id);
        if (s != nullptr && NS6_NATIVE_SCALE(s)) {
            ns6.made++;
            logInfo(ns6LogMade, (int) id, (int) w, (int) h, ns6CreatorName(r, creator));
        }
    }
}

// The class of drawing into the bound target: 0 when it is the half-resolution application surface or a native-scale
// surface.
static NS6_TEXT uint32_t ns6TargetClass(Renderer* r, int32_t source) {
    int32_t b = NS6_AT(r, L_BOUND_SURFACE, int32_t);
    const char* d = ns6Surface(r, b);
    if (d == nullptr || b == source) return NS6_HOST;
    if (b == r->runner->applicationSurfaceId) return NS6_AT(d, L_S_SHIFT, int32_t) == 1 ? 0u : NS6_TO_FULL;
    if (NS6_NATIVE_SCALE(d)) return 0;
    if (NS6_RECORDED(d)) return NS6_TO_RECORDED;
    const Ns6Surface* t = 0 <= b && b < NS6_MAX ? &ns6.s[b] : nullptr;
    if (t != nullptr && t->state == NS6_WATCH && NS6_W(d) == t->w && NS6_H(d) == t->h) return NS6_DEPENDS;
    return NS6_TO_FULL;
}

static NS6_TEXT uint32_t ns6StateClass(Renderer* r) {
    uint32_t c = 0;
    uint8_t fx = NS6_AT(r, L_SHADER_FX, uint8_t);
    if (r->currentShader >= 0 || fx != Natsurf6_layout[L_SHADER_FX_NONE]) c |= NS6_SHADER;
    if (fx == Natsurf6_layout[L_SHADER_FX_TOWER]) c |= NS6_TOWER;
    int8_t filter = NS6_AT(r, L_FILTER_OVERRIDE, int8_t);
    if (filter != 0 ? filter == (int8_t) Natsurf6_layout[L_FILTER_LINEAR] : r->texFilter) c |= NS6_LINEAR;
    return c;
}

// Target pixel of a room point, through the game's projection and the viewport (logical pixels, y down).
static NS6_TEXT void ns6ToTarget(Renderer* r, double x, double y, double* tx, double* ty) {
    const C3D_Mtx* m = &NS6_AT(r, L_PROJECTION, C3D_Mtx);
    const int32_t* vp = &NS6_AT(r, L_VIEWPORT, int32_t); // x, y, w, h
    double cx = (double) m->r[0].x * x + (double) m->r[0].y * y + (double) m->r[0].w;
    double cy = (double) m->r[1].x * x + (double) m->r[1].y * y + (double) m->r[1].w;
    double cw = (double) m->r[3].x * x + (double) m->r[3].y * y + (double) m->r[3].w;
    if (cw == 0.0) cw = 1.0;
    if (NS6_AT(r, L_HOST_PASS, bool)) cy = -cy;
    *tx = (double) vp[0] + (cx / cw + 1.0) * 0.5 * (double) vp[2];
    *ty = (double) vp[1] + (1.0 - cy / cw) * 0.5 * (double) vp[3];
}

static NS6_TEXT bool ns6Near(double a, double b) {
    return fabs(a - b) < 1e-3;
}

// One axis, target t = a u + c for source pixel u: a native-scale target's texel I samples t = 2I + 0.5, the full
// source gives pixel floor(u) there and a native-scale source its even neighbour: the same for every I when |a| = 1
// and c is an integer, even for a = 1 and odd for a = -1.
static NS6_TEXT uint32_t ns6Axis(double a, double c) {
    if (!ns6Near(fabs(a), 1.0)) return NS6_SCALE;
    double ci = floor(c + 0.5);
    if (!ns6Near(c, ci)) return NS6_FRAC;
    bool even = ((long long) ci & 1) == 0;
    return (a > 0.0 ? even : !even) ? 0u : NS6_ODD;
}

// Class of a quad whose corners 0, 1 and 3 (room coordinates) show source pixels (u0, v0), (u1, v0) and (u0, v1).
static NS6_TEXT uint32_t ns6QuadClass(Renderer* r, const double* px, const double* py, double u0, double v0, double u1, double v1) {
    double x[4], y[4];
    for (int i = 0; 4 > i; i++) ns6ToTarget(r, px[i], py[i], &x[i], &y[i]);
    double du = u1 - u0, dv = v1 - v0;
    if (du == 0.0) du = 1.0;
    if (dv == 0.0) dv = 1.0;
    double ax = (x[1] - x[0]) / du, ay = (y[1] - y[0]) / du;
    double bx = (x[3] - x[0]) / dv, by = (y[3] - y[0]) / dv;
    if (ax * ax + ay * ay >= 2.25 || bx * bx + by * by >= 2.25) return NS6_SCREENRES;
    if (!ns6Near(bx, 0.0) || !ns6Near(ay, 0.0)) return NS6_ROT;
    return ns6Axis(ax, x[0] - ax * u0) | ns6Axis(by, y[0] - by * v0);
}

static NS6_TEXT void ns6Observe(Renderer* r, int32_t id, uint32_t classes) {
    Ns6Surface* t = ns6Track(r, id);
    if (t == nullptr || t->state == NS6_FULL) return;
    uint32_t frame = NS6_FRAME_OF(r);
    if (classes & NS6_DEPENDS) {
        t->depFrame = frame;
        return;
    }
    t->classes |= (uint16_t) classes;
    if (t->state == NS6_WATCH) {
        if (classes & ~(uint32_t) NS6_ALLOWED) {
            ns6Keep(r, t, classes);
        } else if (t->lastFrame != frame) {
            t->lastFrame = frame;
            if (t->seen != UINT16_MAX) t->seen++;
        }
        return;
    }
    // Native-scale already: a draw it can't show exactly is shown at half detail this once; it is made full at the
    // start of the next frame and the creator's later surfaces stay full.
    if (classes & ~(uint32_t) NS6_ALLOWED) {
        ns6.late++;
        logInfo(ns6LogLate, (int) id, (int) t->w, (int) t->h, ns6CreatorName(r, t->creator), (unsigned) classes, (unsigned) frame);
        t->state = NS6_FULL;
        t->tries = UINT8_MAX;
        if (0 <= t->creator) ns6.c[t->creator].verdict = NS6_NO;
    }
}

static NS6_TEXT bool ns6ObserveDraw(Renderer* r, int32_t id, int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight, float x, float y,
                                    float xscale, float yscale, float angleDeg) {
    if (!ns6.active || 0 > id || id >= NS6_MAX || ns6.s[id].state == NS6_NONE || ns6.s[id].state == NS6_FULL) return false;
    const char* s = ns6Surface(r, id);
    if (s == nullptr) return false;
    if (0 > srcWidth) {
        srcLeft = 0;
        srcTop = 0;
        srcWidth = NS6_W(s);
        srcHeight = NS6_H(s);
    }
    double c = 1.0, sn = 0.0;
    if (angleDeg != 0.0f) {
        double rad = (double) angleDeg * (M_PI / 180.0);
        c = cos(rad);
        sn = sin(rad);
    }
    // GameMaker's draw transform: scale, then rotate counter-clockwise (y down), then translate.
    double w = (double) srcWidth * (double) xscale, h = (double) srcHeight * (double) yscale;
    double px[4] = { x, x + w * c, 0.0, x + h * sn };
    double py[4] = { y, y - w * sn, 0.0, y + h * c };
    uint32_t classes = ns6QuadClass(r, px, py, (double) srcLeft, (double) srcTop, (double) (srcLeft + srcWidth), (double) (srcTop + srcHeight));
    ns6Observe(r, id, classes | ns6StateClass(r) | ns6TargetClass(r, id));
#ifdef CTRPORT_NATSURF6_WIDE
    return (ns6.allowed & NS6_SOFT) && ns6.s[id].state == NS6_NATIVE && NS6_NATIVE_SCALE(s) && (classes & (NS6_ODD | NS6_FRAC | NS6_SCALE | NS6_ROT));
#else
    return false;
#endif
}

// Draws with linear filtering when soft (ns6ObserveDraw), the renderer's own filter choice restored after.
#define NS6_SOFTLY(r, soft, call)                                                                                    \
    do {                                                                                                             \
        int8_t* f_ = &NS6_AT(r, L_FILTER_OVERRIDE, int8_t);                                                          \
        int8_t old_ = *f_;                                                                                           \
        if (soft) *f_ = (int8_t) Natsurf6_layout[L_FILTER_LINEAR];                                                   \
        call;                                                                                                        \
        *f_ = old_;                                                                                                  \
    } while (0)

// Remakes the surfaces that are ready, and makes full again the ones a draw found wrong; at the start of a frame,
// before the renderer's beginFrame binds the application surface.
static NS6_TEXT void ns6Frame(Renderer* r) {
    Ns6RemakeSurface remake = (Ns6RemakeSurface) Natsurf6_refs[REF_REMAKE_SURFACE];
    bool native = ns6NativeDisplay(r) && !NS6_AT(r, L_STEREO_DETAIL, bool);
    uint32_t frame = NS6_FRAME_OF(r);
    bool any = false;
    for (int32_t id = 0; NS6_MAX > id; id++) {
        Ns6Surface* t = &ns6.s[id];
        if (t->state == NS6_NONE) continue;
        char* s = ns6Surface(r, id);
        if (s == nullptr || NS6_W(s) != t->w || NS6_H(s) != t->h) {
            t->state = NS6_NONE;
            continue;
        }
        any = true;
        NS6_AT(s, L_S_NATIVE_HELD, bool) = false;
        if (t->state == NS6_FULL) {
            if (t->tries == UINT8_MAX && NS6_NATIVE_SCALE(s) && !NS6_RECORDED(s) && !NS6_BUSY(s)) {
                t->tries = 0;
                remake(r, id, false);
            }
            continue;
        }
        if (!native || NS6_NATIVE_SCALE(s) || NS6_BUSY(s)) continue;
        if (0 <= t->creator && ns6.c[t->creator].verdict == NS6_NO) {
            t->state = NS6_FULL; // another surface of its creator met a draw it can't show
            continue;
        }
#ifdef CTRPORT_NATSURF6_WIDE
        if ((ns6.allowed & NS6_ONLY_SHORT) && !NS6_RECORDED(s) && !NS6_AT(s, L_S_SPILLED, bool)) continue;
#endif
        if (t->state == NS6_WATCH && (t->seen < NS6_SEEN_FRAMES || t->depFrame == frame - 1 || t->depFrame == frame)) continue;
        if (NS6_RECORDED(s)) {
#ifdef CTRPORT_NATSURF6_WIDE
            if (ns6.allowed & NS6_NO_MARK) continue;
#else
            // Step 2 leaves it recorded: marked, Titan's four 640x640 climb_explosion surfaces got native textures in
            // linear memory where main replays them, GPU +1.0 ms and 2.6 MB less linear memory (Azahar New, 10-22).
            continue;
#endif
            // Recorded for want of VRAM: a clear or a bind may give it a texture (promoteRecorded), native-scale with
            // our size asked for then (NS6_WANT_FOR).
            if (NS6_AT(s, L_S_NO_VRAM, bool)) NS6_AT(s, L_S_WANT_NATIVE, bool) = true;
            logInfo(ns6LogMarked, (int) id, (int) t->w, (int) t->h, ns6CreatorName(r, t->creator));
            t->state = NS6_NATIVE;
            if (0 <= t->creator) ns6.c[t->creator].verdict = NS6_YES;
            continue;
        }
        if (t->tries >= NS6_TRIES) continue;
        size_t before = NS6_AT(s, L_S_TEX, C3D_Tex).size;
        Natsurf6_want[0] = t->w;
        Natsurf6_want[1] = t->h;
        bool ok = remake(r, id, true);
        Natsurf6_want[0] = Natsurf6_want[1] = 0;
        if (!ok) {
            if (++t->tries == NS6_TRIES) logInfo(ns6LogFailed, (int) id, (int) t->w, (int) t->h, ns6CreatorName(r, t->creator));
            continue;
        }
        ns6.remade++;
        s = ns6Surface(r, id);
        size_t after = s != nullptr ? NS6_AT(s, L_S_TEX, C3D_Tex).size : 0;
        logInfo(ns6LogRemade, (int) id, (int) t->w, (int) t->h, ns6CreatorName(r, t->creator), (unsigned) frame,
                (unsigned) ((before > after ? before - after : 0) / 1024u));
        t->state = NS6_NATIVE;
        if (0 <= t->creator) ns6.c[t->creator].verdict = NS6_YES;
    }
    ns6.active = any;
}

// A recorded surface wanting native scale gets its texture at a clear or a bind (promoteRecorded): our size then.
#define NS6_WANT_FOR(r, id, call)                                                                                     \
    do {                                                                                                             \
        const Ns6Surface* t_ = ns6.active ? ns6Track((r), (id)) : nullptr;                                          \
        if (t_ != nullptr && t_->state == NS6_NATIVE) {                                                             \
            Natsurf6_want[0] = t_->w;                                                                               \
            Natsurf6_want[1] = t_->h;                                                                               \
        }                                                                                                            \
        call;                                                                                                        \
        Natsurf6_want[0] = Natsurf6_want[1] = 0;                                                                    \
    } while (0)

static NS6_TEXT void ns6BeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    if (ns6.active && !NS6_AT(renderer, L_SKIP_ACTIVE, bool)) ns6Frame(renderer);
    ns6.prev.beginFrame(renderer, gameW, gameH, windowW, windowH);
}

static NS6_TEXT int32_t ns6CreateSurface(Renderer* renderer, int32_t width, int32_t height) {
    if (!ns6Eligible(renderer, width, height)) {
        int32_t id = ns6.prev.createSurface(renderer, width, height);
        ns6Forget(id);
        return id;
    }
    int32_t creator = ns6Creator(renderer, width, height);
    uint8_t verdict = 0 <= creator ? ns6.c[creator].verdict : NS6_NO;
    if (verdict == NS6_YES) {
        Natsurf6_want[0] = width;
        Natsurf6_want[1] = height;
    }
    int32_t id = ns6.prev.createSurface(renderer, width, height);
    Natsurf6_want[0] = Natsurf6_want[1] = 0;
    if (verdict == NS6_NO) {
        ns6Forget(id);
        return id;
    }
    ns6Begin(renderer, id, creator, verdict, width, height);
    return id;
}

static NS6_TEXT void ns6SurfaceResize(Renderer* renderer, int32_t surfaceId, int32_t width, int32_t height) {
    const char* s = ns6Surface(renderer, surfaceId);
    if (s == nullptr || (NS6_W(s) == width && NS6_H(s) == height) || surfaceId == renderer->runner->applicationSurfaceId ||
        !ns6Eligible(renderer, width, height)) {
        if (s != nullptr && (NS6_W(s) != width || NS6_H(s) != height)) ns6Forget(surfaceId);
        ns6.prev.surfaceResize(renderer, surfaceId, width, height);
        return;
    }
    int32_t creator = ns6Creator(renderer, width, height);
    uint8_t verdict = 0 <= creator ? ns6.c[creator].verdict : NS6_NO;
    if (verdict == NS6_YES) {
        Natsurf6_want[0] = width;
        Natsurf6_want[1] = height;
    }
    ns6.prev.surfaceResize(renderer, surfaceId, width, height);
    Natsurf6_want[0] = Natsurf6_want[1] = 0;
    ns6Forget(surfaceId);
    if (verdict != NS6_NO && ns6Surface(renderer, surfaceId) != nullptr) ns6Begin(renderer, surfaceId, creator, verdict, width, height);
}

static NS6_TEXT void ns6SurfaceFree(Renderer* renderer, int32_t surfaceId) {
    ns6Forget(surfaceId);
    ns6.prev.surfaceFree(renderer, surfaceId);
}

static NS6_TEXT bool ns6SetRenderTarget(Renderer* renderer, int32_t surfaceId, bool implicitApplicationSurface) {
    bool ok;
    NS6_WANT_FOR(renderer, surfaceId, ok = ns6.prev.setRenderTarget(renderer, surfaceId, implicitApplicationSurface));
    return ok;
}

static NS6_TEXT void ns6ClearScreen(Renderer* renderer, uint32_t color, float alpha) {
    NS6_WANT_FOR(renderer, NS6_AT(renderer, L_BOUND_SURFACE, int32_t), NS6_DRAWS.clearScreen(renderer, color, alpha));
}

static NS6_TEXT void ns6DrawSurface(Renderer* renderer, int32_t surfaceId, int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight, float x,
                                    float y, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    bool soft = ns6ObserveDraw(renderer, surfaceId, srcLeft, srcTop, srcWidth, srcHeight, x, y, xscale, yscale, angleDeg);
    NS6_SOFTLY(renderer, soft, NS6_DRAWS.drawSurface(renderer, surfaceId, srcLeft, srcTop, srcWidth, srcHeight, x, y, xscale, yscale, angleDeg, color, alpha));
}

static NS6_TEXT void ns6DrawSurfaceColor(Renderer* renderer, int32_t surfaceId, int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight,
                                         float x, float y, float xscale, float yscale, float angleDeg, uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4,
                                         float alpha) {
    bool soft = ns6ObserveDraw(renderer, surfaceId, srcLeft, srcTop, srcWidth, srcHeight, x, y, xscale, yscale, angleDeg);
    NS6_SOFTLY(renderer, soft, NS6_DRAWS.drawSurfaceColor(renderer, surfaceId, srcLeft, srcTop, srcWidth, srcHeight, x, y, xscale, yscale, angleDeg, c1, c2, c3, c4, alpha));
}

static NS6_TEXT void ns6DrawSurfaceTiled(Renderer* renderer, int32_t surfaceId, float x, float y, float xscale, float yscale, float roomW, float roomH,
                                         uint32_t color, float alpha) {
    // Every tile has the first one's parity when the surface's size is even (it is, to be watched).
    bool soft = ns6ObserveDraw(renderer, surfaceId, -1, -1, -1, -1, x, y, xscale, yscale, 0.0f);
    NS6_SOFTLY(renderer, soft, NS6_DRAWS.drawSurfaceTiled(renderer, surfaceId, x, y, xscale, yscale, roomW, roomH, color, alpha));
}

static NS6_TEXT void ns6SurfaceCopy(Renderer* renderer, int32_t destSurfaceID, int32_t destX, int32_t destY, int32_t srcSurfaceID, int32_t srcX, int32_t srcY,
                                    int32_t srcW, int32_t srcH, bool part) {
    if (ns6.active && ns6Track(renderer, srcSurfaceID) != nullptr) {
        int32_t dx = destX - (part ? srcX : 0), dy = destY - (part ? srcY : 0);
        uint32_t classes = ((dx | dy) & 1) ? NS6_ODD : 0u;
        const char* d = ns6Surface(renderer, destSurfaceID);
        if (d == nullptr) classes |= NS6_HOST;
        else if (!(NS6_NATIVE_SCALE(d) || (destSurfaceID == renderer->runner->applicationSurfaceId && NS6_AT(d, L_S_SHIFT, int32_t) == 1)))
            classes |= NS6_TO_FULL;
        ns6Observe(renderer, srcSurfaceID, classes);
    }
    NS6_DRAWS.surfaceCopy(renderer, destSurfaceID, destX, destY, srcSurfaceID, srcX, srcY, srcW, srcH, part);
}

static NS6_TEXT int32_t ns6CreateSpriteFromSurface(Renderer* renderer, int32_t surfaceID, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback,
                                                   bool smooth, int32_t xorig, int32_t yorig) {
    if (ns6.active) ns6Observe(renderer, surfaceID, NS6_SPRITE);
    return NS6_DRAWS.createSpriteFromSurface(renderer, surfaceID, x, y, w, h, removeback, smooth, xorig, yorig);
}

static NS6_TEXT uint32_t ns6SurfaceGetTexture(Renderer* renderer, int32_t surfaceID) {
    if (ns6.active) ns6Observe(renderer, surfaceID, NS6_TEXTURE);
    return NS6_DRAWS.surfaceGetTexture(renderer, surfaceID);
}

static NS6_TEXT bool ns6SurfaceGetPixels(Renderer* renderer, int32_t surfaceID, uint8_t* outRGBA) {
    if (ns6.active) ns6Observe(renderer, surfaceID, NS6_READ);
    return ns6.prev.surfaceGetPixels(renderer, surfaceID, outRGBA);
}

#ifdef CTRPORT_NATSURF6_WIDE
// Wide build only (afterimage churn): what surface_create and surface_free cost on the console,
// measured around the whole renderer call (the inner create's VRAM or linear placement, clear and log line), and the
// renderer's beginFrame in frames after a free (its deferred frees, and any VRAM move they lead to) against the others.
// Logged every 32 creates of 640x480 or more.
static struct {
    uint32_t n[4], max[4];
    uint64_t sum[4];
    uint32_t freedSince, bigCreates;
} ns6Cost __attribute__((section(".bss.late.natsurf6")));
static const char ns6LogCost[] NS6_CONST =
    "natsurf6 cost: create %lu avg %lu us max %lu; free %lu avg %lu us max %lu; beginFrame after a free %lu avg %lu us max %lu, others %lu avg %lu us max %lu\n";

static NS6_TEXT void ns6CostAdd(int k, uint64_t t0) {
    uint32_t us = (uint32_t) ((svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000000));
    ns6Cost.n[k]++;
    ns6Cost.sum[k] += us;
    if (us > ns6Cost.max[k]) ns6Cost.max[k] = us;
}

static NS6_TEXT unsigned long ns6CostAvg(int k) {
    return ns6Cost.n[k] ? (unsigned long) (ns6Cost.sum[k] / ns6Cost.n[k]) : 0ul;
}

static NS6_TEXT int32_t ns6CostCreate(Renderer* renderer, int32_t width, int32_t height) {
    uint64_t t0 = svcGetSystemTick();
    int32_t id = ns6CreateSurface(renderer, width, height);
    ns6CostAdd(0, t0);
    if (width * height >= 640 * 480 && ++ns6Cost.bigCreates % 32 == 0) {
        logInfo(ns6LogCost, (unsigned long) ns6Cost.n[0], ns6CostAvg(0), (unsigned long) ns6Cost.max[0], (unsigned long) ns6Cost.n[1],
                ns6CostAvg(1), (unsigned long) ns6Cost.max[1], (unsigned long) ns6Cost.n[2], ns6CostAvg(2), (unsigned long) ns6Cost.max[2],
                (unsigned long) ns6Cost.n[3], ns6CostAvg(3), (unsigned long) ns6Cost.max[3]);
    }
    return id;
}

static NS6_TEXT void ns6CostFree(Renderer* renderer, int32_t surfaceId) {
    uint64_t t0 = svcGetSystemTick();
    ns6SurfaceFree(renderer, surfaceId);
    ns6CostAdd(1, t0);
    ns6Cost.freedSince = 1;
}

static NS6_TEXT void ns6CostBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    uint64_t t0 = svcGetSystemTick();
    ns6BeginFrame(renderer, gameW, gameH, windowW, windowH);
    ns6CostAdd(ns6Cost.freedSince ? 2 : 3, t0);
    ns6Cost.freedSince = 0;
}
#endif

static NS6_TEXT void ns6Install(Renderer* renderer) {
    memset(ns6.s, 0, sizeof(ns6.s));
    ns6.nCreators = 0;
    ns6.active = false;
    ns6.made = ns6.remade = ns6.kept = ns6.late = 0;
#ifdef CTRPORT_NATSURF6_WIDE
    ns6.allowed = NS6_ALLOWED_DEFAULT;
    ns6.minTexels = 256 * 256;
    ns6.seenFrames = 8;
    const char* from = ns6MaskDefault;
    FILE* f = fopen(ns6MaskPath, ns6MaskRead);
    if (f != nullptr) {
        unsigned mask, texels = 256 * 256, frames = 8;
        if (fscanf(f, ns6MaskFormat, &mask, &texels, &frames) >= 1) {
            ns6.allowed = mask;
            ns6.minTexels = texels;
            ns6.seenFrames = frames;
            from = ns6MaskPath;
        }
        fclose(f);
    }
    logInfo(ns6LogMask, (unsigned) ns6.allowed, (unsigned) ns6.minTexels, (unsigned) ns6.seenFrames, from);
#endif
    Natsurf6_want[0] = Natsurf6_want[1] = 0;
    if (renderer->vtable == &ns6.outer) return;
    ns6.prev = *renderer->vtable;
    ns6.outer = *renderer->vtable;
    ns6.outer.beginFrame = ns6BeginFrame;
    ns6.outer.createSurface = ns6CreateSurface;
    ns6.outer.surfaceResize = ns6SurfaceResize;
    ns6.outer.surfaceFree = ns6SurfaceFree;
    ns6.outer.setRenderTarget = ns6SetRenderTarget;
    ns6.outer.clearScreen = ns6ClearScreen;
    ns6.outer.drawSurface = ns6DrawSurface;
    ns6.outer.drawSurfaceColor = ns6DrawSurfaceColor;
    ns6.outer.drawSurfaceTiled = ns6DrawSurfaceTiled;
    ns6.outer.surfaceCopy = ns6SurfaceCopy;
    ns6.outer.createSpriteFromSurface = ns6CreateSpriteFromSurface;
    ns6.outer.surfaceGetTexture = ns6SurfaceGetTexture;
    ns6.outer.surfaceGetPixels = ns6SurfaceGetPixels;
#ifdef CTRPORT_NATSURF6_WIDE
    memset(&ns6Cost, 0, sizeof(ns6Cost));
    ns6.outer.createSurface = ns6CostCreate;
    ns6.outer.surfaceFree = ns6CostFree;
    ns6.outer.beginFrame = ns6CostBeginFrame;
#endif
    renderer->vtable = &ns6.outer;
}

#ifdef ENABLE_WAD17
// DELTARUNE: cmdlabel wraps CtrRenderer_loadFlickerProbe (a symbol can be wrapped once), so ours goes in at the next
// call main.c makes once per load, CtrBtnprompt_setDisplay, before it runs (where it went in before). Only init is
// patched in between (pixfix_hpname, at the display-caps query, in place); our table copies it, so the chain is the same.
// boxedge hooks the same call (ctr_boxedge.c's Boxedge_setDisplay, which calls the real one): ours calls it, so its
// drawSprite hook goes in on our table, after every other one, as before (integrate12).
void Boxedge_setDisplay(Renderer* renderer, const CtrDisplaySettings* settings);
void __wrap_CtrBtnprompt_setDisplay(Renderer* renderer, const CtrDisplaySettings* settings);

NS6_TEXT void __wrap_CtrBtnprompt_setDisplay(Renderer* renderer, const CtrDisplaySettings* settings) {
    ns6Install(renderer);
    Boxedge_setDisplay(renderer, settings);
}
#else
bool __real_CtrRenderer_loadFlickerProbe(Renderer* renderer, const char* path);
bool __wrap_CtrRenderer_loadFlickerProbe(Renderer* renderer, const char* path);

// main.c calls it once per load, after CtrRenderer_create and CtrRenderer_setSurfaceReaders (whose wrappers have put
// their tables in front by then); UNDERTALE's uthud_snap patches the table in front (ours) later, keeping ours.
NS6_TEXT bool __wrap_CtrRenderer_loadFlickerProbe(Renderer* renderer, const char* path) {
    bool loaded = __real_CtrRenderer_loadFlickerProbe(renderer, path);
    ns6Install(renderer);
    return loaded;
}
#endif
