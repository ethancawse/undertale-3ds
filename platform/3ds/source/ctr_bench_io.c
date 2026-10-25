// Timing rows stream their existing log records to the host. No extra row buffer.
// Keep the benchmark and logger's old instructions/slots; only stdio call targets
// change. Coverage keeps persistence; mode 2 is a CSV-only diagnostic control.
#ifndef CTRPORT_PACER_CSV_DIAG
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "ctr_launch_args.h"
#include "log.h"

#define IO_CODE __attribute__((noinline, section(".text.late.pacer_io")))
#define IO_RO __attribute__((section(".rodata.pacer_io_in_text"), aligned(4)))
// Also read by queue_lint: old builds must not silently accept this policy key.
static const char ioKey[] IO_RO = "bench_io=";
static const char ioRoot[] IO_RO = "sdmc:/3ds/";
static const char ioGames[] IO_RO = "undertale-3ds/\0deltarune-3ds/";
static const char ioLog[] IO_RO = "log.txt";
static const char ioSweep[] IO_RO = "sweep.csv";
static const char ioFiles[] IO_RO = "benchmark.txt\0sweep.csv\0sweep.pos\0tour.csv\0tour.pos\0battles.csv\0battles.pos\0";
static const char ioShots[] IO_RO = "sweep_shots";
static const char ioCache[] IO_RO = "cache";
static const char ioHint[] IO_RO = "/cache/rooms.dtm.tmp";
static const char ioNoShots[] IO_RO = " noshots";
static const char ioTiming[] IO_RO = "Bench I/O: timing log-only (bench_io=0); no SD benchmark output\n";
static const char ioCoverage[] IO_RO = "Bench I/O: coverage (bench_io=1); SD output; NOT release timing\n";
static const char ioControl[] IO_RO = "Bench I/O: debug CSV control (bench_io=2); CSV only; NOT release timing\n";
static const char ioInvalid[] IO_RO = "Bench I/O: invalid bench_io; using timing log-only\n";
// Consume zero-initialized data reserve, preserving BSS-end/startup/heap literals.
static struct { unsigned mode; bool active; unsigned char sink; } io
    __attribute__((section(".data.late.pacer_io"))) = {0};

CtrLaunchArgs* CtrBenchIo_launch(void);
void __real_CtrExitFiles_openLog(const char*);
int __real_stat(const char*, struct stat*);
FILE* __real_fopen(const char*, const char*);
int __real_fputs(const char*, FILE*);
int __real_fputc(int, FILE*);
int __real_fclose(FILE*);
int __real_fflush(FILE*);
int __real_fprintf(FILE*, const char*, ...);
size_t __real_fwrite(const void*, size_t, size_t, FILE*);
int __real_mkdir(const char*, mode_t);
// Variadic forwarding uses vfprintf, so explicitly retain the old fprintf body
// as well. Its original address/bytes are covered by the old-byte gate.
static int (*const ioKeepFprintf)(FILE*, const char*, ...) IO_RO __attribute__((used)) = __real_fprintf;

static IO_CODE const char* outputName(const char* path) {
    if (!io.active || io.mode == 1 || strncmp(path, ioRoot, sizeof(ioRoot)-1) != 0) return NULL;
    const char* game = path + sizeof(ioRoot)-1;
    if (strncmp(game, ioGames, 14) != 0 && strncmp(game, ioGames+15, 14) != 0) return NULL;
    const char* name = strrchr(path, '/');
    return name != NULL ? name+1 : NULL;
}

static IO_CODE bool discard(const char* path) {
    const char* name = outputName(path);
    if (name == NULL) return false;
    // Only the game-root harness outputs, never a same-named game save below it.
    if (name != path + sizeof(ioRoot)-1 + 14) return false;
    if (strcmp(name, ioLog) == 0) return true;
    if (io.mode == 2 && strcmp(name, ioSweep) == 0) return false;
    for (const char* p = ioFiles; *p != '\0'; p += strlen(p)+1)
        if (strcmp(name, p) == 0) return true;
    return false;
}

// Before opening log.txt or starting its writer, after launch argument parsing.
IO_CODE void __wrap_CtrExitFiles_openLog(const char* path) {
    CtrLaunchArgs* args = CtrBenchIo_launch();
    io.active = args->bench[0] != '\0';
    bool invalid = false;
    int kept = 0;
    for (int i = 0; i < args->iniCount; i++) {
        if (strncmp(args->ini[i], ioKey, sizeof(ioKey)-1) == 0) {
            const char* value = args->ini[i] + sizeof(ioKey)-1;
            io.active = true;
            if (value[0] >= '0' && value[0] <= '2' && value[1] == '\0') io.mode = (unsigned)(value[0]-'0');
            else { io.mode = 0; invalid = true; }
        } else {
            if (kept != i) memcpy(args->ini[kept], args->ini[i], sizeof(args->ini[i]));
            kept++;
        }
    }
    args->iniCount = kept;
    if (io.active && io.mode != 1) {
        // Timing cannot include screenshot/state-trace writes. Lint rejects these
        // requests; the runtime also enforces the policy for standalone launches.
        args->shots = false;
        args->autoshotCount = 0;
        args->stateHash = false;
        size_t n = strlen(args->bench);
        // A constant memcpy becomes a mergeable pooled string. Even in late
        // text, its NUL suffix can absorb libctru's old empty string and shift
        // everything after that slot. Read this explicit late array directly.
        if (n + sizeof(ioNoShots) <= sizeof(args->bench))
            for (size_t i = 0; i < sizeof(ioNoShots); i++)
                args->bench[n+i] = ((const volatile char*)ioNoShots)[i];
    }
    __real_CtrExitFiles_openLog(path);
    if (io.active) {
        if (invalid) logWarn(ioInvalid);
        logInfo(io.mode == 1 ? ioCoverage : io.mode == 2 ? ioControl : ioTiming);
    }
}

IO_CODE int __wrap_stat(const char* path, struct stat* st) {
    if (!discard(path)) return __real_stat(path, st);
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG;
    st->st_size = 1;
    return 0;
}

IO_CODE FILE* __wrap_fopen(const char* path, const char* mode) {
    // The existing room-plan wrapper/reader retains ownership of its payload.
    // Its background writer stops on NULL before writes/rename/remove. The
    // learned hint is optional and is never game state or an in-RAM row queue.
    const char* output = outputName(path);
    size_t pathLength = output != NULL ? strlen(path) : 0;
    if ((mode[0] == 'w' || mode[0] == 'a') && output != NULL && pathLength >= sizeof(ioHint)-1 &&
        strcmp(path + pathLength - (sizeof(ioHint)-1), ioHint) == 0) return NULL;
    if ((mode[0] == 'w' || mode[0] == 'a') && discard(path)) {
        // A NULL logFile makes the existing writer stream only to screen/link/
        // emulator debug output. Other benchmark writers use the discard token.
        const char* name = strrchr(path, '/');
        if (name != NULL && strcmp(name+1, ioLog) == 0) return NULL;
        return (FILE*)&io.sink;
    }
    return __real_fopen(path, mode);
}

IO_CODE int __wrap_fputs(const char* text, FILE* stream) {
    return stream == (FILE*)&io.sink ? 0 : __real_fputs(text, stream);
}
IO_CODE int __wrap_fputc(int c, FILE* stream) {
    return stream == (FILE*)&io.sink ? (unsigned char)c : __real_fputc(c, stream);
}
IO_CODE int __wrap_fclose(FILE* stream) {
    return stream == (FILE*)&io.sink ? 0 : __real_fclose(stream);
}
IO_CODE int __wrap_fflush(FILE* stream) {
    return stream == (FILE*)&io.sink ? 0 : __real_fflush(stream);
}
IO_CODE int __wrap_fprintf(FILE* stream, const char* format, ...) {
    if (stream == (FILE*)&io.sink) return 0;
    va_list args;
    va_start(args, format);
    // Newlib's public vfprintf entry was not linked in the accepted builds.
    // Use exactly its thread-local forwarding path without adding a normal-
    // text entry that would move every following libc/retained-slot symbol.
#ifdef __3DS__
    int result = _vfprintf_r(__getreent(), stream, format, args);
#else
    int result = vfprintf(stream, format, args);
#endif
    va_end(args);
    return result;
}
IO_CODE size_t __wrap_fwrite(const void* data, size_t size, size_t count, FILE* stream) {
    return stream == (FILE*)&io.sink ? (size != 0 ? count : 0) : __real_fwrite(data, size, count, stream);
}
IO_CODE int __wrap_mkdir(const char* path, mode_t mode) {
    const char* name = outputName(path);
    return name != NULL && (strcmp(name, ioShots) == 0 || strcmp(name, ioCache) == 0) ? 0 : __real_mkdir(path, mode);
}
#endif
