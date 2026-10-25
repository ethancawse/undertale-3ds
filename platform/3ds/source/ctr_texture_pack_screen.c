// The first-run screen that packs every texture page before the game starts.
//
// A page the game draws before the background worker has packed it is decoded
// from its image on the spot, which stalls the game for a second or more, and
// entering a room that uses several such pages stalls it for many. That only
// happens until every page has a cache file, so a run that finds pages not
// packed yet packs them all first, while nothing else needs the CPU. Each
// finished page is kept as it is done, so a run cut short (HOME, the power
// button, B) carries on from there next time.
//
// The screen is drawn straight into the top framebuffer with the console
// font: the renderer doesn't exist yet.
#include "ctr_texture_pack_screen.h"

#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "ctr_log.h"
#include "ctr_qoi.h"
#include "ctr_texture_pack.h"
#include "ctr_watchdog.h"
#include "gettime.h"
#include "log.h"

#define SCREEN_W 400
#define SCREEN_H 240

#define COLOR_BACKGROUND 0x10101Cu
#define COLOR_TEXT 0xFFFFFFu
#define COLOR_DIM 0x9090A0u
#define COLOR_BAR 0x40C0FFu

#define BAR_X 50
#define BAR_Y 124
#define BAR_W 300
#define BAR_H 16

typedef struct {
    u8* fb;
    u16 rows;      // framebuffer width: the screen's height
    int32_t scale; // framebuffer columns per screen column: 2 in 800-pixel mode
    uint32_t bpp;
} Canvas;

static void fillRect(const Canvas* c, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb) {
    u8 r = (u8) (rgb >> 16);
    u8 g = (u8) (rgb >> 8);
    u8 b = (u8) rgb;
    for (int32_t sx = x; x + w > sx; sx++) {
        if (0 > sx || sx >= SCREEN_W) continue;
        for (int32_t i = 0; c->scale > i; i++) {
            // The framebuffer is the screen turned a quarter: a column of the
            // screen, bottom to top, is a row of the framebuffer.
            u8* column = c->fb + (size_t) (sx * c->scale + i) * c->rows * c->bpp;
            for (int32_t sy = y; y + h > sy; sy++) {
                if (0 > sy || sy >= SCREEN_H) continue;
                u8* p = column + (size_t) (c->rows - 1 - sy) * c->bpp;
                if (c->bpp == 4) {
                    p[0] = 0xFF;
                    p[1] = b;
                    p[2] = g;
                    p[3] = r;
                } else {
                    p[0] = b;
                    p[1] = g;
                    p[2] = r;
                }
            }
        }
    }
}

static void drawText(const Canvas* c, int32_t x, int32_t y, int32_t scale, const char* text, uint32_t rgb) {
    const ConsoleFont* font = &consoleGetDefault()->font;
    for (const char* ch = text; *ch != '\0'; ch++, x += 8 * scale) {
        int32_t glyph = (int32_t) (unsigned char) *ch - (int32_t) font->asciiOffset;
        if (0 > glyph || glyph >= (int32_t) font->numChars) continue;
        const u8* rows = font->gfx + glyph * 8;
        for (int32_t row = 0; 8 > row; row++) {
            for (int32_t col = 0; 8 > col; col++) {
                if (rows[row] & (0x80 >> col)) fillRect(c, x + col * scale, y + row * scale, scale, scale, rgb);
            }
        }
    }
}

static void drawCentered(const Canvas* c, int32_t y, int32_t scale, const char* text, uint32_t rgb) {
    drawText(c, (SCREEN_W - (int32_t) strlen(text) * 8 * scale) / 2, y, scale, text, rgb);
}

static void drawEye(gfx3dSide_t side, uint32_t done, uint32_t total, bool blank, const char* estimate, unsigned activity) {
    GSPGPU_FramebufferFormat format = gfxGetScreenFormat(GFX_TOP);
    if (format != GSP_BGR8_OES && format != GSP_RGBA8_OES) return;
    u16 width = 0;
    u16 height = 0;
    Canvas c = { gfxGetFramebuffer(GFX_TOP, side, &width, &height), width, height >= 2 * SCREEN_W ? 2 : 1,
                 format == GSP_RGBA8_OES ? 4u : 3u };
    if (c.fb == nullptr) return;
    fillRect(&c, 0, 0, SCREEN_W, SCREEN_H, blank ? 0 : COLOR_BACKGROUND);
    if (blank) return;

    char line[64];
    drawCentered(&c, 56, 2, "Preparing textures", COLOR_TEXT);
    drawCentered(&c, 92, 1, "Converting the game's art for the 3DS.", COLOR_DIM);
    fillRect(&c, BAR_X - 2, BAR_Y - 2, BAR_W + 4, BAR_H + 4, COLOR_TEXT);
    fillRect(&c, BAR_X, BAR_Y, BAR_W, BAR_H, COLOR_BACKGROUND);
    int32_t filled = total > 0 ? (int32_t) ((uint64_t) BAR_W * done / total) : 0;
    fillRect(&c, BAR_X, BAR_Y, filled, BAR_H, COLOR_BAR);
    snprintf(line, sizeof(line), "%u / %u pages", (unsigned) done, (unsigned) total);
    drawCentered(&c, 152, 1, line, COLOR_TEXT);
    drawCentered(&c, 168, 1, estimate, COLOR_DIM);
    // Activity keeps moving while a large page is being decoded or written.
    fillRect(&c, BAR_X - 14, BAR_Y + 4, 6, 8, activity % 2 ? COLOR_BAR : COLOR_DIM);
    drawCentered(&c, 188, 1, "Only needed once; quitting keeps what's done.", COLOR_DIM);
    drawCentered(&c, 204, 1, "B: play now, finish in the background", COLOR_DIM);
}

static void present(uint32_t done, uint32_t total, bool blank, const char* estimate, unsigned activity) {
    drawEye(GFX_LEFT, done, total, blank, estimate, activity);
    if (gfxIs3D()) drawEye(GFX_RIGHT, done, total, blank, estimate, activity);
    gfxFlushBuffers();
    gfxSwapBuffers();
}

void CtrPackScreen_run(const DataWin* dataWin) {
    CtrPack_init();
    CtrPackKey key = CtrPack_keyFor(dataWin);
    if (CtrPack_isComplete(&key, dataWin->txtr.count)) return;
    CtrPackWorker* worker = CtrPackWorker_start(dataWin, &key, true);
    if (worker == nullptr) return;

    uint64_t start = nowNanos();
    // The game's virtual state-hash clock must not freeze the setup display.
    u64 displayStart = svcGetSystemTick();
    u64 lastDisplay = 0;
    uint64_t totalWork = 0;
    uint64_t finishedWork = 0;
    double estimatedSeconds = 0;
    for (uint32_t i = 0; i < dataWin->txtr.count; i++) {
        const Texture* texture = &dataWin->txtr.textures[i];
        if (texture->externalPath == nullptr && texture->blobOffset > 0 && texture->blobSize > 0)
            totalWork += (uint64_t) texture->blobSize + 128u * 1024u;
    }
    uint32_t done = 0;
    uint32_t total = 0;
    uint32_t built = 0;
    bool skipped = false;
    double bytesWritten = 0;
    double writingMs = 0; // on the SD card, not waiting for another thread
    double waitingMs = 0;
    logInfo("Textures: preparing texture pages on %u threads\n", (unsigned) CtrPackWorker_threadCount(worker));
    CtrLog_flush();
    while (aptMainLoop()) {
        CtrWatchdog_mark("preparing textures");
        hidScanInput();
        if (hidKeysDown() & KEY_B) {
            skipped = true;
            break;
        }
        CtrPackResult result;
        while (CtrPackWorker_poll(worker, &result)) {
            // Source bytes approximate decode work; a fixed allowance per page
            // includes file publication. Counting pages alone badly overstates
            // the time left after the largest pages finish first. This is an
            // estimate learned from this run, not a hardware speed assumption.
            finishedWork += (uint64_t) dataWin->txtr.textures[result.pageId].blobSize + 128u * 1024u;
            double seconds = (double) (svcGetSystemTick() - displayStart) / SYSCLOCK_ARM11;
            if (finishedWork > 0) estimatedSeconds = seconds * (double) totalWork / (double) finishedWork;
            if (result.built) {
                built++;
                logInfo("Prepared page %u: %u KB, %.0f+%.0f ms (read %.0f, inflate %.0f, convert %.0f, compress %.0f, write %.0f) thread %d "
                        "core %d at %.1f s\n",
                        (unsigned) result.pageId, (unsigned) (result.size / 1024), (double) result.decodeMs, (double) result.packMs,
                        (double) result.readMs, (double) result.inflateMs, (double) result.convertMs, (double) result.compressMs,
                        (double) result.writeMs, (int) result.thread, (int) result.core, (double) (nowNanos() - start) / 1e9);
                const CtrPackWriteStats* io = &result.io;
                double ms = (double) (io->openMs + io->sizeMs + io->dataMs + io->flushMs + io->closeMs + io->renameMs);
                logInfo("  wrote %u KB %s in %u calls (%.0f-%.0f ms each), %.2f MB/s: open %.0f, size %.0f, data %.0f, flush %.0f, close %.0f, "
                        "rename %.0f ms; waited %.0f ms\n",
                        (unsigned) (result.size / 1024), io->streamed ? "streamed" : io->staged ? "whole, aligned" : "whole", (unsigned) io->calls,
                        (double) io->callMinMs, (double) io->callMaxMs, ms > 0 ? (double) result.size / (1024.0 * 1024.0) / (ms / 1000.0) : 0.0,
                        (double) io->openMs, (double) io->sizeMs, (double) io->dataMs, (double) io->flushMs, (double) io->closeMs,
                        (double) io->renameMs, (double) io->waitMs);
                bytesWritten += (double) result.size;
                logInfo("  publication %s: delete %.2f ms, rename %.2f ms\n", io->publication.direct ? "SD archive" : "stdio",
                        (double) io->publication.deleteMs, (double) io->publication.renameMs);
                writingMs += ms;
                waitingMs += (double) io->waitMs;
            }
            CtrPacked_free(result.packed);
        }
        CtrPackWorker_progress(worker, &done, &total);
        if (done >= total) break;
        u64 tick = svcGetSystemTick();
        // Four refreshes per second give activity and a live estimate without
        // spending every vblank redrawing the framebuffer on the CPU.
        if (lastDisplay == 0 || tick - lastDisplay >= SYSCLOCK_ARM11 / 4) {
            double seconds = (double) (tick - displayStart) / SYSCLOCK_ARM11;
            char estimate[64];
            double remaining = estimatedSeconds - seconds;
            if (finishedWork == 0 || seconds < 2.0) {
                snprintf(estimate, sizeof(estimate), "Estimating time...  %.0f s elapsed", seconds);
            } else if (remaining > 0.0) {
                unsigned rounded = remaining > 86400.0 ? 86400u : ((unsigned) remaining / 5u + 1u) * 5u;
                snprintf(estimate, sizeof(estimate), "About %u min %02u s left", rounded / 60u, rounded % 60u);
            } else {
                snprintf(estimate, sizeof(estimate), "Finishing a page...  %.0f s elapsed", seconds);
            }
            present(done, total, false, estimate, (unsigned) (seconds * 4.0));
            lastDisplay = tick;
        }
        gspWaitForVBlank();
    }
    uint32_t memoryWaits = 0;
    uint32_t deferred = 0;
    CtrPackWorker_memoryStats(worker, &memoryWaits, &deferred);
    CtrPackWorker_stop(worker);
    CtrQoi_releaseWorkspace();
    // Neither framebuffer keeps the progress screen while the game loads.
    present(0, 0, true, "", 0);
    present(0, 0, true, "", 0);
    // The heap is never trimmed (ctr_memory.c), so its arena is the peak so far.
    struct mallinfo mi = mallinfo();
    logInfo("Textures: %u of %u pages ready (%u packed now) in %.1f s%s; heap peak %.1f MB, %u waits for memory, %u pages left to the "
            "background\n",
            (unsigned) (done - deferred), (unsigned) total, (unsigned) built, (double) (nowNanos() - start) / 1e9,
            skipped ? ", the rest in the background" : "", (double) mi.arena / (1024.0 * 1024.0), (unsigned) memoryWaits, (unsigned) deferred);
    if (built > 0) {
        logInfo("Textures: wrote %.1f MB of cache files in %.1f s on the SD card, %.2f MB/s; threads waited %.1f s to write\n",
                bytesWritten / (1024.0 * 1024.0), writingMs / 1000.0, writingMs > 0 ? bytesWritten / (1024.0 * 1024.0) / (writingMs / 1000.0) : 0.0,
                waitingMs / 1000.0);
    }
}
