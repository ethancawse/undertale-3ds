// ===[ Per-room texture repack (repack; DELTARUNE on New 3DS) ]===
// GameMaker packs sprites onto pages by size, not by use, so a room pulls whole 256 px cache tiles full of art it
// never draws: Chapter 3's room 109 (Knight, Tepisode) wants 417 tiles, 71.5 MiB, past the New 3DS's tile budget,
// and reads tiles from SD all through the fight. tools/repack/repack.py copies the 8x8 texel blocks a room's recorded
// draws read (each rectangle and a one-texel border) onto fewer new tiles, at the same place in each tile, and
// writes them as extra cache pages (page<N>.dtx, N past data.win's pages) with a remap, repack.bin.
//
// Repack_texturesInit reads repack.bin after CtrTextures_init and, if it is for this data.win and its rounded
// cache, gives the extra pages Texture entries past txtr.count (txtr.count itself is unchanged, so the game and
// the pack worker never see them) and CtrPage entries, so the cache loads, keeps and evicts their tiles like any.
// repackEmitPageTiles stands in for emitPageTiles: in a remapped room, a piece of a draw whose texels (and their
// one-texel neighbours, which bilinear filtering reads) all lie in blocks a new tile holds is drawn from that tile
// with the same texture coordinates; the texels, clamp edges and filtering are the same. Every other piece, room,
// palette shader and page is drawn as before; a new tile that can't load falls back to the original one.
// A tile something rewrites as it loads (pixpress's PRESS) is never remapped: see repackKeep.
#if defined(ENABLE_WAD17) && !defined(CTRPORT_TARGET_O3DS)
#define REPACK_TEXT __attribute__((section(".text.repack_late")))
#define REPACK_BSS __attribute__((section(".bss.repack_late")))
#define REPACK_RO __attribute__((aligned(4), section(".rodata.repack_in_text")))
#define REPACK_MAGIC 0x314B5052u // "RPK1"
#define REPACK_VERSION 3u       // 2: with the donor pages' pack headers; 3: kept tiles (vpage UINT16_MAX) to load
#define REPACK_PACK_VERSION 3u   // the rounded cache's (ctr_texture_pack.c PACK_VERSION)
#define REPACK_HEADER_SIZE 52u
#define REPACK_PAGE_SIZE 12u
#define REPACK_ROOM_SIZE 12u
#define REPACK_ENTRY_SIZE 136u
#define REPACK_DONOR_SIZE 12u
#define REPACK_PACK_HEAD 52u     // a page file's PackHeader up to its tableChecksum (ctr_texture_pack.c)
#define REPACK_MAX_PAGES 128u    // all pages, the extra ones too: texcache (ctr_textures.c) follows at most 128
#define REPACK_MAX_FILE (1024u * 1024u)
#define REPACK_TILE 256

static const char repackFile[] REPACK_RO = "repack.bin";
static const char repackRb[] REPACK_RO = "rb";
static const char repackLogOn[] REPACK_RO = "Repack: %u extra pages, %u rooms, %u remapped tiles\n";
static const char repackLogOff[] REPACK_RO = "Repack: repack.bin not used (out of memory or damaged)\n";
static const char repackLogDonor[] REPACK_RO = "Repack: not used (donor mismatch)\n";
static const char repackLogRoom[] REPACK_RO = "Repack: room load: %u more new tiles and %u kept tiles in %.1f ms (hidden)\n";
static const char repackPageName[] REPACK_RO = "page%03u.dtx";

typedef struct {
    uint16_t donorPage, donorTile; // the original tile
    uint16_t vpage, vtile;         // the new one: extra page vpage (from 0), tile vtile
    uint32_t mask[8 * 4];          // the 8x8 blocks it holds: row by, bit bx
} RepackEntry;

typedef struct {
    uint32_t room, first, count;
} RepackRoom;

REPACK_BSS static struct {
    const CtrTextureCache* cache; // the cache the extra pages were added to
    const CtrPage* pages;
    uint32_t firstPage;           // data.win's page count: the first extra page
    uint32_t roomCount;
    RepackRoom* rooms;
    RepackEntry* entries;         // per room, sorted by donor page and tile
    uint32_t entryCount;
    int32_t room;                 // the room the lists below are for, -1 for none yet
    const RepackEntry* active;    // that room's entries
    uint32_t activeCount;
    uint32_t wantFrame;           // the frame repackWant last ran in
    uint16_t pageStart[REPACK_MAX_PAGES + 1]; // per donor page, its entries in active[]
    uint64_t tileMask[REPACK_MAX_PAGES];      // per donor page, bit t for each tile t (below 64) with an entry; all set past 64
    uint8_t blob[16];             // what the extra pages' Texture entries point at: no image, never read
} repack;

// What repackEmitPageTiles decided for a draw's source rectangle on a page, so that a sprite drawn again (bullets,
// the same frame each frame) skips the lookups: draws of up to REPACK_MEMO_TILES tiles. gen is repackMemoGen when it
// was made; the generation changes with the room and whenever the entries are read or forgotten, which their
// pointers follow.
#define REPACK_MEMO 2048u // in the heap, not linear memory (which bss comes out of), and only while repack.bin is used
#define REPACK_MEMO_PLAIN 1u // drawn as emitPageTiles draws it
#define REPACK_MEMO_MOVED 2u // one tile, drawn from entry to[0]'s new tile
#define REPACK_MEMO_MULTI 3u // the pieces in moved drawn from the new tiles of to[]
#define REPACK_MEMO_TILES 4
typedef struct {
    const CtrPage* page;
    float s[4];
    const RepackEntry* to[REPACK_MEMO_TILES];
    uint32_t gen, kind, moved;
    uint16_t tx0, ty0; // the range's first tile
    uint8_t count, nx; // its tiles and columns
    // regionTilePart's piece and texture coordinates for each moved tile of the range
    float part[REPACK_MEMO_TILES][4], uv[REPACK_MEMO_TILES][4];
} RepackMemo;

REPACK_BSS static RepackMemo* repackMemo;

// The same for draws of more than REPACK_MEMO_TILES tiles (backgrounds, a few each frame): which pieces are drawn from
// new tiles and their entries, so the per-tile lookups and coverage tests run once, not every frame.
#define REPACK_BIG 32u
typedef struct {
    const CtrPage* page;
    float s[4];
    uint32_t gen;
    uint64_t moved;
    const RepackEntry* to[64];
} RepackBig;
REPACK_BSS static RepackBig* repackBig;
REPACK_BSS static uint32_t repackMemoGen;

REPACK_TEXT static uint32_t repackLoad32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

// tools/repack/repack.py fnv_words, ctr_texture_pack.c checksum.
REPACK_TEXT static uint32_t repackChecksum(const uint8_t* p, size_t size) {
    uint32_t h = 2166136261u;
    size_t i = 0;
    for (; size >= i + 4; i += 4) h = (h ^ repackLoad32(p + i)) * 16777619u;
    for (; size > i; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

REPACK_TEXT static void repackForget(void) {
    repackMemoGen++;
    free(repack.rooms);
    free(repack.entries);
    free(repackMemo);
    repackMemo = nullptr;
    free(repackBig);
    repackBig = nullptr;
    memset(&repack, 0, sizeof(repack));
    repack.room = -1;
}

// True if every donor page file in the cache folder has the pack header repack.bin was built against: the same
// payload size and tile table checksum (which covers every tile's checksum), so the new tiles hold that page's
// texels. Any page missing, unfinished or packed differently, or an entry whose page isn't listed, turns repack off.
REPACK_TEXT static bool repackDonorsMatch(const CtrTextureCache* cache, const uint8_t* donorAt, uint32_t donors, const uint8_t* entryAt,
                                          uint32_t entries) {
    uint32_t listed[REPACK_MAX_PAGES / 32] = {0};
    for (uint32_t i = 0; donors > i; i++) {
        uint32_t page = repackLoad32(donorAt + i * REPACK_DONOR_SIZE);
        if (page >= cache->pageCount) return false;
        listed[page / 32] |= 1u << (page % 32);
    }
    for (uint32_t i = 0; entries > i; i++) {
        uint16_t page;
        memcpy(&page, entryAt + i * REPACK_ENTRY_SIZE, sizeof(page));
        if (page >= cache->pageCount || (listed[page / 32] & (1u << (page % 32))) == 0) return false;
    }
    for (uint32_t i = 0; donors > i; i++) {
        const uint8_t* d = donorAt + i * REPACK_DONOR_SIZE;
        char name[16], path[CTR_PACK_PATH_MAX];
        snprintf(name, sizeof(name), repackPageName, (unsigned) repackLoad32(d));
        CtrPack_cachePath(path, sizeof(path), name);
        FILE* f = fopen(path, repackRb);
        if (f == nullptr) return false;
        uint8_t head[REPACK_PACK_HEAD];
        bool read = fread(head, 1, sizeof(head), f) == sizeof(head);
        fclose(f);
        uint64_t timestamp;
        memcpy(&timestamp, head + 16, sizeof(timestamp));
        if (!read || repackLoad32(head) != 0x31585444u /* "DTX1" */ || repackLoad32(head + 4) != REPACK_PACK_VERSION ||
            repackLoad32(head + 8) != cache->key.dataWinSize || repackLoad32(head + 12) != cache->key.gameId || timestamp != cache->key.timestamp ||
            repackLoad32(head + 44) != repackLoad32(d + 4) || repackLoad32(head + 48) != repackLoad32(d + 8)) {
            return false;
        }
    }
    return true;
}

// Whether the cache file of extra page pageId is there and packed for this data.win and cache (its header, as a
// donor's above). The cache then counts it as a valid file, as it does a page the pack worker has packed, so the room
// change's hidden load reads its tiles (z_roomload.c reads only pages whose file is known good); a page that still
// fails to load later falls back to the original tiles.
REPACK_TEXT static bool repackPageFile(const CtrTextureCache* cache, uint32_t pageId) {
    char name[16], path[CTR_PACK_PATH_MAX];
    snprintf(name, sizeof(name), repackPageName, (unsigned) pageId);
    CtrPack_cachePath(path, sizeof(path), name);
    FILE* f = fopen(path, repackRb);
    if (f == nullptr) return false;
    uint8_t head[REPACK_PACK_HEAD];
    bool read = fread(head, 1, sizeof(head), f) == sizeof(head);
    fclose(f);
    uint64_t timestamp;
    memcpy(&timestamp, head + 16, sizeof(timestamp));
    return read && repackLoad32(head) == 0x31585444u /* "DTX1" */ && repackLoad32(head + 4) == REPACK_PACK_VERSION &&
           repackLoad32(head + 8) == cache->key.dataWinSize && repackLoad32(head + 12) == cache->key.gameId && timestamp == cache->key.timestamp;
}

// Reads repack.bin; false (and nothing kept) unless it is whole and for this data.win and cache.
REPACK_TEXT static bool repackRead(CtrTextureCache* cache, DataWin* dw, uint8_t** out, long* outSize) {
    char path[CTR_PACK_PATH_MAX];
    CtrPack_cachePath(path, sizeof(path), repackFile);
    FILE* f = fopen(path, repackRb);
    if (f == nullptr) return false;
    long size = CtrPack_fileSize(f);
    uint8_t* data = nullptr;
    if (size > (long) REPACK_HEADER_SIZE && REPACK_MAX_FILE >= (unsigned long) size) data = malloc((size_t) size);
    if (data != nullptr && fread(data, 1, (size_t) size, f) != (size_t) size) {
        free(data);
        data = nullptr;
    }
    fclose(f);
    if (data == nullptr) return false;
    uint64_t timestamp;
    memcpy(&timestamp, data + 20, sizeof(timestamp));
    uint32_t vpages = repackLoad32(data + 32), rooms = repackLoad32(data + 36), entries = repackLoad32(data + 40), donors = repackLoad32(data + 44);
    bool ok = repackLoad32(data) == REPACK_MAGIC && repackLoad32(data + 4) == REPACK_VERSION && repackLoad32(data + 8) == REPACK_PACK_VERSION &&
              repackLoad32(data + 12) == cache->key.dataWinSize && repackLoad32(data + 16) == cache->key.gameId && timestamp == cache->key.timestamp &&
              repackLoad32(data + 28) == dw->txtr.count && cache->pageCount == dw->txtr.count && vpages > 0 &&
              REPACK_MAX_PAGES >= dw->txtr.count + vpages && 4096u >= rooms && 65535u >= entries && REPACK_MAX_PAGES >= donors &&
              (uint64_t) size == REPACK_HEADER_SIZE + (uint64_t) vpages * REPACK_PAGE_SIZE + (uint64_t) rooms * REPACK_ROOM_SIZE +
                                     (uint64_t) entries * REPACK_ENTRY_SIZE + (uint64_t) donors * REPACK_DONOR_SIZE &&
              repackLoad32(data + 48) == repackChecksum(data + REPACK_HEADER_SIZE, (size_t) size - REPACK_HEADER_SIZE);
    if (!ok) {
        free(data);
        return false;
    }
    const uint8_t* entryAt = data + REPACK_HEADER_SIZE + vpages * REPACK_PAGE_SIZE + rooms * REPACK_ROOM_SIZE;
    if (!repackDonorsMatch(cache, entryAt + entries * REPACK_ENTRY_SIZE, donors, entryAt, entries)) {
        free(data);
        logWarn(repackLogDonor);
        return false;
    }
    *out = data;
    *outSize = size;
    return true;
}

void __real_CtrTextures_init(CtrTextureCache* cache, DataWin* dataWin, FileSystem* fileSystem, uint32_t budgetBytes);

// The sprites whose texels a load-time rewrite changes: pixpress (ctr_pixfix_press.c) rewrites PRESS in this one.
static const char repackPress[] REPACK_RO = "spr_pressfront_b";
static const char repackLogKeep[] REPACK_RO = "Repack: page %u tile %u kept for a load-time rewrite\n";

// A tile whose texels something rewrites as it loads (pixpress's PRESS, ctr_pixfix_press.c, which looks at the
// original tile only) is not drawn from a new tile in any room: its entries are emptied (no blocks, vpage
// UINT16_MAX), so every piece on it is drawn from it and repackWant leaves it wanted. x0..y1: a rectangle on the page.
REPACK_TEXT static void repackKeep(DataWin* dw, uint32_t page, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (repack.entries == nullptr || repack.firstPage <= page || 0 > x0 || 0 > y0 || x0 >= x1 || y0 >= y1) return;
    int32_t tilesX = (int32_t) (dw->txtr.textures[page].textureWidth + REPACK_TILE - 1) / REPACK_TILE;
    for (uint32_t i = 0; repack.entryCount > i; i++) {
        RepackEntry* e = &repack.entries[i];
        int32_t tx = e->donorTile % tilesX, ty = e->donorTile / tilesX;
        if (e->donorPage != page || e->vpage == UINT16_MAX || tx < x0 / REPACK_TILE || tx > (x1 - 1) / REPACK_TILE ||
            ty < y0 / REPACK_TILE || ty > (y1 - 1) / REPACK_TILE) {
            continue;
        }
        memset(e->mask, 0, sizeof(e->mask));
        e->vpage = UINT16_MAX;
        logInfo(repackLogKeep, (unsigned) page, (unsigned) e->donorTile);
    }
}

#ifdef CTRPORT_REPACK_TEST
#include <dirent.h>
// Test builds only (build.sh repacktest=<folder>): the build carries a chapter's repack files in its RomFS
// (romfs:/repack/<data.win folder>/) and puts them in the cache folder before they are read, so a hardware row needs
// no SD step. A file already there with the same bytes is left alone; any other is deleted first (the SD's game
// files can be read-only hard links). The donor check (repackDonorsMatch) still decides whether they are used.
static const char repackRomfsMount[] REPACK_RO = "romfs";
static const char repackRomfsDir[] REPACK_RO = "romfs:/repack/%s";
static const char repackWb[] REPACK_RO = "wb";
static const char repackLogInstall[] REPACK_RO = "Repack: test file %s from the RomFS: %s (%ld B)\n";
static const char repackLogSame[] REPACK_RO = "already in the cache";
static const char repackLogCopied[] REPACK_RO = "copied";
static const char repackLogFailed[] REPACK_RO = "NOT copied";

REPACK_TEXT static uint8_t* repackSlurp(const char* path, long* size) {
    FILE* f = fopen(path, repackRb);
    if (f == nullptr) return nullptr;
    long n = CtrPack_fileSize(f);
    uint8_t* data = n > 0 && 16L * 1024 * 1024 >= n ? malloc((size_t) n) : nullptr;
    if (data != nullptr && fread(data, 1, (size_t) n, f) != (size_t) n) {
        free(data);
        data = nullptr;
    }
    fclose(f);
    *size = n;
    return data;
}

REPACK_TEXT static void repackInstall(void) {
    char folder[64], dirPath[96];
    if (!CtrPack_romfsName(folder, sizeof(folder)) || R_FAILED(CtrRomfs_init(repackRomfsMount))) return;
    snprintf(dirPath, sizeof(dirPath), repackRomfsDir, folder);
    DIR* dir = opendir(dirPath);
    if (dir == nullptr) return;
    for (struct dirent* d; (d = readdir(dir)) != nullptr;) {
        if (d->d_name[0] == '.') continue;
        char from[CTR_PACK_PATH_MAX], to[CTR_PACK_PATH_MAX];
        snprintf(from, sizeof(from), "%s/%s", dirPath, d->d_name);
        CtrPack_cachePath(to, sizeof(to), d->d_name);
        long size = 0, haveSize = 0;
        uint8_t* data = repackSlurp(from, &size);
        if (data == nullptr) continue;
        uint8_t* have = repackSlurp(to, &haveSize);
        bool same = have != nullptr && haveSize == size && memcmp(have, data, (size_t) size) == 0;
        free(have);
        bool copied = same;
        if (!same) {
            remove(to);
            FILE* f = fopen(to, repackWb);
            copied = f != nullptr && fwrite(data, 1, (size_t) size, f) == (size_t) size;
            if (f != nullptr && fclose(f) != 0) copied = false;
            if (!copied) remove(to);
        }
        free(data);
        logInfo(repackLogInstall, d->d_name, same ? repackLogSame : copied ? repackLogCopied : repackLogFailed, size);
    }
    closedir(dir);
}
#endif

void Repack_texturesInit(CtrTextureCache* cache, DataWin* dataWin, FileSystem* fileSystem, uint32_t budgetBytes);

// pixpress's CtrTextures_init wrapper (ctr_pixfix_press.c) calls this in the real one's place.
REPACK_TEXT __attribute__((noipa)) void Repack_texturesInit(CtrTextureCache* cache, DataWin* dw, FileSystem* fileSystem, uint32_t budgetBytes) {
    __real_CtrTextures_init(cache, dw, fileSystem, budgetBytes);
    repackForget();
    uint8_t* data;
    long size;
#ifdef CTRPORT_REPACK_TEST
    repackInstall();
#endif
    if (!CtrPack_lossy() || !repackRead(cache, dw, &data, &size)) return;
    uint32_t count = dw->txtr.count;
    uint32_t vpages = repackLoad32(data + 32), roomCount = repackLoad32(data + 36), entryCount = repackLoad32(data + 40);
    const uint8_t* pageAt = data + REPACK_HEADER_SIZE;
    const uint8_t* roomAt = pageAt + vpages * REPACK_PAGE_SIZE;
    const uint8_t* entryAt = roomAt + roomCount * REPACK_ROOM_SIZE;
    RepackRoom* rooms = malloc((roomCount > 0 ? roomCount : 1) * sizeof(RepackRoom));
    RepackEntry* entries = malloc((entryCount > 0 ? entryCount : 1) * sizeof(RepackEntry));
    RepackMemo* memo = calloc(REPACK_MEMO, sizeof(RepackMemo));
    RepackBig* big = calloc(REPACK_BIG, sizeof(RepackBig));
    bool ok = rooms != nullptr && entries != nullptr && memo != nullptr && big != nullptr;
    for (uint32_t i = 0; ok && vpages > i; i++) {
        uint16_t w, h;
        memcpy(&w, pageAt + i * REPACK_PAGE_SIZE, 2);
        memcpy(&h, pageAt + i * REPACK_PAGE_SIZE + 2, 2);
        ok = w == 16 * REPACK_TILE && h > 0 && h <= 16 * REPACK_TILE && h % REPACK_TILE == 0 && repackPageFile(cache, count + i);
    }
    for (uint32_t i = 0; ok && roomCount > i; i++) {
        memcpy(&rooms[i], roomAt + i * REPACK_ROOM_SIZE, sizeof(RepackRoom));
        ok = entryCount >= rooms[i].first && entryCount - rooms[i].first >= rooms[i].count;
    }
    for (uint32_t i = 0; ok && entryCount > i; i++) {
        memcpy(&entries[i], entryAt + i * REPACK_ENTRY_SIZE, sizeof(RepackEntry));
        const RepackEntry* e = &entries[i];
        // vpage UINT16_MAX: a kept tile, drawn from the original one, which the room load loads (see the wrapper).
        ok = count > e->donorPage && 256u > e->donorTile && ((vpages > e->vpage && 256u > e->vtile) || e->vpage == UINT16_MAX);
    }
    // Sorted within each room: a room's entries for a page are one run.
    for (uint32_t r = 0; ok && roomCount > r; r++) {
        for (uint32_t i = rooms[r].first + 1; ok && rooms[r].first + rooms[r].count > i; i++) {
            const RepackEntry* a = &entries[i - 1];
            const RepackEntry* b = &entries[i];
            ok = a->donorPage < b->donorPage || (a->donorPage == b->donorPage && a->donorTile < b->donorTile);
        }
    }
    Texture* textures = ok ? realloc(dw->txtr.textures, (count + vpages) * sizeof(Texture)) : nullptr;
    if (textures != nullptr) dw->txtr.textures = textures;
    CtrPage* pages = textures != nullptr ? realloc(cache->pages, (count + vpages) * sizeof(CtrPage)) : nullptr;
    if (pages == nullptr) {
        free(rooms);
        free(entries);
        free(memo);
        free(big);
        free(data);
        logWarn(repackLogOff);
        return;
    }
    cache->pages = pages;
    for (uint32_t i = 0; vpages > i; i++) {
        Texture* t = &textures[count + i];
        memset(t, 0, sizeof(*t));
        uint16_t w, h;
        memcpy(&w, pageAt + i * REPACK_PAGE_SIZE, 2);
        memcpy(&h, pageAt + i * REPACK_PAGE_SIZE + 2, 2);
        t->present = true;
        t->mapped = true;
        t->textureWidth = w;
        t->textureHeight = h;
        t->blobOffset = repackLoad32(pageAt + i * REPACK_PAGE_SIZE + 4);
        t->blobSize = repackLoad32(pageAt + i * REPACK_PAGE_SIZE + 8);
        t->blobData = repack.blob;
        memset(&pages[count + i], 0, sizeof(CtrPage));
        pages[count + i].firstUse = UINT32_MAX;
        pages[count + i].cacheFile = CTR_CACHE_FILE_VALID;
    }
    cache->pageCount = count + vpages;
    free(data);
    repack.cache = cache;
    repack.pages = cache->pages;
    repack.firstPage = count;
    repack.roomCount = roomCount;
    repack.rooms = rooms;
    repack.entries = entries;
    repack.entryCount = entryCount;
    repack.room = -1;
    repackMemo = memo;
    repackBig = big;
    logInfo(repackLogOn, (unsigned) vpages, (unsigned) roomCount, (unsigned) entryCount);
    repackMemoGen++;
    for (uint32_t i = 0; dw->sprt.count > i; i++) {
        const Sprite* sp = &dw->sprt.sprites[i];
        if (sp->name == nullptr || strcmp(sp->name, repackPress) != 0) continue;
        for (uint32_t f = 0; sp->textureCount > f; f++) {
            int32_t t = sp->tpagIndices[f];
            if (0 > t || dw->tpag.count <= (uint32_t) t) continue;
            const TexturePageItem* item = &dw->tpag.items[t];
            if (0 > item->texturePageId) continue;
            repackKeep(dw, (uint32_t) item->texturePageId, item->sourceX, item->sourceY, item->sourceX + item->sourceWidth,
                       item->sourceY + item->sourceHeight);
        }
    }
}

// The room's whole recorded set is what it wants: all its new tiles, which filling in (ctr_textures.c fillWanted)
// loads over its first frames, and none of the original tiles they stand for, which the room's start marked for
// the sprites it shows. Done on the room's first draw and again on the frames after the room's second look
// (secondLook), which marks its instances' sprites again.
REPACK_TEXT static void repackWant(CtrTextureCache* cache, uint32_t count) {
    bool marked = false;
    for (uint32_t i = 0; count > i; i++) {
        const RepackEntry* e = &repack.active[i];
        if (e->vpage == UINT16_MAX) continue;
        CtrPage* dp = &cache->pages[e->donorPage];
        bool now = false;
        if (dp->layoutKnown && dp->tilesX * dp->tilesY > e->donorTile) {
            CtrTile* t = &dp->tiles[e->donorTile];
            now = t->now;
            t->wanted = false;
            t->now = false;
        }
        CtrPage* vp = CtrTextures_acquire(cache, repack.firstPage + e->vpage);
        if (vp == nullptr || vp->tilesX * vp->tilesY <= e->vtile) continue;
        CtrTile* vt = &vp->tiles[e->vtile];
        if (!vt->wanted) marked = true;
        vt->wanted = true;
        vt->now = vt->now || now;
        // As ctr_textures.c's marking: kept from eviction for the others, and a first-frame tile's page is looked at
        // by the room load's first pass (z_roomload.c).
        vt->lastUsedFrame = cache->frame;
        if (now) {
            if (!vp->wantedNow) vp->wantDepth = INT32_MIN;
            vp->wantedNow = true;
        }
    }
    if (marked && !cache->fillPending) {
        cache->fillPending = true;
        cache->fillPage = repack.firstPage;
        cache->fillRevisit = UINT32_MAX;
        if (cache->fillFrom > cache->frame) cache->fillFrom = cache->frame;
    }
}

// The remapped tiles of the room now shown on the page, if any.
REPACK_TEXT static bool repackList(CtrTextureCache* cache, const CtrPage* page, const RepackEntry** list, uint32_t* n) {
    if (repack.cache != cache || repack.pages != cache->pages || cache->runner == nullptr) return false;
    if (page < cache->pages || page >= cache->pages + repack.firstPage) return false;
    int32_t room = cache->runner->currentRoomIndex;
    if (room == repack.room && repack.active != nullptr && repack.wantFrame != cache->frame && cache->fillFrom + 2 >= cache->frame &&
        cache->frame >= cache->fillFrom) {
        repack.wantFrame = cache->frame;
        repackWant(cache, repack.activeCount);
    }
    if (room != repack.room) {
        repack.room = room;
        repackMemoGen++;
        repack.active = nullptr;
        repack.activeCount = 0;
        memset(repack.pageStart, 0, sizeof(repack.pageStart));
        memset(repack.tileMask, 0, sizeof(repack.tileMask));
        for (uint32_t i = 0; repack.roomCount > i; i++) {
            if ((int32_t) repack.rooms[i].room != room) continue;
            const RepackEntry* e = repack.entries + repack.rooms[i].first;
            uint32_t count = repack.rooms[i].count;
            repack.active = e;
            repack.activeCount = count;
            uint32_t at = 0;
            for (uint32_t p = 0; repack.firstPage >= p; p++) {
                repack.pageStart[p] = (uint16_t) at;
                while (count > at && e[at].donorPage == p) at++;
            }
            for (uint32_t j = 0; count > j; j++) {
                if (e[j].vpage == UINT16_MAX || e[j].donorPage >= repack.firstPage) continue;
                repack.tileMask[e[j].donorPage] |= 64 > e[j].donorTile ? (uint64_t) 1u << e[j].donorTile : UINT64_MAX;
            }
            repack.wantFrame = cache->frame;
            repackWant(cache, count);
            break;
        }
    }
    if (repack.active == nullptr) return false;
    uint32_t pageId = (uint32_t) (page - cache->pages);
    uint32_t a = repack.pageStart[pageId], b = repack.pageStart[pageId + 1];
    if (a == b || page->tileW != REPACK_TILE || page->tileH != REPACK_TILE) return false;
    *list = repack.active + a;
    *n = b - a;
    return true;
}

// The room change's hidden load (z_roomload.c) loads the tiles the new room's first frame draws, and what else fits
// its budget, before that frame is shown. The room's marks are moved onto the new tiles first (repackList's
// repackWant), so it loads those, not the original tiles they stand in for. Without repack.bin, nothing changes.
#define REPACK_EXTRA_NANOS (300ull * 1000000ull)
void __real_CtrTextures_roomLoadFinish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles);
void __wrap_CtrTextures_roomLoadFinish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles);
REPACK_TEXT void __wrap_CtrTextures_roomLoadFinish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles) {
    const RepackEntry* list;
    uint32_t n;
    if (cache->pageCount != 0) repackList(cache, cache->pages, &list, &n);
    // A room with new tiles gets REPACK_EXTRA_NANOS more: its first frame reads the new tiles and the original tiles
    // they don't stand in for (palette-shader draws, art the record missed), and in flowerydash the base budget then
    // left 26 marked tiles to the first frame (155 ms of reads in it). The frame is never shown (z_utstall.c's rule).
    bool ours = repack.cache == cache && repack.active != nullptr && cache->runner != nullptr && repack.room == cache->runner->currentRoomIndex &&
                repack.activeCount > 0;
    __real_CtrTextures_roomLoadFinish(cache, ours ? deadline + REPACK_EXTRA_NANOS : deadline, nowTiles, otherTiles);
    // Then the rest of the room's new tiles, past the load's budget: the room's read-ahead was planned before the
    // marks above (CtrTextures_prefetchRoom), so their page would be read after the room's first step, and a draw
    // then waits for the whole page file. Loaded here, behind the room change, nothing is left to read in play.
    // The kept tiles too: repack.bin lists a room's tiles that its draws read with no recorded sprite to put them in
    // the room's set (flowerydash's finale reads 8 at its frame 830, 10 SD loads and 165 ms in one frame on a New
    // 3DS), and the tiles around them that the same draws read (repack.py build).
    if (!ours) return;
    uint64_t start = svcGetSystemTick();
    uint32_t loaded = 0, kept = 0;
    for (uint32_t i = 0; repack.activeCount > i; i++) {
        const RepackEntry* e = &repack.active[i];
        if (e->vpage == UINT16_MAX) {
            CtrPage* dp = CtrTextures_acquire(cache, e->donorPage);
            if (dp == nullptr || dp->tileW != REPACK_TILE || dp->tileH != REPACK_TILE || dp->tilesX * dp->tilesY <= (int32_t) e->donorTile) continue;
            if (dp->tiles[e->donorTile].valid) continue;
            CtrTextures_ensureTiles(cache, dp, (int32_t) e->donorTile % dp->tilesX, (int32_t) e->donorTile / dp->tilesX,
                                    (int32_t) e->donorTile % dp->tilesX, (int32_t) e->donorTile / dp->tilesX);
            kept++;
            continue;
        }
        CtrPage* vp = CtrTextures_acquire(cache, repack.firstPage + e->vpage);
        if (vp == nullptr || vp->tileW != REPACK_TILE || vp->tileH != REPACK_TILE || vp->tilesX * vp->tilesY <= (int32_t) e->vtile) continue;
        if (vp->tiles[e->vtile].valid) continue;
        CtrTextures_ensureTiles(cache, vp, (int32_t) e->vtile % vp->tilesX, (int32_t) e->vtile / vp->tilesX, (int32_t) e->vtile % vp->tilesX,
                                (int32_t) e->vtile / vp->tilesX);
        loaded++;
    }
    if (loaded + kept > 0) logInfo(repackLogRoom, (unsigned) loaded, (unsigned) kept, (double) (svcGetSystemTick() - start) / CPU_TICKS_PER_MSEC);
}

REPACK_TEXT static const RepackEntry* repackFind(const RepackEntry* list, uint32_t n, int32_t tile) {
    uint32_t lo = 0, hi = n;
    while (hi > lo) {
        uint32_t mid = (lo + hi) / 2;
        if (list[mid].donorTile < tile) lo = mid + 1;
        else hi = mid;
    }
    return n > lo && list[lo].donorTile == tile ? &list[lo] : nullptr;
}

// Whether the entry's new tile holds every block of texels x0 <= x < x1, y0 <= y < y1 of its tile (clipped to it).
REPACK_TEXT static bool repackCoversBox(const RepackEntry* e, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (0 > x0) x0 = 0;
    if (0 > y0) y0 = 0;
    if (x1 > REPACK_TILE) x1 = REPACK_TILE;
    if (y1 > REPACK_TILE) y1 = REPACK_TILE;
    if (x0 >= x1 || y0 >= y1) return false;
    int32_t bx0 = x0 >> 3, bx1 = (x1 - 1) >> 3;
    uint32_t row = (bx1 == 31 ? UINT32_MAX : (1u << (bx1 + 1)) - 1u) & ~((1u << bx0) - 1u);
    for (int32_t by = y0 >> 3; (y1 - 1) >> 3 >= by; by++) {
        if ((e->mask[by] & row) != row) return false;
    }
    return true;
}

// Whether the entry's new tile holds every texel a piece with texture coordinates uv (on its 256 x 256 tile)
// reads: its texels and one more on each side, which bilinear filtering and the GPU's rounding can reach.
REPACK_TEXT static bool repackCovers(const RepackEntry* e, const float* uv) {
    float u0 = uv[0] < uv[2] ? uv[0] : uv[2], u1 = uv[0] < uv[2] ? uv[2] : uv[0];
    float v0 = uv[1] < uv[3] ? uv[1] : uv[3], v1 = uv[1] < uv[3] ? uv[3] : uv[1];
    return repackCoversBox(e, (int32_t) floorf(u0 * REPACK_TILE) - 1, (int32_t) floorf(v0 * REPACK_TILE) - 1, (int32_t) ceilf(u1 * REPACK_TILE) + 1,
                           (int32_t) ceilf(v1 * REPACK_TILE) + 1);
}

REPACK_TEXT static uint32_t repackBits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

REPACK_TEXT static uint32_t repackHash(const CtrPage* page, float sx0, float sy0, float sx1, float sy1) {
    uint32_t h = repackBits(sx0) * 0x9E3779B1u ^ repackBits(sy0) * 0x85EBCA77u ^ repackBits(sx1) * 0xC2B2AE3Du ^
                 repackBits(sy1) * 0x27D4EB2Fu ^ (uint32_t) (uintptr_t) page;
    return h ^ h >> 15;
}

REPACK_TEXT static RepackMemo* repackMemoFor(const CtrPage* page, float sx0, float sy0, float sx1, float sy1) {
    return &repackMemo[repackHash(page, sx0, sy0, sx1, sy1) & (REPACK_MEMO - 1u)];
}

// quadPosition's arithmetic, inlined here: a call from this section made gcc keep a copy of it in the main .text.
__attribute__((always_inline)) static inline void repackQuadPosition(const float* px, const float* py, float s, float t, float* outX, float* outY) {
    float topX = px[0] + (px[1] - px[0]) * s;
    float topY = py[0] + (py[1] - py[0]) * s;
    float botX = px[3] + (px[2] - px[3]) * s;
    float botY = py[3] + (py[2] - py[3]) * s;
    *outX = topX + (botX - topX) * t;
    *outY = topY + (botY - topY) * t;
}

// Whether CtrTextures_ensureTiles would change nothing for this one tile: already stamped this frame (so not newly
// hot), loaded, and recorded for this visit if a room is being recorded (ctr_renderer.c utkTileDone).
__attribute__((always_inline)) static inline bool repackTileDone(const CtrTextureCache* cache, const CtrTile* tile) {
    return tile->lastUsedFrame == cache->frame && tile->valid && (0 > cache->recordRoom || tile->seenVisit == cache->recordVisit);
}

// The new tile vtile of extra page vpage, its page's use stamped and the tile ensured, as CtrTextures_acquire and
// CtrTextures_ensureTiles would leave them; nullptr if the page can't be used.
__attribute__((always_inline)) static inline CtrPage* repackNewTile(CtrTextureCache* cache, uint32_t vpage, uint32_t vtile) {
    CtrPage* vp = &cache->pages[repack.firstPage + vpage];
    vp->lastUsedFrame = cache->frame;
    if (!vp->layoutKnown) vp = CtrTextures_acquire(cache, repack.firstPage + vpage);
    if (vp == nullptr || vp->tileW != REPACK_TILE || vp->tileH != REPACK_TILE || vp->tilesX * vp->tilesY <= (int32_t) vtile) return nullptr;
    if (!repackTileDone(cache, &vp->tiles[vtile])) {
        CtrTextures_ensureTiles(cache, vp, (int32_t) vtile % vp->tilesX, (int32_t) vtile / vp->tilesX, (int32_t) vtile % vp->tilesX,
                                (int32_t) vtile / vp->tilesX);
    }
    return vp;
}

// Smart smoothing's first looks in a remapped room. itemVerdict and smartFontState (ctr_renderer.c) read an item's or
// a font's texels on the CPU once a session, at its first draw, from the tiles CtrTextures_ensureTiles loads for them.
// A remapped room no longer loads the original tiles at its room change, so such a look loaded them from SD in play
// (ch4_titan's frame 420 on Azahar: 7 tiles, 118 ms, for a battle-panel item and two sprite fonts). Cmdlabel_smartMix
// (items over tiles of two formats) still reads the originals: a change there would move the late code after it.
// repackSmartPage stands in for that ensureTiles call. Each tile of the region that isn't resident is read from its
// new tile when that holds every block of the region's part of the tile and one texel around it; every other tile
// is the original, ensured as before. With a new tile in use it returns a copy of the page whose tile list has the
// new tiles in the originals' places: each block keeps its place in its tile and a new tile has its donor's format, so
// every texel reads as it would from the original. The look reads the copy and keeps nothing of it (its verdicts are
// per item). Anything else (repack off, another room or page, nothing to move) is the original call.
#define REPACK_SMART_TILES 64
REPACK_BSS static CtrPage repackSmartCopy;
REPACK_BSS static CtrTile* repackSmartTiles;
REPACK_BSS static int32_t repackSmartCap;

REPACK_TEXT __attribute__((noipa)) static CtrPage* repackSmartPage(CtrTextureCache* cache, CtrPage* page, int32_t x0, int32_t y0, int32_t w, int32_t h) {
    int32_t tx0 = x0 / page->tileW, ty0 = y0 / page->tileH, tx1 = (x0 + w - 1) / page->tileW, ty1 = (y0 + h - 1) / page->tileH;
    int32_t nx = tx1 - tx0 + 1, count = nx * (ty1 - ty0 + 1);
    const RepackEntry* to[REPACK_SMART_TILES];
    const RepackEntry* list;
    uint32_t n;
    uint64_t moved = 0;
    if (0 <= tx0 && 0 <= ty0 && page->tilesX > tx1 && page->tilesY > ty1 && REPACK_SMART_TILES >= count && repackList(cache, page, &list, &n)) {
        for (int32_t k = 0; count > k; k++) {
            int32_t tx = tx0 + k % nx, ty = ty0 + k / nx, ti = ty * page->tilesX + tx;
            int32_t bx = tx * REPACK_TILE, by = ty * REPACK_TILE;
            // Whole tiles only, as repack.py remaps them.
            if (page->tiles[ti].valid || bx + REPACK_TILE > page->width || by + REPACK_TILE > page->height) continue;
            const RepackEntry* e = repackFind(list, n, ti);
            if (e == nullptr || e->vpage == UINT16_MAX || !repackCoversBox(e, x0 - 1 - bx, y0 - 1 - by, x0 + w + 1 - bx, y0 + h + 1 - by)) continue;
            to[k] = e;
            moved |= (uint64_t) 1u << k;
        }
    }
    if (moved != 0 && page->tilesX * page->tilesY > repackSmartCap) {
        CtrTile* grown = realloc(repackSmartTiles, (size_t) (page->tilesX * page->tilesY) * sizeof(CtrTile));
        if (grown != nullptr) {
            repackSmartTiles = grown;
            repackSmartCap = page->tilesX * page->tilesY;
        } else {
            moved = 0;
        }
    }
    if (moved == 0) {
        CtrTextures_ensureTiles(cache, page, tx0, ty0, tx1, ty1);
        return page;
    }
    // The new tiles, then the originals of the rest (a new tile that can't load leaves its original to load).
    for (int32_t k = 0; count > k; k++) {
        if ((moved >> k & 1u) == 0) continue;
        CtrPage* vp = repackNewTile(cache, to[k]->vpage, to[k]->vtile);
        if (vp == nullptr || !vp->tiles[to[k]->vtile].valid) moved &= ~((uint64_t) 1u << k);
    }
    for (int32_t k = 0; count > k; k++) {
        if ((moved >> k & 1u) != 0) continue;
        int32_t tx = tx0 + k % nx, ty = ty0 + k / nx;
        CtrTextures_ensureTiles(cache, page, tx, ty, tx, ty);
    }
    if (moved == 0) return page;
    memcpy(repackSmartTiles, page->tiles, (size_t) (page->tilesX * page->tilesY) * sizeof(CtrTile));
    for (int32_t k = 0; count > k; k++) {
        if ((moved >> k & 1u) == 0) continue;
        int32_t ti = (ty0 + k / nx) * page->tilesX + tx0 + k % nx;
        repackSmartTiles[ti] = cache->pages[repack.firstPage + to[k]->vpage].tiles[to[k]->vtile];
    }
    repackSmartCopy = *page;
    repackSmartCopy.tiles = repackSmartTiles;
    return &repackSmartCopy;
}

// A draw whose memo has every tile of its range moved (REPACK_MEMO_MOVED, or REPACK_MEMO_MULTI with all pieces in
// moved), drawn as repackEmitSlow draws it from that memo, without its lookups: the new tiles ensured, then each piece
// drawn from its new tile, or from the original one if the new one isn't loaded. False, having done nothing, for
// any other memo.
__attribute__((always_inline)) static inline bool repackEmitHit(CtrRenderer* r, CtrPage* page, const RepackMemo* memo, const float* px,
                                                               const float* py, const CtrColor* c) {
    uint32_t count = memo->count;
    if (memo->moved != (1u << count) - 1u) return false;
    CtrTextureCache* cache = &r->textures;
    for (uint32_t j = 0; count > j; j++) repackNewTile(cache, memo->to[j]->vpage, memo->to[j]->vtile);
    bool uniform = c[0].r == c[1].r && c[0].g == c[1].g && c[0].b == c[1].b && c[0].a == c[1].a &&
                   c[0].r == c[2].r && c[0].g == c[2].g && c[0].b == c[2].b && c[0].a == c[2].a &&
                   c[0].r == c[3].r && c[0].g == c[3].g && c[0].b == c[3].b && c[0].a == c[3].a;
    for (uint32_t j = 0; count > j; j++) {
        const RepackEntry* e = memo->to[j];
        CtrPage* from = &cache->pages[repack.firstPage + e->vpage];
        int32_t fromTile = e->vtile;
        if (!from->layoutKnown || from->tilesX * from->tilesY <= fromTile || !from->tiles[fromTile].valid) {
            int32_t tx = memo->tx0 + (int32_t) (j % memo->nx), ty = memo->ty0 + (int32_t) (j / memo->nx);
            CtrTextures_ensureTiles(cache, page, tx, ty, tx, ty);
            from = page;
            fromTile = ty * page->tilesX + tx;
        }
        CtrTile* tile = &from->tiles[fromTile];
        if (!tile->valid) continue;
        const float* q = memo->part[j];
        float qx[4], qy[4], uv[4] = {memo->uv[j][0], memo->uv[j][1], memo->uv[j][2], memo->uv[j][3]};
        CtrColor qc[4];
        if (uniform) {
            repackQuadPosition(px, py, q[0], q[1], &qx[0], &qy[0]);
            repackQuadPosition(px, py, q[2], q[1], &qx[1], &qy[1]);
            repackQuadPosition(px, py, q[2], q[3], &qx[2], &qy[2]);
            repackQuadPosition(px, py, q[0], q[3], &qx[3], &qy[3]);
            qc[0] = qc[1] = qc[2] = qc[3] = c[0];
        } else {
            quadPoint(px, py, c, q[0], q[1], &qx[0], &qy[0], &qc[0]);
            quadPoint(px, py, c, q[2], q[1], &qx[1], &qy[1], &qc[1]);
            quadPoint(px, py, c, q[2], q[3], &qx[2], &qy[2], &qc[2]);
            quadPoint(px, py, c, q[0], q[3], &qx[3], &qy[3], &qc[3]);
        }
        C3D_Tex* tex = palTileTex(r, from, fromTile, &tile->tex, &uv[0], &uv[1], &uv[2], &uv[3]);
        emitQuad(r, tex, qx, qy, uv[0], uv[1], uv[2], uv[3], qc);
        r->palNextB = nullptr;
    }
    return true;
}

// The memo's piece k: regionTilePart's piece s and coordinates uv.
__attribute__((always_inline)) static inline void repackMemoPiece(RepackMemo* memo, uint32_t k, const float* s, const float* uv) {
    memcpy(memo->part[k], s, sizeof(memo->part[k]));
    memcpy(memo->uv[k], uv, sizeof(memo->uv[k]));
}

#undef emitPageTiles
// emitPageTiles with remapped pieces drawn from the new tiles. Its arithmetic, piece for piece, is
// emitPageTiles' (without palette shaders, which this leaves to it), which a change must keep alike.
REPACK_TEXT __attribute__((noinline)) static void repackEmitSlow(CtrRenderer* r, CtrPage* page, float sx0, float sy0, float sx1, float sy1,
                                                                const float* px, const float* py, const CtrColor* c) {
    CtrTextureCache* cache = &r->textures;
    const RepackEntry* list;
    uint32_t n;
    if (r->palKind != PAL_NONE || (page->tilesX == 1 && page->tilesY == 1) || !repackList(cache, page, &list, &n)) {
        emitPageTiles(r, page, sx0, sy0, sx1, sy1, px, py, c);
        return;
    }
    float tileW = (float) page->tileW;
    float tileH = (float) page->tileH;
    float w = sx1 - sx0;
    float h = sy1 - sy0;
    if (w == 0.0f || h == 0.0f) return;
    float minX = sx0 < sx1 ? sx0 : sx1, maxX = sx0 < sx1 ? sx1 : sx0;
    float minY = sy0 < sy1 ? sy0 : sy1, maxY = sy0 < sy1 ? sy1 : sy0;
    int32_t tx0 = (int32_t) (minX / tileW), tx1 = (int32_t) ((maxX - 0.001f) / tileW);
    int32_t ty0 = (int32_t) (minY / tileH), ty1 = (int32_t) ((maxY - 0.001f) / tileH);
    if (0 > tx0) tx0 = 0;
    if (0 > ty0) ty0 = 0;
    if (tx1 >= page->tilesX) tx1 = page->tilesX - 1;
    if (ty1 >= page->tilesY) ty1 = page->tilesY - 1;
    if (tx0 > tx1 || ty0 > ty1 || (tx1 - tx0 + 1) * (ty1 - ty0 + 1) > 64) {
        emitPageTiles(r, page, sx0, sy0, sx1, sy1, px, py, c);
        return;
    }
    // No tile of the range has an entry: drawn as before, without the lookups.
    if (page->tilesX * page->tilesY <= 64) {
        uint64_t row = (tx1 == 7 ? 0xFFu : (1u << (tx1 + 1)) - 1u) & ~((1u << tx0) - 1u);
        uint64_t range = 0;
        if (page->tilesX == 8) {
            for (int32_t ty = ty0; ty1 >= ty; ty++) range |= row << (ty * 8);
        } else {
            for (int32_t ty = ty0; ty1 >= ty; ty++) {
                for (int32_t tx = tx0; tx1 >= tx; tx++) range |= (uint64_t) 1u << (ty * page->tilesX + tx);
            }
        }
        if ((range & repack.tileMask[page - cache->pages]) == 0) {
            emitPageTiles(r, page, sx0, sy0, sx1, sy1, px, py, c);
            return;
        }
    }
    RepackMemo* memo = repackMemoFor(page, sx0, sy0, sx1, sy1);
    uint32_t kind = memo->gen == repackMemoGen && memo->page == page && memo->s[0] == sx0 && memo->s[1] == sy0 && memo->s[2] == sx1 &&
                    memo->s[3] == sy1 ? memo->kind : 0;
    if (kind == REPACK_MEMO_PLAIN) {
        emitPageTiles(r, page, sx0, sy0, sx1, sy1, px, py, c);
        return;
    }
    // The pieces drawn from new tiles (bit k for the k-th tile of the range, row by row), and their entries.
    uint64_t moved = 0;
    const RepackEntry* toAt[64];
    const RepackEntry** to = toAt;
    uint32_t k = 0;
    uint32_t pieceCount = (uint32_t) ((tx1 - tx0 + 1) * (ty1 - ty0 + 1));
    RepackBig* big = REPACK_MEMO_TILES < (int32_t) pieceCount ? &repackBig[repackHash(page, sx0, sy0, sx1, sy1) & (REPACK_BIG - 1u)] : nullptr;
    if (big != nullptr && big->gen == repackMemoGen && big->page == page && big->s[0] == sx0 && big->s[1] == sy0 && big->s[2] == sx1 &&
        big->s[3] == sy1) {
        moved = big->moved;
        to = big->to;
    } else if (kind == REPACK_MEMO_MOVED || kind == REPACK_MEMO_MULTI) {
        moved = memo->moved;
        for (int32_t j = 0; REPACK_MEMO_TILES > j; j++) to[j] = memo->to[j];
    } else {
        for (int32_t ty = ty0; ty1 >= ty; ty++) {
            for (int32_t tx = tx0; tx1 >= tx; tx++, k++) {
                float s[4], uv[4];
                const RepackEntry* e = repackFind(list, n, ty * page->tilesX + tx);
                if (e == nullptr || !regionTilePart(sx0, sy0, w, h, minX, minY, maxX, maxY, tileW, tileH, tx, ty, s, uv) || !repackCovers(e, uv)) continue;
                to[k] = e;
                moved |= (uint64_t) 1u << k;
                if (big == nullptr) repackMemoPiece(memo, k, s, uv); // the memo made below (a range of up to REPACK_MEMO_TILES)
            }
        }
        const CtrPage* vp = moved != 0 ? &cache->pages[repack.firstPage + to[0]->vpage] : nullptr;
        float part[4], uv[4];
        if (tx0 == tx1 && ty0 == ty1 &&
            (vp == nullptr || (vp->layoutKnown && vp->tileW == REPACK_TILE && vp->tileH == REPACK_TILE && vp->tilesX * vp->tilesY > to[0]->vtile &&
                               regionTilePart(sx0, sy0, w, h, minX, minY, maxX, maxY, tileW, tileH, tx0, ty0, part, uv)))) {
            repackMemoPiece(memo, 0, part, uv);
            memo->page = page;
            memo->s[0] = sx0;
            memo->s[1] = sy0;
            memo->s[2] = sx1;
            memo->s[3] = sy1;
            memo->to[0] = moved != 0 ? to[0] : nullptr;
            memo->moved = (uint32_t) moved;
            memo->tx0 = (uint16_t) tx0;
            memo->ty0 = (uint16_t) ty0;
            memo->count = 1;
            memo->nx = 1;
            memo->gen = repackMemoGen;
            memo->kind = moved != 0 ? REPACK_MEMO_MOVED : REPACK_MEMO_PLAIN;
        } else if (big != nullptr) {
            big->page = page;
            big->s[0] = sx0;
            big->s[1] = sy0;
            big->s[2] = sx1;
            big->s[3] = sy1;
            for (uint32_t j = 0; pieceCount > j; j++) big->to[j] = (moved >> j & 1u) != 0 ? to[j] : nullptr;
            big->moved = moved;
            big->gen = repackMemoGen;
        } else if (REPACK_MEMO_TILES >= (int32_t) k) {
            memo->page = page;
            memo->s[0] = sx0;
            memo->s[1] = sy0;
            memo->s[2] = sx1;
            memo->s[3] = sy1;
            for (int32_t j = 0; REPACK_MEMO_TILES > j; j++) memo->to[j] = (moved >> j & 1u) != 0 ? to[j] : nullptr;
            memo->moved = (uint32_t) moved;
            memo->tx0 = (uint16_t) tx0;
            memo->ty0 = (uint16_t) ty0;
            memo->count = (uint8_t) k;
            memo->nx = (uint8_t) (tx1 - tx0 + 1);
            memo->gen = repackMemoGen;
            memo->kind = moved != 0 ? REPACK_MEMO_MULTI : REPACK_MEMO_PLAIN;
        }
    }
    if (moved == 0) {
        emitPageTiles(r, page, sx0, sy0, sx1, sy1, px, py, c);
        return;
    }
    // The new tiles first, then the original ones not moved (each alone while the room's tile record, which the next
    // visit loads first, is being made, so that it holds no tile drawn from a new one).
    k = 0;
    for (int32_t ty = ty0; ty1 >= ty; ty++) {
        for (int32_t tx = tx0; tx1 >= tx; tx++, k++) {
            if ((moved >> k & 1u) == 0) continue;
            repackNewTile(cache, to[k]->vpage, to[k]->vtile);
        }
    }
    if (0 > cache->recordRoom) {
        // Every piece moved: ensureTilesBut would look at the tiles and change nothing.
        if (moved != (64 > pieceCount ? ((uint64_t) 1u << pieceCount) - 1u : UINT64_MAX)) CtrTextures_ensureTilesBut(cache, page, tx0, ty0, tx1, ty1, moved);
    } else {
        k = 0;
        for (int32_t ty = ty0; ty1 >= ty; ty++) {
            for (int32_t tx = tx0; tx1 >= tx; tx++, k++) {
                if ((moved >> k & 1u) == 0) CtrTextures_ensureTiles(cache, page, tx, ty, tx, ty);
            }
        }
    }
    bool uniform = c[0].r == c[1].r && c[0].g == c[1].g && c[0].b == c[1].b && c[0].a == c[1].a &&
                   c[0].r == c[2].r && c[0].g == c[2].g && c[0].b == c[2].b && c[0].a == c[2].a &&
                   c[0].r == c[3].r && c[0].g == c[3].g && c[0].b == c[3].b && c[0].a == c[3].a;
    k = 0;
    for (int32_t ty = ty0; ty1 >= ty; ty++) {
        for (int32_t tx = tx0; tx1 >= tx; tx++, k++) {
            int32_t ti = ty * page->tilesX + tx;
            CtrPage* from = page;
            int32_t fromTile = ti;
            if ((moved >> k & 1u) != 0) {
                CtrPage* vp = &cache->pages[repack.firstPage + to[k]->vpage];
                if (vp->layoutKnown && vp->tilesX * vp->tilesY > to[k]->vtile && vp->tiles[to[k]->vtile].valid) {
                    from = vp;
                    fromTile = to[k]->vtile;
                } else {
                    CtrTextures_ensureTiles(cache, page, tx, ty, tx, ty);
                }
            }
            CtrTile* tile = &from->tiles[fromTile];
            float s[4], uv[4];
            if (!tile->valid || !regionTilePart(sx0, sy0, w, h, minX, minY, maxX, maxY, tileW, tileH, tx, ty, s, uv)) continue;
            float qx[4], qy[4];
            CtrColor qc[4];
            if (uniform) {
                quadPosition(px, py, s[0], s[1], &qx[0], &qy[0]);
                quadPosition(px, py, s[2], s[1], &qx[1], &qy[1]);
                quadPosition(px, py, s[2], s[3], &qx[2], &qy[2]);
                quadPosition(px, py, s[0], s[3], &qx[3], &qy[3]);
                qc[0] = qc[1] = qc[2] = qc[3] = c[0];
            } else {
                quadPoint(px, py, c, s[0], s[1], &qx[0], &qy[0], &qc[0]);
                quadPoint(px, py, c, s[2], s[1], &qx[1], &qy[1], &qc[1]);
                quadPoint(px, py, c, s[2], s[3], &qx[2], &qy[2], &qc[2]);
                quadPoint(px, py, c, s[0], s[3], &qx[3], &qy[3], &qc[3]);
            }
            C3D_Tex* tex = palTileTex(r, from, fromTile, &tile->tex, &uv[0], &uv[1], &uv[2], &uv[3]);
            emitQuad(r, tex, qx, qy, uv[0], uv[1], uv[2], uv[3], qc);
            r->palNextB = nullptr;
        }
    }
}

// smartTileBegin's tile for a fine draw whose texels lie in tile (tx, ty) of the page (Smart smoothing makes its quads
// itself, on the tile it is given): the new tile when the draw's piece of it is remapped and loaded, so the original
// tile is neither loaded nor batched beside the new ones. The quads' texture coordinates are the same on both, as
// each block keeps its place.
REPACK_TEXT __attribute__((noinline)) static bool repackSmartTile(CtrRenderer* r, CtrPage* page, float sx0, float sy0, float sx1, float sy1,
                                                                 int32_t tx, int32_t ty, CtrTile** out) {
    CtrTextureCache* cache = &r->textures;
    if (repack.cache != cache || repack.pages != cache->pages || cache->runner == nullptr || page < cache->pages ||
        page >= cache->pages + repack.firstPage) {
        return false;
    }
    // As repackEmitPageTiles: the room change and the fill's first frames go through repackList.
    const RepackEntry* list;
    uint32_t n;
    if (cache->runner->currentRoomIndex != repack.room ||
        (repack.wantFrame != cache->frame && cache->fillFrom + 2 >= cache->frame && cache->frame >= cache->fillFrom)) {
        if (!repackList(cache, page, &list, &n)) return false;
    }
    uint32_t pageId = (uint32_t) (page - cache->pages);
    if (repack.active == nullptr || repack.pageStart[pageId] == repack.pageStart[pageId + 1]) return false;
    int32_t ti = ty * page->tilesX + tx;
    if (64 > ti && (repack.tileMask[pageId] >> ti & 1u) == 0) return false;
    // The draw's memo, shared with repackEmitPageTiles (the same decision for the same rectangle).
    RepackMemo* memo = repackMemoFor(page, sx0, sy0, sx1, sy1);
    const RepackEntry* e;
    if (memo->gen == repackMemoGen && memo->page == page && memo->s[0] == sx0 && memo->s[1] == sy0 && memo->s[2] == sx1 && memo->s[3] == sy1 &&
        (memo->kind == REPACK_MEMO_PLAIN || memo->kind == REPACK_MEMO_MOVED)) {
        if (memo->kind == REPACK_MEMO_PLAIN) return false;
        e = memo->to[0];
    } else {
        if (!repackList(cache, page, &list, &n)) return false;
        e = repackFind(list, n, ti);
        float w = sx1 - sx0, h = sy1 - sy0;
        if (w == 0.0f || h == 0.0f) return false;
        float minX = sx0 < sx1 ? sx0 : sx1, maxX = sx0 < sx1 ? sx1 : sx0;
        float minY = sy0 < sy1 ? sy0 : sy1, maxY = sy0 < sy1 ? sy1 : sy0;
        float part[4], uv[4];
        if (!regionTilePart(sx0, sy0, w, h, minX, minY, maxX, maxY, (float) page->tileW, (float) page->tileH, tx, ty, part, uv)) return false;
        if (e != nullptr && !repackCovers(e, uv)) e = nullptr;
        const CtrPage* vp = e != nullptr ? &cache->pages[repack.firstPage + e->vpage] : nullptr;
        if (vp == nullptr || (vp->layoutKnown && vp->tileW == REPACK_TILE && vp->tileH == REPACK_TILE && vp->tilesX * vp->tilesY > e->vtile)) {
            repackMemoPiece(memo, 0, part, uv);
            memo->page = page;
            memo->s[0] = sx0;
            memo->s[1] = sy0;
            memo->s[2] = sx1;
            memo->s[3] = sy1;
            memo->to[0] = e;
            memo->moved = e != nullptr ? 1u : 0u;
            memo->tx0 = (uint16_t) tx;
            memo->ty0 = (uint16_t) ty;
            memo->count = 1;
            memo->nx = 1;
            memo->gen = repackMemoGen;
            memo->kind = e != nullptr ? REPACK_MEMO_MOVED : REPACK_MEMO_PLAIN;
        }
        if (e == nullptr) return false;
    }
    CtrPage* vp = repackNewTile(cache, e->vpage, e->vtile);
    if (vp == nullptr || !vp->tiles[e->vtile].valid) return false;
    *out = &vp->tiles[e->vtile];
    return true;
}

// What the renderer calls for emitPageTiles. A draw with nothing remapped to look at (repack off, a page with no
// entry in this room, a room with none) goes straight to emitPageTiles; a draw on a page with entries that has a
// memo is drawn from it (repackEmitHit); the room change, the first frames of the room's fill (repackList's
// repackWant) and the rest go through repackEmitSlow, which makes the memos.
REPACK_TEXT __attribute__((noipa)) static void repackEmitPageTiles(CtrRenderer* r, CtrPage* page, float sx0, float sy0, float sx1, float sy1,
                                                                  const float* px, const float* py, const CtrColor* c) {
    const CtrTextureCache* cache = &r->textures;
    if (repack.cache != cache || repack.pages != cache->pages || cache->runner == nullptr) {
        emitPageTiles(r, page, sx0, sy0, sx1, sy1, px, py, c);
        return;
    }
    if (cache->runner->currentRoomIndex == repack.room &&
        (repack.wantFrame == cache->frame || cache->fillFrom + 2 < cache->frame || cache->frame < cache->fillFrom)) {
        if (repack.active == nullptr || page < cache->pages || page >= cache->pages + repack.firstPage) {
            emitPageTiles(r, page, sx0, sy0, sx1, sy1, px, py, c);
            return;
        }
        uint32_t pageId = (uint32_t) (page - cache->pages);
        if (repack.pageStart[pageId] == repack.pageStart[pageId + 1]) {
            emitPageTiles(r, page, sx0, sy0, sx1, sy1, px, py, c);
            return;
        }
        // A draw seen before in this room (its memo made by repackEmitSlow past the same tests): drawn from the memo.
        if (r->palKind == PAL_NONE) {
            const RepackMemo* memo = repackMemoFor(page, sx0, sy0, sx1, sy1);
            if (memo->gen == repackMemoGen && memo->page == page && memo->s[0] == sx0 && memo->s[1] == sy0 && memo->s[2] == sx1 &&
                memo->s[3] == sy1) {
                if (memo->kind == REPACK_MEMO_PLAIN) {
                    emitPageTiles(r, page, sx0, sy0, sx1, sy1, px, py, c);
                    return;
                }
                if (repackEmitHit(r, page, memo, px, py, c)) return;
            }
        }
    }
    repackEmitSlow(r, page, sx0, sy0, sx1, sy1, px, py, c);
}
#define emitPageTiles repackEmitPageTiles

// smartEmitAveraged calls repackEmitPageTiles, which (noipa) may use any register, where emitPageTiles' own were
// known, and smartTileBegin, now out of line (ctr_renderer.c), where it had it inline: its body is 312 bytes
// shorter. The rest of its slot is padding after its code (smartEmitSlot's way), so nothing after it moves.
#if defined(__3DS__) && defined(NDEBUG) && !defined(CTRPORT_RENDER_TIMING)
__asm__(".pushsection .text.smartEmitAveraged, 1\n.space 312\n.popsection");
// The same for partDrawSprite (ctr_renderer_parts.inc, last in .text.utpart_late): its locals now reach a noipa
// callee (partEmit -> emitPageRegionVisible -> repackEmitPageTiles), so its fallback call to ctrDrawSpritePart is no
// longer a tail call (GCC: "call invocation refers to locals") and its body is 8 bytes shorter. The padding keeps
// everything after it in place.
__asm__(".pushsection .text.utpart_late, 1\n.space 8\n.popsection");
// itemVerdict and drawTextImpl (smartFontState and smartFontClean, inline in it) call repackSmartPage where they
// called CtrTextures_ensureTiles with the region's tiles, worked out with four divisions: 100 and 376 bytes shorter.
__asm__(".pushsection .text.itemVerdict, 1\n.space 100\n.popsection");
__asm__(".pushsection .text.drawTextImpl, 1\n.space 376\n.popsection");
// smartCleanCopy (inline in smartSpriteFine's part clone) is out of line in repack's late section (ctr_renderer.c):
// the clone is 1708 bytes shorter.
__asm__(".pushsection .text.smartSpriteFine.part.0, 1\n.space 1708\n.popsection");
#endif
#endif
