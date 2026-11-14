// Unified friendly crash screen and release-quiet logging (argcrash).
//
// Every fatal path on the 3DS (VM error, out of memory, ARM exception on any thread, 60 s hang, svcBreak,
// unexpected abort()/exit(), wrong-model notice, missing/incompatible data.win, save failure) routes to one calm,
// zero-allocation software-rendered screen on both displays while keeping the lab's TCP and SD crash reporting intact.
//
// All code, constants and statics live in .text.late.argcrash, .rodata.argcrash_in_text and .bss.late.argcrash
// (platform/3ds/late_reserve.ld), and existing translation units are hooked via -Wl,--wrap in CMakeLists.txt so
// every existing object file and hot-pinned function keeps its exact address and size.

#include <3ds.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>

#include "common.h"
#include "log.h"
#include "runner.h"
#include "ctr_r216_ut.h" // R216: UNDERTALE only (R216_PICK, R216_UT)
#line 24
#include "utils.h"

#include "ctr_benchmark.h"
#include "ctr_boot_trail.h"
#include "ctr_build_id.h"
#include "ctr_display.h"
#include "ctr_hbldr.h"
#include "ctr_lab.h"
#include "ctr_launch_args.h"
#include "ctr_loadtime.h"
#include "ctr_log.h"
#include "ctr_memory.h"
#include "ctr_release_branding.h"
#include "ctr_watchdog.h"
#include "ctr_wrap_chain.h"

#if defined(__3DS__) && defined(__GNUC__)
#define ARGCRASH_LATE __attribute__((noinline, section(".text.late.argcrash")))
#define ARGCRASH_FUNC __attribute__((section(".text.late.argcrash")))
// Constants sit in the late code reserve (.rodata.argcrash_in_text -> .late.text in late_reserve.ld),
// following .rodata.screens_in_text so late rodata costs 0 B.
#define ARGCRASH_RODATA __attribute__((section(".rodata.argcrash_in_text")))
#define ARGCRASH_BSS __attribute__((section(".bss.late.argcrash")))
#else
#define ARGCRASH_LATE
#define ARGCRASH_FUNC
#define ARGCRASH_RODATA
#define ARGCRASH_BSS
#endif

#define ARGCRASH_STR(name, text) static const char name[(sizeof(text) + 3u) & ~3u] ARGCRASH_RODATA = text

extern u32 __ctru_heap_size;
extern u32 __ctru_linear_heap_size;
extern bool CtrScreens_otherOff;

void __real_gfxScreenSwapBuffers(gfxScreen_t scr, bool hasStereo);
bool CTR_LAUNCHARGS_AFTER_ARGCRASH(CtrLaunchArgs* args, int argc, char** argv);
void __real_CtrMemory_markGameThread(void);
void __real_CtrWatchdog_watch(Runner* runner);
void __real_CtrWatchdog_stop(void);
const char* __real_CtrHbldr_setNext(const char* path, const char* const* args, int count);
void __real_CtrLab_exiting(int seconds);
void __real_CtrLab_poll(void);
void __real_CtrLog_emergency(const char* text, int waitMs);
void __real_CtrLog_setConsoleEnabled(bool enabled);
void __real_LightEvent_Init(LightEvent* event, ResetType reset_type);
void __real_LightEvent_Signal(LightEvent* event);
void __real_platformLog(const logType type, const char* format, va_list va);
Result __real_svcOutputDebugString(const char* str, s32 length);
bool homeaudioEnter(void); bool __real_aptMainLoop(void);
void __real_svcBreak(UserBreakType breakReason) __attribute__((noreturn));
void __real_exit(int status) __attribute__((noreturn));

bool __wrap_CtrLaunchArgs_parse(CtrLaunchArgs* args, int argc, char** argv);
void __wrap_CtrMemory_markGameThread(void);
void __wrap_CtrWatchdog_watch(Runner* runner);
const char* __wrap_CtrHbldr_setNext(const char* path, const char* const* args, int count);
void __wrap_CtrLab_exiting(int seconds);
void __wrap_CtrLab_poll(void);
void __wrap_CtrLog_emergency(const char* text, int waitMs);
void __wrap_LightEvent_Init(LightEvent* event, ResetType reset_type);
void __wrap_LightEvent_Signal(LightEvent* event);
void __wrap_platformLog(const logType type, const char* format, va_list va);
Result __wrap_svcOutputDebugString(const char* str, s32 length);
bool __wrap_aptMainLoop(void);
void __wrap_svcBreak(UserBreakType breakReason) __attribute__((noreturn));
void __wrap_exit(int status) __attribute__((noreturn));

typedef enum {
    CRASH_NONE = 0,
    CRASH_VM,
    CRASH_OOM,
    CRASH_ARM_DATA,
    CRASH_ARM_PREFETCH,
    CRASH_ARM_UNDEF,
    CRASH_ARM_VFP,
    CRASH_HANG,
    CRASH_BREAK,
    CRASH_ABORT,
    CRASH_SAVE,
    CRASH_MODEL,
    CRASH_DATAWIN,
    CRASH_DATAVER,
    CRASH_KIND_COUNT
} ArgcrashKind;

// ===[ Late Constants in .rodata.argcrash_in_text ]===

#define ARGCRASH_STRIFY2(x) #x
#define ARGCRASH_STRIFY(x) ARGCRASH_STRIFY2(x)

#ifdef CTRPORT_RELEASE_VER
#define ARGCRASH_VER_STR ARGCRASH_STRIFY(CTRPORT_RELEASE_VER)
#else
#define ARGCRASH_VER_STR "1.0.0-beta.1"
#endif

#ifdef ENABLE_WAD17
#define ARGCRASH_VER_TEXT "deltarune-3ds " ARGCRASH_VER_STR
#else
#define ARGCRASH_VER_TEXT "undertale-3ds " ARGCRASH_VER_STR
#endif

ARGCRASH_STR(strGameTitle, CTRPORT_GAME_TITLE);
ARGCRASH_STR(strLogHeading, "Log:");
ARGCRASH_STR(strLogPath, CTRPORT_GAME_DIR "/crash.txt");
ARGCRASH_STR(strVerLine, ARGCRASH_VER_TEXT);
ARGCRASH_STR(strPressStart, "Press START to quit.");
ARGCRASH_STR(strReturningHome, "Quitting...");

ARGCRASH_STR(strTitleCrash, "* The game stopped unexpectedly.");
ARGCRASH_STR(strTitleOom, "* The console ran out of memory.");
#ifdef CTRPORT_TARGET_O3DS
ARGCRASH_STR(strTitleOomCia, "* Out of memory. Use the .cia build.");
#endif
ARGCRASH_STR(strTitleHang, "* The game stopped responding.");
ARGCRASH_STR(strTitleSave, "* Saving failed. Check SD card space.");
ARGCRASH_STR(strTitleModel, "* This version needs a New 3DS. Use the Old 3DS version.");
ARGCRASH_STR(strTitleData, "* Game files are missing. Run the builder again.");
// datarefuse (playblocker #8): UNDERTALE stops on a data.win that isn't the file it was built from (z_datarefuse.c),
// whose message carries "bytecode version" (strBytecodeVer) to reach this title. Same array size (60 B), so no
// constant moves; DELTARUNE keeps its text.
#ifdef ENABLE_WAD16
ARGCRASH_STR(strTitleDataVer, "* Game files don't match this build. Run the builder again.");
#else
ARGCRASH_STR(strTitleDataVer, "* This game version isn't supported. Run the builder again.");
#endif

ARGCRASH_STR(strCodeVm, "ERR-VM");
ARGCRASH_STR(strCodeOom, "ERR-OOM");
ARGCRASH_STR(strCodeArmData, "ERR-ARM-DATA");
ARGCRASH_STR(strCodeArmPref, "ERR-ARM-PREF");
ARGCRASH_STR(strCodeArmUndef, "ERR-ARM-UNDEF");
ARGCRASH_STR(strCodeArmVfp, "ERR-ARM-VFP");
ARGCRASH_STR(strCodeHang, "ERR-HANG");
ARGCRASH_STR(strCodeBreak, "ERR-BREAK");
ARGCRASH_STR(strCodeAbort, "ERR-ABORT");
ARGCRASH_STR(strCodeSave, "ERR-SAVE");
ARGCRASH_STR(strCodeModel, "ERR-MODEL");
ARGCRASH_STR(strCodeData, "ERR-DATA");

typedef struct {
    const char* code;
    const char* title;
} CrashDesc;

static const CrashDesc crashDescs[CRASH_KIND_COUNT] ARGCRASH_RODATA = {
    [CRASH_NONE]         = { strCodeAbort,    strTitleCrash   },
    [CRASH_VM]           = { strCodeVm,       strTitleCrash   },
    [CRASH_OOM]          = { strCodeOom,      strTitleOom     },
    [CRASH_ARM_DATA]     = { strCodeArmData,  strTitleCrash   },
    [CRASH_ARM_PREFETCH] = { strCodeArmPref,  strTitleCrash   },
    [CRASH_ARM_UNDEF]    = { strCodeArmUndef, strTitleCrash   },
    [CRASH_ARM_VFP]      = { strCodeArmVfp,   strTitleCrash   },
    [CRASH_HANG]         = { strCodeHang,     strTitleHang    },
    [CRASH_BREAK]        = { strCodeBreak,    strTitleCrash   },
    [CRASH_ABORT]        = { strCodeAbort,    strTitleCrash   },
    [CRASH_SAVE]         = { strCodeSave,     strTitleSave    },
    [CRASH_MODEL]        = { strCodeModel,    strTitleModel   },
    [CRASH_DATAWIN]      = { strCodeData,     strTitleData    },
    [CRASH_DATAVER]      = { strCodeData,     strTitleDataVer },
};

// Strings for classification and release-quiet log filtering.
ARGCRASH_STR(argcrashStrEmpty, "");
ARGCRASH_STR(strCrashReportPath, CTRPORT_GAME_DIR "/crash.txt");
ARGCRASH_STR(strLabHostDefault, "sdmc:/3ds/ctrport-lab.3dsx");
ARGCRASH_STR(strNormalPlay, "normal play");

ARGCRASH_STR(strBenchrunPath, CTRPORT_GAME_DIR "/benchrun.txt");
ARGCRASH_STR(strStatehashPath, CTRPORT_GAME_DIR "/statehash.txt");
ARGCRASH_STR(strAutoshotPath, CTRPORT_GAME_DIR "/autoshot.txt");
ARGCRASH_STR(strGputracePath, CTRPORT_GAME_DIR "/gputrace.txt");
ARGCRASH_STR(strFlickerPath, CTRPORT_GAME_DIR "/flickerprobe.txt");
ARGCRASH_STR(strProfilePath, CTRPORT_GAME_DIR "/profile.txt");

static const char* const diagFilePaths[6] ARGCRASH_RODATA = {
    strBenchrunPath,
    strStatehashPath,
    strAutoshotPath,
    strGputracePath,
    strFlickerPath,
    strProfilePath,
};

ARGCRASH_STR(strErrFatalPrefix, "[ERROR] Fatal: ");
ARGCRASH_STR(strErrOomPrefix, "[ERROR] Out of memory: ");
ARGCRASH_STR(strSavingFailed, "Saving failed");
ARGCRASH_STR(strNew3ds, "New 3DS");
ARGCRASH_STR(strBytecodeVer, "bytecode version");
ARGCRASH_STR(strCrashDataAbort, "crash: data abort");
ARGCRASH_STR(strCrashPrefAbort, "crash: prefetch abort");
ARGCRASH_STR(strCrashUndef, "crash: undefined instruction");
ARGCRASH_STR(strCrashVfp, "crash: VFP exception");
ARGCRASH_STR(strFailBreak, "failure: break");
ARGCRASH_STR(strFailOom, "failure: oom");
ARGCRASH_STR(strFailHang, "failure: hang");
ARGCRASH_STR(strHangColon, "hang:");
ARGCRASH_STR(strReasonBreak, "break");
ARGCRASH_STR(strDetailBreak, "svcBreak() called");
ARGCRASH_STR(strReasonExit, "exit");
ARGCRASH_STR(strDetailExit0, "unexpected exit(0)");
ARGCRASH_STR(strDetailExit1, "unexpected exit(1)");

typedef struct {
    const char* needle;
    uint8_t kind;
} ReportMatch;

static const ReportMatch reportMatches[8] ARGCRASH_RODATA = {
    { strCrashDataAbort, CRASH_ARM_DATA },
    { strCrashPrefAbort, CRASH_ARM_PREFETCH },
    { strCrashUndef,     CRASH_ARM_UNDEF },
    { strCrashVfp,       CRASH_ARM_VFP },
    { strFailBreak,      CRASH_BREAK },
    { strFailOom,        CRASH_OOM },
    { strFailHang,       CRASH_HANG },
    { strHangColon,      CRASH_HANG },
};

ARGCRASH_STR(strLogFatal, "FATAL: ");
ARGCRASH_STR(strLogFailedRead, "failed to read");
ARGCRASH_STR(strLogVm, "VM:");
ARGCRASH_STR(strLogGameError, "Game error");
ARGCRASH_STR(strLogExitFmt, "Exit: %s\n");

// Per-frame / high-frequency normal-log prefixes suppressed in default Release play.
// Kept in Release logs for diagnosing player and developer reports:
//   [perf], [pace], Slow frame, Files (room): / Files:, [rl], [code], [mem], "Textures:" and all bench/profile lines.
// [pace] (every 5 s, beside [perf]) is kept since ethanlog5: dropped with Other screen Off, a player's log lost its
// held-frame counts from the mode change on. Its slot stays (8 B and a pointer, so nothing after it moves) with a
// prefix no log format starts with.
ARGCRASH_STR(strTagSkip, "[skip");
ARGCRASH_STR(strTagRender, "[render]");
ARGCRASH_STR(strTagSmartCount, "[smartcount");
ARGCRASH_STR(strTagFill, "[fill]");
ARGCRASH_STR(strTagPace, "\x01pace");
ARGCRASH_STR(strTagFlips, "[flips]");
ARGCRASH_STR(strTagLevel, "[level]");
ARGCRASH_STR(strLogGameDebug, "Game: %s\n");
ARGCRASH_STR(strLogSmartSmooth, "Smart smoothing: ");
ARGCRASH_STR(strLogSpatialGrid, "SpatialGrid: ");
ARGCRASH_STR(strLogVmSurfRead, "VM: %s reads surfaces back\n");
ARGCRASH_STR(strLogSurface, "Surface: ");
ARGCRASH_STR(strLogGpuTransfer, "GPU: transfer ");
ARGCRASH_STR(strLogVram, "VRAM ");
ARGCRASH_STR(strLogPreparedPage, "Prepared page ");
ARGCRASH_STR(strLogWrotePage, "  wrote ");
ARGCRASH_STR(strLogPubPage, "  publication ");
ARGCRASH_STR(strLogRendStreams, "Renderer: %u ");
ARGCRASH_STR(strLogRendPrewarm, "Renderer: prewarmed");

#define DROP_PREFIX_COUNT 19u
static const char* const dropPrefixes[DROP_PREFIX_COUNT] ARGCRASH_RODATA = {
    strTagSkip,
    strTagRender,
    strTagSmartCount,
    strTagFill,
    strTagPace,
    strTagFlips,
    strTagLevel,
    strLogGameDebug,
    strLogVmSurfRead,
    strLogSmartSmooth,
    strLogSpatialGrid,
    strLogSurface,
    strLogGpuTransfer,
    strLogVram,
    strLogPreparedPage,
    strLogWrotePage,
    strLogPubPage,
    strLogRendStreams,
    strLogRendPrewarm,
};

// ===[ Late BSS State ]===

static CtrLaunchArgs* argcrashLaunch ARGCRASH_BSS;
static Runner* argcrashRunner ARGCRASH_BSS;
static const void* argcrashMainTls ARGCRASH_BSS;
static Handle argcrashMainThread ARGCRASH_BSS;
static LightEvent* labLeaveEvent ARGCRASH_BSS;
static LightEvent* deferredLeaveEvent ARGCRASH_BSS;
static devoptab_t argcrashNullOut ARGCRASH_BSS;
static u8* curFbPtr ARGCRASH_BSS;
static u16 curFbH ARGCRASH_BSS;
static GSPGPU_FramebufferFormat curFbFmt ARGCRASH_BSS;
static bool realLabMode ARGCRASH_BSS;
static bool inDefaultLabStart ARGCRASH_BSS;
static bool argcrashNormalGuard ARGCRASH_BSS;
static volatile bool argcrashGfxReady ARGCRASH_BSS;
static volatile bool argcrashInCrash ARGCRASH_BSS;
static volatile bool argcrashMainParked ARGCRASH_BSS;
static volatile bool cleanExitStarted ARGCRASH_BSS;
static volatile bool pendingScreenActive ARGCRASH_BSS;
static volatile bool pendingScreenPainted ARGCRASH_BSS;
static volatile ArgcrashKind pendingScreenKind ARGCRASH_BSS;
static volatile ArgcrashKind lastErrorKind ARGCRASH_BSS;
static bool argcrashDiagFiles ARGCRASH_BSS;

// ===[ Zero-Allocation Software Crash Screen Renderer ]===

typedef struct {
    uint8_t dx;
    uint8_t y;
    uint32_t rgb;
    const char* text;
} ScreenLine;

static const ScreenLine screenLines[8] ARGCRASH_RODATA = {
    {   0,  42, 0xFFFFFFu, strGameTitle },
    {   0,  98, 0xB8C4D0u, strLogHeading },
    {   0, 112, 0xFFE066u, strLogPath },
    {   0, 132, 0xD8E0E8u, strVerLine },
    {   0, 146, 0xD8E0E8u, CTRPORT_COMMIT },
    {   0,  58, 0xFFFFFFu, nullptr },
    {   0, 158, 0xFFA860u, nullptr },
    {   0, 182, 0xFFFF60u, nullptr },
};

ARGCRASH_FUNC static ssize_t nullWrite(struct _reent* r, void* fd, const char* ptr, size_t len) {
    (void) r;
    (void) fd;
    (void) ptr;
    return (ssize_t) len;
}

ARGCRASH_LATE static void writeCrashFile(const char* text) {
    if (text == nullptr) return;
    Handle file = 0;
    if (R_FAILED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, argcrashStrEmpty),
                                         fsMakePath(PATH_ASCII, strCrashReportPath + 5),
                                         FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
        return;
    }
    FSFILE_SetSize(file, 0);
    u32 written = 0;
    FSFILE_Write(file, &written, 0, text, (u32) strlen(text), FS_WRITE_FLUSH);
    FSFILE_Close(file);
}

ARGCRASH_LATE static void putPixel(int x, int y, u32 rgb) {
    int wide = (curFbH == 800u);
    int col = x << wide;
    u8 r = (u8) (rgb >> 16), g = (u8) (rgb >> 8), b = (u8) rgb;
    u16 rgb565 = (u16) (((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
    for (int n = 0; n <= wide; n++, col++) {
        int idx = col * 240 + (239 - y);
        if (curFbFmt == GSP_BGR8_OES) {
            u8* p = curFbPtr + idx * 3;
            p[0] = b;
            p[1] = g;
            p[2] = r;
        } else {
            ((u16*) curFbPtr)[idx] = rgb565;
        }
    }
}

ARGCRASH_LATE static void paintBuffer(u8* fb, u16 fbW, u16 fbH, GSPGPU_FramebufferFormat fmt,
                                      bool isTop, ArgcrashKind kind, bool exiting) {
    if (fb == nullptr || fbW == 0 || fbH == 0) return;
    curFbPtr = fb;
    curFbH = fbH;
    curFbFmt = fmt;
    u32 fbBytes = (u32) fbW * (u32) fbH * (fmt == GSP_BGR8_OES ? 3u : 2u);
    memset(fb, 0, fbBytes);

    int textX = isTop ? 28 : 4;
    int ruleW = isTop ? 344 : 312;
    for (int x = textX; x < textX + ruleW; x++) {
        putPixel(x, 30, 0xFFFFFFu);
        putPixel(x, 86, 0x505868u);
        putPixel(x, 166, 0x505868u);
        putPixel(x, 204, 0xFFFFFFu);
    }

    if ((unsigned int) kind >= CRASH_KIND_COUNT) kind = CRASH_NONE;
    const CrashDesc* d = &crashDescs[kind];
    const char* title = d->title;
#ifdef CTRPORT_TARGET_O3DS
    if (kind == CRASH_OOM && envIsHomebrew() && CTR_OOM_CIA_HINT_BELOW > __ctru_heap_size + __ctru_linear_heap_size) {
        title = strTitleOomCia;
    }
#endif
    const char* dynText[3] = {
        title,
        d->code,
        exiting ? strReturningHome : strPressStart,
    };

    const ConsoleFont* font = &consoleGetDefault()->font;
    for (size_t i = 0; i < 8 && font->gfx != nullptr; i++) {
        const char* s = (i < 5) ? screenLines[i].text : dynText[i - 5];
        if (s == nullptr) continue;
        u32 rgb = (i == 7 && exiting) ? 0x80FF60u : screenLines[i].rgb;
        int y0 = screenLines[i].y;
        for (int x = textX + screenLines[i].dx; *s != '\0'; s++, x += 8) {
            if (*s == ' ' && x > textX + ruleW - 48) {
                x = textX + 8;
                y0 += 12;
                continue;
            }
            unsigned int ch = (unsigned char) *s - font->asciiOffset;
            if (ch >= font->numChars) continue;
            const u8* glyph = font->gfx + ch * 8u;
            for (int r = 0; r < 8; r++) {
                u8 bits = glyph[r];
                for (int c = 0; c < 8; c++) {
                    if ((bits & (0x80u >> c)) != 0) putPixel(x + c, y0 + r, rgb);
                }
            }
        }
    }

    GSPGPU_FlushDataCache(fb, fbBytes);
}
#include "z_argcrash_sd.h" // UTSCOURD-001 (the code is at the end)
#if !defined(NDEBUG) || defined(CTRPORT_CRASH_TEST)
ARGCRASH_STR(strCrashTestPath, CTRPORT_GAME_DIR "/crashtest.txt");
ARGCRASH_STR(strBmpTopFmt, CTRPORT_GAME_DIR "/crash_top_%s.bmp");
ARGCRASH_STR(strBmpBotFmt, CTRPORT_GAME_DIR "/crash_bottom_%s.bmp");
ARGCRASH_STR(strReadMode, "r");
ARGCRASH_STR(strAutoToken, "auto");
// Present in rodata so queue_lint.py accepts --ini crashtest=<N> on CTRPORT_CRASH_TEST builds.
ARGCRASH_STR(strIniCrashtest, "crashtest");
ARGCRASH_STR(strIniCrashtestEq, "crashtest=");
ARGCRASH_STR(strTestSummaryFmt, "[argcrash-test] painted code=%s title=\"%s\" crash_txt=%lu B\n");
static char argcrashTestKind[24] ARGCRASH_BSS;
static bool argcrashTestChecked ARGCRASH_BSS;
static bool argcrashTestFired ARGCRASH_BSS;
static bool argcrashTestAutoExit ARGCRASH_BSS;
static bool argcrashChainLab ARGCRASH_BSS;
static bool argcrashShotSaved ARGCRASH_BSS;
static int argcrashAutoExitFrames ARGCRASH_BSS;
static int argcrashRecoverFrame ARGCRASH_BSS;
static u64 argcrashGameEndStartTick ARGCRASH_BSS;

ARGCRASH_FUNC static void readPixelRgb(const u8* fb, u16 fbW, u16 fbH, GSPGPU_FramebufferFormat fmt,
                                       int x, int y, u8* r, u8* g, u8* b) {
    int col = (fbH == 800u) ? (2 * x) : x;
    int idx = col * (int) fbW + ((int) fbW - 1 - y);
    if (fmt == GSP_BGR8_OES) {
        const u8* p = fb + idx * 3;
        *b = p[0];
        *g = p[1];
        *r = p[2];
    } else {
        u16 v = ((const u16*) fb)[idx];
        *r = (u8) (((v >> 11) & 0x1Fu) * 255u / 31u);
        *g = (u8) (((v >> 5) & 0x3Fu) * 255u / 63u);
        *b = (u8) ((v & 0x1Fu) * 255u / 31u);
    }
}

ARGCRASH_FUNC static void writeBmpFile(const char* sdRelPath, const u8* fb, u16 fbH,
                                       GSPGPU_FramebufferFormat fmt, int width) {
    if (fb == nullptr) return;
    Handle file = 0;
    if (R_FAILED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, argcrashStrEmpty),
                                         fsMakePath(PATH_ASCII, sdRelPath),
                                         FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
        return;
    }
    u32 rowBytes = (u32) width * 3u;
    u32 imgBytes = rowBytes * 240u;
    u32 fileSize = 54u + imgBytes;
    FSFILE_SetSize(file, fileSize);
    u8 header[54];
    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    memcpy(header + 2, &fileSize, 4);
    u32 dataOffset = 54u;
    memcpy(header + 10, &dataOffset, 4);
    u32 dibSize = 40u;
    memcpy(header + 14, &dibSize, 4);
    int32_t w32 = width, h32 = 240;
    memcpy(header + 18, &w32, 4);
    memcpy(header + 22, &h32, 4);
    u16 planes = 1u, bpp = 24u;
    memcpy(header + 26, &planes, 2);
    memcpy(header + 28, &bpp, 2);
    memcpy(header + 34, &imgBytes, 4);
    u32 written = 0;
    u64 offset = 0;
    FSFILE_Write(file, &written, offset, header, sizeof(header), 0);
    offset += sizeof(header);
    static u8 rowBuf[400 * 3] ARGCRASH_BSS;
    for (int y = 239; y >= 0; y--) {
        for (int x = 0; x < width; x++) {
            u8 r = 0, g = 0, b = 0;
            readPixelRgb(fb, 240u, fbH, fmt, x, y, &r, &g, &b);
            rowBuf[x * 3 + 0] = b;
            rowBuf[x * 3 + 1] = g;
            rowBuf[x * 3 + 2] = r;
        }
        FSFILE_Write(file, &written, offset, rowBuf, rowBytes, y == 0 ? FS_WRITE_FLUSH : 0);
        offset += rowBytes;
    }
    FSFILE_Close(file);
}

ARGCRASH_FUNC static u32 checkCrashTxtSize(void) {
    Handle file = 0;
    u64 size = 0;
    if (R_SUCCEEDED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, argcrashStrEmpty),
                                            fsMakePath(PATH_ASCII, strCrashReportPath + 5),
                                            FS_OPEN_READ, 0))) {
        FSFILE_GetSize(file, &size);
        FSFILE_Close(file);
    }
    return (u32) size;
}
#endif
static void argcrashPaintFixed(u8*, u16, u16, GSPGPU_FramebufferFormat, bool, ArgcrashKind, bool); // file end
R216_UT(static Result argcrashLcdSkip(void);) // file end: the backlight comes after the painter's claim (argcrashPaintGated)
ARGCRASH_LATE static void argcrashPaintScreens(ArgcrashKind kind, bool exiting) {
    if (!argcrashGfxReady) return;
    // The real setter (only the flag): the boot log's wrap would point the consoles back and free their own buffer
    // (main.c CtrBootlog_reveal), a heap call that a main thread stuck in the allocator would block (c26rev r22).
    // The painter needs neither; the buffer is kept until the exit.
    __real_CtrLog_setConsoleEnabled(false);
    const devoptab_t* current = devoptab_list[STD_OUT];
    if (current != nullptr && current != &argcrashNullOut) {
        argcrashNullOut = *current;
        argcrashNullOut.write_r = nullWrite;
        devoptab_list[STD_OUT] = &argcrashNullOut;
    }
    if (R_SUCCEEDED(R216_PICK(argcrashLcdSkip, gspLcdInit)())) { // R188g: never (argcrashPaintGated sets the backlight, outside its claim)
        GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_TOP | GSPLCD_SCREEN_BOTTOM);
        gspLcdExit();
    }
    for (int s = 0; s < 2; s++) {
        gfxScreen_t screen = s ? GFX_BOTTOM : GFX_TOP;
        GSPGPU_FramebufferFormat fmt = gfxGetScreenFormat(screen);
        u8* prevFb = nullptr;
        for (int i = 0; i < 2; i++) {
            u16 fbW = 0, fbH = 0;
            u8* fb = gfxGetFramebuffer(screen, GFX_LEFT, &fbW, &fbH);
            if (fb != prevFb) ARGCRASH_SD_PAINT(fb, fbW, fbH, fmt, s == 0, kind, exiting); // SCOURS4-004, UTSCOURD-001
            prevFb = fb;
            gfxSetDoubleBuffering(screen, false);
        }
        __real_gfxScreenSwapBuffers(screen, false);
    }
#if !defined(NDEBUG) || defined(CTRPORT_CRASH_TEST)
    if (!exiting && !argcrashShotSaved && argcrashTestKind[0] != '\0') {
        argcrashShotSaved = true;
        char path[128];
        u16 fbW = 0, fbH = 0;
        u8* topFb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbW, &fbH);
        snprintf(path, sizeof(path), strBmpTopFmt, argcrashTestKind);
        writeBmpFile(path + 5, topFb, fbH, gfxGetScreenFormat(GFX_TOP), 400);
        u8* botFb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fbW, &fbH);
        snprintf(path, sizeof(path), strBmpBotFmt, argcrashTestKind);
        writeBmpFile(path + 5, botFb, fbH, gfxGetScreenFormat(GFX_BOTTOM), 320);
    }
#endif
}

ARGCRASH_LATE static ArgcrashKind argcrashClassifyReport(const char* text) {
    if (text == nullptr) return CRASH_ABORT;
    for (size_t i = 0; i < 8; i++) {
        if (strstr(text, reportMatches[i].needle) != nullptr) {
            return (ArgcrashKind) reportMatches[i].kind;
        }
    }
    return lastErrorKind != CRASH_NONE ? lastErrorKind : CRASH_ABORT;
}

R216_UT(static void argcrashPaintGated(ArgcrashKind, bool);) // SCOURS4-005, file end
// Emergency callback for normal (non-lab) runs: idles the watchdog without joining or freeing anything,
// flushes the technical report to TCP and SD log.txt, parks the main thread if a worker thread faulted,
// paints the friendly crash screen on both displays, and waits for START (or HOME -> Close) before returning to leave().
ARGCRASH_LATE static void argcrashEmergency(const char* report, int waitMs) {
    argcrashInCrash = true;
    __real_CtrWatchdog_watch(nullptr);

    bool onMain = (getThreadLocalStorage() == argcrashMainTls);
    if (!onMain) {
        for (int waited = 0; waited < 200 && !argcrashMainParked; waited += 10) {
            svcSleepThread(10000000LL);
        }
        if (argcrashMainThread != 0) svcSetThreadPriority(argcrashMainThread, 0x3F);
        for (int waited = 0; waited < 300 && !argcrashMainParked; waited += 10) {
            svcSleepThread(10000000LL);
        }
    }

    ArgcrashKind kind = argcrashClassifyReport(report);
#if !defined(NDEBUG) || defined(CTRPORT_CRASH_TEST)
    if (argcrashTestKind[0] != '\0') {
        CtrLog_emergencyKeepLink(report, waitMs);
        R216_PICK(argcrashPaintGated, argcrashPaintScreens)(kind, false);
        const CrashDesc* d = &crashDescs[(unsigned int) kind < CRASH_KIND_COUNT ? kind : CRASH_NONE];
        char msg[192];
        snprintf(msg, sizeof(msg), strTestSummaryFmt, d->code, d->title, (unsigned long) checkCrashTxtSize());
        __real_CtrLog_emergency(msg, 500);
    } else
#endif
    {
        __real_CtrLog_emergency(report, waitMs);
        R216_PICK(argcrashPaintGated, argcrashPaintScreens)(kind, false);
    }

    bool unattendedRun = argcrashLaunch != nullptr &&
                         (argcrashLaunch->exitAfter || argcrashLaunch->timeoutSeconds > 0);
    if (unattendedRun) {
        for (int i = 0; i < 15; i++) gspWaitForVBlank();
        if (deferredLeaveEvent != nullptr) __real_LightEvent_Signal(deferredLeaveEvent);
        return;
    }

    int frames = 0;
    for (;;) {
        gspWaitForVBlank();
        hidScanInput();
        if ((hidKeysDown() & KEY_START) != 0) break;
        if (!__real_aptMainLoop()) break;
        if ((++frames % 30) == 0) R216_PICK(argcrashPaintGated, argcrashPaintScreens)(kind, false);
#if !defined(NDEBUG) || defined(CTRPORT_CRASH_TEST)
        if (argcrashTestAutoExit && frames >= 30) break;
#endif
    }
    R216_PICK(argcrashPaintGated, argcrashPaintScreens)(kind, true);
    if (deferredLeaveEvent != nullptr) __real_LightEvent_Signal(deferredLeaveEvent);
}

static const CtrLabConfig defaultLabConfig ARGCRASH_RODATA = {
    .appName = ctrReleaseAppName,
    .buildId = CTRPORT_COMMIT,
    .hostPath = strLabHostDefault,
    .reportPath = strCrashReportPath,
    .hangSeconds = 60,
    .deadlineSeconds = 0,
    .logTail = CtrLog_tail,
    .emergencyLog = argcrashEmergency,
    .progress = CtrWatchdog_progress,
    .watching = CtrWatchdog_watching,
    .phase = CtrWatchdog_phase,
    .threadName = CtrWatchdog_threadNameOf,
    .hangDetails = CtrWatchdog_lastReport,
    .bootSeconds = 0,
    .bootStep = CtrBootTrail_last,
    .trail = CtrBootTrail_mark,
};

// ===[ Release-Quiet Logging Filter ]===

ARGCRASH_LATE static bool startsWith(const char* str, const char* prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

ARGCRASH_LATE static bool matchesAnyPrefix(const char* str, const char* const* list, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (startsWith(str, list[i])) return true;
    }
    return false;
}

ARGCRASH_LATE static void argcrashFail(const char* reason, const char* detail) {
    if (CtrLab_active()) CtrLab_fail(reason, detail);
}

// ===[ Debug / Test-Only Crash Trigger ]===

#if !defined(NDEBUG) || defined(CTRPORT_CRASH_TEST)
ARGCRASH_STR(strTestVm, "vm");
ARGCRASH_STR(strTestOom, "oom");
ARGCRASH_STR(strTestArm, "arm");
ARGCRASH_STR(strTestArmData, "arm-data");
ARGCRASH_STR(strTestArmPref, "arm-prefetch");
ARGCRASH_STR(strTestArmUndef, "arm-undef");
ARGCRASH_STR(strTestArmVfp, "arm-vfp");
ARGCRASH_STR(strTestDataShort, "data");
ARGCRASH_STR(strTestPrefShort, "prefetch");
ARGCRASH_STR(strTestUndefShort, "undef");
ARGCRASH_STR(strTestHang, "hang");
ARGCRASH_STR(strTestStall20, "stall20");
ARGCRASH_STR(strTestGameEnd, "gameend");
ARGCRASH_STR(strTestThread, "thread");
ARGCRASH_STR(strTestFree, "free");
ARGCRASH_STR(strTestBreak, "break");
ARGCRASH_STR(strTestAbort, "abort");
ARGCRASH_STR(strTestExit, "exit");
ARGCRASH_STR(strTestModel, "model");
ARGCRASH_STR(strTestSave, "save");
ARGCRASH_STR(strTestDataWin, "datawin");
ARGCRASH_STR(strTestDataVer, "dataver");
ARGCRASH_STR(strTestVmErrLog, "VM: [test_trigger] DoDiv :: Divide by zero\n");
ARGCRASH_STR(strTestModelErrFmt, "[ERROR] Fatal: This build requires a New 3DS\n");
ARGCRASH_STR(strTestSaveErrFmt, "[ERROR] Fatal: Saving failed. Check SD card space.\n");
ARGCRASH_STR(strTestDataWinErrFmt, "[ERROR] Fatal: Failed to load game data: failed to read data.win\n");
ARGCRASH_STR(strTestDataVerErrFmt, "[ERROR] Fatal: Unsupported bytecode version 14 (this build supports 16 and 17)\n");
ARGCRASH_STR(strTestTriggerLog, "Crash test: triggering '%s' at frame %d\n");
ARGCRASH_STR(strTestStallRecoveredLog, "[argcrash-test] stall20 recovered at frame %d with no crash screen\n");
ARGCRASH_STR(strTestGameEndHeldLog, "[argcrash-test] game_end notice held 65 s with no crash screen; exiting cleanly\n");

static const char* const iniTestNames[10] ARGCRASH_RODATA = {
    strIniCrashtest,
    strTestHang,     // 1: hang (main thread spinning; 12 s watchdog report -> 60 s ERR-HANG)
    strTestStall20,  // 2: stall20 (20 s single-frame sleep; 12 s watchdog report fires, game recovers and carries on)
    strTestGameEnd,  // 3: gameend (UNDERTALE game_end notice held 65 s with no ERR-HANG, then clean exit)
    strTestArmData,  // 4: arm-data (main-thread data abort)
    strTestThread,   // 5: thread (worker-thread data abort while main thread draws a normal room)
    strTestFree,     // 6: free (heap-corruption fault inside free() with __malloc_lock held)
    strTestVm,       // 7: vm (VM error -> abort)
    strTestBreak,    // 8: break (svcBreak(USERBREAK_PANIC))
    strTestDataVer,  // 9: dataver (wrong data.win bytecode version -> ERR-DATA split wording)
};

extern void __malloc_lock(struct _reent* reent);

ARGCRASH_FUNC static void invokeEmulatorException(ERRF_ExceptionType exType, void* pcAddr) {
    s64 isEmulator = 0;
    if (R_SUCCEEDED(svcGetSystemInfo(&isEmulator, 0x20000, 0)) && isEmulator != 0) {
        void (**tlsSlots)(ERRF_ExceptionInfo*, CpuRegisters*) =
            (void (**)(ERRF_ExceptionInfo*, CpuRegisters*)) getThreadLocalStorage();
        void (*handler)(ERRF_ExceptionInfo*, CpuRegisters*) = tlsSlots[16];
        if (handler != nullptr) {
            ERRF_ExceptionInfo info;
            CpuRegisters regs;
            memset(&info, 0, sizeof(info));
            memset(&regs, 0, sizeof(regs));
            info.type = exType;
            info.fsr = (exType == ERRF_EXCEPTION_DATA_ABORT) ? 0x00000805u : 0x00000005u;
            info.far = (exType == ERRF_EXCEPTION_PREFETCH_ABORT) ? 0x00000010u : 0x00000008u;
            regs.pc = (u32) (uintptr_t) pcAddr;
            regs.lr = (u32) (uintptr_t) __builtin_return_address(0);
            regs.sp = (u32) (uintptr_t) __builtin_frame_address(0);
            regs.cpsr = 0x60000010u;
            handler(&info, &regs);
        }
    }
}

ARGCRASH_FUNC static void workerCrashMain(void* arg) {
    (void) arg;
    svcSleepThread(100000000LL);
    invokeEmulatorException(ERRF_EXCEPTION_DATA_ABORT, (void*) (uintptr_t) &workerCrashMain);
    *(volatile u32*) 8 = 0xDEADu;
}

ARGCRASH_FUNC static void argcrashReadTestFile(void) {
    if (argcrashTestChecked) return;
    argcrashTestChecked = true;
    if (argcrashTestKind[0] == '\0') {
        FILE* f = fopen(strCrashTestPath, strReadMode);
        if (f != nullptr) {
            char line[48];
            if (fgets(line, sizeof(line), f) != nullptr) {
                if (strstr(line, strAutoToken) != nullptr) argcrashTestAutoExit = true;
                size_t j = 0;
                for (size_t i = 0; line[i] != '\0' && j + 1 < sizeof(argcrashTestKind); i++) {
                    if (line[i] == '\r' || line[i] == '\n' || line[i] == ' ' || line[i] == '\t') break;
                    argcrashTestKind[j++] = line[i];
                }
                argcrashTestKind[j] = '\0';
            }
            fclose(f);
        }
    }
}

ARGCRASH_FUNC static void argcrashRunTestTrigger(void) {
    argcrashReadTestFile();
    if (argcrashRecoverFrame > 0 && argcrashRunner != nullptr) {
        if (argcrashRunner->frameCount >= argcrashRecoverFrame + 60) {
            logInfo(strTestStallRecoveredLog, argcrashRunner->frameCount);
            CtrLog_flush();
            argcrashRecoverFrame = 0;
            cleanExitStarted = true;
            exit(0);
        }
        return;
    }
    if (argcrashTestFired || argcrashTestKind[0] == '\0') return;
    if (argcrashRunner == nullptr || 5 > argcrashRunner->frameCount) return;
    argcrashTestFired = true;
    logWarn(strTestTriggerLog, argcrashTestKind, argcrashRunner->frameCount);
    CtrLog_flush();

    if (strcmp(argcrashTestKind, strTestVm) == 0) {
        logError(strTestVmErrLog);
        abort();
    } else if (strcmp(argcrashTestKind, strTestOom) == 0) {
        void* volatile p = safeMalloc(0x10000000u);
        (void) p;
    } else if (strcmp(argcrashTestKind, strTestArm) == 0 ||
               strcmp(argcrashTestKind, strTestArmData) == 0 ||
               strcmp(argcrashTestKind, strTestArmPref) == 0 ||
               strcmp(argcrashTestKind, strTestArmUndef) == 0 ||
               strcmp(argcrashTestKind, strTestArmVfp) == 0) {
        ERRF_ExceptionType exType = ERRF_EXCEPTION_DATA_ABORT;
        if (strcmp(argcrashTestKind, strTestArmPref) == 0) exType = ERRF_EXCEPTION_PREFETCH_ABORT;
        else if (strcmp(argcrashTestKind, strTestArmUndef) == 0) exType = ERRF_EXCEPTION_UNDEFINED;
        else if (strcmp(argcrashTestKind, strTestArmVfp) == 0) exType = ERRF_EXCEPTION_VFP;

        // Azahar (2126) does not pass CPU exceptions to userland threadOnException handlers (ctr_lab.c:46-50).
        // Invoke the registered TLS[16] handler directly on emulator; on console hardware, fault directly.
        invokeEmulatorException(exType, (void*) (uintptr_t) &argcrashRunTestTrigger);
        CtrLab_crashTest(exType == ERRF_EXCEPTION_PREFETCH_ABORT ? strTestPrefShort
                         : exType == ERRF_EXCEPTION_UNDEFINED    ? strTestUndefShort
                                                                 : strTestDataShort);
    } else if (strcmp(argcrashTestKind, strTestThread) == 0) {
        // Spawn a worker thread that faults after 100 ms while the main thread continues drawing frames.
        threadCreate(workerCrashMain, nullptr, 8192, 0x30, -2, true);
    } else if (strcmp(argcrashTestKind, strTestFree) == 0) {
        // Corrupt a heap block's dlmalloc chunk header so free() faults inside _free_r while holding __malloc_lock.
        volatile u32* p = (volatile u32*) malloc(128);
        if (p != nullptr) {
            p[0] = 0x00000008u; // fake prev chunk fd -> unmapped address 0x00000008
            p[1] = 0x00000008u; // fake prev chunk bk -> unmapped address 0x00000008
            p[-2] = 0u;         // prev_size = 0 (coalesce self)
            p[-1] &= ~1u;       // clear PREV_INUSE so _free_r unlinks fake prev chunk under __malloc_lock
            s64 isEmulator = 0;
            if (R_SUCCEEDED(svcGetSystemInfo(&isEmulator, 0x20000, 0)) && isEmulator != 0) {
                __malloc_lock(_REENT);
                invokeEmulatorException(ERRF_EXCEPTION_DATA_ABORT, (void*) (uintptr_t) &free);
            }
            free((void*) p);
        }
    } else if (strcmp(argcrashTestKind, strTestHang) == 0) {
        // Spin on the main thread: at 12 s CtrWatchdog writes hang.txt (no screen), and at 60 s guardMain triggers ERR-HANG.
        for (;;) __asm__ volatile("" : : : "memory");
    } else if (strcmp(argcrashTestKind, strTestStall20) == 0) {
        // Stall for 20 s inside one frame: 12 s watchdog writes hang.txt, then the frame finishes and play carries on.
        svcSleepThread(20000000000LL);
        argcrashRecoverFrame = argcrashRunner->frameCount;
    } else if (strcmp(argcrashTestKind, strTestGameEnd) == 0) {
        // Trigger UNDERTALE's game_end notice and hold it past 60 s to prove CtrLab_exiting is ignored outside lab mode.
        argcrashRunner->shouldExit = true;
    } else if (strcmp(argcrashTestKind, strTestModel) == 0 ||
               strcmp(argcrashTestKind, strTestSave) == 0 ||
               strcmp(argcrashTestKind, strTestDataWin) == 0 ||
               strcmp(argcrashTestKind, strTestDataVer) == 0) {
        const char* fatalStr = strTestDataVerErrFmt;
        if (strcmp(argcrashTestKind, strTestModel) == 0) fatalStr = strTestModelErrFmt;
        else if (strcmp(argcrashTestKind, strTestSave) == 0) fatalStr = strTestSaveErrFmt;
        else if (strcmp(argcrashTestKind, strTestDataWin) == 0) fatalStr = strTestDataWinErrFmt;
        __wrap_svcOutputDebugString(fatalStr, (s32) strlen(fatalStr));
        while (__wrap_aptMainLoop()) {
            hidScanInput();
            if ((hidKeysDown() & KEY_START) != 0) break;
            gspWaitForVBlank();
        }
        cleanExitStarted = true;
        exit(1);
    } else if (strcmp(argcrashTestKind, strTestBreak) == 0) {
        svcBreak(USERBREAK_PANIC);
    } else if (strcmp(argcrashTestKind, strTestAbort) == 0) {
        abort();
    } else if (strcmp(argcrashTestKind, strTestExit) == 0) {
        exit(1);
    }
}
#endif

// ===[ Linker Wrappers ]===

ARGCRASH_LATE bool __wrap_CtrLaunchArgs_parse(CtrLaunchArgs* args, int argc, char** argv) {
    bool res = CTR_LAUNCHARGS_AFTER_ARGCRASH(args, argc, argv);
    argcrashLaunch = args;
    if (argc > 1) argcrashDiagFiles = true;
#if !defined(NDEBUG) || defined(CTRPORT_CRASH_TEST)
    if (args != nullptr) {
        int kept = 0;
        for (int i = 0; i < args->iniCount; i++) {
            if (startsWith(args->ini[i], strIniCrashtestEq)) {
                int code = atoi(args->ini[i] + 10);
                if (code >= 1 && code <= 9 && iniTestNames[code] != nullptr) {
                    strncpy(argcrashTestKind, iniTestNames[code], sizeof(argcrashTestKind) - 1);
                    argcrashTestAutoExit = true;
                    if (args->lab) {
                        argcrashChainLab = true;
                        args->lab = false;
                    }
                }
                continue;
            }
            if (kept != i) {
                memcpy(args->ini[kept], args->ini[i], sizeof(args->ini[kept]));
            }
            kept++;
        }
        args->iniCount = kept;
    }
#endif
    return res;
}

ARGCRASH_LATE void __wrap_CtrMemory_markGameThread(void) {
    __real_CtrMemory_markGameThread();
    argcrashMainTls = getThreadLocalStorage();
    u32 threadId = 0;
    if (R_FAILED(svcGetThreadId(&threadId, CUR_THREAD_HANDLE)) ||
        R_FAILED(svcOpenThread(&argcrashMainThread, CUR_PROCESS_HANDLE, threadId))) {
        argcrashMainThread = 0;
    }
    argcrashGfxReady = true;
    realLabMode = CtrLab_active();
    struct stat st;
    for (size_t i = 0; i < 6 && !argcrashDiagFiles; i++) {
        if (bootHookStat(diagFilePaths[i], &st) == 0) argcrashDiagFiles = true;
    }
    if (!realLabMode && !CtrLab_active()) {
        inDefaultLabStart = true;
        argcrashNormalGuard = CtrLab_start(&defaultLabConfig);
        inDefaultLabStart = false;
        CtrLab_booted();
    }
}

ARGCRASH_LATE void __wrap_CtrWatchdog_watch(Runner* runner) {
    argcrashRunner = runner;
    __real_CtrWatchdog_watch(runner);
}

// A link of CtrWatchdog_stop's wrap chain (its order: ctr_wrap_chain.h).
ARGCRASH_LATE void CTR_WATCHDOGSTOP_ARGCRASH(void) {
    if (pendingScreenActive || argcrashInCrash) {
        __real_CtrWatchdog_watch(nullptr);
        return;
    }
    CTR_WATCHDOGSTOP_AFTER_ARGCRASH();
}

ARGCRASH_LATE const char* __wrap_CtrHbldr_setNext(const char* path, const char* const* args, int count) {
#if !defined(NDEBUG) || defined(CTRPORT_CRASH_TEST)
    if (argcrashChainLab) return CTR_HBLDR_AFTER_ARGCRASH(path, args, count);
#endif
    if (!realLabMode) return strNormalPlay;
    return CTR_HBLDR_AFTER_ARGCRASH(path, args, count);
}

ARGCRASH_LATE void __wrap_CtrLab_exiting(int seconds) {
    if (realLabMode) __real_CtrLab_exiting(seconds);
}

ARGCRASH_LATE void __wrap_CtrLab_poll(void) {
    bool isMain = (getThreadLocalStorage() == argcrashMainTls);
    if (isMain) argcrashMainParked = true;
    CTR_LABPOLL_AFTER_ARGCRASH();
    if (isMain) argcrashMainParked = false;
}

ARGCRASH_LATE void __wrap_CtrLog_emergency(const char* text, int waitMs) {
    __real_CtrLog_emergency(text, waitMs);
    if (argcrashGfxReady && !cleanExitStarted) {
        R216_PICK(argcrashPaintGated, argcrashPaintScreens)(argcrashClassifyReport(text), false);
    }
}

ARGCRASH_LATE void __wrap_LightEvent_Init(LightEvent* event, ResetType reset_type) {
    if (inDefaultLabStart && labLeaveEvent == nullptr) labLeaveEvent = event;
    __real_LightEvent_Init(event, reset_type);
}

ARGCRASH_LATE void __wrap_LightEvent_Signal(LightEvent* event) {
    if (argcrashNormalGuard && event != nullptr && event == labLeaveEvent) {
        deferredLeaveEvent = event;
        return;
    }
    __real_LightEvent_Signal(event);
}

ARGCRASH_LATE void __wrap_platformLog(const logType type, const char* format, va_list va) {
    if (format != nullptr) {
        if (type == LOG_TYPE_ERROR) {
            if (startsWith(format, strLogFatal)) {
                if (strstr(format, strBytecodeVer) != nullptr) lastErrorKind = CRASH_DATAVER;
                else if (strstr(format, strLogFailedRead) != nullptr) lastErrorKind = CRASH_DATAWIN;
                else lastErrorKind = CRASH_OOM;
            } else if (startsWith(format, strLogVm) || startsWith(format, strLogGameError)) {
                lastErrorKind = CRASH_VM;
            } else if (lastErrorKind == CRASH_NONE) {
                lastErrorKind = CRASH_ABORT;
            }
        } else if (type == LOG_TYPE_NORMAL && strcmp(format, strLogExitFmt) == 0) {
            cleanExitStarted = true;
            if (pendingScreenActive) R216_PICK(argcrashPaintGated, argcrashPaintScreens)(pendingScreenKind, true);
        }
    }

    if ((type == LOG_TYPE_NORMAL || type == LOG_TYPE_DEBUG) &&
        !(realLabMode || argcrashDiagFiles || CtrBench_running() || !CtrScreens_otherOff)) {
        if (type == LOG_TYPE_DEBUG || (format != nullptr && matchesAnyPrefix(format, dropPrefixes, DROP_PREFIX_COUNT))) {
            return;
        }
    }
    __real_platformLog(type, format, va);
}

ARGCRASH_LATE Result __wrap_svcOutputDebugString(const char* str, s32 length) {
    Result rc = ARGCRASH_SD_DEBUG_OUT(str, length); // UTSCOURD-001
    if (str != nullptr) {
        if (startsWith(str, strErrFatalPrefix)) {
            pendingScreenActive = true;
            if (strstr(str, strSavingFailed) != nullptr) pendingScreenKind = CRASH_SAVE;
            else if (strstr(str, strNew3ds) != nullptr) pendingScreenKind = CRASH_MODEL;
            else if (strstr(str, strBytecodeVer) != nullptr) pendingScreenKind = CRASH_DATAVER;
            else pendingScreenKind = CRASH_DATAWIN;
            writeCrashFile(str);
        } else if (startsWith(str, strErrOomPrefix)) {
            pendingScreenActive = true;
            pendingScreenKind = CRASH_OOM;
            writeCrashFile(str);
        }
    }
    return rc;
}

ARGCRASH_LATE bool __wrap_aptMainLoop(void) {
    argcrashGfxReady = true;
    if (pendingScreenActive && pendingScreenKind != CRASH_NONE) {
        if (!pendingScreenPainted) {
            pendingScreenPainted = true;
#if !defined(NDEBUG) || defined(CTRPORT_CRASH_TEST)
            argcrashReadTestFile();
#endif
            R216_PICK(argcrashPaintGated, argcrashPaintScreens)(pendingScreenKind, false);
#if !defined(NDEBUG) || defined(CTRPORT_CRASH_TEST)
            if (argcrashTestKind[0] != '\0') {
                const CrashDesc* d = &crashDescs[(unsigned int) pendingScreenKind < CRASH_KIND_COUNT ? pendingScreenKind : CRASH_NONE];
                logInfo(strTestSummaryFmt, d->code, d->title, (unsigned long) checkCrashTxtSize());
                CtrLog_flush();
            }
#endif
        }
#if !defined(NDEBUG) || defined(CTRPORT_CRASH_TEST)
        if (argcrashTestAutoExit && ++argcrashAutoExitFrames >= 30) {
            R216_PICK(argcrashPaintGated, argcrashPaintScreens)(pendingScreenKind, true);
            return false;
        }
#endif
    }
#if !defined(NDEBUG) || defined(CTRPORT_CRASH_TEST)
    if (argcrashTestFired && strcmp(argcrashTestKind, strTestGameEnd) == 0 &&
        argcrashRunner != nullptr && argcrashRunner->shouldExit) {
        if (argcrashGameEndStartTick == 0) {
            argcrashGameEndStartTick = svcGetSystemTick();
        } else if (argcrashTestAutoExit &&
                   svcGetSystemTick() - argcrashGameEndStartTick >= 65ULL * (u64) SYSCLOCK_ARM11) {
            logInfo(strTestGameEndHeldLog);
            CtrLog_flush();
            return false;
        }
    }
    argcrashRunTestTrigger();
#endif
    return R216_PICK(homeaudioEnter, __real_aptMainLoop)();
}

ARGCRASH_LATE void __wrap_svcBreak(UserBreakType breakReason) {
    lastErrorKind = CRASH_BREAK;
    argcrashFail(strReasonBreak, strDetailBreak);
    __real_svcBreak(breakReason);
}

ARGCRASH_LATE void __wrap_exit(int status) {
    if (!cleanExitStarted && !pendingScreenActive) {
        if (lastErrorKind == CRASH_NONE) lastErrorKind = CRASH_ABORT;
        argcrashFail(strReasonExit, status == 0 ? strDetailExit0 : strDetailExit1);
    }
    __real_exit(status);
}

// ---- SCOURS4-004 on UT Old's frozen late layout (RULES 3adf7ac34: the release link's freebytes H1 holes have no
// pad, so late code there grows only append-only). argcrashPaintScreens paints each framebuffer with
// argcrashPaintFixed, which calls the old paintBuffer only where that returns at once (no framebuffer). The new
// function sits between the old caller and the old callee, so GCC's call graph keeps its order and every old
// function, constant and static keeps its place and size (only the call's target changes). Own section, appended in
// late_reserve.ld. SCOURS4-005 (the crash screen stops the loading bar) is the next block (R216b; UNDERTALE only).
#if defined(__3DS__) && defined(__GNUC__)
#define ARGCRASH_FIX __attribute__((noinline, noipa, section(".text_argcrashfix")))
#else
#define ARGCRASH_FIX __attribute__((noinline, noipa))
#endif

// SCOURS4-004: paintBuffer, but a line breaks at a space when the next word would end past the rule, not only past
// ruleW - 48, and no pixel lands past the edge. With no framebuffer it calls paintBuffer, which then returns at once
// (that call keeps paintBuffer's place, above). It paints the left buffer only (argcrashPaintScreens presents
// mono). The console font is libctru's default one (nothing here sets another): its glyphs are there, 256 from
// code 0; each line's text is set.
ARGCRASH_FIX static void argcrashPaintFixed(u8* fb, u16 fbW, u16 fbH, GSPGPU_FramebufferFormat fmt, bool isTop,
                                            ArgcrashKind kind, bool exiting) {
    if (fb == nullptr || fbW == 0 || fbH == 0) {
        paintBuffer(fb, fbW, fbH, fmt, isTop, kind, exiting);
        return;
    }
    curFbPtr = fb;
    curFbH = fbH;
    curFbFmt = fmt;
    u32 fbBytes = (u32) fbW * (u32) fbH * (fmt == GSP_BGR8_OES ? 3u : 2u);
    memset(fb, 0, fbBytes);

    int textX = isTop ? 28 : 4;
    int ruleW = isTop ? 344 : 312;
    for (int x = textX; x < textX + ruleW; x++) {
        putPixel(x, 30, 0xFFFFFFu);
        putPixel(x, 86, 0x505868u);
        putPixel(x, 166, 0x505868u);
        putPixel(x, 204, 0xFFFFFFu);
    }

    if ((unsigned int) kind >= CRASH_KIND_COUNT) kind = CRASH_NONE;
    const CrashDesc* d = &crashDescs[kind];
    const char* title = d->title;
#ifdef CTRPORT_TARGET_O3DS
    if (kind == CRASH_OOM && envIsHomebrew() && CTR_OOM_CIA_HINT_BELOW > __ctru_heap_size + __ctru_linear_heap_size) {
        title = strTitleOomCia;
    }
#endif
    const char* dynText[3] = {
        title,
        d->code,
        exiting ? strReturningHome : strPressStart,
    };

    const ConsoleFont* font = &consoleGetDefault()->font;
    for (size_t i = 0; i < 8; i++) {
        const char* s = (i < 5) ? screenLines[i].text : dynText[i - 5];
        u32 rgb = (i == 7 && exiting) ? 0x80FF60u : screenLines[i].rgb;
        int y0 = screenLines[i].y;
        for (int x = textX + screenLines[i].dx; *s != '\0'; s++, x += 8) {
            if (*s == ' ') { // break here past ruleW - 48, or when the next word would end past the rule
                const char* w = s + 1;
                while (*w != ' ' && *w != '\0') w++;
                if (x > textX + ruleW - 48 || x + 8 * (int) (w - s) > textX + ruleW) {
                    x = textX + 8;
                    y0 += 12;
                    continue;
                }
            }
            const u8* glyph = font->gfx + ((unsigned char) *s - font->asciiOffset) * 8u;
            for (int r = 0; r < 8; r++) {
                u8 bits = glyph[r];
                for (int c = 0; c < 8; c++) {
                    // A glyph past the edge never leaves the buffer (putPixel doubles x on the wide top).
                    if ((bits & (0x80u >> c)) != 0 && 240u > (unsigned) (y0 + r) &&
                        curFbH > (unsigned) ((x + c) << (curFbH == 800u)))
                        putPixel(x + c, y0 + r, rgb);
                }
            }
        }
    }

    GSPGPU_FlushDataCache(fb, fbBytes);
}
// ---- UTSCOURD-001 (change auditfixe2), UNDERTALE only: a stop because the SD card couldn't be read gets its own title.
// The overlay reader's stop (z_overlayread.c's overlayreadUnreadable: a save that's there but can't be read) and the
// data.win check's (z_datarefuse.c's datarefuseUnreadable) reach __wrap_svcOutputDebugString as "[ERROR] Fatal: ..."
// lines with neither "Saving failed", "New 3DS" nor "bytecode version", so they are CRASH_DATAWIN, whose title
// ("Game files are missing. Run the builder again.") is wrong advice for a card that can't be read. Append-only
// (RULES 3adf7ac34): z_argcrash_sd.h, included on a line that was blank, sends that wrap's call of the real
// svcOutputDebugString to argcrashSdOut and argcrashPaintScreens's call of argcrashPaintFixed to argcrashPaintSd.
// No kind is added, so crashDescs and every old constant, static and function keep their place and size; the old
// classification is untouched (a line with "Saving failed", "New 3DS" or "bytecode version" keeps its title). Own
// sections, appended in late_reserve.ld. The screen's other old statics are reached by their assembler names, so
// nothing here changes where GCC puts them.
#ifdef ENABLE_WAD16
#if defined(__3DS__) && defined(__GNUC__)
#define ARGCRASH_SD __attribute__((noinline, noipa, section(".text_utsdread")))
#define ARGCRASH_SD_RODATA __attribute__((section(".rodata.utsdread_in_text")))
#define ARGCRASH_SD_BSS __attribute__((section(".bss.utsdread_late")))
#else
#define ARGCRASH_SD
#define ARGCRASH_SD_RODATA
#define ARGCRASH_SD_BSS
#endif
#define ARGCRASH_SD_STR(name, text) static const char name[(sizeof(text) + 3u) & ~3u] ARGCRASH_SD_RODATA = text

ARGCRASH_SD_STR(strSdFatalPrefix, "[ERROR] Fatal: ");
// The two messages' own words (one line of each, so fatalSummary's line joins can't split them); a test pins them
// to z_overlayread.c and z_datarefuse.c.
ARGCRASH_SD_STR(strSdOverlay, "from the SD card. It wasn't changed.");
ARGCRASH_SD_STR(strSdDataWin, "data.win couldn't be read to check it.");
ARGCRASH_SD_STR(strSdTitle, "* The SD card couldn't be read. Check the card and try again.");
ARGCRASH_SD_STR(strSdCode, "ERR-SD-READ");

// Whether the latest "[ERROR] Fatal: " line was an SD read (set before that line's kind, both before any paint).
static volatile bool argcrashSdRead ARGCRASH_SD_BSS;

extern const ScreenLine argcrashSdLines[8] __asm__("screenLines");
extern const char argcrashSdPressStart[] __asm__("strPressStart");
extern const char argcrashSdQuitting[] __asm__("strReturningHome");

ARGCRASH_SD static Result argcrashSdOut(const char* str, s32 length) {
    Result rc = __real_svcOutputDebugString(str, length);
    if (str != nullptr && strncmp(str, strSdFatalPrefix, sizeof("[ERROR] Fatal: ") - 1) == 0) {
        argcrashSdRead = strstr(str, strSdOverlay) != nullptr || strstr(str, strSdDataWin) != nullptr;
    }
    return rc;
}

// putPixel on the given framebuffer (the old one draws on curFbPtr, which this painter leaves alone).
ARGCRASH_SD static void argcrashSdPixel(u8* fb, u16 fbH, GSPGPU_FramebufferFormat fmt, int x, int y, u32 rgb) {
    int wide = (fbH == 800u);
    int col = x << wide;
    u8 r = (u8) (rgb >> 16), g = (u8) (rgb >> 8), b = (u8) rgb;
    u16 rgb565 = (u16) (((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
    for (int n = 0; n <= wide; n++, col++) {
        int idx = col * 240 + (239 - y);
        if (fmt == GSP_BGR8_OES) {
            u8* p = fb + idx * 3;
            p[0] = b;
            p[1] = g;
            p[2] = r;
        } else {
            ((u16*) fb)[idx] = rgb565;
        }
    }
}

// argcrashPaintFixed's screen with the SD card title and code, for CRASH_DATAWIN after an SD read's fatal line;
// anything else (and no framebuffer) goes to argcrashPaintFixed as before.
ARGCRASH_SD static void argcrashPaintSd(u8* fb, u16 fbW, u16 fbH, GSPGPU_FramebufferFormat fmt, bool isTop,
                                        ArgcrashKind kind, bool exiting) {
    if (kind != CRASH_DATAWIN || !argcrashSdRead || fb == nullptr || fbW == 0 || fbH == 0) {
        argcrashPaintFixed(fb, fbW, fbH, fmt, isTop, kind, exiting);
        return;
    }
    u32 fbBytes = (u32) fbW * (u32) fbH * (fmt == GSP_BGR8_OES ? 3u : 2u);
    memset(fb, 0, fbBytes);

    int textX = isTop ? 28 : 4;
    int ruleW = isTop ? 344 : 312;
    for (int x = textX; x < textX + ruleW; x++) {
        argcrashSdPixel(fb, fbH, fmt, x, 30, 0xFFFFFFu);
        argcrashSdPixel(fb, fbH, fmt, x, 86, 0x505868u);
        argcrashSdPixel(fb, fbH, fmt, x, 166, 0x505868u);
        argcrashSdPixel(fb, fbH, fmt, x, 204, 0xFFFFFFu);
    }

    const char* dynText[3] = {
        strSdTitle,
        strSdCode,
        exiting ? argcrashSdQuitting : argcrashSdPressStart,
    };

    const ConsoleFont* font = &consoleGetDefault()->font;
    for (size_t i = 0; i < 8; i++) {
        const char* s = (i < 5) ? argcrashSdLines[i].text : dynText[i - 5];
        u32 rgb = (i == 7 && exiting) ? 0x80FF60u : argcrashSdLines[i].rgb;
        int y0 = argcrashSdLines[i].y;
        for (int x = textX + argcrashSdLines[i].dx; *s != '\0'; s++, x += 8) {
            if (*s == ' ') { // break here past ruleW - 48, or when the next word would end past the rule
                const char* w = s + 1;
                while (*w != ' ' && *w != '\0') w++;
                if (x > textX + ruleW - 48 || x + 8 * (int) (w - s) > textX + ruleW) {
                    x = textX + 8;
                    y0 += 12;
                    continue;
                }
            }
            const u8* glyph = font->gfx + ((unsigned char) *s - font->asciiOffset) * 8u;
            for (int r = 0; r < 8; r++) {
                u8 bits = glyph[r];
                for (int c = 0; c < 8; c++) {
                    // A glyph past the edge never leaves the buffer (the pixel doubles x on the wide top).
                    if ((bits & (0x80u >> c)) != 0 && 240u > (unsigned) (y0 + r) &&
                        fbH > (unsigned) ((x + c) << (fbH == 800u)))
                        argcrashSdPixel(fb, fbH, fmt, x + c, y0 + r, rgb);
                }
            }
        }
    }

    GSPGPU_FlushDataCache(fb, fbBytes);
}

// ---- UTSCOURD F4 (change auditfixe2; regaudit F4): the data.win check's stop is an SD read only when the card failed.
// When its 64 KB read buffer couldn't be allocated (ctr_data_version.c's CtrDataVersion_buffer), the check answers
// CTR_DATA_UNREADABLE and z_datarefuse.c stops with the same message as for a card that can't be read; that screen
// gets CRASH_OOM's title and code (the console ran out of memory, or the .cia hint), as any other out-of-memory stop.
// Every other screen goes to argcrashPaintSd unchanged. Own section, appended (late_reserve.ld: .text_utsdmem).
extern volatile bool ctrDataVersionNoMemory;
#if defined(__3DS__) && defined(__GNUC__)
#define ARGCRASH_MEM __attribute__((noinline, noipa, section(".text_utsdmem")))
#else
#define ARGCRASH_MEM
#endif
ARGCRASH_MEM static void argcrashPaintMem(u8* fb, u16 fbW, u16 fbH, GSPGPU_FramebufferFormat fmt, bool isTop,
                                          ArgcrashKind kind, bool exiting) {
    if (kind == CRASH_DATAWIN && argcrashSdRead && ctrDataVersionNoMemory) kind = CRASH_OOM;
    argcrashPaintSd(fb, fbW, fbH, fmt, isTop, kind, exiting);
}
#include "z_fatalgfx.inc"
#endif

// R333: fatal exitStep("done") also logs after gfxExit. The shared painter must
// become unavailable before graphics are released, including every fatal kind.
// Own appended sections preserve all old bodies, constants and static addresses.
#ifdef ENABLE_WAD16
#define SAVEAUDIT_FATAL __attribute__((noinline, noipa, section(".text_saveauditfatal")))
#define SAVEAUDIT_FATAL_RO __attribute__((section(".rodata_saveauditfatal")))
static volatile bool saveauditFolderFatal __attribute__((section(".bss.saveauditfatal_late")));
static const char saveauditFatalPrefix[] SAVEAUDIT_FATAL_RO = "[ERROR] Fatal: ";
static const char saveauditFolderWords[] SAVEAUDIT_FATAL_RO = "The SD card or saves folder";
static const char saveauditFolderTitle[] SAVEAUDIT_FATAL_RO =
    "* SD card or saves folder unreadable. Saves left alone.";
static const char saveauditFolderCode[] SAVEAUDIT_FATAL_RO = "ERR-SAVE-READ";

SAVEAUDIT_FATAL static Result saveauditFatalOut(const char* str, s32 length) {
    Result rc = argcrashSdOut(str, length);
    if (str != nullptr && startsWith(str, saveauditFatalPrefix))
        saveauditFolderFatal = strstr(str, saveauditFolderWords) != nullptr;
    return rc;
}

SAVEAUDIT_FATAL static void saveauditFatalPaint(u8* fb, u16 fbW, u16 fbH, GSPGPU_FramebufferFormat fmt,
                                               bool isTop, ArgcrashKind kind, bool exiting) {
    if (kind != CRASH_DATAWIN || !saveauditFolderFatal || fb == nullptr || fbW == 0 || fbH == 0) {
        argcrashPaintMem(fb, fbW, fbH, fmt, isTop, kind, exiting);
        return;
    }
    u32 bytes = (u32) fbW * fbH * (fmt == GSP_BGR8_OES ? 3u : 2u);
    memset(fb, 0, bytes);
    int textX = isTop ? 28 : 4;
    int ruleW = isTop ? 344 : 312;
    for (int x = textX; x < textX + ruleW; x++) {
        argcrashSdPixel(fb, fbH, fmt, x, 30, 0xFFFFFFu);
        argcrashSdPixel(fb, fbH, fmt, x, 86, 0x505868u);
        argcrashSdPixel(fb, fbH, fmt, x, 166, 0x505868u);
        argcrashSdPixel(fb, fbH, fmt, x, 204, 0xFFFFFFu);
    }
    const char* dynText[3] = {saveauditFolderTitle, saveauditFolderCode,
                            exiting ? argcrashSdQuitting : argcrashSdPressStart};
    const ConsoleFont* font = &consoleGetDefault()->font;
    for (size_t i = 0; i < 8; i++) {
        const char* s = i < 5 ? argcrashSdLines[i].text : dynText[i - 5];
        u32 rgb = i == 7 && exiting ? 0x80FF60u : argcrashSdLines[i].rgb;
        int y = argcrashSdLines[i].y;
        for (int x = textX + argcrashSdLines[i].dx; *s != '\0'; s++, x += 8) {
            if (*s == ' ') {
                const char* word = s + 1;
                while (*word != ' ' && *word != '\0') word++;
                if (x > textX + ruleW - 48 || x + 8 * (int) (word - s) > textX + ruleW) {
                    x = textX + 8;
                    y += 12;
                    continue;
                }
            }
            const u8* glyph = font->gfx + ((unsigned char) *s - font->asciiOffset) * 8u;
            for (int r = 0; r < 8; r++) {
                for (int c = 0; c < 8; c++) {
                    if ((glyph[r] & (0x80u >> c)) != 0 && 240u > (unsigned) (y + r) &&
                        fbH > (unsigned) ((x + c) << (fbH == 800u)))
                        argcrashSdPixel(fb, fbH, fmt, x + c, y + r, rgb);
                }
            }
        }
    }
    GSPGPU_FlushDataCache(fb, bytes);
}
#endif
#line 1311

#ifdef ENABLE_WAD16 // SCOURS4-005, R216b..R216c: UNDERTALE only (ctr_r216_ut.h)
#line 1312
// ---- SCOURS4-005 (R188e..R188i; R216b: ported to the 1.0 layout, and c26rev2 R216 finding 1). Every crash-screen
// paint call (above) goes to argcrashPaintGated, which calls argcrashPaintScreens. Own sections after every other
// late input of the release builds (late_reserve.ld: .text_argcrashgate, .bss.argcrashfix_late), so no earlier late
// code, constant or static moves: the old callers differ only in their calls' targets.
#if defined(__3DS__) && defined(__GNUC__)
#define ARGCRASH_GATE __attribute__((noinline, noipa, section(".text_argcrashgate")))
#define ARGCRASH_FIX_BSS __attribute__((section(".bss.argcrashfix_late")))
#else
#define ARGCRASH_GATE __attribute__((noinline, noipa))
#define ARGCRASH_FIX_BSS
#endif

bool CtrLoadscreen_halt(void);    // z_loadscreen.c: the loading bar stops; true once no draw on another thread is live
bool CtrLoadscreen_drawing(void); // z_loadscreen.c: a bar draw is still live (no wait)
u32 CtrLab_fatalAt(void);         // ctr_lab.c: the fatal claim's clock for the save guard (0 before the claim)
bool CtrRosalina_quiet(void); bool CtrDimlight_crashQuiet(void); // restore before claiming the crash painter
void CtrArgcrash_depart(ArgcrashKind kind, void (*paint)(ArgcrashKind, bool)); // R216c: file end

// An ordinary bar draw is one frame: the bar into the back buffer and a swap, no wait for the GPU or VBlank, so a few
// ms (with the loadshot.txt test hook a draw also writes a picture to the card first). One that holds the gate for a
// second (30 frames at the bar's 33 ms) is taken as stuck: waiting for it more would leave the crash unseen (a dead
// bar is not worth a blank crash screen). Ticks (u32: a difference is exact up to 16 s). After the takeover the bar
// can't show anything new: no draw starts once the halt has come, and the one still live skips its swap
// (z_loadscreen.c lsSwapGated). A swap already past its check may still present once, the crash screen's slot (double
// buffering is off by then), at worst with an old right eye in 3D; that and its pixel writes already under way are
// covered by the next repaint (R188i: no wait on the crash path is unbounded, so a frame can be stale, never dark).
#define ARGCRASH_STUCK_TICKS ((u32) SYSCLOCK_ARM11)
static u32 argcrashStuckTick ARGCRASH_FIX_BSS; // the first refusal's tick (odd: at most 1 early, never 0); else 0
static bool argcrashOverStuck ARGCRASH_FIX_BSS; // the last paint went over a stuck draw
static void* argcrashPainter ARGCRASH_FIX_BSS;  // the thread that paints (its TLS pointer), until it parks for good
// pendingScreenActive, pendingScreenPainted and argcrashMainTls (above) by their link names: GCC places a file's
// statics in the order its functions first use them, so naming them here would move them.
extern volatile bool argcrashPendingActive __asm__("pendingScreenActive");
extern volatile bool argcrashPendingPainted __asm__("pendingScreenPainted");
extern const void* argcrashMainTlsRef __asm__("argcrashMainTls");

// SCOURS4-005 (R188e..R188i): every crash-screen paint ends up on screen without painting into a frame a live draw
// owns, and nothing on the way waits without a bound. R188h (c26rev2 R188g): one thread paints at a time. The first
// to claim argcrashPainter (ldrex/strex) keeps it, with no handoff, while it can still run, and alone owns the state
// below; it may come back in (a fault inside its own paint). It lets go only when it parks for good (R216b:
// CtrArgcrash_labSleep, below). Every other thread's call returns without painting. The halt stops the bar and waits up
// to 50 ms for a draw already started. While a draw is still live this asks again, up to a second from the first
// refusal, and paints as soon as it ends. A draw that holds the gate past the second is stuck (its thread may be
// parked for good inside its swap: c26rev2 R188h finding 1): the crash screen paints over it (the repaints too). The
// pending screen's paint (__wrap_aptMainLoop asks once, each frame until it has) is asked for again while the stuck
// draw holds the gate (no wait: the gate is only read) and paints once more when it ends, in case the draw's last
// pixels covered it.
// The backlight goes on a gsp::Lcd session of this call's own, best-effort (as ctr_rosalina.c's rosaBacklight):
// never libctru's shared gspLcdInit session, which another thread may be opening (c26rev2 R188g finding 2). The
// painter lights after its frames are presented; every other thread's call lights at once, without painting, so a
// painter held anywhere can't keep presented frames dark (R188h finding 2). Lit before the first frame, the screens
// show the old picture until it comes. The srv and gsp::Lcd requests have no deadline of the caller's: lighting is
// best-effort, as accepted (R216).
// R216b4: after the claim, a paint fixes the save guard's clock if nothing has yet (ctr_lab.c's end: the clock read
// on the claimer's way in; no look after the claim moves it). R240 F1 (c26rev6): first, ctr_rosalina.c's watcher is
// stopped (CtrRosalina_quiet: a flag, then a wait of at most 100 ms for it to end), so a Rosalina combo can no longer
// clear the bottom error or switch the bottom off: this light goes past ctr_screens.c's dark mark, which the watcher
// reads.
ARGCRASH_GATE static void argcrashPaintGated(ArgcrashKind kind, bool exiting) {
    bool quiet = R216_PICK(CtrDimlight_crashQuiet, CtrRosalina_quiet)();
    (void) CtrLab_fatalAt();
    bool pending = argcrashPendingActive && !exiting; // the pending screen's paint: asked for again each frame
    void* self = getThreadLocalStorage();
    void* owner = nullptr;
    bool painted = false;
    bool mine = __atomic_compare_exchange_n(&argcrashPainter, &owner, self, false, __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED) || owner == self;
    if (mine) {
        bool clear = false;
        if (!argcrashOverStuck || !CtrLoadscreen_drawing()) {
            while (!(clear = CtrLoadscreen_halt())) {
                if (argcrashStuckTick == 0) argcrashStuckTick = ((u32) svcGetSystemTick() - 1u) | 1u; // <= now, not 0
                if (ARGCRASH_STUCK_TICKS <= (u32) svcGetSystemTick() - argcrashStuckTick) break;
            }
        }
        painted = clear || !argcrashOverStuck || !pending;
        if (painted) argcrashPaintScreens(kind, exiting);
        // R216c: the departing screen has no later frame to repair it (file end).
        if (painted && exiting && !quiet) CtrArgcrash_depart(kind, argcrashPaintScreens);
        argcrashOverStuck = !clear;
        if (clear) argcrashStuckTick = 0;
        // Over a stuck draw: the pending paint once more. Only the game thread (any thread before it is marked: the
        // model check's screen comes first) asks: it alone runs __wrap_aptMainLoop, which owns the flag.
        else if (pending && (argcrashMainTlsRef == nullptr || self == argcrashMainTlsRef)) argcrashPendingPainted = false;
    }
    if (painted || !mine) {
        u32 name[3]; // "gsp::Lcd", stored word by word from the literal pool (a string or initializer adds rodata)
        name[0] = 0x3A707367u;
        name[1] = 0x64634C3Au;
        name[2] = 0;
        Handle lcd;
        if (R_SUCCEEDED(srvGetServiceHandle(&lcd, (const char*) name))) {
            u32* cmd = getThreadCommandBuffer();
            cmd[0] = IPC_MakeHeader(0x11, 1, 0); // gsp::Lcd PowerOnBacklight
            cmd[1] = GSPLCD_SCREEN_BOTH;
            svcSendSyncRequest(lcd);
            svcCloseHandle(lcd);
        }
    }
}

// R188g: argcrashPaintScreens's backlight call, which now never runs there (the call's target only, so that function
// keeps its size): argcrashPaintGated sets the backlight on a session of its own (R188h, R188i).
ARGCRASH_GATE static Result argcrashLcdSkip(void) {
    return -1;
}

// R216b (c26rev2 R216 finding 1): the painter keeps argcrashPainter only while its thread can still run. Every
// svcSleepThread in ctr_lab.c is this (a rename at that file's top), which sleeps the same way; a sleep of a second
// or more is one of its sleeps for good: standAside's (a thread that faults, or calls CtrLab_fail, while another
// thread leaves), CtrLab_poll's (the game thread stopped for another thread's leave), and fallbackMain's 25 s before
// the exit (its thread never paints). After any of them the thread only sleeps again or ends the run.
// If the sleeping thread holds the token, it lets it go here (a compare-exchange from its own TLS pointer, with
// release order, so its writes to the state above come first), and the thread that leaves, the fatal report's
// owner, paints on its next call (argcrashEmergency repaints each 30 frames). Before that every call of the leaver
// lit the screens without painting, and the old picture stayed (R216: A claims the token, is preempted before its
// first frame, B faults and takes the leave, then A faults and parks in standAside). The parked thread never runs
// paint code again, so two threads still never paint at once; its paint may have stopped halfway (a fault inside
// it), and every paint draws both screens from the start. No wait; nothing else changes for a thread that holds no
// token.
ARGCRASH_GATE void CtrArgcrash_labSleep(s64 ns) {
    if (ns >= 1000000000LL) {
        void* self = getThreadLocalStorage();
        __atomic_compare_exchange_n(&argcrashPainter, &self, nullptr, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
    svcSleepThread(ns);
}

// R216d: this block's #ifdef ENABLE_WAD16 now opens at the SCOURS4-005 header above
// R216b5 (c26rev2 R216b4 finding 2; UNDERTALE only, as ctr_rosalina.c's part): argcrashPaintGated's first call is
// this (CMakeLists.txt renames its CtrRosalina_quiet call). It stops the Rosalina watcher as before and says whether
// the watcher is quiet (R216c: argcrashPaintGated keeps the answer). While the watcher is not yet quiet (being
// created, or still running past the 100 ms wait), a clear it had already checked for can still land after this call's
// paint, so the pending screen's paint (__wrap_aptMainLoop's one-shot screen, which the game thread alone asks for and
// owns the flag of) is asked for again. That repairs only while frames keep coming: each frame repaints until the
// watcher is quiet, and the frame after that paints once more, after its end. A watcher that never ends is repainted
// over each frame, about 10 Hz (each wait is 100 ms); argcrashEmergency's loop repaints every 30 frames anyway. A
// paint with no frame after it, the departing screen's, is CtrArgcrash_depart's (below).
// The body is CtrArgcrash_quietBody, which nothing here calls, and the asm below names it CtrArgcrash_quiet (a
// link-time alias GCC doesn't see): argcrashPaintGated's call stays a call to an outside function, so this file's
// functions and statics keep their order (GCC orders them by calls and first uses).
#undef CtrRosalina_quiet
bool CtrRosalina_quiet(void); // ctr_rosalina.c: true once its watcher can send nothing more
ARGCRASH_GATE bool CtrArgcrash_quietBody(void) {
    bool quiet = CtrRosalina_quiet();
    if (!quiet && argcrashPendingActive &&
        (argcrashMainTlsRef == nullptr || getThreadLocalStorage() == argcrashMainTlsRef))
        argcrashPendingPainted = false;
    return quiet;
}

// R216c (c26rev2 R216b5 LOW): the departing screen ("returning to HOME": argcrashEmergency's last paint, and each of
// main.c exitAfterScreen's exit steps) is a paint after which no frame comes, so the pending paint asked for above is
// never made, and a clear the watcher had already checked for could land after it and stay (the bottom text erased,
// the backlight on). So when a departing paint finds the watcher not yet quiet, its painter (argcrashPaintGated, inside
// its claim, after the paint and before its backlight) serves the repaint itself while the departure waits: the stop
// again (at most 100 ms each), then the screen painted again, until a stop says the watcher has ended (that paint
// comes after its end) or one second has gone from the first such wait of the run (a deadline kept for the whole
// departure: the exit steps that follow each find it spent, and add only their own stop's 100 ms, as before). A
// watcher still running after that second (held inside a GSP request past its check) can still erase the departing
// screen's bottom text; the backlight ends lit (CtrRosalina_watchRun). Called only by the painter that holds the claim
// (argcrashPainter), so one thread alone reads and writes the deadline; the screen is painted through the pointer the
// gate passes (argcrashPaintScreens), so this file gets no new call to its own functions. An alias as above.
#define ARGCRASH_DEPART_BSS __attribute__((section(".bss_argdepart_late,\"aw\",%nobits @")))
u64 argcrashDepartBy ARGCRASH_DEPART_BSS; // the departure's repaint deadline in ticks; 0 before the first
ARGCRASH_GATE void CtrArgcrash_departBody(ArgcrashKind kind, void (*paint)(ArgcrashKind, bool)) {
    if (argcrashDepartBy == 0) argcrashDepartBy = svcGetSystemTick() + (u64) ARGCRASH_STUCK_TICKS;
    while (argcrashDepartBy > svcGetSystemTick()) {
        bool quiet = CtrRosalina_quiet();
        paint(kind, true);
        if (quiet) break;
    }
}
__asm__(".global CtrArgcrash_quiet\n"
        ".type CtrArgcrash_quiet, %function\n"
        ".set CtrArgcrash_quiet, CtrArgcrash_quietBody\n"
        ".global CtrArgcrash_depart\n"
        ".type CtrArgcrash_depart, %function\n"
        ".set CtrArgcrash_depart, CtrArgcrash_departBody\n");
#endif

// HOME: the sound stops with the picture (change homeaudio, maintainer 11-06: "the music keeps playing [when] the games screen
// is frozen"). The game thread stops making frames as it enters libctru's jump to the HOME Menu, but libctru puts the
// DSP to sleep only after PrepareToJumpToHomeMenu, every suspend hook, the system VRAM save and the screen hand-over.
// So the game's channels pause here first (ctr_audio_system.c's end has the why and the paths), and go on when the game
// is back. aptShouldJumpToHome is the test aptMainLoop itself makes. Close from the HOME Menu returns false: no resume,
// as the options menu's Quit. Not on a crash screen: it stops nothing, and a crash may have left an audio lock held.
// UNDERTALE only for 1.0, as the crash net above (parked for the DELTARUNE round, until DR work
// resume): DELTARUNE keeps its bytes. The two flags by their link names: naming the statics here would move them. The
// wrap calls an alias: seeing this function as a callee, GCC puts the wrap after __wrap_svcBreak and __wrap_exit.
#ifdef ENABLE_WAD16
#define HOMEAUDIO_CODE __attribute__((noinline, noipa, section(".text_homeaudio")))
void CtrAudio_homeFreeze(void); void mlHomeFreeze(void); // musicloop: cuts a stopped stream's fade, then CtrAudio_homeFreeze
void CtrAudio_homeThaw(void);
extern volatile bool homeaudioPending __asm__("pendingScreenActive");
extern volatile bool homeaudioInCrash __asm__("argcrashInCrash");

HOMEAUDIO_CODE bool homeaudioMainLoop(void) {
    if (homeaudioPending || homeaudioInCrash || !aptShouldJumpToHome()) return __real_aptMainLoop();
    mlHomeFreeze();
    bool running = __real_aptMainLoop();
    if (running) CtrAudio_homeThaw();
    return running;
}
__asm__(".global homeaudioEnter\n"
        ".type homeaudioEnter, %function\n"
        ".set homeaudioEnter, homeaudioMainLoop\n");
#endif
