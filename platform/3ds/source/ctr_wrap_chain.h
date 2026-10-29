// Wrap chains: functions that more than one feature wraps at link time.
//
// The link wraps each of these functions once (CMakeLists.txt: -Wl,--wrap=X), and only one __wrap_X may exist. The
// first link of a chain takes the name __wrap_X. Each link calls the next one exactly as it called __real_X before,
// and the last link calls __real_X. To reorder a chain, or add or drop a link, change only this file.
#ifndef CTR_WRAP_CHAIN_H
#define CTR_WRAP_CHAIN_H

#include "ctr_data_version.h"
#include <stdint.h>

// CtrAudioSystem_shedSfx (UNDERTALE): flowend -> utflowey2 (Old only, CTRPORT_PKLIN) -> real.
//   flowend (ctr_audio_system.c's end): the room's planned sounds count as just played during the shed.
//   utflowey2 (z_utstall.c's end): after the sound effects' shed, idle linear copies go before any tile.
#ifdef CTRPORT_PKLIN
#define CTR_SHEDSFX_AFTER_FLOWEND CtrAudioSystem_chainPklin
uint32_t CtrAudioSystem_chainPklin(uint32_t bytes);
#else
#define CTR_SHEDSFX_AFTER_FLOWEND __real_CtrAudioSystem_shedSfx
#endif

// UNDERTALE's bench-only alternate root precedes the ordinary argument parser.
#ifdef CTRPORT_GOG_COMPAT
#define CTR_LAUNCHARGS_AFTER_ARGCRASH CtrLaunchArgs_chainGogut
#else
#define CTR_LAUNCHARGS_AFTER_ARGCRASH __real_CtrLaunchArgs_parse
#endif

// CtrDataVersion_check: loadscreen -> nativemac -> roomplan -> crispart (DELTARUNE) or utsfx (UNDERTALE) -> credfix
// -> real.
//   loadscreen (z_loadscreen.c): while the loading screen runs, passes on its own progress callback.
//   nativemac (ctr_data_version.c): UNDERTALE's later known files (the macOS data) for an unknown result.
//   roomplan (z_roomplan.c): keeps the digest for the room plan (DELTARUNE: and the chapter folder).
//   utsfx (z_utsfx.c, UNDERTALE only): keeps the digest for the sound plan.
//   flowcode (z_flowcode.c, DELTARUNE New flowcode=lazy builds only, between crispart and credfix): keeps the
//   boot-checked file identity for the chapter-5 code plan.
//   credfix (z_credfix.c): a notice when a file the game reads late (credits.txt; DELTARUNE: Chapter 1's
//   lang/lang_en.json) is missing next to data.win.
#define CTR_DATAVERSION_LOADSCREEN __wrap_CtrDataVersion_check
#define CTR_DATAVERSION_NATIVEMAC CtrDataVersion_chainNativemac
#define CTR_DATAVERSION_AFTER_LOADSCREEN CTR_DATAVERSION_NATIVEMAC
#define CTR_DATAVERSION_ROOMPLAN CtrDataVersion_chainRoomplan
#define CTR_DATAVERSION_GOGUT CtrDataVersion_chainGogut
#ifdef CTRPORT_GOG_COMPAT
#define CTR_DATAVERSION_AFTER_NATIVEMAC CTR_DATAVERSION_GOGUT
#else
#define CTR_DATAVERSION_AFTER_NATIVEMAC CTR_DATAVERSION_ROOMPLAN
#endif
#define CTR_DATAVERSION_AFTER_GOGUT CTR_DATAVERSION_ROOMPLAN
#ifdef ENABLE_WAD17
#define CTR_DATAVERSION_CRISPART CtrDataVersion_chainCrispart
#define CTR_DATAVERSION_AFTER_ROOMPLAN CTR_DATAVERSION_CRISPART
#ifdef CTRPORT_FLOWCODE // flowfull code paging (DELTARUNE New flowcode=lazy builds only): crispart -> flowcode -> credfix
#define CTR_DATAVERSION_AFTER_CRISPART CtrDataVersion_chainFlowcode
#define CTR_DATAVERSION_AFTER_FLOWCODE CTR_DATAVERSION_CREDFIX
#else
#define CTR_DATAVERSION_AFTER_CRISPART CTR_DATAVERSION_CREDFIX
#endif
#elif defined(ENABLE_WAD16)
#define CTR_DATAVERSION_UTSFX CtrDataVersion_chainUtsfx
#define CTR_DATAVERSION_AFTER_ROOMPLAN CTR_DATAVERSION_UTSFX
#define CTR_DATAVERSION_AFTER_UTSFX CTR_DATAVERSION_CREDFIX
#else
#define CTR_DATAVERSION_AFTER_ROOMPLAN __real_CtrDataVersion_check
#endif

#define CTR_DATAVERSION_CHAIN_PARAMS                                                                                   \
    const char *path, const char *game, const char *part, const char *cachePath, void (*progress)(void), bool *cached, \
        char digest[65]
CtrDataVersionResult __real_CtrDataVersion_check(CTR_DATAVERSION_CHAIN_PARAMS);
#ifdef CTRPORT_FLOWCODE
CtrDataVersionResult CtrDataVersion_chainFlowcode(CTR_DATAVERSION_CHAIN_PARAMS);
#endif
CtrDataVersionResult CTR_DATAVERSION_LOADSCREEN(CTR_DATAVERSION_CHAIN_PARAMS);
CtrDataVersionResult CTR_DATAVERSION_NATIVEMAC(CTR_DATAVERSION_CHAIN_PARAMS);
CtrDataVersionResult CTR_DATAVERSION_ROOMPLAN(CTR_DATAVERSION_CHAIN_PARAMS);
CtrDataVersionResult CTR_DATAVERSION_GOGUT(CTR_DATAVERSION_CHAIN_PARAMS);
#if defined(ENABLE_WAD17) || defined(ENABLE_WAD16)
#define CTR_DATAVERSION_CREDFIX CtrDataVersion_chainCredfix
#define CTR_DATAVERSION_AFTER_CREDFIX __real_CtrDataVersion_check
CtrDataVersionResult CTR_DATAVERSION_CREDFIX(CTR_DATAVERSION_CHAIN_PARAMS);
#endif
#ifdef ENABLE_WAD17
CtrDataVersionResult CTR_DATAVERSION_CRISPART(CTR_DATAVERSION_CHAIN_PARAMS);
#elif defined(ENABLE_WAD16)
CtrDataVersionResult CTR_DATAVERSION_UTSFX(CTR_DATAVERSION_CHAIN_PARAMS);
#endif

// CtrRenderer_prefetchRoom: roomplan -> [textfront -> (writerfront) -> dodgefront, UNDERTALE Old 3DS only] -> real.
//   roomplan (z_roomplan.c): reads the room's planned code entries back.
//   textfront (z_textfront.c): binds the centred-text scripts' hand fronts first, then calls the next link.
//   writerfront (z_writerfront.c; not in build.sh's nowriterfront builds): binds the dialogue writer's Draw front.
//   dodgefront (z_dodgefront.c): binds the centred text line's hand front after each room change (where textfront
//   hasn't).
#ifdef CTRPORT_DODGEFRONT
#define CTR_PREFETCHROOM_TEXTFRONT CtrRenderer_chainTextfront
#define CTR_PREFETCHROOM_DODGEFRONT CtrRenderer_chainDodgefront
#define CTR_PREFETCHROOM_AFTER_ROOMPLAN CTR_PREFETCHROOM_TEXTFRONT
#ifdef CTRPORT_WRITERFRONT
#define CTR_PREFETCHROOM_WRITERFRONT CtrRenderer_chainWriterfront
#define CTR_PREFETCHROOM_AFTER_TEXTFRONT CTR_PREFETCHROOM_WRITERFRONT
#define CTR_PREFETCHROOM_AFTER_WRITERFRONT CTR_PREFETCHROOM_DODGEFRONT
struct Renderer;
void CTR_PREFETCHROOM_WRITERFRONT(struct Renderer* renderer);
#else
#define CTR_PREFETCHROOM_AFTER_TEXTFRONT CTR_PREFETCHROOM_DODGEFRONT
#endif
struct Renderer;
void CTR_PREFETCHROOM_TEXTFRONT(struct Renderer* renderer);
void CTR_PREFETCHROOM_DODGEFRONT(struct Renderer* renderer);
#else
#define CTR_PREFETCHROOM_AFTER_ROOMPLAN __real_CtrRenderer_prefetchRoom
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

// CtrRenderer_getDisplayCaps: uthud -> soulshake (UNDERTALE) or pixfix (DELTARUNE) -> itemverdict -> real.
//   soulshake (ctr_uthud_snap.c's end, UNDERTALE builds): puts its view follower in the beginView slot before uthud
//   saves that slot, so view_snap stands down while the view scrolls.
//   uthud (ctr_uthud_snap.c, UNDERTALE builds): reads view_snap and hooks the renderer's vtable.
//   pixfix (ctr_pixfix_hpname.c, DELTARUNE builds): hooks the renderer's init to narrow the HP label.
//   itemverdict (z_itemverdict.c, both games): reads Smart smoothing's item verdict table if the renderer lacks it (the
//   options menu opening, or closing after a switch to Smart).
// Each link passes CtrDisplayCaps (two bools) through as a word, declared as uint32_t (Renderer*) in its own file.
#define CTR_DISPLAYCAPS_UTHUD __wrap_CtrRenderer_getDisplayCaps
#define CTR_DISPLAYCAPS_PIXFIX __wrap_CtrRenderer_getDisplayCaps
#define CTR_DISPLAYCAPS_ITEMVERDICT CtrRenderer_chainCapsItemverdict
#define CTR_DISPLAYCAPS_SOULSHAKE CtrRenderer_chainCapsSoulshake
#define CTR_DISPLAYCAPS_AFTER_UTHUD CTR_DISPLAYCAPS_SOULSHAKE
#define CTR_DISPLAYCAPS_AFTER_SOULSHAKE CTR_DISPLAYCAPS_ITEMVERDICT
#define CTR_DISPLAYCAPS_AFTER_PIXFIX CTR_DISPLAYCAPS_ITEMVERDICT
#define CTR_DISPLAYCAPS_AFTER_ITEMVERDICT __real_CtrRenderer_getDisplayCaps

// CtrRenderer_loadFontVerdicts (UNDERTALE): smalltext -> symverdict -> itemverdict -> real. DELTARUNE: crispart ->
// itemverdict -> real on Old 3DS, smalltext -> crispart -> symverdict -> itemverdict -> real on New 3DS. Each link
// calls the next one first.
//   smalltext (z_smalltext.c; DELTARUNE New through z_smalltext_dr.c): puts its outer vtable in front of the
//   renderer's when a Crisp text key is on (after crispart's hooks, which it copies).
//   crispart (z_crispart.c, DELTARUNE builds): hooks the renderer's table for the Crisp sprite art when it is on.
//   symverdict (z_symverdict.c): marks the items the build found symmetric halving would draw plain. Its sections are
//   named after CTR_SYMVERDICT_NAME (DELTARUNE New: symverdict_dr, linked last; z_symverdict_dr.c).
//   itemverdict (z_itemverdict.c, both games): reads Smart smoothing's item verdict table for this data.win.
#if !defined(ENABLE_WAD17)
#define CTR_SYMVERDICT_NAME "symverdict"
#define CTR_FONTVERDICTS_SMALLTEXT __wrap_CtrRenderer_loadFontVerdicts
#define CTR_FONTVERDICTS_SYMVERDICT CtrRenderer_chainFontVerdictsSymverdict
#define CTR_FONTVERDICTS_AFTER_SMALLTEXT CTR_FONTVERDICTS_SYMVERDICT
#define CTR_FONTVERDICTS_AFTER_SYMVERDICT CTR_FONTVERDICTS_ITEMVERDICT
#else
#if defined(CTRPORT_TARGET_N3DS)
#define CTR_FONTVERDICTS_SMALLTEXT __wrap_CtrRenderer_loadFontVerdicts
#define CTR_FONTVERDICTS_CRISPART CtrRenderer_chainFontVerdictsCrispart
#define CTR_FONTVERDICTS_AFTER_SMALLTEXT CTR_FONTVERDICTS_CRISPART
#define CTR_SYMVERDICT_NAME "symverdict_dr"
#define CTR_FONTVERDICTS_SYMVERDICT CtrRenderer_chainFontVerdictsSymverdict
#define CTR_FONTVERDICTS_AFTER_CRISPART CTR_FONTVERDICTS_SYMVERDICT
#define CTR_FONTVERDICTS_AFTER_SYMVERDICT CTR_FONTVERDICTS_ITEMVERDICT
#else
#define CTR_FONTVERDICTS_CRISPART __wrap_CtrRenderer_loadFontVerdicts
#define CTR_FONTVERDICTS_AFTER_CRISPART CTR_FONTVERDICTS_ITEMVERDICT
#endif
#endif
#define CTR_FONTVERDICTS_ITEMVERDICT CtrRenderer_chainFontVerdictsItemverdict
#define CTR_FONTVERDICTS_AFTER_ITEMVERDICT __real_CtrRenderer_loadFontVerdicts

// CtrArgui_override/saveEdits: optmenu -> crispart (DELTARUNE Old) or smalltext (UNDERTALE) -> real (argui's);
// DELTARUNE New: optmenu -> smalltext -> crispart -> real (z_smalltext_dr.c names crispart's links as its next).
//   crispart (z_crispart.c, CtrArgui_chainOverrideCrispart/SaveEditsCrispart): its --ini keys and saved lines.
//   smalltext (z_smalltext.c, CtrArgui_chainOverrideSmalltext/SaveEditsSmalltext): the Crisp text test keys given
//   with --ini, and the file's own Crisp lines written back after the display's save rewrites the file.
// ctr_optmenu.c names the smalltext links and, on DELTARUNE Old, maps those two names to crispart's, without shifting its
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
// CtrRenderer_endFrame, continued: swkbd -> freebies (z_freebies.c: restarting the room into another planned scene gets
// the room load after the frame, as main.c gives a room change) -> real.
#define CTR_ENDFRAME_FREEBIES CtrRenderer_chainEndFrameFreebies
#define CTR_ENDFRAME_AFTER_SWKBD CTR_ENDFRAME_FREEBIES
#ifdef __3DS__
void CTR_ENDFRAME_FREEBIES(Renderer* renderer);
#endif
#endif

// DR exact cold SHA/read implementation, after every existing digest consumer.
#if defined(ENABLE_WAD17) && (defined(__3DS__) || defined(BX_COLD_HOST_TEST))
CtrDataVersionResult BxCold_check(CTR_DATAVERSION_CHAIN_PARAMS);
#undef CTR_DATAVERSION_AFTER_CRISPART
#define CTR_DATAVERSION_AFTER_CRISPART BxCold_check
#endif

// CtrOpenFiles_track (main()'s first call): gamedir (z_gamedir.c, both games: its layer over the card's device) ->
// sdlat (z_sdlat.c: profile builds with CTRPORT_SDLAT only, which CMakeLists.txt marks with CTRPORT_SDLAT_CHAIN on
// z_gamedir.c; Azahar's SD floors go under gamedir's layer, where the card is) -> real.
#define CTR_OPENFILES_GAMEDIR __wrap_CtrOpenFiles_track
#define CTR_OPENFILES_SDLAT CtrOpenFiles_chainSdlat
#ifdef CTRPORT_SDLAT_CHAIN
#define CTR_OPENFILES_AFTER_GAMEDIR CTR_OPENFILES_SDLAT
#else
#define CTR_OPENFILES_AFTER_GAMEDIR __real_CtrOpenFiles_track
#endif
#define CTR_OPENFILES_AFTER_SDLAT __real_CtrOpenFiles_track

// _exit: inputlate (UNDERTALE: stops the HID sampler) -> exitonce (z_leavekeep.c: the first thread to reach _exit
// runs the exit, a second one waits) -> real. DELTARUNE has no inputlate link: exitonce is its __wrap__exit.
#if !defined(ENABLE_WAD17)
#define CTR_EXIT_ONCE CtrExitOnce_chain
#define CTR_EXIT_AFTER_INPUTLATE CTR_EXIT_ONCE
void CTR_EXIT_ONCE(int status) __attribute__((noreturn));
#else
#define CTR_EXIT_ONCE __wrap__exit
#endif

// CtrLab_poll: argcrash (z_argcrash.c) -> leavekeep (z_leavekeep.c: the lab's "died" target, once) -> real.
#define CTR_LABPOLL_AFTER_ARGCRASH CtrLabPoll_chainLeavekeep
void CtrLabPoll_chainLeavekeep(void);


#if defined(CTRPORT_ADPCM_SFX) || defined(CTRPORT_ADPCM_MUSIC) || defined(CTRPORT_ADPCM_SFXPACK)
// Capture the already checked data identity without rereading data.win on
// the game thread. Original per-game links remain downstream in the chain.
#undef CTR_DATAVERSION_AFTER_ROOMPLAN
#ifdef CTRPORT_ADPCM_MUSIC
#define CTR_DATAVERSION_AFTER_ROOMPLAN CtrDataVersion_chainAdpcmMusic
#elif defined(CTRPORT_ADPCM_SFXPACK)
#define CTR_DATAVERSION_AFTER_ROOMPLAN CtrDataVersion_chainAdpcmPack
#else
#define CTR_DATAVERSION_AFTER_ROOMPLAN CtrDataVersion_chainAdpcm
#endif
#ifdef ENABLE_WAD17
#define CTR_DATAVERSION_AFTER_ADPCM CTR_DATAVERSION_CRISPART
#elif defined(ENABLE_WAD16)
#define CTR_DATAVERSION_AFTER_ADPCM CTR_DATAVERSION_UTSFX
#else
#define CTR_DATAVERSION_AFTER_ADPCM __real_CtrDataVersion_check
#endif
CtrDataVersionResult CtrDataVersion_chainAdpcm(CTR_DATAVERSION_CHAIN_PARAMS);
CtrDataVersionResult CtrDataVersion_chainAdpcmMusic(CTR_DATAVERSION_CHAIN_PARAMS);
CtrDataVersionResult CtrDataVersion_chainAdpcmPack(CTR_DATAVERSION_CHAIN_PARAMS);
#endif

#endif
