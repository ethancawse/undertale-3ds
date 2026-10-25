#pragma once

#include <stdint.h>
#include <stdio.h>

// Opt-in, single game-thread diagnostics. The clock must be monotonic, independent
// of the game's virtual clock. No file operations are logged while timing them.
void LoadTrace_begin(const char* scope, uint64_t (*clock)(void), double ticksPerSecond);
void LoadTrace_phase(const char* phase);
uint64_t LoadTrace_scriptBegin(int32_t codeId);
void LoadTrace_scriptEnd(uint64_t start);
// flush may block: it runs only after the measured interval has ended.
void LoadTrace_end(void (*flush)(void));
size_t LoadTrace_read(void* destination, size_t size, size_t count, FILE* file);
int LoadTrace_seek(FILE* file, long offset, int origin);

// Individual script names are diagnostic output on 3DS: a chapter can enqueue
// more than the link ring holds before its low-priority sender gets CPU time.
#if defined(PLATFORM_3DS)
void __attribute__((noinline, noipa, no_reorder, section(".text.late.loadtime")))
LoadTrace_logGlobalInit(const char* format, ...);
#else
#define LoadTrace_logGlobalInit logInfo
#endif
