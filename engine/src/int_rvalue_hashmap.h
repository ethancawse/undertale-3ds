#ifndef _BS_INT_RVALUE_HASHMAP_H_
#define _BS_INT_RVALUE_HASHMAP_H_

#include "common.h"
#include "utils.h"
#include "rvalue.h"
#include <stdint.h>
#include <stdbool.h>

// Open-addressing, linear-probing hashmap from int32_t to RValue.
//
// While stb_ds HashMaps are good, we use our own handrolled HashMap because it is FASTER on the PlayStation 2 (MIPS) target.
// We also implement functions tailored for Butterscotch, to avoid multiple calls to do a single thing (example: hmgeti -> hmput -> hmgeti).
// We also try packing the data more tightly to avoid cache misses, which are VERY expensive on the PlayStation 2 target.

#define INT_RVALUE_HASHMAP_EMPTY_KEY ((int32_t) -1)

typedef struct {
    int32_t key;
    RValue value;
} IntRValueEntry;

// All fields are public so iterators (debug overlay, JSON dump, Instance_copyFields, Instance_free) can scan the entries array directly. capacity is always 0 or a power of two; mask is capacity - 1.
typedef struct {
    IntRValueEntry* entries;
    uint32_t capacity;
    uint32_t mask;
    uint32_t count;
} IntRValueHashMap;

// RValue_free's every occupied entry, then releases the entries buffer.
void IntRValueHashMap_freeAllValues(IntRValueHashMap* map);


// Returns a pointer to the existing value slot for key. If the key is absent, inserts an RVALUE_UNDEFINED entry first and returns that slot.
// Replaces the hmgeti + hmput(UNDEFINED) + hmgeti pattern with a single lookup.
// Only an insert moves entries, so the pointer (and any other slot pointer) is valid until a key is added.
RValue* IntRValueHashMap_getOrInsertUndefined(IntRValueHashMap* map, int32_t key);

// Returns the number of occupied slots.
static inline uint32_t IntRValueHashMap_count(const IntRValueHashMap* map) {
    return map->count;
}

// Returns a pointer to the value slot for key, or nullptr if absent. The pointer is valid until the next mutation of the map.
// Inline because the interpreter calls it for nearly every instance and global variable access.
static inline RValue* IntRValueHashMap_findSlot(IntRValueHashMap* map, int32_t key) {
    if (map->capacity == 0) return nullptr;
    IntRValueEntry* entries = map->entries;
    uint32_t mask = map->mask;
    uint32_t idx = ((uint32_t) key * 0x9E3779B9u) & mask;
    while (true) {
        int32_t slotKey = entries[idx].key;
        if (slotKey == key) return &entries[idx].value;
        if (slotKey == INT_RVALUE_HASHMAP_EMPTY_KEY) return nullptr;
        idx = (idx + 1) & mask;
    }
}

// Fast inline get. Returns a non-owning (weak) view of the value, or RVALUE_UNDEFINED if absent. Caller must NOT RValue_free the result without first strengthening it (strdup / incRef).
static inline RValue IntRValueHashMap_get(const IntRValueHashMap* map, int32_t key) {
    if (map->capacity == 0) return RValue_makeUndefined();
    const IntRValueEntry* entries = map->entries;
    uint32_t mask = map->mask;
    uint32_t idx = ((uint32_t) key * 0x9E3779B9u) & mask;
    while (true) {
        int32_t slotKey = entries[idx].key;
        if (slotKey == key) {
            RValue result = entries[idx].value;
            result.ownsReference = false;
            return result;
        }
        if (slotKey == INT_RVALUE_HASHMAP_EMPTY_KEY) return RValue_makeUndefined();
        idx = (idx + 1) & mask;
    }
}

// Same result as IntRValueHashMap_getOrInsertUndefined, with its common case inline: the key is already present.
static inline RValue* IntRValueHashMap_getOrInsertUndefinedFast(IntRValueHashMap* map, int32_t key) {
    RValue* slot = IntRValueHashMap_findSlot(map, key);
    if (slot != nullptr) return slot;
    return IntRValueHashMap_getOrInsertUndefined(map, key);
}

// Returns true if the map contains the given key. Used for variable_instance_exists() and similar existence checks.
static inline bool IntRValueHashMap_contains(const IntRValueHashMap* map, int32_t key) {
    if (map->capacity == 0) return false;
    const IntRValueEntry* entries = map->entries;
    uint32_t mask = map->mask;
    uint32_t idx = ((uint32_t) key * 0x9E3779B9u) & mask;
    while (true) {
        int32_t slotKey = entries[idx].key;
        if (slotKey == key) return true;
        if (slotKey == INT_RVALUE_HASHMAP_EMPTY_KEY) return false;
        idx = (idx + 1) & mask;
    }
}

#endif /* _BS_INT_RVALUE_HASHMAP_H_ */
