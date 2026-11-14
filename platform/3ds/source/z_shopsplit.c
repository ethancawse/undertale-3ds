// Dedicated-shop S3 consumer. A missing or rejected table stays Original.
#if defined(ENABLE_WAD16) && defined(__3DS__)
#include "renderer.h"
#include "runner.h"
#include "ctr_layout.h"
#include "ctr_shopsplit.h"

// Separate object: ctr_renderer.o's old .bss wildcard must not take this early.
// Bit 0: GPU preparation requested (including partial failure); bit 1: armed.
uint32_t ctrShopsplitState __attribute__((section(".bss.late.shopsplit")));

__attribute__((noinline, noipa, section(".text.late.shopsplit")))
bool CtrShopsplit_admitted(Renderer* renderer, const CtrDisplaySettings* settings) {
    if (renderer == nullptr || renderer->runner == nullptr || settings == nullptr) return false;
    CtrLayoutPlaces places;
    CtrLayout_places(settings, &places);
    if (places.game != CTR_LAYOUT_GAME_TOP || places.text != CTR_LAYOUT_OTHER) return false;
    Runner* runner = renderer->runner;
    const CtrLayoutScene* scene = CtrLayout_scene(runner->dataWin, runner->currentRoomIndex, -1);
    return scene != nullptr && scene->mode == CTR_LAYOUT_MODE_SHOP && scene->flags == CTR_LAYOUT_SCENE_SPLIT &&
           scene->band == CTR_SHOPSPLIT_BAND && scene->lift == 0 && scene->count == 0;
}
#endif
