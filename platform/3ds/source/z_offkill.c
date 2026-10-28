// offkill (offdraw; UNDERTALE only): early destroy of bullets that have left the view for good.
//
// Some of UNDERTALE's bullets fly on past the view and live on until a far line (the room's edge plus a margin, a
// timer, the end of the turn), running their Step and Draw events the whole time. The builder lists the ones whose
// PC code proves they can never come back or matter again (tools/aot/offkill.py lists the reasons) in
// romfs:/offkill.bin: per entry an object index, an axis (0 = x, 1 = y), a direction (+1 = right or down, -1 = left
// or up) and a reach in pixels, the farthest any of its pixels can lie from its position plus twice the scene's
// largest view shake. At the end of each frame (after Draw, from Utsw_handleRoomChange) an instance of a listed object
// that is moving away along the axis and is farther than the reach past that edge of view 0 is destroyed without its
// Destroy event (the gameplay-state rule, "Early destroy": only the early-destroyed instances differ). Without the file, nothing.

#include "runner.h"

#if defined(UTSW_ON)
#include <stdio.h>

#include "ctr_benchmark.h"
#include "stb_ds.h"

#define OK_TEXT __attribute__((noinline, optimize("Os"), section(".text.offkill_late")))
#define OK_MAX 8

// One entry of romfs:/offkill.bin (8 bytes, little-endian).
typedef struct {
    int16_t object;
    int8_t axis;
    int8_t sign;
    uint8_t reach;
    uint8_t pad[3];
} OffKillRule;

static OffKillRule okRules[OK_MAX] __attribute__((section(".bss.offkill_late")));
// 0: the file not read yet; -1: no file or no entries; otherwise the number of entries.
static int8_t okCount __attribute__((section(".bss.offkill_late")));
static const char okPath[] __attribute__((section(".rodata.offkill_in_text"))) = "romfs:/offkill.bin";
static const char okMode[] __attribute__((section(".rodata.offkill_in_text"))) = "rb";

#if defined(CTRPORT_OFFDRAW)
bool CtrOffdraw_offkillOff(void);
#endif

void OffKill_roomChange(Runner* runner) OK_TEXT;

// Utsw_handleRoomChange calls this in place of CtrBench_handleRoomChange.
void OffKill_roomChange(Runner* runner) {
    CtrBench_handleRoomChange(runner);
    if (okCount == 0) {
        okCount = -1;
        FILE* f = fopen(okPath, okMode);
        if (f == nullptr) return;
        size_t n = fread(okRules, sizeof(OffKillRule), OK_MAX, f);
        fclose(f);
        if (n != 0) okCount = (int8_t) n;
    }
#if defined(CTRPORT_OFFDRAW)
    if (CtrOffdraw_offkillOff()) return;
#endif
    // A frame that changes room ends its instances as the game does.
    if (0 >= okCount || runner->pendingRoom != -1 || runner->currentRoom == nullptr) return;
    // View 0's edges: its camera, or the room without one (as the diagnostic that proved the entries,
    // platform/3ds/diag/ctr_offdraw.c).
    float lo[2] = { 0.0f, 0.0f };
    float hi[2] = { (float) runner->currentRoom->width, (float) runner->currentRoom->height };
    GMLCamera* cam = Runner_getCameraForView(runner, 0);
    if (cam != nullptr && cam->allocated) {
        lo[0] = cam->viewX;
        lo[1] = cam->viewY;
        hi[0] = lo[0] + (float) cam->viewWidth;
        hi[1] = lo[1] + (float) cam->viewHeight;
    }
    int32_t n = (int32_t) arrlen(runner->instances);
    for (int32_t i = 0; n > i; i++) {
        Instance* inst = runner->instances[i];
        if (inst->destroyed || !inst->active) continue;
        for (int32_t k = 0; okCount > k; k++) {
            const OffKillRule* rule = &okRules[k];
            if (inst->objectIndex != rule->object) continue;
            int a = rule->axis != 0;
            float s = (float) rule->sign;
            float p = (float) (a ? inst->y : inst->x);
            float v = (float) (a ? inst->vspeed : inst->hspeed);
            float edge = rule->sign > 0 ? hi[a] : lo[a];
            if (s * v > 0.0f && s * (p - edge) > (float) rule->reach) {
                Runner_destroyInstance(runner, inst, false);
                break;
            }
        }
    }
    // No cleanup here: the next Step's own cleanup takes them out, as it does the game's own destroys.
}
#endif
