// utorder (credfix, ruling 2026-10-27; UNDERTALE only): GameMaker 1.4's draw order between instances of the
// same depth.
//
// Why: GameMaker 1.4 keeps one list per depth and draws it front to back. An instance goes in at the front when it is
// made, and again when its depth changes to another value (a write of the depth it already has leaves it where it
// is). So at equal depth the newest instance draws first and the oldest ends on top. pcref showed it (credfix's
// results, section 10: runs credfix-order6, order9 and order11). The engine drew equal depths by ascending instance
// id, the reverse, and never moved an instance whose depth changed. In UNDERTALE that drew Undyne's battle buttons
// dim (her fight's darkened copies on top of them), stacked Mettaton's smoke the other way, and changed the order of
// the credits dodge's touched names (its hit list is filled in Draw).
//
// What: each instance gets a draw order key, ascending = drawn first: drawElementOrder, GameMaker 2's layer element
// index, which a 1.4 game never sets (runner.c's computeDrawElementOrders returns before 2.0). The key is the next value
// of a counter that only goes down, taken when the instance is made (the Instance_create wrap below) and when its depth
// changes to another value (the Runner_moveInstanceToDepthLayer wrap: the VM's depth write calls it only when the value
// differs). runner.c's draw list puts the key where the instance id was (UTORDER_KEY, instance.h), so the comparators,
// the sort, the keep and the mid-draw cursor are unchanged, and the dust swarm's records take their key with their id
// (z_utkill_swarm.c). A depth change gives its entry the new key (the depth setter's sort pass moves it), or marks the
// list's structure when the entry isn't at its draw rank, so the keep reads the new key. Every structure change
// goes through Utorder_keep (runner.c's end): the keep, else the full rebuild set to the keys. Runner_drawPre,
// drawPost and drawGUI call it first (wraps below), before rebuildDrawableCache's full rebuild could order by id.
// No new per-instance memory; 4 bytes of statics. Builds that draw through drawRoom (UTORDER_ON unset: render timing,
// VM tracing) keep the id order; the wraps then only pass the calls on.
#include "instance.h"
#include "runner.h"
#include "stb_ds.h"

#define UTORDER_LATE __attribute__((noinline, section(".text.utorder_late")))

#if defined(UTORDER_ON)
int32_t Utorder_counter __attribute__((section(".bss.utorder_late")));
#endif

#if defined(UTORDER_ON)
#define UTORDER_NEXT(inst) ((inst)->drawElementOrder = --Utorder_counter)
#else
#define UTORDER_NEXT(inst) ((void) 0)
#endif

Instance* __real_Instance_create(uint32_t instanceId, int32_t objectIndex, GMLReal x, GMLReal y);
Instance* __wrap_Instance_create(uint32_t instanceId, int32_t objectIndex, GMLReal x, GMLReal y) UTORDER_LATE;
Instance* __wrap_Instance_create(uint32_t instanceId, int32_t objectIndex, GMLReal x, GMLReal y) {
    Instance* inst = __real_Instance_create(instanceId, objectIndex, x, y);
    UTORDER_NEXT(inst);
    return inst;
}

void __real_Runner_moveInstanceToDepthLayer(Runner* runner, Instance* inst, int32_t depth);
void __wrap_Runner_moveInstanceToDepthLayer(Runner* runner, Instance* inst, int32_t depth) UTORDER_LATE;
void __wrap_Runner_moveInstanceToDepthLayer(Runner* runner, Instance* inst, int32_t depth) {
#if defined(UTORDER_ON)
    if (depth != inst->depth) {
        // Its entry takes the new key where the list holds it at its draw rank (the setter's sort pass then moves
        // it); otherwise the keep finds it (a structure change).
        int32_t key = UTORDER_NEXT(inst), rank = inst->drawRank;
        Drawable* list = runner->cachedDrawables;
        if (!runner->drawableListStructureDirty && 0 <= rank && (int32_t) arrlen(list) > rank &&
            list[rank].type == DRAWABLE_INSTANCE && list[rank].instance == inst && !UTSW_IS_SPAN(&list[rank]))
            list[rank].order = key;
        else
            runner->drawableListStructureDirty = true;
    }
#endif
    __real_Runner_moveInstanceToDepthLayer(runner, inst, depth);
}

#if defined(UTORDER_ON)
#define UTORDER_DRAW_KEEP(runner) Utorder_keep(runner)
#else
#define UTORDER_DRAW_KEEP(runner) ((void) 0)
#endif

void __real_Runner_drawPre(Runner* runner, int32_t windowW, int32_t windowH);
void __wrap_Runner_drawPre(Runner* runner, int32_t windowW, int32_t windowH) UTORDER_LATE;
void __wrap_Runner_drawPre(Runner* runner, int32_t windowW, int32_t windowH) {
    UTORDER_DRAW_KEEP(runner);
    __real_Runner_drawPre(runner, windowW, windowH);
}

void __real_Runner_drawPost(Runner* runner, int32_t windowW, int32_t windowH);
void __wrap_Runner_drawPost(Runner* runner, int32_t windowW, int32_t windowH) UTORDER_LATE;
void __wrap_Runner_drawPost(Runner* runner, int32_t windowW, int32_t windowH) {
    UTORDER_DRAW_KEEP(runner);
    __real_Runner_drawPost(runner, windowW, windowH);
}

void __real_Runner_drawGUI(Runner* runner, int32_t windowW, int32_t windowH, int32_t targetW, int32_t targetH);
void __wrap_Runner_drawGUI(Runner* runner, int32_t windowW, int32_t windowH, int32_t targetW, int32_t targetH) UTORDER_LATE;
void __wrap_Runner_drawGUI(Runner* runner, int32_t windowW, int32_t windowH, int32_t targetW, int32_t targetH) {
    UTORDER_DRAW_KEEP(runner);
    __real_Runner_drawGUI(runner, windowW, windowH, targetW, targetH);
}
