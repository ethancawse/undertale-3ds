// The first-run screen that packs every texture page before the game starts.
#ifndef CTRPORT_CTR_TEXTURE_PACK_SCREEN_H
#define CTRPORT_CTR_TEXTURE_PACK_SCREEN_H

#include "data_win.h"

// If some page of the data.win has no cache file yet, packs them all on
// every core the app may use, showing progress on the top screen, until done
// or the player presses B (the game then packs the rest in the background).
// Returns at once when every page was packed before. Call after
// CtrPack_setDataWinPath and before the renderer is created.
void CtrPackScreen_run(const DataWin* dataWin);

#endif
