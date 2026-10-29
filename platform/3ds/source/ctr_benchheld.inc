// ===== Held frames (stallfix, rev 3, 10-27): where each hitch is, in the standard bench, no special build.
//
// Presentation v1 (ctr_benchfid_rooms.inc) counts per room visit how long each shown frame stayed up. Here every flip
// that ended a hold of 3 or more vblanks in a measured window (a frame the player saw for longer than a 30 fps slot) is
// listed with the frame it came from, so that a developer can run that frame on Azahar (tools/analysis/azframe.py) and look
// at it. Included at the end of ctr_benchmark.c and hooked by link-time wraps (end of this file), so no existing
// function moves. The lines come at the scene's result, before its room lines (cold: nothing is written while
// measuring):
//     held_frame <id> frame=<f> room=<room> vb=<n> class=<c> ms=<t> step=<t> draw=<t> gpu=<t> gpuwait=<t>
//         tex=<t> snd=<t>                                              (benchmark.txt and [result], one line each)
//     held_frames <id> play=N entry=N hidden=N black=N cut=N                         (benchmark.txt and [result])
// At most HELD_MAX lines a scene (the first ones); cut says how many more there were (the counts include them).
//   frame   the game frame (runner->frameCount: the counter of the bench's "measured frames A-B" line, the scene
//           scripts and Azahar). The pacer flips a frame once the GPU has drawn it and its slot has come, so the late
//           flip the bench sees as frame F+2 begins is the one after frame F's (or F+1's): of the two frames before,
//           F+1 if its interval was over a slot and longer than F's, else F.
//   vb      vblanks the frame before it stayed up (3 = one 30 fps frame dropped; 8+ holds at their mean).
//   class   play: a hitch in play (what fails a scene, tools/analysis/ut_beta_board.py and dr_board.py);
//           entry: one of a room's first 3 frames (visits after the first), including the hold presentation v1
//             leaves out when the new room's first played frame took over 2 vblanks of Step + Draw (stallfix rev 2);
//           hidden: the hold that ends at the first flip after a room change, through the hidden room load (frame =
//             the room-change frame, which is never shown); presentation v1 leaves it out too;
//           black: the game's screen is black (64 points of the top screen's shown framebuffer, every channel
//             under 8; the bench runs the game on the top screen): a fade's
//             black or a black screen, where a held frame shows nothing new.
//   ms      the frame's interval (its start to the next frame's start); step and draw as main.c times them; gpu the
//           GPU's time for the frame; gpuwait the main thread waiting for the GPU's frame before; tex
//           texture page load time; snd sound load time. Times in ms with one decimal (kept as floats). SD reads other than
//           texture and sound loads are not counted (spikerec builds count them).
// The cause (spikerec's rules) is worked out from these by the boards (ut_beta_board.py held_cause), to keep the
// late code small. Code and constants in .text.stallfix_late, state in .bss.stallfix_late (late_reserve.ld, last).
#define HELD_LATE __attribute__((noinline, noclone, optimize("Os"), section(".text.stallfix_late")))
#define HELD_CONST(name, value) static const char name[] __attribute__((section(".text.stallfix_late.constants"))) = value
#define HELD_MAX 24u
#define HELD_SLOT 33.4f // two vblanks, ms

uint64_t CtrAudioSystem_heldLoadTicks(AudioSystem* audio);

// t: ms, step, draw, gpu, gpuwait, tex, snd. frame 0: not recorded (frame 0 is never measured).
typedef struct {
    uint32_t frame;
    float t[7];
    uint8_t vb, cls, visit;
} BenchHeld;

static struct {
    BenchHeld ring[4]; // the last frames, by frame & 3
    BenchHeld list[HELD_MAX];
    uint16_t counts[4];
    uint16_t listed, cut;
    bool started, open;
    float texMs;
    uint32_t wait, snd; // tick totals, low words (a frame is far under 2^32 ticks, 16 s)
} benchHeld __attribute__((section(".bss.stallfix_late")));

HELD_CONST(heldClasses, "play\0\0\0entry\0\0hidden\0black");
HELD_CONST(heldLine, "held_frame %s frame=%lu room=%s vb=%u class=%s ms=%.1f step=%.1f draw=%.1f gpu=%.1f "
                     "gpuwait=%.1f tex=%.1f snd=%.1f\n");
HELD_CONST(heldTotal, "held_frames %s play=%u entry=%u hidden=%u black=%u cut=%u\n");

// The game's screen black at 64 points: the top screen, as the bench runs the game (BGR8; the bottom one is RGB565).
static inline __attribute__((always_inline)) bool heldBlack(void) {
    if (gfxGetScreenFormat(GFX_TOP) != GSP_BGR8_OES) return false;
    u16 w = 0, h = 0;
    const u8* fb = CtrPacer_shownFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
    uint32_t pixels = (uint32_t) w * h;
    if (fb == nullptr || pixels < 64) return false;
    GSPGPU_InvalidateDataCache(fb, pixels * 3);
    for (uint32_t i = 0; i < 64; i++) {
        const u8* p = fb + ((pixels >> 6) * i + (pixels >> 7)) * 3;
        if ((p[0] | p[1] | p[2]) >= 8) return false;
    }
    return true;
}

// One held flip of vb vblanks seen as frame now begins; hidden: the hold presentation v1 leaves out.
static inline __attribute__((always_inline)) void heldAdd(uint32_t now, uint32_t vb, bool hidden) {
    uint32_t visit = benchRooms.count - 1, first = (uint32_t) benchRooms.visits[visit].firstFrame;
    const BenchHeld* f2 = &benchHeld.ring[(now - 2) & 3];
    const BenchHeld* f1 = &benchHeld.ring[(now - 1) & 3];
    uint32_t want = now - 2;
    uint8_t cls = 0;
    if (hidden) {
        // The room-change frame, or the new room's first played frame when its Step + Draw took over 2 vblanks.
        const BenchHeld* e = &benchHeld.ring[first & 3];
        want = first - 1;
        cls = 2;
        if (e->frame == first && e->t[1] + e->t[2] > HELD_SLOT) {
            want = first;
            cls = 1;
        }
    } else {
        if (f1->frame == now - 1 && f1->t[0] > HELD_SLOT + 0.5f && (f2->frame != now - 2 || f1->t[0] > f2->t[0])) want = now - 1;
        if (visit > 0 && want >= first && first + 3 > want) cls = 1;
        else if (heldBlack()) cls = 3;
    }
    benchHeld.counts[cls]++;
    if (benchHeld.listed >= HELD_MAX) {
        benchHeld.cut++;
        return;
    }
    BenchHeld* r = &benchHeld.list[benchHeld.listed++];
    *r = benchHeld.ring[want & 3];
    if (r->frame != want) {
        memset(r, 0, sizeof(*r));
        r->frame = want;
    }
    r->vb = vb > 255 ? 255 : (uint8_t) vb;
    r->cls = cls;
    r->visit = (uint8_t) (want >= first || visit == 0 ? visit : visit - 1);
}

// Each measured frame, at benchshownRead's flip read (the wrapper below, before benchshownRead takes its new totals):
// the frame just ended goes in the ring, then each hold of 3+ vblanks since benchshownRead's last read is added.
static inline __attribute__((always_inline)) void heldRead(const CtrFlipStats* t) {
    // benchshownRoomsOpen clears benchfidShown, then reads: a new window (lastSum is never 0 after the first flip).
    if (benchfidShown.lastSum == 0) {
        memset(&benchHeld, 0, sizeof(benchHeld));
        benchHeld.open = true;
        return;
    }
    // benchshownRoomsCollect's condition (benchRoomsCollect counts work after it: one read a frame).
    if (!benchHeld.open || !benchRooms.active || benchRooms.count == 0 || bench.measureFrames <= (int32_t) benchRooms.work)
        return;
    Runner* runner = bench.session.runner;
    uint32_t now = (uint32_t) runner->frameCount;
    // The flips since benchshownRead's last read, bucketed as it does (the 8+ holds' vblanks in longSum).
    uint32_t d[CTR_FLIP_BUCKETS], flips = 0, shortSum = 0;
    for (uint32_t i = 0; i < CTR_FLIP_BUCKETS; i++) {
        d[i] = t->count[i] - benchfidShown.lastCount[i];
        flips += d[i];
        shortSum += d[i] * (i + 1);
    }
    uint32_t longSum = (uint32_t) t->sum - benchfidShown.lastSum - (shortSum - d[CTR_FLIP_BUCKETS - 1] * CTR_FLIP_BUCKETS);
    CtrRenderStats rs = CtrRenderer_getStats(bench.session.renderer);
    uint32_t wait = (uint32_t) CtrGpu_waitTotalTicks();
    uint32_t snd = runner->audioSystem != nullptr ? (uint32_t) CtrAudioSystem_heldLoadTicks(runner->audioSystem) : 0;
    BenchHeld* f = &benchHeld.ring[(now - 1) & 3];
    memset(f, 0, sizeof(*f));
    if (benchHeld.started) {
        f->frame = now - 1;
        f->t[0] = (float) runner->deltaTime * 0.001f;
        f->t[1] = (float) (bench.stepSum - benchRooms.previousStep);
        f->t[2] = (float) (bench.drawSum - benchRooms.previousDraw);
        // The GPU's last finished frame is the one before.
        benchHeld.ring[(now - 2) & 3].t[3] = rs.gpuMs;
        f->t[4] = (float) (wait - benchHeld.wait) * (float) (1.0 / CPU_TICKS_PER_MSEC);
        f->t[5] = rs.textureLoadMs - benchHeld.texMs;
        // The audio system's window total starts again each stats window.
        f->t[6] = (float) (snd >= benchHeld.snd ? snd - benchHeld.snd : snd) * (float) (1.0 / CPU_TICKS_PER_MSEC);
    }
    benchHeld.started = true;
    benchHeld.wait = wait;
    benchHeld.snd = snd;
    benchHeld.texMs = rs.textureLoadMs;
    if (flips == 0 || CtrRenderer_screenshotTimingAffected(bench.session.renderer)) return;
    bool hidden = benchfidShown.skipNext;
    for (int32_t i = CTR_FLIP_BUCKETS - 1; i >= 2; i--) {
        for (uint32_t n = 0; n < d[i]; n++) {
            heldAdd(now, i == CTR_FLIP_BUCKETS - 1 ? longSum / d[i] : (uint32_t) i + 1, hidden);
            hidden = false;
        }
    }
}

// At the scene's result (computeResult, cold), appended to benchmark.txt as the room lines are.
static inline __attribute__((always_inline)) void heldReport(const char* id) {
    char row[256];
    snprintf(row, sizeof(row), roomsPathFormat, bench.session.gameDir, roomsFileName);
    FILE* f = fopen(row, roomsFileMode);
    for (uint32_t i = 0; i < benchHeld.listed; i++) {
        const BenchHeld* r = &benchHeld.list[i];
        emit(f, heldLine, id, (unsigned long) r->frame, benchRooms.visits[r->visit].room, r->vb, heldClasses + 7 * r->cls,
             (double) r->t[0], (double) r->t[1], (double) r->t[2], (double) r->t[3], (double) r->t[4],
             (double) r->t[5], (double) r->t[6]);
    }
    emit(f, heldTotal, id, benchHeld.counts[0], benchHeld.counts[1], benchHeld.counts[2], benchHeld.counts[3],
         benchHeld.cut);
    if (f != nullptr) fclose(f);
    benchHeld.open = false;
}

// The hooks, by link-time wraps (CMakeLists.txt) so that no existing function changes: CtrPacer_getFlipTotals is
// called by benchshownRead each measured frame (and at the window's start), CtrPacer_logLevels once by computeResult
// at each scene's result. Other callers (ctr_rosalina.c's thread, z_spikerec.c) are passed through: only the game
// thread (threadGetCurrent() is null there) inside a rooms window reads. The pacer-observe diagnostic build wraps
// CtrPacer_getFlipTotals itself and has no held lines.
#ifndef CTRPORT_PACER_OBSERVE
void __real_CtrPacer_getFlipTotals(CtrFlipStats* out);
void __wrap_CtrPacer_getFlipTotals(CtrFlipStats* out);
__attribute__((noinline, optimize("Os"), section(".text.stallfix_late"))) void __wrap_CtrPacer_getFlipTotals(CtrFlipStats* out) {
    __real_CtrPacer_getFlipTotals(out);
    if (threadGetCurrent() == nullptr) heldRead(out);
}

// benchfix round 6 (argstage47 F2): the report is bfHeldReport (ctr_benchmark.c, after this file), which first
// completes the window's last read (the gpu and gpuwait benchfix fills in), then runs heldReport. Round 7: the
// held lines' window boundary (bfHeldBoundary) runs first. This wrap keeps its
// stage slot (cand24): padded to the old size with a fill reached only by a phase that never occurs, so no late code
// after it moves.
static void bfHeldReport(const char* id);
static void bfHeldBoundary(void);
void __real_CtrPacer_logLevels(const char* id, const CtrLevelTotals* start, double seconds);
void __wrap_CtrPacer_logLevels(const char* id, const CtrLevelTotals* start, double seconds);
__attribute__((noinline, optimize("Os"), section(".text.stallfix_late"))) void __wrap_CtrPacer_logLevels(const char* id,
        const CtrLevelTotals* start, double seconds) {
    __real_CtrPacer_logLevels(id, start, seconds);
    if (benchHeld.open && benchRooms.active && !sweep.on) {
        bfHeldBoundary(); // round 7 (argbenchr6 M3): a window that closes in the call after its first read
        bfHeldReport(id);
        return;
    }
    if (__builtin_expect(bench.phase != 255, 1)) return;
    __asm__ volatile(".space 320"); // the stage body: 420 B
    __builtin_unreachable();
}
#endif

#undef HELD_LATE
#undef HELD_CONST
#undef HELD_MAX
#undef HELD_SLOT
