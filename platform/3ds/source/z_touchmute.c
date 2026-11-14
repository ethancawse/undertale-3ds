// Keep the game's logical move voice, silencing only the instance whose discarded ID we receive.
#if defined(ENABLE_WAD16)
#include <stdlib.h>
#include <string.h>
// UNDERTALE's shared conversions are exported under collision_gm14.c's names.
#define RValue_toInt32Other Colflip14_toInt32Other
#define RValue_toRealOther Colflip14_toRealOther
#ifndef RVALUE_OTHER_EXTERN
#define RVALUE_OTHER_EXTERN
#endif
#include "audio_system.h"
#include "instance.h"
#include "runner.h"
#include "vm.h"
#include "ctr_touch_mute.h"
#ifdef CTRPORT_TOUCH_TEST
#include "log.h"
#endif
#undef VM_registerBuiltin
#define TM_CODE __attribute__((noinline, section(".touchui_text")))
#define TM_RO __attribute__((section(".touchui_ro")))
typedef struct {
    BuiltinFunc original;
    Runner* runner;
    uint32_t instance;
    int32_t frame, object, key;
    const char* script;
    bool armed;
} TouchMute;
#if defined(__arm__)
_Static_assert(sizeof(TouchMute) == 32, "touch mute ARM heap size changed");
#endif
static TouchMute* tm __attribute__((section(".bss.touchui_late")));
static const char tmPlayName[] TM_RO = "audio_play_sound";
#ifdef CTRPORT_TOUCH_TEST
static const char tmTrace[] TM_RO =
    "[touchsound] f=%lu obj=%ld inst=%lu event=%ld:%ld sound=%ld voice=%ld mute=%d\n";
#endif

TM_CODE void CtrTouchMute_clear(void) {
    if (tm != nullptr) tm->armed = false;
}

TM_CODE void CtrTouchMute_mark(Runner* runner, uint32_t instance, int32_t object,
                               int32_t key, const char* script) {
    if (tm == nullptr || runner == nullptr || script == nullptr) return;
    tm->runner = runner;
    tm->frame = runner->frameCount;
    tm->instance = instance;
    tm->object = object;
    tm->key = key;
    tm->script = script;
    tm->armed = true;
}

static TM_CODE RValue tmPlay(VMContext* ctx, RValue* args, int32_t count) {
    // Determine provenance before calling the original, which may run nested work. Consume once.
    int32_t sound = count > 0 && args[0].type != RVALUE_UNDEFINED ? RValue_toInt32(args[0]) : -1;
    bool mute = tm->armed && ctx->runner == tm->runner && ctx->runner->frameCount == tm->frame
        && ctx->currentInstance != nullptr && ctx->currentInstance->instanceId == tm->instance
        && ctx->currentInstance->objectIndex == tm->object && ctx->currentEventObjectIndex == tm->object
        && ctx->currentEventType == EVENT_KEYPRESS && ctx->currentEventSubtype == tm->key
        && sound == 115 && ctx->currentCodeName != nullptr && strcmp(ctx->currentCodeName, tm->script) == 0;
    if (mute) tm->armed = false;
    RValue result = tm->original(ctx, args, count);
    int32_t voice = RValue_toInt32(result);
    AudioSystem* audio = ctx->runner->audioSystem;
    if (mute && voice >= 0 && audio != nullptr)
        audio->vtable->setSoundGain(audio, voice, 0.0f, 0);
#ifdef CTRPORT_TOUCH_TEST
    logInfo(tmTrace, (unsigned long) ctx->runner->frameCount, (long) ctx->currentEventObjectIndex,
            (unsigned long) (ctx->currentInstance != nullptr ? ctx->currentInstance->instanceId : 0),
            (long) ctx->currentEventType, (long) ctx->currentEventSubtype, (long) sound, (long) voice, (int) mute);
#endif
    return result;
}

void __real_VM_registerBuiltin(VMContext* ctx, const char* name, BuiltinFunc func);
TM_CODE void __wrap_VM_registerBuiltin(VMContext* ctx, const char* name, BuiltinFunc func) {
    if (strcmp(name, tmPlayName) == 0) {
        if (tm == nullptr) tm = calloc(1, sizeof(*tm));
        if (tm != nullptr) {
            tm->original = func;
            tm->armed = false;
            func = tmPlay;
        }
    }
    __real_VM_registerBuiltin(ctx, name, func);
}
#endif
