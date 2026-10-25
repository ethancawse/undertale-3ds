// Personal chapter selection and unavailable entries. Cold launcher hooks only; no game bytecode or art is patched.
// All-present runs keep the original builtin table and every game variable untouched.
#ifndef ENABLE_WAD16
// Give this object's out-of-line copies of header helpers their own late labels.
// Existing copies in other objects retain their names, addresses, sizes and bytes.
#define Instance_setSelfVar bxInstance_setSelfVar
#define RValue_free bxRValue_free
#define RValue_makeIndependent bxRValue_makeIndependent
#define RValue_makeUndefined bxRValue_makeUndefined
#define RValue_toInt32Other bxRValue_toInt32Other
#define requireMessageFormatted bxRequireMessageFormatted
#include <stdio.h>
#include <string.h>
#include "ctr_chapters.h"
#include "ctr_data_version.h"
#include "ctr_benchromfs.h"
#include "ctr_watchdog.h"
#include "ctr_wrap_chain.h"
#include "instance.h"
#include "vm.h"

#define BX_CODE __attribute__((noinline, section(".text.late.builderx")))
#define BX_BSS __attribute__((section(".bss.late.builderx")))

static char bxRoot[512] BX_BSS;
static char bxHashes[6][65] BX_BSS;
static unsigned bxSelected BX_BSS;
static unsigned bxPlayable BX_BSS;
static bool bxManifest BX_BSS;
static bool bxLauncher BX_BSS;
static bool bxKnownLauncher BX_BSS;
static BuiltinFunc bxCreateOriginal BX_BSS;

static BX_CODE void bxProgress(void) { CtrWatchdog_mark("Checking chapters"); }

static BX_CODE bool bxCheck(int chapter) {
    char part[32], path[600], cache[640], digest[65];
    if (chapter) snprintf(part, sizeof(part), "chapter%d_windows", chapter);
    else part[0] = '\0';
    snprintf(path, sizeof(path), "%s/%s%sdata.win", bxRoot, part, chapter ? "/" : "");
    snprintf(cache, sizeof(cache), "%s/%s%scache/data-version.txt", bxRoot, part, chapter ? "/" : "");
    bool cached;
    CtrDataVersionResult result = CtrDataVersion_check(path, "deltarune", chapter ? part : "root", cache,
                                                     bxProgress, &cached, digest);
    if (result == CTR_DATA_UNREADABLE) return false;
    // The builder's single policy switch decides which input hashes a personal build may contain.
    if (bxManifest) return strcmp(digest, bxHashes[chapter]) == 0;
    return result == CTR_DATA_KNOWN;
}

static BX_CODE void bxReadManifest(void) {
    bxManifest = false;
    bxSelected = 31;
    memset(bxHashes, 0, sizeof(bxHashes));
    if (CtrRomfs_init("romfs") < 0) return;
    FILE* file = fopen("romfs:/chapters.txt", "r");
    if (file == NULL) return; // Manual builds from before chapter selection: all known, present chapters.
    unsigned revision = 0, mask = 0;
    bool good = fscanf(file, "%u %x", &revision, &mask) == 2 && revision == 1 && mask && !(mask & ~31u);
    for (int i = 0; good && 6 > i; i++) {
        good = fscanf(file, "%64s", bxHashes[i]) == 1;
        if (good && (i == 0 || (mask & (1u << (i - 1)))))
            good = strlen(bxHashes[i]) == 64 && strspn(bxHashes[i], "0123456789abcdef") == 64;
        else if (good) good = strcmp(bxHashes[i], "-") == 0;
    }
    if (fclose(file) != 0) good = false;
    bxManifest = true; // A malformed manifest fails closed, never becomes "all chapters".
    bxSelected = good ? mask : 0;
    if (!good) memset(bxHashes, 0, sizeof(bxHashes));
}

static BX_CODE void bxRoute(CtrGameSession* session) {
    char launcher[600];
    snprintf(launcher, sizeof(launcher), "%s/data.win", bxRoot);
    bxLauncher = strcmp(session->dataWinPath, launcher) == 0;
    if (bxLauncher) return;
    for (int i = 1; 5 >= i; i++) {
        char path[600];
        snprintf(path, sizeof(path), "%s/chapter%d_windows/data.win", bxRoot, i);
        if (strcmp(session->dataWinPath, path) == 0 && (bxPlayable & (1u << (i - 1)))) {
            if (bxCheck(i)) return;
            bxPlayable &= ~(1u << (i - 1));
        }
    }
    // Direct boot, saved continuation, next chapter, an SD file removed/changed since startup: return safely.
    CtrChapters_returnToLauncher(session, bxRoot);
    bxLauncher = true;
}

void __real_CtrChapters_init(CtrGameSession* session, const char* gameDir);
void __wrap_CtrChapters_init(CtrGameSession* session, const char* gameDir);
BX_CODE void __wrap_CtrChapters_init(CtrGameSession* session, const char* gameDir) {
    snprintf(bxRoot, sizeof(bxRoot), "%s", gameDir);
    bxReadManifest();
    bxKnownLauncher = bxCheck(0);
    bxPlayable = 0;
    for (int i = 1; 5 >= i; i++)
        if ((bxSelected & (1u << (i - 1))) && bxCheck(i)) bxPlayable |= 1u << (i - 1);
    __real_CtrChapters_init(session, gameDir);
    bxRoute(session);
}

bool __real_CtrChapters_takeGameChange(CtrGameSession* session, Runner* runner);
bool __wrap_CtrChapters_takeGameChange(CtrGameSession* session, Runner* runner);
BX_CODE bool __wrap_CtrChapters_takeGameChange(CtrGameSession* session, Runner* runner) {
    bool changed = __real_CtrChapters_takeGameChange(session, runner);
    if (changed) bxRoute(session);
    return changed;
}

static BX_CODE int32_t bxVar(VMContext* ctx, const char* name) {
    ptrdiff_t index = shgeti(ctx->varNameMap, name);
    return index < 0 ? -1 : ctx->varNameMap[index].value;
}

static BX_CODE RValue bxGet(VMContext* ctx, Instance* instance, const char* name) {
    int32_t id = bxVar(ctx, name);
    return instance == NULL || id < 0 ? RValue_makeUndefined() : Instance_getSelfVar(instance, id);
}

static BX_CODE void bxSet(VMContext* ctx, Instance* instance, const char* name, int value) {
    int32_t id = bxVar(ctx, name);
    if (instance != NULL && id >= 0) Instance_setSelfVar(instance, id, RValue_makeReal(value));
}

static BX_CODE Instance* bxInstance(VMContext* ctx, RValue value) {
    if (value.type != RVALUE_REAL && value.type != RVALUE_INT32 && value.type != RVALUE_ASSETREF) return NULL;
    return VM_findInstanceByTarget(ctx, RValue_toInt32(value));
}

static BX_CODE void bxFilterList(VMContext* ctx, Instance* list) {
    RValue slots = bxGet(ctx, list, "_slots");
    RValue chapters = bxGet(ctx, list, "_chapters");
    if (slots.type != RVALUE_ARRAY || chapters.type != RVALUE_ARRAY) return;
    // Keep all visual slots in place. Only the game's navigable array is compacted, preserving gaps.
    GMLArray* playable = GMLArray_create(ctx->dataWin, 0);
    int count = GMLArray_length1D(chapters.array);
    for (int i = 0; count > i; i++) {
        RValue value = GMLArray_get(chapters.array, i);
        Instance* entry = bxInstance(ctx, value);
        int number = RValue_toInt32(bxGet(ctx, entry, "_chapter"));
        if (number > 0 && 5 >= number && (bxPlayable & (1u << (number - 1)))) {
            GMLArray_set(playable, GMLArray_length1D(playable), value);
        } else {
            bxSet(ctx, entry, "_enabled", 0);
            bxSet(ctx, entry, "_input_enabled", 0);
            bxSet(ctx, entry, "_icon_index", 0);
            bxSet(ctx, entry, "_color", 0x808080);
            Instance* choice = bxInstance(ctx, bxGet(ctx, entry, "_chapter_choice"));
            bxSet(ctx, choice, "_disabled", 1);
            bxSet(ctx, choice, "_input_enabled", 0);
            bxSet(ctx, choice, "_color", 0x808080);
        }
    }
    int32_t id = bxVar(ctx, "_chapters");
    if (id >= 0) {
        RValue value = RValue_makeArray(playable);
        Instance_setSelfVar(list, id, value);
        RValue_free(&value);
    } else GMLArray_decRef(playable);
    // A card emptied after delivery still draws all disabled slots; it cannot index an empty navigable list.
    if (!bxPlayable) {
        int32_t enable = bxVar(ctx, "enable_input");
        RValue disable = bxGet(ctx, list, "disable_input");
        if (enable >= 0 && disable.type != RVALUE_UNDEFINED) Instance_setSelfVar(list, enable, disable);
    }
}

static BX_CODE RValue bxCreate(VMContext* ctx, RValue* args, int32_t count) {
    RValue value = bxCreateOriginal(ctx, args, count);
    Instance* instance = bxInstance(ctx, value);
    if (instance != NULL && instance->objectIndex >= 0 && ctx->dataWin->objt.count > (uint32_t) instance->objectIndex) {
        const char* name = ctx->dataWin->objt.objects[instance->objectIndex].name;
        if (name != NULL && strcmp(name, "obj_screen_select_list") == 0) bxFilterList(ctx, instance);
    }
    return value;
}

Runner* __real_Runner_create(DataWin* dataWin, VMContext* vm, Renderer* renderer, FileSystem* fileSystem,
                            AudioSystem* audioSystem, uint32_t randomSeed);
Runner* CTR_RUNNERCREATE_BUILDERX(DataWin* dataWin, VMContext* vm, Renderer* renderer, FileSystem* fileSystem,
                            AudioSystem* audioSystem, uint32_t randomSeed);
BX_CODE Runner* CTR_RUNNERCREATE_BUILDERX(DataWin* dataWin, VMContext* vm, Renderer* renderer, FileSystem* fileSystem,
                                    AudioSystem* audioSystem, uint32_t randomSeed) {
    if (bxLauncher && bxKnownLauncher && bxPlayable != 31) {
        bxCreateOriginal = VM_findBuiltin(vm, "instance_create_depth");
        ptrdiff_t index = shgeti(vm->builtinMap, "instance_create_depth");
        if (index >= 0 && bxCreateOriginal != NULL) {
            vm->builtinMap[index].value = bxCreate;
            for (uint32_t i = 0; vm->funcCallCacheCount > i; i++)
                if (strcmp(dataWin->func.functions[i].name, "instance_create_depth") == 0)
                    vm->funcCallCache[i].builtin = (void*) bxCreate;
        }
    }
    return __real_Runner_create(dataWin, vm, renderer, fileSystem, audioSystem, randomSeed);
}
#endif
