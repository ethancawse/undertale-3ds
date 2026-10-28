// chapterleak: the chapter switch gives back the textures the renderer made for effects before the renderer goes
// (ctr_renderer.c, CtrRenderer_releaseChapterTextures, which says why). Wrapped (-Wl,--wrap), so ctr_chapters.c and
// main.c stay as they were. Late code only (platform/3ds/late_reserve.ld): nothing that existed before moves.
#include "runner.h"

void CtrRenderer_releaseChapterTextures(Renderer* renderer);
void __real_CtrChapters_unloadGame(Runner* runner);
void __wrap_CtrChapters_unloadGame(Runner* runner);

__attribute__((noinline, section(".text.late.chapterleak"))) void __wrap_CtrChapters_unloadGame(Runner* runner) {
    WAD17_ONLY(if (runner != nullptr && runner->renderer != nullptr) CtrRenderer_releaseChapterTextures(runner->renderer);)
    __real_CtrChapters_unloadGame(runner);
}
