// Virtual desktop-window coordinates for UNDERTALE's ending. Nothing in the game's draw list or RNG changes.
#if defined(ENABLE_WAD16) && defined(__3DS__)
#include "ctr_utwindow.h"
#include "renderer.h"
#include "runner.h"
#include "stb_ds.h"
#include "vm.h"

#define UW_TEXT __attribute__((noinline, section(".text.late.utbugs4")))
#define UW_RO __attribute__((section(".rodata.utbugs4_late")))
static int32_t uwPosition[2] __attribute__((section(".bss.late.utbugs4")));
static const char uwCenterName[] UW_RO = "window_center";

void __real_Btnprompt_registerAll(VMContext* ctx);
void CtrUtWindow_register(VMContext* ctx);
void __real_Runner_handlePendingRoomChange(Runner* runner);
void CtrUtWindow_pendingRoom(Runner* runner);

static UW_TEXT RValue uwCenter(VMContext* ctx, RValue* args, int32_t argc) {
    (void) ctx; (void) args; (void) argc;
    uwPosition[0] = uwPosition[1] = 0;
    return RValue_makeUndefined();
}

static UW_TEXT void uwSetHook(int32_t x, int32_t y) {
    uwPosition[0] = x;
    uwPosition[1] = y;
}

static UW_TEXT bool uwGetHook(int32_t* x, int32_t* y) {
    *x = uwPosition[0];
    *y = uwPosition[1];
    return true;
}

UW_TEXT void CtrUtWindow_register(VMContext* ctx) {
    __real_Btnprompt_registerAll(ctx);
    // Only center is a stub. The existing position builtins keep their conversion and return behavior.
    ptrdiff_t index = shgeti(ctx->builtinMap, (char*) uwCenterName);
    if (index >= 0) ctx->builtinMap[index].value = uwCenter;
}

UW_TEXT void CtrUtWindow_pendingRoom(Runner* runner) {
    // The Runner exists here, before any room's creation events; it does not exist during builtin registration.
    runner->getWindowPosition = uwGetHook;
    runner->setWindowPosition = uwSetHook;
    // A new game must not inherit the previous game's window position. Ordinary room changes preserve it.
    if (runner->pendingRoom == ROOM_RESTARTGAME) uwPosition[0] = uwPosition[1] = 0;
    __real_Runner_handlePendingRoomChange(runner);
}

UW_TEXT void CtrUtWindow_blitApp(void* renderer, C3D_Tex* tex, float srcX, float srcY, float srcW, float srcH,
                                float offX, float offY, const float* dst, bool linear) {
    if ((uwPosition[0] | uwPosition[1]) == 0) {
        CtrUtWindow_originalBlit(renderer, tex, srcX, srcY, srcW, srcH, offX, offY, dst, linear);
        return;
    }
    Runner* runner = ((Renderer*) renderer)->runner;
    float moved[4] = {dst[0], dst[1], dst[2], dst[3]};
    const uint32_t size[2] = {runner->dataWin->gen8.defaultWindowWidth, runner->dataWin->gen8.defaultWindowHeight};
    for (int axis = 0; axis < 2; axis++) {
        float extent = dst[axis + 2];
        float offset = size[axis] != 0 ? (float) uwPosition[axis] * extent / (float) size[axis] : 0.0f;
        // Keep at least half the picture visible, even for synthetic extreme coordinates. beginFrame already
        // clears the complete game target black, including when CtrScreens routes it to the bottom screen.
        float limit = extent * 0.5f;
        if (offset < -limit) offset = -limit;
        if (offset > limit) offset = limit;
        moved[axis] += floorf(offset + 0.5f);
    }
    CtrUtWindow_originalBlit(renderer, tex, srcX, srcY, srcW, srcH, offX, offY, moved, linear);
}
#endif
