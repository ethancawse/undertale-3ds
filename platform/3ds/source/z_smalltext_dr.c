// Crisp small text, DELTARUNE New 3DS (source/z_smalltext.c; sansfont round 3, maintainer 10-26).
//
// DELTARUNE New builds compile z_smalltext.c through this file, so that its object (z_smalltext_dr.o) and its sections
// (named smalltext_dr) have their own lines last in platform/3ds/late_reserve.ld: code merged before it keeps its
// place. UNDERTALE compiles z_smalltext.c itself, and this file adds nothing to it or to DELTARUNE Old builds.
#if defined(ENABLE_WAD17) && defined(CTRPORT_TARGET_N3DS)
#define ST_DR_UNIT 1
// stfix (below): z_smalltext.c's room-change wrapper keeps its code and place under another name.
#define __wrap_CtrRenderer_loadFontVerdicts __attribute__((noinline)) stLoadFontVerdicts
#include "z_smalltext.c"
#undef __wrap_CtrRenderer_loadFontVerdicts

// ===[ stfix (change flowerhw, 2026-11-03): the outer table goes on a renderer once ]===
// z_smalltext.c's wrapper tells whether its outer table is on by renderer->vtable == &st.outer alone. The tower
// decorations (vine_new_retain.inc's dqInstall) put a table of their own in front of it at the first tower draw,
// copied from it and calling through the copy. At the next room change the outer table then went on again, in front
// of that table: st.prev, st.text and st.textColor led back through the copy to the outer table's own text entries,
// so any text stDraw declines (a gradient, a scale, another font) looped there forever, and so did stDestroy at the
// chapter's end. Here the renderer whose chain holds the outer table is marked where the table goes on and forgotten
// in its destroy (the next chapter's renderer may be made at the same address); a marked renderer keeps the room's
// verdicts and nothing more. Own sections, appended last in late_reserve.ld, so no earlier late address moves.
// UNDERTALE: nothing puts a table in front after smalltext's (un-gate after UNDERTALE 1.0, change flowerhw).
#define STFIX_TEXT __attribute__((noinline, section(".text_flowerhw_stfix")))
#define STFIX_BSS __attribute__((section(".bss_flowerhw_stfix,\"aw\",%nobits @"))) // nobits, as z_drresid.c's cold state

static const Renderer* stOn STFIX_BSS;

static STFIX_TEXT void stfixDestroy(Renderer* renderer) {
    stOn = nullptr;
    stDestroy(renderer);
}

STFIX_TEXT void __wrap_CtrRenderer_loadFontVerdicts(Renderer* renderer) {
    if (renderer != nullptr && renderer == stOn) {
        CTR_FONTVERDICTS_AFTER_SMALLTEXT(renderer); // what stLoadFontVerdicts does first
        return;
    }
    stLoadFontVerdicts(renderer);
    // Put on just now: stLoadFontVerdicts copies the table and sets its destroy to stDestroy each time it does.
    if (renderer != nullptr && renderer->vtable == &st.outer && st.outer.destroy == stDestroy) {
        stOn = renderer;
        st.outer.destroy = stfixDestroy;
        CTR_VTCHAIN_NOTE(renderer, &st.outer, st.prev, smalltext)
    }
}
#else
#include <stdint.h> // a translation unit of its own in other builds
#endif
