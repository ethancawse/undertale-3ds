// Shared ordinary/CIA80 startup allocator; original entry retains its bytes.
    Handle reslimit = 0;
    if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE))) svcBreak(USERBREAK_PANIC);
    ResourceLimitType type = RESLIMIT_COMMIT;
    s64 maxCommit = 0;
    s64 currentCommit = 0;
    svcGetResourceLimitLimitValues(&maxCommit, reslimit, &type, 1);
    svcGetResourceLimitCurrentValues(&currentCommit, reslimit, &type, 1);
    svcCloseHandle(reslimit);
    u32 remaining = (u32) (maxCommit - currentCommit) & ~0xFFFu;
#ifdef CTRPORT_MEMORY_CAP_KB
    // A test build that gets what a console grants (CTRPORT_MEMORY_CAP_KB) from an emulator that grants more, so the
    // heap and linear memory are the console's size and split.
    u32 beyondCap = 0;
    if (remaining > ((u32) CTRPORT_MEMORY_CAP_KB * 1024u & ~0xFFFu)) {
        beyondCap = remaining - ((u32) CTRPORT_MEMORY_CAP_KB * 1024u & ~0xFFFu);
        remaining -= beyondCap;
    }
#endif
#ifdef CTRPORT_AOT_MODULES
    u32 region = remaining > CTR_AOT_MODULE_REGION_SIZE + 48 * MB ? CTR_AOT_MODULE_REGION_SIZE : 0;
    remaining -= region;
#endif

    u32 linear = (remaining / 2) & ~0xFFFu;
    if (LARGE_MEMORY > remaining && remaining - linear > DEFAULT_HEAP_MIN_MB * MB) {
        // libctru's default (24 / 32 MB; UNDERTALE Old 20 / 38): the heap at least DEFAULT_HEAP_MIN_MB, linear at most.
        linear = remaining - DEFAULT_HEAP_MIN_MB * MB;
        if (linear > DEFAULT_LINEAR_MAX_MB * MB) linear = DEFAULT_LINEAR_MAX_MB * MB;
    }
#ifdef FIXED_HEAP_SIZE
    if (remaining > FIXED_HEAP_SIZE + 32 * MB) linear = remaining - FIXED_HEAP_SIZE;
#endif
#ifdef TARGET_HEAP_SIZE
    if (remaining > TARGET_HEAP_SIZE && remaining - TARGET_HEAP_SIZE > linear) linear = (remaining - TARGET_HEAP_SIZE) & ~0xFFFu;
#endif
#ifdef CTRPORT_ALLOCATE_CIA80
    // Only the DR Old CIA80 entry includes this hook. The commit limit is the
    // application's grant before its load image/AOT region, so image growth does
    // not accidentally select a different memory mode. Old 64/72 MB and New
    // grants are outside this range. Leave at least the small-layout 24 MiB heap.
    if (maxCommit >= 76 * (s64) MB && maxCommit <= 80 * (s64) MB && remaining - linear >= 32 * MB) {
        linear += 8 * MB;
    }
#endif
    __ctru_heap_size = remaining - linear;
#if defined(CTRPORT_MEMORY_CAP_KB) && defined(CTRPORT_HEAP_PROFILE)
    // The heap profiler's tables, in linear memory, come on top of the console's.
    if (beyondCap >= CTR_HEAP_PROFILE_TABLE_BYTES) linear += CTR_HEAP_PROFILE_TABLE_BYTES;
#endif
    __ctru_linear_heap_size = linear;

    u32 heapAt = OS_HEAP_AREA_BEGIN;
#ifdef CTRPORT_AOT_MODULES
    u32 regionAt = 0;
    if (region > 0 && R_SUCCEEDED(svcControlMemory(&regionAt, CTR_AOT_MODULE_BASE, 0, region, MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE))) {
        if (regionAt == CTR_AOT_MODULE_BASE) {
            aotRegionSize = region;
            heapAt = CTR_AOT_MODULE_BASE + region;
        } else {
            // Somewhere the modules don't run: give it back, and the heap gets it.
            svcControlMemory(&regionAt, regionAt, 0, region, MEMOP_FREE, 0);
            __ctru_heap_size += region;
        }
    } else {
        __ctru_heap_size += region;
    }
#endif
    if (R_FAILED(svcControlMemory(&__ctru_heap, heapAt, 0, __ctru_heap_size, MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE))) {
        svcBreak(USERBREAK_PANIC);
    }
    if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0, 0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR, MEMPERM_READ | MEMPERM_WRITE))) {
        svcBreak(USERBREAK_PANIC);
    }
    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
    // Before anything allocates linear memory.
    LightLock_Init(&linearLock);

    fake_heap_start = (char*) __ctru_heap;
    fake_heap_end = fake_heap_start + __ctru_heap_size;
