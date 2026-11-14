// Late code for ctr_audio_system.c (utaudio), included by that file only, after its other headers.
//
// The code is at the end of ctr_audio_system.c, in late sections (platform/3ds/late_reserve.ld). These macros send the
// file's calls there (its NDSP channel calls, its frame callback, its decoder calls and its setup notice), so the
// existing functions keep their code and size and only their call targets change:
// - The setup notice for a missing sound file goes through utaNoticeMissing, a pass-through since 2026-10-21: round
//   5's exception for two music files was wrong (Steam ships both), so every file gets it.
// - Text and sound effect cuts (stop, then a new sound at once) no longer end the old sound at whatever level it was
//   on the DSP's 160-sample frame. A cached sound effect is given to the DSP in pieces of about a DSP frame, chained on
//   its channel a few frames ahead; a stop queues no more of it and instead adds the next 3 ms of its own samples,
//   faded out (the tail), after the last piece on the same channel, so the voice and its resampler carry on. A clear
//   plus a new buffer would restart the voice from empty history and click anyway. Channels the
//   engine names are "logical"; a voice whose channel still plays a tail is moved to an idle one when it starts its
//   next sound. With no idle channel left, the tail gives its channel up (the old hard cut).
// - A looping stream whose loop joins samples far apart (a file whose end doesn't meet its start: the step at the
//   splice at least 4x the largest step anywhere in the file, per channel, measured on the first full pass) gets a
//   ~3 ms decaying offset after each splice instead of the step. Nothing else in a stream changes. A cached looping
//   sound gets the same, measured once on its cached samples, as a seam piece the feeder queues at each splice.
// In a diagnosis build (audiocap) the capture's wrappers are the physical layer, so it records what the DSP is told.
#ifndef CTR_AUDIO_UTAUDIO_H
#define CTR_AUDIO_UTAUDIO_H

#include <3ds.h>
#include <stddef.h>

#include "ctr_audio_decoder.h"
#include "ctr_notice.h"

#define UTA_LATE __attribute__((noinline, noipa, section(".text.late.utaudio2")))
// The earlier utaNoticeMissing (5b5fcb65) keeps its own section, linked where it was first placed.
#define UTA_LATE_R5 __attribute__((noinline, noipa, section(".text.late.utaudio")))

UTA_LATE static void utaChnReset(int id);
UTA_LATE static void utaChnSetInterp(int id, ndspInterpType type);
UTA_LATE static void utaChnSetFormat(int id, u16 format);
UTA_LATE static void utaChnSetRate(int id, float rate);
UTA_LATE static void utaChnSetMix(int id, float mix[12]);
UTA_LATE static void utaChnSetPaused(int id, bool paused);
#if defined(ENABLE_WAD16)
#define LM_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text.late.loadaudio")))
LM_CODE static void lmPaused(int, bool);
LM_CODE static void lmReset(void);
LM_CODE static void lmCallback(void*);
LM_CODE static int lmDecodeWait(LightEvent*, s64);
LM_CODE static void lmRegister(ndspCallback, void*);
#endif
#if defined(ENABLE_WAD16) && defined(CTR_AUDIO_PCM_HASH)
#define LAT_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text.late.loadaudio")))
LAT_CODE static void latRequest(CtrAudioSystem*, int);
LAT_CODE static void latPhysicalAdd(int, ndspWaveBuf*);
LAT_CODE static void latSample(CtrAudioSystem*);
LAT_CODE static void latReset(void);
LAT_CODE static void latCall(CtrAudioSystem*, int32_t);
LAT_CODE static void latMark(uint32_t);
LAT_CODE static bool latOpenFile(CtrDecoder*, const char*);
LAT_CODE static bool latOpenMemory(CtrDecoder*, const uint8_t*, uint32_t);
LAT_CODE static int32_t latRead(CtrDecoder*, int16_t*, uint32_t);
LAT_CODE static bool latPreparationOn(void);
#endif
UTA_LATE static void utaChnWaveBufClear(int id);
UTA_LATE static void utaChnWaveBufAdd(int id, ndspWaveBuf* buf);
UTA_LATE static u16 utaChnGetWaveBufSeq(int id);
UTA_LATE static u32 utaChnGetSamplePos(int id);
UTA_LATE static void utaSetCallback(ndspCallback callback, void* data);
UTA_LATE static bool utaDecOpenFile(CtrDecoder* dec, const char* path);
UTA_LATE static bool utaDecOpenMemory(CtrDecoder* dec, const uint8_t* data, uint32_t size);
UTA_LATE static int32_t utaDecRead(CtrDecoder* dec, int16_t* out, uint32_t maxFrames);
UTA_LATE static bool utaDecSeek(CtrDecoder* dec, uint64_t frame);
UTA_LATE_R5 static void utaNoticeMissing(const char* kind, const char* path);
// flowpitch (change flowend): late code at the end of ctr_audio_system.c.
__attribute__((noinline, noipa, optimize("Os"), section(".text.flowpitch_late"))) static void flowpitchChnSetRate(int id, float rate);

// The physical layer: libctru's functions (a parenthesised name is not a function-like macro call), or the capture's.
#ifdef CTR_AUDIO_CAPTURE_WRAP_H
#undef ndspChnSetMix
#undef ndspChnWaveBufAdd
#undef ndspChnWaveBufClear
#undef ndspChnReset
#undef ndspChnSetFormat
#undef ndspChnSetPaused
#undef ndspChnSetRate
#undef ndspSetCallback
#undef CtrDecoder_read
#undef CtrDecoder_seek
#define UTA_PHYS_RESET CtrCap_chnReset
#define UTA_PHYS_FORMAT CtrCap_chnSetFormat
#define UTA_PHYS_RATE CtrCap_chnSetRate
#define UTA_PHYS_MIX CtrCap_chnSetMix
#define GOM_MIX_NEXT CtrCap_chnSetMix
#define UTA_PHYS_PAUSED CtrCap_chnSetPaused
#define UTA_PHYS_CLEAR CtrCap_chnWaveBufClear
#define UTA_PHYS_ADD CtrCap_chnWaveBufAdd
#define UTA_PHYS_CALLBACK CtrCap_setCallback
#define UTA_PHYS_READ CtrCap_decRead
#define UTA_PHYS_SEEK CtrCap_decSeek
#else
#define UTA_PHYS_RESET (ndspChnReset)
#define UTA_PHYS_FORMAT (ndspChnSetFormat)
#define UTA_PHYS_RATE (ndspChnSetRate)
#define UTA_PHYS_MIX (ndspChnSetMix)
#define GOM_MIX_NEXT (ndspChnSetMix)
#define UTA_PHYS_PAUSED (ndspChnSetPaused)
#define UTA_PHYS_CLEAR (ndspChnWaveBufClear)
#define UTA_PHYS_ADD (ndspChnWaveBufAdd)
#define UTA_PHYS_CALLBACK (ndspSetCallback)
#define UTA_PHYS_READ (CtrDecoder_read)
#define UTA_PHYS_SEEK (CtrDecoder_seek)
#endif
// gomusic (UNDERTALE): a stream's mix goes through gomPhysMix (ctr_audio_system.c's end), by an asm alias, so GCC sees
// an external call there as before.
#if defined(ENABLE_WAD16)
void gomMixLink(int id, float mix[12]);
#undef UTA_PHYS_MIX
#define UTA_PHYS_MIX gomMixLink
#endif
#define UTA_PHYS_INTERP (ndspChnSetInterp)
#define UTA_PHYS_SEQ (ndspChnGetWaveBufSeq)
#define UTA_PHYS_POS (ndspChnGetSamplePos)
#define UTA_PHYS_OPEN_FILE (CtrDecoder_openFile)
#define UTA_PHYS_OPEN_MEMORY (CtrDecoder_openMemory)
#ifdef ENABLE_WAD16
void Upd1_audioNotice(const char* kind, const char* path);
#define UTA_PHYS_NOTICE (Upd1_audioNotice)
#else
#define UTA_PHYS_NOTICE (CtrNotice_missing)
#endif
#line 122

#if defined(ENABLE_WAD16) && defined(CTR_AUDIO_PCM_HASH)
#undef UTA_PHYS_ADD
#define UTA_PHYS_ADD latPhysicalAdd
#undef UTA_PHYS_OPEN_FILE
#define UTA_PHYS_OPEN_FILE latOpenFile
#undef UTA_PHYS_OPEN_MEMORY
#define UTA_PHYS_OPEN_MEMORY latOpenMemory
#undef UTA_PHYS_READ
#define UTA_PHYS_READ latRead
#endif

// gounder (UNDERTALE Old): copies of the song playing at unequal pitches share one decode (ctr_audio_system.c's end). The
// decoder calls of the stream layer go through it, by asm aliases, so GCC sees one changed external call in each.
#if defined(ENABLE_WAD16) && defined(CTRPORT_TARGET_O3DS) && defined(CTRPORT_HAVE_TREMOR)
#if defined(CTR_AUDIO_PCM_HASH)
#define GUN_OPEN_FILE_NEXT latOpenFile
#define GUN_OPEN_MEMORY_NEXT latOpenMemory
#define GUN_READ_NEXT latRead
#else
#define GUN_OPEN_FILE_NEXT (CtrDecoder_openFile)
#define GUN_OPEN_MEMORY_NEXT (CtrDecoder_openMemory)
#ifdef CTR_AUDIO_CAPTURE_WRAP_H
#define GUN_READ_NEXT CtrCap_decRead
#else
#define GUN_READ_NEXT (CtrDecoder_read)
#endif
#endif
#ifdef CTR_AUDIO_CAPTURE_WRAP_H
#define GUN_SEEK_NEXT CtrCap_decSeek
#else
#define GUN_SEEK_NEXT (CtrDecoder_seek)
#endif
bool gunOpenFileLink(CtrDecoder* dec, const char* path);
bool gunOpenMemoryLink(CtrDecoder* dec, const uint8_t* data, uint32_t size);
int32_t gunReadLink(CtrDecoder* dec, int16_t* out, uint32_t maxFrames);
bool gunSeekLink(CtrDecoder* dec, uint64_t frame);
#undef UTA_PHYS_OPEN_FILE
#define UTA_PHYS_OPEN_FILE gunOpenFileLink
#undef UTA_PHYS_OPEN_MEMORY
#define UTA_PHYS_OPEN_MEMORY gunOpenMemoryLink
#undef UTA_PHYS_READ
#define UTA_PHYS_READ gunReadLink
#undef UTA_PHYS_SEEK
#define UTA_PHYS_SEEK gunSeekLink
void gunCloseLink(CtrDecoder* dec);
#define GUN_STREAM_CLOSE gunCloseLink
#else
#define GUN_STREAM_CLOSE CtrDecoder_close
#endif

#define ndspChnReset(id) utaChnReset(id)
#define ndspChnSetInterp(id, type) utaChnSetInterp(id, type)
#define ndspChnSetFormat(id, format) utaChnSetFormat(id, format)
// flowpitch: every voice at the game's frame clock (ctr_music_clock.h), then utaChnSetRate.
#define ndspChnSetRate(id, rate) flowpitchChnSetRate(id, rate)
#define ndspChnSetMix(id, mix) utaChnSetMix(id, mix)
#define ndspChnSetPaused(id, paused) utaChnSetPaused(id, paused)
#define ndspChnWaveBufClear(id) utaChnWaveBufClear(id)
#define ndspChnWaveBufAdd(id, buf) utaChnWaveBufAdd(id, buf)
#define ndspChnGetWaveBufSeq(id) utaChnGetWaveBufSeq(id)
#define ndspChnGetSamplePos(id) utaChnGetSamplePos(id)
#define ndspSetCallback(callback, data) utaSetCallback(callback, data)
#define CtrDecoder_openFile(dec, path) utaDecOpenFile(dec, path)
#define CtrDecoder_openMemory(dec, data, size) utaDecOpenMemory(dec, data, size)
#define CtrDecoder_read(dec, out, maxFrames) utaDecRead(dec, out, maxFrames)
#define CtrDecoder_seek(dec, frame) utaDecSeek(dec, frame)
#define CtrNotice_missing(kind, path) utaNoticeMissing(kind, path)

// A wave buffer the feeder doesn't take goes to the DSP as it is. The ADPCM builds redefine UTA_RAW_ADD to drop one
// that would carry compressed bytes (adSfxPacked). UTA_FAULT_ALLOC: the feeder's and the pack's linear allocations;
// the test-only build word adpcmfault makes one of them fail (1 tail, 2 seam, 3 piece pool, 4 a voice's ring).
#define UTA_RAW_ADD(p, buf) UTA_PHYS_ADD(p, buf)
#ifdef CTRPORT_ADPCM_FAULT
#define UTA_FAULT_ALLOC(k, n) ((k) == CTRPORT_ADPCM_FAULT ? NULL : linearAlloc(n))
#else
#define UTA_FAULT_ALLOC(k, n) linearAlloc(n)
#endif

// utstall5 (UNDERTALE): a silent play (no DSP: no dspfirm.cdc, or Azahar without it) asks for the sound's length,
// and soundLength read a small embedded sound's whole blob for it on the game thread (a text sound, 302 KB: 56.8 ms on
// Azahar Old at every battle's first frame). utsSilentLength, late code at the end of ctr_audio_system.c, reads only
// the WAV header for the same length and falls back to soundLength for anything else. DELTARUNE keeps soundLength.
#if defined(ENABLE_WAD16)
#define UTS_LATE __attribute__((noinline, noipa, section(".text.late.utstall")))
UTS_LATE static float utsSilentLength(CtrAudioSystem* ca, int32_t soundIndex);
#else
#define utsSilentLength(ca, soundIndex) soundLength(ca, soundIndex)
#endif

// flowend burst (UNDERTALE): the vtable's playSound and update entries, late code at the end of ctr_audio_system.c
// (flowendPlaySound: a step's plays past the first FLOWEND_BURST_FIRST start at the frame's audio update).
// DELTARUNE keeps ctrPlaySound and pcmWaitUpdate. sndcut (UNDERTALE): stopSound is sndcutStopSound, at the file's end.
#if defined(ENABLE_WAD16)
#define FLOWEND_BURST_DECL __attribute__((noinline, optimize("Os"), section(".text.flowburst_late"))) static int32_t \
    flowendPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop); \
    __attribute__((noinline, optimize("Os"), section(".text.flowburst_late"))) static void \
    flowendBurstUpdate(AudioSystem* audio, float deltaTime); \
    __attribute__((noinline, optimize("Os"), section(".text.sndcut_late"))) static void \
    sndcutStopSound(AudioSystem* audio, int32_t soundOrInstance);
#define CTR_PLAY_ENTRY flowendPlaySound
#define CTR_UPDATE_ENTRY flowendBurstUpdate
#define CTR_STOP_ENTRY sndcutStopSound
#else
#define FLOWEND_BURST_DECL
#define CTR_PLAY_ENTRY ctrPlaySound
#define CTR_UPDATE_ENTRY pcmWaitUpdate
#define CTR_STOP_ENTRY ctrStopSound
#endif
// audofix drone (UNDERTALE, Tremor): the update publishes the cached copies of the sounds that streams play
// (audofixUpdate, then flowendBurstUpdate), flowhitch's read link reads them (audofixFeedRead, then the real read),
// and pcmwait's frees take a copy off first (audofixFree, then the real free). The two links are asm aliases, so
// GCC sees the same external calls there as before. ctr_audio_system.c's end says why.
// audofix play-through (UNDERTALE): the load hold's two physical pause writes (lmPaused, the load transport) go
// through audofixPtPaused, an asm alias of ptPhysPaused (ctr_loadaudio_mix.inc's end), so GCC sees the same external
// call there as before.
#define PT_PHYS_PAUSED audofixPtPaused
void audofixPtPaused(int p, bool paused);
// gomusic (UNDERTALE, change omegaflowey): a stream's open request (startStream's unlock after it publishes the request)
// goes through gomRequested, which notes the request's frame, so streams of one sound asked for in the same frame start
// together (ctr_audio_system.c's end). Same call signature: GCC sees one changed call target there and nothing else.
#if defined(ENABLE_WAD16)
__attribute__((noinline, noipa, optimize("Os"), section(".text_gomusic"))) static void gomRequested(LightLock* lock);
#define GOM_PUBLISH(lock) gomRequested(lock)
#else
#define GOM_PUBLISH(lock) LightLock_Unlock(lock)
#endif
#if defined(ENABLE_WAD16) && defined(CTRPORT_HAVE_TREMOR)
// gomusic: the update entry is gomUpdate, which notes each step's voice serials and then runs audofixUpdate
// (ctr_audio_system.c's end).
#define AUDOFIX_FEED_DECL __attribute__((noinline, noipa, optimize("Os"), section(".text.audofix_feed"))) static void \
    audofixUpdate(AudioSystem* audio, float deltaTime); \
    __attribute__((noinline, noipa, optimize("Os"), section(".text_gomusic"))) static void gomUpdate(AudioSystem* audio, float deltaTime);
#undef CTR_UPDATE_ENTRY
#define CTR_UPDATE_ENTRY gomUpdate
#define GOM_UPDATE_NEXT audofixUpdate
#define AUDOFIX_FREE_LINK audofixFreeLink
#define AUDOFIX_READ_LINK audofixReadLink
#else
#define AUDOFIX_FEED_DECL
#define AUDOFIX_FREE_LINK __wrap_linearFree
#define AUDOFIX_READ_LINK __real_CtrDecoder_read
#endif

// cbcut A: a tail's or a loop seam's PCM leaves the CPU's data cache as the ADPCM feeder's pieces do (adFlush in
// ctr_adpcm_sfx_pieces.inc: the kernel SVC where it is allowed, the dsp service otherwise), not through the dsp
// service's IPC, which waits behind core 1 inside the NDSP callback (~0.6 ms a tail in maintainer's Omega Flowey log).
// Same bytes flushed at the same point, so the DSP reads the same samples. adFlush is reached by its assembler name:
// a direct call to the static function adds call-graph edges, and GCC then emits utaStop 1280 B later in
// .text.late.utaudio2 (check_layout --full FAIL); through this declaration every function keeps its place.
// Builds without the ADPCM feeder keep the service.
#if defined(CTRPORT_ADPCM_SFX) || defined(CTRPORT_ADPCM_MUSIC) || defined(CTRPORT_ADPCM_SFXPACK)
void utaDeclickFlush(const void* data, uint32_t bytes) __asm__("adFlush");
#define UTA_DECLICK_FLUSH utaDeclickFlush
#else
#define UTA_DECLICK_FLUSH DSP_FlushDataCache
#endif

// musicloop (UNDERTALE): a stopped stream fades out as on PC, and a loop seam is smoothed on both channels once either
// qualifies (ctr_audio_system.c's end). Each hook is one changed external call target or one inline call in a nop pad,
// so no function of the file gains or loses a call-graph edge (which would reorder them) and none changes size:
// - the stream layer's physical clear (UTA_PHYS_CLEAR) goes through mlPhysClear, which leaves a just-stopped stream's
//   buffers playing, faded out in place, for the frame callback to cut once they are silent;
// - claimStreamContext's free context goes through mlClaim (in its nop pad), which passes over a context whose buffers
//   still fade;
// - utaDecRead's decoder read (UTA_PHYS_READ) goes through mlRead, which at a loop splice lets a channel's seam rule
//   follow the other channel's.
#if defined(ENABLE_WAD16)
#ifdef CTR_AUDIO_CAPTURE_WRAP_H
#define ML_CLEAR_NEXT CtrCap_chnWaveBufClear
#else
#define ML_CLEAR_NEXT (ndspChnWaveBufClear)
#endif
#if defined(CTRPORT_TARGET_O3DS) && defined(CTRPORT_HAVE_TREMOR)
#define ML_READ_NEXT gunReadLink
#elif defined(CTR_AUDIO_PCM_HASH)
#define ML_READ_NEXT latRead
#elif defined(CTR_AUDIO_CAPTURE_WRAP_H)
#define ML_READ_NEXT CtrCap_decRead
#else
#define ML_READ_NEXT (CtrDecoder_read)
#endif
void mlPhysClearLink(int p) __asm__("mlPhysClear");
int32_t mlReadLink(CtrDecoder* dec, int16_t* out, uint32_t maxFrames) __asm__("mlRead");
#undef UTA_PHYS_CLEAR
#define UTA_PHYS_CLEAR mlPhysClearLink
#undef UTA_PHYS_READ
#define UTA_PHYS_READ mlReadLink
#define ML_CLAIM(ca, ctx) ({ \
    extern int mlClaimLink(CtrAudioSystem*, int) __asm__("mlClaim"); \
    (ctx) = mlClaimLink((ca), (ctx)); \
    __asm__ volatile(".rept 21; nop; .endr"); \
})
// The options menu's hold starts (menuHeldUpdate's picture pause, same call, same argument): mlMenuPause first cuts a
// stopped stream's fade, which has no voice for the hold to pause. HOME's freeze does the same (z_argcrash.c).
void mlMenuPauseLink(bool pause) __asm__("mlMenuPause");
#define ML_MENU_PAUSE mlMenuPauseLink
// A resumed sound fades in, as on PC (F5): utaSetCallback's registration (ML_REGISTER) goes through mlRegister, which
// points the vtable's resume entries at musicloop's; the physical mix write after gomPhysMix (GOM_MIX_NEXT) goes through
// mlPhysMix, which scales a fading channel's mix; the frame callback's lock (ML_FRAME_LOCK) goes through mlFrameLock,
// which steps the fades on, one DSP frame at a time; every physical pause write (UTA_PHYS_PAUSED) goes through
// mlPhysPaused, which holds a resumed channel's unpause until its silent mix has reached the DSP. ML_MIX_NEXT and
// ML_PAUSED_NEXT are the physical writes they make.
void mlRegisterLink(ndspCallback callback, void* data) __asm__("mlRegister");
#define ML_REGISTER mlRegisterLink
void mlFrameLockLink(LightLock* lock) __asm__("mlFrameLock");
#define ML_FRAME_LOCK mlFrameLockLink
#ifdef CTR_AUDIO_CAPTURE_WRAP_H
#define ML_MIX_NEXT CtrCap_chnSetMix
#define ML_PAUSED_NEXT CtrCap_chnSetPaused
#else
#define ML_MIX_NEXT (ndspChnSetMix)
#define ML_PAUSED_NEXT (ndspChnSetPaused)
#endif
void mlPhysMixLink(int id, float mix[12]) __asm__("mlPhysMix");
#undef GOM_MIX_NEXT
#define GOM_MIX_NEXT mlPhysMixLink
void mlPhysPausedLink(int id, bool paused) __asm__("mlPhysPaused");
#undef UTA_PHYS_PAUSED
#define UTA_PHYS_PAUSED mlPhysPausedLink
#else
#define ML_CLAIM(ca, ctx) ({ __asm__ volatile(".rept 24; nop; .endr"); })
#define ML_MENU_PAUSE CtrVideo_menuPause
#define ML_FRAME_LOCK LightLock_Lock
#endif

// loadclass 4c (UNDERTALE): two layers of one song played next to each other join sample-locked (ctr_audio_system.c's
// end). Its DSP frame link comes before gomusic's (lmRegister registers it in gomDspCallback's place, which it calls
// first) and its pause link before gomusic's hold (the asm alias at ctr_loadaudio_mix.inc's end). Elsewhere both links
// are gomusic's own, as before. lmRegister calls loadclassCallbackLink, an asm alias of loadclassSetCallback, so GCC
// sees an external call there as before (a call into this file makes it emit the load mix's functions in another order).
#if defined(ENABLE_WAD16) && defined(CTRPORT_HAVE_TREMOR)
void loadclassSetCallback(ndspCallback callback, void* data);
void loadclassCallbackLink(ndspCallback callback, void* data);
void loadclassPhysPaused(int p, bool paused);
#define LOADCLASS_PHYS_CALLBACK loadclassCallbackLink
#define LOADCLASS_PAUSE_LINK "loadclassPhysPaused"
#else
#define LOADCLASS_PHYS_CALLBACK UTA_PHYS_CALLBACK
#define LOADCLASS_PAUSE_LINK "gomPhysPaused"
#endif

// hplcopy (UNDERTALE Old): a short looping stream played fast (pitch 2 or more) is decoded once into memory on the app
// core, at low priority, and the stream reads that copy once it is whole (ctr_audio_system.c's end). utaSetCallback's
// registration (ML_REGISTER, one changed external call target) goes through hplRegister first, which points the
// vtable's update at hplUpdate (it runs the file's own, then starts and frees the copies from the voices), then on to
// mlRegister; flowhitch's read link (audofixReadLink, an asm alias) is hplcopy's read, which falls through to
// audofix's.
#if defined(ENABLE_WAD16) && defined(CTRPORT_TARGET_O3DS) && defined(CTRPORT_HAVE_TREMOR)
void hplRegisterLink(ndspCallback callback, void* data) __asm__("hplRegister");
#undef ML_REGISTER
#define ML_REGISTER hplRegisterLink
#define HPL_READ_LINK "hplFeedRead"
#else
#define HPL_READ_LINK "audofixFeedRead"
#endif

// seamlocal (UNDERTALE, row 23): a loop seam is measured against the steps within 10 ms of it, not the file's
// largest step anywhere (ctr_audio_system.c's end). utaDecRead's decoder read (UTA_PHYS_READ) goes through slRead,
// which measures the stream's first pass on its way to mlRead; utaSetCallback's registration (ML_REGISTER) goes
// through slRegister, which points the vtable's update and play entries at seamlocal's (they set a cached loop's seam
// triggers before its seam piece is built) and then runs the chain above. Each is one changed external call target.
// UNDERTALE only: slRead and slRegister are defined only there (ctr_audio_system.c's end, under ENABLE_WAD16), so a
// DELTARUNE build keeps the chain above (unguarded, its link failed on the undefined slRead).
#if defined(ENABLE_WAD16)
int32_t slReadLink(CtrDecoder* dec, int16_t* out, uint32_t maxFrames) __asm__("slRead");
#undef UTA_PHYS_READ
#define UTA_PHYS_READ slReadLink
void slRegisterLink(ndspCallback callback, void* data) __asm__("slRegister");
#undef ML_REGISTER
#define ML_REGISTER slRegisterLink
#endif

// lcstop (change loadclass, 11-09; UNDERTALE): the vtable's stopSound and stopAll go through lcstopStopSound and
// lcstopStopAll (ctr_audio_system.c's end), which note the sounds they stop for the asset query's short window;
// ctrStopAll and sndcutStopSound keep their code. The declarations ride on the vtable's line (LCSTOP_DECL).
#if defined(ENABLE_WAD16)
#define LCSTOP_DECL __attribute__((noinline, optimize("Os"), section(".text_lcstop"))) static void \
    lcstopStopSound(AudioSystem* audio, int32_t soundOrInstance); \
    __attribute__((noinline, optimize("Os"), section(".text_lcstop"))) static void lcstopStopAll(AudioSystem* audio);
#undef CTR_STOP_ENTRY
#define CTR_STOP_ENTRY lcstopStopSound
#define CTR_STOPALL_ENTRY lcstopStopAll
#else
#define LCSTOP_DECL
#define CTR_STOPALL_ENTRY ctrStopAll
#endif

#endif
