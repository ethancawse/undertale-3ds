// UT-028: private build-generated texels; no artwork is stored in source.
// Load/decode only at hidden EX entry (or while the options menu holds play).
// smartCleanCopy's existing lookup/sampling is unchanged; no Draw hook or I/O.
#if defined(ENABLE_WAD16)
#include <3ds.h>
#include <citro3d.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ctr_benchromfs.h"
#include "ctr_lz.h"
#include "ctr_memory.h"
#include "ctr_texture_pack.h"
#include "ctr_wrap_chain.h"
#include "log.h"
#include "renderer.h"
#include "runner.h"

#define TA_TEXT __attribute__((noinline, section(".text.late.thinart")))
#define TA_CONST __attribute__((aligned(4), section(".rodata.late.thinart")))
typedef struct {
    uint32_t magic;
    uint16_t version, header;
    uint32_t fileBytes, payloadBytes, dataBytes, gameId;
    uint64_t timestamp;
    char digest[64];
    uint32_t item;
    uint16_t x, y, w, h, tw, th;
    uint8_t format, modes, ink[4];
    uint16_t reserved;
    uint32_t crc, reserved2;
} TaHeader;
typedef struct {
    int32_t tpag, x, y;
    uint32_t usedFrame;
    C3D_Tex tex;
} TaCopy;
_Static_assert(sizeof(TaHeader) == 128 && sizeof(TaCopy) == 40, "thinart ABI");
_Static_assert(offsetof(TaHeader, w) == 104 && offsetof(TaHeader, dataBytes) == 16 &&
               sizeof(CtrPackKey) == 16 &&
               offsetof(TexturePageItem, sourceHeight) - offsetof(TexturePageItem, sourceX) == 6,
               "thinart contiguous metadata ABI");
// Frozen landing-stage offsets, checked in the renderer TU on BOTH models.
#define TA_COPIES(r) ((TaCopy*) ((char*) (r) + 0x3260))
#define TA_COUNT(r) (*(int32_t*) ((char*) (r) + 0x34e0))
#define TA_FRAME(r) (*(uint32_t*) ((char*) (r) + 0x515c))
extern const uint32_t Smartiv_layout[10];
uint8_t* Krsharp_verdicts(Renderer* renderer);
uint8_t* Krsharp_ink(Renderer* renderer, int32_t item);
int32_t RoomPlan_utstallGroup(Runner* runner);
void __real_CtrRenderer_prefetchRoom(Renderer* renderer);
uint32_t CTR_DISPLAYCAPS_ITEMVERDICT(Renderer* renderer);

static const char taPath[] TA_CONST = "romfs:/fonts/undertale.dtta";
static const char taMount[] TA_CONST = "romfs";
static const char taMode[] TA_CONST = "rb";
static const char taRoom[] TA_CONST = "room_battle";
// Header's fixed geometry/format/modes/ink/reserved fields (offset 104..119).
// Metadata only; one memcmp retains all guards without repeated ARM branches.
static const uint16_t taShape[8] TA_CONST = {251, 172, 256, 256, 0x0309, 0, 0xacfb, 0};
static const char taLoaded[] TA_CONST = "Thinart: item %lu prebuilt LA4 65536 B, entry %.3f ms\n";
static const char taFailed[] TA_CONST = "Thinart: stock art (asset unavailable or invalid)\n";
static struct {
    Renderer* owner;
    TaCopy* copy;
    uint8_t* state;
    const void* room;
    int32_t item;
    char digest[65];
    bool known;
    bool wait; // logsweep: every Smart copy was in use at entry; in the struct's padding, so .bss doesn't grow
} ta __attribute__((section(".bss.late.thinart")));

TA_TEXT CtrDataVersionResult Thinart_check(CTR_DATAVERSION_CHAIN_PARAMS) {
    CtrDataVersionResult result = CTR_DATAVERSION_CREDFIX(path, game, part, cachePath, progress, cached, digest);
    // The old renderer owns/deletes any installed texture on restart. Never
    // dereference its saved pointers after the data-version boundary.
    ta.owner = nullptr;
    ta.copy = nullptr;
    ta.room = nullptr;
    ta.known = result == CTR_DATA_KNOWN && digest != nullptr;
    if (ta.known) memcpy(ta.digest, digest, 65);
    return result;
}

static TA_TEXT bool taLoad(Renderer* r) {
    FILE* f = nullptr;
    uint8_t* buffer = nullptr;
    bool ok = false;
    uint64_t start = svcGetSystemTick();
    if (!ta.known || R_FAILED(CtrRomfs_init(taMount))) return false;
    f = fopen(taPath, taMode);
    if (f == nullptr) return false;
    setvbuf(f, nullptr, _IONBF, 0);
    bool relief = CtrMemory_allowRelief(false);
    buffer = malloc(128 + 4096 + 1);
    CtrMemory_allowRelief(relief);
    if (buffer == nullptr) goto done;
    size_t bytes = fread(buffer, 1, 128 + 4096 + 1, f);
    if (bytes < sizeof(TaHeader)) goto done;
    TaHeader* header = (TaHeader*) buffer;
#define h (*header)
    CtrPackKey key = CtrPack_keyFor(r->dataWin);
    if (h.magic != 0x41545444u || h.version != 1 || h.header != sizeof(h) ||
        h.payloadBytes == 0 || h.payloadBytes > 4096 || h.fileBytes != h.payloadBytes + sizeof(h) ||
        memcmp(&h.dataBytes, &key, sizeof(key)) != 0 ||
        memcmp(h.digest, ta.digest, 64) != 0 || h.item >= *(uint32_t*) ((char*) r + Smartiv_layout[2]) ||
        bytes != h.fileBytes ||
        memcmp(&h.w, taShape, sizeof(taShape)) != 0 || h.reserved2 != 0) goto done;
    TexturePageItem* item = &r->dataWin->tpag.items[h.item];
    if (!item->present || memcmp(&item->sourceX, &h.x, 8) != 0) goto done;
    const uint8_t* payload = buffer + sizeof(TaHeader);
    uint32_t crc = UINT32_MAX;
    for (uint32_t i = 0; i < h.payloadBytes; i++) {
        crc ^= payload[i];
        for (uint32_t b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    if (~crc != h.crc) goto done;
    int32_t slot = 0;
    while (slot < TA_COUNT(r) && TA_COPIES(r)[slot].tpag >= 0) slot++;
    if (slot >= 16) goto done;
    uint8_t* state = Krsharp_verdicts(r);
    uint8_t* ink = Krsharp_ink(r, (int32_t) h.item);
    if (state == nullptr || ink == nullptr) goto done;
    TaCopy* c = &TA_COPIES(r)[slot];
    if (!C3D_TexInit(&c->tex, 256, 256, GPU_LA4)) goto done;
    if (!CtrLz_decompress(payload, h.payloadBytes, c->tex.data, 65536)) {
        C3D_TexDelete(&c->tex);
        goto done;
    }
    GSPGPU_FlushDataCache(c->tex.data, 65536);
    C3D_TexSetWrap(&c->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    c->tpag = (int32_t) h.item;
    c->x = (int32_t) h.x - 1;
    c->y = (int32_t) h.y - 1;
    c->usedFrame = TA_FRAME(r);
    if (slot == TA_COUNT(r)) TA_COUNT(r)++;
    ta.copy = c;
    ta.item = (int32_t) h.item;
    ta.state = &state[h.item];
    memcpy(ink, h.ink, 4);
    *ta.state = 0xd0;
    ok = true;
done:
    fclose(f);
    free(buffer);
    if (ok) logInfo(taLoaded, (unsigned long) ta.item, (double) (svcGetSystemTick() - start) / CPU_TICKS_PER_MSEC);
#undef h
    return ok;
}

static TA_TEXT void taValidate(void) {
    if (ta.copy != nullptr && ta.copy->tpag != ta.item) {
        *ta.state = 0x80;
        ta.copy = nullptr; // Another item owns this slot: never delete it.
    }
}

static TA_TEXT void taRoomUpdate(Renderer* r) {
    taValidate();
    Runner* runner = r->runner;
    int32_t group = runner == nullptr ? -1 : RoomPlan_utstallGroup(runner);
    bool active = runner != nullptr && runner->currentRoom != nullptr &&
        strcmp(runner->currentRoom->name, taRoom) == 0 && (group == 57 || group == 80 || group == 81);
    if (!active) {
        if (ta.owner == r) {
            if (ta.copy != nullptr) {
                *ta.state = 0x80;
                C3D_TexDelete(&ta.copy->tex);
                ta.copy->tpag = -1;
            }
            ta.owner = nullptr;
            ta.copy = nullptr;
        }
        return;
    }
    // Retain across colour/smoothing changes: one identical atlas covers both.
    if (ta.owner != r && r->dataWin != nullptr &&
        *(uint8_t*) ((char*) r + Smartiv_layout[8]) == Smartiv_layout[9]) {
        ta.owner = r; // Failed loads are not retried in Draw or each menu query.
        ta.copy = nullptr;
        if (!taLoad(r)) logInfo(taFailed);
    }
    // ivLoad only fills entries whose state is 0; it preserves this published
    // 0xd0 entry and its ink. Both colour modes use identical atlas texels.
}

TA_TEXT void Thinart_prefetchRoom(Renderer* r) {
    taRoomUpdate(r);
    CTR_PREFETCHROOM_AFTER_THINART(r);
}

TA_TEXT uint32_t Thinart_caps(Renderer* r) {
    uint32_t caps = CTR_DISPLAYCAPS_ITEMVERDICT(r);
    taRoomUpdate(r);
    return caps;
}

TA_TEXT void Thinart_endFrame(Renderer* r) {
    taValidate();
    // The cache evicts only ages >= 2. Pin at age 0/1 until EX exit; the
    // renderer destructor is the only other deletion path for Smart copies.
    if (ta.owner == r && ta.copy != nullptr) ta.copy->usedFrame = TA_FRAME(r);
    CTR_ENDFRAME_SWKBD(r);
    // Submit this frame's batches before releasing their atlas. Main skips
    // presentation on room change; install still precedes the next room's
    // first Draw and main's following texture prefetch.
    if (r->runner != nullptr && ta.room != r->runner->currentRoom) {
        ta.room = r->runner->currentRoom;
        taRoomUpdate(r);
    }
}

// ===[ logsweep (S8b): the bubble after any number of Smart copies, and on GOG ]===
//
// The functions above are unchanged; these links wrap them (ctr_wrap_chain.h) and are appended after every other
// late input (late_reserve.ld), so no earlier late code moves.
#define TA_LINK __attribute__((noinline, section(".text_thinslot")))
// The room's name again, after this code in late text: a use of taRoom here would reorder thinart's constants above.
static const char taSlotRoom[] __attribute__((aligned(4), section(".rodata.thinslot_in_text"))) = "room_battle";

// GOG's link (z_gogut.c) marks its file known only after Thinart_check returns, so it saw UNKNOWN and kept stock art.
// An UNKNOWN answer still carries the file's full digest: taLoad's checks of the header's SHA-256, CtrPackKey and
// CRC decide, as for a known file.
TA_LINK CtrDataVersionResult Thinart_dataCheck(CTR_DATAVERSION_CHAIN_PARAMS) {
    CtrDataVersionResult result = Thinart_check(path, game, part, cachePath, progress, cached, digest);
    ta.wait = false;
    if (result == CTR_DATA_UNKNOWN && digest != nullptr) {
        ta.known = true;
        memcpy(ta.digest, digest, 65);
    }
    return result;
}

// taLoad takes only an unused or free Smart copy, so after 16 copies in a session the bubble stayed stock. Before
// taRoomUpdate would load, a full cache gives up its slot as smartCopySlot (ctr_renderer.c) would: the copy drawn
// longest ago, never one drawn this frame or the one before (nothing queued samples it then; the renderer copies that
// item again when it next draws it). If every copy is that recent, the load waits (no I/O, no log) and a later call
// frees the slot and loads; leaving the room ends the wait.
static TA_LINK void taMakeRoom(Renderer* r) {
    Runner* runner = r->runner;
    if ((ta.owner == r && !ta.wait) || !ta.known || r->dataWin == nullptr) return;
    bool active = runner != nullptr && runner->currentRoom != nullptr && strcmp(runner->currentRoom->name, taSlotRoom) == 0;
    if (active) {
        int32_t group = RoomPlan_utstallGroup(runner);
        active = group == 57 || group == 80 || group == 81;
    }
    if (!active || *(uint8_t*) ((char*) r + Smartiv_layout[8]) != Smartiv_layout[9]) {
        ta.wait = false;
        return;
    }
    int32_t slot = TA_COUNT(r) < 16 ? TA_COUNT(r) : 0;
    while (slot < TA_COUNT(r) && TA_COPIES(r)[slot].tpag >= 0) slot++;
    if (slot >= 16) {
        uint32_t oldest = 1;
        slot = -1;
        for (int32_t i = 0; i < 16; i++) {
            uint32_t age = TA_FRAME(r) - TA_COPIES(r)[i].usedFrame;
            if (age > oldest) {
                slot = i;
                oldest = age;
            }
        }
        if (0 > slot) {
            ta.wait = true;
            ta.owner = r; // taRoomUpdate then skips the load; its exit branch still clears this
            ta.copy = nullptr;
            return;
        }
        C3D_TexDelete(&TA_COPIES(r)[slot].tex);
        TA_COPIES(r)[slot].tpag = -1; // stays free if taLoad then fails, as after smartCleanCopy's eviction
    }
    if (ta.wait) {
        ta.wait = false;
        ta.owner = nullptr;
    }
    ta.room = nullptr; // Thinart_endFrame loads this frame too, not only on a room change
}

TA_LINK void Thinart_slotPrefetch(Renderer* r) {
    taMakeRoom(r);
    Thinart_prefetchRoom(r);
}

TA_LINK uint32_t Thinart_slotCaps(Renderer* r) {
    taMakeRoom(r);
    return Thinart_caps(r);
}

TA_LINK void Thinart_slotEndFrame(Renderer* r) {
    taMakeRoom(r);
    Thinart_endFrame(r);
}
#endif
