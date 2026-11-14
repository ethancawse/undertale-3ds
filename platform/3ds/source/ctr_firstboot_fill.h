// firstboot (review decision 11-08, F2): the first time setup bar's fill between texture pages, for z_loadscreen.c's
// CtrFirstboot_packRun (UNDERTALE). The setup bar used to move only when a page finished; the biggest pages go first
// (ctr_texture_pack.c's foreground order), so on the Old 3DS it stood still for up to 5 s at a time.
//
// Work is each page's source bytes plus a fixed allowance per page (FB_PAGE_SLACK). The fill guesses the work
// done from the rate measured so far (the work done over the time it took; a fixed whole-setup time before the first
// page), never past a cap: the work done, the pages in hand (claimed by a thread now) and one average page more. Both
// come from the worker, under its lock (ctr_texture_pack.c: CtrPackWorker_firstbootWork). When the guess shows nothing
// new for FB_CRAWL_MS it creeps on by one pixel, still under the cap. It never goes back: when a page goes back to
// pending after a memory wait the cap falls by that page and the fill holds until the cap passes it again (each step
// grows the fill only up to the cap of that moment). It is full only when the last page is done. Pure integer math,
// inlined into its one caller; host-tested by tools/analysis/test_firstboot_fill.py. No state outside the caller's
// FbFill (on its stack).
#pragma once

#include <stdbool.h>
#include <stdint.h>

// A page's work: its source bytes and this. Each page also costs a fixed time (open, header, write, publish): on the
// Old 3DS about 0.7 s a page on top of about 1.9 s per MB (kit-1107g's 26 pages), so about 384 KB of bytes' time.
// (The old setup bar's 128 KB made the small last pages look cheap: its guess ran out of bar before them.)
#define FB_PAGE_SLACK (384u * 1024u)
#define FB_CRAWL_MS 450u     // at least a pixel this often while under the cap
#define FB_FIRST_MS 40000u   // the whole setup's time before the first page says otherwise (Old 3DS, UNDERTALE: 37 s)
// The first boot's files phase (the bytecode copy to the SD card): Old 3DS milliseconds per 10 MB of data.win, added
// to the loading bar's expectation when a first time setup ran. UNDERTALE on the Old 3DS: 6.9 s cold against 0.65 s
// warm for its 62.9 MB (kit-1107g, 1107e/f, 1108a/b/c).
#define FB_COLD_FILES_PER_MB10 1050u

typedef struct {
    uint64_t shown;    // the work the fill stands for
    uint64_t finished; // the finished work as last seen
    uint32_t doneMs;   // when it last changed (ms from the setup's start)
    uint32_t movedMs;  // when the fill last grew
    uint32_t px;       // the fill in pixels
} FbFill;

// The fill in pixels (0..px) at nowMs. finished (done), inFlight (in hand) and total are work; pages is the number of
// pages to pack.
__attribute__((always_inline)) static inline uint32_t FbFill_step(FbFill* s, uint64_t finished, uint64_t inFlight,
                                                                  uint64_t total, uint32_t pages, uint32_t px,
                                                                  uint32_t nowMs, bool last) {
    if (last || total == 0 || px == 0) {
        s->px = px;
        return px;
    }
    if (finished > total - 1) finished = total - 1;
    if (finished != s->finished) {
        s->finished = finished;
        s->doneMs = nowMs;
    }
    uint64_t cap = finished + inFlight + (pages > 0 ? total / pages : total);
    if (cap > total - 1) cap = total - 1; // full is for the last page
    uint64_t guess;
    if (finished == 0) guess = total * nowMs / FB_FIRST_MS;
    else if (s->doneMs == 0) guess = finished;
    else guess = finished + finished * (uint64_t) (nowMs - s->doneMs) / s->doneMs;
    if (guess > cap) guess = cap;
    if (guess > s->shown) s->shown = guess;
    if (finished > s->shown) s->shown = finished;
    uint32_t p = (uint32_t) ((uint64_t) px * s->shown / total);
    if (p <= s->px && nowMs - s->movedMs >= FB_CRAWL_MS) {
        uint64_t next = ((uint64_t) (s->px + 1) * total + px - 1) / px; // the least work that shows one pixel more
        if (next <= cap) {
            s->shown = next;
            p = s->px + 1;
        }
    }
    if (p > s->px) {
        s->px = p;
        s->movedMs = nowMs;
    }
    return s->px;
}
