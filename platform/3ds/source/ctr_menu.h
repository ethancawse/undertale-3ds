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

#endif
