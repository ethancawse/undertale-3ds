// Cold availability/menu fixes. Original callers and data keep their emitted slots.
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "ctr_benchromfs.h"
#include "ctr_data_version.h"
#include "ctr_menu.h"
#include "ctr_watchdog.h"
#include "ctr_wrap_chain.h"

#define BX_REVIEW __attribute__((noinline, section(".text.late.zzzz_builderxreview")))
#define BX_REVIEW_RO __attribute__((section(".rodata.late.zzzz_builderxreview")))
#define BX_REVIEW_DATA __attribute__((section(".data.late.zzzz_builderxreview")))

static const char bxReviewScenePath[] BX_REVIEW_RO = CTRPORT_GAME_DIR "/benchscenes.txt";
static const char bxReviewRomfs[] BX_REVIEW_RO = "romfs";
static const char bxReviewRomfsScenes[] BX_REVIEW_RO = "romfs:/bench_scenes.txt";
static int bxReviewScenes BX_REVIEW_DATA = -1;

bool CtrBuilderx_hasBenchScenes(void);
// The romfs's list first: every build packs bench_scenes.txt there (CMakeLists), so the SD card's override is looked
// up only when that one is empty. A stat of a missing name in the game's SD folder took 400-440 ms on the Old test
// consoles (docs/3ds-hardware.md), and the options menu's first page paid it on the game thread (the Debug row).
BX_REVIEW bool CtrBuilderx_hasBenchScenes(void) {
    if (bxReviewScenes < 0) {
        struct stat st;
        // Named first, as main's version did, so GCC emits the three strings in main's order.
        const char* sdPath = bxReviewScenePath;
        __asm__("" : "+r"(sdPath));
        bxReviewScenes = CtrRomfs_init(bxReviewRomfs) >= 0 && stat(bxReviewRomfsScenes, &st) == 0 && st.st_size > 0;
        if (!bxReviewScenes) bxReviewScenes = stat(sdPath, &st) == 0 && st.st_size > 0;
    }
    // The branch over a fill that kept main's 228 B went in argcheck's UT late-text reclaim (a layout event).



    return bxReviewScenes != 0;
}

// Mirrors the private OptRow ABI; compiler-launcher tests bind its fields and the Debug row's ordinal (ctr_optmenu.c's
// OPT_MAIN_DEBUG: 6 since legalui's option C took the Other row out; tools/release/test_builder_review.py checks it).
typedef struct {
    const char* label;
    const char* value;
    const char* help;
    bool dis;
} BxMenuRow;
_Static_assert(offsetof(BxMenuRow, dis) == 3 * sizeof(void*), "options row ABI");
enum { BX_MENU_DEBUG = 6 };
void Bx_menuRowOriginal(const CtrMenu* menu, uint32_t index, BxMenuRow* row);
CtrMenuAction Bx_menuActivateOriginal(CtrMenu* menu, uint32_t index, int32_t direction);
void Bx_menuRow(const CtrMenu* menu, uint32_t index, BxMenuRow* row);
CtrMenuAction Bx_menuActivate(CtrMenu* menu, uint32_t index, int32_t direction);

BX_REVIEW void Bx_menuRow(const CtrMenu* menu, uint32_t index, BxMenuRow* row) {
    Bx_menuRowOriginal(menu, index, row);
    if (index == BX_MENU_DEBUG) row->dis = !CtrBuilderx_hasBenchScenes();
}

BX_REVIEW CtrMenuAction Bx_menuActivate(CtrMenu* menu, uint32_t index, int32_t direction) {
    if (index == BX_MENU_DEBUG && direction >= 0 && !CtrBuilderx_hasBenchScenes()) return MENU_ACTION_NONE;
    return Bx_menuActivateOriginal(menu, index, direction);
}

#ifndef ENABLE_WAD16
void CtrLoadscreen_chapterHashesBegin(uint32_t size);
void CtrLoadscreen_chapterHashProgress(void);
static const char bxReviewChecking[] BX_REVIEW_RO = "Checking chapters";
static const char bxReviewRoot[] BX_REVIEW_RO = "root";
static const char bxReviewManifest[] BX_REVIEW_RO = "romfs:/chapters.txt";
static const char bxReviewRead[] BX_REVIEW_RO = "r";
static const char bxReviewManifestFormat[] BX_REVIEW_RO = "%u %x";
static const char bxReviewChapterFormat[] BX_REVIEW_RO = "%s/chapter%d_windows/data.win";

static BX_REVIEW void bxReviewProgress(void) {
    CtrWatchdog_mark(bxReviewChecking);
    CtrLoadscreen_chapterHashProgress();
}

static BX_REVIEW void bxReviewBegin(const char* launcher) {
    char root[600], path[640];
    size_t length = strlen(launcher);
    if (length >= sizeof(root)) return;
    memcpy(root, launcher, length + 1);
    char* slash = strrchr(root, '/');
    if (slash == NULL) return;
    *slash = '\0';
    unsigned revision = 0, mask = 31;
    FILE* file = fopen(bxReviewManifest, bxReviewRead);
    if (file != NULL) {
        unsigned selected;
        if (fscanf(file, bxReviewManifestFormat, &revision, &selected) == 2 && revision == 1 && !(selected & ~31u))
            mask = selected;
        fclose(file);
    }
    uint32_t total = 0;
    struct stat st;
    if (stat(launcher, &st) == 0 && st.st_size > 0) total += (uint32_t) st.st_size;
    for (int chapter = 1; 5 >= chapter; chapter++) {
        if (!(mask & (1u << (chapter - 1)))) continue;
        snprintf(path, sizeof(path), bxReviewChapterFormat, root, chapter);
        if (stat(path, &st) == 0 && st.st_size > 0) total += (uint32_t) st.st_size;
    }
    CtrLoadscreen_chapterHashesBegin(total);
}

CtrDataVersionResult Bx_preflightCheck(const char* path, const char* game, const char* part,
    const char* cache, void (*progress)(void), bool* cached, char digest[65]);
BX_REVIEW CtrDataVersionResult Bx_preflightCheck(const char* path, const char* game, const char* part,
    const char* cache, void (*progress)(void), bool* cached, char digest[65]) {
    (void) progress;
    if (strcmp(part, bxReviewRoot) == 0) bxReviewBegin(path);
    char directory[640];
    size_t length = strlen(cache);
    if (length < sizeof(directory)) {
        memcpy(directory, cache, length + 1);
        char* slash = strrchr(directory, '/');
        if (slash != NULL) { *slash = '\0'; mkdir(directory, 0777); }
    }
    // Availability is not a load: in particular, the room-plan wrapper must not reset the live plan.
    return __real_CtrDataVersion_check(path, game, part, cache, bxReviewProgress, cached, digest);
}
#endif
