// The genuine UNDERTALE Continue path gets a displayed black transition before its synchronous save/room loading.
//
// The title menu's Continue runs the game's save-load script through script_execute with a script index, which calls
// the script's code directly in both the interpreter (builtin_script_execute) and AOT (aotCallScript): a builtin
// registered under the script's name never sees it. The script's first call, though, is the builtin named in cbName
// below, and that script is the game's only caller of it. So wrap the builtin: called in the title menu (room 3), it
// shows black first, then does exactly what it did. Every other call (other rooms, the bench's direct load) is
// unchanged.
#if defined(ENABLE_WAD16) && defined(__3DS__)
#include "log.h"
#include "renderer.h"
#include "runner.h"
#include "stb_ds.h"
#include "vm.h"

#define CB_TEXT __attribute__((noinline, section(".text.utoldcontblack_late")))
#define CB_RO __attribute__((section(".rodata.utoldcontblack_late")))
static BuiltinFunc cbOrig __attribute__((section(".bss.utoldcontblack_late")));
static const char cbName[] CB_RO = "room_set_persistent";
// One line per genuine Continue: the logs show the black ran, and when.
static const char cbLog[] CB_RO = "Continue: black\n";

void Utold_continueBlack(Renderer* renderer);
void __real_Btnprompt_registerAll(VMContext* ctx);
void __wrap_Btnprompt_registerAll(VMContext* ctx);

static CB_TEXT RValue cbPersist(VMContext* ctx, RValue* args, int32_t argc) {
    Runner* runner = ctx->runner;
    if (runner != nullptr && runner->currentRoomIndex == 3) {
        Utold_continueBlack(runner->renderer);
        logInfo(cbLog);
    }
    return cbOrig(ctx, args, argc);
}

// Before VM_create builds its call cache from the builtins, so every call (interpreted or AOT) gets the wrapper.
CB_TEXT void __wrap_Btnprompt_registerAll(VMContext* ctx) {
    __real_Btnprompt_registerAll(ctx);
    if (ctx->dataWin == nullptr || ctx->dataWin->gen8.wadVersion != 16) return;
    ptrdiff_t i = shgeti(ctx->builtinMap, (char*) cbName);
    if (0 > i) return;
    cbOrig = ctx->builtinMap[i].value;
    ctx->builtinMap[i].value = cbPersist;
}
#endif
