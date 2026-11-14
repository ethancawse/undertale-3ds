// The first draw of a bone wall (UNDERTALE, Old 3DS): the call of draw_self_border_true that finds its variables missing.
//
// The script has a native body (z_asgborder.c, asgore30). The body reads ten instance variables, and on the first call
// on an instance they aren't there yet, so it hands that call to the generated function: 15,576 B of code on a 16 KB
// instruction cache. A frame draws one such call per new instance, and Sans's attacks 17 and 18 make 70 bone walls in
// one frame. Draw on that frame is 4 to 5 ms over the frames after it, in both bone modes (sansspawn, 11-06).
//
// Here the first call makes the ten variables and takes the native body. Same result: when the checks below pass, the
// generated function overwrites each one's old value before it first reads it (bytecode 0000-0120), making them in this
// key order, so the same keys land in the same slots of the same table, and nothing in between runs GML or can see that
// a key exists. The eighth variable is the global with the box edges, which this never makes. The checks are the body's
// own, none of them writes, and when one fails nothing is made and the call goes to the generated function as before.
//
// gmlaot.py's UTF_NATIVE_FIRST binds it: the wrapper aotu_<k> gives the body aotuf_<k> where it gave it aotf_<k>, and
// aotuf_<k> calls this, so the body's path on every later call has no added instruction. z_asgborder.c doesn't change.
// Built for UNDERTALE Old only (CMakeLists.txt). Late code only and last (late_reserve.ld); no statics, no constants.
#include "wad_versions.h"

#if defined(ENABLE_WAD16) && defined(CTRPORT_AOT)

#include "aot/vm_aot.h"
#include "data_win.h"
#include "instance.h"
#include "int_rvalue_hashmap.h"
#include "rvalue.h"
#include "runner.h"
#include "vm.h"
#include "vm_builtins.h"

#include <stdint.h>

RValue UtFlames_borderDraw(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a);
RValue SansSpawn_borderFirst(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a);

#if !defined(ENABLE_VM_TRACING) && !defined(CTRPORT_AOT_STRESS)
RValue* UtFlames_slot(Instance* inst, int32_t varID);
#endif

__attribute__((noinline, section(".text_sansspawn")))
RValue SansSpawn_borderFirst(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) {
#if defined(ENABLE_VM_TRACING) || defined(CTRPORT_AOT_STRESS)
    // Tracing and stress builds test the generated code itself.
    (void) k;
    (void) a;
    return generated(ctx);
#else
    // UtFlames_borderDraw's checks, in its order.
    Instance* inst = (Instance*) ctx->currentInstance;
    Instance* global = (Instance*) ctx->globalScopeInstance;
    if (inst == nullptr || inst->objectIndex == STRUCT_OBJECT_INDEX || global == nullptr ||
        (uint32_t) a[11] >= ctx->funcCallCacheCount || (uint32_t) a[12] >= ctx->funcCallCacheCount)
        return generated(ctx);
    const RValue* args = ctx->scriptArgs;
    if (ctx->funcCallCache[a[11]].builtin == nullptr || ctx->funcCallCache[a[12]].builtin == nullptr || args == nullptr ||
        2 > ctx->scriptArgCount || !vmIsPlainNumber(args[0].type) || !vmIsPlainNumber(args[1].type))
        return generated(ctx);
    const RValue* edges = UtFlames_slot(global, a[7]);
    if (edges == nullptr) return generated(ctx);
    RValue u;
    AOT_SET_INT(&u, 2, GML_TYPE_INT16);
    const RValue* b2 = aotArrayFastCell(ctx, edges, &u);
    AOT_SET_INT(&u, 3, GML_TYPE_INT16);
    const RValue* b3 = aotArrayFastCell(ctx, edges, &u);
    if (b2 == nullptr || b3 == nullptr || !vmIsPlainNumber(b2->type) || !vmIsPlainNumber(b3->type)) return generated(ctx);
    Instance* lb = VM_findInstanceByTarget(ctx, a[13]);
    Instance* rb = VM_findInstanceByTarget(ctx, a[14]);
    if (lb == nullptr || rb == nullptr || lb->objectIndex == STRUCT_OBJECT_INDEX || rb->objectIndex == STRUCT_OBJECT_INDEX)
        return generated(ctx);
    // The ten instance variables, in the order the generated function's stores make them.
    for (int32_t i = 0; 11 > i; i++) {
        if (i != 7) IntRValueHashMap_getOrInsertUndefined(&inst->selfVars, a[i]);
    }
    return UtFlames_borderDraw(ctx, k, generated, a);
#endif
}

#endif
