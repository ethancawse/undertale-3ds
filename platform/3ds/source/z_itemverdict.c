// smartcut: Smart smoothing's item verdicts from the build.
//
// ctr_renderer.c's itemVerdict looks at each texture page item once, at its first draw under Smart smoothing: several
// milliseconds an item on the Old 3DS, eight items on UNDERTALE's first battle frame (about 11 ms on Azahar, the
// probe). The build works the same verdicts out from the game data (tools/aot/itemverdict.py, through
// fontverdict.py's build mode) into romfs:/fonts/<data.win folder>.dtiv: "DTIV", version 1, the CtrPackKey, the item
// count, then 10 bytes per item: with rounded colours the state byte itemVerdict would store (0 = not known, looked at
// on the console as before) and the ink box smartInk would leave, then the same with exact colours.
//
// Read once per renderer and data.win under Smart smoothing, into the renderer's smart.itemState and smart.itemInk,
// which itemVerdict then finds filled: right after the font verdicts at a room change (the last link of the
// CtrRenderer_loadFontVerdicts chain, ctr_wrap_chain.h, so roomload's code stays as it was; the first is after the first
// frame of the boot's or chapter's init room, or of the chapter select with its 308 B table), and when
// the options menu opens or closes with changes (the display caps query's wrap chain, ctr_wrap_chain.h): a switch to
// Smart reads the table then, while the menu holds the game, instead of looking at items in play until the next room
// change. Any failure leaves the renderer as if there were no table. A missing, stale or short file is tried once per
// data.win (the RomFS doesn't change); without memory for the read, the next chance tries again. Crisp looks at items
// only for tiled sprites' phase (gridItemParity), so it works those out on the console as before rather than spend the
// table's memory and read. Late code only (platform/3ds/late_reserve.ld): ctr_renderer.c gains only a data table at
// its end (Smartiv_layout: the fields' offsets and the state bits), so every existing translation unit keeps its exact
// code. Its log formats and strings are named late constants (no printf folding, no string literals in .rodata).
//
// Build with -DCTRPORT_ITEMVERDICT_CHECK to keep the table aside instead and compare it with every verdict
// itemVerdict works out (ctr_renderer.c calls itemVerdictCheck in those builds only).
#include <3ds.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "ctr_benchromfs.h"
#include "ctr_memory.h"
#include "ctr_texture_pack.h"
#include "ctr_wrap_chain.h"
#include "data_win.h"
#include "log.h"
#include "renderer.h"

#define IV_LATE __attribute__((noinline, section(".text.late.itemverdict")))
#define IV_RODATA __attribute__((aligned(4), section(".rodata.itemverdict_in_text")))
#define IV_MAGIC 0x56495444u // "DTIV"
#define IV_HEADER 28
#define IV_ENTRY 10
// Entries a read. Each read of a RomFS file is a request to the SD card that costs 10-20 ms on the console whatever
// its size, and stdio's buffer splits reads into 1 KB requests: 128 entries a read took 2.2 s for UNDERTALE's 65 KB on
// the Old 3DS (smartcut-iv3 rows). So the file is unbuffered and read 40 KB at a time.
#define IV_CHUNK 4096

// ctr_renderer.c: offsetof(CtrRenderer, smart.itemState), smart.itemInk, originalTpagCount, then ITEM_KNOWN,
// ITEM_FINE, ITEM_CLEAN, ITEM_BLACKEN and ITEM_PHASED, then offsetof(CtrRenderer, display.smoothing) and
// SMOOTHING_SMART (an enum, read by its first byte: little-endian, and the values are small).
enum { IVL_STATE, IVL_INK, IVL_COUNT, IVL_KNOWN, IVL_FINE, IVL_CLEAN, IVL_BLACKEN, IVL_PHASED, IVL_SMOOTHING, IVL_SMART,
       IVL_N };
extern const uint32_t Smartiv_layout[IVL_N];
#define IV_FIELD(r, i, type) (*(type*) ((char*) (r) + Smartiv_layout[i]))
typedef uint8_t (*IvInk)[4]; // smart.itemInk: per item, the ink box

static const char ivPath[] IV_RODATA = "romfs:/fonts/%s.dtiv";
static const char ivMount[] IV_RODATA = "romfs";
static const char ivMode[] IV_RODATA = "rb";
static const char ivLog[] IV_RODATA = "Smart smoothing: %u of %u item verdicts from %s (%s colours, read in %lu ms)\n";
static const char ivRounded[] IV_RODATA = "rounded";
static const char ivExact[] IV_RODATA = "exact";

bool CtrPack_romfsName(char* name, size_t size);
void CTR_FONTVERDICTS_ITEMVERDICT(Renderer* renderer);
void CTR_FONTVERDICTS_AFTER_ITEMVERDICT(Renderer* renderer);
void itemVerdictCheck(Renderer* renderer, int32_t i, uint8_t v);
uint32_t CTR_DISPLAYCAPS_AFTER_ITEMVERDICT(Renderer* renderer);
uint32_t CTR_DISPLAYCAPS_ITEMVERDICT(Renderer* renderer);

// What a try gave: the table is in the renderer's arrays; there is no usable table for this data.win (a missing,
// stale, short or unreadable file: not tried again); or not now (no memory for the read, or the renderer has no items
// yet: tried again at the next chance).
enum { IV_LOADED, IV_NO_TABLE, IV_NOT_NOW };

#ifndef CTRPORT_ITEMVERDICT_CHECK
// The last try's data.win (CtrPackKey) and what it left: no table for it, or the state array it filled and how many of
// its items were still 0 then. Drawing only turns 0s into verdicts, so that array keeps as many 0s or fewer while it
// holds the table. A later renderer of the same data.win (a chapter loaded again) can get its array at the same
// address, but that one is all 0s but the items it has drawn, and is filled again.
static struct {
    uint8_t* state;
    uint32_t gameId;
    uint64_t timestamp;
    uint32_t dataWinSize, zeros;
    bool failed;
} ivDone __attribute__((section(".bss.late.smartcut")));
#else
static struct {
    uint8_t* state;
    IvInk ink;
    uint64_t timestamp; // the table's data.win (CtrPackKey): a chapter's DataWin can reuse the last one's address
    uint32_t dataWinSize;
    uint32_t count, compared, mismatches, unknown;
} ivCheck __attribute__((section(".bss.late.smartcut")));
#endif

// Only the state bytes itemVerdict can store: known, and coarse, line art at a phase, or fine (clean, cleaned or
// neither).
static IV_LATE bool ivStateValid(uint8_t v) {
    uint32_t known = Smartiv_layout[IVL_KNOWN], fine = Smartiv_layout[IVL_FINE], phased = Smartiv_layout[IVL_PHASED];
    if ((v & known) == 0) return false;
    uint32_t rest = v & ~known;
    if (rest == 0) return true;
    if ((rest & ~(phased | 3u)) == 0) return (rest & phased) != 0;
    return rest == fine || rest == (fine | Smartiv_layout[IVL_CLEAN]) || rest == (fine | Smartiv_layout[IVL_BLACKEN]);
}

#ifndef CTRPORT_ITEMVERDICT_CHECK
// The items of state still at 0, a word at a time (0x80 in each byte of the word that is 0).
static IV_LATE uint32_t ivZeros(const uint8_t* state, uint32_t count) {
    uint32_t zeros = 0, i = 0;
    for (; count - i >= 4; i += 4) {
        uint32_t w;
        memcpy(&w, state + i, 4);
        uint32_t z = ~(((w & 0x7f7f7f7fu) + 0x7f7f7f7fu) | w | 0x7f7f7f7fu);
        zeros += ((z >> 7) * 0x01010101u) >> 24;
    }
    for (; count > i; i++) zeros += state[i] == 0 ? 1u : 0u;
    return zeros;
}
#endif

static IV_LATE uint32_t ivLoad(Renderer* renderer, DataWin* dw) {
    uint32_t count = IV_FIELD(renderer, IVL_COUNT, uint32_t);
    if (count == 0) return IV_NOT_NOW;
    // Named as every RomFS file of this data.win is: its folder without the SD folder's "-3ds".
    char file[96], folder[64];
    if (!CtrPack_romfsName(folder, sizeof(folder))) return IV_NO_TABLE;
    snprintf(file, sizeof(file), ivPath, folder);
#ifndef CTRPORT_ITEMVERDICT_CHECK
    // Straight into the renderer's arrays where its draws made them already (itemVerdict makes the ink box's when it
    // first needs one); its verdicts stay, and the table fills the items it hasn't looked at.
    uint8_t* state = IV_FIELD(renderer, IVL_STATE, uint8_t*);
    IvInk ink = IV_FIELD(renderer, IVL_INK, IvInk);
#else
    uint8_t* state = nullptr;
    IvInk ink = nullptr;
#endif
    bool ownState = state == nullptr, ownInk = ink == nullptr;
    // The memory before the file, without the reliever or linear memory (no packed texture is let go for this): without
    // it nothing is read, and the next chance tries again. In a state array the renderer has, filled marks the items the
    // table fills, which go back to 0 if the read fails.
    bool relief = CtrMemory_allowRelief(false);
    if (ownState) state = calloc(count, 1);
    if (ownInk) ink = calloc(count, sizeof(ink[0]));
    uint8_t* filled = ownState ? nullptr : calloc((count + 7) / 8, 1);
    uint8_t* chunk = malloc(IV_CHUNK * IV_ENTRY);
    CtrMemory_allowRelief(relief);
    uint32_t result = IV_NOT_NOW, known = 0;
    bool exact = !CtrPack_lossy();
    uint64_t t0 = svcGetSystemTick();
    if (state != nullptr && ink != nullptr && chunk != nullptr && (ownState || filled != nullptr)) {
        // The font verdicts' load has mounted the RomFS already; this only counts as another use of it.
        FILE* f = R_SUCCEEDED(CtrRomfs_init(ivMount)) ? fopen(file, ivMode) : nullptr;
        bool ok = f != nullptr;
        if (ok) {
            setvbuf(f, nullptr, _IONBF, 0);
            uint8_t head[IV_HEADER];
            ok = fread(head, 1, sizeof(head), f) == sizeof(head);
            uint32_t magic = 0, version = 0, size = 0, gameId = 0, items = 0;
            uint64_t timestamp = 0;
            if (ok) {
                memcpy(&magic, head, 4);
                memcpy(&version, head + 4, 4);
                memcpy(&size, head + 8, 4);
                memcpy(&gameId, head + 12, 4);
                memcpy(&timestamp, head + 16, 8);
                memcpy(&items, head + 24, 4);
                CtrPackKey key = CtrPack_keyFor(dw);
                ok = magic == IV_MAGIC && version == 1 && size == key.dataWinSize && gameId == key.gameId && timestamp == key.timestamp &&
                     items == count;
            }
        }
        for (uint32_t i = 0; ok && count > i; i += IV_CHUNK) {
            uint32_t n = count - i > IV_CHUNK ? IV_CHUNK : count - i;
            if (fread(chunk, IV_ENTRY, n, f) != n) {
                ok = false;
                break;
            }
            for (uint32_t k = 0; n > k; k++) {
                const uint8_t* e = chunk + k * IV_ENTRY + (exact ? 5 : 0);
                if (!ivStateValid(e[0])) continue;
                known++;
                if (ownInk) memcpy(ink[i + k], e + 1, 4); // a new ink array takes every box (as the table's own did)
                if (state[i + k] != 0) continue;          // looked at on the console already
                state[i + k] = e[0];
                if (filled != nullptr) filled[(i + k) >> 3] |= (uint8_t) (1u << ((i + k) & 7u));
                if (!ownInk) memcpy(ink[i + k], e + 1, 4);
            }
        }
        if (f != nullptr) fclose(f);
        result = ok ? IV_LOADED : IV_NO_TABLE;
    }
    free(chunk);
    if (result != IV_LOADED) {
        // As if there were no table: the items it filled in the renderer's state array go back to 0, and the arrays it
        // made go. (An item at 0 has its ink box written by itemVerdict before it is ever drawn as fine.)
        for (uint32_t i = 0; filled != nullptr && count > i; i++) {
            if ((filled[i >> 3] >> (i & 7u)) & 1u) state[i] = 0;
        }
        free(filled);
        if (ownState) free(state);
        if (ownInk) free(ink);
        return result;
    }
    free(filled);
#ifndef CTRPORT_ITEMVERDICT_CHECK
    IV_FIELD(renderer, IVL_STATE, uint8_t*) = state;
    IV_FIELD(renderer, IVL_INK, IvInk) = ink;
    ivDone.state = state;
    ivDone.zeros = ivZeros(state, count);
#else
    free(ivCheck.state);
    free(ivCheck.ink);
    ivCheck.state = state;
    ivCheck.ink = ink;
    ivCheck.count = count;
    CtrPackKey key = CtrPack_keyFor(dw);
    ivCheck.timestamp = key.timestamp;
    ivCheck.dataWinSize = key.dataWinSize;
#endif
    logInfo(ivLog, (unsigned) known, (unsigned) count, file, exact ? ivExact : ivRounded,
            (unsigned long) ((svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000)));
    return IV_LOADED;
}

// Under Smart smoothing, unless this renderer holds this data.win's table already or the data.win has none (see the
// top). Cheap when there is nothing to do: the CtrPackKey and a count of the state array's 0s.
static IV_LATE void ivEnsure(Renderer* renderer) {
    if (renderer == nullptr || renderer->dataWin == nullptr ||
        *((const uint8_t*) renderer + Smartiv_layout[IVL_SMOOTHING]) != Smartiv_layout[IVL_SMART])
        return;
    DataWin* dw = renderer->dataWin;
    CtrPackKey key = CtrPack_keyFor(dw);
#ifndef CTRPORT_ITEMVERDICT_CHECK
    if (key.timestamp == ivDone.timestamp && key.dataWinSize == ivDone.dataWinSize && key.gameId == ivDone.gameId) {
        if (ivDone.failed) return;
        uint8_t* state = IV_FIELD(renderer, IVL_STATE, uint8_t*);
        if (state != nullptr && state == ivDone.state && ivDone.zeros >= ivZeros(state, IV_FIELD(renderer, IVL_COUNT, uint32_t))) return;
    }
    uint32_t result = ivLoad(renderer, dw);
    if (result == IV_NOT_NOW) return;
    ivDone.timestamp = key.timestamp;
    ivDone.dataWinSize = key.dataWinSize;
    ivDone.gameId = key.gameId;
    ivDone.failed = result == IV_NO_TABLE;
    if (ivDone.failed) ivDone.state = nullptr;
#else
    if (ivCheck.state != nullptr && key.timestamp == ivCheck.timestamp && key.dataWinSize == ivCheck.dataWinSize) return;
    ivLoad(renderer, dw);
#endif
}

// The CtrRenderer_loadFontVerdicts chain's last link before the real one (ctr_wrap_chain.h): smalltext (UNDERTALE)
// or crispart (DELTARUNE) calls it first thing, at each room change.
IV_LATE void CTR_FONTVERDICTS_ITEMVERDICT(Renderer* renderer) {
    CTR_FONTVERDICTS_AFTER_ITEMVERDICT(renderer);
    ivEnsure(renderer);
}

// The display caps query's last link before the real one (ctr_wrap_chain.h). main.c asks right after each
// CtrRenderer_create (the runner hasn't given the renderer its data yet: nothing to do), when the options menu opens,
// and when it closes with changes, before the game goes on. CtrDisplayCaps is passed through as a word, as the links
// before this one do.
IV_LATE uint32_t CTR_DISPLAYCAPS_ITEMVERDICT(Renderer* renderer) {
    uint32_t caps = CTR_DISPLAYCAPS_AFTER_ITEMVERDICT(renderer);
    ivEnsure(renderer);
    return caps;
}

#ifdef CTRPORT_ITEMVERDICT_CHECK
// Test builds: itemVerdict has just worked out item i's state v (and its ink box where it looks at the ink); compare
// them with the table's.
IV_LATE void itemVerdictCheck(Renderer* renderer, int32_t i, uint8_t v) {
    if (ivCheck.state == nullptr || 0 > i || (uint32_t) i >= ivCheck.count || renderer->dataWin == nullptr) return;
    // Not against the last chapter's table (a new chapter's first frame draws before its first room change).
    CtrPackKey key = CtrPack_keyFor(renderer->dataWin);
    if (key.timestamp != ivCheck.timestamp || key.dataWinSize != ivCheck.dataWinSize) return;
    uint8_t t = ivCheck.state[i];
    if (t == 0) {
        ivCheck.unknown++;
        return;
    }
    ivCheck.compared++;
    bool same = t == v;
    // The ink box matters where the draw reads it: fine items.
    IvInk ink = IV_FIELD(renderer, IVL_INK, IvInk);
    if (same && (v & Smartiv_layout[IVL_FINE]) != 0) same = ink != nullptr && memcmp(ink[i], ivCheck.ink[i], 4) == 0;
    if (!same && 32 > ivCheck.mismatches++) {
        static const char badFmt[] IV_RODATA = "[itemverdict] check MISMATCH item %ld: runtime 0x%02x table 0x%02x\n";
        logInfo(badFmt, (long) i, (unsigned) v, (unsigned) t);
    }
    if ((ivCheck.compared & 0x3fu) == 0 || ivCheck.compared == 1 || !same) {
        static const char sumFmt[] IV_RODATA = "[itemverdict] check: %lu compared, %lu mismatches, %lu not in the table\n";
        logInfo(sumFmt, (unsigned long) ivCheck.compared, (unsigned long) ivCheck.mismatches, (unsigned long) ivCheck.unknown);
    }
}
#endif
