#pragma once

#include <3ds.h>

// Game-thread users share one cached mount attempt for the lifetime of the app; neither owns an unmount.
Result CtrRomfs_init(const char* name);
// Caller owns the NUL-terminated text. Failure is logged and returns NULL (no embedded fallback).
char* CtrBench_readRomfs(void);

// freebytes H1 (change freebytes, 2026-10-31): on UNDERTALE Old 3DS only, late code that never runs in a played frame is
// compiled for size (CTR_COLD_OS, as ctr_display.h's), and whole cold functions of the loading screen move into the
// holes that leaves (CTR_H1_SEC(own section, hole section); late_reserve.ld places each hole's section). Every other
// target keeps each function's own section and compiles it as before. At the header's end, so no line number moves.
#ifndef CTR_COLD_OS
#if defined(ENABLE_WAD16) && defined(CTRPORT_TARGET_O3DS)
#define CTR_COLD_OS __attribute__((optimize("Os")))
#else
#define CTR_COLD_OS
#endif
#endif
#ifndef CTR_H1_SEC
#if defined(ENABLE_WAD16) && defined(CTRPORT_TARGET_O3DS)
#define CTR_H1_SEC(own, hole) hole
#else
#define CTR_H1_SEC(own, hole) own
#endif
#endif
// z_loadscreen.c's LS_CODE for a function that fills the H1 hole named by the string literal `hole`.
#ifndef CTR_H1_LS
#define CTR_H1_LS(hole) __attribute__((noinline, section(CTR_H1_SEC(".text.late.loadscreen", ".text.h1hole." hole))))
#endif
