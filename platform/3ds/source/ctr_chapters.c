// game_change() support for the 3DS frontend; see ctr_chapters.h.

#include "ctr_chapters.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "data_win.h"
#include "ctr_file_system.h"
#include "stb_ds.h"
#include "utils.h"
#include "vm.h"

// What parameter_string(0) returns. The PC runner passes the executable path.
#define PROGRAM_NAME "ctrport"
#define BOOT_FILE_NAME "boot.txt"
#define DEFAULT_LAUNCH_PARAMETERS "-game data.win"

// The session's strings outlive every chapter. On the heap they landed
// wherever the allocator had a gap when the next chapter was chosen, which
// split the heap the chapter after loads into, so they are kept here. The
// argument array is allocated once, while the heap is still empty.
#define SESSION_TEXT_BYTES 2048
#define SESSION_ARGS_CAPACITY 32
static char sessionText[SESSION_TEXT_BYTES];

static void freeArgs(char*** args) {
    for (ptrdiff_t i = 0; arrlen(*args) > i; i++) free((*args)[i]);
    arrfree(*args);
    *args = nullptr;
}

// Splits launch parameters on whitespace, like the desktop runner does.
static char** splitParameters(const char* parameters) {
    char** tokens = nullptr;
    char* copy = safeStrdup(parameters);
    char* savePtr = nullptr;
    for (char* token = strtok_r(copy, " \t\r\n", &savePtr); token != nullptr; token = strtok_r(nullptr, " \t\r\n", &savePtr)) {
        arrput(tokens, safeStrdup(token));
    }
    free(copy);
    return tokens;
}

// Joins base and relative and resolves "." and ".." components, since the SD
// card driver doesn't understand them. "sdmc:" (anything before the first
// '/') is kept as the root and ".." never climbs above it.
static char* joinNormalized(const char* base, const char* relative) {
    size_t length = strlen(base) + 1 + strlen(relative) + 1;
    char* joined = safeMalloc(length);
    snprintf(joined, length, "%s/%s", base, relative);
    for (char* p = joined; *p != '\0'; p++) {
        if (*p == '\\') *p = '/';
    }

    char* firstSlash = strchr(joined, '/');
    size_t rootLength = firstSlash != nullptr ? (size_t) (firstSlash - joined) : 0;
    char* out = safeMalloc(length + 1);
    memcpy(out, joined, rootLength);
    size_t outLength = rootLength;

    char* savePtr = nullptr;
    for (char* part = strtok_r(joined + rootLength, "/", &savePtr); part != nullptr; part = strtok_r(nullptr, "/", &savePtr)) {
        if (strcmp(part, ".") == 0) continue;
        if (strcmp(part, "..") == 0) {
            while (outLength > rootLength && out[outLength - 1] != '/') outLength--;
            if (outLength > rootLength) outLength--;
            continue;
        }
        out[outLength++] = '/';
        size_t partLength = strlen(part);
        memcpy(out + outLength, part, partLength);
        outLength += partLength;
    }
    if (outLength == rootLength) out[outLength++] = '/';
    out[outLength] = '\0';
    free(joined);
    return out;
}

// Copies the session's next strings into sessionText (the old ones may be
// read until then). False if they don't fit.
static bool storeSession(CtrGameSession* session, const char* dataWinPath, const char* bundleDir, char** tokens) {
    char text[SESSION_TEXT_BYTES];
    size_t used = 0;
    const char* parts[SESSION_ARGS_CAPACITY + 3];
    size_t count = 0;
    parts[count++] = dataWinPath;
    parts[count++] = bundleDir;
    parts[count++] = PROGRAM_NAME;
    for (ptrdiff_t i = 0; arrlen(tokens) > i && SESSION_ARGS_CAPACITY + 2 > count; i++) parts[count++] = tokens[i];
    size_t offsets[SESSION_ARGS_CAPACITY + 3];
    for (size_t i = 0; count > i; i++) {
        size_t length = strlen(parts[i]) + 1;
        if (length > sizeof(text) - used) {
            logError("game_change: the next data.win's path and parameters are over %d bytes\n", SESSION_TEXT_BYTES);
            return false;
        }
        memcpy(text + used, parts[i], length);
        offsets[i] = used;
        used += length;
    }
    memcpy(sessionText, text, used);
    session->dataWinPath = sessionText + offsets[0];
    session->bundleDir = sessionText + offsets[1];
    arrsetlen(session->gameArgs, 0);
    for (size_t i = 2; count > i; i++) arrput(session->gameArgs, sessionText + offsets[i]);
    return true;
}

// Points the session at <baseDir>/<workingDirectory>/<file named by "-game">.
static bool applyGameChange(CtrGameSession* session, const char* baseDir, const char* workingDirectory, const char* parameters) {
    char** tokens = splitParameters(parameters);
    const char* dataWinName = nullptr;
    for (ptrdiff_t i = 0; arrlen(tokens) - 1 > i; i++) {
        if (strcmp(tokens[i], "-game") == 0) {
            dataWinName = tokens[i + 1];
            break;
        }
    }
    if (dataWinName == nullptr) {
        logError("game_change: launch parameters \"%s\" have no \"-game <file>\"\n", parameters);
        freeArgs(&tokens);
        return false;
    }

    char* bundleDir = joinNormalized(baseDir, workingDirectory);
    char* dataWinPath = joinNormalized(bundleDir, dataWinName);
    bool ok = storeSession(session, dataWinPath, bundleDir, tokens);
    free(dataWinPath);
    free(bundleDir);
    freeArgs(&tokens);
    return ok;
}

// boot.txt: line 1 is a folder under the game directory (game_change's first
// argument), line 2 optional launch parameters (default "-game data.win").
static bool applyBootFile(CtrGameSession* session, const char* gameDir) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", gameDir, BOOT_FILE_NAME);
    FILE* f = fopen(path, "r");
    if (f == nullptr) return false;
    char folder[256] = "";
    char parameters[512] = "";
    if (fgets(folder, sizeof(folder), f) == nullptr) folder[0] = '\0';
    if (fgets(parameters, sizeof(parameters), f) == nullptr) parameters[0] = '\0';
    fclose(f);
    folder[strcspn(folder, "\r\n")] = '\0';
    parameters[strcspn(parameters, "\r\n")] = '\0';
    if (parameters[0] == '\0') snprintf(parameters, sizeof(parameters), "%s", DEFAULT_LAUNCH_PARAMETERS);
    logInfo("%s: starting \"%s\" with \"%s\"\n", path, folder, parameters);
    return applyGameChange(session, gameDir, folder, parameters);
}

void CtrChapters_init(CtrGameSession* session, const char* gameDir) {
    memset(session, 0, sizeof(*session));
    session->loadCount = 1;
    arrsetcap(session->gameArgs, SESSION_ARGS_CAPACITY);
    if (applyBootFile(session, gameDir)) return;
    applyGameChange(session, gameDir, "", DEFAULT_LAUNCH_PARAMETERS);
    // The desktop runner passes no parameters on a normal start, only the program name.
    if (arrlen(session->gameArgs) > 1) arrsetlen(session->gameArgs, 1);
}

bool CtrChapters_changeRequested(const Runner* runner) {
    return runner->pendingWorkingDirectory != nullptr && runner->pendingLaunchParameters != nullptr;
}

void CtrChapters_returnToLauncher(CtrGameSession* session, const char* gameDir) {
    applyGameChange(session, gameDir, "", DEFAULT_LAUNCH_PARAMETERS);
    if (arrlen(session->gameArgs) > 1) arrsetlen(session->gameArgs, 1);
    session->loadCount++;
}

bool CtrChapters_takeGameChange(CtrGameSession* session, Runner* runner) {
    if (!CtrChapters_changeRequested(runner)) return false;
    char* workingDirectory = runner->pendingWorkingDirectory;
    char* parameters = runner->pendingLaunchParameters;
    runner->pendingWorkingDirectory = nullptr;
    runner->pendingLaunchParameters = nullptr;

    logInfo("game_change(\"%s\", \"%s\") from %s\n", workingDirectory, parameters, session->dataWinPath);
    char* baseDir = safeStrdup(session->bundleDir);
    bool ok = applyGameChange(session, baseDir, workingDirectory, parameters);
    if (ok) {
        session->loadCount++;
        logInfo("game_change: next data.win is %s\n", session->dataWinPath);
    }
    free(baseDir);
    free(workingDirectory);
    free(parameters);
    return ok;
}

// Frees the data.win, then the process-wide scratch the chapter grew (heapfrag Stage 1.1; runner.c). Declared
// locally, not in runner.h: a declaration added to vm.h moved builtin_ds_list_set (1072 -> 1068 B) and every warm
// function after it.
void Runner_finishChapterUnload(DataWin* dataWin);

void CtrChapters_unloadGame(Runner* runner) {
    // Same order as the desktop runner's game_change path (engine/src/loop.c).
    DataWin* dataWin = runner->dataWin;
    VMContext* vm = runner->vmContext;
    FileSystem* fileSystem = runner->fileSystem;
    Video_closeAll(runner);
    runner->audioSystem->vtable->destroy(runner->audioSystem);
    runner->audioSystem = nullptr;
    runner->renderer->vtable->destroy(runner->renderer);
    Runner_free(runner);
    CtrFileSystem_destroy(fileSystem);
    VM_free(vm);
    Runner_finishChapterUnload(dataWin);
}

void CtrChapters_free(CtrGameSession* session) {
    arrfree(session->gameArgs);
    memset(session, 0, sizeof(*session));
}
