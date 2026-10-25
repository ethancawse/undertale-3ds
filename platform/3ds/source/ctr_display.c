#include "ctr_display.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "ctr_notice.h"
#include "log.h"

CtrDisplayCaps CtrDisplay_queryCaps(void) {
    CtrDisplayCaps caps = { true, true };
    u8 model = CFG_MODEL_3DS;
    if (R_SUCCEEDED(cfguInit())) {
        CFGU_GetSystemModel(&model);
        cfguExit();
    }
    if (model == CFG_MODEL_2DS) caps.wide = false;
    if (model == CFG_MODEL_2DS || model == CFG_MODEL_N2DSXL) caps.stereo = false;
    return caps;
}

void CtrDisplay_defaults(CtrDisplaySettings* settings) {
    settings->topMode = TOP_MODE_STANDARD;
    settings->stereoMode = STEREO_HD;
    settings->smoothing = SMOOTHING_SMART;
    settings->drawing = DRAWING_NATIVE;
    settings->bottomMode = BOTTOM_MODE_MAGNIFIER;
    CtrInputMap_defaults(&settings->controls);
    settings->dialogueBottom = false;
    settings->prepareSyscore = true;
    settings->repackTextures = false;
    settings->sdWriteTest = false;
    settings->textureCacheRun = 0;
    settings->cachePublishDirect = true;
    settings->loadTrace = false;
    settings->roomMemoryReset = false;
    settings->openFilesMax = 0;
    settings->openFilesFakeMax = 0;
    // Skipping drops shown frames, so it stays opt-in on every console.
    settings->frameSkip = FRAME_SKIP_OFF;
    settings->frameSkipMax = 1;
    settings->frameSkipDeep = 0;
    settings->frameSkipPolicy = 0;
    settings->frameSkipOverlap = 0;
    settings->frameSkipRecord = 1;
    settings->frameSkipAuto = 1;
    settings->exactColours = false;
    settings->benchFrames = 0;
}

// The settings file's format. 2: smoothing is 0 off, 1 full, 2 smart. Files
// before it (no settings_version line) stored smoothing as on or off, on
// being the old default, so their 1 loads as the new default, smart, once:
// the next save writes version 2.
#define SETTINGS_VERSION 2

// A Controls page line (map_a = 1); false if key isn't one or the value isn't one of that button's choices.
static bool applyMapLine(CtrDisplaySettings* settings, const char* key, int value) {
    for (int32_t row = 0; MAP_ROW_COUNT > row; row++) {
        if (strcmp(key, CtrInputMap_iniKey((CtrMapRow) row)) != 0) continue;
        if (!CtrInputMap_allowed((CtrMapRow) row, value)) return false;
        settings->controls.keys[row] = (uint8_t) value;
        return true;
    }
    return false;
}

// Applies one line of the file; false if it isn't a setting this build knows (or its value is out of range).
static bool applyLine(CtrDisplaySettings* settings, const char* line) {
    char key[64];
    int value;
    if (sscanf(line, " %63[^= ] = %d", key, &value) != 2) return false;
    // The retired Zoom value loads as Standard (ctr_display.h).
    if (strcmp(key, "top_mode") == 0 && 0 <= value && TOP_MODE_RETIRED_ZOOM >= value)
        settings->topMode = TOP_MODE_COUNT > value ? (CtrTopMode) value : TOP_MODE_STANDARD;
    else if (strcmp(key, "stereo_mode") == 0 && 0 <= value && STEREO_MODE_COUNT > value) settings->stereoMode = (CtrStereoMode) value;
    else if (strcmp(key, "stereo_hd") == 0) settings->stereoMode = value != 0 ? STEREO_HD : STEREO_NORMAL; // older files
    else if (strcmp(key, "smoothing") == 0 && 0 <= value && SMOOTHING_COUNT > value) settings->smoothing = (CtrSmoothing) value;
    else if (strcmp(key, "drawing") == 0 && 0 <= value && DRAWING_COUNT > value) settings->drawing = (CtrDrawing) value;
    else if (strcmp(key, "bottom_mode") == 0 && 0 <= value && BOTTOM_MODE_COUNT > value) settings->bottomMode = (CtrBottomMode) value;
    else if (strcmp(key, "dialogue_bottom") == 0) settings->dialogueBottom = value != 0;
    else if (strcmp(key, "confirm_with_b") == 0) { // before the Controls page
        if (value != 0) {
            settings->controls.keys[MAP_ROW_A] = GAME_KEY_X;
            settings->controls.keys[MAP_ROW_B] = GAME_KEY_Z;
        }
    }
    else if (strcmp(key, "prepare_syscore") == 0) settings->prepareSyscore = value != 0;
    else if (strcmp(key, "repack_textures") == 0) settings->repackTextures = value != 0;
    else if (strcmp(key, "sd_write_test") == 0) settings->sdWriteTest = value != 0;
    else if (strcmp(key, "texture_cache_run") == 0 && value >= 0 && value <= 1000000000) settings->textureCacheRun = value;
    else if (strcmp(key, "cache_publish_direct") == 0) settings->cachePublishDirect = value != 0;
    else if (strcmp(key, "load_trace") == 0) settings->loadTrace = value != 0;
    else if (strcmp(key, "room_memory_reset") == 0) settings->roomMemoryReset = value != 0;
    else if (strcmp(key, "open_files_max") == 0 && 0 <= value && 64 >= value) settings->openFilesMax = value;
    else if (strcmp(key, "open_files_fake_max") == 0 && 0 <= value && 64 >= value) settings->openFilesFakeMax = value;
    else if (strcmp(key, "frame_skip") == 0 && 0 <= value && FRAME_SKIP_COUNT > value) settings->frameSkip = (CtrFrameSkip) value;
    else if (strcmp(key, "frame_skip_max") == 0 && 1 <= value && FRAME_SKIP_MAX_LIMIT >= value) settings->frameSkipMax = value;
    else if (strcmp(key, "frame_skip_deep") == 0 && 0 <= value && 3 >= value) settings->frameSkipDeep = value;
    else if (strcmp(key, "frame_skip_policy") == 0 && 0 <= value && 1 >= value) settings->frameSkipPolicy = value;
    else if (strcmp(key, "frame_skip_overlap") == 0 && 0 <= value && 1 >= value) settings->frameSkipOverlap = value;
    else if (strcmp(key, "frame_skip_record") == 0 && 0 <= value && 1 >= value) settings->frameSkipRecord = value;
    else if (strcmp(key, "frame_skip_auto") == 0 && 0 <= value && 1 >= value) settings->frameSkipAuto = value;
    else if (strcmp(key, "exact_colours") == 0) settings->exactColours = value != 0;
    else if (strcmp(key, "bench_frames") == 0 && 0 <= value && 100000 >= value) settings->benchFrames = value;
    else if (strcmp(key, "settings_version") == 0) {
        // read by CtrDisplay_load
    } else return applyMapLine(settings, key, value);
    return true;
}

static void applyCaps(CtrDisplaySettings* settings, CtrDisplayCaps caps) {
    if (!caps.wide && settings->topMode == TOP_MODE_WIDE) settings->topMode = TOP_MODE_STANDARD;
    if (!caps.stereo) settings->stereoMode = STEREO_NORMAL;
}

void CtrDisplay_load(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps) {
    FILE* f = fopen(path, "r");
    if (f != nullptr) {
        char line[128];
        int version = 1;
        while (fgets(line, sizeof(line), f) != nullptr) {
            applyLine(settings, line);
            sscanf(line, " settings_version = %d", &version);
        }
        fclose(f);
        if (2 > version && settings->smoothing == SMOOTHING_FULL) settings->smoothing = SMOOTHING_SMART;
    }
    applyCaps(settings, caps);
}

bool CtrDisplay_override(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps) {
    bool known = applyLine(settings, line);
    applyCaps(settings, caps);
    return known;
}

void CtrDisplay_save(const CtrDisplaySettings* settings, const char* path) {
    FILE* f = fopen(path, "w");
    if (f == nullptr) {
        logWarn("Display: could not save settings to %s\n", path);
        return;
    }
    fprintf(f, "# settings\n");
    fprintf(f, "settings_version = %d\n", SETTINGS_VERSION);
    fprintf(f, "top_mode = %d\n", (int) settings->topMode);
    fprintf(f, "stereo_mode = %d\n", (int) settings->stereoMode);
    fprintf(f, "smoothing = %d\n", (int) settings->smoothing);
    fprintf(f, "drawing = %d\n", (int) settings->drawing);
    fprintf(f, "bottom_mode = %d\n", (int) settings->bottomMode);
    fprintf(f, "dialogue_bottom = %d\n", settings->dialogueBottom ? 1 : 0);
    for (int32_t row = 0; MAP_ROW_COUNT > row; row++) fprintf(f, "%s = %d\n", CtrInputMap_iniKey((CtrMapRow) row), (int) settings->controls.keys[row]);
    fprintf(f, "prepare_syscore = %d\n", settings->prepareSyscore ? 1 : 0);
    fprintf(f, "frame_skip = %d\n", (int) settings->frameSkip);
    fprintf(f, "frame_skip_max = %d\n", (int) settings->frameSkipMax);
    if (settings->frameSkipDeep != 0) fprintf(f, "frame_skip_deep = %d\n", (int) settings->frameSkipDeep);
    if (settings->frameSkipPolicy != 0) fprintf(f, "frame_skip_policy = %d\n", (int) settings->frameSkipPolicy);
    if (settings->frameSkipOverlap != 0) fprintf(f, "frame_skip_overlap = %d\n", (int) settings->frameSkipOverlap);
    if (settings->frameSkipRecord != 1) fprintf(f, "frame_skip_record = %d\n", (int) settings->frameSkipRecord);
    if (settings->frameSkipAuto != 1) fprintf(f, "frame_skip_auto = %d\n", (int) settings->frameSkipAuto);
    fprintf(f, "exact_colours = %d\n", settings->exactColours ? 1 : 0);
    CtrNotice_writeDismissals(f);
    fclose(f);
}

void CtrDisplay_saveEdits(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps) {
    CtrDisplaySettings saved;
    CtrDisplay_defaults(&saved);
    CtrDisplay_load(&saved, path, caps);
    // The settings the options menu can change (ctr_menu.c).
    if (before->topMode != after->topMode) saved.topMode = after->topMode;
    if (before->stereoMode != after->stereoMode) saved.stereoMode = after->stereoMode;
    if (before->smoothing != after->smoothing) saved.smoothing = after->smoothing;
    if (before->drawing != after->drawing) saved.drawing = after->drawing;
    if (before->bottomMode != after->bottomMode) saved.bottomMode = after->bottomMode;
    if (before->dialogueBottom != after->dialogueBottom) saved.dialogueBottom = after->dialogueBottom;
    for (int32_t row = 0; MAP_ROW_COUNT > row; row++) {
        if (before->controls.keys[row] != after->controls.keys[row]) saved.controls.keys[row] = after->controls.keys[row];
    }
    if (before->frameSkip != after->frameSkip) saved.frameSkip = after->frameSkip;
    if (before->exactColours != after->exactColours) saved.exactColours = after->exactColours;
    CtrDisplay_save(&saved, path);
}

const char* CtrDisplay_topModeName(CtrTopMode mode) {
    switch (mode) {
        case TOP_MODE_STANDARD: return "Standard (400 px)";
        case TOP_MODE_WIDE: return "Wide (800 px, no 3D)";
        default: return "?";
    }
}

const char* CtrDisplay_drawingName(CtrDrawing mode) {
    switch (mode) {
        case DRAWING_NATIVE: return "Native (fast)";
        case DRAWING_FULL: return "Full (exact)";
        default: return "?";
    }
}

const char* CtrDisplay_stereoModeName(CtrStereoMode mode) {
    switch (mode) {
        case STEREO_NORMAL: return "Normal";
        case STEREO_HD: return "HD";
        case STEREO_DEPTH: return "Depth";
        default: return "?";
    }
}

const char* CtrDisplay_frameSkipName(CtrFrameSkip mode) {
    switch (mode) {
        case FRAME_SKIP_OFF: return "Off";
        case FRAME_SKIP_AUTO: return "Auto";
        case FRAME_SKIP_HALF: return "2:1 (half shown)";
        case FRAME_SKIP_20: return "20 fps";
        case FRAME_SKIP_15: return "15 fps";
        default: return "?";
    }
}

const char* CtrDisplay_smoothingName(CtrSmoothing mode) {
    switch (mode) {
        case SMOOTHING_OFF: return "Off";
        case SMOOTHING_FULL: return "Full";
        case SMOOTHING_SMART: return "Smart";
        default: return "?";
    }
}

const char* CtrDisplay_bottomModeName(CtrBottomMode mode) {
    switch (mode) {
        case BOTTOM_MODE_MAGNIFIER: return "Text magnifier";
        case BOTTOM_MODE_OFF: return "Off";
        case BOTTOM_MODE_CONSOLE: return "Stats and log";
        default: return "?";
    }
}
