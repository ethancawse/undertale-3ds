// Wrap chains: functions that more than one feature wraps at link time.
//
// The link wraps each of these functions once (CMakeLists.txt: -Wl,--wrap=X), and only one __wrap_X may exist. The
// first link of a chain takes the name __wrap_X. Each link calls the next one exactly as it called __real_X before,
// and the last link calls __real_X. To reorder a chain, or add or drop a link, change only this file.
#ifndef CTR_WRAP_CHAIN_H
#define CTR_WRAP_CHAIN_H

#include "ctr_data_version.h"

// CtrDataVersion_check: loadscreen -> nativemac -> roomplan -> crispart (DELTARUNE) or utsfx (UNDERTALE) -> real.
//   loadscreen (z_loadscreen.c): while the loading screen runs, passes on its own progress callback.
//   nativemac (ctr_data_version.c): UNDERTALE's later known files (the macOS data) for an unknown result.
//   roomplan (z_roomplan.c): keeps the digest for the room plan (DELTARUNE: and the chapter folder).
//   utsfx (z_utsfx.c, UNDERTALE only): keeps the digest for the sound plan.
#define CTR_DATAVERSION_LOADSCREEN __wrap_CtrDataVersion_check
#define CTR_DATAVERSION_NATIVEMAC CtrDataVersion_chainNativemac
#define CTR_DATAVERSION_AFTER_LOADSCREEN CTR_DATAVERSION_NATIVEMAC
#define CTR_DATAVERSION_ROOMPLAN CtrDataVersion_chainRoomplan
#define CTR_DATAVERSION_AFTER_NATIVEMAC CTR_DATAVERSION_ROOMPLAN
#ifdef ENABLE_WAD17
#define CTR_DATAVERSION_CRISPART CtrDataVersion_chainCrispart
#define CTR_DATAVERSION_AFTER_ROOMPLAN CTR_DATAVERSION_CRISPART
#define CTR_DATAVERSION_AFTER_CRISPART __real_CtrDataVersion_check
#elif defined(ENABLE_WAD16)
#define CTR_DATAVERSION_UTSFX CtrDataVersion_chainUtsfx
#define CTR_DATAVERSION_AFTER_ROOMPLAN CTR_DATAVERSION_UTSFX
#define CTR_DATAVERSION_AFTER_UTSFX __real_CtrDataVersion_check
#else
#define CTR_DATAVERSION_AFTER_ROOMPLAN __real_CtrDataVersion_check
#endif

#define CTR_DATAVERSION_CHAIN_PARAMS                                                                                   \
    const char *path, const char *game, const char *part, const char *cachePath, void (*progress)(void), bool *cached, \
        char digest[65]
CtrDataVersionResult __real_CtrDataVersion_check(CTR_DATAVERSION_CHAIN_PARAMS);
CtrDataVersionResult CTR_DATAVERSION_LOADSCREEN(CTR_DATAVERSION_CHAIN_PARAMS);
CtrDataVersionResult CTR_DATAVERSION_NATIVEMAC(CTR_DATAVERSION_CHAIN_PARAMS);
CtrDataVersionResult CTR_DATAVERSION_ROOMPLAN(CTR_DATAVERSION_CHAIN_PARAMS);
#ifdef ENABLE_WAD17
CtrDataVersionResult CTR_DATAVERSION_CRISPART(CTR_DATAVERSION_CHAIN_PARAMS);
#elif defined(ENABLE_WAD16)
CtrDataVersionResult CTR_DATAVERSION_UTSFX(CTR_DATAVERSION_CHAIN_PARAMS);
#endif

// CtrWatchdog_stop: loadscreen -> [inputlate, UT only] -> argcrash -> real.
//   loadscreen (z_loadscreen.c): stops and joins the loading screen's thread first, always.
//   argcrash (z_argcrash.c): while its crash or pending screen shows, keeps the watchdog running unwatched instead.
#define CTR_WATCHDOGSTOP_LOADSCREEN __wrap_CtrWatchdog_stop
#define CTR_WATCHDOGSTOP_ARGCRASH CtrWatchdog_chainStopArgcrash
#if !defined(ENABLE_WAD17)
#define CTR_WATCHDOGSTOP_AFTER_LOADSCREEN CTR_WATCHDOGSTOP_INPUTLATE
#else
#define CTR_WATCHDOGSTOP_AFTER_LOADSCREEN CTR_WATCHDOGSTOP_ARGCRASH
#endif
#define CTR_WATCHDOGSTOP_AFTER_ARGCRASH __real_CtrWatchdog_stop

void __real_CtrWatchdog_stop(void);
void CTR_WATCHDOGSTOP_LOADSCREEN(void);
void CTR_WATCHDOGSTOP_ARGCRASH(void);

// CtrRenderer_loadFontVerdicts (UNDERTALE): smalltext -> symverdict -> real. DELTARUNE: crispart -> real on Old 3DS,
// crispart -> symverdict -> real on New 3DS (CMakeLists.txt renames z_crispart.c's next link to symverdict's).
//   smalltext (z_smalltext.c): puts its outer vtable in front of the renderer's when a Crisp text key is on.
//   symverdict (z_symverdict.c): marks the items the build found symmetric halving would draw plain. Its sections are
//   named after CTR_SYMVERDICT_NAME (DELTARUNE New: symverdict_dr, linked last; z_symverdict_dr.c).
#if !defined(ENABLE_WAD17)
#define CTR_SYMVERDICT_NAME "symverdict"
#define CTR_FONTVERDICTS_SMALLTEXT __wrap_CtrRenderer_loadFontVerdicts
#define CTR_FONTVERDICTS_SYMVERDICT CtrRenderer_chainFontVerdictsSymverdict
#define CTR_FONTVERDICTS_AFTER_SMALLTEXT CTR_FONTVERDICTS_SYMVERDICT
#define CTR_FONTVERDICTS_AFTER_SYMVERDICT __real_CtrRenderer_loadFontVerdicts
#elif defined(CTRPORT_TARGET_N3DS)
#define CTR_SYMVERDICT_NAME "symverdict_dr"
#define CTR_FONTVERDICTS_SYMVERDICT CtrRenderer_chainFontVerdictsSymverdict
#define CTR_FONTVERDICTS_AFTER_SYMVERDICT __real_CtrRenderer_loadFontVerdicts
#endif

// CtrArgui_override/saveEdits: optmenu -> crispart (DELTARUNE) or smalltext (UNDERTALE) -> real (argui's).
//   crispart (z_crispart.c, CtrArgui_chainOverrideCrispart/SaveEditsCrispart): its --ini keys and saved lines.
//   smalltext (z_smalltext.c, CtrArgui_chainOverrideSmalltext/SaveEditsSmalltext): the Crisp text test keys given
//   with --ini, and the file's own Crisp lines written back after the display's save rewrites the file.
// ctr_optmenu.c names the smalltext links and, on DELTARUNE, maps those two names to crispart's, without shifting its
// original __LINE__ values. Each game links only its own late link.

// Input sampling observes the existing display/menu/exit calls. Existing links
// keep their symbols, addresses and bodies; only downstream call targets change.
#if !defined(ENABLE_WAD17)
// Only 3DS builds see the typed prototypes: host builds of shared files (test_data_versions.py compiles
// ctr_data_version.c with the host clang) have no libctru headers.
#ifdef __3DS__
#include "ctr_menu.h"
#include "ctr_renderer.h"
#endif
#define CTR_WATCHDOGSTOP_INPUTLATE CtrWatchdog_chainStopInputlate
#define CTR_WATCHDOGSTOP_AFTER_INPUTLATE CTR_WATCHDOGSTOP_ARGCRASH
void CTR_WATCHDOGSTOP_INPUTLATE(void);
#define CTR_DISPLAY_INPUTLATE CtrRenderer_chainDisplayInputlate
#define CTR_DISPLAY_AFTER_SCREENS CTR_DISPLAY_INPUTLATE
#ifdef __3DS__
void CTR_DISPLAY_INPUTLATE(Renderer* renderer, const CtrDisplaySettings* settings);
#endif
#define CTR_MENUOPEN_INPUTLATE CtrMenu_chainOpenInputlate
#define CTR_MENUOPEN_AFTER_OPTMENU CTR_MENUOPEN_INPUTLATE
#define CTR_MENUCLOSE_INPUTLATE CtrMenu_chainCloseInputlate
#define CTR_MENUCLOSE_AFTER_OPTMENU CTR_MENUCLOSE_INPUTLATE
#ifdef __3DS__
void CTR_MENUOPEN_INPUTLATE(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps);
void CTR_MENUCLOSE_INPUTLATE(CtrMenu* menu);
#endif
#define CTR_HBLDR_INPUTLATE CtrHbldr_chainInputlate
#define CTR_HBLDR_AFTER_ARGCRASH CTR_HBLDR_INPUTLATE
#define CTR_HBLDR_AFTER_INPUTLATE __real_CtrHbldr_setNext
const char* CTR_HBLDR_INPUTLATE(const char* path, const char* const* args, int count);
#else
#define CTR_DISPLAY_AFTER_SCREENS __real_CtrRenderer_setDisplay
#define CTR_MENUOPEN_AFTER_OPTMENU CtrMenu_open
#define CTR_MENUCLOSE_AFTER_OPTMENU CtrMenu_close
#define CTR_HBLDR_AFTER_ARGCRASH __real_CtrHbldr_setNext
#endif
#if !defined(ENABLE_WAD17)
const char* CTR_HBLDR_AFTER_INPUTLATE(const char* path, const char* const* args, int count);
#endif

// Runner_step and CtrRenderer_endFrame (UNDERTALE only): inputlate (z_inputlate.c's __wrap_ functions) -> swkbd
// (z_swkbd.c: Mettaton's essay gets the software keyboard) -> real. inputlate keeps its bodies and lines; only its
// two downstream call targets change.
#if !defined(ENABLE_WAD17)
#define CTR_STEP_SWKBD CtrRunner_chainStepSwkbd
#define CTR_STEP_AFTER_INPUTLATE CTR_STEP_SWKBD
#define CTR_ENDFRAME_SWKBD CtrRenderer_chainEndFrameSwkbd
#define CTR_ENDFRAME_AFTER_INPUTLATE CTR_ENDFRAME_SWKBD
struct Runner;
void CTR_STEP_SWKBD(struct Runner* runner);
#ifdef __3DS__
void CTR_ENDFRAME_SWKBD(Renderer* renderer);
#endif
#endif

#endif
