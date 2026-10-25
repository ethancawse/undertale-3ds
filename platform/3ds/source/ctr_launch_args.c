#include "ctr_launch_args.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

static void problem(CtrLaunchArgs* args, const char* format, ...) {
    args->problems++;
    size_t used = strlen(args->messages);
    if (used + 3 >= sizeof(args->messages)) return;
    if (used > 0) {
        memcpy(args->messages + used, "; ", 3);
        used += 2;
    }
    va_list va;
    va_start(va, format);
    vsnprintf(args->messages + used, sizeof(args->messages) - used, format, va);
    va_end(va);
}

// Appends a scene list ("ruins,snowpuzz" or "ruins snowpuzz") to args->bench as space-separated ids.
static void addScenes(CtrLaunchArgs* args, const char* list) {
    const char* p = list;
    while (*p != '\0') {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        // A sweep's battle list ("sweep=ch1_jevil,ch2_spamtn") is one word: its commas stay in it.
        size_t length = strncmp(p, "sweep=", 6) == 0 ? strcspn(p, " \t") : strcspn(p, ", \t");
        if (length == 0) break;
        size_t used = strlen(args->bench);
        if (used + (used > 0 ? 1 : 0) + length >= sizeof(args->bench)) {
            problem(args, "--bench: too many scenes, dropped '%.*s' and after", (int) length, p);
            return;
        }
        if (used > 0) args->bench[used++] = ' ';
        memcpy(args->bench + used, p, length);
        args->bench[used + length] = '\0';
        p += length;
    }
}

static void addIni(CtrLaunchArgs* args, const char* setting) {
    const char* eq = strchr(setting, '=');
    if (eq == nullptr || eq == setting || eq[1] == '\0') {
        problem(args, "--ini wants key=value, not '%s'", setting);
        return;
    }
    if (strlen(setting) >= LAUNCH_ARGS_INI_LENGTH) {
        problem(args, "--ini '%s' is too long", setting);
        return;
    }
    if (args->iniCount >= LAUNCH_ARGS_INI_MAX) {
        problem(args, "--ini: more than %d settings, dropped '%s'", LAUNCH_ARGS_INI_MAX, setting);
        return;
    }
    snprintf(args->ini[args->iniCount++], LAUNCH_ARGS_INI_LENGTH, "%s", setting);
}

// Appends a frame list ("300,900" or "300 900") to frames (--gputrace's or --autoshot's), which holds up to max.
static void addFrames(CtrLaunchArgs* args, const char* option, const char* list, int* frames, int* count, int max) {
    const char* p = list;
    int before = *count;
    while (*p != '\0') {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        char* end = nullptr;
        long frame = strtol(p, &end, 10);
        if (end == p || (*end != '\0' && *end != ',' && *end != ' ' && *end != '\t') || frame < 0 || frame > 100000000L) {
            problem(args, "%s wants frame numbers like 300,900, not '%s'", option, list);
            return;
        }
        if (*count >= max) {
            problem(args, "%s: more than %d frames, dropped %ld and after", option, max, frame);
            return;
        }
        frames[(*count)++] = (int) frame;
        p = end;
    }
    if (*count == before) problem(args, "%s names no frames", option);
}

bool CtrLaunchArgs_parse(CtrLaunchArgs* args, int argc, char** argv) {
    memset(args, 0, sizeof(*args));
    bool any = false;
    for (int i = 1; argc > i; i++) {
        const char* arg = argv[i];
        if (arg == nullptr || arg[0] == '\0') continue;
        any = true;
        if (strncmp(arg, "--", 2) != 0) {
            problem(args, "unexpected argument '%s'", arg);
            continue;
        }
        // --name=value or --name value
        char name[32];
        const char* value = nullptr;
        const char* eq = strchr(arg, '=');
        size_t nameLength = eq != nullptr ? (size_t) (eq - arg) : strlen(arg);
        if (nameLength >= sizeof(name)) nameLength = sizeof(name) - 1;
        memcpy(name, arg, nameLength);
        name[nameLength] = '\0';
        if (eq != nullptr) value = eq + 1;

        bool takesValue = strcmp(name, "--bench") == 0 || strcmp(name, "--ini") == 0 || strcmp(name, "--timeout") == 0 ||
                          strcmp(name, "--lab-host") == 0 || strcmp(name, "--lab-hang") == 0 || strcmp(name, "--lab-crash") == 0 ||
                          strcmp(name, "--gputrace") == 0 || strcmp(name, "--selftest") == 0 || strcmp(name, "--drawing-full-at") == 0 ||
                          strcmp(name, "--slider-at") == 0 || strcmp(name, "--autoshot") == 0 ||
                          strcmp(name, "--flicker-probe") == 0 || strcmp(name, "--video-suspend") == 0;
        if (takesValue && value == nullptr) {
            if (argc > i + 1) {
                value = argv[++i];
            } else {
                problem(args, "%s needs a value", name);
                continue;
            }
        }
        if (!takesValue && value != nullptr &&
            (strcmp(name, "--shots") == 0 || strcmp(name, "--exit-after") == 0 || strcmp(name, "--lab") == 0 ||
             strcmp(name, "--knightdiag") == 0 || strcmp(name, "--force-materialize") == 0 || strcmp(name, "--no-surface-readers") == 0 ||
             strcmp(name, "--statehash") == 0)) {
            problem(args, "%s takes no value", name);
            continue;
        }
        if (strcmp(name, "--profile") == 0 && value != nullptr && strcmp(value, "step") != 0) {
            problem(args, "--profile takes no value but step, not '%s'", value);
            continue;
        }

        if (strcmp(name, "--bench") == 0) {
            size_t before = strlen(args->bench);
            addScenes(args, value);
            if (strlen(args->bench) == before) problem(args, "--bench names no scenes");
        } else if (strcmp(name, "--shots") == 0) {
            args->shots = true;
        } else if (strcmp(name, "--exit-after") == 0) {
            args->exitAfter = true;
        } else if (strcmp(name, "--timeout") == 0) {
            char* end = nullptr;
            long seconds = strtol(value, &end, 10);
            if (end == value || *end != '\0' || seconds <= 0 || seconds > 24L * 3600L) {
                problem(args, "--timeout wants seconds (1-86400), not '%s'", value);
            } else {
                args->timeoutSeconds = (int) seconds;
            }
        } else if (strcmp(name, "--ini") == 0) {
            addIni(args, value);
        } else if (strcmp(name, "--lab") == 0) {
            args->lab = true;
        } else if (strcmp(name, "--lab-host") == 0) {
            // An SD card path to a .3dsx: "sdmc:/3ds/x.3dsx" or "/3ds/x.3dsx".
            const char* path = strncmp(value, "sdmc:", 5) == 0 ? value + 5 : value;
            if (path[0] != '/' || strlen(path) >= sizeof(args->labHost) || strstr(path, "..") != nullptr) {
                problem(args, "--lab-host wants an SD card path like sdmc:/3ds/ctrport-lab.3dsx, not '%s'", value);
            } else {
                snprintf(args->labHost, sizeof(args->labHost), "%s", path);
                args->lab = true;
            }
        } else if (strcmp(name, "--lab-hang") == 0) {
            char* end = nullptr;
            long seconds = strtol(value, &end, 10);
            if (end == value || *end != '\0' || seconds < 0 || seconds > 24L * 3600L) {
                problem(args, "--lab-hang wants seconds (0-86400, 0: off), not '%s'", value);
            } else {
                args->labHangSeconds = (int) seconds;
                args->labHangSet = true;
            }
        } else if (strcmp(name, "--lab-crash") == 0) {
            static const char* const kinds[] = { "data", "prefetch", "undef", "thread", "break", "abort", "hang" };
            bool known = false;
            for (size_t k = 0; sizeof(kinds) / sizeof(kinds[0]) > k; k++) known = known || strcmp(value, kinds[k]) == 0;
            if (!known) {
                problem(args, "--lab-crash wants data, prefetch, undef, thread, break, abort or hang, not '%s'", value);
            } else {
                snprintf(args->labCrash, sizeof(args->labCrash), "%s", value);
            }
        } else if (strcmp(name, "--profile") == 0) {
            args->profile = true;
            args->profileStepOnly = value != nullptr;
        } else if (strcmp(name, "--knightdiag") == 0) {
            args->knightDiagnostic = true;
        } else if (strcmp(name, "--fixed-seed") == 0) {
            char* end = nullptr;
            unsigned long long seed = value != nullptr ? strtoull(value, &end, 10) : 0;
            if (value != nullptr && (end == value || *end != '\0' || value[0] == '-' || seed > 0xFFFFFFFFull)) {
                problem(args, "--fixed-seed wants a seed (0-4294967295), not '%s'", value);
            } else {
                args->fixedSeed = true;
                args->fixedSeedValue = (unsigned) seed;
            }
        } else if (strcmp(name, "--force-materialize") == 0) {
            args->forceMaterialize = true;
        } else if (strcmp(name, "--no-surface-readers") == 0) {
            args->noSurfaceReaders = true;
        } else if (strcmp(name, "--gputrace") == 0) {
            addFrames(args, "--gputrace", value, args->gpuTraceFrames, &args->gpuTraceCount, LAUNCH_ARGS_GPU_TRACE_MAX);
        } else if (strcmp(name, "--drawing-full-at") == 0) {
            char* end = nullptr;
            long frame = strtol(value, &end, 10);
            if (end == value || *end != '\0' || frame <= 0 || frame > 10000000L) {
                problem(args, "--drawing-full-at wants a frame number (1-10000000), not '%s'", value);
            } else {
                args->drawingFullAt = (int) frame;
            }
        } else if (strcmp(name, "--slider-at") == 0) {
            char* end = nullptr;
            long up = strtol(value, &end, 10), down = 0;
            bool ok = end != value && up > 0 && up <= 10000000L;
            if (ok && *end == ',') {
                const char* from = end + 1;
                down = strtol(from, &end, 10);
                ok = end != from && down > up && down <= 10000000L;
            }
            if (!ok || *end != '\0') {
                problem(args, "--slider-at wants a frame number, or two (up,down), 1-10000000, not '%s'", value);
            } else {
                args->sliderUpAt = (int) up;
                args->sliderDownAt = (int) down;
            }
        } else if (strcmp(name, "--selftest") == 0) {
            static const char* const tests[] = { "smart", "fcram_target", "mirror_cancel", "oom" };
            bool known = false;
            for (size_t k = 0; sizeof(tests) / sizeof(tests[0]) > k; k++) known = known || strcmp(value, tests[k]) == 0;
            if (!known) {
                problem(args, "--selftest wants smart, fcram_target, mirror_cancel or oom, not '%s'", value);
            } else {
                snprintf(args->selftest, sizeof(args->selftest), "%s", value);
            }
        } else if (strcmp(name, "--autoshot") == 0) {
            addFrames(args, "--autoshot", value, args->autoshotFrames, &args->autoshotCount, LAUNCH_ARGS_AUTOSHOT_MAX);
        } else if (strcmp(name, "--statehash") == 0) {
            args->stateHash = true;
        } else if (strcmp(name, "--send-files") == 0) {
            char* end = nullptr;
            long mb = value != nullptr ? strtol(value, &end, 10) : 0;
            if (value != nullptr && (end == value || *end != '\0' || mb < 1 || mb > LAUNCH_ARGS_SEND_FILES_MAX_MB)) {
                problem(args, "--send-files wants a size in MB (1-%d), not '%s'", LAUNCH_ARGS_SEND_FILES_MAX_MB, value);
            } else {
                args->sendFiles = true;
                args->sendFilesMB = (int) mb;
            }
        } else if (strcmp(name, "--flicker-probe") == 0) {
            if (value[0] == '\0' || strlen(value) >= sizeof(args->flickerProbe)) {
                problem(args, "--flicker-probe wants words like base or nomulti,frames=20-160, not '%s'", value);
            } else {
                snprintf(args->flickerProbe, sizeof(args->flickerProbe), "%s", value);
            }
        } else if (strcmp(name, "--video-suspend") == 0) {
            unsigned frame = 0, seconds = 0;
            char extra;
            if (sscanf(value, "%u:%u%c", &frame, &seconds, &extra) != 2 ||
                frame == 0 || frame > 100000000u || seconds == 0 || seconds > 120u) {
                problem(args, "--video-suspend wants frame:seconds (1-100000000:1-120), not '%s'", value);
            } else {
                args->videoSuspendFrame = frame;
                args->videoSuspendSeconds = seconds;
            }
        } else {
            problem(args, "unknown option '%s'", name);
        }
    }
    if (args->shots && args->bench[0] == '\0') problem(args, "--shots does nothing without --bench");
    if (!args->lab && (args->labHangSet || args->labCrash[0] != '\0')) problem(args, "--lab-hang and --lab-crash do nothing without --lab");
    return any;
}

void CtrLaunchArgs_describe(const CtrLaunchArgs* args, char* out, size_t size) {
    if (size == 0) return;
    out[0] = '\0';
    size_t used = 0;
#define DESCRIBE(...)                                                                                  \
    do {                                                                                               \
        if (used < size) {                                                                             \
            int n = snprintf(out + used, size - used, __VA_ARGS__);                                    \
            if (n > 0) used += (size_t) n;                                                             \
        }                                                                                              \
    } while (0)
    if (args->bench[0] != '\0') DESCRIBE("%s--bench %s", used > 0 ? " " : "", args->bench);
    if (args->shots) DESCRIBE("%s--shots", used > 0 ? " " : "");
    if (args->videoSuspendSeconds) DESCRIBE("%s--video-suspend %u:%u", used > 0 ? " " : "", args->videoSuspendFrame, args->videoSuspendSeconds);
    if (args->exitAfter) DESCRIBE("%s--exit-after", used > 0 ? " " : "");
    if (args->timeoutSeconds > 0) DESCRIBE("%s--timeout %d", used > 0 ? " " : "", args->timeoutSeconds);
    for (int i = 0; args->iniCount > i; i++) DESCRIBE("%s--ini %s", used > 0 ? " " : "", args->ini[i]);
    if (args->labHost[0] != '\0') DESCRIBE("%s--lab-host %s", used > 0 ? " " : "", args->labHost);
    else if (args->lab) DESCRIBE("%s--lab", used > 0 ? " " : "");
    if (args->labHangSet) DESCRIBE("%s--lab-hang %d", used > 0 ? " " : "", args->labHangSeconds);
    if (args->labCrash[0] != '\0') DESCRIBE("%s--lab-crash %s", used > 0 ? " " : "", args->labCrash);
    if (args->profile) DESCRIBE("%s--profile%s", used > 0 ? " " : "", args->profileStepOnly ? "=step" : "");
    if (args->knightDiagnostic) DESCRIBE("%s--knightdiag", used > 0 ? " " : "");
    if (args->fixedSeed) DESCRIBE("%s--fixed-seed=%u", used > 0 ? " " : "", args->fixedSeedValue);
    for (int i = 0; args->gpuTraceCount > i; i++) DESCRIBE("%s%d", i == 0 ? (used > 0 ? " --gputrace " : "--gputrace ") : ",", args->gpuTraceFrames[i]);
    if (args->selftest[0] != '\0') DESCRIBE("%s--selftest %s", used > 0 ? " " : "", args->selftest);
    if (args->forceMaterialize) DESCRIBE("%s--force-materialize", used > 0 ? " " : "");
    if (args->noSurfaceReaders) DESCRIBE("%s--no-surface-readers", used > 0 ? " " : "");
    if (args->drawingFullAt > 0) DESCRIBE("%s--drawing-full-at %d", used > 0 ? " " : "", args->drawingFullAt);
    if (args->sliderUpAt > 0) DESCRIBE("%s--slider-at %d", used > 0 ? " " : "", args->sliderUpAt);
    if (args->sliderDownAt > 0) DESCRIBE(",%d", args->sliderDownAt);
    for (int i = 0; args->autoshotCount > i; i++) DESCRIBE("%s%d", i == 0 ? (used > 0 ? " --autoshot " : "--autoshot ") : ",", args->autoshotFrames[i]);
    if (args->stateHash) DESCRIBE("%s--statehash", used > 0 ? " " : "");
    if (args->sendFiles && args->sendFilesMB > 0) DESCRIBE("%s--send-files=%d", used > 0 ? " " : "", args->sendFilesMB);
    else if (args->sendFiles) DESCRIBE("%s--send-files", used > 0 ? " " : "");
    if (args->flickerProbe[0] != '\0') DESCRIBE("%s--flicker-probe %s", used > 0 ? " " : "", args->flickerProbe);
    if (used == 0) DESCRIBE("(none)");
#undef DESCRIBE
}

int CtrLaunchArgs_sendFilesMB(const CtrLaunchArgs* args) {
    if (!args->sendFiles && args->autoshotCount == 0 && !args->stateHash) return 0;
    return args->sendFilesMB > 0 ? args->sendFilesMB : LAUNCH_ARGS_SEND_FILES_DEFAULT_MB;
}
