#pragma once

// The room plan file, romfs:/roomplan/<part>.dtp (roomplan; written by tools/roomplan/build_plan.py, read by
// z_roomplan.c). Numbers only: no names, pixels or code. Little-endian, 4-byte aligned.
//
//   RoomPlanHeader
//   sceneCount x { RoomPlanScene, tileCount x uint32 tile, tileCount x uint8 bucket (padded to 4),
//                  codeCount x uint16 code entry (padded to 4) }
//
// A tile is rooms.dtm's entry (ctr_textures.c): late (bit 31) | page << 8 | tile, the tile numbered row by row in the
// page's grid of power-of-two tiles of at most ROOMPLAN_TILE_MAX texels a side (CtrPack_layoutGrid). Its bucket is
// the frame of first use after the room change (0-254, 255 later). A scene is a room (sub ROOMPLAN_NO_SUB) or, for
// battle rooms, a room and a battle group. Code entries: those first run after the room change, in first-use order
// (phase 2: read back at the room change where the bytecode lives on file).

#include <stddef.h>
#include <stdint.h>

#define ROOMPLAN_MAGIC 0x50525444u // "DTRP"
#define ROOMPLAN_VERSION 1u
#define ROOMPLAN_TILE_MAX 256u     // CTR_TILE_MAX the plan was made for
#define ROOMPLAN_GRID_VERSION 1u   // the tile grid rule (CtrPack_layoutGrid: the smallest power of two >= 8, at most the max)
#define ROOMPLAN_TILES_MAX 512u    // ROOM_TILES_MAX: rooms.dtm keeps no more per room
#define ROOMPLAN_NO_SUB 0xFFFFu
#define ROOMPLAN_MAX_SIZE (1024u * 1024u)
#define ROOMPLAN_SEED_FRAMES 45u   // ROOM_RECORD_FRAMES: a room-level scene seeds the tiles first used by then

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t sha256[32];    // of the data.win it was made from
    uint32_t dataWinSize;  // CtrPackKey of that data.win
    uint32_t gameId;
    uint64_t timestamp;
    uint32_t tileMax;
    uint32_t gridVersion;
    uint32_t roomCount;    // the data.win's
    uint32_t sceneCount;
    uint32_t bodySize;     // what follows the header
    uint32_t checksum;     // FNV-1a of the body
    char builder[16];      // the builder's version, for the log only
} RoomPlanHeader;

typedef struct {
    uint16_t room;
    uint16_t sub;          // ROOMPLAN_NO_SUB, or the battle group
    uint16_t tileCount;
    uint16_t codeCount;
    uint32_t tileBytes;    // the tiles' texture memory at most (4 bytes a texel), for capping without walking them
} RoomPlanScene;

_Static_assert(sizeof(RoomPlanHeader) == 96, "plan header layout");
_Static_assert(sizeof(RoomPlanScene) == 12, "plan scene layout");

static inline __attribute__((always_inline)) size_t RoomPlan_sceneSize(const RoomPlanScene* scene) {
    return sizeof(RoomPlanScene) + (size_t) scene->tileCount * 4 + (((size_t) scene->tileCount + 3) & ~(size_t) 3)
         + (((size_t) scene->codeCount * 2 + 3) & ~(size_t) 3);
}

typedef enum {
    ROOMPLAN_OK,
    ROOMPLAN_NO_FILE,
    ROOMPLAN_BAD_FILE,
    ROOMPLAN_OTHER_DATA,
    ROOMPLAN_OTHER_VERSION,
    ROOMPLAN_NO_DIGEST,
    ROOMPLAN_NO_MEMORY
} RoomPlanStatus;

// rooms.dtm's header (ctr_textures.c, RoomMemoryHeader): magic, version, CtrPackKey (16 B), tileMax, roomCount,
// bodySize, checksum. Only the magic, body size and checksum are touched here.
#define ROOMPLAN_DTM_MAGIC 0x4D525444u // "DTRM"
#define ROOMPLAN_DTM_HEADER_SIZE 40u
#define ROOMPLAN_DTM_BODY_SIZE_AT 32u
#define ROOMPLAN_DTM_CHECKSUM_AT 36u

uint32_t RoomPlan_fnv1a(const uint8_t* data, size_t size);
RoomPlanStatus RoomPlan_check(const uint8_t* data, size_t size, const char* digest, uint32_t dataWinSize, uint32_t gameId,
                              uint64_t timestamp, uint32_t roomCount);
void RoomPlan_forEachRoom(const uint8_t* data, void (*take)(void*, uint32_t, const uint32_t*, uint32_t), void* context);
const uint8_t* RoomPlan_sceneCode(const uint8_t* data, uint32_t room, int32_t (*group)(void*), void* context, uint32_t* count,
                                  int32_t* sub);
size_t RoomPlan_dropPlanned(uint8_t* data, size_t size, _Bool (*same)(void*, uint32_t, const uint32_t*, uint32_t), void* context,
                            uint32_t* dropped);
