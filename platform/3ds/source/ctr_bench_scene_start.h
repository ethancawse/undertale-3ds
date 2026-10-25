#pragma once

// Each bench scene starts from a clean sandbox. SandboxFileSystem holds what the game and the scene scripts write
// while the test runs, and it lasted until the next game load (main.c makes one per load). Each DELTARUNE scene loads
// its chapter, but UNDERTALE's share one load: after flowey wrote undertale.ini with [FFFFF] F=1, later file0-only
// scenes booted to the wrong room. Now each scene sees the files on the card (the bundle the sandbox reads through to)
// plus what it writes itself. The clear happens once, as the scene starts and before its script runs, never mid-scene.
//
// Late code (platform/3ds/late_reserve.ld, .text.late.sandbox): startNextScene calls CtrBench_lateSceneStart
// instead of logInfo, with the same arguments, so ctr_benchmark.c's existing code keeps its size and address.

#include <stddef.h>
#include <stdint.h>

#include "file_system.h"

// logInfo for the "Bench: scene" line; then, if earlier scenes left files in the sandbox, logs them and clears it.
void CtrBench_lateSceneStart(const char* format, ...) __attribute__((format(printf, 1, 2)));
// The bench's sandbox (ctr_benchmark.c).
FileSystem* CtrBench_lateSandbox(void);
// Writes "path (N B)" for each file the sandbox holds and "path (deleted)" for each bundle file it hides into out,
// comma-separated and cut short with "..." if out is too small. Returns how many there are, or -1 while the sandbox
// is off (engine/src/sandbox_file_system.c).
int32_t SandboxFileSystem_lateDescribe(FileSystem* fs, char* out, size_t size);
