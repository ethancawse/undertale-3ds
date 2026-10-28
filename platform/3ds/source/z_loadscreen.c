// loadscreen: a loading screen on the top screen while a game loads, from the launch (right
// after C3D_Init) and from each chapter switch, until the first room is ready. DELTARUNE shows the game's own loading
// text (obj_screen_loading's "...CHAPTER n" or the chapter select's), with Chapter 3's dog (spr_dog_sway) above it
// when the player's Chapter 3 is installed and known, over a thin bar; UNDERTALE shows a bar alone. The DELTARUNE art
// is cut from the player's own files when the game is built (tools/aot/loadscreen_art.py) and read from the RomFS
// (loadscreen.bin); nothing of it is in the repository. Without it, a DELTARUNE build shows the bar alone.
//
// A thread of its own draws, at a priority above the game thread's, so the bar and the dog move while the game thread
// is busy; it draws only what changed, into the back buffer, at most every 33 ms. The game thread marks the phases
// through link wraps (-Wl,--wrap in CMakeLists.txt), so main.c stays as it was: C3D_Init (launch),
// CtrPack_setDataWinPath (every load), CtrDataVersion_check (its progress, for a first hash), DataWin_parse,
// CtrPackScreen_run (paused; a first time setup draws here: the end), VM_create, SandboxFileSystem_create (paused while
// the renderer is made) and CtrPacer_loadStress (resumed), Runner_initFirstRoom (the end: the full bar is drawn and
// the thread goes idle before the game's first frame), and CtrWatchdog_stop (error screens and exit: the thread
// ends). Nothing runs per game frame. The thread is made once, at the first load, and kept idle between loads.
//
// The bar's progress: each phase's expected time on the Old 3DS (an o3DS UNDERTALE launch, 2026-10-21: parse
// 2449 ms, vm 197, bytecode cache 276, file system 297, runner 559, first room 1160), the size-bound phases scaled by
// the data.win's size, all of them by how fast this load's finished phases went. A phase that runs over creeps on;
// the bar never moves back and stops at 97% until the room is ready.
//
// Off with game_screen = 1 (the game on the bottom screen: the top shows the consoles) or loading_screen = 0 in
// settings.ini. Test hook: loadshot.txt in the game folder lists times (ms from each load's start); the screen is
// then saved as loadshot-<load>-<ms>.bmp at each.
//
// Late code only (platform/3ds/late_reserve.ld, "loadscreen"); every string is a named array in the late code, and
// this file is compiled without the printf builtins and switch tables (CMakeLists.txt).

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ctr_benchromfs.h"
#include "ctr_data_version.h"
#include "ctr_texture_pack_screen.h"
#include "ctr_wrap_chain.h"
#include "data_win.h"
#include "log.h"
#include "runner.h"
#include "sandbox_file_system.h"
#include "vm.h"

#define LS_CODE __attribute__((noinline, section(".text.late.loadscreen")))
#define LS_RO __attribute__((section(".rodata.loadscreen_in_text")))
#define LS_BSS __attribute__((section(".bss.late.loadscreen")))
#define LS_STR(name, text) static const char name[(sizeof(text) + 3u) & ~3u] LS_RO = text

#ifdef ENABLE_WAD16
#define LS_DELTARUNE 0
#define LS_DOG_HOOK(c, frame)
#else
#define LS_DELTARUNE 1
// The dog on its rope: lsDrawDog hands over to lsDogDraw (end of file; see the note there).
#define LS_DOG_HOOK(c, frame) { register u8* a0 __asm__("r0") = (c)->fb; register uint32_t a1 __asm__("r1") = (c)->rows; register int32_t a2 __asm__("r2") = (c)->scale; register uint32_t a3 __asm__("r3") = (c)->bpp << 16 | (uint32_t) (frame); __asm__ volatile ("bl lsDogDraw\n\tb 1f\n\t.space 128\n1:" : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3) : : "ip", "lr", "memory", "cc"); if ((frame) < 0) lsRect((c), 0, 0, 0, 0, 0); return; }
#endif
#line 55

#define LS_SCREEN_W 400
#define LS_SCREEN_H 240
#define LS_TICKS_PER_MS 268111u
#define LS_TICK_NS 33000000LL
#define LS_STACK 8192
#define LS_ART_MAX 65536u
#define LS_SHOTS_MAX 8
#define LS_CAP 970u

enum { P_BOOT, P_HASH, P_PARSE, P_PACK, P_VM, P_FS, P_GAP, P_RUNNER, P_ROOM, P_COUNT };

// Old 3DS milliseconds: a fixed part, and a part per MB of data.win in tenths.
static const uint16_t lsFixedMs[P_COUNT] LS_RO = { 600, 250, 0, 60, 0, 0, 5, 40, 0 };
static const uint16_t lsPerMb10[P_COUNT] LS_RO = { 0, 0, 389, 0, 31, 91, 0, 89, 184 };
// The phases whose speed says how fast the rest will go.
static const uint8_t lsMeasured[P_COUNT] LS_RO = { 0, 0, 1, 0, 1, 1, 0, 1, 1 };

LS_STR(lsSettingsPath, CTRPORT_GAME_DIR "/settings.ini");
LS_STR(lsShotsPath, CTRPORT_GAME_DIR "/loadshot.txt");
LS_STR(lsShotFormat, CTRPORT_GAME_DIR "/loadshot-%u-%05u.bmp");
LS_STR(lsKeyFormat, " %63[^= ] = %d");
LS_STR(lsGameKey, "game_screen");
LS_STR(lsOnKey, "loading_screen");
LS_STR(lsRead, "r");
LS_STR(lsWriteBinary, "wb");
LS_STR(lsLogOff, "Loading screen: off (%s)\n");
LS_STR(lsWhySetting, "loading_screen = 0");
LS_STR(lsWhyThread, "no thread");
LS_STR(lsLogDone, "Loading screen: load=%u %u ms, %u pictures, bar %u/1000 at the end; phases ms: start %u, "
                  "version %u, parse %u, textures %u, vm %u, files %u, renderer %u, runner %u, room %u\n");
#if LS_DELTARUNE
LS_STR(lsRomfsName, "romfs");
LS_STR(lsReadBinary, "rb");
LS_STR(lsChapterWord, "/chapter");
LS_STR(lsMagic, "DTLS");
LS_STR(lsArtPath, "romfs:/loadscreen.bin");
LS_STR(lsLogArt, "Loading screen: %u messages%s\n");
LS_STR(lsLogDog, ", Chapter 3's dog");
LS_STR(lsLogNoArt, "Loading screen: no loadscreen.bin in the RomFS: the bar alone\n");
LS_STR(lsNothing, "");
#endif

typedef struct {
    u8* fb;
    u16 rows;      // the framebuffer's width: the screen's height
    int32_t scale; // framebuffer columns per screen column: 2 in 800-pixel mode
    uint32_t bpp;
} LsCanvas;

typedef struct {
    // The game thread changes the state under the lock; the thread draws under it.
    LightLock lock;
    LightEvent wake;
    Thread thread;
    bool quit, running, paused, decided, enabled;
    bool firstBelow; // settings.ini puts the game on the bottom screen (the first load; later ones ask ctr_screens.c)
    bool firstDone;  // the first load's room is ready
    uint8_t phase;
    uint32_t load;
    uint64_t loadStart, phaseStart;
    uint32_t base[P_COUNT];   // this load's expected ms, unscaled
    uint32_t actual[P_COUNT]; // finished phases' ms
    uint32_t ratio;           // per mille: how long this load's phases take against base
    uint32_t sizeBytes;       // the data.win's
    volatile uint32_t hashCalls;
    void (*progress)(void);
    uint32_t shown;           // the bar's per mille as drawn
    int32_t fillW, frame;     // as last drawn
    uint32_t gen, key;        // a new generation redraws everything
    u8* drawnFb[2];
    uint32_t drawnGen[2];
    uint32_t pictures;
    // Layout.
    int32_t barX, barY, barW, barH, fillX, fillY, fillMax, fillH, frameT;
    int32_t textX, textY, dogX, dogY;
    // The art (DELTARUNE).
    const uint8_t* art;
    uint32_t artSize;
    const uint8_t* palette;
    const uint8_t* frames;
    uint32_t colours, frameCount, dogW, dogH, frameMs, messageCount;
    const uint8_t* text;      // this load's message bits, or nullptr
    uint32_t textW, textH;
    // loadshot.txt
    uint32_t shots[LS_SHOTS_MAX];
    uint32_t shotCount, shotNext;
    // Said once the log is open (the first room): why it is off, or what art it has (1 none, 2 some).
    const char* why;
    uint32_t artNote;
} LsState;

static LsState ls LS_BSS;

void __real_gfxScreenSwapBuffers(gfxScreen_t screen, bool hasStereo);
extern uint8_t CtrScreens_game;

// ===[ Drawing ]===

LS_CODE static void lsRect(const LsCanvas* c, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb) {
    u8 r = (u8) (rgb >> 16), g = (u8) (rgb >> 8), b = (u8) rgb;
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > LS_SCREEN_W) w = LS_SCREEN_W - x;
    if (y + h > LS_SCREEN_H) h = LS_SCREEN_H - y;
    if (w <= 0 || h <= 0) return;
    for (int32_t sx = x * c->scale; (x + w) * c->scale > sx; sx++) {
        // The framebuffer is the screen turned a quarter: a column of the screen, bottom to top, is a row of it.
        u8* p = c->fb + ((size_t) sx * c->rows + (size_t) (c->rows - y - h)) * c->bpp;
        for (int32_t i = 0; h > i; i++, p += c->bpp) {
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

LS_CODE static void lsClear(const LsCanvas* c) {
    size_t pixels = (size_t) LS_SCREEN_W * (size_t) c->scale * c->rows;
    if (c->bpp == 3) {
        memset(c->fb, 0, pixels * 3);
        return;
    }
    uint32_t* p = (uint32_t*) c->fb;
    for (size_t i = 0; pixels > i; i++) p[i] = 0xFFu;
}

LS_CODE static void lsDrawText(const LsCanvas* c) {
    if (ls.text == nullptr) return;
    uint32_t stride = (ls.textW + 7) / 8;
    for (uint32_t y = 0; ls.textH > y; y++) {
        const uint8_t* row = ls.text + y * stride;
        for (uint32_t x = 0; ls.textW > x; x++) {
            if (row[x >> 3] & (0x80u >> (x & 7))) lsRect(c, ls.textX + (int32_t) x, ls.textY + (int32_t) y, 1, 1, 0xFFFFFFu);
        }
    }
}

LS_CODE static void lsDrawDog(const LsCanvas* c, int32_t frame) {
    LS_DOG_HOOK(c, frame) if (ls.frames == nullptr) return;
    const uint8_t* f = ls.frames + (size_t) frame * ls.dogW * ls.dogH;
    for (uint32_t y = 0; ls.dogH > y; y++) {
        for (uint32_t x = 0; ls.dogW > x; x++) {
            uint32_t i = f[y * ls.dogW + x];
            uint32_t rgb = 0;
            if (i != 0 && i <= ls.colours) {
                const uint8_t* p = ls.palette + (i - 1) * 3;
                rgb = (uint32_t) p[0] << 16 | (uint32_t) p[1] << 8 | p[2];
            }
            lsRect(c, ls.dogX + (int32_t) x * 2, ls.dogY + (int32_t) y * 2, 2, 2, rgb);
        }
    }
}

LS_CODE static void lsDrawEye(gfx3dSide_t side, bool full, int32_t fillW, int32_t frame, LsCanvas* out) {
    u16 width = 0, height = 0;
    GSPGPU_FramebufferFormat format = gfxGetScreenFormat(GFX_TOP);
    LsCanvas c = { gfxGetFramebuffer(GFX_TOP, side, &width, &height), width, height >= 2 * LS_SCREEN_W ? 2 : 1,
                   format == GSP_RGBA8_OES ? 4u : 3u };
    if (full) {
        lsClear(&c);
        lsDrawText(&c);
        lsRect(&c, ls.barX, ls.barY, ls.barW, ls.barH, 0xFFFFFFu);
        lsRect(&c, ls.barX + ls.frameT, ls.barY + ls.frameT, ls.barW - 2 * ls.frameT, ls.barH - 2 * ls.frameT, 0);
    }
    lsRect(&c, ls.fillX, ls.fillY, fillW, ls.fillH, 0xFFFFFFu);
    lsRect(&c, ls.fillX + fillW, ls.fillY, ls.fillMax - fillW, ls.fillH, 0);
    lsDrawDog(&c, frame);
    GSPGPU_FlushDataCache(c.fb, (u32) width * height * c.bpp);
    if (out != nullptr) *out = c;
}

LS_CODE static void lsPut32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16);
    p[3] = (uint8_t) (v >> 24);
}

// The loadshot.txt test hook: the picture as a 400 x 240 BMP.
LS_CODE static void lsSaveShot(const LsCanvas* c, uint32_t ms) {
    char name[96];
    snprintf(name, sizeof(name), lsShotFormat, (unsigned) ls.load, (unsigned) ms);
    FILE* f = fopen(name, lsWriteBinary);
    if (f == nullptr) return;
    uint8_t* row = malloc(LS_SCREEN_W * 3 + 54);
    if (row != nullptr) {
        uint32_t data = LS_SCREEN_W * 3 * LS_SCREEN_H;
        memset(row, 0, 54);
        row[0] = 'B';
        row[1] = 'M';
        lsPut32(row + 2, data + 54);
        lsPut32(row + 10, 54);
        lsPut32(row + 14, 40);
        lsPut32(row + 18, LS_SCREEN_W);
        lsPut32(row + 22, LS_SCREEN_H);
        row[26] = 1;
        row[28] = 24;
        lsPut32(row + 34, data);
        fwrite(row, 1, 54, f);
        for (int32_t y = LS_SCREEN_H - 1; y >= 0; y--) {
            for (int32_t x = 0; LS_SCREEN_W > x; x++) {
                const u8* p = c->fb + ((size_t) x * c->scale * c->rows + (size_t) (c->rows - 1 - y)) * c->bpp;
                u8* q = row + x * 3;
                if (c->bpp == 4) {
                    q[0] = p[1];
                    q[1] = p[2];
                    q[2] = p[3];
                } else {
                    q[0] = p[0];
                    q[1] = p[1];
                    q[2] = p[2];
                }
            }
            fwrite(row, 1, LS_SCREEN_W * 3, f);
        }
        free(row);
    }
    fclose(f);
}

// Draws what changed into the back buffer and shows it. Under the lock.
LS_CODE static void lsPresent(int32_t fillW, int32_t frame, bool force) {
    GSPGPU_FramebufferFormat format = gfxGetScreenFormat(GFX_TOP);
    if (format != GSP_BGR8_OES && format != GSP_RGBA8_OES) return;
    u16 width = 0, height = 0;
    u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &width, &height);
    bool stereo = gfxIs3D();
    if (fb == nullptr || width != LS_SCREEN_H || height < LS_SCREEN_W) return;
    uint32_t key = (uint32_t) format | (uint32_t) height << 4 | (uint32_t) stereo << 16;
    if (key != ls.key) {
        ls.key = key;
        ls.gen++;
    }
    int32_t slot = ls.drawnFb[0] == fb ? 0 : ls.drawnFb[1] == fb ? 1 : -1;
    bool full = slot < 0 || ls.drawnGen[slot] != ls.gen;
    if (!full && !force && fillW == ls.fillW && frame == ls.frame) return;
    if (slot < 0) {
        slot = ls.drawnFb[0] == nullptr ? 0 : 1;
        if (ls.drawnFb[0] != nullptr && ls.drawnFb[1] != nullptr) slot = 0;
    }
    LsCanvas shot;
    lsDrawEye(GFX_LEFT, full, fillW, frame, &shot);
    if (stereo) lsDrawEye(GFX_RIGHT, full, fillW, frame, nullptr);
    ls.drawnFb[slot] = fb;
    ls.drawnGen[slot] = ls.gen;
    ls.fillW = fillW;
    ls.frame = frame;
    ls.pictures++;
    if (ls.shotNext < ls.shotCount) {
        uint32_t ms = (uint32_t) ((svcGetSystemTick() - ls.loadStart) / LS_TICKS_PER_MS);
        if (ms >= ls.shots[ls.shotNext]) {
            lsSaveShot(&shot, ls.shots[ls.shotNext]);
            ls.shotNext++;
        }
    }
    __real_gfxScreenSwapBuffers(GFX_TOP, stereo);
}

// ===[ Progress ]===

LS_CODE static uint32_t lsMs(uint64_t from, uint64_t to) {
    return (uint32_t) ((to - from) / LS_TICKS_PER_MS);
}

LS_CODE static uint32_t lsExpected(int32_t p, uint32_t elapsed) {
    uint32_t e = (uint32_t) ((uint64_t) ls.base[p] * ls.ratio / 1000u);
    if (p == P_HASH && ls.hashCalls > 0 && ls.sizeBytes > 0) {
        // A first check hashes the whole file: as long as its progress says.
        uint64_t bytes = (uint64_t) ls.hashCalls << 16;
        if (bytes > ls.sizeBytes) bytes = ls.sizeBytes;
        uint32_t hash = (uint32_t) ((uint64_t) elapsed * ls.sizeBytes / bytes);
        if (hash > e) e = hash;
    }
    return e > 0 ? e : 1;
}

// Per mille of the load done, by the clock.
LS_CODE static uint32_t lsTarget(uint64_t now) {
    uint64_t done = 0, total = 0;
    int32_t current = ls.phase;
    for (int32_t p = 0; current > p; p++) {
        uint32_t a = ls.actual[p];
        // "Preparing textures" has its own bar: its time isn't this one's.
        if (p == P_PACK) {
            uint32_t e = lsExpected(p, 0);
            if (a > e) a = e;
        }
        done += a;
        total += a;
    }
    uint32_t elapsed = lsMs(ls.phaseStart, now);
    uint32_t e = lsExpected(current, elapsed);
    // Linear to 90% of the phase at its expected end, then creeping on.
    uint32_t f = elapsed <= e ? 900u * elapsed / e : 900u + 100u * (elapsed - e) / elapsed;
    done += (uint64_t) e * f / 1000u;
    total += e;
    for (int32_t p = current + 1; P_COUNT > p; p++) total += lsExpected(p, 0);
    return total > 0 ? (uint32_t) (done * 1000u / total) : 0;
}

LS_CODE static void lsBase(uint32_t bytes) {
    for (int32_t p = 0; P_COUNT > p; p++)
        ls.base[p] = lsFixedMs[p] + (uint32_t) ((uint64_t) lsPerMb10[p] * bytes / (10u << 20));
}

// The game thread, under the lock: the load moves on to phase p.
LS_CODE static void lsPhaseLocked(int32_t p) {
    if (p <= ls.phase) return;
    uint64_t now = svcGetSystemTick();
    ls.actual[ls.phase] = lsMs(ls.phaseStart, now);
    for (int32_t q = ls.phase + 1; p > q; q++) ls.actual[q] = 0;
    ls.phase = (uint8_t) p;
    ls.phaseStart = now;
    uint64_t a = 0, b = 0;
    for (int32_t q = 0; p > q; q++) {
        if (!lsMeasured[q]) continue;
        a += ls.actual[q];
        b += ls.base[q];
    }
    if (b >= 100) {
        uint64_t r = a * 1000u / b;
        ls.ratio = r < 250 ? 250 : r > 4000 ? 4000 : (uint32_t) r;
    }
}

LS_CODE static void lsPhase(int32_t p) {
    if (!ls.running) return;
    LightLock_Lock(&ls.lock);
    if (ls.running) lsPhaseLocked(p);
    LightLock_Unlock(&ls.lock);
}

// One step of the thread, under the lock.
LS_CODE static void lsStep(bool force) {
    uint64_t now = svcGetSystemTick();
    uint32_t t = lsTarget(now);
    if (t > LS_CAP) t = LS_CAP;
    if (t > ls.shown) ls.shown += (t - ls.shown + 3) / 4;
    int32_t frame = 0;
    if (ls.frames != nullptr && ls.frameMs > 0) frame = (int32_t) ((lsMs(ls.loadStart, now) / ls.frameMs) % ls.frameCount);
    lsPresent((int32_t) ((uint32_t) ls.fillMax * ls.shown / 1000u), frame, force);
}

LS_CODE static void lsThread(void* arg) {
    (void) arg;
    while (!ls.quit) {
        LightLock_Lock(&ls.lock);
        bool active = ls.running && !ls.paused;
        if (active) lsStep(false);
        LightLock_Unlock(&ls.lock);
        if (active) LightEvent_WaitTimeout(&ls.wake, LS_TICK_NS);
        else LightEvent_Wait(&ls.wake);
    }
}

// ===[ Starting and stopping ]===

LS_CODE static void lsLayout(void) {
    if (!LS_DELTARUNE) {
        // UNDERTALE: a 160 x 14 bar, 3 pixels of frame and 2 of gap.
        ls.barW = 160, ls.barH = 14, ls.frameT = 3;
        ls.barX = (LS_SCREEN_W - ls.barW) / 2, ls.barY = 150;
    } else {
        // DELTARUNE: a 120 x 7 bar, 1 pixel of frame and 2 of gap, 18 pixels under the text, the dog 16 above it.
        ls.barW = 120, ls.barH = 7, ls.frameT = 1;
        ls.barX = (LS_SCREEN_W - ls.barW) / 2;
        int32_t textH = ls.text != nullptr ? (int32_t) ls.textH : 0;
        int32_t dogH = ls.frames != nullptr ? (int32_t) ls.dogH * 2 + 16 : 0;
        int32_t top = ls.frames != nullptr ? (LS_SCREEN_H - (dogH + textH + 18 + ls.barH)) / 2 - 4
                                           : LS_SCREEN_H / 2 - 16 - textH / 2;
        if (ls.text == nullptr && ls.frames == nullptr) top = (LS_SCREEN_H - ls.barH) / 2 - 18;
        ls.dogX = (LS_SCREEN_W - (int32_t) ls.dogW * 2) / 2, ls.dogY = top;
        ls.textX = (LS_SCREEN_W - (int32_t) ls.textW) / 2, ls.textY = top + dogH;
        ls.barY = top + dogH + textH + 18;
    }
    ls.fillX = ls.barX + ls.frameT + 2, ls.fillY = ls.barY + ls.frameT + 2;
    ls.fillMax = ls.barW - 2 * (ls.frameT + 2), ls.fillH = ls.barH - 2 * (ls.frameT + 2);
}

#if LS_DELTARUNE
// loadscreen.bin (tools/aot/loadscreen_art.py): "DTLS", version 1, then the palette, the messages and the dog.
LS_CODE static void lsReadArt(void) {
    CtrRomfs_init(lsRomfsName);
    struct stat st;
    FILE* f = stat(lsArtPath, &st) == 0 && st.st_size > 0 && st.st_size <= LS_ART_MAX ? fopen(lsArtPath, lsReadBinary)
                                                                                       : nullptr;
    if (f == nullptr) {
        ls.artNote = 1;
        return;
    }
    // Kept for the whole run: every load shows it.
    uint8_t* art = malloc((size_t) st.st_size);
    uint32_t size = art != nullptr ? (uint32_t) fread(art, 1, (size_t) st.st_size, f) : 0;
    fclose(f);
    uint16_t h[10];
    if (size < 20 || memcmp(art, lsMagic, 4) != 0) goto bad;
    memcpy(h, art + 4, 16);
    // h: version, messages, frames, dog width, dog height, ms; then colours in the low byte of h[6].
    uint32_t colours = h[6] & 0xFF, at = 20 + ((colours * 3 + 3) & ~3u);
    if (h[0] != 1 || colours > 15 || at > size) goto bad;
    ls.palette = art + 20;
    ls.colours = colours;
    ls.messageCount = h[1];
    for (uint32_t i = 0; ls.messageCount > i; i++) {
        if (at + 8 > size) goto bad;
        uint32_t w = art[at + 2] | (uint32_t) art[at + 3] << 8, hh = art[at + 4] | (uint32_t) art[at + 5] << 8;
        if (w > LS_SCREEN_W || hh > LS_SCREEN_H) goto bad;
        at += 8 + ((((w + 7) / 8) * hh + 3) & ~3u);
    }
    if (at > size) goto bad;
    if (h[2] > 0) {
        if (h[3] == 0 || h[4] == 0 || h[3] > 64 || h[4] > 64 || at + (uint32_t) h[2] * h[3] * h[4] > size) goto bad;
        ls.frames = art + at;
        ls.frameCount = h[2], ls.dogW = h[3], ls.dogH = h[4], ls.frameMs = h[5];
    }
    ls.art = art;
    ls.artSize = size;
    ls.artNote = 2;
    return;
bad:
    ls.palette = ls.frames = nullptr;
    ls.messageCount = 0;
    free(art);
    ls.artNote = 1;
}

// The message for the data.win at path: its chapter's, or the chapter select's for the launcher.
LS_CODE static void lsPickText(const char* path) {
    uint32_t target = 0;
    const char* at = path != nullptr ? strstr(path, lsChapterWord) : nullptr;
    if (at != nullptr) {
        at += sizeof("/chapter") - 1;
        while (*at >= '0' && *at <= '9') target = target * 10 + (uint32_t) (*at++ - '0');
    }
    ls.text = nullptr;
    if (ls.art == nullptr) return;
    uint32_t pos = 20 + ((ls.colours * 3 + 3) & ~3u);
    for (uint32_t i = 0; ls.messageCount > i; i++) {
        const uint8_t* m = ls.art + pos;
        uint32_t w = m[2] | (uint32_t) m[3] << 8, h = m[4] | (uint32_t) m[5] << 8;
        if (m[0] == target) {
            ls.text = m + 8;
            ls.textW = w, ls.textH = h;
            return;
        }
        pos += 8 + ((((w + 7) / 8) * h + 3) & ~3u);
    }
}
#endif

// The first load: settings.ini's game_screen and loading_screen, the art, loadshot.txt, and the thread.
LS_CODE static void lsDecide(void) {
    ls.decided = true;
    const char* why = nullptr;
    FILE* f = fopen(lsSettingsPath, lsRead);
    if (f != nullptr) {
        char line[128], key[64];
        int value;
        while (fgets(line, sizeof(line), f) != nullptr) {
            if (sscanf(line, lsKeyFormat, key, &value) != 2) continue;
            if (strcmp(key, lsGameKey) == 0) ls.firstBelow = value == 1;
            if (strcmp(key, lsOnKey) == 0) why = value == 0 ? lsWhySetting : nullptr;
        }
        fclose(f);
    }
    if (why == nullptr) {
        f = fopen(lsShotsPath, lsRead);
        if (f != nullptr) {
            // Numbers, any separators.
            char text[128];
            while (fgets(text, sizeof(text), f) != nullptr) {
                for (const char* c = text; *c != 0 && LS_SHOTS_MAX > ls.shotCount;) {
                    if (*c < '0' || *c > '9') {
                        c++;
                        continue;
                    }
                    uint32_t ms = 0;
                    while (*c >= '0' && *c <= '9') ms = ms * 10 + (uint32_t) (*c++ - '0');
                    ls.shots[ls.shotCount++] = ms;
                }
            }
            fclose(f);
        }
#if LS_DELTARUNE
        lsReadArt();
#endif
        LightLock_Init(&ls.lock);
        LightEvent_Init(&ls.wake, RESET_ONESHOT);
        s32 priority = 0x30;
        svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        ls.thread = threadCreate(lsThread, nullptr, LS_STACK, priority > 0x19 ? priority - 1 : 0x18, -2, false);
        if (ls.thread == nullptr) why = lsWhyThread;
    }
    if (why != nullptr) {
        ls.why = why;
        return;
    }
    ls.enabled = true;
}

LS_CODE static void lsBegin(uint32_t load, const char* path) {
    if (!ls.decided) lsDecide();
    bool below = ls.firstDone ? CtrScreens_game == 1 : ls.firstBelow;
    if (!ls.enabled || below || ls.running) return;
    LightLock_Lock(&ls.lock);
    ls.load = load;
    ls.loadStart = ls.phaseStart = svcGetSystemTick();
    ls.phase = load == 1 ? P_BOOT : P_HASH;
    memset(ls.actual, 0, sizeof(ls.actual));
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    ls.ratio = isNew ? 600 : 1000;
    ls.sizeBytes = 0;
    ls.hashCalls = 0;
    struct stat st;
    if (path != nullptr && stat(path, &st) == 0) ls.sizeBytes = (uint32_t) st.st_size;
    // Before the first data.win is known: UNDERTALE's, or DELTARUNE's launcher.
    lsBase(ls.sizeBytes != 0 ? ls.sizeBytes : LS_DELTARUNE ? 3u << 20 : 63u << 20);
    ls.shown = 0;
    ls.fillW = -1;
    ls.frame = -1;
    ls.pictures = 0;
    ls.shotNext = 0;
    ls.gen++;
#if LS_DELTARUNE
    lsPickText(path);
#endif
    lsLayout();
    ls.paused = false;
    ls.running = true;
    LightLock_Unlock(&ls.lock);
    LightEvent_Signal(&ls.wake);
}

static uint32_t lsLoads LS_BSS;

// The first data.win of a load: its size and, in DELTARUNE, its message.
LS_CODE static void lsDataWin(const char* path) {
    if (!ls.running) {
        lsBegin(ls.firstDone ? ++lsLoads : 1, path);
        return;
    }
    LightLock_Lock(&ls.lock);
    struct stat st;
    if (path != nullptr && stat(path, &st) == 0 && st.st_size > 0) {
        ls.sizeBytes = (uint32_t) st.st_size;
        lsBase(ls.sizeBytes);
    }
    lsPhaseLocked(P_HASH);
#if LS_DELTARUNE
    lsPickText(path);
    lsLayout();
    ls.gen++;
#endif
    LightLock_Unlock(&ls.lock);
}

LS_CODE static void lsPause(bool paused) {
    if (!ls.running) return;
    LightLock_Lock(&ls.lock);
    ls.paused = paused;
    // Whatever drew meanwhile ("Preparing textures", the renderer's setup) is drawn over in full.
    if (!paused) ls.gen++;
    LightLock_Unlock(&ls.lock);
    if (!paused) LightEvent_Signal(&ls.wake);
}

// The room is ready: the full bar, then the thread idles until the next load.
LS_CODE static void lsFinish(void) {
    if (!ls.running) return;
    LightLock_Lock(&ls.lock);
    lsPhaseLocked(P_COUNT - 1);
    uint64_t now = svcGetSystemTick();
    ls.actual[P_ROOM] = lsMs(ls.phaseStart, now);
    uint32_t before = ls.shown;
    if (!ls.paused) {
        ls.shown = 1000;
        lsPresent(ls.fillMax, ls.frame < 0 ? 0 : ls.frame, true);
    }
    ls.running = false;
    LightLock_Unlock(&ls.lock);
    logInfo(lsLogDone, (unsigned) ls.load, (unsigned) lsMs(ls.loadStart, now), (unsigned) ls.pictures, (unsigned) before,
            (unsigned) ls.actual[P_BOOT], (unsigned) ls.actual[P_HASH], (unsigned) ls.actual[P_PARSE],
            (unsigned) ls.actual[P_PACK], (unsigned) ls.actual[P_VM], (unsigned) ls.actual[P_FS],
            (unsigned) ls.actual[P_GAP], (unsigned) ls.actual[P_RUNNER], (unsigned) ls.actual[P_ROOM]);
}

// The first load's notes: the log wasn't open when they were made.
LS_CODE static void lsNotes(void) {
    if (ls.why != nullptr) logInfo(lsLogOff, ls.why);
#if LS_DELTARUNE
    if (ls.artNote == 1) logInfo(lsLogNoArt);
    if (ls.artNote == 2) logInfo(lsLogArt, (unsigned) ls.messageCount, ls.frames != nullptr ? lsLogDog : lsNothing);
#endif
}

// An error screen or the exit: the thread ends (threads left running can keep the console from leaving).
LS_CODE static void lsQuit(void) {
    if (ls.thread == nullptr) return;
    LightLock_Lock(&ls.lock);
    ls.running = false;
    ls.quit = true;
    LightLock_Unlock(&ls.lock);
    LightEvent_Signal(&ls.wake);
    threadJoin(ls.thread, U64_MAX);
    threadFree(ls.thread);
    ls.thread = nullptr;
    ls.enabled = false;
}

// ===[ The wraps ]===

bool __real_C3D_Init(size_t cmdBufSize);
bool __wrap_C3D_Init(size_t cmdBufSize);
void __real_CtrPack_setDataWinPath(const char* path);
void __wrap_CtrPack_setDataWinPath(const char* path);
DataWin* __real_DataWin_parse(const char* filePath, DataWinParserOptions options);
DataWin* __wrap_DataWin_parse(const char* filePath, DataWinParserOptions options);
void __real_CtrPackScreen_run(const DataWin* dataWin);
void __wrap_CtrPackScreen_run(const DataWin* dataWin); __attribute__((noinline, noipa, section(".text.late.packscreen"))) static void lsPackRun(const DataWin* dataWin);
VMContext* __real_VM_create(DataWin* dataWin);
VMContext* __wrap_VM_create(DataWin* dataWin);
FileSystem* __real_SandboxFileSystem_create(FileSystem* real, FileSystem* bundleOnly);
FileSystem* __wrap_SandboxFileSystem_create(FileSystem* real, FileSystem* bundleOnly);
void __real_CtrPacer_loadStress(const char* path);
void __wrap_CtrPacer_loadStress(const char* path);
void __real_Runner_initFirstRoom(Runner* runner);
void __wrap_Runner_initFirstRoom(Runner* runner);
extern const char ctrSdLegacyDir[];
int CtrSdMigrate_run(const char* legacyParent, int mode);

LS_CODE bool __wrap_C3D_Init(size_t cmdBufSize) {
    // The retired SD folder move (z_sdmigrate.c): a stub since 2026-10-27, kept so this code does not move.
    CtrSdMigrate_run(ctrSdLegacyDir, 0777);
    bool ok = __real_C3D_Init(cmdBufSize);
    lsLoads = 1;
    lsBegin(1, nullptr);
    return ok;
}

LS_CODE void __wrap_CtrPack_setDataWinPath(const char* path) {
    lsDataWin(path);
    __real_CtrPack_setDataWinPath(path);
}

LS_CODE static void lsHashProgress(void) {
    ls.hashCalls++;
    if (ls.progress != nullptr) ls.progress();
}

// A link of CtrDataVersion_check's wrap chain (its order: ctr_wrap_chain.h).
LS_CODE CtrDataVersionResult CTR_DATAVERSION_LOADSCREEN(const char* path, const char* game, const char* part,
                                                         const char* cachePath, void (*progress)(void), bool* cached,
                                                         char digest[65]) {
    if (!ls.running) return CTR_DATAVERSION_AFTER_LOADSCREEN(path, game, part, cachePath, progress, cached, digest);
    ls.progress = progress;
    ls.hashCalls = 0;
    CtrDataVersionResult result = CTR_DATAVERSION_AFTER_LOADSCREEN(path, game, part, cachePath, lsHashProgress, cached, digest);
    ls.progress = nullptr;
    return result;
}

LS_CODE DataWin* __wrap_DataWin_parse(const char* filePath, DataWinParserOptions options) {
    lsPhase(P_PARSE);
    DataWin* dataWin = __real_DataWin_parse(filePath, options);
    lsPhase(P_PACK);
    return dataWin;
}

LS_CODE void __wrap_CtrPackScreen_run(const DataWin* dataWin) {
    lsPause(true);
    lsPackRun(dataWin);
    lsPhase(P_VM);
    lsPause(false);
}

LS_CODE VMContext* __wrap_VM_create(DataWin* dataWin) {
    VMContext* vm = __real_VM_create(dataWin);
    lsPhase(P_FS);
    return vm;
}

LS_CODE FileSystem* __wrap_SandboxFileSystem_create(FileSystem* real, FileSystem* bundleOnly) {
    FileSystem* fs = __real_SandboxFileSystem_create(real, bundleOnly);
    lsPhase(P_GAP);
    lsPause(true);
    return fs;
}

LS_CODE void __wrap_CtrPacer_loadStress(const char* path) {
    lsPhase(P_RUNNER);
    lsPause(false);
    __real_CtrPacer_loadStress(path);
}

LS_CODE void __wrap_Runner_initFirstRoom(Runner* runner) {
    lsPhase(P_ROOM);
    __real_Runner_initFirstRoom(runner);
    lsFinish();
    if (!ls.firstDone) lsNotes();
    ls.firstDone = true;
}

// A link of CtrWatchdog_stop's wrap chain (its order: ctr_wrap_chain.h).
LS_CODE void CTR_WATCHDOGSTOP_LOADSCREEN(void) {
    lsQuit();
    CTR_WATCHDOGSTOP_AFTER_LOADSCREEN();
}

#if LS_DELTARUNE
// ===[ The dog on its rope (dogcheck) ]===
//
// In Chapter 3 the dog (the game's own dog object) hangs from a line: Step puts it at
// lengthdir_x/y(length, angle) from an anchor far above, turned along the line, and Draw draws the line
// (draw_line_width, its colour and width) and then the dog. loadscreen_art.py bakes those facts into a trailer
// ("DSW1") after the frames, and the header's frame time is then the drawing tick, so lsStep's frame number changes
// every tick and the dog is redrawn as it swings; the sprite's own frame time is in the trailer. Here the dog hangs
// from the same length of line, its anchor straight above it and off the screen, so the line comes in from the top
// edge. The game sweeps the angle one way at its anglechange a frame; a 1200-pixel line through that arc would carry
// the dog off a 400-pixel screen, so the dog swings like a pendulum instead: 6 degrees each side, at the game's
// angular speed at the bottom of the swing (0.75 degrees a frame at 30 frames a second: a 1.68 s period). It turns
// along the line as in the game (image_angle = the line's direction - 90). The swing rises at most
// length * (1 - cos 6) = 7 pixels and stays above the text (lsLayout leaves the dog's block 2 * height + 16 high).
// By default the dog hangs still, where main draws it, and the line comes straight down
// from the top edge; the swing is an easter egg on one load in 1000, decided at the load's first picture from the tick
// the load started at (never the game's RNG or state), and never in a test run (benchrun.txt, --bench or
// loadshot.txt) unless settings.ini says loaddog = 2. loaddog = 0 draws main's picture (no line, still dog).
// Without the trailer the dog stands as before, unturned and without a line.
//
// Layout: in DELTARUNE builds lsDrawDog calls lsDogDraw from inline assembly, with the canvas in r0-r3 by value
// (bpp and frame share r3), and branches over padding so its slot keeps main's size. GCC orders a file's functions
// by its call graph: a C call from lsDrawDog, or a canvas pointer passed out of it, moved or reshaped the other
// loadscreen functions. The asm call is invisible to the call graph, and the
// never-taken lsRect call (frame is never negative) keeps lsDrawDog's one edge as on main, so every existing
// symbol keeps its address and size. lsDogDraw is `used` (only the asm names it) and noipa (its ABI must not
// change). UNDERTALE builds none of this. The float constants sit in literal pools in .text.late.dogcheck; nothing
// is added to .rodata.

#include <math.h>

#define LS_DOG __attribute__((noinline, section(".text.late.dogcheck")))
#define LS_DOG_BSS __attribute__((section(".bss.late.dogcheck")))
#define LS_DOG_TRAILER 20u
#define LS_DOG_SWING 6.0f
#define LS_DOG_BOXES 4

typedef struct {
    const u8* fb;
    uint32_t gen;            // ls.gen when drawn: a new generation has redrawn the buffer in full
    int16_t x0, y0, x1, y1; // what the rope and dog covered in this buffer (x1 and y1 past the end)
} LsDogBox;

static LsDogBox lsDogBoxes[LS_DOG_BOXES] LS_DOG_BSS;
static uint32_t lsDogNext LS_DOG_BSS;
// settings.ini's loaddog (read on the first picture): 0 no line, 1 (default) the still dog on its line and, on one
// load in 1000, the swing, 2 always the swing. lsDogTest: a test run (benchrun.txt or --bench, or loadshot.txt), where
// the swing comes only from loaddog = 2. lsDogStart: the load the swing was decided for.
static int32_t lsDogMode LS_DOG_BSS;
static bool lsDogRead LS_DOG_BSS, lsDogTest LS_DOG_BSS, lsDogSwings LS_DOG_BSS;
static u64 lsDogStart LS_DOG_BSS;
bool CtrScreens_benchLaunched(void); // main.c

// settings.ini's loaddog, once. The key is matched as two words so that no string is added to .rodata.
LS_DOG static void lsDogSettings(void) {
    lsDogRead = true;
    lsDogMode = 1;
    lsDogTest = CtrScreens_benchLaunched() || ls.shotCount > 0;
    FILE* f = fopen(lsSettingsPath, lsRead);
    if (f == nullptr) return;
    char line[128], key[64];
    int value;
    while (fgets(line, sizeof(line), f) != nullptr) {
        if (sscanf(line, lsKeyFormat, key, &value) != 2) continue;
        uint32_t k0 = (u8) key[0] | (uint32_t) (u8) key[1] << 8 | (uint32_t) (u8) key[2] << 16 | (uint32_t) (u8) key[3] << 24;
        if (k0 != 0x64616F6Cu || key[4] != 'd' || key[5] != 'o' || key[6] != 'g' || key[7] != 0) continue; // "load" "dog"
        lsDogMode = value >= 0 && value <= 2 ? value : 1;
    }
    fclose(f);
}

LS_DOG static void lsDogRect(const LsCanvas* c, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb) {
    u8 r = (u8) (rgb >> 16), g = (u8) (rgb >> 8), b = (u8) rgb;
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > LS_SCREEN_W) w = LS_SCREEN_W - x;
    if (y + h > LS_SCREEN_H) h = LS_SCREEN_H - y;
    if (w <= 0 || h <= 0) return;
    for (int32_t sx = x * c->scale; (x + w) * c->scale > sx; sx++) {
        u8* p = c->fb + ((size_t) sx * c->rows + (size_t) (c->rows - y - h)) * c->bpp;
        for (int32_t i = 0; h > i; i++, p += c->bpp) {
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

LS_DOG static int32_t lsDogFloor(float v) {
    int32_t i = (int32_t) v;
    return (float) i > v ? i - 1 : i;
}

LS_DOG __attribute__((used, noipa)) static void lsDogDraw(u8* fb, uint32_t rows, int32_t scale, uint32_t bppFrame) {
    const LsCanvas canvas = { fb, (u16) rows, scale, bppFrame >> 16 }, *c = &canvas;
    int32_t frame = (int32_t) (bppFrame & 0xFFFFu);
    if (ls.frames == nullptr || ls.frameCount == 0) return;
    const uint8_t* t = ls.artSize >= 20 + LS_DOG_TRAILER ? ls.art + ls.artSize - LS_DOG_TRAILER : nullptr;
    bool trailer = t != nullptr && t[0] == 'D' && t[1] == 'S' && t[2] == 'W' && t[3] == '1';
    if (!lsDogRead) lsDogSettings();
    // Once a load, at its first picture: the swing on one load in 1000, by the tick the load started at (no game state,
    // no game RNG). Never in a test run unless loaddog = 2.
    if (lsDogStart != ls.loadStart) {
        lsDogStart = ls.loadStart;
        lsDogSwings = lsDogMode == 2 || (lsDogMode == 1 && !lsDogTest && (uint32_t) ls.loadStart % 1000u == 0);
    }
    bool rope = trailer && lsDogMode != 0;
    int32_t w = (int32_t) ls.dogW, h = (int32_t) ls.dogH;
    int32_t ox = rope ? t[4] : w / 2, oy = rope ? t[5] : h / 2;
    float si = 0.0f, co = 1.0f, length = 0.0f;
    uint32_t ropeRgb = 0;
    int32_t ropeW = 0;
    if (trailer) {
        uint32_t animMs = t[6] | (uint32_t) t[7] << 8, speed = t[16] | (uint32_t) t[17] << 8;
        ropeRgb = t[8] | (uint32_t) t[9] << 8 | (uint32_t) t[10] << 16;
        ropeW = t[12] | (int32_t) t[13] << 8;
        length = (float) (t[14] | (uint32_t) t[15] << 8);
        uint32_t ms = (uint32_t) ((svcGetSystemTick() - ls.loadStart) / LS_TICKS_PER_MS);
        frame = animMs > 0 ? (int32_t) (ms / animMs % ls.frameCount) : 0;
        // A pendulum whose speed at the bottom is the game's anglechange (hundredths of a degree a frame, 30 a second).
        uint32_t period = speed > 0 ? (uint32_t) (6.2831853f * LS_DOG_SWING * 100000.0f / ((float) speed * 30.0f)) : 0;
        if (period > 0 && rope && lsDogSwings) {
            float theta = LS_DOG_SWING * 0.017453293f * sinf(6.2831853f * (float) (ms % period) / (float) period);
            si = sinf(theta), co = cosf(theta);
        }
    }
    if (frame < 0 || (uint32_t) frame >= ls.frameCount) frame = 0;
    // The pivot (the sprite's origin, where the line ends): straight under the anchor when the dog hangs still.
    int32_t px0 = ls.dogX + 2 * ox, py0 = ls.dogY + 2 * oy;
    float px = (float) px0 + length * si, py = (float) py0 - length * (1.0f - co);
    // The farthest a pixel of the turned dog gets from the pivot.
    int32_t dx = 2 * (ox > w - ox ? ox : w - ox), dy = 2 * (oy > h - oy ? oy : h - oy), reach = 1;
    while (reach * reach < dx * dx + dy * dy) reach++;
    reach++;
    int32_t bottom = ls.text != nullptr ? ls.textY - 2 : LS_SCREEN_H;
    // This buffer still shows what was drawn into it two pictures ago: clear that first.
    LsDogBox* box = nullptr;
    for (uint32_t i = 0; LS_DOG_BOXES > i; i++)
        if (lsDogBoxes[i].fb == c->fb) box = &lsDogBoxes[i];
    if (box == nullptr) {
        box = &lsDogBoxes[lsDogNext++ % LS_DOG_BOXES];
        box->fb = c->fb;
    } else if (box->gen == ls.gen) {
        lsDogRect(c, box->x0, box->y0, box->x1 - box->x0, box->y1 - box->y0, 0);
    }
    int32_t x0 = lsDogFloor(px) - reach, x1 = lsDogFloor(px) + reach + 1;
    int32_t y0 = rope ? 0 : lsDogFloor(py) - reach, y1 = lsDogFloor(py) + reach + 1;
    if (y1 > bottom) y1 = bottom;
    // The line, from the top edge down to the pivot (the dog covers its end).
    if (rope && ropeW > 0) {
        float ay = py - length * co, ax = px - length * si;
        for (int32_t y = 0; (float) y < py && bottom > y; y++) {
            float cx = ax + ((float) y + 0.5f - ay) / (py - ay) * (px - ax);
            int32_t sx = lsDogFloor(cx - (float) ropeW * 0.5f + 0.5f);
            lsDogRect(c, sx, y, ropeW, 1, ropeRgb);
            if (sx < x0) x0 = sx;
            if (sx + ropeW > x1) x1 = sx + ropeW;
        }
    }
    // The dog at twice its size, turned with the line: each screen pixel takes the sprite pixel under it.
    const uint8_t* f = ls.frames + (size_t) frame * ls.dogW * ls.dogH;
    for (int32_t y = lsDogFloor(py) - reach; lsDogFloor(py) + reach >= y; y++) {
        if (y >= bottom) break;
        for (int32_t x = lsDogFloor(px) - reach; lsDogFloor(px) + reach >= x; x++) {
            float rx = (float) x + 0.5f - px, ry = (float) y + 0.5f - py;
            float u = (rx * co - ry * si) * 0.5f + (float) ox, v = (rx * si + ry * co) * 0.5f + (float) oy;
            if (u < 0.0f || v < 0.0f) continue;
            int32_t sx = (int32_t) u, sy = (int32_t) v;
            if (sx >= w || sy >= h) continue;
            uint32_t i = f[sy * w + sx];
            if (i == 0 || i > ls.colours) continue;
            const uint8_t* p = ls.palette + (i - 1) * 3;
            lsDogRect(c, x, y, 1, 1, (uint32_t) p[0] << 16 | (uint32_t) p[1] << 8 | p[2]);
        }
    }
    box->gen = ls.gen;
    box->x0 = (int16_t) x0, box->y0 = (int16_t) y0, box->x1 = (int16_t) x1, box->y1 = (int16_t) y1;
}
#endif
#ifdef ENABLE_WAD16
// Reserved for the deferred chapter preflight. DR's Policy E build emits no new hooks.
// Chapter preflight is not a chapter load. Keep its progress separate from the data-version wrap
// chain, whose room-plan link owns the currently loaded chapter's plan until that chapter unloads.
void CtrLoadscreen_chapterHashesBegin(uint32_t size);
__attribute__((noinline, section(".text.late.zzzz_builderxreview"))) void CtrLoadscreen_chapterHashesBegin(uint32_t size) {
    if (!ls.running) return;
    LightLock_Lock(&ls.lock);
    ls.sizeBytes = size;
    ls.hashCalls = 0;
    lsBase(size);
    lsPhaseLocked(P_HASH);
    LightLock_Unlock(&ls.lock);
}

void CtrLoadscreen_chapterHashProgress(void);
__attribute__((noinline, section(".text.late.zzzz_builderxreview"))) void CtrLoadscreen_chapterHashProgress(void) {
    if (ls.running) ls.hashCalls++;
}
#endif

// ===[ First time setup inside the loading screen ]===
//
// A data.win whose texture pages are not all in the SD card's cache yet (its first start, a pack-format change, a
// damaged or unfinished cache: docs/3ds-hardware.md, "The texture page cache survives build switches") has them
// prepared before its game starts. While the loading screen runs, that now happens inside it (design "One
// line"): the loading screen as it is, its bar showing the preparation's real progress, and under the bar one line in
// the game's own font, "First time setup. Please wait.": UNDERTALE's fnt_maintext, DELTARUNE's fnt_main
// under its INITIALIZING CHAPTER text and dog. The line is cut from the player's files when the game is built
// (tools/aot/loadscreen_art.py: the message for target 255 in loadscreen.bin; UNDERTALE's file holds only that line).
// Without it the bar shows alone. B still starts the game at once and leaves the rest to the background packing, but
// the screen no longer offers it: each page not prepared yet stalls the game when first drawn. With the loading screen
// off (loading_screen = 0, or the game on the bottom screen) ctr_texture_pack_screen.c's console-font screen runs as
// before.
//
// The preparation is CtrPackScreen_run's, with the same log lines. Its loop runs on the game thread, which draws the
// screen itself, under the loading screen's lock, while __wrap_CtrPackScreen_run keeps that screen's thread paused (its
// resume redraws everything, and its bar holds where the preparation left it). Like the thread, it draws
// only what changed, at most every 33 ms.
//
// Layout: new code in .text.late.packscreen, its constants in the late text block (.rodata.packscreen_in_text), its
// statics in .bss.late.packscreen. The loading screen's own drawing functions are copied rather than called, so that
// GCC's view of them stays as on main; only lsDogDraw (noipa, DELTARUNE) is called, from inline assembly as lsDrawDog
// calls it. __wrap_CtrPackScreen_run's call names lsPackRun instead of __real_CtrPackScreen_run, at the same place
// and size.

#include <malloc.h>

#include "ctr_log.h"
#include "ctr_qoi.h"
#include "ctr_texture_pack.h"
#include "ctr_watchdog.h"
#include "gettime.h"

#define PS_CODE __attribute__((noinline, noipa, section(".text.late.packscreen")))
#define PS_RO __attribute__((section(".rodata.packscreen_in_text")))
#define PS_BSS __attribute__((section(".bss.late.packscreen")))
#define PS_STR(name, text) static const char name[(sizeof(text) + 3u) & ~3u] PS_RO = text
#define PS_LINE_TARGET 255u
#define PS_LINE_GAP 12
#define PS_PAGE_SLACK (128u * 1024u)

PS_STR(psWatchPhase, "preparing textures");
PS_STR(psNothing, "");
PS_STR(psLogThreads, "Textures: preparing texture pages on %u threads\n");
PS_STR(psLogPage, "Prepared page %u: %u KB, %.0f+%.0f ms (read %.0f, inflate %.0f, convert %.0f, compress %.0f, write %.0f) "
                  "thread %d core %d at %.1f s\n");
PS_STR(psLogWrote, "  wrote %u KB %s in %u calls (%.0f-%.0f ms each), %.2f MB/s: open %.0f, size %.0f, data %.0f, flush %.0f, "
                   "close %.0f, rename %.0f ms; waited %.0f ms\n");
PS_STR(psStreamed, "streamed");
PS_STR(psStaged, "whole, aligned");
PS_STR(psWhole, "whole");
PS_STR(psLogPublication, "  publication %s: delete %.2f ms, rename %.2f ms\n");
PS_STR(psSdArchive, "SD archive");
PS_STR(psStdio, "stdio");
PS_STR(psLogReady, "Textures: %u of %u pages ready (%u packed now) in %.1f s%s; heap peak %.1f MB, %u waits for memory, %u pages "
                   "left to the background\n");
PS_STR(psSkipped, ", the rest in the background");
PS_STR(psLogWritten, "Textures: wrote %.1f MB of cache files in %.1f s on the SD card, %.2f MB/s; threads waited %.1f s to write\n");
PS_STR(psLogScreen, "Loading screen: first time setup with %s, %u pictures\n");
PS_STR(psWithLine, "its line");
PS_STR(psWithoutLine, "the bar alone (no setup line in loadscreen.bin)");
PS_STR(psMagic, "DTLS");
#if !LS_DELTARUNE
PS_STR(psRomfsName, "romfs");
PS_STR(psArtPath, "romfs:/loadscreen.bin");
PS_STR(psReadBinary, "rb");
#endif

// The setup line's bits (in loadscreen.bin), and UNDERTALE's loadscreen.bin, read for the setup and freed after it.
static const uint8_t* psLine PS_BSS;
static uint32_t psLineW PS_BSS, psLineH PS_BSS;
static uint8_t* psOwned PS_BSS;

// The message for target 255 in a loadscreen.bin (its format: lsReadArt).
PS_CODE static void psFindLine(const uint8_t* art, uint32_t size) {
    psLine = nullptr;
    if (art == nullptr || size < 20 || memcmp(art, psMagic, 4) != 0 || (art[4] | art[5] << 8) != 1) return;
    uint32_t colours = art[16], count = art[6] | (uint32_t) art[7] << 8, at = 20 + ((colours * 3 + 3) & ~3u);
    for (uint32_t i = 0; count > i && size >= at + 8; i++) {
        uint32_t w = art[at + 2] | (uint32_t) art[at + 3] << 8, h = art[at + 4] | (uint32_t) art[at + 5] << 8;
        uint32_t bytes = (w + 7) / 8 * h;
        if (w > LS_SCREEN_W || h > LS_SCREEN_H || at + 8 + bytes > size) return;
        if (art[at] == PS_LINE_TARGET && w > 0 && h > 0) {
            psLine = art + at + 8;
            psLineW = w, psLineH = h;
            return;
        }
        at += 8 + ((bytes + 3) & ~3u);
    }
}

#if !LS_DELTARUNE
// UNDERTALE reads no loading screen art otherwise: its loadscreen.bin holds only the setup line.
PS_CODE static void psReadArt(void) {
    CtrRomfs_init(psRomfsName);
    struct stat st;
    if (stat(psArtPath, &st) != 0 || st.st_size < 20 || st.st_size > (off_t) LS_ART_MAX) return;
    FILE* f = fopen(psArtPath, psReadBinary);
    if (f == nullptr) return;
    psOwned = malloc((size_t) st.st_size);
    uint32_t size = psOwned != nullptr ? (uint32_t) fread(psOwned, 1, (size_t) st.st_size, f) : 0;
    fclose(f);
    psFindLine(psOwned, size);
}
#endif

// lsRect's copy.
PS_CODE static void psRect(const LsCanvas* c, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb) {
    u8 r = (u8) (rgb >> 16), g = (u8) (rgb >> 8), b = (u8) rgb;
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > LS_SCREEN_W) w = LS_SCREEN_W - x;
    if (y + h > LS_SCREEN_H) h = LS_SCREEN_H - y;
    if (w <= 0 || h <= 0) return;
    for (int32_t sx = x * c->scale; (x + w) * c->scale > sx; sx++) {
        u8* p = c->fb + ((size_t) sx * c->rows + (size_t) (c->rows - y - h)) * c->bpp;
        for (int32_t i = 0; h > i; i++, p += c->bpp) {
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

// 1-bit text, white, its top left at (x0, y0) (lsDrawText's way).
PS_CODE static void psBits(const LsCanvas* c, const uint8_t* bits, uint32_t w, uint32_t h, int32_t x0, int32_t y0) {
    uint32_t stride = (w + 7) / 8;
    for (uint32_t y = 0; h > y; y++) {
        const uint8_t* row = bits + y * stride;
        for (uint32_t x = 0; w > x; x++) {
            if (row[x >> 3] & (0x80u >> (x & 7))) psRect(c, x0 + (int32_t) x, y0 + (int32_t) y, 1, 1, 0xFFFFFFu);
        }
    }
}

// lsDrawEye with the setup line under the bar.
PS_CODE static void psDrawEye(gfx3dSide_t side, bool full, int32_t fillW, int32_t frame, LsCanvas* out) {
    u16 width = 0, height = 0;
    GSPGPU_FramebufferFormat format = gfxGetScreenFormat(GFX_TOP);
    LsCanvas c = { gfxGetFramebuffer(GFX_TOP, side, &width, &height), width, height >= 2 * LS_SCREEN_W ? 2 : 1,
                   format == GSP_RGBA8_OES ? 4u : 3u };
    if (full) {
        size_t pixels = (size_t) LS_SCREEN_W * (size_t) c.scale * c.rows;
        if (c.bpp == 3) {
            memset(c.fb, 0, pixels * 3);
        } else {
            uint32_t* p = (uint32_t*) c.fb;
            for (size_t i = 0; pixels > i; i++) p[i] = 0xFFu;
        }
        if (ls.text != nullptr) psBits(&c, ls.text, ls.textW, ls.textH, ls.textX, ls.textY);
        psRect(&c, ls.barX, ls.barY, ls.barW, ls.barH, 0xFFFFFFu);
        psRect(&c, ls.barX + ls.frameT, ls.barY + ls.frameT, ls.barW - 2 * ls.frameT, ls.barH - 2 * ls.frameT, 0);
        if (psLine != nullptr)
            psBits(&c, psLine, psLineW, psLineH, (LS_SCREEN_W - (int32_t) psLineW) / 2, ls.barY + ls.barH + PS_LINE_GAP);
    }
    psRect(&c, ls.fillX, ls.fillY, fillW, ls.fillH, 0xFFFFFFu);
    psRect(&c, ls.fillX + fillW, ls.fillY, ls.fillMax - fillW, ls.fillH, 0);
#if LS_DELTARUNE
    // From inline assembly, as lsDrawDog does: a C call would be a call graph edge main does not have, and GCC then
    // orders the loading screen's existing functions differently (the __wrap_ functions moved by 44-180 B).
    register u8* a0 __asm__("r0") = c.fb;
    register uint32_t a1 __asm__("r1") = c.rows;
    register int32_t a2 __asm__("r2") = c.scale;
    register uint32_t a3 __asm__("r3") = c.bpp << 16 | (uint32_t) frame;
    __asm__ volatile("bl lsDogDraw" : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3) : : "ip", "lr", "memory", "cc", "d0", "d1",
                     "d2", "d3", "d4", "d5", "d6", "d7");
#else
    (void) frame;
#endif
    GSPGPU_FlushDataCache(c.fb, (u32) width * height * c.bpp);
    if (out != nullptr) *out = c;
}

PS_CODE static void psPut32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16);
    p[3] = (uint8_t) (v >> 24);
}

// lsSaveShot's copy: the loadshot.txt picture as a 400 x 240 BMP.
PS_CODE static void psSaveShot(const LsCanvas* c, uint32_t ms) {
    char name[96];
    snprintf(name, sizeof(name), lsShotFormat, (unsigned) ls.load, (unsigned) ms);
    FILE* f = fopen(name, lsWriteBinary);
    if (f == nullptr) return;
    uint8_t* row = malloc(LS_SCREEN_W * 3 + 54);
    if (row != nullptr) {
        uint32_t data = LS_SCREEN_W * 3 * LS_SCREEN_H;
        memset(row, 0, 54);
        row[0] = 'B';
        row[1] = 'M';
        psPut32(row + 2, data + 54);
        psPut32(row + 10, 54);
        psPut32(row + 14, 40);
        psPut32(row + 18, LS_SCREEN_W);
        psPut32(row + 22, LS_SCREEN_H);
        row[26] = 1;
        row[28] = 24;
        psPut32(row + 34, data);
        fwrite(row, 1, 54, f);
        for (int32_t y = LS_SCREEN_H - 1; y >= 0; y--) {
            for (int32_t x = 0; LS_SCREEN_W > x; x++) {
                const u8* p = c->fb + ((size_t) x * c->scale * c->rows + (size_t) (c->rows - 1 - y)) * c->bpp;
                u8* q = row + x * 3;
                q[0] = p[c->bpp == 4 ? 1 : 0];
                q[1] = p[c->bpp == 4 ? 2 : 1];
                q[2] = p[c->bpp == 4 ? 3 : 2];
            }
            fwrite(row, 1, LS_SCREEN_W * 3, f);
        }
        free(row);
    }
    fclose(f);
}

// The picture's key for the dog: lsDogDraw's own frame (the trailer's animation time, 133 ms for Chapter 3's dog; every
// 33 ms while it swings), so that the game thread draws only when the dog changes. Each picture costs the core that
// texture worker 0 shares about 1 ms on Old 3DS (Azahar: 907 pictures at 33 ms made DELTARUNE Chapter 3's preparation
// 0.85 s slower than the console-font screen). Under the lock.
PS_CODE static int32_t psDogKey(uint32_t ms) {
#if LS_DELTARUNE
    if (ls.frames == nullptr || ls.frameCount == 0) return 0;
    const uint8_t* t = ls.artSize >= 20 + LS_DOG_TRAILER ? ls.art + ls.artSize - LS_DOG_TRAILER : nullptr;
    uint32_t animMs = ls.frameMs;
    if (t != nullptr && t[0] == 'D' && t[1] == 'S' && t[2] == 'W' && t[3] == '1') {
        // lsDogSwings by its address from inline assembly: a C reference reorders GCC's statics (.bss moves).
        const volatile bool* swings;
        __asm__("ldr %0, 1f\n\tb 2f\n\t.p2align 2\n1:\t.word lsDogSwings\n2:" : "=r"(swings));
        if (*swings) return (int32_t) (ms / 33u);
        animMs = t[6] | (uint32_t) t[7] << 8;
    }
    return animMs > 0 ? (int32_t) (ms / animMs % ls.frameCount) : 0;
#else
    (void) ms;
    return 0;
#endif
}

// lsPresent's way: what changed, into the back buffer, then shown. Under the lock, the thread paused.
PS_CODE static void psPresent(int32_t fillW, int32_t frame) {
    GSPGPU_FramebufferFormat format = gfxGetScreenFormat(GFX_TOP);
    if (format != GSP_BGR8_OES && format != GSP_RGBA8_OES) return;
    u16 width = 0, height = 0;
    u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &width, &height);
    bool stereo = gfxIs3D();
    if (fb == nullptr || width != LS_SCREEN_H || height < LS_SCREEN_W) return;
    uint32_t key = (uint32_t) format | (uint32_t) height << 4 | (uint32_t) stereo << 16;
    if (key != ls.key) {
        ls.key = key;
        ls.gen++;
    }
    int32_t slot = ls.drawnFb[0] == fb ? 0 : ls.drawnFb[1] == fb ? 1 : -1;
    bool full = slot < 0 || ls.drawnGen[slot] != ls.gen;
    if (!full && fillW == ls.fillW && frame == ls.frame) return;
    if (slot < 0) {
        slot = ls.drawnFb[0] == nullptr ? 0 : 1;
        if (ls.drawnFb[0] != nullptr && ls.drawnFb[1] != nullptr) slot = 0;
    }
    LsCanvas shot;
    psDrawEye(GFX_LEFT, full, fillW, frame, &shot);
    if (stereo) psDrawEye(GFX_RIGHT, full, fillW, frame, nullptr);
    ls.drawnFb[slot] = fb;
    ls.drawnGen[slot] = ls.gen;
    ls.fillW = fillW;
    ls.frame = frame;
    ls.pictures++;
    if (ls.shotNext < ls.shotCount) {
        uint32_t ms = (uint32_t) ((svcGetSystemTick() - ls.loadStart) / LS_TICKS_PER_MS);
        if (ms >= ls.shots[ls.shotNext]) {
            psSaveShot(&shot, ls.shots[ls.shotNext]);
            ls.shotNext++;
        }
    }
    __real_gfxScreenSwapBuffers(GFX_TOP, stereo);
}

// __wrap_CtrPackScreen_run's call, the loading screen's thread paused: CtrPackScreen_run inside the loading screen.
PS_CODE static void lsPackRun(const DataWin* dataWin) {
    if (!ls.running) {
        __real_CtrPackScreen_run(dataWin);
        return;
    }
    CtrPack_init();
    CtrPackKey key = CtrPack_keyFor(dataWin);
    if (CtrPack_isComplete(&key, dataWin->txtr.count)) return;
    CtrPackWorker* worker = CtrPackWorker_start(dataWin, &key, true);
    if (worker == nullptr) return;

    uint64_t start = nowNanos();
    // The bar: source bytes and a fixed allowance per page, as CtrPackScreen_run's estimate weighs the work. It is the
    // setup's own bar, from empty (when the line appears) to LS_CAP, and ls.shown is left there: it only grows, so the
    // load's bar then holds at LS_CAP and is full at the end. (Going on from the load's bar instead left the setup
    // nothing to show after a first data.win hash, whose time had already taken that bar past 90%.)
    uint64_t totalWork = 0, finishedWork = 0;
    for (uint32_t i = 0; i < dataWin->txtr.count; i++) {
        const Texture* texture = &dataWin->txtr.textures[i];
        if (texture->externalPath == nullptr && texture->blobOffset > 0 && texture->blobSize > 0)
            totalWork += (uint64_t) texture->blobSize + PS_PAGE_SLACK;
    }
    uint32_t done = 0, total = 0, built = 0, pictures = ls.pictures;
    bool skipped = false;
    double bytesWritten = 0, writingMs = 0, waitingMs = 0;
#if LS_DELTARUNE
    psFindLine(ls.art, ls.artSize);
#else
    psReadArt();
#endif
    bool line = psLine != nullptr;
    logInfo(psLogThreads, (unsigned) CtrPackWorker_threadCount(worker));
    CtrLog_flush();
    LightLock_Lock(&ls.lock);
    ls.gen++;
    uint32_t barAt = 0;
    LightLock_Unlock(&ls.lock);
    u64 lastDisplay = 0;
    while (aptMainLoop()) {
        CtrWatchdog_mark(psWatchPhase);
        hidScanInput();
        if (hidKeysDown() & KEY_B) {
            skipped = true;
            break;
        }
        CtrPackResult result;
        while (CtrPackWorker_poll(worker, &result)) {
            finishedWork += (uint64_t) dataWin->txtr.textures[result.pageId].blobSize + PS_PAGE_SLACK;
            if (result.built) {
                built++;
                logInfo(psLogPage, (unsigned) result.pageId, (unsigned) (result.size / 1024), (double) result.decodeMs,
                        (double) result.packMs, (double) result.readMs, (double) result.inflateMs, (double) result.convertMs,
                        (double) result.compressMs, (double) result.writeMs, (int) result.thread, (int) result.core,
                        (double) (nowNanos() - start) / 1e9);
                const CtrPackWriteStats* io = &result.io;
                double ms = (double) (io->openMs + io->sizeMs + io->dataMs + io->flushMs + io->closeMs + io->renameMs);
                logInfo(psLogWrote, (unsigned) (result.size / 1024), io->streamed ? psStreamed : io->staged ? psStaged : psWhole,
                        (unsigned) io->calls, (double) io->callMinMs, (double) io->callMaxMs,
                        ms > 0 ? (double) result.size / (1024.0 * 1024.0) / (ms / 1000.0) : 0.0, (double) io->openMs,
                        (double) io->sizeMs, (double) io->dataMs, (double) io->flushMs, (double) io->closeMs, (double) io->renameMs,
                        (double) io->waitMs);
                bytesWritten += (double) result.size;
                logInfo(psLogPublication, io->publication.direct ? psSdArchive : psStdio, (double) io->publication.deleteMs,
                        (double) io->publication.renameMs);
                writingMs += ms;
                waitingMs += (double) io->waitMs;
            }
            CtrPacked_free(result.packed);
        }
        CtrPackWorker_progress(worker, &done, &total);
        bool last = done >= total;
        u64 tick = svcGetSystemTick();
        if (last || lastDisplay == 0 || tick - lastDisplay >= 33ull * LS_TICKS_PER_MS) {
            uint64_t share = last || totalWork == 0 ? 1000u : finishedWork * 1000u / totalWork;
            if (share > 1000u) share = 1000u;
            LightLock_Lock(&ls.lock);
            int32_t frame = psDogKey((uint32_t) ((tick - ls.loadStart) / LS_TICKS_PER_MS));
            barAt = (uint32_t) (LS_CAP * share / 1000u);
            psPresent((int32_t) ((uint32_t) ls.fillMax * barAt / 1000u), frame);
            LightLock_Unlock(&ls.lock);
            lastDisplay = tick;
        }
        if (last) break;
        gspWaitForVBlank();
    }
    LightLock_Lock(&ls.lock);
    if (barAt > ls.shown) ls.shown = barAt;
    LightLock_Unlock(&ls.lock);
    uint32_t memoryWaits = 0, deferred = 0;
    CtrPackWorker_memoryStats(worker, &memoryWaits, &deferred);
    CtrPackWorker_stop(worker);
    CtrQoi_releaseWorkspace();
    psLine = nullptr;
    free(psOwned);
    psOwned = nullptr;
    // The heap is never trimmed (ctr_memory.c), so its arena is the peak so far.
    struct mallinfo mi = mallinfo();
    logInfo(psLogReady, (unsigned) (done - deferred), (unsigned) total, (unsigned) built, (double) (nowNanos() - start) / 1e9,
            skipped ? psSkipped : psNothing, (double) mi.arena / (1024.0 * 1024.0), (unsigned) memoryWaits, (unsigned) deferred);
    if (built > 0) {
        logInfo(psLogWritten, bytesWritten / (1024.0 * 1024.0), writingMs / 1000.0,
                writingMs > 0 ? bytesWritten / (1024.0 * 1024.0) / (writingMs / 1000.0) : 0.0, waitingMs / 1000.0);
    }
    logInfo(psLogScreen, line ? psWithLine : psWithoutLine, (unsigned) (ls.pictures - pictures));
}

// DR chapter preflight deliberately bypasses the digest-consumer wrap chain. Cached receipts cause no
// callback and no setup screen. For a real hash, reuse the packscreen line and renderer, without changing
// the active room/crisp/sound digest. One sixth of the bar per launcher/chapter; missing or unselected
// slots are skipped. Within a slot, only completed 64 KiB reads advance it (last read clamped to size).
#if LS_DELTARUNE
#define BH_CODE __attribute__((noinline, section(".text.late.builderxhash")))
#define BH_DATA __attribute__((section(".data.late.builderxhash")))
// Aliases keep the old static call graph/order intact, just as the dog drawer's assembly call does.
__asm__(".global BxHash_pause\n.set BxHash_pause, lsPause\n"
        ".global BxHash_present\n.set BxHash_present, psPresent\n"
        ".global BxHash_dogKey\n.set BxHash_dogKey, psDogKey\n"
        ".global BxHash_findLine\n.set BxHash_findLine, psFindLine\n");
void BxHash_pause(bool paused);
void BxHash_present(int32_t fillW, int32_t frame);
int32_t BxHash_dogKey(uint32_t ms);
void BxHash_findLine(const uint8_t* art, uint32_t size);

static struct {
    const char* path;
    void (*progress)(void);
    uint64_t size, read;
    uint32_t slot, bar;
    int32_t fill;
    bool active;
} bh BH_DATA;

BH_CODE static void bhProgress(void) {
    if (bh.progress != nullptr) bh.progress();
    if (!ls.running) return;
    if (!bh.active) {
        struct stat st;
        if (stat(bh.path, &st) != 0 || st.st_size <= 0) return;
        bh.size = (uint64_t) st.st_size;
        BxHash_pause(true);
        LightLock_Lock(&ls.lock);
        BxHash_findLine(ls.art, ls.artSize);
        ls.gen++;
        LightLock_Unlock(&ls.lock);
        bh.fill = -1;
        bh.active = true;
    }
    bh.read += 65536u;
    if (bh.read > bh.size) bh.read = bh.size;
    bh.bar = (uint32_t) ((bh.slot * 1000u + bh.read * 1000u / bh.size) * LS_CAP / 6000u);
    LightLock_Lock(&ls.lock);
    if (bh.bar < ls.shown) bh.bar = ls.shown;
    ls.shown = bh.bar;
    int32_t fill = (int32_t) ((uint32_t) ls.fillMax * bh.bar / 1000u);
    // Painting only a new filled pixel bounds setup overhead; cached checks never pause/draw/stat here.
    if (fill != bh.fill) BxHash_present(fill, BxHash_dogKey(lsMs(ls.loadStart, svcGetSystemTick())));
    LightLock_Unlock(&ls.lock);
    bh.fill = fill;
}

CtrDataVersionResult BxChapterHash_check(const char*, const char*, const char*, const char*,
                                      void (*)(void), bool*, char[65]);
BH_CODE CtrDataVersionResult BxChapterHash_check(const char* path, const char* game, const char* part,
        const char* cachePath, void (*progress)(void), bool* cached, char digest[65]) {
    bh.path = path;
    bh.progress = progress;
    bh.slot = part[0] == 'c' ? (uint32_t) (part[7] - '0') : 0;
    bh.read = 0;
    bh.active = false;
    CtrDataVersionResult result = BxCold_check(path, game, part, cachePath, bhProgress, cached, digest);
    if (bh.active) {
        LightLock_Lock(&ls.lock);
        psLine = nullptr;
        LightLock_Unlock(&ls.lock);
        BxHash_pause(false);
    }
    return result;
}
#endif
