// Preserve natural group verdicts across Crisp/Smooth edits, outside the draw path.
#include <stdint.h>
#include <stdlib.h>
#include "renderer.h"

#ifndef ENABLE_WAD17
typedef struct {
    const DataWin* dataWin;
    uint32_t count;
    uint8_t on, applied;
    uint8_t group[96];
    int32_t item[96];
} MenulagGroups;
extern MenulagGroups CtrSmooth_sharp;
extern uint8_t* Krsharp_verdicts(Renderer* renderer);

#ifdef PLATFORM_3DS
// An underscore section name needs explicit NOBITS, as the port's other late state does.
#define ML_BSS __attribute__((section(".bss_menulag,\"aw\",%nobits @")))
#else
#define ML_BSS __attribute__((section(".bss_menulag")))
#endif
static struct {
    uint8_t* original;
    uint32_t valid;
} ml ML_BSS;

__attribute__((noinline, optimize("Os"), section(".text_menulag")))
uint8_t* Menulag_verdicts(Renderer* renderer) {
    uint8_t* values = Krsharp_verdicts(renderer);
    MenulagGroups* g = &CtrSmooth_sharp;
    uint32_t changed = (uint32_t)(g->on ^ g->applied);
    if (!values || g->dataWin != renderer->dataWin || g->count > 96 || !changed) return values;
    uint32_t added = g->on & changed;
    if (!ml.original && added) ml.original = malloc(96);
    if (!ml.original) return values; // Keep the native invalidation/reload on allocation failure.
    ml.valid &= g->applied; // A fresh group table resets applied; old snapshots become invalid.
    uint32_t restore = ml.valid & changed;
    for (uint32_t i = 0; i < g->count; i++) {
        uint32_t bit = 1u << g->group[i];
        uint8_t* v = &values[g->item[i]];
        if (added & bit) ml.original[i] = *v;
        else if (restore & bit) *v = ml.original[i];
    }
    ml.valid |= added;
    // The native loop still forces Crisp and commits applied=on. Only skip
    // clearing off-groups whose natural verdict this helper just restored.
    g->applied &= (uint8_t)~restore;
    return values;
}
#endif
