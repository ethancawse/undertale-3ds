"""Host tests of the first time setup bar (change firstboot 11-08; c26rev6 FIRSTBOOT-1).

1. ctr_firstboot_fill.h's FbFill_step, compiled with clang into a small driver that reads one step per line (work done,
   work in hand, total work, pages, pixels, ms, last) and prints the fill. Python makes the timelines: an invented
   two-thread setup that packs the biggest pages first, one page alone, very slow pages, and edge cases. Checks: the
   fill never goes back; it never shows more work than the cap (done, in hand and one average page); it is full only
   at the last page and then exactly full; it never stands still for more than 2 s while under the cap in the invented
   setup (FB_CRAWL_MS is 450 ms).
2. The production worker's own claimPage, finishPage and CtrPackWorker_firstbootWork, cut from ctr_texture_pack.c
   with its page states, statuses and retry constants, compiled at -O0 and -O2 into a driver that runs workerMain's
   loop (each thread's search position, kept across a memory retry and reset only at the end of its pass) with
   memory retries, hard failures and pages deferred after two solo failures, and FbFill_step on top every 33 ms.
   The driver checks the reader against its own record of what each thread holds and what finishPage settled, and
   that the fill grows only up to the cap of that moment (a page put back holds it). The c26rev6 schedule (both big
   pages put back once while the threads move on) is run as given. Page sizes are invented; FIRSTBOOT_SIZES_FILE
   (lines "name size size ...") adds schedules read at test time, which stay out of the repository.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / 'platform' / '3ds' / 'source' / 'ctr_firstboot_fill.h'
CLANG = shutil.which('clang') or 'C:/msys64/ucrt64/bin/clang.exe'
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
SLACK = 384 * 1024  # FB_PAGE_SLACK

DRIVER = r'''
#include <inttypes.h>
#include <stdio.h>
#include "ctr_firstboot_fill.h"
int main(void) {
    FbFill fill = {0};
    unsigned long long finished, inFlight, total;
    unsigned pages, px, ms, last;
    while (scanf("%llu %llu %llu %u %u %u %u", &finished, &inFlight, &total, &pages, &px, &ms, &last) == 7)
        printf("%u %llu\n", FbFill_step(&fill, finished, inFlight, total, pages, px, ms, last != 0),
               (unsigned long long) fill.shown);
    return 0;
}
'''


def build(tmp, driver=DRIVER, name='driver', opt='-O2'):
    source, exe = Path(tmp) / (name + '.c'), Path(tmp) / (name + opt + '.exe')
    source.write_text(driver, encoding='utf-8')
    subprocess.run([CLANG, '-std=c2x', '-Wall', '-Wextra', '-Werror', opt, '-I', str(HEADER.parent), str(source),
                    '-o', str(exe)], check=True, stdin=subprocess.DEVNULL, timeout=120, creationflags=NO_WINDOW)
    return exe


def setup_timeline(sizes, ms_per_byte, page_ms, threads=2, step=33):
    """An invented setup: pages biggest first, `threads` at a time; page i takes page_ms + size * ms_per_byte[i]."""
    work = [s + SLACK for s in sizes]
    order = sorted(range(len(sizes)), key=lambda i: (-sizes[i], i))
    running, nxt, now, ends = {}, 0, 0.0, {}
    for _ in range(threads):
        if nxt < len(order):
            page = order[nxt]; nxt += 1
            running[page] = now + page_ms + sizes[page] * ms_per_byte[page]
    while running:
        page = min(running, key=running.get)
        now = running.pop(page)
        ends[page] = now
        if nxt < len(order):
            following = order[nxt]; nxt += 1
            running[following] = now + page_ms + sizes[following] * ms_per_byte[following]
    end = max(ends.values())
    claims, started = {}, 0.0
    # Claim times: a thread claims its next page when it finishes the last one.
    queue = list(order)
    free_at = [0.0] * threads
    for page in queue:
        thread = min(range(threads), key=lambda t: free_at[t])
        claims[page] = free_at[thread]
        free_at[thread] = ends[page]
    rows, t = [], 0
    total = sum(work)
    while True:
        finished = sum(work[p] for p in ends if ends[p] <= t)
        flight = sum(work[p] for p in ends if claims[p] <= t < ends[p])
        last = all(ends[p] <= t for p in ends)
        rows.append((finished, flight, total, len(sizes), t, last))
        if last:
            return rows, end
        t += step


class FirstbootFillTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = build(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_rows(self, rows, px):
        text = ''.join('%d %d %d %d %d %d %d\n' % (f, i, total, pages, px, ms, int(last))
                       for f, i, total, pages, ms, last in rows)
        out = subprocess.run([str(self.exe)], input=text, capture_output=True, text=True, check=True, timeout=60,
                             creationflags=NO_WINDOW).stdout.split('\n')
        return [tuple(int(x) for x in line.split()) for line in out if line]

    def check(self, rows, px, max_still_ms=None):
        out = self.run_rows(rows, px)
        self.assertEqual(len(out), len(rows))
        previous, moved, longest = 0, rows[0][4], 0
        for (finished, flight, total, pages, ms, last), (fill, shown) in zip(rows, out):
            self.assertGreaterEqual(fill, previous, 'went back at %d ms' % ms)
            if last:
                self.assertEqual(fill, px, 'not full at the last page')
            else:
                cap = min(finished + flight + (total // pages if pages else total), total - 1)
                self.assertLessEqual(shown, max(cap, min(finished, total - 1)), 'past the cap at %d ms' % ms)
                self.assertLessEqual(fill, px * max(cap, finished) // total, 'drawn past the cap at %d ms' % ms)
                self.assertLess(fill, px, 'full before the last page at %d ms' % ms)
            if fill != previous:
                longest = max(longest, ms - moved)
                moved = ms
            previous = fill
        if max_still_ms is not None:
            self.assertLessEqual(longest, max_still_ms)
        return out, longest

    def test_two_threads_biggest_first(self):
        # Invented: three big pages (40% of the work) and 23 small ones; per-byte cost varies 2.5x between pages.
        sizes = [3_300_000, 1_700_000, 1_650_000, 1_150_000, 700_000] + [500_000 - 15_000 * i for i in range(21)]
        cost = [0.0018 if i % 3 else 0.0045 for i in range(len(sizes))]
        rows, end = setup_timeline(sizes, cost, page_ms=250)
        self.assertGreater(end, 20_000)
        for px in (110, 145):
            with self.subTest(px=px):
                self.check(rows, px, max_still_ms=2000)

    def test_one_page(self):
        rows, _ = setup_timeline([2_000_000], [0.01], page_ms=500, threads=2)
        out, _ = self.check(rows, 120)
        self.assertGreater(out[len(out) // 2][0], 0, 'one page: the fill should creep while it packs')

    def test_very_slow_pages_stop_at_the_cap(self):
        sizes = [1_000_000] * 4
        rows, _ = setup_timeline(sizes, [0.06] * 4, page_ms=1000)  # each page about a minute
        out, _ = self.check(rows, 145)
        total = sum(s + SLACK for s in sizes)
        # While the first two pages pack, nothing is finished: the fill stops at two pages in flight plus one average.
        first_done = next(i for i, row in enumerate(rows) if row[0] > 0)
        self.assertLessEqual(out[first_done - 1][1], 3 * (1_000_000 + SLACK))
        self.assertLessEqual(out[first_done - 1][0], 145 * 3 * (1_000_000 + SLACK) // total)

    def test_edges(self):
        # Nothing to pack, no pixels, and a last step before any progress.
        self.assertEqual(self.run_rows([(0, 0, 0, 0, 0, False)], 100), [(100, 0)])
        self.assertEqual(self.run_rows([(0, 0, 1000, 1, 0, False)], 0), [(0, 0)])
        self.assertEqual(self.run_rows([(0, 0, 1000, 1, 0, False), (0, 0, 1000, 1, 33, True)], 100)[-1][0], 100)
        # Finished work counted past the total (a page outside it) stays short of full until the last page.
        out = self.run_rows([(2000, 0, 1000, 2, 33, False), (2000, 0, 1000, 2, 66, True)], 100)
        self.assertEqual([fill for fill, _ in out], [99, 100])


PACK_SOURCE = ROOT / 'platform' / '3ds' / 'source' / 'ctr_texture_pack.c'


def cut(text, pattern):
    """The text from the line matching pattern (a regex, multiline) through its brace-matched end."""
    import re
    match = re.search(pattern, text, re.M)
    if match is None:
        raise AssertionError('not in ctr_texture_pack.c: ' + pattern)
    start = text.index('{', match.start())
    depth, i = 0, start
    while True:
        depth += {'{': 1, '}': -1}.get(text[i], 0)
        i += 1
        if depth == 0:
            break
    end = text.index(';', i) + 1 if text[i:].lstrip().startswith(('PageState', 'PackStatus')) else i
    return text[match.start():end]


def production_parts():
    """claimPage, finishPage and CtrPackWorker_firstbootWork as ctr_texture_pack.c has them, with its page states,
    statuses and the foreground retry constants."""
    import re
    text = PACK_SOURCE.read_text(encoding='utf-8')
    parts = [cut(text, r'^typedef enum \{\n    PAGE_NONE'), cut(text, r'^typedef enum \{\n    PACK_DONE')]
    for name in ('FOREGROUND_MAX_SOLO_FAILURES', 'FOREGROUND_RETRY_MS'):
        parts.append(re.search(r'^#define %s .*$' % name, text, re.M).group(0))
    functions = [cut(text, r'^static int32_t claimPage\(CtrPackWorker\* worker, uint32_t\* next\) \{'),
                 cut(text, r'^static PackStatus finishPage\(CtrPackWorker\* worker, uint32_t page, PackStatus status\) \{'),
                 cut(text, r'^void CtrPackWorker_firstbootWork\([^)]*\) \{')]
    return '\n'.join(parts), '\n\n'.join(functions)


WORKER_PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "ctr_firstboot_fill.h"
#define MAXP 512
%(types)s
// The worker's fields these functions use, as ctr_texture_pack.c names them; the lock checks that each lock is
// released before it is taken again.
typedef struct CtrPackWorker {
    uint32_t pageCount, pagesLeft, deferred, memoryWaits;
    uint32_t order[MAXP], blobSizes[MAXP];
    uint8_t pageStates[MAXP], soloFailures[MAXP];
    bool foreground, quit, anyFailed;
    int lock;
} CtrPackWorker;
static void LightLock_Lock(int* lock) { assert(*lock == 0); *lock = 1; }
static void LightLock_Unlock(int* lock) { assert(*lock == 1); *lock = 0; }
static void lockWorker(CtrPackWorker* worker) { LightLock_Lock(&worker->lock); }
static int32_t racefixTakeHurry(CtrPackWorker* worker) { (void) worker; return -1; } // foreground: no hurry
static void racefixPublish(CtrPackWorker* worker) { (void) worker; }
%(functions)s
'''

WORKER_DRIVER = r'''
static uint32_t rng;
static uint32_t rnd(uint32_t n) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) % n; }

// CtrPackWorker_start's foreground setup: packable pages pending, the order biggest first (a stable insertion sort).
static uint64_t start(CtrPackWorker* w, const uint32_t* sizes, uint32_t count) {
    *w = (CtrPackWorker) {.pageCount = count, .foreground = true};
    uint64_t total = 0;
    for (uint32_t i = 0; count > i; i++) {
        w->order[i] = i;
        if (sizes[i] == 0) continue;
        w->blobSizes[i] = sizes[i];
        w->pageStates[i] = PAGE_PENDING;
        w->pagesLeft++;
        total += (uint64_t) sizes[i] + FB_PAGE_SLACK;
    }
    for (uint32_t i = 1; count > i; i++) {
        uint32_t page = w->order[i], j = i;
        for (; j > 0 && w->blobSizes[page] > w->blobSizes[w->order[j - 1]]; j--) w->order[j] = w->order[j - 1];
        w->order[j] = page;
    }
    return total;
}

// One display step against the driver's own record: the reader must agree with it, and the fill may grow only up
// to the cap of this moment. Returns the fill.
static uint32_t display(CtrPackWorker* w, FbFill* f, uint64_t truthDone, uint64_t truthHand, uint64_t total,
                        uint32_t pages, uint32_t px, uint32_t ms, bool last, unsigned* held) {
    uint64_t done = 1, hand = 1;
    CtrPackWorker_firstbootWork(w, FB_PAGE_SLACK, &done, &hand);
    assert(w->lock == 0);
    assert(done == truthDone && hand == truthHand);
    uint64_t before = f->shown;
    uint32_t beforePx = f->px;
    uint32_t p = FbFill_step(f, done, hand, total, pages, px, ms, last);
    assert(p >= beforePx && f->shown >= before);
    if (last) {
        assert(p == px);
        return p;
    }
    uint64_t cap = done + hand + total / pages;
    if (cap > total - 1) cap = total - 1;
    if (f->shown > before) assert(f->shown <= cap);
    if (f->shown > cap) {
        assert(p == beforePx); // held: shown before a page went back
        (*held)++;
    }
    assert(p < px && (uint64_t) p * total <= (uint64_t) px * (f->shown > cap ? f->shown : cap));
    return p;
}

// c26rev6 FIRSTBOOT-1: two threads claim the two biggest pages; each gets one solo memory failure (not yet deferred)
// and moves on to the next page in the order. The reader then counts the pages actually held; 40 s of display with
// nothing finished stay under done + held + one average page. Then the first big page fails hard (no result) and
// counts as done.
static void retrySchedule(const char* name, const uint32_t* sizes, uint32_t count, uint32_t px) {
    CtrPackWorker w;
    uint64_t total = start(&w, sizes, count);
    uint32_t next[2] = {0, 0};
    int32_t a = claimPage(&w, &next[0]), b = claimPage(&w, &next[1]);
    assert(a == (int32_t) w.order[0] && b == (int32_t) w.order[1]);
    assert(finishPage(&w, (uint32_t) a, PACK_NO_MEMORY_ALONE) == PACK_NO_MEMORY_ALONE);
    int32_t c = claimPage(&w, &next[0]);
    assert(finishPage(&w, (uint32_t) b, PACK_NO_MEMORY_ALONE) == PACK_NO_MEMORY_ALONE);
    int32_t d = claimPage(&w, &next[1]);
    assert(c == (int32_t) w.order[2] && d == (int32_t) w.order[3]);
    assert(w.pageStates[a] == PAGE_PENDING && w.pageStates[b] == PAGE_PENDING);
    uint64_t hand = (uint64_t) sizes[c] + sizes[d] + 2ull * FB_PAGE_SLACK;
    FbFill f = {0};
    unsigned held = 0;
    uint32_t p = 0;
    for (uint32_t ms = 0; 40000 >= ms; ms += 33) p = display(&w, &f, 0, hand, total, w.pagesLeft, px, ms, false, &held);
    uint64_t cap = hand + total / w.pagesLeft;
    assert(f.shown <= cap && (uint64_t) p * total <= (uint64_t) px * cap);
    printf("retry %s: put back %d %d, held %d %d, fill %u/%u px, cap %llu px\n", name, (int) a, (int) b, (int) c,
           (int) d, (unsigned) p, (unsigned) px, (unsigned long long) ((uint64_t) px * cap / total));
    // A hard failure publishes no result; the reader counts it done all the same.
    uint32_t pages = w.pagesLeft;
    assert(finishPage(&w, (uint32_t) a, PACK_FAILED) == PACK_FAILED && w.pageStates[a] == PAGE_DONE);
    uint64_t done = (uint64_t) sizes[a] + FB_PAGE_SLACK;
    display(&w, &f, done, hand, total, pages, px, 40033, false, &held);
    // Thread 0 finishes its page; its next pass (from the order's head) takes the other big page again, and a second
    // solo failure defers it to the game's background packing: done too.
    assert(finishPage(&w, (uint32_t) c, PACK_DONE) == PACK_DONE);
    done += (uint64_t) sizes[c] + FB_PAGE_SLACK;
    hand -= (uint64_t) sizes[c] + FB_PAGE_SLACK;
    display(&w, &f, done, hand, total, pages, px, 40050, false, &held);
    next[0] = 0;
    int32_t again = claimPage(&w, &next[0]);
    assert(again == b);
    hand += (uint64_t) sizes[b] + FB_PAGE_SLACK;
    display(&w, &f, done, hand, total, pages, px, 40066, false, &held);
    assert(finishPage(&w, (uint32_t) b, PACK_NO_MEMORY_ALONE) == PACK_FAILED && w.deferred == 1);
    hand -= (uint64_t) sizes[b] + FB_PAGE_SLACK;
    done += (uint64_t) sizes[b] + FB_PAGE_SLACK;
    display(&w, &f, done, hand, total, pages, px, 40099, false, &held);
    printf("failed and deferred %s: done %llu, fill %u/%u px\n", name, (unsigned long long) done, (unsigned) f.px,
           (unsigned) px);
}

// workerMain's loop for `threads` threads, event by event: each pass searches from the order's head (next = 0) and
// keeps its position across a memory retry; a pass that met a memory wait naps FOREGROUND_RETRY_MS and starts again,
// one that did not ends the thread. Page outcomes are random: done, a memory wait (shared or alone), or a hard
// failure; a page's fourth attempt is done, so every schedule ends.
typedef struct { int state; uint32_t next, until; int32_t page; bool waiting; } Thread; // 0 claim, 1 pack, 2 nap, 3 out

static void randomSchedule(const uint32_t* sizes, uint32_t count, uint32_t threads, uint32_t px, unsigned* held,
                           unsigned* retries, unsigned* failures, unsigned* deferrals) {
    CtrPackWorker w;
    uint64_t total = start(&w, sizes, count);
    uint32_t pages = w.pagesLeft;
    if (pages == 0) return;
    uint8_t tries[MAXP] = {0};
    Thread t[3] = {0};
    uint64_t truthDone = 0;
    FbFill f = {0};
    uint32_t now = 0, nextDisplay = 0;
    for (;;) {
        // Every thread acts at `now` until all wait for a later time.
        bool acted = true;
        while (acted) {
            acted = false;
            for (uint32_t k = 0; threads > k; k++) {
                Thread* th = &t[k];
                if (th->state == 0) {
                    int32_t page = claimPage(&w, &th->next);
                    if (0 <= page) {
                        th->state = 1;
                        th->page = page;
                        th->until = now + 20 + (uint32_t) ((uint64_t) w.blobSizes[page] * (1 + rnd(4)) / 1000);
                    } else if (th->waiting) {
                        th->state = 2;
                        th->until = now + FOREGROUND_RETRY_MS;
                    } else {
                        th->state = 3;
                    }
                    acted = true;
                } else if ((th->state == 1 || th->state == 2) && th->until <= now) {
                    if (th->state == 1) {
                        uint32_t page = (uint32_t) th->page;
                        uint32_t roll = rnd(100);
                        PackStatus status = ++tries[page] > 3 ? PACK_DONE
                                            : roll < 25 ? PACK_NO_MEMORY_ALONE : roll < 45 ? PACK_NO_MEMORY
                                            : roll < 50 ? PACK_FAILED : PACK_DONE;
                        PackStatus got = finishPage(&w, page, status);
                        if (got == PACK_NO_MEMORY || got == PACK_NO_MEMORY_ALONE) {
                            th->waiting = true;
                            (*retries)++;
                        } else {
                            truthDone += (uint64_t) w.blobSizes[page] + FB_PAGE_SLACK;
                            if (got == PACK_FAILED) (*failures)++;
                        }
                        th->state = 0; // the same pass goes on from th->next
                    } else {
                        th->state = 0;
                        th->next = 0; // a new pass
                        th->waiting = false;
                    }
                    acted = true;
                }
            }
        }
        bool out = true;
        uint32_t wake = UINT32_MAX;
        uint64_t truthHand = 0;
        for (uint32_t k = 0; threads > k; k++) {
            if (t[k].state != 3) out = false;
            if (t[k].state == 1) truthHand += (uint64_t) w.blobSizes[t[k].page] + FB_PAGE_SLACK;
            if ((t[k].state == 1 || t[k].state == 2) && t[k].until < wake) wake = t[k].until;
        }
        // The setup's loop: it stops at the last page (CtrPackWorker_progress), threads still napping or not.
        assert(!out || w.pagesLeft == 0);
        if (w.pagesLeft == 0) {
            display(&w, &f, truthDone, truthHand, total, pages, px, nextDisplay, true, held);
            break;
        }
        for (; nextDisplay < wake; nextDisplay += 33)
            display(&w, &f, truthDone, truthHand, total, pages, px, nextDisplay, false, held);
        now = wake;
    }
    assert(f.px == px);
    *deferrals += w.deferred;
}

int main(int argc, char** argv) {
    (void) argc;
    rng = (uint32_t) strtoul(argv[1], 0, 10);
    char name[64];
    uint32_t count, sizes[MAXP];
    unsigned schedules = 0, held = 0, retries = 0, failures = 0, deferrals = 0;
    while (scanf("%63s %u", name, &count) == 2) {
        assert(count <= MAXP);
        for (uint32_t i = 0; count > i; i++) assert(scanf("%u", &sizes[i]) == 1);
        if (count >= 4) retrySchedule(name, sizes, count, 110);
        for (uint32_t run = 0; 40 > run; run++) {
            randomSchedule(sizes, count, 1 + run % 3, run % 2 ? 110 : 145, &held, &retries, &failures, &deferrals);
            schedules++;
        }
    }
    printf("random: %u schedules, %u memory retries, %u hard failures, %u deferred, %u held steps\n", schedules,
           retries, failures, deferrals, held);
    return 0;
}
'''


def invented_sizes():
    """Invented page sets: a two-big-pages set like UNDERTALE's shape, c26rev6's four-page case (both big pages put
    back while the threads hold the two small ones; the old estimate showed 109/110), equal sizes, one page,
    unpackable pages."""
    import random
    rng = random.Random(1108)
    sets = [('big2', [3_200_000, 1_700_000, 1_650_000, 1_150_000, 700_000] + [500_000 - 15_000 * i for i in range(21)]),
            ('four', [20_000_000, 19_000_000, 1_000_000, 1_000_000]), ('equal', [1_000_000] * 9), ('one', [2_000_000]), ('holes', [0, 900_000, 0, 1_800_000, 40_000, 0, 700_000])]
    for k in range(6):
        sets.append(('mixed%d' % k, [rng.choice([0, 4096, 65536, 1 << 20, 3_300_000, rng.randrange(1, 4_000_000)])
                                      for _ in range(rng.choice([2, 5, 26, 60]))]))
    extra = os.environ.get('FIRSTBOOT_SIZES_FILE')
    if extra:
        for line in Path(extra).read_text(encoding='utf-8').splitlines():
            if line.strip():
                name, *values = line.split()
                sets.append((name, [int(v) for v in values]))
    return sets


class ProductionWorkerTest(unittest.TestCase):
    """c26rev6 FIRSTBOOT-1: the cap must hold under the worker's real retry, failure and deferral paths."""

    def test_cap_under_retries_failures_and_deferrals(self):
        types, functions = production_parts()
        self.assertIn('worker->pageStates[page] = PAGE_PENDING;', functions)  # a memory wait puts the page back
        source = WORKER_PRELUDE % dict(types=types, functions=functions) + WORKER_DRIVER
        stdin = ''.join('%s %d %s\n' % (name, len(sizes), ' '.join(map(str, sizes))) for name, sizes in invented_sizes())
        with tempfile.TemporaryDirectory() as tmp:
            for opt in ('-O0', '-O2'):
                with self.subTest(opt=opt):
                    exe = build(tmp, source, 'worker', opt)
                    done = subprocess.run([str(exe), '1108'], input=stdin, capture_output=True, text=True,
                                          timeout=300, creationflags=NO_WINDOW)
                    self.assertEqual(done.returncode, 0, done.stdout[-2000:] + done.stderr[-2000:])
                    lines = done.stdout.splitlines()
                    summary = lines[-1].split()
                    # random: S schedules, R memory retries, F hard failures, D deferred, H held steps
                    schedules, retries, failures, deferrals, held = (int(summary[i]) for i in (1, 3, 6, 9, 11))
                    self.assertEqual(schedules, 40 * len(invented_sizes()))
                    self.assertGreater(retries, 1000)
                    self.assertGreater(failures, 50)
                    self.assertGreater(deferrals, 20)
                    self.assertGreater(held, 0, 'no schedule put back a page the fill had already shown')
                    for name, most in (('big2', 110), ('four', 40)):
                        retry = [line for line in lines if line.startswith('retry %s:' % name)]
                        self.assertEqual(len(retry), 1)
                        # The two big pages (0, 1) are put back; the threads hold the next two (2, 3).
                        self.assertIn('put back 0 1, held 2 3', retry[0])
                        fill = int(retry[0].split('fill ')[1].split('/')[0])
                        cap = int(retry[0].split('cap ')[1].split()[0])
                        self.assertLessEqual(fill, cap)
                        # four: done 0 + in hand 2.8 MB + an average page 10.6 MB of 41.5 MB is 35 px of 110.
                        self.assertLessEqual(fill, most)
                    if opt == '-O2':
                        print('\n' + '\n'.join(line for line in lines if line.startswith(('retry', 'failed', 'random'))))


if __name__ == '__main__':
    unittest.main()
