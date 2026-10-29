// Minimal host device surface for testing the production native queue.
#ifndef TEST_NDSP_H
#define TEST_NDSP_H
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t s16;
typedef struct { u16 index; s16 history0, history1; } ndspAdpcmData;
typedef struct tag_ndspWaveBuf ndspWaveBuf;
struct tag_ndspWaveBuf {
    union { u8* data_adpcm; const void* data_vaddr; };
    u32 nsamples;
    ndspAdpcmData* adpcm_data;
    u32 offset;
    bool looping;
    u8 status;
    u16 sequence_id;
    ndspWaveBuf* next;
};
enum { NDSP_WBUF_FREE, NDSP_WBUF_QUEUED, NDSP_WBUF_PLAYING, NDSP_WBUF_DONE };
enum { NDSP_FORMAT_MONO_ADPCM = 9, NDSP_INTERP_POLYPHASE = 0 };
void ndspChnReset(int channel);
void ndspChnSetPaused(int channel, bool paused);
void ndspChnWaveBufClear(int channel);
void ndspChnSetFormat(int channel, u16 format);
void ndspChnSetAdpcmCoefs(int channel, u16* coefficients);
void ndspChnSetInterp(int channel, int interpolation);
void ndspChnSetRate(int channel, float rate);
void ndspChnSetMix(int channel, float mix[12]);
void ndspChnWaveBufAdd(int channel, ndspWaveBuf* wave);
u16 ndspChnGetWaveBufSeq(int channel);
u32 ndspChnGetSamplePos(int channel);
void DSP_FlushDataCache(const void* data, u32 bytes);
#endif
