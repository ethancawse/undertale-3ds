// Shared idle-bottom ownership (paintchg). UNDERTALE, both targets.
#ifndef CTR_BOTTOM_PAINT_H
#define CTR_BOTTOM_PAINT_H
#include <stdbool.h>
#include <stdint.h>

struct Runner;

// CPU-only borrowed spare: RGB565, 320 x 240, rotated column-major.
// Pixel (x,y) is pixels[x * 240 + 239 - y]. Valid only inside paint().
// Paint the entire image; no allocation, LCD call, flush, swap or saved pointer.
typedef struct { uint16_t* pixels; } CtrBottomPaintSurface;
typedef enum {
    CTR_BOTTOM_IDLE_DIM,       // release keep-lit; also use on asset failure
    CTR_BOTTOM_IDLE_KEEP,      // Frame requested, content unchanged
    CTR_BOTTOM_IDLE_DIRTY      // Frame requested, content changed
} CtrBottomIdleAction;

// update() runs once after recording and before bottom composition. It receives
// eligibility INCLUDING queued writers; false never grants access to pixels.
// A generation change forces paint even if update returns KEEP. Return DIM on
// unavailable assets. paint() returns false on failure, releasing keep-lit.
// Multiple changes belong in one image/update, one paint and one CPU publish.
// Callbacks hold the painter lock; paint also holds the console claim. Do not call registration,
// invalidate(), blocking console helpers, framebuffer APIs or LCD services.
typedef struct {
    CtrBottomIdleAction (*update)(void* user, struct Runner*, bool eligible, uint32_t generation);
    bool (*paint)(void* user, const CtrBottomPaintSurface*);
    void* user;
} CtrBottomIdlePainter;

// Game-thread only. Copies callbacks; NULL unregisters. No LCD allocation.
// No painter is installed by default. Registration can fail on heap exhaustion.
// Single owner: a second non-NULL install is refused and logged, preserving the
// current callbacks, pending image and generation. Unregister before replacing.
bool CtrBottomPaint_setIdle(const CtrBottomIdlePainter* painter);
uint32_t CtrBottomPaint_generation(void);
// Foreign owners must invalidate BEFORE touching either bottom framebuffer.
// Current setup/notice, console, configuration, yield, APT and Rosalina paths
// are wired by paintchg. Future owners must join this contract too.
void CtrBottomPaint_invalidate(void);

// Weak layout-only gate. Default: default two-screen/game-on-top layout.
// S1/S2 supply their strong override. A touchable NONE element is insufficient.
bool CtrLayout_bottomIdleAllowed(void);
#endif
