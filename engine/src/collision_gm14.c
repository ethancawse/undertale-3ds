// UNDERTALE's collision rules (colflip; collision_gm14.h): GameMaker: Studio 1.4's point, rectangle and instance
// tests, and late builtins for collision_point, instance_position, position_meeting, collision_rectangle and
// collision_rectangle_list that use them. Colflip14_registerAll registers those over collision_compat.c's for
// bytecode 16 or older; for DELTARUNE it changes nothing after Colflip_registerAll.
//
// Everything here is late code (.text.late.colflip_gm14, .rodata.late.colflip_gm14, .bss.late.colflip_gm14; listed
// last in platform/3ds/late_reserve.ld), so collision_compat.c and every late function before it keep their bytes.

// rvalue.h's out-of-line RValue_toRealOther and RValue_toInt32Other get a copy in every file that calls them; this
// file's copies take other names, as collision_compat.c's do.
#define RValue_toRealOther Colflip14_toRealOther
#define RValue_toInt32Other Colflip14_toInt32Other
#if defined(ENABLE_WAD16) // freebytes: global, for alarm_gm14.c and colline_gm14.c (UNDERTALE)
#define RVALUE_OTHER_GLOBAL
#endif
#line 13
#define COLLISION_GM14_DEFINE
#include "collision_gm14.h"

#include "rvalue.h"
#include "runner.h"
#include "spatial_grid.h"
#include "stb_ds.h"
#include "vm.h"
#include "vm_builtins.h"

// The builtins registered before these (collision_compat.c's), for modern mode and for calls with too few arguments.
typedef struct {
    BuiltinFunc collisionPoint, instancePosition, positionMeeting, collisionRectangle, collisionRectangleList;
} Colflip14Previous;
static __attribute__((section(".bss.late.colflip_gm14"))) Colflip14Previous colflip14Previous;

// Names in late constants: a new string literal would land in .rodata.
static const char colflip14NameCollisionPoint[] __attribute__((section(".rodata.late.colflip_gm14"))) = "collision_point";
static const char colflip14NameInstancePosition[] __attribute__((section(".rodata.late.colflip_gm14"))) = "instance_position";
static const char colflip14NamePositionMeeting[] __attribute__((section(".rodata.late.colflip_gm14"))) = "position_meeting";
static const char colflip14NameCollisionRectangle[] __attribute__((section(".rodata.late.colflip_gm14"))) = "collision_rectangle";
static const char colflip14NameCollisionRectangleList[] __attribute__((section(".rodata.late.colflip_gm14"))) = "collision_rectangle_list";

static inline __attribute__((always_inline)) DsList* colflip14DsList(Runner* runner, int32_t id) {
    if (0 > id || id >= (int32_t) arrlen(runner->dsListPool)) return nullptr;
    if (runner->dsListPool[id].freed) return nullptr;
    return &runner->dsListPool[id];
}

// The builtins below are collision_compat.c's with GMS 1.4's tests.
static COLFLIP14_LATE COLFLIP14_FLAT RValue builtin_collision_point_gm14(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = ctx->runner;
    if (!runner->collisionCompatibilityMode || 5 > argCount) return colflip14Previous.collisionPoint(ctx, args, argCount);
    GMLReal px = RValue_toReal(args[0]);
    GMLReal py = RValue_toReal(args[1]);
    int32_t targetObjIndex = VM_resolveInstanceTarget(ctx, RValue_toInt32(args[2]));
    int32_t prec = RValue_toInt32(args[3]);
    int32_t notme = RValue_toInt32(args[4]);
    Instance* self = ctx->currentInstance;

    int32_t resultId = INSTANCE_NOONE;
    int32_t snapBase = Runner_pushInstancesForTarget(runner, targetObjIndex);
    int32_t snapEnd  = (int32_t) arrlen(runner->instanceSnapshots);
    for (int32_t snapIdx = snapBase; snapEnd > snapIdx; snapIdx++) {
        Instance* inst = runner->instanceSnapshots[snapIdx];
        if (!inst->active) continue;
        if (notme && inst == self) continue;
        if (!Colflip14_pointHits(runner, inst, px, py, prec != 0)) continue;
        resultId = inst->instanceId;
        break;
    }
    Runner_popInstanceSnapshot(runner, snapBase);
    return RValue_makeReal((GMLReal) resultId);
}

static COLFLIP14_LATE COLFLIP14_FLAT RValue builtin_instance_position_gm14(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = ctx->runner;
    if (!runner->collisionCompatibilityMode || 3 > argCount) return colflip14Previous.instancePosition(ctx, args, argCount);
    GMLReal px = RValue_toReal(args[0]);
    GMLReal py = RValue_toReal(args[1]);
    int32_t targetObjIndex = VM_resolveInstanceTarget(ctx, RValue_toInt32(args[2]));

    int32_t resultId = INSTANCE_NOONE;
    int32_t snapBase = Runner_pushInstancesForTarget(runner, targetObjIndex);
    int32_t snapEnd  = (int32_t) arrlen(runner->instanceSnapshots);
    for (int32_t i = snapBase; snapEnd > i; i++) {
        Instance* inst = runner->instanceSnapshots[i];
        if (!inst->active) continue;
        // GameMaker always tests the mask here.
        if (!Colflip14_pointHits(runner, inst, px, py, true)) continue;
        resultId = inst->instanceId;
        break;
    }
    Runner_popInstanceSnapshot(runner, snapBase);
    return RValue_makeReal((GMLReal) resultId);
}

static COLFLIP14_LATE COLFLIP14_FLAT RValue builtin_position_meeting_gm14(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = ctx->runner;
    if (!runner->collisionCompatibilityMode || 3 > argCount) return colflip14Previous.positionMeeting(ctx, args, argCount);
    GMLReal px = RValue_toReal(args[0]);
    GMLReal py = RValue_toReal(args[1]);
    int32_t target = VM_resolveInstanceTarget(ctx, RValue_toInt32(args[2]));
    if (target == INSTANCE_NOONE) return RValue_makeBool(false);

    SpatialGrid_syncGrid(runner, runner->spatialGrid);
    SpatialGridQuery query = SpatialGrid_prepareQuery(runner, px, py, px, py, target);
    bool found = false;
    for (int32_t gx = query.range.minGridX; query.range.maxGridX >= gx && !found; gx++) {
        for (int32_t gy = query.range.minGridY; query.range.maxGridY >= gy && !found; gy++) {
            Instance** cell = runner->spatialGrid->grid[SpatialGrid_cellIndex(runner->spatialGrid, gx, gy)];
            int32_t cellLen = (int32_t) arrlen(cell);
            repeat(cellLen, ci) {
                Instance* other = cell[ci];
                // Self is not skipped.
                if (!other->active) continue;
                if (other->lastCollisionQueryId == query.queryId) continue;
                other->lastCollisionQueryId = query.queryId;
                if (!query.matchAll && query.filterByObject && !VM_isObjectOrDescendant(runner->dataWin, other->objectIndex, target)) continue;
                if (!query.matchAll && query.filterByInstanceId && other->instanceId != (uint32_t) target) continue;
                // GameMaker always tests the mask here.
                if (!Colflip14_pointHits(runner, other, px, py, true)) continue;
                found = true;
                break;
            }
        }
    }
    return RValue_makeBool(found);
}

static COLFLIP14_LATE COLFLIP14_FLAT RValue builtin_collision_rectangle_gm14(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = ctx->runner;
    if (!runner->collisionCompatibilityMode || 7 > argCount) return colflip14Previous.collisionRectangle(ctx, args, argCount);
    GMLReal x1 = RValue_toReal(args[0]);
    GMLReal y1 = RValue_toReal(args[1]);
    GMLReal x2 = RValue_toReal(args[2]);
    GMLReal y2 = RValue_toReal(args[3]);
    int32_t targetObjIndex = VM_resolveInstanceTarget(ctx, RValue_toInt32(args[4]));
    int32_t prec = RValue_toInt32(args[5]);
    int32_t notme = RValue_toInt32(args[6]);
    if (x1 > x2) { GMLReal tmp = x1; x1 = x2; x2 = tmp; }
    if (y1 > y2) { GMLReal tmp = y1; y1 = y2; y2 = tmp; }
    Instance* self = ctx->currentInstance;

    int32_t resultId = INSTANCE_NOONE;
    int32_t snapBase = Runner_pushInstancesForTarget(runner, targetObjIndex);
    int32_t snapEnd  = (int32_t) arrlen(runner->instanceSnapshots);
    for (int32_t snapIdx = snapBase; snapEnd > snapIdx; snapIdx++) {
        Instance* inst = runner->instanceSnapshots[snapIdx];
        if (!inst->active) continue;
        if (notme && inst == self) continue;
        if (!Colflip14_rectHitsInstance(runner, inst, x1, y1, x2, y2, prec != 0)) continue;
        resultId = inst->instanceId;
        break;
    }
    Runner_popInstanceSnapshot(runner, snapBase);
    return RValue_makeReal((GMLReal) resultId);
}

static COLFLIP14_LATE COLFLIP14_FLAT RValue builtin_collision_rectangle_list_gm14(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = ctx->runner;
    if (!runner->collisionCompatibilityMode || 8 > argCount) return colflip14Previous.collisionRectangleList(ctx, args, argCount);
    GMLReal x1 = RValue_toReal(args[0]);
    GMLReal y1 = RValue_toReal(args[1]);
    GMLReal x2 = RValue_toReal(args[2]);
    GMLReal y2 = RValue_toReal(args[3]);
    int32_t target = VM_resolveInstanceTarget(ctx, RValue_toInt32(args[4]));
    int32_t prec = RValue_toInt32(args[5]);
    int32_t notme = RValue_toInt32(args[6]);
    int32_t listId = RValue_toInt32(args[7]);
    // arg 8 (ordered) is ignored, as in the original: instances are appended in iteration order.
    if (target == INSTANCE_NOONE) return RValue_makeReal(0.0);
    DsList* list = colflip14DsList(runner, listId);
    if (list == nullptr) return RValue_makeReal(0.0);
    if (x1 > x2) { GMLReal tmp = x1; x1 = x2; x2 = tmp; }
    if (y1 > y2) { GMLReal tmp = y1; y1 = y2; y2 = tmp; }
    Instance* self = ctx->currentInstance;
    int32_t count = 0;

    SpatialGrid_syncGrid(runner, runner->spatialGrid);
    SpatialGridQuery query = SpatialGrid_prepareQuery(runner, x1, y1, x2, y2, target);
    for (int32_t gx = query.range.minGridX; query.range.maxGridX >= gx; gx++) {
        for (int32_t gy = query.range.minGridY; query.range.maxGridY >= gy; gy++) {
            Instance** cell = runner->spatialGrid->grid[SpatialGrid_cellIndex(runner->spatialGrid, gx, gy)];
            int32_t cellLen = (int32_t) arrlen(cell);
            repeat(cellLen, ci) {
                Instance* inst = cell[ci];
                if (!inst->active) continue;
                if (notme && inst == self) continue;
                if (inst->lastCollisionQueryId == query.queryId) continue;
                inst->lastCollisionQueryId = query.queryId;
                if (!query.matchAll && query.filterByObject && !VM_isObjectOrDescendant(ctx->dataWin, inst->objectIndex, target)) continue;
                if (!query.matchAll && query.filterByInstanceId && inst->instanceId != (uint32_t) target) continue;
                if (!Colflip14_rectHitsInstance(runner, inst, x1, y1, x2, y2, prec != 0)) continue;
                arrput(list->items, RValue_makeReal((GMLReal) inst->instanceId));
                count++;
            }
        }
    }
    return RValue_makeReal((GMLReal) count);
}

// Puts late over the builtin registered as name and returns the one it replaced (nullptr, and nothing changed, if
// name is not registered).
static inline __attribute__((always_inline)) BuiltinFunc colflip14Replace(VMContext* ctx, const char* name, BuiltinFunc late) {
    ptrdiff_t idx = shgeti(ctx->builtinMap, (char*) name);
    if (0 > idx) return nullptr;
    BuiltinFunc previous = ctx->builtinMap[idx].value;
    if (previous != nullptr) ctx->builtinMap[idx].value = late;
    return previous;
}

// Called in place of Colflip_registerAll (VM_create): Colflip_registerAll, then, for GMS 1.4's data, these builtins
// over collision_compat.c's.
COLFLIP14_LATE void Colflip14_registerAll(VMContext* ctx) {
    WAD17_ONLY(Colflip_registerAll(ctx);) WAD17_NONE(VMBuiltins_registerAll(ctx);)
    // GameMaker: Studio 1.4's runner (UNDERTALE): bytecode 16 or older.
    Colflip_gm14 = ctx->dataWin != nullptr && 16 >= ctx->dataWin->gen8.wadVersion;
    if (!Colflip_gm14) return;
    colflip14Previous.collisionPoint = colflip14Replace(ctx, colflip14NameCollisionPoint, builtin_collision_point_gm14);
    colflip14Previous.instancePosition = colflip14Replace(ctx, colflip14NameInstancePosition, builtin_instance_position_gm14);
    colflip14Previous.positionMeeting = colflip14Replace(ctx, colflip14NamePositionMeeting, builtin_position_meeting_gm14);
    colflip14Previous.collisionRectangle = colflip14Replace(ctx, colflip14NameCollisionRectangle, builtin_collision_rectangle_gm14);
    colflip14Previous.collisionRectangleList = colflip14Replace(ctx, colflip14NameCollisionRectangleList, builtin_collision_rectangle_list_gm14);
}
#if defined(__3DS__) && !defined(ENABLE_WAD17)
// UNDERTALE's late-text reclaim (change argcheck): collision_compat.c's code is not linked (UNDERTALE registers the
// builtins itself above, and its scan is GMS 1.4's), so --gc-sections drops it. Its late constants and statics leave
// with it; these take their exact places (platform/3ds/late_reserve.ld, KEEP), so no late rodata or bss moves and the
// heap starts where it did. The constants keep their bytes: the five names, then the 18-byte switch table.
__attribute__((used, aligned(4), section(".rodata.late.colflipgap"))) static const unsigned char colflipGapRodata[0x77] = {
    'c', 'o', 'l', 'l', 'i', 's', 'i', 'o', 'n', '_', 'p', 'o', 'i', 'n', 't', 0,
    'i', 'n', 's', 't', 'a', 'n', 'c', 'e', '_', 'p', 'o', 's', 'i', 't', 'i', 'o', 'n', 0, 0, 0,
    'p', 'o', 's', 'i', 't', 'i', 'o', 'n', '_', 'm', 'e', 'e', 't', 'i', 'n', 'g', 0, 0, 0, 0,
    'c', 'o', 'l', 'l', 'i', 's', 'i', 'o', 'n', '_', 'r', 'e', 'c', 't', 'a', 'n', 'g', 'l', 'e', 0,
    'c', 'o', 'l', 'l', 'i', 's', 'i', 'o', 'n', '_', 'r', 'e', 'c', 't', 'a', 'n', 'g', 'l', 'e', '_', 'l', 'i', 's',
    't', 0,
    0x07, 0x00, 0x0c, 0x00, 0x11, 0x04, 0x04, 0x14, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x06, 0x03, 0x03, 0x12, 0x00,
};
__attribute__((used, aligned(8), section(".bss.late.colflipgap"))) static unsigned char colflipGapBss[0x1818];
#endif

// hitbox (change hitbox, 2026-11-01): a non-precise sprite's stored collision mask, read where data_win.c's parseSPRT
// used to skip it. UNDERTALE.exe's pair scan (0x48afc0) unpacks every sprite's stored masks (0x48ae20), precise or
// not, and its unit, scaled and rotated paths all test a non-precise partner's mask too, so a spinning knife hits with
// its blade box, not its whole 110x42 picture (tools/pcref/trace/hitbox_ut.txt). In UNDERTALE each of the 1328
// non-precise sprites stores one mask. A filled rectangle (1,296 of them, 3 empty) keeps no bytes: masks stays nullptr
// and maskOffset and maskSize hold the rectangle, which Colflip14_shape reads as all set. The last 29 keep their bits,
// cropped to the set pixels' box (38,126 B). Only the GMS 1.4 scan reads a non-precise sprite's mask (the point,
// rectangle and line tests read a precise one's only, as UNDERTALE.exe does), so other games (bytecode 17:
// DELTARUNE), other mask counts and a failed allocation skip it as before.
#include "binary_reader.h"
#define HITBOX_LATE __attribute__((noinline, optimize("Os"), section(".hitbox_late")))
#define HITBOX_ROW 256

HITBOX_LATE void DataWin_sep0Masks(BinaryReader* reader, DataWin* dw, Sprite* spr) {
    uint32_t stride = (spr->maskWidth + 7) / 8, height = spr->maskHeight;
    size_t start = BinaryReader_getPosition(reader);
    size_t total = (size_t) stride * height * spr->maskCount;
    spr->masks = nullptr;
#if defined(ENABLE_WAD16)
    if (16 >= dw->gen8.wadVersion && spr->maskCount == 1 && HITBOX_ROW >= stride && spr->maskOffsetX == 0 && spr->maskOffsetY == 0) {
        uint8_t row[HITBOX_ROW];
        uint32_t width = spr->maskWidth, n = 0, l = UINT32_MAX, r = 0, t = UINT32_MAX, b = 0;
        uint8_t lastBits = (uint8_t) (0xFF00u >> (((width - 1) & 7) + 1));
        for (uint32_t y = 0; height > y; y++) {
            BinaryReader_readBytes(reader, row, stride);
            row[stride - 1] &= lastBits;
            for (uint32_t k = 0; stride > k; k++) {
                uint32_t v = row[k];
                if (v == 0) continue;
                uint32_t x0 = k * 8 + (uint32_t) __builtin_clz(v << 24), x1 = k * 8 + 7 - (uint32_t) __builtin_ctz(v);
                if (l > x0) l = x0;
                if (x1 > r) r = x1;
                if (t > y) t = y;
                b = y;
                for (; v != 0; v &= v - 1) n++;   // no __builtin_popcount: libgcc's would land in the main text
            }
        }
        if (n == 0) {
            spr->maskWidth = spr->maskHeight = 0;
        } else {
            uint32_t w = r - l + 1, h = b - t + 1;
            if (n != w * h) {
                uint32_t outStride = (w + 7) / 8;
                uint8_t** masks = (uint8_t**) malloc(sizeof(uint8_t*));
                uint8_t* bits = (uint8_t*) calloc((size_t) outStride * h, 1);
                if (masks == nullptr || bits == nullptr) {
                    free(masks);
                    free(bits);
                    return;
                }
                for (uint32_t y = 0; h > y; y++) {
                    BinaryReader_seek(reader, start + (size_t) (t + y) * stride);
                    BinaryReader_readBytes(reader, row, stride);
                    for (uint32_t x = 0; w > x; x++) {
                        if (row[(l + x) >> 3] & (0x80 >> ((l + x) & 7))) bits[y * outStride + (x >> 3)] |= (uint8_t) (0x80 >> (x & 7));
                    }
                }
                masks[0] = bits;
                spr->masks = masks;
                BinaryReader_seek(reader, start + total);
            }
            spr->maskOffsetX = (int32_t) l;
            spr->maskOffsetY = (int32_t) t;
            spr->maskWidth = w;
            spr->maskHeight = h;
        }
        return;
    }
#else
    (void) dw;
#endif
    BinaryReader_skip(reader, total);
}

// hitbox: sprite_collision_mask in GMS 1.4. UNDERTALE.exe's (0x5185c0) sets the sprite's precise flag and has 0x494190
// rebuild its masks and margins, which returns at once for a sprite of the game's texture pages (+0x54 set, +0x36
// clear), so the margins and stored masks stay as data.win has them. UNDERTALE's one call, in the Create of Omega
// Flowey's controller, is on his nuke's sprite (rectangle kind): on PC its instances keep bbox_left = x + 22 and
// bbox_top = y + 20, where vm_builtins.c's version moved them to x + 13 and y + 44 (and the nuke left the room a frame
// early). The precise flag is not kept: the nuke's stored mask fills its margins and the nuke never turns or scales,
// so precise or not, every test gives the same pixels.
static const char hitboxNameSpriteCollisionMask[] __attribute__((section(".rodata.hitbox_in_text"))) = "sprite_collision_mask";

static HITBOX_LATE RValue builtin_sprite_collision_mask_gm14(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    (void) args;
    (void) argCount;
    return RValue_makeUndefined();
}

// VM_create calls this through COLFLIP_REGISTER_ALL (vm_builtins.h): the chain before it, then the replacement.
HITBOX_LATE void Hitbox_registerAll(VMContext* ctx) {
    Utgap_registerAll(ctx);
    if (Colflip_gm14) colflip14Replace(ctx, hitboxNameSpriteCollisionMask, builtin_sprite_collision_mask_gm14);
}
