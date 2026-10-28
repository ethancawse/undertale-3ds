// Compact, cold chapter guard for DR New only. Original game code and art stay untouched.
#include <stdio.h>
#include <string.h>
#include "ctr_chapters.h"
#include "ctr_benchromfs.h"
#include "ctr_watchdog.h"
#include "ctr_wrap_chain.h"
#include "instance.h"
#include "vm.h"

#define BX __attribute__((noinline, section(".text.late.builderxnew")))
#define BR __attribute__((section(".rodata.late.builderxnew")))
// Use the already allocated writable reserve: appending BSS would change crt0/_sbrk_r end literals.
#define BB __attribute__((section(".data.late.builderxnew")))
#define STR(name, value) static const char name[] BR = value
STR(bnProgress, "Checking chapters");
STR(bnChapter, "chapter%d_windows");
STR(bnPath, "%s/%s%sdata.win");
STR(bnCache, "%s/%s%scache/data-version.txt");
STR(bnSlash, "/"); STR(bnEmpty, ""); STR(bnGame, "deltarune"); STR(bnRootPart, "root");
STR(bnRomfs, "romfs"); STR(bnManifest, "romfs:/chapters.txt"); STR(bnRead, "r");
STR(bnHeader, "%u %x"); STR(bnHash, "%64s"); STR(bnHex, "0123456789abcdef");
STR(bnRootPath, "%s/data.win"); STR(bnPartPath, "%s/chapter%d_windows/data.win");
STR(bnString, "%s");
STR(bnSlots, "_chapters"); STR(bnChapterNumber, "_chapter"); STR(bnEnabled, "_enabled");
STR(bnInput, "_input_enabled"); STR(bnIcon, "_icon_index"); STR(bnColor, "_color");
STR(bnChoice, "_chapter_choice"); STR(bnDisabled, "_disabled");
STR(bnEnable, "enable_input"); STR(bnDisable, "disable_input");
STR(bnList, "obj_screen_select_list"); STR(bnCreate, "instance_create_depth");

static char bnRoot[512] BB, bnHashes[6][65] BB;
static unsigned bnSelected BB, bnPlayable BB;
static bool bnHasManifest BB, bnLauncher BB, bnKnownLauncher BB;
static BuiltinFunc bnOriginal BB;

static BX void bnTick(void) { CtrWatchdog_mark(bnProgress); }

CtrDataVersionResult BxChapterHash_check(const char*, const char*, const char*, const char*,
                                      void (*)(void), bool*, char[65]);
#line 38
static BX bool bnCheck(int chapter) {
    char part[32], path[600], cache[640], digest[65];
    if (chapter) snprintf(part, sizeof(part), bnChapter, chapter);
    else part[0] = 0;
    snprintf(path, sizeof(path), bnPath, bnRoot, part, chapter ? bnSlash : bnEmpty);
    snprintf(cache, sizeof(cache), bnCache, bnRoot, part, chapter ? bnSlash : bnEmpty);
    bool cached;
    // Preflight must not replace the active room-plan/crisp-art digest with a different chapter's digest.
    CtrDataVersionResult result = BxChapterHash_check(path, bnGame, chapter ? part : bnRootPart,
                                                            cache, bnTick, &cached, digest);
    return result != CTR_DATA_UNREADABLE && (bnHasManifest ? !strcmp(digest, bnHashes[chapter]) :
                                                           result == CTR_DATA_KNOWN);
}

static BX void bnReadManifest(void) {
    bnHasManifest = false;
    bnSelected = 31;
    if (CtrRomfs_init(bnRomfs) < 0) return;
    FILE* file = fopen(bnManifest, bnRead);
    if (!file) return; // Older manual builds: only known, present files are playable.
    unsigned revision = 0, mask = 0;
    bool good = fscanf(file, bnHeader, &revision, &mask) == 2 && revision == 1 && mask && !(mask & ~31u);
    for (int i = 0; good && i < 6; ++i) {
        good = fscanf(file, bnHash, bnHashes[i]) == 1;
        if (good && (!i || (mask & (1u << (i - 1)))))
            good = strlen(bnHashes[i]) == 64 && strspn(bnHashes[i], bnHex) == 64;
        else if (good) good = bnHashes[i][0] == '-' && !bnHashes[i][1];
    }
    if (fclose(file)) good = false;
    bnHasManifest = true;
    bnSelected = good ? mask : 0; // Malformed is never mistaken for an older manual build.
    if (!good) memset(bnHashes, 0, sizeof(bnHashes));
}

static BX void bnRoute(CtrGameSession* session) {
    char path[600];
    snprintf(path, sizeof(path), bnRootPath, bnRoot);
    bnLauncher = !strcmp(session->dataWinPath, path);
    if (bnLauncher) return;
    for (int i = 1; i <= 5; ++i) {
        snprintf(path, sizeof(path), bnPartPath, bnRoot, i);
        if (!strcmp(session->dataWinPath, path) && (bnPlayable & (1u << (i - 1)))) {
            if (bnCheck(i)) return;
            bnPlayable &= ~(1u << (i - 1));
        }
    }
    CtrChapters_returnToLauncher(session, bnRoot);
    bnLauncher = true;
}

BX void BxN_init(CtrGameSession* session, const char* gameDir) {
    snprintf(bnRoot, sizeof(bnRoot), bnString, gameDir);
    bnReadManifest();
    bnKnownLauncher = bnCheck(0);
    bnPlayable = 0;
    if (bnKnownLauncher)
        for (int i = 1; i <= 5; ++i)
            if ((bnSelected & (1u << (i - 1))) && bnCheck(i)) bnPlayable |= 1u << (i - 1);
    CtrChapters_init(session, gameDir);
    bnRoute(session);
}

BX bool BxN_change(CtrGameSession* session, Runner* runner) {
    bool changed = CtrChapters_takeGameChange(session, runner);
    if (changed) bnRoute(session);
    return changed;
}

// These are existing fields of the hash-verified BC17 launcher, never arbitrary user variables.
// Only look up slots: insertion could invalidate another slot held by the filter.
static BX RValue* bnSlot(VMContext* ctx, Instance* instance, const char* name) {
    ptrdiff_t index = shgeti(ctx->varNameMap, name);
    if (!instance || index < 0) return NULL;
    return IntRValueHashMap_findSlot(&instance->selfVars, ctx->varNameMap[index].value);
}

static BX int bnNumber(const RValue* value) {
    if (!value) return -1;
    if (value->type == RVALUE_REAL) return (int) value->real;
    if (value->type == RVALUE_INT32 || value->type == RVALUE_ASSETREF) return value->int32;
    return -1;
}

static BX Instance* bnInstance(VMContext* ctx, const RValue* value) {
    int id = bnNumber(value);
    return id < 0 ? NULL : VM_findInstanceByTarget(ctx, id);
}

static BX void bnSet(VMContext* ctx, Instance* instance, const char* name, int value) {
    RValue* slot = bnSlot(ctx, instance, name);
    if (!slot && instance) {
        ptrdiff_t index = shgeti(ctx->varNameMap, name);
        if (index >= 0) slot = IntRValueHashMap_getOrInsertUndefined(&instance->selfVars, ctx->varNameMap[index].value);
    }
    if (slot && (slot->type == RVALUE_REAL || slot->type == RVALUE_INT32 || slot->type == RVALUE_BOOL ||
                 slot->type == RVALUE_UNDEFINED))
        *slot = RValue_makeReal(value);
}

static BX void bnFilter(VMContext* ctx, Instance* list) {
    RValue* slot = bnSlot(ctx, list, bnSlots);
    if (!slot || slot->type != RVALUE_ARRAY || !slot->array || slot->array->type != GML_MODERN_ARRAY) return;
    GMLArray* original = slot->array;
    GMLArray* playable = GMLArray_create(ctx->dataWin, original->modern.length);
    int written = 0;
    for (int i = 0; i < original->modern.length; ++i) {
        RValue value = original->modern.data[i];
        Instance* entry = bnInstance(ctx, &value);
        int number = bnNumber(bnSlot(ctx, entry, bnChapterNumber));
        if (number > 0 && number <= 5 && (bnPlayable & (1u << (number - 1)))) {
            // Instance IDs are numeric, so copying them never transfers an owned reference.
            value.ownsReference = false;
            playable->modern.data[written++] = value;
        } else {
            bnSet(ctx, entry, bnEnabled, 0);
            bnSet(ctx, entry, bnInput, 0);
            bnSet(ctx, entry, bnIcon, 0);
            bnSet(ctx, entry, bnColor, 0x808080);
            Instance* choice = bnInstance(ctx, bnSlot(ctx, entry, bnChoice));
            bnSet(ctx, choice, bnDisabled, 1);
            bnSet(ctx, choice, bnInput, 0);
            bnSet(ctx, choice, bnColor, 0x808080);
        }
    }
    playable->modern.length = written;
    if (slot->ownsReference) GMLArray_decRef(original);
    *slot = RValue_makeArray(playable); // Transfer the newly allocated strong reference; no alias of _slots.
    if (!written) {
        RValue* enable = bnSlot(ctx, list, bnEnable);
        RValue* disable = bnSlot(ctx, list, bnDisable);
        if (enable && disable && enable->type == RVALUE_METHOD && disable->type == RVALUE_METHOD) {
            GMLMethod_incRef(disable->method);
            if (enable->ownsReference) GMLMethod_decRef(enable->method);
            *enable = *disable;
            enable->ownsReference = true;
        }
    }
}

static BX RValue bnCreateInstance(VMContext* ctx, RValue* args, int32_t count) {
    RValue value = bnOriginal(ctx, args, count);
    Instance* instance = bnInstance(ctx, &value);
    if (instance && instance->objectIndex >= 0 && (uint32_t) instance->objectIndex < ctx->dataWin->objt.count) {
        const char* name = ctx->dataWin->objt.objects[instance->objectIndex].name;
        if (name && !strcmp(name, bnList)) bnFilter(ctx, instance);
    }
    return value;
}

BX Runner* BxN_create(DataWin* dataWin, VMContext* vm, Renderer* renderer, FileSystem* fileSystem,
                     AudioSystem* audioSystem, uint32_t randomSeed) {
    if (bnLauncher && bnKnownLauncher && bnPlayable != 31) {
        bnOriginal = VM_findBuiltin(vm, bnCreate);
        ptrdiff_t index = shgeti(vm->builtinMap, bnCreate);
        if (index >= 0 && bnOriginal) {
            vm->builtinMap[index].value = bnCreateInstance;
            for (uint32_t i = 0; i < vm->funcCallCacheCount; ++i)
                if (!strcmp(dataWin->func.functions[i].name, bnCreate))
                    vm->funcCallCache[i].builtin = (void*) bnCreateInstance;
        }
    }
    return Runner_create(dataWin, vm, renderer, fileSystem, audioSystem, randomSeed);
}
