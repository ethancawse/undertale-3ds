// view_snap (uthud, made UNDERTALE's default by pixfix): at native drawing, the view's offset rounds to whole
// output pixels. Late code only (platform/3ds/late_reserve.ld): nothing here moves existing code or data.
//
// At native drawing (an application surface with shift > 0) 2^shift logical pixels share one output pixel. A view
// offset that isn't a multiple of that puts every edge on a pixel centre, and each draw then snaps one way or the
// other on its own. With the
// switch on, the projection's translation moves to a whole output pixel, so the picture moves rigidly.
// Only the render transform changes: view_xview, view_yview, cameras and instances keep their values. The snapped
// matrix is the renderer's gmlMatrices[MATRIX_PROJECTION], so matrix_get(matrix_projection) and
// matrix_get(matrix_world_view_projection) return it. UNDERTALE's 320x240 views on its 640x480 surface already move in
// whole output pixels; its 640x480 views (the battles) are the ones a 1 px offset splits.
//
// How it is reached without changing any existing function (docs/3ds-hardware.md, "Late code goes in a new file or
// at a file's end"): CtrRenderer_getDisplayCaps is wrapped (CMakeLists.txt; screens-port already wraps
// CtrRenderer_create, and a symbol has one wrapper). main.c calls CtrRenderer_getDisplayCaps right after each
// CtrRenderer_create. Once the switch is on, the renderer's vtable entries beginView, applyProjection and
// setRenderTarget point at hooks here. Each runs the original, then moves the projection's translation and hands it,
// with the current view, to the original applyProjection. That path recomputes the world-view-projection and re-runs
// setProjectionFromEngine, so the GPU and the CPU users of the projection (culling, smart grid mapping) agree. With the switch off, the vtable is never touched.
// ctr_renderer.c ends with UthudSnap_layout, the offsets of the private CtrRenderer fields read here.
//
// The switch: UNDERTALE defaults to 2. A "view_snap = N" line in settings.ini overrides it; the settings menu neither
// shows nor saves the key. 1 rounds an odd view offset down to the next lower even view (a +1 step shows the resting
// picture), 2 rounds it up (a +1 step shows the +2 picture, so a view offset of +2, 0, +1, 0 becomes a whole-pixel
// -1, 0, -1, 0 on screen), 0 is off. DELTARUNE builds leave all of this out (CMakeLists.txt wraps for UNDERTALE only)
// and ignore the key: there the snap makes walking characters and scrolling text jitter.
//
// The UNDERTALE 1.0 late reserve is tight, so this file is compiled with -Os (CMakeLists.txt) and kept small.

#include <stdint.h>
#include <stdio.h>

#include "common.h"
#include "ctr_display.h"
#include "ctr_renderer.h"
#include "log.h"
#include "matrix_math.h"
#include "renderer.h"
#include "runner.h"

#define UTHUD_TEXT __attribute__((noinline, section(".text.late.uthud")))

#ifndef ENABLE_WAD17

#define UTHUD_SNAP_DEFAULT 2

// Offsets in CtrRenderer and CtrSurface (end of ctr_renderer.c).
enum { LAYOUT_BOUND_SURFACE, LAYOUT_SURFACES, LAYOUT_SURFACE_SIZE, LAYOUT_SURFACE_SHIFT, LAYOUT_VIEWPORT, LAYOUT_HOST_PASS };
extern const uint32_t UthudSnap_layout[6];
#define UTHUD_FIELD(base, index, type) (*(const type*) ((const char*) (base) + UthudSnap_layout[index]))

typedef struct {
    void (*beginView)(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, int32_t portX, int32_t portY,
                      int32_t portW, int32_t portH, float viewAngle);
    void (*applyProjection)(Renderer* renderer, const Matrix4f* viewMatrix, const Matrix4f* projectionMatrix);
    bool (*setRenderTarget)(Renderer* renderer, int32_t surfaceID, bool implicitApplicationSurface);
} UthudSnapHooks;

// A float's bits (exact tests in integer code: +0 and -0 have no bits but the sign, 1.0f has one pattern).
typedef uint32_t __attribute__((may_alias)) UthudSnapBits;
#define UTHUD_ONE_BITS 0x3f800000u

// One block, so each function finds it all from one address. Global, not static: uthudSnapBeginView's assembly names
// it and UthudSnap_resnap, and reads saved at 0 and original at 8. Whole words: .bss ends the data segment, which
// must end on a word.
typedef struct {
    uint32_t saved[2]; // the renderer and the return address while an original runs (uthudSnapBeginView)
    UthudSnapHooks original;
    uint32_t on; // the switch: 0 off, 2 round up, anything else round down
} UthudSnapState;
__attribute__((aligned(4), section(".bss.late.uthud"))) UthudSnapState UthudSnap_state;

static inline __attribute__((always_inline)) float uthudSnapFloor(float x) {
    int32_t i = (int32_t) x;
    return (float) ((float) i > x ? i - 1 : i);
}

// Moves the view's offset (the translation of projection x view) to whole output pixels of the application surface.
// Odd offsets all round the same way: with view_snap = 1 the bias shows the next lower even view, with 2 the next
// higher one.
void UthudSnap_resnap(Renderer* renderer);
UTHUD_TEXT void UthudSnap_resnap(Renderer* renderer) {
    if (!UthudSnap_state.on || renderer->runner == nullptr) return;
    int32_t bound = UTHUD_FIELD(renderer, LAYOUT_BOUND_SURFACE, int32_t);
    if (0 > bound || bound != renderer->runner->applicationSurfaceId) return;
    const char* surfaces = UTHUD_FIELD(renderer, LAYOUT_SURFACES, char*);
    int32_t shift = UTHUD_FIELD(surfaces + (size_t) bound * UthudSnap_layout[LAYOUT_SURFACE_SIZE], LAYOUT_SURFACE_SHIFT, int32_t);
    const int32_t* viewport = &UTHUD_FIELD(renderer, LAYOUT_VIEWPORT, int32_t); // x, y, w, h
    if (0 >= shift) return;
    bool hostPass = UTHUD_FIELD(renderer, LAYOUT_HOST_PASS, bool);
    const float* p = renderer->gmlMatrices[MATRIX_PROJECTION].m;
    const float* v = renderer->gmlMatrices[MATRIX_VIEW].m;
    const UthudSnapBits* pb = (const UthudSnapBits*) p;
    const UthudSnapBits* vb = (const UthudSnapBits*) v;
    // Column-major (m[column * 4 + row]). Only an orthographic projection of an affine, unrotated view is snapped:
    // bottom rows (0, 0, 0, 1) exactly, and no x-y coupling in projection x view.
    if (((pb[3] | pb[7] | pb[11] | vb[3] | vb[7] | vb[11]) << 1 | (pb[15] ^ UTHUD_ONE_BITS) | (vb[15] ^ UTHUD_ONE_BITS)) != 0)
        return;
    float s = (float) (1 << shift);
    float bias = UthudSnap_state.on == 2 ? 0.484375f : 0.515625f;
    float d[2];
    bool moved = false;
    // Per axis: where the room origin lands in output pixels (pos), and the move (d, in clip units) that puts it on
    // the biased floor of a whole output pixel.
    // The loop stays rolled (the empty asm hides its count from GCC, which would otherwise peel it): code size.
    int axes = 2;
    __asm__("" : "+r"(axes));
    for (int axis = 0; axes > axis; axis++) {
        const float* row = p + axis; // row[0], row[4], row[8], row[12]: the projection's row for this axis
        const float* other = v + 4 * (1 - axis); // the view's column for the other axis
        if (row[0] * other[0] + row[4] * other[1] + row[8] * other[2] != 0.0f) return; // rotated
        float t = row[0] * v[12] + row[4] * v[13] + row[8] * v[14] + row[12];
        // Output y runs down; setProjectionFromEngine negates Y on host passes, which already run down.
        bool flip = axis == 1 && !hostPass;
        if (flip) t = -t;
        if (0 >= viewport[2 + axis]) return; // an empty viewport draws nothing
        float half = (float) viewport[2 + axis] * 0.5f;
        float pos = (float) viewport[axis] + (1.0f + t) * half;
        float e = (uthudSnapFloor(pos / s + bias) * s - pos) / half;
        d[axis] = flip ? -e : e;
        // Already on whole pixels, to float rounding (an output pixel is 2^shift * 2 / viewport size in clip units)?
        if (__builtin_fabsf(e) >= 1e-4f) moved = true;
    }
    if (!moved) return;
    // The bottom row is (0, 0, 0, 1), so moving the clip-space result by (dx, dy) moves the translation column only.
    // ctrApplyProjection (Renderer_applyProjection copies both matrices before it stores any) then recomputes the
    // world-view and world-view-projection, as setMatrix would, without a by-value matrix argument.
    Matrix4f* projection = &renderer->gmlMatrices[MATRIX_PROJECTION];
    projection->m[12] += d[0];
    projection->m[13] += d[1];
    UthudSnap_state.original.applyProjection(renderer, &renderer->gmlMatrices[MATRIX_VIEW], projection);
}

// beginView has ten arguments, six of them on the stack. Forwarding them in C copies each one; this keeps the stack as
// the caller left it, so the original reads them in place: the renderer and the return address wait in
// UthudSnap_state.saved (one render thread, and neither original calls a vtable entry), and the snap runs as a tail
// call. UthudSnap_applyProjection, the applyProjection hook, is a second entry that shares the tail and the literal.
// UthudSnap_setRenderTarget, the setRenderTarget hook, shares the literal: its arguments are all in registers, so it
// keeps the renderer and the return address on the stack, then swaps the original's result in for the renderer around
// the snap (8 B less than the same in C, for the UNDERTALE 1.0 reserve).
// Reached only through the vtable, so no caller is compiled against their register use (naked
// stubs).
_Static_assert(__builtin_offsetof(UthudSnapState, saved) == 0 && __builtin_offsetof(UthudSnapState, original.beginView) == 8 &&
                   __builtin_offsetof(UthudSnapState, original.applyProjection) == 12 &&
                   __builtin_offsetof(UthudSnapState, original.setRenderTarget) == 16,
               "uthudSnapBeginView's offsets");
void UthudSnap_applyProjection(Renderer* renderer, const Matrix4f* viewMatrix, const Matrix4f* projectionMatrix);
bool UthudSnap_setRenderTarget(Renderer* renderer, int32_t surfaceID, bool implicitApplicationSurface);
static __attribute__((naked, noinline, section(".text.late.uthud"))) void uthudSnapBeginView(
    MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t viewX, MAYBE_UNUSED int32_t viewY, MAYBE_UNUSED int32_t viewW,
    MAYBE_UNUSED int32_t viewH, MAYBE_UNUSED int32_t portX, MAYBE_UNUSED int32_t portY, MAYBE_UNUSED int32_t portW,
    MAYBE_UNUSED int32_t portH, MAYBE_UNUSED float viewAngle) {
    __asm__("ldr ip, =UthudSnap_state\n\t"
            "stmia ip, {r0, lr}\n\t"
            "ldr ip, [ip, #8]\n\t" // original.beginView
            ".Luthud_snap_call:\n\t"
            "blx ip\n\t"
            "ldr ip, =UthudSnap_state\n\t"
            "ldmia ip, {r0, lr}\n\t"
            "b UthudSnap_resnap\n\t"
            ".global UthudSnap_applyProjection\n\t"
            ".type UthudSnap_applyProjection, %function\n\t"
            "UthudSnap_applyProjection:\n\t"
            "ldr ip, =UthudSnap_state\n\t"
            "stmia ip, {r0, lr}\n\t"
            "ldr ip, [ip, #12]\n\t" // original.applyProjection
            "b .Luthud_snap_call\n\t"
            ".size UthudSnap_applyProjection, . - UthudSnap_applyProjection\n\t"
            ".global UthudSnap_setRenderTarget\n\t"
            ".type UthudSnap_setRenderTarget, %function\n\t"
            "UthudSnap_setRenderTarget:\n\t"
            "push {r0, lr}\n\t" // the renderer, the return address (8 B: the stack stays 8-aligned)
            "ldr ip, =UthudSnap_state\n\t"
            "ldr ip, [ip, #16]\n\t" // original.setRenderTarget
            "blx ip\n\t"
            "ldr r1, [sp]\n\t"
            "str r0, [sp]\n\t" // its result, popped into r0 on return
            "mov r0, r1\n\t"
            "bl UthudSnap_resnap\n\t"
            "pop {r0, pc}\n\t"
            ".size UthudSnap_setRenderTarget, . - UthudSnap_setRenderTarget\n\t"
            ".ltorg");
}

// CtrDisplayCaps is two bools, returned in r0 (AAPCS: a composite of 4 bytes or less): passed through as a word, so
// the wrapper doesn't unpack and repack it.
uint32_t __real_CtrRenderer_getDisplayCaps(Renderer* renderer);
uint32_t __wrap_CtrRenderer_getDisplayCaps(Renderer* renderer);

// Whole words each, in its own read-only section (a string literal would grow .rodata).
#define UTHUD_WORDS(lit) ((sizeof(lit) + 3) & ~(size_t) 3)
#define UTHUD_SNAP_KEY " view_snap = %d"
#define UTHUD_SNAP_PATH CTRPORT_GAME_DIR "/settings.ini"
#define UTHUD_SNAP_LOG "Display: view_snap %d\n"
static const char uthudSnapKey[UTHUD_WORDS(UTHUD_SNAP_KEY)] __attribute__((aligned(4), section(".rodata.late.uthud"))) = UTHUD_SNAP_KEY;
static const char uthudSnapPath[UTHUD_WORDS(UTHUD_SNAP_PATH)] __attribute__((aligned(4), section(".rodata.late.uthud"))) = UTHUD_SNAP_PATH;
static const char uthudSnapRead[4] __attribute__((aligned(4), section(".rodata.late.uthud"))) = "r";
static const char uthudSnapLog[UTHUD_WORDS(UTHUD_SNAP_LOG)] __attribute__((aligned(4), section(".rodata.late.uthud"))) = UTHUD_SNAP_LOG;

// main.c asks for the display caps right after each CtrRenderer_create, before it draws anything, and again when the
// options menu opens or saves. Each time the switch is read again: the default (2 for UNDERTALE), then settings.ini's
// last view_snap line. 0 off, 2 round up, any other value rounds down (UthudSnap_resnap).
// The vtable entries go back in whenever they are not already the hooks: CtrRenderer_create fills ctrVtable again
// each time it runs.
UTHUD_TEXT uint32_t __wrap_CtrRenderer_getDisplayCaps(Renderer* renderer) {
    uint32_t caps = __real_CtrRenderer_getDisplayCaps(renderer);
    int value = UTHUD_SNAP_DEFAULT;
    FILE* f = fopen(uthudSnapPath, uthudSnapRead);
    if (f != nullptr) {
        char line[128];
        while (fgets(line, sizeof(line), f) != nullptr) sscanf(line, uthudSnapKey, &value);
        fclose(f);
    }
    UthudSnap_state.on = (uint32_t) value;
    logInfo(uthudSnapLog, value);
    RendererVtable* vtable = renderer->vtable; // ctr_renderer.c's static ctrVtable
    if (UthudSnap_state.on && vtable->beginView != uthudSnapBeginView) {
        UthudSnap_state.original.beginView = vtable->beginView;
        UthudSnap_state.original.applyProjection = vtable->applyProjection;
        UthudSnap_state.original.setRenderTarget = vtable->setRenderTarget;
        vtable->beginView = uthudSnapBeginView;
        vtable->applyProjection = UthudSnap_applyProjection;
        vtable->setRenderTarget = UthudSnap_setRenderTarget;
    }
    return caps;
}

#endif // ENABLE_WAD17
