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
void __real_LightEvent_Init(LightEvent* event, ResetType reset_type);
void __real_LightEvent_Signal(LightEvent* event);
void __real_platformLog(const logType type, const char* format, va_list va);
Result __real_svcOutputDebugString(const char* str, s32 length);
bool __real_aptMainLoop(void);
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
ARGCRASH_STR(strTitleDataVer, "* This game version isn't supported. Run the builder again.");

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
//   [perf], Slow frame, Files (room): / Files:, [rl], [code], [mem], "Textures:" and all bench/profile lines.
ARGCRASH_STR(strTagSkip, "[skip");
ARGCRASH_STR(strTagRender, "[render]");
ARGCRASH_STR(strTagSmartCount, "[smartcount");
ARGCRASH_STR(strTagFill, "[fill]");
ARGCRASH_STR(strTagPace, "[pace]");
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
    { 176, 146, 0xFFA860u, nullptr },
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

ARGCRASH_LATE static void argcrashPaintScreens(ArgcrashKind kind, bool exiting) {
    if (!argcrashGfxReady) return;
    CtrLog_setConsoleEnabled(false);
    const devoptab_t* current = devoptab_list[STD_OUT];
    if (current != nullptr && current != &argcrashNullOut) {
        argcrashNullOut = *current;
        argcrashNullOut.write_r = nullWrite;
        devoptab_list[STD_OUT] = &argcrashNullOut;
    }
    if (R_SUCCEEDED(gspLcdInit())) {
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
            if (fb != prevFb) paintBuffer(fb, fbW, fbH, fmt, s == 0, kind, exiting);
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
        argcrashPaintScreens(kind, false);
        const CrashDesc* d = &crashDescs[(unsigned int) kind < CRASH_KIND_COUNT ? kind : CRASH_NONE];
        char msg[192];
        snprintf(msg, sizeof(msg), strTestSummaryFmt, d->code, d->title, (unsigned long) checkCrashTxtSize());
        __real_CtrLog_emergency(msg, 500);
    } else
#endif
    {
        __real_CtrLog_emergency(report, waitMs);
        argcrashPaintScreens(kind, false);
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
        if ((++frames % 30) == 0) argcrashPaintScreens(kind, false);
#if !defined(NDEBUG) || defined(CTRPORT_CRASH_TEST)
        if (argcrashTestAutoExit && frames >= 30) break;
#endif
    }
    argcrashPaintScreens(kind, true);
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
        argcrashPaintScreens(argcrashClassifyReport(text), false);
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
            if (pendingScreenActive) argcrashPaintScreens(pendingScreenKind, true);
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
    Result rc = __real_svcOutputDebugString(str, length);
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
            argcrashPaintScreens(pendingScreenKind, false);
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
            argcrashPaintScreens(pendingScreenKind, true);
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
    return __real_aptMainLoop();
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
