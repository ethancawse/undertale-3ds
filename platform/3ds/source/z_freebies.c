// freebies (UNDERTALE): restarting the room into another planned scene gets the hidden room load. Late code, constants
// and statics only (platform/3ds/late_reserve.ld): nothing that existed before moves.
//
// main.c runs CtrRenderer_prefetchRoom (the room plan, roomload, utstall: the scene's code and tiles in the frame that
// isn't shown) only when the room index changes. UNDERTALE's battles also change scene by restarting the room: the
// Asriel phases and SAVEs (306:255 -> 256 -> 89/87), Undyne the Undying's transformation (306:91 -> 92) and others
// set global.battlegroup and restart the battle room (306), so the new group's planned tiles and code loaded in play
// instead (argutrans and argasrsave: nx_utrans 194-215 ms frames; Asriel SAVE frames with 6 code reads in Step).
//
// Here a restart is handled as main.c handles a room change, at the same point of the frame (right after the frame's
// CtrRenderer_endFrame, which the hidden frame still calls), when the room plan's scene key changes to another planned
// scene: (room, battle group) as the plan tells scenes apart (RoomPlan_sceneCode: the group's scene if the plan has
// one). Nothing new loads, as before, for a restart into the same key (a fight restarting itself), into a group the
// plan lacks (its scene would be the room's own, and utstall would keep the old group's tiles), or when the restarted
// room's events already asked for another room or a game restart (the load would be for a room left next frame).
//
// Records (rooms.dtm, perf only): the restart's own frames record nothing (recordRoom is cleared after the load). The
// load does what a room change's does: an entering visit still in its first 45 frames is finished and saved there, onto
// the remembered tiles utstall has just set to the new scene's; the room's next save writes those. utstall sets them
// again at every planned entry, so the next visit loads as before.
//
// Hooks (CMakeLists.txt, UNDERTALE only): Runner_handlePendingRoomChange (CtrBench_handleRoomChange's call) and
// RoomPlan_utstallPlan (z_utstall.c's call: the checked plan and its texture cache) are wrapped; CtrRenderer_endFrame's
// wrap chain (ctr_wrap_chain.h) gets a link after swkbd, which calls on to nopromo's (below).
//
// Scene keys whose restart loads nothing here (fbSkip): a restart frame keeps the last picture on screen, so its load is
// a held picture the player sees, unless the game has faded out. The Asriel restarts hold a faded picture: black
// before 255 -> 256, a full white flash around the SAVEs (256 <-> 87-90). Undyne the Undying (306:92) restarts from a
// lit battle picture, and the base already loads only the first frame's tiles in that frame (Azahar Old: held 33 + 63
// ms); the restart's load held it 243 ms (152 ms reading no page whole). Its tiles and code come in play, as before.
//
// nopromo (round 12 rev 2, 2026-10-29): with no RAM for packed pages, Draw asks no promotions. A tile load from a
// page's cache file (ctr_textures.c, loadRequestedFrom) first takes the file's size (CtrPack_fileSize, a size request
// to the card outside the load's logged steps, which waits behind an audio stream's read: nx_opera page 24's second
// tile, ~45 ms o2ds / ~35 ms o3ds over its read, check, unpack and flush) to ask for the page whole in RAM. On the Old
// models the UNDERTALE .3dsx's packed budget starts at 310-321 KB and z_utstall.c's utflPkCollect sets it to the heap
// share, 0, at the first RoomLoad_collect; pklin then stays off. With the budget 0, no packed copy and no whole read,
// it stays 0 (relieveHeap and relieveFailedAlloc set it to the packed bytes held, 0; keepPacked never adds over it;
// utflPk sets it once per cache), and every promotion fails at makePromotionRoom's size > budget before any read. So
// once, after the first such frame's CtrRenderer_endFrame with the tile reader on, every page's promotionRetryFrame is
// set to the last frame number (page 0's then marks it done): Draw and roomload's font read-backs no longer ask. What
// changes: the [perf] failed-promotions count and roomload's font-pages count. The tiles load as before. In sections of
// its own (late_reserve.ld, after every other late code), so nothing before it moves.

#if defined(ENABLE_WAD16) && defined(__3DS__)

#include <stdbool.h>
#include <stdint.h>

#include "ctr_renderer.h"
#include "ctr_textures.h"
#include "ctr_wrap_chain.h"
#include "log.h"
#include "renderer.h"
#include "roomplan_format.h"
#include "runner.h"

#define FB_TEXT __attribute__((noinline, section(".text.freebies_late")))
#define FB_CONST __attribute__((aligned(4), section(".rodata.freebies_in_text")))
#define FB_BSS __attribute__((section(".bss.freebies_late")))

int32_t RoomPlan_utstallGroup(Runner* runner);
const uint8_t* __real_RoomPlan_utstallPlan(const CtrTextureCache* cache);
const uint8_t* __wrap_RoomPlan_utstallPlan(const CtrTextureCache* cache);
void __real_Runner_handlePendingRoomChange(Runner* runner);
void __wrap_Runner_handlePendingRoomChange(Runner* runner);
void __real_CtrRenderer_endFrame(Renderer* renderer);

static FB_BSS struct {
    const uint8_t* plan;    // the checked plan, as z_utstall.c last asked for it
    CtrTextureCache* cache; // its texture cache
    Runner* restarted;      // a restart into another scene, for the end of this frame
    int32_t room, sub;      // the scene key of the last room load (sub -1: the room's own scene)
} fb;

static const int16_t fbSkip[][2] FB_CONST = {{306, 92}};

static const char fbLog[] FB_CONST = "Room restart: room %ld scene %ld -> %ld, room load\n";
static const char npLog[] __attribute__((aligned(4), section(".late_nopromo_ro"))) =
    "Textures: no RAM for packed pages: Draw asks no promotions (frame %lu)\n";

FB_TEXT const uint8_t* __wrap_RoomPlan_utstallPlan(const CtrTextureCache* cache) {
    const uint8_t* plan = __real_RoomPlan_utstallPlan(cache);
    if (plan != nullptr) {
        fb.plan = plan;
        fb.cache = (CtrTextureCache*) cache;
    }
    return plan;
}

FB_TEXT static int32_t fbGroup(void* runner) {
    return RoomPlan_utstallGroup(runner);
}

// The plan's scene key of the runner's room now: the battle group's scene if planned, else -1.
FB_TEXT static int32_t fbSub(Runner* runner) {
    uint32_t count = 0;
    int32_t sub = -1;
    if (fb.plan != nullptr && 0 <= runner->currentRoomIndex) {
        RoomPlan_sceneCode(fb.plan, (uint32_t) runner->currentRoomIndex, fbGroup, runner, &count, &sub);
    }
    return sub;
}

FB_TEXT void __wrap_Runner_handlePendingRoomChange(Runner* runner) {
    int32_t pending = runner->pendingRoom;
    int32_t before = runner->currentRoomIndex;
    __real_Runner_handlePendingRoomChange(runner);
    // No change asked (most frames). Any other value (a room, a game restart) leaves fb with the key now.
    if (pending == -1) return;
    int32_t room = runner->currentRoomIndex;
    int32_t sub = fbSub(runner);
    // Another room: main.c's room load takes this key at the end of the frame.
    bool skip = false;
    for (uint32_t i = 0; sizeof(fbSkip) / sizeof(fbSkip[0]) > i; i++) skip |= fbSkip[i][0] == room && fbSkip[i][1] == sub;
    bool restart = pending == before && room == before && 0 <= sub && runner->pendingRoom == -1;
    if (restart && (fb.room != room || fb.sub != sub) && !skip) {
        logInfo(fbLog, (long) room, (long) fb.sub, (long) sub);
        fb.restarted = runner;
    }
    fb.room = room;
    fb.sub = sub;
}

// After the frame's CtrRenderer_endFrame, where main.c runs a room change's CtrRenderer_prefetchRoom.
FB_TEXT void CTR_ENDFRAME_FREEBIES(Renderer* renderer) {
    CTR_ENDFRAME_AFTER_FREEBIES(renderer);
    Runner* runner = fb.restarted;
    if (runner != nullptr && renderer->runner == runner) {
        fb.restarted = nullptr;
        CtrRenderer_prefetchRoom(renderer);
        // The restart's own frames record nothing (Records, above).
        if (fb.cache != nullptr) fb.cache->recordRoom = -1;
    }
}

// nopromo (above), the chain's next link: the real CtrRenderer_endFrame, then the check.
__attribute__((noinline, section(".late_nopromo"))) void CTR_ENDFRAME_NOPROMO(Renderer* renderer) {
    __real_CtrRenderer_endFrame(renderer);
    CtrTextureCache* c = fb.cache;
    if (c == nullptr || c->pageCount == 0 || c->pages[0].promotionRetryFrame == UINT32_MAX || c->packedBudgetBytes != 0 ||
        c->packedBytes != 0 || c->wholeBytes != 0 || c->wholeRead != nullptr || c->reader == nullptr)
        return;
    for (uint32_t i = 0; c->pageCount > i; i++) c->pages[i].promotionRetryFrame = UINT32_MAX;
    logInfo(npLog, (unsigned long) c->frame);
}

#endif
