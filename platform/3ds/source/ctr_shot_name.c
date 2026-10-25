#include "ctr_shot_name.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "common.h"

// minigames. Late code only (platform/3ds/late_reserve.ld): nothing here moves existing code or data.
__attribute__((noinline, section(".text.late.minigames")))
int CtrShotName_format(char* out, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int n = vsnprintf(out, size, format, args);
    va_end(args);
    const char* scene = CtrBench_lateSceneId();
    size_t sceneLength = scene != nullptr ? strlen(scene) : 0;
    // The scene goes just before the ".bmp". A name it would cut short keeps the plain form.
    if (sceneLength == 0 || n < 4 || (size_t) n + 1 + sceneLength >= size) return n;
    char* extension = out + n - 4;
    memmove(extension + 1 + sceneLength, extension, 5); // ".bmp" and its NUL
    extension[0] = '_';
    memcpy(extension + 1, scene, sceneLength);
    return n + 1 + (int) sceneLength;
}
