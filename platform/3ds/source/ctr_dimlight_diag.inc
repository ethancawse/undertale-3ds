// TEST ONLY. Included after the complete production policy so release code,
// line numbers and storage are identical when the option is OFF.
#include "ctr_benchmark.h"
#include "ctr_frame_pacer.h"
#include "ctr_lab.h"
#include "sandbox_file_system.h"

// The compile definition redirects dlRaw's call above. This diagnostic calls
// the real syscall and reads the actual hardware after each completed request.
#undef svcSendSyncRequest
Result svcSendSyncRequest(Handle lcd); // original SDK declaration was renamed above
FileSystem* CtrBench_lateSandbox(void);
bool CtrDimdiag_sandboxEnabled(FileSystem* fs);
u32 C3D_FrameCounter(int id);
void CtrMain_fatalScreen(const char*, ...) __attribute__((noreturn));
void __real_CtrScreens_pacerSwap(gfxScreen_t screen, bool hasStereo);

// The linker places DL_RO constants in late text; neither target's scarce
// late data or late rodata reserve pays for these immutable diagnostic strings.
static const char ddFormat[] DL_RO = "[dldiag] e=%u f=%lu t=%llu a=%08lX b=%08lX c=%08lX d=%08lX\n";
static const char ddFpsOff[] DL_RO = "fps_counter=0";
static const char ddFpsOn[] DL_RO = "fps_counter=2";
static const char ddFault[] DL_RO = "data";
static const char ddFatal[] DL_RO = "dimlight diagnostic: fatal after verified dim";


// Event codes and units are specified in dimlight-diagnostic.md. Only actual
// sandbox activation allows menu input or overlay painting during a bench.
DL_CODE static bool ddSandbox(void) {
    if (!dlReady()) return false;
    FileSystem* fs = CtrBench_lateSandbox();
    return CtrBench_running() && fs && CtrDimdiag_sandboxEnabled(fs);
}

DL_CODE static void ddLog(unsigned event, u32 a, u32 b, u32 c, u32 d) {
    if (!dlReady()) return;
    logInfo(ddFormat, event, (unsigned long) __atomic_load_n(&dd.frame, __ATOMIC_RELAXED),
            (unsigned long long) svcGetSystemTick(), (unsigned long) a,
            (unsigned long) b, (unsigned long) c, (unsigned long) d);
}

DL_CODE Result CtrDimdiag_send(Handle lcd) {
    if (!dlReady()) return svcSendSyncRequest(lcd);
    u32* cmd = getThreadCommandBuffer();
    u32 request[3] = {cmd[0], cmd[1], cmd[2]}, raw = cmd[2];
    u32 serial = __atomic_add_fetch(&dd.requestSerial, 1u, __ATOMIC_ACQ_REL);
    __atomic_store_n(&dd.raw0Serial, 0u, __ATOMIC_RELEASE);
    cmd[0] = request[0]; cmd[1] = request[1]; cmd[2] = request[2];
    Result transport = svcSendSyncRequest(lcd);
    Result service = R_SUCCEEDED(transport) ? (Result) cmd[1] : transport;
    u32 header = cmd[0], reply = cmd[1], duty = 0, pwm = 0;
    // The service may clamp raw 0 above the calibration polynomial's floor.
    // Qualify its settled readback; invalidate it on every intervening request.
    if (raw == 0 && R_SUCCEEDED(service)) {
        __atomic_store_n(&dd.floorStart, C3D_FrameCounter(0), __ATOMIC_RELAXED);
        __atomic_store_n(&dd.raw0Serial, serial, __ATOMIC_RELEASE);
    }
    // ReadHWRegs uses the same thread command buffer. Preserve dlRaw's reply.
    Result rd = GSPGPU_ReadHWRegs(DL_DUTY, &duty, sizeof(duty));
    Result rp = GSPGPU_ReadHWRegs(DL_PWM, &pwm, sizeof(pwm));
    if (R_SUCCEEDED(service)) {
        u32 readback[7] = {raw, (u32) service, duty, pwm, (u32) rd, (u32) rp, C3D_FrameCounter(0)};
        memcpy(dd.readback, readback, sizeof(readback));
        dd.readbackPending = true;
    }
    cmd[0] = header; cmd[1] = reply;
    return transport;
}

// Copy under the public owner, then report after the outermost release. A
// rollback within one scope coalesces to its final readback; production still
// emits a separate summary for each successful raw request. Event 1 is retired.
DL_CODE bool CtrDimdiag_takeRaw(u32 out[7]) {
    if (!dlReady()) return false;
    bool pending = dd.readbackPending;
    if (pending) memcpy(out, dd.readback, sizeof(dd.readback));
    dd.readbackPending = false;
    return pending;
}
DL_CODE void CtrDimdiag_reportRaw(const u32 value[7]) {
    ddLog(2, value[0], value[1], value[2], value[3]);
    ddLog(3, value[4], value[5], value[6], 0);
}

DL_CODE void CtrDimdiag_beginFrame(Renderer* renderer) {
    CtrRenderer_beginFrame(renderer);
    if (!dlEnsure()) return;
    dd.firstRunning = true;
    __atomic_store_n(&dd.active, ddSandbox(), __ATOMIC_RELEASE);
    if (!dd.active) return;
    __atomic_add_fetch(&dd.frame, 1u, __ATOMIC_RELAXED);
    u32 held = hidKeysHeld(), down = hidKeysDown(); // last main-thread scan
    if (dd.settingsKnown && (dd.frame % 120u == 0 || ((held & KEY_R) && (down & KEY_A)))) {
        __atomic_store_n(&dd.fps, !dd.fps, __ATOMIC_RELEASE);
        CtrDisplaySettings settings = dd.settings;
        CtrDimlight_override(&settings, dd.fps ? ddFpsOn : ddFpsOff,
                            CtrDisplay_queryCaps());
        ddLog(4, dd.fps, 0, 0, 0);
    }
    // A late handled fault is meaningful only after a real dim and readback.
    if ((held & KEY_R) && (down & (KEY_X | KEY_Y)) && dl.dimmed) {
        u32 duty = 0, pwm = 0;
        bool verified = dd.floorValid &&
                        dd.floorSerial == __atomic_load_n(&dd.requestSerial, __ATOMIC_ACQUIRE) &&
                        dd.floorSerial == __atomic_load_n(&dd.raw0Serial, __ATOMIC_ACQUIRE) &&
                        R_SUCCEEDED(GSPGPU_ReadHWRegs(DL_DUTY, &duty, 4)) &&
                        R_SUCCEEDED(GSPGPU_ReadHWRegs(DL_PWM, &pwm, 4)) &&
                        duty == dd.floorDuty && pwm == dd.floorPwm;
        ddLog(5, verified, duty, dd.floorDuty, pwm);
        if (verified) {
            if (down & KEY_X) CtrLab_crashTest(ddFault);
            else CtrMain_fatalScreen(ddFatal);
        }
    }
}

DL_CODE void CtrDimdiag_setDisplay(Renderer* renderer, const CtrDisplaySettings* settings) {
    if (!dlEnsure()) { CtrDimlight_setDisplay(renderer, settings); return; }
    // The screen wrapper passes its narrowed stack copy. Snapshot its 76 B,
    // without retaining that pointer or pulling the dead getter into warm text.
    dd.settings = *settings;
    dd.settingsKnown = true;
    CtrDimlight_setDisplay(renderer, settings);
}

DL_CODE bool CtrDimdiag_mainRunning(void) {
    if (!dlReady()) return CtrBench_running();
    bool running = CtrBench_running();
    bool inputCheck = dd.firstRunning;
    dd.firstRunning = false;
    // The first main.c query after beginFrame guards SELECT/Options. The
    // second guards sendInput and must still suppress player gameplay input.
    return inputCheck && dd.active && ddSandbox() ? false : running;
}

DL_CODE bool CtrDimdiag_paintRunning(void) {
    return ddSandbox() ? false : CtrBench_running();
}

DL_CODE void CtrDimdiag_saveEdits(const CtrDisplaySettings* before,
                                const CtrDisplaySettings* after,
                                const char* path, CtrDisplayCaps caps) {
    (void) before; (void) after; (void) path; (void) caps;
    ddLog(6, 0, 0, 0, 0); // no settings write, including post-loop pending edits
}

DL_CODE void CtrDimdiag_endFrame(Renderer* renderer) {
    if (!dlReady()) { CtrRenderer_endFrame(renderer); return; }
    dd.ending = renderer;
    if (dd.active) ddLog(7, CtrRenderer_dialogueShown(renderer), dl.dimmed, C3D_FrameCounter(0), 0);
    CtrRenderer_endFrame(renderer);
    dd.ending = nullptr;
    if (dd.active) ddLog(8, CtrRenderer_dialogueShown(renderer), dl.dimmed, C3D_FrameCounter(0), 0);
    if (dd.active && dd.frame % 30u == 0) {
        u32 duty = 0, pwm = 0;
        Result rd = GSPGPU_ReadHWRegs(DL_DUTY, &duty, 4);
        Result rp = GSPGPU_ReadHWRegs(DL_PWM, &pwm, 4);
        // A separate later read in beginFrame must match this settled sample.
        // Unknown, raised or changed-control values cannot authorize a fault.
        u32 serial = __atomic_load_n(&dd.raw0Serial, __ATOMIC_ACQUIRE);
        if (serial && serial == __atomic_load_n(&dd.requestSerial, __ATOMIC_ACQUIRE) &&
            dl.dimmed && R_SUCCEEDED(rd) && R_SUCCEEDED(rp) &&
            (u32)(C3D_FrameCounter(0) - __atomic_load_n(&dd.floorStart, __ATOMIC_RELAXED)) >= 8u &&
            pwm == dl.savedPwm && duty <= dl.savedDuty) {
            dd.floorSerial = serial;
            dd.floorDuty = duty; dd.floorPwm = pwm; dd.floorValid = true;
        } else dd.floorValid = false;
        ddLog(9, duty, pwm, dl.savedDuty, dl.savedPwm);
        ddLog(10, (u32) rd, (u32) rp, 0, 0);
    }
}

// Renderer-only alias. Its normal wait has completed the previous frame's
// callbacks; present has updated dlg.shown, but this frame is not submitted.
// Keep the existing screen-aware frameEnding wrapper in this call chain.
DL_CODE void CtrDimdiag_frameEnding(void) {
    CtrPacer_frameEnding();
    if (!dlReady()) return;
    u32 tag = 0;
    if (dd.ending && dd.active && dd.settingsKnown &&
        dd.settings.topMode == TOP_MODE_STANDARD && dd.settings.frameSkip == FRAME_SKIP_OFF &&
        CtrScreens_game == GFX_TOP && !gfxIsWide() && !gfxIs3D()) {
        dd.box = CtrRenderer_dialogueShown(dd.ending);
        tag = (dd.frame << 3) | 4u | (dd.box ? 1u : 0u) | (dd.fps ? 2u : 0u);
    }
    u32 prior = __atomic_exchange_n(&dd.published, tag, __ATOMIC_ACQ_REL);
    if ((prior & 7u) != (tag & 7u)) ddLog(11, tag, C3D_FrameCounter(0), 0, 0);
}

// Only the async swap observer calls this, before the pacer takes its lock.
// The GPU has completed this transfer and the flip has not been requested.
DL_CODE static void ddMarker(u8* fb, u32 tag) {
    if (!fb || !(tag & 4u) ||
        gfxGetScreenFormat(GFX_TOP) != GSP_BGR8_OES) return;
    for (unsigned x = 384; x < 392; ++x) {
        for (unsigned y = 208; y < 232; ++y) {
            u8 colour = y < 216 ? 255 : y < 224 ? ((tag & 1u) ? 255 : 0) : ((tag & 2u) ? 255 : 0);
            unsigned at = (x * 240u + 239u - y) * 3u;
            fb[at] = fb[at + 1] = fb[at + 2] = colour;
        }
    }
    GSPGPU_FlushDataCache(fb + 384u * 240u * 3u, 8u * 240u * 3u);
    if ((dd.painted & 7u) != (tag & 7u)) ddLog(12, tag, C3D_FrameCounter(0), 0, 0);
    dd.painted = tag; // GSP callback thread only; never drives the production policy
}

DL_CODE void CtrDimdiag_pacerSwap(gfxScreen_t screen, bool hasStereo) {
    if (!dlReady()) { __real_CtrScreens_pacerSwap(screen, hasStereo); return; }
    u32 tag = __atomic_load_n(&dd.published, __ATOMIC_ACQUIRE);
    if (screen == GFX_TOP && !hasStereo && (tag & 4u))
        ddMarker(CtrPacer_backFramebuffer(screen, GFX_LEFT), tag);
    __real_CtrScreens_pacerSwap(screen, hasStereo);
}
