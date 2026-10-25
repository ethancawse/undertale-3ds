// Reach-mode state hash. Included by state_hash.c in gate builds only (BS_REACH_GATE), after
// its helpers; the literal hash (hashRunner) and its output are untouched.
//
// Differences from the literal hash:
// - Structs are hashed sorted by instance ID, not in registry order (registry order is not GML-visible, and a sweep's
//   swap-removes reorder it differently once a temporary is missing).
// - The structs the typed run reports as replaced are left out, and only those (A3): in the base run, the IDs listed
//   in the oracle; in the typed run they don't exist. Every other struct, alive or dead, is hashed.
// - Each struct also hashes its constructor, its pinned flag, and its members in slot order with their gmlStackTypes,
//   which is what reflection (struct_get_names) and later conversions see (A5).
// - nextInstanceId is hashed, so a reserved ID that drifts shows on the frame it drifts.
// - The frame's observer-builtin counts and gate failures are mixed in (reach_gate.c).
// - A method value bound to a replaced ID, or a reference to a replaced struct, anywhere in the hashed state fails the
//   gate (A2): reading a method member of a replaced struct as a value is an escape.

bool ReachGate_reachMode(void);
bool ReachGate_isReplaced(int32_t id);
int32_t ReachGate_replacedCount(void);
void ReachGate_beginSession(const char* stateHashPath);
void ReachGate_beforeHash(Runner* runner);
uint64_t ReachGate_frameMix(uint64_t h, uint64_t (*mix)(uint64_t, uint64_t));
void ReachGate_failExternal(const char* message);
void ReachGate_endFrame(Runner* runner);
void ReachGate_endSession(void);

static uint64_t reachMix(uint64_t h, uint64_t v) { return mix64(h, v); }

static uint64_t hashStructOrdered(uint64_t h, Instance* s) {
    IntRValueHashMap* map = &s->selfVars;
    h = mix64(mix64(h, map->count), map->capacity);
    if (map->entries != nullptr) {
        for (uint32_t i = 0; map->capacity > i; i++) {
            IntRValueEntry* e = &map->entries[i];
            if (e->key == INT_RVALUE_HASHMAP_EMPTY_KEY) continue;
            h = mix64(h, (uint32_t) e->key);
            h = hashRValue(h, e->value, 0);
            h = mix64(h, e->value.gmlStackType);
        }
    }
    return h;
}

static int compareStructIds(const void* a, const void* b) {
    uint32_t x = (*(Instance* const*) a)->instanceId, y = (*(Instance* const*) b)->instanceId;
    return x < y ? -1 : (x > y ? 1 : 0);
}

// ---[ A2: escapes into the hashed state ]---

static const char* g_scanWhere = "?";
static int32_t g_scanOwner = 0;

static void scanValue(RValue v, int depth);

static void scanArray(GMLArray* arr, int depth) {
    if (arr == nullptr || depth > 16) return;
    if (arr->type == GML_LEGACY_ARRAY) {
        for (int32_t r = 0; arr->legacy.rowCount > r; r++) {
            GMLArrayRow* row = &arr->legacy.rows[r];
            for (int32_t c = 0; row->length > c; c++) scanValue(row->data[c], depth + 1);
        }
    } else {
        for (int32_t i = 0; arr->modern.length > i; i++) scanValue(arr->modern.data[i], depth + 1);
    }
}

static void scanValue(RValue v, int depth) {
    char msg[256];
    if (v.type == RVALUE_STRUCT && v.structInst != nullptr && ReachGate_isReplaced((int32_t) v.structInst->instanceId)) {
        snprintf(msg, sizeof(msg), "A2: a reference to replaced struct %u is in the hashed state (%s %d)",
                 v.structInst->instanceId, g_scanWhere, g_scanOwner);
        ReachGate_failExternal(msg);
    }
#if IS_WAD17_OR_HIGHER_ENABLED
    else if (v.type == RVALUE_METHOD && v.method != nullptr && v.method->boundInstanceId > 0 &&
             ReachGate_isReplaced(v.method->boundInstanceId)) {
        snprintf(msg, sizeof(msg), "A2: a method bound to replaced struct %d escaped into the hashed state (%s %d)",
                 v.method->boundInstanceId, g_scanWhere, g_scanOwner);
        ReachGate_failExternal(msg);
    }
#endif
    else if (v.type == RVALUE_ARRAY) {
        scanArray(v.array, depth);
    }
}

static void scanVarMap(IntRValueHashMap* map, const char* where, int32_t owner) {
    if (map->entries == nullptr) return;
    g_scanWhere = where;
    g_scanOwner = owner;
    for (uint32_t i = 0; map->capacity > i; i++) {
        if (map->entries[i].key != INT_RVALUE_HASHMAP_EMPTY_KEY) scanValue(map->entries[i].value, 0);
    }
}

static void scanEscapes(Runner* runner) {
    for (int32_t i = 0; arrlen(runner->instances) > i; i++) scanVarMap(&runner->instances[i]->selfVars, "instance", (int32_t) runner->instances[i]->instanceId);
    if (runner->vmContext->globalScopeInstance != nullptr) scanVarMap(&runner->vmContext->globalScopeInstance->selfVars, "global", 0);
    for (int32_t i = 0; arrlen(runner->structInstances) > i; i++) {
        Instance* s = runner->structInstances[i];
        if (!ReachGate_isReplaced((int32_t) s->instanceId)) scanVarMap(&s->selfVars, "struct", (int32_t) s->instanceId);
    }
    g_scanWhere = "ds_map";
    for (int32_t i = 0; arrlen(runner->dsMapPool) > i; i++) {
        DsMapEntry* map = runner->dsMapPool[i];
        g_scanOwner = i;
        for (int32_t j = 0; shlen(map) > j; j++) scanValue(map[j].value, 0);
    }
    g_scanWhere = "ds_list";
    for (int32_t i = 0; arrlen(runner->dsListPool) > i; i++) {
        g_scanOwner = i;
        for (int32_t j = 0; arrlen(runner->dsListPool[i].items) > j; j++) scanValue(runner->dsListPool[i].items[j], 0);
    }
    g_scanWhere = "ds_grid";
    for (int32_t i = 0; arrlen(runner->dsGridPool) > i; i++) {
        DsGrid* g = &runner->dsGridPool[i];
        if (g->freed || g->items == nullptr) continue;
        g_scanOwner = i;
        for (int32_t j = 0; g->width * g->height > j; j++) scanValue(g->items[j], 0);
    }
    g_scanWhere = "call_later";
    for (int32_t i = 0; arrlen(runner->callLaterEntries) > i; i++) {
        g_scanOwner = i;
        scanValue(runner->callLaterEntries[i].callback, 0);
    }
}

static uint64_t hashRunnerReach(Runner* runner) {
    ReachGate_beforeHash(runner);
    uint64_t h = 0x5EAC4ull;
    h = mix64(h, (uint32_t) runner->frameCount);
    h = mix64(h, (uint32_t) runner->currentRoomIndex);
    h = hashBytes(h, runner->random.state, sizeof(runner->random.state));
    h = mix64(h, runner->random.index);

    int32_t n = (int32_t) arrlen(runner->instances);
    h = mix64(h, (uint32_t) n);
    for (int32_t i = 0; n > i; i++) h = hashInstance(h, runner->instances[i]);

    if (runner->vmContext->globalScopeInstance != nullptr) h = hashVarMap(h, &runner->vmContext->globalScopeInstance->selfVars);

    static Instance** sorted = nullptr;
    arrsetlen(sorted, 0);
    for (int32_t i = 0; arrlen(runner->structInstances) > i; i++) {
        Instance* s = runner->structInstances[i];
        if (!ReachGate_isReplaced((int32_t) s->instanceId)) arrput(sorted, s);
    }
    int32_t structCount = (int32_t) arrlen(sorted);
    if (structCount > 1) qsort(sorted, (size_t) structCount, sizeof(Instance*), compareStructIds);
    h = mix64(h, (uint32_t) structCount);
    for (int32_t i = 0; structCount > i; i++) {
        Instance* s = sorted[i];
        h = mix64(h, s->instanceId);
        h = mix64(h, (uint32_t) s->constructorCodeIndex);
        h = mix64(h, (uint64_t) s->pinned);
        h = hashStructOrdered(h, s);
    }
    h = mix64(h, runner->nextInstanceId);

    bool fullMaps = (runner->frameCount % 16) == 0;
    int32_t mapCount = (int32_t) arrlen(runner->dsMapPool);
    h = mix64(h, (uint32_t) mapCount);
    for (int32_t i = 0; mapCount > i; i++) {
        DsMapEntry* map = runner->dsMapPool[i];
        uint64_t sum = 0;
        int32_t len = (int32_t) shlen(map);
        if (fullMaps) {
            for (int32_t j = 0; len > j; j++) sum += hashRValue(hashString(0x3A9ull, map[j].key), map[j].value, 0);
        }
        h = mix64(mix64(h, (uint32_t) len), sum);
    }
    int32_t listCount = (int32_t) arrlen(runner->dsListPool);
    h = mix64(h, (uint32_t) listCount);
    for (int32_t i = 0; listCount > i; i++) {
        DsList* list = &runner->dsListPool[i];
        int32_t len = (int32_t) arrlen(list->items);
        h = mix64(h, (uint32_t) len | ((uint64_t) list->freed << 32));
        for (int32_t j = 0; len > j; j++) h = hashRValue(h, list->items[j], 0);
    }

    if (ReachGate_replacedCount() != 0) scanEscapes(runner);
    return ReachGate_frameMix(h, reachMix);
}
