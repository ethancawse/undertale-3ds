// Native DSP format check before game audio starts. Diagnostic builds only.
// Queue one DTAD channel on the left and its C reference render on the right.
// Capture the final stereo mix using the same public shared-memory observation
// as ctr_audio_capture.c; no hardware access is performed by the build tools.
#include <3ds.h>
#include <limits.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ctr_adpcm_format.h"
#include "ctr_adpcm_player.h"
#include "ctr_audio_system.h"
#include "log.h"

#define AP_CODE __attribute__((noinline, noipa, section(".text.late.adpcm")))
#define AP_RO __attribute__((section(".rodata.late.adpcm")))
#define AP_BSS __attribute__((section(".bss.late.adpcm")))
#define AP_CAPTURE_FRAMES 96u
#define AP_PLAY_BLOCKS 6u

extern void* ndspVars[16][2];
extern u16 ndspBufferId;
CtrAudioSystem* __real_CtrAudioSystem_create(void);

static const char apDirectory[] AP_RO = "sdmc:/3ds/undertale-3ds/adpcm-probe";
static const char apNames[][24] AP_RO = {
    "01_boss", "02_hopes", "03_quiet", "04_dr_ch4", "05_sfx_sparkles", "06_sfx_explosion"
};
static const char apInput[] AP_RO = "%s/%s.dtad";
static const char apOutput[] AP_RO = "%s/%s-ch%lu-block%lu.pcm";
static const char apRead[] AP_RO = "rb";
static const char apWrite[] AP_RO = "wb";
static const char apCase[] AP_RO =
    "[adpcmprobe] %s ch %lu block %lu: %lu source frames; %lu captured frames; %lu nonzero; %lu unequal; max %lu; %s\n";
static const char apResult[] AP_RO = "[adpcmprobe] COMPLETE %lu cases; %lu failed; %lu nonzero samples\n";
static const char apFailed[] AP_RO = "FAIL";
static const char apPassed[] AP_RO = "PASS";
static const char apHeap[] AP_RO = "[adpcmprobe] standalone heap before %lu B after %lu B\n";
static const char apQueue[] AP_RO = "[adpcmprobe] native queue: 4 blocks; 6-block spans; reads outside callback\n";

static volatile uint32_t apState AP_BSS;
static volatile uint32_t apCaptured AP_BSS;
static int16_t* apCapture AP_BSS;
static CtrAdpcmPlayer* apPlayer AP_BSS;
static LightLock apLock AP_BSS;

// Runs before libctru publishes this callback's channel updates to the DSP.
// Both source channels therefore start on the same DSP frame.
AP_CODE static void apCallback(void* unused) {
    (void)unused;
    LightLock_Lock(&apLock);
    if (apPlayer != NULL) CtrAdpcmPlayer_frame(apPlayer);
    if (apState == 1) {
        ndspChnSetPaused(1, false);
        apCaptured = 0;
        apState = 2;
    } else if (apState == 2) {
        const void* mix = ndspVars[6][ndspBufferId & 1u];
        if (mix != NULL) memcpy(apCapture + apCaptured * 320u, mix, 640u);
        else memset(apCapture + apCaptured * 320u, 0, 640u);
        if (++apCaptured >= AP_CAPTURE_FRAMES) {
            __sync_synchronize();
            apState = 3;
        }
    }
    LightLock_Unlock(&apLock);
}

AP_CODE static bool apReadAt(void* owner, uint64_t offset, void* out, uint32_t bytes) {
    FILE* file = (FILE*)owner;
    return offset <= LONG_MAX && fseek(file, (long)offset, SEEK_SET) == 0 && fread(out, 1, bytes, file) == bytes;
}

typedef struct {
    FILE* file;
    uint8_t header[CTR_ADPCM_HEADER_BYTES];
    uint64_t bytes;
} ApView;

AP_CODE static bool apViewRead(void* owner, uint64_t offset, void* out, uint32_t bytes) {
    ApView* v = owner;
    if (offset > v->bytes || bytes > v->bytes - offset) return false;
    if (offset < sizeof(v->header)) {
        if (bytes > sizeof(v->header) - offset) return false;
        memcpy(out, v->header + (size_t)offset, bytes);
        return true;
    }
    return apReadAt(v->file, offset, out, bytes);
}

AP_CODE static void apLe32(uint8_t* p, uint32_t value) {
    for (unsigned n = 0; n < 4; n++) p[n] = (uint8_t)(value >> (n * 8));
}

AP_CODE static bool apRunCase(const CtrAdpcmInfo* info, FILE* file, uint32_t ch,
                              const char* name, uint32_t index, uint8_t* storage,
                              int16_t* reference, uint32_t* nonzeroOut) {
    int16_t decoded[CTR_ADPCM_BLOCK_FRAMES * 2u];
    uint32_t end = index + AP_PLAY_BLOCKS;
    if (end > info->blocks) end = info->blocks;
    uint32_t frames = 0;
    for (uint32_t b = index; b < end; b++) {
        CtrAdpcmBlock block;
        if (!CtrAdpcm_readBlock(info, apReadAt, file, b, storage,
                CTR_ADPCM_QUEUE_BLOCKS * CTR_ADPCM_STEREO_BLOCK_BYTES, &block) ||
            !CtrAdpcm_decodeSpan(info, &block, 0, block.frames, decoded, CTR_ADPCM_BLOCK_FRAMES)) return false;
        for (uint32_t i = 0; i < block.frames; i++) reference[frames + i] = decoded[i * info->channels + ch];
        frames += block.frames;
    }
    DSP_FlushDataCache(reference, frames * sizeof(int16_t));
    ApView view = {.file = file};
    uint64_t offset;
    uint32_t bytes, lastFrames;
    if (!apReadAt(file, 0, view.header, sizeof(view.header)) ||
        !CtrAdpcm_blockRange(info, end - 1u, &offset, &bytes, &lastFrames)) return false;
    view.bytes = offset + bytes;
    apLe32(view.header + 20, index * CTR_ADPCM_BLOCK_FRAMES + frames);
    apLe32(view.header + 32, end);
    CtrAdpcmSource source = {apViewRead, &view, view.bytes, NULL};
    CtrAdpcmPlayer player = {0};
    int channels[2] = {0, 2};
    if (!CtrAdpcmPlayer_start(&player, &source, storage,
            CTR_ADPCM_QUEUE_BLOCKS * CTR_ADPCM_STEREO_BLOCK_BYTES,
            channels, index * CTR_ADPCM_BLOCK_FRAMES, false, false)) return false;
    CtrAdpcmPlayer_rate(&player, NDSP_SAMPLE_RATE);
    float native[2][12] = {{0}};
    native[ch][0] = 0.25f;
    CtrAdpcmPlayer_mix(&player, native[0], native[1]);
    for (uint32_t c = 0; c < info->channels; c++) ndspChnSetInterp(channels[c], NDSP_INTERP_NONE);
    ndspWaveBuf wave = {0};
    wave.data_pcm16 = reference;
    wave.nsamples = frames;
    {
        ndspChnReset(1);
        ndspChnSetPaused(1, true);
        ndspChnSetInterp(1, NDSP_INTERP_NONE);
        ndspChnSetRate(1, NDSP_SAMPLE_RATE);
        ndspChnSetFormat(1, NDSP_FORMAT_MONO_PCM16);
        float mix[12] = {0};
        mix[1] = 0.25f;
        ndspChnSetMix(1, mix);
    }
    ndspChnWaveBufAdd(1, &wave);
    memset(apCapture, 0, AP_CAPTURE_FRAMES * 640u);
    LightLock_Lock(&apLock);
    apPlayer = &player;
    apState = 1;
    LightLock_Unlock(&apLock);
    uint64_t deadline = svcGetSystemTick() + 2u * SYSCLOCK_ARM11;
    bool refillOk = true;
    while (apState != 3 && svcGetSystemTick() < deadline) {
        CtrAdpcmRequest request;
        LightLock_Lock(&apLock);
        bool requested = CtrAdpcmPlayer_request(&player, &request);
        LightLock_Unlock(&apLock);
        if (requested) {
            CtrAdpcmPlayer_read(&request); // No SD read holds the callback lock.
            LightLock_Lock(&apLock);
            refillOk &= CtrAdpcmPlayer_commit(&player, &request);
            LightLock_Unlock(&apLock);
        } else svcSleepThread(1000000);
    }
    bool complete = apState == 3;
    __sync_synchronize();
    LightLock_Lock(&apLock);
    apState = 0;
    apPlayer = NULL;
    LightLock_Unlock(&apLock);
    // Stop observing the old case before reusing its wave/context storage.
    ndspSetCallback(NULL, NULL);
    CtrAdpcmPlayer_stop(&player);
    ndspChnWaveBufClear(1);
    uint32_t nonzero = 0, unequal = 0, largest = 0;
    for (uint32_t i = 0; i < apCaptured * 160u; i++) {
        int32_t a = apCapture[i * 2u], b = apCapture[i * 2u + 1u];
        uint32_t delta = (uint32_t)abs(a - b);
        if (a != 0 || b != 0) nonzero++;
        if (delta != 0) unequal++;
        if (delta > largest) largest = delta;
    }
    char path[192];
    snprintf(path, sizeof(path), apOutput, apDirectory, name, (unsigned long)ch, (unsigned long)index);
    FILE* out = fopen(path, apWrite);
    bool saved = out != NULL;
    if (out != NULL) {
        saved = fwrite(apCapture, 640u, apCaptured, out) == apCaptured;
        if (fclose(out) != 0) saved = false;
    }
    bool ok = complete && saved && refillOk && unequal == 0;
    *nonzeroOut += nonzero;
    logInfo(apCase, name, (unsigned long)ch, (unsigned long)index, (unsigned long)frames,
        (unsigned long)apCaptured * 160u, (unsigned long)nonzero, (unsigned long)unequal,
        (unsigned long)largest, ok ? apPassed : apFailed);
    ndspSetCallback(apCallback, NULL);
    return ok;
}

AP_CODE static Result apRun(void) {
    Result result = ndspInit();
    if (R_FAILED(result)) return result;
    uint8_t* storage = (uint8_t*)linearAlloc(CTR_ADPCM_QUEUE_BLOCKS * CTR_ADPCM_STEREO_BLOCK_BYTES);
    int16_t* reference = (int16_t*)linearAlloc(AP_PLAY_BLOCKS * CTR_ADPCM_BLOCK_FRAMES * sizeof(int16_t));
    apCapture = (int16_t*)malloc(AP_CAPTURE_FRAMES * 640u);
    uint32_t cases = 0, failed = 0, nonzero = 0;
    if (storage == NULL || reference == NULL || apCapture == NULL) failed++;
    else {
        logInfo(apQueue);
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspSetClippingMode(NDSP_CLIP_NORMAL);
        ndspSetMasterVol(1.0f);
        ndspSetCallback(apCallback, NULL);
        for (uint32_t sample = 0; sample < 6; sample++) {
            char path[192];
            snprintf(path, sizeof(path), apInput, apDirectory, apNames[sample]);
            FILE* file = fopen(path, apRead);
            CtrAdpcmInfo info;
            uint8_t header[CTR_ADPCM_HEADER_BYTES];
            bool ok = file != NULL && fseek(file, 0, SEEK_END) == 0;
            long bytes = ok ? ftell(file) : -1;
            ok = ok && bytes > 0 && fseek(file, 0, SEEK_SET) == 0 && fread(header, 1, sizeof(header), file) == sizeof(header) &&
                CtrAdpcm_parse(header, sizeof(header), (uint64_t)bytes, &info);
            if (!ok) failed++;
            else {
                uint32_t indices[3] = {info.blocks > 32u ? 32u : info.blocks / 4u,
                                      info.blocks / 2u, info.blocks - 1u};
                for (uint32_t position = 0; position < 3; position++) {
                    for (uint32_t ch = 0; ch < info.channels; ch++) {
                        cases++;
                        if (!apRunCase(&info, file, ch, apNames[sample], indices[position], storage, reference, &nonzero)) failed++;
                    }
                }
            }
            if (file != NULL) fclose(file);
        }
        ndspSetCallback(NULL, NULL);
    }
    for (int ch = 0; ch < 24; ch++) ndspChnReset(ch);
    linearFree(storage);
    linearFree(reference);
    free(apCapture);
    apCapture = NULL;
    ndspExit();
    if (cases != 36 || nonzero < 64u) failed++;
    logInfo(apResult, (unsigned long)cases, (unsigned long)failed, (unsigned long)nonzero);
    return result;
}

// The factory runs before Runner_create initializes the renderer's texture
// worker. A long probe inside audio.init let that worker accumulate packed
// pages before the first frame could collect them, exhausting the Old heap.
// Own the diagnostic NDSP session here and close it before ordinary init.
AP_CODE CtrAudioSystem* __wrap_CtrAudioSystem_create(void) {
    struct mallinfo before = mallinfo();
    apRun();
    struct mallinfo after = mallinfo();
    logInfo(apHeap, (unsigned long)before.uordblks, (unsigned long)after.uordblks);
    return __real_CtrAudioSystem_create();
}
