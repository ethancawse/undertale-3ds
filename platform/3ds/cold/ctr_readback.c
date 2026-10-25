#include "ctr_readback.h"

#include <stdint.h>
#include <stdlib.h>

void* CtrReadback_reserve(void* pixels, size_t bytes) {
    size_t* header = pixels == NULL ? NULL : (size_t*) pixels - 1;
    if (header != NULL && *header >= bytes) return pixels;
    if (bytes > SIZE_MAX - sizeof(size_t)) return NULL;
    size_t* grown = realloc(header, sizeof(size_t) + bytes);
    if (grown == NULL) return NULL;
    *grown = bytes;
    return grown + 1;
}

void CtrReadback_free(void* pixels) {
    if (pixels != NULL) free((size_t*) pixels - 1);
}

#ifdef __3DS__
#include <3ds.h>
#include "log.h"

static const char tpaggrowMessage0[] TPAGGROW_READBACK_RODATA = "Renderer: native-scale surface %d (%dx%d) read back at half detail\n";
static const char tpaggrowMessage1[] TPAGGROW_READBACK_RODATA = "Renderer: read surface %d (%dx%d) at %p back in %.1f ms, alpha>0 %zu\n";
const char CtrReadback_slotLimitMessage[] TPAGGROW_READBACK_RODATA =
    "Renderer: sprite_create_from_surface exceeds 32767 encodable surface-sprite slots\n";

void CtrReadback_logPixels(int32_t surfaceId, int width, int height, const void* data,
                           uint64_t start, bool nativeScale, const uint8_t* rgba, size_t bytes) {
    // Each texel repeats over its 2x2 block (see "Native-scale surfaces").
    if (nativeScale) logWarn(tpaggrowMessage0,
                             surfaceId, width, height);
    double ms = (double) (svcGetSystemTick() - start) / (double) (SYSCLOCK_ARM11 / 1000);
    // Address and opaque count expose stale copies: ch2_slide's first three
    // 20x20 reads should have 95, 106 and 176 pixels with alpha > 0.
    size_t opaque = 0;
    for (size_t i = 3; bytes > i; i += 4) opaque += rgba[i] != 0;
    logInfo(tpaggrowMessage1,
            surfaceId, width, height, data, ms, opaque);
}
#endif
