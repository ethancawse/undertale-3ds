#include "ctr_bench_scene_start.h"
#include <stdarg.h>
#include <stdio.h>

#include "common.h"
#include "log.h"
#include "sandbox_file_system.h"

// Late code and constants only (platform/3ds/late_reserve.ld, .text.late.sandbox and .rodata.late.sandbox): nothing
// here moves existing code or data. A string literal would join .rodata and move what follows it, so the strings
// are late constants. Only functions the build already keeps are called: one the linker had dropped as unused
// (vLogInfo, SandboxFileSystem_isEnabled) would come back at its old place and move the code after it.
__attribute__((section(".rodata.late.sandbox"))) static const char lineFormat[] = "%s";
__attribute__((section(".rodata.late.sandbox")))
static const char clearedFormat[] = "Bench: sandbox cleared, %ld file(s) from earlier scenes: %s\n";

__attribute__((noinline, section(".text.late.sandbox")))
void CtrBench_lateSceneStart(const char* format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    logInfo(lineFormat, line);
    FileSystem* sandbox = CtrBench_lateSandbox();
    if (sandbox == nullptr) return;
    char held[384];
    int32_t count = SandboxFileSystem_lateDescribe(sandbox, held, sizeof(held));
    if (count <= 0) return;
    // Disabling drops every file the sandbox holds; enabling again starts it empty, reading through to the bundle.
    SandboxFileSystem_setEnabled(sandbox, false);
    SandboxFileSystem_setEnabled(sandbox, true);
    logInfo(clearedFormat, (long) count, held);
}
