// Wrap chains: functions that more than one feature wraps at link time.
//
// The link wraps each of these functions once (CMakeLists.txt: -Wl,--wrap=X), and only one __wrap_X may exist. The
// first link of a chain takes the name __wrap_X. Each link calls the next one exactly as it called __real_X before,
// and the last link calls __real_X. To reorder a chain, or add or drop a link, change only this file.
#ifndef CTR_WRAP_CHAIN_H
#define CTR_WRAP_CHAIN_H

#include "ctr_data_version.h"
#include <stdint.h>

// CtrAudioSystem_shedSfx (UNDERTALE): flowend -> flowhitch -> flowburst -> utflowey2 (Old only, CTRPORT_PKLIN) ->
// real.
//   flowend (ctr_audio_system.c's end): the room's planned sounds count as just played during the shed.
//   flowhitch (ctr_audio_system.c, before flowend's beginSfxPlan wrap): the sounds this shed frees play as streams
//   until the next room change.
//   flowburst (ctr_audio_system.c's end): the step's deferred voices start before the shed picks its victims.
//   utflowey2 (z_utstall.c's end): after the sound effects' shed, idle linear copies go before any tile.
#define CTR_SHEDSFX_AFTER_FLOWEND CtrAudioSystem_chainFlowhitch
uint32_t CtrAudioSystem_chainFlowhitch(uint32_t bytes);
#define CTR_SHEDSFX_AFTER_FLOWHITCH CtrAudioSystem_chainFlowburst
uint32_t CtrAudioSystem_chainFlowburst(uint32_t bytes);
#ifdef CTRPORT_PKLIN
#define CTR_SHEDSFX_AFTER_FLOWBURST CtrAudioSystem_chainPklin
uint32_t CtrAudioSystem_chainPklin(uint32_t bytes);
#else
#define CTR_SHEDSFX_AFTER_FLOWBURST __real_CtrAudioSystem_shedSfx
#endif

// CtrAudioSystem_beginSfxPlan (UNDERTALE): flowend -> papvoice -> flowhitch -> real.
//   papvoice (z_utsfx.c's end): the plan's text voices with a small WAV are read at their play (change papvoice).
//   flowhitch (ctr_audio_system.c, before flowend's wrap): the sounds the last room shed may be decoded into the cache again.
#define CTR_BEGINSFXPLAN_AFTER_FLOWEND UtSfx_chainPlanVoices
void UtSfx_chainPlanVoices(void);
void CtrAudioSystem_chainPlanFlowhitch(void);

// CtrAudioSystem_preloadSfx (UNDERTALE): flowend [-> the ADPCM pack's admit] -> flame (Old only) -> real.
//   flame (ctr_audio_system.c's end, omegaflowey W1): a planned compressed sound effect has its decode into the cache
//   asked for at the room change (the Ogg flame loop), where the real one only copies WAVs.
#if defined(ENABLE_WAD16) && defined(CTRPORT_TARGET_O3DS)
#define CTR_PRELOADSFX_AFTER_FLOWEND CtrAudioSystem_chainFlame
int CtrAudioSystem_chainFlame(int32_t soundIndex, uint32_t bytes, uint32_t floorBytes);
#else
#define CTR_PRELOADSFX_AFTER_FLOWEND __real_CtrAudioSystem_preloadSfx
#endif

// UNDERTALE's bench-only alternate root precedes the ordinary argument parser. UNDERTALE lab builds (labguard's
// --labguard-crash, z_labguard.c, whose own next link is gogut's or the parser) filter their option first.
#ifdef CTRPORT_LABGUARD_ARGS
#define CTR_LAUNCHARGS_AFTER_ARGCRASH CtrLaunchArgs_chainLabguard
#elif defined(CTRPORT_GOG_COMPAT)
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

// CtrRenderer_prefetchRoom: roomplan -> [flowhitch, UNDERTALE] -> [textfront -> (writerfront -> trailfront ->
// itemfront -> writerfirst) -> textcase -> dodgefront, UNDERTALE Old 3DS only] -> real.
//   roomplan (z_roomplan.c): reads the room's planned code entries back.
//   textfront (z_textfront.c): binds the centred-text scripts' hand fronts first, then calls the next link.
//   writerfront (z_writerfront.c; not in build.sh's nowriterfront builds): binds the dialogue writer's Draw front.
//   trailfront (z_trailfront.c; writerfront builds only): binds the SAVE Asriel rainbow trail's Draw front.
//   itemfront (z_itemfront.c; writerfront builds only): binds the item-name scripts' fronts (the DREAM press).
//   writerfirst (z_writerfirst.c; writerfront builds only): wraps writerfront's Draw front so a writer's first Draw
//   takes it too.
//   bulletsplit (z_bulletsplit.c; writerfront builds only): the credits bullets' text drawn after their run.
//   textcase (z_textcase.c): binds the dialogue script's case-table front (its switch jumps straight to the case).
//   dodgefront (z_dodgefront.c): binds the centred text line's hand front after each room change (where textfront
//   hasn't).
#ifdef CTRPORT_DODGEFRONT
#define CTR_PREFETCHROOM_TEXTFRONT CtrRenderer_chainTextfront
#define CTR_PREFETCHROOM_DODGEFRONT CtrRenderer_chainDodgefront
#define CTR_PREFETCHROOM_AFTER_FLOWHITCH CTR_PREFETCHROOM_TEXTFRONT
#ifdef CTRPORT_WRITERFRONT
#define CTR_PREFETCHROOM_WRITERFRONT CtrRenderer_chainWriterfront
#define CTR_PREFETCHROOM_AFTER_TEXTFRONT CTR_PREFETCHROOM_WRITERFRONT
#define CTR_PREFETCHROOM_TRAILFRONT CtrRenderer_chainTrailfront
#define CTR_PREFETCHROOM_AFTER_WRITERFRONT CTR_PREFETCHROOM_TRAILFRONT
#define CTR_PREFETCHROOM_ITEMFRONT CtrRenderer_chainItemfront
#define CTR_PREFETCHROOM_AFTER_TRAILFRONT CTR_PREFETCHROOM_ITEMFRONT
#define CTR_PREFETCHROOM_WRITERFIRST CtrRenderer_chainWriterfirst
#define CTR_PREFETCHROOM_AFTER_ITEMFRONT CTR_PREFETCHROOM_WRITERFIRST
#define CTR_PREFETCHROOM_BULLETSPLIT CtrRenderer_chainBulletsplit
#define CTR_PREFETCHROOM_AFTER_WRITERFIRST CTR_PREFETCHROOM_BULLETSPLIT
#define CTR_PREFETCHROOM_AFTER_BULLETSPLIT CTR_PREFETCHROOM_TEXTCASE
struct Renderer;
void CTR_PREFETCHROOM_WRITERFRONT(struct Renderer* renderer);
void CTR_PREFETCHROOM_TRAILFRONT(struct Renderer* renderer);
void CTR_PREFETCHROOM_ITEMFRONT(struct Renderer* renderer);
void CTR_PREFETCHROOM_WRITERFIRST(struct Renderer* renderer);
void CTR_PREFETCHROOM_BULLETSPLIT(struct Renderer* renderer);
#else
#define CTR_PREFETCHROOM_AFTER_TEXTFRONT CTR_PREFETCHROOM_TEXTCASE
#endif
#define CTR_PREFETCHROOM_TEXTCASE CtrRenderer_chainTextcase
#define CTR_PREFETCHROOM_AFTER_TEXTCASE CTR_PREFETCHROOM_DODGEFRONT
struct Renderer;
void CTR_PREFETCHROOM_TEXTFRONT(struct Renderer* renderer);
void CTR_PREFETCHROOM_TEXTCASE(struct Renderer* renderer);
void CTR_PREFETCHROOM_DODGEFRONT(struct Renderer* renderer);
#else
#define CTR_PREFETCHROOM_AFTER_FLOWHITCH __real_CtrRenderer_prefetchRoom
#endif
// flowhitch (ctr_audio_system.c, UNDERTALE with Tremor): marks the room for the musical's song spares, then the next
// link (roomplan -> flowhitch -> the rest).
#if defined(ENABLE_WAD16) && defined(CTRPORT_HAVE_TREMOR)
#define CTR_PREFETCHROOM_AFTER_ROOMPLAN CtrRenderer_chainFlowhitch
struct Renderer;
void CtrRenderer_chainFlowhitch(struct Renderer* renderer);
#else
#define CTR_PREFETCHROOM_AFTER_ROOMPLAN CTR_PREFETCHROOM_AFTER_FLOWHITCH
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

// CtrRenderer_loadFontVerdicts (UNDERTALE): smalltext -> symverdict -> bubbleread -> itemverdict -> real. DELTARUNE: crispart ->
// itemverdict -> real on Old 3DS, smalltext -> crispart -> symverdict -> itemverdict -> real on New 3DS. Each link
// calls the next one first.
//   smalltext (z_smalltext.c; DELTARUNE New through z_smalltext_dr.c): puts its outer vtable in front of the
//   renderer's when a Crisp text key is on (after crispart's hooks, which it copies).
//   crispart (z_crispart.c, DELTARUNE builds): hooks the renderer's table for the Crisp sprite art when it is on.
//   symverdict (z_symverdict.c): marks the items the build found symmetric halving would draw plain. Its sections are
//   named after CTR_SYMVERDICT_NAME (DELTARUNE New: symverdict_dr, linked last; z_symverdict_dr.c).
//   itemverdict (z_itemverdict.c, both games): reads Smart smoothing's item verdict table for this data.win.
//   bubbleread (z_smalltext.c's end, UNDERTALE): reads bubbleclip's bubble table in the room change, not in Draw.
#if !defined(ENABLE_WAD17)
#define CTR_SYMVERDICT_NAME "symverdict"
#define CTR_FONTVERDICTS_SMALLTEXT __wrap_CtrRenderer_loadFontVerdicts
#define CTR_FONTVERDICTS_SYMVERDICT CtrRenderer_chainFontVerdictsSymverdict
#define CTR_FONTVERDICTS_AFTER_SMALLTEXT CTR_FONTVERDICTS_SYMVERDICT
#define CTR_FONTVERDICTS_AFTER_SYMVERDICT CTR_FONTVERDICTS_BUBBLEREAD
#define CTR_FONTVERDICTS_BUBBLEREAD CtrRenderer_chainFontVerdictsBubbleread
#define CTR_FONTVERDICTS_AFTER_BUBBLEREAD CTR_FONTVERDICTS_ITEMVERDICT
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
#define CTR_ENDFRAME_AFTER_SWKBD CtrLoadmix_chainEndFrame
#ifdef CTR_AUDIO_PCM_HASH
#define CTR_ENDFRAME_AFTER_LOADMIX CtrLoadaudio_chainEndFrame
#define CTR_ENDFRAME_AFTER_LOADAUDIO CTR_ENDFRAME_FREEBIES
#else
#define CTR_ENDFRAME_AFTER_LOADMIX CTR_ENDFRAME_FREEBIES
#endif
#ifdef __3DS__
void CtrLoadmix_chainEndFrame(Renderer* renderer);
void CTR_ENDFRAME_FREEBIES(Renderer* renderer);
#ifdef CTR_AUDIO_PCM_HASH
void CtrLoadaudio_chainEndFrame(Renderer* renderer);
#endif
#endif
// CtrRenderer_endFrame, continued: freebies -> nopromo (z_nopromo.c: with no RAM for packed pages, Draw asks no
// promotions) -> real.
#define CTR_ENDFRAME_NOPROMO CtrRenderer_chainEndFrameNopromo
#define CTR_ENDFRAME_AFTER_FREEBIES CTR_ENDFRAME_NOPROMO
#ifdef __3DS__
void CTR_ENDFRAME_NOPROMO(Renderer* renderer);
#endif
// CtrRenderer_endFrame, continued: nopromo -> postkick (z_postkick.c: a frame that changes no room runs its
// end-of-frame instance pass, UTSW_ROOMCHANGE, after the real endFrame has handed the frame to the GPU) -> real.
#define CTR_ENDFRAME_POSTKICK CtrRenderer_chainEndFramePostkick
#define CTR_ENDFRAME_AFTER_NOPROMO CTR_ENDFRAME_POSTKICK
#ifdef __3DS__
void CTR_ENDFRAME_POSTKICK(Renderer* renderer);
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

// CtrTextures_takeHot, CtrMemory_vramAllocNow, CtrTextures_destroy: knightgpu FC rev 2 (DELTARUNE New) -> contention
// (both ctr_renderer.c) -> real. CtrMemory_vramAllocOnce, _vramAllocAtOnce, _vramMemAlignOnce, _vramMemAlignAtOnce:
// FC rev 2 -> vramtouch (ctr_vram_touch.c) -> real.
//   FC rev 2 (ctr_renderer.c's end): the copies FC places are given up before any other VRAM allocation, the textures'
//   destroy forgets them, and mirrorHotTiles' list ends where it would stop without them.
#ifdef CTRPORT_KGFC2_ON
#define CTR_TAKEHOT_CONTENTION CtrTextures_chainContentionTakeHot
#define CTR_VRAMALLOCNOW_CONTENTION CtrMemory_chainContentionAllocNow
#define CTR_DESTROY_CONTENTION CtrTextures_chainContentionDestroy
#define CTR_VRAMALLOCONCE_TOUCH CtrMemory_chainTouchAllocOnce
#define CTR_VRAMALLOCATONCE_TOUCH CtrMemory_chainTouchAllocAtOnce
#define CTR_VRAMMEMALIGNONCE_TOUCH CtrMemory_chainTouchMemAlignOnce
#define CTR_VRAMMEMALIGNATONCE_TOUCH CtrMemory_chainTouchMemAlignAtOnce
#else
#define CTR_TAKEHOT_CONTENTION __wrap_CtrTextures_takeHot
#define CTR_VRAMALLOCNOW_CONTENTION __wrap_CtrMemory_vramAllocNow
#define CTR_DESTROY_CONTENTION __wrap_CtrTextures_destroy
#define CTR_VRAMALLOCONCE_TOUCH __wrap_CtrMemory_vramAllocOnce
#define CTR_VRAMALLOCATONCE_TOUCH __wrap_CtrMemory_vramAllocAtOnce
#define CTR_VRAMMEMALIGNONCE_TOUCH __wrap_CtrMemory_vramMemAlignOnce
#define CTR_VRAMMEMALIGNATONCE_TOUCH __wrap_CtrMemory_vramMemAlignAtOnce
#endif

// thinart: private LA4 atlas at hidden EX entry; existing chains keep their bodies.
// File-scope tag, so the prototypes below never declare a prototype-scope struct Renderer (R249).
struct Renderer;
#if defined(ENABLE_WAD16)
#undef CTR_DATAVERSION_AFTER_UTSFX
// logsweep (S8b): each link's caller goes to a wrapper appended in late text (z_thinart.c's end), so the bodies stay.
#define CTR_DATAVERSION_AFTER_UTSFX Thinart_dataCheck
CtrDataVersionResult Thinart_dataCheck(CTR_DATAVERSION_CHAIN_PARAMS);
CtrDataVersionResult Thinart_check(CTR_DATAVERSION_CHAIN_PARAMS);
// DODGEFRONT is a per-source definition, not a target-wide one. The new unit
// must retain Old's chain even though it is not one of the original readers.
#if defined(CTRPORT_TARGET_O3DS)
#define CTR_PREFETCHROOM_AFTER_THINART CtrRenderer_chainTextfront
#else
#define CTR_PREFETCHROOM_AFTER_THINART __real_CtrRenderer_prefetchRoom
#endif
#undef CTR_PREFETCHROOM_AFTER_FLOWHITCH
#define CTR_PREFETCHROOM_AFTER_FLOWHITCH Thinart_slotPrefetch
void Thinart_slotPrefetch(struct Renderer* renderer);
void Thinart_prefetchRoom(struct Renderer* renderer);
void CTR_PREFETCHROOM_AFTER_THINART(struct Renderer* renderer);
#undef CTR_DISPLAYCAPS_AFTER_SOULSHAKE
#define CTR_DISPLAYCAPS_AFTER_SOULSHAKE Thinart_slotCaps
uint32_t Thinart_slotCaps(struct Renderer* renderer);
uint32_t Thinart_caps(struct Renderer* renderer);
#undef CTR_ENDFRAME_AFTER_INPUTLATE
#define CTR_ENDFRAME_AFTER_INPUTLATE Thinart_slotEndFrame
void Thinart_slotEndFrame(struct Renderer* renderer);
void Thinart_endFrame(struct Renderer* renderer);
#endif

// thinrule (first update, UNDERTALE): Smart smoothing's whole-rows table (z_thinrule.c), the link after itemverdict's
// in the font verdict and display caps chains; itemverdict's link calls it where it called the real one.
#if defined(ENABLE_WAD16)
#undef CTR_FONTVERDICTS_AFTER_ITEMVERDICT
#define CTR_FONTVERDICTS_AFTER_ITEMVERDICT Thinrule_fontVerdicts
#undef CTR_DISPLAYCAPS_AFTER_ITEMVERDICT
#define CTR_DISPLAYCAPS_AFTER_ITEMVERDICT Thinrule_caps
#endif


// dwkeep (change loadclass 10, UNDERTALE): data.win's later opens to read are sub-files of one kept handle
// (z_dwkeep.c). Its layer goes over libctru's device before the real track, so ctr_open_files' list counts its opens:
// gamedir -> [sdlat] -> dwkeep -> real.
#if defined(ENABLE_WAD16) && !defined(ENABLE_WAD17)
void CtrOpenFiles_chainDwkeep(void);
#undef CTR_OPENFILES_AFTER_SDLAT
#define CTR_OPENFILES_AFTER_SDLAT CtrOpenFiles_chainDwkeep
#ifndef CTRPORT_SDLAT_CHAIN
#undef CTR_OPENFILES_AFTER_GAMEDIR
#define CTR_OPENFILES_AFTER_GAMEDIR CtrOpenFiles_chainDwkeep
#endif
#define CTR_OPENFILES_AFTER_DWKEEP __real_CtrOpenFiles_track
#endif

// battlesplit (UNDERTALE): the split battle layout (z_battlesplit.c). CtrRenderer_beginFrame: inputlate -> battlesplit
// (the layer made or dropped before the frame, the bottom screen taken after it) -> real. CtrRenderer_endFrame:
// ... nopromo -> battlesplit (the two screens presented in present's place) -> postkick -> real.
#if !defined(ENABLE_WAD17)
#undef CTR_ENDFRAME_AFTER_NOPROMO
#define CTR_ENDFRAME_AFTER_NOPROMO CtrRenderer_chainEndFrameBattlesplit
#define CTR_BEGINFRAME_AFTER_INPUTLATE CtrRenderer_chainBeginFrameBattlesplit
#ifdef __3DS__
void CtrRenderer_chainEndFrameBattlesplit(Renderer* renderer);
void CtrRenderer_chainBeginFrameBattlesplit(Renderer* renderer);
#endif
#endif

// battletouch (UNDERTALE): the Runner_step chain becomes inputlate -> battletouch (z_battletouch.c: keeps the runner
// for the stylus's next input sample) -> swkbd -> real. inputlate's call target is the only change there.
#if defined(ENABLE_WAD16) && defined(__3DS__)
#undef CTR_STEP_AFTER_INPUTLATE
#define CTR_STEP_AFTER_INPUTLATE Battletouch_step
void Battletouch_step(struct Runner* runner);
// The first-battle hint (z_battletouch.c's fourth pair, 11-08): one more link after battletouch in the Runner_step
// chain (inputlate -> battletouch -> the hint -> swkbd -> real), and after screens' settings read and save, so the
// hint's touch_hint_seen line is read and kept. The first slot's three calls change target only.
// The stylus hint (lead 11-10, z_battletouch.c's .text_battletouchhint) replaced it: the step and settings-read
// names are its code now (once a session: nothing read from or written to settings.ini for it), and the save goes
// straight on to the display's rewrite.
#define CTR_STEP_AFTER_BATTLETOUCH Battletouch_stepHint
#define CTR_BENCHFIDLOAD_AFTER_BATTLETOUCH Battletouch_loadHint
#define CTR_DISPLAYSAVE_AFTER_BATTLETOUCH __real_CtrDisplay_save
void Battletouch_stepHint(struct Runner* runner); // the other two are declared in z_battletouch.c, their only caller
#endif

// dimlight: z_leavekeep's post-claim CtrAptTrace_exit call is retargeted by
// CMake. Keep inputlate's exitonce link: no LCD work may precede its claim.

// Dimlight's context-specific retry targets. Replacing a no-argument call
// preserves existing slots; DELTARUNE keeps its original call targets.
#ifdef ENABLE_WAD16
int32_t CtrDimlight_topOnInit(void);
int32_t CtrDimlight_bottomOnInit(void);
int32_t CtrDimlight_topOffInit(void);
void CtrDimlight_topBacklight(void* renderer, bool off);
#else
#define CtrDimlight_topOnInit gspLcdInit
#define CtrDimlight_bottomOnInit gspLcdInit
#define CtrDimlight_topOffInit gspLcdInit
#define CtrDimlight_topBacklight screensTopBacklight
#endif

// bottomframe: prepare private CPU assets at the existing room-load hook.
// The idle paint is called by paintchg at its post-recording ownership point.
#ifdef ENABLE_WAD16
#ifndef CTRPORT_NO_BOTTOMFRAME
#undef CTR_PREFETCHROOM_AFTER_FLOWHITCH
#define CTR_PREFETCHROOM_AFTER_FLOWHITCH CtrBottomframe_prefetchRoom
void CtrBottomframe_prefetchRoom(struct Renderer* renderer);
#endif // CTRPORT_NO_BOTTOMFRAME
#endif

#endif
