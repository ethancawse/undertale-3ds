// Symmetric halving's first look from the build (UNDERTALE; DELTARUNE on New 3DS).
//
// Without smoothing in full, ctr_renderer.c draws a sprite at 1:1 that is mirror-symmetric about its middle from a
// copy of half its width ("Symmetric halving"). Whether an item is such a sprite is looked at once per renderer, at
// its first draw that way (symCopy): every texel of the item is read through symTexel and the two halves compared,
// and nearly every item turns out not to be one, so it is drawn as it is (SYM_STATE_PLAIN). The read costs the game
// thread its size in texels at that first draw: on Azahar's Old 3DS, UNDERTALE's battle target (546x115 texels)
// took 18.3 ms at the first FIGHT of a battle, and each of the four battle buttons 1.4 ms at the first battle frame
// (measured in a first-use census). The build works the same look out from the game data
// (tools/aot/symverdict.py, through fontverdict.py's build mode) into romfs:/fonts/<data.win folder>.dtsy: "DTSY",
// version 1, the CtrPackKey, the item count, then a bit per item that is 1 where symCopy would leave the item plain in
// both colour modes. Items it would halve, or that the build couldn't look at, are 0 and left to symCopy, as before.
//
// Read once per data.win, at a room change (a link of the CtrRenderer_loadFontVerdicts chain, ctr_wrap_chain.h, so
// z_roomload.c stays as it was), and kept (one bit an item: 819 bytes for UNDERTALE). At that room change and each
// later one, every bit-1 item the renderer hasn't looked at yet (symState 0) is marked plain in its symState, which is
// what symCopy would store for it at its first draw; the renderer's draws then find it decided. A renderer made again
// (its array starts at 0) is marked at its first room change. Any failure leaves the renderer as if there were no
// table: symCopy looks at every item itself, as before. A missing, stale or short file is tried once per data.win.
// Late code only (platform/3ds/late_reserve.ld): ctr_renderer.c gains only a data table at its end
// (Symverdict_layout: the fields' offsets and the plain state), so every existing translation unit keeps its code.
// Pixels and game state are the same as before: a plain item is drawn as it was, from the same first frame.
// DELTARUNE (New 3DS builds only; Old builds compile this file empty, for their late text): one table per data.win,
// chapterN_windows.dtsy, read again when the chapter's data.win changes (its CtrPackKey). Its link comes after
// crispart's (z_crispart.c's call to the next link is renamed in CMakeLists.txt). On Azahar's New 3DS, Spamton NEO's
// 612x590 items each took 77-105 ms at their first draw, and Chapter 5's 512x512 intro item 79 ms. DELTARUNE New
// builds compile this file through z_symverdict_dr.c, with sections named symverdict_dr (CTR_SYMVERDICT_NAME,
// ctr_wrap_chain.h), so that its code and statics link last in the late blocks, after code merged before it.
#include <3ds.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctr_benchromfs.h"
#include "ctr_memory.h"
#include "ctr_texture_pack.h"
#include "ctr_wrap_chain.h"
#include "data_win.h"
#include "log.h"
#include "renderer.h"

#if (defined(ENABLE_WAD16) && !defined(SY_DR_UNIT)) || (defined(ENABLE_WAD17) && defined(CTRPORT_TARGET_N3DS) && defined(SY_DR_UNIT))
#define SY_LATE __attribute__((noinline, section(".text.late." CTR_SYMVERDICT_NAME)))
#define SY_CONST __attribute__((aligned(4), section(".rodata." CTR_SYMVERDICT_NAME "_in_text")))
#define SY_BSS __attribute__((section(".bss.late." CTR_SYMVERDICT_NAME)))
#define SY_MAGIC 0x59535444u // "DTSY"
#define SY_HEADER 28
// More items than any data.win has: a count past it is a bad file.
#define SY_ITEMS_MAX 65536u

// ctr_renderer.c: offsetof(CtrRenderer, symState), offsetof(CtrRenderer, originalTpagCount), SYM_STATE_PLAIN.
enum { SYL_STATE, SYL_COUNT, SYL_PLAIN, SYL_N };
extern const uint32_t Symverdict_layout[SYL_N];
#define SY_FIELD(r, i, type) (*(type*) ((char*) (r) + Symverdict_layout[i]))

static const char syPath[] SY_CONST = "romfs:/fonts/%s.dtsy";
static const char syMount[] SY_CONST = "romfs";
static const char syMode[] SY_CONST = "rb";
static const char syLog[] SY_CONST = "Symmetric halving: %lu of %lu items plain from %s (read in %lu ms)\n";
static const char syLogNone[] SY_CONST = "Symmetric halving: no usable %s; items are looked at when first drawn\n";

bool CtrPack_romfsName(char* name, size_t size);
void CTR_FONTVERDICTS_SYMVERDICT(Renderer* renderer);
void CTR_FONTVERDICTS_AFTER_SYMVERDICT(Renderer* renderer); // itemverdict's link (z_itemverdict.c)

// The table of the last data.win tried (CtrPackKey): its bits, or none (failed: not tried again for that data.win).
static struct {
    uint8_t* bits;
    uint32_t count;
    uint32_t gameId;
    uint64_t timestamp;
    uint32_t dataWinSize;
    bool tried, failed;
} sy SY_BSS;

// Reads the table for dw into sy.bits; false if there is no usable one (a missing, stale, short or unreadable file),
// or no memory for it.
static SY_LATE bool syRead(DataWin* dw, uint32_t count) {
    char file[96], folder[64];
    file[0] = '\0';
    bool ok = CtrPack_romfsName(folder, sizeof(folder));
    if (ok) snprintf(file, sizeof(file), syPath, folder);
    uint64_t t0 = svcGetSystemTick();
    // The font verdicts' load has mounted the RomFS already; this only counts as another use of it.
    FILE* f = ok && R_SUCCEEDED(CtrRomfs_init(syMount)) ? fopen(file, syMode) : nullptr;
    uint8_t head[SY_HEADER];
    uint32_t bytes = (count + 7u) / 8u;
    uint8_t* bits = nullptr;
    ok = f != nullptr;
    if (ok) {
        // Unbuffered: the header and the bits are two requests to the SD card, not one a KB.
        setvbuf(f, nullptr, _IONBF, 0);
        ok = fread(head, 1, sizeof(head), f) == sizeof(head);
    }
    if (ok) {
        uint32_t magic, version, size, gameId, items;
        uint64_t timestamp;
        memcpy(&magic, head, 4);
        memcpy(&version, head + 4, 4);
        memcpy(&size, head + 8, 4);
        memcpy(&gameId, head + 12, 4);
        memcpy(&timestamp, head + 16, 8);
        memcpy(&items, head + 24, 4);
        CtrPackKey key = CtrPack_keyFor(dw);
        ok = magic == SY_MAGIC && version == 1 && size == key.dataWinSize && gameId == key.gameId && timestamp == key.timestamp &&
             items == count;
    }
    if (ok) {
        // Without the reliever: no packed texture is let go for this.
        bool relief = CtrMemory_allowRelief(false);
        bits = malloc(bytes);
        CtrMemory_allowRelief(relief);
        ok = bits != nullptr && fread(bits, 1, bytes, f) == bytes;
    }
    if (f != nullptr) fclose(f);
    if (!ok) {
        free(bits);
        logInfo(syLogNone, file);
        return false;
    }
    uint32_t plain = 0;
    // Counted by hand: __builtin_popcount links libgcc's __popcountsi2 into the frozen text, which moves it.
    for (uint32_t i = 0; bytes > i; i++) {
        for (uint32_t b = bits[i]; b != 0; b &= b - 1u) plain++;
    }
    sy.bits = bits;
    sy.count = count;
    logInfo(syLog, (unsigned long) plain, (unsigned long) count, file,
            (unsigned long) ((svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000)));
    return true;
}

// The CtrRenderer_loadFontVerdicts chain's link after smalltext (ctr_wrap_chain.h): at each room change, once the
// font verdicts are in, the renderer's symState gets the table's plain items it hasn't looked at yet.
SY_LATE void CTR_FONTVERDICTS_SYMVERDICT(Renderer* renderer) {
    CTR_FONTVERDICTS_AFTER_SYMVERDICT(renderer);
    if (renderer == nullptr || renderer->dataWin == nullptr) return;
    uint32_t count = SY_FIELD(renderer, SYL_COUNT, uint32_t);
    if (count == 0 || count > SY_ITEMS_MAX) return;
    DataWin* dw = renderer->dataWin;
    CtrPackKey key = CtrPack_keyFor(dw);
    if (!sy.tried || key.timestamp != sy.timestamp || key.dataWinSize != sy.dataWinSize || key.gameId != sy.gameId) {
        free(sy.bits);
        sy.bits = nullptr;
        sy.tried = true;
        sy.timestamp = key.timestamp;
        sy.dataWinSize = key.dataWinSize;
        sy.gameId = key.gameId;
        sy.failed = !syRead(dw, count);
    }
    if (sy.failed || sy.count != count) return;
    uint16_t* state = SY_FIELD(renderer, SYL_STATE, uint16_t*);
    if (state == nullptr) {
        // The array symCopy makes at its first look (calloc'd, freed with the renderer), made here instead.
        bool relief = CtrMemory_allowRelief(false);
        state = calloc(count, sizeof(uint16_t));
        CtrMemory_allowRelief(relief);
        if (state == nullptr) return; // symCopy makes it later, and looks at every item itself
        SY_FIELD(renderer, SYL_STATE, uint16_t*) = state;
    }
    uint16_t plain = (uint16_t) Symverdict_layout[SYL_PLAIN];
    const uint8_t* bits = sy.bits;
    for (uint32_t i = 0; count > i; i += 8) {
        uint32_t b = bits[i >> 3];
        for (uint32_t k = i; b != 0 && count > k; k++, b >>= 1) {
            if ((b & 1u) != 0 && state[k] == 0) state[k] = plain;
        }
    }
}
#endif
