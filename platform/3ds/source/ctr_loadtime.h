#pragma once
#include <stdio.h>
#include <sys/stat.h>

// Cold chapter-load status and diagnostic-hook presence; game files stay live.
void __attribute__((noinline, noipa, section(".text.late.loadtime"))) flushMemoryStatus(void);
void __attribute__((noinline, noipa, no_reorder, section(".text.late.loadtime"))) flushVersionStatus(void);
void __attribute__((noinline, noipa, no_reorder, section(".text.late.loadtime"))) flushParseStatus(void);
int __attribute__((noinline, noipa, no_reorder, section(".text.late.loadtime"))) bootHookStat(const char* path, struct stat* st);
FILE* __attribute__((noinline, noipa, no_reorder, section(".text.late.loadtime"))) openBootHook(const char* path, const char* mode);
