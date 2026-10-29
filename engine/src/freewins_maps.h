#pragma once
// Included in place of stb_ds.h by runner.c and vm_builtins.c, so neither file's line numbers move.
#include "stb_ds.h"

// freewins round 5 (UNDERTALE Old): the big ds_maps keep their storage across a VM reset.
//
// At game_restart UNDERTALE rebuilds its two language text maps (English, Japanese) with ds_map_add. Built
// from nothing, each one's entry array (16,455 entries) grows by doubling to 32,768 entries of 24 B, and its hash index
// to 32,768 slots: a 786,448 B realloc and a 262 KB malloc, each needing one free piece while the smaller is still held.
// After a long session the heap had no such piece (docs/3ds-hardware.md, "free heap bytes are not usable bytes").
//
// So cleanupState parks a map of FREEWINS_KEEP_MIN entries or more instead of freeing it: its keys and values are
// freed as before, then its entry array is emptied (length 1, the default entry) and its hash index cleared (every
// slot empty, no used or deleted slots), keeping both blocks' capacity. The map with the same id takes them back on
// its first ds_map_add after the reset (map ids are the pool's indexes, assigned in creation order from 0 after each
// reset). The game rebuilds the content itself, so content and order are a fresh build's: ds_map's order is the entry
// array's, which is insertion order (stb_ds deletes move the last entry into the hole, the same in either case), and
// the hash index only maps a key to its entry. A bigger index changes where keys sit in it, never what a lookup
// finds. A third big map is freed as before, and a parked map that no ds_map_add claimed is freed at the next reset.
#if defined(PLATFORM_3DS) && defined(__GNUC__) && defined(ENABLE_WAD16) && defined(CTRPORT_TARGET_O3DS)
#define FREEWINS_KEEP_MIN 4096
// The two retained slots' padding (runner.c's and vm_builtins.c's ends): cleanupState's code grows by 188 B (all of its
// padding, so it has none left), dsMapAddCommon's by 72 B, so both keep base's size and place
// (tools/analysis/check_freewins_layout.py).
#define FREEWINS_CLEANUP_PAD_ASM ""
#define FREEWINS_MAPADD_PAD "244"
typedef struct {
    int32_t id;
    DsMapEntry* map; // a parked map (stb_ds's pointer past the default entry), or nullptr
} FreewinsKeptMap;
extern FreewinsKeptMap freewinsKeptMaps[2]; // vm_builtins.c's end

// stb_ds's stbds_hash_index and stbds_hash_bucket as 32-bit ARM lays them out (stb_ds.c's end checks this mirror).
typedef struct {
    size_t hash[8];
    ptrdiff_t index[8];
} FreewinsHashBucket;
typedef struct {
    char* temp_key;
    size_t slot_count, used_count, used_count_threshold, used_count_shrink_threshold, tombstone_count,
        tombstone_count_threshold, seed, slot_count_log2;
    struct { void* storage; size_t remaining; unsigned char block, mode; } string;
    FreewinsHashBucket* storage;
} FreewinsHashIndex;

// cleanupState, before its ds_map loop: a map parked at the last reset and never claimed since is freed now.
static inline __attribute__((always_inline)) void FreewinsMap_dropUnclaimed(void) {
    repeat(2, k) {
        if (freewinsKeptMaps[k].map != nullptr) shfree(freewinsKeptMaps[k].map);
    }
}

// cleanupState, after freeing the map's keys and values: park it, or free it as before.
static inline __attribute__((always_inline)) void FreewinsMap_park(DsMapEntry* map, int32_t id) {
    stbds_array_header* header = stbds_header(map - 1);
    if (header->length > FREEWINS_KEEP_MIN) {
        repeat(2, k) {
            FreewinsKeptMap* kept = &freewinsKeptMaps[k];
            if (kept->map != nullptr) continue;
            FreewinsHashIndex* table = (FreewinsHashIndex*) header->hash_table;
            // Each 64 B bucket (64-aligned): 8 hashes, then 8 indexes. Hashes STBDS_HASH_EMPTY (0), indexes
            // STBDS_INDEX_EMPTY (-1): a word's address bit 5 says which half it is in.
            uint32_t* word = (uint32_t*) table->storage;
            uint32_t* end = word + table->slot_count * 2;
            for (; word < end; word++) *word = ((uintptr_t) word & 32) ? ~0u : 0u;
            table->used_count = 0;
            table->tombstone_count = 0;
            header->length = 1;
            kept->id = id;
            kept->map = map;
            return;
        }
    }
    shfree(map);
}

// dsMapAddCommon: an id's map is nullptr until its first add; give it back its parked storage.
static inline __attribute__((always_inline)) void FreewinsMap_claim(DsMapEntry** mapPtr, int32_t id) {
    if (*mapPtr != nullptr) return;
    repeat(2, k) {
        FreewinsKeptMap* kept = &freewinsKeptMaps[k];
        if (kept->map != nullptr && kept->id == id) {
            *mapPtr = kept->map;
            kept->map = nullptr;
            return;
        }
    }
}
#else
#define FREEWINS_CLEANUP_PAD_ASM ".pushsection .text.cleanupState,\"ax\",%progbits\n.subsection 1\n.space 188\n.popsection\n"
#define FREEWINS_MAPADD_PAD "316"
#define FreewinsMap_dropUnclaimed() ((void) 0)
#define FreewinsMap_park(map, id) shfree(map)
#define FreewinsMap_claim(mapPtr, id) ((void) 0)
#endif
