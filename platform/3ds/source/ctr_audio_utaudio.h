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
// flowpitch (flowend): late code at the end of ctr_audio_system.c.
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
#define UTA_PHYS_PAUSED (ndspChnSetPaused)
#define UTA_PHYS_CLEAR (ndspChnWaveBufClear)
#define UTA_PHYS_ADD (ndspChnWaveBufAdd)
#define UTA_PHYS_CALLBACK (ndspSetCallback)
#define UTA_PHYS_READ (CtrDecoder_read)
#define UTA_PHYS_SEEK (CtrDecoder_seek)
#endif
#define UTA_PHYS_INTERP (ndspChnSetInterp)
#define UTA_PHYS_SEQ (ndspChnGetWaveBufSeq)
#define UTA_PHYS_POS (ndspChnGetSamplePos)
#define UTA_PHYS_OPEN_FILE (CtrDecoder_openFile)
#define UTA_PHYS_OPEN_MEMORY (CtrDecoder_openMemory)
#define UTA_PHYS_NOTICE (CtrNotice_missing)

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
// DELTARUNE keeps ctrPlaySound and pcmWaitUpdate.
#if defined(ENABLE_WAD16)
#define FLOWEND_BURST_DECL __attribute__((noinline, optimize("Os"), section(".text.flowburst_late"))) static int32_t \
    flowendPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop); \
    __attribute__((noinline, optimize("Os"), section(".text.flowburst_late"))) static void \
    flowendBurstUpdate(AudioSystem* audio, float deltaTime);
#define CTR_PLAY_ENTRY flowendPlaySound
#define CTR_UPDATE_ENTRY flowendBurstUpdate
#else
#define FLOWEND_BURST_DECL
#define CTR_PLAY_ENTRY ctrPlaySound
#define CTR_UPDATE_ENTRY pcmWaitUpdate
#endif

#endif
