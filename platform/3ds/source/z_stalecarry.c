// stalecarry (change battlesplit, 11-13; UNDERTALE development builds only): what a Game screen change carries, and how
// long the carried frame shows (an estimate).
//
// Never in a release build: CMake compiles this file, and points the calls below here, only without
// CTRPORT_RELEASE_SOURCE_URL. At the move up into a battle the top screen showed a picture from before the battle (b8hand
// snow top_00249: the frame the move down at frame 92 had carried, 156 frames old) until the game's first frame there.
// Each move now adds to the bench's screen-move line (z_screenmovestamp.c):
// - "carry shown about N vblanks (estimate)": from the vblank that showed the carried frame on the game's new screen to
//   the one after the game's first present there (bottomflip's counts, ctr_frame_pacer.c; -1 if another present came
//   between or none was counted), and how many presents came between. An estimate, not a measurement: it is computed
//   just before that present is submitted, and the line is logged before it too, so the replacement can show later
//   than the next refresh (c26rev6 STALECARRY-1). Never a timing gate: the hashes and buffers answer which picture was
//   carried;
// - the screen the game leaves: the buffer CtrPacer_shownFramebuffer answers (the carry's source) and libctru's other
//   one, their physical addresses and a hash of the 320 columns a carry copies, each marked when it equals the last
//   move's carried picture; that screen's framebuffer registers (A, B and the select bit: the buffer its LCD scans); the
//   buffer bottomflip saw presented last there; and the buffer of the game's first present on its new screen.
// Hooks are call targets only, set by CMake: ctr_renderer.c's CtrPacer_setGameScreen calls (only in its two moves,
// screensMoveGame and scanoutMove, after the carry's source is chosen and before it is cleared) come to
// CtrStale_setGameScreen, and its CtrLog_endConsole calls (the moves' and others', screenshots and notices; only a move's
// does anything here) to CtrStale_endConsole; ctr_frame_pacer.c's __real_gspPresentBuffer calls (bottomflip's) to
// CtrStale_present, and its __real_CtrBottomPaint_vblank1Lock call to CtrStale_vblank, and ctr_gpu_queue.c's
// gfxScreenSwapBuffers calls to CtrStale_gpuSwap (those two: the death flash watch, at the end, off unless battle_probe
// has 32); z_screenmovestamp.c's logInfo calls to CtrStale_log, which keeps the
// screen-move line until the game's first present after the move and logs it then with this added, from that present's
// thread (the main thread, or the GSP event thread for a held flip). A move with no bench stamp logs the same under its
// own prefix. A line kept when no present follows (the app exits) is not logged. scanoutMove has cleared the leaving
// screen's other buffer by then, so its hash there is the black picture's; screensMoveGame's is intact.
// Cost: one load and branch per present (and the flash watch's switch, below); the two hashes (230,400 B each) run
// inside the move, before the other screen's vblank wait, and the first present's line is logged before that present:
// both add time (not measured) to the move's ms on the line and to the hand-over, so neither compares with a build
// without the probe. Code and constants last in late text, state last in late .bss (late_reserve.ld).

#include <3ds.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ctr_frame_pacer.h"
#include "log.h"

#define SC_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text_stalecarry")))
#define SC_CONST __attribute__((aligned(4), section(".rodata_stalecarry")))
#define SC_CARRY_BYTES (320u * 240u * 3u)
#define SC_CARRY_AT (40u * 240u * 3u)

extern bool CtrScreens_onBottom;
void CtrPacer_setGameScreen(gfxScreen_t screen); // the linker's wrap (ctr_frame_pacer.c), as ctr_renderer.c's calls
void CtrLog_endConsole(void);
bool __real_gspPresentBuffer(unsigned screen, unsigned swap, const void* fb_a, const void* fb_b, u32 stride, u32 mode);
bool CtrStale_bottomflip(unsigned screen, u32 out[6]); // ctr_frame_pacer.c: seq, seen, vblanks, lastShown, valid, lastFb

static struct {
    u8* fb[2]; // the leaving screen's shown buffer (the carry's source) and its other one
    u32 phys[2], hash[2], regs[5], lastFb, carryHash, carrySeq;
    bool armed, moving, same[2], pending;
    u8 from, to;
    char line[192];
} sc __attribute__((section(".bss.stalecarry_late")));

static const char scMove[] SC_CONST = "Renderer: screen move frame=";
static const char scNoStamp[] SC_CONST = "Renderer: screen move (no bench stamp)";
static const char scSame[] SC_CONST = " (the last carry)";
static const char scTop[] SC_CONST = "top";
static const char scBottom[] SC_CONST = "bottom";
static const char scLine[] SC_CONST =
    "%s carry shown about %ld vblanks (estimate; %lu presents between); from the %s screen: shown %08lx phys %08lx "
    "hash %08lx%s, other %08lx phys %08lx hash %08lx%s; registers A %08lx B %08lx select %lu; last presented %08lx; "
    "first present %08lx\n";

// A hash of the carry's 320 columns of a buffer (the top screen's middle ones). Its cache lines are dropped first, as
// the move's copy (the source) and clear (the other buffer) do next.
SC_CODE static u32 scHash(u8* fb, unsigned screen) {
    u8* p = fb + (screen == GFX_TOP ? SC_CARRY_AT : 0u);
    GSPGPU_InvalidateDataCache(p, SC_CARRY_BYTES);
    const u32* w = (const u32*) p;
    u32 h = 2166136261u;
    for (u32 i = 0; SC_CARRY_BYTES / 4u > i; i++) h = (h ^ w[i]) * 16777619u;
    return h;
}

// In a move, before the game screen changes: the game's last frame still shown on the screen it leaves and nothing held
// (the move waited). That screen's double buffering is put back as it was (the pacer's queries leave it on).
SC_CODE static void scBegin(unsigned from) {
    u16 w = 0, h = 0;
    u8* now = CtrPacer_backFramebuffer((gfxScreen_t) from, GFX_LEFT);
    u8* shown = CtrPacer_shownFramebuffer((gfxScreen_t) from, GFX_LEFT, &w, &h);
    u8* other = CtrPacer_backFramebuffer((gfxScreen_t) from, GFX_LEFT);
    gfxSetDoubleBuffering((gfxScreen_t) from, now == other && other != shown);
    u32 bf[6] = { 0 };
    bool counted = CtrStale_bottomflip(from, bf);
    sc.from = (u8) from;
    sc.fb[0] = shown;
    sc.fb[1] = other;
    sc.lastFb = counted ? bf[5] : 0u;
    memset(sc.regs, 0, sizeof(sc.regs));
    GSPGPU_ReadHWRegs(from == GFX_TOP ? 0x400468u : 0x400568u, sc.regs, sizeof(sc.regs));
    bool sized = w == 240u && h == (from == GFX_TOP ? 400u : 320u) && gfxGetScreenFormat((gfxScreen_t) from) == GSP_BGR8_OES;
    for (u32 i = 0; 2u > i; i++) {
        sc.phys[i] = sc.fb[i] != NULL ? osConvertVirtToPhys(sc.fb[i]) : 0u;
        sc.hash[i] = sized && sc.fb[i] != NULL ? scHash(sc.fb[i], from) : 0u;
        sc.same[i] = sc.hash[i] != 0u && sc.hash[i] == sc.carryHash;
    }
    sc.carryHash = sc.hash[0];
    sc.moving = true;
}

// After the move: the carried frame presented on the game's new screen (scanoutMove waited for it to show;
// screensMoveGame waits just after). The game's next present there is its first frame.
SC_CODE static void scEnd(void) {
    if (!sc.moving) return;
    sc.moving = false;
    sc.to = (u8) (CtrScreens_onBottom ? GFX_BOTTOM : GFX_TOP);
    u32 bf[6] = { 0 };
    sc.carrySeq = CtrStale_bottomflip(sc.to, bf) ? bf[0] : 0u;
    __atomic_store_n(&sc.armed, sc.to != sc.from, __ATOMIC_RELEASE);
}

static void flMove(void); // the death flash watch (below)
void CtrStale_setGameScreen(gfxScreen_t screen);
SC_CODE void CtrStale_setGameScreen(gfxScreen_t screen) {
    flMove();
    scBegin(screen == GFX_TOP ? GFX_BOTTOM : GFX_TOP);
    CtrPacer_setGameScreen(screen);
}

void CtrStale_endConsole(void);
SC_CODE void CtrStale_endConsole(void) {
    CtrLog_endConsole();
    scEnd();
}

// The game's first present on its new screen (from bottomflip's wrap, before the real present, so that screen's last
// counted present is still the carry's). Any thread; the pacer's lock may be held.
SC_CODE static void scFirst(unsigned screen, const void* fb) {
    if (screen != sc.to || !__atomic_exchange_n(&sc.armed, false, __ATOMIC_ACQ_REL)) return;
    u32 bf[6] = { 0 };
    long shown = -1;
    u32 between = 0;
    if (CtrStale_bottomflip(screen, bf)) {
        between = bf[0] - sc.carrySeq;
        if (between == 0u && bf[1] == sc.carrySeq && bf[4] != 0u) shown = (long) (bf[2] + 1u - bf[3]);
    }
    logInfo(scLine, sc.pending ? sc.line : scNoStamp, shown, (unsigned long) between, sc.from == GFX_TOP ? scTop : scBottom,
            (unsigned long) sc.fb[0], (unsigned long) sc.phys[0], (unsigned long) sc.hash[0],
            sc.same[0] ? scSame : scSame + sizeof(scSame) - 1u, (unsigned long) sc.fb[1], (unsigned long) sc.phys[1],
            (unsigned long) sc.hash[1], sc.same[1] ? scSame : scSame + sizeof(scSame) - 1u, (unsigned long) sc.regs[0],
            (unsigned long) sc.regs[1], (unsigned long) (sc.regs[4] & 1u), (unsigned long) sc.lastFb, (unsigned long) fb);
    sc.pending = false;
}

bool CtrBattlesplit_flashProbe(void);
static void flPresent(unsigned screen, const void* fb_a, u32 stride, u32 mode); // the death flash watch (below)
bool CtrStale_present(unsigned screen, unsigned swap, const void* fb_a, const void* fb_b, u32 stride, u32 mode);
SC_CODE bool CtrStale_present(unsigned screen, unsigned swap, const void* fb_a, const void* fb_b, u32 stride, u32 mode) {
    if (__builtin_expect(__atomic_load_n(&sc.armed, __ATOMIC_ACQUIRE), 0)) scFirst(screen, fb_a);
    if (__builtin_expect(CtrBattlesplit_flashProbe(), 0)) flPresent(screen, fb_a, stride, mode);
    return __real_gspPresentBuffer(screen, swap, fb_a, fb_b, stride, mode);
}

// z_screenmovestamp.c's lines: the screen-move one kept for the first present (above), the others logged as before.
// The others go to platformLog, logInfo's own entry (log.c's vLogInfo is exactly this call), as ctr_exit.c does:
// vLogInfo is otherwise dead-stripped, so calling it would link a new function into .text and move dev's later code.
extern void platformLog(logType type, const char* format, va_list args);
void CtrStale_log(const char* fmt, ...);
SC_CODE void CtrStale_log(const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    if (strncmp(fmt, scMove, sizeof(scMove) - 1u) == 0) {
        vsnprintf(sc.line, sizeof(sc.line), fmt, va);
        size_t n = strlen(sc.line);
        if (n > 0u && sc.line[n - 1u] == '\n') sc.line[n - 1u] = '\0';
        sc.pending = true;
    } else {
        platformLog(LOG_TYPE_NORMAL, fmt, va);
    }
    va_end(va);
}

// ===[ Death flash watch (battle_probe 32) ]===
// maintainer's 11-12 report (play1 o3ds, Muffet): at his death, as the SOUL splits or just before, the screens flicker
// black. No Game screen move happens there (the game-over room keeps the game where it is), so the probe above sees nothing.
// With battle_probe 32 in a bench run (CtrBattlesplit_flashProbe, z_battlesplit.c), two more paths log, each only on a
// change, one "flash:" line each with a number n counted across both (taken before the line is queued, so sort a
// capture by n first; the log writer logs its own count of the lines it dropped):
// - every present, on both screens (bottomflip's real present calls, its kept bottom ones on the GSP event thread with
//   pace's lock held too). Only a GPU frame is sampled (c26rev6 FLASHW-1): the present must be of the very buffer the
//   GPU queue's flip named (ctr_gpu_queue.c's gfxScreenSwapBuffers calls come to CtrStale_gpuSwap; they run once the
//   queue has finished, its display transfers into that buffer done), the first present on that screen since, with no
//   Game screen move between, in RGBA8 or BGR8: no console or painter draws those (libctru's consoles, the bottom
//   painter, menus and notices are 16-bit), and the CPU writers that do draw into such a buffer (battletouch's hints
//   and recolour, carries, fades) flush what they wrote before its present. Its cache lines are then dropped (the CPU
//   holds no newer data there) and a grid read (every 8th column, every 4th pixel down); a pixel is lit when a channel
//   reaches an eighth of its range. Any other present is "not sampled", nothing invalidated. A future CPU writer into a
//   GPU-flipped RGBA8/BGR8 buffer must flush before that present too, or this invalidate drops its pixels. Logged when
//   the screen's state (black: no lit sample; lit; not sampled) or format changes, with the present's number,
//   bottomflip's vblank count, the buffer, the lit count and a hash. Black means no lit grid sample, not every pixel;
//   top wide mode: the first 400 columns only. COVERAGE ABSENT for 16-bit pictures: a Split battle's GPU-drawn RGB565
//   bottom is never sampled (a black one there logs "not sampled", and only a register or fill change would show);
// - every bottom vblank (bottomflip's call of the bottom painter's vblank lock, on the GSP event thread, pace's lock
//   not held): each screen's framebuffer registers and LCD fill register, read with GSPGPU_ReadHWRegs. Logged when
//   the state of the buffer that screen scans (that buffer's last present: black, lit, not sampled; "not presented"
//   when it is neither of the screen's last two presents; "unknown" with no coherent copy of them) or the format, the
//   select bit, the fill register or either read's result changes; a failed read logs as "read failed" with its
//   result, never its zeros. It reuses the last present's sample: a picture changed in place, or black never
//   presented nor set by a register, shows in neither path.
// The presents' records (c26rev6 FLASHW-2): a buffer and its state are written and copied together, under a one-byte
// flag taken without waiting (no spinning, no pace lock). A present that finds it taken counts a miss (on its screen's
// next line) and marks the record lost; a vblank that finds it taken, or lost, reports "unknown". INVARIANT: one
// serialized publisher per screen. The lost mark is not a multi-publisher protocol: if one present held the flag while
// a second failed and marked the record lost, the first one's success would clear that mark. UNDERTALE's gameplay
// presents (bottomflip's kept one included) are serialized by pace's lock, and the vblank reader never clears lost;
// loading and setup paths that bypass that lock do not overlap a bench's gameplay. A new presenter that bypasses
// pace's lock during play breaks this: revisit it then (c26rev6 r2).
// Gate off: a call, a load and a branch per present, per GPU flip and per bottom vblank. Gate on: a cache invalidate
// and up to 3,000 samples per GPU frame present, four register reads per vblank: its rows are not timing evidence.
void __real_CtrBottomPaint_vblank1Lock(LightLock* gate);

enum { FL_BLACK = 1, FL_LIT, FL_UNSAMPLED, FL_UNPRESENTED, FL_UNKNOWN };
typedef struct {
    u32 phys[2], fmt;          // a screen's last two presented buffers; the format last logged
    u8 state[2], next, last;   // their states; the slot replaced next; the state last logged
} FlRecord;
static struct {
    FlRecord rec[2];
    const void* gpuFb[2]; // the buffer the GPU queue's last flip of a screen named, until that screen's next present
    u32 n, missed[2], vfmt[2], select[2], fill[2], rcRegs[2], rcFill[2];
    u8 busy[2], lost[2], scan[2];
} fl __attribute__((section(".bss.stalecarry_late")));

static const char flStates[6][14] SC_CONST = { "", "black", "lit", "not sampled", "not presented", "unknown" };
static const char flPresentLine[] SC_CONST =
    "flash: n=%lu present %s #%lu vblank %lu: %s, fb %08lx phys %08lx format %lu, lit %lu of %lu, hash %08lx, "
    "records missed %lu\n";
// Indexed by (framebuffer registers' read failed) * 2 + (fill read failed).
static const char flVblankLines[4][112] SC_CONST = {
    "flash: n=%lu vblank %lu %s scans %08lx (%s), format %08lx select %lu, fill %08lx\n",
    "flash: n=%lu vblank %lu %s scans %08lx (%s), format %08lx select %lu, fill read failed (%08lx)\n",
    "flash: n=%lu vblank %lu %s framebuffer registers read failed (%08lx), fill %08lx\n",
    "flash: n=%lu vblank %lu %s framebuffer registers read failed (%08lx), fill read failed (%08lx)\n",
};

// The GPU queue's flips (ctr_gpu_queue.c, after its queue finished): the buffer this one presents is libctru's back
// buffer now, the one the display transfers wrote. The call goes on to the pacer's wrap, as the queue's did.
void CtrStale_gpuSwap(gfxScreen_t screen, bool hasStereo);
SC_CODE void CtrStale_gpuSwap(gfxScreen_t screen, bool hasStereo) {
    if (__builtin_expect(CtrBattlesplit_flashProbe(), 0) && GFX_BOTTOM >= screen)
        __atomic_store_n(&fl.gpuFb[screen], gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL), __ATOMIC_RELEASE);
    gfxScreenSwapBuffers(screen, hasStereo);
}

// A Game screen move (CPU copies and clears, CtrStale_setGameScreen): no flip named before it is a GPU frame after.
SC_CODE static void flMove(void) {
    __atomic_store_n(&fl.gpuFb[0], NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&fl.gpuFb[1], NULL, __ATOMIC_RELEASE);
}

// A GPU frame's grid (RGBA8 or BGR8): its state, lit count, sample count and hash.
SC_CODE static u32 flSample(const u8* fb, unsigned screen, u32 stride, u32 format, u32 out[3]) {
    u32 bpp = format == GSP_RGBA8_OES ? 4u : 3u;
    if (stride == 0u) stride = 240u * bpp;
    u32 cols = screen == GFX_TOP ? 400u : 320u;
    GSPGPU_InvalidateDataCache(fb, cols * stride);
    u32 h = 2166136261u;
    for (u32 x = 4; cols > x; x += 8u) {
        for (u32 y = 0; 240u > y; y += 4u) {
            const u8* p = fb + x * stride + y * bpp;
            u32 v = (u32) p[0] | (u32) p[1] << 8 | (u32) p[2] << 16;
            u32 c = v;
            if (bpp == 4u) {
                v |= (u32) p[3] << 24;
                c = v >> 8; // RGBA8: alpha is the low byte
            }
            out[0] += ((c | c >> 8 | c >> 16) & 0xE0u) != 0u;
            out[1]++;
            h = (h ^ v) * 16777619u;
        }
    }
    out[2] = h;
    return out[0] != 0u ? FL_LIT : FL_BLACK;
}

SC_CODE static void flPresent(unsigned screen, const void* fb_a, u32 stride, u32 mode) {
    if (screen > GFX_BOTTOM) return;
    u32 s[3] = { 0 }, bf[6] = { 0 }, format = mode & 7u;
    const void* gpu = __atomic_exchange_n(&fl.gpuFb[screen], NULL, __ATOMIC_ACQ_REL);
    bool gpuFrame = fb_a != NULL && gpu == fb_a && GSP_BGR8_OES >= format;
    u32 state = gpuFrame ? flSample((const u8*) fb_a, screen, stride, format, s) : FL_UNSAMPLED;
    u32 phys = fb_a != NULL ? osConvertVirtToPhys(fb_a) : 0u;
    if (__atomic_test_and_set(&fl.busy[screen], __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&fl.lost[screen], 1u, __ATOMIC_RELAXED);
        __atomic_add_fetch(&fl.missed[screen], 1u, __ATOMIC_RELAXED);
        return;
    }
    FlRecord* r = &fl.rec[screen];
    u32 i = r->phys[0] == phys ? 0u : r->phys[1] == phys ? 1u : (r->next ^= 1u);
    r->phys[i] = phys;
    r->state[i] = (u8) state;
    bool changed = state != r->last || format != r->fmt;
    r->last = (u8) state;
    r->fmt = format;
    __atomic_store_n(&fl.lost[screen], 0u, __ATOMIC_RELAXED);
    __atomic_clear(&fl.busy[screen], __ATOMIC_RELEASE);
    if (!changed) return;
    CtrStale_bottomflip(screen, bf);
    logInfo(flPresentLine, (unsigned long) __atomic_add_fetch(&fl.n, 1u, __ATOMIC_RELAXED),
            screen == GFX_TOP ? scTop : scBottom, (unsigned long) (bf[0] + 1u), (unsigned long) bf[2], flStates[state],
            (unsigned long) fb_a, (unsigned long) phys, (unsigned long) format, (unsigned long) s[0],
            (unsigned long) s[1], (unsigned long) s[2],
            (unsigned long) __atomic_load_n(&fl.missed[screen], __ATOMIC_RELAXED));
}

// The state of the buffer a screen scans, from a coherent copy of its record; unknown without one.
SC_CODE static u32 flScanned(unsigned screen, u32 scanned) {
    if (__atomic_test_and_set(&fl.busy[screen], __ATOMIC_ACQUIRE)) return FL_UNKNOWN;
    FlRecord r = fl.rec[screen];
    bool lost = __atomic_load_n(&fl.lost[screen], __ATOMIC_RELAXED) != 0u;
    __atomic_clear(&fl.busy[screen], __ATOMIC_RELEASE);
    if (lost) return FL_UNKNOWN;
    if (scanned != 0u && r.phys[0] == scanned) return r.state[0];
    if (scanned != 0u && r.phys[1] == scanned) return r.state[1];
    return FL_UNPRESENTED;
}

SC_CODE static void flVblank(void) {
    u32 bf[6] = { 0 };
    CtrStale_bottomflip(GFX_BOTTOM, bf);
    for (unsigned screen = 0; 2u > screen; screen++) {
        u32 regs[5] = { 0 }, fill = 0;
        Result rcRegs = GSPGPU_ReadHWRegs(screen == GFX_TOP ? 0x400468u : 0x400568u, regs, sizeof(regs));
        Result rcFill = GSPGPU_ReadHWRegs(screen == GFX_TOP ? 0x202204u : 0x202A04u, &fill, sizeof(fill));
        bool regsOk = R_SUCCEEDED(rcRegs), fillOk = R_SUCCEEDED(rcFill);
        u32 scanned = regsOk ? ((regs[4] & 1u) != 0u ? regs[1] : regs[0]) : 0u;
        u32 state = regsOk ? flScanned(screen, scanned) : 0u;
        u32 format = regsOk ? regs[2] : 0u, select = regsOk ? regs[4] & 1u : 0u;
        if (!fillOk) fill = 0u;
        if (state == fl.scan[screen] && format == fl.vfmt[screen] && select == fl.select[screen] &&
            fill == fl.fill[screen] && (u32) rcRegs == fl.rcRegs[screen] && (u32) rcFill == fl.rcFill[screen])
            continue;
        fl.scan[screen] = (u8) state;
        fl.vfmt[screen] = format;
        fl.select[screen] = select;
        fl.fill[screen] = fill;
        fl.rcRegs[screen] = (u32) rcRegs;
        fl.rcFill[screen] = (u32) rcFill;
        const char* line = flVblankLines[(regsOk ? 0u : 2u) + (fillOk ? 0u : 1u)];
        unsigned long n = __atomic_add_fetch(&fl.n, 1u, __ATOMIC_RELAXED), last = fillOk ? fill : (u32) rcFill;
        const char* name = screen == GFX_TOP ? scTop : scBottom;
        if (regsOk)
            logInfo(line, n, (unsigned long) bf[2], name, (unsigned long) scanned, flStates[state],
                    (unsigned long) format, (unsigned long) select, last);
        else
            logInfo(line, n, (unsigned long) bf[2], name, (unsigned long) (u32) rcRegs, last);
    }
}

// In place of bottomflip's call of the bottom painter's vblank lock (ctr_frame_pacer.c), after its counts.
void CtrStale_vblank(LightLock* gate);
SC_CODE void CtrStale_vblank(LightLock* gate) {
    if (__builtin_expect(CtrBattlesplit_flashProbe(), 0)) flVblank();
    __real_CtrBottomPaint_vblank1Lock(gate);
}
