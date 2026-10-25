// Azahar only: every new VRAM block has one word per 4 KB rewritten with its own value, so the emulator forgets any
// dead surface it still holds for that memory (docs/3ds-hardware.md, "In Azahar, a GX copy out of reused VRAM can
// return a freed surface's pixels"). Without it, a GX copy out of a new surface's VRAM can hand back the pixels of
// the surface that used the memory before, and identical builds draw different pictures.
//
// Never on a console. The CPU must not write VRAM there: the exheader maps it read-only (cia/*.rsf, "1f000000-1f5fffff:r",
// so a write faults), and were it writable, the mapping is cacheable, so a dirty line evicted later would put the old
// word back over pixels the GPU drew since. A console's GPU reads the same bytes either way: the touch changes nothing
// there but the risk. So it runs only when svcGetSystemInfo(0x20000, 0) says Azahar (ctr_aot_module.c's onEmulator);
// on a console a VRAM allocation pays one test of a cached flag.
//
// All late code and statics (vramtouch), reached through linker wraps (CMakeLists.txt) so that no existing
// function changes: the late allocator entries (ctr_memory_lock.c) call CtrMemory_vram*Once, and the tiles' copies in
// VRAM (ctr_renderer.c's mirrorHotTiles, from CtrMemory_vramAllocNow) are all handed to CtrTextures_addMirror.
#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ctr_textures.h"

// A section per function, so that --gc-sections drops the wrappers a build never calls (UNDERTALE has no
// vramMemAlign caller: keeping its wrapper would link libctru's vramMemAlign into .text and move what follows).
#define VRAM_TOUCH_LATE(name) __attribute__((noinline, section(".text.late.vramtouch." #name)))
#define VRAM_TOUCH_STRIDE_WORDS 1024u // 4 KB

static int8_t vramTouchEmulator __attribute__((section(".bss.late.vramtouch"))); // 0 not asked yet, 1 Azahar, -1 not

static bool vramTouchOn(void) VRAM_TOUCH_LATE(on);
static void* vramTouch(void* mem, size_t bytes) VRAM_TOUCH_LATE(touch);

// Azahar answers the query with 1; Luma3DS answers it with 0 (and a result R_SUCCEEDED counts as success).
static bool vramTouchOn(void) {
    if (vramTouchEmulator == 0) {
        s64 out = 0;
        vramTouchEmulator = R_SUCCEEDED(svcGetSystemInfo(&out, 0x20000, 0)) && out != 0 ? 1 : -1;
    }
    return vramTouchEmulator > 0;
}

static void* vramTouch(void* mem, size_t bytes) {
    if (mem == NULL || !vramTouchOn()) return mem;
    volatile uint32_t* w = (volatile uint32_t*) mem;
    for (size_t i = 0; bytes / 4u > i; i += VRAM_TOUCH_STRIDE_WORDS) w[i] = w[i];
    return mem;
}

void* __real_CtrMemory_vramAllocOnce(size_t size);
void* __real_CtrMemory_vramAllocAtOnce(size_t size, vramAllocPos pos);
void* __real_CtrMemory_vramMemAlignOnce(size_t size, size_t alignment);
void* __real_CtrMemory_vramMemAlignAtOnce(size_t size, size_t alignment, vramAllocPos pos);
bool __real_CtrTextures_addMirror(CtrTextureCache* cache, CtrTile* tile, void* vram);
void* __wrap_CtrMemory_vramAllocOnce(size_t size) VRAM_TOUCH_LATE(alloc);
void* __wrap_CtrMemory_vramAllocAtOnce(size_t size, vramAllocPos pos) VRAM_TOUCH_LATE(allocAt);
void* __wrap_CtrMemory_vramMemAlignOnce(size_t size, size_t alignment) VRAM_TOUCH_LATE(memAlign);
void* __wrap_CtrMemory_vramMemAlignAtOnce(size_t size, size_t alignment, vramAllocPos pos) VRAM_TOUCH_LATE(memAlignAt);
bool __wrap_CtrTextures_addMirror(CtrTextureCache* cache, CtrTile* tile, void* vram) VRAM_TOUCH_LATE(mirror);

void* __wrap_CtrMemory_vramAllocOnce(size_t size) {
    return vramTouch(__real_CtrMemory_vramAllocOnce(size), size);
}

void* __wrap_CtrMemory_vramAllocAtOnce(size_t size, vramAllocPos pos) {
    return vramTouch(__real_CtrMemory_vramAllocAtOnce(size, pos), size);
}

void* __wrap_CtrMemory_vramMemAlignOnce(size_t size, size_t alignment) {
    return vramTouch(__real_CtrMemory_vramMemAlignOnce(size, alignment), size);
}

void* __wrap_CtrMemory_vramMemAlignAtOnce(size_t size, size_t alignment, vramAllocPos pos) {
    return vramTouch(__real_CtrMemory_vramMemAlignAtOnce(size, alignment, pos), size);
}

// A tile's copy in VRAM: its block came from CtrMemory_vramAllocNow (or, on the Old models, CtrMemory_vramAllocTop),
// which take libctru's allocator directly; every such block is handed here before anything is copied into it.
bool __wrap_CtrTextures_addMirror(CtrTextureCache* cache, CtrTile* tile, void* vram) {
    vramTouch(vram, tile->tex.size);
    return __real_CtrTextures_addMirror(cache, tile, vram);
}
