// Bench epoch rows-ram/v1. Only this translation unit's result files use RAM.
#undef fopen
#undef stat
#undef fputs
#undef fwrite
#undef fputc
#undef fprintf
#undef fclose

#define ROWS_TEXT __attribute__((noinline, noipa, section(".text.late.pacer_rows")))
#define ROWS_CONST __attribute__((section(".rodata.pacer_rows_in_text")))
#define ROWS_CHUNK 16384u
#define ROWS_TARGET (64u * 1024u)
typedef struct BenchRowChunk {
    struct BenchRowChunk* next;
    size_t used;
    char bytes[ROWS_CHUNK];
} BenchRowChunk;
typedef struct {
    char path[256];
    BenchRowChunk* first;
    BenchRowChunk* last;
    off_t length;
    bool exists, pending, truncate, failed;
} BenchRowFile;
typedef struct {
    BenchRowFile files[7];
    size_t queued;
    bool active, allocationFailed;
} BenchRowStore;
static BenchRowStore* benchRows __attribute__((section(".data.late.pacer_rows"))) = NULL;
static const char rowNames[] ROWS_CONST =
    "benchmark.txt\0sweep.csv\0sweep.pos\0tour.csv\0tour.pos\0battles.csv\0battles.pos\0";
static const char rowPath[] ROWS_CONST = "%s/%s";
static const char rowAppend[] ROWS_CONST = "ab";
static const char rowReplace[] ROWS_CONST = "wb";
static const char rowFlush[] ROWS_CONST = "Bench rows: rows-ram/v1 flushed %lu bytes at %s (outside compared segments)\n";
static const char rowError[] ROWS_CONST = "Bench rows: output failed for %s; result file incomplete\n";
static const char rowAllocation[] ROWS_CONST = "Bench rows: RAM allocation failed; result file incomplete\n";
static const char rowBegin[] ROWS_CONST = "Bench rows: epoch rows-ram/v1; result writes deferred to excluded boundaries/end/exit\n";
static const char rowEnd[] ROWS_CONST = "run end";
static const char rowExcluded[] ROWS_CONST = "capacity: existing excluded entry/setup";
static const char rowExit[] ROWS_CONST = "normal exit";
static const char rowSource[] ROWS_CONST = "./platform/3ds/source/ctr_benchmark_rows.inc";

#ifdef __3DS__
extern int benchRowsRealPuts(const char*, FILE*) __asm__("fputs");
extern size_t benchRowsRealWrite(const void*, size_t, size_t, FILE*) __asm__("fwrite");
#else
#define benchRowsRealPuts fputs
#define benchRowsRealWrite fwrite
#endif

static ROWS_TEXT BenchRowFile* benchRowsPath(const char* path) {
    if (benchRows == NULL || !benchRows->active) return NULL;
    for (unsigned i = 0; i < 7; i++)
        if (strcmp(path, benchRows->files[i].path) == 0) return &benchRows->files[i];
    return NULL;
}
static ROWS_TEXT BenchRowFile* benchRowsStream(FILE* stream) {
    if (benchRows == NULL) return NULL;
    for (unsigned i = 0; i < 7; i++)
        if (stream == (FILE*)&benchRows->files[i]) return &benchRows->files[i];
    return NULL;
}
static ROWS_TEXT void benchRowsDiscard(BenchRowFile* file) {
    while (file->first != NULL) {
        BenchRowChunk* chunk = file->first;
        file->first = chunk->next;
        benchRows->queued -= chunk->used;
        free(chunk);
    }
    file->last = NULL;
}
static ROWS_TEXT int benchRowsCopy(BenchRowFile* file, const char* bytes, size_t size) {
    if (file->failed) return EOF;
    while (size > 0) {
        if (file->last == NULL || file->last->used == ROWS_CHUNK) {
            BenchRowChunk* chunk = malloc(sizeof(*chunk));
            if (chunk == NULL) {
                file->failed = true;
                benchRows->allocationFailed = true;
                return EOF; // Never fall back to SD from a measured frame.
            }
            chunk->next = NULL;
            chunk->used = 0;
            if (file->last != NULL) file->last->next = chunk;
            else file->first = chunk;
            file->last = chunk;
        }
        size_t count = ROWS_CHUNK - file->last->used;
        if (count > size) count = size;
        memcpy(file->last->bytes + file->last->used, bytes, count);
        file->last->used += count;
        benchRows->queued += count;
        file->length += count;
        bytes += count;
        size -= count;
    }
    return 0;
}
static ROWS_TEXT FILE* benchRowsOpen(const char* path, const char* mode) {
    BenchRowFile* file = benchRowsPath(path);
    if (file == NULL || (mode[0] != 'a' && mode[0] != 'w')) return fopen(path, mode);
    if (file->failed) return NULL;
    if (mode[0] == 'w') {
        benchRowsDiscard(file);
        file->length = 0;
        file->truncate = true;
    }
    file->exists = file->pending = true;
    return (FILE*)file;
}
static ROWS_TEXT int benchRowsStat(const char* path, struct stat* result) {
    BenchRowFile* file = benchRowsPath(path);
    if (file == NULL) return stat(path, result);
    if (!file->exists) return -1;
    memset(result, 0, sizeof(*result));
    result->st_mode = S_IFREG;
    result->st_size = file->length;
    return 0;
}
static ROWS_TEXT __attribute__((used)) int benchRowsPuts(const char* text, FILE* stream) {
    BenchRowFile* file = benchRowsStream(stream);
    return file != NULL ? benchRowsCopy(file, text, strlen(text)) : benchRowsRealPuts(text, stream);
}
static ROWS_TEXT __attribute__((used)) size_t benchRowsWrite(const void* data, size_t size, size_t count, FILE* stream) {
    BenchRowFile* file = benchRowsStream(stream);
    if (file == NULL) return benchRowsRealWrite(data, size, count, stream);
    if (size == 0 || count == 0) return 0;
    if (count > SIZE_MAX / size) { file->failed = true; return 0; }
    return benchRowsCopy(file, data, size * count) == 0 ? count : 0;
}
static ROWS_TEXT int benchRowsPutc(int value, FILE* stream) {
    BenchRowFile* file = benchRowsStream(stream);
    if (file == NULL) return fputc(value, stream);
    char byte = (char)value;
    return benchRowsCopy(file, &byte, 1) == 0 ? (unsigned char)byte : EOF;
}
static ROWS_TEXT int benchRowsPrintf(FILE* stream, const char* format, ...) {
    va_list args;
    va_start(args, format);
    BenchRowFile* file = benchRowsStream(stream);
    int result;
    if (file == NULL) {
#ifdef __3DS__
        result = _vfprintf_r(__getreent(), stream, format, args);
#else
        result = vfprintf(stream, format, args);
#endif
    } else {
        char text[512]; // Only the small position records use fprintf.
        va_list copy;
        va_copy(copy, args);
        result = vsnprintf(text, sizeof(text), format, copy);
        va_end(copy);
        if (result >= 0 && (size_t)result < sizeof(text)) {
            if (benchRowsCopy(file, text, (size_t)result) != 0) result = -1;
        } else if (result >= 0) {
            char* large = malloc((size_t)result + 1);
            if (large != NULL) {
                vsnprintf(large, (size_t)result + 1, format, args);
                if (benchRowsCopy(file, large, (size_t)result) != 0) result = -1;
                free(large);
            } else {
                file->failed = benchRows->allocationFailed = true;
                result = -1;
            }
        }
    }
    va_end(args);
    return result;
}
static ROWS_TEXT int benchRowsClose(FILE* stream) {
    BenchRowFile* file = benchRowsStream(stream);
    return file != NULL ? (file->failed ? EOF : 0) : fclose(stream);
}
static ROWS_TEXT bool benchRowsFlush(const char* reason) {
    if (benchRows == NULL) return true;
    size_t bytes = benchRows->queued;
    bool okay = !benchRows->allocationFailed;
    if (!okay) logWarn(rowAllocation);
    for (unsigned i = 0; i < 7; i++) {
        BenchRowFile* file = &benchRows->files[i];
        if (!file->pending) continue;
        // A partial append is not retried: that would duplicate its prefix.
        FILE* output = file->failed ? NULL : fopen(file->path, file->truncate ? rowReplace : rowAppend);
        bool written = output != NULL;
        for (BenchRowChunk* chunk = file->first; written && chunk != NULL; chunk = chunk->next)
            written = benchRowsRealWrite(chunk->bytes, 1, chunk->used, output) == chunk->used;
        if (output != NULL && fclose(output) != 0) written = false;
        if (!written) { logWarn(rowError, file->path); file->failed = true; okay = false; }
        benchRowsDiscard(file);
        file->pending = file->truncate = false;
    }
    if (bytes != 0) logInfo(rowFlush, (unsigned long)bytes, reason);
    return okay;
}

static ROWS_TEXT void benchRowsBegin(void) {
    if (benchRows == NULL) {
#ifdef __3DS__
        benchRows = safeCallocFunction(1, sizeof(*benchRows), rowSource, 1);
#else
        benchRows = safeCalloc(1, sizeof(*benchRows));
        (void)rowSource;
#endif
    }
    // A second run is a new file snapshot; no SD stat is permitted at a cut.
    benchRowsFlush(rowEnd);
    memset(benchRows, 0, sizeof(*benchRows));
    const char* name = rowNames;
    for (unsigned i = 0; i < 7; i++, name += strlen(name) + 1) {
        BenchRowFile* file = &benchRows->files[i];
        snprintf(file->path, sizeof(file->path), rowPath, bench.session.gameDir, name);
        struct stat before;
        file->exists = stat(file->path, &before) == 0;
        if (file->exists) file->length = before.st_size;
    }
    benchRows->active = true;
    logInfo(rowBegin);
    beginTest();
}
static ROWS_TEXT void benchRowsSave(void) {
    saveResults();
    if (!benchRowsFlush(rowEnd)) bench.saved = false;
    if (benchRows != NULL) benchRows->active = false;
}
static ROWS_TEXT BenchScriptState benchRowsTick(BenchScript* script, Runner* runner) {
    if (benchRows != NULL && benchRows->queued >= ROWS_TARGET &&
        (bench.phase != PHASE_MEASURE || (sweep.on && sweep.segment == SEG_ENTER))) {
        // A full chunk during an attack spills to RAM. Only an already
        // excluded entry/setup may drain it; do not exclude another frame.
        benchRowsFlush(rowExcluded);
    }
    return benchRoomsTick(script, runner);
}
void __real_CtrExit_begin(int seconds);
void __wrap_CtrExit_begin(int seconds);
ROWS_TEXT void __wrap_CtrExit_begin(int seconds) {
    __real_CtrExit_begin(seconds); // Arm the existing exit deadline first.
    benchRowsFlush(rowExit);
}

// Keep the existing writer body and slot. Cross-TU helpers receive these same
// fake FILE handles, so expose its dispatcher without another formatting path.
__asm__(".globl CtrBench_resultPuts\n.set CtrBench_resultPuts, benchRowsPuts\n");
