// Streaming decoder for the QOI variant GameMaker 2022+ stores texture pages
// as: "fioq" pages, and "2zoq" pages, which are the same BZip2-compressed.
//
// The engine's decoder inflates a 2zoq page into one buffer (a few MB) and
// then decodes it into a whole RGBA8 page (16 MB for 2048x2048). This one
// inflates through a 64 KB buffer and hands the image over a strip of rows at
// a time, so it needs memory for one strip and the BZip2 state only. The
// output is the same, pixel for pixel, as ImageDecoder_decodeToRgba's.
#ifndef CTRPORT_CTR_QOI_H
#define CTRPORT_CTR_QOI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CTR_QOI_OK,
    CTR_QOI_BAD_DATA,
    CTR_QOI_NO_MEMORY,
    CTR_QOI_STOPPED, // the strip callback returned false
    CTR_QOI_YIELDED, // stopped for an urgent decode; try again later
} CtrQoiStatus;

// Receives `count` rows of RGBA8 pixels starting at row y, `width * 4` bytes
// each. Returns false to stop decoding.
typedef bool (*CtrQoiStripFn)(void* user, const uint8_t* rows, int32_t y, int32_t count);

// Reads the image size from the header, and the heap the decoder needs besides
// the strip itself. Returns false if this is not a QOI page.
bool CtrQoi_info(const uint8_t* blob, size_t size, int32_t* width, int32_t* height, size_t* workBytes);

// Call once before the first decode.
void CtrQoi_init(void);
// Frees the workspace decodes share (see ctr_qoi.c). Call while no decode runs.
void CtrQoi_releaseWorkspace(void);
// Frees the workspace if no decode is using it at the moment, without waiting.
// Returns the bytes freed (0 if there was none, or it was in use).
size_t CtrQoi_releaseWorkspaceIfIdle(void);
// Whether an urgent decode is waiting for the workspace. A non-urgent decode's
// strip callback that takes long can check this and stop early.
bool CtrQoi_yieldWanted(void);
// Time the calling thread has spent inflating BZip2 since the last call.
uint64_t CtrQoi_takeInflateNanos(void);

// Decodes the page, calling fn with strips of stripRows rows (fewer for the
// last strip), top to bottom. gm2022_5 says whether a 2zoq header stores the
// inflated length (GameMaker 2022.5 and later). An urgent decode (one the game
// is waiting for) makes a non-urgent one in progress stop with
// CTR_QOI_YIELDED and takes over its workspace.
CtrQoiStatus CtrQoi_decode(const uint8_t* blob, size_t size, bool gm2022_5, int32_t stripRows, bool urgent, CtrQoiStripFn fn, void* user);

#endif
