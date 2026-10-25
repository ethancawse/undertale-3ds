#include "ctr_apt_trace.h"

#include <3ds.h>
#include <citro3d.h>

#include "common.h"
#include "ctr_log.h"
#include "log.h"

// The console froze on real hardware between the app's suspend hook and the
// HOME Menu appearing, inside libctru's aptJumpToHomeMenu. These wrappers
// (linked with -Wl,--wrap, see CMakeLists.txt) log each step it takes from
// there, flushing as they go, so the last line in log.txt names the step that
// never returned:
//   citro3d waits for the GPU queue -> GSP saves its VRAM area -> APT hands
//   the screens to the HOME Menu -> NDSP puts the DSP to sleep -> GSP gives
//   up the GPU -> HOME Menu.

static volatile bool tracing = false;
static bool hooked = false;
static aptHookCookie aptCookie;
static dspHookCookie dspCookie;

Result __real_GSPGPU_SaveVramSysArea(void);
Result __real_GSPGPU_ImportDisplayCaptureInfo(GSPGPU_CaptureInfo* captureinfo);
Result __real_GSPGPU_ReleaseRight(void);
Result __real_GSPGPU_AcquireRight(u8 flags);
bool __real_gxCmdQueueWait(gxCmdQueue_s* queue, s64 timeout);

static void step(const char* text) {
    logInfo("APT: %s\n", text);
    CtrLog_flush();
}

Result __wrap_GSPGPU_SaveVramSysArea(void) {
    if (tracing) step("saving the system VRAM area");
    Result res = __real_GSPGPU_SaveVramSysArea();
    if (tracing) step("saved; handing the screens over");
    return res;
}

Result __wrap_GSPGPU_ImportDisplayCaptureInfo(GSPGPU_CaptureInfo* captureinfo) {
    Result res = __real_GSPGPU_ImportDisplayCaptureInfo(captureinfo);
    if (tracing) {
        logInfo("APT: screen capture info: top %p format 0x%lX, bottom %p format 0x%lX\n", captureinfo->screencapture[0].framebuf0_vaddr,
                (unsigned long) captureinfo->screencapture[0].format, captureinfo->screencapture[1].framebuf0_vaddr,
                (unsigned long) captureinfo->screencapture[1].format);
        CtrLog_flush();
    }
    return res;
}

Result __wrap_GSPGPU_ReleaseRight(void) {
    if (tracing) step("releasing the GPU");
    Result res = __real_GSPGPU_ReleaseRight();
    if (tracing) step("GPU released; the HOME Menu should open");
    return res;
}

Result __wrap_GSPGPU_AcquireRight(u8 flags) {
    if (tracing) step("taking the GPU back");
    Result res = __real_GSPGPU_AcquireRight(flags);
    if (tracing) step("GPU back");
    return res;
}

bool __wrap_gxCmdQueueWait(gxCmdQueue_s* queue, s64 timeout) {
    // citro3d's own suspend hook waits for its queue with no timeout.
    bool trace = tracing && 0 > timeout;
    if (trace) step("waiting for the GPU queue");
    bool done = __real_gxCmdQueueWait(queue, timeout);
    if (trace) step("GPU queue done");
    return done;
}

static void onDspEvent(DSP_HookType hook) {
    if (!tracing) return;
    if (hook == DSPHOOK_ONSLEEP) step("putting the DSP to sleep");
    else if (hook == DSPHOOK_ONWAKEUP) step("waking the DSP");
    else if (hook == DSPHOOK_ONCANCEL) step("DSP sleep cancelled");
}

// While suspended, a thread at the top and one at the bottom of the app's
// priorities each log every few seconds. If the HOME Menu never shows up,
// the high one ticking means the process is alive, and the low one ticking
// means none of the game's threads is hogging the app core.
#define HEARTBEAT_SECONDS 3
#define HEARTBEAT_LIMIT_SECONDS 45
#define HEARTBEAT_STACK_SIZE (8 * 1024)

static volatile u64 suspendTick = 0;
static volatile bool closing = false;

static void heartbeatMain(void* arg) {
    const char* name = arg;
    while (!closing) {
        svcSleepThread(HEARTBEAT_SECONDS * 1000 * 1000 * 1000LL);
        if (!tracing || closing) continue;
        u64 seconds = (svcGetSystemTick() - suspendTick) / SYSCLOCK_ARM11;
        if (seconds > HEARTBEAT_LIMIT_SECONDS) continue;
        logInfo("APT: %s thread still running, %llu s after suspending\n", name, (unsigned long long) seconds);
        CtrLog_flush();
    }
}

static void onAptEvent(APT_HookType hook, void* param) {
    (void) param;
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) {
        suspendTick = svcGetSystemTick();
        tracing = true;
        step(hook == APTHOOK_ONSLEEP ? "going to sleep" : "HOME pressed, suspending");
    } else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) {
        step("back in the game");
        tracing = false;
    } else if (hook == APTHOOK_ONEXIT) {
        closing = true;
        tracing = false;
    }
}

void CtrAptTrace_init(void) {
    if (hooked) return;
    hooked = true;
    // Hooks run newest first, so these run before the renderer's, the
    // watchdog's, citro3d's and NDSP's.
    aptHook(&aptCookie, onAptEvent, nullptr);
    dspHook(&dspCookie, onDspEvent);
    threadCreate(heartbeatMain, (void*) "high-priority (0x18)", HEARTBEAT_STACK_SIZE, 0x18, 0, true);
    threadCreate(heartbeatMain, (void*) "low-priority (0x3F)", HEARTBEAT_STACK_SIZE, 0x3F, 0, true);
}

void CtrAptTrace_stop(void) {
    closing = true;
    tracing = false;
}
