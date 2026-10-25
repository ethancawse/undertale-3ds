#ifndef CTRPORT_CTR_VIDEO_SW_H
#define CTRPORT_CTR_VIDEO_SW_H

#include <stdbool.h>
#include <stdint.h>

// Software H.264 decoding, for consoles without the hardware decoder, with
// openh264, when the build links it in (CTRPORT_HAVE_OPENH264).
// CTRPORT_SW_DECODE says whether it does.
#ifdef CTRPORT_HAVE_OPENH264
#define CTRPORT_SW_DECODE 1
#endif

typedef struct CtrSwDecoder CtrSwDecoder;

// A decoded picture: YUV 4:2:0 planes, owned by the decoder until the next call.
typedef struct {
    const uint8_t* planes[3];
    int32_t strides[2]; // luma, chroma
    int32_t width, height;
} CtrSwPicture;

CtrSwDecoder* CtrSwDecoder_open(void);
// Annex B data (start codes): one or more NAL units. 1 if a picture came out
// (in *out), 0 if none did, -1 if the decoder reported an error.
int32_t CtrSwDecoder_decode(CtrSwDecoder* d, const uint8_t* data, uint32_t size, CtrSwPicture* out);
// After a run of decode calls: the pictures still held inside the decoder
// (for reordering), one per call, 1 while one comes out (in *out), then 0,
// after which the decoder is reset and must be given a keyframe next.
int32_t CtrSwDecoder_drain(CtrSwDecoder* d, CtrSwPicture* out);
// The decoder's own code for its last error (openh264's DECODING_STATE bits).
int32_t CtrSwDecoder_lastError(const CtrSwDecoder* d);
void CtrSwDecoder_close(CtrSwDecoder* d);

// The picture's top-left width x height at half size, each output sample the
// rounded mean of a 2x2 block (as the GPU's halving transfer averages), into
// buffer: luma rows of stride bytes, then the two chroma planes with rows of
// stride / 2. buffer holds stride * rows * 3 / 2 bytes, rows at least
// height / 2 and stride at least width / 2, both even. *out describes it.
void CtrSwPicture_halve(const CtrSwPicture* p, int32_t width, int32_t height, uint8_t* buffer, int32_t stride, int32_t rows,
                        CtrSwPicture* out);
// The same at full size: the picture's top-left width x height (rounded up
// to 8 as Y2R reads it, within stride and rows) copied into buffer, so the
// decoder can go on while Y2R converts the copy. stride at least width and
// rows at least height, both even.
void CtrSwPicture_copy(const CtrSwPicture* p, int32_t width, int32_t height, uint8_t* buffer, int32_t stride, int32_t rows,
                       CtrSwPicture* out);

// ===[ Y2R ]===
// Colour conversion of a decoded picture to RGB565 by the Y2R hardware (on
// every 3DS): YUV 4:2:0 planes in, linear RGB565 rows out.
bool CtrSwConvert_open(void);
void CtrSwConvert_close(void);
// Converts the picture's top-left width x height (each rounded up to 8) into
// dst, whose rows are dstStride pixels apart. bt709: the HD matrix rather
// than the SD one (both for TV-range input). *ms: how long it took.
bool CtrSwConvert_picture(const CtrSwPicture* p, int32_t width, int32_t height, uint16_t* dst, int32_t dstStride, bool bt709, double* ms);
// The same without waiting for the end: the conversion (its last strip, for
// a picture wider than 1024) runs on while the CPU does something else, and
// CtrSwConvert_finish waits for it. The picture's planes and dst must stay
// as they are until then. A conversion still running is finished first.
bool CtrSwConvert_start(const CtrSwPicture* p, int32_t width, int32_t height, uint16_t* dst, int32_t dstStride, bool bt709);
// Waits for the conversion CtrSwConvert_start left running, if any. False if it failed.
bool CtrSwConvert_finish(void);

#endif
