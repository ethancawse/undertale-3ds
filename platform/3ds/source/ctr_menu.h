// Options menu on the bottom screen, opened with SELECT. The game is paused
// while it is open; display changes apply immediately so they can be judged
// on the top screen.
#ifndef CTRPORT_CTR_MENU_H
#define CTRPORT_CTR_MENU_H

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>

#include "ctr_display.h"

typedef enum {
    MENU_ACTION_NONE,
    MENU_ACTION_CHANGED, // settings were edited; apply them
    MENU_ACTION_RESUME,
    MENU_ACTION_QUIT,
    MENU_ACTION_PERF_TEST, // open the performance test (ctr_benchmark.h)
} CtrMenuAction;

typedef enum {
    MENU_PAGE_OPTIONS,
    MENU_PAGE_CONTROLS, // which key each button sends (settings.controls)
} CtrMenuPage;

typedef struct {
    bool open;
    CtrMenuPage page;
    int32_t cursor;         // on the options page
    int32_t controlsCursor; // on the Controls page
    CtrDisplaySettings settings;
    CtrDisplayCaps caps;
    bool edited; // since opening, so closing knows whether to save
    PrintConsole* console;
} CtrMenu;

// console must cover the whole bottom screen.
void CtrMenu_open(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps);
// down is this frame's newly pressed keys.
CtrMenuAction CtrMenu_update(CtrMenu* menu, u32 down);
void CtrMenu_close(CtrMenu* menu);

void CtrArgui_menuOpen(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps);
CtrMenuAction CtrArgui_menuUpdate(CtrMenu* menu, u32 down);
void CtrArgui_menuClose(CtrMenu* menu);
#ifdef CTRPORT_CTR_ARGUI_REDIRECT
#define CtrMenu_open CtrArgui_menuOpen
#define CtrMenu_update CtrArgui_menuUpdate
#define CtrMenu_close CtrArgui_menuClose
#endif

// libctru places C-stick directions four bits below the Circle Pad's directions.
// Keep Old 3DS and DELTARUNE's existing input/code unchanged.
#if defined(ENABLE_WAD16) && defined(CTRPORT_TARGET_N3DS)
#define CTR_MENU_PAD_RIGHT (KEY_CPAD_RIGHT | KEY_CSTICK_RIGHT)
#define CTR_MENU_PAD_LEFT (KEY_CPAD_LEFT | KEY_CSTICK_LEFT)
#define CTR_MENU_PAD_UP (KEY_CPAD_UP | KEY_CSTICK_UP)
#define CTR_MENU_PAD_DOWN (KEY_CPAD_DOWN | KEY_CSTICK_DOWN)
#if defined(__arm__)
// Three ARM instructions, as in the old Circle Pad fold: retain the menu bodies' slots.
static inline __attribute__((always_inline)) u32 CtrMenu_fold(u32 down) {
    u32 folded;
    __asm__("orr %0, %1, %1, lsl #4\n\t"
            "and %0, %0, #0xf0000000\n\t"
            "orr %0, %1, %0, lsr #24"
            : "=&r" (folded) : "r" (down));
    return folded;
}
#define CTR_MENU_FOLD(down) CtrMenu_fold(down)
#else
#define CTR_MENU_FOLD(down) ((down) | ((((down) | ((down) << 4)) & 0xF0000000u) >> 24))
#endif
#else
#define CTR_MENU_PAD_RIGHT KEY_CPAD_RIGHT
#define CTR_MENU_PAD_LEFT KEY_CPAD_LEFT
#define CTR_MENU_PAD_UP KEY_CPAD_UP
#define CTR_MENU_PAD_DOWN KEY_CPAD_DOWN
#define CTR_MENU_FOLD(down) ((down) | (((down) >> 24) & 0xF0u))
#endif

// Whether the menu's close writes settings.ini. Shots builds' --ini opt_probe=3 (change optplan: console rows on lab
// consoles, whose settings.ini stays as it was) turns it off: main.c's save and the 3DS page's owed line both ask.
// Every other build: true, as a constant.
#ifdef CTRPORT_PLAYER_SHOTS
extern bool CtrOptmenu_probeNoSave;
#define CTR_MENU_SAVES (!CtrOptmenu_probeNoSave)
#else
#define CTR_MENU_SAVES true
#endif

#endif
