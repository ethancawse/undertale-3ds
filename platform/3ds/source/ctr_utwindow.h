#ifndef CTR_UTWINDOW_H
#define CTR_UTWINDOW_H

// Only final presentation moves. The application's surface and game coordinates stay intact.
#include <citro3d.h>
#include <stdbool.h>
typedef void (*CtrUtWindowBlit)(void* renderer, C3D_Tex* tex, float srcX, float srcY, float srcW, float srcH,
                               float offX, float offY, const float* dst, bool linear);
extern const CtrUtWindowBlit CtrUtWindow_originalBlit;
void CtrUtWindow_blitApp(void* renderer, C3D_Tex* tex, float srcX, float srcY, float srcW, float srcH,
                         float offX, float offY, const float* dst, bool linear);
#endif
