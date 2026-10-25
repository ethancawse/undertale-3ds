// Software H.264 decoding: openh264's decoder (BSD-2-Clause), built for the
// ARM11 as C++ without assembly (its ARM code needs NEON).
#include "ctr_video_sw.h"

#ifdef CTRPORT_SW_DECODE
#include <3ds.h>
#include <stdlib.h>
#include <string.h>

#include "codec_api.h"

struct CtrSwDecoder {
    ISVCDecoder* oh;
    int32_t lastError;
};

static void takePicture(unsigned char* planes[3], const SBufferInfo* info, CtrSwPicture* out) {
    for (int i = 0; 3 > i; i++) out->planes[i] = planes[i];
    out->strides[0] = info->UsrData.sSystemBuffer.iStride[0];
    out->strides[1] = info->UsrData.sSystemBuffer.iStride[1];
    out->width = info->UsrData.sSystemBuffer.iWidth;
    out->height = info->UsrData.sSystemBuffer.iHeight;
}

CtrSwDecoder* CtrSwDecoder_open(void) {
    CtrSwDecoder* d = calloc(1, sizeof(*d));
    if (d == NULL) return NULL;
    if (WelsCreateDecoder(&d->oh) != 0 || d->oh == NULL) {
        free(d);
        return NULL;
    }
    int level = WELS_LOG_QUIET;
    (*d->oh)->SetOption(d->oh, DECODER_OPTION_TRACE_LEVEL, &level);
    SDecodingParam param;
    memset(&param, 0, sizeof(param));
    param.uiTargetDqLayer = 0xFF;
    param.eEcActiveIdc = ERROR_CON_DISABLE;
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    if ((*d->oh)->Initialize(d->oh, &param) != 0) {
        WelsDestroyDecoder(d->oh);
        free(d);
        return NULL;
    }
    return d;
}

int32_t CtrSwDecoder_decode(CtrSwDecoder* d, const uint8_t* data, uint32_t size, CtrSwPicture* out) {
    unsigned char* planes[3] = { NULL, NULL, NULL };
    SBufferInfo info;
    memset(&info, 0, sizeof(info));
    DECODING_STATE state = (*d->oh)->DecodeFrameNoDelay(d->oh, data, (int) size, planes, &info);
    if (state != dsErrorFree) d->lastError = (int32_t) state;
    if (state != dsErrorFree && info.iBufferStatus != 1) return -1;
    if (info.iBufferStatus != 1) return 0;
    takePicture(planes, &info, out);
    return 1;
}

int32_t CtrSwDecoder_drain(CtrSwDecoder* d, CtrSwPicture* out) {
    unsigned char* planes[3] = { NULL, NULL, NULL };
    SBufferInfo info;
    memset(&info, 0, sizeof(info));
    (*d->oh)->FlushFrame(d->oh, planes, &info);
    if (info.iBufferStatus != 1) return 0;
    takePicture(planes, &info, out);
    return 1;
}

int32_t CtrSwDecoder_lastError(const CtrSwDecoder* d) { return d->lastError; }

void CtrSwDecoder_close(CtrSwDecoder* d) {
    if (d == NULL) return;
    (*d->oh)->Uninitialize(d->oh);
    WelsDestroyDecoder(d->oh);
    free(d);
}

// ===[ Halving ]===

static void halvePlane(const uint8_t* src, int32_t srcStride, int32_t outW, int32_t outH, uint8_t* dst, int32_t dstStride) {
    for (int32_t y = 0; outH > y; y++) {
        const uint8_t* a = src + (size_t) (2 * y) * (size_t) srcStride;
        const uint8_t* b = a + srcStride;
        uint8_t* o = dst + (size_t) y * (size_t) dstStride;
        for (int32_t x = 0; outW > x; x++) {
            o[x] = (uint8_t) ((a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2);
        }
    }
}

void CtrSwPicture_halve(const CtrSwPicture* p, int32_t width, int32_t height, uint8_t* buffer, int32_t stride, int32_t rows,
                        CtrSwPicture* out) {
    int32_t w = (width / 2) & ~1, h = (height / 2) & ~1;
    uint8_t* y = buffer;
    uint8_t* u = y + (size_t) stride * (size_t) rows;
    uint8_t* v = u + (size_t) (stride / 2) * (size_t) (rows / 2);
    halvePlane(p->planes[0], p->strides[0], w, h, y, stride);
    halvePlane(p->planes[1], p->strides[1], w / 2, h / 2, u, stride / 2);
    halvePlane(p->planes[2], p->strides[1], w / 2, h / 2, v, stride / 2);
    out->planes[0] = y;
    out->planes[1] = u;
    out->planes[2] = v;
    out->strides[0] = stride;
    out->strides[1] = stride / 2;
    out->width = w;
    out->height = h;
}

void CtrSwPicture_copy(const CtrSwPicture* p, int32_t width, int32_t height, uint8_t* buffer, int32_t stride, int32_t rows,
                       CtrSwPicture* out) {
    // Whole blocks of 8, as Y2R reads them.
    int32_t w = (width + 7) & ~7, h = (height + 7) & ~7;
    if (w > stride) w = stride;
    if (h > rows) h = rows;
    uint8_t* planes[3] = { buffer, buffer + (size_t) stride * (size_t) rows, NULL };
    planes[2] = planes[1] + (size_t) (stride / 2) * (size_t) (rows / 2);
    for (int i = 0; 3 > i; i++) {
        int32_t pw = i == 0 ? w : w / 2, ph = i == 0 ? h : h / 2, ps = i == 0 ? stride : stride / 2;
        int32_t srcStride = p->strides[i == 0 ? 0 : 1];
        for (int32_t y = 0; ph > y; y++) memcpy(planes[i] + (size_t) y * (size_t) ps, p->planes[i] + (size_t) y * (size_t) srcStride, (size_t) pw);
        out->planes[i] = planes[i];
    }
    out->strides[0] = stride;
    out->strides[1] = stride / 2;
    out->width = width;
    out->height = height;
}

// ===[ Y2R ]===

static Handle y2rEvent;
static bool converting; // a strip started and not yet waited for

bool CtrSwConvert_open(void) {
    if (R_FAILED(y2rInit())) return false;
    Y2RU_StopConversion();
    Y2RU_SetTransferEndInterrupt(true);
    if (R_FAILED(Y2RU_GetTransferEndEvent(&y2rEvent))) {
        y2rExit();
        return false;
    }
    return true;
}

void CtrSwConvert_close(void) {
    converting = false;
    Y2RU_StopConversion();
    y2rExit();
    y2rEvent = 0;
}

static void flushRange(const void* p, uint32_t size) { svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32) p, size); }

// Starts one strip of at most 1024 pixels (Y2R's widest line), x0 a multiple of 8.
static bool startStrip(const CtrSwPicture* p, int32_t x0, int32_t w, int32_t lines, uint16_t* dst, int32_t dstStride, bool bt709) {
    Y2RU_ConversionParams params;
    memset(&params, 0, sizeof(params));
    params.input_format = INPUT_YUV420_INDIV_8;
    params.output_format = OUTPUT_RGB_16_565;
    params.rotation = ROTATION_NONE;
    params.block_alignment = BLOCK_LINE;
    params.input_line_width = (s16) w;
    params.input_lines = (s16) lines;
    params.standard_coefficient = bt709 ? COEFFICIENT_ITU_R_BT_709_SCALING : COEFFICIENT_ITU_R_BT_601_SCALING;
    params.alpha = 0xFF;
    if (R_FAILED(Y2RU_SetConversionParams(&params))) return false;
    int32_t ys = p->strides[0], cs = p->strides[1], cw = w / 2;
    const uint8_t* y = p->planes[0] + x0;
    const uint8_t* u = p->planes[1] + x0 / 2;
    const uint8_t* v = p->planes[2] + x0 / 2;
    flushRange(y, (uint32_t) (ys * lines));
    flushRange(u, (uint32_t) (cs * lines / 2));
    flushRange(v, (uint32_t) (cs * lines / 2));
    uint16_t* out = dst + x0;
    flushRange(out, (uint32_t) (dstStride * lines * 2));
    Y2RU_SetSendingY(y, (u32) (w * lines), (s16) w, (s16) (ys - w));
    Y2RU_SetSendingU(u, (u32) (cw * lines / 2), (s16) cw, (s16) (cs - cw));
    Y2RU_SetSendingV(v, (u32) (cw * lines / 2), (s16) cw, (s16) (cs - cw));
    // Whole rows go 8 lines a transfer; a strip of wider rows a line at a time.
    if (dstStride == w) Y2RU_SetReceiving(out, (u32) (w * lines * 2), (s16) (w * 2 * 8), 0);
    else Y2RU_SetReceiving(out, (u32) (w * lines * 2), (s16) (w * 2), (s16) ((dstStride - w) * 2));
    svcClearEvent(y2rEvent);
    if (R_FAILED(Y2RU_StartConversion())) return false;
    converting = true;
    return true;
}

bool CtrSwConvert_finish(void) {
    if (!converting) return true;
    converting = false;
    // The end event is sometimes missed or early, so the receiving side is
    // checked too.
    svcWaitSynchronization(y2rEvent, 100 * 1000 * 1000LL);
    for (int32_t i = 0; 400 > i; i++) {
        bool done = false;
        if (R_SUCCEEDED(Y2RU_IsDoneReceiving(&done)) && done) return true;
        svcSleepThread(250 * 1000);
    }
    Y2RU_StopConversion();
    return false;
}

bool CtrSwConvert_start(const CtrSwPicture* p, int32_t width, int32_t height, uint16_t* dst, int32_t dstStride, bool bt709) {
    if (!CtrSwConvert_finish()) return false;
    int32_t w = (width + 7) & ~7, lines = (height + 7) & ~7;
    if (w > dstStride) w = dstStride & ~7;
    int32_t strips = (w + 1023) / 1024;
    int32_t stripW = ((w + strips - 1) / strips + 7) & ~7;
    for (int32_t x0 = 0; w > x0; x0 += stripW) {
        // Each strip but the last is waited for here.
        if (x0 > 0 && !CtrSwConvert_finish()) return false;
        if (!startStrip(p, x0, w - x0 < stripW ? w - x0 : stripW, lines, dst, dstStride, bt709)) return false;
    }
    return true;
}

bool CtrSwConvert_picture(const CtrSwPicture* p, int32_t width, int32_t height, uint16_t* dst, int32_t dstStride, bool bt709, double* ms) {
    u64 t0 = svcGetSystemTick();
    bool ok = CtrSwConvert_start(p, width, height, dst, dstStride, bt709) && CtrSwConvert_finish();
    *ms = (double) (svcGetSystemTick() - t0) * 1000.0 / (double) SYSCLOCK_ARM11;
    return ok;
}
#else
// ISO C forbids an empty translation unit.
typedef int ctrVideoSwUnused;
#endif
